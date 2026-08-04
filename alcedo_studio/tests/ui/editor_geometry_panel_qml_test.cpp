//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QApplication>
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
#include <QRectF>
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

class GeometrySession final : public QObject, public IEditorAdjustmentSubmitter {
  Q_OBJECT
  Q_PROPERTY(
      QVariantMap adjustmentSnapshot READ adjustmentSnapshot NOTIFY adjustmentSnapshotChanged)
  Q_PROPERTY(quint64 snapshotRevision READ snapshotRevision NOTIFY adjustmentSnapshotChanged)
  Q_PROPERTY(QString activeAdjustmentPanel READ activeAdjustmentPanel WRITE setActiveAdjustmentPanel
                 NOTIFY activeAdjustmentPanelChanged)
  Q_PROPERTY(bool canEdit READ canEdit NOTIFY canEditChanged)

 public:
  struct Call {
    QString field_key;
    QString params;
    bool    settled = false;
  };

  explicit GeometrySession(QVariantMap snapshot = {}, quint64 revision = 0)
      : snapshot_(std::move(snapshot)), revision_(revision) {}

  [[nodiscard]] auto adjustmentSnapshot() const -> QVariantMap { return snapshot_; }
  [[nodiscard]] auto snapshotRevision() const -> quint64 { return revision_; }
  [[nodiscard]] auto activeAdjustmentPanel() const -> QString { return active_panel_; }
  void               setActiveAdjustmentPanel(const QString& panel) {
    if (active_panel_ == panel) {
      return;
    }
    active_panel_ = panel;
    actions.push_back(QStringLiteral("panel:") + panel);
    emit activeAdjustmentPanelChanged();
  }
  [[nodiscard]] auto canEdit() const -> bool override { return can_edit_; }

  Q_INVOKABLE bool   submitPatch(QString fieldKey, QString paramsJson, bool settled) override {
    actions.push_back(QStringLiteral("submit:") + fieldKey);
    calls.push_back({std::move(fieldKey), std::move(paramsJson), settled});
    return can_edit_;
  }

  std::vector<Call> calls;
  QStringList       actions;

 signals:
  void adjustmentSnapshotChanged();
  void activeAdjustmentPanelChanged();
  void canEditChanged();

 private:
  QVariantMap snapshot_;
  quint64     revision_     = 0;
  QString     active_panel_ = QStringLiteral("geometry");
  bool        can_edit_     = true;
};

class FakeGeometryInteraction final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QRectF cropRectNormalized READ cropRectNormalized WRITE setCropRectNormalized NOTIFY
                 cropChanged)
  Q_PROPERTY(float cropRotationDegrees READ cropRotationDegrees WRITE setCropRotationDegrees NOTIFY
                 cropChanged)
  Q_PROPERTY(float metricAspect READ metricAspect NOTIFY cropChanged)

 public:
  [[nodiscard]] auto cropRectNormalized() const -> QRectF { return crop_rect_; }
  [[nodiscard]] auto cropRotationDegrees() const -> float { return rotation_degrees_; }
  [[nodiscard]] auto metricAspect() const -> float { return metric_aspect_; }
  [[nodiscard]] auto cropToolEnabled() const -> bool { return crop_tool_enabled_; }
  [[nodiscard]] auto cropOverlayVisible() const -> bool { return crop_overlay_visible_; }
  [[nodiscard]] auto imageWidth() const -> int { return image_width_; }
  [[nodiscard]] auto imageHeight() const -> int { return image_height_; }

  Q_INVOKABLE void   setMetricAspectForTest(float aspect) {
    metric_aspect_ = aspect;
    emit cropChanged();
  }
  Q_INVOKABLE void setImageSize(int width, int height) {
    image_width_  = width;
    image_height_ = height;
    if (width > 0 && height > 0) {
      metric_aspect_ = static_cast<float>(width) / static_cast<float>(height);
      emit cropChanged();
    }
  }
  Q_INVOKABLE void setCropToolEnabled(bool enabled) { crop_tool_enabled_ = enabled; }
  Q_INVOKABLE void setCropOverlayVisible(bool visible) { crop_overlay_visible_ = visible; }
  Q_INVOKABLE void setCropAspectLock(bool enabled, float aspectRatio) {
    aspect_locked_ = enabled;
    aspect_ratio_  = aspectRatio;
    emit cropChanged();
  }
  Q_INVOKABLE void setViewChangeRoutingEnabled(bool enabled) {
    view_change_routing_enabled_ = enabled;
  }
  Q_INVOKABLE void setCropRectNormalized(const QRectF& rect) {
    crop_rect_ = rect;
    emit cropChanged();
    emit cropRectCommitted(crop_rect_, true);
    if (view_change_routing_enabled_) {
      ++view_change_count_;
    }
  }
  Q_INVOKABLE void setCropRotationDegrees(float degrees) {
    rotation_degrees_ = degrees;
    emit cropChanged();
    emit cropRotationCommitted(rotation_degrees_, true);
    if (view_change_routing_enabled_) {
      ++view_change_count_;
    }
  }
  Q_INVOKABLE void publishCropRect(const QRectF& rect, bool isFinal) {
    crop_rect_ = rect;
    emit cropChanged();
    emit cropRectCommitted(crop_rect_, isFinal);
  }

  [[nodiscard]] auto viewChangeCount() const -> int { return view_change_count_; }
  [[nodiscard]] auto viewChangeRoutingEnabled() const -> bool {
    return view_change_routing_enabled_;
  }

 signals:
  void cropChanged();
  void cropRectCommitted(const QRectF& rect, bool isFinal);
  void cropRotationCommitted(float degrees, bool isFinal);

 private:
  QRectF crop_rect_{0.0, 0.0, 1.0, 1.0};
  float  metric_aspect_               = 2.0F;
  float  rotation_degrees_            = 0.0F;
  float  aspect_ratio_                = 1.0F;
  bool   aspect_locked_               = false;
  bool   crop_tool_enabled_           = false;
  bool   crop_overlay_visible_        = false;
  bool   view_change_routing_enabled_ = true;
  int    view_change_count_           = 0;
  int    image_width_                 = 0;
  int    image_height_                = 0;
};

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto AdjustmentStackUrl() -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorAdjustmentStack.qml"));
}

auto MakeSnapshot(const QString& preset, double x, double y, double width, double height,
                  double angle, const QString& maker, const QString& model, int source_width = 6000,
                  int source_height = 4000) -> QVariantMap {
  QVariantMap cropRect;
  cropRect.insert(QStringLiteral("x"), x);
  cropRect.insert(QStringLiteral("y"), y);
  cropRect.insert(QStringLiteral("w"), width);
  cropRect.insert(QStringLiteral("h"), height);

  QVariantMap aspect;
  aspect.insert(QStringLiteral("width"), 16.0);
  aspect.insert(QStringLiteral("height"), 9.0);

  QVariantMap crop;
  crop.insert(QStringLiteral("enabled"), true);
  crop.insert(QStringLiteral("angle_degrees"), angle);
  crop.insert(QStringLiteral("enable_crop"), true);
  crop.insert(QStringLiteral("crop_rect"), cropRect);
  crop.insert(QStringLiteral("expand_to_fit"), false);
  crop.insert(QStringLiteral("aspect_ratio_preset"), preset);
  crop.insert(QStringLiteral("aspect_ratio"), aspect);
  crop.insert(QStringLiteral("source_size"),
              QVariantMap{{QStringLiteral("width"), source_width},
                          {QStringLiteral("height"), source_height}});
  QVariantMap cropWrapper;
  cropWrapper.insert(QStringLiteral("crop_rotate"), crop);

  QVariantMap lens;
  lens.insert(QStringLiteral("enabled"), false);
  lens.insert(QStringLiteral("lens_maker"), maker);
  lens.insert(QStringLiteral("lens_model"), model);
  QVariantMap lensWrapper;
  lensWrapper.insert(QStringLiteral("lens_calib"), lens);

  QVariantMap snapshot;
  snapshot.insert(QStringLiteral("crop_rotate"), cropWrapper);
  snapshot.insert(QStringLiteral("lens_calib"), lensWrapper);
  return snapshot;
}

class AdjustmentStackHarness {
 public:
  AdjustmentStackHarness(GeometrySession* session, FakeGeometryInteraction* interaction) {
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

    QVariantMap initialProperties;
    initialProperties.insert(QStringLiteral("editorSession"),
                             QVariant::fromValue(static_cast<QObject*>(session)));
    initialProperties.insert(QStringLiteral("interaction"),
                             QVariant::fromValue(static_cast<QObject*>(interaction)));
    initialProperties.insert(QStringLiteral("controlsEnabled"), true);
    root_.reset(
        qobject_cast<QQuickItem*>(component.createWithInitialProperties(initialProperties)));
    if (!root_) {
      errors_ = component.errors();
      return;
    }
    root_->setWidth(400);
    root_->setHeight(700);
    root_->setParentItem(window_.contentItem());
    window_.resize(400, 700);
    window_.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  }

  [[nodiscard]] auto root() const -> QQuickItem* { return root_.get(); }
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

auto ParseParams(const GeometrySession::Call& call) -> QJsonObject {
  return QJsonDocument::fromJson(call.params.toUtf8()).object();
}

}  // namespace

TEST(EditorGeometryPanelQmlTest, GeometryPanelExposesTypedModelsAndEnablesOverlay) {
  GeometrySession         session;
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  ASSERT_NE(harness.findObject<QObject>(QStringLiteral("editorAdjustmentPanel_geometry")), nullptr);
  ASSERT_NE(harness.findObject<QObject>(QStringLiteral("geometryAspectModel")), nullptr);
  ASSERT_NE(harness.findObject<QObject>(QStringLiteral("geometryRotationModel")), nullptr);
  ASSERT_NE(harness.findObject<QObject>(QStringLiteral("geometryLensEnabledModel")), nullptr);
  auto* lens_section =
      harness.findObject<QObject>(QStringLiteral("editorAdjustmentGroupShell_geometry_lens"));
  ASSERT_NE(lens_section, nullptr);
  EXPECT_TRUE(lens_section->property("expanded").toBool());
  EXPECT_TRUE(interaction.cropToolEnabled());
  EXPECT_TRUE(interaction.cropOverlayVisible());
}

TEST(EditorGeometryPanelQmlTest, PanelEnterSyncDoesNotRouteDuplicateCropViewChanges) {
  GeometrySession session(MakeSnapshot(QStringLiteral("ratio_16_9"), 0.2, 0.25, 0.5, 0.4, 8.0,
                                       QStringLiteral(""), QStringLiteral("")),
                          2);
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* geometryPanel =
      harness.findObject<QObject>(QStringLiteral("editorAdjustmentPanel_geometry"));
  ASSERT_NE(geometryPanel, nullptr);

  // Re-enter the panel with a non-default crop so syncToInteraction writes
  // rotation/rect. Session owns the source-frame refresh; panel sync must not
  // count as additional view-change routing (the CUDA overlay race).
  const int before = interaction.viewChangeCount();
  geometryPanel->setProperty("panelActive", false);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  geometryPanel->setProperty("panelActive", true);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

  EXPECT_TRUE(interaction.cropToolEnabled());
  EXPECT_TRUE(interaction.cropOverlayVisible());
  EXPECT_EQ(interaction.viewChangeCount(), before);
  EXPECT_TRUE(interaction.viewChangeRoutingEnabled());
}

TEST(EditorGeometryPanelQmlTest, SnapshotProjectsCropLensAndDoesNotSubmit) {
  GeometrySession session(
      MakeSnapshot(QStringLiteral("ratio_16_9"), 0.1, 0.2, 0.7, 0.6, 12.5,
                   QStringLiteral("Unknown Maker"), QStringLiteral("Unknown Model")),
      4);
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* geometryPanel =
      harness.findObject<QObject>(QStringLiteral("editorAdjustmentPanel_geometry"));
  ASSERT_FALSE(session.adjustmentSnapshot().isEmpty());
  // Stack counts successful fan-outs; first bind applies once (content gate is
  // AdjustmentSnapshotChanged on the controller, not a public revision property).
  EXPECT_GE(harness.root()->property("lastAppliedRevision").toInt(), 0);
  EXPECT_FALSE(geometryPanel->property("inputActive").toBool());
  EXPECT_TRUE(QMetaObject::invokeMethod(
      geometryPanel, "loadFromSnapshot",
      Q_ARG(QVariant, QVariant::fromValue(session.adjustmentSnapshot()))));
  auto* xModel   = harness.findObject<QObject>(QStringLiteral("geometryCropXModel"));
  auto* wModel   = harness.findObject<QObject>(QStringLiteral("geometryCropWidthModel"));
  auto* rotation = harness.findObject<QObject>(QStringLiteral("geometryRotationModel"));
  auto* aspect   = harness.findObject<QObject>(QStringLiteral("geometryAspectModel"));
  auto* lens     = harness.findObject<QObject>(QStringLiteral("geometryLensEnabledModel"));
  auto* brand    = harness.findObject<QObject>(QStringLiteral("geometryLensBrandModel"));
  auto* model    = harness.findObject<QObject>(QStringLiteral("geometryLensModelModel"));
  ASSERT_NE(xModel, nullptr);
  ASSERT_NE(wModel, nullptr);
  ASSERT_NE(rotation, nullptr);
  ASSERT_NE(aspect, nullptr);
  ASSERT_NE(lens, nullptr);
  ASSERT_NE(brand, nullptr);
  ASSERT_NE(model, nullptr);

  EXPECT_NEAR(xModel->property("value").toDouble(), 0.1, 1e-6);
  EXPECT_NEAR(wModel->property("value").toDouble(), 0.7, 1e-6);
  EXPECT_DOUBLE_EQ(rotation->property("value").toDouble(), 12.5);
  EXPECT_EQ(aspect->property("currentValue").toString(), QStringLiteral("ratio_16_9"));
  EXPECT_EQ(geometryPanel->property("sourceImageWidth").toInt(), 6000);
  EXPECT_EQ(geometryPanel->property("sourceImageHeight").toInt(), 4000);
  EXPECT_EQ(interaction.imageWidth(), 6000);
  EXPECT_EQ(interaction.imageHeight(), 4000);
  EXPECT_FALSE(lens->property("value").toBool());
  EXPECT_EQ(brand->property("currentValue").toString(), QStringLiteral("Unknown Maker"));
  EXPECT_EQ(model->property("currentValue").toString(), QStringLiteral("Unknown Model"));
  EXPECT_TRUE(session.calls.empty());
  EXPECT_NEAR(interaction.cropRectNormalized().x(), 0.1, 1e-6);
}

TEST(EditorGeometryPanelQmlTest, SliderEditIsDraftOnlyUntilConfirmPendingCrop) {
  GeometrySession         session(MakeSnapshot(QStringLiteral("free"), 0.0, 0.0, 1.0, 1.0, 0.0,
                                               QStringLiteral(""), QStringLiteral(""), 2731, 4096));
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* geometryPanel =
      harness.findObject<QObject>(QStringLiteral("editorAdjustmentPanel_geometry"));
  auto* xModel     = harness.findObject<QObject>(QStringLiteral("geometryCropXModel"));
  auto* widthModel = harness.findObject<QObject>(QStringLiteral("geometryCropWidthModel"));
  ASSERT_NE(geometryPanel, nullptr);
  ASSERT_NE(xModel, nullptr);
  ASSERT_NE(widthModel, nullptr);

  widthModel->setProperty("value", 0.7);
  ASSERT_TRUE(QMetaObject::invokeMethod(xModel, "editValue", Q_ARG(double, 0.25)));
  EXPECT_NEAR(xModel->property("value").toDouble(), 0.25, 1e-6);
  // Draft-only: slider motion must not touch the pipeline.
  EXPECT_TRUE(session.calls.empty());
  EXPECT_TRUE(geometryPanel->property("draftDirty").toBool());
  EXPECT_NEAR(interaction.cropRectNormalized().x(), 0.25, 1e-6);

  ASSERT_TRUE(QMetaObject::invokeMethod(geometryPanel, "confirmPendingCrop"));
  ASSERT_EQ(session.calls.size(), 1u);
  EXPECT_TRUE(session.calls.back().settled);
  EXPECT_EQ(session.calls.back().field_key, QStringLiteral("crop_rotate"));
  EXPECT_FALSE(geometryPanel->property("draftDirty").toBool());

  const auto crop =
      ParseParams(session.calls.back()).value(QStringLiteral("crop_rotate")).toObject();
  EXPECT_DOUBLE_EQ(
      crop.value(QStringLiteral("crop_rect")).toObject().value(QStringLiteral("x")).toDouble(),
      0.25);
  EXPECT_TRUE(crop.contains(QStringLiteral("angle_degrees")));
  EXPECT_TRUE(crop.contains(QStringLiteral("aspect_ratio_preset")));
  const auto source_size = crop.value(QStringLiteral("source_size")).toObject();
  EXPECT_EQ(source_size.value(QStringLiteral("width")).toInt(), 2731);
  EXPECT_EQ(source_size.value(QStringLiteral("height")).toInt(), 4096);
}

TEST(EditorGeometryPanelQmlTest, OverlayDragIsDraftOnlyAndConfirmSubmitsOneSettledPatch) {
  GeometrySession         session;
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* geometryPanel =
      harness.findObject<QObject>(QStringLiteral("editorAdjustmentPanel_geometry"));
  ASSERT_NE(geometryPanel, nullptr);

  // Mid-drag (isFinal=false) and release (isFinal=true) stay pure UI.
  interaction.publishCropRect(QRectF(0.2, 0.1, 0.4, 0.5), false);
  interaction.publishCropRect(QRectF(0.3, 0.15, 0.5, 0.7), true);
  auto* xModel = harness.findObject<QObject>(QStringLiteral("geometryCropXModel"));
  auto* hModel = harness.findObject<QObject>(QStringLiteral("geometryCropHeightModel"));
  ASSERT_NE(xModel, nullptr);
  ASSERT_NE(hModel, nullptr);
  EXPECT_DOUBLE_EQ(xModel->property("value").toDouble(), 0.3);
  EXPECT_DOUBLE_EQ(hModel->property("value").toDouble(), 0.7);
  EXPECT_TRUE(session.calls.empty());
  EXPECT_TRUE(geometryPanel->property("draftDirty").toBool());
  EXPECT_FALSE(geometryPanel->property("overlayInputActive").toBool());

  ASSERT_TRUE(QMetaObject::invokeMethod(geometryPanel, "confirmPendingCrop"));
  ASSERT_EQ(session.calls.size(), 1u);
  EXPECT_TRUE(session.calls.back().settled);
  EXPECT_EQ(session.calls.back().field_key, QStringLiteral("crop_rotate"));
  EXPECT_FALSE(geometryPanel->property("draftDirty").toBool());
}

TEST(EditorGeometryPanelQmlTest, SelectingAspectPresetResizesDraftWithoutPipelineSubmit) {
  GeometrySession         session;
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* aspect = harness.findObject<QObject>(QStringLiteral("geometryAspectModel"));
  ASSERT_NE(aspect, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(aspect, "selectIndex", Q_ARG(int, 4)));

  const QRectF rect = interaction.cropRectNormalized();
  EXPECT_NEAR((rect.width() / rect.height()) * interaction.metricAspect(), 16.0 / 9.0, 1e-4);
  EXPECT_NEAR(rect.center().x(), 0.5, 1e-4);
  EXPECT_TRUE(session.calls.empty());
}

TEST(EditorGeometryPanelQmlTest, SelectingLandscapePresetOrientsCropForPortraitImage) {
  GeometrySession         session(MakeSnapshot(QStringLiteral("free"), 0.0, 0.0, 1.0, 1.0, 0.0,
                                               QStringLiteral(""), QStringLiteral(""), 3000, 4000));
  FakeGeometryInteraction interaction;
  interaction.setMetricAspectForTest(1.5F);
  AdjustmentStackHarness harness(&session, &interaction);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  EXPECT_EQ(interaction.imageWidth(), 3000);
  EXPECT_EQ(interaction.imageHeight(), 4000);
  auto* aspect = harness.findObject<QObject>(QStringLiteral("geometryAspectModel"));
  ASSERT_NE(aspect, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(aspect, "selectIndex", Q_ARG(int, 4)));

  const QRectF rect = interaction.cropRectNormalized();
  EXPECT_NEAR((rect.width() / rect.height()) * interaction.metricAspect(), 9.0 / 16.0, 1e-4);
  EXPECT_NEAR(rect.center().x(), 0.5, 1e-4);
  EXPECT_TRUE(session.calls.empty());
}

TEST(EditorGeometryPanelQmlTest, ConfirmAndReturnToToneQueuesPanelRefreshBeforeFinalCrop) {
  GeometrySession         session;
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* stack = harness.root();
  auto* geometryPanel =
      harness.findObject<QObject>(QStringLiteral("editorAdjustmentPanel_geometry"));
  auto* xModel = harness.findObject<QObject>(QStringLiteral("geometryCropXModel"));
  ASSERT_NE(geometryPanel, nullptr);
  ASSERT_NE(xModel, nullptr);

  xModel->setProperty("value", 0.22);
  EXPECT_TRUE(geometryPanel->property("draftDirty").toBool());
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("geometry"));

  ASSERT_TRUE(QMetaObject::invokeMethod(stack, "confirmGeometryAndReturnToTone"));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  ASSERT_EQ(session.calls.size(), 1u);
  EXPECT_TRUE(session.calls.back().settled);
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("tone"));
  ASSERT_EQ(session.actions.size(), 2);
  EXPECT_EQ(session.actions.at(0), QStringLiteral("panel:tone"));
  EXPECT_EQ(session.actions.at(1), QStringLiteral("submit:crop_rotate"));
  EXPECT_FALSE(geometryPanel->property("draftDirty").toBool());
  EXPECT_FALSE(interaction.cropToolEnabled());
  EXPECT_FALSE(interaction.cropOverlayVisible());
}

TEST(EditorGeometryPanelQmlTest, LeavingGeometryQueuesPanelRefreshBeforeFinalCrop) {
  GeometrySession         session;
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* stack  = harness.root();
  auto* xModel = harness.findObject<QObject>(QStringLiteral("geometryCropXModel"));
  ASSERT_NE(xModel, nullptr);
  xModel->setProperty("value", 0.18);
  EXPECT_TRUE(session.calls.empty());

  ASSERT_TRUE(QMetaObject::invokeMethod(stack, "selectPanel",
                                        Q_ARG(QVariant, QVariant(QStringLiteral("look")))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  ASSERT_EQ(session.calls.size(), 1u);
  EXPECT_TRUE(session.calls.back().settled);
  EXPECT_EQ(session.calls.back().field_key, QStringLiteral("crop_rotate"));
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("look"));
  ASSERT_EQ(session.actions.size(), 2);
  EXPECT_EQ(session.actions.at(0), QStringLiteral("panel:look"));
  EXPECT_EQ(session.actions.at(1), QStringLiteral("submit:crop_rotate"));
  EXPECT_FALSE(interaction.cropOverlayVisible());
}

TEST(EditorGeometryPanelQmlTest, LensSelectionKeepsLegacyDefaultsAndIsAvailableWhenDisabled) {
  GeometrySession         session;
  FakeGeometryInteraction interaction;
  AdjustmentStackHarness  harness(&session, &interaction);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* enabled = harness.findObject<QObject>(QStringLiteral("geometryLensEnabledModel"));
  auto* brand   = harness.findObject<QObject>(QStringLiteral("geometryLensBrandModel"));
  auto* model   = harness.findObject<QObject>(QStringLiteral("geometryLensModelModel"));
  ASSERT_NE(enabled, nullptr);
  ASSERT_NE(brand, nullptr);
  ASSERT_NE(model, nullptr);

  EXPECT_FALSE(enabled->property("value").toBool());
  EXPECT_TRUE(brand->property("enabled").toBool());
  const auto brand_entries = brand->property("entries").toList();
  ASSERT_GT(brand_entries.size(), 1);

  ASSERT_TRUE(QMetaObject::invokeMethod(brand, "selectIndex", Q_ARG(int, 1)));
  ASSERT_EQ(session.calls.size(), 1u);
  const auto lens =
      ParseParams(session.calls.back()).value(QStringLiteral("lens_calib")).toObject();
  EXPECT_EQ(lens.value(QStringLiteral("lens_maker")).toString(),
            brand_entries.at(1).toMap().value(QStringLiteral("value")).toString());
  EXPECT_TRUE(lens.contains(QStringLiteral("apply_distortion")));
  EXPECT_TRUE(lens.contains(QStringLiteral("lens_profile_db_path")));
  EXPECT_TRUE(model->property("enabled").toBool());
  EXPECT_FALSE(model->property("currentValue").toString().isEmpty());
}

}  // namespace alcedo::ui::test

#include "editor_geometry_panel_qml_test.moc"
