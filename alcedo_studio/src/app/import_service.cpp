// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "app/import_service.hpp"

#include <future>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "edit/graph/pipeline_document.hpp"
#include "image/metadata_extractor.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "storage/store/image/source_file_store.hpp"
#include "utils/file/source_file_reader.hpp"
#include "utils/file/source_image_path.hpp"

namespace alcedo {
namespace {
constexpr std::size_t kImportBatchSize = 64;

auto IsRawExtension(const image_path_t& path) -> bool {
  return CategoryForPath(path) == ImportFileCategory::Raw;
}

/// Algorithm output owned until publication. The open reader protects the accepted bytes
/// through persistence. Image data stays in the pool, with exclusive worker access until get().
struct PreparedImport {
  std::unique_ptr<SourceFileReader> reader;
  EncodedImageRoot                  root;
  ImportErrorCode                   error = ImportErrorCode::UNKNOWN;
  std::string                       message;
};

/// Provisional IDs, source path, and owned pin/future of one bounded batch item.
struct PendingImport {
  image_id_t                                           image   = 0;
  sl_element_id_t                                      element = 0;
  image_path_t                                         path;
  std::wstring                                         normalized;
  std::shared_ptr<ImagePoolManager::PinnedImageHandle> pin;
  std::future<PreparedImport>                          ready;
};

/// Encode on a private default document, without constructing an image executor.
auto EncodeImportedImageRoot(const PipelineMgmtService& service, sl_element_id_t element,
                             const std::shared_ptr<Image>& image) -> EncodedImageRoot {
  if (image->HasRasterColorDescription()) {
    return service.EncodeImageRoot(
        element, CreateDefaultRasterPipelineDocument(image->GetRasterColorDescription()), nullptr);
  }
  return service.EncodeImageRoot(
      element, CreateDefaultPipelineDocument(),
      image->HasRawColorContext() ? &image->GetRawColorContext() : nullptr);
}
}  // namespace

auto ImportServiceImpl::ImportToFolder(const std::vector<image_path_t>& paths,
                                       const image_path_t& dest, const ImportOptions& options,
                                       std::shared_ptr<ImportJob> job)
    -> std::shared_ptr<ImportJob> {
  if (!job) job = std::make_shared<ImportJob>();
  job->import_log_       = std::make_shared<ImportLog>();
  job->progress_         = std::make_shared<ImportProgress>();
  job->progress_->total_ = static_cast<uint32_t>(paths.size());
  job->progress_->raw_total_ =
      static_cast<uint32_t>(std::count_if(paths.begin(), paths.end(), IsRawExtension));
  if (active_.exchange(true)) {
    ImportResult result;
    result.requested_ = result.failed_ = job->progress_->total_;
    result.raw_requested_ = result.raw_failed_ = job->progress_->raw_total_;
    result.errors_.push_back({ImportErrorCode::BUSY, {}, "An import is already running"});
    job->progress_->failed_     = result.failed_;
    job->progress_->raw_failed_ = result.raw_failed_;
    job->cancelation_acked_     = true;
    if (job->on_finished_) job->on_finished_(result);
    return job;
  }
  submission_thread_.Submit(
      [this, paths, dest, options, job] { RunImport(paths, dest, options, job); });
  return job;
}

void ImportServiceImpl::RunImport(const std::vector<image_path_t>& paths, const image_path_t& dest,
                                  const ImportOptions&              options,
                                  const std::shared_ptr<ImportJob>& job) {
  auto&           progress = *job->progress_;
  ImportResult    result;
  SourceFileStore sources(fs_service_->GetStorage()->GetDatabase());
  std::map<std::wstring, std::vector<SourceFileBinding>> bindings;
  std::vector<PendingImport>                             batch;
  std::set<std::wstring>                                 pending_paths;
  bool stop_admission       = !restoration_error_.empty();
  result.restoration_error_ = restoration_error_;
  auto report               = [&] {
    if (job->on_progress_) job->on_progress_(progress);
  };
  auto fail = [&](const image_path_t& path, ImportErrorCode code, const std::string& message,
                  image_id_t image = 0) {
    result.errors_.push_back({code, path, message});
    progress.failed_.fetch_add(1);
    if (IsRawExtension(path)) progress.raw_failed_.fetch_add(1);
    if (code == ImportErrorCode::CANCELED) progress.canceled_.fetch_add(1);
    if (code == ImportErrorCode::UNSUPPORTED_FORMAT) progress.unsupported_.fetch_add(1);
    if (code == ImportErrorCode::EXCLUDED_TYPE) progress.excluded_type_.fetch_add(1);
    if (image)
      job->import_log_->MarkMetadataFailure(image, code, message);
    else
      job->import_log_->RecordFailure(path, code, message);
    report();
  };
  /// Restore only provisional IDs. Removal errors stop admission and remain in the result.
  auto restore = [&](const std::vector<image_id_t>&      images,
                     const std::vector<sl_element_id_t>& elements) {
    if (images.empty() && elements.empty()) return;
    try {
      sources.Remove(images);
      {
        auto             guard = fs_service_->GetStorage()->GetDatabase().GetConnectionGuard();
        auto             lock  = guard.Lock();
        CommitGraphStore history(guard.conn_);
        for (auto id : elements) history.DeleteGraphForElement(id);
      }
      fs_service_->Write_NoSync<void>([&](FileSystem& fs) { fs.DeleteFilesEverywhere(elements); });
      const auto sync = fs_service_->Sync();
      if (!sync.success_) throw std::runtime_error(sync.message_);
      image_pool_service_->DiscardImportedImages(images);
    } catch (const std::exception& error) {
      result.restoration_error_ = error.what();
      restoration_error_        = error.what();
      stop_admission            = true;
      result.errors_.push_back({ImportErrorCode::RESTORATION_FAILED, {}, error.what()});
    }
  };
  /// Consume worker outputs and publish a checked batch before confirming any follower skip.
  auto flush = [&] {
    if (batch.empty()) return;
    std::vector<PreparedImport>  prepared;
    std::vector<std::size_t>     accepted;
    std::vector<image_id_t>      rejected_images;
    std::vector<sl_element_id_t> rejected_elements;
    for (std::size_t i = 0; i < batch.size(); ++i) {
      prepared.push_back(batch[i].ready.get());
      job->metadata_tasks_finished_.fetch_add(1);
      auto& work = prepared.back();
      if (work.reader)
        accepted.push_back(i);
      else {
        fail(batch[i].path, work.error, work.message, batch[i].image);
        rejected_images.push_back(batch[i].image);
        rejected_elements.push_back(batch[i].element);
      }
    }
    restore(rejected_images, rejected_elements);
    std::vector<image_id_t>      images;
    std::vector<sl_element_id_t> elements;
    for (auto i : accepted) {
      images.push_back(batch[i].image);
      elements.push_back(batch[i].element);
    }
    if (!accepted.empty()) {
      try {
        if (stop_admission) throw std::runtime_error("Import stopped after restoration failure");
        std::vector<EncodedImageRoot> roots;
        for (auto i : accepted) {
          prepared[i].reader->Validate();
          roots.push_back(std::move(prepared[i].root));
        }
        image_pool_service_->PersistImportedImages(images);
        const auto sync = fs_service_->Sync();
        if (!sync.success_) throw std::runtime_error(sync.message_);
        pipeline_service_->WriteImageRoots(std::move(roots));
        std::vector<std::pair<image_id_t, uint64_t>> sizes;
        for (auto i : accepted) sizes.emplace_back(batch[i].image, prepared[i].reader->Size());
        sources.RecordSizes(sizes);
        for (auto i : accepted) prepared[i].reader->Validate();
        for (auto i : accepted) {
          job->import_log_->MarkMetadataSuccess(batch[i].image);
          bindings[batch[i].normalized].push_back(
              {batch[i].image, batch[i].element, prepared[i].reader->Size()});
          progress.metadata_done_.fetch_add(1);
          if (IsRawExtension(batch[i].path)) progress.raw_imported_.fetch_add(1);
        }
      } catch (const std::exception& error) {
        const auto* source_error = dynamic_cast<const SourceReadError*>(&error);
        const auto  code = source_error ? source_error->code : ImportErrorCode::DB_WRITE_FAILED;
        for (auto i : accepted) fail(batch[i].path, code, error.what(), batch[i].image);
        restore(images, elements);
        if (!source_error) stop_admission = true;
      }
    }
    batch.clear();
    pending_paths.clear();
    report();
  };
  try {
    std::set<std::wstring> requested;
    for (const auto& path : paths) {
      if (path.is_absolute()) requested.insert(NormalizeSourceImagePath(path));
    }
    if (options.skip_duplicates_) bindings = sources.FindBindings(requested);
    for (const auto& path : paths) {
      if (job->IsCancelled()) {
        fail(path, ImportErrorCode::CANCELED, "Import canceled before the file was read");
        continue;
      }
      if (stop_admission) {
        fail(path, ImportErrorCode::DB_WRITE_FAILED,
             "Import admission stopped after storage failure");
        continue;
      }
      std::wstring normalized;
      try {
        normalized = NormalizeSourceImagePath(path);
        if (options.skip_duplicates_ && pending_paths.contains(normalized)) flush();
        // Completion reporting inside flush can deliver cancellation before this occurrence
        // is inspected. Finalize the saved batch, then leave this source unread.
        if (job->IsCancelled()) {
          fail(path, ImportErrorCode::CANCELED, "Import canceled before the file was read");
          continue;
        }
        if (stop_admission) {
          fail(path, ImportErrorCode::DB_WRITE_FAILED,
               "Import admission stopped after storage failure");
          continue;
        }
        const auto candidates = bindings.find(normalized);
        if (options.skip_duplicates_ && candidates != bindings.end()) {
          // Size comparison never reads image contents. The same Image and editing roots
          // continue to reference this path after replacement; only its recorded size changes.
          SourceFileReader               reader(path);
          bool                           changed = false;
          std::map<image_id_t, uint64_t> updates;
          ImportSourceChange             change{path, {}};
          for (const auto& binding : candidates->second) {
            if (binding.byte_size && *binding.byte_size != reader.Size()) changed = true;
            if (!binding.byte_size || *binding.byte_size != reader.Size())
              updates.emplace(binding.image_id, reader.Size());
            change.entries.emplace_back(binding.element_id, binding.image_id);
          }
          reader.Validate();
          const std::vector<std::pair<image_id_t, uint64_t>> sizes(updates.begin(), updates.end());
          sources.RecordSizes(sizes);
          for (auto& binding : candidates->second) binding.byte_size = reader.Size();
          if (changed) {
            result.source_changes_.push_back(std::move(change));
            progress.updated_.fetch_add(1);
            if (IsRawExtension(path)) progress.raw_updated_.fetch_add(1);
          } else {
            job->import_log_->RecordDuplicate(path);
            progress.duplicates_.fetch_add(1);
            if (IsRawExtension(path)) progress.raw_duplicates_.fetch_add(1);
          }
          report();
          continue;
        }
      } catch (const SourceReadError& error) {
        fail(path, error.code, error.what());
        continue;
      } catch (const std::invalid_argument& error) {
        fail(path, ImportErrorCode::READ_FAILED, error.what());
        continue;
      } catch (const std::exception& error) {
        fail(path, ImportErrorCode::COMPARISON_FAILED, error.what());
        stop_admission = true;
        continue;
      }
      PendingImport pending;
      pending.path       = path;
      pending.normalized = normalized;
      try {
        if (!std::filesystem::is_regular_file(path)) {
          fail(path, ImportErrorCode::FILE_NOT_FOUND, "Source is not a readable regular file");
          continue;
        }
        auto handle = image_pool_service_->CreateAndReturnPinned([&](Image& image) {
          image.image_path_ = path;
          image.image_name_ = path.filename().wstring();
        });
        if (!handle) throw std::runtime_error("Cannot allocate import Image");
        pending.pin   = std::make_shared<ImagePoolManager::PinnedImageHandle>(std::move(handle));
        auto image    = pending.pin->Get();
        pending.image = image->image_id_;
        pending.element    = fs_service_->Write_NoSync<sl_element_id_t>([&](FileSystem& fs) {
          std::shared_ptr<SleeveElement> target;
          const auto                     target_path = dest.lexically_normal();
          if (!target_path.empty() && target_path != L"/" && target_path != L".") {
            target = fs.Get(dest, false);
            if (!target || target->type_ != ElementType::FOLDER) {
              throw std::runtime_error("Import target is not a folder");
            }
          }
          auto file       = fs.CreateFileInLibrary(image->image_name_);
          pending.element = file->element_id_;
          file->SetImage(image);
          if (target) fs.LinkFileToFolder(file->element_id_, target->element_id_);
          return file->element_id_;
        });
        job->import_log_->AddPlaceholder(pending.image, pending.element, image->image_name_, path);
        progress.placeholders_created_.fetch_add(1);
        auto promise  = std::make_shared<std::promise<PreparedImport>>();
        pending.ready = promise->get_future();
        job->metadata_tasks_submitted_.fetch_add(1);
        thread_pool_.Submit([this, pin = pending.pin, element = pending.element, path,
                             allowed = options.allowed_categories_, promise, job] {
          PreparedImport work;
          try {
            if (job->IsCancelled()) {
              throw SourceReadError(ImportErrorCode::CANCELED,
                                    "Import canceled before reading source");
            }
            auto reader = std::make_unique<SourceFileReader>(path);
            auto image  = pin->Get();
            MetadataExtractor::ExtractEXIF_ToImage(path, *image, allowed);
            if (job->IsCancelled()) {
              throw SourceReadError(ImportErrorCode::CANCELED,
                                    "Import canceled while reading metadata");
            }
            reader->Validate();
            work.root   = EncodeImportedImageRoot(*pipeline_service_, element, image);
            work.reader = std::move(reader);
          } catch (const SourceReadError& error) {
            work.error   = error.code;
            work.message = error.what();
          } catch (const MetadataExtractionError& error) {
            work.error   = error.code();
            work.message = error.message();
          } catch (const std::exception& error) {
            work.error   = ImportErrorCode::METADATA_EXTRACTION_FAILED;
            work.message = error.what();
          }
          promise->set_value(std::move(work));
        });
        pending_paths.insert(normalized);
        batch.push_back(std::move(pending));
        if (batch.size() == kImportBatchSize) flush();
        report();
      } catch (const std::exception& error) {
        fail(path, ImportErrorCode::SLEEVE_CREATE_FAILED, error.what());
        std::vector<image_id_t>      images;
        std::vector<sl_element_id_t> elements;
        if (pending.image) images.push_back(pending.image);
        if (pending.element) elements.push_back(pending.element);
        restore(images, elements);
      }
    }
    flush();
  } catch (const std::exception& error) {
    flush();
    while (progress.metadata_done_ + progress.updated_ + progress.duplicates_ + progress.failed_ <
           progress.total_) {
      fail(paths[progress.metadata_done_ + progress.updated_ + progress.duplicates_ +
                 progress.failed_],
           ImportErrorCode::DB_WRITE_FAILED, error.what());
    }
  }
  job->submission_closed_ = true;
  result.updated_         = progress.updated_;
  result.raw_updated_     = progress.raw_updated_;
  result.requested_       = progress.total_;
  result.imported_        = progress.metadata_done_;
  result.duplicates_      = progress.duplicates_;
  result.failed_          = progress.failed_;
  result.unsupported_     = progress.unsupported_;
  result.excluded_type_   = progress.excluded_type_;
  result.canceled_        = progress.canceled_;
  result.raw_requested_   = progress.raw_total_;
  result.raw_imported_    = progress.raw_imported_;
  result.raw_duplicates_  = progress.raw_duplicates_;
  result.raw_failed_      = progress.raw_failed_;
  active_                 = false;
  job->cancelation_acked_ = true;
  if (job->on_finished_) job->on_finished_(result);
}

void ImportServiceImpl::SyncImports(const ImportLogSnapshot&, const image_path_t&) {
  const auto images = image_pool_service_->SyncWithStorage();
  if (!images.failed_images_.empty())
    throw std::runtime_error(images.failed_images_.front().message_);
  const auto files = fs_service_->Sync();
  if (!files.success_) throw std::runtime_error(files.message_);
}
}  // namespace alcedo
