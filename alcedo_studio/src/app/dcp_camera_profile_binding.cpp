//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/dcp_camera_profile_binding.hpp"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "image/dcp_profile_library.hpp"
#include "image/dng_color_profile.hpp"
#include "utils/logging/log.hpp"

namespace alcedo {
namespace {

auto FolderText() -> std::string {
  const auto root = DcpProfileLibrary::Shared().Root();
  return root.empty() ? std::string("(no DCP folder is set)") : "\"" + root.string() + "\"";
}

auto IdentityText(const std::string& camera_model, const std::string& profile_name) -> std::string {
  return "camera profile \"" + profile_name + "\" for \"" + camera_model + "\"";
}

}  // namespace

auto FindSelectableDcpCameraProfile(const std::string& camera_model,
                                    const std::string& profile_name) -> DcpCameraProfilePtr {
  auto profile = DcpProfileLibrary::Shared().Find(camera_model, profile_name);
  if (!profile) {
    throw std::invalid_argument(IdentityText(camera_model, profile_name) +
                                " is not in the DCP folder " + FolderText());
  }
  return profile;
}

auto ResolveStoredDcpCameraProfileFile(const DevelopCameraProfileFile& stored)
    -> DevelopCameraProfileFile {
  DcpCameraProfilePtr profile;
  try {
    profile = DcpProfileLibrary::Shared().Find(stored.camera_model, stored.profile_name);
  } catch (const std::exception& error) {
    // The reference stays unbound, so the render fails with the named error; the read error is
    // the cause the user needs.
    ALCEDO_LOG_ERROR(Decode) << IdentityText(stored.camera_model, stored.profile_name)
                             << " cannot be read: " << error.what();
  }
  if (!profile) {
    ALCEDO_LOG_WARN(Decode) << IdentityText(stored.camera_model, stored.profile_name)
                            << " is not in the DCP folder " << FolderText()
                            << "; the render of this image fails until the profile is available "
                               "or another profile is selected.";
    DevelopCameraProfileFile unbound = stored;
    unbound.profile                  = nullptr;
    return unbound;
  }
  if (profile->fingerprint != stored.fingerprint) {
    ALCEDO_LOG_WARN(Decode) << IdentityText(stored.camera_model, stored.profile_name)
                            << " changed in the DCP folder ("
                            << DngColorProfileFingerprintToText(stored.fingerprint) << " -> "
                            << DngColorProfileFingerprintToText(profile->fingerprint)
                            << "); the DCP folder file is used.";
  }
  return MakeDevelopCameraProfileFile(std::move(profile));
}

void BindDcpCameraProfile(PipelineDocument& document) {
  auto* develop = document.Develop();
  if (develop == nullptr) {
    return;
  }
  const auto stored = develop->Params().CameraProfileFile();
  if (!stored.has_value() || stored->IsBound()) {
    return;
  }
  auto resolved = ResolveStoredDcpCameraProfileFile(*stored);
  // An unresolved identity keeps the stored reference unbound.
  if (resolved.IsBound()) {
    develop->Params().ApplyCameraProfileFileUpdate({std::move(resolved.profile), std::nullopt});
  }
}

}  // namespace alcedo
