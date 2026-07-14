//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "opencl/opencl_context.hpp"

namespace alcedo {

namespace detail {
auto FormatPathForOpenClDiagnostics(const std::filesystem::path& path) -> std::string;
}

struct OpenClProgramDescriptor {
  std::string                        name;
  std::vector<std::filesystem::path> source_paths;
  std::string                        build_options;
  bool                               required_at_startup = false;
};

class OpenClProgramLibrary {
 private:
  struct ProgramSlot {
    OpenClProgramDescriptor descriptor;
    cl_program              program     = nullptr;
    bool                    is_building = false;
    std::exception_ptr      error;
    std::condition_variable cv;
  };

  mutable std::mutex                                            mutex_;
  std::unordered_map<std::string, std::shared_ptr<ProgramSlot>> programs_;

  OpenClProgramLibrary() = default;

  ~OpenClProgramLibrary();

  auto        GetProgramSlot(const std::string& name) -> std::shared_ptr<ProgramSlot>;
  static auto LoadSourceText(const std::vector<std::filesystem::path>& source_paths)
      -> std::vector<std::string>;
  static auto GetBuildLog(cl_program program, cl_device_id device) -> std::string;
  auto        BuildProgram(const std::shared_ptr<ProgramSlot>& slot) -> cl_program;

 public:
  OpenClProgramLibrary(const OpenClProgramLibrary&)                      = delete;
  auto operator=(const OpenClProgramLibrary&) -> OpenClProgramLibrary&   = delete;
  OpenClProgramLibrary(OpenClProgramLibrary&&)                           = delete;
  auto        operator=(OpenClProgramLibrary&&) -> OpenClProgramLibrary& = delete;

  static auto Instance() -> OpenClProgramLibrary&;

  // Registration stays backend-local. Higher layers should trigger warm-up,
  // but should not know how OpenCL programs are sourced or built.
  void        RegisterProgram(OpenClProgramDescriptor descriptor);

  void        WarmUpRequiredPrograms();
  void        WarmUpAllPrograms();

  auto        GetProgram(std::string_view name) -> cl_program;

  // True when the named program is registered and has already been built.
  // Does not trigger compilation. Used by lifecycle tests and diagnostics.
  auto        IsProgramBuilt(std::string_view name) const -> bool;

  auto        RegisteredProgramNames() const -> std::vector<std::string>;
};

}  // namespace alcedo

#endif
