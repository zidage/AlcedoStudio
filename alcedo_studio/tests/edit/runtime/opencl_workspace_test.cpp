//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/render_backend.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/transient_buffer_arena.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_kernel_cache.hpp"
#include "opencl/opencl_program_library.hpp"
#include "opencl_workspace_test_support.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace alcedo {
namespace {

using opencl_workspace_test::BindSharpen;
using opencl_workspace_test::ExposureFieldBindings;
using opencl_workspace_test::HasOpenClDevice;
using opencl_workspace_test::OpenClWorkspaceFixture;
using opencl_workspace_test::UploadFullAndClearDirty;

auto FileContainsForbiddenToken(const std::filesystem::path& path,
                                std::initializer_list<const char*> tokens) -> std::string {
  std::ifstream input(path);
  std::string   line;
  int           line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    for (const char* token : tokens) {
      if (line.find(token) != std::string::npos) {
        return path.filename().string() + ":" + std::to_string(line_number) + " " + token;
      }
    }
  }
  return {};
}

auto TestExecutableDir() -> std::filesystem::path {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD copied =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < buffer.size()) {
      buffer.resize(copied);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  return std::filesystem::current_path();
#endif
}

auto UniqueSuffix() -> std::string {
  static int sequence = 0;
  return std::to_string(++sequence);
}

TEST(GpuDagOpenClWorkspace, RendererTemplateInstantiatesOpenClWithoutCudaOrMetalHeaders) {
  const char* files[] = {"renderer.hpp",      "plan_executor.hpp", "pass_encoder.hpp",
                         "basic_render_device.hpp", "frame_presenter.hpp", "render_backend.hpp",
                         "render_device_type.hpp"};
  const std::filesystem::path root{ALCEDO_RUNTIME_HEADER_ROOT};
  for (const char* name : files) {
    const auto hit = FileContainsForbiddenToken(
        root / name, {"cuda_runtime", "cuda.h", "Metal/", "metal.h", "CL/cl.h", "OpenCL/"});
    EXPECT_TRUE(hit.empty()) << hit;
  }

  const auto opencl_backend_path =
      std::filesystem::path{ALCEDO_RUNTIME_HEADER_ROOT} / "opencl" / "opencl_backend.hpp";
  std::ifstream input(opencl_backend_path);
  ASSERT_TRUE(input);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  EXPECT_EQ(text.find("cuda_runtime"), std::string::npos);
  EXPECT_EQ(text.find("cuda.h"), std::string::npos);
  EXPECT_EQ(text.find("Metal/"), std::string::npos);
  EXPECT_EQ(text.find("metal.h"), std::string::npos);

  static_assert(RenderBackend<OpenClBackend>);
  static_assert(std::is_same_v<OpenClRenderer, Renderer<OpenClBackend>>);
  static_assert(sizeof(OpenClRenderer) > 0);
  EXPECT_EQ(kOpenClDagBackendCapabilityVersion, OpenClBackend::kCapabilityVersion);

  if (!HasOpenClDevice()) {
    GTEST_SKIP() << "No OpenCL device available.";
  }
  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  OpenClRenderer renderer(document);
  EXPECT_EQ(renderer.PlanCache().BackendCapabilityVersion(), kOpenClDagBackendCapabilityVersion);
  EXPECT_EQ(renderer.SessionResources().published_result_count, 0U);
  EXPECT_EQ(renderer.OneShotPublishedResultCount(), 0U);
}

TEST_F(OpenClWorkspaceFixture, OpenClMaxSlabBytesUsesDeviceReportedMaxMemAllocSize) {
  auto& ctx = OpenClContext::Instance();
  ASSERT_TRUE(ctx.IsInitialized());
  const auto reported = static_cast<std::size_t>(ctx.Capabilities().max_single_allocation_bytes);
  ASSERT_GT(reported, 0U);

  OpenClRenderDevice device;
  const auto         slab = device.Workspace().Device().MaxSlabBytes();
  EXPECT_GT(slab, 0U);
  EXPECT_LE(slab, reported);
  if (reported >= 256) {
    EXPECT_EQ(slab % 256, 0U);
    EXPECT_GE(slab, reported - 255);
  }
}

TEST_F(OpenClWorkspaceFixture, OpenClBackendUsesThePreparedProcessContextAndProductQueue) {
  auto& ctx = OpenClContext::Instance();
  ASSERT_TRUE(ctx.IsInitialized());
  const auto device  = ctx.Device();
  const auto context = ctx.Context();
  const auto queue   = ctx.ProductQueue();
  ASSERT_NE(device, nullptr);
  ASSERT_NE(context, nullptr);
  ASSERT_NE(queue, nullptr);

  OpenClRenderDevice first;
  EXPECT_EQ(first.Workspace().Device().NativeDevice(), device);
  EXPECT_EQ(first.Workspace().Device().NativeContext(), context);
  EXPECT_EQ(first.Workspace().Device().NativeQueue(), queue);
  EXPECT_EQ(ctx.ProductQueue(), queue);

  OpenClRenderDevice second;
  EXPECT_EQ(second.Workspace().Device().NativeDevice(), device);
  EXPECT_EQ(second.Workspace().Device().NativeContext(), context);
  EXPECT_EQ(second.Workspace().Device().NativeQueue(), queue);
  EXPECT_EQ(ctx.ProductQueue(), queue);
  EXPECT_GT(first.Workspace().Device().WorkingSetBudgetBytes(), 0U);
}

TEST_F(OpenClWorkspaceFixture, OpenClParameterArenaUploadsOnlyDirtyRanges) {
  OpenClRenderDevice device;
  ParameterSlotKey   key{NodeId{"grade.primary"}, AdjustmentInstanceId{"sharpen"}};
  SharpenModel       model;
  BindSharpen(device.Workspace().Parameters(), key);
  ASSERT_TRUE(UploadFullAndClearDirty(device, key, model));

  model.SetAmount(12.0f);
  auto pending = TakePendingParameterPatch(model);
  ASSERT_TRUE(pending.has_value());

  auto& backend = device.Workspace().Device();
  backend.ResetCounters();
  device.Workspace().Parameters().ApplyPatch(key, pending->Patch());
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  device.WaitIdle();
  pending->Commit();

  ASSERT_EQ(backend.LastHostToDeviceRanges().size(), 1U);
  EXPECT_EQ(backend.LastHostToDeviceRanges().front().size, 4U);
  EXPECT_EQ(backend.HostToDeviceBytes(), 4U);
  EXPECT_LT(backend.HostToDeviceBytes(), sizeof(SharpenPayload));

  backend.ResetCounters();
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  EXPECT_EQ(backend.HostToDeviceBytes(), 0U);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
}

TEST_F(OpenClWorkspaceFixture, OpenClTransientArenaRewindsWithoutAllocatingAnotherBuffer) {
  OpenClRenderDevice device;
  auto&              transients = device.Workspace().TransientBuffers();
  transients.Reserve(4096);
  device.Workspace().Device().ResetCounters();

  void* first = transients.Allocate(512);
  ASSERT_NE(first, nullptr);
  const auto capacity = transients.capacity_bytes();
  transients.Reset();
  EXPECT_EQ(transients.used_bytes(), 0U);
  EXPECT_EQ(transients.capacity_bytes(), capacity);
  void* second = transients.Allocate(512);
  EXPECT_EQ(second, first);
  EXPECT_EQ(device.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().FreeCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().BufferCreateCount(), 0U);
}

TEST_F(OpenClWorkspaceFixture, OpenClTransientReserveAboveMaxSlabUsesSeparateDeviceBuffers) {
  OpenClRenderDevice device;
  auto&              backend    = device.Workspace().Device();
  auto&              transients = device.Workspace().TransientBuffers();
  constexpr std::size_t kSlab   = 1 << 20;
  constexpr std::size_t kChunk  = 700 << 10;
  backend.SetMaxSlabBytes(kSlab);
  backend.ResetCounters();
  transients.Reserve(3 << 20);
  EXPECT_GE(transients.capacity_bytes(), 3U << 20);
  EXPECT_GE(backend.BufferCreateCount(), 3U);

  void* first  = transients.Allocate(kChunk);
  void* second = transients.Allocate(kChunk);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  const auto a = backend.ResolveDeviceMemory(first, kChunk);
  const auto b = backend.ResolveDeviceMemory(second, kChunk);
  EXPECT_NE(a.first, b.first);

  std::vector<std::byte> ones(kChunk, std::byte{0x5A});
  std::vector<std::byte> twos(kChunk, std::byte{0xA5});
  backend.UploadDeviceMemory(first, ones, device.CommandContext());
  backend.UploadDeviceMemory(second, twos, device.CommandContext());
  device.WaitIdle();
}

TEST_F(OpenClWorkspaceFixture, OpenClTransientAllocateAppendsASlabWhenTheReservedTailIsTooShort) {
  OpenClRenderDevice device;
  auto&              backend    = device.Workspace().Device();
  auto&              transients = device.Workspace().TransientBuffers();
  constexpr std::size_t kSlab   = 1 << 20;
  constexpr std::size_t kLarge  = 700 << 10;
  backend.SetMaxSlabBytes(kSlab);
  transients.Reserve(kSlab + (256 << 10));
  void* first  = transients.Allocate(kLarge);
  void* second = transients.Allocate(kLarge);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  const auto a = backend.ResolveDeviceMemory(first, kLarge);
  const auto b = backend.ResolveDeviceMemory(second, kLarge);
  EXPECT_NE(a.first, b.first);
  EXPECT_GE(transients.capacity_bytes(), kLarge + kLarge);
}

TEST(TransientBufferArena, ReserveFreesUnusedSlabsBeforeAllocatingReplacements) {
  struct RecordingBackend {
    struct Slab {
      RecordingBackend*          owner = nullptr;
      std::size_t               bytes  = 0;
      std::unique_ptr<std::byte[]> storage;

      Slab() = default;
      Slab(RecordingBackend* owner_ptr, std::size_t n)
          : owner(owner_ptr), bytes(n), storage(std::make_unique<std::byte[]>(n)) {}
      Slab(const Slab&)                    = delete;
      auto operator=(const Slab&) -> Slab& = delete;
      Slab(Slab&& other) noexcept { *this = std::move(other); }
      auto operator=(Slab&& other) noexcept -> Slab& {
        Reset();
        owner        = other.owner;
        bytes        = other.bytes;
        storage      = std::move(other.storage);
        other.owner  = nullptr;
        other.bytes  = 0;
        return *this;
      }
      ~Slab() { Reset(); }

      void Reset() noexcept {
        if (owner != nullptr && storage) {
          owner->events.push_back("free");
        }
        owner = nullptr;
        storage.reset();
        bytes = 0;
      }

      [[nodiscard]] auto DevicePointer() const -> void* { return storage.get(); }
      [[nodiscard]] auto Bytes() const -> std::size_t { return bytes; }
    };

    std::vector<std::string> events;

    auto CreateSlab(std::size_t n) -> Slab {
      events.push_back("create");
      return Slab(this, n);
    }
  };

  RecordingBackend backend;
  TransientBufferArena<RecordingBackend> arena(backend);
  arena.Reserve(256);
  ASSERT_EQ(backend.events, (std::vector<std::string>{"create"}));
  arena.Reserve(1024);
  ASSERT_EQ(backend.events, (std::vector<std::string>{"create", "free", "create"}));
}

TEST(TransientBufferArena, UnalignedFirstAllocationAppendsAnotherSlabWithoutLooping) {
  struct AppendRecordingBackend {
    struct Slab {
      std::unique_ptr<std::byte[]> storage;
      std::size_t                  bytes = 0;

      Slab() = default;
      explicit Slab(std::size_t n) : storage(std::make_unique<std::byte[]>(n)), bytes(n) {}
      Slab(const Slab&)                    = delete;
      auto operator=(const Slab&) -> Slab& = delete;
      Slab(Slab&&) noexcept                = default;
      auto operator=(Slab&&) noexcept -> Slab& = default;

      [[nodiscard]] auto DevicePointer() const -> void* { return storage.get(); }
      [[nodiscard]] auto Bytes() const -> std::size_t { return bytes; }
    };

    std::vector<std::size_t> create_sizes;

    auto CreateSlab(std::size_t bytes) -> Slab {
      create_sizes.push_back(bytes);
      return Slab(bytes);
    }
  };

  AppendRecordingBackend                       backend;
  TransientBufferArena<AppendRecordingBackend> arena(backend);

  void* first  = arena.Allocate(257, 256);
  void* second = arena.Allocate(512, 256);

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(backend.create_sizes, (std::vector<std::size_t>{257, 512}));
  EXPECT_EQ(arena.capacity_bytes(), 769U);
  EXPECT_EQ(arena.used_bytes(), 769U);
}

TEST_F(OpenClWorkspaceFixture, OpenClSynchronizeRecordedWorkFlushesTheProductQueueBeforeWait) {
  OpenClRenderDevice device;
  device.BeginRender();
  const auto flushes = device.Workspace().Device().FlushCount();
  const auto waits   = device.Workspace().Device().WaitCount();
  device.Workspace().Device().SynchronizeRecordedWork(device.CommandContext());
  EXPECT_GT(device.Workspace().Device().FlushCount(), flushes);
  EXPECT_GT(device.Workspace().Device().WaitCount(), waits);
  device.CancelRender();
}

TEST_F(OpenClWorkspaceFixture, OpenClCreateBufferRejectsSizeAboveMaxSlabWithoutInvalidBufferSize) {
  OpenClRenderDevice device;
  auto&              backend = device.Workspace().Device();
  backend.SetMaxSlabBytes(4096);
  try {
    (void)backend.CreateBuffer(4097);
    FAIL() << "CreateBuffer should reject sizes above MaxSlabBytes";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("exceeds device max allocation"), std::string::npos);
    EXPECT_EQ(message.find("OpenCL error -61"), std::string::npos);
  }
}

TEST_F(OpenClWorkspaceFixture, OpenClTexturePoolReusesMatchingImages) {
  OpenClRenderDevice device;
  auto&              textures = device.Workspace().Textures();
  textures.SetByteBudget(64 * 64 * 16);

  device.BeginRender();
  auto first = textures.Acquire({32, 16, TextureFormat::Rgba32f});
  ASSERT_FALSE(first.Empty());
  const auto             resource_id = first.Texture().ResourceId();
  const auto             width       = first.Texture().Width();
  std::vector<std::byte> pixels(static_cast<std::size_t>(32) * 16 * 16, std::byte{0x3F});
  device.Workspace().Device().UploadTexture2D(first.Texture(), pixels, device.CommandContext());
  device.EndRender();
  first.Release();
  device.WaitIdle();
  device.Workspace().Device().ResetCounters();

  device.BeginRender();
  auto second = textures.Acquire({32, 16, TextureFormat::Rgba32f});
  EXPECT_EQ(second.Texture().ResourceId(), resource_id);
  EXPECT_EQ(second.Texture().Width(), width);
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(device.Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().FreeCount(), 0U);

  device.BeginRender();
  auto               r32 = textures.Acquire({8, 8, TextureFormat::R32f});
  std::vector<float> r32_host(64, 2.5f);
  auto               r32_bytes = std::as_bytes(std::span<const float>(r32_host));
  device.Workspace().Device().UploadTexture2D(r32.Texture(), r32_bytes, device.CommandContext());
  std::vector<float> r32_back(64, 0.0f);
  device.EndRender();
  device.WaitIdle();
  device.Workspace().Device().DownloadTexture2D(
      r32.Texture(), std::as_writable_bytes(std::span<float>(r32_back)), device.CommandContext());
  EXPECT_FLOAT_EQ(r32_back[0], 2.5f);
  EXPECT_FLOAT_EQ(r32_back[63], 2.5f);
}

TEST_F(OpenClWorkspaceFixture, OpenClTexturePoolDoesNotEvictBusySubmissionImages) {
  OpenClRenderDevice      device;
  auto&                   textures = device.Workspace().Textures();
  constexpr std::uint32_t kSize    = 64;
  textures.SetByteBudget(static_cast<std::size_t>(kSize) * kSize);

  device.BeginRender();
  auto       lease_a  = textures.Acquire({kSize, kSize, TextureFormat::R8});
  const auto handle_a = lease_a.Handle();
  auto       lease_b  = textures.Acquire({kSize, kSize, TextureFormat::R8});
  EXPECT_TRUE(textures.Contains(handle_a));
  device.EndRender();

  lease_a.Release();
  lease_b.Release();
  textures.EvictUntil(static_cast<std::size_t>(kSize) * kSize);
  EXPECT_TRUE(textures.Contains(handle_a));

  device.BeginRender();
  textures.EvictUntil(static_cast<std::size_t>(kSize) * kSize);
  EXPECT_FALSE(textures.Contains(handle_a));
  device.EndRender();
  device.WaitIdle();
}

TEST_F(OpenClWorkspaceFixture, OpenClMaskTextureCacheUsesOneWorkspaceByteBudget) {
  OpenClRenderDevice device;
  auto&              masks = device.Workspace().MaskTextures();
  const Extent2D     extent{8, 8};
  std::size_t        chain_bytes = 0;
  auto               level       = extent;
  while (true) {
    chain_bytes += static_cast<std::size_t>(level.width) * level.height;
    if (level.width == 1 && level.height == 1) {
      break;
    }
    level.width  = std::max<std::uint32_t>(level.width / 2, 1);
    level.height = std::max<std::uint32_t>(level.height / 2, 1);
  }
  masks.SetByteBudget(chain_bytes);

  {
    auto first = masks.Acquire(MaskAssetKey{"mask.a"}, extent);
    EXPECT_EQ(masks.EntryCount(), 1U);
    EXPECT_LE(masks.UsedBytes(), chain_bytes);
  }
  device.BeginRender();
  device.EndRender();
  device.WaitIdle();
  auto second = masks.Acquire(MaskAssetKey{"mask.b"}, extent);
  EXPECT_EQ(masks.EntryCount(), 1U);
  EXPECT_FALSE(masks.Contains(MaskAssetKey{"mask.a"}));
  EXPECT_TRUE(masks.Contains(MaskAssetKey{"mask.b"}));
  EXPECT_LE(masks.UsedBytes(), chain_bytes);
}

TEST_F(OpenClWorkspaceFixture, OpenClPlanWarmUpBuildsOnlyRequiredProgramsAndKernels) {
  RegisterOpenClBackendPrograms();
  const bool grade_before =
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kPrimaryGradeProgramName);
  const bool tone_before =
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kLocalToneProgramName);
  const bool mask_before =
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kMaskProgramName);
  const bool drt_before =
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kDrtProgramName);
  const bool camera_kernel_before = OpenClKernelCache::Instance().IsCached(
      OpenCL::GpuDag::kGeometryCameraProgramName, OpenCL::GpuDag::kCameraColorKernelName);
  const bool drt_kernel_before = OpenClKernelCache::Instance().IsCached(
      OpenCL::GpuDag::kDrtProgramName, OpenCL::GpuDag::kDrtKernelName);

  ExecutionPlan plan;
  plan.passes.push_back(GpuPassDesc{GpuPassKind::GeometryResample});

  OpenClRenderDevice device;
  device.Workspace().Device().WarmUpPlan(plan);

  EXPECT_TRUE(OpenClProgramLibrary::Instance().IsProgramBuilt(
      OpenCL::GpuDag::kGeometryCameraProgramName));
  EXPECT_TRUE(OpenClKernelCache::Instance().IsCached(OpenCL::GpuDag::kGeometryCameraProgramName,
                                                     OpenCL::GpuDag::kGeometryResampleKernelName));
  EXPECT_EQ(OpenClKernelCache::Instance().IsCached(OpenCL::GpuDag::kGeometryCameraProgramName,
                                                   OpenCL::GpuDag::kCameraColorKernelName),
            camera_kernel_before);
  EXPECT_EQ(OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kPrimaryGradeProgramName),
            grade_before);
  EXPECT_EQ(OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kLocalToneProgramName),
            tone_before);
  EXPECT_EQ(OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kMaskProgramName),
            mask_before);
  EXPECT_EQ(OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::GpuDag::kDrtProgramName),
            drt_before);
  EXPECT_EQ(OpenClKernelCache::Instance().IsCached(OpenCL::GpuDag::kDrtProgramName,
                                                   OpenCL::GpuDag::kDrtKernelName),
            drt_kernel_before);
}

TEST_F(OpenClWorkspaceFixture, OpenClSecondEmptyRenderCreatesNoBufferImageProgramOrKernel) {
  OpenClApiCounterScope     counter_scope(true);
  OpenClRenderDevice        device;
  auto&                     workspace = device.Workspace();
  ExecutionPlan             plan;
  plan.passes.push_back(GpuPassDesc{GpuPassKind::GeometryResample});
  workspace.Device().WarmUpPlan(plan);
  workspace.Device().ResetCounters();
  workspace.Device().WarmUpPlan(plan);
  EXPECT_TRUE(OpenClKernelCache::Instance().IsCached(OpenCL::GpuDag::kGeometryCameraProgramName,
                                                     OpenCL::GpuDag::kGeometryResampleKernelName));
  EXPECT_EQ(workspace.Device().KernelCreateCount(), 0U);
  EXPECT_GE(workspace.Device().KernelHitCount(), 1U);

  workspace.Parameters().Reserve(256);
  workspace.TransientBuffers().Reserve(1 << 20);
  workspace.Textures().SetByteBudget(64 * 64);
  workspace.MaskTextures().SetByteBudget(64 * 64);

  device.BeginRender();
  {
    auto texture = workspace.Textures().Acquire({64, 64, TextureFormat::R8});
    auto mask    = workspace.MaskTextures().Acquire(MaskAssetKey{"mask.stable"}, {32, 32});
    ASSERT_NE(workspace.TransientBuffers().Allocate(2048), nullptr);
    auto buffer = workspace.Device().CreateBuffer(64);
    workspace.Values().Store({NodeId{"grade.primary"}, PortId{"commands"}}, std::move(buffer));
    device.EndRender();
  }
  device.WaitIdle();
  workspace.Device().ResetCounters();
  ResetOpenClApiCounters();
  const auto before = SnapshotOpenClApiCounters();

  device.BeginRender();
  {
    auto texture = workspace.Textures().Acquire({64, 64, TextureFormat::R8});
    auto mask    = workspace.MaskTextures().Acquire(MaskAssetKey{"mask.stable"}, {32, 32});
    ASSERT_NE(workspace.TransientBuffers().Allocate(2048), nullptr);
    device.EndRender();
  }
  device.WaitIdle();

  const auto delta = DeltaOpenClApiCounters(before, SnapshotOpenClApiCounters());
  EXPECT_EQ(workspace.Device().BufferCreateCount(), 0U);
  EXPECT_EQ(workspace.Device().TextureCreateCount(), 0U);
  EXPECT_EQ(workspace.Device().KernelCreateCount(), 0U);
  EXPECT_EQ(workspace.Device().ProgramBuildCount(), 0U);
  EXPECT_EQ(workspace.Device().MallocCount(), 0U);
  EXPECT_EQ(workspace.Device().FreeCount(), 0U);
  EXPECT_EQ(workspace.Device().HostToDeviceBytes(), 0U);
  EXPECT_EQ(delta.create_buffer, 0U);
  EXPECT_EQ(delta.create_image, 0U);
  EXPECT_EQ(delta.create_kernel, 0U);
  EXPECT_EQ(delta.program_builds, 0U);
  EXPECT_EQ(delta.release_mem_object, 0U);
}

TEST_F(OpenClWorkspaceFixture, OpenClFailedUploadRestoresDirtyFieldsAndPublishesNoResult) {
  OpenClRenderDevice device;
  ParameterSlotKey   key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel      model;
  const auto         fields = ExposureFieldBindings();
  device.Workspace().Parameters().BindSlot(key, 4, fields);
  ASSERT_TRUE(UploadFullAndClearDirty(device, key, model));

  model.SetValue(0.75f);
  {
    auto pending = TakePendingParameterPatch(model);
    ASSERT_TRUE(pending.has_value());
    device.Workspace().Device().FailNextUpload();
    device.Workspace().Parameters().ApplyPatch(key, pending->Patch());
    EXPECT_THROW(device.Workspace().Parameters().UploadDirty(device.CommandContext()),
                 std::runtime_error);
  }
  EXPECT_TRUE(model.IsDirty());

  const GraphValueId id{NodeId{"develop"}, PortId{"sensor_linear"}};
  const ContentKey   content{41};
  device.BeginRender();
  (void)device.Workspace().AcquireImageForWrite(id, {8, 8, TextureFormat::Rgba32f});
  device.Workspace().Images().RecordUnpublished(id, content, {8, 8}, TextureFormat::Rgba32f,
                                                device.CommandContext().SubmissionId());
  device.Workspace().Device().FailNextUpload();
  std::vector<std::byte> pixels(8 * 8 * 16, std::byte{1});
  EXPECT_THROW(
      device.Workspace().Device().UploadTexture2D(device.Workspace().Images().Find(id)->Texture(),
                                                  pixels, device.CommandContext()),
      std::runtime_error);
  device.CancelRender();
  device.WaitIdle();
  EXPECT_FALSE(device.Workspace().Images().FindValidResult(
      id, content, {8, 8}, TextureFormat::Rgba32f,
      device.Workspace().Device().CompletedSubmission()));
  EXPECT_EQ(device.Workspace().Images().PublishedCount(), 0U);
}

TEST_F(OpenClWorkspaceFixture, OpenClProgramManifestCanLoadFromInstalledResourceLayout) {
  RegisterOpenClBackendPrograms();
  const auto manifests = OpenClBackendProgramRegistry::Instance().RegisteredManifestNames();
  EXPECT_NE(std::find(manifests.begin(), manifests.end(), OpenCL::GpuDag::kManifestName),
            manifests.end());

  const auto source_root = std::filesystem::path{ALCEDO_OPENCL_SHADER_SOURCE_ROOT};
  const auto file_name   = std::string("o0_install_probe_") + UniqueSuffix() + ".cl";
  const auto missing_source =
      source_root / "edit" / "runtime" / "opencl" / "shader" / file_name;
  ASSERT_FALSE(std::filesystem::exists(missing_source)) << missing_source.string();

  const auto installed =
      TestExecutableDir() / "opencl" / "edit" / "runtime" / "opencl" / "shader" / file_name;
  std::filesystem::create_directories(installed.parent_path());
  {
    std::ofstream source(installed, std::ios::binary);
    ASSERT_TRUE(source.is_open());
    source << "__kernel void o0_install_probe(__global float* out) { out[get_global_id(0)] = 1.0f; }";
  }

  const auto program_name = std::string("opencl_dag_install_probe_") + UniqueSuffix();
  OpenClProgramLibrary::Instance().RegisterProgram(OpenClProgramDescriptor{
      .name                = program_name,
      .source_paths        = {missing_source},
      .build_options       = "-cl-std=CL1.2",
      .required_at_startup = false,
  });
  EXPECT_NO_THROW((void)OpenClProgramLibrary::Instance().GetProgram(program_name));
  EXPECT_TRUE(OpenClProgramLibrary::Instance().IsProgramBuilt(program_name));
  std::filesystem::remove(installed);
}

TEST_F(OpenClWorkspaceFixture, OpenClCommandContextReleasesEveryRetainedEvent) {
  OpenClApiCounterScope counter_scope(true);
  ResetOpenClApiCounters();
  OpenClRenderDevice device;
  device.Workspace().Parameters().Reserve(64);

  std::vector<std::byte> bytes(16, std::byte{2});
  device.BeginRender();
  auto buffer = device.Workspace().Device().CreateBuffer(64);
  device.Workspace().Device().UploadBufferRange(buffer, 0, bytes, device.CommandContext());
  EXPECT_GT(device.CommandContext().TrackedEventCount(), 0U);
  device.EndRender();
  device.WaitIdle();

  const auto after = SnapshotOpenClApiCounters();
  EXPECT_GT(after.create_event, 0U);
  EXPECT_EQ(after.create_event, after.release_event);
  EXPECT_EQ(device.Workspace().Device().EventCreateCount(),
            device.Workspace().Device().EventReleaseCount());
  EXPECT_EQ(device.CommandContext().TrackedEventCount(), 0U);
}

}  // namespace
}  // namespace alcedo
