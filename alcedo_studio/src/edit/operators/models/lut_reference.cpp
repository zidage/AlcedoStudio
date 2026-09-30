//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/lut_reference.hpp"

#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace alcedo {
namespace {

auto HasControlSeparator(std::string_view text) -> bool {
  return text.find('\0') != std::string_view::npos || text.find('\n') != std::string_view::npos ||
         text.find('\r') != std::string_view::npos;
}

auto ValidateText(std::string_view text, std::string_view name) -> std::string {
  if (text.empty()) {
    return std::string{name} + " is empty";
  }
  if (HasControlSeparator(text)) {
    return std::string{name} + " contains a NUL or line break";
  }
  return {};
}

auto ValidateRelativePath(std::string_view path) -> std::string {
  if (auto error = ValidateText(path, "LUT library path"); !error.empty()) {
    return error;
  }
  if (path.front() == '/' || path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos) {
    return "LUT library path must be a relative '/' path";
  }
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t end = path.find('/', begin);
    const auto        segment =
        path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
    if (segment.empty() || segment == "." || segment == "..") {
      return "LUT library path has an empty, '.', or '..' segment";
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return {};
}

auto RequireString(const nlohmann::json& json, const char* key) -> std::string {
  if (!json.contains(key) || !json.at(key).is_string()) {
    throw std::invalid_argument(std::string{"LUT reference requires string '"} + key + "'");
  }
  return json.at(key).get<std::string>();
}

void RejectUnknownKeys(const nlohmann::json& json, std::initializer_list<std::string_view> keys) {
  for (const auto& [key, value] : json.items()) {
    (void)value;
    bool known = false;
    for (const auto allowed : keys) {
      known = known || key == allowed;
    }
    if (!known) {
      throw std::invalid_argument("LUT reference has unknown key '" + key + "'");
    }
  }
}

}  // namespace

auto ValidateLutReference(const LutReference& reference) -> std::string {
  if (const auto* official = std::get_if<OfficialLutReference>(&reference)) {
    if (auto error = ValidateText(official->package_id, "LUT package ID"); !error.empty()) {
      return error;
    }
    return ValidateText(official->lut_id, "LUT ID");
  }
  if (const auto* library = std::get_if<LibraryLutReference>(&reference)) {
    return ValidateRelativePath(library->relative_path);
  }
  if (const auto* file = std::get_if<FileLutReference>(&reference)) {
    return ValidateText(file->path, "LUT file path");
  }
  return {};
}

auto DescribeLutReference(const LutReference& reference) -> std::string {
  if (const auto* official = std::get_if<OfficialLutReference>(&reference)) {
    return "official:" + official->package_id + "/" + official->lut_id;
  }
  if (const auto* library = std::get_if<LibraryLutReference>(&reference)) {
    return "library:" + library->relative_path;
  }
  if (const auto* file = std::get_if<FileLutReference>(&reference)) {
    return "file:" + file->path;
  }
  return "none";
}

auto TaggedLutReferenceToJson(const LutReference& reference) -> nlohmann::json {
  if (const auto* official = std::get_if<OfficialLutReference>(&reference)) {
    return {
        {"kind", "official"}, {"package_id", official->package_id}, {"lut_id", official->lut_id}};
  }
  if (const auto* library = std::get_if<LibraryLutReference>(&reference)) {
    return {{"kind", "library"}, {"path", library->relative_path}};
  }
  throw std::invalid_argument("Only official and library LUT references have a tagged form");
}

auto TaggedLutReferenceFromJson(const nlohmann::json& json) -> LutReference {
  if (!json.is_object()) {
    throw std::invalid_argument("LUT reference must be an object");
  }
  const std::string kind = RequireString(json, "kind");
  LutReference      reference;
  if (kind == "official") {
    RejectUnknownKeys(json, {"kind", "package_id", "lut_id"});
    reference =
        OfficialLutReference{RequireString(json, "package_id"), RequireString(json, "lut_id")};
  } else if (kind == "library") {
    RejectUnknownKeys(json, {"kind", "path"});
    reference = LibraryLutReference{RequireString(json, "path")};
  } else {
    throw std::invalid_argument("LUT reference kind '" + kind + "' is not supported");
  }
  if (auto error = ValidateLutReference(reference); !error.empty()) {
    throw std::invalid_argument(error);
  }
  return reference;
}

}  // namespace alcedo
