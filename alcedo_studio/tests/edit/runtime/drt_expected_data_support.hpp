//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "edit/graph/drt_node_model.hpp"

namespace alcedo::drt_expected_data {

/**
 * @brief One row of the G10.5 DRT configuration matrix.
 *
 * The stored parameter files were packed through the pre-G10.5 `ODT_Op` path at commit
 * `0cf45f45`. Each backend stores one file per row, named
 * `<backend>_<file_stem>_expected_params.bin`.
 */
struct DrtConfiguration {
  std::string file_stem_;
  DrtPayload  payload_;
};

/**
 * @brief {ACES 2.0, OpenDRT standard look, OpenDRT Arriba look} x {Rec.709 Gamma 2.2,
 * P3-D65 Gamma 2.2, Rec.2020 PQ 1000 nit, Rec.2020 HLG 1000 nit}. The limiting space equals the
 * encoding space. The look applies only to OpenDRT rows.
 */
inline auto ConfigurationMatrix() -> std::vector<DrtConfiguration> {
  struct Output {
    const char*   name_;
    DrtColorSpace space_;
    DrtEotf       eotf_;
    float         peak_;
  };
  struct Method {
    const char* name_;
    DrtMethod   method_;
    const char* look_;
  };
  const Output outputs[] = {
      {"rec709_gamma22", DrtColorSpace::Rec709, DrtEotf::Gamma22, 100.0f},
      {"p3d65_gamma22", DrtColorSpace::P3D65, DrtEotf::Gamma22, 100.0f},
      {"rec2020_pq_1000nit", DrtColorSpace::Rec2020, DrtEotf::St2084, 1000.0f},
      {"rec2020_hlg_1000nit", DrtColorSpace::Rec2020, DrtEotf::Hlg, 1000.0f},
  };
  const Method methods[] = {
      {"aces20", DrtMethod::Aces20, "standard"},
      {"opendrt_standard", DrtMethod::OpenDrt, "standard"},
      {"opendrt_arriba", DrtMethod::OpenDrt, "arriba"},
  };
  std::vector<DrtConfiguration> rows;
  for (const auto& method : methods) {
    for (const auto& output : outputs) {
      DrtConfiguration row;
      row.file_stem_              = std::string(method.name_) + "_" + output.name_;
      row.payload_.method         = method.method_;
      row.payload_.encoding_space = output.space_;
      row.payload_.encoding_eotf  = output.eotf_;
      row.payload_.limiting_space = output.space_;
      row.payload_.peak_luminance = output.peak_;
      row.payload_.look_preset    = method.look_;
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

inline auto ExpectedParameterPath(const std::filesystem::path& directory, const char* backend,
                                  const DrtConfiguration& row) -> std::filesystem::path {
  return directory / (std::string(backend) + "_" + row.file_stem_ + "_expected_params.bin");
}

/** @brief Read a stored expected-data file. Returns an empty vector when it cannot be read. */
inline auto ReadBytes(const std::filesystem::path& path) -> std::vector<std::byte> {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  for (std::size_t i = 0; i < chars.size(); ++i) bytes[i] = static_cast<std::byte>(chars[i]);
  return bytes;
}

template <class T>
void AppendBytes(std::vector<std::byte>& out, const T& value) {
  const auto* first = reinterpret_cast<const std::byte*>(&value);
  out.insert(out.end(), first, first + sizeof(T));
}

/** @brief First differing byte offset, or `npos` when both spans are equal. */
inline auto FirstDifference(std::span<const std::byte> lhs, std::span<const std::byte> rhs)
    -> std::size_t {
  const std::size_t count = std::min(lhs.size(), rhs.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (lhs[i] != rhs[i]) return i;
  }
  return lhs.size() == rhs.size() ? std::string::npos : count;
}

}  // namespace alcedo::drt_expected_data
