//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/pipeline_document_checkpoint.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/history/edit_commit.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

void RequireObject(const nlohmann::json& json, std::string_view context) {
  if (!json.is_object()) {
    Fail(std::string{context} + ": expected object");
  }
}

void RequireExactObjectKeys(const nlohmann::json& json, std::initializer_list<const char*> keys,
                            std::string_view context) {
  RequireObject(json, context);
  if (json.size() != keys.size()) {
    Fail(std::string{context} + ": unexpected field count");
  }
  for (const char* key : keys) {
    if (!json.contains(key)) {
      Fail(std::string{context} + ": missing required field '" + key + "'");
    }
  }
}

auto RequireUint32(const nlohmann::json& json, const char* key, std::uint32_t expected,
                   std::string_view context) -> std::uint32_t {
  const auto& value = json.at(key);
  if (!value.is_number_unsigned() && !value.is_number_integer()) {
    Fail(std::string{context} + ": '" + key + "' must be an integer");
  }
  const auto parsed = value.get<std::uint32_t>();
  if (parsed != expected) {
    Fail(std::string{context} + ": unsupported " + key);
  }
  return parsed;
}

auto RequireString(const nlohmann::json& json, const char* key, std::string_view context)
    -> std::string {
  if (!json.at(key).is_string()) {
    Fail(std::string{context} + ": '" + key + "' must be a string");
  }
  return json.at(key).get<std::string>();
}

void AppendBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  out.insert(out.end(), bytes, bytes + size);
}

void AppendU32LE(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void RequireCanonicalDocument(const nlohmann::json& stored, const PipelineDocument& document,
                              std::string_view context) {
  const auto canonical = document.ToJson();
  if (stored.dump() != canonical.dump()) {
    Fail(std::string{context} + ": nested pipeline document is not in canonical form");
  }
}

void ValidateDocument(const PipelineDocument& document, std::string_view context) {
  if (document.FormatVersion() != kPipelineDocumentFormatVersion) {
    Fail(std::string{context} + ": unsupported pipeline document format_version");
  }
  const auto graph_errors = document.Graph().Validate();
  if (!graph_errors.empty()) {
    Fail(std::string{context} + ": " + graph_errors.front().message);
  }
  const auto backbone_errors = document.Graph().ValidateImageBackbone();
  if (!backbone_errors.empty()) {
    Fail(std::string{context} + ": " + backbone_errors.front().message);
  }
}

auto CanonicalRawColorDump(const std::optional<nlohmann::json>& raw_color_context) -> std::string {
  if (!raw_color_context.has_value() || raw_color_context->is_null()) {
    return "null";
  }
  return raw_color_context->dump();
}

}  // namespace

auto ComputeRootId(sl_element_id_t element_id, const PipelineDocument& document,
                   const std::optional<nlohmann::json>& raw_color_context) -> root_id_t {
  ValidateDocument(document, "ComputeRootId");
  constexpr std::string_view kDomain = "alcedo.pipeline_root.v2";
  const auto                 document_dump = document.ToJson().dump();
  const auto                 raw_dump      = CanonicalRawColorDump(raw_color_context);

  std::vector<std::uint8_t> bytes;
  AppendBytes(bytes, kDomain.data(), kDomain.size());
  AppendU32LE(bytes, kRootStateFormatVersion);
  AppendU32LE(bytes, kPipelineDocumentFormatVersion);
  AppendU32LE(bytes, kImageEditSchemaVersion);
  AppendU32LE(bytes, element_id);
  AppendBytes(bytes, document_dump.data(), document_dump.size());
  bytes.push_back(0);
  AppendBytes(bytes, raw_dump.data(), raw_dump.size());
  return Hash128::Compute(bytes.data(), bytes.size());
}

auto EncodePipelineRootState(sl_element_id_t element_id, const PipelineDocument& document,
                             const std::optional<nlohmann::json>& raw_color_context)
    -> nlohmann::json {
  ValidateDocument(document, "EncodePipelineRootState");
  nlohmann::json encoded{
      {"root_state_format_version", kRootStateFormatVersion},
      {"project_schema_version", kImageEditSchemaVersion},
      {"pipeline_document_format_version", kPipelineDocumentFormatVersion},
      {"element_id", element_id},
      {"pipeline_document", document.ToJson()},
  };
  encoded["raw_color_context"] =
      raw_color_context.has_value() ? *raw_color_context : nlohmann::json(nullptr);
  return encoded;
}

auto DecodePipelineRootState(const nlohmann::json& json) -> PipelineRootState {
  RequireExactObjectKeys(json,
                         {"root_state_format_version", "project_schema_version",
                          "pipeline_document_format_version", "element_id", "pipeline_document",
                          "raw_color_context"},
                         "PipelineRootState");
  RequireUint32(json, "root_state_format_version", kRootStateFormatVersion, "PipelineRootState");
  RequireUint32(json, "project_schema_version", kImageEditSchemaVersion, "PipelineRootState");
  RequireUint32(json, "pipeline_document_format_version", kPipelineDocumentFormatVersion,
                "PipelineRootState");
  if (!json.at("element_id").is_number_unsigned() && !json.at("element_id").is_number_integer()) {
    Fail("PipelineRootState: element_id must be an integer");
  }

  PipelineRootState state;
  state.element_id = json.at("element_id").get<sl_element_id_t>();
  if (state.element_id == 0) {
    Fail("PipelineRootState: element_id must be non-zero");
  }
  const auto& document_json = json.at("pipeline_document");
  state.document            = PipelineDocument::FromJson(document_json);
  RequireCanonicalDocument(document_json, state.document, "PipelineRootState");
  ValidateDocument(state.document, "PipelineRootState");

  const auto& raw = json.at("raw_color_context");
  if (raw.is_null()) {
    state.raw_color_context = std::nullopt;
  } else if (raw.is_object()) {
    state.raw_color_context = raw;
  } else {
    Fail("PipelineRootState: raw_color_context must be an object or null");
  }
  return state;
}

auto EncodePipelineDocumentCheckpoint(const root_id_t& root_id, head_commit_hash_t head,
                                      const transaction_chain_hash_t& chain,
                                      const PipelineDocument&         document) -> nlohmann::json {
  ValidateDocument(document, "EncodePipelineDocumentCheckpoint");
  return nlohmann::json{
      {"checkpoint_state_format_version", kCheckpointStateFormatVersion},
      {"project_schema_version", kImageEditSchemaVersion},
      {"pipeline_document_format_version", kPipelineDocumentFormatVersion},
      {"root_id", root_id.ToString()},
      {"head_commit_hash", HeadCommitHashToStorage(head)},
      {"transaction_chain_hash", chain.ToString()},
      {"pipeline_document", document.ToJson()},
  };
}

auto DecodePipelineDocumentCheckpoint(const nlohmann::json& json) -> PipelineDocumentCheckpoint {
  RequireExactObjectKeys(json,
                         {"checkpoint_state_format_version", "project_schema_version",
                          "pipeline_document_format_version", "root_id", "head_commit_hash",
                          "transaction_chain_hash", "pipeline_document"},
                         "PipelineDocumentCheckpoint");
  RequireUint32(json, "checkpoint_state_format_version", kCheckpointStateFormatVersion,
                "PipelineDocumentCheckpoint");
  RequireUint32(json, "project_schema_version", kImageEditSchemaVersion,
                "PipelineDocumentCheckpoint");
  RequireUint32(json, "pipeline_document_format_version", kPipelineDocumentFormatVersion,
                "PipelineDocumentCheckpoint");

  PipelineDocumentCheckpoint checkpoint;
  checkpoint.root_id = Hash128::FromString(RequireString(json, "root_id", "PipelineDocumentCheckpoint"));
  checkpoint.head_commit_hash = HeadCommitHashFromStorage(
      RequireString(json, "head_commit_hash", "PipelineDocumentCheckpoint"));
  checkpoint.transaction_chain_hash = Hash128::FromString(
      RequireString(json, "transaction_chain_hash", "PipelineDocumentCheckpoint"));
  const auto& document_json = json.at("pipeline_document");
  checkpoint.document       = PipelineDocument::FromJson(document_json);
  RequireCanonicalDocument(document_json, checkpoint.document, "PipelineDocumentCheckpoint");
  ValidateDocument(checkpoint.document, "PipelineDocumentCheckpoint");
  return checkpoint;
}

auto IsPipelineDocumentCheckpointJson(const nlohmann::json& json) -> bool {
  return json.is_object() && json.contains("checkpoint_state_format_version") &&
         json.contains("pipeline_document");
}

}  // namespace alcedo
