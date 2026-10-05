//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_root_state.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/pipeline_history_applier.hpp"
#include "app/source_dng_profile_binding.hpp"
#include "edit/graph/develop_color_transform.hpp"

namespace alcedo {

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

auto TryDecodeRootState(const nlohmann::json& encoded, sl_element_id_t element_id,
                        const root_id_t& expected_root_id) -> std::optional<LoadedRootState> {
  try {
    const auto root = DecodePipelineRootState(encoded);
    if (root.element_id != element_id) {
      return std::nullopt;
    }
    if (ComputeRootId(root.element_id, root.document, root.raw_color_context) != expected_root_id) {
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

void BindWorkingSpaceDevelopData(PipelineDocument& document) {
  auto* develop = document.Develop();
  if (develop == nullptr) {
    return;
  }
  auto payload = develop->Params().Params();
  auto next    = payload;
  BindRgbWorkingSpaceCameraProfile(next);
  if (next != payload) {
    develop->Params().ReplaceParams(std::move(next));
  }
}

void EnsureRenderableCameraProfile(PipelineDocument&                            document,
                                   const std::optional<RawRuntimeColorContext>& raw_color_context) {
  auto* develop = document.Develop();
  if (develop == nullptr) {
    return;
  }
  // A raster document converts its pixels with its own `input` description and has no camera
  // profile.
  if (develop->Params().RasterInput().has_value()) {
    return;
  }
  if (develop->Params().Params().camera_profile.color_matrices_valid) {
    return;
  }
  if (raw_color_context.has_value()) {
    return;
  }
  BindWorkingSpaceDevelopData(document);
}

void BindSourceDngProfiles(Storage& storage, sl_element_id_t id, LoadedRootState& root) {
  if (root.raw_color_context.has_value()) {
    BindSourceDngColorProfile(storage, id, *root.raw_color_context);
  }
  BindSourceDngColorProfile(storage, id, root.document);
}

void BindRootCameraProfile(PipelineDocument&                            document,
                           const std::optional<RawRuntimeColorContext>& raw_color_context) {
  EnsureRenderableCameraProfile(document, raw_color_context);
  if (raw_color_context.has_value()) {
    BindImportedCameraProfile(document, *raw_color_context);
  }
}

auto BuildDocumentFromRoot(const CommitGraph& graph, const LoadedRootState& root_state,
                           head_commit_hash_t head, std::string* error)
    -> std::shared_ptr<PipelineDocument> {
  try {
    auto replayed = ReplayPipelineDocumentFromRoot(root_state.document,
                                                   FirstParentCommitsForHead(graph, head), error);
    if (!replayed.has_value()) {
      return nullptr;
    }
    auto document = std::make_shared<PipelineDocument>(std::move(*replayed));
    BindRootCameraProfile(*document, root_state.raw_color_context);
    return document;
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
  } catch (...) {
    if (error != nullptr) {
      *error = "PipelineMgmtService: document replay failed with an unknown error";
    }
  }
  return nullptr;
}

auto BuildDocumentFromCheckpoint(const PipelineDocumentCheckpoint& checkpoint,
                                 const LoadedRootState&            root_state)
    -> std::shared_ptr<PipelineDocument> {
  auto document = std::make_shared<PipelineDocument>(ClonePipelineDocument(checkpoint.document));
  BindRootCameraProfile(*document, root_state.raw_color_context);
  return document;
}

}  // namespace alcedo
