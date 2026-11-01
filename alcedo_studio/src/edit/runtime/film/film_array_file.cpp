//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_array_file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace alcedo {
namespace {

constexpr std::array<char, 8> kMagic{'A', 'L', 'C', 'D', 'F', 'I', 'L', 'M'};
constexpr std::size_t         kArrayAlignment = 8;

[[noreturn]] void             Fail(std::string_view path_text, std::string_view reason) {
  throw std::runtime_error("Film print resource '" + std::string(path_text) +
                                       "': " + std::string(reason));
}

auto ParseType(const std::string& text, std::string_view path_text) -> FilmArrayType {
  if (text == "f8") return FilmArrayType::kF64;
  if (text == "f4") return FilmArrayType::kF32;
  if (text == "i4") return FilmArrayType::kI32;
  if (text == "i8") return FilmArrayType::kI64;
  if (text == "u1") return FilmArrayType::kU8;
  Fail(path_text, "unknown array type '" + text + "'");
}

auto ElementBytes(FilmArrayType type) -> std::size_t {
  switch (type) {
    case FilmArrayType::kF64:
    case FilmArrayType::kI64:
      return 8;
    case FilmArrayType::kF32:
    case FilmArrayType::kI32:
      return 4;
    case FilmArrayType::kU8:
      return 1;
  }
  return 0;
}

auto ShapeText(std::span<const std::int64_t> shape) -> std::string {
  std::string text = "(";
  for (std::size_t axis = 0; axis < shape.size(); ++axis) {
    text += (axis == 0 ? "" : ", ") + std::to_string(shape[axis]);
  }
  return text + ")";
}

}  // namespace

auto FilmPathText(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

auto ReadFilmResourceBytes(const std::filesystem::path& path) -> std::vector<std::byte> {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    Fail(FilmPathText(path), "the file cannot be opened");
  }
  const auto size = static_cast<std::streamoff>(stream.tellg());
  if (size < 0) {
    Fail(FilmPathText(path), "the size of the file cannot be read");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!stream) {
    Fail(FilmPathText(path), "the file cannot be read");
  }
  return bytes;
}

auto FilmArrayFile::Parse(std::vector<std::byte> bytes, const std::filesystem::path& path)
    -> FilmArrayFile {
  FilmArrayFile file;
  file.path_text_                 = FilmPathText(path);
  const std::string&    path_text = file.path_text_;
  constexpr std::size_t kPrefix   = kMagic.size() + sizeof(std::uint64_t);
  if (bytes.size() < kPrefix || std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    Fail(path_text, "the file has no array file header");
  }
  std::uint64_t header_length = 0;
  std::memcpy(&header_length, bytes.data() + kMagic.size(), sizeof(header_length));
  if (header_length > bytes.size() - kPrefix) {
    Fail(path_text, "the header length is larger than the file");
  }
  const auto  data_start = kPrefix + static_cast<std::size_t>(header_length);
  const auto* header     = reinterpret_cast<const char*>(bytes.data()) + kPrefix;
  try {
    const auto json = nlohmann::json::parse(header, header + header_length);
    if (json.at("schema_version").get<int>() != kFilmResourceSchemaVersion) {
      Fail(path_text, "schema version " + json.at("schema_version").dump() + " is not version " +
                          std::to_string(kFilmResourceSchemaVersion));
    }
    file.metadata_ = json.at("metadata");
    for (const auto& [name, description] : json.at("arrays").items()) {
      ArrayEntry entry;
      entry.type       = ParseType(description.at("type").get<std::string>(), path_text);
      entry.shape      = description.at("shape").get<std::vector<std::int64_t>>();
      entry.byte_count = ElementBytes(entry.type);
      for (const auto extent : entry.shape) {
        constexpr auto kLargest = (std::numeric_limits<std::size_t>::max)();
        if (extent < 0 ||
            (extent > 0 && entry.byte_count > kLargest / static_cast<std::size_t>(extent))) {
          Fail(path_text, "array '" + name + "' has an invalid shape");
        }
        entry.byte_count *= static_cast<std::size_t>(extent);
      }
      const auto offset   = description.at("offset").get<std::uint64_t>();
      const auto capacity = bytes.size() - data_start;
      if (offset > capacity || entry.byte_count > capacity - offset) {
        Fail(path_text, "array '" + name + "' ends after the end of the file");
      }
      entry.first_byte = data_start + static_cast<std::size_t>(offset);
      if (entry.first_byte % kArrayAlignment != 0) {
        Fail(path_text, "array '" + name + "' is not on an 8 byte boundary");
      }
      file.arrays_.emplace(name, std::move(entry));
    }
  } catch (const nlohmann::json::exception& error) {
    Fail(path_text, std::string("the header is not valid: ") + error.what());
  }
  file.bytes_ = std::move(bytes);
  return file;
}

auto FilmArrayFile::Find(std::string_view name) const -> const ArrayEntry& {
  const auto entry = arrays_.find(name);
  if (entry == arrays_.end()) {
    Fail(path_text_, "array '" + std::string(name) + "' is missing");
  }
  return entry->second;
}

auto FilmArrayFile::Checked(std::string_view name, FilmArrayType type, ShapeList shape) const
    -> const ArrayEntry& {
  const auto& entry = Find(name);
  if (entry.type != type) {
    Fail(path_text_, "array '" + std::string(name) + "' has another element type");
  }
  if (!std::equal(entry.shape.begin(), entry.shape.end(), shape.begin(), shape.end())) {
    Fail(path_text_, "array '" + std::string(name) + "' has shape " + ShapeText(entry.shape) +
                         ", not " + ShapeText(std::span(shape.begin(), shape.size())));
  }
  return entry;
}

auto FilmArrayFile::Shape(std::string_view name) const -> std::span<const std::int64_t> {
  return Find(name).shape;
}

auto FilmArrayFile::F64(std::string_view name, ShapeList shape) const -> std::span<const double> {
  const auto& entry = Checked(name, FilmArrayType::kF64, shape);
  return {reinterpret_cast<const double*>(bytes_.data() + entry.first_byte),
          entry.byte_count / sizeof(double)};
}

auto FilmArrayFile::F32(std::string_view name, ShapeList shape) const -> std::span<const float> {
  const auto& entry = Checked(name, FilmArrayType::kF32, shape);
  return {reinterpret_cast<const float*>(bytes_.data() + entry.first_byte),
          entry.byte_count / sizeof(float)};
}

auto FilmArrayFile::I32(std::string_view name, ShapeList shape) const
    -> std::span<const std::int32_t> {
  const auto& entry = Checked(name, FilmArrayType::kI32, shape);
  return {reinterpret_cast<const std::int32_t*>(bytes_.data() + entry.first_byte),
          entry.byte_count / sizeof(std::int32_t)};
}

auto FilmArrayFile::I64(std::string_view name, ShapeList shape) const
    -> std::span<const std::int64_t> {
  const auto& entry = Checked(name, FilmArrayType::kI64, shape);
  return {reinterpret_cast<const std::int64_t*>(bytes_.data() + entry.first_byte),
          entry.byte_count / sizeof(std::int64_t)};
}

}  // namespace alcedo
