//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_module.hpp"

#include <QDate>
#include <QDateTime>
#include <QInputDialog>
#include <QSettings>
#include <QTimeZone>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <variant>

#include "app/project_service.hpp"
#include "image/image.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/storage.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "utils/string/convert.hpp"

namespace alcedo::ui {

using namespace album_util;
#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {
constexpr size_t kAlbumMetadataPageSize  = 1000;
constexpr size_t kSearchMetadataPageSize = 120;
/// Occurrence pages that a grouped view keeps loaded near its viewport.
constexpr size_t kMaxRetainedSectionPages = 3;

auto FormatCacheSize(size_t bytes) -> QString {
  if (bytes == 0) {
    return QStringLiteral("0 KiB");
  }
  if (bytes < 1024) {
    return QStringLiteral("< 1 KiB");
  }
  static constexpr const char* kUnits[] = {"KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes) / 1024.0;
  int unit_idx = 0;
  while (value >= 1024.0 && unit_idx < 3) {
    value /= 1024.0;
    ++unit_idx;
  }
  const int decimals = value >= 10.0 ? 1 : 2;
  return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals).arg(QLatin1String(kUnits[unit_idx]));
}

class ThumbnailModelLoadingGuard {
 public:
  explicit ThumbnailModelLoadingGuard(AlbumThumbnailModel& model) : model_(model) {
    model_.setLoading(true);
  }
  ~ThumbnailModelLoadingGuard() { model_.setLoading(false); }
  ThumbnailModelLoadingGuard(const ThumbnailModelLoadingGuard&) = delete;
  ThumbnailModelLoadingGuard& operator=(const ThumbnailModelLoadingGuard&) = delete;
 private:
  AlbumThumbnailModel& model_;
};
}  // namespace

LibraryModule::LibraryModule(ProjectModule* project, QObject* parent)
    : QObject(parent),
      project_(project),
      thumbs_(*this),
      query_worker_(std::make_unique<SearchRequestWorker>()) {
  LoadThumbnailDiskCacheSettings();
}

LibraryModule::~LibraryModule() { query_worker_.reset(); }

void LibraryModule::BindCollaborators(FolderController* folders, SearchController* search,
                                       StatsEngine* stats) {
  folders_ = folders;
  search_ = search;
  stats_ = stats;
}

void LibraryModule::SetSemanticLabelProvider(
    std::function<QString(sl_element_id_t)> provider) {
  semantic_label_provider_ = std::move(provider);
}

void LibraryModule::NotifyThumbnailsChanged() {
  emit ThumbnailsChanged();
  emit thumbnailsChanged();
}

void LibraryModule::NotifyCountsChanged() { emit CountsChanged(); }

void LibraryModule::EmitThumbnailUpdated(uint elementId, const QString& dataUrl, bool loading,
                                         bool missingSource, const QString& errorText) {
  emit ThumbnailUpdated(elementId, dataUrl, loading, missingSource, errorText);
  emit thumbnailUpdated(elementId, dataUrl, loading, missingSource, errorText);
}


auto LibraryModule::FilterInfo() const -> QString {
  return stats_->FormatPhotoInfo(ShownCount(), TotalCount());
}


int LibraryModule::TotalCount() const {
  return static_cast<int>(
      std::min<size_t>(view_state_.total_count_, std::numeric_limits<int>::max()));
}

QVariantList LibraryModule::Thumbnails() const {
  QVariantList rows;
  rows.reserve(static_cast<qsizetype>(thumbnail_model_.items().size()));
  int index = 0;
  for (const AlbumItem& image : thumbnail_model_.items()) {
    rows.push_back(stats_->MakeThumbMap(image, index++));
  }
  return rows;
}

// ── Q_INVOKABLE: Folder delegation ──────────────────────────────────────────


void LibraryModule::SetThumbnailVisible(uint elementId, uint imageId, bool visible, uint maxEdge) {
  thumbs().SetThumbnailVisible(elementId, imageId, visible, maxEdge);
}


void LibraryModule::SetThumbnailCacheHint(uint visibleCells, uint maxEdge) {
  auto thumb_svc = project_->handler().thumbnail_service();
  if (!thumb_svc) {
    return;
  }

  // Cache by count, but cap high-resolution tiers aggressively because
  // thumbnails are stored as float RGBA ImageBuffers before QML conversion.
  const uint32_t scroll_buffer = std::max<uint32_t>(visibleCells * 3, visibleCells + 4);
  uint32_t       tier_cap      = 96;
  if (maxEdge > 1024) {
    tier_cap = 8;
  } else if (maxEdge > 512) {
    tier_cap = 16;
  } else if (maxEdge > 256) {
    tier_cap = 48;
  }
  const uint32_t desired = std::clamp<uint32_t>(scroll_buffer, 4, tier_cap);
  try {
    thumb_svc->ResizeCache(desired);
  } catch (...) {
  }
}


bool LibraryModule::LoadMoreThumbnails() {
  if (refresh_in_flight_ || page_in_flight_ || !thumbnail_model_.hasMore() || !accepted_input_) {
    return false;
  }
  if (accepted_options_.group_field_ != AlbumGroupField::kNone) {
    // Grouped mode: the page after the last loaded page.
    const auto starts = section_model_.LoadedPageStarts();
    const auto next   = starts.empty() ? 0 : starts.back() + accepted_input_->page_size_;
    if (next >= occurrence_count_) {
      return false;
    }
    requested_begin_ = next;
    requested_end_   = std::min(occurrence_count_, next + accepted_input_->page_size_);
    SubmitPage(next);
    return true;
  }
  SubmitPage(loaded_occurrence_end_);
  return true;
}

bool LibraryModule::LoadThumbnailsThroughIndex(int index) {
  if (index < 0) {
    return false;
  }
  load_through_ = std::max<int64_t>(load_through_, index);
  if (accepted_options_.group_field_ != AlbumGroupField::kNone) {
    // Grouped mode: load the page that holds the occurrence, not every earlier page.
    if (accepted_input_ && index < occurrence_count_) {
      const auto page  = accepted_input_->page_size_;
      requested_begin_ = (index / page) * page;
      requested_end_   = std::min(occurrence_count_, requested_begin_ + page);
      ContinuePaging();
      return true;
    }
    return false;
  }
  if (loaded_occurrence_end_ > index) {
    return false;
  }
  return LoadMoreThumbnails();
}

int LibraryModule::IndexOfElementInCurrentView(uint elementId) {
  if (elementId == 0) {
    return -1;
  }
  const int loaded = thumbnail_model_.rowByElementId(elementId);
  if (loaded >= 0) {
    return loaded;
  }
  const auto input = AcceptedReadInput({});
  if (!input) {
    return -1;
  }
  // The editor restore asks synchronously whether a file is still in the list. One scalar
  // position read answers it; the page that holds the file then loads on the worker.
  try {
    const auto position = input->browse_->ReadAlbumFilePosition(
        input->folder_id_, MergeFilterNodes(input->stats_filter_, input->search_.filter_),
        input->options_, input->active_model_key_, static_cast<sl_element_id_t>(elementId),
        std::nullopt);
    if (!position.has_value()) {
      return -1;
    }
    LoadThumbnailsThroughIndex(static_cast<int>(position->occurrence_index_));
    return static_cast<int>(position->occurrence_index_);
  } catch (const std::exception& e) {
    SetQueryError(QString::fromUtf8(e.what()));
    return -1;
  }
}

// ── Q_INVOKABLE: Project I/O ────────────────────────────────────────────────


void LibraryModule::ReloadFolderTree(const std::filesystem::path& preferredFolderPath) {
  auto proj = project_->handler().project();
  if (!proj) {
    folders_->ClearState();
    if (folders_) emit folders_->FoldersChanged();
    if (folders_) emit folders_->FolderSelectionChanged();
    if (folders_) emit folders_->folderSelectionChanged();
    return;
  }

  auto browse = proj->GetAlbumBrowseService();
  if (!browse) {
    return;
  }

  folders_->ReloadTree(preferredFolderPath.empty() ? folders_->current_folder_path()
                                                      : preferredFolderPath);
}

void LibraryModule::ReloadCurrentFolder() { RequestLibraryRefresh(); }

bool LibraryModule::LoadThumbnailWindow(const std::optional<FilterNode>& /*statsFilter*/,
                                        bool reset) {
  if (reset) {
    RequestLibraryRefresh();
    return true;
  }
  return LoadMoreThumbnails();
}

void LibraryModule::ResetThumbnailWindow() {
  thumbs().ReleaseVisibleThumbnailPins();
  view_state_.all_images_.clear();
  view_state_.total_count_ = 0;
  occurrence_count_        = 0;
  loaded_occurrence_end_   = 0;
  load_through_            = -1;
  accepted_input_.reset();
  thumbnail_model_.resetModel({}, 0);
  section_model_.Clear();
  emit CountsChanged();
}

// ── Library query ───────────────────────────────────────────────────────────

auto CurrentLocalDayTimeZone() -> std::string {
  return QTimeZone::systemTimeZoneId().toStdString();
}

auto InspectorFieldName(AlbumSortField field) -> QString {
  switch (field) {
    case AlbumSortField::kCaptureTime:
      return QStringLiteral("date");
    case AlbumSortField::kImportTime:
      return QStringLiteral("import");
    case AlbumSortField::kCameraModel:
      return QStringLiteral("camera");
    case AlbumSortField::kLens:
      return QStringLiteral("lens");
    case AlbumSortField::kRating:
      return QStringLiteral("rating");
    case AlbumSortField::kLabels:
      return QStringLiteral("label");
    case AlbumSortField::kEditTime:
      return QStringLiteral("edited");
    case AlbumSortField::kNone:
      break;
  }
  return {};
}

auto InspectorFieldName(AlbumGroupField field) -> QString {
  switch (field) {
    case AlbumGroupField::kCaptureDay:
      return QStringLiteral("date");
    case AlbumGroupField::kImportDay:
      return QStringLiteral("import");
    case AlbumGroupField::kCameraModel:
      return QStringLiteral("camera");
    case AlbumGroupField::kLens:
      return QStringLiteral("lens");
    case AlbumGroupField::kRating:
      return QStringLiteral("rating");
    case AlbumGroupField::kLabels:
      return QStringLiteral("label");
    case AlbumGroupField::kEditDay:
      return QStringLiteral("edited");
    case AlbumGroupField::kNone:
      break;
  }
  return {};
}

namespace {

auto SortFieldFromName(const QString& name) -> std::optional<AlbumSortField> {
  for (const auto field :
       {AlbumSortField::kCaptureTime, AlbumSortField::kImportTime, AlbumSortField::kCameraModel,
        AlbumSortField::kLens, AlbumSortField::kRating, AlbumSortField::kLabels,
        AlbumSortField::kEditTime}) {
    if (InspectorFieldName(field) == name) {
      return field;
    }
  }
  return std::nullopt;
}

auto GroupFieldFromName(const QString& name) -> std::optional<AlbumGroupField> {
  for (const auto field :
       {AlbumGroupField::kCaptureDay, AlbumGroupField::kImportDay, AlbumGroupField::kCameraModel,
        AlbumGroupField::kLens, AlbumGroupField::kRating, AlbumGroupField::kLabels,
        AlbumGroupField::kEditDay}) {
    if (InspectorFieldName(field) == name) {
      return field;
    }
  }
  return std::nullopt;
}

/// Runs on the query worker: the pending search WHERE build, then one library read.
/// Fill output.row_display_ for the rows of output.result_ on the query worker: the image pool
/// fields that SQL does not return and the semantic label text. A cold image pool reads each
/// Image from the database, so doing this on the UI thread stalled the window for large folders.
void ReadRowDisplayValues(const LibraryQueryInput& input, LibraryQueryOutput& output) {
  const auto& rows = output.result_.rows_;
  output.row_display_.assign(rows.size(), AlbumItem{});
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row     = rows[i].photo_;
    auto&       display = output.row_display_[i];
    if (row.file_id_ == 0 || row.image_id_ == 0) {
      continue;
    }
    if (input.image_pool_) {
      try {
        input.image_pool_->Read<void>(row.image_id_, [&display](std::shared_ptr<Image> image) {
          if (!image) return;
          if (!image->image_path_.empty()) {
            display.extension = ExtensionUpper(image->image_path_);
          }
          const auto& exif     = image->exif_display_;
          display.iso          = static_cast<int>(exif.iso_);
          display.aperture     = static_cast<double>(exif.aperture_);
          display.focal_length = static_cast<double>(exif.focal_);
          display.is_hdr       = exif.is_hdr_;
        });
      } catch (...) {
      }
    }
    if (input.storage_ && !input.active_model_key_.empty()) {
      try {
        display.tags = ReadSemanticLabelDisplayText(*input.storage_, row.file_id_,
                                                    input.active_model_key_,
                                                    input.label_language_);
      } catch (...) {
      }
    }
  }
}

auto RunLibraryQuery(const LibraryQueryInput& input) -> LibraryQueryOutput {
  LibraryQueryOutput output;
  try {
    output.search_filter_ = input.search_.filter_;
    if (input.search_.pending_text_.has_value()) {
      if (!input.filter_service_) {
        throw std::runtime_error("Search requires the project filter service.");
      }
      output.search_filter_ = input.filter_service_->BuildFuzzySearchWhere(
          *input.search_.pending_text_, input.search_.field_mask_);
    }
    output.result_ = input.browse_->ReadAlbumQuery(
        input.folder_id_, MergeFilterNodes(input.stats_filter_, output.search_filter_),
        input.options_, input.active_model_key_, input.read_);
    ReadRowDisplayValues(input, output);
  } catch (const std::exception& e) {
    output.error_ = QString::fromUtf8(e.what());
  } catch (...) {
    output.error_ = QStringLiteral("Unknown library query error.");
  }
  return output;
}

auto GroupKeyFromTitle(AlbumGroupField field, const QString& title, bool unknown)
    -> std::optional<AlbumGroupKey> {
  if (field == AlbumGroupField::kNone) {
    return std::nullopt;
  }
  if (unknown) {
    return AlbumGroupKey{std::monostate{}};
  }
  if (title.isEmpty()) {
    return std::nullopt;
  }
  if (field == AlbumGroupField::kRating) {
    bool       ok    = false;
    const auto value = title.toLongLong(&ok);
    return ok ? std::optional<AlbumGroupKey>(AlbumGroupKey{static_cast<int64_t>(value)})
              : std::nullopt;
  }
  return AlbumGroupKey{title.toStdString()};
}

auto SamePresentation(const AlbumQueryOptions& left, const AlbumQueryOptions& right) -> bool {
  return left.sort_field_ == right.sort_field_ && left.sort_direction_ == right.sort_direction_ &&
         left.group_field_ == right.group_field_;
}

}  // namespace

auto LibraryModule::RequestedOptions() const -> AlbumQueryOptions {
  return pending_options_.value_or(accepted_options_);
}

void LibraryModule::ToggleInspectorSort(const QString& field, bool descending) {
  const auto sort_field = SortFieldFromName(field);
  if (!sort_field.has_value()) {
    SetQueryError(PL_TEXT("Unknown sort field: %1", field).Render());
    return;
  }
  auto       options   = RequestedOptions();
  const auto direction = descending ? SortDirection::kDescending : SortDirection::kAscending;
  if (options.sort_field_ == *sort_field && options.sort_direction_ == direction) {
    options.sort_field_     = AlbumSortField::kNone;
    options.sort_direction_ = SortDirection::kAscending;
  } else {
    options.sort_field_     = *sort_field;
    options.sort_direction_ = direction;
  }
  pending_options_ = options;
  RequestLibraryRefresh();
}

void LibraryModule::SetInspectorGrouping(const QString& field, bool enabled) {
  const auto group_field = GroupFieldFromName(field);
  if (!group_field.has_value()) {
    SetQueryError(PL_TEXT("Unknown group field: %1", field).Render());
    return;
  }
  auto options = RequestedOptions();
  if (enabled) {
    options.group_field_ = *group_field;
  } else if (options.group_field_ == *group_field) {
    options.group_field_ = AlbumGroupField::kNone;
  } else {
    return;
  }
  pending_options_ = options;
  RequestLibraryRefresh();
}

void LibraryModule::RetryLibraryQuery() { RequestLibraryRefresh(); }

void LibraryModule::SetQueryError(const QString& message) {
  query_error_ = message;
  emit QueryStateChanged();
}

void LibraryModule::RunAfterRefresh(std::function<void()> action) {
  after_refresh_.push_back(std::move(action));
}

void LibraryModule::RequestLibraryRefresh() {
  if (!refresh_in_flight_) {
    refresh_in_flight_ = true;
    emit QueryStateChanged();
  }
  if (refresh_scheduled_) {
    return;
  }
  refresh_scheduled_ = true;
  // Combine the requests of this event loop turn: a caller that clears a filter and then
  // reloads the folder gets one read with both changes.
  QMetaObject::invokeMethod(this, [this]() { SubmitPendingRefresh(); }, Qt::QueuedConnection);
}

void LibraryModule::SubmitPendingRefresh() {
  refresh_scheduled_   = false;
  // This read sees every edit written so far.
  edit_order_stale_    = false;
  auto       proj      = project_ ? project_->handler().project() : nullptr;
  const auto folder_id = folders_ ? folders_->CurrentFolderElementId() : std::nullopt;
  auto       browse    = proj ? proj->GetAlbumBrowseService() : nullptr;
  if (!proj || !browse || !folder_id.has_value() || !search_ || !stats_) {
    // No scope: show no content and run no SQL.
    query_worker_->Invalidate(SearchRequestKind::kApply);
    query_worker_->Invalidate(SearchRequestKind::kLibraryPage);
    query_worker_->Invalidate(SearchRequestKind::kLibraryIds);
    ResetThumbnailWindow();
    if (stats_) {
      stats_->ClearStats();
    }
    pending_options_.reset();
    after_refresh_.clear();
    refresh_in_flight_ = false;
    query_error_.clear();
    emit QueryStateChanged();
    return;
  }

  auto input                            = std::make_shared<LibraryQueryInput>();
  input->folder_id_                     = folder_id.value();
  input->stats_filter_                  = stats_->BuildStatsFilterNode();
  input->search_                        = search_->LibrarySearch();
  input->options_                       = RequestedOptions();
  input->options_.local_day_time_zone_  = CurrentLocalDayTimeZone();
  input->active_model_key_              = stats_->ActiveSemanticModelKey();
  input->page_size_ = static_cast<int64_t>(input->search_.pending_text_.has_value() ||
                                                   input->search_.filter_.has_value()
                                               ? kSearchMetadataPageSize
                                               : kAlbumMetadataPageSize);
  input->read_      = AlbumQueryRead{
           .offset_ = 0, .limit_ = input->page_size_, .read_groups_ = true, .read_statistics_ = true};
  input->browse_         = std::move(browse);
  input->filter_service_ = proj->GetSleeveFilterService();
  input->image_pool_     = proj->GetImagePoolService();
  input->storage_        = proj->GetStorage();
  input->label_language_ = CurrentUiSemanticLabelLanguage();

  // A different scope must not show the previous scope's photos while the new read runs.
  if (accepted_input_ && accepted_input_->folder_id_ != input->folder_id_) {
    ResetThumbnailWindow();
    stats_->ClearStats();
  }

  // Later reads of the old result must not publish after this refresh.
  query_worker_->Invalidate(SearchRequestKind::kLibraryPage);
  page_in_flight_                                    = false;
  std::shared_ptr<const LibraryQueryInput> submitted = input;
  query_worker_->Submit(SearchRequestKind::kApply, [this, submitted](std::uint64_t generation) {
    auto output = RunLibraryQuery(*submitted);
    QMetaObject::invokeMethod(
        this,
        [this, submitted, generation, output = std::move(output)]() mutable {
          if (!query_worker_->IsCurrent(SearchRequestKind::kApply, generation)) {
            return;
          }
          PublishRefresh(*submitted, std::move(output));
        },
        Qt::QueuedConnection);
  });
}

auto LibraryModule::ItemFromRow(const SearchResultRow& row, const AlbumItem& display,
                                sl_element_id_t folderId) -> AlbumItem {
  const QString scope_type  = folderId == 0 ? QStringLiteral("root") : QStringLiteral("album");
  const auto    folder_path = folders_ ? folders_->CurrentFolderFsPath() : std::filesystem::path{};
  const auto    file_name   = conv::FromBytes(row.file_name_);
  auto& item = UpsertAlbumItem(row.file_id_, row.image_id_, folderId, scope_type, file_name,
                               folder_path / file_name);
  // The query worker read these from the image pool and the label store (row_display_).
  item.extension    = display.extension;
  item.iso          = display.iso;
  item.aperture     = display.aperture;
  item.focal_length = display.focal_length;
  item.is_hdr       = display.is_hdr;
  item.tags         = display.tags;
  // SQL supplies these display values.
  item.file_name    = QString::fromUtf8(row.file_name_.c_str());
  item.camera_model = QString::fromUtf8(row.camera_model_.c_str());
  item.lens         = QString::fromUtf8(row.lens_.c_str());
  item.rating       = row.rating_;
  item.capture_date = QDate::fromString(QString::fromUtf8(row.capture_date_.c_str()), Qt::ISODate);
  item.import_date =
      row.added_time_.has_value()
          ? QDateTime::fromSecsSinceEpoch(*row.added_time_, QTimeZone::utc()).toLocalTime().date()
          : QDate{};
  if (item.extension.isEmpty()) {
    item.extension = ExtensionFromFileName(item.file_name);
  }
  return item;
}

void LibraryModule::PublishRefresh(const LibraryQueryInput& input, LibraryQueryOutput output) {
  if (!output.error_.isEmpty()) {
    // The accepted content and settings stay; the pending changes stay for Retry.
    qWarning().noquote() << "Library query failed:" << output.error_;
    refresh_in_flight_ = refresh_scheduled_;
    after_refresh_.clear();
    SetQueryError(output.error_);
    return;
  }
  auto&      result        = output.result_;
  const bool grouped       = input.options_.group_field_ != AlbumGroupField::kNone;
  const bool same_grouping = input.options_.group_field_ == accepted_options_.group_field_;

  // Begin the resets while every owner still exposes the previous accepted result.
  thumbnail_model_.beginReplace();
  section_model_.BeginReplace();
  thumbs().ReleaseVisibleThumbnailPins();
  view_state_.all_images_.clear();

  std::vector<sl_element_id_t> page_ids;
  page_ids.reserve(result.rows_.size());
  std::vector<AlbumItem>              items;
  std::unordered_set<sl_element_id_t> seen;
  for (std::size_t i = 0; i < result.rows_.size(); ++i) {
    const auto& row = result.rows_[i];
    page_ids.push_back(row.photo_.file_id_);
    if (row.photo_.file_id_ == 0 || row.photo_.image_id_ == 0 ||
        !seen.insert(row.photo_.file_id_).second) {
      continue;
    }
    items.push_back(ItemFromRow(row.photo_, output.row_display_[i], input.folder_id_));
  }

  accepted_options_ = input.options_;
  // A change requested after this read was submitted stays pending for its own refresh.
  if (pending_options_.has_value() && SamePresentation(*pending_options_, input.options_)) {
    pending_options_.reset();
  }
  accepted_input_           = std::make_shared<const LibraryQueryInput>(LibraryQueryInput{
                .folder_id_        = input.folder_id_,
                .stats_filter_     = input.stats_filter_,
                .search_           = LibrarySearchInput{.display_query_ = input.search_.display_query_,
                                                        .filter_        = output.search_filter_,
                                                        .pending_       = false,
                                                        .field_mask_    = input.search_.field_mask_},
                .options_          = input.options_,
                .active_model_key_ = input.active_model_key_,
                .page_size_        = input.page_size_});
  const bool search_changed = search_->AcceptLibrarySearch(input.search_, output.search_filter_);
  if (result.statistics_.has_value()) {
    stats_->ApplyFolderStats(ToAlbumStatsView(*result.statistics_), false);
  }
  occurrence_count_        = result.occurrence_count_;
  loaded_occurrence_end_   = static_cast<int64_t>(result.rows_.size());
  load_through_            = -1;
  view_state_.total_count_ = static_cast<size_t>(result.unique_file_count_);
  section_model_.InstallGroups(grouped ? result.groups_ : std::vector<AlbumGroupDescriptor>{},
                               result.unique_file_count_, grouped ? result.occurrence_count_ : 0,
                               same_grouping);
  view_state_.all_images_ = items;
  query_error_.clear();
  refresh_in_flight_ = refresh_scheduled_;

  // End the resets after every owner holds the new result, then notify.
  thumbnail_model_.endReplace(std::move(items), view_state_.total_count_,
                              loaded_occurrence_end_ < occurrence_count_);
  section_model_.EndReplace();
  if (grouped) {
    section_model_.StorePage(0, page_ids, kMaxRetainedSectionPages);
    requested_begin_ = 0;
    requested_end_   = static_cast<int64_t>(page_ids.size());
  }
  emit CountsChanged();
  emit PresentationChanged();
  emit QueryStateChanged();
  emit stats_->StatsChanged();
  emit stats_->StatsFilterChanged();
  if (search_changed) {
    emit search_->SearchStateChanged();
  }
  NotifyThumbnailsChanged();

  auto actions = std::move(after_refresh_);
  after_refresh_.clear();
  for (auto& action : actions) {
    action();
  }
}

auto LibraryModule::AcceptedReadInput(AlbumQueryRead read) const
    -> std::shared_ptr<const LibraryQueryInput> {
  auto proj = project_ ? project_->handler().project() : nullptr;
  if (!accepted_input_ || !proj) {
    return nullptr;
  }
  // The accepted input keeps no service: holding one would keep a closed project's database
  // open. Each read takes the services of the open project when it is submitted.
  auto input             = std::make_shared<LibraryQueryInput>(*accepted_input_);
  input->read_           = read;
  input->browse_         = proj->GetAlbumBrowseService();
  input->filter_service_ = proj->GetSleeveFilterService();
  input->image_pool_     = proj->GetImagePoolService();
  input->storage_        = proj->GetStorage();
  input->label_language_ = CurrentUiSemanticLabelLanguage();
  if (!input->browse_) {
    return nullptr;
  }
  return input;
}

void LibraryModule::SubmitPage(int64_t offset) {
  if (refresh_in_flight_ || !accepted_input_ || offset < 0 || offset >= occurrence_count_) {
    return;
  }
  const auto limit = std::min(accepted_input_->page_size_, occurrence_count_ - offset);
  auto       input = AcceptedReadInput(AlbumQueryRead{.offset_ = offset, .limit_ = limit});
  page_in_flight_  = true;
  thumbnail_model_.setLoading(true);
  query_worker_->Submit(SearchRequestKind::kLibraryPage, [this, input](std::uint64_t generation) {
    auto output = RunLibraryQuery(*input);
    QMetaObject::invokeMethod(
        this,
        [this, input, generation, output = std::move(output)]() mutable {
          if (!query_worker_->IsCurrent(SearchRequestKind::kLibraryPage, generation)) {
            return;
          }
          PublishPage(*input, std::move(output));
        },
        Qt::QueuedConnection);
  });
}

void LibraryModule::PublishPage(const LibraryQueryInput& input, LibraryQueryOutput output) {
  page_in_flight_ = false;
  thumbnail_model_.setLoading(false);
  if (!output.error_.isEmpty()) {
    SetQueryError(output.error_);
    return;
  }
  const auto& rows = output.result_.rows_;
  if (accepted_options_.group_field_ == AlbumGroupField::kNone) {
    // Flat mode: append the next part of the ordered unique stream.
    std::vector<AlbumItem> batch;
    batch.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& row = rows[i];
      if (row.photo_.file_id_ == 0 || row.photo_.image_id_ == 0 ||
          FindAlbumItem(row.photo_.file_id_) != nullptr) {
        continue;
      }
      batch.push_back(ItemFromRow(row.photo_, output.row_display_[i], input.folder_id_));
    }
    loaded_occurrence_end_ = input.read_.offset_ + static_cast<int64_t>(rows.size());
    if (!batch.empty()) {
      thumbnail_model_.appendPage(batch);
    }
    thumbnail_model_.setHasMore(loaded_occurrence_end_ < occurrence_count_);
    emit CountsChanged();
    if (load_through_ >= loaded_occurrence_end_ && thumbnail_model_.hasMore()) {
      LoadMoreThumbnails();
    }
    return;
  }

  // Grouped mode: keep the pages near the requested rows; the photo rows are the unique
  // files of those pages in occurrence order.
  std::vector<sl_element_id_t> ids;
  ids.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    ids.push_back(row.photo_.file_id_);
    if (row.photo_.file_id_ != 0 && row.photo_.image_id_ != 0) {
      (void)ItemFromRow(row.photo_, output.row_display_[i], input.folder_id_);
    }
  }
  section_model_.StorePage(input.read_.offset_, ids, kMaxRetainedSectionPages);
  std::vector<AlbumItem> items;
  for (const auto id : section_model_.LoadedUniqueFileIds()) {
    if (const auto* item = FindAlbumItem(id)) {
      items.push_back(*item);
    }
  }
  view_state_.all_images_ = items;
  const auto starts       = section_model_.LoadedPageStarts();
  const bool has_more =
      !starts.empty() && starts.back() + accepted_input_->page_size_ < occurrence_count_;
  thumbnail_model_.beginReplace();
  thumbnail_model_.endReplace(std::move(items), view_state_.total_count_, has_more);
  emit CountsChanged();
  ContinuePaging();
}

void LibraryModule::ContinuePaging() {
  if (page_in_flight_ || refresh_in_flight_ || !accepted_input_ ||
      accepted_options_.group_field_ == AlbumGroupField::kNone) {
    return;
  }
  const auto page = accepted_input_->page_size_;
  for (int64_t start = (requested_begin_ / page) * page; start < requested_end_; start += page) {
    if (section_model_.FileIdAt(start) == 0) {
      SubmitPage(start);
      return;
    }
  }
}

void LibraryModule::RequestSectionRows(int firstRow, int lastRow) {
  const auto range = section_model_.OccurrenceRangeForRows(firstRow, lastRow);
  const auto begin = range.value(QStringLiteral("begin")).toLongLong();
  const auto end   = range.value(QStringLiteral("end")).toLongLong();
  if (begin >= end) {
    return;
  }
  requested_begin_ = begin;
  requested_end_   = end;
  ContinuePaging();
}

auto LibraryModule::RequestOrderedFileIds(const QVariantList& ranges) -> qulonglong {
  std::vector<std::pair<int64_t, int64_t>> occurrence_ranges;
  for (const auto& value : ranges) {
    const auto map   = value.toMap();
    const auto begin = map.value(QStringLiteral("begin")).toLongLong();
    const auto end   = map.value(QStringLiteral("end")).toLongLong();
    if (begin >= 0 && end > begin) {
      occurrence_ranges.emplace_back(begin, end);
    }
  }
  if (refresh_in_flight_) {
    // The ids must come from the result that the next refresh accepts.
    RunAfterRefresh([this, ranges]() { RequestOrderedFileIds(ranges); });
    return 0;
  }
  auto input = AcceptedReadInput({});
  if (!input) {
    return 0;
  }
  const auto request_id = query_worker_->Submit(
      SearchRequestKind::kLibraryIds, [this, input, occurrence_ranges](std::uint64_t generation) {
        QVariantList ids;
        QString      error;
        try {
          std::unordered_set<sl_element_id_t> seen;
          std::vector<sl_element_id_t>        ordered;
          const auto filter = MergeFilterNodes(input->stats_filter_, input->search_.filter_);
          for (const auto& [begin, end] : occurrence_ranges) {
            for (const auto id :
                 input->browse_->ReadAlbumFileIds(input->folder_id_, filter, input->options_,
                                                  input->active_model_key_, begin, end)) {
              if (seen.insert(id).second) {
                ordered.push_back(id);
              }
            }
          }
          // Selection items: ids and names from typed columns, no thumbnail metadata.
          for (const auto& row : input->browse_->ReadAlbumFileRows(ordered)) {
            ids.push_back(QVariantMap{
                {QStringLiteral("elementId"), static_cast<uint>(row.file_id_)},
                {QStringLiteral("fileId"), static_cast<uint>(row.file_id_)},
                {QStringLiteral("imageId"), static_cast<uint>(row.image_id_)},
                {QStringLiteral("fileName"), QString::fromUtf8(row.file_name_.c_str())}});
          }
        } catch (const std::exception& e) {
          error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, generation, ids = std::move(ids), error]() {
              if (!query_worker_->IsCurrent(SearchRequestKind::kLibraryIds, generation)) {
                return;
              }
              if (!error.isEmpty()) {
                SetQueryError(error);
                return;
              }
              emit OrderedFileIdsReady(static_cast<qulonglong>(generation), ids);
              emit orderedFileIdsReady(static_cast<qulonglong>(generation), ids);
            },
            Qt::QueuedConnection);
      });
  return static_cast<qulonglong>(request_id);
}

auto LibraryModule::RequestAllFileIds() -> qulonglong {
  return RequestOrderedFileIds(QVariantList{QVariantMap{
      {QStringLiteral("begin"), qint64{0}},
      {QStringLiteral("end"), static_cast<qint64>(std::max<int64_t>(occurrence_count_, 1))}}});
}

void LibraryModule::RequestFocusPosition(uint fileId, const QString& preferredGroupTitle,
                                         bool preferredGroupUnknown) {
  if (fileId == 0) {
    return;
  }
  if (refresh_in_flight_) {
    RunAfterRefresh([this, fileId, preferredGroupTitle, preferredGroupUnknown]() {
      RequestFocusPosition(fileId, preferredGroupTitle, preferredGroupUnknown);
    });
    return;
  }
  auto input = AcceptedReadInput({});
  if (!input) {
    return;
  }
  const auto preferred =
      GroupKeyFromTitle(input->options_.group_field_, preferredGroupTitle, preferredGroupUnknown);
  query_worker_->Submit(SearchRequestKind::kLibraryIds, [this, input, fileId,
                                                         preferred](std::uint64_t generation) {
    std::optional<AlbumFilePosition> position;
    QString                          error;
    try {
      position = input->browse_->ReadAlbumFilePosition(
          input->folder_id_, MergeFilterNodes(input->stats_filter_, input->search_.filter_),
          input->options_, input->active_model_key_, static_cast<sl_element_id_t>(fileId),
          preferred);
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    QMetaObject::invokeMethod(
        this,
        [this, generation, fileId, position, error]() {
          if (!query_worker_->IsCurrent(SearchRequestKind::kLibraryIds, generation)) {
            return;
          }
          if (!error.isEmpty()) {
            SetQueryError(error);
            return;
          }
          if (SettlePendingReveal(fileId, position.has_value())) {
            return;
          }
          if (!position.has_value()) {
            emit FocusPositionReady(fileId, -1, -1);
            emit focusPositionReady(fileId, -1, -1);
            return;
          }
          const auto occurrence = position->occurrence_index_;
          LoadThumbnailsThroughIndex(static_cast<int>(occurrence));
          const int row = section_model_.RowForOccurrence(occurrence);
          emit      FocusPositionReady(fileId, occurrence, row);
          emit      focusPositionReady(fileId, occurrence, row);
        },
        Qt::QueuedConnection);
  });
}

void LibraryModule::SetOccurrenceThumbnailVisible(const QString& groupTitle, bool groupUnknown,
                                                  uint elementId, uint imageId, bool visible,
                                                  uint maxEdge) {
  // The view's group title (or the unknown flag) identifies the occurrence of the photo.
  thumbs().SetOccurrenceThumbnailVisible(
      groupUnknown ? std::string("u:") : "t:" + groupTitle.toStdString(), elementId, imageId,
      visible, maxEdge);
}

auto LibraryModule::UpsertAlbumItem(sl_element_id_t elementId, image_id_t imageId,
                                    sl_element_id_t folderId, const QString& scopeType,
                                    const file_name_t&           fallbackName,
                                    const std::filesystem::path& filePath) -> AlbumItem& {
  AlbumItem* item = FindAlbumItem(elementId);

  if (!item) {
    AlbumItem next;
    next.element_id = elementId;
    next.file_id    = elementId;
    next.image_id   = imageId;
    next.folder_id  = folderId;
    next.scope_type = scopeType;
    next.file_path_ = filePath;
    next.file_name  = WStringToQString(fallbackName);
    next.extension  = ExtensionFromFileName(next.file_name);
    next.accent     = AccentForIndex(view_state_.all_images_.size());

    view_state_.all_images_.push_back(std::move(next));
    item = &view_state_.all_images_.back();
  }

  item->element_id = elementId;
  item->file_id    = elementId;
  item->image_id   = imageId;
  item->folder_id  = folderId;
  item->scope_type = scopeType;
  item->file_path_ = filePath;
  return *item;
}

void LibraryModule::AddOrUpdateAlbumItem(sl_element_id_t elementId, image_id_t imageId,
                                        sl_element_id_t folderId, const QString& scopeType,
                                        const file_name_t&           fallbackName,
                                        const std::filesystem::path& filePath) {
  AlbumItem* item =
      &UpsertAlbumItem(elementId, imageId, folderId, scopeType, fallbackName, filePath);

  auto proj        = project_->handler().project();
  if (proj) {
    try {
      proj->GetImagePoolService()->Read<void>(imageId, [item](std::shared_ptr<Image> image) {
        if (!image) return;
        if (!image->image_name_.empty()) {
          item->file_name = WStringToQString(image->image_name_);
        }
        if (!image->image_path_.empty()) {
          item->extension = ExtensionUpper(image->image_path_);
        }

        const auto& exif        = image->exif_display_;
        item->camera_model      = QString::fromUtf8(exif.model_.c_str());
        item->lens              = QString::fromUtf8(exif.lens_.c_str());
        item->iso               = static_cast<int>(exif.iso_);
        item->aperture          = static_cast<double>(exif.aperture_);
        item->focal_length      = static_cast<double>(exif.focal_);
        item->rating            = exif.rating_;
        item->is_hdr            = exif.is_hdr_;
        const QDate captureDate = DateFromExifString(exif.date_time_str_);
        if (captureDate.isValid()) {
          item->capture_date = captureDate;
        }
      });
    } catch (...) {
    }
  }

  if (item->extension.isEmpty()) {
    item->extension = ExtensionFromFileName(item->file_name);
  }
  item->tags = semantic_label_provider_ ? semantic_label_provider_(elementId) : QString{};
}


void LibraryModule::SetAlbumItemHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr) {
  if (auto* item = FindAlbumItem(elementId);
      item != nullptr && (imageId == 0 || item->image_id == imageId)) {
    item->is_hdr = isHdr;
  }
  thumbnail_model_.updateHdrFlag(elementId, imageId, isHdr);
}


void LibraryModule::PersistImageHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr) {
  auto proj = project_->handler().project();
  if (!proj || imageId == 0) {
    return;
  }

  try {
    proj->GetImagePoolService()->Write_NoSync<void>(imageId,
                                                    [isHdr](const std::shared_ptr<Image>& image) {
                                                      if (image) {
                                                        image->SetHdrDisplayMetadata(isHdr);
                                                      }
                                                    });
    SetAlbumItemHdrFlag(elementId, imageId, isHdr);
  } catch (...) {
  }
}


auto LibraryModule::FindAlbumItem(sl_element_id_t elementId) -> AlbumItem* {
  for (auto& item : view_state_.all_images_) {
    if (item.element_id == elementId) {
      return &item;
    }
  }
  return nullptr;
}

auto LibraryModule::FindAlbumItem(sl_element_id_t elementId) const -> const AlbumItem* {
  for (const auto& item : view_state_.all_images_) {
    if (item.element_id == elementId) {
      return &item;
    }
  }

  const auto& visible_items = thumbnail_model_.items();
  for (const auto& item : visible_items) {
    if (item.element_id == elementId) {
      return &item;
    }
  }
  return nullptr;
}

// ── Phase 4: Thumbnail disk cache settings ─────────────────────────────────


void LibraryModule::LoadThumbnailDiskCacheSettings() {
  QSettings settings;
  thumbnail_disk_cache_enabled_ =
      settings.value(QStringLiteral("thumbnailCache/enabled"), true).toBool();
  thumbnail_disk_cache_root_ =
      settings.value(QStringLiteral("thumbnailCache/rootPath"), QString{}).toString();
  thumbnail_disk_cache_max_entries_ =
      settings.value(QStringLiteral("thumbnailCache/maxEntries"), 10000).toInt();
  thumbnail_disk_cache_jpeg_quality_ =
      settings.value(QStringLiteral("thumbnailCache/jpegQuality"), 85).toInt();
}


void LibraryModule::SaveThumbnailDiskCacheSettings() {
  QSettings settings;
  settings.setValue(QStringLiteral("thumbnailCache/enabled"), thumbnail_disk_cache_enabled_);
  settings.setValue(QStringLiteral("thumbnailCache/rootPath"), thumbnail_disk_cache_root_);
  settings.setValue(QStringLiteral("thumbnailCache/maxEntries"), thumbnail_disk_cache_max_entries_);
  settings.setValue(QStringLiteral("thumbnailCache/jpegQuality"),
                    thumbnail_disk_cache_jpeg_quality_);
}


void LibraryModule::ApplyThumbnailDiskCacheSettingsToService() {
  auto thumb_svc = project_->handler().thumbnail_service();
  if (!thumb_svc) return;

  thumb_svc->SetDiskCacheEnabled(thumbnail_disk_cache_enabled_);
  if (!thumbnail_disk_cache_root_.isEmpty()) {
    thumb_svc->SetDiskCacheRoot(std::filesystem::path(thumbnail_disk_cache_root_.toStdWString()));
  }
  thumb_svc->SetDiskCacheMaxEntries(static_cast<size_t>(thumbnail_disk_cache_max_entries_));
  thumb_svc->SetDiskCacheJpegQuality(thumbnail_disk_cache_jpeg_quality_);
}

bool    LibraryModule::ThumbnailDiskCacheEnabled() const { return thumbnail_disk_cache_enabled_; }


QString LibraryModule::ThumbnailDiskCacheRoot() const {
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc) {
    const auto root = thumb_svc->GetDiskCacheRoot();
    if (!root.empty()) {
      return QString::fromStdWString(root.wstring());
    }
  }
  return thumbnail_disk_cache_root_;
}


int LibraryModule::ThumbnailDiskCacheMaxEntries() const {
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc) {
    return static_cast<int>(thumb_svc->GetDiskCacheMaxEntries());
  }
  return thumbnail_disk_cache_max_entries_;
}


int LibraryModule::ThumbnailDiskCacheJpegQuality() const {
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc) {
    return thumb_svc->GetDiskCacheJpegQuality();
  }
  return thumbnail_disk_cache_jpeg_quality_;
}


QString LibraryModule::ThumbnailDiskCacheStats() const {
  auto thumb_svc = project_->handler().thumbnail_service();
  if (!thumb_svc) {
    return QStringLiteral("No thumbnail service.");
  }
  const auto stats = thumb_svc->GetDiskCacheStats();
  return QStringLiteral(
             "Enabled: %1\n"
             "Entries: %2\n"
             "Size: %3\n"
             "Max entries: %4\n"
             "Hits: %5 / Misses: %6\n"
             "Root: %7")
      .arg(stats.enabled ? QStringLiteral("Yes") : QStringLiteral("No"))
      .arg(stats.total_entries)
      .arg(FormatCacheSize(stats.total_size_bytes))
      .arg(stats.max_entries)
      .arg(stats.hit_count)
      .arg(stats.miss_count)
      .arg(QString::fromStdString(stats.cache_root_path));
}


void LibraryModule::SetThumbnailDiskCacheEnabled(bool enabled) {
  thumbnail_disk_cache_enabled_ = enabled;
  SaveThumbnailDiskCacheSettings();
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc) {
    thumb_svc->SetDiskCacheEnabled(enabled);
  }
  emit ThumbnailDiskCacheStateChanged();
}


void LibraryModule::SetThumbnailDiskCacheRoot(const QString& rootPath) {
  const auto    root_path_opt   = InputToPath(rootPath);
  const QString normalized_root = root_path_opt.has_value()
                                      ? PathToQString(root_path_opt.value().lexically_normal())
                                      : rootPath;
  thumbnail_disk_cache_root_    = normalized_root;
  SaveThumbnailDiskCacheSettings();
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc && !thumbnail_disk_cache_root_.isEmpty()) {
    thumb_svc->SetDiskCacheRoot(std::filesystem::path(thumbnail_disk_cache_root_.toStdWString()));
  }
  emit ThumbnailDiskCacheStateChanged();
}


void LibraryModule::SetThumbnailDiskCacheMaxEntries(int maxEntries) {
  thumbnail_disk_cache_max_entries_ = std::max(1, maxEntries);
  SaveThumbnailDiskCacheSettings();
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc) {
    thumb_svc->SetDiskCacheMaxEntries(static_cast<size_t>(thumbnail_disk_cache_max_entries_));
  }
  emit ThumbnailDiskCacheStateChanged();
}


void LibraryModule::SetThumbnailDiskCacheJpegQuality(int quality) {
  thumbnail_disk_cache_jpeg_quality_ = std::clamp(quality, 1, 100);
  SaveThumbnailDiskCacheSettings();
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc) {
    thumb_svc->SetDiskCacheJpegQuality(thumbnail_disk_cache_jpeg_quality_);
  }
  emit ThumbnailDiskCacheStateChanged();
}


void LibraryModule::ClearAllThumbnailDiskCache() {
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc) {
    thumb_svc->ClearAllDiskCache();
  }
  emit ThumbnailDiskCacheStateChanged();
  project_->SetServiceMessageForCurrentProject(PL_TEXT("All thumbnail disk cache cleared."));
}


void LibraryModule::ClearProjectThumbnailDiskCache() {
  auto thumb_svc = project_->handler().thumbnail_service();
  if (thumb_svc) {
    thumb_svc->ClearProjectDiskCache();
  }
  emit ThumbnailDiskCacheStateChanged();
  project_->SetServiceMessageForCurrentProject(PL_TEXT("Current project thumbnail disk cache cleared."));
}


int LibraryModule::PromptForInt(const QString& title, const QString& label, int defaultValue,
                               int minValue, int maxValue) {
  bool accepted = false;
  int  value =
      QInputDialog::getInt(nullptr, title, label, defaultValue, minValue, maxValue, 1, &accepted);
  return accepted ? value : defaultValue;
}

}  // namespace alcedo::ui

#undef PL_TEXT
