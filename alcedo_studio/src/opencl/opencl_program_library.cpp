//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/opencl_program_library.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "opencl/opencl_api_counters.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace detail {

auto FormatPathForOpenClDiagnostics(const std::filesystem::path& path) -> std::string {
#if defined(_WIN32)
  return conv::ToBytes(path.wstring());
#else
  return path.string();
#endif
}

auto IsRegularFile(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec && std::filesystem::is_regular_file(path, ec) &&
         !ec;
}

auto GetExecutableDir() -> std::filesystem::path {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD copied =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < buffer.size()) {
      buffer.resize(copied);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  return {};
#endif
}

auto RelativeOpenClSourcePath(const std::filesystem::path& path) -> std::filesystem::path {
#ifdef ALCEDO_OPENCL_SHADER_SOURCE_ROOT
  const auto root = std::filesystem::path(ALCEDO_OPENCL_SHADER_SOURCE_ROOT).lexically_normal();
  const auto rel  = path.lexically_normal().lexically_relative(root);
  if (!rel.empty() && !rel.is_absolute()) {
    const auto first = *rel.begin();
    if (first != "..") {
      return rel;
    }
  }
#endif
  return {};
}

auto ResolveOpenClSourcePath(const std::filesystem::path& path) -> std::filesystem::path {
  if (IsRegularFile(path)) {
    return path;
  }

  const auto rel = RelativeOpenClSourcePath(path);
  const auto exe_dir = GetExecutableDir();
  if (rel.empty() || exe_dir.empty()) {
    return path;
  }

  const std::vector<std::filesystem::path> candidates = {
      exe_dir / "opencl" / rel,
      exe_dir / "Resources" / "opencl" / rel,
  };
  for (const auto& candidate : candidates) {
    if (IsRegularFile(candidate)) {
      return candidate;
    }
  }
  return path;
}

}  // namespace detail

OpenClProgramLibrary::~OpenClProgramLibrary() {
  for (auto& [_, slot] : programs_) {
    if (slot && slot->program != nullptr) {
      clReleaseProgram(slot->program);
      slot->program = nullptr;
    }
  }
}

auto OpenClProgramLibrary::Instance() -> OpenClProgramLibrary& {
  static OpenClProgramLibrary library;
  return library;
}

auto OpenClProgramLibrary::GetProgramSlot(const std::string& name) -> std::shared_ptr<ProgramSlot> {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto                  it = programs_.find(name);
  if (it == programs_.end()) {
    throw std::runtime_error("OpenClProgramLibrary: program is not registered: " + name);
  }
  return it->second;
}

auto OpenClProgramLibrary::LoadSourceText(const std::vector<std::filesystem::path>& source_paths)
    -> std::vector<std::string> {
  if (source_paths.empty()) {
    throw std::runtime_error("OpenClProgramLibrary: no source paths were registered.");
  }

  std::vector<std::string> sources;
  sources.reserve(source_paths.size());
  for (const auto& path : source_paths) {
    const auto    resolved_path = detail::ResolveOpenClSourcePath(path);
    std::ifstream file(resolved_path, std::ios::binary);
    if (!file.is_open()) {
      throw std::runtime_error("OpenClProgramLibrary: failed to open source file: " +
                               detail::FormatPathForOpenClDiagnostics(resolved_path));
    }
    sources.emplace_back(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  return sources;
}

auto OpenClProgramLibrary::GetBuildLog(cl_program program, cl_device_id device) -> std::string {
  size_t log_size = 0;
  cl_int error =
      clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
  if (error != CL_SUCCESS || log_size == 0) {
    return {};
  }

  std::string build_log(log_size, '\0');
  error = clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, build_log.data(),
                                nullptr);
  if (error != CL_SUCCESS) {
    return {};
  }
  if (!build_log.empty() && build_log.back() == '\0') {
    build_log.pop_back();
  }
  return build_log;
}

auto OpenClProgramLibrary::BuildProgram(const std::shared_ptr<ProgramSlot>& slot) -> cl_program {
  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    throw std::runtime_error("OpenClProgramLibrary: OpenCL context is not initialized.");
  }

  const auto& capabilities = context.Capabilities();
  const std::string build_options = slot->descriptor.build_options.empty()
                                      ? std::string("<none>")
                                      : slot->descriptor.build_options;
  const auto build_diagnostic_prefix = [&]() {
    std::ostringstream message;
    message << "OpenClProgramLibrary: program='" << slot->descriptor.name << "'"
            << ", device='" << capabilities.name << "'"
            << ", driver='" << capabilities.driver_version << "'"
            << ", build_options='" << build_options << "'";
    return message.str();
  };

  const auto               sources = LoadSourceText(slot->descriptor.source_paths);

  std::vector<const char*> source_ptrs;
  std::vector<size_t>      source_lengths;
  source_ptrs.reserve(sources.size());
  source_lengths.reserve(sources.size());
  for (const auto& source : sources) {
    source_ptrs.push_back(source.c_str());
    source_lengths.push_back(source.size());
  }

  cl_int     error = CL_SUCCESS;
  cl_program program =
      clCreateProgramWithSource(context.Context(), static_cast<cl_uint>(source_ptrs.size()),
                                source_ptrs.data(), source_lengths.data(), &error);
  if (error != CL_SUCCESS || program == nullptr) {
    throw std::runtime_error(build_diagnostic_prefix() +
                             ": failed to create program (OpenCL error " +
                             std::to_string(error) + ").");
  }

  const char* build_options_arg =
      slot->descriptor.build_options.empty() ? nullptr : slot->descriptor.build_options.c_str();
  const cl_device_id device = context.Device();
  error = clBuildProgram(program, 1, &device, build_options_arg, nullptr, nullptr);
  NoteOpenClProgramBuild();
  if (error != CL_SUCCESS) {
    const auto build_log = GetBuildLog(program, device);
    clReleaseProgram(program);

    std::ostringstream message;
    message << build_diagnostic_prefix() << ": failed to build program (OpenCL error " << error
            << ").";
    if (!build_log.empty()) {
      message << "\n" << build_log;
    }
    throw std::runtime_error(message.str());
  }

  return program;
}

void OpenClProgramLibrary::RegisterProgram(OpenClProgramDescriptor descriptor) {
  if (descriptor.name.empty()) {
    throw std::runtime_error("OpenClProgramLibrary: program name must not be empty.");
  }
  if (descriptor.source_paths.empty()) {
    throw std::runtime_error("OpenClProgramLibrary: program must have at least one source path.");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (programs_.contains(descriptor.name)) {
    throw std::runtime_error("OpenClProgramLibrary: duplicate program registration: " +
                             descriptor.name);
  }

  auto slot        = std::make_shared<ProgramSlot>();
  slot->descriptor = std::move(descriptor);
  programs_.emplace(slot->descriptor.name, std::move(slot));
}

void OpenClProgramLibrary::WarmUpRequiredPrograms() {
  std::vector<std::string> required_programs;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    required_programs.reserve(programs_.size());
    for (const auto& [name, slot] : programs_) {
      if (slot && slot->descriptor.required_at_startup) {
        required_programs.push_back(name);
      }
    }
  }

  for (const auto& program_name : required_programs) {
    (void)GetProgram(program_name);
  }
}

void OpenClProgramLibrary::WarmUpAllPrograms() {
  const auto program_names = RegisteredProgramNames();
  for (const auto& program_name : program_names) {
    (void)GetProgram(program_name);
  }
}

auto OpenClProgramLibrary::GetProgram(std::string_view name) -> cl_program {
  auto                         slot = GetProgramSlot(std::string(name));

  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (slot->program != nullptr) {
      return slot->program;
    }
    if (slot->error) {
      std::rethrow_exception(slot->error);
    }
    if (!slot->is_building) {
      slot->is_building = true;
      break;
    }
    slot->cv.wait(lock);
  }
  lock.unlock();

  try {
    cl_program program = BuildProgram(slot);
    lock.lock();
    slot->program     = program;
    slot->is_building = false;
    slot->cv.notify_all();
    return slot->program;
  } catch (...) {
    lock.lock();
    slot->error       = std::current_exception();
    slot->is_building = false;
    slot->cv.notify_all();
    throw;
  }
}

auto OpenClProgramLibrary::IsProgramBuilt(std::string_view name) const -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto                  it = programs_.find(std::string(name));
  if (it == programs_.end() || !it->second) {
    return false;
  }
  return it->second->program != nullptr;
}

auto OpenClProgramLibrary::RegisteredProgramNames() const -> std::vector<std::string> {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string>    names;
  names.reserve(programs_.size());
  for (const auto& [name, _] : programs_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace alcedo

#endif
