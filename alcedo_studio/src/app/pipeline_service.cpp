//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_service.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {
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
      {"mode", mode}, {"cct", cct}, {"tint", tint}, {"resolved_cct", cct}, {"resolved_tint", tint},
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

constexpr std::uint32_t kSerializedPipelineStateFormatVersion = 1;
constexpr const char*   kSerializedPipelineStateFormatKey     = "state_format_version";
constexpr const char*   kSerializedPipelineStateRootKey       = "root_id";
constexpr const char*   kSerializedPipelineStateHeadKey       = "head_commit_hash";
constexpr const char*   kSerializedPipelineStateChainKey      = "transaction_chain_hash";
constexpr const char*   kSerializedPipelineStateParamsKey     = "pipeline_params";
constexpr const char*   kRootRawColorContextKey               = "raw_color_context";

struct DecodedSerializedPipelineState {
  root_id_t                root_id{};
  head_commit_hash_t       head_commit_hash = std::nullopt;
  transaction_chain_hash_t transaction_chain_hash{};
  nlohmann::json           pipeline_params;
};

struct DecodedRootPipelineState {
  nlohmann::json                        pipeline_params;
  std::optional<RawRuntimeColorContext> raw_color_context;
};

auto MakeSerializedPipelineState(const PipelineGuard& guard, const nlohmann::json& pipeline_params)
    -> nlohmann::json {
  return nlohmann::json{
      {kSerializedPipelineStateFormatKey, kSerializedPipelineStateFormatVersion},
      {kSerializedPipelineStateRootKey, guard.root_id_.ToString()},
      {kSerializedPipelineStateHeadKey, HeadCommitHashToStorage(guard.working_head_commit_hash_)},
      {kSerializedPipelineStateChainKey, guard.transaction_chain_hash_.ToString()},
      {kSerializedPipelineStateParamsKey, pipeline_params}};
}

auto DecodeSerializedPipelineState(const nlohmann::json& encoded)
    -> std::optional<DecodedSerializedPipelineState> {
  if (!encoded.is_object() ||
      encoded.value(kSerializedPipelineStateFormatKey, 0u) !=
          kSerializedPipelineStateFormatVersion ||
      !encoded.contains(kSerializedPipelineStateRootKey) ||
      !encoded.contains(kSerializedPipelineStateHeadKey) ||
      !encoded.contains(kSerializedPipelineStateChainKey) ||
      !encoded.contains(kSerializedPipelineStateParamsKey) ||
      !encoded.at(kSerializedPipelineStateRootKey).is_string() ||
      !encoded.at(kSerializedPipelineStateHeadKey).is_string() ||
      !encoded.at(kSerializedPipelineStateChainKey).is_string()) {
    return std::nullopt;
  }

  try {
    DecodedSerializedPipelineState state;
    state.root_id =
        Hash128::FromString(encoded.at(kSerializedPipelineStateRootKey).get<std::string>());
    state.head_commit_hash =
        HeadCommitHashFromStorage(encoded.at(kSerializedPipelineStateHeadKey).get<std::string>());
    state.transaction_chain_hash =
        Hash128::FromString(encoded.at(kSerializedPipelineStateChainKey).get<std::string>());
    state.pipeline_params = encoded.at(kSerializedPipelineStateParamsKey);
    return state;
  } catch (...) {
    return std::nullopt;
  }
}

auto DecodeRootPipelineState(const nlohmann::json& encoded)
    -> std::optional<DecodedRootPipelineState> {
  if (!encoded.is_object() ||
      encoded.value(kSerializedPipelineStateFormatKey, 0u) !=
          kSerializedPipelineStateFormatVersion ||
      !encoded.contains(kSerializedPipelineStateParamsKey)) {
    return std::nullopt;
  }

  try {
    DecodedRootPipelineState state;
    state.pipeline_params = encoded.at(kSerializedPipelineStateParamsKey);
    if (encoded.contains(kRootRawColorContextKey) &&
        !encoded.at(kRootRawColorContextKey).is_null()) {
      RawRuntimeColorContext context;
      if (!RawColorContextFromJson(encoded.at(kRootRawColorContextKey), context)) {
        return std::nullopt;
      }
      state.raw_color_context = std::move(context);
    }
    return state;
  } catch (...) {
    return std::nullopt;
  }
}

void SetPipelineHistoryState(PipelineGuard& guard, const CommitGraph& graph) {
  guard.root_id_                          = graph.GetRootId();
  guard.working_head_commit_hash_         = graph.GetActiveVersionRef().head_commit_hash;
  guard.transaction_chain_hash_           = graph.ChainHashForHead(guard.working_head_commit_hash_);
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

void ApplyCommitField(CPUPipelineExecutor& exec, OperatorType operator_type,
                      PipelineStageName stage_name, const std::string& field_name,
                      const nlohmann::json& value, bool enabled) {
  if (field_name.empty()) {
    throw std::runtime_error("Pipeline reconstruction encountered an empty edit field");
  }

  auto&          stage  = exec.GetStage(stage_name);
  nlohmann::json params = nlohmann::json::object();
  const auto     entry  = stage.GetOperator(operator_type);
  if (entry.has_value() && entry.value() && entry.value()->op_) {
    params = entry.value()->op_->GetParams();
  }
  if (!params.is_object()) {
    throw std::runtime_error("Pipeline reconstruction requires object operator parameters");
  }

  if (field_name == "$operator_params") {
    if (!value.is_object()) {
      throw std::runtime_error("Pipeline reconstruction requires object operator parameters");
    }
    params = value;
  } else if (field_name.front() == '/') {
    params[nlohmann::json::json_pointer(field_name)] = value;
  } else {
    params[field_name] = value;
  }

  auto& global_params = exec.GetGlobalParams();
  stage.SetOperator(operator_type, std::move(params), global_params);
  stage.EnableOperator(operator_type, enabled, global_params);
}

void ApplyCommit(CPUPipelineExecutor& exec, const EditCommit& commit) {
  if (commit.GetKind() == EditCommitKind::kEdit) {
    const auto payload = OrdinaryEditPayload::FromJSON(commit.GetPayloadJSON());
    ApplyCommitField(exec, payload.operator_type, payload.stage_name, payload.field_name,
                     payload.after_value, payload.after_enabled);
    return;
  }

  if (commit.GetKind() == EditCommitKind::kMerge) {
    const auto payload = MergeEditPayload::FromJSON(commit.GetPayloadJSON());
    for (const auto& field : payload.fields) {
      ApplyCommitField(exec, field.operator_type, field.stage_name, field.field_name,
                       field.resolved_value, field.resolved_enabled);
    }
    return;
  }

  throw std::runtime_error("Pipeline reconstruction encountered an unknown commit kind");
}

void RebuildPipelineFromRoot(CPUPipelineExecutor& exec, const CommitGraph& graph,
                             const DecodedRootPipelineState& root_state,
                             AcceleratorBackendPreference    accelerator_preference) {
  ImportSerializedPipelineState(exec, root_state.pipeline_params, root_state.raw_color_context,
                                accelerator_preference);
  for (const auto& commit_hash :
       graph.FirstParentChain(graph.GetActiveVersionRef().head_commit_hash)) {
    ApplyCommit(exec, graph.GetCommit(commit_hash));
  }
  exec.SetExecutionStages();
}
}  // namespace

void PipelineMgmtService::HandleEviction(sl_element_id_t evicted_id) {
  // If the would-be evicted pipeline is pinned, keep it and evict another entry instead.
  // This avoids unbounded cache growth during batch export when a pipeline is temporarily pinned.
  sl_element_id_t candidate    = evicted_id;
  const size_t    max_attempts = loaded_pipelines_.empty() ? 1 : (loaded_pipelines_.size() + 1);

  for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
    auto it = loaded_pipelines_.find(candidate);
    if (it == loaded_pipelines_.end()) {
      return;
    }

    auto pipeline_guard = it->second;
    if (pipeline_guard->pin_count_ == 0) {
      pipeline_guard->pinned_ = false;
      std::unique_lock<std::mutex> render_guard(pipeline_guard->pipeline_->GetRenderLock());
      if (pipeline_guard->dirty_) {
        storage_service_->GetElementController().UpdatePipelineByElementId(
            candidate, pipeline_guard->pipeline_);
      }
      // Clear intermediate buffers before removing from cache to ensure timely memory release
      pipeline_guard->pipeline_->ClearAllIntermediateBuffers();
      loaded_pipelines_.erase(it);
      return;
    }

    // Pinned: put it back into the LRU and evict a different entry.
    auto next = pipeline_cache_.RecordAccess_WithEvict(candidate, candidate);
    if (!next.has_value()) {
      return;
    }
    candidate = next.value();
  }

  // Fallback: if everything is pinned, allow temporary growth to avoid evicting in-use pipelines.
  auto keys = pipeline_cache_.GetLRUKeys();
  pipeline_cache_.Resize(static_cast<uint32_t>(keys.size() + 5));
  pipeline_cache_.RecordAccess(evicted_id, evicted_id);
}

auto PipelineMgmtService::LoadPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard> {
  std::unique_lock<std::mutex> guard(lock_);

  if (pipeline_cache_.Contains(id)) {
    auto cached_id = pipeline_cache_.AccessElement(id);
    if (cached_id.has_value()) {
      auto it = loaded_pipelines_.find(cached_id.value());
      if (it != loaded_pipelines_.end()) {
        // If the pipeline was previously returned to cache (unpinned), it likely had its
        // execution stages reset (e.g. to detach frame sinks). Re-initialize it here so callers
        // that don't explicitly call SetExecutionStages() won't pay the cost or crash.
        if (!it->second->pinned_) {
          it->second->pipeline_->SetBoundFile(id);
          it->second->pipeline_->SetAcceleratorBackendPreference(accelerator_preference_);
          it->second->pipeline_->SetExecutionStages();
          // Reset transient render/cache state to a consistent preview baseline.
          ResetTransientPreviewState(*it->second->pipeline_);

          EnsureDefaultOutputTransform(*it->second->pipeline_);
          EnsureDefaultRawDecode(*it->second->pipeline_);
          EnsureDefaultColorTemp(*it->second->pipeline_);
          EnsureDefaultLensCalib(*it->second->pipeline_);
          ResyncGlobalParamsFromOperators(*it->second->pipeline_);
        }

        it->second->pin_count_++;
        it->second->pinned_ = true;
        it->second->id_     = id;
        storage_service_->RememberLivePipeline(id, it->second->pipeline_);
        return it->second;
      }
    }
  } else {
    std::shared_ptr<CPUPipelineExecutor> pipeline;
    std::shared_ptr<PipelineGuard>       pipeline_guard;
    pipeline = storage_service_->GetLivePipeline(id);
    try {
      if (!pipeline) {
        pipeline = storage_service_->GetElementController().GetPipelineByElementId(id);
      }
      pipeline_guard         = std::make_shared<PipelineGuard>();
      pipeline_guard->dirty_ = false;
    } catch (std::exception& e) {
      throw std::runtime_error(
          "[ERROR] PipelineMgmtService: Failed to load pipeline from storage for element ID " +
          std::to_string(id) + ": " + e.what());
    }
    if (pipeline == nullptr) {
      pipeline = std::make_shared<CPUPipelineExecutor>();
      pipeline->SetBoundFile(id);
      pipeline_guard->dirty_ = false;
    }

    // Ensure the loaded pipeline is bound to the requested element id.
    pipeline->SetBoundFile(id);
    pipeline->SetAcceleratorBackendPreference(accelerator_preference_);
    ResetTransientPreviewState(*pipeline);

    EnsureDefaultOutputTransform(*pipeline);
    EnsureDefaultRawDecode(*pipeline);
    EnsureDefaultColorTemp(*pipeline);
    EnsureDefaultLensCalib(*pipeline);
    ResyncGlobalParamsFromOperators(*pipeline);
    storage_service_->RememberLivePipeline(id, pipeline);

    pipeline
        ->SetExecutionStages();  // TODO: Use service as the only way to set/reset execution stages
    pipeline_guard->pipeline_              = std::move(pipeline);
    pipeline_guard->id_                    = id;
    pipeline_guard->pinned_                = true;
    pipeline_guard->pin_count_             = 1;
    std::optional<sl_element_id_t> evicted = pipeline_cache_.RecordAccess_WithEvict(id, id);
    if (evicted.has_value()) {
      HandleEviction(evicted.value());
    }
    loaded_pipelines_[id] = pipeline_guard;
    // If no eviction happened, and the cache size is still in "boost" range, resize it
    if (!evicted.has_value() && loaded_pipelines_.size() + 1 > default_cache_capacity_) {
      pipeline_cache_.Resize(loaded_pipelines_.size() - 1);
    }
    return pipeline_guard;
  }
  throw std::runtime_error("[ERROR] PipelineMgmtService: Failed to load pipeline.");
}

void PipelineMgmtService::InitializeImageRoot(const std::shared_ptr<PipelineGuard>& pipeline,
                                              const RawRuntimeColorContext* raw_color_context) {
  if (!pipeline || !pipeline->pipeline_) {
    throw std::runtime_error("PipelineMgmtService: cannot initialize a null pipeline root");
  }

  std::unique_lock<std::mutex> service_lock(lock_);

  nlohmann::json               root_params;
  {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    root_params = pipeline->pipeline_->ExportPipelineParams();
  }

  auto               db_guard = storage_service_->GetDBController().GetConnectionGuard();
  auto               db_lock  = db_guard.Lock();
  CommitGraphService graph_service(db_guard.conn_);
  auto               state = graph_service.GetImageEditState(pipeline->id_);
  if (!state.has_value()) {
    auto graph = graph_service.CreateRootPipelinePersisted(
        pipeline->id_, root_params,
        raw_color_context ? std::optional<nlohmann::json>(RawColorContextToJson(*raw_color_context))
                          : std::nullopt);
    SetPipelineHistoryState(*pipeline, graph);
    return;
  }

  auto graph = graph_service.LoadGraph(pipeline->id_);
  if (!graph.has_value()) {
    throw std::runtime_error(
        "PipelineMgmtService: image edit state disappeared while loading root");
  }
  if (!graph_service.GetRootSerializedPipelineState(pipeline->id_, graph->GetRootId())
           .has_value()) {
    throw std::runtime_error("PipelineMgmtService: immutable root state is missing for image " +
                             std::to_string(pipeline->id_));
  }
  SetPipelineHistoryState(*pipeline, *graph);
}

auto PipelineMgmtService::LoadEditorPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard> {
  auto pipeline = LoadPipeline(id);
  try {
    InitializeImageRoot(pipeline);

    std::unique_lock<std::mutex> service_lock(lock_);
    auto               db_guard = storage_service_->GetDBController().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphService graph_service(db_guard.conn_);
    auto               graph = graph_service.LoadGraph(id);
    if (!graph.has_value()) {
      throw std::runtime_error("PipelineMgmtService: image edit state is missing after root setup");
    }
    const auto root_encoded_state =
        graph_service.GetRootSerializedPipelineState(id, graph->GetRootId());
    if (!root_encoded_state.has_value()) {
      throw std::runtime_error("PipelineMgmtService: immutable root state is missing for image " +
                               std::to_string(id));
    }
    const auto root_state = DecodeRootPipelineState(*root_encoded_state);
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

    SetPipelineHistoryState(*pipeline, *graph);
    bool accepted_serialized_state = false;
    if (state.serialized_pipeline_state.has_value()) {
      const auto stored = DecodeSerializedPipelineState(*state.serialized_pipeline_state);
      if (stored.has_value() && stored->root_id == graph->GetRootId() &&
          stored->head_commit_hash == expected_head &&
          stored->transaction_chain_hash == expected_chain) {
        try {
          std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
          ImportSerializedPipelineState(*pipeline->pipeline_, stored->pipeline_params,
                                        root_state->raw_color_context, accelerator_preference_);
          pipeline->pipeline_->SetExecutionStages();
          accepted_serialized_state = true;
        } catch (...) {
          // Serialized state that cannot build an executor is stale. Rebuild from immutable
          // history below and write the corrected state when the guard is returned.
          accepted_serialized_state = false;
        }
      }
    }

    if (!accepted_serialized_state) {
      std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
      RebuildPipelineFromRoot(*pipeline->pipeline_, *graph, *root_state, accelerator_preference_);
      pipeline->serialized_state_needs_writeback_ = true;
    }
    return pipeline;
  } catch (const std::exception& e) {
    try {
      SavePipeline(pipeline);
    } catch (...) {
    }
    throw std::runtime_error("[ERROR] PipelineMgmtService: editor history validation failed for " +
                             std::to_string(id) + ": " + e.what());
  } catch (...) {
    try {
      SavePipeline(pipeline);
    } catch (...) {
    }
    throw std::runtime_error("[ERROR] PipelineMgmtService: editor history validation failed for " +
                             std::to_string(id) + ": unknown error");
  }
}

void PipelineMgmtService::SavePipeline(std::shared_ptr<PipelineGuard> pipeline) {
  if (!pipeline) {
    return;
  }

  std::unique_lock<std::mutex> guard(lock_);
  storage_service_->RememberLivePipeline(pipeline->id_, pipeline->pipeline_);

  const bool will_release_last_pin = pipeline->pin_count_ <= 1;
  if (will_release_last_pin && pipeline->serialized_state_needs_writeback_) {
    try {
      nlohmann::json pipeline_params;
      {
        std::unique_lock<std::mutex> render_guard(pipeline->pipeline_->GetRenderLock());
        pipeline_params = pipeline->pipeline_->ExportPipelineParams();
      }

      auto               db_guard = storage_service_->GetDBController().GetConnectionGuard();
      auto               db_lock  = db_guard.Lock();
      CommitGraphService graph_service(db_guard.conn_);
      auto stored_graph = graph_service.LoadGraph(pipeline->id_);
      if (!stored_graph.has_value()) {
        throw std::runtime_error(
            "PipelineMgmtService: cannot write serialized state without an edit graph");
      }

      // A newly pasted Version only exists in the live graph until this checkpoint.
      // Still compare the graph's last materialized state with DuckDB before writing it so a
      // different writer cannot be silently replaced by this serialized pipeline state.
      const auto& graph = pipeline->commit_graph_ ? *pipeline->commit_graph_ : *stored_graph;

      const auto expected_head  = graph.GetActiveVersionRef().head_commit_hash;
      const auto expected_chain = graph.ChainHashForHead(expected_head);
      if (graph.GetElementId() != pipeline->id_ || graph.GetRootId() != pipeline->root_id_ ||
           expected_head != pipeline->working_head_commit_hash_ ||
           expected_chain != pipeline->transaction_chain_hash_) {
        throw std::runtime_error(
            "PipelineMgmtService: live history identity changed before serialized state writeback");
      }

      const auto& stored_state = stored_graph->GetImageEditState();
      const auto& graph_state  = graph.GetImageEditState();
      if (stored_state.root_id != graph_state.root_id ||
          stored_state.active_version_id != graph_state.active_version_id ||
          stored_state.materialized_head_commit_hash != graph_state.materialized_head_commit_hash ||
          stored_state.materialized_transaction_chain_hash !=
              graph_state.materialized_transaction_chain_hash) {
        throw std::runtime_error(
            "PipelineMgmtService: persisted history changed before serialized state writeback");
      }

      const auto materialization = graph.CaptureMaterializationWithSerializedPipelineState(
          MakeSerializedPipelineState(*pipeline, pipeline_params));
      graph_service.Materialize(materialization);
      if (pipeline->commit_graph_) {
        pipeline->commit_graph_->ApplyMaterializedState(materialization.image_state);
      }
      pipeline->serialized_state_needs_writeback_ = false;
    } catch (...) {
      // This state accelerates editor open but is not the history source of truth. Keep the flag
      // set so the next guard release retries without leaking the pipeline pin.
    }
  }

  if (pipeline->pin_count_ > 0) {
    pipeline->pin_count_--;
  }
  const bool last_pin = (pipeline->pin_count_ == 0);
  pipeline->pinned_   = !last_pin;

  // Shared by multiple callers (e.g. thumbnail + export): only the last owner may release/reset.
  if (!last_pin) {
    return;
  }

  // Always clear intermediate buffers and GPU resources when returning a pipeline to cache.
  // This prevents large cached allocations (and any frame-sink related state) from leaking across
  // editor sessions and hurting interactive performance.
  {
    std::unique_lock<std::mutex> render_guard(pipeline->pipeline_->GetRenderLock());
    pipeline->pipeline_->ClearAllIntermediateBuffers();
    pipeline->pipeline_->ResetExecutionStages();
  }

  if (!pipeline->dirty_) {
    return;
  }

  // Save the pipeline back to the cache
  sl_element_id_t                id      = pipeline->id_;
  // Store it back to the pipeline cache
  std::optional<sl_element_id_t> evicted = pipeline_cache_.RecordAccess_WithEvict(id, id);
  if (evicted.has_value()) {
    HandleEviction(evicted.value());
  }
  // Unpin the pipeline after saving
  pipeline->pinned_     = false;
  loaded_pipelines_[id] = pipeline;

  // If eviction did not happen, but the cache size is still in "boost" range, resize it
  if (!evicted.has_value() && loaded_pipelines_.size() + 1 > default_cache_capacity_) {
    pipeline_cache_.Resize(static_cast<uint32_t>(loaded_pipelines_.size() - 1));
  }
}

auto PipelineMgmtService::CheckoutVersion(const std::shared_ptr<PipelineGuard>& pipeline,
                                          const version_ref_id_t& version_id, std::string* error)
    -> bool {
  if (!pipeline || !pipeline->pipeline_ || !pipeline->commit_graph_) {
    if (error != nullptr) {
      *error = "PipelineMgmtService: checkout requires a loaded editor pipeline with a commit graph";
    }
    return false;
  }

  auto& graph = *pipeline->commit_graph_;
  const auto prior_version_id = graph.GetActiveVersionId();
  if (prior_version_id == version_id) {
    // Already on the requested Version: refresh head/chain and succeed without rebuild.
    pipeline->working_head_commit_hash_ = graph.GetActiveVersionRef().head_commit_hash;
    pipeline->transaction_chain_hash_ =
        graph.ChainHashForHead(pipeline->working_head_commit_hash_);
    return true;
  }

  try {
    (void)graph.GetVersionRef(version_id);
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }

  DecodedRootPipelineState root_state;
  {
    std::unique_lock<std::mutex> service_lock(lock_);
    auto               db_guard = storage_service_->GetDBController().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphService graph_service(db_guard.conn_);
    const auto root_encoded =
        graph_service.GetRootSerializedPipelineState(pipeline->id_, graph.GetRootId());
    if (!root_encoded.has_value()) {
      if (error != nullptr) {
        *error = "PipelineMgmtService: immutable root state is missing for checkout";
      }
      return false;
    }
    auto decoded = DecodeRootPipelineState(*root_encoded);
    if (!decoded.has_value()) {
      if (error != nullptr) {
        *error = "PipelineMgmtService: immutable root state is invalid for checkout";
      }
      return false;
    }
    root_state = std::move(*decoded);
  }

  // Snapshot the live executor so a failed rebuild can restore the prior pipeline.
  nlohmann::json prior_params;
  {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    prior_params = pipeline->pipeline_->ExportPipelineParams();
  }
  const auto prior_head  = pipeline->working_head_commit_hash_;
  const auto prior_chain = pipeline->transaction_chain_hash_;

  graph.SetActiveVersionId(version_id);

  try {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    RebuildPipelineFromRoot(*pipeline->pipeline_, graph, root_state, accelerator_preference_);
    pipeline->working_head_commit_hash_ = graph.GetActiveVersionRef().head_commit_hash;
    pipeline->transaction_chain_hash_ =
        graph.ChainHashForHead(pipeline->working_head_commit_hash_);
    pipeline->serialized_state_needs_writeback_ = true;
    pipeline->dirty_                            = true;
    return true;
  } catch (const std::exception& ex) {
    // Fail closed: restore the prior Version and prior pipeline contents.
    try {
      graph.SetActiveVersionId(prior_version_id);
      std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
      ImportSerializedPipelineState(*pipeline->pipeline_, prior_params, root_state.raw_color_context,
                                    accelerator_preference_);
      pipeline->pipeline_->SetExecutionStages();
      pipeline->working_head_commit_hash_ = prior_head;
      pipeline->transaction_chain_hash_   = prior_chain;
    } catch (...) {
      // Best-effort restore; still report the original checkout failure.
    }
    if (error != nullptr) {
      *error = std::string("PipelineMgmtService: checkout rebuild failed: ") + ex.what();
    }
    return false;
  } catch (...) {
    try {
      graph.SetActiveVersionId(prior_version_id);
      std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
      ImportSerializedPipelineState(*pipeline->pipeline_, prior_params, root_state.raw_color_context,
                                    accelerator_preference_);
      pipeline->pipeline_->SetExecutionStages();
      pipeline->working_head_commit_hash_ = prior_head;
      pipeline->transaction_chain_hash_   = prior_chain;
    } catch (...) {
    }
    if (error != nullptr) {
      *error = "PipelineMgmtService: checkout rebuild failed with an unknown error";
    }
    return false;
  }
}

auto PipelineMgmtService::RebuildActiveEditorPipeline(
    const std::shared_ptr<PipelineGuard>& pipeline, std::string* error) -> bool {
  if (!pipeline || !pipeline->pipeline_ || !pipeline->commit_graph_) {
    if (error != nullptr) {
      *error =
          "PipelineMgmtService: active Version rebuild requires a loaded editor pipeline with a "
          "commit graph";
    }
    return false;
  }

  auto& graph = *pipeline->commit_graph_;
  DecodedRootPipelineState root_state;
  {
    std::unique_lock<std::mutex> service_lock(lock_);
    auto               db_guard = storage_service_->GetDBController().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphService graph_service(db_guard.conn_);
    const auto root_encoded =
        graph_service.GetRootSerializedPipelineState(pipeline->id_, graph.GetRootId());
    if (!root_encoded.has_value()) {
      if (error != nullptr) {
        *error = "PipelineMgmtService: immutable root state is missing for active Version rebuild";
      }
      return false;
    }
    auto decoded = DecodeRootPipelineState(*root_encoded);
    if (!decoded.has_value()) {
      if (error != nullptr) {
        *error = "PipelineMgmtService: immutable root state is invalid for active Version rebuild";
      }
      return false;
    }
    root_state = std::move(*decoded);
  }

  nlohmann::json prior_params;
  {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    prior_params = pipeline->pipeline_->ExportPipelineParams();
  }
  const auto prior_head  = pipeline->working_head_commit_hash_;
  const auto prior_chain = pipeline->transaction_chain_hash_;

  try {
    std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
    RebuildPipelineFromRoot(*pipeline->pipeline_, graph, root_state, accelerator_preference_);
    pipeline->working_head_commit_hash_ = graph.GetActiveVersionRef().head_commit_hash;
    pipeline->transaction_chain_hash_ =
        graph.ChainHashForHead(pipeline->working_head_commit_hash_);
    pipeline->serialized_state_needs_writeback_ = true;
    pipeline->dirty_                            = true;
    return true;
  } catch (const std::exception& ex) {
    try {
      std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
      ImportSerializedPipelineState(*pipeline->pipeline_, prior_params, root_state.raw_color_context,
                                    accelerator_preference_);
      pipeline->pipeline_->SetExecutionStages();
      pipeline->working_head_commit_hash_ = prior_head;
      pipeline->transaction_chain_hash_   = prior_chain;
    } catch (...) {
    }
    if (error != nullptr) {
      *error = std::string("PipelineMgmtService: active Version rebuild failed: ") + ex.what();
    }
    return false;
  } catch (...) {
    try {
      std::unique_lock<std::mutex> render_lock(pipeline->pipeline_->GetRenderLock());
      ImportSerializedPipelineState(*pipeline->pipeline_, prior_params, root_state.raw_color_context,
                                    accelerator_preference_);
      pipeline->pipeline_->SetExecutionStages();
      pipeline->working_head_commit_hash_ = prior_head;
      pipeline->transaction_chain_hash_   = prior_chain;
    } catch (...) {
    }
    if (error != nullptr) {
      *error = "PipelineMgmtService: active Version rebuild failed with an unknown error";
    }
    return false;
  }
}

auto PipelineMgmtService::CollectUnreachableEditCommits() -> std::size_t {
  std::unique_lock<std::mutex> service_lock(lock_);
  auto               db_guard = storage_service_->GetDBController().GetConnectionGuard();
  auto               db_lock  = db_guard.Lock();
  CommitGraphService graph_service(db_guard.conn_);
  return graph_service.DeleteUnreachableCommitsForProject();
}

void PipelineMgmtService::DeletePipeline(sl_element_id_t id) {
  std::unique_lock<std::mutex> guard(lock_);
  pipeline_cache_.RemoveRecord(id);
  loaded_pipelines_.erase(id);
  storage_service_->ForgetLivePipeline(id);
  try {
    storage_service_->GetElementController().RemovePipelineByElementId(id);
  } catch (...) {
  }
}

void PipelineMgmtService::DeletePipelines(std::span<const sl_element_id_t> ids) {
  std::unique_lock<std::mutex> guard(lock_);
  for (const auto id : ids) {
    if (id == 0) {
      continue;
    }
    pipeline_cache_.RemoveRecord(id);
    loaded_pipelines_.erase(id);
    storage_service_->ForgetLivePipeline(id);
  }
  try {
    auto               db_guard = storage_service_->GetDBController().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphService graph_service(db_guard.conn_);
    for (const auto id : ids) {
      if (id != 0) {
        graph_service.DeleteGraphForElement(id);
      }
    }
  } catch (...) {
  }
  try {
    storage_service_->GetElementController().RemovePipelinesByElementIds(ids);
  } catch (...) {
  }
}

void PipelineMgmtService::SetAcceleratorBackendPreference(AcceleratorBackendPreference preference) {
  std::unique_lock<std::mutex> guard(lock_);
  if (accelerator_preference_ == preference) {
    return;
  }

  accelerator_preference_ = preference;
  for (auto& [id, pipeline_guard] : loaded_pipelines_) {
    (void)id;
    if (!pipeline_guard || !pipeline_guard->pipeline_) {
      continue;
    }
    std::unique_lock<std::mutex> render_guard(pipeline_guard->pipeline_->GetRenderLock());
    pipeline_guard->pipeline_->SetAcceleratorBackendPreference(preference);
    pipeline_guard->pipeline_->ClearAllIntermediateBuffers();
  }
}

void PipelineMgmtService::Sync() {
  std::unique_lock<std::mutex> guard(lock_);
  for (auto& pair : loaded_pipelines_) {
    auto pipeline_guard = pair.second;
    if (pipeline_guard->dirty_) {
      storage_service_->GetElementController().UpdatePipelineByElementId(pipeline_guard->id_,
                                                                         pipeline_guard->pipeline_);
      pipeline_guard->dirty_ = false;
    }
  }
}

void PipelineMgmtService::SyncPipeline(sl_element_id_t id) {
  std::unique_lock<std::mutex> guard(lock_);
  const auto                   it = loaded_pipelines_.find(id);
  if (it == loaded_pipelines_.end() || !it->second || !it->second->pipeline_ ||
      !it->second->dirty_) {
    return;
  }

  auto&                        pipeline_guard = it->second;
  std::unique_lock<std::mutex> render_guard(pipeline_guard->pipeline_->GetRenderLock());
  storage_service_->GetElementController().UpdatePipelineByElementId(id, pipeline_guard->pipeline_);
  pipeline_guard->dirty_ = false;
}

auto PipelineMgmtService::LoadPipelineSnapshot(sl_element_id_t id, image_id_t image_id,
                                               std::string* error)
    -> std::shared_ptr<PipelineSnapshot> {
  std::shared_ptr<CPUPipelineExecutor> live_exec;

  // Cache-hit path: borrow the executor shared_ptr without touching pin_count_.
  // The copy keeps the executor alive even if the guard is evicted mid-capture.
  {
    std::unique_lock<std::mutex> g(lock_);
    auto                         it = loaded_pipelines_.find(id);
    if (it != loaded_pipelines_.end() && it->second && it->second->pipeline_) {
      live_exec = it->second->pipeline_;
    }
  }

  // Cache-miss fallback: LoadPipeline (pin + repair + build stages), capture the
  // executor, then SavePipeline to release the pin (net-zero). dirty_ is false on
  // a fresh load, so SavePipeline's last-pin path returns without writing storage
  // or re-recording into the cache; the guard stays in cache unpinned. This reuses
  // the EnsureDefault*/ResyncGlobalParamsFromOperators repair logic so the
  // snapshot reflects a coherent state even when the image was not live.
  if (!live_exec) {
    std::shared_ptr<PipelineGuard> guard;
    try {
      guard = LoadPipeline(id);
    } catch (const std::exception& e) {
      if (error) {
        *error = std::format("LoadPipelineSnapshot: load failed for {}: {}", id, e.what());
      }
      return nullptr;
    } catch (...) {
      if (error) {
        *error = std::format("LoadPipelineSnapshot: load failed for {}: unknown error", id);
      }
      return nullptr;
    }
    if (!guard || !guard->pipeline_) {
      if (error) {
        *error = std::format("LoadPipelineSnapshot: no executor for {}", id);
      }
      return nullptr;
    }
    live_exec = guard->pipeline_;
    try {
      SavePipeline(guard);
    } catch (...) {
      // Non-fatal: live_exec is valid. A leaked pin is rare and bounded; the guard
      // remains in cache and is reaped by Sync/eviction on the next opportunity.
    }
  }

  // Capture params under the live executor's render lock. This serializes with any
  // in-flight editor render on the same executor, so the snapshot is coherent.
  // ExportPipelineParams is const and reads stages_ only.
  nlohmann::json params;
  try {
    std::unique_lock<std::mutex> rg(live_exec->GetRenderLock());
    params = live_exec->ExportPipelineParams();
  } catch (const std::exception& e) {
    if (error) {
      *error = std::format("LoadPipelineSnapshot: export failed for {}: {}", id, e.what());
    }
    return nullptr;
  } catch (...) {
    if (error) {
      *error = std::format("LoadPipelineSnapshot: export failed for {}: unknown error", id);
    }
    return nullptr;
  }

  // Build the independent snapshot executor. SetAcceleratorBackendPreference
  // before ImportPipelineParams so the import's final SetExecutionStages() is the
  // last stage-build call and the imported params win. ResetTransientPreviewState
  // mirrors LoadPipeline's normalization of cached executors; it touches only
  // transient render state, not serialized operator params.
  std::shared_ptr<CPUPipelineExecutor> snap_exec;
  try {
    snap_exec = std::make_shared<CPUPipelineExecutor>();
    snap_exec->SetBoundFile(id);
    snap_exec->SetAcceleratorBackendPreference(accelerator_preference_);
    snap_exec->ImportPipelineParams(params);
    ResetTransientPreviewState(*snap_exec);
  } catch (const std::exception& e) {
    if (error) {
      *error = std::format("LoadPipelineSnapshot: snapshot build failed for {}: {}", id, e.what());
    }
    return nullptr;
  } catch (...) {
    if (error) {
      *error = std::format("LoadPipelineSnapshot: snapshot build failed for {}: unknown error", id);
    }
    return nullptr;
  }

  return std::make_shared<PipelineSnapshot>(
      PipelineSnapshot{id, image_id, std::move(params), snap_exec});
}

void PipelineMgmtService::ReleasePipelineSnapshot(std::shared_ptr<PipelineSnapshot> snapshot) {
  if (!snapshot || !snapshot->executor_) {
    return;
  }
  try {
    std::unique_lock<std::mutex> rg(snapshot->executor_->GetRenderLock());
    snapshot->executor_->ClearAllIntermediateBuffers();
  } catch (...) {
    // Best-effort cleanup; the shared_ptr drops naturally regardless.
  }
}
}  // namespace alcedo
