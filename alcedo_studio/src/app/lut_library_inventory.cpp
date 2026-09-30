//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_library_inventory.hpp"

#include <QFile>
#include <QSaveFile>
#include <QString>
#include <algorithm>
#include <json.hpp>
#include <system_error>
#include <utility>

#include "utils/lut/lut_inventory_digest.hpp"
#include "utils/lut/lut_metadata.hpp"

namespace alcedo {
namespace {

using Json                                  = nlohmann::json;

constexpr std::string_view kUserStateKind   = "alcedo-lut-library-state";
constexpr std::string_view kReceiptKind     = "alcedo-lut-package-receipt";
constexpr std::size_t      kMaxReceiptBytes = 1024 * 1024;

auto                       ToQString(const std::filesystem::path& path) -> QString {
  const std::u8string text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                                                 static_cast<qsizetype>(text.size()));
}

auto ReadSmallFile(const std::filesystem::path& path, std::size_t limit, std::string* bytes)
    -> std::string {
  QFile input(ToQString(path));
  if (!input.open(QIODevice::ReadOnly)) return input.errorString().toStdString();
  if (static_cast<std::size_t>(input.size()) > limit) return "file is too large";
  const QByteArray data = input.readAll();
  bytes->assign(data.constData(), static_cast<std::size_t>(data.size()));
  return {};
}

auto IsLowerHexSha256(std::string_view text) -> bool {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char value) {
           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
         });
}

auto ReadUnsigned(const Json& object, const char* key, std::uint64_t* output) -> bool {
  const auto found = object.find(key);
  if (found == object.end()) return true;
  if (!found->is_number_unsigned()) return false;
  *output = found->get<std::uint64_t>();
  return true;
}

auto ReadString(const Json& object, const char* key, std::string* output) -> bool {
  const auto found = object.find(key);
  if (found == object.end()) return true;
  if (!found->is_string()) return false;
  *output = found->get<std::string>();
  return true;
}

/// Descriptor fields are optional (receipts written before L3 lack them), but a
/// present field must be well formed.
auto ReadReceiptDescriptor(const Json& receipt, LutPackageReceipt* output) -> bool {
  const Json empty_artifact = Json::object();
  const auto artifact_found = receipt.find("artifact");
  if (artifact_found != receipt.end() && !artifact_found->is_object()) return false;
  const Json& artifact = artifact_found != receipt.end() ? *artifact_found : empty_artifact;
  if (!ReadString(receipt, "revision", &output->revision) ||
      !ReadUnsigned(receipt, "file_count", &output->file_count) ||
      !ReadString(receipt, "inventory_sha256", &output->inventory_sha256) ||
      !ReadUnsigned(receipt, "unpacked_bytes", &output->unpacked_bytes) ||
      !ReadUnsigned(receipt, "feed_sequence", &output->feed_sequence) ||
      !ReadString(artifact, "url", &output->artifact_url) ||
      !ReadUnsigned(artifact, "size", &output->artifact_size) ||
      !ReadString(artifact, "sha256", &output->artifact_sha256)) {
    return false;
  }
  return (output->inventory_sha256.empty() || IsLowerHexSha256(output->inventory_sha256)) &&
         (output->artifact_sha256.empty() || IsLowerHexSha256(output->artifact_sha256));
}

auto StringList(const Json& root, const char* key, std::vector<std::string>* output) -> bool {
  const auto found = root.find(key);
  if (found == root.end()) return true;
  if (!found->is_array()) return false;
  for (const Json& item : *found) {
    if (!item.is_string()) return false;
    output->push_back(item.get<std::string>());
  }
  return true;
}

auto HeaderDiffers(const LutLibraryEntry& left, const LutLibraryEntry& right) -> bool {
  if (left.size != right.size || left.sha256 != right.sha256 ||
      left.header_error != right.header_error || left.header_message != right.header_message ||
      left.managed_package_id != right.managed_package_id ||
      left.header.lut_3d_size != right.header.lut_3d_size ||
      left.header.lut_1d_size != right.header.lut_1d_size ||
      left.header.title != right.header.title ||
      left.header.metadata.has_value() != right.header.metadata.has_value()) {
    return true;
  }
  return left.header.metadata && SerializeLutMetadataJson(*left.header.metadata) !=
                                     SerializeLutMetadataJson(*right.header.metadata);
}

}  // namespace

auto LutPathToUtf8(const std::filesystem::path& path) -> std::string {
  const std::u8string text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

auto LutPathFromUtf8(std::string_view utf8) -> std::filesystem::path {
  const auto* begin = reinterpret_cast<const char8_t*>(utf8.data());
  return std::filesystem::path(std::u8string(begin, begin + utf8.size()));
}

auto IsValidLutLibraryEntryId(std::string_view entry_id) -> bool {
  constexpr std::string_view kOfficial = "official:";
  constexpr std::string_view kLibrary  = "library:";
  if (entry_id.starts_with(kLibrary)) {
    return IsSafeLutRelativePath(entry_id.substr(kLibrary.size()));
  }
  if (!entry_id.starts_with(kOfficial)) return false;
  const std::string_view ids       = entry_id.substr(kOfficial.size());
  const std::size_t      separator = ids.find('/');
  return separator != std::string_view::npos && separator > 0 && separator + 1 < ids.size() &&
         ids.find_first_of(std::string_view("\0\r\n", 3)) == std::string_view::npos;
}

auto SerializeLutLibraryUserState(const LutLibraryUserState& state) -> std::string {
  Json root = {{"schema", 1},
               {"kind", kUserStateKind},
               {"favorite_entries", state.favorite_entry_ids},
               {"previous_roots", state.previous_roots}};
  // Paths not yet converted to entry IDs keep their original key.
  if (!state.legacy_favorite_paths.empty()) root["favorites"] = state.legacy_favorite_paths;
  return root.dump(1);
}

auto ParseLutLibraryUserState(std::string_view json_bytes) -> LutLibraryUserStateReadResult {
  const Json root = Json::parse(json_bytes.begin(), json_bytes.end(), nullptr, false);
  if (root.is_discarded() || !root.is_object() || root.value("schema", 0) != 1 ||
      root.value("kind", std::string{}) != kUserStateKind) {
    return {std::nullopt, "LUT library state schema or kind is not supported"};
  }
  LutLibraryUserState state;
  if (!StringList(root, "favorite_entries", &state.favorite_entry_ids) ||
      !StringList(root, "favorites", &state.legacy_favorite_paths) ||
      !StringList(root, "previous_roots", &state.previous_roots) ||
      !std::all_of(state.favorite_entry_ids.begin(), state.favorite_entry_ids.end(),
                   [](const std::string& id) { return IsValidLutLibraryEntryId(id); }) ||
      !std::all_of(state.legacy_favorite_paths.begin(), state.legacy_favorite_paths.end(),
                   [](const std::string& path) { return IsSafeLutRelativePath(path); })) {
    return {std::nullopt, "LUT library state lists are invalid"};
  }
  for (std::vector<std::string>* list : {&state.favorite_entry_ids, &state.legacy_favorite_paths}) {
    std::sort(list->begin(), list->end());
    list->erase(std::unique(list->begin(), list->end()), list->end());
  }
  return {std::move(state), {}};
}

auto ReadLutLibraryUserStateFile(const std::filesystem::path& root)
    -> LutLibraryUserStateReadResult {
  const std::filesystem::path path = root / LutPathFromUtf8(kLutLibraryStateFileName);
  std::error_code             error;
  if (!std::filesystem::exists(path, error) && !error) return {LutLibraryUserState{}, {}};
  std::string bytes;
  if (std::string read_error = ReadSmallFile(path, 16 * 1024 * 1024, &bytes); !read_error.empty()) {
    return {std::nullopt, "LUT library state cannot be read: " + read_error};
  }
  return ParseLutLibraryUserState(bytes);
}

auto WriteLutLibraryUserStateFile(const std::filesystem::path& root,
                                  const LutLibraryUserState&   state) -> std::string {
  const std::string bytes = SerializeLutLibraryUserState(state);
  QSaveFile         output(ToQString(root / LutPathFromUtf8(kLutLibraryStateFileName)));
  if (!output.open(QIODevice::WriteOnly)) return output.errorString().toStdString();
  if (output.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
      static_cast<qint64>(bytes.size())) {
    std::string error = output.errorString().toStdString();
    output.cancelWriting();
    return error;
  }
  if (!output.commit()) return output.errorString().toStdString();
  return {};
}

void ReadLutPackageReceipts(const std::filesystem::path&    root,
                            std::vector<LutPackageReceipt>* receipts,
                            std::vector<LutScanDiagnostic>* diagnostics) {
  const std::filesystem::path packages = root / LutPathFromUtf8(kLutPackagesDirectoryName);
  std::error_code             error;
  if (!std::filesystem::is_directory(packages, error)) return;
  std::filesystem::directory_iterator iterator(packages, error);
  for (; !error && iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
    std::error_code status_error;
    if (!iterator->is_directory(status_error) || iterator->is_symlink(status_error)) continue;
    const std::string package_id   = LutPathToUtf8(iterator->path().filename());
    const std::string receipt_path = std::string(kLutPackagesDirectoryName) + "/" + package_id +
                                     "/" + std::string(kLutPackageReceiptFileName);
    const std::filesystem::path receipt_file = root / LutPathFromUtf8(receipt_path);
    if (!std::filesystem::exists(receipt_file, status_error)) continue;
    const auto fail = [&](std::string message) {
      diagnostics->push_back(
          {LutScanDiagnosticKind::kInvalidPackageReceipt, receipt_path, std::move(message)});
    };
    std::string bytes;
    if (std::string read_error = ReadSmallFile(receipt_file, kMaxReceiptBytes, &bytes);
        !read_error.empty()) {
      fail(read_error);
      continue;
    }
    const Json receipt = Json::parse(bytes, nullptr, false);
    if (receipt.is_discarded() || !receipt.is_object() || receipt.value("schema", 0) != 1 ||
        receipt.value("kind", std::string{}) != kReceiptKind) {
      fail("package receipt schema or kind is not supported");
      continue;
    }
    const std::string declared_id = receipt.value("package_id", std::string{});
    const std::string content     = receipt.value("content_directory", std::string{});
    const std::string prefix =
        std::string(kLutPackagesDirectoryName) + "/" + package_id + "/content/";
    if (declared_id != package_id || !IsLutPackageId(package_id) ||
        !IsSafeLutRelativePath(content) || !content.starts_with(prefix) ||
        content.find('/', prefix.size()) != std::string::npos) {
      fail("package receipt names an invalid package or content directory");
      continue;
    }
    LutPackageReceipt parsed{.package_id = package_id, .content_directory = content};
    if (!ReadReceiptDescriptor(receipt, &parsed)) {
      fail("package receipt descriptor fields are invalid");
      continue;
    }
    receipts->push_back(std::move(parsed));
  }
  if (error) {
    diagnostics->push_back({LutScanDiagnosticKind::kUnreadableDirectory,
                            std::string(kLutPackagesDirectoryName), error.message()});
  }
  std::sort(receipts->begin(), receipts->end(),
            [](const LutPackageReceipt& left, const LutPackageReceipt& right) {
              return left.package_id < right.package_id;
            });
}

auto IsLutPackageId(std::string_view id) -> bool {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' ||
           value == '-';
  });
}

auto SerializeLutPackageReceipt(const LutPackageReceipt& receipt) -> std::string {
  const Json root = {{"schema", 1},
                     {"kind", kReceiptKind},
                     {"package_id", receipt.package_id},
                     {"content_directory", receipt.content_directory},
                     {"revision", receipt.revision},
                     {"file_count", receipt.file_count},
                     {"inventory_sha256", receipt.inventory_sha256},
                     {"unpacked_bytes", receipt.unpacked_bytes},
                     {"feed_sequence", receipt.feed_sequence},
                     {"artifact",
                      {{"url", receipt.artifact_url},
                       {"size", receipt.artifact_size},
                       {"sha256", receipt.artifact_sha256}}}};
  return root.dump(1);
}

auto WriteLutPackageReceiptFile(const std::filesystem::path& root, const LutPackageReceipt& receipt)
    -> std::string {
  if (!IsLutPackageId(receipt.package_id)) return "the package ID is not valid";
  const std::string bytes = SerializeLutPackageReceipt(receipt);
  QSaveFile         output(ToQString(root / LutPathFromUtf8(kLutPackagesDirectoryName) /
                                     LutPathFromUtf8(receipt.package_id) /
                                     LutPathFromUtf8(kLutPackageReceiptFileName)));
  if (!output.open(QIODevice::WriteOnly)) return output.errorString().toStdString();
  if (output.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
      static_cast<qint64>(bytes.size())) {
    std::string error = output.errorString().toStdString();
    output.cancelWriting();
    return error;
  }
  if (!output.commit()) return output.errorString().toStdString();
  return {};
}

auto LutInventoryMatchesPackageReceipts(const LutLibraryInventory&            inventory,
                                        const std::vector<LutPackageReceipt>& receipts) -> bool {
  std::vector<bool> receipt_has_entry(receipts.size(), false);
  for (const LutLibraryEntry& entry : inventory.entries) {
    if (entry.managed_package_id.empty()) continue;
    const auto found =
        std::find_if(receipts.begin(), receipts.end(), [&](const LutPackageReceipt& receipt) {
          return receipt.package_id == entry.managed_package_id;
        });
    if (found == receipts.end() ||
        !entry.relative_path.starts_with(found->content_directory + "/")) {
      return false;
    }
    receipt_has_entry[static_cast<std::size_t>(found - receipts.begin())] = true;
  }
  for (std::size_t index = 0; index < receipts.size(); ++index) {
    if (receipts[index].file_count > 0 && !receipt_has_entry[index]) return false;
  }
  return true;
}

auto ScanLutLibraryRoot(const std::filesystem::path& root, unsigned worker_count)
    -> LutLibraryInventory {
  std::vector<LutPackageReceipt> receipts;
  std::vector<LutScanDiagnostic> receipt_diagnostics;
  ReadLutPackageReceipts(root, &receipts, &receipt_diagnostics);

  LutLibraryScanOptions options;
  options.worker_count                  = worker_count;
  options.excluded_directory_names      = {std::string(kLutDownloadsDirectoryName)};
  options.excluded_relative_directories = {std::string(kLutPackagesDirectoryName)};
  for (const LutPackageReceipt& receipt : receipts) {
    options.additional_relative_directories.push_back(receipt.content_directory);
  }
  LutLibraryInventory inventory = ScanLutLibrary(root, options);
  for (LutLibraryEntry& entry : inventory.entries) {
    for (const LutPackageReceipt& receipt : receipts) {
      if (entry.relative_path.starts_with(receipt.content_directory + "/")) {
        entry.managed_package_id = receipt.package_id;
        break;
      }
    }
  }
  inventory.diagnostics.insert(inventory.diagnostics.end(), receipt_diagnostics.begin(),
                               receipt_diagnostics.end());
  std::sort(inventory.diagnostics.begin(), inventory.diagnostics.end(),
            [](const LutScanDiagnostic& left, const LutScanDiagnostic& right) {
              return left.relative_path < right.relative_path;
            });
  return inventory;
}

auto FindLutLibraryEntry(const LutLibraryInventory& inventory, std::string_view relative_path)
    -> const LutLibraryEntry* {
  const auto found =
      std::lower_bound(inventory.entries.begin(), inventory.entries.end(), relative_path,
                       [](const LutLibraryEntry& entry, std::string_view path) {
                         return entry.relative_path < path;
                       });
  if (found == inventory.entries.end() || found->relative_path != relative_path) return nullptr;
  return &*found;
}

auto ChangedLutLibraryEntryPaths(const LutLibraryInventory& before,
                                 const LutLibraryInventory& after) -> std::vector<std::string> {
  std::vector<std::string> changed;
  auto                     left  = before.entries.begin();
  auto                     right = after.entries.begin();
  while (left != before.entries.end() || right != after.entries.end()) {
    if (right == after.entries.end() ||
        (left != before.entries.end() && left->relative_path < right->relative_path)) {
      changed.push_back((left++)->relative_path);
    } else if (left == before.entries.end() || right->relative_path < left->relative_path) {
      changed.push_back((right++)->relative_path);
    } else {
      if (HeaderDiffers(*left, *right)) changed.push_back(right->relative_path);
      ++left;
      ++right;
    }
  }
  return changed;
}

}  // namespace alcedo
