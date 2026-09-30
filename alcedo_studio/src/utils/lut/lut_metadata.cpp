//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/lut/lut_metadata.hpp"

#include <charconv>
#include <fstream>
#include <json.hpp>
#include <utility>

namespace alcedo {
namespace {

using Json                                          = nlohmann::json;

constexpr std::string_view kMarker                  = "ALCEDO_LUT";
constexpr std::size_t      kIdLimitBytes            = 160;
constexpr std::size_t      kSlugLimitBytes          = 96;
constexpr std::size_t      kNameLimitBytes          = 128;
constexpr std::size_t      kSpaceLimitBytes         = 64;
constexpr std::size_t      kDescriptionLimitBytes   = 1024;
constexpr std::size_t      kDisplayRequirementBytes = 512;
constexpr std::size_t      kAliasLimitCount         = 16;
constexpr std::size_t      kAliasLimitBytes         = 128;

auto                       IsSpace(char value) -> bool {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
         value == '\v';
}

auto Trim(std::string_view text) -> std::string_view {
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
  return text;
}

auto IsSlugChar(char value) -> bool {
  return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

/// Matches `[a-z0-9]+([<separators>][a-z0-9]+)*`.
auto MatchesIdentifier(std::string_view text, std::string_view separators) -> bool {
  if (text.empty() || !IsSlugChar(text.front()) || !IsSlugChar(text.back())) {
    return false;
  }
  bool previous_was_separator = false;
  for (const char value : text) {
    if (IsSlugChar(value)) {
      previous_was_separator = false;
    } else if (separators.find(value) != std::string_view::npos && !previous_was_separator) {
      previous_was_separator = true;
    } else {
      return false;
    }
  }
  return true;
}

auto HasControlCharacter(std::string_view text) -> bool {
  for (const char value : text) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte < 0x20 || byte == 0x7F) {
      return true;
    }
  }
  return false;
}

/// Accumulates the first field error; later errors do not overwrite it.
class FieldValidator {
 public:
  void Fail(std::string message) {
    if (message_.empty()) message_ = std::move(message);
  }
  [[nodiscard]] auto Failed() const -> bool { return !message_.empty(); }
  [[nodiscard]] auto Message() const -> const std::string& { return message_; }

  auto Text(const Json& object, const char* key, const std::string& label, std::size_t limit)
      -> std::string {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
      Fail(label + " must be a non-empty string");
      return {};
    }
    std::string value = found->get<std::string>();
    if (value.empty()) {
      Fail(label + " must be a non-empty string");
    } else if (value.size() > limit) {
      Fail(label + " exceeds " + std::to_string(limit) + " bytes");
    } else if (HasControlCharacter(value)) {
      Fail(label + " contains a control character");
    }
    return value;
  }

  auto Slug(const Json& object, const char* key, const std::string& label) -> std::string {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() ||
        found->get_ref<const std::string&>().size() > kSlugLimitBytes ||
        !MatchesIdentifier(found->get_ref<const std::string&>(), "._-")) {
      Fail(label + " must be a slug of at most " + std::to_string(kSlugLimitBytes) + " bytes");
      return {};
    }
    return found->get<std::string>();
  }

  auto Object(const Json& parent, const char* key) -> const Json* {
    const auto found = parent.find(key);
    if (found == parent.end() || !found->is_object()) {
      Fail(std::string(key) + " must be an object");
      return nullptr;
    }
    return &*found;
  }

 private:
  std::string message_;
};

auto Failure(LutHeaderError error, std::string message) -> LutHeaderReadResult {
  LutHeaderReadResult result;
  result.error   = error;
  result.message = std::move(message);
  return result;
}

/// Returns the JSON payload of a marker line, or std::nullopt for any other line.
auto MetadataPayload(std::string_view line) -> std::optional<std::string_view> {
  std::string_view text = Trim(line);
  if (text.empty() || text.front() != '#') return std::nullopt;
  text.remove_prefix(1);
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  if (text.substr(0, kMarker.size()) != kMarker) return std::nullopt;
  text.remove_prefix(kMarker.size());
  if (!text.empty() && text.front() != ' ' && text.front() != '\t') return std::nullopt;
  return Trim(text);
}

auto IsNumericRow(std::string_view line) -> bool {
  const std::string_view text = Trim(line);
  if (text.empty()) return false;
  const char first = text.front();
  return (first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.';
}

auto ParseSize(std::string_view token, int* output) -> bool {
  int        value        = 0;
  const auto end          = token.data() + token.size();
  const auto [ptr, error] = std::from_chars(token.data(), end, value);
  if (error != std::errc{} || ptr != end || value < 2) return false;
  *output = value;
  return true;
}

auto FirstToken(std::string_view text, std::string_view* rest) -> std::string_view {
  text              = Trim(text);
  std::size_t index = 0;
  while (index < text.size() && !IsSpace(text[index])) ++index;
  *rest = Trim(text.substr(index));
  return text.substr(0, index);
}

}  // namespace

auto ParseLutMetadataJson(std::string_view json_text, LutHeaderError* error, std::string* message)
    -> std::optional<LutMetadata> {
  const auto fail = [&](LutHeaderError kind, std::string text) -> std::optional<LutMetadata> {
    if (error) *error = kind;
    if (message) *message = std::move(text);
    return std::nullopt;
  };
  const Json root = Json::parse(json_text.begin(), json_text.end(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return fail(LutHeaderError::kInvalidMetadataJson, "ALCEDO_LUT JSON is not a valid object");
  }
  const auto schema = root.find("schema");
  if (schema == root.end() || !schema->is_number_integer() || schema->get<long long>() != 1) {
    return fail(LutHeaderError::kUnsupportedSchema, "ALCEDO_LUT schema is not supported");
  }

  FieldValidator check;
  LutMetadata    metadata;
  const auto     id = root.find("id");
  if (id == root.end() || !id->is_string() ||
      id->get_ref<const std::string&>().size() > kIdLimitBytes ||
      !MatchesIdentifier(id->get_ref<const std::string&>(), "._:-")) {
    check.Fail("id must match the identifier rule and be at most 160 bytes");
  } else {
    metadata.id = id->get<std::string>();
  }

  const auto origin = root.find("origin");
  if (origin != root.end() && *origin == "alcedo") {
    metadata.origin = LutOrigin::kAlcedo;
  } else if (origin != root.end() && *origin == "user") {
    metadata.origin = LutOrigin::kUser;
  } else {
    check.Fail("origin must be 'alcedo' or 'user'");
  }

  const auto category = root.find("category");
  if (category != root.end() && *category == "general") {
    metadata.category = LutCategory::kGeneral;
  } else if (category != root.end() && *category == "film_simulation") {
    metadata.category = LutCategory::kFilmSimulation;
  } else {
    check.Fail("category must be 'general' or 'film_simulation'");
  }
  const bool film_simulation = metadata.category == LutCategory::kFilmSimulation;

  if (root.contains("source") || film_simulation) {
    if (const Json* source = check.Object(root, "source")) {
      metadata.source = LutSourceInfo{check.Slug(*source, "id", "source.id"),
                                      check.Text(*source, "name", "source.name", kNameLimitBytes)};
    }
  }
  if (film_simulation) {
    if (const Json* film = check.Object(root, "film")) {
      metadata.film = LutFilmInfo{check.Slug(*film, "id", "film.id"),
                                  check.Text(*film, "name", "film.name", kNameLimitBytes),
                                  check.Text(*film, "brand", "film.brand", kNameLimitBytes)};
    }
    if (root.contains("print")) {
      if (const Json* print = check.Object(root, "print")) {
        LutPrintInfo value{check.Slug(*print, "id", "print.id"),
                           check.Text(*print, "name", "print.name", kNameLimitBytes),
                           check.Text(*print, "brand", "print.brand", kNameLimitBytes),
                           LutPrintKind::kFilm};
        const auto   kind = print->find("kind");
        if (kind != print->end() && *kind == "paper") {
          value.kind = LutPrintKind::kPaper;
        } else if (kind == print->end() || *kind != "film") {
          check.Fail("print.kind must be 'film' or 'paper'");
        }
        metadata.print = std::move(value);
      }
    }
  } else if (root.contains("film") || root.contains("print")) {
    check.Fail("film and print are not allowed on a general LUT");
  }

  if (root.contains("variant")) metadata.variant = check.Slug(root, "variant", "variant");
  metadata.input_space  = check.Text(root, "input_space", "input_space", kSpaceLimitBytes);
  metadata.output_space = check.Text(root, "output_space", "output_space", kSpaceLimitBytes);
  if (root.contains("description")) {
    metadata.description = check.Text(root, "description", "description", kDescriptionLimitBytes);
  }
  if (root.contains("display_requirement")) {
    metadata.display_requirement =
        check.Text(root, "display_requirement", "display_requirement", kDisplayRequirementBytes);
  }
  if (root.contains("aliases")) {
    const Json& aliases = root.at("aliases");
    if (!aliases.is_array() || aliases.size() > kAliasLimitCount) {
      check.Fail("aliases must be a list of at most 16 strings");
    } else {
      for (std::size_t index = 0; index < aliases.size(); ++index) {
        const Json wrapper = Json{{"alias", aliases[index]}};
        metadata.aliases.push_back(check.Text(
            wrapper, "alias", "aliases[" + std::to_string(index) + "]", kAliasLimitBytes));
      }
    }
  }

  if (check.Failed()) {
    return fail(LutHeaderError::kInvalidMetadataField, check.Message());
  }
  if (error) *error = LutHeaderError::kNone;
  return metadata;
}

auto SerializeLutMetadataJson(const LutMetadata& metadata) -> std::string {
  Json root      = Json::object();
  root["schema"] = 1;
  root["id"]     = metadata.id;
  root["origin"] = metadata.origin == LutOrigin::kAlcedo ? "alcedo" : "user";
  root["category"] =
      metadata.category == LutCategory::kFilmSimulation ? "film_simulation" : "general";
  if (metadata.source) {
    root["source"] = {{"id", metadata.source->id}, {"name", metadata.source->name}};
  }
  if (metadata.film) {
    root["film"] = {
        {"id", metadata.film->id}, {"name", metadata.film->name}, {"brand", metadata.film->brand}};
  }
  if (metadata.print) {
    root["print"] = {{"id", metadata.print->id},
                     {"name", metadata.print->name},
                     {"brand", metadata.print->brand},
                     {"kind", metadata.print->kind == LutPrintKind::kPaper ? "paper" : "film"}};
  }
  if (!metadata.variant.empty()) root["variant"] = metadata.variant;
  root["input_space"]  = metadata.input_space;
  root["output_space"] = metadata.output_space;
  if (!metadata.description.empty()) root["description"] = metadata.description;
  if (!metadata.display_requirement.empty()) {
    root["display_requirement"] = metadata.display_requirement;
  }
  if (!metadata.aliases.empty()) root["aliases"] = metadata.aliases;
  return root.dump();
}

auto ReadLutHeader(std::string_view bytes) -> LutHeaderReadResult {
  constexpr std::string_view kBom = "\xEF\xBB\xBF";
  if (bytes.substr(0, kBom.size()) == kBom) bytes.remove_prefix(kBom.size());

  LutHeaderReadResult result;
  bool                found_metadata = false;
  std::size_t         offset         = 0;
  while (offset < bytes.size()) {
    std::size_t end = bytes.find('\n', offset);
    if (end == std::string_view::npos) end = bytes.size();
    const std::string_view line = bytes.substr(offset, end - offset);
    if (IsNumericRow(line)) {
      return result;
    }
    if (end > kLutHeaderLimitBytes) {
      return Failure(LutHeaderError::kHeaderTooLarge, "CUBE header exceeds 64 KiB");
    }

    if (const auto payload = MetadataPayload(line)) {
      if (found_metadata) {
        return Failure(LutHeaderError::kDuplicateMetadata,
                       "CUBE header has more than one ALCEDO_LUT comment");
      }
      if (Trim(line).size() > kLutMetadataLineLimitBytes) {
        return Failure(LutHeaderError::kMetadataLineTooLarge, "ALCEDO_LUT comment exceeds 16 KiB");
      }
      LutHeaderError error = LutHeaderError::kNone;
      std::string    message;
      auto           metadata = ParseLutMetadataJson(*payload, &error, &message);
      if (!metadata) {
        return Failure(error, std::move(message));
      }
      result.header.metadata = std::move(metadata);
      found_metadata         = true;
    } else {
      std::string_view       rest;
      const std::string_view directive = FirstToken(line, &rest);
      if (directive == "LUT_3D_SIZE" || directive == "LUT_1D_SIZE") {
        int& target =
            directive == "LUT_3D_SIZE" ? result.header.lut_3d_size : result.header.lut_1d_size;
        if (!ParseSize(rest, &target)) {
          return Failure(LutHeaderError::kInvalidDirective,
                         std::string(directive) + " is not a valid size");
        }
      } else if (directive == "TITLE") {
        if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
          rest = rest.substr(1, rest.size() - 2);
        }
        result.header.title = std::string(rest);
      }
    }
    offset = end + 1;
  }
  return Failure(LutHeaderError::kNoNumericTable, "CUBE file has no numeric table");
}

auto ReadLutHeaderFile(const std::filesystem::path& path) -> LutHeaderReadResult {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Failure(LutHeaderError::kUnreadable, "CUBE file cannot be opened");
  }
  // One metadata line past the header limit is enough to decide every error case.
  std::string buffer(kLutHeaderLimitBytes + kLutMetadataLineLimitBytes, '\0');
  input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  if (input.bad()) {
    return Failure(LutHeaderError::kUnreadable, "CUBE file cannot be read");
  }
  buffer.resize(static_cast<std::size_t>(input.gcount()));
  return ReadLutHeader(buffer);
}

auto LutDisplayName(const LutHeader& header, std::string_view file_stem) -> std::string {
  if (header.metadata && header.metadata->origin == LutOrigin::kAlcedo && header.metadata->film) {
    return header.metadata->film->name;
  }
  return std::string(file_stem);
}

auto LutPrintOptionName(const LutHeader& header) -> std::string {
  if (header.metadata && header.metadata->origin == LutOrigin::kAlcedo && header.metadata->film &&
      header.metadata->print) {
    return header.metadata->print->name;
  }
  return {};
}

auto CanonicalLutFileStem(const LutMetadata& metadata) -> std::string {
  if (!metadata.film) return {};
  std::string stem = metadata.film->id;
  if (metadata.print) stem += "__" + metadata.print->id;
  if (!metadata.variant.empty()) stem += "__" + metadata.variant;
  return stem;
}

}  // namespace alcedo
