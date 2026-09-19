//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/operators/models/operator_type_id.hpp"

namespace alcedo {

/**
 * @brief Product section that groups one transferable item row.
 *
 * Mirrors the Adjustment Stack panel sections so the item column can render
 * the same groups: Tone, Look, LUT, Display Transform, and Mask. `Node`
 * covers the node-level Enabled and Mix rows, which have no stack panel.
 */
enum class AdjustmentTransferItemSection : std::uint8_t {
  /// Node-level values: Enabled and Mix.
  Node = 0,
  /// Tone panel values: White Balance, Exposure, Contrast, Whites, Blacks,
  /// Shadows, Highlights, Tone Curve.
  Tone,
  /// Look panel values: HLS, Saturation, Vibrance, Color Wheels, and the
  /// DRT/Post-owned Clarity, Sharpen, Halation, Film Grain.
  Look,
  /// LUT panel value: Look LUT.
  Lut,
  /// The complete Display Transform value of the DRT/Post endpoint.
  DisplayTransform,
  /// The all-or-none Masks row of a Color Grade.
  Masks,
};

/// Node row kind in the transferable-node column.
enum class AdjustmentTransferNodeKind : std::uint8_t {
  ColorGrade = 0,
  /// The single DRT and Post Processing endpoint row.
  DrtPost,
};

/**
 * @brief Immutable descriptor for one transferable item row.
 *
 * Carries stable domain identity plus display data. Never carries a row index,
 * a writable Model pointer, or a display label as identity.
 */
struct AdjustmentTransferItemDescriptor {
  AdjustmentTransferItemKind    kind = AdjustmentTransferItemKind::Adjustment;
  AdjustmentTransferItemSection section = AdjustmentTransferItemSection::Node;
  /// Stable identity when @p kind is @ref AdjustmentTransferItemKind::Adjustment;
  /// empty for every other kind.
  std::optional<AdjustmentInstanceId> adjustment_id;
  /// Adjustment type when @p kind is @ref AdjustmentTransferItemKind::Adjustment;
  /// empty for every other kind.
  OperatorTypeId                  type;
  std::string                     display_name;
  std::string                     display_value;
  /// False only for the Masks row of a Color Grade with no Masks.
  bool                            enabled      = true;
  /// 0-based position inside the owning node's item list.
  std::uint32_t                   source_order = 0;

  auto operator==(const AdjustmentTransferItemDescriptor&) const -> bool = default;
};

/**
 * @brief Immutable descriptor for one transferable node row.
 *
 * One row per Color Grade on the source image backbone plus one DRT/Post
 * endpoint row last. @p node_id is the stable identity.
 */
struct AdjustmentTransferNodeDescriptor {
  AdjustmentTransferNodeKind                        kind = AdjustmentTransferNodeKind::ColorGrade;
  NodeId                                            node_id;
  std::string                                       display_name;
  /// True when this row is the document default Color Grade.
  bool                                              is_default_grade = false;
  /// 0-based backbone position; the DRT/Post row is always last.
  std::uint32_t                                     source_order = 0;
  std::vector<AdjustmentTransferItemDescriptor>     items;

  auto operator==(const AdjustmentTransferNodeDescriptor&) const -> bool = default;
};

/// Immutable descriptor for one source Version row.
struct AdjustmentTransferVersionDescriptor {
  version_ref_id_t version_id;
  std::string      display_name;
  std::time_t      created_at   = 0;
  std::time_t      updated_at   = 0;
  /// True when this Version is the graph's active Version.
  bool             active       = false;
  /// 0-based position in the Version column.
  std::uint32_t    source_order = 0;

  auto             operator==(const AdjustmentTransferVersionDescriptor&) const
      -> bool = default;
};

/**
 * @brief Result of one read-only Version replay.
 *
 * @p document is the independent immutable replayed document owned by this
 * result value. It is destroyed when the result is replaced or released and is
 * never written back to the live guard or to project storage.
 */
struct AdjustmentTransferCatalogRead {
  AdjustmentTransferVersionDescriptor           version;
  PipelineDocument                              document;
  std::vector<AdjustmentTransferNodeDescriptor> nodes;
};

/**
 * @brief Read-only Version catalog for the Adjustment Transfer source column.
 *
 * Reads Version metadata from a const CommitGraph and replays one selected
 * Version from the immutable root document into an independent immutable
 * document. Never calls SetActiveVersionId, never rebuilds the live pipeline,
 * and never touches dirty state, WAL, render state, or project storage.
 */
class AdjustmentTransferCatalogService final {
 public:
  AdjustmentTransferCatalogService() = delete;

  /**
   * @brief List source Version descriptors in Version-column order.
   *
   * Order is creation time, then Version id — the same order the existing
   * Version column shows. Reads only const CommitGraph state.
   */
  [[nodiscard]] static auto ListVersions(const CommitGraph& graph)
      -> std::vector<AdjustmentTransferVersionDescriptor>;

  /**
   * @brief Replay one Version from the immutable root and build descriptors.
   *
   * Collects the Version's first-parent commits through
   * @ref FirstParentCommitsForHead and replays them through
   * @ref ReplayPipelineDocumentFromRoot onto @p root_document.
   *
   * @return The read result, or nullopt with @p error on an unknown Version id,
   *         a missing commit, a replay failure, or an invalid replayed document.
   *         The caller's prior read result is untouched and no live source
   *         state changes.
   */
  [[nodiscard]] static auto ReadVersion(const CommitGraph&      graph,
                                        const PipelineDocument& root_document,
                                        const version_ref_id_t& version_id,
                                        std::string*            error)
      -> std::optional<AdjustmentTransferCatalogRead>;

  /**
   * @brief Build node and item descriptors for an available document.
   *
   * Color Grade order follows @ref ColorGradesOnImageBackbone; item order
   * follows document and catalog order. Every Color Grade carries exactly one
   * Masks row, disabled when the Grade has no Masks. The DRT/Post row carries
   * the Display Transform item plus its owned adjustments.
   *
   * @return The descriptor list, or nullopt with @p error when the document
   *         lacks a valid DRT endpoint or an owned adjustment has a
   *         wrong-owner type.
   */
  [[nodiscard]] static auto BuildNodeDescriptors(const PipelineDocument& document,
                                                 std::string*            error)
      -> std::optional<std::vector<AdjustmentTransferNodeDescriptor>>;
};

}  // namespace alcedo
