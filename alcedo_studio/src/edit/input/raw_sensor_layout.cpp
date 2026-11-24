//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/input/raw_sensor_layout.hpp"

#include <libraw/libraw.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {
namespace {

auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

auto OpenRaw(LibRaw& raw, const std::filesystem::path& path) -> int {
#ifdef _WIN32
  return raw.open_file(path.wstring().c_str());
#else
  return raw.open_file(path.string().c_str());
#endif
}

}  // namespace

auto ReadRawSensorLayout(const std::filesystem::path& path) -> RawSensorLayout {
  // LibRaw is larger than the macOS worker stack; it must not be a local object.
  auto raw = std::make_unique<LibRaw>();
  if (OpenRaw(*raw, path) != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw cannot open " + PathText(path));
  }
  const auto& idata  = raw->imgdata.idata;
  // The same rules as the loader (RawInputLoader::LoadEncoded): Foveon and rotated Fujifilm
  // files are not developed; filters 0 and 1 are decoded or monochrome data.
  auto        layout = RawSensorLayout::Other;
  if (idata.is_foveon == 0U && raw->is_fuji_rotated() == 0 && idata.colors == 3) {
    if (idata.filters == 9U) {
      layout = RawSensorLayout::XTrans;
    } else if (idata.filters > 1U && IsClassic2x2Bayer(ReadLibRawBayerPattern(*raw))) {
      layout = RawSensorLayout::Bayer;
    }
  }
  raw->recycle();
  return layout;
}

}  // namespace alcedo
