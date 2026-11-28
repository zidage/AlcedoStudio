//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw_types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "decoders/dng_warp_rectilinear.hpp"

namespace alcedo::dng {
namespace detail {

enum class TiffByteOrder {
  LittleEndian,
  BigEndian,
};

inline auto ReadTiffU16(const std::span<const uint8_t> buffer, const size_t offset,
                        const TiffByteOrder order, uint16_t& out) -> bool {
  if (offset + 2 > buffer.size()) {
    return false;
  }
  if (order == TiffByteOrder::LittleEndian) {
    out = static_cast<uint16_t>(buffer[offset]) |
          (static_cast<uint16_t>(buffer[offset + 1]) << 8U);
  } else {
    out = (static_cast<uint16_t>(buffer[offset]) << 8U) |
          static_cast<uint16_t>(buffer[offset + 1]);
  }
  return true;
}

inline auto ReadTiffU32(const std::span<const uint8_t> buffer, const size_t offset,
                        const TiffByteOrder order, uint32_t& out) -> bool {
  if (offset + 4 > buffer.size()) {
    return false;
  }
  if (order == TiffByteOrder::LittleEndian) {
    out = static_cast<uint32_t>(buffer[offset]) |
          (static_cast<uint32_t>(buffer[offset + 1]) << 8U) |
          (static_cast<uint32_t>(buffer[offset + 2]) << 16U) |
          (static_cast<uint32_t>(buffer[offset + 3]) << 24U);
  } else {
    out = (static_cast<uint32_t>(buffer[offset]) << 24U) |
          (static_cast<uint32_t>(buffer[offset + 1]) << 16U) |
          (static_cast<uint32_t>(buffer[offset + 2]) << 8U) |
          static_cast<uint32_t>(buffer[offset + 3]);
  }
  return true;
}

inline auto ReadTiffUnsignedValues(const std::span<const uint8_t> buffer, const size_t entry_offset,
                                   const TiffByteOrder order, std::vector<uint32_t>& values)
    -> bool {
  uint16_t type  = 0;
  uint32_t count = 0;
  if (!ReadTiffU16(buffer, entry_offset + 2, order, type) ||
      !ReadTiffU32(buffer, entry_offset + 4, order, count)) {
    return false;
  }
  if ((type != 3 && type != 4) || count == 0 || count > 16) {
    return false;
  }

  const size_t value_size = (type == 3) ? sizeof(uint16_t) : sizeof(uint32_t);
  const size_t byte_size  = static_cast<size_t>(count) * value_size;
  uint32_t     data_offset_or_inline = 0;
  if (!ReadTiffU32(buffer, entry_offset + 8, order, data_offset_or_inline)) {
    return false;
  }

  const size_t data_offset =
      (byte_size <= 4) ? (entry_offset + 8) : static_cast<size_t>(data_offset_or_inline);
  if (data_offset + byte_size > buffer.size()) {
    return false;
  }

  values.clear();
  values.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (type == 3) {
      uint16_t value = 0;
      if (!ReadTiffU16(buffer, data_offset + static_cast<size_t>(i) * value_size, order, value)) {
        return false;
      }
      values.push_back(value);
    } else {
      uint32_t value = 0;
      if (!ReadTiffU32(buffer, data_offset + static_cast<size_t>(i) * value_size, order, value)) {
        return false;
      }
      values.push_back(value);
    }
  }
  return true;
}

inline auto ReadTiffByteValues(const std::span<const uint8_t> buffer, const size_t entry_offset,
                               const TiffByteOrder order, std::vector<uint8_t>& values) -> bool {
  uint16_t type  = 0;
  uint32_t count = 0;
  if (!ReadTiffU16(buffer, entry_offset + 2, order, type) ||
      !ReadTiffU32(buffer, entry_offset + 4, order, count)) {
    return false;
  }
  if ((type != 1 && type != 7) || count == 0 || count > (1U << 20U)) {
    return false;
  }

  uint32_t data_offset_or_inline = 0;
  if (!ReadTiffU32(buffer, entry_offset + 8, order, data_offset_or_inline)) {
    return false;
  }

  const size_t byte_size   = static_cast<size_t>(count);
  const size_t data_offset = (byte_size <= 4) ? (entry_offset + 8)
                                              : static_cast<size_t>(data_offset_or_inline);
  if (data_offset + byte_size > buffer.size()) {
    return false;
  }

  values.assign(buffer.begin() + static_cast<std::ptrdiff_t>(data_offset),
                buffer.begin() + static_cast<std::ptrdiff_t>(data_offset + byte_size));
  return true;
}

inline auto ClampToUshort(const uint32_t value) -> ushort {
  return static_cast<ushort>(std::min<uint32_t>(value, std::numeric_limits<ushort>::max()));
}

inline auto ReadBeU32(const std::span<const uint8_t> buffer, const size_t offset, uint32_t& out)
    -> bool {
  if (offset + 4 > buffer.size()) {
    return false;
  }
  out = (static_cast<uint32_t>(buffer[offset]) << 24U) |
        (static_cast<uint32_t>(buffer[offset + 1]) << 16U) |
        (static_cast<uint32_t>(buffer[offset + 2]) << 8U) |
        static_cast<uint32_t>(buffer[offset + 3]);
  return true;
}

inline auto ReadBeF64(const std::span<const uint8_t> buffer, const size_t offset, double& out)
    -> bool {
  if (offset + 8 > buffer.size()) {
    return false;
  }
  const uint64_t bits = (static_cast<uint64_t>(buffer[offset]) << 56U) |
                        (static_cast<uint64_t>(buffer[offset + 1]) << 48U) |
                        (static_cast<uint64_t>(buffer[offset + 2]) << 40U) |
                        (static_cast<uint64_t>(buffer[offset + 3]) << 32U) |
                        (static_cast<uint64_t>(buffer[offset + 4]) << 24U) |
                        (static_cast<uint64_t>(buffer[offset + 5]) << 16U) |
                        (static_cast<uint64_t>(buffer[offset + 6]) << 8U) |
                        static_cast<uint64_t>(buffer[offset + 7]);
  out = std::bit_cast<double>(bits);
  return true;
}

}  // namespace detail

struct Metadata {
  std::array<ushort, 4>            default_crop = {};
  std::optional<WarpRectilinear>  warp_rectilinear;
};

inline auto ParseOpcodeList3WarpRectilinear(const std::span<const uint8_t> opcodes)
    -> std::optional<WarpRectilinear> {
  uint32_t opcode_count = 0;
  if (!detail::ReadBeU32(opcodes, 0, opcode_count)) {
    return std::nullopt;
  }

  size_t offset = 4;
  for (uint32_t opcode_index = 0; opcode_index < opcode_count; ++opcode_index) {
    uint32_t opcode_id = 0;
    uint32_t version   = 0;
    uint32_t flags     = 0;
    uint32_t byte_size = 0;
    if (!detail::ReadBeU32(opcodes, offset, opcode_id) ||
        !detail::ReadBeU32(opcodes, offset + 4, version) ||
        !detail::ReadBeU32(opcodes, offset + 8, flags) ||
        !detail::ReadBeU32(opcodes, offset + 12, byte_size)) {
      return std::nullopt;
    }
    (void)version;
    (void)flags;
    offset += 16;
    if (offset + static_cast<size_t>(byte_size) > opcodes.size()) {
      return std::nullopt;
    }

    if (opcode_id == 1) {
      const auto payload = opcodes.subspan(offset, static_cast<size_t>(byte_size));
      uint32_t   coefficient_set_count = 0;
      if (!detail::ReadBeU32(payload, 0, coefficient_set_count) ||
          coefficient_set_count == 0 || coefficient_set_count > 3) {
        return std::nullopt;
      }
      const size_t expected_size =
          4 + static_cast<size_t>(coefficient_set_count) * 6 * sizeof(double) + 2 * sizeof(double);
      if (payload.size() < expected_size) {
        return std::nullopt;
      }

      WarpRectilinear warp{};
      warp.coefficient_set_count = coefficient_set_count;
      size_t payload_offset       = 4;
      for (uint32_t set = 0; set < coefficient_set_count; ++set) {
        for (size_t term = 0; term < warp.coefficient_sets[set].size(); ++term) {
          if (!detail::ReadBeF64(payload, payload_offset, warp.coefficient_sets[set][term])) {
            return std::nullopt;
          }
          payload_offset += sizeof(double);
        }
      }
      if (!detail::ReadBeF64(payload, payload_offset, warp.center_x) ||
          !detail::ReadBeF64(payload, payload_offset + sizeof(double), warp.center_y)) {
        return std::nullopt;
      }
      return warp;
    }

    offset += static_cast<size_t>(byte_size);
  }
  return std::nullopt;
}

inline auto ExtractMetadata(const std::span<const uint8_t> buffer) -> Metadata {
  Metadata metadata{};
  if (buffer.size() < 8) {
    return metadata;
  }

  detail::TiffByteOrder order{};
  if (buffer[0] == 'I' && buffer[1] == 'I') {
    order = detail::TiffByteOrder::LittleEndian;
  } else if (buffer[0] == 'M' && buffer[1] == 'M') {
    order = detail::TiffByteOrder::BigEndian;
  } else {
    return metadata;
  }

  uint16_t magic     = 0;
  uint32_t first_ifd = 0;
  if (!detail::ReadTiffU16(buffer, 2, order, magic) || magic != 42 ||
      !detail::ReadTiffU32(buffer, 4, order, first_ifd)) {
    return metadata;
  }

  constexpr uint16_t kTagSubIfds           = 330;
  constexpr uint16_t kTagDefaultCropOrigin = 50719;
  constexpr uint16_t kTagDefaultCropSize   = 50720;
  constexpr uint16_t kTagOpcodeList3       = 51022;

  std::array<uint32_t, 2> origin{};
  std::array<uint32_t, 2> size{};
  bool                    have_origin = false;
  bool                    have_size   = false;
  std::vector<uint8_t>    opcode_list3;
  std::unordered_set<uint32_t> visited_ifds;

  const auto visit_ifd = [&](const auto& self, const uint32_t ifd_offset, const int depth) -> void {
    if (depth > 8 || ifd_offset == 0 || visited_ifds.contains(ifd_offset) ||
        static_cast<size_t>(ifd_offset) + 2 > buffer.size()) {
      return;
    }
    visited_ifds.insert(ifd_offset);

    uint16_t entry_count = 0;
    if (!detail::ReadTiffU16(buffer, ifd_offset, order, entry_count)) {
      return;
    }

    const size_t entries_offset = static_cast<size_t>(ifd_offset) + 2;
    const size_t entries_size   = static_cast<size_t>(entry_count) * 12;
    if (entries_offset + entries_size + 4 > buffer.size()) {
      return;
    }

    std::vector<uint32_t> values;
    std::vector<uint32_t> sub_ifds;
    for (uint16_t i = 0; i < entry_count; ++i) {
      const size_t entry_offset = entries_offset + static_cast<size_t>(i) * 12;
      uint16_t     tag          = 0;
      if (!detail::ReadTiffU16(buffer, entry_offset, order, tag)) {
        continue;
      }
      if (tag != kTagSubIfds && tag != kTagDefaultCropOrigin &&
          tag != kTagDefaultCropSize && tag != kTagOpcodeList3) {
        continue;
      }
      if (tag == kTagOpcodeList3) {
        if (opcode_list3.empty()) {
          (void)detail::ReadTiffByteValues(buffer, entry_offset, order, opcode_list3);
        }
      } else if (!detail::ReadTiffUnsignedValues(buffer, entry_offset, order, values)) {
        continue;
      } else if (tag == kTagDefaultCropOrigin && values.size() >= 2) {
        origin      = {values[0], values[1]};
        have_origin = true;
      } else if (tag == kTagDefaultCropSize && values.size() >= 2) {
        size      = {values[0], values[1]};
        have_size = true;
      } else if (tag == kTagSubIfds) {
        sub_ifds = values;
      }
    }

    if (have_origin && have_size && !opcode_list3.empty()) {
      return;
    }
    for (const uint32_t sub_ifd : sub_ifds) {
      self(self, sub_ifd, depth + 1);
      if (have_origin && have_size && !opcode_list3.empty()) {
        return;
      }
    }

    uint32_t next_ifd = 0;
    if (detail::ReadTiffU32(buffer, entries_offset + entries_size, order, next_ifd)) {
      self(self, next_ifd, depth + 1);
    }
  };

  visit_ifd(visit_ifd, first_ifd, 0);
  if (have_origin && have_size) {
    metadata.default_crop[0] = detail::ClampToUshort(origin[0]);
    metadata.default_crop[1] = detail::ClampToUshort(origin[1]);
    metadata.default_crop[2] = detail::ClampToUshort(size[0]);
    metadata.default_crop[3] = detail::ClampToUshort(size[1]);
  }
  if (!opcode_list3.empty()) {
    metadata.warp_rectilinear = ParseOpcodeList3WarpRectilinear(opcode_list3);
  }
  return metadata;
}

inline auto ExtractDefaultCrop(const std::span<const uint8_t> buffer) -> std::array<ushort, 4> {
  return ExtractMetadata(buffer).default_crop;
}

}  // namespace alcedo::dng
