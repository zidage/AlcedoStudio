//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/lut_library_service.hpp"
#include "ui/alcedo_main/album_backend/lut_library_query.hpp"
#include "utils/lut/lut_metadata.hpp"

namespace alcedo::ui {

/// Why the grade stage cannot apply a LUT with @p header: a 1D shaper with a 3D table, or a
/// 1D table only. Empty when LutHeader::SupportsGradeApplication() is true.
[[nodiscard]] auto UnsupportedLutText(const LutHeader& header) -> QString;

/**
 * @brief Browser list of the LUT library: filter, search, order, favorites, and focus.
 *
 * Ownership: LutLibraryService owns the entries. This model owns only row indices, the query
 * and filter state, the focused browser entry, and derived search keys (LutSearchKeys) that it
 * rebuilds for entries the service reports as changed. Row values are read from the service
 * through a scoped entry read when a view requests them, so a view instantiates delegates only
 * for its visible rows (plan 6.3).
 *
 * Rows: entries passing every filter predicate (LutKeysPassFilter) and the query
 * (RankLutSearchKeys). With a query, rows follow the search rank; without one, the chosen
 * order. The entry ID breaks every tie.
 *
 * Changes: query, filter, order, and inventory changes reset the model. Focus, the applied
 * entry, and favorite changes emit dataChanged for the affected roles only; with the favorites
 * filter on, an unfavorited row is removed with beginRemoveRows. None of these rebuild the
 * library or move other rows.
 *
 * Browsing never changes the document: applying an entry belongs to LutLibraryController.
 * Thread affinity: GUI thread, the owner thread of the service.
 */
class LutLibraryModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(alcedo::LutLibraryService* library READ library WRITE setLibrary NOTIFY libraryChanged)
  /// Search text as typed; the rows follow it 100 ms after the last change (applyQueryNow).
  Q_PROPERTY(QString queryText READ queryText WRITE setQueryText NOTIFY queryTextChanged)
  /// `all`, `general` (labelled User), or `film_simulation`. Choosing `general` clears brand
  /// and print.
  Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY filterChanged)
  /// Source ID, or empty for All.
  Q_PROPERTY(QString source READ source WRITE setSource NOTIFY filterChanged)
  /// Normalized film brand key (a `value` of brandChoices), or empty for All.
  Q_PROPERTY(QString brand READ brand WRITE setBrand NOTIFY filterChanged)
  /// Normalized print key (a `value` of printChoices), kLutNoPrintKey for film simulations
  /// without a print, or empty for All.
  Q_PROPERTY(QString print READ print WRITE setPrint NOTIFY filterChanged)
  Q_PROPERTY(bool favoritesOnly READ favoritesOnly WRITE setFavoritesOnly NOTIFY filterChanged)
  /// `name` or `modified`; used when the query is empty.
  Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY orderChanged)
  Q_PROPERTY(bool sortAscending READ sortAscending WRITE setSortAscending NOTIFY orderChanged)
  /// Filter choices `{value, label, count, selected}`: All, then every value the library
  /// declares. Counts apply every other predicate and the query; a value can have a zero count.
  Q_PROPERTY(QVariantList categoryChoices READ categoryChoices NOTIFY choicesChanged)
  Q_PROPERTY(QVariantList sourceChoices READ sourceChoices NOTIFY choicesChanged)
  Q_PROPERTY(QVariantList brandChoices READ brandChoices NOTIFY choicesChanged)
  Q_PROPERTY(QVariantList printChoices READ printChoices NOTIFY choicesChanged)
  /// `all` and `favorites`; `favorites` is selected while favoritesOnly is on.
  Q_PROPERTY(QVariantList favoriteChoices READ favoriteChoices NOTIFY choicesChanged)
  /// Brand and print choices apply only outside the General category.
  Q_PROPERTY(bool filmFiltersAvailable READ filmFiltersAvailable NOTIFY choicesChanged)
  /// True when the library declares a print, so the print choices (each print, and No print
  /// for film simulations without one) can narrow the results.
  Q_PROPERTY(bool printFilterAvailable READ printFilterAvailable NOTIFY choicesChanged)
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged)
  Q_PROPERTY(QString focusedEntryId READ focusedEntryId WRITE focusEntry NOTIFY focusChanged)
  Q_PROPERTY(int focusedRow READ focusedRow NOTIFY focusChanged)
  /// Entry the current target applies (load-only; set from LutLibraryController).
  Q_PROPERTY(QString appliedEntryId READ appliedEntryId WRITE setAppliedEntryId NOTIFY
                 appliedEntryIdChanged)

 public:
  enum Role {
    EntryIdRole = Qt::UserRole + 1,
    RelativePathRole,
    DisplayNameRole,
    PrintNameRole,
    SourceNameRole,
    FilmBrandRole,
    CategoryRole,
    HasPrintRole,
    OfficialRole,
    PackageIdRole,
    FavoriteRole,
    FocusedRole,
    AppliedRole,
    SelectableRole,
    ValidRole,
    StatusTextRole,
    DetailTextRole,
    LutSizeTextRole,
    FileSizeRole,
  };
  Q_ENUM(Role)

  explicit LutLibraryModel(QObject* parent = nullptr);
  ~LutLibraryModel() override;

  [[nodiscard]] auto rowCount(const QModelIndex& parent = QModelIndex()) const -> int override;
  [[nodiscard]] auto data(const QModelIndex& index, int role = Qt::DisplayRole) const
      -> QVariant override;
  [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;

  [[nodiscard]] auto library() const -> alcedo::LutLibraryService* { return library_; }
  /// Bind the library; rebuilds the search keys and follows inventory, root, and favorites.
  void               setLibrary(alcedo::LutLibraryService* library);

  [[nodiscard]] auto queryText() const -> QString { return query_text_; }
  /// Record the typed text and apply it after the 100 ms typing delay.
  void               setQueryText(const QString& text);
  /// Apply the typed text now (Enter key and tests).
  Q_INVOKABLE void   applyQueryNow();

  [[nodiscard]] auto category() const -> QString;
  void               setCategory(const QString& category);
  [[nodiscard]] auto source() const -> QString { return filter_.source_id; }
  void               setSource(const QString& source_id);
  [[nodiscard]] auto brand() const -> QString { return filter_.brand_key; }
  void               setBrand(const QString& brand_key);
  [[nodiscard]] auto print() const -> QString { return filter_.print_key; }
  void               setPrint(const QString& print_key);
  [[nodiscard]] auto favoritesOnly() const -> bool { return filter_.favorites_only; }
  void               setFavoritesOnly(bool favorites_only);
  /// Remove every filter predicate and the query.
  Q_INVOKABLE void   clearFilters();

  [[nodiscard]] auto sortKey() const -> QString;
  void               setSortKey(const QString& key);
  [[nodiscard]] auto sortAscending() const -> bool { return sort_ascending_; }
  void               setSortAscending(bool ascending);

  [[nodiscard]] auto categoryChoices() const -> QVariantList { return category_choices_; }
  [[nodiscard]] auto sourceChoices() const -> QVariantList { return source_choices_; }
  [[nodiscard]] auto brandChoices() const -> QVariantList { return brand_choices_; }
  [[nodiscard]] auto printChoices() const -> QVariantList { return print_choices_; }
  [[nodiscard]] auto favoriteChoices() const -> QVariantList { return favorite_choices_; }
  [[nodiscard]] auto filmFiltersAvailable() const -> bool {
    return filter_.category != LutCategoryFilter::kGeneral;
  }
  [[nodiscard]] auto printFilterAvailable() const -> bool { return print_filter_available_; }
  [[nodiscard]] auto count() const -> int { return static_cast<int>(rows_.size()); }
  [[nodiscard]] auto totalCount() const -> int { return static_cast<int>(keys_.size()); }

  [[nodiscard]] auto focusedEntryId() const -> QString { return focused_entry_id_; }
  [[nodiscard]] auto focusedRow() const -> int { return rowOfEntry(focused_entry_id_); }
  /// Focus a browser entry; only the focused role of the old and new rows changes.
  Q_INVOKABLE void   focusEntry(const QString& entry_id);
  /// Move focus to the next selectable row in the direction of @p step (positive: down),
  /// wrapping around. Returns false when no other selectable row exists.
  Q_INVOKABLE bool   focusRelative(int step);
  [[nodiscard]] auto appliedEntryId() const -> QString { return applied_entry_id_; }
  void               setAppliedEntryId(const QString& entry_id);

  [[nodiscard]] Q_INVOKABLE int     rowOfEntry(const QString& entry_id) const;
  [[nodiscard]] Q_INVOKABLE QString entryIdAt(int row) const;
  [[nodiscard]] Q_INVOKABLE bool    isFavorite(const QString& entry_id) const;
  /// Toggle a favorite through the library. Returns false and emits favoriteFailed when the
  /// library rejects the change.
  Q_INVOKABLE bool                  toggleFavorite(const QString& entry_id);

 signals:
  void libraryChanged();
  void queryTextChanged();
  void filterChanged();
  void orderChanged();
  void choicesChanged();
  void countChanged();
  void focusChanged();
  void appliedEntryIdChanged();
  void favoriteFailed(const QString& message);

 private:
  /// Rebuild search keys; entries outside @p changed_paths reuse their keys unless @p all.
  void               rebuildKeys(const QStringList& changed_paths, bool all);
  /// Rank every entry for the applied query.
  void               rerank();
  /// Recompute rows and choices from the ranks, then reset the model.
  void               refilter();
  /// Recompute rows after a favorite change; removes unfavorited rows without a reset.
  void               applyFavoriteChange();
  [[nodiscard]] auto computeRows() const -> std::vector<int>;
  void               rebuildChoices();
  [[nodiscard]] auto keysFavorite(const LutSearchKeys& keys) const -> bool;
  void               emitRoleChanged(int row, int role);

  QPointer<alcedo::LutLibraryService> library_;
  /// Search keys in library path order.
  std::vector<LutSearchKeys>          keys_;
  /// Search rank of each keys_ entry for the applied query; std::nullopt when it does not match.
  std::vector<std::optional<LutSearchRank>> ranks_;
  /// Visible rows: indices into keys_.
  std::vector<int>                    rows_;
  /// Entry ID to keys_ index.
  std::unordered_map<std::string, int> key_index_;
  QString                              query_text_;
  LutSearchQuery                       query_;
  QTimer                               query_timer_;
  LutFacetFilter                       filter_;
  bool                                 sort_by_modified_ = false;
  bool                                 sort_ascending_   = true;
  QVariantList                         category_choices_;
  QVariantList                         source_choices_;
  QVariantList                         brand_choices_;
  QVariantList                         print_choices_;
  QVariantList                         favorite_choices_;
  bool                                 print_filter_available_ = false;
  QString                              focused_entry_id_;
  QString                              applied_entry_id_;
};

}  // namespace alcedo::ui
