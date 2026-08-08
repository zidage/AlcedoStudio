//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_module.hpp"

#include <QInputDialog>
#include <QSettings>
#include <algorithm>
#include <limits>

#include "image/image.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"

namespace alcedo::ui {

using namespace album_util;
#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {
constexpr size_t kAlbumMetadataPageSize  = 1000;
constexpr size_t kSearchMetadataPageSize = 120;

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
    : QObject(parent), project_(project), thumbs_(*this) {
  LoadThumbnailDiskCacheSettings();
}

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
  if (thumbnail_model_.loading() || !thumbnail_model_.hasMore()) {
    return false;
  }
  return stats_->LoadMoreThumbnailView();
}


bool LibraryModule::LoadThumbnailsThroughIndex(int index) {
  if (index < 0 || thumbnail_model_.loading()) {
    return false;
  }

  const int target     = TotalCount() > 0 ? std::min(index, TotalCount() - 1) : index;
  bool      loaded_any = false;
  while (thumbnail_model_.hasMore() && thumbnail_model_.count() <= target) {
    if (!stats_->LoadMoreThumbnailView()) {
      break;
    }
    loaded_any = true;
  }
  return loaded_any;
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


void LibraryModule::ReloadCurrentFolder() {
  stats_->RefreshStats();
  stats_->RebuildThumbnailView();
}


bool LibraryModule::LoadThumbnailWindow(const std::optional<FilterNode>& statsFilter, bool reset) {
  if (thumbnail_model_.loading()) {
    return false;
  }
  ThumbnailModelLoadingGuard loading_guard(thumbnail_model_);
  const auto                 merged_filter =
      MergeFilterNodes(statsFilter, search_->ActiveSearchFilterNode());
  const auto effective_filter_where = CompileFilterWhere(merged_filter);

  if (reset) {
    thumbs().ReleaseVisibleThumbnailPins();

    view_state_.all_images_.clear();
    view_state_.total_count_ = 0;
    thumbnail_model_.resetModel({}, 0);
    emit CountsChanged();
  }

  auto proj = project_->handler().project();
  if (!proj) {
    return false;
  }

  auto browse = proj->GetAlbumBrowseService();
  if (!browse) {
    return false;
  }

  const auto folder_id_opt = folders_->CurrentFolderElementId();
  if (!folder_id_opt.has_value()) {
    return false;
  }

  const auto folder_id   = folder_id_opt.value();
  const auto folder_path = folders_->CurrentFolderFsPath();
  if (reset || view_state_.total_count_ == 0) {
    view_state_.total_count_ = browse->CountFilesInFolderById(folder_id, effective_filter_where);
  }

  const size_t oldSize = view_state_.all_images_.size();
  if (oldSize >= view_state_.total_count_) {
    thumbnail_model_.setHasMore(false);
    emit CountsChanged();
    return false;
  }

  const auto page_size =
      search_->HasActiveSearchFilter() ? kSearchMetadataPageSize : kAlbumMetadataPageSize;
  const auto files =
      browse->ListFilesInFolderById(folder_id, oldSize, page_size, effective_filter_where);
  for (const auto& file : files) {
    const auto file_path =
        file.file_path_.empty() ? folder_path / file.file_name_ : file.file_path_;
    AddOrUpdateAlbumItem(
        file.file_id_, file.image_id_, file.folder_id_,
        file.scope_type_ == AlbumScopeType::Root ? QStringLiteral("root") : QStringLiteral("album"),
        file.file_name_, file_path);
  }

  const size_t           newSize = view_state_.all_images_.size();
  std::vector<AlbumItem> newBatch;
  if (newSize > oldSize) {
    newBatch.reserve(newSize - oldSize);
    for (size_t i = oldSize; i < newSize; ++i) {
      newBatch.push_back(view_state_.all_images_[i]);
    }
  }

  if (oldSize == 0) {
    thumbnail_model_.resetModel(view_state_.all_images_, view_state_.total_count_);
  } else if (!newBatch.empty()) {
    thumbnail_model_.appendPage(newBatch);
  } else {
    thumbnail_model_.setHasMore(thumbnail_model_.items().size() < view_state_.total_count_);
  }

  emit CountsChanged();
  return !files.empty();
}


void LibraryModule::AddOrUpdateAlbumItem(sl_element_id_t elementId, image_id_t imageId,
                                        sl_element_id_t folderId, const QString& scopeType,
                                        const file_name_t&           fallbackName,
                                        const std::filesystem::path& filePath) {
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

  if (!item) return;

  item->element_id = elementId;
  item->file_id    = elementId;
  item->image_id   = imageId;
  item->folder_id  = folderId;
  item->scope_type = scopeType;
  item->file_path_ = filePath;

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

  if (!item->import_date.isValid()) {
    item->import_date = QDate::currentDate();
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
