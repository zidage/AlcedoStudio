//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX2 experiment: a QQuickRhiItem with the settings of EditorViewportItem (RGBA32F colour buffer,
// no mirror, no blending) that draws the photo with the product composite shaders
// (editor_viewport.vert, editor_viewport.frag) and the display table. Experiment code: it does
// not merge.
#pragma once

#include <QQuickRhiItem>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vx2 {

class Producer;

struct DisplayLut {
  int                edge = 0;
  std::vector<float> rgba;
  std::string        description;
  std::wstring       profile_path;
};

auto BuildDisplayLut(const std::wstring& display_device, int edge) -> DisplayLut;
auto SampleLut(const DisplayLut& lut, float r, float g, float b) -> std::array<double, 3>;

enum class PhotoSource {
  /// The producer thread writes VkImages on the pipeline queue; Qt samples them with zero copy.
  VulkanProducer,
  /// A QRhi compute pass in the Qt frame writes the photo (Direct3D 11 reference).
  RhiCompute,
};

/// Render thread marks of the frame in progress; read after the frame end.
struct FrameMarks {
  std::uint64_t new_id         = 0;
  std::int64_t  new_request_ns = 0;
};

class Vx2Item : public QQuickRhiItem {
  Q_OBJECT

 public:
  explicit Vx2Item(QQuickItem* parent = nullptr);

  struct Config {
    PhotoSource                       source = PhotoSource::RhiCompute;
    Producer*                         producer = nullptr;
    const std::vector<float>*         fixture  = nullptr;
    int                               width    = 0;
    int                               height   = 0;
    int                               work     = 0;
    std::shared_ptr<const DisplayLut> lut;
    /// scaleZoom.y = -1 when the NDC of the API has Y down (Vulkan).
    bool                              flip_correction = false;
  };

  void Configure(const Config& config) { config_ = config; }
  [[nodiscard]] auto config() const -> const Config& { return config_; }

  /// GUI thread. RhiCompute: the next frame runs the compute pass with @p gain.
  void RequestGain(float gain, std::int64_t request_ns);
  auto TakeRequest(float& gain, std::uint64_t& id, std::int64_t& request_ns) -> bool;

  static std::atomic<std::uint64_t> submitted_frames;
  static FrameMarks                 marks;  // render thread only
  static std::atomic<bool>          y_up_in_ndc;
  static std::atomic<int>           errors;

 protected:
  QQuickRhiItemRenderer* createRenderer() override;

 private:
  Config        config_;
  std::mutex    request_mutex_;
  bool          request_pending_ = false;
  float         request_gain_    = 1.0f;
  std::uint64_t request_id_      = 0;
  std::int64_t  request_ns_      = 0;
};

}  // namespace vx2
