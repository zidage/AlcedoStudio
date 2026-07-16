//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "nn/safetensors.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include <json.hpp>

namespace alcedo::nn {
namespace {

constexpr std::uint64_t kMaxHeaderBytes = 1ull << 28;  // 256 MiB header cap
constexpr std::uint64_t kMaxDataBytes   = 1ull << 32;  // 4 GiB data cap (safety)

[[nodiscard]] auto FormatPath(const std::filesystem::path& path) -> std::string {
  return path.string();
}

[[nodiscard]] auto Fail(const std::string& msg) -> std::runtime_error {
  return std::runtime_error(msg);
}

[[nodiscard]] auto ReadExact(std::ifstream& in, void* dst, std::size_t nbytes,
                             const std::string& what) -> void {
  in.read(static_cast<char*>(dst), static_cast<std::streamsize>(nbytes));
  if (!in || static_cast<std::size_t>(in.gcount()) != nbytes) {
    throw Fail("safetensors: failed reading " + what);
  }
}

[[nodiscard]] auto ParseShape(const nlohmann::json& entry, const std::string& key)
    -> std::vector<std::int64_t> {
  if (!entry.contains("shape") || !entry["shape"].is_array()) {
    throw Fail("safetensors: tensor '" + key + "' missing shape array");
  }
  std::vector<std::int64_t> shape;
  shape.reserve(entry["shape"].size());
  for (const auto& dim : entry["shape"]) {
    if (!dim.is_number_integer()) {
      throw Fail("safetensors: tensor '" + key + "' has non-integer shape element");
    }
    const auto v = dim.get<std::int64_t>();
    if (v < 0) {
      throw Fail("safetensors: tensor '" + key + "' has negative shape element");
    }
    shape.push_back(v);
  }
  return shape;
}

[[nodiscard]] auto ParseOffsets(const nlohmann::json& entry, const std::string& key)
    -> std::pair<std::uint64_t, std::uint64_t> {
  if (!entry.contains("data_offsets") || !entry["data_offsets"].is_array() ||
      entry["data_offsets"].size() != 2) {
    throw Fail("safetensors: tensor '" + key + "' missing data_offsets [start, end]");
  }
  const auto& off = entry["data_offsets"];
  if (!off[0].is_number_integer() || !off[1].is_number_integer()) {
    throw Fail("safetensors: tensor '" + key + "' data_offsets must be integers");
  }
  const auto start = off[0].get<std::uint64_t>();
  const auto end   = off[1].get<std::uint64_t>();
  if (end < start) {
    throw Fail("safetensors: tensor '" + key + "' has inverted data_offsets");
  }
  return {start, end};
}

[[nodiscard]] auto ExpectedF32Bytes(const std::vector<std::int64_t>& shape) -> std::uint64_t {
  if (shape.empty()) {
    return 0;
  }
  std::uint64_t n = 1;
  for (const std::int64_t d : shape) {
    // Overflow-safe-ish multiply for reasonable weight tensors.
    if (d > 0 && n > (std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(d))) {
      throw Fail("safetensors: shape product overflow");
    }
    n *= static_cast<std::uint64_t>(d);
  }
  if (n > (std::numeric_limits<std::uint64_t>::max() / sizeof(float))) {
    throw Fail("safetensors: tensor byte size overflow");
  }
  return n * sizeof(float);
}

}  // namespace

auto LoadSafetensors(const std::filesystem::path& path) -> SafetensorsTensorMap {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw Fail("safetensors: cannot open file: " + FormatPath(path));
  }

  // Determine file size for bounds checks.
  in.seekg(0, std::ios::end);
  const auto file_size = static_cast<std::uint64_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  if (file_size < sizeof(std::uint64_t)) {
    throw Fail("safetensors: truncated file (no header_len): " + FormatPath(path));
  }

  std::uint64_t header_len = 0;
  ReadExact(in, &header_len, sizeof(header_len), "header_len");
  // Safetensors is little-endian; on LE hosts the raw read is correct. MSVC/x64 is LE.
  if (header_len == 0 || header_len > kMaxHeaderBytes) {
    throw Fail("safetensors: invalid header_len=" + std::to_string(header_len) + " in " +
               FormatPath(path));
  }
  if (file_size < sizeof(std::uint64_t) + header_len) {
    throw Fail("safetensors: truncated file (header incomplete): " + FormatPath(path));
  }

  std::string header_bytes(static_cast<std::size_t>(header_len), '\0');
  ReadExact(in, header_bytes.data(), static_cast<std::size_t>(header_len), "JSON header");

  nlohmann::json header;
  try {
    header = nlohmann::json::parse(header_bytes);
  } catch (const nlohmann::json::parse_error& e) {
    throw Fail(std::string("safetensors: invalid JSON header: ") + e.what());
  }
  if (!header.is_object()) {
    throw Fail("safetensors: header must be a JSON object");
  }

  SafetensorsTensorMap map;

  if (header.contains("__metadata__")) {
    const auto& meta = header["__metadata__"];
    if (!meta.is_object()) {
      throw Fail("safetensors: __metadata__ must be an object");
    }
    std::unordered_map<std::string, std::string> meta_map;
    for (auto it = meta.begin(); it != meta.end(); ++it) {
      if (!it.value().is_string()) {
        throw Fail("safetensors: __metadata__ value for '" + it.key() + "' is not a string");
      }
      meta_map.emplace(it.key(), it.value().get<std::string>());
    }
    map.SetMetadata(std::move(meta_map));
  }

  struct Pending {
    std::string               name;
    std::vector<std::int64_t> shape;
    std::uint64_t             off0 = 0;
    std::uint64_t             off1 = 0;
  };
  std::vector<Pending> pending;
  pending.reserve(header.size());

  std::uint64_t max_end = 0;
  for (auto it = header.begin(); it != header.end(); ++it) {
    if (it.key() == "__metadata__") {
      continue;
    }
    if (!it.value().is_object()) {
      throw Fail("safetensors: tensor entry '" + it.key() + "' must be an object");
    }
    const auto& entry = it.value();
    if (!entry.contains("dtype") || !entry["dtype"].is_string()) {
      throw Fail("safetensors: tensor '" + it.key() + "' missing dtype");
    }
    const auto dtype = entry["dtype"].get<std::string>();
    if (dtype != "F32") {
      throw Fail("safetensors: tensor '" + it.key() + "' has unsupported dtype '" + dtype +
                 "' (v1 requires F32)");
    }

    Pending p;
    p.name  = it.key();
    p.shape = ParseShape(entry, p.name);
    std::tie(p.off0, p.off1) = ParseOffsets(entry, p.name);

    const auto nbytes = p.off1 - p.off0;
    const auto expect = ExpectedF32Bytes(p.shape);
    if (nbytes != expect) {
      std::ostringstream oss;
      oss << "safetensors: tensor '" << p.name << "' byte size " << nbytes
          << " does not match F32 shape product " << expect;
      throw Fail(oss.str());
    }
    max_end = std::max(max_end, p.off1);
    pending.push_back(std::move(p));
  }

  const auto data_base = static_cast<std::uint64_t>(sizeof(std::uint64_t)) + header_len;
  if (max_end > kMaxDataBytes) {
    throw Fail("safetensors: data section exceeds safety cap in " + FormatPath(path));
  }
  if (file_size < data_base + max_end) {
    throw Fail("safetensors: truncated file (tensor data incomplete): " + FormatPath(path));
  }

  // Read the contiguous data region once, then slice into host vectors.
  std::vector<char> data_blob(static_cast<std::size_t>(max_end));
  if (max_end > 0) {
    in.seekg(static_cast<std::streamoff>(data_base));
    ReadExact(in, data_blob.data(), static_cast<std::size_t>(max_end), "tensor data");
  }

  for (auto& p : pending) {
    SafetensorsTensor tensor;
    tensor.name  = std::move(p.name);
    tensor.dtype = SafetensorsTensor::Dtype::F32;
    tensor.shape = std::move(p.shape);

    const auto nbytes = static_cast<std::size_t>(p.off1 - p.off0);
    const auto count  = nbytes / sizeof(float);
    tensor.data.resize(count);
    if (count > 0) {
      std::memcpy(tensor.data.data(), data_blob.data() + static_cast<std::size_t>(p.off0), nbytes);
    }
    map.Insert(std::move(tensor));
  }

  return map;
}

auto RequireF32Tensor(const SafetensorsTensorMap& map, std::string_view key,
                      std::initializer_list<std::int64_t> expected_shape)
    -> const SafetensorsTensor& {
  const auto* t = map.find(key);
  if (t == nullptr) {
    throw Fail("RequireF32Tensor: key not found: " + std::string(key));
  }
  if (t->dtype != SafetensorsTensor::Dtype::F32) {
    throw Fail("RequireF32Tensor: tensor '" + std::string(key) + "' is not F32");
  }
  if (!t->ShapeEquals(expected_shape)) {
    std::ostringstream oss;
    oss << "RequireF32Tensor: tensor '" << key << "' shape mismatch; got [";
    for (std::size_t i = 0; i < t->shape.size(); ++i) {
      if (i > 0) {
        oss << ", ";
      }
      oss << t->shape[i];
    }
    oss << "], expected [";
    std::size_t i = 0;
    for (const std::int64_t d : expected_shape) {
      if (i++ > 0) {
        oss << ", ";
      }
      oss << d;
    }
    oss << "]";
    throw Fail(oss.str());
  }
  if (t->data.size() != t->numel()) {
    throw Fail("RequireF32Tensor: tensor '" + std::string(key) +
               "' data length does not match shape product");
  }
  return *t;
}

}  // namespace alcedo::nn
