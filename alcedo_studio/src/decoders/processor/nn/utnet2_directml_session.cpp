//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/utnet2_directml_session.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
// After windows.h.
#include <pdh.h>
#include <pdhmsg.h>

// The libraries of the session (see decoders/CMakeLists.txt: not on the CMake link line).
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")
// The delay load of onnxruntime.dll (/DELAYLOAD, an interface link option of the target).
#pragma comment(lib, "delayimp.lib")
#include <wrl/client.h>

// ORT_API_MANUAL_INIT comes from the alcedo::onnxruntime_directml target: onnxruntime.dll is
// delay-loaded, and OnnxRuntimeApi() loads it at the first session.
#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>
#include <onnxruntime_session_options_config_keys.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "decoders/processor/nn/utnet2_onnx_graph.hpp"
#include "decoders/processor/nn/utnet2_specs.hpp"
#include "decoders/processor/nn/utnet2_tile_io.hpp"

namespace alcedo::nn {
namespace {

using Microsoft::WRL::ComPtr;

auto HresultText(HRESULT result) -> std::string {
  std::array<char, 16> text{};
  std::snprintf(text.data(), text.size(), "0x%08lX", static_cast<unsigned long>(result));
  return text.data();
}

auto WideText(const wchar_t* text) -> std::string {
  return UtNet2PathText(std::filesystem::path(text));
}

auto ModulePath(HMODULE module) -> std::string {
  std::wstring buffer(MAX_PATH, L'\0');
  const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return UtNet2PathText(std::filesystem::path(buffer));
}

/// Loads onnxruntime.dll (the folder of the executable first, then System32) and initializes the
/// C++ API. The delay-load helper of the linker then resolves to the loaded module. A
/// System32 copy of Windows (an older version) fails the API version check with its path.
auto OnnxRuntimeApi() -> const OrtApi& {
  static const OrtApi* api = [] {
    HMODULE module =
        LoadLibraryExW(L"onnxruntime.dll", nullptr,
                       LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
      throw std::runtime_error("ONNX Runtime: onnxruntime.dll failed to load (Windows error " +
                               std::to_string(GetLastError()) + ")");
    }
    const OrtApiBase* base   = OrtGetApiBase();
    const OrtApi*     result = base->GetApi(ORT_API_VERSION);
    if (result == nullptr) {
      throw std::runtime_error("ONNX Runtime: " + ModulePath(module) + " (version " +
                               base->GetVersionString() + ") does not provide API version " +
                               std::to_string(ORT_API_VERSION));
    }
    Ort::InitApi(result);
    return result;
  }();
  return *api;
}

/// The process-wide environment. It is not destroyed: sessions of other owners can end during
/// the static destruction of the process, after this function's statics.
auto ProcessEnv() -> Ort::Env& {
  static Ort::Env* env = [] {
    OnnxRuntimeApi();
    return new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "alcedo_ai_denoise");
  }();
  return *env;
}

auto DmlApi() -> const OrtDmlApi& {
  const void* api = nullptr;
  Ort::ThrowOnError(OnnxRuntimeApi().GetExecutionProviderApi("DML", ORT_API_VERSION, &api));
  if (api == nullptr) {
    throw std::runtime_error("ONNX Runtime: the DirectML execution provider API is absent");
  }
  return *static_cast<const OrtDmlApi*>(api);
}

/// `DMLCreateDevice1` of the DirectML.dll beside the executable (the redistributable 1.15.4 that
/// ONNX Runtime also uses; the System32 copy of Windows is older).
auto DmlCreateDevice() -> decltype(&DMLCreateDevice1) {
  static const auto function = [] {
    HMODULE module =
        LoadLibraryExW(L"DirectML.dll", nullptr,
                       LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
      throw std::runtime_error("DirectML: DirectML.dll failed to load (Windows error " +
                               std::to_string(GetLastError()) + ")");
    }
    auto* address = GetProcAddress(module, "DMLCreateDevice1");
    if (address == nullptr) {
      throw std::runtime_error("DirectML: " + ModulePath(module) + " has no DMLCreateDevice1");
    }
    return reinterpret_cast<decltype(&DMLCreateDevice1)>(reinterpret_cast<void*>(address));
  }();
  return function;
}

/// True when the adapter has no memory of its own (D3D12 UMA: an integrated GPU).
auto IsUmaAdapter(IDXGIAdapter3* adapter) -> bool {
  ComPtr<ID3D12Device> device;
  if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    return false;
  }
  D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
  return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                               sizeof(architecture))) &&
         architecture.UMA != FALSE;
}

/// The Windows performance counter of the local video memory that all processes use on one
/// adapter (`\GPU Adapter Memory(luid_<high>_<low>_phys_0)\Dedicated Usage`, bytes). The DXGI
/// budget of a process does not count the memory of the other processes (issue #1043).
class AdapterLocalUsageCounter {
 public:
  explicit AdapterLocalUsageCounter(const LUID& luid) {
    std::array<wchar_t, 128> path{};
    std::swprintf(path.data(), path.size(),
                  L"\\GPU Adapter Memory(luid_0x%08lX_0x%08lX_phys_0)\\Dedicated Usage",
                  static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
      query_ = nullptr;
      return;
    }
    if (PdhAddEnglishCounterW(query_, path.data(), 0, &counter_) != ERROR_SUCCESS) {
      PdhCloseQuery(query_);
      query_ = nullptr;
    }
  }
  ~AdapterLocalUsageCounter() {
    if (query_ != nullptr) {
      PdhCloseQuery(query_);
    }
  }
  AdapterLocalUsageCounter(const AdapterLocalUsageCounter&)                    = delete;
  auto operator=(const AdapterLocalUsageCounter&) -> AdapterLocalUsageCounter& = delete;

  /// The current value, or nothing when Windows has no such counter.
  [[nodiscard]] auto Read() const -> std::optional<std::uint64_t> {
    if (query_ == nullptr || PdhCollectQueryData(query_) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    PDH_FMT_COUNTERVALUE value{};
    if (PdhGetFormattedCounterValue(counter_, PDH_FMT_LARGE, nullptr, &value) != ERROR_SUCCESS ||
        value.CStatus != PDH_CSTATUS_VALID_DATA || value.largeValue < 0) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(value.largeValue);
  }

 private:
  PDH_HQUERY   query_   = nullptr;
  PDH_HCOUNTER counter_ = nullptr;
};

/// The first hardware adapter of `preference` that creates a Direct3D 12 device.
auto SelectAdapter(DirectMlAdapterPreference preference) -> ComPtr<IDXGIAdapter3> {
  ComPtr<IDXGIFactory6> factory;
  if (const HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)); FAILED(result)) {
    throw std::runtime_error("DXGI: CreateDXGIFactory2 failed (" + HresultText(result) + ")");
  }
  const auto gpu_preference = preference == DirectMlAdapterPreference::MinimumPower
                                  ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                  : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter3> adapter;
    if (factory->EnumAdapterByGpuPreference(index, gpu_preference, IID_PPV_ARGS(&adapter)) ==
        DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 description{};
    if (adapter == nullptr || FAILED(adapter->GetDesc1(&description)) ||
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
      continue;
    }
    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                    nullptr))) {
      return adapter;
    }
  }
  throw std::runtime_error("DirectML: no hardware adapter with Direct3D 12");
}

auto CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type, std::uint64_t bytes,
                  D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    -> ComPtr<ID3D12Resource> {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = heap_type;
  D3D12_RESOURCE_DESC description{};
  description.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Width            = bytes;
  description.Height           = 1;
  description.DepthOrArraySize = 1;
  description.MipLevels        = 1;
  description.SampleDesc.Count = 1;
  description.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  description.Flags            = flags;
  ComPtr<ID3D12Resource> buffer;
  if (const HRESULT result = device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(&buffer));
      FAILED(result)) {
    throw std::runtime_error("Direct3D 12: a buffer of " + std::to_string(bytes) +
                             " bytes failed (" + HresultText(result) + ")");
  }
  return buffer;
}

/// A float16 buffer in device memory that the DirectML execution provider can bind, with the
/// allocation of the provider that wraps it.
struct DmlBuffer {
  ComPtr<ID3D12Resource> resource;
  void*                  allocation              = nullptr;
  std::uint64_t          bytes                   = 0;

  DmlBuffer()                                    = default;
  DmlBuffer(const DmlBuffer&)                    = delete;
  auto operator=(const DmlBuffer&) -> DmlBuffer& = delete;
  ~DmlBuffer() { Reset(); }

  void Reset() {
    if (allocation != nullptr) {
      DmlApi().FreeGPUAllocation(allocation);
      allocation = nullptr;
    }
    resource.Reset();
    bytes = 0;
  }

  /// Creates the buffer again when it holds fewer than `required` bytes.
  void Reserve(ID3D12Device* device, std::uint64_t required) {
    if (bytes >= required) {
      return;
    }
    Reset();
    resource =
        CreateBuffer(device, D3D12_HEAP_TYPE_DEFAULT, required,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Ort::ThrowOnError(DmlApi().CreateGPUAllocationFromD3DResource(resource.Get(), &allocation));
    bytes = required;
  }
};

/// A host-visible buffer (upload or readback heap), mapped while it exists.
struct MappedBuffer {
  ComPtr<ID3D12Resource> resource;
  void*                  data  = nullptr;
  std::uint64_t          bytes = 0;

  void Reserve(ID3D12Device* device, D3D12_HEAP_TYPE heap_type, std::uint64_t required) {
    if (bytes >= required) {
      return;
    }
    resource.Reset();
    data             = nullptr;
    bytes            = 0;
    const auto state = heap_type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                           : D3D12_RESOURCE_STATE_COPY_DEST;
    resource         = CreateBuffer(device, heap_type, required, D3D12_RESOURCE_FLAG_NONE, state);
    if (const HRESULT result = resource->Map(0, nullptr, &data); FAILED(result)) {
      resource.Reset();
      throw std::runtime_error("Direct3D 12: Map failed (" + HresultText(result) + ")");
    }
    bytes = required;
  }
};

/// The Direct3D 12 and DirectML devices of a loaded session, its queue, and the buffers of its
/// tiles. The DirectML execution provider runs the graph on `queue`; the copies of the session
/// run on the same queue, so the queue orders them with the graph.
struct DeviceObjects {
  ComPtr<ID3D12Device>                             device;
  ComPtr<IDMLDevice>                               dml;
  ComPtr<ID3D12CommandQueue>                       queue;
  std::array<ComPtr<ID3D12CommandAllocator>, 2>    allocators;
  std::array<ComPtr<ID3D12GraphicsCommandList>, 2> lists;
  ComPtr<ID3D12Fence>                              fence;
  std::uint64_t                                    fence_value = 0;
  HANDLE                                           fence_event = nullptr;
  ComPtr<ID3D12QueryHeap>                          timestamps;
  MappedBuffer                                     timestamp_readback;
  std::uint64_t                                    timestamp_frequency = 0;
  DmlBuffer                                        input;
  DmlBuffer                                        output;
  MappedBuffer                                     upload;
  MappedBuffer                                     readback;

  DeviceObjects()                                        = default;
  DeviceObjects(const DeviceObjects&)                    = delete;
  auto operator=(const DeviceObjects&) -> DeviceObjects& = delete;
  ~DeviceObjects() {
    if (fence_event != nullptr) {
      CloseHandle(fence_event);
    }
  }

  void Check(HRESULT result, std::string_view call) const {
    if (FAILED(result)) {
      std::string text = "Direct3D 12: " + std::string(call) + " failed (" + HresultText(result);
      if (device != nullptr) {
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
          text += ", device removed: " + HresultText(removed);
        }
      }
      throw std::runtime_error(text + ")");
    }
  }

  void Create(IDXGIAdapter3* adapter) {
    Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
          "D3D12CreateDevice");
    const HRESULT dml_result = DmlCreateDevice()(device.Get(), DML_CREATE_DEVICE_FLAG_NONE,
                                                 DML_FEATURE_LEVEL_5_0, IID_PPV_ARGS(&dml));
    if (FAILED(dml_result)) {
      throw std::runtime_error("DirectML: DMLCreateDevice1 with feature level 5.0 failed (" +
                               HresultText(dml_result) + ")");
    }
    // The queue of the DirectML execution provider (ONNX Runtime creates the same for
    // SessionOptionsAppendExecutionProvider_DML2).
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    Check(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue)),
          "CreateCommandQueue");
    for (std::size_t index = 0; index < allocators.size(); ++index) {
      Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocators[index])),
            "CreateCommandAllocator");
      Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[index].Get(),
                                      nullptr, IID_PPV_ARGS(&lists[index])),
            "CreateCommandList");
      Check(lists[index]->Close(), "Close");
    }
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr) {
      throw std::runtime_error("Windows: CreateEvent failed (" + std::to_string(GetLastError()) +
                               ")");
    }
    D3D12_QUERY_HEAP_DESC query_description{};
    query_description.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_description.Count = 2;
    Check(device->CreateQueryHeap(&query_description, IID_PPV_ARGS(&timestamps)),
          "CreateQueryHeap");
    timestamp_readback.Reserve(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * sizeof(std::uint64_t));
    Check(queue->GetTimestampFrequency(&timestamp_frequency), "GetTimestampFrequency");
  }

  auto Begin(std::size_t index) -> ID3D12GraphicsCommandList* {
    Check(allocators[index]->Reset(), "CommandAllocator::Reset");
    Check(lists[index]->Reset(allocators[index].Get(), nullptr), "CommandList::Reset");
    return lists[index].Get();
  }

  void Submit(std::size_t index) {
    Check(lists[index]->Close(), "CommandList::Close");
    ID3D12CommandList* list = lists[index].Get();
    queue->ExecuteCommandLists(1, &list);
  }

  /// Waits until the queue has run all submitted work.
  void Wait() {
    ++fence_value;
    Check(queue->Signal(fence.Get(), fence_value), "Signal");
    if (fence->GetCompletedValue() < fence_value) {
      Check(fence->SetEventOnCompletion(fence_value, fence_event), "SetEventOnCompletion");
      WaitForSingleObject(fence_event, INFINITE);
    }
    Check(device->GetDeviceRemovedReason(), "the queue");
  }
};

auto SessionOptions(IDMLDevice* dml, ID3D12CommandQueue* queue) -> Ort::SessionOptions {
  Ort::SessionOptions options;
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  // Required by the DirectML execution provider.
  options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  options.DisableMemPattern();
  options.DisableCpuMemArena();
  options.SetIntraOpNumThreads(4);
  options.SetInterOpNumThreads(1);
  options.AddConfigEntry(kOrtSessionOptionsConfigAllowIntraOpSpinning, "0");
  // A node that DirectML cannot run is a load error, not a CPU run (plan 2.4).
  options.AddConfigEntry(kOrtSessionOptionsDisableCPUEPFallback, "1");
  Ort::ThrowOnError(DmlApi().SessionOptionsAppendExecutionProvider_DML1(options, dml, queue));
  return options;
}

}  // namespace

struct UtNet2DirectMlSession::Impl {
  std::filesystem::path          graph_path;
  ComPtr<IDXGIAdapter3>          adapter;
  // The local video memory of the adapter (DXGI `DedicatedVideoMemory`), 0 for a UMA adapter,
  // and the counter of its use by all processes (absent for a UMA adapter).
  std::uint64_t                             adapter_local_bytes = 0;
  std::unique_ptr<AdapterLocalUsageCounter> adapter_local_usage;
  float                          output_scale = 1.0f;
  // Declared before `session`, so that the session ends first: it holds the queue and the
  // DirectML device.
  std::optional<DeviceObjects>   devices;
  std::optional<Ort::Session>    session;
  std::optional<Ort::MemoryInfo> dml_memory;
  // The tile input shapes (width, height) that ran since the load: DirectML compiles the graph
  // at the first run of each shape (DN3 completion record).
  std::set<std::pair<int, int>>  ran_shapes;
  int                            largest_tile_side = 0;
  std::chrono::nanoseconds       last_device_time{};

  void                           Load() {
    if (!std::filesystem::is_regular_file(graph_path)) {
      throw std::runtime_error("UtNet2 graph " + UtNet2PathText(graph_path) + " does not exist");
    }
    try {
      auto& env = ProcessEnv();
      devices.emplace();
      devices->Create(adapter.Get());
      auto         options = SessionOptions(devices->dml.Get(), devices->queue.Get());
      Ort::Session created(env, graph_path.c_str(), options);
      output_scale = CheckUtNet2Graph(created, graph_path);
      session.emplace(std::move(created));
      dml_memory.emplace("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);
    } catch (const Ort::Exception& error) {
      Unload();
      throw std::runtime_error("UtNet2 graph " + UtNet2PathText(graph_path) +
                                          ": ONNX Runtime: " + error.what());
    } catch (const std::runtime_error& error) {
      Unload();
      throw std::runtime_error("UtNet2 graph " + UtNet2PathText(graph_path) + ": " + error.what());
    }
    ran_shapes.clear();
    largest_tile_side = 0;
  }

  void Unload() {
    dml_memory.reset();
    session.reset();
    devices.reset();
  }

  void RunOnDevice(std::span<const float> input, const PackedTileRect& tile,
                   const TileOutputCrop& crop) {
    auto&         objects       = *devices;
    const auto    pixels        = static_cast<std::size_t>(tile.width) * tile.height;
    constexpr int kUpscale      = UtNet2Spec::kOutputUpscale;
    const int     output_width  = kUpscale * tile.width;
    const int     output_height = kUpscale * tile.height;
    const auto    input_count   = UtNet2Spec::kInputChannels * pixels;
    const auto    output_count =
        UtNet2Spec::kOutputChannels * static_cast<std::size_t>(output_width) * output_height;
    const auto plane_bytes    = static_cast<std::uint64_t>(output_width) * output_height * 2;
    const auto row_bytes      = static_cast<std::uint64_t>(output_width) * 2;
    const auto readback_plane = row_bytes * crop.height;

    objects.input.Reserve(objects.device.Get(), input_count * 2);
    objects.output.Reserve(objects.device.Get(), output_count * 2);
    objects.upload.Reserve(objects.device.Get(), D3D12_HEAP_TYPE_UPLOAD, input_count * 2);
    objects.readback.Reserve(objects.device.Get(), D3D12_HEAP_TYPE_READBACK,
                             UtNet2Spec::kOutputChannels * readback_plane);

    // Input: float32 to float16 on the host (rows of the planes in parallel), into the upload
    // buffer; then the copy to the input buffer and the first timestamp.
    ConvertUtNet2TileInput(input, tile, objects.upload.data);
    auto* copy_in = objects.Begin(0);
    copy_in->CopyBufferRegion(objects.input.resource.Get(), 0, objects.upload.resource.Get(), 0,
                              input_count * 2);
    copy_in->EndQuery(objects.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    objects.Submit(0);

    const std::array<std::int64_t, 4> input_shape  = {1, UtNet2Spec::kInputChannels, tile.height,
                                                      tile.width};
    const std::array<std::int64_t, 4> output_shape = {1, UtNet2Spec::kOutputChannels, output_height,
                                                      output_width};
    try {
      auto input_value = Ort::Value::CreateTensor(
          *dml_memory, objects.input.allocation, objects.input.bytes, input_shape.data(),
          input_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
      auto output_value = Ort::Value::CreateTensor(
          *dml_memory, objects.output.allocation, objects.output.bytes, output_shape.data(),
          output_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
      Ort::IoBinding binding(*session);
      binding.BindInput(UtNet2Spec::kInputName.data(), input_value);
      binding.BindOutput(UtNet2Spec::kOutputName.data(), output_value);
      session->Run(Ort::RunOptions{nullptr}, binding);
      binding.SynchronizeOutputs();
    } catch (const Ort::Exception& error) {
      std::string text = UtNet2TileText(tile) + ": ONNX Runtime: " + error.what();
      // The input copy must end before a later run resets its command allocator. A failure of
      // this wait (a removed device) is added to the text of the run error.
      try {
        objects.Wait();
      } catch (const std::runtime_error& wait_error) {
        text += "; ";
        text += wait_error.what();
      }
      throw std::runtime_error(text);
    }

    // Output: the second timestamp, the rows of the rectangle of each channel to the readback
    // buffer, and the timestamps.
    auto* copy_out = objects.Begin(1);
    copy_out->EndQuery(objects.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    for (int channel = 0; channel < UtNet2Spec::kOutputChannels; ++channel) {
      copy_out->CopyBufferRegion(objects.readback.resource.Get(), channel * readback_plane,
                                 objects.output.resource.Get(),
                                 channel * plane_bytes + crop.source_y * row_bytes, readback_plane);
    }
    copy_out->ResolveQueryData(objects.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                               objects.timestamp_readback.resource.Get(), 0);
    objects.Submit(1);
    objects.Wait();

    const auto* stamps       = static_cast<const std::uint64_t*>(objects.timestamp_readback.data);
    const auto  ticks        = stamps[1] >= stamps[0] ? stamps[1] - stamps[0] : 0;
    last_device_time         = std::chrono::nanoseconds(static_cast<std::int64_t>(
        static_cast<double>(ticks) * 1e9 / static_cast<double>(objects.timestamp_frequency)));

    // float16 to float32 times output_scale, into the rectangle of the destination, and the
    // check for values that are not finite.
    WriteUtNet2OutputCrop(UtNet2Float16OutputRows{.first_row = static_cast<const std::uint8_t*>(
                                                      objects.readback.data),
                                                  .row_bytes   = row_bytes,
                                                  .plane_bytes = readback_plane},
                          tile, crop, output_scale);
  }
};

void LoadDirectMlRuntime() {
  (void)OnnxRuntimeApi();
  (void)DmlCreateDevice();
}

auto DirectMlAdapterKey(DirectMlAdapterPreference preference) -> std::string {
  const auto         adapter = SelectAdapter(preference);
  DXGI_ADAPTER_DESC1 description{};
  if (const HRESULT result = adapter->GetDesc1(&description); FAILED(result)) {
    throw std::runtime_error("DXGI: GetDesc1 failed (" + HresultText(result) + ")");
  }
  std::array<char, 32> luid{};
  std::snprintf(luid.data(), luid.size(), "0x%08lX%08lX",
                static_cast<unsigned long>(description.AdapterLuid.HighPart),
                static_cast<unsigned long>(description.AdapterLuid.LowPart));
  return WideText(description.Description) + " (LUID " + luid.data() + ")";
}

UtNet2DirectMlSession::UtNet2DirectMlSession(std::filesystem::path     graph_path,
                                             DirectMlAdapterPreference preference)
    : impl_(std::make_unique<Impl>()) {
  impl_->graph_path = std::move(graph_path);
  if (!std::filesystem::is_regular_file(impl_->graph_path)) {
    throw std::runtime_error("UtNet2 graph " + UtNet2PathText(impl_->graph_path) +
                             " does not exist");
  }
  impl_->adapter = SelectAdapter(preference);
  DXGI_ADAPTER_DESC1 description{};
  if (!IsUmaAdapter(impl_->adapter.Get()) && SUCCEEDED(impl_->adapter->GetDesc1(&description))) {
    impl_->adapter_local_bytes = description.DedicatedVideoMemory;
    impl_->adapter_local_usage =
        std::make_unique<AdapterLocalUsageCounter>(description.AdapterLuid);
  }
  impl_->Load();
}

UtNet2DirectMlSession::~UtNet2DirectMlSession()                                = default;
UtNet2DirectMlSession::UtNet2DirectMlSession(UtNet2DirectMlSession&&) noexcept = default;
auto UtNet2DirectMlSession::operator=(UtNet2DirectMlSession&&) noexcept
    -> UtNet2DirectMlSession& = default;

void UtNet2DirectMlSession::Run(std::span<const float> input, const PackedTileRect& tile,
                                const TileOutputCrop& crop) {
  CheckUtNet2TileRun(input, tile, crop);
  if (!impl_->session) {
    impl_->Load();
  }
  impl_->RunOnDevice(input, tile, crop);
  impl_->ran_shapes.emplace(tile.width, tile.height);
  impl_->largest_tile_side = std::max({impl_->largest_tile_side, tile.width, tile.height});
}

void UtNet2DirectMlSession::Run(std::span<const float> input, const PackedTileRect& tile,
                                std::span<float> output) {
  constexpr int kUpscale = UtNet2Spec::kOutputUpscale;
  Run(input, tile,
      TileOutputCrop{.source_x      = 0,
                     .source_y      = 0,
                     .width         = kUpscale * tile.width,
                     .height        = kUpscale * tile.height,
                     .destination   = {output, kUpscale * tile.width, kUpscale * tile.height},
                     .destination_x = 0,
                     .destination_y = 0});
}

auto UtNet2DirectMlSession::IsReadyFor(int width, int height) const -> bool {
  return impl_->session.has_value() && impl_->ran_shapes.contains({width, height});
}

auto UtNet2DirectMlSession::TileMemory() const -> std::span<const UtNet2TileMemory> {
  return UtNet2Spec::kDirectMlTileMemory;
}

auto UtNet2DirectMlSession::DeviceMemory() const -> DeviceMemoryState {
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (const HRESULT result =
          impl_->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
      FAILED(result)) {
    throw std::runtime_error("DXGI: QueryVideoMemoryInfo failed (" + HresultText(result) + ")");
  }
  DeviceMemoryState state{.budget_bytes = info.Budget, .usage_bytes = info.CurrentUsage};
  if (impl_->adapter_local_usage) {
    if (const auto usage = impl_->adapter_local_usage->Read(); usage.has_value()) {
      state.adapter_local_bytes       = impl_->adapter_local_bytes;
      state.adapter_local_usage_bytes = *usage;
    }
  }
  if (impl_->session) {
    for (const auto& entry : UtNet2Spec::kDirectMlTileMemory) {
      if (entry.tile_size <= impl_->largest_tile_side) {
        state.held_bytes = std::max(state.held_bytes, entry.device_bytes);
      }
    }
  }
  return state;
}

void UtNet2DirectMlSession::Release() {
  impl_->Unload();
  impl_->ran_shapes.clear();
  impl_->largest_tile_side = 0;
}

auto UtNet2DirectMlSession::IsLoaded() const -> bool { return impl_->session.has_value(); }

auto UtNet2DirectMlSession::OutputScale() const -> float { return impl_->output_scale; }

auto UtNet2DirectMlSession::GraphPath() const -> const std::filesystem::path& {
  return impl_->graph_path;
}

auto UtNet2DirectMlSession::AdapterName() const -> std::string {
  DXGI_ADAPTER_DESC1 description{};
  impl_->adapter->GetDesc1(&description);
  return WideText(description.Description);
}

auto UtNet2DirectMlSession::LastRunDeviceTime() const -> std::chrono::nanoseconds {
  return impl_->last_device_time;
}

}  // namespace alcedo::nn
