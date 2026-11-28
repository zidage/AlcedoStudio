//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/image/source_file_mapper.hpp"

#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "storage/mapper/utc_timestamp_text.hpp"

namespace alcedo {

void SourceFileMapper::ClearChangeMarks(std::span<const sl_element_id_t> element_ids) {
  if (!element_ids.empty()) {
    duckorm::remove(conn_, "SourceFileChanged",
                    duckorm::expr::in_list(duckorm::expr::col("element_id"), element_ids));
  }
}

void SourceFileMapper::RemoveByIds(std::span<const sl_element_id_t> element_ids) {
  ClearChangeMarks(element_ids);
  Mapper::RemoveByIds(element_ids);
}

void SourceFileMapper::RemoveById(const sl_element_id_t& element_id) {
  RemoveByIds(std::span(&element_id, 1));
}

auto SourceFileMapper::FromRawData(std::vector<duckorm::VarTypes>&& data)
    -> SourceFileMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("SourceFileMapper: the row does not have the SourceFile columns");
  }
  const auto* element_id          = std::get_if<uint32_t>(&data[0]);
  const auto* byte_size           = std::get_if<int64_t>(&data[1]);
  const auto* modified_ns         = std::get_if<int64_t>(&data[2]);
  auto*       fingerprint         = std::get_if<std::unique_ptr<std::string>>(&data[3]);
  const auto* fingerprint_version = std::get_if<int32_t>(&data[4]);
  const auto* state               = std::get_if<int32_t>(&data[5]);
  auto*       missing_since       = std::get_if<std::unique_ptr<std::string>>(&data[6]);
  if (element_id == nullptr || byte_size == nullptr || modified_ns == nullptr ||
      fingerprint == nullptr || *fingerprint == nullptr || fingerprint_version == nullptr ||
      state == nullptr || missing_since == nullptr) {
    throw std::runtime_error("SourceFileMapper: a SourceFile cell has an unexpected type");
  }
  return {*element_id,
          *byte_size,
          *modified_ns,
          std::move(**fingerprint),
          *fingerprint_version,
          *state,
          *missing_since ? std::move(**missing_since) : std::string{}};
}

auto SourceFileMapper::ToParams(const SourceFileRecord& source) -> SourceFileMapperParams {
  if (source.byte_size > static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max())) {
    throw std::invalid_argument(std::format(
        "SourceFileMapper: the byte size {} of element {} does not fit the SourceFile row",
        source.byte_size, source.element_id));
  }
  if (source.fingerprint_version >
      static_cast<std::uint32_t>(std::numeric_limits<int32_t>::max())) {
    throw std::invalid_argument(
        std::format("SourceFileMapper: the fingerprint version {} of element {} does not fit the "
                    "SourceFile row",
                    source.fingerprint_version, source.element_id));
  }
  const bool missing = source.state == SourceFileState::kMissing;
  if (missing != source.missing_since.has_value()) {
    throw std::invalid_argument(std::format(
        "SourceFileMapper: the record of element {} needs a missing time exactly when its state "
        "is missing",
        source.element_id));
  }
  return {source.element_id,
          static_cast<int64_t>(source.byte_size),
          source.modified_ns,
          source.fingerprint.ToString(),
          static_cast<int32_t>(source.fingerprint_version),
          static_cast<int32_t>(source.state),
          missing ? FormatUtcTimestampText(*source.missing_since) : std::string{}};
}

auto SourceFileMapper::FromParams(SourceFileMapperParams&& param) -> SourceFileRecord {
  if (param.byte_size < 0 || param.fingerprint_version < 0) {
    throw std::runtime_error(std::format(
        "SourceFileMapper: the SourceFile row of element {} has a negative byte size or "
        "fingerprint version",
        param.element_id));
  }
  SourceFileRecord record;
  record.element_id          = param.element_id;
  record.byte_size           = static_cast<std::uint64_t>(param.byte_size);
  record.modified_ns         = param.modified_ns;
  record.fingerprint_version = static_cast<std::uint32_t>(param.fingerprint_version);
  try {
    record.fingerprint = Hash128::FromString(param.fingerprint);
  } catch (const std::exception& error) {
    throw std::runtime_error(
        std::format("SourceFileMapper: the SourceFile row of element {} has the fingerprint "
                    "'{}', which is not 32 hexadecimal digits ({})",
                    param.element_id, param.fingerprint, error.what()));
  }
  switch (param.state) {
    case static_cast<int32_t>(SourceFileState::kPresent):
      record.state = SourceFileState::kPresent;
      break;
    case static_cast<int32_t>(SourceFileState::kMissing):
      record.state = SourceFileState::kMissing;
      break;
    default:
      throw std::runtime_error(
          std::format("SourceFileMapper: the SourceFile row of element {} has the unknown state {}",
                      param.element_id, param.state));
  }
  if ((record.state == SourceFileState::kMissing) == param.missing_since.empty()) {
    throw std::runtime_error(std::format(
        "SourceFileMapper: the SourceFile row of element {} has a missing time that does not "
        "agree with its state {}",
        param.element_id, param.state));
  }
  if (!param.missing_since.empty()) {
    record.missing_since = ParseUtcTimestampText(param.missing_since);
  }
  return record;
}

}  // namespace alcedo
