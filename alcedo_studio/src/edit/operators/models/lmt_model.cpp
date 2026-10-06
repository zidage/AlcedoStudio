//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/lmt_model.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

#include "color/color_encoding_catalog.hpp"

namespace alcedo {

static_assert(kDefaultLutEncodingId == color::kDefaultColorEncodingId,
              "The LMT default encoding is the catalog default (ACEScc)");

namespace {

void RequireValid(const LmtUpdate& update) {
  if (update.reference.has_value()) {
    if (auto error = ValidateLutReference(*update.reference); !error.empty()) {
      throw std::invalid_argument(error);
    }
  }
  if (update.strength.has_value()) {
    if (auto error = ValidateLutStrength(*update.strength); !error.empty()) {
      throw std::invalid_argument(error);
    }
  }
  for (const auto* encoding : {&update.input_encoding, &update.output_encoding}) {
    if (encoding->has_value()) {
      if (auto error = ValidateLutEncodingId(**encoding); !error.empty()) {
        throw std::invalid_argument(error);
      }
    }
  }
}

/// The encoding id stored under @p key, or the default when the key is missing.
auto ReadEncoding(const nlohmann::json& json, const char* key) -> std::string {
  if (!json.contains(key)) {
    return std::string{kDefaultLutEncodingId};
  }
  if (!json.at(key).is_string()) {
    throw std::invalid_argument(std::string("LMT ") + key + " must be a string");
  }
  return json.at(key).get<std::string>();
}

auto ReadReference(const nlohmann::json& json) -> LutReference {
  std::string cube_path;
  if (json.contains("cube_path")) {
    if (!json.at("cube_path").is_string()) {
      throw std::invalid_argument("LMT cube_path must be a string");
    }
    cube_path = json.at("cube_path").get<std::string>();
  }
  if (json.contains("reference") && !json.at("reference").is_null()) {
    if (!cube_path.empty()) {
      throw std::invalid_argument("LMT has both a cube_path and a tagged reference");
    }
    return TaggedLutReferenceFromJson(json.at("reference"));
  }
  if (cube_path.empty()) {
    return std::monostate{};
  }
  return FileLutReference{std::move(cube_path)};
}

}  // namespace

auto ValidateLutStrength(float strength) -> std::string {
  if (!std::isfinite(strength) || strength < 0.0f || strength > 1.0f) {
    return "LUT strength must be a finite value from 0 to 1";
  }
  return {};
}

auto ValidateLutEncodingId(std::string_view encoding_id) -> std::string {
  if (color::FindColorEncoding(encoding_id) == nullptr) {
    return "LUT color encoding '" + std::string(encoding_id) + "' is not in the catalog";
  }
  return {};
}

auto LmtUpdateFromModelJson(const nlohmann::json& json) -> LmtUpdate {
  if (json.is_null()) {
    return LmtUpdateFromModelJson(nlohmann::json::object());
  }
  if (!json.is_object()) {
    throw std::invalid_argument("LMT parameters must be an object");
  }
  for (const auto& [key, value] : json.items()) {
    (void)value;
    if (key != "cube_path" && key != "reference" && key != "strength" && key != "name" &&
        key != "input_encoding" && key != "output_encoding") {
      throw std::invalid_argument("LMT parameters have unknown key '" + key + "'");
    }
  }
  LmtUpdate update;
  update.reference = ReadReference(json);
  if (json.contains("name")) {
    if (!json.at("name").is_string()) {
      throw std::invalid_argument("LMT name must be a string");
    }
    update.display_name = json.at("name").get<std::string>();
  }
  float strength = kDefaultLutStrength;
  if (json.contains("strength")) {
    if (!json.at("strength").is_number()) {
      throw std::invalid_argument("LMT strength must be a number");
    }
    strength = json.at("strength").get<float>();
  }
  update.strength        = strength;
  update.input_encoding  = ReadEncoding(json, "input_encoding");
  update.output_encoding = ReadEncoding(json, "output_encoding");
  RequireValid(update);
  return update;
}

auto LmtModel::IsDefault() const -> bool {
  return Read([](const LmtPayload& payload) {
    return IsEmptyLutReference(payload.reference) && payload.strength == kDefaultLutStrength &&
           payload.input_encoding == kDefaultLutEncodingId &&
           payload.output_encoding == kDefaultLutEncodingId;
  });
}

void LmtModel::SetReference(LutReference reference, std::string display_name) {
  LmtUpdate update;
  update.reference    = std::move(reference);
  update.display_name = std::move(display_name);
  ApplyUpdate(update);
}

void LmtModel::SetStrength(float strength) {
  LmtUpdate update;
  update.strength = strength;
  ApplyUpdate(update);
}

void LmtModel::SetEncodings(std::string input_encoding, std::string output_encoding) {
  LmtUpdate update;
  update.input_encoding  = std::move(input_encoding);
  update.output_encoding = std::move(output_encoding);
  ApplyUpdate(update);
}

void LmtModel::ApplyUpdate(const LmtUpdate& update) {
  RequireValid(update);
  MutateWithDirtyFields([&update](LmtPayload& payload) {
    DirtyFieldMask changed{};
    if (update.reference.has_value()) {
      // An empty reference keeps no name: nothing is associated.
      const std::string name =
          IsEmptyLutReference(*update.reference) ? std::string{} : update.display_name;
      if (payload.reference != *update.reference || payload.display_name != name) {
        payload.reference    = *update.reference;
        payload.display_name = name;
        changed |= LmtDirty::Reference;
      }
    }
    if (update.strength.has_value() && payload.strength != *update.strength) {
      payload.strength = *update.strength;
      changed |= LmtDirty::Strength;
    }
    if (update.input_encoding.has_value() && payload.input_encoding != *update.input_encoding) {
      payload.input_encoding = *update.input_encoding;
      changed |= LmtDirty::Encoding;
    }
    if (update.output_encoding.has_value() && payload.output_encoding != *update.output_encoding) {
      payload.output_encoding = *update.output_encoding;
      changed |= LmtDirty::Encoding;
    }
    return changed;
  });
}

void LmtModel::SetCubePath(std::string path) {
  if (path.empty()) {
    SetReference(std::monostate{});
    return;
  }
  SetReference(FileLutReference{std::move(path)});
}

auto LmtModel::Reference() const -> LutReference {
  return Read([](const LmtPayload& payload) { return payload.reference; });
}

auto LmtModel::DisplayName() const -> std::string {
  return Read([](const LmtPayload& payload) { return payload.display_name; });
}

auto LmtModel::Strength() const -> float {
  return Read([](const LmtPayload& payload) { return payload.strength; });
}

auto LmtModel::InputEncoding() const -> std::string {
  return Read([](const LmtPayload& payload) { return payload.input_encoding; });
}

auto LmtModel::OutputEncoding() const -> std::string {
  return Read([](const LmtPayload& payload) { return payload.output_encoding; });
}

auto LmtModel::CubePath() const -> std::string {
  return Read([](const LmtPayload& payload) {
    const auto* file = std::get_if<FileLutReference>(&payload.reference);
    return file == nullptr ? std::string{} : file->path;
  });
}

auto LmtModel::ToJson() const -> nlohmann::json {
  return Read([](const LmtPayload& payload) {
    const auto*    file = std::get_if<FileLutReference>(&payload.reference);
    nlohmann::json json{{"cube_path", file == nullptr ? std::string{} : file->path}};
    if (std::holds_alternative<OfficialLutReference>(payload.reference) ||
        std::holds_alternative<LibraryLutReference>(payload.reference)) {
      json["reference"] = TaggedLutReferenceToJson(payload.reference);
    }
    if (!payload.display_name.empty()) {
      json["name"] = payload.display_name;
    }
    if (payload.strength != kDefaultLutStrength) {
      json["strength"] = payload.strength;
    }
    if (payload.input_encoding != kDefaultLutEncodingId) {
      json["input_encoding"] = payload.input_encoding;
    }
    if (payload.output_encoding != kDefaultLutEncodingId) {
      json["output_encoding"] = payload.output_encoding;
    }
    return json;
  });
}

void LmtModel::LoadJson(const nlohmann::json& json) { ApplyUpdate(LmtUpdateFromModelJson(json)); }

}  // namespace alcedo
