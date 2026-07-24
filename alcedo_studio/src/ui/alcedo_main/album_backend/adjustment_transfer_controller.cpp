//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"

#include <QFileInfo>
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "edit/operators/utils/color_utils.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

struct AdjustmentItemSpec {
  const char*       key;
  const char*       section;
  const char*       label;
  PipelineStageName stage;
  OperatorType      op_type;
  bool              checked_by_default;
  bool              merge_params = false;
};

constexpr auto kItems = std::array{
    AdjustmentItemSpec{"raw.highlights_reconstruct", "Raw", "Highlight Recovery",
                       PipelineStageName::Image_Loading, OperatorType::RAW_DECODE, false, true},
    AdjustmentItemSpec{"crop_rotate", "Geometry", "Crop / Rotate",
                       PipelineStageName::Geometry_Adjustment, OperatorType::CROP_ROTATE, false},
    AdjustmentItemSpec{"exposure", "Basic", "Exposure", PipelineStageName::Basic_Adjustment,
                       OperatorType::EXPOSURE, true},
    AdjustmentItemSpec{"contrast", "Basic", "Contrast", PipelineStageName::Basic_Adjustment,
                       OperatorType::CONTRAST, true},
    AdjustmentItemSpec{"black", "Basic", "Blacks", PipelineStageName::Basic_Adjustment,
                       OperatorType::BLACK, true},
    AdjustmentItemSpec{"white", "Basic", "Whites", PipelineStageName::Basic_Adjustment,
                       OperatorType::WHITE, true},
    AdjustmentItemSpec{"shadows", "Basic", "Shadows", PipelineStageName::Basic_Adjustment,
                       OperatorType::SHADOWS, true},
    AdjustmentItemSpec{"highlights", "Basic", "Highlights", PipelineStageName::Basic_Adjustment,
                       OperatorType::HIGHLIGHTS, true},
    AdjustmentItemSpec{"curve", "Basic", "Tone Curve", PipelineStageName::Basic_Adjustment,
                       OperatorType::CURVE, true},
    AdjustmentItemSpec{"color_temp", "Color", "White Balance", PipelineStageName::To_WorkingSpace,
                       OperatorType::COLOR_TEMP, true},
    AdjustmentItemSpec{"saturation", "Color", "Saturation", PipelineStageName::Color_Adjustment,
                       OperatorType::SATURATION, true},
    AdjustmentItemSpec{"vibrance", "Color", "Vibrance", PipelineStageName::Color_Adjustment,
                       OperatorType::VIBRANCE, true},
    AdjustmentItemSpec{"tint", "Color", "Tint", PipelineStageName::Color_Adjustment,
                       OperatorType::TINT, true},
    AdjustmentItemSpec{"HLS", "Color", "HLS", PipelineStageName::Color_Adjustment,
                       OperatorType::HLS, true},
    AdjustmentItemSpec{"color_wheel", "Color", "Color Wheels", PipelineStageName::Color_Adjustment,
                       OperatorType::COLOR_WHEEL, true},
    AdjustmentItemSpec{"ocio_lmt", "Color", "Look LUT", PipelineStageName::Color_Adjustment,
                       OperatorType::LMT, true},
    AdjustmentItemSpec{"sharpen", "Detail", "Sharpen", PipelineStageName::Detail_Adjustment,
                       OperatorType::SHARPEN, false},
    AdjustmentItemSpec{"clarity", "Detail", "Clarity", PipelineStageName::Detail_Adjustment,
                       OperatorType::CLARITY, false},
    AdjustmentItemSpec{"lens_calib", "Optics", "Lens Correction", PipelineStageName::Image_Loading,
                       OperatorType::LENS_CALIBRATION, false},
    AdjustmentItemSpec{"odt", "Output", "Output Transform", PipelineStageName::Output_Transform,
                       OperatorType::ODT, false},
};

auto FindSpec(const QString& key) -> const AdjustmentItemSpec* {
  const std::string key_utf8 = key.toStdString();
  for (const auto& spec : kItems) {
    if (key_utf8 == spec.key) {
      return &spec;
    }
  }
  return nullptr;
}

auto ErrorResult(const QString& message) -> QVariantMap {
  return {{"success", false}, {"message", message}};
}

auto SuccessResult(const QString& message = {}) -> QVariantMap {
  QVariantMap result{{"success", true}};
  if (!message.isEmpty()) {
    result.insert("message", message);
  }
  return result;
}

auto CurrentExceptionText(const char* fallback) -> QString {
  try {
    throw;
  } catch (const std::exception& e) {
    return QString::fromUtf8(e.what());
  } catch (...) {
    return QString::fromUtf8(fallback);
  }
}

auto OperatorEntryFor(CPUPipelineExecutor& pipeline, const AdjustmentItemSpec& spec)
    -> std::optional<OperatorEntry*> {
  return pipeline.GetStage(spec.stage).GetOperator(spec.op_type);
}

auto OperatorParamsFor(CPUPipelineExecutor& pipeline, const AdjustmentItemSpec& spec)
    -> nlohmann::json {
  auto entry = OperatorEntryFor(pipeline, spec);
  if (!entry.has_value() || entry.value() == nullptr || !entry.value()->op_) {
    return nlohmann::json::object();
  }
  return entry.value()->op_->GetParams();
}

auto OperatorEnabledFor(CPUPipelineExecutor& pipeline, const AdjustmentItemSpec& spec) -> bool {
  auto entry = OperatorEntryFor(pipeline, spec);
  return entry.has_value() && entry.value() != nullptr ? entry.value()->enable_ : true;
}

auto BoolText(bool value) -> QString { return value ? Tr("On") : Tr("Off"); }

auto IsHdrExportEotf(const ColorUtils::EOTF eotf) -> bool {
  return eotf == ColorUtils::EOTF::ST2084 || eotf == ColorUtils::EOTF::HLG;
}

auto JsonNumberText(const nlohmann::json& params, const char* key, int precision = 2) -> QString {
  if (!params.contains(key) || !params[key].is_number()) {
    return Tr("Default");
  }
  return QString::number(params[key].get<double>(), 'f', precision);
}

auto PathTail(const std::string& path) -> QString {
  if (path.empty()) {
    return Tr("None");
  }
  return QFileInfo(QString::fromStdString(path)).fileName();
}

auto CropRotateValue(const nlohmann::json& params, bool enabled) -> QString {
  const auto   inner        = params.value("crop_rotate", nlohmann::json::object());
  const bool   crop_enabled = inner.value("enabled", enabled);
  const double angle        = inner.value("angle_degrees", 0.0);
  return QStringLiteral("%1 · %2°").arg(BoolText(crop_enabled), QString::number(angle, 'f', 1));
}

auto ColorTempValue(const nlohmann::json& params) -> QString {
  const auto    inner = params.value("color_temp", nlohmann::json::object());
  const QString mode  = QString::fromStdString(inner.value("mode", std::string{"as_shot"}));
  if (mode == QStringLiteral("custom")) {
    return Tr("Custom") + QStringLiteral(" · ") +
           QString::number(inner.value("cct", 6500.0), 'f', 0) + QStringLiteral("K · ") +
           QString::number(inner.value("tint", 0.0), 'f', 1);
  }
  return Tr("As Shot");
}

auto CurveValue(const nlohmann::json& params) -> QString {
  const auto inner = params.value("curve", nlohmann::json::object());
  if (inner.contains("points") && inner["points"].is_array()) {
    return Tr("Points") + QStringLiteral(": ") + QString::number(inner["points"].size());
  }
  return Tr("Default");
}

auto HlsValue(const nlohmann::json& params) -> QString {
  const auto inner = params.value("HLS", nlohmann::json::object());
  if (inner.contains("hls_adj_table") && inner["hls_adj_table"].is_array()) {
    return Tr("Profiles") + QStringLiteral(": ") + QString::number(inner["hls_adj_table"].size());
  }
  return Tr("Default");
}

auto WheelValue(const nlohmann::json& params) -> QString {
  const auto inner = params.value("color_wheel", nlohmann::json::object());
  int        count = 0;
  for (const char* key : {"lift", "gamma", "gain"}) {
    if (inner.contains(key)) {
      ++count;
    }
  }
  return count == 0 ? Tr("Default") : Tr("Lift / Gamma / Gain");
}

auto SharpenValue(const nlohmann::json& params) -> QString {
  const auto inner = params.value("sharpen", nlohmann::json::object());
  return JsonNumberText(inner, "offset", 2);
}

auto LensValue(const nlohmann::json& params, bool enabled) -> QString {
  const auto inner = params.value("lens_calib", nlohmann::json::object());
  return BoolText(inner.value("enabled", enabled));
}

auto OdtValue(const nlohmann::json& params) -> QString {
  const auto inner = params.value("odt", nlohmann::json::object());
  return QString::fromStdString(inner.value("method", std::string{"open_drt"}));
}

auto RawHighlightRecoveryValue(const nlohmann::json& params) -> QString {
  const auto inner = params.value("raw", nlohmann::json::object());
  return BoolText(inner.value("highlights_reconstruct", false));
}

auto ValueFor(const AdjustmentItemSpec& spec, const nlohmann::json& params, bool enabled)
    -> QString {
  const std::string key = spec.key;
  if (key == "raw.highlights_reconstruct") return RawHighlightRecoveryValue(params);
  if (key == "crop_rotate") return CropRotateValue(params, enabled);
  if (key == "exposure") return JsonNumberText(params, "exposure", 2);
  if (key == "contrast") return JsonNumberText(params, "contrast", 2);
  if (key == "black") return JsonNumberText(params, "black", 2);
  if (key == "white") return JsonNumberText(params, "white", 2);
  if (key == "shadows") return JsonNumberText(params, "shadows", 2);
  if (key == "highlights") return JsonNumberText(params, "highlights", 2);
  if (key == "curve") return CurveValue(params);
  if (key == "color_temp") return ColorTempValue(params);
  if (key == "saturation") return JsonNumberText(params, "saturation", 2);
  if (key == "vibrance") return JsonNumberText(params, "vibrance", 2);
  if (key == "tint") return JsonNumberText(params, "tint", 2);
  if (key == "HLS") return HlsValue(params);
  if (key == "color_wheel") return WheelValue(params);
  if (key == "ocio_lmt") return PathTail(params.value("ocio_lmt", std::string{}));
  if (key == "sharpen") return SharpenValue(params);
  if (key == "clarity") return JsonNumberText(params, "clarity", 2);
  if (key == "lens_calib") return LensValue(params, enabled);
  if (key == "odt") return OdtValue(params);
  return Tr("Default");
}

void SanitizeColorTemp(nlohmann::json& params) {
  if (!params.contains("color_temp") || !params["color_temp"].is_object()) {
    return;
  }
  auto& inner = params["color_temp"];
  inner.erase("resolved_cct");
  inner.erase("resolved_tint");
  if (inner.value("mode", std::string{}) == "as_shot") {
    inner.erase("cct");
    inner.erase("tint");
  }
}

void SanitizeLens(nlohmann::json& params) {
  if (!params.contains("lens_calib") || !params["lens_calib"].is_object()) {
    return;
  }
  auto& inner = params["lens_calib"];
  for (const char* key : {"cam_maker", "cam_model", "lens_maker", "lens_model", "focal_length_mm",
                          "aperture_f_number", "distance_m", "focal_35mm_mm", "crop_factor_hint"}) {
    inner.erase(key);
  }
}

auto TransferParamsFor(CPUPipelineExecutor& pipeline, const AdjustmentItemSpec& spec)
    -> nlohmann::json {
  nlohmann::json    params = OperatorParamsFor(pipeline, spec);
  const std::string key    = spec.key;
  if (key == "raw.highlights_reconstruct") {
    const auto raw = params.value("raw", nlohmann::json::object());
    return {{"raw", {{"highlights_reconstruct", raw.value("highlights_reconstruct", false)}}}};
  }
  if (key == "color_temp") {
    SanitizeColorTemp(params);
  } else if (key == "lens_calib") {
    SanitizeLens(params);
  }
  return params;
}

auto RowFor(CPUPipelineExecutor& pipeline, const AdjustmentItemSpec& spec, bool checked)
    -> QVariantMap {
  const bool enabled = OperatorEnabledFor(pipeline, spec);
  const auto params  = OperatorParamsFor(pipeline, spec);
  return {
      {"key", QString::fromUtf8(spec.key)},
      {"section", Tr(spec.section)},
      {"label", Tr(spec.label)},
      {"value", ValueFor(spec, params, enabled)},
      {"checked", checked},
  };
}

auto SummaryRow(const AdjustmentItemSpec& spec, const QString& value) -> QVariantMap {
  return {
      {"key", QString::fromUtf8(spec.key)},
      {"section", Tr(spec.section)},
      {"label", Tr(spec.label)},
      {"value", value},
      {"checked", true},
  };
}

auto TitleForItem(const AlbumItem* item, sl_element_id_t element_id) -> QString {
  if (item != nullptr && !item->file_name.isEmpty()) {
    return item->file_name;
  }
  return Tr("Image") + QStringLiteral(" ") + QString::number(static_cast<qulonglong>(element_id));
}

auto MakeTargetIds(const std::vector<ExportTarget>& targets) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t>        ids;
  std::unordered_set<sl_element_id_t> seen;
  ids.reserve(targets.size());
  seen.reserve(targets.size() * 2 + 1);
  for (const auto& [element_id, image_id] : targets) {
    (void)image_id;
    if (element_id == 0 || !seen.insert(element_id).second) {
      continue;
    }
    ids.push_back(element_id);
  }
  return ids;
}

}  // namespace

AdjustmentTransferController::AdjustmentTransferController(
    ProjectModule* project, LibraryModule* library, ImportExportHandler* import_export,
    QObject* parent)
    : QObject(parent), project_(project), library_(library), import_export_(import_export) {}

auto AdjustmentTransferController::PrepareCopy(uint elementId) -> QVariantMap {
  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    return ErrorResult(Tr("Pipeline service is unavailable."));
  }
  if (elementId == 0) {
    return ErrorResult(Tr("No image selected."));
  }

  std::shared_ptr<PipelineGuard> guard;
  try {
    guard = pipeline_service->LoadPipeline(static_cast<sl_element_id_t>(elementId));
    if (!guard || !guard->pipeline_) {
      return ErrorResult(Tr("Pipeline was not available."));
    }

    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(kItems.size()));
    {
      std::unique_lock<std::mutex> render_guard(guard->pipeline_->GetRenderLock());
      for (const auto& spec : kItems) {
        rows.push_back(RowFor(*guard->pipeline_, spec, spec.checked_by_default));
      }
    }
    pipeline_service->SavePipeline(guard);

    QVariantMap result = SuccessResult();
    const auto* item   = library_->FindAlbumItem(static_cast<sl_element_id_t>(elementId));
    result.insert("sourceTitle", TitleForItem(item, static_cast<sl_element_id_t>(elementId)));
    result.insert("items", rows);
    return result;
  } catch (...) {
    if (guard) {
      pipeline_service->SavePipeline(guard);
    }
    return ErrorResult(CurrentExceptionText("Failed to prepare adjustment copy."));
  }
}

auto AdjustmentTransferController::Copy(uint elementId, const QVariantList& selectedKeys)
    -> QVariantMap {
  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    return ErrorResult(Tr("Pipeline service is unavailable."));
  }
  if (elementId == 0) {
    return ErrorResult(Tr("No image selected."));
  }
  if (selectedKeys.empty()) {
    return ErrorResult(Tr("No adjustments selected."));
  }

  std::vector<const AdjustmentItemSpec*> specs;
  specs.reserve(static_cast<size_t>(selectedKeys.size()));
  std::unordered_set<std::string> seen;
  seen.reserve(static_cast<size_t>(selectedKeys.size()) * 2 + 1);
  for (const QVariant& value : selectedKeys) {
    const QString key  = value.toString();
    const auto*   spec = FindSpec(key);
    if (spec == nullptr || !seen.insert(key.toStdString()).second) {
      continue;
    }
    specs.push_back(spec);
  }
  if (specs.empty()) {
    return ErrorResult(Tr("No supported adjustments selected."));
  }

  std::shared_ptr<PipelineGuard> guard;
  try {
    guard = pipeline_service->LoadPipeline(static_cast<sl_element_id_t>(elementId));
    if (!guard || !guard->pipeline_) {
      return ErrorResult(Tr("Pipeline was not available."));
    }

    AdjustmentTransferPackage package;
    QVariantList              summary;
    {
      std::unique_lock<std::mutex> render_guard(guard->pipeline_->GetRenderLock());
      for (const auto* spec : specs) {
        const bool enabled = OperatorEnabledFor(*guard->pipeline_, *spec);
        const auto params  = TransferParamsFor(*guard->pipeline_, *spec);
        package.operators_.push_back({
            .stage_         = spec->stage,
            .operator_type_ = spec->op_type,
            .enabled_       = enabled,
            .merge_params_  = spec->merge_params,
            .params_        = params,
        });
        summary.push_back(SummaryRow(
            *spec, ValueFor(*spec, OperatorParamsFor(*guard->pipeline_, *spec), enabled)));
      }
    }
    pipeline_service->SavePipeline(guard);

    copied_package_      = std::move(package);
    copied_summary_      = std::move(summary);
    const auto* item     = library_->FindAlbumItem(static_cast<sl_element_id_t>(elementId));
    copied_source_title_ = TitleForItem(item, static_cast<sl_element_id_t>(elementId));
    emit        PackageChanged();

    QVariantMap result = SuccessResult(Tr("Adjustments copied."));
    result.insert("count", static_cast<int>(copied_summary_.size()));
    result.insert("summary", copied_summary_);
    return result;
  } catch (...) {
    if (guard) {
      pipeline_service->SavePipeline(guard);
    }
    return ErrorResult(CurrentExceptionText("Failed to copy adjustments."));
  }
}

auto AdjustmentTransferController::Paste(const QVariantList& targetEntries, const QString& strategy)
    -> QVariantMap {
  if (!copied_package_.has_value()) {
    return ErrorResult(Tr("No copied adjustments."));
  }

  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    return ErrorResult(Tr("Pipeline service is unavailable."));
  }

  const auto targets = import_export_->CollectExportTargets(targetEntries);
  const auto ids     = MakeTargetIds(targets);
  if (ids.empty()) {
    return ErrorResult(Tr("No target images selected."));
  }

  const bool merge_strategy = strategy != QStringLiteral("paste");

  // Paste is root-relative. Merge requires a per-field resolution request and
  // therefore cannot silently substitute the obsolete transaction-array path.
  if (!merge_strategy) {
    return PasteViaMiniGit(ids, *pipeline_service);
  }
  return ErrorResult(Tr("Merge requires per-field conflict resolutions."));
}

auto AdjustmentTransferController::PasteViaMiniGit(
    const std::vector<sl_element_id_t>& ids,
    PipelineMgmtService& pipeline_service) -> QVariantMap {
  AdjustmentApplyResult result;
  for (sl_element_id_t element_id : ids) {
    if (element_id == 0) {
      continue;
    }

    std::shared_ptr<PipelineGuard> guard;
    try {
      guard = pipeline_service.LoadEditorPipeline(element_id);
    } catch (const std::exception& e) {
      result.failures_.push_back({element_id, e.what()});
      continue;
    }
    if (!guard || !guard->pipeline_) {
      result.failures_.push_back({element_id, "Pipeline was not available."});
      continue;
    }

    // Use the commit graph attached to the pipeline guard.
    CommitGraph* graph = nullptr;
    if (guard->commit_graph_) {
      graph = guard->commit_graph_.get();
    }

    if (graph == nullptr) {
      result.failures_.push_back({element_id, "Mini-Git graph was not available for paste."});
      pipeline_service.SavePipeline(guard);
      continue;
    }

    // Mini-Git paste path.
    try {
      auto paste_result = AdjustmentTransferService::PasteAsRootRelativeVersion(
          *graph, pipeline_service, element_id, *copied_package_,
          Tr("Pasted Adjustments").toStdString());
      if (paste_result.pasted) {
        guard->dirty_                       = true;
        guard->working_head_commit_hash_    = paste_result.new_head;
        guard->transaction_chain_hash_      = graph->ChainHashForHead(paste_result.new_head);
        guard->serialized_state_needs_writeback_ = true;
        result.applied_ids_.push_back(element_id);
      } else {
        result.failures_.push_back({element_id, paste_result.error});
      }
    } catch (const std::exception& e) {
      result.failures_.push_back({element_id, e.what()});
    }
    pipeline_service.SavePipeline(guard);
  }

  pipeline_service.Sync();
  PostProcessApplyResult(result, false);

  QVariantList failures;
  for (const auto& failure : result.failures_) {
    failures.push_back(QVariantMap{{"elementId", static_cast<uint>(failure.file_id_)},
                                   {"message", QString::fromStdString(failure.message_)}});
  }

  QVariantMap response = SuccessResult(Tr("Adjustments pasted."));
  response.insert("appliedCount", static_cast<int>(result.applied_ids_.size()));
  response.insert("unchangedCount", static_cast<int>(result.unchanged_ids_.size()));
  response.insert("failureCount", static_cast<int>(result.failures_.size()));
  response.insert("failures", failures);
  return response;
}

void AdjustmentTransferController::PostProcessApplyResult(
    const AdjustmentApplyResult& result, bool /*merge_strategy*/) {
  auto thumbnail_service = project_->handler().thumbnail_service();
  bool hdr_metadata_dirty = false;
  for (sl_element_id_t element_id : result.applied_ids_) {
    const auto*      item     = library_->FindAlbumItem(element_id);
    const image_id_t image_id = item != nullptr ? item->image_id : 0;
    if (image_id != 0) {
      try {
        auto pipeline_service = project_->handler().pipeline_service();
        if (pipeline_service) {
          auto guard = pipeline_service->LoadPipeline(element_id);
          if (guard && guard->pipeline_) {
            const bool is_hdr = IsHdrExportEotf(
                guard->pipeline_->GetGlobalParams().to_output_params_.eotf_);
            pipeline_service->SavePipeline(guard);
            library_->PersistImageHdrFlag(element_id, image_id, is_hdr);
            hdr_metadata_dirty = true;
          }
        }
      } catch (...) {
      }
    }
    if (thumbnail_service) {
      try {
        thumbnail_service->InvalidateThumbnail(element_id);
      } catch (...) {
      }
    }
    if (image_id != 0) {
      if (!library_->thumbs().RefreshCurrentThumbnail(element_id, image_id)) {
        library_->thumbs().UpdateThumbnailState(element_id, QString(), false, false);
      }
    }
  }
  if (hdr_metadata_dirty) {
    if (auto project = project_->handler().project()) {
      try {
        project->GetImagePoolService()->SyncWithStorage();
        QString ignored_error;
        if (project_->handler().PersistCurrentProjectState()) {
          (void)project_->handler().PackageCurrentProjectFiles(&ignored_error);
        }
      } catch (...) {
      }
    }
  }
}

void AdjustmentTransferController::Discard() {
  copied_package_.reset();
  copied_summary_.clear();
  copied_source_title_.clear();
  emit PackageChanged();
}

}  // namespace alcedo::ui
