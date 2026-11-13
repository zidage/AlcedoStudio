//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace alcedo::ui {

/**
 * @brief User-level Library view settings.
 *
 * Owns two settings of the Settings "Language / Theme" page, Library section:
 *
 * - The layout of the ungrouped Library grid: "classic" (default) puts each photo in a 3:2 card
 *   of a fixed column count; "justified" puts the photos in rows of one height that fill the
 *   view width. Grouped photos always use the classic cells. Stored in QSettings under
 *   `library/gridLayout`; unknown stored values read as "classic".
 * - The user names of the seven color label codes (#768, #770), shared by every project on the
 *   computer. Stored in QSettings under `library/colorLabels/<code>/name`; a missing or empty
 *   name shows the translated color name ("Red").
 *
 * Threading: GUI thread only. Side effects: QSettings writes, gridLayoutChanged and
 * colorLabelNamesChanged on an accepted change; colorLabelNamesChanged also follows a change of
 * the application language.
 */
class LibraryViewPreferences final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString gridLayout READ grid_layout WRITE setGridLayout NOTIFY gridLayoutChanged)
  /// Display name of each code in code order: the user name, or the translated color name.
  Q_PROPERTY(QStringList colorLabelNames READ color_label_names NOTIFY colorLabelNamesChanged)
  /// User name of each code in code order; an empty entry is a code without a user name.
  Q_PROPERTY(
      QStringList colorLabelUserNames READ color_label_user_names NOTIFY colorLabelNamesChanged)
  /// Translated color name of each code in code order ("Red" ... "Gray").
  Q_PROPERTY(QStringList colorLabelDefaultNames READ color_label_default_names NOTIFY
                 colorLabelNamesChanged)

 public:
  static constexpr auto kGridLayoutSettingKey    = "library/gridLayout";
  static constexpr auto kClassicLayout           = "classic";
  static constexpr auto kJustifiedLayout         = "justified";
  /// Longest user name in characters (a surrogate pair is one character); longer names are cut.
  static constexpr int  kColorLabelNameMaxLength = 40;

  explicit LibraryViewPreferences(QObject* parent = nullptr);

  [[nodiscard]] auto        grid_layout() const -> QString { return grid_layout_; }
  /**
   * @brief Persist @p layout as the layout of the ungrouped Library grid.
   * @return false for values other than classic/justified; nothing changes.
   */
  Q_INVOKABLE bool          setGridLayout(const QString& layout);

  [[nodiscard]] static auto IsGridLayout(const QString& layout) -> bool;

  [[nodiscard]] auto        color_label_names() const -> QStringList;
  [[nodiscard]] auto        color_label_user_names() const -> QStringList {
    return color_label_user_names_;
  }
  [[nodiscard]] auto        color_label_default_names() const -> QStringList;

  /**
   * @brief Persist @p name as the user name of color code @p code (0 Red ... 6 Gray).
   *
   * The name is NormalizedColorLabelName(@p name). An empty name removes the user name, so the
   * code shows its translated color name again.
   * @return false for a code outside 0 to 6; nothing changes.
   */
  Q_INVOKABLE bool          SetColorLabelName(int code, const QString& name);

  /**
   * @brief The text a name field keeps while the user types @p text.
   *
   * Line breaks and tabs become spaces; the other control characters (category Cc), the
   * bidirectional embeddings, overrides, and isolates (U+202A to U+202E, U+2066 to U+2069), and
   * lone surrogates are removed. The text is cut to kColorLabelNameMaxLength characters, and a
   * surrogate pair counts as one character and stays whole. White space stays, so the user can
   * type a space between words.
   */
  [[nodiscard]] static auto EditableColorLabelNameText(const QString& text) -> QString;
  /// EditableColorLabelNameText for QML (the Settings name field).
  Q_INVOKABLE QString       EditableColorLabelName(const QString& text) const;
  /**
   * @brief The user name that @p name stores: EditableColorLabelNameText without leading and
   * trailing white space, cut after the leading white space is removed.
   *
   * The command answer, the stored name, and the name after a restart are this same text.
   */
  [[nodiscard]] static auto NormalizedColorLabelName(const QString& name) -> QString;

  /// QSettings key of the user name of @p code.
  [[nodiscard]] static auto ColorLabelNameSettingKey(int code) -> QString;
  /// Translated color name of @p code, or an empty string for a code outside 0 to 6.
  [[nodiscard]] static auto DefaultColorLabelName(int code) -> QString;

 signals:
  void gridLayoutChanged();
  void colorLabelNamesChanged();

 private:
  QString     grid_layout_;
  QStringList color_label_user_names_;
};

}  // namespace alcedo::ui
