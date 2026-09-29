//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/operators/models/parameter_revision.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/result_representation.hpp"
#include "edit/runtime/runtime_revision.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

class PipelineDocument;

/**
 * @brief Owner-maintained result validity: revisions, compiled downstream edges,
 *        and representation identities.
 *
 * Does not copy node parameters and never writes the document. Parameter changes are found
 * by comparing Model, mix, and Mask revisions with the revisions this state saw at its last
 * propagate, so each render workspace detects changes independently of other workspaces that
 * read the same document.
 *
 * Not thread-safe. One in-flight propagate per owner mutation batch.
 */
class RuntimeInvalidationState {
 public:
  struct Record {
    RuntimeRevision      required  = 0;
    RuntimeRevision      completed = 0;
    ResultRepresentation published{};
  };

  /**
   * @brief Rebuild compiled downstream adjacency from @p plan.
   *
   * Call when the static plan identity changes. Parameter edits do not recompile.
   */
  void BindCompiledPlan(const ExecutionPlan& plan);

  /**
   * @brief Compare parameter, mix, and Mask revisions with the last propagate; assign one
   *        change version and propagate once.
   *
   * @pre @ref BindCompiledPlan has run for @p plan.
   * Reads @p document only. The last-seen revisions are updated here, before any GPU work,
   * so a failed GPU publish still keeps required ahead of completed.
   */
  void CollectAndPropagate(const ExecutionPlan& plan, const PipelineDocument& document,
                           const PreparedRawInput& input);

  /**
   * @brief Snapshot source/geometry identities for this frame's bind checks.
   *
   * Viewport is in the frame identity only. Canonical LLF identity omits viewport.
   */
  void CaptureFrameRepresentations(const ExecutionPlan& plan, const PreparedRawInput& input);

  /**
   * @brief New document generation. Same NodeIds cannot reuse prior results.
   *
   * Bumps every required revision. Does not clear completed textures; bind misses
   * until the new generation is published.
   */
  void AdvanceDocumentEpoch();

  /** @brief Drop revisions, adjacency, and all last-seen parameter, mix, and Mask revisions. */
  void Clear();

  [[nodiscard]] auto DocumentEpoch() const -> RuntimeRevision { return document_epoch_; }
  [[nodiscard]] auto ChangeVersion() const -> RuntimeRevision { return change_version_; }

  [[nodiscard]] auto RequiredRevision(const GraphValueId& id) const -> RuntimeRevision;
  [[nodiscard]] auto CompletedRevision(const GraphValueId& id) const -> RuntimeRevision;
  [[nodiscard]] auto PublishedRepresentation(const GraphValueId& id) const -> ResultRepresentation;

  /**
   * @brief True when this value has a published completed revision equal to required.
   *
   * Does not compare GPU extent. Callers pass @p needed to check representation.
   */
  [[nodiscard]] auto IsSatisfied(const GraphValueId& id,
                                 const ResultRepresentation& needed) const -> bool;

  /**
   * @brief True when @p published_revision is the current required revision of @p id.
   *
   * Used to retain a live GPU result. Representation is checked at bind time;
   * a QualityBase frame must not treat an Interactive result as reclaimable
   * just because this frame's extent differs.
   */
  [[nodiscard]] auto HasCurrentRevision(const GraphValueId& id,
                                        RuntimeRevision published_revision) const -> bool;

  /**
   * @brief Frame representation for an image result of @p id.
   *
   * Sensor uses source identity. Canonical LLF ports use the crop/reference
   * identity. Other image values use the viewport-inclusive frame identity.
   */
  [[nodiscard]] auto MakeImageRepresentation(const GraphValueId& id, ImageExtent extent,
                                             TextureFormat format,
                                             std::uint32_t source_detail = 0) const
      -> ResultRepresentation;

  /**
   * @brief Record a successful publish of @p id at its current required revision.
   *
   * @p representation is the GPU result that became current. Failed writes must
   * not call this.
   */
  void MarkCompleted(const GraphValueId& id, const ResultRepresentation& representation);

  /**
   * @brief Mark every published image whose cache revision matches required.
   *
   * Skipped values already have completed == required. Executed values must be
   * published in the image cache first.
   */
  template <class Images>
  void CompleteMatchingImages(const Images& images) {
    for (auto& [id, record] : records_) {
      if (record.required == 0 || record.required != images.PublishedRevision(id)) {
        continue;
      }
      record.completed = record.required;
      record.published = images.PublishedRepresentation(id);
    }
  }

  [[nodiscard]] auto TrackedValueCount() const -> std::size_t { return records_.size(); }

  /**
   * @brief Downstream consumers of @p id from the compiled plan, including LLF ports.
   */
  [[nodiscard]] auto Downstream(const GraphValueId& id) const -> std::vector<GraphValueId>;

 private:
  /// One adjustment instance, owned by a Color Grade or by the DRT node.
  struct AdjustmentKey {
    NodeId               owner;
    AdjustmentInstanceId instance;

    friend auto          operator<(const AdjustmentKey& a, const AdjustmentKey& b) -> bool {
      if (a.owner != b.owner) {
        return a.owner < b.owner;
      }
      return a.instance < b.instance;
    }
  };

  /**
   * @brief Record @p revision for @p key in @p seen and report a change against the last frame.
   *
   * A key absent from the last frame counts as changed.
   */
  template <class Key>
  [[nodiscard]] static auto ObserveRevision(const std::map<Key, ParameterRevision>& previous,
                                            std::map<Key, ParameterRevision>& seen, const Key& key,
                                            ParameterRevision revision) -> bool {
    seen[key]     = revision;
    const auto it = previous.find(key);
    return it == previous.end() || it->second != revision;
  }

  struct MaskKey {
    NodeId grade;
    MaskId mask;

    friend auto operator<(const MaskKey& a, const MaskKey& b) -> bool {
      if (a.grade != b.grade) {
        return a.grade < b.grade;
      }
      return a.mask < b.mask;
    }
  };

  void AddEdge(const GraphValueId& from, const GraphValueId& to);
  void ClearLastSeenRevisions();
  void InvalidateFrom(const GraphValueId& id);
  auto Ensure(const GraphValueId& id) -> Record&;
  void CollectDevelopChanges(const ExecutionPlan& plan, const PipelineDocument& document,
                             std::vector<GraphValueId>& origins);
  void CollectGradeChanges(const ExecutionPlan& plan, const PipelineDocument& document,
                           std::map<AdjustmentKey, ParameterRevision>& seen_adjustments,
                           std::vector<GraphValueId>&                  origins);
  void CollectDrtChanges(const ExecutionPlan& plan, const PipelineDocument& document,
                         std::map<AdjustmentKey, ParameterRevision>& seen_adjustments,
                         std::vector<GraphValueId>&                  origins);
  void CollectStructureChanges(const ExecutionPlan& plan, std::vector<GraphValueId>& origins);

  struct GradeBindState {
    GraphValueId        scene_input{};
    std::vector<MaskId> mask_ids{};
  };

  std::map<GraphValueId, std::vector<GraphValueId>> outgoing_;
  std::map<GraphValueId, Record>                    records_;
  std::map<MaskKey, ParameterRevision>              last_mask_revision_;
  std::map<AdjustmentKey, ParameterRevision>        last_adjustment_revision_;
  std::map<NodeId, ParameterRevision>               last_mix_revision_;
  ParameterRevision                                 last_sensor_revision_ = kNoParameterRevision;
  ParameterRevision last_white_balance_revision_                          = kNoParameterRevision;
  ParameterRevision last_drt_revision_                                    = kNoParameterRevision;
  std::map<NodeId, GradeBindState>                  last_grade_bind_;
  GraphValueId                                      last_drt_input_{};
  StaticPlanKey                                     bound_plan_{};
  RuntimeRevision                                   document_epoch_      = 1;
  RuntimeRevision                                   change_version_      = 0;
  std::uint64_t                                     sensor_identity_     = 0;
  std::uint64_t                                     frame_identity_      = 0;
  std::uint64_t                                     canonical_identity_  = 0;
  GraphValueId                                      sensor_output_{};
  GraphValueId                                      geometry_output_{};
  GraphValueId                                      develop_output_{};
  GraphValueId                                      display_output_{};
};

}  // namespace alcedo
