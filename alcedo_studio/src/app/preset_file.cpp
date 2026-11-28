//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/preset_file.hpp"

#include <QFile>
#include <QString>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <utility>

#include "app/document_transfer.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "json.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace alcedo {
namespace {

namespace fs = std::filesystem;

auto Failure(PresetFileRule rule, std::string message) -> PresetFileReadResult {
  PresetFileReadResult result;
  result.rule    = rule;
  result.message = std::move(message);
  return result;
}

auto WriteFailure(PresetFileRule rule, std::string message) -> PresetFileWriteResult {
  return PresetFileWriteResult{.rule = rule, .message = std::move(message)};
}

auto PathText(const fs::path& path) -> std::string {
  const auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

auto PathFromUtf8(std::string_view text) -> fs::path {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

auto ToQString(const fs::path& path) -> QString {
  const auto text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                           static_cast<qsizetype>(text.size()));
}

/// True when @p bytes is UTF-8 without overlong forms, surrogates, or values above U+10FFFF.
auto IsValidUtf8(std::string_view bytes) -> bool {
  std::size_t index = 0;
  while (index < bytes.size()) {
    const auto lead = static_cast<unsigned char>(bytes[index]);
    if (lead < 0x80) {
      ++index;
      continue;
    }
    std::size_t   length  = 0;
    std::uint32_t value   = 0;
    std::uint32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0) {
      length = 2, value = lead & 0x1F, minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3, value = lead & 0x0F, minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4, value = lead & 0x07, minimum = 0x10000;
    } else {
      return false;
    }
    if (index + length > bytes.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(bytes[index + offset]);
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      value = (value << 6) | (next & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
      return false;
    }
    index += length;
  }
  return true;
}

/**
 * Event handler of nlohmann::json::sax_parse that builds no value. It stops the parse at the
 * first value deeper than the limit, the first long string or key, and the first non-finite
 * number, so that a hostile file costs at most one pass over its bytes.
 */
class PresetTextScanner {
 public:
  using json = nlohmann::json;

  auto null() -> bool { return true; }
  auto boolean(bool) -> bool { return true; }
  auto number_integer(json::number_integer_t) -> bool { return true; }
  auto number_unsigned(json::number_unsigned_t) -> bool { return true; }
  auto number_float(json::number_float_t value, const json::string_t&) -> bool {
    if (!std::isfinite(value)) {
      return Stop(PresetFileRule::kNonFiniteNumber, "a number is not finite");
    }
    return true;
  }
  auto string(json::string_t& value) -> bool { return CheckString(value, "a string"); }
  auto binary(json::binary_t&) -> bool {
    return Stop(PresetFileRule::kInvalidJson, "binary values are not JSON text");
  }
  auto start_object(std::size_t) -> bool { return Enter(); }
  auto key(json::string_t& value) -> bool { return CheckString(value, "an object key"); }
  auto end_object() -> bool { return Leave(); }
  auto start_array(std::size_t) -> bool { return Enter(); }
  auto end_array() -> bool { return Leave(); }
  auto parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception& e)
      -> bool {
    return Stop(PresetFileRule::kInvalidJson,
                "invalid JSON at byte " + std::to_string(position) + ": " + e.what());
  }

  [[nodiscard]] auto Rule() const -> PresetFileRule { return rule_; }
  [[nodiscard]] auto Message() const -> const std::string& { return message_; }

 private:
  auto Enter() -> bool {
    if (++depth_ > kPresetJsonMaxDepth) {
      return Stop(PresetFileRule::kTooDeep, "the JSON is nested deeper than " +
                                                std::to_string(kPresetJsonMaxDepth) + " levels");
    }
    return true;
  }
  auto Leave() -> bool {
    --depth_;
    return true;
  }
  auto CheckString(const json::string_t& value, const char* what) -> bool {
    if (value.size() > kPresetMaxStringBytes) {
      return Stop(PresetFileRule::kStringTooLong, std::string{what} + " is longer than " +
                                                      std::to_string(kPresetMaxStringBytes) +
                                                      " bytes");
    }
    return true;
  }
  auto Stop(PresetFileRule rule, std::string message) -> bool {
    if (rule_ == PresetFileRule::kNone) {
      rule_    = rule;
      message_ = std::move(message);
    }
    return false;
  }

  std::size_t    depth_ = 0;
  PresetFileRule rule_  = PresetFileRule::kNone;
  std::string    message_;
};

auto ArraySize(const nlohmann::json& object, const char* key) -> std::size_t {
  if (!object.is_object()) {
    return 0;
  }
  const auto found = object.find(key);
  return found != object.end() && found->is_array() ? found->size() : 0;
}

/// The count limits of plan 3.2, checked on the JSON before the package is built.
auto CountLimitError(const nlohmann::json& json) -> std::string {
  if (!json.is_object()) {
    return {};
  }
  if (ArraySize(json, "color_grades") > kPresetMaxColorGrades) {
    return "more than " + std::to_string(kPresetMaxColorGrades) + " Color Grades";
  }
  if (const auto grades = json.find("color_grades"); grades != json.end() && grades->is_array()) {
    for (const auto& grade : *grades) {
      if (ArraySize(grade, "adjustments") > kPresetMaxAdjustmentsPerNode) {
        return "a Color Grade has more than " + std::to_string(kPresetMaxAdjustmentsPerNode) +
               " adjustments";
      }
      if (ArraySize(grade, "masks") > kPresetMaxMasksPerGrade) {
        return "a Color Grade has more than " + std::to_string(kPresetMaxMasksPerGrade) + " Masks";
      }
    }
  }
  if (const auto drt_post = json.find("drt_post"); drt_post != json.end()) {
    if (ArraySize(*drt_post, "adjustments") > kPresetMaxAdjustmentsPerNode) {
      return "DRT/Post has more than " + std::to_string(kPresetMaxAdjustmentsPerNode) +
             " adjustments";
    }
  }
  return {};
}

/// Reason why the library path @p relative_path is not below its root, or an empty string.
auto LibraryPathError(const std::string& relative_path) -> std::string {
  const fs::path path = PathFromUtf8(relative_path);
  if (path.has_root_name() || path.has_root_directory() ||
      relative_path.find('\\') != std::string::npos) {
    return "is not a relative path";
  }
  for (const auto& part : path) {
    if (part == "..") {
      return "has a '..' part";
    }
  }
  return {};
}

/**
 * Reason why the LMT parameters @p params refer to a file outside the LUT libraries, or an empty
 * string (plan 3.3). Reads the raw keys, so that the rule applies before the model loads them.
 */
auto LmtParamsReferenceError(const nlohmann::json& params) -> std::string {
  if (!params.is_object()) {
    return {};
  }
  if (const auto path = params.find("cube_path");
      path != params.end() && !(path->is_string() && path->get<std::string>().empty())) {
    return "refers to a LUT file by its path; a preset can refer only to a LUT of the LUT library "
           "or an official LUT";
  }
  const auto reference = params.find("reference");
  if (reference == params.end() || !reference->is_object()) {
    return {};
  }
  const auto kind = reference->find("kind");
  if (kind == reference->end() || !kind->is_string() || kind->get<std::string>() != "library") {
    return {};
  }
  const auto path = reference->find("path");
  if (path == reference->end() || !path->is_string()) {
    return "has a LUT library reference without a path";
  }
  if (auto error = LibraryPathError(path->get<std::string>()); !error.empty()) {
    return "has a LUT library path that " + error;
  }
  return {};
}

/// The file reference rules of plan 3.3 on the adjustments array @p adjustments of @p node.
auto AdjustmentsReferenceError(const nlohmann::json& adjustments, const std::string& node)
    -> std::string {
  if (!adjustments.is_array()) {
    return {};
  }
  const std::string lmt_type{type_ids::Lmt().Text()};
  for (const auto& adjustment : adjustments) {
    if (!adjustment.is_object()) {
      continue;
    }
    const auto type   = adjustment.find("type");
    const auto params = adjustment.find("params");
    if (type == adjustment.end() || !type->is_string() || type->get<std::string>() != lmt_type ||
        params == adjustment.end()) {
      continue;
    }
    if (auto error = LmtParamsReferenceError(*params); !error.empty()) {
      return "the LUT of " + node + " " + error;
    }
  }
  return {};
}

auto LensDatabaseReferenceError(const nlohmann::json& lens_calib) -> std::string {
  if (!lens_calib.is_object()) {
    return {};
  }
  // The only accepted database is the built-in one; its default value is a relative path.
  const auto path = lens_calib.find("lens_profile_db_path");
  if (path == lens_calib.end()) {
    return {};
  }
  if (!path->is_string() || (!path->get<std::string>().empty() &&
                             path->get<std::string>() != DevelopPayload{}.lens_profile_db_path)) {
    return "Lens Correction refers to a lens profile database by its path";
  }
  return {};
}

/// The file reference rules of plan 3.3 on the package JSON @p json, before the package is built.
auto PackageJsonReferenceError(const nlohmann::json& json) -> std::string {
  if (!json.is_object()) {
    return {};
  }
  if (const auto grades = json.find("color_grades"); grades != json.end() && grades->is_array()) {
    for (const auto& grade : *grades) {
      if (!grade.is_object() || !grade.contains("adjustments")) {
        continue;
      }
      const auto  name = grade.find("display_name");
      std::string node_name =
          "the Color Grade '" +
          (name != grade.end() && name->is_string() ? name->get<std::string>() : std::string{}) +
          "'";
      if (auto error = AdjustmentsReferenceError(grade.at("adjustments"), node_name);
          !error.empty()) {
        return error;
      }
    }
  }
  if (const auto drt_post = json.find("drt_post");
      drt_post != json.end() && drt_post->is_object() && drt_post->contains("adjustments")) {
    if (auto error = AdjustmentsReferenceError(drt_post->at("adjustments"), "DRT/Post");
        !error.empty()) {
      return error;
    }
  }
  if (const auto develop = json.find("develop");
      develop != json.end() && develop->is_object() && develop->contains("lens_calib")) {
    return LensDatabaseReferenceError(develop->at("lens_calib"));
  }
  return {};
}

#ifdef _WIN32
/// Close a Windows handle on scope exit.
struct HandleCloser {
  HANDLE handle;
  ~HandleCloser() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};
#else
struct DescriptorCloser {
  int descriptor;
  ~DescriptorCloser() {
    if (descriptor >= 0) {
      close(descriptor);
    }
  }
};
#endif

/// Read the bytes of the regular file @p path through one open handle.
auto ReadRegularFileBytes(const fs::path& path, std::string* bytes) -> PresetFileReadResult {
  const std::string name = PathText(path);
#ifdef _WIN32
  // FILE_FLAG_OPEN_REPARSE_POINT opens a link itself, so that its target is never read.
  HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Failure(PresetFileRule::kReadFailed, "cannot open the preset file " + name);
  }
  HandleCloser            closer{file};
  FILE_ATTRIBUTE_TAG_INFO tag_info{};
  if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag_info, sizeof(tag_info))) {
    return Failure(PresetFileRule::kReadFailed, "cannot read the attributes of " + name);
  }
  const bool link = (tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                    IsReparseTagNameSurrogate(tag_info.ReparseTag);
  if (link || (tag_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      GetFileType(file) != FILE_TYPE_DISK) {
    return Failure(PresetFileRule::kNotRegularFile, name + " is not a regular file");
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) {
    return Failure(PresetFileRule::kReadFailed, "cannot read the size of " + name);
  }
  if (static_cast<std::uintmax_t>(size.QuadPart) > kPresetFileMaxBytes) {
    return Failure(PresetFileRule::kFileTooLarge, name + " is larger than 4 MiB");
  }
  // One byte more than the limit detects a file that grows during the read.
  bytes->resize(static_cast<std::size_t>(kPresetFileMaxBytes) + 1);
  std::size_t total = 0;
  while (total < bytes->size()) {
    DWORD read = 0;
    if (!ReadFile(file, bytes->data() + total, static_cast<DWORD>(bytes->size() - total), &read,
                  nullptr)) {
      return Failure(PresetFileRule::kReadFailed, "cannot read " + name);
    }
    if (read == 0) {
      break;
    }
    total += read;
  }
#else
  // O_NOFOLLOW refuses a link; O_NONBLOCK keeps a FIFO from blocking the open.
  const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) {
    if (errno == ELOOP) {
      return Failure(PresetFileRule::kNotRegularFile, name + " is not a regular file");
    }
    return Failure(PresetFileRule::kReadFailed, "cannot open the preset file " + name);
  }
  DescriptorCloser closer{descriptor};
  struct stat      status{};
  if (fstat(descriptor, &status) != 0) {
    return Failure(PresetFileRule::kReadFailed, "cannot read the attributes of " + name);
  }
  if (!S_ISREG(status.st_mode)) {
    return Failure(PresetFileRule::kNotRegularFile, name + " is not a regular file");
  }
  if (static_cast<std::uintmax_t>(status.st_size) > kPresetFileMaxBytes) {
    return Failure(PresetFileRule::kFileTooLarge, name + " is larger than 4 MiB");
  }
  bytes->resize(static_cast<std::size_t>(kPresetFileMaxBytes) + 1);
  std::size_t total = 0;
  while (total < bytes->size()) {
    const ssize_t read_count = read(descriptor, bytes->data() + total, bytes->size() - total);
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Failure(PresetFileRule::kReadFailed, "cannot read " + name);
    }
    if (read_count == 0) {
      break;
    }
    total += static_cast<std::size_t>(read_count);
  }
#endif
  if (total > kPresetFileMaxBytes) {
    return Failure(PresetFileRule::kFileTooLarge, name + " is larger than 4 MiB");
  }
  bytes->resize(total);
  return {};
}

auto ParsePresetTextUnchecked(std::string_view bytes) -> PresetFileReadResult {
  if (bytes.size() > kPresetFileMaxBytes) {
    return Failure(PresetFileRule::kFileTooLarge, "the preset is larger than 4 MiB");
  }
  if (!IsValidUtf8(bytes)) {
    return Failure(PresetFileRule::kInvalidText, "the preset is not valid UTF-8 text");
  }
  PresetTextScanner scanner;
  const bool        scanned = nlohmann::json::sax_parse(bytes, &scanner);
  if (!scanned || scanner.Rule() != PresetFileRule::kNone) {
    return Failure(
        scanner.Rule() == PresetFileRule::kNone ? PresetFileRule::kInvalidJson : scanner.Rule(),
        scanner.Message().empty() ? "invalid JSON" : scanner.Message());
  }
  nlohmann::json json = nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
  if (json.is_discarded()) {
    return Failure(PresetFileRule::kInvalidJson, "invalid JSON");
  }
  if (auto error = CountLimitError(json); !error.empty()) {
    return Failure(PresetFileRule::kCountLimit, "the preset has " + error);
  }
  if (!json.is_object()) {
    return Failure(PresetFileRule::kInvalidPackage, "the preset is not a JSON object");
  }
  const auto fingerprint = json.find("fingerprint");
  if (fingerprint == json.end()) {
    return Failure(PresetFileRule::kMissingFingerprint, "the preset has no fingerprint");
  }
  if (!fingerprint->is_string()) {
    return Failure(PresetFileRule::kNotCanonical, "the preset fingerprint is not a string");
  }
  if (auto error = PackageJsonReferenceError(json); !error.empty()) {
    return Failure(PresetFileRule::kFileReference, std::move(error));
  }
  const std::string fingerprint_text = fingerprint->get<std::string>();
  json.erase(fingerprint);

  AdjustmentTransferPackage package;
  try {
    package = ImportDocumentTransfer(json);
  } catch (const std::exception& e) {
    return Failure(PresetFileRule::kInvalidPackage, e.what());
  }
  if (package.fingerprint_ != fingerprint_text) {
    return Failure(PresetFileRule::kNotCanonical,
                   "the preset fingerprint does not match its content");
  }
  std::string canonical = ExportDocumentTransfer(package).dump();
  if (canonical != bytes) {
    return Failure(PresetFileRule::kNotCanonical, "the preset text is not the canonical text");
  }
  PresetFileReadResult result;
  result.package        = std::move(package);
  result.canonical_text = std::move(canonical);
  return result;
}

auto IsReservedDeviceName(std::string_view name) -> bool {
  std::string base{name.substr(0, name.find('.'))};
  std::transform(base.begin(), base.end(), base.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  // A trailing space before the extension is ignored by Windows too.
  while (!base.empty() && base.back() == ' ') {
    base.pop_back();
  }
  static constexpr std::array<std::string_view, 4> kNames = {"CON", "PRN", "AUX", "NUL"};
  if (std::find(kNames.begin(), kNames.end(), base) != kNames.end()) {
    return true;
  }
  return base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
         base[3] >= '1' && base[3] <= '9';
}

auto RandomSuffix() -> std::string {
  std::random_device                      device;
  std::uniform_int_distribution<unsigned> digit(0, 15);
  std::string                             suffix;
  for (int index = 0; index < 12; ++index) {
    suffix.push_back("0123456789abcdef"[digit(device)]);
  }
  return suffix;
}

}  // namespace

auto PresetFileRuleText(PresetFileRule rule) -> std::string_view {
  switch (rule) {
    case PresetFileRule::kNone:
      return "accepted";
    case PresetFileRule::kNotRegularFile:
      return "not a regular file";
    case PresetFileRule::kFileTooLarge:
      return "file too large";
    case PresetFileRule::kReadFailed:
      return "read failed";
    case PresetFileRule::kInvalidText:
      return "invalid text";
    case PresetFileRule::kTooDeep:
      return "too deep";
    case PresetFileRule::kInvalidJson:
      return "invalid JSON";
    case PresetFileRule::kStringTooLong:
      return "string too long";
    case PresetFileRule::kNonFiniteNumber:
      return "number not finite";
    case PresetFileRule::kCountLimit:
      return "count limit";
    case PresetFileRule::kMissingFingerprint:
      return "no fingerprint";
    case PresetFileRule::kNotCanonical:
      return "not canonical";
    case PresetFileRule::kInvalidPackage:
      return "invalid package";
    case PresetFileRule::kFileReference:
      return "file reference";
    case PresetFileRule::kInvalidName:
      return "invalid name";
    case PresetFileRule::kTargetExists:
      return "file exists";
    case PresetFileRule::kWriteFailed:
      return "write failed";
  }
  return "unknown rule";
}

auto ReadPresetFile(const fs::path& path) -> PresetFileReadResult {
  try {
    std::string bytes;
    if (auto read = ReadRegularFileBytes(path, &bytes); !read.Ok()) {
      return read;
    }
    auto result = ParsePresetTextUnchecked(bytes);
    if (!result.Ok()) {
      result.message = PathText(path.filename()) + ": " + result.message;
    }
    return result;
  } catch (const std::exception& e) {
    return Failure(PresetFileRule::kReadFailed, PathText(path) + ": " + e.what());
  } catch (...) {
    return Failure(PresetFileRule::kReadFailed, PathText(path) + ": unknown error");
  }
}

auto ParsePresetText(std::string_view bytes) -> PresetFileReadResult {
  try {
    return ParsePresetTextUnchecked(bytes);
  } catch (const std::exception& e) {
    return Failure(PresetFileRule::kInvalidPackage, e.what());
  } catch (...) {
    return Failure(PresetFileRule::kInvalidPackage, "unknown error");
  }
}

auto CheckPresetReferences(const AdjustmentTransferPackage& package) -> PresetFileWriteResult {
  const auto check = [](const std::vector<TransferAdjustmentValue>& adjustments,
                        const std::string&                          node) -> std::string {
    for (const auto& adjustment : adjustments) {
      if (adjustment.type != type_ids::Lmt()) {
        continue;
      }
      if (auto error = LmtParamsReferenceError(adjustment.params); !error.empty()) {
        return "the LUT of " + node + " " + error;
      }
    }
    return {};
  };
  std::string error;
  for (const auto& grade : package.color_grades_) {
    if (error.empty()) {
      error = check(grade.adjustments, "the Color Grade '" + grade.display_name + "'");
    }
  }
  if (error.empty()) {
    error = check(package.drt_post_.adjustments, "DRT/Post");
  }
  if (error.empty() && package.develop_.lens_calib.has_value()) {
    error = LensDatabaseReferenceError(*package.develop_.lens_calib);
  }
  if (!error.empty()) {
    return WriteFailure(PresetFileRule::kFileReference, std::move(error));
  }
  return {};
}

auto PresetNameError(std::string_view name) -> std::string {
  if (name.empty()) {
    return "The preset name is empty.";
  }
  if (!IsValidUtf8(name)) {
    return "The preset name is not valid text.";
  }
  if (name.front() == ' ' || name.back() == ' ') {
    return "The preset name starts or ends with a space.";
  }
  if (name.back() == '.') {
    return "The preset name ends with a dot.";
  }
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) {
      return "The preset name contains a control character.";
    }
    if (std::string_view{"/\\<>:\"|?*"}.find(c) != std::string_view::npos) {
      return std::string{"The preset name contains the character '"} + c + "'.";
    }
  }
  if (IsReservedDeviceName(name)) {
    return "The preset name is a reserved device name.";
  }
  if (static_cast<std::size_t>(
          QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())).size()) >
      kPresetMaxNameLength) {
    return "The preset name is longer than " + std::to_string(kPresetMaxNameLength) +
           " characters.";
  }
  return {};
}

auto PresetNameFromLabel(std::string_view label) -> std::string {
  QString name = QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()));
  for (auto& c : name) {
    if (c.unicode() < 0x20 || c.unicode() == 0x7F || QStringView{u"/\\<>:\"|?*"}.contains(c)) {
      c = QLatin1Char('-');
    }
  }
  const auto trim = [](QString text) {
    text = text.trimmed();
    while (text.endsWith(QLatin1Char('.')) || text.endsWith(QLatin1Char(' '))) {
      text.chop(1);
    }
    return text;
  };
  name = trim(name);
  if (static_cast<std::size_t>(name.size()) > kPresetMaxNameLength) {
    name = trim(name.left(static_cast<qsizetype>(kPresetMaxNameLength)));
  }
  if (name.isEmpty()) {
    return "Preset";
  }
  std::string result = name.toStdString();
  if (IsReservedDeviceName(result)) {
    result += " Preset";
  }
  return result;
}

auto PresetFileNameFor(std::string_view name) -> fs::path {
  return PathFromUtf8(std::string{name} + std::string{kPresetFileExtension});
}

auto WritePresetText(std::string_view canonical_text, const fs::path& path,
                     PresetFileWriteMode mode) -> PresetFileWriteResult {
  try {
    std::error_code error;
    const auto      existing = fs::symlink_status(path, error);
    if (mode == PresetFileWriteMode::kCreateNew && fs::exists(existing)) {
      return WriteFailure(PresetFileRule::kTargetExists,
                          "the file " + PathText(path.filename()) + " exists");
    }
    // A replace writes over a regular file only, never through a link or over a folder.
    if (mode == PresetFileWriteMode::kReplace && fs::exists(existing) &&
        !fs::is_regular_file(existing)) {
      return WriteFailure(PresetFileRule::kWriteFailed,
                          PathText(path) + " is not a regular file");
    }
    const fs::path temporary =
        path.parent_path() / PathFromUtf8("~alcpreset-" + RandomSuffix() + ".tmp");
    {
      std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      stream.write(canonical_text.data(), static_cast<std::streamsize>(canonical_text.size()));
      stream.close();
      if (!stream) {
        fs::remove(temporary, error);
        return WriteFailure(PresetFileRule::kWriteFailed,
                            "cannot write a file in " + PathText(path.parent_path()));
      }
    }
    if (mode == PresetFileWriteMode::kReplace) {
      // std::filesystem::rename replaces an existing file in one step.
      fs::rename(temporary, path, error);
      if (error) {
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        return WriteFailure(PresetFileRule::kWriteFailed,
                            "cannot write " + PathText(path) + ": " + error.message());
      }
      return {};
    }
    // QFile::rename refuses an existing target, so that no file is replaced.
    QFile file(ToQString(temporary));
    if (!file.rename(ToQString(path))) {
      const bool exists = fs::exists(fs::symlink_status(path, error));
      fs::remove(temporary, error);
      return WriteFailure(
          exists ? PresetFileRule::kTargetExists : PresetFileRule::kWriteFailed,
          exists ? "the file " + PathText(path.filename()) + " exists"
                 : "cannot create " + PathText(path) + ": " + file.errorString().toStdString());
    }
    return {};
  } catch (const std::exception& e) {
    return WriteFailure(PresetFileRule::kWriteFailed, e.what());
  }
}

auto WritePresetFile(const AdjustmentTransferPackage& package, const fs::path& path,
                     PresetFileWriteMode mode) -> PresetFileWriteResult {
  std::string text;
  try {
    text = ExportDocumentTransfer(package).dump();
  } catch (const std::exception& e) {
    return WriteFailure(PresetFileRule::kInvalidPackage, e.what());
  }
  const auto parsed = ParsePresetText(text);
  if (!parsed.Ok()) {
    return WriteFailure(parsed.rule, parsed.message);
  }
  return WritePresetText(parsed.canonical_text, path, mode);
}

}  // namespace alcedo
