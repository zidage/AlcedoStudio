//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Editor commands and render.preview through real alcedo-cli and alcedo_main --headless
// processes on a CI RAW file. The CI preset runs these tests (label ci_automation_flow) with the
// default editor backend of the platform; on Windows a build with CUDA uses CUDA.

#include <gtest/gtest.h>

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <functional>

#include "app/editor_parameter_catalog.hpp"
#include "automation/automation_client.hpp"
#include "automation/automation_session_file.hpp"
#include "automation_editor_e2e_session.hpp"

namespace alcedo::automation {
namespace {

using test::CliRun;

class AutomationEditorE2ETest : public test::AutomationEditorE2ETestBase {};

/// Reads notifications from @p watch until one has @p method and satisfies @p match, and returns
/// its params. Returns an empty object when none arrives within one minute.
auto WaitForNotification(AutomationClient& watch, const QString& method,
                         const std::function<bool(const QJsonObject&)>& match) -> QJsonObject {
  const QDeadlineTimer deadline(60000);
  while (!deadline.hasExpired()) {
    const auto notification = watch.ReadNotification(static_cast<int>(deadline.remainingTime()));
    if (!notification.has_value()) {
      break;
    }
    if (notification->method == method && match(notification->params)) {
      return notification->params;
    }
  }
  return {};
}

TEST_F(AutomationEditorE2ETest, SetExposureCreatesOneCommitAndReadsBackUiValue) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("set_project")).isEmpty());
  OpenPhoto();
  const qsizetype  commits_before = CommitCount();
  const QJsonValue head_before    = Head();
  // A value equal to the current one changes nothing and makes no commit.
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 0.5), 0.1);
  ASSERT_GT(std::abs(Value(QStringLiteral("saturation")) - 20), 1);

  const QJsonObject set = Set(QStringLiteral("exposure"), 0.5);
  EXPECT_NEAR(set.value("value").toDouble(), 0.5, 1e-6);
  EXPECT_NE(set.value("head_commit"), head_before);
  EXPECT_EQ(CommitCount(), commits_before + 1);
  EXPECT_EQ(Head(), set.value("head_commit"));
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.5, 1e-6);

  // The issue example: a percent field in UI units.
  EXPECT_NEAR(Set(QStringLiteral("saturation"), 20).value("value").toDouble(), 20, 1e-4);
  EXPECT_NEAR(Value(QStringLiteral("saturation")), 20, 1e-4);
  EXPECT_EQ(CommitCount(), commits_before + 2);

  const QJsonValue  head = Head();
  const QJsonObject same = Set(QStringLiteral("saturation"), 20);
  EXPECT_EQ(same.value("head_commit"), head);
  EXPECT_EQ(CommitCount(), commits_before + 2);
  Stop();
}

TEST_F(AutomationEditorE2ETest, UndoRestoresPreviousUiValue) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("undo_project")).isEmpty());
  OpenPhoto();
  const double before = Value(QStringLiteral("exposure"));
  ASSERT_GT(std::abs(before - 2.0), 0.1);
  const QJsonValue  head_before = Head();
  const QJsonValue  head_after  = Set(QStringLiteral("exposure"), 2.0).value("head_commit");

  const QJsonObject undone      = Result(QStringLiteral("editor.undo"));
  EXPECT_EQ(undone.value("head_commit"), head_before);
  EXPECT_TRUE(undone.value("can_redo").toBool());
  EXPECT_NEAR(Value(QStringLiteral("exposure")), before, 1e-6);

  const QJsonObject redone = Result(QStringLiteral("editor.redo"));
  EXPECT_EQ(redone.value("head_commit"), head_after);
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 2.0, 1e-6);
  Stop();
}

TEST_F(AutomationEditorE2ETest, BatchStopsAtFirstInvalidValueAndReportsApplied) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("batch_project")).isEmpty());
  OpenPhoto();
  const qsizetype commits_before  = CommitCount();
  const double    contrast_before = Value(QStringLiteral("contrast"));
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 0.5), 0.1);
  ASSERT_GT(std::abs(contrast_before - 10), 1);

  const CliRun run =
      Call(QStringLiteral("editor.batch_set"),
           QJsonObject{{"changes", QJsonArray{QJsonObject{{"field", "exposure"}, {"value", 0.5}},
                                              QJsonObject{{"field", "saturation"}, {"value", 1000}},
                                              QJsonObject{{"field", "contrast"}, {"value", 10}}}}});
  EXPECT_EQ(run.exit_code, 1);
  const QJsonObject error = run.json.value("error").toObject();
  EXPECT_EQ(error.value("code").toInt(), -32602);
  const QJsonObject data = error.value("data").toObject();
  EXPECT_EQ(data.value("applied").toInt(), 1);
  EXPECT_EQ(data.value("pointer").toString(), QStringLiteral("/changes/1/value"));
  EXPECT_EQ(data.value("head_commit"), Head());
  EXPECT_EQ(CommitCount(), commits_before + 1);
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.5, 1e-6);
  EXPECT_NEAR(Value(QStringLiteral("contrast")), contrast_before, 1e-6)
      << "changes after the failure stay out";

  const QJsonObject applied = Result(
      QStringLiteral("editor.batch_set"),
      QJsonObject{{"changes", QJsonArray{QJsonObject{{"field", "contrast"}, {"value", 10}},
                                         QJsonObject{{"field", "vibrance"}, {"value", -5}}}}});
  EXPECT_EQ(applied.value("applied").toInt(), 2);
  EXPECT_EQ(CommitCount(), commits_before + 3);
  EXPECT_NEAR(applied.value("values").toObject().value("vibrance").toDouble(), -5, 1e-4);
  Stop();
}

TEST_F(AutomationEditorE2ETest, EveryValueReadByGetIsAcceptedBySetAndKeepsItsValue) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("round_trip_project")).isEmpty());
  OpenPhoto();
  // A LUT with a strength, so that the lut value is not the empty no-LUT value.
  const QString cube_path = QDir(root_).filePath(QStringLiteral("round_trip.cube"));
  {
    QFile cube(cube_path);
    ASSERT_TRUE(cube.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    QTextStream out(&cube);
    out << "LUT_3D_SIZE 2\n";
    for (int b = 0; b <= 1; ++b) {
      for (int g = 0; g <= 1; ++g) {
        for (int r = 0; r <= 1; ++r) {
          out << 0.05 + 0.9 * r << ' ' << 0.1 + 0.8 * g << ' ' << 0.2 + 0.7 * b << '\n';
        }
      }
    }
  }
  const QJsonObject lut{{"cube_path", cube_path}, {"strength", 0.5}};
  EXPECT_EQ(Result(QStringLiteral("editor.set"), QJsonObject{{"field", "lut"}, {"value", lut}})
                .value("value")
                .toObject(),
            (QJsonObject{{"cube_path", cube_path},
                         {"strength", 0.5},
                         {"input_encoding", "acescc"},
                         {"output_encoding", "acescc"}}));

  const QJsonObject values = Result(QStringLiteral("editor.get")).value("values").toObject();
  // A RAW image has no input profile; the RAW Decode panel hides it too.
  EXPECT_FALSE(values.contains("input_profile"));
  ASSERT_FALSE(values.isEmpty());
  // Each value equals the current one, so no write makes a commit.
  const QJsonValue head = Head();
  for (auto it = values.begin(); it != values.end(); ++it) {
    const CliRun run =
        Call(QStringLiteral("editor.set"), QJsonObject{{"field", it.key()}, {"value", it.value()}});
    ASSERT_EQ(run.exit_code, 0) << it.key().toStdString() << ": "
                                << QJsonDocument(run.json).toJson().toStdString();
    EXPECT_EQ(run.json.value("value"), it.value()) << it.key().toStdString();
    EXPECT_EQ(run.json.value("head_commit"), head) << it.key().toStdString();
  }
  EXPECT_EQ(Head(), head);
  EXPECT_EQ(Result(QStringLiteral("editor.get")).value("values").toObject(), values);

  // Writing the input profile of a RAW image names the reason.
  const CliRun input = Call(QStringLiteral("editor.set"),
                            QJsonObject{{"field", "input_profile"},
                                        {"value", QJsonObject{{"profile_override", "auto"}}}});
  EXPECT_EQ(input.exit_code, 1);
  const QJsonObject error = input.json.value("error").toObject();
  EXPECT_EQ(error.value("code").toInt(), -32602);
  EXPECT_EQ(error.value("message").toString(),
            QStringLiteral("input_profile: a RAW image has no input profile"));
  Stop();
}

// Film simulation (issue #718) ----------------------------------------------------------------

/// The commits of the active Version from the head back to the root (newest first).
auto AppliedCommits(const QJsonObject& history) -> QJsonArray {
  QJsonArray applied;
  for (const auto& commit : history.value("commits").toArray()) {
    if (commit.toObject().value("position").toString() != QStringLiteral("future")) {
      applied.push_back(commit);
    }
  }
  return applied;
}

/// The value of the Display Transform item of adjustments.items for @p element.
auto DisplayTransformItemText(const QJsonObject& items) -> QString {
  for (const auto& node : items.value("nodes").toArray()) {
    for (const auto& item : node.toObject().value("items").toArray()) {
      if (item.toObject().value("item").toString() == QStringLiteral("drt")) {
        return item.toObject().value("value").toString();
      }
    }
  }
  return {};
}

class AutomationFilmPrintE2ETest : public test::AutomationEditorE2ETestBase {
 protected:
  /// The editor_value of the one film.list entry that @p id names.
  auto FilmEditorValue(const QString& id) -> QJsonObject {
    const QJsonArray films =
        Result(QStringLiteral("film.list"), QJsonObject{{"query", id}}).value("films").toArray();
    for (const auto& film : films) {
      if (film.toObject().value("id").toString() == id) {
        return film.toObject().value("editor_value").toObject();
      }
    }
    ADD_FAILURE() << "film.list has no entry " << id.toStdString();
    return {};
  }

  /// The odt value of Film Print on Portra 400 and Vision 2383, from the editor_value of film.list.
  auto PortraOn2383() -> QJsonObject {
    QJsonObject odt{{"method", "film_print"}};
    for (const auto& value : {FilmEditorValue(QStringLiteral("kodak_portra_400")),
                              FilmEditorValue(QStringLiteral("kodak_2383"))}) {
      for (auto it = value.begin(); it != value.end(); ++it) {
        odt.insert(it.key(), it.value());
      }
    }
    return odt;
  }

  auto Values() -> QJsonObject {
    return Result(QStringLiteral("editor.get")).value("values").toObject();
  }
};

// editor.set of method Film Print gives the document and the history entries of the panel: the
// complete odt state, then a second commit of Film Grain at 100 % (the method rule).
TEST_F(AutomationFilmPrintE2ETest, FilmPrintSetMatchesThePanelWritesAndTurnsOnFilmGrain) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("film_print_project")).isEmpty());
  OpenPhoto();
  const QJsonObject before = Values();
  ASSERT_NE(before.value("odt").toObject().value("method").toString(),
            QStringLiteral("film_print"));
  ASSERT_EQ(before.value("film_grain").toDouble(-1), 0.0);
  const qsizetype applied_before = AppliedCommits(Result(QStringLiteral("editor.history"))).size();

  // The editor_value of film.list sets the property.
  const QJsonObject odt          = PortraOn2383();
  EXPECT_EQ(odt, (QJsonObject{{"method", "film_print"},
                              {"negative", "kodak_portra_400"},
                              {"print_material", "kodak_2383"}}));
  const QJsonObject set =
      Result(QStringLiteral("editor.set"), QJsonObject{{"field", "odt"}, {"value", odt}});
  const QJsonObject written = set.value("value").toObject();
  EXPECT_EQ(written.value("method").toString(), QStringLiteral("film_print"));
  EXPECT_EQ(written.value("negative").toString(), QStringLiteral("kodak_portra_400"));
  EXPECT_EQ(written.value("print_material").toString(), QStringLiteral("kodak_2383"));
  const QJsonObject set_values = set.value("values").toObject();
  EXPECT_EQ(set_values.value("odt").toObject(), written);
  EXPECT_DOUBLE_EQ(set_values.value("film_grain").toDouble(-1), 100.0);

  const QJsonObject history = Result(QStringLiteral("editor.history"));
  const QJsonArray  applied = AppliedCommits(history);
  ASSERT_EQ(applied.size(), applied_before + 2);
  // The history lists the newest commit first: the Film Grain commit of the rule, then odt.
  EXPECT_EQ(applied.at(0).toObject().value("field").toString(), QStringLiteral("film_grain"));
  EXPECT_EQ(applied.at(0).toObject().value("author").toString(), QStringLiteral("agent"));
  EXPECT_EQ(applied.at(1).toObject().value("field").toString(), QStringLiteral("odt"));
  EXPECT_EQ(history.value("head_commit"), set.value("head_commit"));
  const QJsonObject cli_values = Values();
  EXPECT_DOUBLE_EQ(cli_values.value("film_grain").toDouble(-1), 100.0);

  // The read-back of the summary names the selection.
  EXPECT_EQ(DisplayTransformItemText(
                Result(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", element_}})),
            QStringLiteral("Film Print, Kodak Portra 400, Kodak Vision 2383"));

  // The writes of the panel for the same selection: the complete odt state of its models, then
  // Film Grain.
  Result(QStringLiteral("editor.undo"));
  Result(QStringLiteral("editor.undo"));
  ASSERT_EQ(Values(), before);
  QJsonObject panel_odt = before.value("odt").toObject();
  for (auto it = odt.begin(); it != odt.end(); ++it) {
    panel_odt.insert(it.key(), it.value());
  }
  const QJsonObject batch = Result(
      QStringLiteral("editor.batch_set"),
      QJsonObject{
          {"changes",
           QJsonArray{QJsonObject{{"field", "odt"}, {"value", panel_odt}},
                      QJsonObject{{"field", "film_grain"},
                                  {"value", EditorParameterCatalog::kFilmPrintFilmGrainUi}}}}});
  EXPECT_EQ(batch.value("applied").toInt(), 2);
  EXPECT_EQ(Values(), cli_values);
  EXPECT_EQ(AppliedCommits(Result(QStringLiteral("editor.history"))).size(), applied.size());
  Stop();
}

TEST_F(AutomationFilmPrintE2ETest, LeavingFilmPrintSetsFilmGrainToZeroAndAStoredGrainStays) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("film_leave_project")).isEmpty());
  OpenPhoto();
  // A stored strength above 0 stays when the method changes to Film Print: one commit.
  Set(QStringLiteral("film_grain"), 40);
  qsizetype         commits = CommitCount();
  const QJsonObject enter   = Result(QStringLiteral("editor.set"),
                                     QJsonObject{{"field", "odt"}, {"value", PortraOn2383()}});
  EXPECT_FALSE(enter.value("values").toObject().contains("film_grain"));
  EXPECT_EQ(CommitCount(), commits + 1);
  EXPECT_NEAR(Value(QStringLiteral("film_grain")), 40.0, 1e-4);

  // Another method on Film Print sets Film Grain to 0 in a second commit.
  commits = CommitCount();
  const QJsonObject leave =
      Result(QStringLiteral("editor.set"),
             QJsonObject{{"field", "odt"}, {"value", QJsonObject{{"method", "open_drt"}}}});
  EXPECT_DOUBLE_EQ(leave.value("values").toObject().value("film_grain").toDouble(-1), 0.0);
  EXPECT_EQ(CommitCount(), commits + 2);
  EXPECT_DOUBLE_EQ(Value(QStringLiteral("film_grain")), 0.0);
  EXPECT_EQ(DisplayTransformItemText(
                Result(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", element_}})),
            QStringLiteral("OpenDRT"));
  Stop();
}

// A batch that also writes film_grain keeps the explicit value: the rule writes nothing.
TEST_F(AutomationFilmPrintE2ETest, BatchWithAnExplicitFilmGrainKeepsTheExplicitValue) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("film_batch_project")).isEmpty());
  OpenPhoto();
  ASSERT_EQ(Value(QStringLiteral("film_grain")), 0.0);
  const qsizetype   commits = CommitCount();
  const QJsonObject batch   = Result(
      QStringLiteral("editor.batch_set"),
      QJsonObject{{"changes", QJsonArray{QJsonObject{{"field", "odt"}, {"value", PortraOn2383()}},
                                         QJsonObject{{"field", "film_grain"}, {"value", 30}}}}});
  EXPECT_EQ(batch.value("applied").toInt(), 2);
  EXPECT_EQ(CommitCount(), commits + 2);
  EXPECT_NEAR(batch.value("values").toObject().value("film_grain").toDouble(-1), 30.0, 1e-4);
  EXPECT_NEAR(Value(QStringLiteral("film_grain")), 30.0, 1e-4);

  // Without film_grain in the batch, the rule follows the change of the method, and the result
  // lists film_grain.
  const QJsonObject leave =
      Result(QStringLiteral("editor.batch_set"),
             QJsonObject{{"changes",
                          QJsonArray{QJsonObject{
                              {"field", "odt"}, {"value", QJsonObject{{"method", "aces_2_0"}}}}}}});
  EXPECT_EQ(leave.value("applied").toInt(), 1);
  EXPECT_EQ(CommitCount(), commits + 4);
  EXPECT_DOUBLE_EQ(leave.value("values").toObject().value("film_grain").toDouble(-1), 0.0);
  Stop();
}

// The headless host finds the film print resources next to alcedo_main, as Alcedo Studio does.
TEST_F(AutomationFilmPrintE2ETest, HeadlessPreviewRendersAFilmPrintDocument) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("film_preview_project")).isEmpty());
  OpenPhoto();
  const QJsonObject films = Result(QStringLiteral("film.list"), QJsonObject{{"list", "negative"}});
  EXPECT_TRUE(films.value("warnings").toArray().isEmpty())
      << QJsonDocument(films.value("warnings").toArray()).toJson().toStdString();
  Result(QStringLiteral("editor.set"), QJsonObject{{"field", "odt"}, {"value", PortraOn2383()}});
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("film_preview"));
  const QJsonObject preview = Result(QStringLiteral("render.preview"),
                                     QJsonObject{{"out_dir", out_dir}, {"long_edge", 512}});
  const QImage      current(preview.value("current").toObject().value("path").toString());
  const QImage      root(preview.value("root").toObject().value("path").toString());
  ASSERT_FALSE(current.isNull());
  ASSERT_FALSE(root.isNull());
  EXPECT_EQ(std::max(current.width(), current.height()), 512);
  EXPECT_NE(current, root) << "the current image has the film print";
  Stop();
}

TEST_F(AutomationEditorE2ETest, CurvePointOutsideTheUnitSquareIsRejectedAtValue) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("curve_range_project")).isEmpty());
  OpenPhoto();
  const qsizetype commits_before = CommitCount();
  const auto      point          = [](double x, double y) { return QJsonObject{{"x", x}, {"y", y}}; };
  const struct {
    QJsonArray points;
    QString    reason;
  } cases[] = {
      {QJsonArray{point(0, 0), point(1.2, 1)}, QStringLiteral("curve.points[1].x must be in [0, 1]")},
      {QJsonArray{point(0, -0.5), point(1, 1)}, QStringLiteral("curve.points[0].y must be in [0, 1]")},
      {QJsonArray{point(0, 0), point(0.6, 0.5), point(0.4, 0.6), point(1, 1)},
       QStringLiteral("curve.points[2].x must be greater than curve.points[1].x")},
  };
  for (const auto& rejected : cases) {
    const CliRun run =
        Call(QStringLiteral("editor.set"),
             QJsonObject{{"field", "curve"}, {"value", QJsonObject{{"points", rejected.points}}}});
    EXPECT_EQ(run.exit_code, 1) << rejected.reason.toStdString();
    const QJsonObject error = run.json.value("error").toObject();
    EXPECT_EQ(error.value("code").toInt(), -32602);
    EXPECT_EQ(error.value("data").toObject().value("pointer").toString(), QStringLiteral("/value"));
    EXPECT_EQ(error.value("data").toObject().value("reason").toString(), rejected.reason);
  }
  EXPECT_EQ(CommitCount(), commits_before);
  Stop();
}

TEST_F(AutomationEditorE2ETest, SetBeforeInteractiveIsRejectedWithTheOwnerMessage) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("rejected_project")).isEmpty());
  const CliRun run =
      Call(QStringLiteral("editor.set"), QJsonObject{{"field", "exposure"}, {"value", 1.0}});
  EXPECT_EQ(run.exit_code, 1);
  const QJsonObject error = run.json.value("error").toObject();
  EXPECT_EQ(error.value("code").toInt(), -32002);
  EXPECT_FALSE(error.value("message").toString().isEmpty());
  EXPECT_EQ(error.value("data").toObject().value("reason"), error.value("message"));
  Stop();
}

TEST_F(AutomationEditorE2ETest, PreviewWritesCurrentAndRootPngWithRequestedLongEdge) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("preview_project")).isEmpty());
  OpenPhoto();
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 1.0), 0.1);
  Set(QStringLiteral("exposure"), 1.0);
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("preview"));
  const QJsonObject preview = Result(QStringLiteral("render.preview"),
                                     QJsonObject{{"out_dir", out_dir}, {"long_edge", 512}});
  for (const char* key : {"current", "root"}) {
    const QJsonObject file = preview.value(QLatin1String(key)).toObject();
    const QString     path = file.value("path").toString();
    EXPECT_TRUE(path.endsWith(QStringLiteral("_%1.png").arg(QLatin1String(key)))) << key;
    EXPECT_EQ(QFileInfo(path).absolutePath(), QFileInfo(out_dir).absoluteFilePath()) << key;
    const QImage image(path);
    ASSERT_FALSE(image.isNull()) << path.toStdString();
    EXPECT_EQ(std::max(image.width(), image.height()), 512) << key;
    EXPECT_EQ(file.value("width").toInt(), image.width()) << key;
    EXPECT_EQ(file.value("height").toInt(), image.height()) << key;
  }
  const QImage current(preview.value("current").toObject().value("path").toString());
  const QImage root(preview.value("root").toObject().value("path").toString());
  EXPECT_EQ(current.size(), root.size());
  EXPECT_NE(current, root) << "the current image has the exposure edit";
  Stop();
}

// Issue #715: the root file is a render of the root state. Develop fields of the working values
// (RAW decode and Lens Correction) change the current file only.
TEST_F(AutomationEditorE2ETest, PreviewRootFileKeepsTheRootDevelopSettings) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("preview_root_develop_project")).isEmpty());
  OpenPhoto();
  const auto read_field = [this](const QString& field) {
    return Result(QStringLiteral("editor.get"), QJsonObject{{"fields", QJsonArray{field}}})
        .value("values")
        .toObject()
        .value(field)
        .toObject();
  };
  const auto set_field = [this](const QString& field, const QJsonObject& value) {
    (void)Result(QStringLiteral("editor.set"), QJsonObject{{"field", field}, {"value", value}});
  };
  const QJsonObject root_raw  = read_field(QStringLiteral("raw_decode"));
  const QJsonObject root_lens = read_field(QStringLiteral("lens_calib"));
  ASSERT_TRUE(root_raw.value("highlights_reconstruct").toBool());
  ASSERT_FALSE(root_lens.value("enabled").toBool());

  QJsonObject changed_raw = root_raw;
  changed_raw["highlights_reconstruct"] = false;
  QJsonObject changed_lens              = root_lens;
  changed_lens["enabled"]               = true;
  set_field(QStringLiteral("raw_decode"), changed_raw);
  set_field(QStringLiteral("lens_calib"), changed_lens);
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("preview_root_develop"));
  const QJsonObject changed =
      Result(QStringLiteral("render.preview"),
             QJsonObject{{"out_dir", out_dir}, {"long_edge", 512}, {"file_stem", "changed"}});

  set_field(QStringLiteral("raw_decode"), root_raw);
  set_field(QStringLiteral("lens_calib"), root_lens);
  const QJsonObject restored = Result(
      QStringLiteral("render.preview"),
      QJsonObject{
          {"out_dir", out_dir}, {"long_edge", 512}, {"compare", "none"}, {"file_stem", "restored"}});

  const QImage changed_current(changed.value("current").toObject().value("path").toString());
  const QImage changed_root(changed.value("root").toObject().value("path").toString());
  const QImage root_state(restored.value("current").toObject().value("path").toString());
  ASSERT_FALSE(changed_current.isNull());
  ASSERT_FALSE(changed_root.isNull());
  ASSERT_FALSE(root_state.isNull());
  ASSERT_NE(changed_current, root_state) << "the RAW decode and Lens Correction change the image";
  EXPECT_EQ(changed_root, root_state) << "the root file is a render of the root state";
  Stop();
}

TEST_F(AutomationEditorE2ETest, RegionPreviewHasRegionAspectRatio) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("region_project")).isEmpty());
  OpenPhoto();
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("full"));
  const QJsonObject full    = Result(QStringLiteral("render.preview"),
                                     QJsonObject{{"out_dir", out_dir}, {"long_edge", 1024}})
                               .value("current")
                               .toObject();
  const double full_width  = full.value("width").toDouble();
  const double full_height = full.value("height").toDouble();
  ASSERT_GT(full_width, 0);
  ASSERT_GT(full_height, 0);

  const QJsonObject region{{"x", 0.1}, {"y", 0.2}, {"w", 0.5}, {"h", 0.25}};
  const QJsonObject part =
      Result(QStringLiteral("render.preview"),
             QJsonObject{{"out_dir", QDir(root_).filePath(QStringLiteral("region"))},
                         {"long_edge", 1024},
                         {"region", region}})
          .value("current")
          .toObject();
  const double width  = part.value("width").toDouble();
  const double height = part.value("height").toDouble();
  ASSERT_GT(width, 0);
  ASSERT_GT(height, 0);
  // The region is 0.5 by 0.25 of the image: height = width * (0.25 H) / (0.5 W), to a pixel.
  const double expected_height = width * (0.25 * full_height) / (0.5 * full_width);
  EXPECT_LE(std::abs(height - expected_height), 1.0)
      << width << "x" << height << " for a " << full_width << "x" << full_height << " image";

  const CliRun outside =
      Call(QStringLiteral("render.preview"),
           QJsonObject{{"out_dir", out_dir},
                       {"region", QJsonObject{{"x", 0.8}, {"y", 0.0}, {"w", 0.5}, {"h", 0.5}}}});
  EXPECT_EQ(outside.json.value("error").toObject().value("code").toInt(), -32602);
  Stop();
}

// Issue #355: compare 'none' writes only the current file, and file_stem names the files.
TEST_F(AutomationEditorE2ETest, PreviewWithoutCompareWritesOnlyCurrentFileWithFileStem) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("stem_project")).isEmpty());
  OpenPhoto();
  const QString     out_dir = QDir(root_).filePath(QStringLiteral("stem"));
  const QJsonObject preview = Result(
      QStringLiteral("render.preview"),
      QJsonObject{
          {"out_dir", out_dir}, {"long_edge", 256}, {"compare", "none"}, {"file_stem", "shot_7"}});
  EXPECT_FALSE(preview.contains("root"));
  const QString current_path = preview.value("current").toObject().value("path").toString();
  EXPECT_EQ(QFileInfo(current_path).fileName(), QStringLiteral("shot_7_current.png"));
  const QImage current(current_path);
  ASSERT_FALSE(current.isNull()) << current_path.toStdString();
  EXPECT_EQ(std::max(current.width(), current.height()), 256);
  EXPECT_EQ(QDir(out_dir).entryList(QDir::Files), QStringList{QStringLiteral("shot_7_current.png")})
      << "no root file is written";

  // Issue #401: shot_7_current.png exists, so the request fails and writes no file.
  QFile current_file(current_path);
  ASSERT_TRUE(current_file.open(QIODevice::ReadOnly));
  const QByteArray current_bytes = current_file.readAll();
  current_file.close();
  const CliRun existing =
      Call(QStringLiteral("render.preview"),
           QJsonObject{{"out_dir", out_dir}, {"long_edge", 128}, {"file_stem", "shot_7"}});
  EXPECT_EQ(existing.exit_code, 1);
  EXPECT_EQ(existing.json.value("error").toObject().value("code").toInt(), -32602);
  ASSERT_TRUE(current_file.open(QIODevice::ReadOnly));
  EXPECT_EQ(current_file.readAll(), current_bytes);
  current_file.close();
  EXPECT_EQ(QDir(out_dir).entryList(QDir::Files), QStringList{QStringLiteral("shot_7_current.png")})
      << "the rejected request writes no root file";

  const QJsonObject both =
      Result(QStringLiteral("render.preview"),
             QJsonObject{{"out_dir", out_dir}, {"long_edge", 256}, {"file_stem", "shot_8"}});
  EXPECT_EQ(QFileInfo(both.value("root").toObject().value("path").toString()).fileName(),
            QStringLiteral("shot_8_root.png"))
      << "compare defaults to 'root'";

  // An existing root file alone also fails the request; the current file is not written.
  const QString root_only = QDir(out_dir).filePath(QStringLiteral("shot_9_root.png"));
  QFile         user_file(root_only);
  ASSERT_TRUE(user_file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
  user_file.write("USER DATA MUST SURVIVE");
  user_file.close();
  const CliRun root_exists =
      Call(QStringLiteral("render.preview"),
           QJsonObject{{"out_dir", out_dir}, {"long_edge", 128}, {"file_stem", "shot_9"}});
  EXPECT_EQ(root_exists.json.value("error").toObject().value("code").toInt(), -32602);
  EXPECT_FALSE(QFileInfo::exists(QDir(out_dir).filePath(QStringLiteral("shot_9_current.png"))));
  ASSERT_TRUE(user_file.open(QIODevice::ReadOnly));
  EXPECT_EQ(user_file.readAll(), QByteArray("USER DATA MUST SURVIVE"));

  const CliRun folder = Call(QStringLiteral("render.preview"),
                             QJsonObject{{"out_dir", out_dir}, {"file_stem", "../outside"}});
  EXPECT_EQ(folder.json.value("error").toObject().value("code").toInt(), -32602);
  Stop();
}

TEST_F(AutomationEditorE2ETest, CropRatioPresetGivesPresetAspectInPixels) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("crop_ratio_project")).isEmpty());
  OpenPhoto();
  const QJsonObject root =
      Result(QStringLiteral("render.preview"),
             QJsonObject{{"out_dir", QDir(root_).filePath(QStringLiteral("root"))},
                         {"long_edge", 1400}})
          .value("root")
          .toObject();
  const double source_aspect = root.value("width").toDouble() / root.value("height").toDouble();
  // A square source would hide a constraint in normalized coordinates.
  ASSERT_GT(std::abs(source_aspect - 1.0), 0.1) << "the CI RAW file is square";

  // The write of the issue: the 2.35:1 preset is a pixel ratio, not 1 / 2.35 of the height.
  Result(QStringLiteral("editor.set"),
         QJsonObject{{"field", "crop_rotate"},
                     {"value", QJsonObject{{"x", 0.0},
                                           {"y", 0.31},
                                           {"width", 1.0},
                                           {"height", 0.638},
                                           {"angle_degrees", 0.0},
                                           {"aspect_preset", "ratio_2_35_1_35mm"}}}});
  const QJsonObject crop =
      Result(QStringLiteral("editor.get"), QJsonObject{{"fields", QJsonArray{"crop_rotate"}}})
          .value("values")
          .toObject()
          .value("crop_rotate")
          .toObject();
  EXPECT_EQ(crop.value("aspect_preset").toString(), QStringLiteral("ratio_2_35_1_35mm"));
  // source_aspect comes from a preview rounded to whole pixels.
  EXPECT_NEAR(crop.value("width").toDouble() * source_aspect / crop.value("height").toDouble(),
              2.35, 5e-3);

  const QJsonObject current =
      Result(QStringLiteral("render.preview"),
             QJsonObject{{"out_dir", QDir(root_).filePath(QStringLiteral("cropped"))},
                         {"long_edge", 1400}})
          .value("current")
          .toObject();
  const double width  = current.value("width").toDouble();
  const double height = current.value("height").toDouble();
  ASSERT_GT(width, 0);
  ASSERT_GT(height, 0);
  EXPECT_LE(std::abs(height - width / 2.35), 1.0) << width << "x" << height;
  Stop();
}

TEST_F(AutomationEditorE2ETest, EditsPersistAfterSessionRestart) {
  const QString project_path = StartWithOnePhoto(QStringLiteral("persist_project"));
  ASSERT_FALSE(project_path.isEmpty());
  OpenPhoto();
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 0.75), 0.1);
  ASSERT_GT(std::abs(Value(QStringLiteral("saturation")) - 15), 1);
  Set(QStringLiteral("exposure"), 0.75);
  Set(QStringLiteral("saturation"), 15);
  const QJsonObject history = Result(QStringLiteral("editor.history"));
  EXPECT_TRUE(Result(QStringLiteral("editor.close")).value("closed").toBool());
  Stop();

  ASSERT_TRUE(StartWithProject(project_path));
  OpenPhoto();
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.75, 1e-6);
  EXPECT_NEAR(Value(QStringLiteral("saturation")), 15, 1e-4);
  const QJsonObject reopened = Result(QStringLiteral("editor.history"));
  EXPECT_EQ(reopened.value("head_commit"), history.value("head_commit"));
  EXPECT_EQ(reopened.value("commits").toArray().size(), history.value("commits").toArray().size());
  Stop();
}

// Issue #356: editor and history changes reach a client that watches the session.
TEST_F(AutomationEditorE2ETest, WatchReceivesSessionStateAndHistoryNotifications) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("notify_project")).isEmpty());
  const auto session_file =
      ReadAutomationSessionFile(AutomationSessionFilePath(session_dir_, QStringLiteral("e2e")));
  ASSERT_TRUE(session_file.has_value());
  // The server takes this connection before the connections of the commands below, so the
  // watch receives every notification that they cause.
  AutomationClient watch;
  ASSERT_TRUE(watch.Connect(session_file->socket_name, 10000))
      << watch.error_string().toStdString();
  const double      element = element_.toDouble();

  const QJsonObject opened  = OpenPhoto();
  const QJsonObject state   = WaitForNotification(
      watch, QStringLiteral("session.state_changed"), [element](const QJsonObject& params) {
        return params.value("state").toString() == QStringLiteral("Interactive") &&
               params.value("element_id").toDouble() == element;
      });
  ASSERT_FALSE(state.isEmpty()) << "no session.state_changed for editor.open";
  EXPECT_EQ(state.value("image_id").toDouble(), opened.value("image_id").toDouble());

  const QJsonValue  head    = Set(QStringLiteral("exposure"), 0.5).value("head_commit");
  const QJsonObject history = WaitForNotification(
      watch, QStringLiteral("history.changed"),
      [head](const QJsonObject& params) { return params.value("head_commit") == head; });
  ASSERT_FALSE(history.isEmpty()) << "no history.changed for editor.set";
  EXPECT_EQ(history.value("element_id").toDouble(), element);
  EXPECT_EQ(history.value("image_id").toDouble(), opened.value("image_id").toDouble());
  const QJsonValue first_version = history.value("version_id");
  EXPECT_FALSE(first_version.toString().isEmpty());

  const QJsonValue version_id =
      Result(QStringLiteral("versions.create"), QJsonObject{{"name", "Notified"}})
          .value("version_id");
  ASSERT_NE(version_id, first_version);
  const QJsonObject version = WaitForNotification(
      watch, QStringLiteral("history.changed"),
      [version_id](const QJsonObject& params) { return params.value("version_id") == version_id; });
  EXPECT_FALSE(version.isEmpty()) << "no history.changed for versions.create";

  Result(QStringLiteral("editor.close"));
  const QJsonObject closed = WaitForNotification(
      watch, QStringLiteral("session.state_changed"), [](const QJsonObject& params) {
        return params.value("state").toString() != QStringLiteral("Interactive");
      });
  EXPECT_FALSE(closed.isEmpty()) << "no session.state_changed for editor.close";
  Stop();
}

}  // namespace
}  // namespace alcedo::automation
