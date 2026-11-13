//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_ai_mask_edit_service_test.cpp
/// @brief AI Mask edit Open, Commit, and Cancel through the production session facade over the
///        real history port (SM5.3): asset write before the history publish, one AddMask or
///        ReplaceMaskSource, main Undo and Redo, failure retention, and the lease.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "app/ai_mask_analysis_image.hpp"
#include "app/editor_action_policy.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/editor_session_test_ports.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

constexpr sl_element_id_t kElementId      = 52;
constexpr image_id_t      kImageId        = 520;
constexpr sl_element_id_t kOtherElementId = 53;
constexpr image_id_t      kOtherImageId   = 530;
constexpr Extent2D        kAnalysisExtent{64, 48};
/// Every write bit: Windows makes a file read-only only when all of them are removed.
constexpr auto            kAllWrite = std::filesystem::perms::owner_write |
                           std::filesystem::perms::group_write |
                           std::filesystem::perms::others_write;

auto AiMaskEditTestPath(std::string_view name) -> std::filesystem::path {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  auto dir = std::filesystem::path{"build/tmp/editor_ai_mask_edit"};
  std::filesystem::create_directories(dir);
  return dir / (std::string{name} + "_" + stamp + ".wal");
}

/// Records editor frame requests; frames complete only when the test reports them.
class RecordingScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request,
                EditorPipelineScheduleCompletion /*on_complete*/) -> std::uint64_t override {
    scheduled.push_back(request);
    return ++next_job;
  }
  void                             Cancel(std::uint64_t /*job_id*/) override {}

  std::vector<EditorRenderRequest> scheduled;
  std::uint64_t                    next_job = 0;
};

/// In-memory asset store that records each put and can refuse puts with a fixed reason.
class RecordingMaskAssetPort final : public IEditorMaskAssetPort {
 public:
  auto PutMaskAsset(const MaskAsset& asset, std::string* error) -> bool override {
    if (on_put) {
      on_put();
    }
    put_keys.push_back(std::string{asset.key.Value()});
    if (!put_failure.empty()) {
      *error = put_failure;
      return false;
    }
    stored.emplace(std::string{asset.key.Value()}, asset);
    return true;
  }
  auto ReadMaskAsset(const MaskAssetKey& key, MaskAsset* asset, std::string* error)
      -> bool override {
    const auto found = stored.find(std::string{key.Value()});
    if (found == stored.end()) {
      *error = "Mask asset " + std::string{key.Value()} + " is missing from the project";
      return false;
    }
    *asset = found->second;
    return true;
  }

  std::map<std::string, MaskAsset> stored;
  std::vector<std::string>         put_keys;
  std::string                      put_failure;
  /// Runs at the start of each put, before the store changes.
  std::function<void()>            on_put;
};

/// One session result: an R8 asset and the AI source that refers to it.
struct AiMaskCommitPayload {
  MaskAsset                asset;
  AiSegmentationMaskSource source;
};

/// A 4 x 3 asset at the analysis pixels (16, 12) whose samples start at @p first_value.
auto MakeAiMaskCommitPayload(std::uint8_t first_value) -> AiMaskCommitPayload {
  AiMaskCommitPayload payload;
  payload.asset.descriptor.extent           = {4, 3};
  payload.asset.descriptor.reference_bounds = NormalizedRect{0.25f, 0.25f, 0.0625f, 0.0625f};
  for (std::uint8_t i = 0; i < 12; ++i) {
    payload.asset.pixels.push_back(static_cast<std::uint8_t>(first_value + i));
  }
  payload.asset.key = MakeMaskAssetKey(payload.asset.descriptor, payload.asset.pixels);
  payload.source =
      grade_mask_test::MakeAiSegmentationSource(std::string{payload.asset.key.Value()});
  payload.source.descriptor      = payload.asset.descriptor;
  payload.source.analysis_extent = kAnalysisExtent;
  return payload;
}

auto SourceJson(const AiSegmentationMaskSource& source) -> nlohmann::json {
  return MaskModelToJson(grade_mask_test::MakeAiSegmentationMask(MaskId{"mask.ai.1"}, source))
      .at("source");
}

class EditorAiMaskEditServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    journal_path_ = AiMaskEditTestPath("ai_mask_edit_service");
    auto document = test::WorkingSpaceBoundDefaultDocument();
    (void)grade_mask_test::AddRadialMask(document, MaskId{"mask.radial.1"});
    lease_ = test::MakeInMemoryEditorLease(kElementId, std::move(document));
    other_lease_ =
        test::MakeInMemoryEditorLease(kOtherElementId, test::WorkingSpaceBoundDefaultDocument());
    pipeline_ = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{
        {}, [this](sl_element_id_t element_id) {
          return test::CopyEditorLease(element_id == kElementId ? lease_ : other_lease_);
        }});
    history_ = std::make_shared<EditorSessionHistoryPort>();
    history_->SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_->SetPipelinePort(pipeline_);
    scheduler_   = std::make_shared<RecordingScheduler>();
    mask_assets_ = std::make_shared<RecordingMaskAssetPort>();
    runtime_     = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, std::make_shared<test::FakeEditorTaskPort>(), scheduler_,
        std::make_shared<test::FakeEditorCheckpointStore>(),
        std::make_shared<test::FakeEditorThumbnailPort>(), nullptr, nullptr, nullptr, mask_assets_);
    service_ = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
    Drain();
  }

  void TearDown() override {
    runtime_.reset();
    std::error_code ec;
    std::filesystem::permissions(journal_path_, kAllWrite, std::filesystem::perm_options::add, ec);
    std::filesystem::remove(journal_path_, ec);
  }

  void Drain() { service_->DrainCommandQueueForTests(); }

  /// Completes every frame the coordinator schedules until no new frame follows.
  void CompleteFrames() {
    Drain();
    std::size_t completed = 0;
    while (completed < scheduler_->scheduled.size()) {
      const auto request_id = scheduler_->scheduled[completed].request_id;
      ++completed;
      runtime_->coordinator->NotifySchedulerCompleted(request_id, true);
      Drain();
    }
  }

  void OpenInteractive() {
    (void)service_->Open(kElementId, kImageId);
    CompleteFrames();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  }

  auto OpenAiMaskEdit(std::optional<MaskId> mask_id) -> EditorAiMaskEditOpenResult {
    std::optional<EditorAiMaskEditOpenResult> received;
    (void)service_->OpenAiMaskEdit(GradeId(), std::move(mask_id),
                                   [&received](EditorAiMaskEditOpenResult result) {
                                     EXPECT_FALSE(received.has_value()) << "completion ran twice";
                                     received = std::move(result);
                                   });
    Drain();
    EXPECT_TRUE(received.has_value()) << "completion did not run";
    return received.value_or(EditorAiMaskEditOpenResult{});
  }

  auto Commit(std::optional<MaskId> mask_id, const AiMaskCommitPayload& payload)
      -> EditorSessionResult {
    auto result = service_->CommitAiMaskEdit(
        EditorAiMaskCommit{GradeId(), std::move(mask_id), payload.asset, payload.source});
    CompleteFrames();
    return result;
  }

  /// Opens a new AI Mask edit and commits @p payload; returns the id of the new Mask.
  auto CommitNewAiMask(const AiMaskCommitPayload& payload) -> MaskId {
    EXPECT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
    EXPECT_NE(Commit(std::nullopt, payload).kind, EditorSessionResultKind::Failed);
    return MaskId{"mask.ai.1"};
  }

  auto GradeId() -> NodeId { return Working()->Document().PrimaryGrade()->Id(); }
  auto Graph() -> std::shared_ptr<const CommitGraph> {
    return test::EditorHistoryGraph(*history_, kElementId);
  }
  auto Working() -> std::shared_ptr<const PipelineGraphSnapshot> {
    return test::EditorWorkingPreview(*pipeline_, kElementId);
  }
  auto WorkingMask(const MaskId& mask_id) -> std::optional<MaskModel> {
    const auto  working = Working();
    const auto* mask    = working->Document().PrimaryGrade()->FindMask(mask_id);
    return mask == nullptr ? std::nullopt : std::optional<MaskModel>{*mask};
  }
  auto WorkingAiSource(const MaskId& mask_id) -> AiSegmentationMaskSource {
    const auto mask = WorkingMask(mask_id);
    EXPECT_TRUE(mask.has_value());
    if (!mask.has_value() || !std::holds_alternative<AiSegmentationMaskSource>(mask->source)) {
      ADD_FAILURE() << "the working Mask is not an AI Mask";
      return {};
    }
    return std::get<AiSegmentationMaskSource>(mask->source);
  }
  /// Typed batch of the working head commit.
  auto HeadBatch() -> PipelineEditBatch {
    const auto graph = Graph();
    const auto head  = graph->GetActiveVersionRef().head_commit_hash;
    EXPECT_TRUE(head.has_value());
    return PipelineEditBatch::FromJSON(graph->GetCommit(*head).GetPayloadJSON());
  }
  auto Decision(EditorAction action) const -> EditorActionDecision {
    return service_->action_availability().For(action);
  }

  std::filesystem::path                      journal_path_;
  EditorHistoryLease                         lease_;
  EditorHistoryLease                         other_lease_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  std::shared_ptr<EditorSessionHistoryPort>  history_;
  std::shared_ptr<RecordingScheduler>        scheduler_;
  std::shared_ptr<RecordingMaskAssetPort>    mask_assets_;
  std::unique_ptr<EditorSessionRuntime>      runtime_;
  EditorSessionService*                      service_ = nullptr;
};

TEST_F(EditorAiMaskEditServiceTest, CommitNewAiMaskStoresAssetThenPublishesAddMask) {
  OpenInteractive();
  const auto opened = OpenAiMaskEdit(std::nullopt);
  ASSERT_EQ(opened.result.kind, EditorSessionResultKind::Accepted) << opened.result.message;
  EXPECT_EQ(opened.geometry_fingerprint,
            MakeAiMaskGeometryFingerprint(Working()->Document().Develop()->Params()));
  EXPECT_FALSE(opened.stored_source.has_value());
  EXPECT_FALSE(opened.stored_asset.has_value());
  EXPECT_TRUE(service_->ai_mask_edit_open());
  EXPECT_FALSE(Decision(EditorAction::CommitAdjustment).allowed);

  const auto               commits_before = Graph()->CommitCount();
  std::vector<std::size_t> commits_at_put;
  mask_assets_->on_put = [&] { commits_at_put.push_back(Graph()->CommitCount()); };
  const auto payload   = MakeAiMaskCommitPayload(10);
  const auto committed = Commit(std::nullopt, payload);
  EXPECT_NE(committed.kind, EditorSessionResultKind::Failed) << committed.message;
  EXPECT_NE(committed.kind, EditorSessionResultKind::Rejected) << committed.message;

  // The asset was stored before the history had the commit.
  ASSERT_EQ(mask_assets_->put_keys,
            std::vector<std::string>{std::string{payload.asset.key.Value()}});
  ASSERT_EQ(commits_at_put, std::vector<std::size_t>{commits_before});
  ASSERT_EQ(Graph()->CommitCount(), commits_before + 1);
  const auto batch = HeadBatch();
  ASSERT_EQ(batch.changes.size(), 1u);
  const auto* added = std::get_if<AddMaskChange>(&batch.changes.front());
  ASSERT_NE(added, nullptr);
  EXPECT_EQ(added->node_id, GradeId());
  EXPECT_EQ(added->mask_id, MaskId{"mask.ai.1"});
  EXPECT_EQ(added->mask.at("display_name"), "AI Mask 1");
  EXPECT_EQ(added->mask.at("source"), SourceJson(payload.source));
  EXPECT_EQ(WorkingAiSource(MaskId{"mask.ai.1"}), payload.source);

  // The lease is released.
  EXPECT_FALSE(service_->ai_mask_edit_open());
  EXPECT_TRUE(Decision(EditorAction::CommitAdjustment).allowed);
  EXPECT_TRUE(Decision(EditorAction::Undo).allowed);
}

TEST_F(EditorAiMaskEditServiceTest, CommitOpenedAiMaskPublishesReplaceMaskSource) {
  OpenInteractive();
  const auto first   = MakeAiMaskCommitPayload(10);
  const auto mask_id = CommitNewAiMask(first);

  const auto opened  = OpenAiMaskEdit(mask_id);
  ASSERT_EQ(opened.result.kind, EditorSessionResultKind::Accepted) << opened.result.message;
  ASSERT_TRUE(opened.stored_source.has_value());
  EXPECT_EQ(*opened.stored_source, first.source);
  ASSERT_TRUE(opened.stored_asset.has_value());
  EXPECT_EQ(opened.stored_asset->key, first.asset.key);
  EXPECT_EQ(opened.stored_asset->pixels, first.asset.pixels);

  const auto commits_before = Graph()->CommitCount();
  const auto second         = MakeAiMaskCommitPayload(40);
  (void)Commit(mask_id, second);
  ASSERT_EQ(Graph()->CommitCount(), commits_before + 1);
  const auto batch = HeadBatch();
  ASSERT_EQ(batch.changes.size(), 1u);
  const auto* replaced = std::get_if<ReplaceMaskSourceChange>(&batch.changes.front());
  ASSERT_NE(replaced, nullptr);
  EXPECT_EQ(replaced->mask_id, mask_id);
  EXPECT_EQ(replaced->before_source, SourceJson(first.source));
  EXPECT_EQ(replaced->after_source, SourceJson(second.source));
  EXPECT_EQ(WorkingAiSource(mask_id), second.source);
  EXPECT_FALSE(service_->ai_mask_edit_open());

  // An equal source publishes nothing and still ends the session.
  ASSERT_EQ(OpenAiMaskEdit(mask_id).result.kind, EditorSessionResultKind::Accepted);
  const auto unchanged = Commit(mask_id, second);
  EXPECT_EQ(unchanged.kind, EditorSessionResultKind::Accepted);
  EXPECT_EQ(unchanged.message, "The AI Mask is unchanged");
  EXPECT_EQ(Graph()->CommitCount(), commits_before + 1);
  EXPECT_FALSE(service_->ai_mask_edit_open());
  EXPECT_TRUE(Decision(EditorAction::CommitAdjustment).allowed);
}

TEST_F(EditorAiMaskEditServiceTest, MainUndoRestoresThePreviousAiSource) {
  OpenInteractive();
  const auto first   = MakeAiMaskCommitPayload(10);
  const auto second  = MakeAiMaskCommitPayload(40);
  const auto mask_id = CommitNewAiMask(first);
  ASSERT_EQ(OpenAiMaskEdit(mask_id).result.kind, EditorSessionResultKind::Accepted);
  (void)Commit(mask_id, second);
  ASSERT_EQ(WorkingAiSource(mask_id).asset_key, second.asset.key);

  (void)service_->Undo();
  CompleteFrames();
  EXPECT_EQ(WorkingAiSource(mask_id), first.source);
  EXPECT_EQ(SourceJson(WorkingAiSource(mask_id)), SourceJson(first.source));

  (void)service_->Redo();
  CompleteFrames();
  EXPECT_EQ(WorkingAiSource(mask_id), second.source);

  // Undo of the AddMask removes the Mask.
  (void)service_->Undo();
  (void)service_->Undo();
  CompleteFrames();
  EXPECT_FALSE(WorkingMask(mask_id).has_value());
}

TEST_F(EditorAiMaskEditServiceTest, AssetPutFailureKeepsDocumentAndLease) {
  OpenInteractive();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  const auto commits_before = Graph()->CommitCount();
  const auto masks_before   = Working()->Document().PrimaryGrade()->MaskCount();
  mask_assets_->put_failure = "The project database is read-only";

  const auto failed         = Commit(std::nullopt, MakeAiMaskCommitPayload(10));
  EXPECT_EQ(failed.kind, EditorSessionResultKind::Failed);
  EXPECT_EQ(failed.message, "The project database is read-only");
  EXPECT_EQ(Graph()->CommitCount(), commits_before);
  EXPECT_EQ(Working()->Document().PrimaryGrade()->MaskCount(), masks_before);
  EXPECT_TRUE(service_->ai_mask_edit_open());
  EXPECT_FALSE(Decision(EditorAction::CommitAdjustment).allowed);

  // The user can commit again after the store recovers.
  mask_assets_->put_failure.clear();
  const auto payload = MakeAiMaskCommitPayload(10);
  EXPECT_NE(Commit(std::nullopt, payload).kind, EditorSessionResultKind::Failed);
  EXPECT_EQ(Graph()->CommitCount(), commits_before + 1);
  EXPECT_FALSE(service_->ai_mask_edit_open());
}

TEST_F(EditorAiMaskEditServiceTest, PublishFailureAfterPutKeepsDocument) {
  OpenInteractive();
  const auto first   = MakeAiMaskCommitPayload(10);
  const auto mask_id = CommitNewAiMask(first);
  ASSERT_TRUE(std::filesystem::exists(journal_path_));
  ASSERT_EQ(OpenAiMaskEdit(mask_id).result.kind, EditorSessionResultKind::Accepted);
  const auto commits_before = Graph()->CommitCount();

  // The journal refuses the append, so the history publish fails after the put.
  std::filesystem::permissions(journal_path_, kAllWrite, std::filesystem::perm_options::remove);
  const auto second = MakeAiMaskCommitPayload(40);
  const auto failed = Commit(mask_id, second);
  std::filesystem::permissions(journal_path_, kAllWrite, std::filesystem::perm_options::add);
  EXPECT_EQ(failed.kind, EditorSessionResultKind::Failed);
  EXPECT_FALSE(failed.message.empty());
  EXPECT_EQ(mask_assets_->put_keys.back(), std::string{second.asset.key.Value()});
  EXPECT_EQ(Graph()->CommitCount(), commits_before);
  EXPECT_EQ(WorkingAiSource(mask_id), first.source);
  EXPECT_TRUE(service_->ai_mask_edit_open());
  EXPECT_FALSE(Decision(EditorAction::Undo).allowed);

  (void)service_->CancelAiMaskEdit();
  Drain();
  EXPECT_FALSE(service_->ai_mask_edit_open());
}

TEST_F(EditorAiMaskEditServiceTest, CancelPublishesNothing) {
  OpenInteractive();
  const auto commits_before = Graph()->CommitCount();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  const auto cancelled = service_->CancelAiMaskEdit();
  Drain();
  EXPECT_EQ(cancelled.kind, EditorSessionResultKind::Accepted);
  EXPECT_EQ(cancelled.message, "AI Mask edit cancelled");
  EXPECT_TRUE(mask_assets_->put_keys.empty());
  EXPECT_EQ(Graph()->CommitCount(), commits_before);
  EXPECT_FALSE(service_->ai_mask_edit_open());
  EXPECT_TRUE(Decision(EditorAction::CommitAdjustment).allowed);

  // A Commit after Cancel has no session to commit.
  const auto late = Commit(std::nullopt, MakeAiMaskCommitPayload(10));
  EXPECT_EQ(late.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(late.message, "No AI Mask edit is open");
  EXPECT_TRUE(mask_assets_->put_keys.empty());
}

TEST_F(EditorAiMaskEditServiceTest, OpenRejectsMissingOrNonAiMask) {
  OpenInteractive();
  const auto radial = OpenAiMaskEdit(MaskId{"mask.radial.1"});
  EXPECT_EQ(radial.result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(radial.result.message, "The Mask is not an AI Mask");
  const auto missing = OpenAiMaskEdit(MaskId{"mask.ai.7"});
  EXPECT_EQ(missing.result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(missing.result.message, "The Mask is missing");
  EXPECT_FALSE(missing.stored_source.has_value());
  EXPECT_FALSE(service_->ai_mask_edit_open());
  EXPECT_TRUE(Decision(EditorAction::CommitAdjustment).allowed);

  // A second session is refused while one is open.
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  const auto second = OpenAiMaskEdit(std::nullopt);
  EXPECT_EQ(second.result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(second.result.message, "An AI Mask edit is already open");
}

TEST_F(EditorAiMaskEditServiceTest, OpenAiMaskEditRefusesQueuedEditsAndImageChangeEndsIt) {
  OpenInteractive();
  ASSERT_EQ(OpenAiMaskEdit(std::nullopt).result.kind, EditorSessionResultKind::Accepted);
  EXPECT_TRUE(Decision(EditorAction::RequestViewChange).allowed);
  EXPECT_TRUE(Decision(EditorAction::SelectImage).allowed);
  EXPECT_EQ(service_->Undo().kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_
                ->EnqueueAdjustmentInput(test::WithColorGradeTarget(
                    test::PatchFromJson("exposure", R"({"exposure":3.0})", false)))
                .kind,
            EditorSessionResultKind::Rejected);
  EditorMaskCreationCommand mask_command;
  mask_command.kind = EditorMaskCreationCommandKind::BeginCreation;
  EXPECT_EQ(service_->EnqueueMaskCreation(mask_command).kind, EditorSessionResultKind::Rejected);
  Drain();
  EXPECT_TRUE(service_->PeekPendingInput().sequences.empty());

  // Image selection ends the session first; its Commit then has no session.
  const auto grade_id = GradeId();
  (void)service_->Switch(kOtherElementId, kOtherImageId);
  CompleteFrames();
  EXPECT_EQ(service_->identity().element_id, kOtherElementId);
  EXPECT_FALSE(service_->ai_mask_edit_open());
  EXPECT_TRUE(Decision(EditorAction::CommitAdjustment).allowed);
  const auto payload = MakeAiMaskCommitPayload(10);
  const auto late    = service_->CommitAiMaskEdit(
      EditorAiMaskCommit{grade_id, std::nullopt, payload.asset, payload.source});
  Drain();
  EXPECT_EQ(late.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(late.message, "No AI Mask edit is open");
  EXPECT_TRUE(mask_assets_->put_keys.empty());
}

}  // namespace
}  // namespace alcedo::ui
