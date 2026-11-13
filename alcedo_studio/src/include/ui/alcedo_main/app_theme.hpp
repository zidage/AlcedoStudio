//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

class QApplication;
class QTimer;
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
  /// Warning and error messages: AlertBadge.qml fill and text (DESIGN.md "Alert badges").
  Q_PROPERTY(QColor alertBadgeFillColor READ alertBadgeFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor alertBadgeTextColor READ alertBadgeTextColor NOTIFY ThemeChanged)
  Q_PROPERTY(
      QColor backgroundTaskFinishedColor READ backgroundTaskFinishedColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor backgroundTaskWorkingColor READ backgroundTaskWorkingColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor backgroundTaskFailedColor READ backgroundTaskFailedColor NOTIFY ThemeChanged)
  /// Border of the AI model card that is active in the open project (Settings > AI Models).
  Q_PROPERTY(QColor activeModelBorderColor READ activeModelBorderColor NOTIFY ThemeChanged)
  /// Import progress rings: RAW files and the other image files.
  Q_PROPERTY(QColor importRawRingColor READ importRawRingColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor importOtherRingColor READ importOtherRingColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor selectedTintColor READ selectedTintColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor hoverColor READ hoverColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor dividerColor READ dividerColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor glassPanelColor READ glassPanelColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor glassStrokeColor READ glassStrokeColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor overlayColor READ overlayColor NOTIFY ThemeChanged)
  Q_PROPERTY(int panelRadius READ panelRadius NOTIFY ThemeChanged)
  Q_PROPERTY(
      int currentThemeIndex READ currentThemeIndex WRITE setCurrentThemeIndex NOTIFY ThemeChanged)
  Q_PROPERTY(QVariantList availableThemes READ availableThemes CONSTANT)

  // ── Design tokens — structural (theme-invariant) ─────────────────────────
  // Canonical visual-identity specification: see src/ui/alcedo_main/DESIGN.md.
  // Literal getters (CONSTANT) for radii, icon geometry, spacing, motion
  // durations, and QML-facing typography sizes/weights. Only reduceMotion is
  // stateful (QSettings-backed "ui/reduceMotion"). cardSurfaceColor /
  // cardBorderColor are semantic aliases that follow the active theme.
  Q_PROPERTY(int controlRadius READ controlRadius CONSTANT)
  Q_PROPERTY(int controlRadiusSmall READ controlRadiusSmall CONSTANT)
  Q_PROPERTY(int badgeRadius READ badgeRadius CONSTANT)
  Q_PROPERTY(int iconOpticalSize READ iconOpticalSize CONSTANT)
  Q_PROPERTY(int iconOpticalSizeCompact READ iconOpticalSizeCompact CONSTANT)
  // Raster source size for Image/sourceSize (separate from optical display size).
  // Keep source ≥ optical so vector rasterization stays crisp at fractional DPRs.
  Q_PROPERTY(int iconSourceSize READ iconSourceSize CONSTANT)
  Q_PROPERTY(int iconSourceSizeCompact READ iconSourceSizeCompact CONSTANT)
  Q_PROPERTY(int iconButtonHitSize READ iconButtonHitSize CONSTANT)
  Q_PROPERTY(int iconButtonHitSizeCompact READ iconButtonHitSizeCompact CONSTANT)
  // Editor side-panel + scope geometry (Phase 4C comfort sizing). The preferred
  // width unifies the adjustment stack and the History/Versions/Nodes expanded panel;
  // min/max bound only the adjustment stack. Scope height covers the
  // histogram/waveform slot. See src/ui/alcedo_main/DESIGN.md.
  Q_PROPERTY(int editorSidePanelWidth READ editorSidePanelWidth CONSTANT)
  Q_PROPERTY(int editorSidePanelWidthMin READ editorSidePanelWidthMin CONSTANT)
  Q_PROPERTY(int editorSidePanelWidthMax READ editorSidePanelWidthMax CONSTANT)
  Q_PROPERTY(int editorScopeHeight READ editorScopeHeight CONSTANT)
  Q_PROPERTY(int editorScopeHeightMin READ editorScopeHeightMin CONSTANT)
  Q_PROPERTY(int editorAdjustmentHeaderMinHeight READ editorAdjustmentHeaderMinHeight CONSTANT)
  // LUT browser rail page (LUT library plan L6A): preferred page width, filter card width,
  // minimum tile width, and the tile placeholder icon size. See DESIGN.md.
  Q_PROPERTY(int editorLutBrowserPanelWidth READ editorLutBrowserPanelWidth CONSTANT)
  Q_PROPERTY(int editorLutBrowserPanelWidthMin READ editorLutBrowserPanelWidthMin CONSTANT)
  Q_PROPERTY(int editorLutBrowserFilterWidth READ editorLutBrowserFilterWidth CONSTANT)
  Q_PROPERTY(int editorLutTileMinWidth READ editorLutTileMinWidth CONSTANT)
  Q_PROPERTY(int editorLutTileCompactWidth READ editorLutTileCompactWidth CONSTANT)
  Q_PROPERTY(int editorLutTileIconSize READ editorLutTileIconSize CONSTANT)
  // Persistent left collections column in the application shell (see DESIGN.md).
  Q_PROPERTY(int collectionsSidebarWidth READ collectionsSidebarWidth CONSTANT)
  Q_PROPERTY(int collectionRowHeight READ collectionRowHeight CONSTANT)
  Q_PROPERTY(int collectionIndentStep READ collectionIndentStep CONSTANT)
  Q_PROPERTY(int collectionMenuMaxRows READ collectionMenuMaxRows CONSTANT)
  Q_PROPERTY(int collectionMenuFilterThreshold READ collectionMenuFilterThreshold CONSTANT)
  // Search palette geometry (see DESIGN.md "Search palette").
  Q_PROPERTY(int searchPaletteMaxWidth READ searchPaletteMaxWidth CONSTANT)
  Q_PROPERTY(int searchPaletteWindowMarginX READ searchPaletteWindowMarginX CONSTANT)
  Q_PROPERTY(int searchPaletteWindowMarginY READ searchPaletteWindowMarginY CONSTANT)
  // Welcome surface card geometry and the skeleton placeholder cycle (see DESIGN.md
  // "Welcome surface").
  Q_PROPERTY(int welcomeCardWidth READ welcomeCardWidth CONSTANT)
  Q_PROPERTY(int welcomeCardHeight READ welcomeCardHeight CONSTANT)
  Q_PROPERTY(int welcomeSidebarWidth READ welcomeSidebarWidth CONSTANT)
  Q_PROPERTY(int welcomeCoverWidth READ welcomeCoverWidth CONSTANT)
  Q_PROPERTY(int welcomeCoverCompactWidth READ welcomeCoverCompactWidth CONSTANT)
  Q_PROPERTY(int welcomeCoverHeight READ welcomeCoverHeight CONSTANT)
  Q_PROPERTY(int welcomeInfoMinWidth READ welcomeInfoMinWidth CONSTANT)
  Q_PROPERTY(int welcomeEmptyContentWidth READ welcomeEmptyContentWidth CONSTANT)
  Q_PROPERTY(int skeletonCycleMs READ skeletonCycleMs CONSTANT)
  // Shared clock of the StaggeredGridMark wave (DESIGN.md "Library loading"): 0 to 1 over
  // loadingWaveCycleMs, advanced every loadingWaveFrameMs while at least one mark holds it
  // (setLoadingWaveHolder). Every mark reads the same phase, so a dense grid repaints once per
  // tick, not once per display frame.
  Q_PROPERTY(qreal loadingWavePhase READ loadingWavePhase NOTIFY LoadingWavePhaseChanged)
  Q_PROPERTY(int loadingWaveCycleMs READ loadingWaveCycleMs CONSTANT)
  Q_PROPERTY(int loadingWaveFrameMs READ loadingWaveFrameMs CONSTANT)
  // Line heights (px) for QML Label lineHeight when using fixed pixel sizes.
  Q_PROPERTY(int lineHeightCaption READ lineHeightCaption CONSTANT)
  Q_PROPERTY(int lineHeightBody READ lineHeightBody CONSTANT)
  Q_PROPERTY(int lineHeightTitle READ lineHeightTitle CONSTANT)
  Q_PROPERTY(int lineHeightSection READ lineHeightSection CONSTANT)
  Q_PROPERTY(int lineHeightHeadline READ lineHeightHeadline CONSTANT)
  Q_PROPERTY(int spaceXs READ spaceXs CONSTANT)
  Q_PROPERTY(int spaceSm READ spaceSm CONSTANT)
  Q_PROPERTY(int spaceMd READ spaceMd CONSTANT)
  Q_PROPERTY(int spaceLg READ spaceLg CONSTANT)
  Q_PROPERTY(int spaceXl READ spaceXl CONSTANT)
  // Library date activity graph (GitHub-style capture heatmap).
  Q_PROPERTY(int dateGraphCellMinSize READ dateGraphCellMinSize CONSTANT)
  Q_PROPERTY(int dateGraphCellGap READ dateGraphCellGap CONSTANT)
  Q_PROPERTY(int dateGraphCellRadius READ dateGraphCellRadius CONSTANT)
  // Library grouped sections: fixed header row height (scroll geometry) and the compact
  // Inspector field header actions (sort arrows, Group checkbox row).
  Q_PROPERTY(int librarySectionHeaderHeight READ librarySectionHeaderHeight CONSTANT)
  Q_PROPERTY(int inspectorHeaderActionSize READ inspectorHeaderActionSize CONSTANT)
  Q_PROPERTY(int motionFoldOpenMs READ motionFoldOpenMs CONSTANT)
  Q_PROPERTY(int motionFoldCloseMs READ motionFoldCloseMs CONSTANT)
  Q_PROPERTY(int motionFadeMs READ motionFadeMs CONSTANT)
  // QEasingCurve::Type / QML Easing.Type. Fold and fade hosts bind
  // `easing.type: appTheme.motionEasing` so VI easing is not a QML literal.
  Q_PROPERTY(int motionEasing READ motionEasing CONSTANT)
  Q_PROPERTY(int backgroundTaskAutoCollapseMs READ backgroundTaskAutoCollapseMs CONSTANT)
  Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY ReduceMotionChanged)
  Q_PROPERTY(int fontSizeCaption READ fontSizeCaption CONSTANT)
  Q_PROPERTY(int fontSizeBody READ fontSizeBody CONSTANT)
  Q_PROPERTY(int fontSizeTitle READ fontSizeTitle CONSTANT)
  Q_PROPERTY(int fontSizeSection READ fontSizeSection CONSTANT)
  Q_PROPERTY(int fontSizeHeadline READ fontSizeHeadline CONSTANT)
  /// The large count of a Settings bento tile (CountTile): the one display size.
  Q_PROPERTY(int fontSizeDisplay READ fontSizeDisplay CONSTANT)
  Q_PROPERTY(int fontWeightRegular READ fontWeightRegular CONSTANT)
  Q_PROPERTY(int fontWeightStrong READ fontWeightStrong CONSTANT)
  Q_PROPERTY(int fontWeightHeading READ fontWeightHeading CONSTANT)
  Q_PROPERTY(QColor cardSurfaceColor READ cardSurfaceColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor cardBorderColor READ cardBorderColor NOTIFY ThemeChanged)
  // Phase 4D: opaque button-state fills (alpha 255). These replace the former
  // Qt.rgba(…, alpha) / withAlpha(…, …) / "transparent" surface derivations.
  // Idle matches the card surface so the button blends in; hovered/pressed/
  // selected are pre-computed opaque blends of cardSurfaceColor + hoverColor.
  Q_PROPERTY(QColor buttonIdleFillColor READ buttonIdleFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor buttonHoveredFillColor READ buttonHoveredFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor buttonPressedFillColor READ buttonPressedFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor buttonSelectedFillColor READ buttonSelectedFillColor NOTIFY ThemeChanged)
  // Opaque disabled surface: cardSurfaceColor blended with bgCanvasColor so a
  // disabled panel shell reads as a single concrete fill rather than 0.55 opacity.
  Q_PROPERTY(QColor disabledSurfaceColor READ disabledSurfaceColor NOTIFY ThemeChanged)
  // Raised controls on card and panel surfaces: secondary action buttons (DialogActionButton
  // "normal"), combo fields, and tool icon actions. Clearly lighter than cardSurfaceColor and
  // bgBaseColor so the control reads apart from its surface.
  Q_PROPERTY(QColor controlFillColor READ controlFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor controlHoveredFillColor READ controlHoveredFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor controlPressedFillColor READ controlPressedFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor controlBorderColor READ controlBorderColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor editorSliderTrackColor READ editorSliderTrackColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor editorSliderPositiveColor READ editorSliderPositiveColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor editorSliderNegativeColor READ editorSliderNegativeColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor editorSliderHandleColor READ editorSliderHandleColor NOTIFY ThemeChanged)
  Q_PROPERTY(
      QColor editorSliderHandleBorderColor READ editorSliderHandleBorderColor NOTIFY ThemeChanged)
  // Monochrome inverted list rows (dense catalogs: LUT browser, etc.).
  // Selected well is a light bone bar on the sunken track; text and favorite
  // stars invert ink on that well so they stay readable without ad-hoc rgba.
  Q_PROPERTY(
      QColor editorListSelectedFillColor READ editorListSelectedFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor editorListSelectedInkColor READ editorListSelectedInkColor NOTIFY ThemeChanged)
  Q_PROPERTY(
      QColor editorListFavoriteIdleColor READ editorListFavoriteIdleColor NOTIFY ThemeChanged)
  Q_PROPERTY(
      QColor editorListFavoriteActiveColor READ editorListFavoriteActiveColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor editorListFavoriteIdleOnSelectedColor READ editorListFavoriteIdleOnSelectedColor
                 NOTIFY ThemeChanged)
  Q_PROPERTY(QColor editorListFavoriteActiveOnSelectedColor READ
                 editorListFavoriteActiveOnSelectedColor NOTIFY ThemeChanged)
  // Editor filmstrip: the light gray well of a Library-selected tile that the editor does not
  // show, and the dark caption under the file name and rating, below a thumbnail image.
  // Empty star of a Library card rating and of the Inspector rating filter: a dim neutral ink
  // below textMutedColor, so filled stars (textColor) read at a glance.
  Q_PROPERTY(QColor ratingStarEmptyColor READ ratingStarEmptyColor NOTIFY ThemeChanged)
  // Fill of the edit mark chip on the top-right corner of a thumbnail photo.
  Q_PROPERTY(QColor thumbnailEditMarkFillColor READ thumbnailEditMarkFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor thumbnailDetailsScrimColor READ thumbnailDetailsScrimColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor scopeGridColor READ scopeGridColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor scopePlotBorderColor READ scopePlotBorderColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor scopeHistogramRedColor READ scopeHistogramRedColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor scopeHistogramGreenColor READ scopeHistogramGreenColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor scopeHistogramBlueColor READ scopeHistogramBlueColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor scopeMarkerColor READ scopeMarkerColor NOTIFY ThemeChanged)
  // Plot pane (PlotPane and its layers). Colors follow the active theme; sizes are logical px.
  // See DESIGN.md "Plot pane".
  Q_PROPERTY(QColor plotSurfaceColor READ plotSurfaceColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotGridColor READ plotGridColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotGridStrongColor READ plotGridStrongColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotLabelColor READ plotLabelColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotLineColor READ plotLineColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotReferenceLineColor READ plotReferenceLineColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotHandleColor READ plotHandleColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotHandleOutlineColor READ plotHandleOutlineColor NOTIFY ThemeChanged)
  Q_PROPERTY(qreal plotGridLineWidth READ plotGridLineWidth CONSTANT)
  Q_PROPERTY(qreal plotLineWidth READ plotLineWidth CONSTANT)
  Q_PROPERTY(qreal plotHandleRadius READ plotHandleRadius CONSTANT)
  Q_PROPERTY(qreal plotHandleRingWidth READ plotHandleRingWidth CONSTANT)
  Q_PROPERTY(qreal plotHandleOutlineWidth READ plotHandleOutlineWidth CONSTANT)
  Q_PROPERTY(qreal plotHitRadius READ plotHitRadius CONSTANT)
  // Tone Curve section (DESIGN.md "Tone Curve section").
  Q_PROPERTY(QColor plotChannelRedColor READ plotChannelRedColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotChannelGreenColor READ plotChannelGreenColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotChannelBlueColor READ plotChannelBlueColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor plotPointerLineColor READ plotPointerLineColor NOTIFY ThemeChanged)
  Q_PROPERTY(qreal plotInactiveLineOpacity READ plotInactiveLineOpacity CONSTANT)
  Q_PROPERTY(qreal plotCurveHitRadius READ plotCurveHitRadius CONSTANT)
  Q_PROPERTY(qreal plotChannelMarkSize READ plotChannelMarkSize CONSTANT)
  // Nodes graph canvas and node-card geometry. Colors follow the active theme;
  // sizes are logical px. See DESIGN.md "Nodes panel and graph nodes".
  Q_PROPERTY(QColor graphCanvasColor READ graphCanvasColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor graphEdgeColor READ graphEdgeColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor graphCandidateEdgeColor READ graphCandidateEdgeColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor graphPortFillColor READ graphPortFillColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor graphPortBorderColor READ graphPortBorderColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor graphNodeBorderColor READ graphNodeBorderColor NOTIFY ThemeChanged)
  Q_PROPERTY(
      QColor graphMaskDrawerSurfaceColor READ graphMaskDrawerSurfaceColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor graphSelectionOutlineColor READ graphSelectionOutlineColor NOTIFY ThemeChanged)
  Q_PROPERTY(int graphNodeWidth READ graphNodeWidth CONSTANT)
  Q_PROPERTY(int graphNodeVerticalGap READ graphNodeVerticalGap CONSTANT)
  Q_PROPERTY(int graphNodeOriginX READ graphNodeOriginX CONSTANT)
  Q_PROPERTY(int graphNodeOriginY READ graphNodeOriginY CONSTANT)
  Q_PROPERTY(int graphEndpointHeight READ graphEndpointHeight CONSTANT)
  Q_PROPERTY(int graphNameRowHeight READ graphNameRowHeight CONSTANT)
  Q_PROPERTY(int graphNameRowDividerHeight READ graphNameRowDividerHeight CONSTANT)
  Q_PROPERTY(int graphMaskDrawerHeaderHeight READ graphMaskDrawerHeaderHeight CONSTANT)
  Q_PROPERTY(int graphMaskRowHeight READ graphMaskRowHeight CONSTANT)
  Q_PROPERTY(int maskGroupPreviewSize READ maskGroupPreviewSize CONSTANT)
  Q_PROPERTY(int maskGroupMaskPreviewSize READ maskGroupMaskPreviewSize CONSTANT)
  Q_PROPERTY(int graphPortSize READ graphPortSize CONSTANT)
  Q_PROPERTY(int graphPortHitSize READ graphPortHitSize CONSTANT)
  Q_PROPERTY(int graphEdgeWidth READ graphEdgeWidth CONSTANT)
  Q_PROPERTY(int graphSelectionOutlineWidth READ graphSelectionOutlineWidth CONSTANT)
  // Mask overlay: two-layer control stroke, no coverage-area color. Sizes are
  // logical pixels and stay constant at any zoom or DPR.
  Q_PROPERTY(QColor maskOverlayControlColor READ maskOverlayControlColor NOTIFY ThemeChanged)
  Q_PROPERTY(QColor maskOverlayControlOutlineColor READ maskOverlayControlOutlineColor NOTIFY
                 ThemeChanged)
  Q_PROPERTY(QColor maskOverlayInactiveColor READ maskOverlayInactiveColor NOTIFY ThemeChanged)
  Q_PROPERTY(int maskOverlayHandleRadius READ maskOverlayHandleRadius CONSTANT)
  Q_PROPERTY(qreal maskOverlayHandleOutlineWidth READ maskOverlayHandleOutlineWidth CONSTANT)
  Q_PROPERTY(qreal maskOverlayStrokeWidth READ maskOverlayStrokeWidth CONSTANT)
  Q_PROPERTY(qreal maskOverlayAntialiasWidth READ maskOverlayAntialiasWidth CONSTANT)
  Q_PROPERTY(int maskOverlayHandleHitRadius READ maskOverlayHandleHitRadius CONSTANT)
  Q_PROPERTY(int maskOverlayRotateHandleOffset READ maskOverlayRotateHandleOffset CONSTANT)
  Q_PROPERTY(qreal maskOverlayGuideOuterWidth READ maskOverlayGuideOuterWidth CONSTANT)
  Q_PROPERTY(qreal maskOverlayGuideInnerWidth READ maskOverlayGuideInnerWidth CONSTANT)
  Q_PROPERTY(qreal maskOverlayGripOuterWidth READ maskOverlayGripOuterWidth CONSTANT)
  Q_PROPERTY(qreal maskOverlayGripInnerWidth READ maskOverlayGripInnerWidth CONSTANT)
  Q_PROPERTY(qreal maskOverlayGripSpanT0 READ maskOverlayGripSpanT0 CONSTANT)
  Q_PROPERTY(qreal maskOverlayGripSpanT1 READ maskOverlayGripSpanT1 CONSTANT)
  // Mask coverage display (SM8.6): the tint of the one viewport composite. Presentation only; it
  // never reaches export, thumbnails, or scopes.
  Q_PROPERTY(QColor maskCoverageTintColor READ maskCoverageTintColor CONSTANT)
  Q_PROPERTY(qreal maskCoverageTintOpacity READ maskCoverageTintOpacity CONSTANT)
  // AI Mask tool input markers on the photograph (SM9): fixed in every theme, like the tint.
  Q_PROPERTY(QColor aiMaskInputAddColor READ aiMaskInputAddColor CONSTANT)
  Q_PROPERTY(QColor aiMaskInputSubtractColor READ aiMaskInputSubtractColor CONSTANT)
  // Library color label codes 0 Red ... 6 Gray (#768): fixed in every theme, in code order.
  Q_PROPERTY(QVariantList colorLabelColors READ colorLabelColors CONSTANT)
  Q_PROPERTY(QColor dateGraphLevel0Color READ dateGraphLevel0Color NOTIFY ThemeChanged)
  Q_PROPERTY(QColor dateGraphLevel1Color READ dateGraphLevel1Color NOTIFY ThemeChanged)
  Q_PROPERTY(QColor dateGraphLevel2Color READ dateGraphLevel2Color NOTIFY ThemeChanged)
  Q_PROPERTY(QColor dateGraphLevel3Color READ dateGraphLevel3Color NOTIFY ThemeChanged)
  Q_PROPERTY(QColor dateGraphLevel4Color READ dateGraphLevel4Color NOTIFY ThemeChanged)

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
  static auto EditorPanelToggleStyle(bool active, bool is_first = false, bool is_last = false)
      -> QString;
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

  auto        uiFontFamily() const -> QString;
  auto        headlineFontFamily() const -> QString;
  auto        dataFontFamily() const -> QString;
  auto        monoFontFamily() const -> QString;
  auto        appVersion() const -> QString;

  auto        toneGold() const -> QColor;
  auto        toneWine() const -> QColor;
  auto        toneSteel() const -> QColor;
  auto        toneGraphite() const -> QColor;
  auto        toneMist() const -> QColor;

  auto        bgCanvasColor() const -> QColor;
  auto        bgDeepColor() const -> QColor;
  auto        bgBaseColor() const -> QColor;
  auto        bgPanelColor() const -> QColor;
  auto        textColor() const -> QColor;
  auto        textMutedColor() const -> QColor;
  auto        iconColor() const -> QColor;
  auto        accentColor() const -> QColor;
  auto        accentSecondaryColor() const -> QColor;
  auto        dangerColor() const -> QColor;
  auto        dangerTintColor() const -> QColor;
  auto        alertBadgeFillColor() const -> QColor;
  auto        alertBadgeTextColor() const -> QColor;
  auto        backgroundTaskFinishedColor() const -> QColor;
  auto        backgroundTaskWorkingColor() const -> QColor;
  auto        backgroundTaskFailedColor() const -> QColor;
  auto        activeModelBorderColor() const -> QColor;
  auto        importRawRingColor() const -> QColor;
  auto        importOtherRingColor() const -> QColor;
  auto        selectedTintColor() const -> QColor;
  auto        hoverColor() const -> QColor;
  auto        dividerColor() const -> QColor;
  auto        glassPanelColor() const -> QColor;
  auto        glassStrokeColor() const -> QColor;
  auto        overlayColor() const -> QColor;
  auto        panelRadius() const -> int;

  auto        controlRadius() const -> int;
  auto        controlRadiusSmall() const -> int;
  auto        badgeRadius() const -> int;
  auto        iconOpticalSize() const -> int;
  auto        iconOpticalSizeCompact() const -> int;
  auto        iconSourceSize() const -> int;
  auto        iconSourceSizeCompact() const -> int;
  auto        iconButtonHitSize() const -> int;
  auto        iconButtonHitSizeCompact() const -> int;
  auto        editorSidePanelWidth() const -> int;
  auto        editorSidePanelWidthMin() const -> int;
  auto        editorSidePanelWidthMax() const -> int;
  auto        editorScopeHeight() const -> int;
  auto        editorScopeHeightMin() const -> int;
  auto        editorAdjustmentHeaderMinHeight() const -> int;
  auto        editorLutBrowserPanelWidth() const -> int;
  auto        editorLutBrowserPanelWidthMin() const -> int;
  auto        editorLutBrowserFilterWidth() const -> int;
  auto        editorLutTileMinWidth() const -> int;
  auto        editorLutTileCompactWidth() const -> int;
  auto        editorLutTileIconSize() const -> int;
  auto        collectionsSidebarWidth() const -> int;
  auto        collectionRowHeight() const -> int;
  auto        collectionIndentStep() const -> int;
  auto        collectionMenuMaxRows() const -> int;
  auto        collectionMenuFilterThreshold() const -> int;
  auto        searchPaletteMaxWidth() const -> int;
  auto        searchPaletteWindowMarginX() const -> int;
  auto        searchPaletteWindowMarginY() const -> int;
  auto        welcomeCardWidth() const -> int;
  auto        welcomeCardHeight() const -> int;
  auto        welcomeSidebarWidth() const -> int;
  auto        welcomeCoverWidth() const -> int;
  auto        welcomeCoverCompactWidth() const -> int;
  auto        welcomeCoverHeight() const -> int;
  auto        welcomeInfoMinWidth() const -> int;
  auto        welcomeEmptyContentWidth() const -> int;
  auto        skeletonCycleMs() const -> int;
  auto             loadingWavePhase() const -> qreal;
  auto             loadingWaveCycleMs() const -> int;
  auto             loadingWaveFrameMs() const -> int;
  /// Number of objects that hold the loading wave clock; the clock runs while it is above 0.
  auto             loadingWaveHolders() const -> int;
  /// @p holder holds the loading wave clock while @p holds is true. A destroyed holder lets go
  /// by itself, so a mark that is torn down without its QML handlers releases the clock too.
  Q_INVOKABLE void setLoadingWaveHolder(QObject* holder, bool holds);
  auto        lineHeightCaption() const -> int;
  auto        lineHeightBody() const -> int;
  auto        lineHeightTitle() const -> int;
  auto        lineHeightSection() const -> int;
  auto        lineHeightHeadline() const -> int;
  auto        spaceXs() const -> int;
  auto        spaceSm() const -> int;
  auto        spaceMd() const -> int;
  auto        spaceLg() const -> int;
  auto        spaceXl() const -> int;
  auto        dateGraphCellMinSize() const -> int;
  auto        dateGraphCellGap() const -> int;
  auto        dateGraphCellRadius() const -> int;
  auto        librarySectionHeaderHeight() const -> int;
  auto        inspectorHeaderActionSize() const -> int;
  auto        motionFoldOpenMs() const -> int;
  auto        motionFoldCloseMs() const -> int;
  auto        motionFadeMs() const -> int;
  auto        motionEasing() const -> int;
  auto        backgroundTaskAutoCollapseMs() const -> int;
  auto        reduceMotion() const -> bool;
  void        setReduceMotion(bool enabled);
  auto        fontSizeCaption() const -> int;
  auto        fontSizeBody() const -> int;
  auto        fontSizeTitle() const -> int;
  auto        fontSizeSection() const -> int;
  auto        fontSizeHeadline() const -> int;
  auto        fontSizeDisplay() const -> int;
  auto        fontWeightRegular() const -> int;
  auto        fontWeightStrong() const -> int;
  auto        fontWeightHeading() const -> int;
  auto        cardSurfaceColor() const -> QColor;
  auto        cardBorderColor() const -> QColor;
  auto        buttonIdleFillColor() const -> QColor;
  auto        buttonHoveredFillColor() const -> QColor;
  auto        buttonPressedFillColor() const -> QColor;
  auto        buttonSelectedFillColor() const -> QColor;
  auto        disabledSurfaceColor() const -> QColor;
  auto        controlFillColor() const -> QColor;
  auto        controlHoveredFillColor() const -> QColor;
  auto        controlPressedFillColor() const -> QColor;
  auto        controlBorderColor() const -> QColor;
  auto        editorSliderTrackColor() const -> QColor { return EditorSliderTrackColor(); }
  auto        editorSliderPositiveColor() const -> QColor { return EditorSliderAccentColor(true); }
  auto        editorSliderNegativeColor() const -> QColor { return EditorSliderAccentColor(false); }
  auto        editorSliderHandleColor() const -> QColor { return EditorSliderHandleColor(); }
  auto editorSliderHandleBorderColor() const -> QColor { return EditorSliderHandleBorderColor(); }
  auto editorListSelectedFillColor() const -> QColor;
  auto editorListSelectedInkColor() const -> QColor;
  auto editorListFavoriteIdleColor() const -> QColor;
  auto editorListFavoriteActiveColor() const -> QColor;
  auto editorListFavoriteIdleOnSelectedColor() const -> QColor;
  auto editorListFavoriteActiveOnSelectedColor() const -> QColor;
  auto ratingStarEmptyColor() const -> QColor;
  auto thumbnailEditMarkFillColor() const -> QColor;
  auto thumbnailDetailsScrimColor() const -> QColor;
  auto scopeGridColor() const -> QColor;
  auto scopePlotBorderColor() const -> QColor;
  auto scopeHistogramRedColor() const -> QColor;
  auto scopeHistogramGreenColor() const -> QColor;
  auto scopeHistogramBlueColor() const -> QColor;
  auto scopeMarkerColor() const -> QColor;
  auto plotSurfaceColor() const -> QColor;
  auto plotGridColor() const -> QColor;
  auto plotGridStrongColor() const -> QColor;
  auto plotLabelColor() const -> QColor;
  auto plotLineColor() const -> QColor;
  auto plotReferenceLineColor() const -> QColor;
  auto plotHandleColor() const -> QColor;
  auto plotHandleOutlineColor() const -> QColor;
  auto plotGridLineWidth() const -> qreal;
  auto plotLineWidth() const -> qreal;
  auto plotHandleRadius() const -> qreal;
  auto plotHandleRingWidth() const -> qreal;
  auto plotHandleOutlineWidth() const -> qreal;
  auto plotHitRadius() const -> qreal;
  auto plotChannelRedColor() const -> QColor;
  auto plotChannelGreenColor() const -> QColor;
  auto plotChannelBlueColor() const -> QColor;
  auto plotPointerLineColor() const -> QColor;
  auto plotInactiveLineOpacity() const -> qreal;
  auto plotCurveHitRadius() const -> qreal;
  auto plotChannelMarkSize() const -> qreal;
  auto graphCanvasColor() const -> QColor;
  auto graphEdgeColor() const -> QColor;
  auto graphCandidateEdgeColor() const -> QColor;
  auto graphPortFillColor() const -> QColor;
  auto graphPortBorderColor() const -> QColor;
  auto graphNodeBorderColor() const -> QColor;
  auto graphMaskDrawerSurfaceColor() const -> QColor;
  auto graphSelectionOutlineColor() const -> QColor;
  auto graphNodeWidth() const -> int;
  auto graphNodeVerticalGap() const -> int;
  auto graphNodeOriginX() const -> int;
  auto graphNodeOriginY() const -> int;
  auto graphEndpointHeight() const -> int;
  auto graphNameRowHeight() const -> int;
  auto graphNameRowDividerHeight() const -> int;
  auto graphMaskDrawerHeaderHeight() const -> int;
  auto graphMaskRowHeight() const -> int;
  auto maskGroupPreviewSize() const -> int;
  auto maskGroupMaskPreviewSize() const -> int;
  auto graphPortSize() const -> int;
  auto graphPortHitSize() const -> int;
  auto graphEdgeWidth() const -> int;
  auto graphSelectionOutlineWidth() const -> int;
  auto maskOverlayControlColor() const -> QColor;
  auto maskOverlayControlOutlineColor() const -> QColor;
  auto maskOverlayInactiveColor() const -> QColor;
  auto maskOverlayHandleRadius() const -> int;
  auto maskOverlayHandleOutlineWidth() const -> qreal;
  auto maskOverlayStrokeWidth() const -> qreal;
  auto maskOverlayAntialiasWidth() const -> qreal;
  auto maskOverlayHandleHitRadius() const -> int;
  auto maskOverlayRotateHandleOffset() const -> int;
  auto maskOverlayGuideOuterWidth() const -> qreal;
  auto maskOverlayGuideInnerWidth() const -> qreal;
  auto maskOverlayGripOuterWidth() const -> qreal;
  auto maskOverlayGripInnerWidth() const -> qreal;
  auto maskOverlayGripSpanT0() const -> qreal;
  auto maskOverlayGripSpanT1() const -> qreal;
  auto maskCoverageTintColor() const -> QColor;
  auto maskCoverageTintOpacity() const -> qreal;
  auto aiMaskInputAddColor() const -> QColor;
  auto aiMaskInputSubtractColor() const -> QColor;
  auto colorLabelColors() const -> QVariantList;
  auto dateGraphLevel0Color() const -> QColor;
  auto dateGraphLevel1Color() const -> QColor;
  auto dateGraphLevel2Color() const -> QColor;
  auto dateGraphLevel3Color() const -> QColor;
  auto dateGraphLevel4Color() const -> QColor;

  auto currentThemeIndex() const -> int;
  void setCurrentThemeIndex(int index);
  auto availableThemes() const -> QVariantList;

 signals:
  void UiFontFamilyChanged();
  void ThemeChanged();
  void ReduceMotionChanged();
  void LoadingWavePhaseChanged();

 private:
  explicit AppTheme(QObject* parent = nullptr);

  static auto  ResolveRole(QWidget* widget) -> FontRole;
  void                                     AdvanceLoadingWave();

  void                                     UpdateLoadingWaveTimer();

  QTimer*                                  loading_wave_timer_ = nullptr;
  QElapsedTimer                            loading_wave_clock_{};
  QHash<QObject*, QMetaObject::Connection> loading_wave_holders_{};
  qreal                                    loading_wave_phase_   = 0.0;

  int          current_theme_index_  = 0;
  // Lazy-loaded from QSettings("ui/reduceMotion") on first read so the
  // singleton is safe to construct before QCoreApplication org/app are set.
  mutable bool reduce_motion_loaded_ = false;
  mutable bool reduce_motion_        = false;
};

}  // namespace alcedo::ui
