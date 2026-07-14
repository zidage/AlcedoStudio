//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/opencl_backend_program_registry.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace alcedo {

void RegisterOpenClRawProcessorPrograms();
void RegisterOpenClEditPipelinePrograms();
void RegisterOpenClGeometryPrograms();
void RegisterOpenClScopePrograms();
void RegisterOpenClDemosaicNetPrograms();

namespace {

void RegisterBuiltinOpenClProgramManifests() {
  static std::once_flag once;
  std::call_once(once, [] {
    // OpenCL backend modules add their long-lived program manifests here.
    // This file is intentionally the app/project lifecycle aggregation point;
    // short-lived RAW processor or pipeline instances should not register
    // programs directly.
    RegisterOpenClRawProcessorPrograms();
    RegisterOpenClEditPipelinePrograms();
    RegisterOpenClGeometryPrograms();
    RegisterOpenClScopePrograms();
    RegisterOpenClDemosaicNetPrograms();
  });
}

}  // namespace

auto OpenClBackendProgramRegistry::Instance() -> OpenClBackendProgramRegistry& {
  static OpenClBackendProgramRegistry registry;
  return registry;
}

void OpenClBackendProgramRegistry::RegisterManifest(OpenClProgramManifest manifest) {
  if (manifest.name.empty()) {
    throw std::runtime_error("OpenClBackendProgramRegistry: manifest name must not be empty.");
  }
  if (manifest.programs.empty()) {
    throw std::runtime_error("OpenClBackendProgramRegistry: manifest must contain programs: " +
                             manifest.name);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (manifests_.contains(manifest.name)) {
    throw std::runtime_error("OpenClBackendProgramRegistry: duplicate manifest registration: " +
                             manifest.name);
  }

  auto slot                = ManifestSlot{};
  slot.manifest            = std::move(manifest);
  slot.registered          = false;
  const auto manifest_name = slot.manifest.name;
  manifests_.emplace(manifest_name, std::move(slot));
}

void OpenClBackendProgramRegistry::RegisterProgramsForManifest(std::string_view manifest_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto                  it = manifests_.find(std::string(manifest_name));
  if (it == manifests_.end()) {
    throw std::runtime_error("OpenClBackendProgramRegistry: manifest is not registered: " +
                             std::string(manifest_name));
  }
  if (it->second.registered) {
    return;
  }

  for (auto descriptor : it->second.manifest.programs) {
    OpenClProgramLibrary::Instance().RegisterProgram(std::move(descriptor));
  }
  it->second.registered = true;
}

void OpenClBackendProgramRegistry::RegisterAllPrograms() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [_, slot] : manifests_) {
    if (slot.registered) {
      continue;
    }
    for (auto descriptor : slot.manifest.programs) {
      OpenClProgramLibrary::Instance().RegisterProgram(std::move(descriptor));
    }
    slot.registered = true;
  }
}

auto OpenClBackendProgramRegistry::RegisteredManifestNames() const -> std::vector<std::string> {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string>    names;
  names.reserve(manifests_.size());
  for (const auto& [name, _] : manifests_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

void RegisterOpenClBackendPrograms() {
  RegisterBuiltinOpenClProgramManifests();
  OpenClBackendProgramRegistry::Instance().RegisterAllPrograms();
}

}  // namespace alcedo

#endif
