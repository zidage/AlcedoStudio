//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "edit/history/edit_history.hpp"
#include "edit/history/editor_journal_recovery.hpp"
#include "edit/history/editor_journal_writer.hpp"
#include "edit/history/editor_transaction_journal.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "json.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"

namespace alcedo {

struct EditorMaterializeRequest {
  EditorJournalIdentity identity{};
  /// Last durable edit-history operation sequence to include. Zero means "no
  /// additional journal REDO; rewrite recovery metadata for the current Version".
  std::uint64_t         target_operation_sequence = 0;
  /// Expected chain hash before applying unmaterialized operations (optional).
  Hash128               expected_materialized_chain_hash{};
  std::uint64_t         expected_materialized_operation_sequence = 0;
  bool                  validate_expected_materialized_head     = false;
};

struct EditorMaterializeResult {
  bool          accepted     = false;
  bool          materialized = false;
  std::uint64_t materialized_operation_sequence = 0;
  Hash128       transaction_chain_hash{};
  Hash128       pipeline_parameter_hash{};
  std::string   error;
};

/// Owns replay through a selected durable record sequence and the single DuckDB
/// history / pipeline / recovery-metadata update required by Phase 5H.
class EditorHistoryMaterializer final {
 public:
  explicit EditorHistoryMaterializer(std::shared_ptr<Storage> storage);

  /// Materialize journal-committed state into DuckDB using one connection and
  /// one transaction. Separate SaveHistory/SavePipeline calls are intentionally
  /// not used here.
  auto Materialize(const EditorMaterializeRequest& request, EditorTransactionJournal* journal,
                   const std::shared_ptr<EditHistory>& history,
                   const nlohmann::json&               pipeline_params,
                   std::string*                        error = nullptr) -> EditorMaterializeResult;

  /// Recover from an on-disk journal plus stored recovery metadata, then
  /// materialize any REDO'd journal-committed operations.
  auto RecoverAndMaterialize(const EditorJournalIdentity& identity,
                             EditorTransactionJournal* journal,
                             const std::shared_ptr<EditHistory>& history,
                             const std::optional<nlohmann::json>& stored_pipeline_params,
                             std::string* error = nullptr) -> EditorMaterializeResult;

  auto LoadRecoveryMetadata(sl_element_id_t element_id) const
      -> std::optional<EditorRecoveryMetadata>;

 private:
  std::shared_ptr<Storage> storage_;
};

}  // namespace alcedo
