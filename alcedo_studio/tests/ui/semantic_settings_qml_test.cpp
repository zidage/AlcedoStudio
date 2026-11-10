//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Settings > Local Content Recognition and the AI label dialog (#762), loaded alone from the
// source qml/ directory with a QML stand-in for SemanticGenerationController and the
// interaction policy. The stand-in records the invokables the page calls.
//
// Buttons are activated through their `clicked` signal: the handlers hold the page logic under
// test, and offscreen pointer delivery is not needed for it.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QLocale>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <filesystem>
#include <vector>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

namespace fs = std::filesystem;

// The page area of the Settings dialog at its full size: 620 px dialog height, less the 96 px
// page header and its 1 px divider; 980 px dialog width, less the 250 px navigation.
constexpr int kSettingsPageWidth  = 730;
constexpr int kSettingsPageHeight = 620 - 96 - 1;

void          ProcessEvents(int milliseconds = 40) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto QmlDirectoryUrl() -> QString {
  const auto path = fs::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml";
  return QUrl::fromLocalFile(QString::fromStdString(path.string())).toString();
}

auto PanelText(const char* source) -> QString {
  return QCoreApplication::translate("SemanticGenerationSettingsPanel", source);
}

auto FormatText(const char* source) -> QString {
  return QCoreApplication::translate("DataFormat", source);
}

// Stand-in for SemanticGenerationController: the properties the page reads and the invokables
// it calls, which it records. A QML QtObject cannot declare the upper-case invokable names.
class StandInLabelController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int albumTotalCount MEMBER album_total_count_ NOTIFY StateChanged)
  Q_PROPERTY(int albumLabeledCount MEMBER album_labeled_count_ NOTIFY StateChanged)
  Q_PROPERTY(int albumUnlabeledCount MEMBER album_unlabeled_count_ NOTIFY StateChanged)
  Q_PROPERTY(bool running MEMBER running_ NOTIFY StateChanged)
  Q_PROPERTY(int total MEMBER total_ NOTIFY StateChanged)
  Q_PROPERTY(int embedded MEMBER embedded_ NOTIFY StateChanged)
  Q_PROPERTY(int skipped MEMBER skipped_ NOTIFY StateChanged)
  Q_PROPERTY(int failed MEMBER failed_ NOTIFY StateChanged)
  Q_PROPERTY(int canceled MEMBER canceled_ NOTIFY StateChanged)
  Q_PROPERTY(int remainingSeconds MEMBER remaining_seconds_ NOTIFY StateChanged)
  Q_PROPERTY(QString activeModelKey MEMBER active_model_key_ NOTIFY StateChanged)
  Q_PROPERTY(QString activeModelDisplayName MEMBER active_model_display_name_ NOTIFY StateChanged)
  Q_PROPERTY(int sidecarIdleStopSeconds MEMBER sidecar_idle_stop_seconds_ NOTIFY StateChanged)

 public:
  Q_INVOKABLE void RefreshAlbumSummary() { ++refresh_calls_; }
  Q_INVOKABLE void StartAlbumGeneration(bool regenerate) { start_calls_.push_back(regenerate); }
  Q_INVOKABLE void CancelGeneration() { ++cancel_calls_; }

  int               refresh_calls_ = 0;
  int               cancel_calls_  = 0;
  std::vector<bool> start_calls_;

 signals:
  void StateChanged();

 private:
  int     album_total_count_         = 2975;
  int     album_labeled_count_       = 2410;
  int     album_unlabeled_count_     = 565;
  bool    running_                   = false;
  int     total_                     = 0;
  int     embedded_                  = 0;
  int     skipped_                   = 0;
  int     failed_                    = 0;
  int     canceled_                  = 0;
  int     remaining_seconds_         = -1;
  QString active_model_key_          = QStringLiteral("siglip2");
  QString active_model_display_name_ = QStringLiteral("SigLIP2 B/32 256 Multilingual");
  int     sidecar_idle_stop_seconds_ = 60;
};

const char* const kHarnessQml = R"QML(
import QtQuick
import QtQuick.Controls
import "__QML_DIR__"

ApplicationWindow {
    width: __PAGE_WIDTH__
    height: 900
    visible: true

    QtObject {
        id: policy
        objectName: "policyStandIn"
        property bool canRunSemanticGeneration: true
    }

    SemanticGenerationSettingsPanel {
        width: parent.width
        semanticController: standInController
        interactionPolicy: policy
        projectOpen: true
        importPreference: "ask"
    }

    SemanticGenerationDialog {
        objectName: "labelDialog"
        promptVisible: false
    }
}
)QML";

class SemanticSettingsHarness {
 public:
  SemanticSettingsHarness() {
    // Digit grouping follows Qt.locale(); the expected texts use English grouping.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    AppTheme::RegisterFonts();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("standInController"), &controller_);
    QByteArray qml{kHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    qml.replace("__PAGE_WIDTH__", QByteArray::number(kSettingsPageWidth));
    engine_.loadData(qml, QUrl(QStringLiteral("file:///SemanticSettingsHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
    }
    ProcessEvents(120);
  }

  auto item(const QString& name) -> QQuickItem* {
    return window_ != nullptr ? window_->findChild<QQuickItem*>(name) : nullptr;
  }
  auto object(const QString& name) -> QObject* {
    return window_ != nullptr ? window_->findChild<QObject*>(name) : nullptr;
  }
  auto panel() -> QQuickItem* { return item(QStringLiteral("semanticSettingsPanel")); }
  auto controller() -> StandInLabelController* { return &controller_; }
  auto policy() -> QObject* { return object(QStringLiteral("policyStandIn")); }
  auto dialog() -> QObject* { return object(QStringLiteral("labelDialog")); }
  void Set(QObject* target, const char* name, const QVariant& value) {
    ASSERT_NE(target, nullptr);
    target->setProperty(name, value);
    ProcessEvents();
  }

  StandInLabelController controller_;
  QQmlApplicationEngine  engine_;
  QQuickWindow*          window_ = nullptr;
  QStringList            warnings_;
};

auto Text(QObject* item) -> QString {
  return item != nullptr ? item->property("text").toString() : QString();
}

// The count label of a LegendItem (its `countLabel` alias).
auto LegendCount(QObject* legend) -> QString {
  return legend != nullptr ? Text(legend->property("countLabel").value<QObject*>()) : QString();
}

auto Visible(QQuickItem* item) -> bool { return item != nullptr && item->isVisible(); }
auto Enabled(QQuickItem* item) -> bool { return item != nullptr && item->isEnabled(); }

void Press(QQuickItem* button) {
  ASSERT_NE(button, nullptr);
  ASSERT_TRUE(button->isVisible());
  ASSERT_TRUE(button->isEnabled());
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  ProcessEvents();
}

// ── Page ────────────────────────────────────────────────────────────────────

TEST(SemanticSettingsQmlTest, PageFitsTheSettingsPageAreaWithoutScrolling) {
  SemanticSettingsHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();

  EXPECT_LE(harness.panel()->implicitHeight(), kSettingsPageHeight);

  // A running generation keeps the height of the AI labels tile.
  const qreal idle_tile_height = harness.item(QStringLiteral("semanticLabelsTile"))->height();
  harness.Set(harness.controller(), "total", 565);
  harness.Set(harness.controller(), "running", true);
  EXPECT_DOUBLE_EQ(harness.item(QStringLiteral("semanticLabelsTile"))->height(), idle_tile_height);
  EXPECT_LE(harness.panel()->implicitHeight(), kSettingsPageHeight);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(SemanticSettingsQmlTest, PhotoCountLegendAndSplitBarFollowTheControllerCounts) {
  SemanticSettingsHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();

  EXPECT_GE(harness.controller()->refresh_calls_, 1);
  EXPECT_EQ(Text(harness.item(QStringLiteral("semanticPhotoCount"))), QStringLiteral("2,975"));
  EXPECT_FALSE(Visible(harness.item(QStringLiteral("semanticExactPhotoCount"))));
  EXPECT_EQ(LegendCount(harness.item(QStringLiteral("semanticLabeledLegend"))),
            QStringLiteral("2,410"));
  EXPECT_EQ(LegendCount(harness.item(QStringLiteral("semanticUnlabeledLegend"))),
            QStringLiteral("565"));

  // The two parts split the track (less the 3 px gap) in the ratio of the counts.
  QQuickItem* bar = harness.item(QStringLiteral("semanticCoverageBar"));
  ASSERT_NE(bar, nullptr);
  auto* first  = bar->findChild<QQuickItem*>(QStringLiteral("splitBarFirst"));
  auto* second = bar->findChild<QQuickItem*>(QStringLiteral("splitBarSecond"));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_GT(bar->width(), 100.0);
  EXPECT_NEAR(first->width(), (bar->width() - 3.0) * 2410.0 / 2975.0, 0.01);
  EXPECT_NEAR(second->width(), (bar->width() - 3.0) * 565.0 / 2975.0, 0.01);
  EXPECT_NEAR(second->x() + second->width(), bar->width(), 0.01);

  // With every photo labeled, the first part fills the track without a gap.
  harness.Set(harness.controller(), "albumLabeledCount", 2975);
  harness.Set(harness.controller(), "albumUnlabeledCount", 0);
  EXPECT_NEAR(first->width(), bar->width(), 0.01);
  EXPECT_NEAR(second->width(), 0.0, 0.01);
}

TEST(SemanticSettingsQmlTest, LargePhotoCountsShowAShortFormAndTheExactCount) {
  SemanticSettingsHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();
  QQuickItem* count = harness.item(QStringLiteral("semanticPhotoCount"));
  QQuickItem* exact = harness.item(QStringLiteral("semanticExactPhotoCount"));

  const struct {
    int         value;
    const char* english;
    const char* chinese;
  } cases[] = {
      {9999, "9,999", "9,999"},          {12840, "12.8K", "1.28万"},
      {99999, "99.9K", "9.99万"},        {102345, "102K", "10.2万"},
      {1250000, "1.25M", "125万"},       {123456789, "123M", "1.23亿"},
  };
  for (const auto& entry : cases) {
    harness.Set(harness.controller(), "albumTotalCount", entry.value);
    harness.Set(harness.panel(), "languageCode", QStringLiteral("en"));
    EXPECT_EQ(Text(count), QString::fromUtf8(entry.english)) << entry.value;
    harness.Set(harness.panel(), "languageCode", QStringLiteral("zh_CN"));
    EXPECT_EQ(Text(count), QString::fromUtf8(entry.chinese)) << entry.value;
    EXPECT_EQ(Visible(exact), entry.value >= 10000) << entry.value;
    if (entry.value >= 10000) {
      EXPECT_EQ(Text(exact), QLocale().toString(entry.value)) << entry.value;
    }
  }
}

TEST(SemanticSettingsQmlTest, GenerateNeedsPhotosWithoutLabelsAndRegenerateNeedsAnyPhoto) {
  SemanticSettingsHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();
  QQuickItem* generate   = harness.item(QStringLiteral("semanticGenerateButton"));
  QQuickItem* regenerate = harness.item(QStringLiteral("semanticRegenerateButton"));

  EXPECT_TRUE(Enabled(generate));
  EXPECT_TRUE(Enabled(regenerate));
  Press(generate);
  Press(regenerate);
  EXPECT_EQ(harness.controller()->start_calls_, (std::vector<bool>{false, true}));

  // Every photo has labels: only "Regenerate all" is available.
  harness.Set(harness.controller(), "albumUnlabeledCount", 0);
  EXPECT_FALSE(Enabled(generate));
  EXPECT_TRUE(Enabled(regenerate));

  // No photo: neither button.
  harness.Set(harness.controller(), "albumTotalCount", 0);
  EXPECT_FALSE(Enabled(regenerate));

  // The interaction policy refuses a generation: neither button.
  harness.Set(harness.controller(), "albumTotalCount", 2975);
  harness.Set(harness.controller(), "albumUnlabeledCount", 565);
  harness.Set(harness.policy(), "canRunSemanticGeneration", false);
  EXPECT_FALSE(Enabled(generate));
  EXPECT_FALSE(Enabled(regenerate));
}

TEST(SemanticSettingsQmlTest, RunningGenerationShowsProgressRemainingTimeAndCancel) {
  SemanticSettingsHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();
  StandInLabelController* controller = harness.controller();
  harness.Set(controller, "total", 98410);
  harness.Set(controller, "embedded", 24000);
  harness.Set(controller, "skipped", 6);
  harness.Set(controller, "failed", 4);
  harness.Set(controller, "canceled", 2);
  harness.Set(controller, "running", true);

  QQuickItem* progress = harness.item(QStringLiteral("semanticProgressBar"));
  ASSERT_NE(progress, nullptr);
  EXPECT_TRUE(Visible(progress));
  EXPECT_FALSE(Visible(harness.item(QStringLiteral("semanticCoverageBar"))));
  EXPECT_FALSE(Visible(harness.item(QStringLiteral("semanticGenerateButton"))));
  EXPECT_FALSE(Visible(harness.item(QStringLiteral("semanticLabeledLegend"))));
  EXPECT_EQ(Text(harness.item(QStringLiteral("semanticRunningCount"))), QStringLiteral("98,410"));
  EXPECT_NEAR(progress->property("progressValue").toDouble(), 24012.0 * 100.0 / 98410.0, 1e-6);
  EXPECT_FALSE(progress->property("indeterminate").toBool());
  EXPECT_EQ(progress->property("leadingText").toString(), QStringLiteral("24,012 / 98,410"));

  const struct {
    int         seconds;
    QString     expected;
  } times[] = {
      {-1, FormatText("Estimating the remaining time")},
      {30, FormatText("Less than a minute remaining")},
      {180, FormatText("About %1 min remaining").arg(3)},
      {7200, FormatText("About %1 h remaining").arg(2)},
      {7800, FormatText("About %1 h %2 min remaining").arg(2).arg(10)},
  };
  for (const auto& entry : times) {
    harness.Set(controller, "remainingSeconds", entry.seconds);
    EXPECT_EQ(progress->property("etaText").toString(), entry.expected) << entry.seconds;
  }

  QSignalSpy background(harness.panel(), SIGNAL(backgroundRequested()));
  Press(harness.item(QStringLiteral("semanticBackgroundButton")));
  EXPECT_EQ(background.count(), 1);
  Press(harness.item(QStringLiteral("semanticCancelButton")));
  EXPECT_EQ(controller->cancel_calls_, 1);

  // The end of the generation brings the coverage and the generate buttons back.
  harness.Set(controller, "running", false);
  EXPECT_TRUE(Visible(harness.item(QStringLiteral("semanticCoverageBar"))));
  EXPECT_TRUE(Visible(harness.item(QStringLiteral("semanticGenerateButton"))));
  EXPECT_FALSE(Visible(progress));
}

TEST(SemanticSettingsQmlTest, ModelRowShowsTheActiveModelOrWhyThereIsNone) {
  SemanticSettingsHarness harness;
  ASSERT_NE(harness.panel(), nullptr) << harness.warnings_.join('\n').toStdString();
  QQuickItem* name = harness.item(QStringLiteral("semanticActiveModelName"));

  EXPECT_EQ(Text(name), QStringLiteral("SigLIP2 B/32 256 Multilingual"));

  harness.Set(harness.controller(), "activeModelKey", QString());
  EXPECT_EQ(Text(name), PanelText("No model"));

  harness.Set(harness.panel(), "projectOpen", false);
  EXPECT_EQ(Text(name), PanelText("No open project"));

  QSignalSpy manage(harness.panel(), SIGNAL(aiModelsPageRequested()));
  Press(harness.item(QStringLiteral("semanticOpenAiModelsButton")));
  EXPECT_EQ(manage.count(), 1);
  EXPECT_EQ(Text(harness.item(QStringLiteral("semanticOpenAiModelsButton"))),
            PanelText("Manage"));
  EXPECT_NE(harness.item(QStringLiteral("sidecarIdleStopCombo")), nullptr);
  EXPECT_NE(harness.item(QStringLiteral("semanticImportCombo")), nullptr);

  // The controls of the rows share one right column of 220 px.
  QQuickItem* idle_stop = harness.item(QStringLiteral("sidecarIdleStopControl"));
  QQuickItem* imports   = harness.item(QStringLiteral("semanticImportControl"));
  ASSERT_NE(idle_stop, nullptr);
  ASSERT_NE(imports, nullptr);
  EXPECT_DOUBLE_EQ(idle_stop->width(), 220.0);
  EXPECT_DOUBLE_EQ(imports->width(), 220.0);
  EXPECT_DOUBLE_EQ(idle_stop->mapToScene(QPointF(idle_stop->width(), 0)).x(),
                   imports->mapToScene(QPointF(imports->width(), 0)).x());
}

// ── AI label dialog ─────────────────────────────────────────────────────────

TEST(SemanticSettingsQmlTest, LabelDialogPromptsWithTheCountAndShowsProgress) {
  SemanticSettingsHarness harness;
  QObject* dialog = harness.dialog();
  ASSERT_NE(dialog, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.Set(dialog, "pendingCount", 12840);
  harness.Set(dialog, "promptVisible", true);
  ProcessEvents(150);
  EXPECT_EQ(Text(harness.item(QStringLiteral("semanticGenerationDialogPendingCount"))),
            QStringLiteral("12,840"));
  QQuickItem* generate = harness.item(QStringLiteral("semanticGenerationDialogGenerateButton"));
  EXPECT_TRUE(Enabled(generate));
  harness.Set(dialog, "pendingCount", 0);
  EXPECT_FALSE(Enabled(generate));
  harness.Set(dialog, "pendingCount", 12840);

  QSignalSpy start(dialog, SIGNAL(startRequested(bool)));
  Press(generate);
  ASSERT_EQ(start.count(), 1);
  EXPECT_FALSE(start.front().front().toBool());

  // The controller starts: the progress view shows completed / total and the remaining time.
  harness.Set(dialog, "total", 12840);
  harness.Set(dialog, "embedded", 5393);
  harness.Set(dialog, "remainingSeconds", 60);
  harness.Set(dialog, "generationRunning", true);
  harness.Set(dialog, "promptVisible", false);
  EXPECT_EQ(Text(harness.item(QStringLiteral("semanticGenerationDialogProgressText"))),
            QStringLiteral("5,393 / 12,840"));
  EXPECT_EQ(Text(harness.item(QStringLiteral("semanticGenerationDialogRemainingText"))),
            FormatText("About %1 min remaining").arg(1));
  EXPECT_FALSE(Visible(generate));

  QSignalSpy cancel(dialog, SIGNAL(cancelRequested()));
  Press(harness.item(QStringLiteral("semanticGenerationDialogSkipButton")));
  EXPECT_EQ(cancel.count(), 1);

  // "Move task to background" closes the dialog while the generation continues.
  Press(harness.item(QStringLiteral("semanticGenerationDialogBackgroundButton")));
  ProcessEvents(150);
  EXPECT_TRUE(dialog->property("backgrounded").toBool());
  EXPECT_FALSE(dialog->property("showingGeneration").toBool());
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test

#include "semantic_settings_qml_test.moc"
