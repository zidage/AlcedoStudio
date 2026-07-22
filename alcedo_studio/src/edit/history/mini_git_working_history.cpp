//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/mini_git_working_history.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <stdexcept>
#include <utility>

namespace alcedo {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

auto EncodeHead(const head_commit_hash_t& head) -> std::string {
  return HeadCommitHashToStorage(head);
}

auto DecodeHead(const nlohmann::json& value) -> head_commit_hash_t {
  if (!value.is_string()) {
    throw std::runtime_error("mini-Git journal head must be a string");
  }
  return HeadCommitHashFromStorage(value.get<std::string>());
}

auto RecordToJSON(const MiniGitJournalRecord& record) -> nlohmann::json {
  nlohmann::json j{{"format_version", 1},
                   {"kind", static_cast<int>(record.kind)},
                   {"expected_source_head", EncodeHead(record.expected_source_head)},
                   {"expected_source_chain_hash", record.expected_source_chain_hash.ToString()},
                   {"target_head", EncodeHead(record.target_head)},
                   {"target_chain_hash", record.target_chain_hash.ToString()}};
  j["edit_commit"] =
      record.edit_commit.has_value() ? record.edit_commit->ToJSON() : nlohmann::json(nullptr);
  return j;
}

auto RecordFromJSON(const nlohmann::json& j) -> MiniGitJournalRecord {
  if (!j.is_object() || j.value("format_version", 0) != 1 || !j.contains("kind") ||
      !j.contains("expected_source_head") || !j.contains("expected_source_chain_hash") ||
      !j.contains("target_head") || !j.contains("target_chain_hash") ||
      !j.contains("edit_commit")) {
    throw std::runtime_error("mini-Git journal record has an incompatible shape");
  }
  MiniGitJournalRecord record;
  const int            kind = j.at("kind").get<int>();
  if (kind == static_cast<int>(MiniGitJournalRecordKind::kEditCommit)) {
    record.kind = MiniGitJournalRecordKind::kEditCommit;
  } else if (kind == static_cast<int>(MiniGitJournalRecordKind::kHeadMove)) {
    record.kind = MiniGitJournalRecordKind::kHeadMove;
  } else {
    throw std::runtime_error("mini-Git journal record has an unknown kind");
  }
  record.expected_source_head = DecodeHead(j.at("expected_source_head"));
  record.expected_source_chain_hash =
      Hash128::FromString(j.at("expected_source_chain_hash").get<std::string>());
  record.target_head       = DecodeHead(j.at("target_head"));
  record.target_chain_hash = Hash128::FromString(j.at("target_chain_hash").get<std::string>());
  if (!j.at("edit_commit").is_null()) {
    record.edit_commit = EditCommit::FromJSON(j.at("edit_commit"));
  }
  return record;
}

auto ChecksumFor(const nlohmann::json& record) -> Hash128 {
  const auto bytes = record.dump();
  return Hash128::Compute(bytes.data(), bytes.size());
}

auto ValidateAndApplyRecord(CommitGraph& graph, const MiniGitJournalRecord& record,
                            std::string* error) -> bool {
  const auto active_version_id = graph.GetActiveVersionId();
  const auto source_head       = graph.GetActiveVersionRef().head_commit_hash;
  const auto source_chain      = graph.ChainHashForHead(source_head);
  if (record.expected_source_head != source_head ||
      record.expected_source_chain_hash != source_chain) {
    SetError(error, "mini-Git journal source head or chain hash does not match");
    return false;
  }

  if (record.kind == MiniGitJournalRecordKind::kEditCommit) {
    if (!record.edit_commit.has_value()) {
      SetError(error, "mini-Git edit record is missing its commit object");
      return false;
    }
    const auto& commit = *record.edit_commit;
    try {
      commit.ValidateStructure();
    } catch (const std::exception& e) {
      SetError(error, e.what());
      return false;
    }
    if (commit.GetKind() != EditCommitKind::kEdit || commit.GetRootId() != graph.GetRootId() ||
        commit.GetFirstParentHash() != source_head ||
        record.target_head != commit.GetCommitHash()) {
      SetError(error, "mini-Git edit record does not continue the checked-out first-parent path");
      return false;
    }
    if (record.target_chain_hash !=
        FoldTransactionChainHash(source_chain, commit.GetCommitHash())) {
      SetError(error, "mini-Git edit record has an invalid resulting chain hash");
      return false;
    }
    try {
      (void)graph.InsertCommit(commit);
      graph.MoveWorkingHead(active_version_id, record.target_head);
      return true;
    } catch (const std::exception& e) {
      SetError(error, e.what());
      return false;
    }
  }

  if (record.kind == MiniGitJournalRecordKind::kHeadMove) {
    if (record.edit_commit.has_value()) {
      SetError(error, "mini-Git head-move record must not carry a commit object");
      return false;
    }
    if (record.target_head.has_value() && graph.FindCommit(*record.target_head) == nullptr) {
      SetError(error, "mini-Git head-move target is missing from the commit graph");
      return false;
    }
    if (record.target_chain_hash != graph.ChainHashForHead(record.target_head)) {
      SetError(error, "mini-Git head-move record has an invalid target chain hash");
      return false;
    }
    try {
      graph.MoveWorkingHead(active_version_id, record.target_head);
      return true;
    } catch (const std::exception& e) {
      SetError(error, e.what());
      return false;
    }
  }

  SetError(error, "mini-Git journal record has an unknown kind");
  return false;
}

}  // namespace

auto MiniGitJournal::Append(const MiniGitJournalRecord& record, std::string* error) -> bool {
  try {
    if (!path_.empty()) {
      const auto           payload  = RecordToJSON(record);
      const auto           checksum = ChecksumFor(payload);
      const nlohmann::json frame{{"record", payload}, {"checksum", checksum.ToString()}};
      const auto           parent = path_.parent_path();
      if (!parent.empty()) {
        std::filesystem::create_directories(parent);
      }
      std::ofstream output(path_, std::ios::binary | std::ios::app);
      if (!output.is_open()) {
        SetError(error, "mini-Git journal file could not be opened for append");
        return false;
      }
      output << frame.dump() << '\n';
      output.flush();
      if (!output.good()) {
        SetError(error, "mini-Git journal file append failed");
        return false;
      }
    }
    records_.push_back(record);
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal append failed");
  }
  return false;
}

auto MiniGitJournal::Load(std::string* error) -> bool {
  if (path_.empty() || !std::filesystem::exists(path_)) {
    return true;
  }
  try {
    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
      SetError(error, "mini-Git journal file could not be opened for recovery");
      return false;
    }
    std::vector<MiniGitJournalRecord> loaded;
    std::string                       line;
    std::size_t                       line_number = 0;
    while (std::getline(input, line)) {
      ++line_number;
      if (line.empty()) {
        continue;
      }
      const auto frame = nlohmann::json::parse(line);
      if (!frame.is_object() || !frame.contains("record") || !frame.contains("checksum") ||
          !frame.at("checksum").is_string()) {
        throw std::runtime_error("mini-Git journal frame has an incompatible shape at line " +
                                 std::to_string(line_number));
      }
      const auto expected = Hash128::FromString(frame.at("checksum").get<std::string>());
      if (ChecksumFor(frame.at("record")) != expected) {
        throw std::runtime_error("mini-Git journal checksum mismatch at line " +
                                 std::to_string(line_number));
      }
      loaded.push_back(RecordFromJSON(frame.at("record")));
    }
    if (!input.eof()) {
      SetError(error, "mini-Git journal file read failed");
      return false;
    }
    records_ = std::move(loaded);
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal recovery failed");
  }
  return false;
}

MiniGitWorkingHistory::MiniGitWorkingHistory(std::shared_ptr<CommitGraph>             graph,
                                             std::shared_ptr<IMiniGitJournalAppender> journal)
    : graph_(std::move(graph)), journal_(std::move(journal)) {
  if (!graph_) {
    throw std::invalid_argument("MiniGitWorkingHistory requires a commit graph");
  }
  if (!journal_) {
    throw std::invalid_argument("MiniGitWorkingHistory requires a journal appender");
  }
}

auto MiniGitWorkingHistory::working_head() const -> head_commit_hash_t {
  return graph_->GetActiveVersionRef().head_commit_hash;
}

auto MiniGitWorkingHistory::transaction_chain_hash() const -> transaction_chain_hash_t {
  return graph_->ChainHashForHead(working_head());
}

auto MiniGitWorkingHistory::AppendEdit(OrdinaryEditPayload payload) -> MiniGitEditAppendResult {
  MiniGitEditAppendResult result;
  const auto              source_head  = working_head();
  const auto              source_chain = transaction_chain_hash();
  EditCommit              commit;
  try {
    commit = EditCommit::MakeEdit(graph_->GetRootId(), source_head, std::move(payload));
  } catch (const std::exception& e) {
    result.error = e.what();
    return result;
  }

  const auto target_chain = FoldTransactionChainHash(source_chain, commit.GetCommitHash());
  MiniGitJournalRecord record;
  record.kind                       = MiniGitJournalRecordKind::kEditCommit;
  record.expected_source_head       = source_head;
  record.expected_source_chain_hash = source_chain;
  record.target_head                = commit.GetCommitHash();
  record.target_chain_hash          = target_chain;
  record.edit_commit                = commit;

  if (!journal_->Append(record, &result.error)) {
    if (result.error.empty()) {
      result.error = "mini-Git journal rejected edit commit";
    }
    return result;
  }

  try {
    (void)graph_->InsertCommit(commit);
    graph_->MoveWorkingHead(graph_->GetActiveVersionId(), commit.GetCommitHash());
    redo_stack_.clear();
    result.committed = true;
    result.commit    = std::move(commit);
  } catch (const std::exception& e) {
    // A journaled record that cannot be reflected in the working graph is a
    // corruption-level condition. Do not claim the live working head advanced.
    result.error = e.what();
  }
  return result;
}

auto MiniGitWorkingHistory::AppendHeadMove(head_commit_hash_t         target_head,
                                           transaction_chain_hash_t   target_chain,
                                           std::optional<EditCommit>* selected_commit)
    -> MiniGitHeadMoveResult {
  MiniGitHeadMoveResult result;
  const auto            source_head  = working_head();
  const auto            source_chain = transaction_chain_hash();
  if (source_head == target_head) {
    return result;
  }

  MiniGitJournalRecord record;
  record.kind                       = MiniGitJournalRecordKind::kHeadMove;
  record.expected_source_head       = source_head;
  record.expected_source_chain_hash = source_chain;
  record.target_head                = target_head;
  record.target_chain_hash          = target_chain;
  if (!journal_->Append(record, &result.error)) {
    if (result.error.empty()) {
      result.error = "mini-Git journal rejected head move";
    }
    return result;
  }

  try {
    if (target_head.has_value() && selected_commit != nullptr) {
      *selected_commit = graph_->GetCommit(*target_head);
    }
    graph_->MoveWorkingHead(graph_->GetActiveVersionId(), target_head);
    result.moved = true;
    if (selected_commit != nullptr) {
      result.selected_commit = *selected_commit;
    }
  } catch (const std::exception& e) {
    result.error = e.what();
  }
  return result;
}

auto MiniGitWorkingHistory::Undo() -> MiniGitHeadMoveResult {
  if (!working_head().has_value()) {
    return {};
  }
  const auto abandoned = graph_->GetCommit(*working_head());
  const auto target    = abandoned.GetFirstParentHash();
  auto       selected  = std::optional<EditCommit>{};
  auto       result    = AppendHeadMove(target, graph_->ChainHashForHead(target), &selected);
  if (result.moved) {
    redo_stack_.push_back(abandoned.GetCommitHash());
    result.selected_commit = abandoned;
  }
  return result;
}

auto MiniGitWorkingHistory::Redo() -> MiniGitHeadMoveResult {
  if (redo_stack_.empty()) {
    return {};
  }
  const auto  target = redo_stack_.back();
  const auto& commit = graph_->GetCommit(target);
  if (commit.GetFirstParentHash() != working_head()) {
    return {.moved           = false,
            .selected_commit = std::nullopt,
            .error           = "mini-Git redo target is not a child of the working head"};
  }
  auto selected = std::optional<EditCommit>{};
  auto result   = AppendHeadMove(target, graph_->ChainHashForHead(target), &selected);
  if (result.moved) {
    redo_stack_.pop_back();
    result.selected_commit = commit;
  }
  return result;
}

auto MiniGitWorkingHistory::SelectVersion(const version_ref_id_t& version_id, std::string* error)
    -> bool {
  try {
    (void)graph_->GetVersionRef(version_id);
    graph_->SetActiveVersionId(version_id);
    redo_stack_.clear();
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  }
  return false;
}

auto MiniGitWorkingHistory::Replay(CommitGraph&                             graph,
                                   const std::vector<MiniGitJournalRecord>& records,
                                   std::string*                             error) -> bool {
  for (const auto& record : records) {
    if (!ValidateAndApplyRecord(graph, record, error)) {
      return false;
    }
  }
  return true;
}

auto MiniGitWorkingHistory::ReplaySkippingMaterializedPrefix(
    CommitGraph& graph, const std::vector<MiniGitJournalRecord>& records,
    std::size_t* applied_from_index, std::string* error) -> bool {
  if (records.empty()) {
    if (applied_from_index != nullptr) {
      *applied_from_index = 0;
    }
    return true;
  }

  const auto stored_head  = graph.GetActiveVersionRef().head_commit_hash;
  const auto stored_chain = graph.ChainHashForHead(stored_head);

  // Fast path: the first record continues from the stored head.
  if (records.front().expected_source_head == stored_head &&
      records.front().expected_source_chain_hash == stored_chain) {
    if (applied_from_index != nullptr) {
      *applied_from_index = 0;
    }
    return Replay(graph, records, error);
  }

  // Crash window: DuckDB already holds the fold result and the journal still
  // contains the fully materialized prefix. Every edit commit must already be
  // present and the last record must land on the stored head/chain.
  for (const auto& record : records) {
    if (record.kind == MiniGitJournalRecordKind::kEditCommit) {
      if (!record.edit_commit.has_value() ||
          graph.FindCommit(record.edit_commit->GetCommitHash()) == nullptr) {
        SetError(error,
                 "mini-Git journal has an unmaterialized commit that does not continue the "
                 "stored head");
        return false;
      }
    } else if (record.kind == MiniGitJournalRecordKind::kHeadMove) {
      if (record.target_head.has_value() && graph.FindCommit(*record.target_head) == nullptr) {
        SetError(error,
                 "mini-Git journal head-move target is missing from the materialized graph");
        return false;
      }
    }
  }
  if (records.back().target_head != stored_head ||
      records.back().target_chain_hash != stored_chain) {
    SetError(error,
             "mini-Git journal cannot be aligned with the stored materialized head for recovery");
    return false;
  }
  // Entire journal is already reflected by DuckDB; nothing to fold.
  if (applied_from_index != nullptr) {
    *applied_from_index = records.size();
  }
  return true;
}

auto MiniGitJournal::TruncateMaterialized(std::string* error) -> bool {
  try {
    records_.clear();
    if (path_.empty()) {
      return true;
    }
    if (std::filesystem::exists(path_)) {
      std::ofstream output(path_, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        SetError(error, "mini-Git journal file could not be truncated");
        return false;
      }
      output.flush();
      if (!output.good()) {
        SetError(error, "mini-Git journal file truncate failed");
        return false;
      }
    }
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal truncate failed");
  }
  return false;
}

}  // namespace alcedo
