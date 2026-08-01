//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/thumbnail_disk_cache_service.hpp"

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "json.hpp"
#include "type/hash_type.hpp"
#include "utils/cache/lru_cache.hpp"
#include "utils/queue/queue.hpp"

namespace alcedo {

namespace {
OIIO_NAMESPACE_USING

constexpr int         kDefaultJpegQuality     = 85;
constexpr int         kDefaultWebPQuality     = 80;
constexpr size_t      kDefaultMaxEntries      = 10000;
constexpr uint32_t    kCacheSchemaVersion     = 1;
constexpr auto        kGlobalMetadataFilename = "cache_global.json";

std::filesystem::path GetDefaultCacheRoot() {
#if defined(_WIN32)
  const char* local_app_data = std::getenv("LOCALAPPDATA");
  if (local_app_data) {
    return std::filesystem::path(local_app_data) / "alcedo" / "thumbnails";
  }
  return std::filesystem::temp_directory_path() / "alcedo_thumbnails";
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (home) {
    return std::filesystem::path(home) / "Library" / "Caches" / "alcedo" / "thumbnails";
  }
  return std::filesystem::temp_directory_path() / "alcedo_thumbnails";
#else
  const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
  if (xdg_cache) {
    return std::filesystem::path(xdg_cache) / "alcedo" / "thumbnails";
  }
  const char* home = std::getenv("HOME");
  if (home) {
    return std::filesystem::path(home) / ".cache" / "alcedo" / "thumbnails";
  }
  return std::filesystem::temp_directory_path() / "alcedo_thumbnails";
#endif
}

std::string FormatSizeBytes(uint64_t low, uint64_t high) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  oss << std::setw(16) << high;
  oss << std::setw(16) << low;
  return oss.str();
}

const char* FormatFileExtension(ThumbnailCacheFormat format) {
  switch (format) {
    case ThumbnailCacheFormat::kJpeg:
      return ".jpg";
    case ThumbnailCacheFormat::kWebP:
      return ".webp";
    case ThumbnailCacheFormat::kBmp:
      return ".bmp";
  }
  return ".jpg";
}

std::string MakeElementInvalidationKey(const std::string& project_uuid,
                                       sl_element_id_t    element_id) {
  std::ostringstream oss;
  oss << project_uuid << '|' << element_id;
  return oss.str();
}

std::string PathToUtf8(const std::filesystem::path& path) {
  auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

cv::Mat PrepareForOiioEncoding(const cv::Mat& src) {
  if (src.empty()) {
    return {};
  }

  cv::Mat   rgb;
  const int channels = src.channels();
  if (channels == 4) {
    cv::cvtColor(src, rgb, cv::COLOR_RGBA2RGB);
  } else if (channels == 3) {
    if (src.depth() == CV_32F) {
      rgb = src;
    } else {
      cv::cvtColor(src, rgb, cv::COLOR_BGR2RGB);
    }
  } else if (channels == 1) {
    cv::cvtColor(src, rgb, cv::COLOR_GRAY2RGB);
  } else {
    return {};
  }

  cv::Mat rgb8;
  if (rgb.depth() == CV_8U) {
    rgb8 = rgb;
  } else if (rgb.depth() == CV_32F) {
    rgb.convertTo(rgb8, CV_8UC3, 255.0);
  } else {
    rgb.convertTo(rgb8, CV_8UC3);
  }

  return rgb8.isContinuous() ? rgb8 : rgb8.clone();
}

cv::Mat PrepareForOpenCvEncoding(const cv::Mat& src) {
  cv::Mat rgb8 = PrepareForOiioEncoding(src);
  if (rgb8.empty() || rgb8.type() != CV_8UC3) {
    return {};
  }

  cv::Mat bgr8;
  cv::cvtColor(rgb8, bgr8, cv::COLOR_RGB2BGR);
  return bgr8.isContinuous() ? bgr8 : bgr8.clone();
}

bool WriteWithOpenImageIO(const std::filesystem::path& file_path, const cv::Mat& src,
                          ThumbnailCacheFormat format, int quality) {
  if (format != ThumbnailCacheFormat::kJpeg && format != ThumbnailCacheFormat::kWebP) {
    return false;
  }

  cv::Mat rgb8 = PrepareForOiioEncoding(src);
  if (rgb8.empty() || rgb8.type() != CV_8UC3) {
    return false;
  }

  const auto dst = PathToUtf8(file_path);
  auto       out = ImageOutput::create(dst);
  if (!out) {
    return false;
  }

  ImageSpec spec(rgb8.cols, rgb8.rows, 3, TypeDesc::UINT8);
  spec.attribute("CompressionQuality", quality);
  if (!out->open(dst, spec)) {
    return false;
  }

  const bool ok = out->write_image(TypeDesc::UINT8, rgb8.data);
  out->close();
  return ok;
}

cv::Mat ReadWithOpenImageIO(const std::filesystem::path& file_path) {
  auto input = ImageInput::open(PathToUtf8(file_path));
  if (!input) {
    return {};
  }

  const ImageSpec spec = input->spec();
  if (spec.width <= 0 || spec.height <= 0 || spec.nchannels <= 0) {
    input->close();
    return {};
  }

  const int            channels_to_read = std::min(spec.nchannels, 4);
  std::vector<uint8_t> pixels(static_cast<size_t>(spec.width) * static_cast<size_t>(spec.height) *
                              static_cast<size_t>(channels_to_read));
  const bool ok = input->read_image(0, 0, 0, channels_to_read, TypeDesc::UINT8, pixels.data());
  input->close();
  if (!ok) {
    return {};
  }

  cv::Mat decoded(spec.height, spec.width, CV_MAKETYPE(CV_8U, channels_to_read), pixels.data());
  cv::Mat bgr;
  if (channels_to_read == 4) {
    cv::cvtColor(decoded, bgr, cv::COLOR_RGBA2BGR);
  } else if (channels_to_read == 3) {
    cv::cvtColor(decoded, bgr, cv::COLOR_RGB2BGR);
  } else {
    cv::cvtColor(decoded, bgr, cv::COLOR_GRAY2BGR);
  }
  return bgr.isContinuous() ? bgr : bgr.clone();
}

int64_t SystemTimeSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

struct ThumbnailDiskCacheService::State {
  std::filesystem::path                      cache_root_;
  std::string                                project_uuid_;
  std::filesystem::path                      project_cache_dir_;
  std::filesystem::path                      metadata_file_path_;
  std::filesystem::path                      global_metadata_path_;

  std::unordered_map<std::string, EntryMeta> index_;
  LRUCache<std::string, std::string>         lru_index_{kDefaultMaxEntries};
  size_t                                     total_size_bytes_ = 0;
  std::unordered_map<std::string, uint64_t>  invalidation_generations_;
  uint64_t                                   clear_generation_ = 0;

  ConcurrentBlockingQueue<WriteTask>         write_queue_;
  std::thread                                writer_thread_;
  std::atomic<bool>                          writer_running_{false};

  mutable std::mutex                         metadata_mutex_;
  mutable std::atomic<size_t>                hit_count_{0};
  mutable std::atomic<size_t>                miss_count_{0};

  bool                                       initialized_  = false;
  bool                                       enabled_      = true;
  size_t                                     max_entries_  = kDefaultMaxEntries;
  int                                        jpeg_quality_ = kDefaultJpegQuality;
  int                                        webp_quality_ = kDefaultWebPQuality;
};

int64_t     ThumbnailDiskCacheService::CurrentTimeSeconds() const { return SystemTimeSeconds(); }

std::string ThumbnailDiskCacheService::MakeKeyHashString(const ThumbnailDiskCacheKey& key) {
  std::ostringstream oss;
  oss << key.project_uuid << '|' << key.element_id << '|' << static_cast<uint32_t>(key.resolution)
      << '|' << key.edit_version_hash << '|' << key.cache_schema_version;
  if (key.purpose != ThumbnailDiskCachePurpose::kThumbnail) {
    oss << '|' << static_cast<uint32_t>(key.purpose);
  }
  const auto str  = oss.str();
  const auto hash = Hash128::Compute(str.data(), str.size());
  return FormatSizeBytes(hash.low64(), hash.high64());
}

std::filesystem::path ThumbnailDiskCacheService::DeriveFilePath(const std::string&   key_hash,
                                                                ThumbnailCacheFormat format) const {
  const auto& dir = state_->project_cache_dir_;
  return dir / key_hash.substr(0, 2) / key_hash.substr(2, 2) /
         (key_hash + FormatFileExtension(format));
}

ThumbnailDiskCacheService::ThumbnailDiskCacheService()
    : ThumbnailDiskCacheService(GetDefaultCacheRoot()) {}

ThumbnailDiskCacheService::ThumbnailDiskCacheService(const std::filesystem::path& cache_root)
    : state_(std::make_unique<State>()) {
  state_->cache_root_ = cache_root;
}

ThumbnailDiskCacheService::~ThumbnailDiskCacheService() { Shutdown(); }

void ThumbnailDiskCacheService::Initialize(const std::string& project_uuid) {
  if (state_->initialized_) {
    return;
  }

  state_->project_uuid_         = project_uuid;
  state_->project_cache_dir_    = state_->cache_root_ / project_uuid;
  state_->metadata_file_path_   = state_->project_cache_dir_ / "cache_metadata.json";
  state_->global_metadata_path_ = state_->cache_root_ / kGlobalMetadataFilename;

  {
    std::unique_lock lock(state_->metadata_mutex_);
    state_->index_.clear();
    state_->lru_index_.Flush();
    state_->total_size_bytes_ = 0;
  }

  std::error_code ec;
  std::filesystem::create_directories(state_->project_cache_dir_, ec);

  LoadGlobalMetadata();
  LoadMetadata();

  state_->writer_running_ = true;
  state_->writer_thread_  = std::thread(&ThumbnailDiskCacheService::WriterThreadLoop, this);

  state_->initialized_    = true;
}

void ThumbnailDiskCacheService::Shutdown() {
  if (!state_->initialized_) {
    return;
  }

  state_->write_queue_.push(WriteTask{});

  if (state_->writer_thread_.joinable()) {
    state_->writer_thread_.join();
  }

  FlushMetadata();
  state_->initialized_ = false;
}

bool ThumbnailDiskCacheService::Lookup(const ThumbnailDiskCacheKey& key) {
  if (!state_->initialized_ || !state_->enabled_) {
    return false;
  }
  if (key.project_uuid != state_->project_uuid_) {
    return false;
  }

  const auto       key_hash = MakeKeyHashString(key);

  std::unique_lock lock(state_->metadata_mutex_);
  auto             it = state_->index_.find(key_hash);
  if (it != state_->index_.end()) {
    state_->hit_count_++;
    it->second.last_access_time = CurrentTimeSeconds();
    RecordLruAccessLocked(key_hash);
    return true;
  }
  state_->miss_count_++;
  return false;
}

std::unique_ptr<ImageBuffer> ThumbnailDiskCacheService::Read(const ThumbnailDiskCacheKey& key) {
  if (!state_->initialized_ || !state_->enabled_) {
    return nullptr;
  }
  if (key.project_uuid != state_->project_uuid_) {
    return nullptr;
  }

  const auto            key_hash = MakeKeyHashString(key);
  std::filesystem::path file_path;

  {
    std::unique_lock lock(state_->metadata_mutex_);
    auto             it = state_->index_.find(key_hash);
    if (it == state_->index_.end()) {
      state_->miss_count_++;
      return nullptr;
    }
    state_->hit_count_++;
    it->second.last_access_time = CurrentTimeSeconds();
    RecordLruAccessLocked(key_hash);
    file_path = it->second.file_path;
  }

  std::error_code ec;
  if (!std::filesystem::exists(file_path, ec)) {
    std::unique_lock lock(state_->metadata_mutex_);
    RemoveEntryFromIndexLocked(key_hash);
    return nullptr;
  }

  auto file_size = std::filesystem::file_size(file_path, ec);
  if (ec || file_size == 0) {
    std::unique_lock lock(state_->metadata_mutex_);
    RemoveEntryFromIndexLocked(key_hash);
    return nullptr;
  }

  std::vector<uint8_t> file_data(file_size);
  {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
      std::unique_lock lock(state_->metadata_mutex_);
      RemoveEntryFromIndexLocked(key_hash);
      return nullptr;
    }
    file.read(reinterpret_cast<char*>(file_data.data()), static_cast<std::streamsize>(file_size));
    if (file.fail()) {
      std::unique_lock lock(state_->metadata_mutex_);
      RemoveEntryFromIndexLocked(key_hash);
      return nullptr;
    }
  }

  cv::Mat decoded = ReadWithOpenImageIO(file_path);
  if (!decoded.empty()) {
    return std::make_unique<ImageBuffer>(std::move(decoded));
  }

  std::unique_lock lock(state_->metadata_mutex_);
  RemoveEntryFromIndexLocked(key_hash);
  return nullptr;
}

void ThumbnailDiskCacheService::EnqueueWrite(const ThumbnailDiskCacheKey& key, ImageBuffer buffer,
                                             ThumbnailCacheFormat format) {
  EnqueueWrite(key, std::make_shared<ImageBuffer>(std::move(buffer)), format);
}

void ThumbnailDiskCacheService::EnqueueWrite(const ThumbnailDiskCacheKey& key,
                                             std::shared_ptr<ImageBuffer> buffer,
                                             ThumbnailCacheFormat         format) {
  if (!state_->initialized_ || !state_->enabled_) {
    return;
  }
  if (key.project_uuid != state_->project_uuid_) {
    return;
  }

  if (!buffer || !buffer->cpu_data_valid_) {
    return;
  }

  const auto& mat = buffer->GetCPUData();
  if (mat.empty()) {
    return;
  }

  const auto key_hash         = MakeKeyHashString(key);
  const auto invalidation_key = MakeElementInvalidationKey(key.project_uuid, key.element_id);

  WriteTask  task;
  task.key      = key;
  task.key_hash = key_hash;
  task.format   = format;
  task.buffer   = std::move(buffer);
  {
    std::unique_lock lock(state_->metadata_mutex_);
    task.invalidation_generation = state_->invalidation_generations_[invalidation_key];
    task.clear_generation        = state_->clear_generation_;
  }
  state_->write_queue_.push(std::move(task));
}

void ThumbnailDiskCacheService::Invalidate(const std::string& project_uuid,
                                           sl_element_id_t    element_id) {
  if (!state_->initialized_) {
    return;
  }

  std::unique_lock lock(state_->metadata_mutex_);
  state_->invalidation_generations_[MakeElementInvalidationKey(project_uuid, element_id)]++;

  std::vector<std::string> keys_to_remove;
  for (const auto& [hash_str, meta] : state_->index_) {
    if (meta.key.project_uuid == project_uuid && meta.key.element_id == element_id) {
      keys_to_remove.push_back(hash_str);
    }
  }

  for (const auto& hash_str : keys_to_remove) {
    RemoveEntryFromIndexLocked(hash_str);
  }
}

auto ThumbnailDiskCacheService::GetStats() const -> Stats {
  Stats s;
  if (!state_) {
    return s;
  }

  s.cache_root_path = state_->cache_root_.string();
  s.enabled         = state_->enabled_;
  s.max_entries     = state_->max_entries_;

  if (!state_->initialized_) {
    return s;
  }

  std::unique_lock lock(state_->metadata_mutex_);
  s.total_entries    = state_->index_.size();
  s.total_size_bytes = state_->total_size_bytes_;
  s.hit_count        = state_->hit_count_.load();
  s.miss_count       = state_->miss_count_.load();
  return s;
}

// ── Phase 4: Configuration ────────────────────────────────────────────────

void ThumbnailDiskCacheService::SetEnabled(bool enabled) { state_->enabled_ = enabled; }

bool ThumbnailDiskCacheService::IsEnabled() const { return state_->enabled_; }

void ThumbnailDiskCacheService::SetCacheRoot(const std::filesystem::path& cache_root) {
  const auto next_root = cache_root.empty() ? GetDefaultCacheRoot() : cache_root;
  if (state_->cache_root_ == next_root) {
    return;
  }
  if (state_->initialized_) {
    ReopenWithCacheRoot(next_root);
    return;
  }
  state_->cache_root_ = next_root;
}

const std::filesystem::path& ThumbnailDiskCacheService::GetCacheRoot() const {
  return state_->cache_root_;
}

void ThumbnailDiskCacheService::SetMaxEntries(size_t max_entries) {
  state_->max_entries_ = max_entries;
  std::unique_lock lock(state_->metadata_mutex_);
  if (state_->index_.size() > max_entries) {
    EvictLruLocked(max_entries);
  } else {
    const auto capped = static_cast<uint32_t>(
        std::min(max_entries, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    state_->lru_index_.Resize(capped);
  }
}

size_t ThumbnailDiskCacheService::GetMaxEntries() const { return state_->max_entries_; }

void   ThumbnailDiskCacheService::SetJpegQuality(int quality) {
  state_->jpeg_quality_ = std::clamp(quality, 1, 100);
}

int  ThumbnailDiskCacheService::GetJpegQuality() const { return state_->jpeg_quality_; }

void ThumbnailDiskCacheService::SetWebPQuality(int quality) {
  state_->webp_quality_ = std::clamp(quality, 1, 100);
}

int  ThumbnailDiskCacheService::GetWebPQuality() const { return state_->webp_quality_; }

// ── Phase 4: Operations ───────────────────────────────────────────────────

void ThumbnailDiskCacheService::ClearAll() {
  if (!state_->initialized_) {
    std::error_code ec;
    std::filesystem::remove_all(state_->cache_root_, ec);
    return;
  }

  {
    std::unique_lock lock(state_->metadata_mutex_);
    BumpClearGenerationLocked();
    state_->index_.clear();
    state_->lru_index_.Flush();
    state_->total_size_bytes_ = 0;
  }

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(state_->cache_root_, ec)) {
    if (ec) break;
    const auto& entry_path = entry.path();
    if (entry_path.filename() == kGlobalMetadataFilename) continue;
    std::filesystem::remove_all(entry_path, ec);
  }

  FlushMetadata();
}

void ThumbnailDiskCacheService::ClearProject(const std::string& project_uuid) {
  if (!state_->initialized_) {
    return;
  }

  {
    std::unique_lock lock(state_->metadata_mutex_);
    BumpClearGenerationLocked();
    std::vector<std::string> keys_to_remove;
    for (const auto& [hash_str, meta] : state_->index_) {
      if (meta.key.project_uuid == project_uuid) {
        keys_to_remove.push_back(hash_str);
      }
    }
    for (const auto& hash_str : keys_to_remove) {
      RemoveEntryFromIndexLocked(hash_str);
    }
  }

  if (project_uuid == state_->project_uuid_) {
    std::error_code ec;
    std::filesystem::remove_all(state_->project_cache_dir_, ec);
  } else {
    std::error_code ec;
    std::filesystem::remove_all(state_->cache_root_ / project_uuid, ec);
  }

  FlushMetadata();
}

void ThumbnailDiskCacheService::WriterThreadLoop() {
  while (true) {
    auto task = state_->write_queue_.pop_r();

    if (!task.buffer) {
      break;
    }

    if (!state_->enabled_) {
      continue;
    }

    const auto invalidation_key =
        MakeElementInvalidationKey(task.key.project_uuid, task.key.element_id);
    {
      std::unique_lock lock(state_->metadata_mutex_);
      const auto       current_generation = state_->invalidation_generations_[invalidation_key];
      if (current_generation != task.invalidation_generation ||
          state_->clear_generation_ != task.clear_generation) {
        continue;
      }
    }

    if (!task.buffer->cpu_data_valid_) {
      continue;
    }

    const auto& mat = task.buffer->GetCPUData();
    if (mat.empty()) {
      continue;
    }

    std::vector<uint8_t> encoded;
    std::vector<int>     params;
    ThumbnailCacheFormat effective_format = task.format;
    const int            jpeg_q           = state_->jpeg_quality_;
    const int            webp_q           = state_->webp_quality_;
    if (task.format == ThumbnailCacheFormat::kJpeg) {
      params = {cv::IMWRITE_JPEG_QUALITY, jpeg_q};
    } else if (task.format == ThumbnailCacheFormat::kWebP) {
      params = {cv::IMWRITE_WEBP_QUALITY, webp_q};
    }

    auto            file_path = DeriveFilePath(task.key_hash, effective_format);
    std::error_code ec;
    std::filesystem::create_directories(file_path.parent_path(), ec);
    if (ec) {
      continue;
    }

    bool   write_ok        = false;
    size_t file_size_bytes = 0;
    if (effective_format == ThumbnailCacheFormat::kJpeg ||
        effective_format == ThumbnailCacheFormat::kWebP) {
      const int quality = effective_format == ThumbnailCacheFormat::kJpeg ? jpeg_q : webp_q;
      write_ok          = WriteWithOpenImageIO(file_path, mat, effective_format, quality);
      if (write_ok) {
        file_size_bytes = static_cast<size_t>(std::filesystem::file_size(file_path, ec));
        if (ec || file_size_bytes == 0) {
          std::filesystem::remove(file_path, ec);
          write_ok = false;
        }
      }
    }

    if (!write_ok && effective_format != ThumbnailCacheFormat::kBmp) {
      effective_format = ThumbnailCacheFormat::kBmp;
      file_path        = DeriveFilePath(task.key_hash, effective_format);
      std::filesystem::create_directories(file_path.parent_path(), ec);
      if (ec) {
        continue;
      }
      params.clear();
      cv::Mat bgr8 = PrepareForOpenCvEncoding(mat);
      if (bgr8.empty()) {
        continue;
      }
      try {
        write_ok =
            cv::imencode(FormatFileExtension(ThumbnailCacheFormat::kBmp), bgr8, encoded, params);
      } catch (const cv::Exception&) {
        write_ok = false;
      } catch (...) {
        write_ok = false;
      }

      if (write_ok) {
        {
          std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
          if (!file) {
            continue;
          }
          file.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
        }
        file_size_bytes = encoded.size();
      }
    }

    if (!write_ok || file_size_bytes == 0) {
      continue;
    }

    {
      std::unique_lock lock(state_->metadata_mutex_);
      const auto       current_generation = state_->invalidation_generations_[invalidation_key];
      if (current_generation != task.invalidation_generation ||
          state_->clear_generation_ != task.clear_generation) {
        std::filesystem::remove(file_path, ec);
        continue;
      }

      auto it = state_->index_.find(task.key_hash);
      if (it != state_->index_.end()) {
        state_->total_size_bytes_ -= it->second.file_size_bytes;
      }

      EntryMeta meta;
      meta.key                      = task.key;
      meta.file_size_bytes          = file_size_bytes;
      meta.file_path                = file_path;
      meta.last_access_time         = CurrentTimeSeconds();
      state_->index_[task.key_hash] = meta;
      RecordLruAccessLocked(task.key_hash);
      state_->total_size_bytes_ += file_size_bytes;

      if (state_->index_.size() > state_->max_entries_) {
        EvictLruLocked(state_->max_entries_);
      }
    }

    task.buffer.reset();

    FlushMetadata();
  }
}

void ThumbnailDiskCacheService::FlushMetadata() {
  auto make_entry_json = [](const std::string& hash_str, const EntryMeta& meta) {
    nlohmann::json entry;
    entry["key_hash"]             = hash_str;
    entry["project_uuid"]         = meta.key.project_uuid;
    entry["element_id"]           = meta.key.element_id;
    entry["resolution"]           = static_cast<uint32_t>(meta.key.resolution);
    entry["purpose"]              = static_cast<uint32_t>(meta.key.purpose);
    entry["edit_version_hash"]    = meta.key.edit_version_hash;
    entry["cache_schema_version"] = meta.key.cache_schema_version;
    entry["file_size_bytes"]      = meta.file_size_bytes;
    entry["file_path"]            = meta.file_path.string();
    entry["last_access_time"]     = meta.last_access_time;
    return entry;
  };

  // ── Per-project metadata ─────────────────────────────────────────────
  if (!state_->project_uuid_.empty()) {
    nlohmann::json j;
    j["cache_schema_version"] = kCacheSchemaVersion;
    j["project_uuid"]         = state_->project_uuid_;
    j["total_size_bytes"]     = state_->total_size_bytes_;

    auto& entries_json        = j["entries"];
    entries_json              = nlohmann::json::array();

    {
      std::unique_lock lock(state_->metadata_mutex_);
      for (const auto& hash_str : state_->lru_index_.GetLRUKeys()) {
        auto it = state_->index_.find(hash_str);
        if (it == state_->index_.end()) continue;
        const auto& meta = it->second;
        if (meta.key.project_uuid != state_->project_uuid_) continue;
        entries_json.push_back(make_entry_json(hash_str, meta));
      }
    }

    std::error_code ec;
    std::filesystem::create_directories(state_->metadata_file_path_.parent_path(), ec);
    if (!ec) {
      const auto tmp_path = state_->metadata_file_path_.string() + ".tmp";
      {
        std::ofstream file(tmp_path, std::ios::trunc);
        if (file) {
          file << j.dump(2);
        }
      }
      // Windows rename fails when the destination already exists; replace atomically.
      std::filesystem::remove(state_->metadata_file_path_, ec);
      std::filesystem::rename(tmp_path, state_->metadata_file_path_, ec);
    }
  }

  // ── Global metadata ──────────────────────────────────────────────────
  {
    nlohmann::json global;
    global["cache_schema_version"] = kCacheSchemaVersion;
    global["total_entries"]        = state_->index_.size();
    global["total_size_bytes"]     = state_->total_size_bytes_;
    global["max_entries"]          = state_->max_entries_;
    global["enabled"]              = state_->enabled_;

    auto& entries_json             = global["entries"];
    entries_json                   = nlohmann::json::array();

    {
      std::unique_lock lock(state_->metadata_mutex_);
      for (const auto& hash_str : state_->lru_index_.GetLRUKeys()) {
        auto it = state_->index_.find(hash_str);
        if (it == state_->index_.end()) continue;
        entries_json.push_back(make_entry_json(hash_str, it->second));
      }
    }

    std::error_code ec;
    std::filesystem::create_directories(state_->global_metadata_path_.parent_path(), ec);
    if (!ec) {
      const auto tmp_path = state_->global_metadata_path_.string() + ".tmp";
      {
        std::ofstream file(tmp_path, std::ios::trunc);
        if (file) {
          file << global.dump(2);
        }
      }
      std::filesystem::remove(state_->global_metadata_path_, ec);
      std::filesystem::rename(tmp_path, state_->global_metadata_path_, ec);
    }
  }
}

void ThumbnailDiskCacheService::LoadGlobalMetadata() {
  std::error_code ec;
  if (!std::filesystem::exists(state_->global_metadata_path_, ec)) {
    return;
  }

  std::ifstream file(state_->global_metadata_path_);
  if (!file) {
    return;
  }

  nlohmann::json j;
  try {
    file >> j;
  } catch (...) {
    RebuildFromDirectoryScan();
    return;
  }

  if (!j.contains("entries") || !j["entries"].is_array()) {
    RebuildFromDirectoryScan();
    return;
  }

  // Load all entries from the global index. Per-project metadata is still
  // loaded afterwards to refresh current-project details when available.
  std::unique_lock lock(state_->metadata_mutex_);

  size_t           loaded_count = 0;
  for (const auto& entry : j["entries"]) {
    try {
      const auto file_path_str = entry.value("file_path", std::string{});
      if (file_path_str.empty()) continue;

      const auto            project_uuid = entry.value("project_uuid", std::string{});

      std::filesystem::path file_path(file_path_str);
      if (!std::filesystem::exists(file_path, ec)) continue;

      EntryMeta meta;
      meta.file_path        = file_path;
      meta.file_size_bytes  = entry.value("file_size_bytes", size_t{0});
      meta.last_access_time = entry.value("last_access_time", int64_t{0});
      meta.key.project_uuid = project_uuid;
      meta.key.element_id   = entry.value("element_id", sl_element_id_t{0});
      meta.key.resolution   = static_cast<ThumbnailResolution>(
          entry.value("resolution", static_cast<uint32_t>(ThumbnailResolution::k1024)));
      meta.key.purpose = static_cast<ThumbnailDiskCachePurpose>(
          entry.value("purpose", static_cast<uint32_t>(ThumbnailDiskCachePurpose::kThumbnail)));
      meta.key.edit_version_hash    = entry.value("edit_version_hash", std::string{});
      meta.key.cache_schema_version = entry.value("cache_schema_version", uint32_t{0});

      const auto key_hash           = entry.value("key_hash", std::string{});
      if (key_hash.empty()) continue;

      if (auto old_it = state_->index_.find(key_hash); old_it != state_->index_.end()) {
        state_->total_size_bytes_ -= old_it->second.file_size_bytes;
      }
      state_->index_[key_hash] = meta;
      RecordLruAccessLocked(key_hash);
      state_->total_size_bytes_ += meta.file_size_bytes;
      loaded_count++;
    } catch (...) {
      continue;
    }
  }

  // If global metadata says there should be entries but we loaded none for
  // other projects, it might still be valid (only current project has entries).
  // But if the global metadata has bad structure, rebuild.
  if (j.value("total_entries", size_t{0}) > 0 && loaded_count == 0 &&
      !j.contains("cache_schema_version")) {
    // Looks corrupt — rebuild from disk.
    lock.unlock();
    RebuildFromDirectoryScan();
  }
}

void ThumbnailDiskCacheService::LoadMetadata() {
  std::error_code ec;
  if (!std::filesystem::exists(state_->metadata_file_path_, ec)) {
    return;
  }

  std::ifstream file(state_->metadata_file_path_);
  if (!file) {
    return;
  }

  nlohmann::json j;
  try {
    file >> j;
  } catch (...) {
    return;
  }

  if (!j.contains("entries") || !j["entries"].is_array()) {
    return;
  }

  std::unique_lock lock(state_->metadata_mutex_);

  const auto       current_project = state_->project_uuid_;
  for (const auto& entry : j["entries"]) {
    try {
      EntryMeta  meta;

      const auto file_path_str = entry.value("file_path", std::string{});
      if (file_path_str.empty()) {
        continue;
      }
      meta.file_path        = file_path_str;
      meta.file_size_bytes  = entry.value("file_size_bytes", size_t{0});
      meta.last_access_time = entry.value("last_access_time", int64_t{0});
      meta.key.project_uuid = entry.value("project_uuid", std::string{});
      meta.key.element_id   = entry.value("element_id", sl_element_id_t{0});
      meta.key.resolution   = static_cast<ThumbnailResolution>(
          entry.value("resolution", static_cast<uint32_t>(ThumbnailResolution::k1024)));
      meta.key.purpose = static_cast<ThumbnailDiskCachePurpose>(
          entry.value("purpose", static_cast<uint32_t>(ThumbnailDiskCachePurpose::kThumbnail)));
      meta.key.edit_version_hash    = entry.value("edit_version_hash", std::string{});
      meta.key.cache_schema_version = entry.value("cache_schema_version", uint32_t{0});

      const auto key_hash           = entry.value("key_hash", std::string{});
      if (key_hash.empty()) {
        continue;
      }

      if (std::filesystem::exists(meta.file_path, ec)) {
        // Only load entries for the current project from per-project metadata
        if (meta.key.project_uuid == current_project) {
          if (auto old_it = state_->index_.find(key_hash); old_it != state_->index_.end()) {
            state_->total_size_bytes_ -= old_it->second.file_size_bytes;
          }
          state_->index_[key_hash] = meta;
          RecordLruAccessLocked(key_hash);
          state_->total_size_bytes_ += meta.file_size_bytes;
        }
      }
    } catch (...) {
      continue;
    }
  }
}

void ThumbnailDiskCacheService::RecordLruAccessLocked(const std::string& key_hash) {
  auto evicted_key = state_->lru_index_.RecordAccess_WithEvict(key_hash, key_hash);
  if (!evicted_key.has_value() || evicted_key.value() == key_hash) {
    return;
  }

  auto it = state_->index_.find(evicted_key.value());
  if (it == state_->index_.end()) {
    return;
  }

  state_->total_size_bytes_ -= it->second.file_size_bytes;
  std::error_code ec;
  std::filesystem::remove(it->second.file_path, ec);
  state_->index_.erase(it);
}

void ThumbnailDiskCacheService::RemoveEntryFromIndexLocked(const std::string& key_hash) {
  auto it = state_->index_.find(key_hash);
  if (it == state_->index_.end()) {
    state_->lru_index_.RemoveRecord(key_hash);
    return;
  }

  state_->total_size_bytes_ -= it->second.file_size_bytes;

  std::error_code ec;
  std::filesystem::remove(it->second.file_path, ec);

  state_->index_.erase(it);
  state_->lru_index_.RemoveRecord(key_hash);
}

void ThumbnailDiskCacheService::EvictLruLocked(size_t target_count) {
  if (state_->index_.size() <= target_count) {
    return;
  }

  const auto capped = static_cast<uint32_t>(
      std::min(target_count, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
  const auto evicted_keys = state_->lru_index_.Resize_WithEvict(capped);
  for (const auto& key_hash : evicted_keys) {
    auto it = state_->index_.find(key_hash);
    if (it == state_->index_.end()) {
      continue;
    }
    state_->total_size_bytes_ -= it->second.file_size_bytes;
    std::error_code ec;
    std::filesystem::remove(it->second.file_path, ec);
    state_->index_.erase(it);
  }
}

void ThumbnailDiskCacheService::RebuildFromDirectoryScan() {
  std::error_code ec;
  if (!std::filesystem::exists(state_->cache_root_, ec)) {
    return;
  }

  std::unique_lock lock(state_->metadata_mutex_);
  state_->index_.clear();
  state_->lru_index_.Flush();
  state_->total_size_bytes_ = 0;

  for (const auto& project_entry : std::filesystem::directory_iterator(state_->cache_root_, ec)) {
    if (ec) break;
    if (!project_entry.is_directory()) continue;

    const auto project_metadata = project_entry.path() / "cache_metadata.json";
    if (!std::filesystem::exists(project_metadata, ec)) continue;

    std::ifstream file(project_metadata);
    if (!file) continue;

    nlohmann::json j;
    try {
      file >> j;
    } catch (...) {
      continue;
    }

    if (!j.contains("entries") || !j["entries"].is_array()) continue;

    for (const auto& entry : j["entries"]) {
      try {
        const auto file_path_str = entry.value("file_path", std::string{});
        if (file_path_str.empty()) continue;

        std::filesystem::path file_path(file_path_str);
        if (!std::filesystem::exists(file_path, ec)) continue;

        EntryMeta meta;
        meta.file_path        = file_path;
        meta.file_size_bytes  = entry.value("file_size_bytes", size_t{0});
        meta.last_access_time = entry.value("last_access_time", int64_t{0});
        meta.key.project_uuid = entry.value("project_uuid", std::string{});
        meta.key.element_id   = entry.value("element_id", sl_element_id_t{0});
        meta.key.resolution   = static_cast<ThumbnailResolution>(
            entry.value("resolution", static_cast<uint32_t>(ThumbnailResolution::k1024)));
        meta.key.purpose = static_cast<ThumbnailDiskCachePurpose>(
            entry.value("purpose", static_cast<uint32_t>(ThumbnailDiskCachePurpose::kThumbnail)));
        meta.key.edit_version_hash    = entry.value("edit_version_hash", std::string{});
        meta.key.cache_schema_version = entry.value("cache_schema_version", uint32_t{0});

        const auto key_hash           = entry.value("key_hash", std::string{});
        if (key_hash.empty()) continue;

        state_->index_[key_hash] = meta;
        RecordLruAccessLocked(key_hash);
        state_->total_size_bytes_ += meta.file_size_bytes;
      } catch (...) {
        continue;
      }
    }
  }

  // Re-write global metadata with rebuilt state.
  lock.unlock();
  FlushMetadata();
}

void ThumbnailDiskCacheService::ReopenWithCacheRoot(const std::filesystem::path& cache_root) {
  const std::string project_uuid = state_->project_uuid_;
  Shutdown();
  state_->cache_root_ = cache_root;
  if (!project_uuid.empty()) {
    Initialize(project_uuid);
  }
}

void ThumbnailDiskCacheService::BumpClearGenerationLocked() { ++state_->clear_generation_; }

}  // namespace alcedo
