//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/import_service.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "app/pipeline_service.hpp"
#include "decoders/processor/raw_color_context.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
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

/// Install default operator params (already on a freshly loaded executor) plus image-local
/// RAW/lens/color-temp inherent fields, then rebuild global params and execution stages once.
void AssembleImportPipelineParams(CPUPipelineExecutor& exec, const Image& image) {
  auto& global_params = exec.GetGlobalParams();

  auto& geometry_stage = exec.GetStage(PipelineStageName::Geometry_Adjustment);
  nlohmann::json crop_params = pipeline_defaults::MakeDefaultCropRotateParams();
  if (const auto crop_entry = geometry_stage.GetOperator(OperatorType::CROP_ROTATE);
      crop_entry.has_value() && crop_entry.value() && crop_entry.value()->op_) {
    crop_params = crop_entry.value()->op_->GetParams();
  }
  auto& source_size = crop_params["crop_rotate"]["source_size"];
  source_size["width"]  = image.exif_display_.width_;
  source_size["height"] = image.exif_display_.height_;
  geometry_stage.SetOperator(OperatorType::CROP_ROTATE, crop_params, global_params);

  if (image.HasRawColorContext()) {
    const auto& ctx = image.GetRawColorContext();
    auto&       loading_stage = exec.GetStage(PipelineStageName::Image_Loading);

    nlohmann::json raw_params = pipeline_defaults::MakeDefaultRawDecodeParams();
    if (const auto raw_entry = loading_stage.GetOperator(OperatorType::RAW_DECODE);
        raw_entry.has_value() && raw_entry.value() && raw_entry.value()->op_) {
      raw_params = raw_entry.value()->op_->GetParams();
    }
    if (!raw_params.contains("raw") || !raw_params["raw"].is_object()) {
      raw_params["raw"] = nlohmann::json::object();
    }
    const auto context_json = RawColorContextToJson(ctx);
    for (auto it = context_json.begin(); it != context_json.end(); ++it) {
      raw_params["raw"][it.key()] = it.value();
    }
    loading_stage.SetOperator(OperatorType::RAW_DECODE, raw_params, global_params);

    nlohmann::json lens_params = pipeline_defaults::MakeDefaultLensCalibParams();
    if (const auto lens_entry = loading_stage.GetOperator(OperatorType::LENS_CALIBRATION);
        lens_entry.has_value() && lens_entry.value() && lens_entry.value()->op_) {
      lens_params = lens_entry.value()->op_->GetParams();
    }
    if (!lens_params.contains("lens_calib") || !lens_params["lens_calib"].is_object()) {
      lens_params["lens_calib"] = nlohmann::json::object();
    }
    auto& lens_inner = lens_params["lens_calib"];
    if (!ctx.camera_make_.empty()) {
      lens_inner["cam_maker"] = ctx.camera_make_;
    }
    if (!ctx.camera_model_.empty()) {
      lens_inner["cam_model"] = ctx.camera_model_;
    }
    if (ctx.lens_metadata_valid_ || !ctx.lens_make_.empty() || !ctx.lens_model_.empty()) {
      lens_inner["lens_maker"]        = ctx.lens_make_;
      lens_inner["lens_model"]        = ctx.lens_model_;
      lens_inner["focal_length_mm"]   = ctx.focal_length_mm_;
      lens_inner["aperture_f_number"] = ctx.aperture_f_number_;
      lens_inner["distance_m"]        = ctx.focus_distance_m_;
      lens_inner["focal_35mm_mm"]     = ctx.focal_35mm_mm_;
      lens_inner["crop_factor_hint"]  = ctx.crop_factor_hint_;
    }
    loading_stage.SetOperator(OperatorType::LENS_CALIBRATION, lens_params, global_params);
    loading_stage.EnableOperator(OperatorType::LENS_CALIBRATION,
                                 lens_inner.value("enabled", true), global_params);

    // Seed global raw fields and inherent context so ColorTemp can resolve as-shot CCT/Tint.
    exec.InjectRawMetadata(ctx);
  }

  auto& to_ws_stage = exec.GetStage(PipelineStageName::To_WorkingSpace);
  if (const auto color_temp_entry = to_ws_stage.GetOperator(OperatorType::COLOR_TEMP);
      color_temp_entry.has_value() && color_temp_entry.value() && color_temp_entry.value()->op_) {
    color_temp_entry.value()->op_->SetGlobalParams(global_params);
    to_ws_stage.SetOperator(OperatorType::COLOR_TEMP, color_temp_entry.value()->op_->GetParams(),
                            global_params);
  }

  for (int i = 0; i < static_cast<int>(PipelineStageName::Stage_Count); ++i) {
    exec.GetStage(static_cast<PipelineStageName>(i)).RefreshGlobalParams(global_params);
  }
  exec.SetExecutionStages();
}

void PersistAssembledImportPipeline(PipelineMgmtService& pipeline_service,
                                    sl_element_id_t element_id, const std::shared_ptr<Image>& image) {
  auto guard = pipeline_service.LoadPipeline(element_id);
  if (!guard || !guard->pipeline_) {
    throw std::runtime_error("ImportService: pipeline unavailable during import assembly");
  }

  {
    std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
    AssembleImportPipelineParams(*guard->pipeline_, *image);
  }

  guard->dirty_ = true;
  pipeline_service.SyncPipeline(element_id);

  // Graph bootstrap still uses InitializeImageRoot until later plan items delete root.
  // Inherent operator params are already in the executor and serialized pipeline JSON.
  const RawRuntimeColorContext* ctx_ptr =
      image && image->HasRawColorContext() ? &image->GetRawColorContext() : nullptr;
  pipeline_service.InitializeImageRoot(guard, ctx_ptr);

  pipeline_service.SavePipeline(guard);
}

}  // namespace

static void SetImportResult(std::shared_ptr<ImportJob> job, uint32_t requested, uint32_t imported,
                            uint32_t failed) {
  ImportResult result;
  result.requested_ = requested;
  result.imported_  = imported;
  result.failed_    = failed;
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
  SetImportResult(job, progress->total_, progress->metadata_done_.load(),
                  progress->failed_.load());
}

auto ImportServiceImpl::ImportToFolder(const std::vector<image_path_t>& paths,
                                       const image_path_t& dest, const ImportOptions& options,
                                       std::shared_ptr<ImportJob> job)
    -> std::shared_ptr<ImportJob> {
  (void)options;
  // TODO: Use sleeve service to interact with FS
  // The current implementation is a temporary solution
  auto import_log = std::make_shared<ImportLog>();
  if (job) {
    job->import_log_ = import_log;
  }
  std::shared_ptr<ImportProgress> progress_ptr = std::make_shared<ImportProgress>();
  progress_ptr->total_                         = static_cast<uint32_t>(paths.size());

  if (paths.empty()) {
    // Immediately finish
    SetImportResult(job, 0, 0, 0);
    return job;
  }

  for (const auto& image_path : paths) {
    if (job && job->IsCancelled()) {
      break;
    }
    // Validate that the path is a regular file. File-type detection is deferred
    // to metadata extraction (LibRaw first, then Exiv2 raster gate) — non-images
    // are marked failed and cleaned up by SyncImports.
    if (!std::filesystem::is_regular_file(image_path)) {
      progress_ptr->failed_.fetch_add(1);
      if (job && job->on_progress_) {
        job->on_progress_(*progress_ptr);
      }
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
      progress_ptr->failed_.fetch_add(1);
      if (job && job->on_progress_) {
        job->on_progress_(*progress_ptr);
      }
      continue;
    }
    if (!element) {
      progress_ptr->failed_.fetch_add(1);
      if (job && job->on_progress_) {
        job->on_progress_(*progress_ptr);
      }
      continue;
    }
    // Create the corresponding image file
    auto sleeve_file   = std::static_pointer_cast<SleeveFile>(element);

    // auto image_ptr   = image_pool_manager_->InsertEmpty();
    auto image_handler = image_pool_service_->CreateAndReturnPinnedEmpty();

    if (!image_handler) {
      progress_ptr->failed_.fetch_add(1);
      if (job && job->on_progress_) {
        job->on_progress_(*progress_ptr);
      }
      continue;
    }

    auto image_handler_ptr =
        std::make_shared<ImagePoolManager::PinnedImageHandle>(std::move(image_handler));
    auto image_ptr = image_handler_ptr->Get();
    image_ptr->image_path_ = image_path;
    image_ptr->image_name_ = file_name;
    // TODO: Parse image type for future use

    // Link the image to the SleeveFile
    sleeve_file->SetImage(image_ptr);
    progress_ptr->placeholders_created_.fetch_add(1);
    if (import_log) {
      import_log->AddPlaceholder(image_ptr->image_id_, sleeve_file->element_id_, file_name,
                                 image_path);
    }

    if (job) {
      job->metadata_tasks_submitted_.fetch_add(1);
    }

    const auto element_id       = sleeve_file->element_id_;
    const auto pipeline_service = pipeline_service_;

    // Submit the metadata extraction task to thread pool
    thread_pool_.Submit([image_handler_ptr, progress_ptr, job, import_log, element_id,
                         pipeline_service]() {
      auto image_ptr = image_handler_ptr ? image_handler_ptr->Get() : nullptr;
      if (!image_ptr) {
        progress_ptr->failed_.fetch_add(1);
        if (job && job->on_progress_) {
          job->on_progress_(*progress_ptr);
        }
        if (job) {
          job->metadata_tasks_finished_.fetch_add(1);
        }
        TryFinishImportJob(job, progress_ptr);
        return;
      }

      // Extract metadata, assemble full pipeline JSON, then mark success.
      try {
        MetadataExtractor::ExtractEXIF_ToImage(image_ptr->image_path_, *image_ptr);
        if (pipeline_service) {
          PersistAssembledImportPipeline(*pipeline_service, element_id, image_ptr);
        }
        if (import_log) {
          import_log->MarkMetadataSuccess(image_ptr->image_id_);
        }
        // Update progress
        progress_ptr->metadata_done_.fetch_add(1);

      } catch (const MetadataExtractionError& e) {
        if (import_log) {
          import_log->MarkMetadataFailure(image_ptr->image_id_, e.code(), e.message());
        }
        progress_ptr->failed_.fetch_add(1);
      } catch (const std::exception& e) {
        if (import_log) {
          import_log->MarkMetadataFailure(image_ptr->image_id_,
                                          ImportErrorCode::METADATA_EXTRACTION_FAILED, e.what());
        }
        progress_ptr->failed_.fetch_add(1);
      } catch (...) {
        if (import_log) {
          import_log->MarkMetadataFailure(image_ptr->image_id_,
                                          ImportErrorCode::METADATA_EXTRACTION_FAILED);
        }
        progress_ptr->failed_.fetch_add(1);
      }

      if (job && job->on_progress_) {
        job->on_progress_(*progress_ptr);
      }

      if (job) {
        job->metadata_tasks_finished_.fetch_add(1);
      }
      TryFinishImportJob(job, progress_ptr);
    });
  }

  if (job) {
    if (job->IsCancelled()) {
      const auto accounted =
          progress_ptr->metadata_done_.load() + progress_ptr->failed_.load() +
          (job->metadata_tasks_submitted_.load() - job->metadata_tasks_finished_.load());
      if (accounted < progress_ptr->total_) {
        progress_ptr->failed_.fetch_add(progress_ptr->total_ - accounted);
        if (job->on_progress_) {
          job->on_progress_(*progress_ptr);
        }
      }
    }
    job->submission_closed_.store(true);
  }
  TryFinishImportJob(job, progress_ptr);
  return job;
}

void ImportServiceImpl::SyncImports(const ImportLogSnapshot& log_snapshot,
                                    const image_path_t&      dest) {
  (void)dest;
  if (!image_pool_service_) {
    return;
  }

  for (const auto& entry : log_snapshot.metadata_failed_) {
    if (entry.element_id_ != 0) {
      try {
        fs_service_->Write_NoSync<void>(
            [&entry](FileSystem& fs) { fs.DeleteFileEverywhere(entry.element_id_); });
      } catch (...) {
      }
    }
  }
  image_pool_service_->SyncWithStorage();
  fs_service_->Sync();
}
};  // namespace alcedo
