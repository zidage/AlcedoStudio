//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/graph_image_cache.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "edit/runtime/result_representation.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/runtime_revision.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"

namespace alcedo {
namespace {

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

struct HostTextureBackend {
  struct Texture2D {
    std::uint32_t          width       = 0;
    std::uint32_t          height      = 0;
    TextureFormat          format      = TextureFormat::R8;
    std::uint64_t          resource_id = 0;
    std::vector<std::byte> storage;

    Texture2D()                                                      = default;
    Texture2D(const Texture2D&)                                      = delete;
    auto operator=(const Texture2D&) -> Texture2D&                   = delete;
    Texture2D(Texture2D&&) noexcept                                  = default;
    auto               operator=(Texture2D&&) noexcept -> Texture2D& = default;

    [[nodiscard]] auto Width() const -> std::uint32_t { return width; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return height; }
    [[nodiscard]] auto Format() const -> TextureFormat { return format; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return storage.size(); }
  };

  auto CreateTexture2D(std::uint32_t width, std::uint32_t height, TextureFormat format)
      -> Texture2D {
    if (fail_next_create) {
      fail_next_create = false;
      throw std::runtime_error("HostTextureBackend: injected allocation failure");
    }
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

  bool               fail_next_create = false;
  std::uint64_t      next_id_         = 1;
};

struct HostRetentionHarness {
  HostTextureBackend                  backend{};
  TexturePool<HostTextureBackend>     pool{backend};
  GraphImageCache<HostTextureBackend> cache{};
  PipelineDocument                    document = CreateDefaultPipelineDocument();
  PreparedRawInput prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                            gpu_dag_test::FullSensor(16, 12));
  ExecutionPlan    plan{};
  RuntimeInvalidationState invalidation{};

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
    auto&      lease    = cache.AcquireTextureForWrite(pool, id, request);
    (void)lease;
    cache.RecordUnpublished(
        id, required, MakeExtentRepresentation({request.width, request.height}, request.format), 1);
    cache.PublishSuccessfulSubmission(1, ResultPersistenceScope::AllCurrentResults, Sensor());
  }
};

TEST(GraphImageCacheRetention, FreeMatchingAllocationPreservesAllValidPipelineResults) {
  HostRetentionHarness     harness;
  constexpr TextureRequest kRgba{8, 8, TextureFormat::Rgba32f};
  harness.Publish(harness.Sensor(), kRgba);
  harness.Publish(harness.Geometry(), kRgba);
  harness.Publish(harness.Grade(), kRgba);
  ASSERT_EQ(harness.cache.PublishedCount(), 3U);
  const auto develop_handle = harness.cache.Find(harness.Sensor())->Handle();
  const auto develop_rev    = harness.cache.PublishedRevision(harness.Sensor());

  auto       extra          = harness.pool.Acquire(kRgba);
  extra.Release();
  ASSERT_TRUE(harness.pool.HasReusable(kRgba));
  const auto         entries_before = harness.pool.EntryCount();

  const GraphValueId display        = harness.plan.display_output;
  (void)harness.cache.AcquireTextureForWrite(harness.pool, display, kRgba);
  EXPECT_EQ(harness.cache.PublishedCount(), 3U);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Handle(), develop_handle);
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Sensor()), develop_rev);
  EXPECT_EQ(harness.pool.EntryCount(), entries_before);
  EXPECT_TRUE(harness.cache.FindValidResult(
      harness.Sensor(), develop_rev, MakeExtentRepresentation({8, 8}, TextureFormat::Rgba32f), 1));
}

TEST(GraphImageCacheRetention, InvalidDownstreamDoesNotDropValidDevelopOrSharedGeometry) {
  HostRetentionHarness harness;
  const TextureRequest large{16, 16, TextureFormat::Rgba32f};
  const TextureRequest small{8, 8, TextureFormat::Rgba32f};
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
  (void)harness.cache.AcquireTextureForWrite(harness.pool, display, small);
  ASSERT_NE(harness.cache.Find(harness.Sensor()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Handle(), develop_handle);
  ASSERT_NE(harness.cache.Find(harness.Geometry()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Geometry())->Handle(), geometry_handle);
  ASSERT_NE(harness.cache.Find(harness.Grade()), nullptr);
}

TEST(GraphImageCacheRetention, HeldUnpublishedWriteAllowsNextStage) {
  HostRetentionHarness harness;
  const TextureRequest stage{16, 16, TextureFormat::Rgba32f};
  (void)harness.cache.AcquireTextureForWrite(harness.pool, harness.Sensor(), stage);
  ASSERT_NO_THROW(
      (void)harness.cache.AcquireTextureForWrite(harness.pool, harness.Geometry(), stage));
  ASSERT_NE(harness.cache.Find(harness.Sensor()), nullptr);
  ASSERT_NE(harness.cache.Find(harness.Geometry()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Texture().Width(), 16U);
  EXPECT_EQ(harness.cache.Find(harness.Geometry())->Texture().Width(), 16U);
  EXPECT_EQ(harness.cache.PublishedCount(), 0U);
}

TEST(GraphImageCacheRetention, LiveWriteDoesNotEvictValidDevelop) {
  HostRetentionHarness harness;
  const TextureRequest develop_req{16, 16, TextureFormat::Rgba32f};
  const TextureRequest other_req{8, 8, TextureFormat::Rgba32f};
  harness.Publish(harness.Sensor(), develop_req);
  const auto develop_handle = harness.cache.Find(harness.Sensor())->Handle();
  const auto develop_rev    = harness.cache.PublishedRevision(harness.Sensor());
  ASSERT_NO_THROW(
      (void)harness.cache.AcquireTextureForWrite(harness.pool, harness.Grade(), other_req));
  EXPECT_EQ(harness.cache.PublishedCount(), 1U);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Handle(), develop_handle);
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Sensor()), develop_rev);
}

TEST(GraphImageCacheRetention, RequiredLiveResourcesReportAllocationFailureWithoutEvictingDevelop) {
  HostRetentionHarness harness;
  const TextureRequest develop_req{16, 16, TextureFormat::Rgba32f};
  const TextureRequest other_req{8, 8, TextureFormat::Rgba32f};
  harness.Publish(harness.Sensor(), develop_req);
  const auto develop_handle        = harness.cache.Find(harness.Sensor())->Handle();
  const auto develop_rev           = harness.cache.PublishedRevision(harness.Sensor());
  harness.backend.fail_next_create = true;
  try {
    (void)harness.cache.AcquireTextureForWrite(harness.pool, harness.Grade(), other_req);
    FAIL() << "expected injected allocation failure";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "HostTextureBackend: injected allocation failure");
  }
  EXPECT_EQ(harness.cache.PublishedCount(), 1U);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Handle(), develop_handle);
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Sensor()), develop_rev);
}

TEST(GraphImageCacheRetention, QualityBaseWriteDoesNotReplaceValidInteractiveGeometry) {
  HostRetentionHarness harness;
  const TextureRequest interactive{8, 8, TextureFormat::Rgba32f};
  const TextureRequest quality{16, 16, TextureFormat::Rgba32f};
  harness.Publish(harness.Sensor(), interactive);
  harness.Publish(harness.Geometry(), interactive);
  const auto geometry_handle = harness.cache.Find(harness.Geometry())->Handle();
  const auto geometry_rev    = harness.cache.PublishedRevision(harness.Geometry());
  (void)harness.cache.AcquireTextureForWrite(harness.pool, harness.Geometry(), quality);
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Geometry()), geometry_rev);
  harness.cache.RecordUnpublished(harness.Geometry(),
                                  harness.invalidation.RequiredRevision(harness.Geometry()),
                                  MakeExtentRepresentation({16, 16}, TextureFormat::Rgba32f), 1);
  harness.cache.PublishSuccessfulSubmission(1, ResultPersistenceScope::SensorDevelopOnly,
                                            harness.Sensor());
  EXPECT_EQ(harness.cache.PublishedRevision(harness.Geometry()), geometry_rev);
  harness.cache.DiscardUnpublished();
  ASSERT_NE(harness.cache.Find(harness.Geometry()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Geometry())->Handle(), geometry_handle);
  EXPECT_EQ(harness.cache.Find(harness.Geometry())->Texture().Width(), 8U);
}

TEST(GraphImageCacheRetention, DropStalePublishedKeepsCurrentDevelopAndFreesIdleTextures) {
  HostRetentionHarness harness;
  const TextureRequest rgba{8, 8, TextureFormat::Rgba32f};
  harness.Publish(harness.Sensor(), rgba);
  harness.Publish(harness.Geometry(), rgba);
  harness.Publish(harness.Grade(), rgba);
  const auto develop_handle = harness.cache.Find(harness.Sensor())->Handle();

  auto* exposure = dynamic_cast<ExposureModel*>(
      harness.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.5f);
  harness.invalidation.CollectAndPropagate(harness.plan, harness.document, harness.prepared, {});

  harness.cache.DropStalePublished([&](const GraphValueId& id, RuntimeRevision revision) {
    return harness.invalidation.HasCurrentRevision(id, revision);
  });
  harness.pool.ReleaseUnleased();

  ASSERT_NE(harness.cache.Find(harness.Sensor()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Sensor())->Handle(), develop_handle);
  EXPECT_NE(harness.cache.Find(harness.Geometry()), nullptr);
  EXPECT_EQ(harness.cache.Find(harness.Grade()), nullptr);
  EXPECT_FALSE(harness.pool.HasReusable(rgba));
}

TEST(GraphImageCacheRetention, ExtraLeaseKeepsDisplayedTextureAfterStalePublishDrop) {
  HostRetentionHarness harness;
  const TextureRequest rgba{8, 8, TextureFormat::Rgba32f};
  harness.Publish(harness.Sensor(), rgba);
  const auto handle        = harness.cache.Find(harness.Sensor())->Handle();
  auto       display_lease = harness.pool.DuplicateLease(handle);

  auto payload            = harness.document.Develop()->Params().Params();
  payload.demosaic_method = payload.demosaic_method == "legacy" ? "neural_engine" : "legacy";
  harness.document.Develop()->Params().ReplaceParams(std::move(payload));
  harness.invalidation.CollectAndPropagate(harness.plan, harness.document, harness.prepared, {});

  harness.cache.DropStalePublished([&](const GraphValueId& id, RuntimeRevision revision) {
    return harness.invalidation.HasCurrentRevision(id, revision);
  });
  harness.pool.ReleaseUnleased();
  EXPECT_TRUE(harness.pool.Contains(handle));
  EXPECT_EQ(harness.cache.Find(harness.Sensor()), nullptr);

  display_lease.Release();
  harness.pool.ReleaseUnleased();
  EXPECT_FALSE(harness.pool.Contains(handle));
}

}  // namespace
}  // namespace alcedo
