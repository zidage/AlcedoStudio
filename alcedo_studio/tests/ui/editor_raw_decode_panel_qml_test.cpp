//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

class RawDecodeSession final : public QObject, public IEditorAdjustmentSubmitter {
  Q_OBJECT
  Q_PROPERTY(
      QVariantMap adjustmentSnapshot READ adjustmentSnapshot NOTIFY adjustmentSnapshotChanged)
  Q_PROPERTY(quint64 snapshotRevision READ snapshotRevision NOTIFY adjustmentSnapshotChanged)
  Q_PROPERTY(QString activeAdjustmentPanel READ activeAdjustmentPanel WRITE setActiveAdjustmentPanel
                 NOTIFY activeAdjustmentPanelChanged)

 public:
  struct Call {
    QString field_key;
    QString params_json;
    bool    settled = false;
  };

  explicit RawDecodeSession(QVariantMap snapshot, QObject* parent = nullptr)
      : QObject(parent), snapshot_(std::move(snapshot)) {}

  [[nodiscard]] auto adjustmentSnapshot() const -> QVariantMap { return snapshot_; }
  [[nodiscard]] auto snapshotRevision() const -> quint64 { return snapshot_revision_; }
  [[nodiscard]] auto activeAdjustmentPanel() const -> QString { return active_panel_; }
  void               setActiveAdjustmentPanel(const QString& panel) {
    if (active_panel_ == panel) {
      return;
    }
    active_panel_ = panel;
    emit activeAdjustmentPanelChanged();
  }
  [[nodiscard]] auto canEdit() const -> bool override { return true; }

  bool               submitPatch(QString fieldKey, QString paramsJson, bool settled) override {
    calls.push_back({std::move(fieldKey), std::move(paramsJson), settled});
    if (settled) {
      const auto& call     = calls.back();
      const auto  document = QJsonDocument::fromJson(call.params_json.toUtf8());
      if (document.isObject()) {
        snapshot_.insert(call.field_key, document.object().toVariantMap());
        ++snapshot_revision_;
        emit adjustmentSnapshotChanged();
      }
    }
    return true;
  }

  void SaveReplayAndReopen() { Publish(snapshot_); }

  void SaveVersion() { saved_version_ = snapshot_; }

  void ReconstructSavedVersion() { Publish(saved_version_); }

  void SwitchImage(const QVariantMap& snapshot) { Publish(snapshot); }

  void ReopenSavedImage() { Publish(saved_version_); }

  void Publish(const QVariantMap& snapshot) {
    snapshot_ = snapshot;
    ++snapshot_revision_;
    emit adjustmentSnapshotChanged();
  }

  std::vector<Call> calls;

 signals:
  void adjustmentSnapshotChanged();
  void activeAdjustmentPanelChanged();

 private:
  QVariantMap snapshot_;
  QVariantMap saved_version_;
  quint64     snapshot_revision_ = 1;
  QString     active_panel_      = QStringLiteral("raw");
};

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto AdjustmentStackUrl() -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorAdjustmentStack.qml"));
}

void ProcessEvents(int milliseconds = 50) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto CenterInWindow(QQuickItem* item) -> QPoint {
  return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0)).toPoint();
}

auto RawParams(double userWb, const QString& method, bool highlights) -> QVariantMap {
  // The accelerator backend is a runtime property of the pipeline (user
  // setting), never an edit param; fixtures must not carry it.
  QVariantMap raw;
  raw.insert(QStringLiteral("method"), method);
  raw.insert(QStringLiteral("highlights_reconstruct"), highlights);
  raw.insert(QStringLiteral("use_camera_wb"), false);
  raw.insert(QStringLiteral("user_wb"), userWb);
  raw.insert(QStringLiteral("backend"), QStringLiteral("alcedo"));
  raw.insert(QStringLiteral("decode_res"), QStringLiteral("full"));
  return {{QStringLiteral("raw"), raw}};
}

auto Snapshot(const QString& method, bool highlights) -> QVariantMap {
  QVariantMap snapshot;
  snapshot.insert(QStringLiteral("raw_decode"), RawParams(5120.0, method, highlights));
  return snapshot;
}

class AdjustmentStackHarness {
 public:
  AdjustmentStackHarness(RawDecodeSession* session) {
    static bool registered = false;
    if (!registered) {
      RegisterEditorAdjustmentQmlTypes();
      registered = true;
    }
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());

    QQmlComponent component(&engine_, AdjustmentStackUrl());
    if (component.isError()) {
      errors_ = component.errors();
      return;
    }

    QVariantMap initial_properties;
    initial_properties.insert(QStringLiteral("editorSession"),
                              QVariant::fromValue(static_cast<QObject*>(session)));
    initial_properties.insert(QStringLiteral("controlsEnabled"), true);
    root_.reset(
        qobject_cast<QQuickItem*>(component.createWithInitialProperties(initial_properties)));
    if (!root_) {
      errors_ = component.errors();
      return;
    }
    root_->setWidth(440);
    root_->setHeight(760);
    root_->setParentItem(window_.contentItem());
    window_.resize(440, 760);
    window_.show();
    ProcessEvents(100);
  }

  [[nodiscard]] auto root() const -> QQuickItem* { return root_.get(); }
  [[nodiscard]] auto window() -> QQuickWindow* { return &window_; }
  [[nodiscard]] auto errors() const -> QString {
    QStringList text;
    for (const auto& error : errors_) {
      text.push_back(error.toString());
    }
    return text.join('\n');
  }

  template <typename T>
  T* findObject(const QString& objectName) const {
    return root_ ? root_->findChild<T*>(objectName) : nullptr;
  }

 private:
  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> root_;
  QList<QQmlError>            errors_;
};

auto SelectSegmentedEntry(AdjustmentStackHarness& harness, const QString& objectName,
                          qsizetype index) -> bool {
  auto* switcher = harness.findObject<QQuickItem>(objectName);
  if (switcher == nullptr || !switcher->isEnabled()) {
    return false;
  }
  auto* row = switcher->findChild<QQuickItem*>(QStringLiteral("segmentedCardSwitcherCardRow"));
  if (row == nullptr) {
    return false;
  }
  const auto segments = row->childItems();
  if (index < 0 || index >= segments.size()) {
    return false;
  }
  auto* segment = segments.at(index);
  if (segment == nullptr || !segment->isEnabled()) {
    return false;
  }
  QTest::mouseClick(harness.window(), Qt::LeftButton, {}, CenterInWindow(segment));
  ProcessEvents(80);
  return true;
}

}  // namespace

TEST(EditorRawDecodePanelQmlTest, ControlsEnabledWhenImageSelected) {
  RawDecodeSession       session(Snapshot(QStringLiteral("legacy"), false));
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* method_model = harness.findObject<QObject>(QStringLiteral("rawDemosaicMethodModel"));
  auto* highlights   = harness.findObject<QQuickItem>(QStringLiteral("rawHighlightsControl"));
  auto* status       = harness.findObject<QObject>(QStringLiteral("rawDecodeStatus"));
  auto* geometry_lens =
      harness.findObject<QObject>(QStringLiteral("editorAdjustmentGroupShell_geometry_lens"));
  ASSERT_NE(method_model, nullptr);
  ASSERT_NE(highlights, nullptr);
  ASSERT_NE(geometry_lens, nullptr);

  EXPECT_TRUE(method_model->property("enabled").toBool());
  EXPECT_TRUE(
      highlights->findChild<QQuickItem*>(QStringLiteral("adjustmentToggleSwitch"))->isEnabled());
  EXPECT_EQ(status, nullptr);
  EXPECT_EQ(harness.findObject<QObject>(QStringLiteral("editorAdjustmentGroupShell_raw_lens")),
            nullptr);
  EXPECT_EQ(harness.findObject<QObject>(QStringLiteral("rawLensEnabledModel")), nullptr);
  EXPECT_TRUE(geometry_lens->property("expanded").toBool());
}

TEST(EditorRawDecodePanelQmlTest, UserChangesSubmitCompleteRawOperatorParams) {
  RawDecodeSession       session(Snapshot(QStringLiteral("default"), true));
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  ASSERT_TRUE(session.calls.empty());

  ASSERT_TRUE(SelectSegmentedEntry(harness, QStringLiteral("rawDemosaicMethodControl"), 2));
  ASSERT_EQ(session.calls.size(), 1u);
  EXPECT_EQ(session.calls.back().field_key, QStringLiteral("raw_decode"));
  EXPECT_TRUE(session.calls.back().settled);
  const auto raw_after_method = QJsonDocument::fromJson(session.calls.back().params_json.toUtf8())
                                    .object()
                                    .value(QStringLiteral("raw"))
                                    .toObject();
  EXPECT_EQ(raw_after_method.value(QStringLiteral("method")).toString(),
            QStringLiteral("neural_engine"));
  // The accelerator backend is never part of the edit params.
  EXPECT_FALSE(raw_after_method.contains(QStringLiteral("gpu_backend")));
  EXPECT_FALSE(raw_after_method.contains(QStringLiteral("cuda")));
  EXPECT_FALSE(raw_after_method.contains(QStringLiteral("opencl")));
  EXPECT_TRUE(raw_after_method.contains(QStringLiteral("decode_res")));
  EXPECT_FALSE(raw_after_method.value(QStringLiteral("use_camera_wb")).toBool());
  EXPECT_DOUBLE_EQ(raw_after_method.value(QStringLiteral("user_wb")).toDouble(), 5120.0);

  auto* highlights = harness.findObject<QQuickItem>(QStringLiteral("rawHighlightsControl"));
  ASSERT_NE(highlights, nullptr);
  auto* highlight_switch =
      highlights->findChild<QQuickItem*>(QStringLiteral("adjustmentToggleSwitch"));
  ASSERT_NE(highlight_switch, nullptr);
  QTest::mouseClick(harness.window(), Qt::LeftButton, {}, CenterInWindow(highlight_switch));
  ProcessEvents(80);
  ASSERT_EQ(session.calls.size(), 2u);
  const auto raw_after_toggle = QJsonDocument::fromJson(session.calls.back().params_json.toUtf8())
                                    .object()
                                    .value(QStringLiteral("raw"))
                                    .toObject();
  EXPECT_EQ(raw_after_toggle.value(QStringLiteral("method")).toString(),
            QStringLiteral("neural_engine"));
  EXPECT_FALSE(raw_after_toggle.value(QStringLiteral("highlights_reconstruct")).toBool());
}

TEST(EditorRawDecodePanelQmlTest,
     SnapshotReplayVersionReconstructionImageSwitchAndReopenAreLoadOnly) {
  const auto image_a = Snapshot(QStringLiteral("legacy"), false);
  const auto image_b = Snapshot(QStringLiteral("default"), true);
  RawDecodeSession       session(image_a);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* method     = harness.findObject<QObject>(QStringLiteral("rawDemosaicMethodModel"));
  auto* highlights = harness.findObject<QObject>(QStringLiteral("rawHighlightsModel"));
  ASSERT_NE(method, nullptr);
  ASSERT_NE(highlights, nullptr);

  EXPECT_EQ(method->property("currentValue").toString(), QStringLiteral("legacy"));
  EXPECT_FALSE(highlights->property("value").toBool());
  EXPECT_TRUE(session.calls.empty());

  session.SaveVersion();
  session.SaveReplayAndReopen();
  const auto calls_after_replay = session.calls.size();
  EXPECT_EQ(method->property("currentValue").toString(), QStringLiteral("legacy"));
  EXPECT_EQ(session.calls.size(), calls_after_replay);

  session.SwitchImage(image_b);
  ProcessEvents(100);
  EXPECT_EQ(method->property("currentValue").toString(), QStringLiteral("default"));
  EXPECT_TRUE(highlights->property("value").toBool());
  EXPECT_EQ(session.calls.size(), calls_after_replay);

  session.ReconstructSavedVersion();
  ProcessEvents(100);
  EXPECT_EQ(method->property("currentValue").toString(), QStringLiteral("legacy"));
  EXPECT_FALSE(highlights->property("value").toBool());
  EXPECT_EQ(session.calls.size(), calls_after_replay);

  session.SwitchImage(image_b);
  session.ReopenSavedImage();
  ProcessEvents(100);
  EXPECT_EQ(method->property("currentValue").toString(), QStringLiteral("legacy"));
  EXPECT_FALSE(highlights->property("value").toBool());
  EXPECT_EQ(session.calls.size(), calls_after_replay);
}

}  // namespace alcedo::ui::test

#include "editor_raw_decode_panel_qml_test.moc"
