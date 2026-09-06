//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda_workspace_test_support.hpp"

#include <stdexcept>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "edit/runtime/result_representation.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/runtime_revision.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

using cuda_workspace_test::CudaWorkspaceFixture;

constexpr std::uint32_t kWidth  = 8;
constexpr std::uint32_t kHeight = 8;

void ConsumeOperatorDirty(PipelineDocument& document) {
  if (auto* develop = document.Develop()) {
    (void)develop->Params().TakeDirtyPatch();
  }
  if (auto* drt = document.Drt()) {
    (void)drt->Params().TakeDirtyPatch();
    for (std::size_t index = 0; index < drt->AdjustmentCount(); ++index) {
      (void)drt->AdjustmentAt(index).TakeDirtyPatch();
    }
  }
  for (const auto& node : document.Graph().Nodes()) {
    auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(node->Id()));
    if (grade == nullptr) {
      continue;
    }
    for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
      (void)grade->AdjustmentAt(index).TakeDirtyPatch();
    }
  }
}

auto Value() -> GraphValueId { return {NodeId{"develop"}, PortId{"sensor_linear"}}; }

auto ImageRepr(TextureFormat format = TextureFormat::Rgba32f) -> ResultRepresentation {
  return MakeExtentRepresentation({kWidth, kHeight}, format);
}

void PublishWrite(CudaRenderDevice& device, const GraphValueId& id, RuntimeRevision revision) {
  auto& workspace = device.Workspace();
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, revision, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
}

TEST_F(CudaWorkspaceFixture, ResultCacheDoesNotTreatReusedTextureAllocationAsContentHit) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  workspace.Textures().SetByteBudget(static_cast<std::size_t>(kWidth) * kHeight * 16);
  const GraphValueId    id         = Value();
  constexpr RuntimeRevision first  = 11;
  constexpr RuntimeRevision second = 22;
  PublishWrite(device, id, first);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(), completed));
  const auto resource_a = workspace.Images().Find(id)->Texture().ResourceId();

  device.BeginRender();
  auto& write = workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  EXPECT_EQ(write.Texture().ResourceId(), resource_a);
  EXPECT_FALSE(workspace.Images().FindValidResult(id, first, ImageRepr(), completed));
  workspace.Images().RecordUnpublished(id, second, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
  const auto done = workspace.Device().CompletedSubmission();
  EXPECT_FALSE(workspace.Images().FindValidResult(id, first, ImageRepr(), done));
  EXPECT_TRUE(workspace.Images().FindValidResult(id, second, ImageRepr(), done));
  EXPECT_EQ(workspace.Images().PublishedCount(), 1U);
}

TEST_F(CudaWorkspaceFixture, FailedSubmissionDoesNotPublishResultRevision) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId    id       = Value();
  constexpr RuntimeRevision revision = 31;
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, revision, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_FALSE(workspace.Images().FindValidResult(id, revision, ImageRepr(),
                                                  workspace.Device().CompletedSubmission()));
  EXPECT_EQ(workspace.Images().PublishedCount(), 0U);
}

TEST_F(CudaWorkspaceFixture, CancelledSubmissionKeepsPreviouslyCompletedCacheEntriesUsable) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId    id     = Value();
  constexpr RuntimeRevision first  = 41;
  constexpr RuntimeRevision second = 42;
  PublishWrite(device, id, first);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(), completed));

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, second, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(),
                                                 workspace.Device().CompletedSubmission()));
  EXPECT_FALSE(workspace.Images().FindValidResult(id, second, ImageRepr(),
                                                  workspace.Device().CompletedSubmission()));
}

TEST_F(CudaWorkspaceFixture, AliasTextureFromSharesOneAllocationAcrossTwoValueIds) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId sensor{NodeId{"develop"}, PortId{"sensor_linear"}};
  const GraphValueId geometry{NodeId{"geometry"}, PortId{"scene_source"}};
  constexpr RuntimeRevision sensor_rev   = 61;
  constexpr RuntimeRevision geometry_rev = 62;

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(sensor, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(sensor, sensor_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  const auto used_before    = workspace.Textures().UsedBytes();
  const auto entries_before = workspace.Textures().EntryCount();
  const auto sensor_id      = workspace.Images().Find(sensor)->Texture().ResourceId();
  (void)workspace.AliasImageFrom(geometry, sensor);
  workspace.Images().RecordUnpublished(geometry, geometry_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  EXPECT_EQ(workspace.Images().Find(geometry)->Texture().ResourceId(), sensor_id);
  EXPECT_EQ(workspace.Textures().UsedBytes(), used_before);
  EXPECT_EQ(workspace.Textures().EntryCount(), entries_before);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();

  const auto completed = workspace.Device().CompletedSubmission();
  EXPECT_TRUE(workspace.Images().FindValidResult(sensor, sensor_rev, ImageRepr(), completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(geometry, geometry_rev, ImageRepr(), completed));
  EXPECT_EQ(workspace.Images().Find(sensor)->Texture().ResourceId(),
            workspace.Images().Find(geometry)->Texture().ResourceId());
  EXPECT_EQ(workspace.Images().PublishedCount(), 2U);
  EXPECT_EQ(workspace.Textures().EntryCount(), 1U);
}

TEST_F(CudaWorkspaceFixture, AliasTextureFromRejectsMissingSourceAndSelfAlias) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId sensor{NodeId{"develop"}, PortId{"sensor_linear"}};
  const GraphValueId geometry{NodeId{"geometry"}, PortId{"scene_source"}};
  device.BeginRender();
  EXPECT_THROW((void)workspace.AliasImageFrom(geometry, sensor), std::runtime_error);
  (void)workspace.AcquireImageForWrite(sensor, {kWidth, kHeight, TextureFormat::Rgba32f});
  EXPECT_THROW((void)workspace.AliasImageFrom(sensor, sensor), std::runtime_error);
  device.CancelRender();
}

TEST_F(CudaWorkspaceFixture, UnpublishedWriteIsNotValidUntilPublish) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId    id       = Value();
  constexpr RuntimeRevision revision = 51;
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, revision, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  EXPECT_FALSE(workspace.Images().FindValidResult(id, revision, ImageRepr(),
                                                  workspace.Device().CompletedSubmission()));
  ASSERT_NE(workspace.Images().Find(id), nullptr);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, revision, ImageRepr(),
                                                 workspace.Device().CompletedSubmission()));
}

TEST_F(CudaWorkspaceFixture, BackgroundIntermediateImagesReleaseAfterLastConsumer) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId develop{NodeId{"develop"}, PortId{"image"}};
  const GraphValueId grade{NodeId{"grade.primary"}, PortId{"image"}};
  const GraphValueId mask{NodeId{"mask"}, PortId{"mask"}};
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(develop, {kWidth, kHeight, TextureFormat::Rgba32f});
  (void)workspace.AcquireImageForWrite(grade, {kWidth, kHeight, TextureFormat::Rgba32f});
  (void)workspace.AcquireImageForWrite(mask, {kWidth, kHeight, TextureFormat::R8});
  EXPECT_EQ(workspace.Images().UnpublishedWriteCount(), 3U);
  device.WaitIdle();
  workspace.ReleaseConsumedImage(develop);
  EXPECT_EQ(workspace.Images().UnpublishedWriteCount(), 2U);
  EXPECT_EQ(workspace.Images().Find(develop), nullptr);
  ASSERT_NE(workspace.Images().Find(grade), nullptr);
  ASSERT_NE(workspace.Images().Find(mask), nullptr);
  workspace.ReleaseConsumedImage(mask);
  EXPECT_EQ(workspace.Images().UnpublishedWriteCount(), 1U);
  EXPECT_NE(workspace.Images().Find(grade), nullptr);
  device.CancelRender();
}

TEST_F(CudaWorkspaceFixture, SinkFailurePublishesNoNewResults) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId    id     = Value();
  constexpr RuntimeRevision first  = 81;
  constexpr RuntimeRevision second = 82;
  PublishWrite(device, id, first);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(), completed));

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, second, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(),
                                                 workspace.Device().CompletedSubmission()));
  EXPECT_FALSE(workspace.Images().FindValidResult(id, second, ImageRepr(),
                                                  workspace.Device().CompletedSubmission()));
  EXPECT_EQ(workspace.Images().UnpublishedCount(), 0U);
}

TEST_F(CudaWorkspaceFixture, SharedInputSurvivesBothBranchReaders) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId develop{NodeId{"develop"}, PortId{"image"}};
  const GraphValueId grade_a{NodeId{"grade.a"}, PortId{"image"}};
  const GraphValueId grade_b{NodeId{"grade.b"}, PortId{"image"}};
  constexpr RuntimeRevision develop_rev = 91;
  constexpr RuntimeRevision a_rev       = 92;
  constexpr RuntimeRevision b_rev       = 93;

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(develop, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(develop, develop_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  const auto develop_id = workspace.Images().Find(develop)->Texture().ResourceId();
  (void)workspace.AliasImageFrom(grade_a, develop);
  workspace.Images().RecordUnpublished(grade_a, a_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  (void)workspace.AliasImageFrom(grade_b, develop);
  workspace.Images().RecordUnpublished(grade_b, b_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  EXPECT_EQ(workspace.Images().Find(grade_a)->Texture().ResourceId(), develop_id);
  EXPECT_EQ(workspace.Images().Find(grade_b)->Texture().ResourceId(), develop_id);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();

  const auto completed = workspace.Device().CompletedSubmission();
  EXPECT_TRUE(workspace.Images().FindValidResult(develop, develop_rev, ImageRepr(), completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(grade_a, a_rev, ImageRepr(), completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(grade_b, b_rev, ImageRepr(), completed));
  EXPECT_EQ(workspace.Images().Find(grade_a)->Texture().ResourceId(),
            workspace.Images().Find(develop)->Texture().ResourceId());
  EXPECT_EQ(workspace.Textures().EntryCount(), 1U);
}

struct HostTextureBackend {
  struct Texture2D {
    std::uint32_t          width       = 0;
    std::uint32_t          height      = 0;
    TextureFormat          format      = TextureFormat::R8;
    std::uint64_t          resource_id = 0;
    std::vector<std::byte> storage;

    Texture2D()                                        = default;
    Texture2D(const Texture2D&)                        = delete;
    auto operator=(const Texture2D&) -> Texture2D&     = delete;
    Texture2D(Texture2D&&) noexcept                    = default;
    auto operator=(Texture2D&&) noexcept -> Texture2D& = default;

    [[nodiscard]] auto Width() const -> std::uint32_t { return width; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return height; }
    [[nodiscard]] auto Format() const -> TextureFormat { return format; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return storage.size(); }
  };

  auto CreateTexture2D(std::uint32_t width, std::uint32_t height, TextureFormat format)
      -> Texture2D {
    Texture2D texture;
    texture.width       = width;
    texture.height      = height;
    texture.format      = format;
    texture.resource_id = next_id_++;
    texture.storage.resize(static_cast<std::size_t>(width) * height *
                           TextureFormatBytesPerPixel(format));
    return texture;
  }

  [[nodiscard]] auto IsResourceBusy(std::uint64_t) const -> bool { return false; }

  std::uint64_t next_id_ = 1;
};

struct HostRetentionHarness {
  HostTextureBackend                  backend{};
  TexturePool<HostTextureBackend>     pool{backend};
  GraphImageCache<HostTextureBackend> cache{};
  PipelineDocument                    document = CreateDefaultPipelineDocument();
  PreparedRawInput                    prepared = RawInputLoader::FromDirectRgb(
      gpu_dag_test::MakeF32RgbaPlane(16, 12), gpu_dag_test::FullSensor(16, 12));
  ExecutionPlan                       plan{};
  RuntimeInvalidationState            invalidation{};

  HostRetentionHarness() {
    gpu_dag_test::EnsureTestCameraProfile(document);
    plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
    invalidation.CollectAndPropagate(plan, document, prepared, {});
    ConsumeOperatorDirty(document);
  }

  auto Sensor() const -> GraphValueId { return plan.sensor_linear_output; }
  auto Geometry() const -> GraphValueId { return plan.geometry_output; }
  auto Grade() const -> GraphValueId {
    const auto* grade = plan.FirstGrade();
    if (grade == nullptr) {
      throw std::runtime_error("HostRetentionHarness: compiled plan has no Color Grade");
    }
    return grade->scene_output;
  }

  void Publish(const GraphValueId& id, const TextureRequest& request) {
    const auto required = invalidation.RequiredRevision(id);
    auto&      lease    = cache.AcquireTextureForWrite(
        pool, backend, id, request, invalidation, ResultPersistenceScope::AllCurrentResults,
        Sensor());
    (void)lease;
    cache.RecordUnpublished(
        id, required, MakeExtentRepresentation({request.width, request.height}, request.format), 1);
    cache.PublishSuccessfulSubmission(1, ResultPersistenceScope::AllCurrentResults, Sensor());
  }
};

TEST(GraphImageCacheRetention, FreeMatchingAllocationPreservesAllValidPipelineResults) {
  HostRetentionHarness harness;
  constexpr TextureRequest kRgba{8, 8, TextureFormat::Rgba32f};
  const auto               bytes = static_cast<std::size_t>(8) * 8 * 16;
  harness.pool.SetByteBudget(bytes * 3);
  harness.Publish(harness.Sensor(), kRgba);
  harness.Publish(harness.Geometry(), kRgba);
  harness.Publish(harness.Grade(), kRgba);
  ASSERT_EQ(harness.cache.PublishedCount(), 3U);
  const auto develop_handle = harness.cache.Find(harness.Sensor())->Handle();
  const auto develop_rev    = harness.cache.PublishedRevision(harness.Sensor());

  auto extra = harness.pool.Acquire(kRgba);
  extra.Release();
  ASSERT_TRUE(harness.pool.HasReusable(kRgba));
  const auto entries_before = harness.pool.EntryCount();

  const GraphValueId display = harness.plan.display_output;
  (void)harness.cache.AcquireTextureForWrite(harness.pool, harness.backend, display, kRgba,
                                             harness.invalidation,
                                             ResultPersistenceScope::AllCurrentResults,
                                             harness.Sensor());
  EXPECT_EQ(harness.cache.PublishedCount(), 3U);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Handle(), develop_handle);
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Sensor()), develop_rev);
  EXPECT_EQ(harness.pool.EntryCount(), entries_before);
  EXPECT_TRUE(harness.cache.FindValidResult(
      harness.Sensor(), develop_rev, MakeExtentRepresentation({8, 8}, TextureFormat::Rgba32f), 1));
}

TEST(GraphImageCacheRetention, InvalidMixedSizeResultsReleaseOnlyRequiredUnleasedAllocations) {
  HostRetentionHarness harness;
  const TextureRequest large{16, 16, TextureFormat::Rgba32f};
  const TextureRequest small{8, 8, TextureFormat::Rgba32f};
  const auto           large_bytes = static_cast<std::size_t>(16) * 16 * 16;
  const auto           small_bytes = static_cast<std::size_t>(8) * 8 * 16;
  harness.pool.SetByteBudget(large_bytes + small_bytes * 2);
  harness.Publish(harness.Sensor(), large);
  harness.Publish(harness.Geometry(), small);
  harness.Publish(harness.Grade(), small);
  const auto develop_handle  = harness.cache.Find(harness.Sensor())->Handle();
  const auto geometry_handle = harness.cache.Find(harness.Geometry())->Handle();
  (void)harness.cache.AliasTextureFrom(harness.pool, harness.plan.develop_output,
                                       harness.Geometry());
  harness.cache.RecordUnpublished(
      harness.plan.develop_output,
      harness.invalidation.RequiredRevision(harness.plan.develop_output),
      MakeExtentRepresentation({8, 8}, TextureFormat::Rgba32f), 1);
  harness.cache.PublishSuccessfulSubmission(1, ResultPersistenceScope::AllCurrentResults,
                                            harness.Sensor());
  EXPECT_EQ(harness.cache.Find(harness.Geometry())->Handle(),
            harness.cache.Find(harness.plan.develop_output)->Handle());

  auto* exposure = dynamic_cast<ExposureModel*>(
      harness.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.5f);
  harness.invalidation.CollectAndPropagate(harness.plan, harness.document, harness.prepared, {});
  EXPECT_TRUE(harness.invalidation.HasCurrentRevision(
      harness.Sensor(), harness.cache.PublishedRevision(harness.Sensor())));
  EXPECT_FALSE(harness.invalidation.HasCurrentRevision(
      harness.Grade(), harness.cache.PublishedRevision(harness.Grade())));

  const GraphValueId display = harness.plan.display_output;
  (void)harness.cache.AcquireTextureForWrite(harness.pool, harness.backend, display, small,
                                             harness.invalidation,
                                             ResultPersistenceScope::AllCurrentResults,
                                             harness.Sensor());
  ASSERT_NE(harness.cache.Find(harness.Sensor()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Handle(), develop_handle);
  ASSERT_NE(harness.cache.Find(harness.Geometry()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Geometry())->Handle(), geometry_handle);
  EXPECT_EQ(harness.cache.Find(harness.Grade()), nullptr);
}

TEST(GraphImageCacheRetention, RequiredLiveResourcesReportAllocationFailureWithoutEvictingDevelop) {
  HostRetentionHarness harness;
  const TextureRequest develop_req{16, 16, TextureFormat::Rgba32f};
  const TextureRequest other_req{8, 8, TextureFormat::Rgba32f};
  harness.pool.SetByteBudget(static_cast<std::size_t>(16) * 16 * 16);
  harness.Publish(harness.Sensor(), develop_req);
  const auto develop_handle = harness.cache.Find(harness.Sensor())->Handle();
  const auto develop_rev    = harness.cache.PublishedRevision(harness.Sensor());
  EXPECT_THROW((void)harness.cache.AcquireTextureForWrite(
                   harness.pool, harness.backend, harness.Grade(), other_req, harness.invalidation,
                   ResultPersistenceScope::AllCurrentResults, harness.Sensor()),
               std::runtime_error);
  EXPECT_EQ(harness.cache.PublishedCount(), 1U);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Handle(), develop_handle);
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Sensor()), develop_rev);
}

TEST(GraphImageCacheRetention, QualityBaseWriteDoesNotReplaceValidInteractiveGeometry) {
  HostRetentionHarness harness;
  const TextureRequest interactive{8, 8, TextureFormat::Rgba32f};
  const TextureRequest quality{16, 16, TextureFormat::Rgba32f};
  harness.pool.SetByteBudget(static_cast<std::size_t>(16) * 16 * 16 * 4);
  harness.Publish(harness.Sensor(), interactive);
  harness.Publish(harness.Geometry(), interactive);
  const auto geometry_handle = harness.cache.Find(harness.Geometry())->Handle();
  const auto geometry_rev    = harness.cache.PublishedRevision(harness.Geometry());
  (void)harness.cache.AcquireTextureForWrite(harness.pool, harness.backend, harness.Geometry(),
                                             quality, harness.invalidation,
                                             ResultPersistenceScope::SensorDevelopOnly,
                                             harness.Sensor());
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Geometry()), geometry_rev);
  harness.cache.RecordUnpublished(
      harness.Geometry(), harness.invalidation.RequiredRevision(harness.Geometry()),
      MakeExtentRepresentation({16, 16}, TextureFormat::Rgba32f), 1);
  harness.cache.PublishSuccessfulSubmission(1, ResultPersistenceScope::SensorDevelopOnly,
                                            harness.Sensor());
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Geometry()), geometry_rev);
  harness.cache.DiscardUnpublished();
  ASSERT_NE(harness.cache.Find(harness.Geometry()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Geometry())->Handle(), geometry_handle);
  EXPECT_EQ(harness.cache.Find(harness.Geometry())->Texture().Width(), 8U);
}

}  // namespace
}  // namespace alcedo
