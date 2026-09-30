//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/lut/lut_library_scan.hpp"

#include <QFile>
#include <QSaveFile>
#include <QString>
#include <algorithm>
#include <array>
#include <atomic>
#include <json.hpp>
#include <thread>
#include <utility>

#include "utils/lut/lut_inventory_digest.hpp"

namespace alcedo {
namespace {

using Json                                 = nlohmann::json;

constexpr std::string_view kInventoryKind  = "alcedo-lut-library-inventory";
constexpr unsigned         kMaxScanWorkers = 8;

struct HeaderErrorName {
  LutHeaderError   error;
  std::string_view name;
};

constexpr std::array<HeaderErrorName, 10> kHeaderErrorNames{{
    {LutHeaderError::kNone, "none"},
    {LutHeaderError::kUnreadable, "unreadable"},
    {LutHeaderError::kHeaderTooLarge, "header_too_large"},
    {LutHeaderError::kInvalidDirective, "invalid_directive"},
    {LutHeaderError::kNoNumericTable, "no_numeric_table"},
    {LutHeaderError::kDuplicateMetadata, "duplicate_metadata"},
    {LutHeaderError::kMetadataLineTooLarge, "metadata_line_too_large"},
    {LutHeaderError::kInvalidMetadataJson, "invalid_metadata_json"},
    {LutHeaderError::kUnsupportedSchema, "unsupported_schema"},
    {LutHeaderError::kInvalidMetadataField, "invalid_metadata_field"},
}};

constexpr std::array<std::string_view, 3> kDiagnosticNames{"skipped_link", "unreadable_directory",
                                                           "unreadable_file"};

auto HeaderErrorToName(LutHeaderError error) -> std::string_view {
  for (const HeaderErrorName& item : kHeaderErrorNames) {
    if (item.error == error) return item.name;
  }
  return "unreadable";
}

auto HeaderErrorFromName(std::string_view name) -> std::optional<LutHeaderError> {
  for (const HeaderErrorName& item : kHeaderErrorNames) {
    if (item.name == name) return item.error;
  }
  return std::nullopt;
}

auto ToUtf8(const std::filesystem::path& path) -> std::string {
  const std::u8string text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

auto ToQString(const std::filesystem::path& path) -> QString {
  const std::u8string text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                           static_cast<qsizetype>(text.size()));
}

auto HasCubeExtension(const std::filesystem::path& path) -> bool {
  const std::u8string extension = path.extension().u8string();
  if (extension.size() != 5) return false;
  constexpr std::string_view kCube = ".cube";
  for (std::size_t index = 0; index < kCube.size(); ++index) {
    char value = static_cast<char>(extension[index]);
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    if (value != kCube[index]) return false;
  }
  return true;
}

auto IsLinkOrJunction(const std::filesystem::file_status& status) -> bool {
#ifdef _WIN32
  if (status.type() == std::filesystem::file_type::junction) return true;
#endif
  return std::filesystem::is_symlink(status);
}

struct ScanCandidate {
  std::filesystem::path absolute_path;
  std::string           relative_path;
  std::string           name;
};

/// Serial depth-first enumeration. Reports unreadable directories and skipped links.
void Enumerate(const std::filesystem::path& root, const LutLibraryScanOptions& options,
               std::vector<ScanCandidate>*     candidates,
               std::vector<LutScanDiagnostic>* diagnostics) {
  std::vector<std::filesystem::path> pending{std::filesystem::path{}};
  while (!pending.empty()) {
    const std::filesystem::path relative_dir = std::move(pending.back());
    pending.pop_back();
    std::error_code                     error;
    std::filesystem::directory_iterator iterator(root / relative_dir, error);
    if (error) {
      diagnostics->push_back(
          {LutScanDiagnosticKind::kUnreadableDirectory, ToUtf8(relative_dir), error.message()});
      continue;
    }
    for (; iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
      if (error) break;
      const std::filesystem::path relative = relative_dir / iterator->path().filename();
      std::error_code             status_error;
      const auto                  status = iterator->symlink_status(status_error);
      if (status_error) {
        diagnostics->push_back(
            {LutScanDiagnosticKind::kUnreadableFile, ToUtf8(relative), status_error.message()});
        continue;
      }
      if (IsLinkOrJunction(status)) {
        diagnostics->push_back(
            {LutScanDiagnosticKind::kSkippedLink, ToUtf8(relative), "Links are not followed."});
        continue;
      }
      if (std::filesystem::is_directory(status)) {
        const std::string name = ToUtf8(relative.filename());
        if (std::find(options.excluded_directory_names.begin(),
                      options.excluded_directory_names.end(),
                      name) == options.excluded_directory_names.end()) {
          pending.push_back(relative);
        }
        continue;
      }
      if (std::filesystem::is_regular_file(status) && HasCubeExtension(relative)) {
        candidates->push_back({iterator->path(), ToUtf8(relative), ToUtf8(relative.stem())});
      }
    }
    if (error) {
      diagnostics->push_back(
          {LutScanDiagnosticKind::kUnreadableDirectory, ToUtf8(relative_dir), error.message()});
    }
  }
}

/// Read one header and, for an official file only, stream its SHA-256.
auto ClassifyCandidate(const ScanCandidate& candidate) -> LutLibraryEntry {
  LutLibraryEntry entry;
  entry.name          = candidate.name;
  entry.relative_path = candidate.relative_path;
  std::error_code size_error;
  entry.size = std::filesystem::file_size(candidate.absolute_path, size_error);
  if (size_error) entry.size = 0;

  LutHeaderReadResult header = ReadLutHeaderFile(candidate.absolute_path);
  entry.header_error         = header.error;
  entry.header_message       = std::move(header.message);
  entry.header               = std::move(header.header);
  if (entry.IsOfficial()) {
    const std::optional<LutFileHash> hash = HashLutFile(candidate.absolute_path);
    if (hash) {
      entry.sha256 = hash->sha256;
      entry.size   = hash->size;
    } else {
      entry.header_error   = LutHeaderError::kUnreadable;
      entry.header_message = "CUBE file cannot be read for hashing";
    }
  }
  return entry;
}

auto EntryToJson(const LutLibraryEntry& entry) -> Json {
  Json item = {{"name", entry.name},
               {"path", entry.relative_path},
               {"size", entry.size},
               {"status", HeaderErrorToName(entry.header_error)}};
  if (entry.header_error != LutHeaderError::kNone) {
    item["error"] = entry.header_message;
    return item;
  }
  item["lut_3d_size"] = entry.header.lut_3d_size;
  item["lut_1d_size"] = entry.header.lut_1d_size;
  if (!entry.header.title.empty()) item["title"] = entry.header.title;
  if (entry.header.metadata) {
    item["metadata"] = Json::parse(SerializeLutMetadataJson(*entry.header.metadata));
  }
  if (!entry.sha256.empty()) item["sha256"] = entry.sha256;
  return item;
}

auto ParseFailure(std::string message) -> LutLibraryInventoryParseResult {
  return LutLibraryInventoryParseResult{std::nullopt, std::move(message)};
}

auto EntryFromJson(const Json& item, std::string* error) -> std::optional<LutLibraryEntry> {
  const auto text = [&](const char* key, std::string* output) {
    const auto found = item.find(key);
    if (found == item.end() || !found->is_string()) return false;
    *output = found->get<std::string>();
    return true;
  };
  LutLibraryEntry entry;
  std::string     status;
  const auto      size = item.find("size");
  if (!item.is_object() || !text("name", &entry.name) || !text("path", &entry.relative_path) ||
      !text("status", &status) || size == item.end() || !size->is_number_unsigned() ||
      !IsSafeLutRelativePath(entry.relative_path)) {
    *error = "inventory entry fields are missing or invalid";
    return std::nullopt;
  }
  entry.size              = size->get<std::uint64_t>();
  const auto header_error = HeaderErrorFromName(status);
  if (!header_error) {
    *error = "inventory entry status is unknown: " + status;
    return std::nullopt;
  }
  entry.header_error = *header_error;
  if (entry.header_error != LutHeaderError::kNone) {
    text("error", &entry.header_message);
    return entry;
  }
  entry.header.lut_3d_size = item.value("lut_3d_size", 0);
  entry.header.lut_1d_size = item.value("lut_1d_size", 0);
  text("title", &entry.header.title);
  if (const auto metadata = item.find("metadata"); metadata != item.end()) {
    LutHeaderError metadata_error = LutHeaderError::kNone;
    std::string    message;
    entry.header.metadata = ParseLutMetadataJson(metadata->dump(), &metadata_error, &message);
    if (!entry.header.metadata) {
      *error = "inventory entry metadata is invalid: " + message;
      return std::nullopt;
    }
  }
  text("sha256", &entry.sha256);
  if (entry.IsOfficial() != !entry.sha256.empty()) {
    *error = "inventory entry hash does not match its declared origin: " + entry.relative_path;
    return std::nullopt;
  }
  return entry;
}

}  // namespace

auto LutLibraryInventory::Complete() const -> bool {
  return std::none_of(diagnostics.begin(), diagnostics.end(),
                      [](const LutScanDiagnostic& item) {
                        return item.kind != LutScanDiagnosticKind::kSkippedLink;
                      }) &&
         std::none_of(entries.begin(), entries.end(), [](const LutLibraryEntry& entry) {
           return entry.header_error == LutHeaderError::kUnreadable;
         });
}

auto ScanLutLibrary(const std::filesystem::path& root, const LutLibraryScanOptions& options)
    -> LutLibraryInventory {
  LutLibraryInventory        inventory;
  std::vector<ScanCandidate> candidates;
  Enumerate(root, options, &candidates, &inventory.diagnostics);
  std::sort(candidates.begin(), candidates.end(),
            [](const ScanCandidate& left, const ScanCandidate& right) {
              return left.relative_path < right.relative_path;
            });
  std::sort(inventory.diagnostics.begin(), inventory.diagnostics.end(),
            [](const LutScanDiagnostic& left, const LutScanDiagnostic& right) {
              return left.relative_path < right.relative_path;
            });

  // Each worker writes only the slots it claimed through `next`, so the result
  // vector needs no lock; joining the threads publishes every slot.
  inventory.entries.resize(candidates.size());
  unsigned worker_count = options.worker_count;
  if (worker_count == 0) {
    worker_count = std::clamp(std::thread::hardware_concurrency(), 1u, kMaxScanWorkers);
  }
  worker_count = static_cast<unsigned>(
      std::min<std::size_t>(worker_count, std::max<std::size_t>(candidates.size(), 1)));
  std::atomic<std::size_t> next{0};
  const auto               work = [&] {
    for (std::size_t index = next.fetch_add(1); index < candidates.size();
         index             = next.fetch_add(1)) {
      inventory.entries[index] = ClassifyCandidate(candidates[index]);
    }
  };
  {
    std::vector<std::jthread> workers;
    workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for (unsigned index = 1; index < worker_count; ++index) workers.emplace_back(work);
    work();
  }
  return inventory;
}

auto SerializeLutLibraryInventory(const LutLibraryInventory& inventory) -> std::string {
  Json entries = Json::array();
  for (const LutLibraryEntry& entry : inventory.entries) entries.push_back(EntryToJson(entry));
  Json diagnostics = Json::array();
  for (const LutScanDiagnostic& item : inventory.diagnostics) {
    diagnostics.push_back({{"kind", kDiagnosticNames[static_cast<std::size_t>(item.kind)]},
                           {"path", item.relative_path},
                           {"message", item.message}});
  }
  const Json root = {{"schema", 1},
                     {"kind", kInventoryKind},
                     {"complete", inventory.Complete()},
                     {"luts", std::move(entries)},
                     {"diagnostics", std::move(diagnostics)}};
  return root.dump(1);
}

auto ParseLutLibraryInventory(std::string_view json_bytes) -> LutLibraryInventoryParseResult {
  const Json root = Json::parse(json_bytes.begin(), json_bytes.end(), nullptr, false);
  if (root.is_discarded() || !root.is_object() || root.value("schema", 0) != 1 ||
      root.value("kind", std::string{}) != kInventoryKind) {
    return ParseFailure("LUT inventory schema or kind is not supported");
  }
  const auto luts        = root.find("luts");
  const auto diagnostics = root.find("diagnostics");
  if (luts == root.end() || !luts->is_array() || diagnostics == root.end() ||
      !diagnostics->is_array()) {
    return ParseFailure("LUT inventory lists are missing");
  }
  LutLibraryInventory inventory;
  for (const Json& item : *luts) {
    std::string error;
    auto        entry = EntryFromJson(item, &error);
    if (!entry) return ParseFailure(std::move(error));
    inventory.entries.push_back(std::move(*entry));
  }
  for (const Json& item : *diagnostics) {
    const std::string kind  = item.is_object() ? item.value("kind", std::string{}) : std::string{};
    const auto        found = std::find(kDiagnosticNames.begin(), kDiagnosticNames.end(), kind);
    if (found == kDiagnosticNames.end()) return ParseFailure("LUT inventory diagnostic is invalid");
    inventory.diagnostics.push_back(
        {static_cast<LutScanDiagnosticKind>(found - kDiagnosticNames.begin()),
         item.value("path", std::string{}), item.value("message", std::string{})});
  }
  return LutLibraryInventoryParseResult{std::move(inventory), {}};
}

auto WriteLutLibraryInventoryFile(const std::filesystem::path& root,
                                  const LutLibraryInventory&   inventory) -> std::string {
  const std::string bytes = SerializeLutLibraryInventory(inventory);
  QSaveFile         output(ToQString(root / std::filesystem::path(kLutLibraryInventoryFileName)));
  if (!output.open(QIODevice::WriteOnly)) {
    return output.errorString().toStdString();
  }
  if (output.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
      static_cast<qint64>(bytes.size())) {
    const std::string error = output.errorString().toStdString();
    output.cancelWriting();
    return error;
  }
  if (!output.commit()) {
    return output.errorString().toStdString();
  }
  return {};
}

auto ReadLutLibraryInventoryFile(const std::filesystem::path& root)
    -> LutLibraryInventoryParseResult {
  QFile input(ToQString(root / std::filesystem::path(kLutLibraryInventoryFileName)));
  if (!input.open(QIODevice::ReadOnly)) {
    return ParseFailure(input.errorString().toStdString());
  }
  const QByteArray bytes = input.readAll();
  return ParseLutLibraryInventory(std::string_view(bytes.constData(), bytes.size()));
}

}  // namespace alcedo
