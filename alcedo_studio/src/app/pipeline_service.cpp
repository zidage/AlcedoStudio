//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "image/metadata_extractor.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {
void ValidateProductDocument(const PipelineDocument& document, sl_element_id_t id) {
  const auto graph_errors = document.Graph().Validate();
  if (!graph_errors.empty()) {
    throw std::runtime_error("PipelineMgmtService: invalid graph for element " +
                             std::to_string(id) + ": " + graph_errors.front().message);
  }
  const auto backbone_errors = document.Graph().ValidateImageBackbone();
  if (!backbone_errors.empty()) {
    throw std::runtime_error("PipelineMgmtService: invalid image backbone for element " +
                             std::to_string(id) + ": " + backbone_errors.front().message);
  }
}

auto LoadPipelineDocument(ElementStore& store, sl_element_id_t id)
    -> std::shared_ptr<PipelineDocument> {
  const auto stored = store.GetPipelineJsonByElementId(id);
  if (!stored.has_value()) {
    return std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  }

  auto document = std::make_shared<PipelineDocument>(PipelineDocument::FromJson(*stored));
  ValidateProductDocument(*document, id);
  return document;
}

void ResetToDefaults(OperatorParams& params) {
  // OperatorParams has const members, so it is not assignable.
  // Reinitialize in-place to restore default values.
  std::destroy_at(&params);
  std::construct_at(&params);
}

void EnsureDefaultOutputTransform(CPUPipelineExecutor& exec) {
  auto& global_params = exec.GetGlobalParams();
  auto& output_stage  = exec.GetStage(PipelineStageName::Output_Transform);

  // Older stored pipelines (or partially-initialized ones) might miss the ODT descriptor.
  // Without it, the GPU path won't have precomputed ODT tables and can render black.
  if (!output_stage.GetOperator(OperatorType::ODT).has_value()) {
    const nlohmann::json output_params = pipeline_defaults::MakeDefaultODTParams();
    output_stage.SetOperator(OperatorType::ODT, output_params, global_params);
  }
}

void EnsureDefaultColorTemp(CPUPipelineExecutor& exec) {
  auto& global_params = exec.GetGlobalParams();
  auto& to_ws_stage   = exec.GetStage(PipelineStageName::To_WorkingSpace);

  if (to_ws_stage.GetOperator(OperatorType::COLOR_TEMP).has_value()) {
    return;
  }

  std::string mode      = "as_shot";
  float       cct       = 6500.0f;
  float       tint      = 0.0f;

  auto&       raw_stage = exec.GetStage(PipelineStageName::Image_Loading);
  auto        raw_entry = raw_stage.GetOperator(OperatorType::RAW_DECODE);
  if (raw_entry.has_value() && raw_entry.value() && raw_entry.value()->op_) {
    const nlohmann::json raw_params = raw_entry.value()->op_->GetParams();
    if (raw_params.contains("raw") && raw_params["raw"].is_object()) {
      const auto& raw = raw_params["raw"];
      if (raw.contains("use_camera_wb") && raw["use_camera_wb"].is_boolean() &&
          !raw["use_camera_wb"].get<bool>()) {
        mode = "custom";
      }
      if (raw.contains("user_wb") && raw["user_wb"].is_number()) {
        cct = std::clamp(raw["user_wb"].get<float>(), 2000.0f, 15000.0f);
      }
    }
  }

  nlohmann::json color_temp_params;
  color_temp_params["color_temp"] = {
      {"mode", mode},       {"custom_cct", cct},    {"custom_tint", tint},
      {"as_shot_cct", cct}, {"as_shot_tint", tint},
  };
  to_ws_stage.SetOperator(OperatorType::COLOR_TEMP, color_temp_params, global_params);
}

void EnsureDefaultRawDecode(CPUPipelineExecutor& exec) {
  auto& loading_stage = exec.GetStage(PipelineStageName::Image_Loading);
  if (loading_stage.GetOperator(OperatorType::RAW_DECODE).has_value()) {
    return;
  }

  const nlohmann::json raw_params = pipeline_defaults::MakeDefaultRawDecodeParams();
  loading_stage.SetOperator(OperatorType::RAW_DECODE, raw_params);
}

void EnsureDefaultLensCalib(CPUPipelineExecutor& exec) {
  auto& global_params = exec.GetGlobalParams();
  auto& loading_stage = exec.GetStage(PipelineStageName::Image_Loading);

  if (!loading_stage.GetOperator(OperatorType::LENS_CALIBRATION).has_value()) {
    const nlohmann::json lens_params = pipeline_defaults::MakeDefaultLensCalibParams();
    loading_stage.SetOperator(OperatorType::LENS_CALIBRATION, lens_params, global_params);
  }

  const auto op = loading_stage.GetOperator(OperatorType::LENS_CALIBRATION);
  if (!op.has_value() || !op.value() || !op.value()->op_) {
    return;
  }

  bool enabled = op.value()->enable_;
  auto params  = op.value()->op_->GetParams();
  if (params.contains("lens_calib") && params["lens_calib"].is_object()) {
    enabled = params["lens_calib"].value("enabled", enabled);
  }

  if (!params.contains("lens_calib") || !params["lens_calib"].is_object()) {
    params["lens_calib"] = nlohmann::json::object();
  }
  params["lens_calib"]["enabled"] = enabled;

  // Keep the operator-local descriptor and the stage-level enable bit in lockstep.
  // Stored pipelines may carry an older mismatch between the two; nested params are
  // the durable source of truth because they are part of the serialized operator state.
  loading_stage.SetOperator(OperatorType::LENS_CALIBRATION, params, global_params);
  loading_stage.EnableOperator(OperatorType::LENS_CALIBRATION, enabled, global_params);
}

void ResyncGlobalParamsFromOperators(CPUPipelineExecutor& exec) {
  // Global params are consumed/mutated during GPU parameter conversion (dirty flags cleared).
  // Cached pipelines also release GPU resources when returned to the service.
  // Rebuild global params from operator params so ODT/LMT GPU resources are re-uploaded.
  auto& global_params = exec.GetGlobalParams();
  ResetToDefaults(global_params);

  for (int i = 0; i < static_cast<int>(PipelineStageName::Stage_Count); ++i) {
    auto& stage = exec.GetStage(static_cast<PipelineStageName>(i));
    for (auto& [op_type, op_entry] : stage.GetAllOperators()) {
      (void)op_type;
      if (!op_entry.op_) {
        continue;
      }
      op_entry.op_->SetGlobalParams(global_params);
    }
  }
}

void ResetTransientPreviewState(CPUPipelineExecutor& exec) {
  exec.SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm::Bilinear);
  exec.SetRenderRegion(0, 0, 1.0f);
  exec.SetRenderRes(false, 4096);
  exec.SetForceCPUOutput(false);
  exec.SetEnableCache(true);
  exec.SetDecodeRes(DecodeRes::FULL);
}

struct LoadedRootState {
  PipelineDocument                      document;
  std::optional<RawRuntimeColorContext> raw_color_context;
};

auto TryDecodeRootState(const nlohmann::json& encoded, sl_element_id_t element_id,
                        const root_id_t& expected_root_id) -> std::optional<LoadedRootState> {
  try {
    const auto root = DecodePipelineRootState(encoded);
    if (root.element_id != element_id) {
      return std::nullopt;
    }
    if (ComputeRootId(root.element_id, root.document, root.raw_color_context) !=
        expected_root_id) {
      return std::nullopt;
    }
    LoadedRootState loaded;
    loaded.document = ClonePipelineDocument(root.document);
    if (root.raw_color_context.has_value()) {
      RawRuntimeColorContext context;
      if (!RawColorContextFromJson(*root.raw_color_context, context)) {
        return std::nullopt;
      }
      loaded.raw_color_context = std::move(context);
    }
    return loaded;
  } catch (...) {
    return std::nullopt;
  }
}

auto TryDecodeCheckpoint(const nlohmann::json& encoded)
    -> std::optional<PipelineDocumentCheckpoint> {
  try {
    return DecodePipelineDocumentCheckpoint(encoded);
  } catch (...) {
    return std::nullopt;
  }
}

auto MakeSerializedPipelineState(const PipelineGuard& guard) -> nlohmann::json {
  return EncodePipelineDocumentCheckpoint(guard.root_id_, guard.working_head_commit_hash(),
                                          guard.transaction_chain_hash(), *guard.document_);
}

void BindLiveDocument(PipelineGuard& guard, PipelineDocument document) {
  BindLivePipelineDocument(guard, std::move(document));
}

void CacheRootDocument(PipelineGuard& guard, const PipelineDocument& document) {
  guard.root_document_ = std::make_shared<PipelineDocument>(ClonePipelineDocument(document));
}

void BindDevelopData(PipelineDocument& document, const RawRuntimeColorContext& raw_color_context) {
  auto* develop = document.Develop();
  if (develop == nullptr) {
    return;
  }
  auto payload = develop->Params().Params();
  auto next    = payload;
  BindDevelopCameraProfile(next, raw_color_context);
  if (next != payload) {
    develop->Params().ReplaceParams(std::move(next));
  }
}

auto FirstParentCommits(const CommitGraph& graph, head_commit_hash_t head)
    -> std::vector<EditCommit> {
  return FirstParentCommitsForHead(graph, head);
}

void SetPipelineHistoryState(PipelineGuard& guard, const CommitGraph& graph) {
  guard.root_id_                          = graph.GetRootId();
  guard.serialized_state_needs_writeback_ = false;
  guard.commit_graph_                     = std::make_shared<CommitGraph>(graph);
}

void ImportSerializedPipelineState(CPUPipelineExecutor& exec, const nlohmann::json& pipeline_params,
                                   const std::optional<RawRuntimeColorContext>& raw_color_context,
                                   AcceleratorBackendPreference accelerator_preference) {
  exec.ImportPipelineParams(pipeline_params);
  exec.SetAcceleratorBackendPreference(accelerator_preference);
  ResetTransientPreviewState(exec);
  EnsureDefaultOutputTransform(exec);
  EnsureDefaultRawDecode(exec);
  EnsureDefaultColorTemp(exec);
  EnsureDefaultLensCalib(exec);
  ResyncGlobalParamsFromOperators(exec);
  if (raw_color_context.has_value()) {
    exec.InjectRawMetadata(*raw_color_context);
  }
}

auto ReplayLiveDocumentFromRoot(PipelineGuard& guard, const CommitGraph& graph,
                                const LoadedRootState& root_state, head_commit_hash_t head,
                                MaskStore* mask_store, std::string* error) -> bool {
  const auto commits = FirstParentCommits(graph, head);
  PipelineHistoryApplyContext context;
  context.mask_store = mask_store;
  auto replayed = ReplayPipelineDocumentFromRoot(root_state.document, commits, error, context);
  if (!replayed.has_value()) {
    return false;
  }
  if (!VerifyPersistentMaskAssets(*replayed, mask_store, error)) {
    return false;
  }
  BindLiveDocument(guard, std::move(*replayed));
  if (root_state.raw_color_context.has_value()) {
    guard.pipeline_->InjectRawMetadata(*root_state.raw_color_context);
  }
  if (!ApplyVersionHeadToLivePipeline(*guard.pipeline_, graph, head, error)) {
    return false;
  }
  guard.pipeline_->SetExecutionStages();
  return true;
}

}  // namespace

void BindLivePipelineDocument(PipelineGuard& guard, PipelineDocument document) {
  guard.document_ = std::make_shared<PipelineDocument>(std::move(document));
  if (guard.pipeline_) {
    guard.pipeline_->SetPipelineDocument(guard.document_, false);
  }
}

void PipelineMgmtService::InjectImageRawMetadata(CPUPipelineExecutor& executor, const Image& image) {
  if (image.HasRawColorContext()) {
    executor.InjectRawMetadata(MetadataExtractor::ReadRawColorContextForRender(image));
  }
}

void PipelineMgmtService::HandleEviction(sl_element_id_t evicted_id) {
  // If the would-be evicted pipeline is pinned, keep it and evict another entry instead.
  // This avoids unbounded cache growth during batch export when a pipeline is temporarily pinned.
  // Only cache metadata is protected by lock_. Storage writes and executor cleanup happen after
  // the cache lock is released so the cache cannot serialize with a render or DuckDB operation.
  std::shared_ptr<PipelineGuard> pipeline_guard;
  sl_element_id_t                candidate = evicted_id;
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    const size_t max_attempts = loaded_pipelines_.empty() ? 1 : (loaded_pipelines_.size() + 1);

    for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
      auto it = loaded_pipelines_.find(candidate);
      if (it == loaded_pipelines_.end()) {
        return;
      }

      pipeline_guard = it->second;
      if (pipeline_guard->pin_count_ == 0) {
        pipeline_guard->pinned_ = false;
        loaded_pipelines_.erase(it);
        break;
      }

      // Pinned: put it back into the LRU and evict a different entry.
      auto next = pipeline_cache_.RecordAccess_WithEvict(candidate, candidate);
      if (!next.has_value()) {
        pipeline_guard.reset();
        return;
      }
      candidate = next.value();
      pipeline_guard.reset();
    }

    if (!pipeline_guard) {
      // Fallback: if everything is pinned, allow temporary growth to avoid evicting in-use
      // pipelines.
      auto keys = pipeline_cache_.GetLRUKeys();
      pipeline_cache_.Resize(static_cast<uint32_t>(keys.size() + 5));
      pipeline_cache_.RecordAccess(evicted_id, evicted_id);
      return;
    }
  }

  try {
    SyncDirtyPipelineDocument(pipeline_guard);
  } catch (...) {
    // The guard was removed from the cache before the storage operation so the cache lock is not
    // held across DuckDB I/O. Put it back when the document cannot be written; otherwise an
    // uncommitted edit would disappear with the evicted entry.
    std::unique_lock<std::mutex> cache_lock(lock_);
    const auto keys = pipeline_cache_.GetLRUKeys();
    pipeline_cache_.Resize(static_cast<uint32_t>(keys.size() + 1));
    pipeline_cache_.RecordAccess(pipeline_guard->id_, pipeline_guard->id_);
    pipeline_guard->pinned_       = false;
    pipeline_guard->live_ready_   = true;
    pipeline_guard->initializing_ = false;
    pipeline_guard->load_error_   = nullptr;
    loaded_pipelines_[pipeline_guard->id_] = pipeline_guard;
    cache_cv_.notify_all();
    throw;
  }
  if (pipeline_guard->pipeline_) {
    std::unique_lock<std::mutex> render_guard(pipeline_guard->pipeline_->GetRenderLock());
    // Clear intermediate buffers before removing from cache to ensure timely memory release.
    pipeline_guard->pipeline_->ClearAllIntermediateBuffers();
  }
  pipeline_guard->live_ready_ = false;
}

void PipelineMgmtService::CleanupIdlePipelineResources(
    const std::shared_ptr<PipelineGuard>& pipeline) {
  if (!pipeline || !pipeline->pipeline_) {
    return;
  }
  std::unique_lock<std::mutex> render_guard(pipeline->pipeline_->GetRenderLock());
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    if (pipeline->pin_count_ > 0) {
      return;
    }
    pipeline->live_ready_ = false;
    cache_cv_.notify_all();
  }
  try {
    pipeline->pipeline_->ClearAllIntermediateBuffers();
    pipeline->pipeline_->ResetExecutionStages();
  } catch (...) {
  }
}

auto PipelineMgmtService::WaitUntilPinCount(const std::shared_ptr<PipelineGuard>& pipeline,
                                            size_t                                expected,
                                            std::chrono::milliseconds             timeout) -> bool {
  if (!pipeline) {
    return false;
  }
  std::unique_lock<std::mutex> cache_lock(lock_);
  return cache_cv_.wait_for(cache_lock, timeout,
                            [&] { return pipeline->pin_count_ == expected; });
}

auto PipelineMgmtService::LoadPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard> {
  std::shared_ptr<PipelineGuard> cached;
  bool                           need_reinit = false;
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    const auto                   it = loaded_pipelines_.find(id);
    if (it != loaded_pipelines_.end() && it->second) {
      cached = it->second;
      cached->pin_count_++;
      cached->pinned_ = true;
      cached->id_     = id;
      ++pipeline_load_count_;
      pipeline_cache_.AccessElement(id);
      cache_cv_.notify_all();
      for (;;) {
        if (cached->load_error_) {
          if (cached->pin_count_ > 0) {
            cached->pin_count_--;
          }
          cached->pinned_ = cached->pin_count_ > 0;
          cache_cv_.notify_all();
          std::rethrow_exception(cached->load_error_);
        }
        if (cached->live_ready_) {
          return cached;
        }
        if (cached->initializing_) {
          cache_cv_.wait(cache_lock);
          continue;
        }
        if (cached->pipeline_) {
          cached->initializing_ = true;
          need_reinit           = true;
          break;
        }
        cache_cv_.wait(cache_lock);
      }
    }
  }

  if (cached && need_reinit) {
    try {
      std::unique_lock<std::mutex> render_guard(cached->pipeline_->GetRenderLock());
      cached->pipeline_->SetBoundFile(id);
      cached->pipeline_->SetAcceleratorBackendPreference(accelerator_preference_);
      cached->pipeline_->SetExecutionStages();
      ResetTransientPreviewState(*cached->pipeline_);

      EnsureDefaultOutputTransform(*cached->pipeline_);
      EnsureDefaultRawDecode(*cached->pipeline_);
      EnsureDefaultColorTemp(*cached->pipeline_);
      EnsureDefaultLensCalib(*cached->pipeline_);
      ResyncGlobalParamsFromOperators(*cached->pipeline_);
      storage_->RememberLivePipeline(id, cached->pipeline_);
      {
        std::unique_lock<std::mutex> cache_lock(lock_);
        cached->live_ready_    = true;
        cached->initializing_  = false;
        cached->load_error_    = nullptr;
        cache_cv_.notify_all();
      }
      return cached;
    } catch (...) {
      {
        std::unique_lock<std::mutex> cache_lock(lock_);
        cached->load_error_   = std::current_exception();
        cached->initializing_ = false;
        cache_cv_.notify_all();
      }
      ReleasePipelineUse(cached);
      throw;
    }
  }

  auto pipeline_guard = std::make_shared<PipelineGuard>();
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    const auto                   it = loaded_pipelines_.find(id);
    if (it != loaded_pipelines_.end() && it->second) {
      cached = it->second;
      cached->pin_count_++;
      cached->pinned_ = true;
      ++pipeline_load_count_;
      pipeline_cache_.AccessElement(id);
      cache_cv_.notify_all();
      while (!cached->live_ready_ && !cached->load_error_) {
        cache_cv_.wait(cache_lock);
      }
      if (cached->load_error_) {
        if (cached->pin_count_ > 0) {
          cached->pin_count_--;
        }
        cached->pinned_ = cached->pin_count_ > 0;
        cache_cv_.notify_all();
        std::rethrow_exception(cached->load_error_);
      }
      return cached;
    }
    pipeline_guard->id_            = id;
    pipeline_guard->pinned_        = true;
    pipeline_guard->pin_count_     = 1;
    pipeline_guard->live_ready_    = false;
    pipeline_guard->initializing_  = true;
    loaded_pipelines_[id]          = pipeline_guard;
    ++pipeline_construct_count_;
    ++pipeline_load_count_;
    cache_cv_.notify_all();
  }

  try {
    std::shared_ptr<CPUPipelineExecutor> pipeline;
    try {
      pipeline = storage_->GetLivePipeline(id);
    } catch (std::exception& e) {
      throw std::runtime_error(
          "[ERROR] PipelineMgmtService: Failed to load pipeline from storage for element ID " +
          std::to_string(id) + ": " + e.what());
    }
    if (pipeline == nullptr) {
      pipeline = std::make_shared<CPUPipelineExecutor>();
    }

    {
      std::unique_lock<std::mutex> render_guard(pipeline->GetRenderLock());
      pipeline->SetBoundFile(id);
      pipeline->SetAcceleratorBackendPreference(accelerator_preference_);
      ResetTransientPreviewState(*pipeline);
      EnsureDefaultOutputTransform(*pipeline);
      EnsureDefaultRawDecode(*pipeline);
      EnsureDefaultColorTemp(*pipeline);
      EnsureDefaultLensCalib(*pipeline);
      ResyncGlobalParamsFromOperators(*pipeline);
      pipeline->SetExecutionStages();
    }

    pipeline_guard->pipeline_ = std::move(pipeline);
    pipeline_guard->document_ = LoadPipelineDocument(storage_->GetElementStore(), id);
    pipeline_guard->pipeline_->SetPipelineDocument(pipeline_guard->document_, false);
    ValidateProductDocument(*pipeline_guard->document_, id);
    pipeline_guard->dirty_ = false;

    std::optional<sl_element_id_t> evicted;
    {
      std::unique_lock<std::mutex> cache_lock(lock_);
      pipeline_guard->live_ready_   = true;
      pipeline_guard->initializing_ = false;
      evicted                       = pipeline_cache_.RecordAccess_WithEvict(id, id);
      if (!evicted.has_value() && loaded_pipelines_.size() + 1 > default_cache_capacity_) {
        pipeline_cache_.Resize(loaded_pipelines_.size() - 1);
      }
      cache_cv_.notify_all();
    }
    if (evicted.has_value()) {
      HandleEviction(evicted.value());
    }
    storage_->RememberLivePipeline(id, pipeline_guard->pipeline_);
    return pipeline_guard;
  } catch (...) {
    {
      std::unique_lock<std::mutex> cache_lock(lock_);
      pipeline_guard->load_error_   = std::current_exception();
      pipeline_guard->initializing_ = false;
      const auto it                 = loaded_pipelines_.find(id);
      if (it != loaded_pipelines_.end() && it->second == pipeline_guard) {
        loaded_pipelines_.erase(it);
      }
      pipeline_cache_.RemoveRecord(id);
      cache_cv_.notify_all();
    }
    throw;
  }
}

void PipelineMgmtService::SyncPipelineDocument(const std::shared_ptr<PipelineGuard>& pipeline) {
  if (!pipeline || !pipeline->pipeline_ || !pipeline->document_) {
    throw std::invalid_argument("PipelineMgmtService: cannot save an incomplete PipelineDocument");
  }

  // Keep the document lock held through serialization and the storage write. A later edit can
  // then either wait for this save and mark the guard dirty, or serialize after this write; it
  // cannot be accidentally covered by this save's dirty transition.
  std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
  if (pipeline->unsettled_preview_) {
    throw std::runtime_error(
        "PipelineMgmtService: cannot save while an editor preview input is unsettled");
  }
  ValidateProductDocument(*pipeline->document_, pipeline->id_);
  const auto json = pipeline->document_->ToJson();
  storage_->GetElementStore().UpdatePipelineJsonByElementId(pipeline->id_, json);
  pipeline->dirty_ = false;
}

void PipelineMgmtService::SyncDirtyPipelineDocument(
    const std::shared_ptr<PipelineGuard>& pipeline) {
  if (!pipeline || !pipeline->pipeline_ || !pipeline->document_) {
    throw std::invalid_argument("PipelineMgmtService: cannot save an incomplete PipelineDocument");
  }

  std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
  if (pipeline->unsettled_preview_) {
    throw std::runtime_error(
        "PipelineMgmtService: cannot save while an editor preview input is unsettled");
  }
  if (!pipeline->dirty_) {
    return;
  }
  ValidateProductDocument(*pipeline->document_, pipeline->id_);
  const auto json = pipeline->document_->ToJson();
  storage_->GetElementStore().UpdatePipelineJsonByElementId(pipeline->id_, json);
  pipeline->dirty_ = false;
}

void PipelineMgmtService::InitializeImageRoot(const std::shared_ptr<PipelineGuard>& pipeline,
                                              const RawRuntimeColorContext* raw_color_context) {
  if (!pipeline || !pipeline->pipeline_ || !pipeline->document_) {
    throw std::runtime_error("PipelineMgmtService: cannot initialize a null pipeline root");
  }

  std::optional<nlohmann::json> raw_json;
  {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    if (raw_color_context != nullptr) {
      BindDevelopData(*pipeline->document_, *raw_color_context);
      pipeline->pipeline_->InjectRawMetadata(*raw_color_context);
      raw_json = RawColorContextToJson(*raw_color_context);
    }
    ValidateProductDocument(*pipeline->document_, pipeline->id_);
  }

  auto             db_guard = storage_->GetDatabase().GetConnectionGuard();
  auto             db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  auto             state = graph_service.GetImageEditState(pipeline->id_);
  if (!state.has_value()) {
    auto graph = graph_service.CreateRootPipelinePersisted(
        pipeline->id_, *pipeline->document_, raw_json);
    SetPipelineHistoryState(*pipeline, graph);
    CacheRootDocument(*pipeline, *pipeline->document_);
    return;
  }

  auto graph = graph_service.LoadGraph(pipeline->id_);
  if (!graph.has_value()) {
    throw std::runtime_error(
        "PipelineMgmtService: image edit state disappeared while loading root");
  }
  const auto root_encoded =
      graph_service.GetRootSerializedPipelineState(pipeline->id_, graph->GetRootId());
  if (!root_encoded.has_value()) {
    throw std::runtime_error("PipelineMgmtService: immutable root state is missing for image " +
                             std::to_string(pipeline->id_));
  }
  const auto root_state =
      TryDecodeRootState(*root_encoded, pipeline->id_, graph->GetRootId());
  if (!root_state.has_value()) {
    throw std::runtime_error("PipelineMgmtService: immutable root identity is invalid for image " +
                             std::to_string(pipeline->id_));
  }
  SetPipelineHistoryState(*pipeline, *graph);
  CacheRootDocument(*pipeline, root_state->document);
}

auto PipelineMgmtService::LoadEditorPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard> {
  auto pipeline = LoadPipeline(id);
  try {
    InitializeImageRoot(pipeline);

    std::optional<CommitGraph>    graph;
    std::optional<LoadedRootState> root_state;
    {
      auto             db_guard = storage_->GetDatabase().GetConnectionGuard();
      auto             db_lock  = db_guard.Lock();
      CommitGraphStore graph_service(db_guard.conn_);
      graph = graph_service.LoadGraph(id);
      if (!graph.has_value()) {
        throw std::runtime_error(
            "PipelineMgmtService: image edit state is missing after root setup");
      }
      const auto root_encoded_state =
          graph_service.GetRootSerializedPipelineState(id, graph->GetRootId());
      if (!root_encoded_state.has_value()) {
        throw std::runtime_error("PipelineMgmtService: immutable root state is missing for image " +
                                 std::to_string(id));
      }
      root_state = TryDecodeRootState(*root_encoded_state, id, graph->GetRootId());
      if (!root_state.has_value()) {
        throw std::runtime_error("PipelineMgmtService: immutable root state is invalid for image " +
                                 std::to_string(id));
      }

      const auto& state          = graph->GetImageEditState();
      const auto  expected_head  = graph->GetActiveVersionRef().head_commit_hash;
      const auto  expected_chain = graph->ChainHashForHead(expected_head);
      if (state.root_id != graph->GetRootId() ||
          state.materialized_head_commit_hash != expected_head ||
          state.materialized_transaction_chain_hash != expected_chain) {
        throw std::runtime_error(
            "PipelineMgmtService: stored image edit state does not match the active Version");
      }
    }

    // History tip is sole authority. A checkpoint is used only when its root, head,
    // and chain labels match the active Version.
    SetPipelineHistoryState(*pipeline, *graph);
    CacheRootDocument(*pipeline, root_state->document);
    const auto& state                     = graph->GetImageEditState();
    const auto  expected_head             = graph->GetActiveVersionRef().head_commit_hash;
    const auto  expected_chain            = graph->ChainHashForHead(expected_head);
    bool        accepted_serialized_state = false;
    if (state.serialized_pipeline_state.has_value()) {
      const auto stored = TryDecodeCheckpoint(*state.serialized_pipeline_state);
      if (stored.has_value() && stored->root_id == graph->GetRootId() &&
          stored->head_commit_hash == expected_head &&
          stored->transaction_chain_hash == expected_chain) {
        try {
          std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
          BindLiveDocument(*pipeline, ClonePipelineDocument(stored->document));
          if (root_state->raw_color_context.has_value()) {
            pipeline->pipeline_->InjectRawMetadata(*root_state->raw_color_context);
          }
          pipeline->pipeline_->SetAcceleratorBackendPreference(accelerator_preference_);
          ResetTransientPreviewState(*pipeline->pipeline_);
          pipeline->pipeline_->SetExecutionStages();
          accepted_serialized_state = true;
        } catch (...) {
          accepted_serialized_state = false;
        }
      }
    }

    if (!accepted_serialized_state) {
      ++editor_pipeline_history_rebuild_count_;
      std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
      std::string                  replay_error;
      if (!ReplayLiveDocumentFromRoot(*pipeline, *graph, *root_state,
                                      graph->GetActiveVersionRef().head_commit_hash, nullptr,
                                      &replay_error)) {
        throw std::runtime_error(replay_error);
      }
      pipeline->serialized_state_needs_writeback_ = true;
    }
    return pipeline;
  } catch (const std::exception& e) {
    ReleasePipelineUse(pipeline);
    throw std::runtime_error("[ERROR] PipelineMgmtService: editor history validation failed for " +
                             std::to_string(id) + ": " + e.what());
  } catch (...) {
    ReleasePipelineUse(pipeline);
    throw std::runtime_error("[ERROR] PipelineMgmtService: editor history validation failed for " +
                             std::to_string(id) + ": unknown error");
  }
}

void PipelineMgmtService::SavePipeline(std::shared_ptr<PipelineGuard> pipeline) {
  if (!pipeline) {
    return;
  }

  try {
    if (!pipeline->pipeline_ || !pipeline->document_) {
      throw std::invalid_argument("PipelineMgmtService: cannot save an incomplete pipeline guard");
    }

    storage_->RememberLivePipeline(pipeline->id_, pipeline->pipeline_);

    // The document is the only product persistence source. Save it before touching the optional
    // history checkpoint so a failed document write leaves both the live edit and its journal
    // state untouched.
    SyncDirtyPipelineDocument(pipeline);

    bool will_release_last_pin = false;
    {
      std::unique_lock<std::mutex> cache_lock(lock_);
      will_release_last_pin = pipeline->pin_count_ <= 1;
    }
    if (will_release_last_pin && pipeline->serialized_state_needs_writeback_) {
      try {
        nlohmann::json checkpoint;
        {
          std::unique_lock<std::mutex> render_guard(pipeline->pipeline_->GetRenderLock());
          if (pipeline->unsettled_preview_) {
            throw std::runtime_error(
                "PipelineMgmtService: cannot checkpoint an unsettled editor preview");
          }
          ValidateProductDocument(*pipeline->document_, pipeline->id_);
          checkpoint = MakeSerializedPipelineState(*pipeline);
        }

        auto             db_guard = storage_->GetDatabase().GetConnectionGuard();
        auto             db_lock  = db_guard.Lock();
        CommitGraphStore graph_service(db_guard.conn_);
        auto             stored_graph = graph_service.LoadGraph(pipeline->id_);
        if (!stored_graph.has_value()) {
          throw std::runtime_error(
              "PipelineMgmtService: cannot write serialized state without an edit graph");
        }

        // A newly pasted Version only exists in the live graph until this checkpoint.
        // Still compare the graph's last materialized state with DuckDB before writing it so a
        // different writer cannot be silently replaced by this serialized pipeline state.
        const auto& graph = pipeline->commit_graph_ ? *pipeline->commit_graph_ : *stored_graph;

        // Logical head is only the live CommitGraph active Version.
        if (graph.GetElementId() != pipeline->id_ || graph.GetRootId() != pipeline->root_id_) {
          throw std::runtime_error(
              "PipelineMgmtService: live history identity changed before serialized state writeback");
        }

        const auto& stored_state = stored_graph->GetImageEditState();
        const auto& graph_state  = graph.GetImageEditState();
        if (stored_state.root_id != graph_state.root_id ||
            stored_state.active_version_id != graph_state.active_version_id ||
            stored_state.materialized_head_commit_hash !=
                graph_state.materialized_head_commit_hash ||
            stored_state.materialized_transaction_chain_hash !=
                graph_state.materialized_transaction_chain_hash) {
          throw std::runtime_error(
              "PipelineMgmtService: persisted history changed before serialized state writeback");
        }

        const auto materialization =
            graph.CaptureMaterializationWithSerializedPipelineState(checkpoint);
        graph_service.Materialize(materialization);
        if (pipeline->commit_graph_) {
          pipeline->commit_graph_->ApplyMaterializedState(materialization.image_state);
        }
        pipeline->serialized_state_needs_writeback_ = false;
      } catch (...) {
        // This state accelerates editor open but is not the history source of truth. Keep the flag
        // set so the next explicit save retries it without leaking the pipeline pin.
      }
    }

    // SavePipeline is an explicit persistence operation. Its final step only releases this
    // caller's cache pin; lifecycle release never performs storage I/O.
    ReleasePipelineUse(std::move(pipeline));
  } catch (...) {
    // A failed save must still release the caller's pin, but ReleasePipelineUse deliberately
    // leaves dirty_ and the history writeback flag unchanged.
    ReleasePipelineUse(std::move(pipeline));
    throw;
  }
}

void PipelineMgmtService::ReleasePipelineUse(std::shared_ptr<PipelineGuard> pipeline) {
  if (!pipeline) {
    return;
  }

  bool last_pin = false;
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    if (pipeline->pin_count_ > 0) {
      pipeline->pin_count_--;
    }
    last_pin          = pipeline->pin_count_ == 0;
    pipeline->pinned_ = !last_pin;
    cache_cv_.notify_all();
  }

  if (!last_pin) {
    return;
  }

  CleanupIdlePipelineResources(pipeline);
}

auto PipelineMgmtService::PersistEditorHistoryState(
    const std::shared_ptr<PipelineGuard>& pipeline,
    const ImageEditState& expected_materialized_state, std::string* error) -> bool {
  if (!pipeline || !pipeline->commit_graph_) {
    if (error != nullptr) {
      *error = "PipelineMgmtService: history persistence requires a loaded editor graph";
    }
    return false;
  }

  try {
    auto             db_guard = storage_->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto             stored_graph = graph_service.LoadGraph(pipeline->id_);
    if (!stored_graph.has_value()) {
      throw std::runtime_error("PipelineMgmtService: persisted editor graph is missing");
    }

    const auto& stored_state = stored_graph->GetImageEditState();
    if (stored_state.element_id != expected_materialized_state.element_id ||
        stored_state.root_id != expected_materialized_state.root_id ||
        stored_state.active_version_id != expected_materialized_state.active_version_id ||
        stored_state.materialized_head_commit_hash !=
            expected_materialized_state.materialized_head_commit_hash ||
        stored_state.materialized_transaction_chain_hash !=
            expected_materialized_state.materialized_transaction_chain_hash) {
      throw std::runtime_error(
          "PipelineMgmtService: persisted history changed before editor history persistence");
    }

    // Logical head is only the live CommitGraph active Version (no guard cache).
    auto& graph = *pipeline->commit_graph_;
    if (graph.GetElementId() != pipeline->id_ || graph.GetRootId() != pipeline->root_id_) {
      throw std::runtime_error(
          "PipelineMgmtService: live history identity changed before editor history persistence");
    }

    const auto materialization = graph.CaptureMaterializationClearingSerializedPipelineState();
    graph_service.Materialize(materialization);
    pipeline->commit_graph_->ApplyMaterializedState(materialization.image_state);
    pipeline->serialized_state_needs_writeback_ = false;
    return true;
  } catch (const std::exception& ex) {
    if (error != nullptr) *error = ex.what();
  } catch (...) {
    if (error != nullptr) *error = "PipelineMgmtService: editor history persistence failed";
  }
  return false;
}

auto PipelineMgmtService::CheckoutVersion(const std::shared_ptr<PipelineGuard>& pipeline,
                                          const version_ref_id_t& version_id, std::string* error,
                                          MaskStore* mask_store) -> bool {
  if (!pipeline || !pipeline->pipeline_ || !pipeline->commit_graph_ || !pipeline->document_) {
    if (error != nullptr) {
      *error =
          "PipelineMgmtService: checkout requires a loaded editor pipeline with a commit graph";
    }
    return false;
  }

  auto&      graph            = *pipeline->commit_graph_;
  const auto prior_version_id = graph.GetActiveVersionId();
  if (prior_version_id == version_id) {
    return true;
  }

  head_commit_hash_t target_head;
  try {
    target_head = graph.GetVersionRef(version_id).head_commit_hash;
    (void)FirstParentCommits(graph, target_head);
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }

  std::optional<LoadedRootState> root_state;
  {
    auto             db_guard = storage_->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       root_encoded =
        graph_service.GetRootSerializedPipelineState(pipeline->id_, graph.GetRootId());
    if (!root_encoded.has_value()) {
      if (error != nullptr) {
        *error = "PipelineMgmtService: immutable root state is missing for checkout";
      }
      return false;
    }
    root_state = TryDecodeRootState(*root_encoded, pipeline->id_, graph.GetRootId());
    if (!root_state.has_value()) {
      if (error != nullptr) {
        *error = "PipelineMgmtService: immutable root state is invalid for checkout";
      }
      return false;
    }
  }
  CacheRootDocument(*pipeline, root_state->document);

  PipelineHistoryApplyContext context;
  context.mask_store = mask_store;
  std::string replay_error;
  auto        replayed = ReplayPipelineDocumentFromRoot(
      root_state->document, FirstParentCommits(graph, target_head), &replay_error, context);
  if (!replayed.has_value()) {
    if (error != nullptr) {
      *error = replay_error.empty() ? "PipelineMgmtService: checkout replay failed" : replay_error;
    }
    return false;
  }
  if (!VerifyPersistentMaskAssets(*replayed, mask_store, &replay_error)) {
    if (error != nullptr) {
      *error = replay_error;
    }
    return false;
  }

  PipelineDocument prior_document;
  nlohmann::json   prior_params;
  {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    prior_document = ClonePipelineDocument(*pipeline->document_);
    prior_params   = pipeline->pipeline_->ExportPipelineParams();
  }

  auto restore_prior = [&]() {
    graph.SetActiveVersionId(prior_version_id);
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    BindLiveDocument(*pipeline, ClonePipelineDocument(prior_document));
    ImportSerializedPipelineState(*pipeline->pipeline_, prior_params,
                                  root_state->raw_color_context, accelerator_preference_);
    pipeline->pipeline_->SetExecutionStages();
  };

  graph.SetActiveVersionId(version_id);
  try {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    BindLiveDocument(*pipeline, std::move(*replayed));
    if (root_state->raw_color_context.has_value()) {
      pipeline->pipeline_->InjectRawMetadata(*root_state->raw_color_context);
    }
    if (!ApplyVersionHeadToLivePipeline(*pipeline->pipeline_, graph, target_head, &replay_error)) {
      throw std::runtime_error(replay_error);
    }
    pipeline->pipeline_->SetExecutionStages();
    pipeline->serialized_state_needs_writeback_ = true;
    pipeline->dirty_                            = true;
    return true;
  } catch (const std::exception& ex) {
    std::string restore_error;
    try {
      restore_prior();
    } catch (const std::exception& restore_ex) {
      restore_error = restore_ex.what();
    } catch (...) {
      restore_error = "unknown restore error";
    }
    if (error != nullptr) {
      if (restore_error.empty()) {
        *error = std::string("PipelineMgmtService: checkout rebuild failed: ") + ex.what();
      } else {
        *error = std::string("fatal editor session: checkout rebuild failed: ") + ex.what() +
                 "; prior Version restoration failed: " + restore_error;
      }
    }
    return false;
  } catch (...) {
    std::string restore_error;
    try {
      restore_prior();
    } catch (const std::exception& restore_ex) {
      restore_error = restore_ex.what();
    } catch (...) {
      restore_error = "unknown restore error";
    }
    if (error != nullptr) {
      if (restore_error.empty()) {
        *error = "PipelineMgmtService: checkout rebuild failed with an unknown error";
      } else {
        *error =
            std::string("fatal editor session: checkout rebuild failed with an unknown error; "
                        "prior Version restoration failed: ") +
            restore_error;
      }
    }
    return false;
  }
}

auto PipelineMgmtService::RebuildActiveEditorPipeline(
    const std::shared_ptr<PipelineGuard>& pipeline, std::string* error, MaskStore* mask_store)
    -> bool {
  if (!pipeline || !pipeline->pipeline_ || !pipeline->commit_graph_) {
    if (error != nullptr) {
      *error =
          "PipelineMgmtService: active Version rebuild requires a loaded editor pipeline with a "
          "commit graph";
    }
    return false;
  }

  auto&                          graph = *pipeline->commit_graph_;
  std::optional<LoadedRootState> root_state;
  {
    auto             db_guard = storage_->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       root_encoded =
        graph_service.GetRootSerializedPipelineState(pipeline->id_, graph.GetRootId());
    if (!root_encoded.has_value()) {
      if (error != nullptr) {
        *error = "PipelineMgmtService: immutable root state is missing for active Version rebuild";
      }
      return false;
    }
    root_state = TryDecodeRootState(*root_encoded, pipeline->id_, graph.GetRootId());
    if (!root_state.has_value()) {
      if (error != nullptr) {
        *error = "PipelineMgmtService: immutable root state is invalid for active Version rebuild";
      }
      return false;
    }
  }
  CacheRootDocument(*pipeline, root_state->document);

  PipelineDocument prior_document;
  nlohmann::json   prior_params;
  {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    prior_document = ClonePipelineDocument(*pipeline->document_);
    prior_params   = pipeline->pipeline_->ExportPipelineParams();
  }

  auto restore_prior = [&]() {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    BindLiveDocument(*pipeline, ClonePipelineDocument(prior_document));
    ImportSerializedPipelineState(*pipeline->pipeline_, prior_params,
                                  root_state->raw_color_context, accelerator_preference_);
    pipeline->pipeline_->SetExecutionStages();
  };

  try {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    std::string                  replay_error;
    ++editor_pipeline_history_rebuild_count_;
    if (!ReplayLiveDocumentFromRoot(*pipeline, graph, *root_state,
                                    graph.GetActiveVersionRef().head_commit_hash, mask_store,
                                    &replay_error)) {
      throw std::runtime_error(replay_error);
    }
    pipeline->serialized_state_needs_writeback_ = true;
    pipeline->dirty_                            = true;
    return true;
  } catch (const std::exception& ex) {
    try {
      restore_prior();
    } catch (...) {
    }
    if (error != nullptr) {
      *error = std::string("PipelineMgmtService: active Version rebuild failed: ") + ex.what();
    }
    return false;
  } catch (...) {
    try {
      restore_prior();
    } catch (...) {
    }
    if (error != nullptr) {
      *error = "PipelineMgmtService: active Version rebuild failed with an unknown error";
    }
    return false;
  }
}

auto PipelineMgmtService::CollectUnreachableEditCommits() -> std::size_t {
  auto             db_guard = storage_->GetDatabase().GetConnectionGuard();
  auto             db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  return graph_service.DeleteUnreachableCommitsForProject();
}

void PipelineMgmtService::DeletePipeline(sl_element_id_t id) {
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    pipeline_cache_.RemoveRecord(id);
    loaded_pipelines_.erase(id);
  }
  storage_->ForgetLivePipeline(id);
  try {
    storage_->GetElementStore().RemovePipelineByElementId(id);
  } catch (...) {
  }
}

void PipelineMgmtService::DeletePipelines(std::span<const sl_element_id_t> ids) {
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    for (const auto id : ids) {
      if (id == 0) {
        continue;
      }
      pipeline_cache_.RemoveRecord(id);
      loaded_pipelines_.erase(id);
    }
  }
  for (const auto id : ids) {
    if (id != 0) {
      storage_->ForgetLivePipeline(id);
    }
  }
  try {
    auto             db_guard = storage_->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    for (const auto id : ids) {
      if (id != 0) {
        graph_service.DeleteGraphForElement(id);
      }
    }
  } catch (...) {
  }
  try {
    storage_->GetElementStore().RemovePipelinesByElementIds(ids);
  } catch (...) {
  }
}

void PipelineMgmtService::SetAcceleratorBackendPreference(AcceleratorBackendPreference preference) {
  std::vector<std::shared_ptr<PipelineGuard>> pipelines;
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    if (accelerator_preference_ == preference) {
      return;
    }

    accelerator_preference_ = preference;
    pipelines.reserve(loaded_pipelines_.size());
    for (auto& [id, pipeline_guard] : loaded_pipelines_) {
      (void)id;
      if (pipeline_guard && pipeline_guard->pipeline_) {
        pipelines.push_back(pipeline_guard);
      }
    }
  }

  for (const auto& pipeline_guard : pipelines) {
    std::unique_lock<std::mutex> render_guard(pipeline_guard->pipeline_->GetRenderLock());
    pipeline_guard->pipeline_->SetAcceleratorBackendPreference(preference);
    pipeline_guard->pipeline_->ClearAllIntermediateBuffers();
  }
}

void PipelineMgmtService::Sync() {
  std::vector<std::shared_ptr<PipelineGuard>> pipelines;
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    pipelines.reserve(loaded_pipelines_.size());
    for (const auto& [id, pipeline_guard] : loaded_pipelines_) {
      (void)id;
      if (pipeline_guard && pipeline_guard->pipeline_ && pipeline_guard->document_) {
        pipelines.push_back(pipeline_guard);
      }
    }
  }

  for (const auto& pipeline_guard : pipelines) {
    SyncDirtyPipelineDocument(pipeline_guard);
  }
}

void PipelineMgmtService::SyncPipeline(sl_element_id_t id) {
  std::shared_ptr<PipelineGuard> pipeline_guard;
  {
    std::unique_lock<std::mutex> cache_lock(lock_);
    const auto it = loaded_pipelines_.find(id);
    if (it == loaded_pipelines_.end() || !it->second || !it->second->pipeline_ ||
        !it->second->document_) {
      return;
    }
    pipeline_guard = it->second;
  }

  SyncDirtyPipelineDocument(pipeline_guard);
}

auto CheckpointMatchesLogicalHead(const ImageEditState& state, head_commit_hash_t logical_head,
                                  const transaction_chain_hash_t& logical_chain) -> bool {
  if (state.serialized_pipeline_state.has_value()) {
    const auto stored = TryDecodeCheckpoint(*state.serialized_pipeline_state);
    if (!stored.has_value()) {
      return false;
    }
    return stored->root_id == state.root_id && stored->head_commit_hash == logical_head &&
           stored->transaction_chain_hash == logical_chain;
  }
  return state.materialized_head_commit_hash == logical_head &&
         state.materialized_transaction_chain_hash == logical_chain;
}

}  // namespace alcedo
