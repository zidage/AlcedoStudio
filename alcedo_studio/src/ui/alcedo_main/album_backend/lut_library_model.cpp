//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/lut_library_model.hpp"

#include <QVariantMap>
#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include "app/lut_library_publication.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

/// Typing delay before a query applies (plan 6.3).
constexpr int kQueryDelayMs = 100;

auto ToQString(const std::string& text) -> QString { return QString::fromStdString(text); }

auto ToUtf8(const QString& text) -> std::string {
  const QByteArray bytes = text.toUtf8();
  return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

auto CategoryText(LutCategoryFilter category) -> QString {
  switch (category) {
    case LutCategoryFilter::kGeneral:
      return QStringLiteral("general");
    case LutCategoryFilter::kFilmSimulation:
      return QStringLiteral("film_simulation");
    case LutCategoryFilter::kAll:
      break;
  }
  return QStringLiteral("all");
}

auto PrintText(LutPrintFilter print) -> QString {
  switch (print) {
    case LutPrintFilter::kWithPrint:
      return QStringLiteral("with_print");
    case LutPrintFilter::kNoPrint:
      return QStringLiteral("no_print");
    case LutPrintFilter::kAll:
      break;
  }
  return QStringLiteral("all");
}

auto Choice(const QString& value, const QString& label, int count, bool selected) -> QVariant {
  QVariantMap map;
  map.insert(QStringLiteral("value"), value);
  map.insert(QStringLiteral("label"), label);
  map.insert(QStringLiteral("count"), count);
  map.insert(QStringLiteral("selected"), selected);
  return map;
}

auto FormatByteSize(std::uint64_t bytes) -> QString {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = 1024.0 * 1024.0;
  if (static_cast<double>(bytes) >= kMiB) {
    return Tr("%1 MB").arg(static_cast<double>(bytes) / kMiB, 0, 'f', 2);
  }
  if (static_cast<double>(bytes) >= kKiB) {
    return Tr("%1 KB").arg(static_cast<double>(bytes) / kKiB, 0, 'f', 1);
  }
  return Tr("%1 B").arg(static_cast<qulonglong>(bytes));
}

auto EntryStatusText(const LutLibraryEntry& entry) -> QString {
  if (entry.header_error != LutHeaderError::kNone) return Tr("Invalid");
  if (!entry.header.SupportsGradeApplication()) return Tr("Unsupported");
  return {};
}

auto EntryDetailText(const LutLibraryEntry& entry) -> QString {
  if (entry.header_error != LutHeaderError::kNone) return ToQString(entry.header_message);
  if (!entry.header.SupportsGradeApplication()) {
    return Tr("1D LUTs cannot be applied by the grade stage.");
  }
  return ToQString(entry.relative_path) + QStringLiteral("  |  ") + FormatByteSize(entry.size);
}

auto LutSizeText(const LutLibraryEntry& entry) -> QString {
  if (entry.header.lut_3d_size > 0) return QStringLiteral("3D %1").arg(entry.header.lut_3d_size);
  if (entry.header.lut_1d_size > 0) return QStringLiteral("1D %1").arg(entry.header.lut_1d_size);
  return {};
}

}  // namespace

LutLibraryModel::LutLibraryModel(QObject* parent) : QAbstractListModel(parent) {
  query_timer_.setSingleShot(true);
  query_timer_.setInterval(kQueryDelayMs);
  connect(&query_timer_, &QTimer::timeout, this, &LutLibraryModel::applyQueryNow);
  rebuildChoices();
}

LutLibraryModel::~LutLibraryModel() = default;

// ── Rows ────────────────────────────────────────────────────────────────────

auto LutLibraryModel::rowCount(const QModelIndex& parent) const -> int {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

auto LutLibraryModel::roleNames() const -> QHash<int, QByteArray> {
  return {{EntryIdRole, "entryId"},
          {RelativePathRole, "relativePath"},
          {DisplayNameRole, "displayName"},
          {PrintNameRole, "printName"},
          {SourceNameRole, "sourceName"},
          {FilmBrandRole, "filmBrand"},
          {CategoryRole, "category"},
          {HasPrintRole, "hasPrint"},
          {OfficialRole, "official"},
          {PackageIdRole, "packageId"},
          {FavoriteRole, "favorite"},
          {FocusedRole, "focused"},
          {AppliedRole, "applied"},
          {SelectableRole, "selectable"},
          {ValidRole, "valid"},
          {StatusTextRole, "statusText"},
          {DetailTextRole, "detailText"},
          {LutSizeTextRole, "lutSizeText"},
          {FileSizeRole, "fileSize"}};
}

auto LutLibraryModel::data(const QModelIndex& index, int role) const -> QVariant {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
    return {};
  }
  const LutSearchKeys& keys = keys_[static_cast<std::size_t>(rows_[index.row()])];
  switch (role) {
    case EntryIdRole:
      return ToQString(keys.entry_id);
    case RelativePathRole:
      return ToQString(keys.relative_path);
    case FavoriteRole:
      return keysFavorite(keys);
    case FocusedRole:
      return !focused_entry_id_.isEmpty() && ToQString(keys.entry_id) == focused_entry_id_;
    case AppliedRole:
      return !applied_entry_id_.isEmpty() && ToQString(keys.entry_id) == applied_entry_id_;
    case CategoryRole:
      return keys.category == LutCategory::kFilmSimulation ? QStringLiteral("film_simulation")
                                                           : QStringLiteral("general");
    case HasPrintRole:
      return keys.has_print;
    case SourceNameRole:
      return keys.source_label;
    case FilmBrandRole:
      return keys.brand_label;
    default:
      break;
  }
  QVariant value;
  if (!library_) return value;
  // Scoped service read: the row does not keep a copy of the entry.
  library_->ReadEntry(keys.relative_path, [&](const LutLibraryEntry& entry) {
    switch (role) {
      case Qt::DisplayRole:
      case DisplayNameRole:
        value = ToQString(entry.DisplayName());
        break;
      case PrintNameRole:
        value = ToQString(entry.PrintOptionName());
        break;
      case OfficialRole:
        value = entry.IsOfficial();
        break;
      case PackageIdRole:
        value = ToQString(entry.managed_package_id);
        break;
      case SelectableRole:
        value = entry.header_error == LutHeaderError::kNone &&
                entry.header.SupportsGradeApplication();
        break;
      case ValidRole:
        value = entry.header_error == LutHeaderError::kNone;
        break;
      case StatusTextRole:
        value = EntryStatusText(entry);
        break;
      case DetailTextRole:
        value = EntryDetailText(entry);
        break;
      case LutSizeTextRole:
        value = LutSizeText(entry);
        break;
      case FileSizeRole:
        value = static_cast<qulonglong>(entry.size);
        break;
      default:
        break;
    }
  });
  return value;
}

auto LutLibraryModel::rowOfEntry(const QString& entry_id) const -> int {
  if (entry_id.isEmpty()) return -1;
  const auto found = key_index_.find(ToUtf8(entry_id));
  if (found == key_index_.end()) return -1;
  const auto row = std::find(rows_.begin(), rows_.end(), found->second);
  return row == rows_.end() ? -1 : static_cast<int>(row - rows_.begin());
}

auto LutLibraryModel::entryIdAt(int row) const -> QString {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
  return ToQString(keys_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(row)])].entry_id);
}

void LutLibraryModel::emitRoleChanged(int row, int role) {
  if (row < 0) return;
  const QModelIndex changed = index(row);
  emit              dataChanged(changed, changed, {role});
}

// ── Library ─────────────────────────────────────────────────────────────────

void LutLibraryModel::setLibrary(alcedo::LutLibraryService* library) {
  if (library_ == library) return;
  if (library_) disconnect(library_, nullptr, this, nullptr);
  library_ = library;
  if (library_) {
    connect(library_, &alcedo::LutLibraryService::InventoryChanged, this,
            [this](const QStringList& changed_paths) {
              rebuildKeys(changed_paths, false);
              rerank();
              refilter();
            });
    connect(library_, &alcedo::LutLibraryService::RootChanged, this, [this] {
      rebuildKeys({}, true);
      rerank();
      refilter();
    });
    connect(library_, &alcedo::LutLibraryService::FavoritesChanged, this,
            &LutLibraryModel::applyFavoriteChange);
  }
  rebuildKeys({}, true);
  rerank();
  refilter();
  emit libraryChanged();
}

void LutLibraryModel::rebuildKeys(const QStringList& changed_paths, bool all) {
  std::unordered_map<std::string, LutSearchKeys> previous;
  if (!all) {
    previous.reserve(keys_.size());
    for (LutSearchKeys& keys : keys_) {
      std::string path = keys.relative_path;
      previous.emplace(std::move(path), std::move(keys));
    }
    for (const QString& path : changed_paths) previous.erase(ToUtf8(path));
  }
  keys_.clear();
  key_index_.clear();
  if (library_) {
    keys_.reserve(library_->EntryCount());
    library_->ForEachEntry([&](const LutLibraryEntry& entry) {
      const auto reused = previous.find(entry.relative_path);
      if (reused != previous.end()) {
        reused->second.modified_time = entry.modified_time;
        keys_.push_back(std::move(reused->second));
      } else {
        keys_.push_back(BuildLutSearchKeys(entry, LutLibraryPublication::EntryIdOf(entry)));
      }
    });
  }
  for (int i = 0; i < static_cast<int>(keys_.size()); ++i) {
    key_index_.emplace(keys_[static_cast<std::size_t>(i)].entry_id, i);
  }
}

auto LutLibraryModel::keysFavorite(const LutSearchKeys& keys) const -> bool {
  return library_ && library_->IsFavorite(keys.entry_id);
}

// ── Rows, order, and choices ────────────────────────────────────────────────

auto LutLibraryModel::computeRows() const -> std::vector<int> {
  struct Candidate {
    int           key_index;
    LutSearchRank rank;
  };
  std::vector<Candidate> candidates;
  for (int i = 0; i < static_cast<int>(keys_.size()); ++i) {
    const LutSearchKeys&                keys = keys_[static_cast<std::size_t>(i)];
    const std::optional<LutSearchRank>& rank = ranks_[static_cast<std::size_t>(i)];
    if (rank && LutKeysPassFilter(keys, keysFavorite(keys), filter_)) {
      candidates.push_back({i, *rank});
    }
  }
  const bool ranked = !query_.Empty();
  std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
    const LutSearchKeys& left  = keys_[static_cast<std::size_t>(a.key_index)];
    const LutSearchKeys& right = keys_[static_cast<std::size_t>(b.key_index)];
    if (ranked) {
      if (a.rank != b.rank) return a.rank < b.rank;
      return left.entry_id < right.entry_id;
    }
    if (sort_by_modified_) {
      if (left.modified_time != right.modified_time) {
        return sort_ascending_ ? left.modified_time < right.modified_time
                               : left.modified_time > right.modified_time;
      }
    } else if (const int order = QString::compare(left.sort_name, right.sort_name); order != 0) {
      return sort_ascending_ ? order < 0 : order > 0;
    }
    return left.entry_id < right.entry_id;
  });
  std::vector<int> rows;
  rows.reserve(candidates.size());
  for (const Candidate& candidate : candidates) rows.push_back(candidate.key_index);
  return rows;
}

void LutLibraryModel::rerank() {
  ranks_.clear();
  ranks_.reserve(keys_.size());
  LutEditDistanceMemo memo(query_);
  for (const LutSearchKeys& keys : keys_) ranks_.push_back(RankLutSearchKeys(keys, query_, &memo));
}

void LutLibraryModel::refilter() {
  beginResetModel();
  rows_ = computeRows();
  endResetModel();
  rebuildChoices();
  emit countChanged();
  emit focusChanged();
}

void LutLibraryModel::applyFavoriteChange() {
  if (filter_.favorites_only) {
    std::vector<int> updated = computeRows();
    std::size_t      kept    = 0;
    for (const int key : rows_) {
      if (kept < updated.size() && updated[kept] == key) ++kept;
    }
    if (kept != updated.size()) {
      // A favorite was added: it enters the favorites view at its place in the order.
      beginResetModel();
      rows_ = std::move(updated);
      endResetModel();
    } else {
      // An unfavorited entry leaves the view; the remaining rows keep their positions.
      std::vector<bool> listed(keys_.size(), false);
      for (const int key : updated) listed[static_cast<std::size_t>(key)] = true;
      for (int row = static_cast<int>(rows_.size()) - 1; row >= 0; --row) {
        if (listed[static_cast<std::size_t>(rows_[static_cast<std::size_t>(row)])]) continue;
        beginRemoveRows({}, row, row);
        rows_.erase(rows_.begin() + row);
        endRemoveRows();
      }
    }
    emit countChanged();
    emit focusChanged();
  }
  if (!rows_.empty()) {
    emit dataChanged(index(0), index(static_cast<int>(rows_.size()) - 1), {FavoriteRole});
  }
  rebuildChoices();
}

void LutLibraryModel::rebuildChoices() {
  // Counts per dimension apply every other predicate and the query (plan 6.3).
  auto count_where = [&](LutFacetDimension dimension, auto&& accept) {
    std::size_t index = 0;
    for (const LutSearchKeys& keys : keys_) {
      const bool matched = ranks_[index++].has_value();
      if (matched && LutKeysPassFilter(keys, keysFavorite(keys), filter_, dimension)) {
        accept(keys);
      }
    }
  };

  int general = 0;
  int film    = 0;
  count_where(LutFacetDimension::kCategory, [&](const LutSearchKeys& keys) {
    ++(keys.category == LutCategory::kFilmSimulation ? film : general);
  });
  category_choices_ = {
      Choice(QStringLiteral("all"), Tr("All"), general + film,
             filter_.category == LutCategoryFilter::kAll),
      Choice(QStringLiteral("general"), Tr("General"), general,
             filter_.category == LutCategoryFilter::kGeneral),
      Choice(QStringLiteral("film_simulation"), Tr("Film simulation"), film,
             filter_.category == LutCategoryFilter::kFilmSimulation)};

  auto dimension_choices = [&](LutFacetDimension dimension, const QString& selected,
                               QString LutSearchKeys::* key, QString LutSearchKeys::* label) {
    // Every value the library declares is a choice; its count may be zero.
    std::map<QString, std::pair<QString, int>> counts;
    for (const LutSearchKeys& keys : keys_) {
      if (!(keys.*key).isEmpty()) counts.try_emplace(keys.*key, keys.*label, 0);
    }
    int total = 0;
    count_where(dimension, [&](const LutSearchKeys& keys) {
      ++total;
      if (!(keys.*key).isEmpty()) ++counts[keys.*key].second;
    });
    // A chosen value stays listed, even when the library no longer declares it.
    if (!selected.isEmpty()) counts.try_emplace(selected, selected, 0);
    QVariantList choices{Choice({}, Tr("All"), total, selected.isEmpty())};
    for (const auto& [value, entry] : counts) {
      choices.push_back(Choice(value, entry.first, entry.second, value == selected));
    }
    return choices;
  };
  source_choices_ = dimension_choices(LutFacetDimension::kSource, filter_.source_id,
                                      &LutSearchKeys::source_id, &LutSearchKeys::source_label);
  if (filmFiltersAvailable()) {
    brand_choices_ = dimension_choices(LutFacetDimension::kBrand, filter_.brand_key,
                                       &LutSearchKeys::brand_key, &LutSearchKeys::brand_label);
  } else {
    brand_choices_ = {Choice({}, Tr("All"), 0, true)};
  }

  int with_print = 0;
  int no_print   = 0;
  if (filmFiltersAvailable()) {
    count_where(LutFacetDimension::kPrint,
                [&](const LutSearchKeys& keys) { ++(keys.has_print ? with_print : no_print); });
  }
  print_filter_available_ = with_print > 0;
  print_choices_          = {
      Choice(QStringLiteral("all"), Tr("All"), with_print + no_print,
                      filter_.print == LutPrintFilter::kAll),
      Choice(QStringLiteral("with_print"), Tr("With print"), with_print,
                      filter_.print == LutPrintFilter::kWithPrint),
      Choice(QStringLiteral("no_print"), Tr("No print"), no_print,
                      filter_.print == LutPrintFilter::kNoPrint)};
  emit choicesChanged();
}

// ── Query and filters ───────────────────────────────────────────────────────

void LutLibraryModel::setQueryText(const QString& text) {
  if (query_text_ == text) return;
  query_text_ = text;
  emit queryTextChanged();
  query_timer_.start();
}

void LutLibraryModel::applyQueryNow() {
  query_timer_.stop();
  LutSearchQuery parsed = ParseLutSearchQuery(query_text_);
  if (parsed.normalized == query_.normalized && parsed.tokens == query_.tokens) return;
  query_ = std::move(parsed);
  rerank();
  refilter();
}

auto LutLibraryModel::category() const -> QString { return CategoryText(filter_.category); }

void LutLibraryModel::setCategory(const QString& category) {
  LutCategoryFilter value = LutCategoryFilter::kAll;
  if (category == QStringLiteral("general")) value = LutCategoryFilter::kGeneral;
  if (category == QStringLiteral("film_simulation")) value = LutCategoryFilter::kFilmSimulation;
  if (value == filter_.category) return;
  filter_.category = value;
  // Brand and print presence do not apply to General LUTs: clear them to All.
  if (value == LutCategoryFilter::kGeneral) {
    filter_.brand_key.clear();
    filter_.print = LutPrintFilter::kAll;
  }
  emit filterChanged();
  refilter();
}

void LutLibraryModel::setSource(const QString& source_id) {
  if (filter_.source_id == source_id) return;
  filter_.source_id = source_id;
  emit filterChanged();
  refilter();
}

void LutLibraryModel::setBrand(const QString& brand_key) {
  const QString key = NormalizeLutSearchText(brand_key);
  if (filter_.brand_key == key || !filmFiltersAvailable()) return;
  filter_.brand_key = key;
  emit filterChanged();
  refilter();
}

auto LutLibraryModel::print() const -> QString { return PrintText(filter_.print); }

void LutLibraryModel::setPrint(const QString& print) {
  LutPrintFilter value = LutPrintFilter::kAll;
  if (print == QStringLiteral("with_print")) value = LutPrintFilter::kWithPrint;
  if (print == QStringLiteral("no_print")) value = LutPrintFilter::kNoPrint;
  if (value == filter_.print || !filmFiltersAvailable()) return;
  filter_.print = value;
  emit filterChanged();
  refilter();
}

void LutLibraryModel::setFavoritesOnly(bool favorites_only) {
  if (filter_.favorites_only == favorites_only) return;
  filter_.favorites_only = favorites_only;
  emit filterChanged();
  refilter();
}

void LutLibraryModel::clearFilters() {
  filter_ = LutFacetFilter{};
  query_text_.clear();
  query_timer_.stop();
  query_ = LutSearchQuery{};
  rerank();
  emit queryTextChanged();
  emit filterChanged();
  refilter();
}

auto LutLibraryModel::sortKey() const -> QString {
  return sort_by_modified_ ? QStringLiteral("modified") : QStringLiteral("name");
}

void LutLibraryModel::setSortKey(const QString& key) {
  const bool by_modified = key == QStringLiteral("modified");
  if (by_modified == sort_by_modified_) return;
  sort_by_modified_ = by_modified;
  emit orderChanged();
  refilter();
}

void LutLibraryModel::setSortAscending(bool ascending) {
  if (ascending == sort_ascending_) return;
  sort_ascending_ = ascending;
  emit orderChanged();
  refilter();
}

// ── Focus, applied entry, and favorites ─────────────────────────────────────

void LutLibraryModel::focusEntry(const QString& entry_id) {
  if (entry_id == focused_entry_id_) return;
  const int old_row = rowOfEntry(focused_entry_id_);
  focused_entry_id_ = entry_id;
  emitRoleChanged(old_row, FocusedRole);
  emitRoleChanged(rowOfEntry(focused_entry_id_), FocusedRole);
  emit focusChanged();
}

auto LutLibraryModel::focusRelative(int step) -> bool {
  if (step == 0 || rows_.empty()) return false;
  const int size    = static_cast<int>(rows_.size());
  const int current = rowOfEntry(focused_entry_id_);
  int       row     = current;
  for (int visited = 0; visited < size; ++visited) {
    row = current < 0 && visited == 0 ? (step > 0 ? 0 : size - 1)
                                      : ((row + (step > 0 ? 1 : -1)) % size + size) % size;
    if (row == current) return false;
    if (data(index(row), SelectableRole).toBool()) {
      focusEntry(entryIdAt(row));
      return true;
    }
  }
  return false;
}

void LutLibraryModel::setAppliedEntryId(const QString& entry_id) {
  if (entry_id == applied_entry_id_) return;
  const int old_row = rowOfEntry(applied_entry_id_);
  applied_entry_id_ = entry_id;
  emitRoleChanged(old_row, AppliedRole);
  emitRoleChanged(rowOfEntry(applied_entry_id_), AppliedRole);
  emit appliedEntryIdChanged();
}

auto LutLibraryModel::isFavorite(const QString& entry_id) const -> bool {
  return library_ && library_->IsFavorite(ToUtf8(entry_id));
}

auto LutLibraryModel::toggleFavorite(const QString& entry_id) -> bool {
  if (!library_) return false;
  const std::string id = ToUtf8(entry_id);
  if (library_->SetFavorite(id, !library_->IsFavorite(id)) ==
      alcedo::LutLibraryService::Status::kOk) {
    return true;
  }
  emit favoriteFailed(library_->last_error().isEmpty() ? Tr("The favorite cannot be changed.")
                                                       : library_->last_error());
  return false;
}

}  // namespace alcedo::ui
