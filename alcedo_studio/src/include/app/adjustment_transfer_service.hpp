//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "app/adjustment_transfer_types.hpp"
#include "app/document_transfer.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Façade for Copy/Paste of transferable Color Grade, Mask, and DRT/Post data.
 *
 * Capture and JSON live in @ref CaptureDocumentTransfer. Paste builds one typed
 * batch and one root-relative Version. This type does not create merge commits.
 */
class AdjustmentTransferService final {
 public:
  AdjustmentTransferService() = delete;

  /**
   * @brief Capture transferable DAG data from @p document.
   *
   * @param document Source pipeline DAG.
   * @param mask_store Required when the document references persistent Brush keys.
   */
  [[nodiscard]] static auto Capture(const PipelineDocument& document,
                                    MaskStore*              mask_store = nullptr)
      -> AdjustmentTransferPackage;

  /**
   * @brief Parse a portable transfer document. Operator-list packages are rejected.
   */
  [[nodiscard]] static auto ImportPackage(const nlohmann::json& package_json)
      -> AdjustmentTransferPackage;

  /** @brief Canonical JSON for clipboard or disk. Includes fingerprint. */
  [[nodiscard]] static auto ExportPackage(const AdjustmentTransferPackage& package)
      -> nlohmann::json;

  /** @brief Hash of the canonical package without the fingerprint field. */
  [[nodiscard]] static auto PackageFingerprint(const AdjustmentTransferPackage& package)
      -> std::string;

  /**
   * @brief Paste as a new Version at the target image root.
   *
   * Validates and remaps before any graph mutation. Inserts one typed Paste
   * commit whose first parent is the root. Sets the new Version active.
   * Does not mutate a live document; callers rebuild from the new head.
   *
   * @param graph Caller-owned commit graph.
   * @param root_document Target immutable root. Develop and geometry stay here.
   * @param package Validated transfer document.
   * @param version_display_name Requested Version label; uniquified if needed.
   * @param options Identity source and Mask stores.
   */
  [[nodiscard]] static auto PasteAsRootRelativeVersion(
      CommitGraph& graph, const PipelineDocument& root_document,
      const AdjustmentTransferPackage& package, std::string version_display_name,
      const DocumentTransferPasteOptions& options = {}) -> AdjustmentPasteResult;
};

}  // namespace alcedo
