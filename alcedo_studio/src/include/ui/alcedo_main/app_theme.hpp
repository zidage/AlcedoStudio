//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QColor>
#include <QFont>
#include <QString>
#include <QVariantList>

class QApplication;
class QWidget;

namespace alcedo::ui {

class AppTheme final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString uiFontFamily READ uiFontFamily NOTIFY UiFontFamilyChanged)
  Q_PROPERTY(QString headlineFontFamily READ headlineFontFamily NOTIFY UiFontFamilyChanged)
  Q_PROPERTY(QString dataFontFamily READ dataFontFamily CONSTANT)
  Q_PROPERTY(QString monoFontFamily READ monoFontFamily CONSTANT)
  Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
  Q_PROPERTY(QColor toneGold READ toneGold NOTIFY ThemeChanged)
  Q_PROPERTY(QColor toneWine READ toneWine NOTIFY ThemeChanged)
  Q_PROPERTY(QColor toneSteel READ toneSteel NOTIFY ThemeChanged)
  Q_PROPERTY(QColor toneGraphite READ toneGraphite NOTIFY ThemeChanged)
  Q_PROPERTY(QColor toneMist READ toneMist NOTIFY ThemeChanged)
  Q_PROPERTY(QColor bgCanvasColor READ bgCanvasColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor bgDeepColor READ bgDeepColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor bgBaseColor READ bgBaseColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor bgPanelColor READ bgPanelColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor textColor READ textColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor textMutedColor READ textMutedColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor iconColor READ iconColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor accentColor READ accentColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor accentSecondaryColor READ accentSecondaryColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor dangerColor READ dangerColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor dangerTintColor READ dangerTintColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor selectedTintColor READ selectedTintColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor hoverColor READ hoverColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor dividerColor READ dividerColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor glassPanelColor READ glassPanelColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor glassStrokeColor READ glassStrokeColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor overlayColor READ overlayColor NOTIFY ThemeChanged)
  Q_PROPERTY(int panelRadius READ panelRadius NOTIFY ThemeChanged)
  Q_PROPERTY(int currentThemeIndex READ currentThemeIndex WRITE setCurrentThemeIndex NOTIFY ThemeChanged)
  Q_PROPERTY(QVariantList availableThemes READ availableThemes CONSTANT)

  // ── Design tokens — structural (theme-invariant) ─────────────────────────
  // Canonical visual-identity contract: see src/ui/alcedo_main/DESIGN.md.
  // Literal getters (CONSTANT) for radii, icon geometry, spacing, motion
  // durations, and QML-facing typography sizes/weights. Only reduceMotion is
  // stateful (QSettings-backed "ui/reduceMotion"). cardSurfaceColor /
  // cardBorderColor are semantic aliases that follow the active theme.
  Q_PROPERTY(int controlRadius READ controlRadius CONSTANT)
  Q_PROPERTY(int controlRadiusSmall READ controlRadiusSmall CONSTANT)
  Q_PROPERTY(int badgeRadius READ badgeRadius CONSTANT)
  Q_PROPERTY(int iconOpticalSize READ iconOpticalSize CONSTANT)
  Q_PROPERTY(int iconOpticalSizeCompact READ iconOpticalSizeCompact CONSTANT)
  Q_PROPERTY(int iconButtonHitSize READ iconButtonHitSize CONSTANT)
  Q_PROPERTY(int iconButtonHitSizeCompact READ iconButtonHitSizeCompact CONSTANT)
  Q_PROPERTY(int spaceXs READ spaceXs CONSTANT)
  Q_PROPERTY(int spaceSm READ spaceSm CONSTANT)
  Q_PROPERTY(int spaceMd READ spaceMd CONSTANT)
  Q_PROPERTY(int spaceLg READ spaceLg CONSTANT)
  Q_PROPERTY(int spaceXl READ spaceXl CONSTANT)
  Q_PROPERTY(int motionFoldOpenMs READ motionFoldOpenMs CONSTANT)
  Q_PROPERTY(int motionFoldCloseMs READ motionFoldCloseMs CONSTANT)
  Q_PROPERTY(int motionFadeMs READ motionFadeMs CONSTANT)
  Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY ReduceMotionChanged)
  Q_PROPERTY(int fontSizeCaption READ fontSizeCaption CONSTANT)
  Q_PROPERTY(int fontSizeBody READ fontSizeBody CONSTANT)
  Q_PROPERTY(int fontSizeTitle READ fontSizeTitle CONSTANT)
  Q_PROPERTY(int fontSizeSection READ fontSizeSection CONSTANT)
  Q_PROPERTY(int fontSizeHeadline READ fontSizeHeadline CONSTANT)
  Q_PROPERTY(int fontWeightRegular READ fontWeightRegular CONSTANT)
  Q_PROPERTY(int fontWeightStrong READ fontWeightStrong CONSTANT)
  Q_PROPERTY(int fontWeightHeading READ fontWeightHeading CONSTANT)
  Q_PROPERTY(QColor cardSurfaceColor READ cardSurfaceColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor cardBorderColor READ cardBorderColor NOTIFY ThemeChanged)

 public:
  enum class FontRole : int {
    UiBody = 0,
    UiBodyStrong,
    UiCaption,
    UiCaptionStrong,
    UiTitle,
    UiHeadline,
    UiOverline,
    UiHint,
    DataBody,
    DataBodyStrong,
    DataCaption,
    DataNumeric,
    DataOverlay,
    MonoBody,
    MonoCaption
  };
  Q_ENUM(FontRole)

  static auto Instance() -> AppTheme&;

  static void RegisterFonts();
  static void SetEffectiveLanguageCode(const QString& code);
  static void ApplyApplicationFont();
  static auto TryRegisterUiFontOverride(const QString& path) -> bool;
  static void ApplyApplicationFont(QApplication& app);

  static auto Font(FontRole role) -> QFont;
  static void ApplyFont(QWidget* widget, FontRole role);
  static void MarkFontRole(QObject* object, FontRole role);
  static void ApplyFontsRecursively(QWidget* root);

  static auto EditorLabelStyle(const QColor& color) -> QString;
  static auto EditorPrimaryButtonStyle(bool include_disabled = false) -> QString;
  static auto EditorSecondaryButtonStyle() -> QString;
  static auto EditorPanelToggleStyle(bool active, bool is_first = false,
                                     bool is_last = false) -> QString;
  static auto EditorMethodCardStyle(bool active) -> QString;
  static auto EditorComboBoxStyle() -> QString;
  static auto EditorSpinBoxStyle() -> QString;
  static auto EditorCheckBoxStyle() -> QString;
  static auto EditorScrollAreaStyle() -> QString;
  static auto EditorListWidgetStyle() -> QString;
  static auto EditorHistoryCardStyle() -> QString;
  static auto EditorTransparentFrameStyle() -> QString;
  static auto EditorSliderTrackColor() -> QColor;
  static auto EditorSliderAccentColor(bool positive) -> QColor;
  static auto EditorSliderBorderColor(bool positive) -> QColor;
  static auto EditorSliderHandleColor() -> QColor;
  static auto EditorSliderHandleBorderColor() -> QColor;

  auto uiFontFamily() const -> QString;
  auto headlineFontFamily() const -> QString;
  auto dataFontFamily() const -> QString;
  auto monoFontFamily() const -> QString;
  auto appVersion() const -> QString;

  auto toneGold() const -> QColor;
  auto toneWine() const -> QColor;
  auto toneSteel() const -> QColor;
  auto toneGraphite() const -> QColor;
  auto toneMist() const -> QColor;

  auto bgCanvasColor() const -> QColor;
  auto bgDeepColor() const -> QColor;
  auto bgBaseColor() const -> QColor;
  auto bgPanelColor() const -> QColor;
  auto textColor() const -> QColor;
  auto textMutedColor() const -> QColor;
  auto iconColor() const -> QColor;
  auto accentColor() const -> QColor;
  auto accentSecondaryColor() const -> QColor;
  auto dangerColor() const -> QColor;
  auto dangerTintColor() const -> QColor;
  auto selectedTintColor() const -> QColor;
  auto hoverColor() const -> QColor;
  auto dividerColor() const -> QColor;
  auto glassPanelColor() const -> QColor;
  auto glassStrokeColor() const -> QColor;
  auto overlayColor() const -> QColor;
  auto panelRadius() const -> int;

  auto controlRadius() const -> int;
  auto controlRadiusSmall() const -> int;
  auto badgeRadius() const -> int;
  auto iconOpticalSize() const -> int;
  auto iconOpticalSizeCompact() const -> int;
  auto iconButtonHitSize() const -> int;
  auto iconButtonHitSizeCompact() const -> int;
  auto spaceXs() const -> int;
  auto spaceSm() const -> int;
  auto spaceMd() const -> int;
  auto spaceLg() const -> int;
  auto spaceXl() const -> int;
  auto motionFoldOpenMs() const -> int;
  auto motionFoldCloseMs() const -> int;
  auto motionFadeMs() const -> int;
  auto reduceMotion() const -> bool;
  void setReduceMotion(bool enabled);
  auto fontSizeCaption() const -> int;
  auto fontSizeBody() const -> int;
  auto fontSizeTitle() const -> int;
  auto fontSizeSection() const -> int;
  auto fontSizeHeadline() const -> int;
  auto fontWeightRegular() const -> int;
  auto fontWeightStrong() const -> int;
  auto fontWeightHeading() const -> int;
  auto cardSurfaceColor() const -> QColor;
  auto cardBorderColor() const -> QColor;

  auto currentThemeIndex() const -> int;
  void setCurrentThemeIndex(int index);
  auto availableThemes() const -> QVariantList;

 signals:
  void UiFontFamilyChanged();
  void ThemeChanged();
  void ReduceMotionChanged();

 private:
  explicit AppTheme(QObject* parent = nullptr);

  static auto ResolveRole(QWidget* widget) -> FontRole;

  int current_theme_index_ = 0;
  // Lazy-loaded from QSettings("ui/reduceMotion") on first read so the
  // singleton is safe to construct before QCoreApplication org/app are set.
  mutable bool reduce_motion_loaded_ = false;
  mutable bool reduce_motion_ = false;
};

}  // namespace alcedo::ui
