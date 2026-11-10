//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_view_preferences.hpp"

#include <QLatin1String>
#include <QSettings>
#include <array>

#include "image/color_label.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

// Source names of the codes in code order (alcedo::ColorLabel); translated in the context of
// LibraryViewPreferences.
constexpr std::array<const char*, kColorLabelCount> kColorLabelSourceNames = {
    QT_TRANSLATE_NOOP("alcedo::ui::LibraryViewPreferences", "Red"),
    QT_TRANSLATE_NOOP("alcedo::ui::LibraryViewPreferences", "Orange"),
    QT_TRANSLATE_NOOP("alcedo::ui::LibraryViewPreferences", "Yellow"),
    QT_TRANSLATE_NOOP("alcedo::ui::LibraryViewPreferences", "Green"),
    QT_TRANSLATE_NOOP("alcedo::ui::LibraryViewPreferences", "Blue"),
    QT_TRANSLATE_NOOP("alcedo::ui::LibraryViewPreferences", "Purple"),
    QT_TRANSLATE_NOOP("alcedo::ui::LibraryViewPreferences", "Gray"),
};

/// True for a character that a name keeps as a space: a line break or a tab.
auto IsLineOrTabControl(char32_t code_point) -> bool {
  return code_point == U'\t' || code_point == U'\n' || code_point == U'\v' || code_point == U'\f' ||
         code_point == U'\r' || code_point == 0x85 || code_point == 0x2028 || code_point == 0x2029;
}

/// True for a character that a name drops: the other control characters (category Cc) and the
/// bidirectional embeddings, overrides, and isolates, which reorder the text after them.
auto IsDroppedControl(char32_t code_point) -> bool {
  return code_point < 0x20 || (code_point >= 0x7F && code_point <= 0x9F) ||
         (code_point >= 0x202A && code_point <= 0x202E) ||
         (code_point >= 0x2066 && code_point <= 0x2069);
}

/// @p name without control characters and lone surrogates; line breaks and tabs become spaces.
auto WithoutControlCharacters(const QString& name) -> QString {
  QString result;
  result.reserve(name.size());
  for (qsizetype index = 0; index < name.size(); ++index) {
    const QChar unit = name.at(index);
    if (unit.isHighSurrogate() && index + 1 < name.size() && name.at(index + 1).isLowSurrogate()) {
      result.append(unit);
      result.append(name.at(index + 1));
      ++index;
      continue;
    }
    if (unit.isSurrogate()) {
      continue;  // A lone half of a pair is not text.
    }
    const char32_t code_point = unit.unicode();
    if (IsLineOrTabControl(code_point)) {
      result.append(QLatin1Char(' '));
    } else if (!IsDroppedControl(code_point)) {
      result.append(unit);
    }
  }
  return result;
}

/// The first @p count characters of @p name; a surrogate pair counts as one character and stays
/// whole. @p name holds no lone surrogates.
auto FirstCharacters(const QString& name, int count) -> QString {
  qsizetype end = 0;
  for (int taken = 0; taken < count && end < name.size(); ++taken) {
    end += name.at(end).isHighSurrogate() ? 2 : 1;
  }
  return name.left(end);
}

}  // namespace

auto LibraryViewPreferences::EditableColorLabelNameText(const QString& text) -> QString {
  return FirstCharacters(WithoutControlCharacters(text), kColorLabelNameMaxLength);
}

auto LibraryViewPreferences::NormalizedColorLabelName(const QString& name) -> QString {
  return FirstCharacters(WithoutControlCharacters(name).trimmed(), kColorLabelNameMaxLength)
      .trimmed();
}

auto LibraryViewPreferences::EditableColorLabelName(const QString& text) const -> QString {
  return EditableColorLabelNameText(text);
}

LibraryViewPreferences::LibraryViewPreferences(QObject* parent) : QObject(parent) {
  QSettings     settings;
  const QString stored =
      settings.value(QLatin1String(kGridLayoutSettingKey), QLatin1String(kClassicLayout))
          .toString();
  grid_layout_ = IsGridLayout(stored) ? stored : QString::fromLatin1(kClassicLayout);

  color_label_user_names_.reserve(kColorLabelCount);
  for (int code = 0; code < kColorLabelCount; ++code) {
    color_label_user_names_.push_back(
        NormalizedColorLabelName(settings.value(ColorLabelNameSettingKey(code)).toString()));
  }

  // The default names are translated; a language change changes them.
  QObject::connect(&i18n::TranslationNotifier::Instance(),
                   &i18n::TranslationNotifier::LanguageChanged, this,
                   &LibraryViewPreferences::colorLabelNamesChanged);
}

auto LibraryViewPreferences::IsGridLayout(const QString& layout) -> bool {
  return layout == QLatin1String(kClassicLayout) || layout == QLatin1String(kJustifiedLayout);
}

bool LibraryViewPreferences::setGridLayout(const QString& layout) {
  if (!IsGridLayout(layout)) {
    return false;
  }
  if (layout == grid_layout_) {
    return true;
  }
  grid_layout_ = layout;
  QSettings{}.setValue(QLatin1String(kGridLayoutSettingKey), grid_layout_);
  emit gridLayoutChanged();
  return true;
}

auto LibraryViewPreferences::ColorLabelNameSettingKey(int code) -> QString {
  return QStringLiteral("library/colorLabels/%1/name").arg(code);
}

auto LibraryViewPreferences::DefaultColorLabelName(int code) -> QString {
  if (!ColorLabelFromIndex(code).has_value()) {
    return {};
  }
  return QCoreApplication::translate("alcedo::ui::LibraryViewPreferences",
                                     kColorLabelSourceNames[static_cast<std::size_t>(code)]);
}

auto LibraryViewPreferences::color_label_default_names() const -> QStringList {
  QStringList names;
  names.reserve(kColorLabelCount);
  for (int code = 0; code < kColorLabelCount; ++code) {
    names.push_back(DefaultColorLabelName(code));
  }
  return names;
}

auto LibraryViewPreferences::color_label_names() const -> QStringList {
  QStringList names = color_label_default_names();
  for (int code = 0; code < kColorLabelCount; ++code) {
    const QString& user_name = color_label_user_names_.at(code);
    if (!user_name.isEmpty()) {
      names[code] = user_name;
    }
  }
  return names;
}

bool LibraryViewPreferences::SetColorLabelName(int code, const QString& name) {
  if (!ColorLabelFromIndex(code).has_value()) {
    return false;
  }
  const QString normalized = NormalizedColorLabelName(name);
  if (normalized == color_label_user_names_.at(code)) {
    return true;
  }
  color_label_user_names_[code] = normalized;
  QSettings settings;
  if (normalized.isEmpty()) {
    settings.remove(ColorLabelNameSettingKey(code));
  } else {
    settings.setValue(ColorLabelNameSettingKey(code), normalized);
  }
  emit colorLabelNamesChanged();
  return true;
}

}  // namespace alcedo::ui
