//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_adjustment_header_qml_test.cpp
/// @brief Header EXIF/name layout, stable navbar, LUT load-only re-entry.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

constexpr auto kEmDash = "\xE2\x80\x94";

class FakeMaskCreation final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool bodyVisible READ bodyVisible NOTIFY maskCreationChanged)
  Q_PROPERTY(bool maskControlsActive READ maskControlsActive NOTIFY maskCreationChanged)
  Q_PROPERTY(bool creating READ creating NOTIFY maskCreationChanged)
  Q_PROPERTY(QString toolKind READ toolKind NOTIFY maskCreationChanged)
  Q_PROPERTY(QString selectedMaskId READ selectedMaskId NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal majorRadiusPercent READ majorRadiusPercent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal minorRadiusPercent READ minorRadiusPercent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal rotationDegrees READ rotationDegrees NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal innerFeatherPercent READ innerFeatherPercent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal outerFeatherPercent READ outerFeatherPercent NOTIFY maskCreationChanged)
  Q_PROPERTY(qreal transitionPercent READ transitionPercent NOTIFY maskCreationChanged)

 public:
  auto             bodyVisible() const -> bool { return active_; }
  auto             maskControlsActive() const -> bool { return active_; }
  auto             creating() const -> bool { return creating_; }
  auto             toolKind() const -> QString { return tool_kind_; }
  auto             selectedMaskId() const -> QString { return selected_mask_id_; }
  auto             majorRadiusPercent() const -> qreal { return 28.0; }
  auto             minorRadiusPercent() const -> qreal { return 18.0; }
  auto             rotationDegrees() const -> qreal { return 20.0; }
  auto             innerFeatherPercent() const -> qreal { return 25.0; }
  auto             outerFeatherPercent() const -> qreal { return 20.0; }
  auto             transitionPercent() const -> qreal { return 30.0; }
  auto             finishCount() const -> int { return finish_count_; }

  Q_INVOKABLE void beginRadial() { Open(QStringLiteral("radial"), true); }
  Q_INVOKABLE void beginLinear() { Open(QStringLiteral("linear"), true); }
  Q_INVOKABLE void cancel() { Close(); }
  Q_INVOKABLE void hideBody() { finishBody(); }
  Q_INVOKABLE void finishBody() {
    ++finish_count_;
    Close();
  }
  Q_INVOKABLE void beginMajorRadius() {}
  Q_INVOKABLE void updateMajorRadius(qreal) {}
  Q_INVOKABLE void beginMinorRadius() {}
  Q_INVOKABLE void updateMinorRadius(qreal) {}
  Q_INVOKABLE void beginRotation() {}
  Q_INVOKABLE void updateRotation(qreal) {}
  Q_INVOKABLE void beginInnerFeather() {}
  Q_INVOKABLE void updateInnerFeather(qreal) {}
  Q_INVOKABLE void beginOuterFeather() {}
  Q_INVOKABLE void updateOuterFeather(qreal) {}
  Q_INVOKABLE void beginTransition() {}
  Q_INVOKABLE void updateTransition(qreal) {}
  Q_INVOKABLE void finishAnalyticControl() {}

  void             SelectExisting(const QString& kind) { Open(kind, false); }

 signals:
  void maskCreationChanged();

 private:
  void Open(const QString& kind, bool creating) {
    active_           = true;
    creating_         = creating;
    tool_kind_        = kind;
    selected_mask_id_ = creating ? QString{} : QStringLiteral("mask.selected");
    emit maskCreationChanged();
  }
  void Close() {
    active_   = false;
    creating_ = false;
    tool_kind_.clear();
    selected_mask_id_.clear();
    emit maskCreationChanged();
  }

  bool    active_   = false;
  bool    creating_ = false;
  QString tool_kind_;
  QString selected_mask_id_;
  int     finish_count_ = 0;
};

class HeaderSession final : public QObject, public IEditorAdjustmentSubmitter {
  Q_OBJECT
  Q_PROPERTY(
      QVariantMap adjustmentSnapshot READ adjustmentSnapshot NOTIFY AdjustmentSnapshotChanged)
  Q_PROPERTY(quint64 snapshotRevision READ snapshotRevision NOTIFY AdjustmentSnapshotChanged)
  Q_PROPERTY(QString activeAdjustmentPanel READ activeAdjustmentPanel WRITE setActiveAdjustmentPanel
                 NOTIFY activeAdjustmentPanelChanged)
  Q_PROPERTY(QString exifLineText READ exifLineText NOTIFY ImageExifChanged)
  Q_PROPERTY(QString exifShutterText READ exifShutterText NOTIFY ImageExifChanged)
  Q_PROPERTY(QString exifIsoText READ exifIsoText NOTIFY ImageExifChanged)
  Q_PROPERTY(QString exifApertureText READ exifApertureText NOTIFY ImageExifChanged)
  Q_PROPERTY(QString exifFocalText READ exifFocalText NOTIFY ImageExifChanged)
  Q_PROPERTY(QObject* maskCreation READ maskCreation CONSTANT)

 public:
  explicit HeaderSession(QObject* mask_creation = nullptr) : mask_creation_(mask_creation) {}
  auto adjustmentSnapshot() const -> QVariantMap { return snapshot_; }
  auto snapshotRevision() const -> quint64 { return revision_; }
  auto activeAdjustmentPanel() const -> QString { return panel_; }
  void setActiveAdjustmentPanel(const QString& panel) {
    if (panel_ == panel) {
      return;
    }
    panel_ = panel;
    emit activeAdjustmentPanelChanged();
  }
  auto exifLineText() const -> QString { return line_; }
  auto exifShutterText() const -> QString { return shutter_; }
  auto exifIsoText() const -> QString { return iso_; }
  auto exifApertureText() const -> QString { return aperture_; }
  auto exifFocalText() const -> QString { return focal_; }
  auto maskCreation() const -> QObject* { return mask_creation_; }

  void setExif(const QString& shutter, const QString& iso, const QString& aperture,
               const QString& focal) {
    shutter_           = shutter;
    iso_               = iso;
    aperture_          = aperture;
    focal_             = focal;
    const QString dash = QString::fromUtf8(kEmDash);
    QStringList   parts;
    for (const auto& token : {focal, aperture, shutter, iso}) {
      if (!token.isEmpty() && token != dash) {
        parts.push_back(token);
      }
    }
    line_ = parts.isEmpty() ? dash : parts.join(QLatin1Char(' '));
    emit ImageExifChanged();
  }

  void setSnapshot(QVariantMap snapshot) {
    snapshot_ = std::move(snapshot);
    ++revision_;
    emit AdjustmentSnapshotChanged();
  }

  auto submitWrite(QString fieldKey, alcedo::EditorParameterWrite write, bool settled)
      -> bool override {
    static_cast<void>(fieldKey);
    static_cast<void>(write);
    static_cast<void>(settled);
    ++submit_count_;
    return true;
  }
  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    static_cast<void>(fieldKey);
    static_cast<void>(paramsJson);
    static_cast<void>(settled);
    ++submit_count_;
    return true;
  }
  auto canEdit() const -> bool override { return true; }
  auto submitCount() const -> int { return submit_count_; }

 signals:
  void AdjustmentSnapshotChanged();
  void activeAdjustmentPanelChanged();
  void ImageExifChanged();

 private:
  QVariantMap snapshot_;
  quint64     revision_      = 0;
  QString     panel_         = QStringLiteral("tone");
  QString     line_          = QString::fromUtf8(kEmDash);
  QString     shutter_       = QString::fromUtf8(kEmDash);
  QString     iso_           = QString::fromUtf8(kEmDash);
  QString     aperture_      = QString::fromUtf8(kEmDash);
  QString     focal_         = QString::fromUtf8(kEmDash);
  int         submit_count_  = 0;
  QObject*    mask_creation_ = nullptr;
};

class FakeNodeController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString selectedNodeName READ selectedNodeName NOTIFY SelectionChanged)
  Q_PROPERTY(QString selectedNodeKind READ selectedNodeKind NOTIFY SelectionChanged)
  Q_PROPERTY(
      QStringList supportedAdjustmentPanels READ supportedAdjustmentPanels NOTIFY SelectionChanged)
  Q_PROPERTY(QVariantList selectedNodeMasks READ selectedNodeMasks NOTIFY SelectionChanged)

 public:
  auto selectedNodeName() const -> QString { return name_; }
  auto selectedNodeKind() const -> QString { return kind_; }
  auto supportedAdjustmentPanels() const -> QStringList { return panels_; }
  auto selectedNodeMasks() const -> QVariantList { return masks_; }

  void setSelection(const QString& name, const QString& kind, const QStringList& panels) {
    name_   = name;
    kind_   = kind;
    panels_ = panels;
    emit SelectionChanged();
  }

 signals:
  void SelectionChanged();

 private:
  QString      name_   = QStringLiteral("Color Grade");
  QString      kind_   = QStringLiteral("colorGrade");
  QStringList  panels_ = {QStringLiteral("tone"), QStringLiteral("look"), QStringLiteral("lut"),
                          QStringLiteral("masks")};
  QVariantList masks_;
};

class FakeLutCatalogModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
  Q_PROPERTY(
      QString selectedPath READ selectedPath WRITE setSelectedPath NOTIFY selectedPathChanged)
  Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedPathChanged)

 public:
  FakeLutCatalogModel() {
    QVariantList rows;
    for (int i = 0; i < 40; ++i) {
      QVariantMap row;
      row.insert(QStringLiteral("kind"), QStringLiteral("file"));
      row.insert(QStringLiteral("path"),
                 QStringLiteral("D:/fake/lut_%1.cube").arg(i, 2, 10, QChar('0')));
      row.insert(QStringLiteral("name"), QStringLiteral("LUT %1").arg(i, 2, 10, QChar('0')));
      row.insert(QStringLiteral("valid"), true);
      rows.push_back(row);
    }
    entries_ = rows;
  }

  auto entries() const -> QVariantList { return entries_; }
  auto selectedPath() const -> QString { return selected_path_; }
  void setSelectedPath(const QString& path) {
    if (selected_path_ == path) {
      return;
    }
    selected_path_ = path;
    emit selectedPathChanged();
  }
  auto selectedIndex() const -> int {
    for (int i = 0; i < entries_.size(); ++i) {
      if (entries_[i].toMap().value(QStringLiteral("path")).toString() == selected_path_) {
        return i;
      }
    }
    return -1;
  }
  Q_INVOKABLE void refresh(bool /*force*/) {}
  Q_INVOKABLE bool isFavoritePath(const QString&) const { return false; }

 signals:
  void entriesChanged();
  void selectedPathChanged();

 private:
  QVariantList entries_;
  QString      selected_path_;
};

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

void RegisterQmlTypesOnce() {
  static std::once_flag once;
  std::call_once(once, [] { RegisterEditorAdjustmentQmlTypes(); });
}

class StackHarness {
 public:
  StackHarness(HeaderSession* session, FakeNodeController* nodes, int width) {
    RegisterQmlTypesOnce();
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());

    QQmlComponent component(
        &engine_,
        QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorAdjustmentStack.qml")));
    if (component.isError()) {
      errors_ = component.errors();
      return;
    }

    QVariantMap initial;
    initial.insert(QStringLiteral("editorSession"),
                   QVariant::fromValue(static_cast<QObject*>(session)));
    initial.insert(QStringLiteral("nodeController"),
                   QVariant::fromValue(static_cast<QObject*>(nodes)));
    initial.insert(QStringLiteral("controlsEnabled"), true);
    root_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(initial)));
    if (!root_) {
      errors_ = component.errors();
      return;
    }
    root_->setWidth(width);
    root_->setHeight(720);
    root_->setParentItem(window_.contentItem());
    window_.resize(width, 720);
    window_.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  }

  auto root() const -> QQuickItem* { return root_.get(); }
  auto errors() const -> QString {
    QStringList text;
    for (const auto& error : errors_) {
      text.push_back(error.toString());
    }
    return text.join('\n');
  }

  template <typename T = QQuickItem>
  auto find(const QString& name) const -> T* {
    return root_ ? root_->findChild<T*>(name) : nullptr;
  }

 private:
  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> root_;
  QList<QQmlError>            errors_;
};

void ExpectNavPresent(const StackHarness& harness) {
  for (const auto& panel : {QStringLiteral("tone"), QStringLiteral("look"), QStringLiteral("lut"),
                            QStringLiteral("display"), QStringLiteral("geometry"),
                            QStringLiteral("raw"), QStringLiteral("masks")}) {
    ASSERT_NE(harness.find(QStringLiteral("editorAdjustmentNav_") + panel), nullptr)
        << panel.toStdString();
  }
}

void ExpectHeaderAboveNav(const StackHarness& harness) {
  auto* header = harness.find(QStringLiteral("editorAdjustmentHeader"));
  auto* nav    = harness.find(QStringLiteral("editorAdjustmentNav"));
  ASSERT_NE(header, nullptr);
  ASSERT_NE(nav, nullptr);
  ASSERT_NE(harness.root(), nullptr);
  const QPointF header_bottom = header->mapToItem(harness.root(), QPointF(0, header->height()));
  const QPointF nav_top       = nav->mapToItem(harness.root(), QPointF(0, 0));
  EXPECT_LE(header_bottom.y(), nav_top.y() + 0.5);
  EXPECT_GE(header->height(), AppTheme::Instance().editorAdjustmentHeaderMinHeight() - 0.5);
}

void ExpectMaskToolButtons(const StackHarness& harness) {
  auto* brush    = harness.find(QStringLiteral("editorAdjustmentHeaderBrushButton"));
  auto* radial   = harness.find(QStringLiteral("editorAdjustmentHeaderRadialButton"));
  auto* gradient = harness.find(QStringLiteral("editorAdjustmentHeaderGradientButton"));
  ASSERT_NE(brush, nullptr);
  ASSERT_NE(radial, nullptr);
  ASSERT_NE(gradient, nullptr);
  EXPECT_EQ(brush->property("iconSrc").toUrl(), QUrl(QStringLiteral("qrc:/mask_icons/brush.svg")));
  EXPECT_EQ(radial->property("iconSrc").toUrl(),
            QUrl(QStringLiteral("qrc:/mask_icons/radial.svg")));
  EXPECT_EQ(gradient->property("iconSrc").toUrl(),
            QUrl(QStringLiteral("qrc:/mask_icons/gradient.svg")));
}

void ExpectExifAboveNameRow(const StackHarness& harness) {
  auto* exif = harness.find(QStringLiteral("editorAdjustmentHeaderExif"));
  auto* name = harness.find(QStringLiteral("editorAdjustmentHeaderNameRow"));
  ASSERT_NE(exif, nullptr);
  ASSERT_NE(name, nullptr);
  ASSERT_NE(harness.root(), nullptr);
  const QPointF exif_bottom = exif->mapToItem(harness.root(), QPointF(0, exif->height()));
  const QPointF name_top    = name->mapToItem(harness.root(), QPointF(0, 0));
  EXPECT_LE(exif_bottom.y(), name_top.y() + 0.5);
}

void ExpectFourEqualExifTokens(const StackHarness& harness, const QString& focal,
                               const QString& aperture, const QString& shutter,
                               const QString& iso) {
  auto* focal_item    = harness.find(QStringLiteral("editorAdjustmentHeaderFocal"));
  auto* aperture_item = harness.find(QStringLiteral("editorAdjustmentHeaderAperture"));
  auto* shutter_item  = harness.find(QStringLiteral("editorAdjustmentHeaderShutter"));
  auto* iso_item      = harness.find(QStringLiteral("editorAdjustmentHeaderIso"));
  ASSERT_NE(focal_item, nullptr);
  ASSERT_NE(aperture_item, nullptr);
  ASSERT_NE(shutter_item, nullptr);
  ASSERT_NE(iso_item, nullptr);
  EXPECT_EQ(focal_item->property("text").toString(), focal);
  EXPECT_EQ(aperture_item->property("text").toString(), aperture);
  EXPECT_EQ(shutter_item->property("text").toString(), shutter);
  EXPECT_EQ(iso_item->property("text").toString(), iso);
  EXPECT_EQ(focal_item->property("font").value<QFont>().family(),
            AppTheme::Instance().monoFontFamily());
  EXPECT_NEAR(focal_item->width(), aperture_item->width(), 1.0);
  EXPECT_NEAR(aperture_item->width(), shutter_item->width(), 1.0);
  EXPECT_NEAR(shutter_item->width(), iso_item->width(), 1.0);
}

void ExpectExifRowMatchesNameRowWidth(const StackHarness& harness) {
  auto* exif = harness.find(QStringLiteral("editorAdjustmentHeaderExif"));
  auto* name = harness.find(QStringLiteral("editorAdjustmentHeaderNameRow"));
  ASSERT_NE(exif, nullptr);
  ASSERT_NE(name, nullptr);
  EXPECT_NEAR(exif->width(), name->width(), 0.5);
  EXPECT_GT(exif->width(), 1.0);
}

void ExpectNameThenMaskTools(const StackHarness& harness) {
  auto* root     = harness.root();
  auto* name     = harness.find(QStringLiteral("editorAdjustmentHeaderNodeName"));
  auto* brush    = harness.find(QStringLiteral("editorAdjustmentHeaderBrushButton"));
  auto* radial   = harness.find(QStringLiteral("editorAdjustmentHeaderRadialButton"));
  auto* gradient = harness.find(QStringLiteral("editorAdjustmentHeaderGradientButton"));
  ASSERT_NE(root, nullptr);
  ASSERT_NE(name, nullptr);
  ASSERT_NE(brush, nullptr);
  ASSERT_NE(radial, nullptr);
  ASSERT_NE(gradient, nullptr);
  EXPECT_EQ(harness.find(QStringLiteral("editorAdjustmentHeaderDivider")), nullptr);
  const qreal name_right    = name->mapToItem(root, QPointF(name->width(), 0)).x();
  const qreal brush_left    = brush->mapToItem(root, QPointF(0, 0)).x();
  const qreal brush_right   = brush->mapToItem(root, QPointF(brush->width(), 0)).x();
  const qreal radial_left   = radial->mapToItem(root, QPointF(0, 0)).x();
  const qreal radial_right  = radial->mapToItem(root, QPointF(radial->width(), 0)).x();
  const qreal gradient_left = gradient->mapToItem(root, QPointF(0, 0)).x();
  EXPECT_LE(name_right, brush_left + 0.5);
  EXPECT_LE(brush_right, radial_left + 0.5);
  EXPECT_LE(radial_right, gradient_left + 0.5);
}

TEST(EditorAdjustmentHeaderQmlTest, HeaderAtMinPreferredAndMaxWidthKeepsNameAndExifAboveNav) {
  HeaderSession      session;
  FakeNodeController nodes;
  session.setExif(QStringLiteral("1/250s"), QStringLiteral("ISO 100"), QStringLiteral("f2.8"),
                  QStringLiteral("50mm"));
  nodes.setSelection(QStringLiteral("Color Grade 2"), QStringLiteral("colorGrade"),
                     {QStringLiteral("tone"), QStringLiteral("look"), QStringLiteral("lut"),
                      QStringLiteral("masks")});

  for (const int width : {260, 320, 460}) {
    StackHarness harness(&session, &nodes, width);
    ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
    harness.root()->setWidth(width);
    ProcessEvents(40);
    ExpectNavPresent(harness);
    ExpectHeaderAboveNav(harness);
    ExpectExifAboveNameRow(harness);
    ExpectExifRowMatchesNameRowWidth(harness);
    ExpectFourEqualExifTokens(harness, QStringLiteral("50mm"), QStringLiteral("f2.8"),
                              QStringLiteral("1/250s"), QStringLiteral("ISO 100"));
    ExpectMaskToolButtons(harness);
    ExpectNameThenMaskTools(harness);
    auto* name = harness.find(QStringLiteral("editorAdjustmentHeaderNodeName"));
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->property("text").toString(), QStringLiteral("Color Grade 2"));
    EXPECT_EQ(name->property("font").value<QFont>().family(), AppTheme::Instance().uiFontFamily());
  }
}

TEST(EditorAdjustmentHeaderQmlTest, LongNodeNameElidesAndKeepsFullAccessibleName) {
  HeaderSession      session;
  FakeNodeController nodes;
  const QString      long_name =
      QStringLiteral("Color Grade with an extremely long display name for elision");
  nodes.setSelection(long_name, QStringLiteral("colorGrade"),
                     {QStringLiteral("tone"), QStringLiteral("look")});
  StackHarness harness(&session, &nodes, 260);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  ExpectHeaderAboveNav(harness);
  auto* header = harness.find(QStringLiteral("editorAdjustmentHeader"));
  auto* name   = harness.find(QStringLiteral("editorAdjustmentHeaderNodeName"));
  ASSERT_NE(header, nullptr);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(header->property("nodeName").toString(), long_name);
  EXPECT_TRUE(name->property("truncated").toBool() ||
              name->height() > AppTheme::Instance().lineHeightTitle());
}

TEST(EditorAdjustmentHeaderQmlTest, MissingExifShowsEmDashOnFourTokens) {
  HeaderSession      session;
  FakeNodeController nodes;
  StackHarness       harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  const QString dash   = QString::fromUtf8(kEmDash);
  auto*         header = harness.find(QStringLiteral("editorAdjustmentHeader"));
  ASSERT_NE(header, nullptr);
  EXPECT_EQ(header->property("focalText").toString(), dash);
  EXPECT_EQ(header->property("apertureText").toString(), dash);
  EXPECT_EQ(header->property("shutterText").toString(), dash);
  EXPECT_EQ(header->property("isoText").toString(), dash);
  ExpectFourEqualExifTokens(harness, dash, dash, dash, dash);
  ExpectMaskToolButtons(harness);
}

TEST(EditorAdjustmentHeaderQmlTest, ReservedMaskButtonsKeepApprovedIconsAndDoNotSubmit) {
  HeaderSession      session;
  FakeNodeController nodes;
  StackHarness       harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  ExpectMaskToolButtons(harness);
  auto* brush    = harness.find(QStringLiteral("editorAdjustmentHeaderBrushButton"));
  auto* radial   = harness.find(QStringLiteral("editorAdjustmentHeaderRadialButton"));
  auto* gradient = harness.find(QStringLiteral("editorAdjustmentHeaderGradientButton"));
  ASSERT_NE(brush, nullptr);
  ASSERT_NE(radial, nullptr);
  ASSERT_NE(gradient, nullptr);
  const int submits = session.submitCount();
  QMetaObject::invokeMethod(brush, "clicked");
  QMetaObject::invokeMethod(radial, "clicked");
  QMetaObject::invokeMethod(gradient, "clicked");
  ProcessEvents(20);
  EXPECT_EQ(session.submitCount(), submits);
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("tone"));
}

TEST(EditorAdjustmentHeaderQmlTest, BothThemesMapHeaderInkToAppThemeTokens) {
  HeaderSession      session;
  FakeNodeController nodes;
  nodes.setSelection(QStringLiteral("Develop"), QStringLiteral("develop"),
                     {QStringLiteral("raw"), QStringLiteral("geometry")});
  StackHarness harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto&      theme    = AppTheme::Instance();
  const int  original = theme.currentThemeIndex();
  const auto themes   = theme.availableThemes();
  ASSERT_GE(themes.size(), 2);

  auto* name  = harness.find(QStringLiteral("editorAdjustmentHeaderNodeName"));
  auto* focal = harness.find(QStringLiteral("editorAdjustmentHeaderFocal"));
  ASSERT_NE(name, nullptr);
  ASSERT_NE(focal, nullptr);

  for (int index = 0; index < 2; ++index) {
    theme.setCurrentThemeIndex(index);
    ProcessEvents(40);
    EXPECT_EQ(name->property("color").value<QColor>(), theme.textColor()) << index;
    EXPECT_EQ(focal->property("color").value<QColor>(), theme.textColor()) << index;
  }
  theme.setCurrentThemeIndex(original);
}

TEST(EditorAdjustmentHeaderQmlTest, ReduceMotionAndTitleSizeKeepHeaderAboveNav) {
  HeaderSession      session;
  FakeNodeController nodes;
  const QString      long_name = QStringLiteral("Second Color Grade with wrapped title text");
  nodes.setSelection(long_name, QStringLiteral("colorGrade"),
                     {QStringLiteral("tone"), QStringLiteral("look"), QStringLiteral("lut")});
  AppTheme::Instance().setReduceMotion(true);
  StackHarness harness(&session, &nodes, 260);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  ExpectHeaderAboveNav(harness);
  EXPECT_GT(AppTheme::Instance().fontSizeTitle(), AppTheme::Instance().fontSizeCaption());
  ExpectNavPresent(harness);
}

TEST(EditorAdjustmentHeaderQmlTest, NavbarKeepsAllPagesWhenSelectedNodeKindChanges) {
  HeaderSession      session;
  FakeNodeController nodes;
  StackHarness       harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  ExpectNavPresent(harness);

  nodes.setSelection(QStringLiteral("Develop"), QStringLiteral("develop"),
                     {QStringLiteral("raw"), QStringLiteral("geometry")});
  ProcessEvents(40);
  ExpectNavPresent(harness);

  nodes.setSelection(QStringLiteral("DRT"), QStringLiteral("drt"),
                     {QStringLiteral("display"), QStringLiteral("detail")});
  ProcessEvents(40);
  ExpectNavPresent(harness);

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.root(), "selectPanel",
                                        Q_ARG(QVariant, QVariant(QStringLiteral("geometry")))));
  ProcessEvents(20);
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("geometry"));
}

TEST(EditorAdjustmentHeaderQmlTest, MaskPageActivatesOnlyForSelectedOrCreatingMask) {
  FakeMaskCreation   mask_creation;
  HeaderSession      session(&mask_creation);
  FakeNodeController nodes;
  StackHarness       harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* nav   = harness.find(QStringLiteral("editorAdjustmentNav_masks"));
  auto* panel = harness.find(QStringLiteral("editorAdjustmentPanel_masks"));
  ASSERT_NE(nav, nullptr);
  ASSERT_NE(panel, nullptr);
  EXPECT_FALSE(nav->isEnabled());
  EXPECT_EQ(harness.find(QStringLiteral("editorMasksDoneButton")), nullptr);
  EXPECT_EQ(harness.find(QStringLiteral("editorMasksCancelButton")), nullptr);

  mask_creation.beginRadial();
  ProcessEvents(20);
  EXPECT_TRUE(nav->isEnabled());
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("masks"));
  auto* radius = harness.find(QStringLiteral("editorMasksMajorRadiusSlider"));
  ASSERT_NE(radius, nullptr);
  EXPECT_FALSE(radius->isVisible());

  mask_creation.SelectExisting(QStringLiteral("radial"));
  ProcessEvents(20);
  EXPECT_TRUE(radius->isVisible());
  EXPECT_NE(harness.find(QStringLiteral("editorMasksMinorRadiusSlider")), nullptr);
  EXPECT_NE(harness.find(QStringLiteral("editorMasksRotationSlider")), nullptr);
  EXPECT_NE(harness.find(QStringLiteral("editorMasksInnerFeatherSlider")), nullptr);
  EXPECT_NE(harness.find(QStringLiteral("editorMasksOuterFeatherSlider")), nullptr);

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.root(), "selectPanel",
                                        Q_ARG(QVariant, QVariant(QStringLiteral("look")))));
  ProcessEvents(20);
  EXPECT_EQ(mask_creation.finishCount(), 1);
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("look"));
  EXPECT_FALSE(nav->isEnabled());
}

TEST(EditorAdjustmentHeaderQmlTest, EnterEquivalentFinishesMaskEditAndRestoresPanel) {
  FakeMaskCreation   mask_creation;
  HeaderSession      session(&mask_creation);
  FakeNodeController nodes;
  session.setActiveAdjustmentPanel(QStringLiteral("lut"));
  StackHarness harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  mask_creation.SelectExisting(QStringLiteral("linear"));
  ProcessEvents(20);
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("masks"));
  auto* transition = harness.find(QStringLiteral("editorMasksTransitionSlider"));
  ASSERT_NE(transition, nullptr);
  EXPECT_TRUE(transition->isVisible());

  QVariant returned;
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.root(), "confirmMaskEditAndReturn",
                                        Q_RETURN_ARG(QVariant, returned)));
  ProcessEvents(20);
  EXPECT_TRUE(returned.toBool());
  EXPECT_EQ(mask_creation.finishCount(), 1);
  EXPECT_EQ(session.activeAdjustmentPanel(), QStringLiteral("lut"));
}

TEST(EditorAdjustmentHeaderQmlTest, GeometryPanelStatesWholeImageScope) {
  HeaderSession      session;
  FakeNodeController nodes;
  session.setActiveAdjustmentPanel(QStringLiteral("geometry"));
  StackHarness harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* scope = harness.find(QStringLiteral("editorGeometryWholeImageScope"));
  ASSERT_NE(scope, nullptr);
  EXPECT_TRUE(scope->property("text").toString().contains(QStringLiteral("whole image")));
}

TEST(EditorAdjustmentHeaderQmlTest, RawDecodeHostsWhiteBalanceAndLoadDoesNotSubmit) {
  HeaderSession      session;
  FakeNodeController nodes;
  QVariantMap        color_temp;
  color_temp.insert(QStringLiteral("cct"), 5600);
  color_temp.insert(QStringLiteral("tint"), 10);
  QVariantMap snapshot;
  snapshot.insert(QStringLiteral("color_temp"), color_temp);
  session.setSnapshot(snapshot);
  session.setActiveAdjustmentPanel(QStringLiteral("raw"));
  StackHarness harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  EXPECT_NE(harness.find(QStringLiteral("editorWhiteBalanceSection")), nullptr);
  EXPECT_NE(harness.find(QStringLiteral("rawCctSlider")), nullptr);
  EXPECT_NE(harness.find(QStringLiteral("rawTintSlider")), nullptr);
  const int submits = session.submitCount();
  ASSERT_TRUE(QMetaObject::invokeMethod(
      harness.root(), "loadFromSnapshot",
      Q_ARG(QVariant, QVariant::fromValue(session.adjustmentSnapshot()))));
  ProcessEvents(40);
  EXPECT_EQ(session.submitCount(), submits);
}

TEST(EditorAdjustmentHeaderQmlTest, LutSelectionAndScrollSurviveStackLoadWithoutSubmit) {
  HeaderSession      session;
  FakeNodeController nodes;
  StackHarness       harness(&session, &nodes, 320);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* lut_panel = harness.find(QStringLiteral("editorAdjustmentPanel_lut"));
  ASSERT_NE(lut_panel, nullptr);
  auto lut_model = std::make_unique<FakeLutCatalogModel>();
  lut_panel->setProperty("lutModel", QVariant::fromValue(static_cast<QObject*>(lut_model.get())));
  ProcessEvents(80);

  lut_model->setSelectedPath(QStringLiteral("D:/fake/lut_12.cube"));
  session.setActiveAdjustmentPanel(QStringLiteral("lut"));
  ProcessEvents(40);
  auto* list = harness.find(QStringLiteral("editorLutListView"));
  ASSERT_NE(list, nullptr);
  if (list->property("contentHeight").toReal() > list->height() + 8.0) {
    list->setProperty("contentY", 80.0);
    ProcessEvents(20);
  }
  const qreal y_before = list->property("contentY").toReal();
  const int   submits  = session.submitCount();

  QVariantMap lut_entry;
  lut_entry.insert(QStringLiteral("path"), QStringLiteral("D:/fake/lut_12.cube"));
  QVariantMap snapshot;
  snapshot.insert(QStringLiteral("lut"), lut_entry);
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.root(), "loadFromSnapshot",
                                        Q_ARG(QVariant, QVariant::fromValue(snapshot))));
  ProcessEvents(40);
  EXPECT_EQ(lut_model->selectedPath(), QStringLiteral("D:/fake/lut_12.cube"));
  EXPECT_NEAR(list->property("contentY").toReal(), y_before, 1.5);
  EXPECT_EQ(session.submitCount(), submits);
}

}  // namespace
}  // namespace alcedo::ui::test

#include "editor_adjustment_header_qml_test.moc"
