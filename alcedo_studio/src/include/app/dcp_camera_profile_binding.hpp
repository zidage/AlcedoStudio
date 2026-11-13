//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/dcp_camera_profile.hpp"

namespace alcedo {

/**
 * @brief Resolution of the `camera_profile_file` reference through the DCP library.
 *
 * The project stores only the reference (camera model, profile name, fingerprint). These
 * functions read the profile from DcpProfileLibrary::Shared(), the library that the Settings page
 * scans (dcp_camera_profile_plan.md, phase C4). A lookup waits for a running scan. A first lookup
 * of an identity reads the DCP file on the calling thread; the library cache keeps the profile.
 *
 * Thread safety: safe from any thread. The document functions change only the document that the
 * caller owns.
 */

/**
 * @brief The library profile for a user selection.
 *
 * @throws std::invalid_argument when the library has no profile with this identity. The text
 *         names the profile, the camera model, and the DCP folder.
 * @throws std::runtime_error when the DCP file cannot be read.
 */
[[nodiscard]] auto FindSelectableDcpCameraProfile(const std::string& camera_model,
                                                  const std::string& profile_name)
    -> DcpCameraProfilePtr;

/**
 * @brief The reference that a stored value of @p stored gives in a live document.
 *
 * - The library holds the identity: a bound reference with the library profile. When the library
 *   file has another fingerprint than @p stored, the library file wins and one warning names both
 *   fingerprints (plan section 2.3).
 * - The library does not hold the identity, or its file cannot be read: @p stored, unbound. The
 *   log names the identity, the DCP folder, and the read error. The render then fails with
 *   `UnboundCameraProfileFile`; no other profile applies (plan decision P-4).
 *
 * The fingerprint of @p stored is compared only; the identity selects the profile. The result keeps
 * the `profile_tone` value of @p stored.
 */
[[nodiscard]] auto ResolveStoredDcpCameraProfileFile(const DevelopCameraProfileFile& stored)
    -> DevelopCameraProfileFile;

/**
 * @brief Add the source tables that @p update needs for an image with the DNG profile @p source.
 *
 * For an update with a profile and a referenced @p source, sets the update's source tables to
 * DcpProfileLibrary::Shared().SourceTables(profile, source) and its source fingerprint to the
 * fingerprint of @p source (plan phase C6). Otherwise @p update stays unchanged, and a RAW source
 * without an embedded profile uses the DCP tables.
 *
 * @throws std::runtime_error when @p source is referenced but not bound (DngColorProfileRef::
 *         RequireBound); rethrows merge errors. @p update is unchanged then.
 */
void AddDcpSourceTables(DevelopCameraProfileFileUpdate& update, const DngColorProfileRef& source);

/**
 * @brief Bind the stored DCP reference of the Develop node of @p document.
 *
 * No-op when the document has no Develop node or no reference. An unbound reference takes the
 * profile of @ref ResolveStoredDcpCameraProfileFile; a bound one keeps its profile. The source
 * tables then follow the current DNG profile of the document (@ref AddDcpSourceTables), so this
 * runs after the import binding, which can change that profile. The update goes through
 * DevelopParamsModel::ApplyCameraProfileFileUpdate, which solves the as-shot values again from the
 * DCP matrices and marks nothing dirty when the binding is the same. An identity that the library
 * does not hold leaves the reference unbound.
 *
 * @pre @p document is not live yet (a document built from the root or a checkpoint).
 * @throws std::runtime_error when the document references a DNG profile that is not bound.
 */
void BindDcpCameraProfile(PipelineDocument& document);

}  // namespace alcedo
