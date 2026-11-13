//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/lut/lut_inventory_digest.hpp"

#include <QCryptographicHash>
#include <algorithm>
#include <fstream>
#include <json.hpp>
#include <set>
#include <tuple>
#include <utility>

namespace alcedo {
namespace {

using Json                                = nlohmann::json;

constexpr std::string_view kInventoryKind = "alcedo-lut-package-inventory";

auto                       IsLowerHexDigest(std::string_view text) -> bool {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char value) {
           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
         });
}

auto HasLineBreakOrNul(std::string_view text) -> bool {
  return text.find_first_of(std::string_view("\0\n\r", 3)) != std::string_view::npos;
}

auto AsciiFolded(std::string_view text) -> std::string {
  std::string folded(text);
  std::transform(folded.begin(), folded.end(), folded.begin(), [](char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
  });
  return folded;
}

auto ReadUnsigned(const Json& object, const char* key, std::uint64_t* output) -> bool {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_number_unsigned()) return false;
  *output = found->get<std::uint64_t>();
  return true;
}

auto ReadString(const Json& object, const char* key, std::string* output) -> bool {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_string()) return false;
  *output = found->get<std::string>();
  return !output->empty();
}

auto ParseFailure(std::string message) -> LutPackageInventoryParseResult {
  return LutPackageInventoryParseResult{std::nullopt, std::move(message)};
}

}  // namespace

auto IsSafeLutRelativePath(std::string_view path) -> bool {
  if (path.empty() || HasLineBreakOrNul(path) || path.find('\\') != std::string_view::npos ||
      path.front() == '/' || path.find(':') != std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    std::size_t end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    const std::string_view segment = path.substr(start, end - start);
    if (segment.empty() || segment == "." || segment == "..") return false;
    start = end + 1;
  }
  return true;
}

auto ComputeLutInventoryDigest(std::vector<LutInventoryRecord> records)
    -> LutInventoryDigestResult {
  std::set<std::string> ids;
  std::set<std::string> folded_paths;
  for (const LutInventoryRecord& record : records) {
    if (record.id.empty() || HasLineBreakOrNul(record.id)) {
      return {{}, "LUT ID is empty or contains NUL or a line break: " + record.id};
    }
    if (!IsSafeLutRelativePath(record.relative_path)) {
      return {{}, "LUT path is not a safe relative path: " + record.relative_path};
    }
    if (!IsLowerHexDigest(record.sha256)) {
      return {{},
              "LUT SHA-256 is not 64 lowercase hexadecimal characters: " + record.relative_path};
    }
    if (!ids.insert(record.id).second) {
      return {{}, "duplicate LUT ID: " + record.id};
    }
    if (!folded_paths.insert(AsciiFolded(record.relative_path)).second) {
      return {{}, "LUT paths collide on a case-insensitive file system: " + record.relative_path};
    }
  }

  std::sort(records.begin(), records.end(),
            [](const LutInventoryRecord& left, const LutInventoryRecord& right) {
              return std::tie(left.relative_path, left.id) <
                     std::tie(right.relative_path, right.id);
            });
  QCryptographicHash hash(QCryptographicHash::Sha256);
  for (const LutInventoryRecord& record : records) {
    const std::string size = std::to_string(record.size);
    hash.addData(QByteArrayView(record.id.data(), static_cast<qsizetype>(record.id.size())));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArrayView(record.relative_path.data(),
                                static_cast<qsizetype>(record.relative_path.size())));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArrayView(size.data(), static_cast<qsizetype>(size.size())));
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArrayView(record.sha256.data(), 64));
    hash.addData(QByteArrayView("\n", 1));
  }
  return {hash.result().toHex().toStdString(), {}};
}

auto HashLutFile(const std::filesystem::path& path) -> std::optional<LutFileHash> {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  std::vector<char>  block(1024 * 1024);
  std::uint64_t      size = 0;
  while (input) {
    input.read(block.data(), static_cast<std::streamsize>(block.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      hash.addData(QByteArrayView(block.data(), static_cast<qsizetype>(count)));
      size += static_cast<std::uint64_t>(count);
    }
  }
  if (input.bad() || !input.eof()) return std::nullopt;
  return LutFileHash{hash.result().toHex().toStdString(), size};
}

auto ParseLutPackageInventory(std::string_view json_bytes) -> LutPackageInventoryParseResult {
  const Json root = Json::parse(json_bytes.begin(), json_bytes.end(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return ParseFailure("package inventory is not a JSON object");
  }
  const auto schema = root.find("schema");
  const auto kind   = root.find("kind");
  if (schema == root.end() || !schema->is_number_integer() || schema->get<long long>() != 1 ||
      kind == root.end() || *kind != std::string(kInventoryKind)) {
    return ParseFailure("package inventory schema or kind is not supported");
  }

  LutPackageInventory inventory;
  if (!ReadString(root, "package_id", &inventory.package_id) ||
      !ReadString(root, "revision", &inventory.revision) ||
      !ReadUnsigned(root, "file_count", &inventory.file_count) ||
      !ReadString(root, "inventory_sha256", &inventory.inventory_sha256) ||
      !ReadUnsigned(root, "unpacked_bytes", &inventory.unpacked_bytes)) {
    return ParseFailure("package inventory header fields are missing or invalid");
  }

  const auto luts      = root.find("luts");
  const auto auxiliary = root.find("auxiliary_files");
  if (luts == root.end() || !luts->is_array() || auxiliary == root.end() ||
      !auxiliary->is_array()) {
    return ParseFailure("package inventory file lists are missing");
  }
  std::uint64_t         total_bytes = 0;
  std::set<std::string> folded_paths;
  for (const Json& item : *luts) {
    LutInventoryRecord record;
    if (!item.is_object() || !ReadString(item, "id", &record.id) ||
        !ReadString(item, "path", &record.relative_path) ||
        !ReadUnsigned(item, "size", &record.size) || !ReadString(item, "sha256", &record.sha256)) {
      return ParseFailure("package inventory has an invalid LUT record");
    }
    folded_paths.insert(AsciiFolded(record.relative_path));
    total_bytes += record.size;
    inventory.luts.push_back(std::move(record));
  }
  for (const Json& item : *auxiliary) {
    LutAuxiliaryFileRecord record;
    if (!item.is_object() || !ReadString(item, "path", &record.relative_path) ||
        !ReadUnsigned(item, "size", &record.size) || !ReadString(item, "sha256", &record.sha256) ||
        !IsSafeLutRelativePath(record.relative_path) || !IsLowerHexDigest(record.sha256)) {
      return ParseFailure("package inventory has an invalid auxiliary file record");
    }
    if (!folded_paths.insert(AsciiFolded(record.relative_path)).second) {
      return ParseFailure("package inventory paths collide: " + record.relative_path);
    }
    total_bytes += record.size;
    inventory.auxiliary_files.push_back(std::move(record));
  }

  const LutInventoryDigestResult digest = ComputeLutInventoryDigest(inventory.luts);
  if (!digest.Ok()) {
    return ParseFailure(digest.error);
  }
  if (inventory.file_count != inventory.luts.size()) {
    return ParseFailure("package inventory file count does not match its LUT records");
  }
  if (inventory.unpacked_bytes != total_bytes) {
    return ParseFailure("package inventory byte total does not match its records");
  }
  if (digest.sha256 != inventory.inventory_sha256) {
    return ParseFailure("package inventory digest does not match its LUT records");
  }
  return LutPackageInventoryParseResult{std::move(inventory), {}};
}

}  // namespace alcedo
