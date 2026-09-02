//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/mini_git_working_history.hpp"

#include <algorithm>
#include <chrono>
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
  nlohmann::json j{{"format_version", 2},
                   {"sequence", record.sequence},
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
  if (!j.is_object() || !j.contains("kind") || !j.contains("expected_source_head") ||
      !j.contains("expected_source_chain_hash") || !j.contains("target_head") ||
      !j.contains("target_chain_hash") || !j.contains("edit_commit")) {
    throw std::runtime_error("mini-Git journal record has an incompatible shape");
  }
  const int format_version = j.value("format_version", 0);
  if (format_version != 1 && format_version != 2) {
    throw std::runtime_error("mini-Git journal record has an incompatible format version");
  }
  MiniGitJournalRecord record;
  if (format_version >= 2) {
    if (!j.contains("sequence") || !j.at("sequence").is_number_unsigned()) {
      throw std::runtime_error("mini-Git journal record is missing a sequence number");
    }
    record.sequence = j.at("sequence").get<std::uint64_t>();
    if (record.sequence == 0) {
      throw std::runtime_error("mini-Git journal record sequence must be non-zero");
    }
  }
  const int kind = j.at("kind").get<int>();
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
    const bool is_edit  = commit.GetKind() == EditCommitKind::kEdit;
    const bool is_merge = commit.GetKind() == EditCommitKind::kMerge;
    if ((!is_edit && !is_merge) || commit.GetRootId() != graph.GetRootId() ||
        commit.GetFirstParentHash() != source_head ||
        record.target_head != commit.GetCommitHash()) {
      SetError(error, "mini-Git commit record does not continue the checked-out first-parent path");
      return false;
    }
    if (is_merge && commit.GetSecondParentHash().has_value() &&
        graph.FindCommit(*commit.GetSecondParentHash()) == nullptr) {
      SetError(error, "mini-Git merge record second parent is missing from the commit graph");
      return false;
    }
    if (record.target_chain_hash !=
        FoldTransactionChainHash(source_chain, commit.GetCommitHash())) {
      SetError(error, "mini-Git commit record has an invalid resulting chain hash");
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

auto MiniGitJournal::AppendUnlocked(const MiniGitJournalRecord& record, std::string* error)
    -> bool {
  try {
    MiniGitJournalRecord stored = record;
    stored.sequence             = next_sequence_;
    if (!path_.empty()) {
      const auto           payload  = RecordToJSON(stored);
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
    records_.push_back(std::move(stored));
    ++next_sequence_;
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal append failed");
  }
  return false;
}

auto MiniGitJournal::Append(const MiniGitJournalRecord& record, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return AppendUnlocked(record, error);
}

auto MiniGitJournal::Load(std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  if (path_.empty() || !std::filesystem::exists(path_)) {
    records_.clear();
    next_sequence_ = 1;
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
    std::size_t                       line_number    = 0;
    std::uint64_t                     next_sequence  = 1;
    std::uint64_t                     fallback_index = 0;
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
      auto record = RecordFromJSON(frame.at("record"));
      // format_version 1 files predate durable sequence numbers; assign a
      // stable load-time sequence so capture/truncate still have a range.
      if (record.sequence == 0) {
        record.sequence = ++fallback_index;
      }
      if (!loaded.empty() && record.sequence <= loaded.back().sequence) {
        throw std::runtime_error("mini-Git journal sequence is not strictly increasing at line " +
                                 std::to_string(line_number));
      }
      next_sequence = std::max(next_sequence, record.sequence + 1);
      loaded.push_back(std::move(record));
    }
    if (!input.eof()) {
      SetError(error, "mini-Git journal file read failed");
      return false;
    }
    records_       = std::move(loaded);
    next_sequence_ = next_sequence;
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal recovery failed");
  }
  return false;
}

auto MiniGitJournal::Snapshot() const -> MiniGitJournalSnapshot {
  std::scoped_lock       lock(mutex_);
  MiniGitJournalSnapshot snapshot;
  snapshot.records = records_;
  if (!records_.empty()) {
    snapshot.first_sequence = records_.front().sequence;
    snapshot.last_sequence  = records_.back().sequence;
  }
  return snapshot;
}

auto MiniGitJournal::records() const -> std::vector<MiniGitJournalRecord> {
  std::scoped_lock lock(mutex_);
  return records_;
}

auto MiniGitJournal::next_sequence() const -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  return next_sequence_;
}

void MiniGitJournal::SetRecords(std::vector<MiniGitJournalRecord> records) {
  std::scoped_lock lock(mutex_);
  records_ = std::move(records);
  std::uint64_t next = 1;
  for (const auto& record : records_) {
    if (record.sequence >= next) {
      next = record.sequence + 1;
    }
  }
  next_sequence_ = next;
}

auto MiniGitJournal::RewriteFileUnlocked(std::string* error) -> bool {
  if (path_.empty()) {
    return true;
  }
  try {
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      SetError(error, "mini-Git journal file could not be rewritten");
      return false;
    }
    for (const auto& record : records_) {
      const auto           payload  = RecordToJSON(record);
      const auto           checksum = ChecksumFor(payload);
      const nlohmann::json frame{{"record", payload}, {"checksum", checksum.ToString()}};
      output << frame.dump() << '\n';
    }
    output.flush();
    if (!output.good()) {
      SetError(error, "mini-Git journal file rewrite failed");
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal rewrite failed");
  }
  return false;
}

auto MiniGitJournal::TruncateThroughSequence(std::uint64_t last_sequence, std::string* error)
    -> bool {
  std::scoped_lock lock(mutex_);
  try {
    records_.erase(std::remove_if(records_.begin(), records_.end(),
                                  [last_sequence](const MiniGitJournalRecord& record) {
                                    return record.sequence <= last_sequence;
                                  }),
                   records_.end());
    return RewriteFileUnlocked(error);
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal range truncate failed");
  }
  return false;
}

auto MiniGitJournal::TruncateMaterialized(std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  try {
    records_.clear();
    // Keep next_sequence_ so later appends never reuse a truncated sequence.
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

auto MiniGitJournal::RevokeLastRecord(std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  try {
    if (records_.empty()) {
      SetError(error, "mini-Git journal has no record to revoke");
      return false;
    }
    records_.pop_back();
    return RewriteFileUnlocked(error);
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal tail revoke failed");
  }
  return false;
}

auto MiniGitJournal::IsolateJournalFile(const std::filesystem::path& path, std::string* error)
    -> bool {
  try {
    if (path.empty() || !std::filesystem::exists(path)) {
      return true;
    }
    const auto stamp =
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const auto isolated = path.string() + ".isolated." + stamp;
    std::error_code ec;
    std::filesystem::rename(path, isolated, ec);
    if (ec) {
      SetError(error, "mini-Git journal isolate failed: " + ec.message());
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal isolate failed");
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

auto MiniGitWorkingHistory::WorkingSelection() const -> MiniGitWorkingSelection {
  return MiniGitWorkingSelection{.redo_suffix = redo_stack_};
}

void MiniGitWorkingHistory::PublishWorkingSelection(MiniGitWorkingSelection selection) {
  redo_stack_ = std::move(selection.redo_suffix);
}

auto MiniGitWorkingHistory::PrepareAppendEdit(PipelineEditBatch payload) const
    -> MiniGitPreparedEdit {
  MiniGitPreparedEdit prepared;
  const auto          source_head  = working_head();
  const auto          source_chain = transaction_chain_hash();
  try {
    prepared.commit =
        EditCommit::MakePipelineEdit(graph_->GetRootId(), source_head, std::move(payload));
  } catch (const std::exception& e) {
    prepared.error = e.what();
    return prepared;
  }

  prepared.target_chain = FoldTransactionChainHash(source_chain, prepared.commit.GetCommitHash());
  prepared.journal_record.kind                       = MiniGitJournalRecordKind::kEditCommit;
  prepared.journal_record.expected_source_head       = source_head;
  prepared.journal_record.expected_source_chain_hash = source_chain;
  prepared.journal_record.target_head                = prepared.commit.GetCommitHash();
  prepared.journal_record.target_chain_hash          = prepared.target_chain;
  prepared.journal_record.edit_commit                = prepared.commit;
  prepared.ready                                     = true;
  return prepared;
}

auto MiniGitWorkingHistory::PrepareAppendEdit(OrdinaryEditPayload payload) const
    -> MiniGitPreparedEdit {
  MiniGitPreparedEdit prepared;
  const auto          source_head  = working_head();
  const auto          source_chain = transaction_chain_hash();
  try {
    prepared.commit = EditCommit::MakeEdit(graph_->GetRootId(), source_head, std::move(payload));
  } catch (const std::exception& e) {
    prepared.error = e.what();
    return prepared;
  }

  prepared.target_chain = FoldTransactionChainHash(source_chain, prepared.commit.GetCommitHash());
  prepared.journal_record.kind                       = MiniGitJournalRecordKind::kEditCommit;
  prepared.journal_record.expected_source_head       = source_head;
  prepared.journal_record.expected_source_chain_hash = source_chain;
  prepared.journal_record.target_head                = prepared.commit.GetCommitHash();
  prepared.journal_record.target_chain_hash          = prepared.target_chain;
  prepared.journal_record.edit_commit                = prepared.commit;
  prepared.ready                                     = true;
  return prepared;
}

auto MiniGitWorkingHistory::PrepareAppendMerge(commit_hash_t     second_parent,
                                               MergeEditPayload  payload) const
    -> MiniGitPreparedEdit {
  MiniGitPreparedEdit prepared;
  const auto          source_head  = working_head();
  const auto          source_chain = transaction_chain_hash();
  if (graph_->FindCommit(second_parent) == nullptr) {
    prepared.error = "mini-Git merge second parent is not in the commit graph";
    return prepared;
  }
  try {
    prepared.commit = EditCommit::MakeMerge(graph_->GetRootId(), source_head, second_parent,
                                            std::move(payload));
  } catch (const std::exception& e) {
    prepared.error = e.what();
    return prepared;
  }

  prepared.target_chain = FoldTransactionChainHash(source_chain, prepared.commit.GetCommitHash());
  prepared.journal_record.kind                       = MiniGitJournalRecordKind::kEditCommit;
  prepared.journal_record.expected_source_head       = source_head;
  prepared.journal_record.expected_source_chain_hash = source_chain;
  prepared.journal_record.target_head                = prepared.commit.GetCommitHash();
  prepared.journal_record.target_chain_hash          = prepared.target_chain;
  prepared.journal_record.edit_commit                = prepared.commit;
  prepared.ready                                     = true;
  return prepared;
}

auto MiniGitWorkingHistory::PublishPreparedEdit(const MiniGitPreparedEdit& prepared)
    -> MiniGitEditAppendResult {
  MiniGitEditAppendResult result;
  if (!prepared.ready) {
    result.error = prepared.error.empty() ? "mini-Git edit is not prepared" : prepared.error;
    return result;
  }
  if (prepared.journal_record.expected_source_head != working_head() ||
      prepared.journal_record.expected_source_chain_hash != transaction_chain_hash()) {
    result.error = "mini-Git prepared edit no longer matches the working head";
    return result;
  }
  if (!journal_->Append(prepared.journal_record, &result.error)) {
    if (result.error.empty()) {
      result.error = "mini-Git journal rejected edit commit";
    }
    return result;
  }
  try {
    if (!graph_->InsertCommit(prepared.commit)) {
      if (auto* durable = dynamic_cast<MiniGitJournal*>(journal_.get())) {
        std::string revoke_error;
        (void)durable->RevokeLastRecord(&revoke_error);
      }
      result.error = "mini-Git commit is already in the graph";
      return result;
    }
    graph_->MoveWorkingHead(graph_->GetActiveVersionId(), prepared.commit.GetCommitHash());
    redo_stack_.clear();
    result.committed = true;
    result.commit    = prepared.commit;
  } catch (const std::exception& e) {
    // WAL is durable but the unique history instance did not advance. Revoke only
    // this tail record so earlier recovery records remain valid.
    if (auto* durable = dynamic_cast<MiniGitJournal*>(journal_.get())) {
      std::string revoke_error;
      (void)durable->RevokeLastRecord(&revoke_error);
    }
    result.error = e.what();
  }
  return result;
}

auto MiniGitWorkingHistory::AbandonPublishedEdit(const MiniGitPreparedEdit& prepared,
                                                 MiniGitWorkingSelection prior_selection,
                                                 std::string* error) -> bool {
  if (!prepared.ready) {
    SetError(error, "mini-Git edit is not prepared");
    return false;
  }
  try {
    if (working_head() == prepared.commit.GetCommitHash()) {
      graph_->MoveWorkingHead(graph_->GetActiveVersionId(),
                              prepared.journal_record.expected_source_head);
      const auto unreachable = graph_->ListUnreachableCommitHashes();
      std::vector<commit_hash_t> to_erase;
      for (const auto& hash : unreachable) {
        if (hash == prepared.commit.GetCommitHash()) {
          to_erase.push_back(hash);
        }
      }
      if (!to_erase.empty()) {
        graph_->EraseUnreachableCommits(to_erase);
      }
    }
    redo_stack_ = std::move(prior_selection.redo_suffix);
    if (auto* durable = dynamic_cast<MiniGitJournal*>(journal_.get())) {
      if (!durable->RevokeLastRecord(error)) {
        return false;
      }
    }
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  }
  return false;
}

auto MiniGitWorkingHistory::AppendEdit(PipelineEditBatch payload) -> MiniGitEditAppendResult {
  return PublishPreparedEdit(PrepareAppendEdit(std::move(payload)));
}

auto MiniGitWorkingHistory::AppendEdit(OrdinaryEditPayload payload) -> MiniGitEditAppendResult {
  return PublishPreparedEdit(PrepareAppendEdit(std::move(payload)));
}

auto MiniGitWorkingHistory::AppendMerge(commit_hash_t second_parent, MergeEditPayload payload)
    -> MiniGitEditAppendResult {
  return PublishPreparedEdit(PrepareAppendMerge(second_parent, std::move(payload)));
}

namespace {

auto MakeHeadMoveJournalRecord(const head_commit_hash_t& source_head,
                               const transaction_chain_hash_t& source_chain,
                               const head_commit_hash_t& target_head,
                               const transaction_chain_hash_t& target_chain)
    -> MiniGitJournalRecord {
  MiniGitJournalRecord record;
  record.kind                       = MiniGitJournalRecordKind::kHeadMove;
  record.expected_source_head       = source_head;
  record.expected_source_chain_hash = source_chain;
  record.target_head                = target_head;
  record.target_chain_hash          = target_chain;
  return record;
}

}  // namespace

auto MiniGitWorkingHistory::PrepareUndo() const -> MiniGitPreparedHeadMove {
  MiniGitPreparedHeadMove prepared;
  if (!working_head().has_value()) {
    prepared.ready   = true;
    prepared.is_noop = true;
    prepared.next_selection.redo_suffix = redo_stack_;
    return prepared;
  }
  const auto abandoned = graph_->GetCommit(*working_head());
  const auto target    = abandoned.GetFirstParentHash();
  prepared.backward    = true;
  prepared.target_head = target;
  prepared.target_chain = graph_->ChainHashForHead(target);
  prepared.journal_record =
      MakeHeadMoveJournalRecord(working_head(), transaction_chain_hash(), target,
                                prepared.target_chain);
  prepared.traversed_commits.push_back(abandoned);
  prepared.selected_commit = abandoned;
  prepared.next_selection.redo_suffix = redo_stack_;
  prepared.next_selection.redo_suffix.push_back(abandoned.GetCommitHash());
  prepared.ready = true;
  return prepared;
}

auto MiniGitWorkingHistory::PrepareRedo() const -> MiniGitPreparedHeadMove {
  MiniGitPreparedHeadMove prepared;
  if (redo_stack_.empty()) {
    prepared.ready   = true;
    prepared.is_noop = true;
    prepared.next_selection.redo_suffix = redo_stack_;
    return prepared;
  }
  const auto  target = redo_stack_.back();
  const auto& commit = graph_->GetCommit(target);
  if (commit.GetFirstParentHash() != working_head()) {
    prepared.error = "mini-Git redo target is not a child of the working head";
    return prepared;
  }
  prepared.backward     = false;
  prepared.target_head  = target;
  prepared.target_chain = graph_->ChainHashForHead(target);
  prepared.journal_record =
      MakeHeadMoveJournalRecord(working_head(), transaction_chain_hash(), target,
                                prepared.target_chain);
  prepared.traversed_commits.push_back(commit);
  prepared.selected_commit = commit;
  prepared.next_selection.redo_suffix = redo_stack_;
  prepared.next_selection.redo_suffix.pop_back();
  prepared.ready = true;
  return prepared;
}

auto MiniGitWorkingHistory::PrepareMoveHeadToCommit(const commit_hash_t& target) const
    -> MiniGitPreparedHeadMove {
  MiniGitPreparedHeadMove prepared;
  const auto              source_head = working_head();
  if (source_head == target) {
    prepared.ready   = true;
    prepared.is_noop = true;
    prepared.target_head = target;
    prepared.target_chain = graph_->ChainHashForHead(target);
    prepared.next_selection.redo_suffix = redo_stack_;
    return prepared;
  }

  // Backward: target is an ancestor of the working head on the first-parent chain.
  if (source_head.has_value()) {
    const auto chain     = graph_->FirstParentChain(source_head);
    const auto target_it = std::find(chain.begin(), chain.end(), target);
    if (target_it != chain.end()) {
      std::vector<commit_hash_t> traversed_hashes(
          chain.begin() + (target_it - chain.begin()) + 1, chain.end());
      // [target.child, ..., head] root→head; reverse to newest-first apply order and
      // push order so target.child lands at back() of the redo suffix.
      std::reverse(traversed_hashes.begin(), traversed_hashes.end());
      prepared.backward = true;
      for (const auto& hash : traversed_hashes) {
        prepared.traversed_commits.push_back(graph_->GetCommit(hash));
      }
      prepared.selected_commit = graph_->GetCommit(target);
      prepared.target_head     = target;
      prepared.target_chain    = graph_->ChainHashForHead(target);
      prepared.journal_record =
          MakeHeadMoveJournalRecord(source_head, transaction_chain_hash(), target,
                                    prepared.target_chain);
      prepared.next_selection.redo_suffix = redo_stack_;
      for (const auto& hash : traversed_hashes) {
        prepared.next_selection.redo_suffix.push_back(hash);
      }
      prepared.ready = true;
      return prepared;
    }
  }

  // Forward: target must be a member of the in-memory redo suffix.
  const auto redo_it = std::find(redo_stack_.begin(), redo_stack_.end(), target);
  if (redo_it != redo_stack_.end()) {
    std::vector<commit_hash_t> traversed_hashes(redo_it, redo_stack_.end());
    std::reverse(traversed_hashes.begin(), traversed_hashes.end());
    head_commit_hash_t prev = source_head;
    for (const auto& hash : traversed_hashes) {
      const auto& commit = graph_->GetCommit(hash);
      if (commit.GetFirstParentHash() != prev) {
        prepared.error =
            "mini-Git redo suffix is not a contiguous chain to the target commit";
        return prepared;
      }
      prev = hash;
    }
    prepared.backward = false;
    for (const auto& hash : traversed_hashes) {
      prepared.traversed_commits.push_back(graph_->GetCommit(hash));
    }
    prepared.selected_commit = graph_->GetCommit(target);
    prepared.target_head     = target;
    prepared.target_chain    = graph_->ChainHashForHead(target);
    prepared.journal_record =
        MakeHeadMoveJournalRecord(source_head, transaction_chain_hash(), target,
                                  prepared.target_chain);
    // Keep the more-future prefix; drop everything from target through head-child.
    prepared.next_selection.redo_suffix.assign(redo_stack_.begin(), redo_it);
    prepared.ready = true;
    return prepared;
  }

  prepared.error =
      "Commit is not on the active Version's first-parent path or redo suffix";
  return prepared;
}

auto MiniGitWorkingHistory::PublishPreparedHeadMove(const MiniGitPreparedHeadMove& prepared)
    -> MiniGitHeadMoveResult {
  MiniGitHeadMoveResult result;
  if (!prepared.ready) {
    result.error = prepared.error.empty() ? "mini-Git head move is not prepared" : prepared.error;
    return result;
  }
  if (prepared.is_noop) {
    result.moved = false;
    result.backward = prepared.backward;
    result.selected_commit = prepared.selected_commit;
    result.traversed_commits = prepared.traversed_commits;
    return result;
  }
  if (prepared.journal_record.expected_source_head != working_head() ||
      prepared.journal_record.expected_source_chain_hash != transaction_chain_hash()) {
    result.error = "mini-Git prepared head move no longer matches the working head";
    return result;
  }
  if (!journal_->Append(prepared.journal_record, &result.error)) {
    if (result.error.empty()) {
      result.error = "mini-Git journal rejected head move";
    }
    return result;
  }
  try {
    graph_->MoveWorkingHead(graph_->GetActiveVersionId(), prepared.target_head);
    redo_stack_              = prepared.next_selection.redo_suffix;
    result.moved             = true;
    result.backward          = prepared.backward;
    result.selected_commit   = prepared.selected_commit;
    result.traversed_commits = prepared.traversed_commits;
  } catch (const std::exception& e) {
    if (auto* durable = dynamic_cast<MiniGitJournal*>(journal_.get())) {
      std::string revoke_error;
      (void)durable->RevokeLastRecord(&revoke_error);
    }
    result.error = e.what();
  }
  return result;
}

auto MiniGitWorkingHistory::AbandonPublishedHeadMove(const MiniGitPreparedHeadMove& prepared,
                                                     MiniGitWorkingSelection prior_selection,
                                                     std::string* error) -> bool {
  if (!prepared.ready || prepared.is_noop) {
    SetError(error, "mini-Git head move is not prepared");
    return false;
  }
  try {
    if (working_head() == prepared.target_head) {
      graph_->MoveWorkingHead(graph_->GetActiveVersionId(),
                              prepared.journal_record.expected_source_head);
    }
    redo_stack_ = std::move(prior_selection.redo_suffix);
    if (auto* durable = dynamic_cast<MiniGitJournal*>(journal_.get())) {
      if (!durable->RevokeLastRecord(error)) {
        return false;
      }
    }
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  }
  return false;
}

auto MiniGitWorkingHistory::Undo() -> MiniGitHeadMoveResult {
  return PublishPreparedHeadMove(PrepareUndo());
}

auto MiniGitWorkingHistory::Redo() -> MiniGitHeadMoveResult {
  return PublishPreparedHeadMove(PrepareRedo());
}

auto MiniGitWorkingHistory::MoveHeadToCommit(const commit_hash_t& target, std::string* error)
    -> MiniGitHeadMoveResult {
  auto prepared = PrepareMoveHeadToCommit(target);
  if (!prepared.ready) {
    SetError(error, prepared.error);
    return {.moved = false, .error = prepared.error};
  }
  auto result = PublishPreparedHeadMove(prepared);
  if (!result.error.empty()) {
    SetError(error, result.error);
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

auto MiniGitWorkingHistory::AlignJournalWithStoredHead(
    const CommitGraph& graph, const std::vector<MiniGitJournalRecord>& records)
    -> JournalAlignment {
  JournalAlignment alignment;
  if (records.empty()) {
    alignment.accepted      = true;
    alignment.fully_covered = true;
    return alignment;
  }

  const auto stored_head  = graph.GetActiveVersionRef().head_commit_hash;
  const auto stored_chain = graph.ChainHashForHead(stored_head);

  // Contiguous parent/chain links across the entire durable journal.
  for (std::size_t index = 1; index < records.size(); ++index) {
    const auto& record = records[index];
    if (record.expected_source_head != records[index - 1].target_head ||
        record.expected_source_chain_hash != records[index - 1].target_chain_hash) {
      alignment.broken = true;
      alignment.error  = "mini-Git journal records are not a contiguous head sequence";
      return alignment;
    }
  }

  // Longest prefix whose target matches the durable head and whose commits are
  // already present (crash after DB commit before WAL clear).
  std::size_t durable_prefix_size = 0;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    if (record.kind == MiniGitJournalRecordKind::kEditCommit) {
      if (!record.edit_commit.has_value() ||
          graph.FindCommit(record.edit_commit->GetCommitHash()) == nullptr) {
        break;
      }
    } else if (record.kind == MiniGitJournalRecordKind::kHeadMove) {
      if (record.target_head.has_value() && graph.FindCommit(*record.target_head) == nullptr) {
        break;
      }
    } else {
      alignment.broken = true;
      alignment.error  = "mini-Git journal record has an unknown kind";
      return alignment;
    }
    if (record.target_head == stored_head && record.target_chain_hash == stored_chain) {
      durable_prefix_size = index + 1;
    }
  }

  if (durable_prefix_size == records.size()) {
    alignment.accepted           = true;
    alignment.fully_covered      = true;
    alignment.missing_from_index = records.size();
    return alignment;
  }

  if (durable_prefix_size == 0) {
    if (records.front().expected_source_head != stored_head ||
        records.front().expected_source_chain_hash != stored_chain) {
      alignment.broken = true;
      alignment.error =
          "mini-Git journal cannot be aligned with the stored history head for recovery";
      return alignment;
    }
  }

  // Remaining suffix must connect from durable head (or last covered record).
  const auto& first_missing = records[durable_prefix_size];
  if (durable_prefix_size == 0) {
    if (first_missing.expected_source_head != stored_head ||
        first_missing.expected_source_chain_hash != stored_chain) {
      alignment.broken = true;
      alignment.error =
          "mini-Git journal missing suffix does not continue the stored history head";
      return alignment;
    }
  }

  alignment.accepted             = true;
  alignment.contiguous_extension = true;
  alignment.missing_from_index   = durable_prefix_size;
  return alignment;
}

auto MiniGitWorkingHistory::ReplaySkippingMaterializedPrefix(
    CommitGraph& graph, const std::vector<MiniGitJournalRecord>& records,
    std::size_t* applied_from_index, std::string* error) -> bool {
  const auto alignment = AlignJournalWithStoredHead(graph, records);
  if (!alignment.accepted) {
    SetError(error, alignment.error);
    return false;
  }
  if (applied_from_index != nullptr) {
    *applied_from_index = alignment.missing_from_index;
  }
  if (alignment.fully_covered || alignment.missing_from_index >= records.size()) {
    return true;
  }
  return Replay(graph,
                std::vector<MiniGitJournalRecord>(
                    records.begin() + static_cast<std::ptrdiff_t>(alignment.missing_from_index),
                    records.end()),
                error);
}

}  // namespace alcedo
