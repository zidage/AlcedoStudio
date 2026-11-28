//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/import_service.hpp"

#include <QScopeGuard>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/pipeline_service.hpp"
#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "sleeve/storage.hpp"
#include "type/source_file_record.hpp"
#include "utils/logging/log.hpp"

namespace alcedo {
namespace {

auto IsRootImportDestination(const image_path_t& dest) -> bool {
  const auto normalized = dest.lexically_normal();
  return normalized.empty() || normalized == image_path_t{L"/"} || normalized == image_path_t{L"."};
}

/// Encode the immutable history root of a newly imported image on a private default document.
/// A RAW image gets the default document with @p raw_import_look. A raster image gets the raster
/// default document, which holds its color description in the Develop `input` object and has no
/// RAW color context. No executor: nothing renders the document before its root exists.
auto EncodeImportedImageRoot(const PipelineMgmtService& pipeline_service,
                             sl_element_id_t element_id, const std::shared_ptr<Image>& image,
                             const RawImportLook& raw_import_look) -> EncodedImageRoot {
  if (image && image->HasRasterColorDescription()) {
    return pipeline_service.EncodeImageRoot(
        element_id, CreateDefaultRasterPipelineDocument(image->GetRasterColorDescription()),
        nullptr);
  }
  const RawRuntimeColorContext* ctx_ptr =
      image && image->HasRawColorContext() ? &image->GetRawColorContext() : nullptr;
  return pipeline_service.EncodeImageRoot(
      element_id, CreateDefaultPipelineDocument(raw_import_look), ctx_ptr);
}

/// Number of encoded roots committed in one storage transaction. A commit flushes the DuckDB
/// write-ahead log, so one transaction per image made the database lock the import bottleneck.
constexpr std::size_t kImportRootWriteBatchSize = 64;

/// The placeholder loop reports progress after this many placeholders, so the caller can show how
/// far the preparation of a large import has come without one report per file.
constexpr uint32_t kPlaceholderProgressInterval = 256;

/// Collects the encoded roots of one import job and commits them in batches.
///
/// Workers encode roots in parallel and Add them here. The worker whose Add fills a batch
/// writes it. The submitter writes the remainder after the workers finish. An image counts
/// as imported only after its root commits.
class ImportRootBatchWriter {
 public:
  struct PendingRoot {
    image_id_t       image_id_      = 0;
    /// The file has a RAW extension; it decides the progress ring the image counts in.
    bool             raw_extension_ = false;
    EncodedImageRoot root_;
    std::optional<SourceFileRecord> source_record;
  };

  /// Called once per submitted metadata task before it is queued.
  void AddOutstandingEncode() {
    std::lock_guard lock(mutex_);
    ++outstanding_encodes_;
  }

  /// Queue @p root. Returns a full batch for the caller to write, or an empty vector.
  auto Add(image_id_t image_id, bool raw_extension, EncodedImageRoot root,
           std::optional<SourceFileRecord> source) -> std::vector<PendingRoot> {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(PendingRoot{image_id, raw_extension, std::move(root), std::move(source)});
    if (pending_.size() < kImportRootWriteBatchSize) {
      return {};
    }
    return TakePendingLocked();
  }

  void FinishEncode() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (--outstanding_encodes_ == 0) {
      encodes_finished_.notify_one();
    }
  }

  /// Wait for submitted tasks to finish, then give the submitter the partial batch to write.
  auto Drain() -> std::vector<PendingRoot> {
    std::unique_lock lock(mutex_);
    encodes_finished_.wait(lock, [this] { return outstanding_encodes_ == 0; });
    return TakePendingLocked();
  }

 private:
  auto TakePendingLocked() -> std::vector<PendingRoot> {
    std::vector<PendingRoot> batch;
    batch.swap(pending_);
    return batch;
  }

  std::mutex               mutex_;
  std::condition_variable  encodes_finished_;
  std::vector<PendingRoot> pending_;
  uint32_t                 outstanding_encodes_ = 0;
};

auto IsRawExtension(const image_path_t& path) -> bool {
  return CategoryForPath(path) == ImportFileCategory::Raw;
}

/// Count one failed file, and the RAW subset when the file has a RAW extension. failed_ first:
/// a reader that loads the subset before failed_ never sees more subset than failed files.
void CountFailedFile(ImportProgress& progress, bool raw_extension) {
  progress.failed_.fetch_add(1);
  if (raw_extension) {
    progress.raw_failed_.fetch_add(1);
  }
}

void ReportImportProgress(const std::shared_ptr<ImportJob>& job, const ImportProgress& progress) {
  if (!job || !job->on_progress_) {
    return;
  }
  try {
    job->on_progress_(progress);
  } catch (const std::exception& error) {
    ALCEDO_LOG_WARN(Library) << "Import progress callback failed: " << error.what();
  } catch (...) {
    ALCEDO_LOG_WARN(Library) << "Import progress callback failed with an unknown error";
  }
}

}  // namespace

static void SetImportResult(const std::shared_ptr<ImportJob>& job, const ImportResult& result) {
  if (job && job->on_finished_ && !job->cancelation_acked_.exchange(true)) {
    job->on_finished_(result);
  }
}

static void TryFinishImportJob(const std::shared_ptr<ImportJob>&      job,
                               const std::shared_ptr<ImportProgress>& progress) {
  if (!job || !progress || !job->submission_closed_.load()) {
    return;
  }
  if (job->metadata_tasks_finished_.load() != job->metadata_tasks_submitted_.load()) {
    return;
  }
  ImportResult result;
  result.requested_     = progress->total_;
  result.imported_      = progress->metadata_done_.load();
  result.failed_        = progress->failed_.load();
  result.unsupported_   = progress->unsupported_.load();
  result.excluded_type_ = progress->excluded_type_.load();
  result.canceled_      = progress->canceled_.load();
  result.raw_requested_ = progress->raw_total_;
  result.raw_imported_  = progress->raw_imported_.load();
  result.raw_failed_    = progress->raw_failed_.load();
  result.duplicates_skipped     = progress->duplicates_skipped.load();
  result.raw_duplicates_skipped = progress->raw_duplicates_skipped.load();
  SetImportResult(job, result);
}

/// Commit one batch of encoded roots, then count each image as imported or failed.
static void WriteImportRootBatch(std::vector<ImportRootBatchWriter::PendingRoot> batch,
                                 PipelineMgmtService&                            pipeline_service,
                                 const std::shared_ptr<ImportJob>&               job,
                                 const std::shared_ptr<ImportLog>&               import_log,
                                 const std::shared_ptr<ImportProgress>&          progress,
                                 OrdinaryImportSources*                          sources) {
  if (batch.empty()) {
    return;
  }
  std::vector<EncodedImageRoot> roots;
  roots.reserve(batch.size());
  for (auto& pending : batch) {
    roots.push_back(std::move(pending.root_));
  }

  std::string error;
  bool        written = false;
  try {
    pipeline_service.WriteImageRoots(std::move(roots));
    written = true;
  } catch (const std::exception& e) {
    error = e.what();
  } catch (...) {
    error = "unknown storage error";
  }

  for (auto& pending : batch) {
    if (written) {
      if (pending.source_record) {
        sources->RecordSource(std::move(*pending.source_record));
      }
      if (import_log) {
        import_log->MarkMetadataSuccess(pending.image_id_);
      }
      progress->metadata_done_.fetch_add(1);
      if (pending.raw_extension_) {
        progress->raw_imported_.fetch_add(1);
      }
    } else {
      if (import_log) {
        import_log->MarkMetadataFailure(pending.image_id_, ImportErrorCode::DB_WRITE_FAILED, error);
      }
      CountFailedFile(*progress, pending.raw_extension_);
    }
  }
  if (job) {
    job->metadata_tasks_finished_.fetch_add(static_cast<uint32_t>(batch.size()));
  }
  ReportImportProgress(job, *progress);
}

auto ImportServiceImpl::ImportToFolder(const std::vector<image_path_t>& paths,
                                       const image_path_t& dest, const ImportOptions& options,
                                       std::shared_ptr<ImportJob> job)
    -> std::shared_ptr<ImportJob> {
  // Refuse a mounted destination before reading any source.
  const auto folder_id = IsRootImportDestination(dest)
                             ? std::optional<sl_element_id_t>{0}
                             : fs_service_->CheckFolderStructureEditable(dest);
  if (!job) {
    job = std::make_shared<ImportJob>();
  }
  auto sources =
      folder_id ? std::make_shared<OrdinaryImportSources>(*fs_service_->GetStorage()) : nullptr;
  StartImport(
      paths,
      [folder_id](FileSystem&        fs, std::size_t,
                  const file_name_t& file_name) -> std::shared_ptr<SleeveElement> {
        if (!folder_id) {
          return nullptr;
        }
        auto file = fs.CreateFileInLibrary(file_name);
        if (*folder_id != 0) {
          try {
            fs.LinkFileToFolder(file->element_id_, *folder_id);
          } catch (...) {
            fs.DeleteFileEverywhere(file->element_id_);
            throw;
          }
        }
        return file;
      },
      options, job, std::move(sources));
  return job;
}

auto ImportServiceImpl::ImportMountedFiles(std::span<const MountFileImportRequest> requests)
    -> std::vector<MountFileImportOutcome> {
  std::vector<MountFileImportOutcome> outcomes(requests.size());
  if (requests.empty()) {
    return outcomes;
  }
  std::vector<image_path_t> paths;
  paths.reserve(requests.size());
  for (const auto& request : requests) {
    paths.push_back(request.path);
  }
  std::vector<MountFileImportRequest> placements(requests.begin(), requests.end());

  auto                                job = std::make_shared<ImportJob>();
  std::promise<ImportResult>          finished;
  auto                                finished_future = finished.get_future();
  job->on_finished_ = [&finished](const ImportResult& result) { finished.set_value(result); };
  StartImport(
      paths,
      [placements = std::move(placements)](
          FileSystem& fs, std::size_t index,
          const file_name_t& file_name) -> std::shared_ptr<SleeveElement> {
        const auto& request = placements.at(index);
        auto        file    = fs.CreateFileInLibrary(file_name);
        file->mount_id_     = request.mount_id;
        try {
          fs.LinkFileToFolder(file->element_id_, request.home_folder_id);
        } catch (...) {
          fs.DeleteFileEverywhere(file->element_id_);
          throw;
        }
        return file;
      },
      ImportOptions{}, job);
  finished_future.wait();

  const auto snapshot = job->import_log_->Snapshot();
  SyncImports(snapshot, L"");

  // SyncImports removed the element of each failed file; each other placeholder is stored.
  std::unordered_map<image_path_t::string_type, std::string> failed;
  for (const auto& entry : snapshot.metadata_failed_) {
    failed.emplace(entry.source_path_.native(), entry.error_message_.empty()
                                                    ? std::string("metadata read failed")
                                                    : entry.error_message_);
  }
  std::unordered_map<image_path_t::string_type, sl_element_id_t> stored;
  for (const auto& entry : snapshot.created_) {
    if (!failed.contains(entry.source_path_.native())) {
      stored.emplace(entry.source_path_.native(), entry.element_id_);
    }
  }
  for (std::size_t index = 0; index < requests.size(); ++index) {
    const auto& key = requests[index].path.native();
    if (const auto it = stored.find(key); it != stored.end()) {
      outcomes[index].element_id = it->second;
    } else if (const auto failure = failed.find(key); failure != failed.end()) {
      outcomes[index].error = failure->second;
    } else {
      outcomes[index].error = "the file is not a regular file, or its element was not created";
    }
  }
  return outcomes;
}

void ImportServiceImpl::StartImport(const std::vector<image_path_t>& paths, FilePlacement placement,
                                    ImportOptions options, const std::shared_ptr<ImportJob>& job,
                                    std::shared_ptr<OrdinaryImportSources> sources) {
  auto import_log = std::make_shared<ImportLog>();
  if (job) {
    job->import_log_ = import_log;
  }
  std::shared_ptr<ImportProgress> progress_ptr = std::make_shared<ImportProgress>();
  progress_ptr->total_                         = static_cast<uint32_t>(paths.size());
  progress_ptr->raw_total_ =
      static_cast<uint32_t>(std::count_if(paths.begin(), paths.end(), IsRawExtension));
  if (job) {
    job->progress_ = progress_ptr;
  }

  if (paths.empty()) {
    // Immediately finish
    SetImportResult(job, ImportResult{});
    return;
  }

  // One look for the whole import: a settings change while the import runs reaches the next
  // import, so the images of one import share one root look. The metadata tasks share this
  // immutable value until the last of them finishes.
  auto raw_import_look = std::make_shared<const RawImportLook>(raw_import_look_source_->Read());
  submission_thread_.Submit([this, paths, placement = std::move(placement), job, import_log,
                             progress_ptr, options = std::move(options),
                             sources         = std::move(sources),
                             raw_import_look = std::move(raw_import_look)]() {
    CreatePlaceholdersAndSubmit(paths, placement, job, import_log, progress_ptr, options, sources,
                                raw_import_look);
  });
}

void ImportServiceImpl::CreatePlaceholdersAndSubmit(
    const std::vector<image_path_t>& paths, const FilePlacement& placement,
    const std::shared_ptr<ImportJob>& job, const std::shared_ptr<ImportLog>& import_log,
    const std::shared_ptr<ImportProgress>& progress_ptr, const ImportOptions& options,
    const std::shared_ptr<OrdinaryImportSources>& sources,
    const std::shared_ptr<const RawImportLook>&   raw_import_look) {
  using CandidateStatus      = OrdinaryImportSources::CandidateStatus;
  // TODO: Use sleeve service to interact with FS
  // The current implementation is a temporary solution
  auto       root_writer     = std::make_shared<ImportRootBatchWriter>();
  const auto report_progress = [&job, &progress_ptr] { ReportImportProgress(job, *progress_ptr); };
  if (sources && options.skip_duplicates) {
    sources->LoadCandidates();
  }

  // Files the loop took up: each is counted failed here or handed to a metadata task.
  uint32_t files_reached = 0;
  for (const auto& image_path : paths) {
    if (job && job->IsCancelled()) {
      break;
    }
    const std::size_t file_index = files_reached;
    ++files_reached;
    const bool      raw_extension = IsRawExtension(image_path);
    // Validate that the path is a regular file. File-type detection is deferred
    // to metadata extraction, which classifies the content; rejected files are
    // marked failed and SyncImports removes their element and Image.
    std::error_code file_ec;
    if (!std::filesystem::is_regular_file(image_path, file_ec) || file_ec) {
      CountFailedFile(*progress_ptr, raw_extension);
      report_progress();
      continue;
    }
    const std::wstring file_name = image_path.filename().wstring();

    auto               lookup    = sources ? sources->LookupPath(image_path) : std::nullopt;
    if (lookup && lookup->status == CandidateStatus::kSubmitted) {
      // Wait for the first occurrence's result. A failed or canceled occurrence cannot match.
      WriteImportRootBatch(root_writer->Drain(), *pipeline_service_, job, import_log, progress_ptr,
                           sources.get());
      if (job && job->IsCancelled()) {
        --files_reached;
        break;
      }
    }
    if (lookup && lookup->status != CandidateStatus::kNone) {
      if (auto measured = OrdinaryImportSources::Measure(image_path)) {
        std::optional<OrdinaryImportSources::Match> match;
        try {
          match = sources->FindMatch(lookup->key, *measured);
        } catch (const std::exception& error) {
          ALCEDO_LOG_WARN(Library)
              << "Import duplicate check for " << image_path << ": " << error.what();
        }
        if (match) {
          if (match->needs_source_record) {
            measured->element_id = match->element_id;
            sources->RecordSource(std::move(*measured));
          }
          progress_ptr->duplicates_skipped.fetch_add(1);
          if (raw_extension) {
            progress_ptr->raw_duplicates_skipped.fetch_add(1);
          }
          report_progress();
          continue;
        }
      }
    }

    // MetadataExtractor sets image_type_ from the file content. The image pool owns the new
    // Image until SyncImports stores it; the metadata task below holds it pinned.
    auto image_handler           = image_pool_service_->CreateAndReturnPinned([&](Image& image) {
      image.image_path_ = image_path;
      image.image_name_ = file_name;
    });
    if (!image_handler) {
      CountFailedFile(*progress_ptr, raw_extension);
      report_progress();
      continue;
    }
    const auto      image_id   = image_handler.Get()->image_id_;

    // The file element and its image id are created in one fs_lock_ scope, so a sync on another
    // thread writes the element with its image id (#1169). The element holds the id only, not
    // the Image (Sleeve Node Cache Plan, SN7): the node cache can then drop a clean file element.
    sl_element_id_t element_id = 0;
    try {
      element_id = fs_service_->Write_NoSync<sl_element_id_t>(
          [&placement, file_index, &file_name, image_id](FileSystem& fs) -> sl_element_id_t {
            const auto element = placement(fs, file_index, file_name);
            if (!element) {
              return 0;
            }
            std::static_pointer_cast<SleeveFile>(element)->image_id_ = image_id;
            return element->element_id_;
          });
    } catch (...) {
      element_id = 0;
    }
    if (element_id == 0) {
      // No element refers to the Image: the next SyncWithStorage drops it.
      image_pool_service_->Remove(image_id);
      CountFailedFile(*progress_ptr, raw_extension);
      report_progress();
      continue;
    }

    auto image_handler_ptr =
        std::make_shared<ImagePoolManager::PinnedImageHandle>(std::move(image_handler));
    const auto placeholders_created = progress_ptr->placeholders_created_.fetch_add(1) + 1;
    if (import_log) {
      import_log->AddPlaceholder(image_id, element_id, file_name, image_path);
    }

    if (job) {
      job->metadata_tasks_submitted_.fetch_add(1);
    }
    root_writer->AddOutstandingEncode();

    if (lookup) {
      sources->RecordSubmission(std::move(lookup->key), element_id);
    }
    const auto pipeline_service = pipeline_service_;
    const auto file_categories  = options.any_category_paths_.contains(image_path.native())
                                      ? kAllImportCategories
                                      : options.allowed_categories_;

    // Extract metadata and encode the history root on the pool; roots commit in batches.
    // The placeholder loop queues files much faster than the pool reads them, so a cancel
    // usually finds most tasks still queued: each one checks the job before reading its file.
    thread_pool_.Submit([image_handler_ptr, progress_ptr, job, import_log, element_id,
                         pipeline_service, root_writer, file_categories, raw_extension,
                         raw_import_look, sources]() {
      const auto finish    = qScopeGuard([&] { root_writer->FinishEncode(); });
      auto image_ptr = image_handler_ptr ? image_handler_ptr->Get() : nullptr;
      std::vector<ImportRootBatchWriter::PendingRoot> full_batch;
      bool                                            encoded     = false;
      bool                                            unsupported = false;
      bool                                            excluded    = false;
      bool                                            canceled    = false;
      if (image_ptr && job && job->IsCancelled()) {
        // Marked failed, so SyncImports removes the placeholder element and Image.
        canceled = true;
        if (import_log) {
          import_log->MarkMetadataFailure(image_ptr->image_id_, ImportErrorCode::CANCELED,
                                          "Import canceled before the file was read");
        }
      } else if (image_ptr) {
        try {
          MetadataExtractor::ExtractEXIF_ToImage(image_ptr->image_path_, *image_ptr,
                                                 file_categories);
          auto source = sources ? OrdinaryImportSources::Measure(image_ptr->image_path_, element_id)
                                : std::nullopt;
          full_batch  = root_writer->Add(
              image_ptr->image_id_, raw_extension,
              EncodeImportedImageRoot(*pipeline_service, element_id, image_ptr, *raw_import_look),
              std::move(source));
          encoded = true;
        } catch (const MetadataExtractionError& e) {
          unsupported = e.code() == ImportErrorCode::UNSUPPORTED_FORMAT;
          excluded    = e.code() == ImportErrorCode::EXCLUDED_TYPE;
          if (import_log) {
            import_log->MarkMetadataFailure(image_ptr->image_id_, e.code(), e.message());
          }
        } catch (const std::exception& e) {
          if (import_log) {
            import_log->MarkMetadataFailure(image_ptr->image_id_,
                                            ImportErrorCode::METADATA_EXTRACTION_FAILED, e.what());
          }
        } catch (...) {
          if (import_log) {
            import_log->MarkMetadataFailure(image_ptr->image_id_,
                                            ImportErrorCode::METADATA_EXTRACTION_FAILED);
          }
        }
      }

      if (!encoded) {
        // failed_ first: unsupported_, excluded_type_ and canceled_ are subsets, so a reader
        // that loads them before failed_ never sees more of them than failed files.
        CountFailedFile(*progress_ptr, raw_extension);
        if (unsupported) {
          progress_ptr->unsupported_.fetch_add(1);
        }
        if (excluded) {
          progress_ptr->excluded_type_.fetch_add(1);
        }
        if (canceled) {
          progress_ptr->canceled_.fetch_add(1);
        }
        if (job) {
          job->metadata_tasks_finished_.fetch_add(1);
        }
        ReportImportProgress(job, *progress_ptr);
      }
      WriteImportRootBatch(std::move(full_batch), *pipeline_service, job, import_log, progress_ptr,
                           sources.get());
    });

    if (placeholders_created % kPlaceholderProgressInterval == 0) {
      report_progress();
    }
  }
  report_progress();

  if (job) {
    // Count the files the loop never reached from its own position: canceled metadata tasks
    // update the shared counters while this runs, so a sum of them can count a file twice.
    if (files_reached < progress_ptr->total_) {
      const auto unreached     = progress_ptr->total_ - files_reached;
      const auto raw_unreached = static_cast<uint32_t>(
          std::count_if(paths.begin() + files_reached, paths.end(), IsRawExtension));
      progress_ptr->failed_.fetch_add(unreached);
      progress_ptr->raw_failed_.fetch_add(raw_unreached);
      progress_ptr->canceled_.fetch_add(unreached);
      report_progress();
    }
    job->submission_closed_.store(true);
  }
  WriteImportRootBatch(root_writer->Drain(), *pipeline_service_, job, import_log, progress_ptr,
                       sources.get());
  if (sources) {
    import_log->PublishSourceRecords(sources->TakeSourceRecords());
  }
  TryFinishImportJob(job, progress_ptr);
}

void ImportServiceImpl::SyncImports(const ImportLogSnapshot& log_snapshot,
                                    const image_path_t&      dest) {
  (void)dest;
  if (!image_pool_service_) {
    return;
  }

  // A failed import leaves no trace: its element is deleted and its placeholder Image is
  // marked deleted in the pool, so the sync below never writes it as an Image row. The elements
  // are deleted in one batch, because a folder import can reject most of its files.
  std::vector<sl_element_id_t> failed_element_ids;
  std::vector<image_id_t>      failed_image_ids;
  failed_element_ids.reserve(log_snapshot.metadata_failed_.size());
  failed_image_ids.reserve(log_snapshot.metadata_failed_.size());
  for (const auto& entry : log_snapshot.metadata_failed_) {
    if (entry.element_id_ != 0) {
      failed_element_ids.push_back(entry.element_id_);
    }
    failed_image_ids.push_back(entry.image_id_);
  }
  if (!failed_element_ids.empty()) {
    fs_service_->Write_NoSync<void>(
        [&failed_element_ids](FileSystem& fs) { fs.DeleteFilesEverywhere(failed_element_ids); });
  }
  image_pool_service_->RemoveBatch(failed_image_ids);
  const auto images = image_pool_service_->SyncWithStorage();
  const auto files  = fs_service_->Sync();
  if (!images.failed_images_.empty()) {
    throw std::runtime_error(images.failed_images_.front().message_);
  }
  if (!files.success_) {
    throw std::runtime_error(files.message_);
  }
  if (log_snapshot.source_records) {
    fs_service_->GetStorage()->GetSourceFileStore().UpsertSourceFiles(*log_snapshot.source_records);
  }
}
};  // namespace alcedo
