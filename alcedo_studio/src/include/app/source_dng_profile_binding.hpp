//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>

#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/dng_color_profile.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Binding of DNG color profiles that project data references by fingerprint.
 *
 * Project tables store only the profile fingerprint (DngColorProfileRef). These functions read
 * the profile tables from the source file of an element through DngColorProfileCache::Shared()
 * and bind them before a loaded document or RAW color context is used for rendering.
 *
 * All functions read storage (Element, FileImage, and Image rows) to find the source file.
 * The caller must not hold a database connection lock of @p storage.
 */

/// Source file of the image bound to element @p id.
/// @throws std::runtime_error when the element is not a file with a stored Image row.
[[nodiscard]] auto SourceImagePath(Storage& storage, sl_element_id_t id) -> std::filesystem::path;

/**
 * @brief Profile for @p reference, read from the source file of element @p id when unbound.
 *
 * Returns the bound profile of a bound reference and null for an empty reference, without a
 * storage read. When the source file now has another fingerprint than @p reference, the source
 * file profile is returned and one warning is written to the error log.
 *
 * @throws std::runtime_error when the source file is missing or has no DNG profile.
 */
[[nodiscard]] auto LoadSourceDngColorProfile(Storage& storage, sl_element_id_t id,
                                             const DngColorProfileRef& reference)
    -> DngColorProfilePtr;

/// Bind the referenced profile of the Develop node of @p document, which is not live yet.
/// No-op when the document has no Develop node or its reference is empty or already bound.
void BindSourceDngColorProfile(Storage& storage, sl_element_id_t id, PipelineDocument& document);

/// Bind the referenced profile of a RAW color context read from project data.
void BindSourceDngColorProfile(Storage& storage, sl_element_id_t id,
                               RawRuntimeColorContext& context);

}  // namespace alcedo
