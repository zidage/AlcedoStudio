//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_controller.hpp"

#include "ui/alcedo_main/album_backend/editor_controller.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

#include "app/pipeline_service.hpp"
#include "app/render_service.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "io/image/image_loader.hpp"
#include "edit/operators/utils/color_utils.hpp"

#include <QCoreApplication>

#include <chrono>

using namespace std::chrono_literals;

namespace alcedo::ui {

using namespace album_util;

namespace {

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

auto Utf8OrNativeToPath(const std::string& raw_path) -> std::filesystem::path {
  if (raw_path.empty()) {
    return {};
  }
  try {
    const auto* begin = reinterpret_cast<const char8_t*>(raw_path.data());
    return std::filesystem::path(std::u8string(begin, begin + raw_path.size()));
  } catch (...) {
    return std::filesystem::path(raw_path);
  }
}

auto IsHdrExportEotf(const ColorUtils::EOTF eotf) -> bool {
  return eotf == ColorUtils::EOTF::ST2084 || eotf == ColorUtils::EOTF::HLG;
}

}  // namespace

#define PL_TEXT(text, ...)                                                    \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT,                              \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text)      \
                              __VA_OPT__(, ) __VA_ARGS__)

EditorController::EditorController(ProjectModule* project, LibraryModule* library, QObject* parent)
    : QObject(parent), project_(project), library_(library) {
  editor_status_text_ = PL_TEXT("Select a photo to edit.");
}

void EditorController::OpenEditor(uint elementId, uint imageId) {
  if (project_->handler().project_loading()) {
    editor_status_text_ = PL_TEXT("Project is loading. Please wait.");
    emit EditorStateChanged();
    return;
  }

  const auto& psvc = project_->handler().pipeline_service();
  auto        proj = project_->handler().project();
  if (!psvc || !proj) {
    editor_status_text_ = PL_TEXT("Editor service is unavailable.");
    emit EditorStateChanged();
    return;
  }

  if (elementId == 0 || imageId == 0) {
    return;
  }

  FinalizeEditorSession(true);

  try {
    editor_pipeline_guard_ = psvc->LoadEditorPipeline(elementId);
    if (!editor_pipeline_guard_ || !editor_pipeline_guard_->pipeline_) {
      throw std::runtime_error("Pipeline is unavailable.");
    }

    editor_element_id_ = elementId;
    editor_image_id_   = imageId;

    if (const auto* item = library_->FindAlbumItem(elementId); item) {
      editor_title_text_ = PL_TEXT("Editing %1", item->file_name);
    } else {
      editor_title_text_ = PL_TEXT("Editing %1", PL_TEXT("image #%1", imageId).Render());
    }
    if (!LoadEditorStateFromPipeline()) {
      throw std::runtime_error("Failed to read editor pipeline state.");
    }
    editor_initial_state_ = editor_state_;
    SetupEditorPipeline();
    editor_scheduler_ = RenderService::GetPreviewScheduler();

    editor_status_text_ = PL_TEXT("Opening editor...");
    editor_active_ = true;
    editor_busy_   = false;
    emit EditorStateChanged();
    QueueEditorRender(RenderType::FAST_PREVIEW);
  } catch (const std::exception& e) {
    editor_pipeline_guard_.reset();
    editor_base_task_ = PipelineTask{};
    editor_status_text_ = PL_TEXT("Failed to open editor: %1", QString::fromUtf8(e.what()));
    editor_active_     = false;
    editor_busy_       = false;
    editor_element_id_ = 0;
    editor_image_id_   = 0;
    editor_title_text_ = {};
    emit EditorStateChanged();
  }
}

void EditorController::CloseEditor() {
  FinalizeEditorSession(true);
}

void EditorController::ResetEditorAdjustments() {
  if (!editor_active_) return;
  editor_state_     = editor_initial_state_;
  editor_lut_index_ = LutIndexForPath(editor_state_.lut_path_);
  emit EditorStateChanged();
  QueueEditorRender(RenderType::FULL_RES_PREVIEW);
}

void EditorController::RequestEditorFullPreview() {
  if (!editor_active_) return;
  QueueEditorRender(RenderType::FULL_RES_PREVIEW);
}

void EditorController::SetEditorLutIndex(int index) {
  if (!editor_active_ || index < 0 ||
      index >= static_cast<int>(editor_lut_paths_.size())) {
    return;
  }
  if (editor_lut_index_ == index) return;
  editor_lut_index_       = index;
  editor_state_.lut_path_ = editor_lut_paths_[static_cast<size_t>(index)];
  emit EditorStateChanged();
  QueueEditorRender(RenderType::FAST_PREVIEW);
}

void EditorController::SetEditorExposure(double value) {
  SetEditorAdjustment(editor_state_.exposure_, value, -10.0, 10.0);
}

void EditorController::SetEditorContrast(double value) {
  SetEditorAdjustment(editor_state_.contrast_, value, -100.0, 100.0);
}

void EditorController::SetEditorSaturation(double value) {
  SetEditorAdjustment(editor_state_.saturation_, value, -100.0, 100.0);
}

void EditorController::SetEditorTint(double value) {
  SetEditorAdjustment(editor_state_.tint_, value, -100.0, 100.0);
}

void EditorController::SetEditorBlacks(double value) {
  SetEditorAdjustment(editor_state_.blacks_, value, -100.0, 100.0);
}

void EditorController::SetEditorWhites(double value) {
  SetEditorAdjustment(editor_state_.whites_, value, -100.0, 100.0);
}

void EditorController::SetEditorShadows(double value) {
  SetEditorAdjustment(editor_state_.shadows_, value, -100.0, 100.0);
}

void EditorController::SetEditorHighlights(double value) {
  SetEditorAdjustment(editor_state_.highlights_, value, -100.0, 100.0);
}

void EditorController::SetEditorSharpen(double value) {
  SetEditorAdjustment(editor_state_.sharpen_, value, -100.0, 100.0);
}

void EditorController::SetEditorClarity(double value) {
  SetEditorAdjustment(editor_state_.clarity_, value, -100.0, 100.0);
}

void EditorController::InitializeEditorLuts() {
  editor_lut_paths_.clear();
  editor_lut_options_.clear();

  editor_lut_paths_.push_back("");
  editor_lut_options_.push_back(QVariantMap{{"text", PL_TEXT("None").Render()}, {"value", 0}});

  const auto appLutsDir = std::filesystem::path(
      QCoreApplication::applicationDirPath().toStdWString()) / "LUTs";
  const auto srcLutsDir = std::filesystem::path(CONFIG_PATH) / "LUTs";
  const auto lutsDir    = std::filesystem::is_directory(appLutsDir) ? appLutsDir : srcLutsDir;
  const auto lutFiles   = ListCubeLutsInDir(lutsDir);
  for (const auto& path : lutFiles) {
    editor_lut_paths_.push_back(PathToUtf8(path));
    editor_lut_options_.push_back(
        QVariantMap{{"text", QString::fromStdWString(path.filename().wstring())},
                    {"value", static_cast<int>(editor_lut_paths_.size() - 1)}});
  }

  editor_lut_index_ = LutIndexForPath(editor_state_.lut_path_);
  if (!editor_lut_paths_.empty()) {
    editor_state_.lut_path_ = editor_lut_paths_[static_cast<size_t>(editor_lut_index_)];
  }
}

auto EditorController::LutIndexForPath(const std::string& lutPath) const -> int {
  if (editor_lut_paths_.empty()) return 0;
  if (lutPath.empty()) return 0;

  for (size_t i = 0; i < editor_lut_paths_.size(); ++i) {
    if (editor_lut_paths_[i] == lutPath) {
      return static_cast<int>(i);
    }
  }

  const auto target = Utf8OrNativeToPath(lutPath).filename().wstring();
  for (size_t i = 0; i < editor_lut_paths_.size(); ++i) {
    if (Utf8OrNativeToPath(editor_lut_paths_[i]).filename().wstring() == target) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

auto EditorController::LoadEditorStateFromPipeline() -> bool {
  auto exec = editor_pipeline_guard_ ? editor_pipeline_guard_->pipeline_ : nullptr;
  if (!exec) return false;

  auto ReadFloat = [](const PipelineStage& stage, OperatorType type,
                      const char* key) -> std::optional<float> {
    const auto op = stage.GetOperator(type);
    if (!op.has_value() || op.value() == nullptr) return std::nullopt;
    const auto j = op.value()->ExportOperatorParams();
    if (!j.contains("params")) return std::nullopt;
    const auto& params = j["params"];
    if (!params.contains(key)) return std::nullopt;
    try { return params[key].get<float>(); } catch (...) { return std::nullopt; }
  };

  auto ReadNestedFloat = [](const PipelineStage& stage, OperatorType type,
                            const char* key1, const char* key2) -> std::optional<float> {
    const auto op = stage.GetOperator(type);
    if (!op.has_value() || op.value() == nullptr) return std::nullopt;
    const auto j = op.value()->ExportOperatorParams();
    if (!j.contains("params")) return std::nullopt;
    const auto& params = j["params"];
    if (!params.contains(key1)) return std::nullopt;
    const auto& inner = params[key1];
    if (!inner.contains(key2)) return std::nullopt;
    try { return inner[key2].get<float>(); } catch (...) { return std::nullopt; }
  };

  auto ReadString = [](const PipelineStage& stage, OperatorType type,
                       const char* key) -> std::optional<std::string> {
    const auto op = stage.GetOperator(type);
    if (!op.has_value() || op.value() == nullptr) return std::nullopt;
    const auto j = op.value()->ExportOperatorParams();
    if (!j.contains("params")) return std::nullopt;
    const auto& params = j["params"];
    if (!params.contains(key)) return std::nullopt;
    try { return params[key].get<std::string>(); } catch (...) { return std::nullopt; }
  };

  const auto& basic  = exec->GetStage(PipelineStageName::Basic_Adjustment);
  const auto& color  = exec->GetStage(PipelineStageName::Color_Adjustment);
  const auto& detail = exec->GetStage(PipelineStageName::Detail_Adjustment);

  EditorState loaded;
  if (const auto v = ReadFloat(basic, OperatorType::EXPOSURE, "exposure"); v.has_value())
    loaded.exposure_ = v.value();
  if (const auto v = ReadFloat(basic, OperatorType::CONTRAST, "contrast"); v.has_value())
    loaded.contrast_ = v.value();
  if (const auto v = ReadFloat(basic, OperatorType::BLACK, "black"); v.has_value())
    loaded.blacks_ = v.value();
  if (const auto v = ReadFloat(basic, OperatorType::WHITE, "white"); v.has_value())
    loaded.whites_ = v.value();
  if (const auto v = ReadFloat(basic, OperatorType::SHADOWS, "shadows"); v.has_value())
    loaded.shadows_ = v.value();
  if (const auto v = ReadFloat(basic, OperatorType::HIGHLIGHTS, "highlights"); v.has_value())
    loaded.highlights_ = v.value();
  if (const auto v = ReadFloat(color, OperatorType::SATURATION, "saturation"); v.has_value())
    loaded.saturation_ = v.value();
  if (const auto v = ReadFloat(color, OperatorType::TINT, "tint"); v.has_value())
    loaded.tint_ = v.value();
  if (const auto v = ReadNestedFloat(detail, OperatorType::SHARPEN, "sharpen", "offset");
      v.has_value())
    loaded.sharpen_ = v.value();
  if (const auto v = ReadFloat(detail, OperatorType::CLARITY, "clarity"); v.has_value())
    loaded.clarity_ = v.value();
  if (const auto lut = ReadString(color, OperatorType::LMT, "ocio_lmt");
      lut.has_value() && !lut->empty()) {
    loaded.lut_path_ = *lut;
  } else {
    loaded.lut_path_.clear();
  }

  editor_state_     = loaded;
  editor_lut_index_ = LutIndexForPath(editor_state_.lut_path_);
  if (!editor_lut_paths_.empty()) {
    editor_state_.lut_path_ = editor_lut_paths_[static_cast<size_t>(editor_lut_index_)];
  }
  return true;
}

void EditorController::SetupEditorPipeline() {
  auto proj = project_->handler().project();
  if (!editor_pipeline_guard_ || !editor_pipeline_guard_->pipeline_ || !proj) {
    throw std::runtime_error("Editor services are unavailable.");
  }

  auto imageDesc = proj->GetImagePoolService()->Read<std::shared_ptr<Image>>(
      editor_image_id_, [](const std::shared_ptr<Image>& img) { return img; });
  auto bytes = ByteBufferLoader::LoadFromImage(imageDesc);
  if (!bytes) {
    throw std::runtime_error("Failed to load image bytes.");
  }

  editor_base_task_                                     = PipelineTask{};
  editor_base_task_.input_                              = std::make_shared<ImageBuffer>(std::move(*bytes));
  editor_base_task_.pipeline_executor_                  = editor_pipeline_guard_->pipeline_;
  editor_base_task_.options_.is_blocking_               = true;
  editor_base_task_.options_.is_callback_               = false;
  editor_base_task_.options_.is_seq_callback_           = false;
  editor_base_task_.options_.task_priority_              = 0;
  editor_base_task_.options_.render_desc_.render_type_  = RenderType::FAST_PREVIEW;

  auto exec           = editor_pipeline_guard_->pipeline_;
  auto& global_params = exec->GetGlobalParams();
  auto& loading       = exec->GetStage(PipelineStageName::Image_Loading);

  if (!loading.GetOperator(OperatorType::RAW_DECODE).has_value()) {
    const nlohmann::json decode_params = pipeline_defaults::MakeDefaultRawDecodeParams();
    loading.SetOperator(OperatorType::RAW_DECODE, decode_params);
  }
  if (!loading.GetOperator(OperatorType::LENS_CALIBRATION).has_value()) {
    const nlohmann::json lens_params = pipeline_defaults::MakeDefaultLensCalibParams();
    loading.SetOperator(OperatorType::LENS_CALIBRATION, lens_params, global_params);
    loading.EnableOperator(OperatorType::LENS_CALIBRATION,
                           lens_params["lens_calib"].value("enabled", true), global_params);
  }

  // Inject pre-extracted raw metadata so downstream operators resolve eagerly.
  if (imageDesc && imageDesc->HasRawColorContext()) {
    exec->InjectRawMetadata(imageDesc->GetRawColorContext());
  }

  exec->SetExecutionStages();
}

void EditorController::ApplyEditorStateToPipeline() {
  if (!editor_pipeline_guard_ || !editor_pipeline_guard_->pipeline_) return;

  auto  exec         = editor_pipeline_guard_->pipeline_;
  auto& globalParams = exec->GetGlobalParams();

  auto& basic = exec->GetStage(PipelineStageName::Basic_Adjustment);
  basic.SetOperator(OperatorType::EXPOSURE,   {{"exposure",   editor_state_.exposure_}},   globalParams);
  basic.SetOperator(OperatorType::CONTRAST,   {{"contrast",   editor_state_.contrast_}},   globalParams);
  basic.SetOperator(OperatorType::BLACK,      {{"black",      editor_state_.blacks_}},     globalParams);
  basic.SetOperator(OperatorType::WHITE,      {{"white",      editor_state_.whites_}},     globalParams);
  basic.SetOperator(OperatorType::SHADOWS,    {{"shadows",    editor_state_.shadows_}},    globalParams);
  basic.SetOperator(OperatorType::HIGHLIGHTS, {{"highlights", editor_state_.highlights_}}, globalParams);

  auto& color = exec->GetStage(PipelineStageName::Color_Adjustment);
  color.SetOperator(OperatorType::SATURATION, {{"saturation", editor_state_.saturation_}}, globalParams);
  color.SetOperator(OperatorType::TINT,       {{"tint",       editor_state_.tint_}},       globalParams);
  color.SetOperator(OperatorType::LMT,        {{"ocio_lmt",   editor_state_.lut_path_}},   globalParams);

  auto& detail = exec->GetStage(PipelineStageName::Detail_Adjustment);
  detail.SetOperator(OperatorType::SHARPEN,  {{"sharpen", {{"offset", editor_state_.sharpen_}}}}, globalParams);
  detail.SetOperator(OperatorType::CLARITY,  {{"clarity", editor_state_.clarity_}}, globalParams);

  editor_pipeline_guard_->dirty_ = true;
}

void EditorController::QueueEditorRender(RenderType renderType) {
  if (!editor_active_ || !editor_scheduler_ || !editor_pipeline_guard_) return;

  editor_pending_state_       = editor_state_;
  editor_pending_render_type_ = renderType;
  editor_has_pending_render_  = true;

  if (!editor_busy_) {
    editor_busy_ = true;
    emit EditorStateChanged();
  }

  if (!editor_render_inflight_) {
    StartNextEditorRender();
  }
}

void EditorController::StartNextEditorRender() {
  if (!editor_has_pending_render_ || !editor_scheduler_ || !editor_pipeline_guard_ ||
      !editor_base_task_.pipeline_executor_) {
    return;
  }

  editor_has_pending_render_ = false;
  editor_state_              = editor_pending_state_;

  try {
    ApplyEditorStateToPipeline();
  } catch (...) {
    editor_status_text_ = PL_TEXT("Failed to apply editor pipeline state.");
    editor_busy_   = false;
    emit EditorStateChanged();
    return;
  }

  PipelineTask task                       = editor_base_task_;
  task.options_.render_desc_.render_type_ = editor_pending_render_type_;
  task.options_.is_blocking_              = true;
  task.options_.is_callback_              = false;
  task.options_.is_seq_callback_          = false;
  task.options_.task_priority_            = 0;

  auto promise = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
  auto future  = promise->get_future();
  task.result_ = std::move(promise);

  editor_render_inflight_ = true;
  editor_status_text_     = PL_TEXT("Rendering preview...");
  emit EditorStateChanged();

  editor_scheduler_->ScheduleTask(std::move(task));
  editor_render_future_ = std::move(future);
  EnsureEditorPollTimer();
  if (editor_poll_timer_ && !editor_poll_timer_->isActive()) {
    editor_poll_timer_->start();
  }
}

void EditorController::PollEditorRender() {
  if (!editor_render_future_.has_value()) {
    if (editor_poll_timer_ && editor_poll_timer_->isActive() && !editor_render_inflight_) {
      editor_poll_timer_->stop();
    }
    return;
  }

  if (editor_render_future_->wait_for(0ms) != std::future_status::ready) {
    return;
  }

  std::shared_ptr<ImageBuffer> result;
  try {
    result = editor_render_future_->get();
  } catch (...) {
    result.reset();
  }
  editor_render_future_.reset();
  editor_render_inflight_ = false;

  if (!UpdateEditorPreviewFromBuffer(result)) {
    editor_status_text_ = PL_TEXT("Preview render did not produce an image.");
  } else {
    editor_status_text_ = PL_TEXT("Preview ready.");
  }

  if (editor_has_pending_render_) {
    StartNextEditorRender();
    return;
  }

  editor_busy_ = false;
  emit EditorStateChanged();

  if (editor_poll_timer_ && editor_poll_timer_->isActive()) {
    editor_poll_timer_->stop();
  }
}

void EditorController::EnsureEditorPollTimer() {
  if (editor_poll_timer_) return;
  editor_poll_timer_ = new QTimer(this);
  editor_poll_timer_->setInterval(16);
  QObject::connect(editor_poll_timer_, &QTimer::timeout, this, [this]() { PollEditorRender(); });
}

void EditorController::FinalizeEditorSession(bool persistChanges) {
  if (!editor_pipeline_guard_) {
    editor_active_ = false;
    editor_busy_   = false;
    return;
  }

  if (editor_render_future_.has_value()) {
    try {
      editor_render_future_->wait();
      auto last = editor_render_future_->get();
      (void)UpdateEditorPreviewFromBuffer(last);
    } catch (...) {
    }
    editor_render_future_.reset();
  }

  editor_has_pending_render_ = false;
  editor_render_inflight_    = false;
  if (editor_poll_timer_ && editor_poll_timer_->isActive()) {
    editor_poll_timer_->stop();
  }

  const auto finishedElement = editor_element_id_;
  const auto finishedImage   = editor_image_id_;

  const auto& psvc = project_->handler().pipeline_service();
  bool        update_hdr_flag = false;
  bool        is_hdr_export   = false;
  if (psvc) {
    try {
      if (persistChanges) {
        ApplyEditorStateToPipeline();
        editor_pipeline_guard_->dirty_ = true;
        is_hdr_export = IsHdrExportEotf(
            editor_pipeline_guard_->pipeline_->GetGlobalParams().to_output_params_.eotf_);
        update_hdr_flag = true;
      } else {
        editor_pipeline_guard_->dirty_ = false;
      }
      psvc->SavePipeline(editor_pipeline_guard_);
      if (persistChanges) {
        psvc->Sync();
      }
    } catch (...) {
    }
  }

  auto proj = project_->handler().project();
  if (persistChanges && proj) {
    try {
      if (update_hdr_flag) {
        library_->PersistImageHdrFlag(finishedElement, finishedImage, is_hdr_export);
      }
      proj->GetImagePoolService()->SyncWithStorage();
      QString ignored_error;
      if (project_->handler().PersistCurrentProjectState()) {
        (void)project_->handler().PackageCurrentProjectFiles(&ignored_error);
      }
    } catch (...) {
    }
  }

  const auto& tsvc = project_->handler().thumbnail_service();
  if (persistChanges && tsvc && finishedElement != 0 && finishedImage != 0) {
    try {
      tsvc->InvalidateThumbnail(finishedElement);
    } catch (...) {
    }
    if (!library_->thumbs().RefreshCurrentThumbnail(finishedElement, finishedImage)) {
      library_->thumbs().UpdateThumbnailState(finishedElement, QString(), false, false);
    }
  }

  editor_pipeline_guard_.reset();
  editor_base_task_  = PipelineTask{};
  editor_active_     = false;
  editor_busy_       = false;
  editor_element_id_ = 0;
  editor_image_id_   = 0;
  editor_title_text_  = {};
  editor_status_text_ = persistChanges ? PL_TEXT("Edits saved.") : PL_TEXT("Editor closed.");
  if (!editor_preview_url_.isEmpty()) {
    editor_preview_url_.clear();
    emit EditorPreviewChanged();
  }
  emit EditorStateChanged();
}

auto EditorController::UpdateEditorPreviewFromBuffer(
    const std::shared_ptr<ImageBuffer>& buffer) -> bool {
  if (!buffer) return false;

  QString dataUrl;
  try {
    if (!buffer->cpu_data_valid_ && buffer->gpu_data_valid_) {
      buffer->SyncToCPU();
    }
    if (!buffer->cpu_data_valid_) return false;

    QImage image = MatRgba32fToQImageCopy(buffer->GetCPUData());
    if (image.isNull()) return false;
    QImage scaled = image.scaled(1180, 760, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    dataUrl = DataUrlFromImage(scaled);
  } catch (...) {
    return false;
  }

  if (dataUrl.isEmpty()) return false;

  if (editor_preview_url_ != dataUrl) {
    editor_preview_url_ = dataUrl;
    emit EditorPreviewChanged();
  }
  return true;
}

void EditorController::SetEditorAdjustment(float& field, double value,
                                           double minValue, double maxValue) {
  if (!editor_active_) return;
  const float clamped = ClampToRange(value, minValue, maxValue);
  if (NearlyEqual(field, clamped)) return;
  field = clamped;
  emit EditorStateChanged();
  QueueEditorRender(RenderType::FAST_PREVIEW);
}

}  // namespace alcedo::ui

#undef PL_TEXT
