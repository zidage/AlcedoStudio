//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/export_service.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"
#include "io/image/image_writer.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {

auto ResolveExportColorProfileConfig(const OperatorParams& params) -> ExportColorProfileConfig {
  return ExportColorProfileConfig{params.to_output_params_.encoding_space_,
                                  params.to_output_params_.eotf_,
                                  params.to_output_params_.peak_luminance_};
}

auto HasExportMetadata(const ExifDisplayMetaData& metadata) -> bool {
  return !metadata.make_.empty() || !metadata.model_.empty() || !metadata.lens_.empty() ||
         !metadata.lens_make_.empty() || !metadata.date_time_str_.empty() ||
         metadata.aperture_ > 0.0f || metadata.focal_ > 0.0f || metadata.focal_35mm_ > 0.0f ||
         metadata.focus_distance_m_ > 0.0f || metadata.iso_ > 0 ||
         (metadata.shutter_speed_.first > 0 && metadata.shutter_speed_.second > 0) ||
         ExifDisplayMetaData::NormalizeRating(metadata.rating_) > 0;
}

auto ResolveImageExportMetadata(const std::shared_ptr<Image>& image)
    -> std::optional<ExifDisplayMetaData> {
  if (!image) {
    return std::nullopt;
  }
  ExifDisplayMetaData metadata;
  if (image->has_exif_display_.load()) {
    metadata = image->exif_display_;
  } else if (image->has_exif_json_.load()) {
    metadata.FromJson(image->exif_json_);
  } else {
    return std::nullopt;
  }
  metadata.rating_ = ExifDisplayMetaData::NormalizeRating(metadata.rating_);
  return HasExportMetadata(metadata) ? std::optional<ExifDisplayMetaData>(std::move(metadata))
                                     : std::nullopt;
}

auto TemporaryExportPath(const std::filesystem::path& final_path, image_id_t image_id)
    -> std::filesystem::path {
  static std::atomic_uint64_t next_temporary_id{0};
  const auto temporary_id = next_temporary_id.fetch_add(1, std::memory_order_relaxed);
  const auto name = final_path.stem().wstring() + L".alcedo-export-" + std::to_wstring(image_id) +
                    L"-" + std::to_wstring(temporary_id) + L".tmp" +
                    final_path.extension().wstring();
  return final_path.parent_path() / name;
}

void CommitExportFile(const std::filesystem::path& temporary_path,
                      const std::filesystem::path& final_path,
                      ExportCollisionPolicy        collision_policy) {
  std::error_code exists_error;
  const bool      final_exists = std::filesystem::exists(final_path, exists_error);
  if (exists_error) {
    throw std::runtime_error("ExportService: cannot inspect output path: " +
                             exists_error.message());
  }
  if (final_exists && collision_policy == ExportCollisionPolicy::FAIL) {
    throw std::runtime_error("ExportService: output file already exists");
  }

#if defined(_WIN32)
  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (collision_policy == ExportCollisionPolicy::REPLACE) flags |= MOVEFILE_REPLACE_EXISTING;
  if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(), flags)) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                            "ExportService: cannot commit output file");
  }
#else
  std::error_code rename_error;
  std::filesystem::rename(temporary_path, final_path, rename_error);
  if (rename_error) {
    throw std::runtime_error("ExportService: cannot commit output file: " + rename_error.message());
  }
#endif
}

}  // namespace

auto ExportService::RunExportRenderTask(const ExportTask& task) -> ExportResult {
  ExportResult result;
  ExportRecipe recipe     = task.recipe_.value_or(ExportRecipe::FromLegacyOptions(task.options_));
  auto         final_path = recipe.codec_.export_path_;
  std::filesystem::path temporary_path;
  result.output_path_ = final_path;
  std::error_code                   cleanup_error;
  std::shared_ptr<PipelineSnapshot> pipeline_snapshot;
  auto                              release_pipeline_snapshot = [&]() {
    if (!pipeline_snapshot) {
      return;
    }
    pipeline_service_->ReleasePipelineSnapshot(pipeline_snapshot);
    pipeline_snapshot.reset();
  };

  std::string stage = "prepare-name";
  try {
    if (final_path.empty() || final_path.filename().empty()) {
      throw std::runtime_error("ExportService: output path must contain a file name");
    }
    if (task.file_name_context_.has_value()) {
      auto name_context = *task.file_name_context_;
      if (name_context.source_stem_.empty()) {
        name_context.source_stem_ = final_path.stem().wstring();
      }
      const auto resolved_name =
          ResolveExportFileName(recipe.file_name_, name_context, recipe.codec_.format_);
      if (!resolved_name.success_) {
        throw std::runtime_error("ExportService: " + resolved_name.message_);
      }
      final_path          = final_path.parent_path() / resolved_name.file_name_;
      result.output_path_ = final_path;
    }
    temporary_path = TemporaryExportPath(final_path, task.image_id_);
    std::filesystem::remove(temporary_path, cleanup_error);

    stage = "load-pipeline";
    std::string snapshot_error;
    pipeline_snapshot =
        pipeline_service_->LoadPipelineSnapshot(task.sleeve_id_, task.image_id_, &snapshot_error);
    if (!pipeline_snapshot || !pipeline_snapshot->executor_) {
      throw std::runtime_error(
          snapshot_error.empty()
              ? "[ERROR] ExportService: Failed to snapshot pipeline for sleeve id " +
                    std::to_string(task.sleeve_id_)
              : snapshot_error);
    }
    stage           = "load-source";
    // Get the image from image pool service
    auto source_img = image_pool_service_->Read<std::shared_ptr<Image>>(
        task.image_id_, [](const std::shared_ptr<Image>& img) { return img; });
    if (!source_img) {
      throw std::runtime_error("[ERROR] ExportService: Failed to load image for id " +
                               std::to_string(task.image_id_));
    }
    auto       img_src_path    = source_img->image_path_;
    const auto export_metadata = ResolveImageExportMetadata(source_img);

    stage                      = "render";
    // Create a pipeline task for export
    PipelineTask render_task;
    // To avoid reading too many images into memory at once, we let the pipeline load the image
    // So we create a dummy Image object with only the path set
    render_task.input_desc_           = std::make_shared<Image>(img_src_path, ImageType::DEFAULT);
    render_task.pipeline_executor_    = pipeline_snapshot->executor_;
    render_task.options_.is_blocking_ = true;
    render_task.options_.is_callback_ = false;

    // Inject pre-extracted raw metadata from the real Image into the pipeline
    // so downstream operators resolve eagerly.
    if (source_img->HasRawColorContext()) {
      pipeline_snapshot->executor_->InjectRawMetadata(source_img->GetRawColorContext());
    }

    // Use full res export, even though the task requires resizing,
    // to benefit from the super sampling
    render_task.options_.render_desc_.render_type_ = RenderType::FULL_RES_EXPORT;
    // Set export options in the pipeline executor
    auto render_promise = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
    render_task.result_ = render_promise;
    auto render_future  = render_promise->get_future();
    // Schedule the render task
    pipeline_scheduler_->ScheduleTask(std::move(render_task));

    std::shared_ptr<ImageBuffer> rendered_image;
    try {
      // Wait for the render to complete
      rendered_image = render_future.get();
    } catch (...) {
      throw;
    }

    stage = "prepare-output";
    // Save pipeline back to storage
    const auto export_profile =
        ResolveExportColorProfileConfig(pipeline_snapshot->executor_->GetGlobalParams());
    const auto effective_profile = recipe.icc_ == ExportIccPolicy::OMIT
                                       ? std::optional<ExportColorProfileConfig>{}
                                       : export_profile;
    const bool wrote_ultra_hdr   = ImageWriter::ShouldWriteUltraHdr(recipe.codec_, export_profile);
    release_pipeline_snapshot();
    stage                      = "encode";
    recipe.codec_.export_path_ = temporary_path;
    ImageWriter::WriteImageToPath(img_src_path, rendered_image, recipe, export_profile,
                                  export_metadata);

    stage = "verify";
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(temporary_path, file_error) || file_error ||
        std::filesystem::file_size(temporary_path, file_error) == 0 || file_error) {
      throw std::runtime_error("ExportService: encoded temporary file is missing or empty");
    }

    stage = "commit";
    CommitExportFile(temporary_path, final_path, recipe.collision_);
    result.success_                        = true;
    result.wrote_ultra_hdr_                = wrote_ultra_hdr;
    result.used_embedded_profile_fallback_ = false;
    result.metadata_written_ =
        recipe.metadata_.mode_ == ExportMetadataMode::STANDARD && export_metadata.has_value();
    result.icc_embedded_ =
        recipe.icc_ == ExportIccPolicy::EMBED_OUTPUT_PROFILE && effective_profile.has_value();
    result.resolution_tags_written_ = recipe.resize_.dpi_ > 0.0;
    return result;
  } catch (const std::exception& error) {
    release_pipeline_snapshot();
    if (!temporary_path.empty()) std::filesystem::remove(temporary_path, cleanup_error);
    result.success_      = false;
    result.failed_stage_ = std::move(stage);
    result.message_      = error.what();
    return result;
  } catch (...) {
    release_pipeline_snapshot();
    if (!temporary_path.empty()) std::filesystem::remove(temporary_path, cleanup_error);
    result.success_      = false;
    result.failed_stage_ = std::move(stage);
    result.message_      = "Unknown export error";
    return result;
  }
}

void ExportService::ExportAll(
    std::function<void(std::shared_ptr<std::vector<ExportResult>>)> callback) {
  ExportAll({}, std::move(callback));
}

void ExportService::ExportAll(
    std::function<void(const ExportProgress&)>                      progress_callback,
    std::function<void(std::shared_ptr<std::vector<ExportResult>>)> callback) {
  auto                    results = std::make_shared<std::vector<ExportResult>>();
  std::vector<ExportTask> tasks;

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    tasks.reserve(export_queue_.size());
    while (!export_queue_.empty()) {
      tasks.push_back(export_queue_.front());
      export_queue_.pop_front();
    }
  }

  const size_t queue_size = tasks.size();
  if (queue_size == 0) {
    try {
      callback(results);
    } catch (...) {
    }
    return;
  }

  results->resize(queue_size);

  auto completed = std::make_shared<std::atomic_size_t>(0);
  auto succeeded = std::make_shared<std::atomic_size_t>(0);
  auto failed    = std::make_shared<std::atomic_size_t>(0);
  for (size_t task_index = 0; task_index < tasks.size(); ++task_index) {
    const auto task = tasks[task_index];
    // Export in thread pool
    export_thread_pool_.Submit([this, task, results, progress_callback, callback, completed,
                                succeeded, failed, queue_size, task_index]() {
      if (progress_callback) {
        try {
          progress_callback(ExportProgress{
              .total_        = queue_size,
              .completed_    = completed->load(std::memory_order_acquire),
              .succeeded_    = succeeded->load(std::memory_order_acquire),
              .failed_       = failed->load(std::memory_order_acquire),
              .sleeve_id_    = task.sleeve_id_,
              .image_id_     = task.image_id_,
              .task_started_ = true,
          });
        } catch (...) {
        }
      }

      ExportResult result;
      // Do export, this call will block until done
      try {
        result = RunExportRenderTask(task);
      } catch (const std::exception& e) {
        result.success_ = false;
        result.message_ = e.what();
      } catch (...) {
        result.success_ = false;
        result.message_ = "Unknown export error";
      }

      const bool export_ok = result.success_;

      // Store result
      {
        std::lock_guard<std::mutex> res_lock(result_mutex_);
        (*results)[task_index] = std::move(result);
      }

      if (export_ok) {
        succeeded->fetch_add(1, std::memory_order_acq_rel);
      } else {
        failed->fetch_add(1, std::memory_order_acq_rel);
      }

      // If all done, call the callback
      const size_t finished = completed->fetch_add(1, std::memory_order_acq_rel) + 1;
      if (progress_callback) {
        try {
          progress_callback(ExportProgress{
              .total_         = queue_size,
              .completed_     = finished,
              .succeeded_     = succeeded->load(std::memory_order_acquire),
              .failed_        = failed->load(std::memory_order_acquire),
              .sleeve_id_     = task.sleeve_id_,
              .image_id_      = task.image_id_,
              .task_finished_ = true,
              .task_success_  = export_ok,
          });
        } catch (...) {
        }
      }
      if (finished == queue_size) {
        try {
          callback(results);
        } catch (...) {
        }
      }
    });
  }
};
};  // namespace alcedo
