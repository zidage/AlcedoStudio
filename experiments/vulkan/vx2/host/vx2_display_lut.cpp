//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX2 experiment: the display table of the Windows display ICC plan, built with the conversion
// of display_color_transform.cpp (relative colorimetric, black point compensation, float RGB,
// one blue slice after the other) from sRGB to the ICC profile of the monitor of the window.
// Experiment code: it does not merge.
#include <windows.h>

#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "vx2_item.hpp"

namespace vx2 {

auto MonitorProfilePath(const std::wstring& display_device) -> std::wstring {
  HDC dc = CreateDCW(L"DISPLAY", display_device.c_str(), nullptr, nullptr);
  if (dc == nullptr) throw std::runtime_error("CreateDCW failed for the display device");
  DWORD   size = MAX_PATH;
  wchar_t path[MAX_PATH]{};
  const BOOL ok = GetICMProfileW(dc, &size, path);
  DeleteDC(dc);
  if (!ok) throw std::runtime_error("GetICMProfileW found no monitor profile");
  return path;
}

auto BuildDisplayLut(const std::wstring& display_device, int edge) -> DisplayLut {
  DisplayLut lut;
  lut.edge         = edge;
  lut.profile_path = MonitorProfilePath(display_device);
  std::ifstream file(lut.profile_path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
  if (bytes.empty()) throw std::runtime_error("the monitor profile file is empty");
  cmsHPROFILE display = cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size()));
  if (display == nullptr) throw std::runtime_error("LittleCMS cannot parse the monitor profile");
  char description[256]{};
  cmsGetProfileInfoASCII(display, cmsInfoDescription, "en", "US", description, sizeof(description));
  lut.description = description;
  cmsHPROFILE   source    = cmsCreate_sRGBProfile();
  cmsHTRANSFORM transform = cmsCreateTransform(source, TYPE_RGB_FLT, display, TYPE_RGB_FLT,
                                               INTENT_RELATIVE_COLORIMETRIC,
                                               cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE);
  cmsCloseProfile(source);
  cmsCloseProfile(display);
  if (transform == nullptr) throw std::runtime_error("LittleCMS cannot convert to the monitor profile");

  lut.rgba.resize(static_cast<std::size_t>(edge) * edge * edge * 4);
  std::vector<float> row_in(static_cast<std::size_t>(edge) * 3), row_out(row_in.size());
  float* node = lut.rgba.data();
  for (int b = 0; b < edge; ++b) {
    for (int g = 0; g < edge; ++g) {
      for (int r = 0; r < edge; ++r) {
        row_in[r * 3]     = static_cast<float>(r) / static_cast<float>(edge - 1);
        row_in[r * 3 + 1] = static_cast<float>(g) / static_cast<float>(edge - 1);
        row_in[r * 3 + 2] = static_cast<float>(b) / static_cast<float>(edge - 1);
      }
      cmsDoTransform(transform, row_in.data(), row_out.data(), static_cast<cmsUInt32Number>(edge));
      for (int r = 0; r < edge; ++r) {
        for (int c = 0; c < 3; ++c) {
          const float v = row_out[r * 3 + c];
          *node++       = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        }
        *node++ = 1.0f;
      }
    }
  }
  cmsDeleteTransform(transform);
  return lut;
}

auto SampleLut(const DisplayLut& lut, float r, float g, float b) -> std::array<double, 3> {
  const int    n = lut.edge;
  const double x = std::clamp(r, 0.0f, 1.0f) * (n - 1);
  const double y = std::clamp(g, 0.0f, 1.0f) * (n - 1);
  const double z = std::clamp(b, 0.0f, 1.0f) * (n - 1);
  const int    x0 = std::min(static_cast<int>(x), n - 2), y0 = std::min(static_cast<int>(y), n - 2),
            z0 = std::min(static_cast<int>(z), n - 2);
  const double fx = x - x0, fy = y - y0, fz = z - z0;
  std::array<double, 3> out{};
  for (int c = 0; c < 3; ++c) {
    auto at = [&](int i, int j, int k) {
      return static_cast<double>(lut.rgba[((static_cast<std::size_t>(k) * n + j) * n + i) * 4 + c]);
    };
    const double c00 = at(x0, y0, z0) * (1 - fx) + at(x0 + 1, y0, z0) * fx;
    const double c10 = at(x0, y0 + 1, z0) * (1 - fx) + at(x0 + 1, y0 + 1, z0) * fx;
    const double c01 = at(x0, y0, z0 + 1) * (1 - fx) + at(x0 + 1, y0, z0 + 1) * fx;
    const double c11 = at(x0, y0 + 1, z0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1, z0 + 1) * fx;
    const double c0  = c00 * (1 - fy) + c10 * fy;
    const double c1  = c01 * (1 - fy) + c11 * fy;
    out[c]           = c0 * (1 - fz) + c1 * fz;
  }
  return out;
}

}  // namespace vx2
