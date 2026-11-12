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
 * The fingerprint of @p stored is compared only; the identity selects the profile.
 */
[[nodiscard]] auto ResolveStoredDcpCameraProfileFile(const DevelopCameraProfileFile& stored)
    -> DevelopCameraProfileFile;

/**
 * @brief Bind the stored DCP reference of the Develop node of @p document.
 *
 * No-op when the document has no Develop node, no reference, or a bound reference. Otherwise the
 * reference takes the result of @ref ResolveStoredDcpCameraProfileFile through
 * DevelopParamsModel::ApplyCameraProfileFileUpdate, which solves the as-shot values again from
 * the DCP matrices. An identity that the library does not hold leaves the reference unbound.
 *
 * @pre @p document is not live yet (a document built from the root or a checkpoint).
 */
void BindDcpCameraProfile(PipelineDocument& document);

}  // namespace alcedo
