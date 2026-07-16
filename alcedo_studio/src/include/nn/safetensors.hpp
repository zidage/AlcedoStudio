//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace alcedo::nn {

// Host-only safetensors DTO. File format → named host tensors only.
// Backend-neutral: no CUDA / OpenCL dependencies.

struct SafetensorsTensor {
  enum class Dtype {
    F32,
  };

  std::string               name;
  Dtype                     dtype = Dtype::F32;
  std::vector<std::int64_t> shape;
  std::vector<float>        data;  // contiguous host payload (F32)

  [[nodiscard]] auto numel() const -> std::size_t {
    if (shape.empty()) {
      return 0;
    }
    std::size_t n = 1;
    for (const std::int64_t d : shape) {
      if (d < 0) {
        throw std::runtime_error("SafetensorsTensor: negative shape dimension in " + name);
      }
      n *= static_cast<std::size_t>(d);
    }
    return n;
  }

  [[nodiscard]] auto ShapeEquals(std::initializer_list<std::int64_t> expected) const -> bool {
    if (shape.size() != expected.size()) {
      return false;
    }
    auto it = expected.begin();
    for (const std::int64_t d : shape) {
      if (d != *it) {
        return false;
      }
      ++it;
    }
    return true;
  }

  [[nodiscard]] auto ShapeEquals(const std::vector<std::int64_t>& expected) const -> bool {
    return shape == expected;
  }
};

// Unified host DTO handed to hard-coded modules via LoadWeights later.
class SafetensorsTensorMap {
 public:
  using MapType        = std::unordered_map<std::string, SafetensorsTensor>;
  using iterator       = MapType::iterator;
  using const_iterator = MapType::const_iterator;

  SafetensorsTensorMap() = default;

  [[nodiscard]] auto empty() const -> bool { return tensors_.empty(); }
  [[nodiscard]] auto size() const -> std::size_t { return tensors_.size(); }

  [[nodiscard]] auto contains(std::string_view key) const -> bool {
    return tensors_.find(std::string(key)) != tensors_.end();
  }

  [[nodiscard]] auto at(std::string_view key) const -> const SafetensorsTensor& {
    const auto it = tensors_.find(std::string(key));
    if (it == tensors_.end()) {
      throw std::runtime_error("SafetensorsTensorMap: key not found: " + std::string(key));
    }
    return it->second;
  }

  [[nodiscard]] auto find(std::string_view key) const -> const SafetensorsTensor* {
    const auto it = tensors_.find(std::string(key));
    if (it == tensors_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  // Optional file-level metadata (__metadata__ object), e.g. format / variant.
  [[nodiscard]] auto metadata() const -> const std::unordered_map<std::string, std::string>& {
    return metadata_;
  }

  [[nodiscard]] auto metadata(std::string_view key) const -> std::string_view {
    const auto it = metadata_.find(std::string(key));
    if (it == metadata_.end()) {
      return {};
    }
    return it->second;
  }

  [[nodiscard]] auto begin() const -> const_iterator { return tensors_.begin(); }
  [[nodiscard]] auto end() const -> const_iterator { return tensors_.end(); }
  [[nodiscard]] auto begin() -> iterator { return tensors_.begin(); }
  [[nodiscard]] auto end() -> iterator { return tensors_.end(); }

  // Construction helpers used by the parser (and tests).
  // Rejects duplicate tensor names so a partially corrupted file cannot silently
  // overwrite an earlier entry.
  void Insert(SafetensorsTensor tensor) {
    auto name = tensor.name;
    const auto [it, inserted] = tensors_.try_emplace(std::move(name), std::move(tensor));
    if (!inserted) {
      throw std::runtime_error("SafetensorsTensorMap: duplicate tensor name: " + it->first);
    }
  }

  // Intentional replacement for mutation tests / tooling. Not used by the parser.
  void InsertOrAssign(SafetensorsTensor tensor) {
    auto name = tensor.name;
    tensors_.insert_or_assign(std::move(name), std::move(tensor));
  }

  void SetMetadata(std::unordered_map<std::string, std::string> meta) {
    metadata_ = std::move(meta);
  }

 private:
  MapType                                     tensors_;
  std::unordered_map<std::string, std::string> metadata_;
};

// Load a .safetensors file into a host-only tensor map.
// v1: F32 tensors only; any other dtype is a hard error.
[[nodiscard]] auto LoadSafetensors(const std::filesystem::path& path) -> SafetensorsTensorMap;

// Shape-compare helper for CRTP loaders / table-driven tests.
[[nodiscard]] inline auto ShapesEqual(const std::vector<std::int64_t>& actual,
                                      std::initializer_list<std::int64_t> expected) -> bool {
  if (actual.size() != expected.size()) {
    return false;
  }
  auto it = expected.begin();
  for (const std::int64_t d : actual) {
    if (d != *it) {
      return false;
    }
    ++it;
  }
  return true;
}

// Lookup + F32 + exact-shape check. Throws std::runtime_error on failure.
[[nodiscard]] auto RequireF32Tensor(const SafetensorsTensorMap& map, std::string_view key,
                                    std::initializer_list<std::int64_t> expected_shape)
    -> const SafetensorsTensor&;

}  // namespace alcedo::nn
