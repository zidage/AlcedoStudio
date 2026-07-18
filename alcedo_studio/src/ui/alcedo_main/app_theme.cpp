//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/app_theme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVariantMap>
#include <QWidget>

#include <algorithm>

namespace alcedo::ui {
namespace {

constexpr char kThemeFontRoleProperty[] = "puerhlabFontRole";

struct ThemeColors {
  QColor tone_gold;
  QColor tone_wine;
  QColor tone_steel;
  QColor tone_graphite;
  QColor tone_mist;
  QColor bg_canvas;
  QColor bg_deep;
  QColor bg_base;
  QColor bg_panel;
  QColor text;
  QColor text_muted;
  QColor icon;
  QColor accent_secondary;
  QColor danger_tint;
  QColor selected_tint;
  QColor hover;
  QColor divider;
  QColor glass_panel;
  QColor glass_stroke;
  QColor overlay;
  int panel_radius;
  // Phase 4D: opaque button-state fills, pre-computed from bg_panel + hover.
  QColor button_idle_fill;
  QColor button_hovered_fill;
  QColor button_pressed_fill;
  QColor button_selected_fill;
  QColor disabled_surface;  // bg_panel blended toward bg_canvas (simulates 0.55 opacity)
};

auto BrandBlueBase() -> QColor { return QColor(104, 146, 185); }
auto BrandBlueHover() -> QColor { return QColor(118, 160, 199); }
auto BrandBluePressed() -> QColor { return QColor(86, 127, 165); }

// Theme 0: Alcedo — layered matte surfaces, saffron/steel/teal CTA palette
auto MakePuerhTheme() -> ThemeColors {
  return ThemeColors{
      .tone_gold = BrandBlueBase(),               // primary brand blue / CTA
      .tone_wine = QColor(0x8A, 0x3A, 0x3A),      // danger red
      .tone_steel = BrandBluePressed(),           // pressed / deeper blue
      .tone_graphite = QColor(0x11, 0x11, 0x11),
      .tone_mist = QColor(0xE0, 0xE0, 0xE0),
      .bg_canvas = QColor(0x12, 0x12, 0x12),      // floor — outermost surface
      .bg_deep = QColor(0x2E, 0x2E, 0x2E),        // floating modals / popovers
      .bg_base = QColor(0x24, 0x24, 0x24),        // interactive panels, inputs
      .bg_panel = QColor(0x1A, 0x1A, 0x1A),       // primary workspaces
      .text = QColor(0xE0, 0xE0, 0xE0),
      .text_muted = QColor(0x88, 0x88, 0x88),
      .icon = QColor(0xFF, 0xFF, 0xFF),
      .accent_secondary = BrandBlueHover(),       // hover / secondary blue
      .danger_tint = QColor(138, 58, 58, 80),
      .selected_tint = QColor(104, 146, 185, 46),
      .hover = QColor(0x26, 0x25, 0x25),
      .divider = QColor(200, 200, 200, 16),
      .glass_panel = QColor(0x1A, 0x1A, 0x1A),
      .glass_stroke = QColor(200, 200, 200, 20),
      .overlay = QColor(0x0A, 0x0A, 0x0A, 0xC8),
      .panel_radius = 10,
      // Phase 4D: opaque button fills. Idle matches the card/panel surface so
      // icon chrome is quiet; hover/pressed/selected step to the hover well so
      // selected actions read on both the card shell and the lighter base
      // inset track (scope / adjustment nav).
      .button_idle_fill = QColor(0x1A, 0x1A, 0x1A),        // = bg_panel
      .button_hovered_fill = QColor(0x20, 0x20, 0x20),      // between panel and base
      .button_pressed_fill = QColor(0x26, 0x25, 0x25),      // = hover (engaged)
      .button_selected_fill = QColor(0x26, 0x25, 0x25),     // = pressed / hover
      .disabled_surface = QColor(0x16, 0x16, 0x16),         // Blend(bg_panel, bg_canvas, 0.45)
  };
}

// Theme 1: Classic (previous neutral theme)
auto MakeClassicTheme() -> ThemeColors {
  return ThemeColors{
      .tone_gold = QColor(0xFC, 0xC7, 0x04),
      .tone_wine = QColor(0x8A, 0x05, 0x26),
      .tone_steel = QColor(0x4A, 0x4A, 0x4A),
      .tone_graphite = QColor(0x1A, 0x1A, 0x1A),
      .tone_mist = QColor(0xE6, 0xE6, 0xE6),
      .bg_canvas = QColor(0x0E, 0x0E, 0x0E),
      .bg_deep = QColor(0x11, 0x11, 0x11),
      .bg_base = QColor(0x17, 0x17, 0x17),
      .bg_panel = QColor(0x22, 0x22, 0x22),
      .text = QColor(0xD0, 0xD0, 0xD0),
      .text_muted = QColor(0x88, 0x88, 0x88),
      .icon = QColor(0xFF, 0xFF, 0xFF),
      .accent_secondary = QColor(0xFC, 0xC7, 0x04),
      .danger_tint = QColor(138, 5, 38, 82),
      .selected_tint = QColor(252, 199, 4, 46),
      .hover = QColor(0x2B, 0x2B, 0x2B),
      .divider = QColor(230, 230, 230, 20),
      .glass_panel = QColor(0x22, 0x22, 0x22),
      .glass_stroke = QColor(230, 230, 230, 28),
      .overlay = QColor(0x12, 0x12, 0x12, 0xC0),
      .panel_radius = 8,
      // Phase 4D: opaque button fills — idle = panel; engaged = hover well.
      .button_idle_fill = QColor(0x22, 0x22, 0x22),        // = bg_panel
      .button_hovered_fill = QColor(0x27, 0x27, 0x27),      // between panel and hover
      .button_pressed_fill = QColor(0x2B, 0x2B, 0x2B),      // = hover
      .button_selected_fill = QColor(0x2B, 0x2B, 0x2B),     // = pressed / hover
      .disabled_surface = QColor(0x19, 0x19, 0x19),         // Blend(bg_panel, bg_canvas, 0.45)
  };
}

auto GetTheme(int index) -> const ThemeColors& {
  static const ThemeColors kPuerhTheme = MakePuerhTheme();
  static const ThemeColors kClassicTheme = MakeClassicTheme();
  return index == 1 ? kClassicTheme : kPuerhTheme;
}

struct FontFamilies {
  QString ui_latin = QStringLiteral("Inter");
  QString ui_zh = QStringLiteral("Noto Sans SC");
  QString ui_override;
  QString ui_headline = QStringLiteral("Manrope");
  QString ui_headline_zh = QStringLiteral("Noto Sans SC");
  QString effective_language_code = QStringLiteral("en");
  QString data = QStringLiteral("IBM Plex Sans");
  QString mono = QStringLiteral("IBM Plex Sans");
};

auto FontState() -> FontFamilies& {
  static FontFamilies families;
  return families;
}

auto FontsRegisteredFlag() -> bool& {
  static bool loaded = false;
  return loaded;
}

auto IsChineseLanguageCode(const QString& code) -> bool {
  return code.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive);
}

auto ActiveUiFamily(const FontFamilies& families) -> const QString& {
  if (!families.ui_override.isEmpty()) {
    return families.ui_override;
  }
  if (IsChineseLanguageCode(families.effective_language_code)) {
    return families.ui_zh;
  }
  return families.ui_latin;
}

void RefreshTopLevelWidgetFonts() {
  const auto widgets = QApplication::topLevelWidgets();
  for (QWidget* widget : widgets) {
    AppTheme::ApplyFontsRecursively(widget);
  }
}

auto RegisterFontResource(const QString& path, const QString& preferred_family) -> QString {
  const int font_id = QFontDatabase::addApplicationFont(path);
  if (font_id < 0) {
    return preferred_family;
  }

  const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
  if (families.isEmpty()) {
    return preferred_family;
  }

  if (preferred_family.isEmpty()) {
    return families.front();
  }

  for (const QString& family : families) {
    if (family.compare(preferred_family, Qt::CaseInsensitive) == 0 ||
        family.startsWith(preferred_family, Qt::CaseInsensitive)) {
      return family;
    }
  }
  return families.front();
}

void ConfigureTextRendering(QFont& font) {
  font.setStyleStrategy(
      static_cast<QFont::StyleStrategy>(QFont::PreferAntialias | QFont::PreferOutline));
#if defined(_WIN32)
  // Match the softer glyph look used by Qt Quick text rendering.
  font.setHintingPreference(QFont::PreferNoHinting);
#endif
}

void AppendUniqueFamily(QStringList& stack, const QString& family) {
  if (family.isEmpty()) {
    return;
  }
  if (!stack.contains(family, Qt::CaseInsensitive)) {
    stack.push_back(family);
  }
}

auto UiFontStack(const FontFamilies& families) -> QStringList {
  QStringList stack;
  AppendUniqueFamily(stack, ActiveUiFamily(families));
  AppendUniqueFamily(stack, families.ui_zh);
  AppendUniqueFamily(stack, families.ui_latin);
  return stack;
}

auto DataFontFallbackStack(const FontFamilies& families) -> QStringList {
  QStringList stack;
  if (IsChineseLanguageCode(families.effective_language_code)) {
    AppendUniqueFamily(stack, families.ui_zh);
    AppendUniqueFamily(stack, families.ui_latin);
  } else {
    AppendUniqueFamily(stack, families.ui_latin);
    AppendUniqueFamily(stack, families.ui_zh);
  }
  return stack;
}

auto HeadlineFontStack(const FontFamilies& families) -> QStringList {
  QStringList stack;
  AppendUniqueFamily(stack, families.ui_headline);
  AppendUniqueFamily(stack, families.ui_headline_zh);
  AppendUniqueFamily(stack, families.ui_zh);
  AppendUniqueFamily(stack, families.ui_latin);
  return stack;
}

auto DataFontStack(const FontFamilies& families) -> QStringList {
  QStringList stack;
  AppendUniqueFamily(stack, families.data);
  const QStringList fallbacks = DataFontFallbackStack(families);
  for (const QString& fallback : fallbacks) {
    AppendUniqueFamily(stack, fallback);
  }
  return stack;
}

void ApplyFamilySubstitutions(const QString& family, const QStringList& fallbacks) {
  if (family.isEmpty()) {
    return;
  }

  QStringList substitution_stack;
  for (const QString& fallback : fallbacks) {
    AppendUniqueFamily(substitution_stack, fallback);
  }

  QFont::removeSubstitutions(family);
  if (!substitution_stack.isEmpty()) {
    QFont::insertSubstitutions(family, substitution_stack);
  }
}

void ApplyUiFontSubstitutions(const FontFamilies& families) {
  ApplyFamilySubstitutions(families.ui_latin, QStringList{families.ui_zh});
  ApplyFamilySubstitutions(
      families.ui_headline,
      QStringList{families.ui_headline_zh, families.ui_zh, families.ui_latin});
}

void ApplyDataFontSubstitutions(const FontFamilies& families) {
  ApplyFamilySubstitutions(families.data, DataFontFallbackStack(families));
}

auto MakeFont(const QStringList& family_stack, qreal point_size, QFont::Weight weight,
              bool italic = false) -> QFont {
  QFont font;
  font.setFamilies(family_stack);
  font.setPointSizeF(point_size);
  font.setWeight(weight);
  font.setItalic(italic);
  ConfigureTextRendering(font);
  return font;
}

auto Hex(const QColor& color) -> QString {
  return color.name(QColor::HexRgb);
}

auto Rgba(const QColor& color) -> QString {
  return color.name(QColor::HexArgb);
}

auto WithAlpha(const QColor& color, int alpha) -> QColor {
  QColor tinted(color);
  tinted.setAlpha(std::clamp(alpha, 0, 255));
  return tinted;
}

auto Blend(const QColor& a, const QColor& b, qreal ratio) -> QColor {
  const qreal t = std::clamp(ratio, 0.0, 1.0);
  const auto lerp = [t](int lhs, int rhs) {
    return static_cast<int>(std::lround(lhs + (rhs - lhs) * t));
  };

  return QColor(lerp(a.red(), b.red()), lerp(a.green(), b.green()), lerp(a.blue(), b.blue()),
                lerp(a.alpha(), b.alpha()));
}

}  // namespace

AppTheme::AppTheme(QObject* parent) : QObject(parent) {}

auto AppTheme::Instance() -> AppTheme& {
  static AppTheme instance;
  return instance;
}

void AppTheme::RegisterFonts() {
  if (FontsRegisteredFlag()) {
    return;
  }

  auto& families = FontState();
  families.ui_latin =
      RegisterFontResource(QStringLiteral(":/fonts/main_Inter.ttf"), QStringLiteral("Inter"));
  RegisterFontResource(QStringLiteral(":/fonts/main_Inter_italic.ttf"), families.ui_latin);
  families.ui_zh = RegisterFontResource(QStringLiteral(":/fonts/main_NotoSans_zh.ttf"),
                                        QStringLiteral("Noto Sans SC"));
  families.data =
      RegisterFontResource(QStringLiteral(":/fonts/main_IBM.ttf"), QStringLiteral("IBM Plex Sans"));
  // Chinese fallback for the Manrope headline font. The struct default is
  // "Noto Sans SC" (same family used for the rest of the UI's Chinese text);
  // we intentionally do NOT override it with the decorative Dinglie Song
  // Typeface, so Chinese headlines render in Noto Sans SC like everything
  // else. The art font stays bundled in resources but is no longer loaded.
  const QString registered_headline =
      RegisterFontResource(QStringLiteral(":/fonts/main_Manrope.ttf"), QStringLiteral("Manrope"));
  if (!registered_headline.isEmpty()) {
    families.ui_headline = registered_headline;
  }
  families.mono = families.data;

  ApplyUiFontSubstitutions(families);
  ApplyDataFontSubstitutions(families);

  FontsRegisteredFlag() = true;
}

void AppTheme::SetEffectiveLanguageCode(const QString& code) {
  RegisterFonts();

  auto& families = FontState();
  const QString previous_family = ActiveUiFamily(families);
  const bool previous_is_chinese = IsChineseLanguageCode(families.effective_language_code);
  const QString normalized_code =
      code.trimmed().isEmpty() ? QStringLiteral("en") : code.trimmed();
  families.effective_language_code = normalized_code;
  const bool current_is_chinese = IsChineseLanguageCode(families.effective_language_code);

  ApplyUiFontSubstitutions(families);
  ApplyDataFontSubstitutions(families);

  if (ActiveUiFamily(families) == previous_family &&
      previous_is_chinese == current_is_chinese) {
    return;
  }

  RefreshTopLevelWidgetFonts();
  emit Instance().UiFontFamilyChanged();
}

auto AppTheme::TryRegisterUiFontOverride(const QString& path) -> bool {
  if (path.isEmpty()) {
    return false;
  }

  RegisterFonts();

  const int font_id = QFontDatabase::addApplicationFont(path);
  if (font_id < 0) {
    return false;
  }

  const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
  if (families.isEmpty()) {
    return false;
  }

  auto& state = FontState();
  const QString previous_family = ActiveUiFamily(state);
  state.ui_override = families.front();
  if (ActiveUiFamily(state) != previous_family) {
    RefreshTopLevelWidgetFonts();
    emit Instance().UiFontFamilyChanged();
  }
  return true;
}

void AppTheme::ApplyApplicationFont(QApplication& app) {
  RegisterFonts();

  const auto& families = FontState();
  QFont app_font = app.font();
  app_font.setFamilies(UiFontStack(families));
  ConfigureTextRendering(app_font);
  app.setFont(app_font);
  RefreshTopLevelWidgetFonts();
}

void AppTheme::ApplyApplicationFont() {
  if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
    ApplyApplicationFont(*app);
  }
}

auto AppTheme::Font(FontRole role) -> QFont {
  RegisterFonts();
  const auto& families = FontState();
  const QStringList ui_stack = UiFontStack(families);
  const QStringList headline_stack = HeadlineFontStack(families);
  const QStringList data_stack = DataFontStack(families);
  const QStringList mono_stack{families.mono};

  switch (role) {
    case FontRole::UiBody:
      return MakeFont(ui_stack, 11.0, QFont::Medium);
    case FontRole::UiBodyStrong:
      return MakeFont(ui_stack, 11.0, QFont::DemiBold);
    case FontRole::UiCaption:
      return MakeFont(ui_stack, 10.0, QFont::Medium);
    case FontRole::UiCaptionStrong:
      return MakeFont(ui_stack, 10.0, QFont::DemiBold);
    case FontRole::UiTitle:
      return MakeFont(ui_stack, 11.0, QFont::DemiBold);
    case FontRole::UiHeadline:
      return MakeFont(headline_stack, 14.0, QFont::Bold);
    case FontRole::UiOverline: {
      QFont font = MakeFont(ui_stack, 9.0, QFont::DemiBold);
      font.setCapitalization(QFont::AllUppercase);
      font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
      return font;
    }
    case FontRole::UiHint:
      return MakeFont(ui_stack, 9.0, QFont::Medium);
    case FontRole::DataBody:
      return MakeFont(data_stack, 11.0, QFont::Medium);
    case FontRole::DataBodyStrong:
      return MakeFont(data_stack, 11.0, QFont::DemiBold);
    case FontRole::DataCaption:
      return MakeFont(data_stack, 10.0, QFont::Medium);
    case FontRole::DataNumeric:
      return MakeFont(data_stack, 12.0, QFont::DemiBold);
    case FontRole::DataOverlay:
      return MakeFont(data_stack, 10.0, QFont::DemiBold);
    case FontRole::MonoBody:
      return MakeFont(mono_stack, 10.0, QFont::DemiBold);
    case FontRole::MonoCaption:
      return MakeFont(mono_stack, 9.0, QFont::Normal);
  }

  return MakeFont(ui_stack, 11.0, QFont::Normal);
}

void AppTheme::ApplyFont(QWidget* widget, FontRole role) {
  if (!widget) {
    return;
  }
  const QFont themed_font = Font(role);
  widget->setFont(themed_font);

  if (auto* combo = qobject_cast<QComboBox*>(widget)) {
    if (QAbstractItemView* view = combo->view()) {
      view->setFont(themed_font);
      if (QWidget* popup = view->window()) {
        popup->setFont(themed_font);
      }
    }
  }
}

void AppTheme::MarkFontRole(QObject* object, FontRole role) {
  if (!object) {
    return;
  }
  object->setProperty(kThemeFontRoleProperty, static_cast<int>(role));
  if (auto* widget = qobject_cast<QWidget*>(object)) {
    ApplyFont(widget, role);
  }
}

void AppTheme::ApplyFontsRecursively(QWidget* root) {
  if (!root) {
    return;
  }

  ApplyFont(root, ResolveRole(root));

  const auto widgets = root->findChildren<QWidget*>();
  for (QWidget* widget : widgets) {
    ApplyFont(widget, ResolveRole(widget));
  }
}

auto AppTheme::EditorLabelStyle(const QColor& color) -> QString {
  return QStringLiteral("QLabel { color: %1; }").arg(color.name(QColor::HexRgb));
}

auto AppTheme::EditorPrimaryButtonStyle(bool include_disabled) -> QString {
  const auto&  theme         = AppTheme::Instance();
  const QColor accent        = theme.accentColor();
  const QColor accent_hover  = theme.accentSecondaryColor();
  const QColor accent_press  = theme.toneSteel();
  const QColor dark_text     = theme.bgCanvasColor();
  const QColor disabled_text = theme.textMutedColor();
  const QColor disabled_bg   = theme.bgBaseColor();
  QString style =
      QStringLiteral("QPushButton {"
                     "  color: %1;"
                     "  background: %2;"
                     "  border: none;"
                     "  border-radius: 10px;"
                     "  padding: 0 14px;"
                     "}"
                     "QPushButton:hover {"
                     "  background: %3;"
                     "}"
                     "QPushButton:pressed {"
                     "  background: %4;"
                     "}")
          .arg(Hex(dark_text), Hex(accent), Hex(accent_hover), Hex(accent_press));
  if (include_disabled) {
    style += QStringLiteral("QPushButton:disabled {"
                            "  color: %1;"
                            "  background: %2;"
                            "}")
                 .arg(Hex(disabled_text), Hex(disabled_bg));
  }
  return style;
}

auto AppTheme::EditorSecondaryButtonStyle() -> QString {
  const auto&  theme   = AppTheme::Instance();
  const QColor text    = theme.textColor();
  const QColor bg      = theme.bgPanelColor();
  const QColor hover   = theme.hoverColor();
  const QColor border  = theme.glassStrokeColor();
  return QStringLiteral("QPushButton {"
                        "  color: %1;"
                        "  background: %2;"
                        "  border: 1px solid %3;"
                        "  border-radius: 10px;"
                        "  padding: 0 14px;"
                        "}"
                        "QPushButton:hover {"
                        "  background: %4;"
                        "  border-color: %5;"
                        "}"
                        "QPushButton:pressed {"
                        "  background: %6;"
                        "}")
      .arg(Hex(text), Rgba(WithAlpha(bg, 224)), Rgba(border), Rgba(WithAlpha(hover, 245)),
           Rgba(WithAlpha(border, 176)), Rgba(WithAlpha(hover, 255)));
}

auto AppTheme::EditorPanelToggleStyle(bool active, bool is_first, bool is_last) -> QString {
  const auto&  theme               = AppTheme::Instance();
  const QString top_left_radius    = is_first ? QStringLiteral("10px") : QStringLiteral("0px");
  const QString bottom_left_radius = is_first ? QStringLiteral("10px") : QStringLiteral("0px");
  const QString top_right_radius   = is_last ? QStringLiteral("10px") : QStringLiteral("0px");
  const QString bottom_right_radius = is_last ? QStringLiteral("10px") : QStringLiteral("0px");

  if (active) {
    return QStringLiteral("QPushButton {"
                          "  color: %1;"
                          "  background: %2;"
                          "  border: 1px solid %3;"
                          "  outline: none;"
                          "  padding: 0px;"
                          "  border-top-left-radius: %4;"
                          "  border-bottom-left-radius: %5;"
                          "  border-top-right-radius: %6;"
                          "  border-bottom-right-radius: %7;"
                          "}"
                          "QPushButton:hover {"
                          "  background: %8;"
                          "  outline: none;"
                          "}")
        .arg(Hex(theme.bgCanvasColor()), Rgba(WithAlpha(theme.accentColor(), 224)),
             Rgba(WithAlpha(theme.accentSecondaryColor(), 112)), top_left_radius,
             bottom_left_radius, top_right_radius, bottom_right_radius,
             Rgba(WithAlpha(theme.accentSecondaryColor(), 255)));
  }

  return QStringLiteral("QPushButton {"
                        "  color: %1;"
                        "  background: transparent;"
                        "  border: 1px solid transparent;"
                        "  outline: none;"
                        "  padding: 0px;"
                        "  border-top-left-radius: %2;"
                        "  border-bottom-left-radius: %3;"
                        "  border-top-right-radius: %4;"
                        "  border-bottom-right-radius: %5;"
                        "}"
                        "QPushButton:hover {"
                        "  background: %6;"
                        "  outline: none;"
                        "}"
                        "QPushButton:pressed {"
                        "  background: %7;"
                        "  outline: none;"
                        "}")
      .arg(Hex(theme.textColor()), top_left_radius, bottom_left_radius, top_right_radius,
           bottom_right_radius, Rgba(WithAlpha(theme.hoverColor(), 210)),
           Rgba(WithAlpha(theme.hoverColor(), 255)));
}

auto AppTheme::EditorMethodCardStyle(bool active) -> QString {
  const auto&  theme   = AppTheme::Instance();
  const QColor accent  = theme.accentColor();
  const QColor bg      = theme.bgBaseColor();
  const QColor hover   = theme.hoverColor();
  const QColor muted   = theme.textMutedColor();
  const QColor text    = theme.textColor();
  const QColor divider = theme.dividerColor();
  if (active) {
    return QStringLiteral("QPushButton {"
                          "  color: %1;"
                          "  background: %2;"
                          "  border: 1px solid %3;"
                          "  border-radius: 12px;"
                          "  padding: 16px 20px;"
                          "  text-align: left;"
                          "}"
                          "QPushButton:hover {"
                          "  background: %4;"
                          "}")
        .arg(Hex(accent), Rgba(WithAlpha(bg, 240)), Rgba(WithAlpha(accent, 136)),
             Rgba(WithAlpha(hover, 255)));
  }

  return QStringLiteral("QPushButton {"
                        "  color: %1;"
                        "  background: %2;"
                        "  border: 1px solid %3;"
                        "  border-radius: 12px;"
                        "  padding: 16px 20px;"
                        "  text-align: left;"
                        "}"
                        "QPushButton:hover {"
                        "  border: 1px solid %4;"
                        "  background: %5;"
                        "  color: %6;"
                        "}")
      .arg(Hex(muted), Rgba(WithAlpha(bg, 224)), Rgba(divider),
           Rgba(WithAlpha(theme.glassStrokeColor(), 176)), Rgba(WithAlpha(hover, 245)),
           Hex(text));
}

auto AppTheme::EditorComboBoxStyle() -> QString {
  const auto&  theme  = AppTheme::Instance();
  const QColor bg     = theme.bgBaseColor();
  const QColor text   = theme.textColor();
  const QColor accent = theme.accentColor();
  const QColor hover  = theme.hoverColor();
  const QColor border = theme.glassStrokeColor();
  return QStringLiteral("QComboBox {"
                        "  background: %1;"
                        "  color: %2;"
                        "  border: 1px solid %3;"
                        "  border-radius: 10px;"
                        "  padding: 4px 8px;"
                        "}"
                        "QComboBox:hover {"
                        "  border-color: %4;"
                        "}"
                        "QComboBox::drop-down {"
                        "  border: 0px;"
                        "  width: 24px;"
                        "}"
                        "QComboBox QAbstractItemView {"
                        "  background: %1;"
                        "  color: %2;"
                        "  border: 1px solid %3;"
                        "  selection-background-color: %5;"
                        "  selection-color: %6;"
                        "}"
                        "QComboBox QAbstractItemView::item:hover {"
                        "  background: %7;"
                        "  color: %2;"
                        "}"
                        "QComboBox QAbstractItemView::item:selected {"
                        "  background: %5;"
                        "  color: %6;"
                        "}")
      .arg(Rgba(WithAlpha(bg, 240)), Hex(text), Rgba(border), Rgba(WithAlpha(border, 196)),
           Hex(accent), Hex(theme.bgCanvasColor()), Rgba(WithAlpha(hover, 255)));
}

auto AppTheme::EditorSpinBoxStyle() -> QString {
  const auto&  theme  = AppTheme::Instance();
  const QColor bg     = theme.bgBaseColor();
  const QColor text   = theme.textColor();
  const QColor border = theme.glassStrokeColor();
  const QColor accent = theme.accentColor();
  return QStringLiteral("QSpinBox {"
                        "  background: %1;"
                        "  color: %2;"
                        "  border: 1px solid %3;"
                        "  border-radius: 8px;"
                        "  padding: 4px 8px;"
                        "}"
                        "QSpinBox:hover {"
                        "  border-color: %4;"
                        "}"
                        "QSpinBox:focus {"
                        "  border: 1px solid %5;"
                        "}"
                        "QSpinBox::up-button, QSpinBox::down-button {"
                        "  width: 0px;"
                        "}")
      .arg(Rgba(WithAlpha(bg, 240)), Hex(text), Rgba(border), Rgba(WithAlpha(border, 196)),
           Rgba(WithAlpha(accent, 224)));
}

auto AppTheme::EditorCheckBoxStyle() -> QString {
  const auto&  theme  = AppTheme::Instance();
  const QColor text   = theme.textColor();
  const QColor base   = theme.bgDeepColor();
  const QColor stroke = theme.glassStrokeColor();
  const QColor accent = theme.accentColor();
  return QStringLiteral("QCheckBox {"
                        "  color: %1;"
                        "  spacing: 8px;"
                        "}"
                        "QCheckBox::indicator {"
                        "  width: 16px;"
                        "  height: 16px;"
                        "}"
                        "QCheckBox::indicator:unchecked {"
                        "  background: %2;"
                        "  border: 1px solid %3;"
                        "  border-radius: 4px;"
                        "}"
                        "QCheckBox::indicator:checked {"
                        "  background: %4;"
                        "  border: 1px solid %4;"
                        "  border-radius: 4px;"
                        "}")
      .arg(Hex(text), Rgba(WithAlpha(base, 232)), Rgba(stroke), Hex(accent));
}

auto AppTheme::EditorScrollAreaStyle() -> QString {
  const auto& theme       = AppTheme::Instance();
  const QString bg_panel  = Hex(theme.bgPanelColor());
  const QString bg_canvas = Rgba(WithAlpha(theme.bgCanvasColor(), 170));
  const QString accent    = Hex(theme.accentColor());

  return QStringLiteral("QScrollArea { background: %1; border: none; border-radius: 12px; }"
                        "QScrollArea > QWidget, QScrollArea > QWidget > QWidget {"
                        "  background: %1;"
                        "  border-radius: 12px;"
                        "}"
                        "QScrollBar:vertical {"
                        "  background: %2;"
                        "  width: 10px;"
                        "  margin: 2px;"
                        "  border-radius: 5px;"
                        "}"
                        "QScrollBar::handle:vertical {"
                        "  background: %3;"
                        "  border-radius: 5px;"
                        "}"
                        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
                        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
      .arg(bg_panel, bg_canvas, accent);
}

auto AppTheme::EditorListWidgetStyle() -> QString {
  const auto&  theme  = AppTheme::Instance();
  const QColor bg     = theme.bgDeepColor();
  const QColor border = theme.dividerColor();
  return QStringLiteral("QListWidget {"
                        "  background: %1;"
                        "  border: 1px solid %2;"
                        "  border-radius: 10px;"
                        "  padding: 8px;"
                        "}"
                        "QListWidget::item {"
                        "  padding: 2px;"
                        "}"
                        "QListWidget::item:selected {"
                        "  background: transparent;"
                        "}"
                        "QScrollBar:vertical {"
                        "  background: transparent;"
                        "  width: 6px;"
                        "  margin: 4px 2px 4px 0;"
                        "}"
                        "QScrollBar::handle:vertical {"
                        "  background: %3;"
                        "  border-radius: 3px;"
                        "  min-height: 24px;"
                        "}"
                        "QScrollBar::handle:vertical:hover,"
                        "QScrollBar::handle:vertical:pressed {"
                        "  background: %4;"
                        "}"
                        "QScrollBar::add-line:vertical,"
                        "QScrollBar::sub-line:vertical {"
                        "  height: 0px;"
                        "}"
                        "QScrollBar::add-page:vertical,"
                        "QScrollBar::sub-page:vertical {"
                        "  background: transparent;"
                        "}")
      .arg(Rgba(WithAlpha(bg, 216)), Rgba(border), Rgba(WithAlpha(theme.textMutedColor(), 132)),
           Hex(theme.accentColor()));
}

auto AppTheme::EditorHistoryCardStyle() -> QString {
  const auto& theme = AppTheme::Instance();
  return QStringLiteral("QFrame#HistoryCard {"
                        "  background: transparent;"
                        "  border: none;"
                        "  border-radius: 8px;"
                        "}"
                        "QFrame#HistoryCard:hover {"
                        "  background: %1;"
                        "}"
                        "QFrame#HistoryCard[selected=\"true\"] {"
                        "  background: %2;"
                        "}"
                        "QFrame#HistoryCard[selected=\"true\"]:hover {"
                        "  background: %2;"
                        "}"
                        "QFrame#HistoryCard[future=\"true\"] {"
                        "  background: transparent;"
                        "}"
                        "QFrame#HistoryCard QFrame#HistoryTxIconTile {"
                        "  background: %3;"
                        "  border: none;"
                        "  border-radius: 7px;"
                        "}"
                        "QFrame#HistoryCard QLabel#HistoryTxTitle {"
                        "  color: %4;"
                        "  background: transparent;"
                        "}"
                        "QFrame#HistoryCard QLabel#HistoryTxSubtitle {"
                        "  color: %5;"
                        "  background: transparent;"
                        "}"
                        "QFrame#HistoryCard[future=\"true\"] QLabel#HistoryTxTitle,"
                        "QFrame#HistoryCard[future=\"true\"] QLabel#HistoryTxSubtitle {"
                        "  color: %6;"
                        "}")
      .arg(Rgba(WithAlpha(theme.bgPanelColor(), 132)),
           Rgba(WithAlpha(theme.bgPanelColor(), 118)),
           Rgba(WithAlpha(theme.bgDeepColor(), 228)),
           Hex(theme.textColor()),
           Hex(theme.textMutedColor()),
           Rgba(WithAlpha(theme.textMutedColor(), 132)));
}

auto AppTheme::EditorTransparentFrameStyle() -> QString {
  return QStringLiteral("QFrame {"
                        "  background: transparent;"
                        "  border: none;"
                        "  border-radius: 12px;"
                        "}");
}

auto AppTheme::EditorSliderTrackColor() -> QColor {
  const auto& theme = AppTheme::Instance();
  return Blend(theme.bgDeepColor(), theme.bgBaseColor(), 0.35);
}

auto AppTheme::EditorSliderAccentColor(bool positive) -> QColor {
  const auto& theme = AppTheme::Instance();
  if (positive) {
    return Blend(theme.accentColor(), QColor(0xD8, 0xEC, 0xFF), 0.42);
  }
  return Blend(theme.dangerColor(), QColor(0xFF, 0xE3, 0xE0), 0.72);
}

auto AppTheme::EditorSliderBorderColor(bool positive) -> QColor {
  const auto accent = EditorSliderAccentColor(positive);
  return WithAlpha(accent, positive ? 112 : 104);
}

auto AppTheme::EditorSliderHandleColor() -> QColor { return QColor(0xF1, 0xEE, 0xEA); }

auto AppTheme::EditorSliderHandleBorderColor() -> QColor {
  return WithAlpha(AppTheme::Instance().bgCanvasColor(), 228);
}

auto AppTheme::uiFontFamily() const -> QString {
  RegisterFonts();
  return ActiveUiFamily(FontState());
}
auto AppTheme::headlineFontFamily() const -> QString {
  RegisterFonts();
  return FontState().ui_headline;
}
auto AppTheme::dataFontFamily() const -> QString {
  RegisterFonts();
  return FontState().data;
}
auto AppTheme::monoFontFamily() const -> QString {
  RegisterFonts();
  return FontState().mono;
}
auto AppTheme::appVersion() const -> QString {
  // Fed by the ALCEDO_APP_VERSION compile definition (set from PROJECT_VERSION
  // in alcedo_studio/src/CMakeLists.txt). Fallback keeps the symbol linkable
  // for any consumer built without that define.
#ifdef ALCEDO_APP_VERSION
  return QStringLiteral(ALCEDO_APP_VERSION);
#else
  return QStringLiteral("0.0.0");
#endif
}

auto AppTheme::toneGold() const -> QColor { return GetTheme(current_theme_index_).tone_gold; }
auto AppTheme::toneWine() const -> QColor { return GetTheme(current_theme_index_).tone_wine; }
auto AppTheme::toneSteel() const -> QColor { return GetTheme(current_theme_index_).tone_steel; }
auto AppTheme::toneGraphite() const -> QColor { return GetTheme(current_theme_index_).tone_graphite; }
auto AppTheme::toneMist() const -> QColor { return GetTheme(current_theme_index_).tone_mist; }

auto AppTheme::bgCanvasColor() const -> QColor { return GetTheme(current_theme_index_).bg_canvas; }
auto AppTheme::bgDeepColor() const -> QColor { return GetTheme(current_theme_index_).bg_deep; }
auto AppTheme::bgBaseColor() const -> QColor { return GetTheme(current_theme_index_).bg_base; }
auto AppTheme::bgPanelColor() const -> QColor { return GetTheme(current_theme_index_).bg_panel; }
auto AppTheme::textColor() const -> QColor { return GetTheme(current_theme_index_).text; }
auto AppTheme::textMutedColor() const -> QColor { return GetTheme(current_theme_index_).text_muted; }
auto AppTheme::iconColor() const -> QColor { return GetTheme(current_theme_index_).icon; }
auto AppTheme::accentColor() const -> QColor { return toneGold(); }
auto AppTheme::accentSecondaryColor() const -> QColor { return GetTheme(current_theme_index_).accent_secondary; }
auto AppTheme::dangerColor() const -> QColor { return toneWine(); }
auto AppTheme::dangerTintColor() const -> QColor { return GetTheme(current_theme_index_).danger_tint; }
auto AppTheme::selectedTintColor() const -> QColor { return GetTheme(current_theme_index_).selected_tint; }
auto AppTheme::hoverColor() const -> QColor { return GetTheme(current_theme_index_).hover; }
auto AppTheme::dividerColor() const -> QColor { return GetTheme(current_theme_index_).divider; }
auto AppTheme::glassPanelColor() const -> QColor { return GetTheme(current_theme_index_).glass_panel; }
auto AppTheme::glassStrokeColor() const -> QColor { return GetTheme(current_theme_index_).glass_stroke; }
auto AppTheme::overlayColor() const -> QColor { return GetTheme(current_theme_index_).overlay; }
auto AppTheme::panelRadius() const -> int { return GetTheme(current_theme_index_).panel_radius; }

// ── Design tokens — structural (theme-invariant) ───────────────────────────
// Values are the canonical VI contract documented in DESIGN.md. Literal
// getters keep a single source of truth shared by QML and (future) QWidget
// consumers; do not duplicate these numbers in feature QML.
auto AppTheme::controlRadius() const -> int { return 10; }
auto AppTheme::controlRadiusSmall() const -> int { return 8; }
auto AppTheme::badgeRadius() const -> int { return 6; }
auto AppTheme::iconOpticalSize() const -> int { return 22; }
auto AppTheme::iconOpticalSizeCompact() const -> int { return 18; }
auto AppTheme::iconSourceSize() const -> int { return 24; }
auto AppTheme::iconSourceSizeCompact() const -> int { return 20; }
auto AppTheme::iconButtonHitSize() const -> int { return 44; }
auto AppTheme::iconButtonHitSizeCompact() const -> int { return 40; }
// Editor side-panel + scope sizing (DESIGN.md "Editor panel geometry"). Values
// are logical px; Qt scales by DPR so they stay comfortable at 1.25/1.5/2.0.
auto AppTheme::editorSidePanelWidth() const -> int { return 320; }
auto AppTheme::editorSidePanelWidthMin() const -> int { return 260; }
auto AppTheme::editorSidePanelWidthMax() const -> int { return 460; }
auto AppTheme::editorScopeHeight() const -> int { return 160; }
auto AppTheme::editorScopeHeightMin() const -> int { return 128; }
auto AppTheme::lineHeightCaption() const -> int { return 14; }
auto AppTheme::lineHeightBody() const -> int { return 16; }
auto AppTheme::lineHeightTitle() const -> int { return 18; }
auto AppTheme::lineHeightSection() const -> int { return 20; }
auto AppTheme::lineHeightHeadline() const -> int { return 28; }
auto AppTheme::spaceXs() const -> int { return 4; }
auto AppTheme::spaceSm() const -> int { return 8; }
auto AppTheme::spaceMd() const -> int { return 12; }
auto AppTheme::spaceLg() const -> int { return 16; }
auto AppTheme::spaceXl() const -> int { return 20; }
auto AppTheme::motionFoldOpenMs() const -> int { return 200; }
auto AppTheme::motionFoldCloseMs() const -> int { return 160; }
auto AppTheme::motionFadeMs() const -> int { return 120; }

auto AppTheme::reduceMotion() const -> bool {
  if (!reduce_motion_loaded_) {
    QSettings settings;
    reduce_motion_ = settings.value(QStringLiteral("ui/reduceMotion"), false).toBool();
    reduce_motion_loaded_ = true;
  }
  return reduce_motion_;
}

void AppTheme::setReduceMotion(bool enabled) {
  if (reduce_motion_loaded_ && reduce_motion_ == enabled) {
    return;
  }
  reduce_motion_ = enabled;
  reduce_motion_loaded_ = true;
  QSettings settings;
  settings.setValue(QStringLiteral("ui/reduceMotion"), enabled);
  settings.sync();
  emit ReduceMotionChanged();
}

auto AppTheme::fontSizeCaption() const -> int { return 11; }
auto AppTheme::fontSizeBody() const -> int { return 12; }
auto AppTheme::fontSizeTitle() const -> int { return 13; }
auto AppTheme::fontSizeSection() const -> int { return 14; }
auto AppTheme::fontSizeHeadline() const -> int { return 22; }
auto AppTheme::fontWeightRegular() const -> int { return 500; }
auto AppTheme::fontWeightStrong() const -> int { return 600; }
auto AppTheme::fontWeightHeading() const -> int { return 700; }

auto AppTheme::cardSurfaceColor() const -> QColor { return bgPanelColor(); }
auto AppTheme::cardBorderColor() const -> QColor { return dividerColor(); }
auto AppTheme::buttonIdleFillColor() const -> QColor { return GetTheme(current_theme_index_).button_idle_fill; }
auto AppTheme::buttonHoveredFillColor() const -> QColor { return GetTheme(current_theme_index_).button_hovered_fill; }
auto AppTheme::buttonPressedFillColor() const -> QColor { return GetTheme(current_theme_index_).button_pressed_fill; }
auto AppTheme::buttonSelectedFillColor() const -> QColor { return GetTheme(current_theme_index_).button_selected_fill; }
auto AppTheme::disabledSurfaceColor() const -> QColor { return GetTheme(current_theme_index_).disabled_surface; }

auto AppTheme::currentThemeIndex() const -> int { return current_theme_index_; }

void AppTheme::setCurrentThemeIndex(int index) {
  if (index < 0 || index > 1) {
    return;
  }
  if (current_theme_index_ == index) {
    return;
  }
  current_theme_index_ = index;
  emit ThemeChanged();
}

auto AppTheme::availableThemes() const -> QVariantList {
  return QVariantList{
      QVariantMap{{QStringLiteral("label"), tr("Alcedo")}, {QStringLiteral("index"), 0}},
      QVariantMap{{QStringLiteral("label"), tr("Classic")}, {QStringLiteral("index"), 1}},
  };
}

auto AppTheme::ResolveRole(QWidget* widget) -> FontRole {
  if (!widget) {
    return FontRole::UiBody;
  }

  const QVariant tagged_role = widget->property(kThemeFontRoleProperty);
  if (tagged_role.isValid()) {
    return static_cast<FontRole>(tagged_role.toInt());
  }

  const QString object_name = widget->objectName();
  if (object_name == QLatin1String("EditorSectionTitle")) {
    return FontRole::UiTitle;
  }
  if (object_name == QLatin1String("EditorSectionSub")) {
    return FontRole::UiCaption;
  }

  if (qobject_cast<QPushButton*>(widget)) {
    return FontRole::UiBodyStrong;
  }
  if (qobject_cast<QCheckBox*>(widget) || qobject_cast<QComboBox*>(widget) ||
      qobject_cast<QSpinBox*>(widget) || qobject_cast<QListWidget*>(widget)) {
    return FontRole::UiBody;
  }

  return FontRole::UiBody;
}

}  // namespace alcedo::ui
