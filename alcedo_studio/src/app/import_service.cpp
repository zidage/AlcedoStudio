//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/import_service.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "app/pipeline_service.hpp"
#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_filesystem.hpp"

namespace alcedo {
namespace {

auto IsRootImportDestination(const image_path_t& dest) -> bool {
  const auto normalized = dest.lexically_normal();
  return normalized.empty() || normalized == image_path_t{L"/"} || normalized == image_path_t{L"."};
}

/// Encode the immutable history root of a newly imported image on a private default document.
/// A raster image gets the raster default document, which holds its color description in the
/// Develop `input` object and has no RAW color context. No executor: nothing renders the
/// document before its root exists.
auto EncodeImportedImageRoot(const PipelineMgmtService& pipeline_service,
                             sl_element_id_t element_id, const std::shared_ptr<Image>& image)
    -> EncodedImageRoot {
  if (image && image->HasRasterColorDescription()) {
    return pipeline_service.EncodeImageRoot(
        element_id, CreateDefaultRasterPipelineDocument(image->GetRasterColorDescription()),
        nullptr);
  }
  const RawRuntimeColorContext* ctx_ptr =
      image && image->HasRawColorContext() ? &image->GetRawColorContext() : nullptr;
  return pipeline_service.EncodeImageRoot(element_id, CreateDefaultPipelineDocument(), ctx_ptr);
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
/// writes it; the last outstanding encode after submission closes writes the remainder. An
/// image counts as imported only after its root commits.
class ImportRootBatchWriter {
 public:
  struct PendingRoot {
    image_id_t       image_id_      = 0;
    /// The file has a RAW extension; it decides the progress ring the image counts in.
    bool             raw_extension_ = false;
    EncodedImageRoot root_;
  };

  /// Called once per submitted metadata task before it is queued.
  void AddOutstandingEncode() { outstanding_encodes_.fetch_add(1); }

  /// Queue @p root. Returns a full batch for the caller to write, or an empty vector.
  auto Add(image_id_t image_id, bool raw_extension, EncodedImageRoot root)
      -> std::vector<PendingRoot> {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(PendingRoot{image_id, raw_extension, std::move(root)});
    if (pending_.size() < kImportRootWriteBatchSize) {
      return {};
    }
    return TakePendingLocked();
  }

  /// Record that one metadata task finished encoding (successfully or not). Returns true when
  /// submission is closed and no encode is outstanding, so the caller must write the remainder.
  auto FinishEncode(const std::shared_ptr<ImportJob>& job) -> bool {
    return outstanding_encodes_.fetch_sub(1) == 1 && IsSubmissionClosed(job);
  }

  /// Returns true when the submitter must write the remainder after closing submission.
  auto NoOutstandingEncode() const -> bool { return outstanding_encodes_.load() == 0; }

  auto TakePending() -> std::vector<PendingRoot> {
    std::lock_guard<std::mutex> lock(mutex_);
    return TakePendingLocked();
  }

 private:
  static auto IsSubmissionClosed(const std::shared_ptr<ImportJob>& job) -> bool {
    return !job || job->submission_closed_.load();
  }

  auto TakePendingLocked() -> std::vector<PendingRoot> {
    std::vector<PendingRoot> batch;
    batch.swap(pending_);
    return batch;
  }

  std::mutex               mutex_;
  std::vector<PendingRoot> pending_;
  std::atomic<uint32_t>    outstanding_encodes_{0};
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
  SetImportResult(job, result);
}

/// Commit one batch of encoded roots, then count each image as imported or failed.
static void WriteImportRootBatch(std::vector<ImportRootBatchWriter::PendingRoot> batch,
                                 PipelineMgmtService&                            pipeline_service,
                                 const std::shared_ptr<ImportJob>&               job,
                                 const std::shared_ptr<ImportLog>&               import_log,
                                 const std::shared_ptr<ImportProgress>&          progress) {
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

  for (const auto& pending : batch) {
    if (written) {
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
  if (job && job->on_progress_) {
    job->on_progress_(*progress);
  }
  if (job) {
    job->metadata_tasks_finished_.fetch_add(static_cast<uint32_t>(batch.size()));
  }
  TryFinishImportJob(job, progress);
}

auto ImportServiceImpl::ImportToFolder(const std::vector<image_path_t>& paths,
                                       const image_path_t& dest, const ImportOptions& options,
                                       std::shared_ptr<ImportJob> job)
    -> std::shared_ptr<ImportJob> {
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
    return job;
  }

  submission_thread_.Submit([this, paths, dest, job, import_log, progress_ptr,
                             allowed   = options.allowed_categories_,
                             any_paths = options.any_category_paths_]() {
    CreatePlaceholdersAndSubmit(paths, dest, job, import_log, progress_ptr, allowed, any_paths);
  });
  return job;
}

void ImportServiceImpl::CreatePlaceholdersAndSubmit(
    const std::vector<image_path_t>& paths, const image_path_t& dest,
    const std::shared_ptr<ImportJob>& job, const std::shared_ptr<ImportLog>& import_log,
    const std::shared_ptr<ImportProgress>& progress_ptr, ImportCategoryMask allowed_categories,
    const ImportPathSet& any_category_paths) {
  // TODO: Use sleeve service to interact with FS
  // The current implementation is a temporary solution
  auto       root_writer     = std::make_shared<ImportRootBatchWriter>();
  const auto report_progress = [&job, &progress_ptr]() {
    if (job && job->on_progress_) {
      job->on_progress_(*progress_ptr);
    }
  };

  // Files the loop took up: each is counted failed here or handed to a metadata task.
  uint32_t files_reached = 0;
  for (const auto& image_path : paths) {
    if (job && job->IsCancelled()) {
      break;
    }
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

    std::shared_ptr<SleeveElement> element = nullptr;
    try {
      element = fs_service_->Write_NoSync<std::shared_ptr<SleeveElement>>(
          [&dest, &file_name](FileSystem& fs) {
            std::shared_ptr<SleeveElement> target;
            if (!IsRootImportDestination(dest)) {
              target = fs.Get(dest, false);
              if (!target || target->type_ != ElementType::FOLDER) {
                throw std::runtime_error("ImportService: import target is not a folder");
              }
            }
            auto file = fs.CreateFileInLibrary(file_name);
            if (target) {
              fs.LinkFileToFolder(file->element_id_, target->element_id_);
            }
            return file;
          });
    } catch (...) {
      CountFailedFile(*progress_ptr, raw_extension);
      report_progress();
      continue;
    }
    if (!element) {
      CountFailedFile(*progress_ptr, raw_extension);
      report_progress();
      continue;
    }
    // Create the corresponding image file
    auto sleeve_file   = std::static_pointer_cast<SleeveFile>(element);

    // MetadataExtractor sets image_type_ from the file content.
    auto image_handler = image_pool_service_->CreateAndReturnPinned([&](Image& image) {
      image.image_path_ = image_path;
      image.image_name_ = file_name;
    });

    if (!image_handler) {
      CountFailedFile(*progress_ptr, raw_extension);
      report_progress();
      continue;
    }

    auto image_handler_ptr =
        std::make_shared<ImagePoolManager::PinnedImageHandle>(std::move(image_handler));
    auto image_ptr = image_handler_ptr->Get();

    // Link the image to the SleeveFile
    sleeve_file->SetImage(image_ptr);
    const auto placeholders_created = progress_ptr->placeholders_created_.fetch_add(1) + 1;
    if (import_log) {
      import_log->AddPlaceholder(image_ptr->image_id_, sleeve_file->element_id_, file_name,
                                 image_path);
    }

    if (job) {
      job->metadata_tasks_submitted_.fetch_add(1);
    }
    root_writer->AddOutstandingEncode();

    const auto element_id       = sleeve_file->element_id_;
    const auto pipeline_service = pipeline_service_;
    const auto file_categories  = any_category_paths.contains(image_path.native())
                                      ? kAllImportCategories
                                      : allowed_categories;

    // Extract metadata and encode the history root on the pool; roots commit in batches.
    // The placeholder loop queues files much faster than the pool reads them, so a cancel
    // usually finds most tasks still queued: each one checks the job before reading its file.
    thread_pool_.Submit([image_handler_ptr, progress_ptr, job, import_log, element_id,
                         pipeline_service, root_writer, file_categories, raw_extension]() {
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
          full_batch =
              root_writer->Add(image_ptr->image_id_, raw_extension,
                               EncodeImportedImageRoot(*pipeline_service, element_id, image_ptr));
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
        if (job && job->on_progress_) {
          job->on_progress_(*progress_ptr);
        }
        if (job) {
          job->metadata_tasks_finished_.fetch_add(1);
        }
      }
      WriteImportRootBatch(std::move(full_batch), *pipeline_service, job, import_log, progress_ptr);
      if (root_writer->FinishEncode(job)) {
        WriteImportRootBatch(root_writer->TakePending(), *pipeline_service, job, import_log,
                             progress_ptr);
      }
      TryFinishImportJob(job, progress_ptr);
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
  if (root_writer->NoOutstandingEncode()) {
    // Every encode finished before submission closed: write the remainder on the pool.
    const auto pipeline_service = pipeline_service_;
    thread_pool_.Submit([root_writer, pipeline_service, job, import_log, progress_ptr]() {
      WriteImportRootBatch(root_writer->TakePending(), *pipeline_service, job, import_log,
                           progress_ptr);
      TryFinishImportJob(job, progress_ptr);
    });
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
  image_pool_service_->SyncWithStorage();
  fs_service_->Sync();
}
};  // namespace alcedo
