//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Builds OpenCL programs through AMD's Windows driver runtime (amdocl64.dll) on an offline
// device, so AMD compiler crashes can be reproduced on a machine without an AMD GPU.
// The API sequence matches OpenClProgramLibrary::BuildProgram: clCreateProgramWithSource with
// one string per source file, then clBuildProgram with the program's build options.
//
// Usage: ocl_probe <amdocl_dir> <device> <threads> <manifest>
//   manifest: one program per line, "name<TAB>build options<TAB>file1;file2;..."
// Exit code 0 when every program builds, 1 when a build fails, 0xC0000005 when the driver's
// compiler crashes (the crash handler prints the faulting module and offset first).
#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <CL/cl_ext.h>
#include <CL/cl_icd.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef CL_CONTEXT_OFFLINE_DEVICES_AMD
#define CL_CONTEXT_OFFLINE_DEVICES_AMD 0x403F
#endif

namespace {

// Every object of an ICD driver starts with its dispatch table.
struct IcdObject {
  cl_icd_dispatch* dispatch;
};

auto Dispatch(void* object) -> cl_icd_dispatch* {
  return reinterpret_cast<IcdObject*>(object)->dispatch;
}

struct Program {
  std::string              name;
  std::string              options;
  std::vector<std::string> sources;
};

auto ReadFile(const std::string& path) -> std::string {
  std::ifstream     file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

auto WINAPI ReportCrash(EXCEPTION_POINTERS* info) -> LONG {
  void*   address        = info->ExceptionRecord->ExceptionAddress;
  HMODULE module         = nullptr;
  char    path[MAX_PATH] = "?";
  if (GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          static_cast<LPCSTR>(address), &module)) {
    GetModuleFileNameA(module, path, MAX_PATH);
  }
  std::fprintf(stderr, "CRASH code=0x%08lX module=%s offset=0x%llx\n",
               info->ExceptionRecord->ExceptionCode, path,
               static_cast<unsigned long long>(static_cast<char*>(address) -
                                               reinterpret_cast<char*>(module)));
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}

auto Build(cl_context context, cl_device_id device, const Program& program) -> bool {
  auto*                    cl = Dispatch(context);
  std::vector<const char*> pointers;
  std::vector<size_t>      lengths;
  for (const auto& source : program.sources) {
    pointers.push_back(source.c_str());
    lengths.push_back(source.size());
  }
  cl_int     error  = CL_SUCCESS;
  cl_program handle = cl->clCreateProgramWithSource(
      context, static_cast<cl_uint>(pointers.size()), pointers.data(), lengths.data(), &error);
  if (error != CL_SUCCESS) {
    std::fprintf(stderr, "  create failed %d\n", error);
    return false;
  }
  error = cl->clBuildProgram(handle, 1, &device,
                             program.options.empty() ? nullptr : program.options.c_str(), nullptr,
                             nullptr);
  if (error != CL_SUCCESS) {
    size_t size = 0;
    cl->clGetProgramBuildInfo(handle, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
    std::string log(size, '\0');
    cl->clGetProgramBuildInfo(handle, device, CL_PROGRAM_BUILD_LOG, size, log.data(), nullptr);
    std::fprintf(stderr, "  build failed %d\n%.4000s\n", error, log.c_str());
  }
  cl->clReleaseProgram(handle);
  return error == CL_SUCCESS;
}

auto ReadManifest(const char* path) -> std::vector<Program> {
  std::vector<Program> programs;
  std::ifstream        manifest(path);
  std::string          line;
  while (std::getline(manifest, line)) {
    if (line.empty()) {
      continue;
    }
    const auto first  = line.find('\t');
    const auto second = line.find('\t', first + 1);
    Program    program{line.substr(0, first), line.substr(first + 1, second - first - 1), {}};
    std::stringstream files(line.substr(second + 1));
    std::string       file;
    while (std::getline(files, file, ';')) {
      if (!file.empty()) {
        program.sources.push_back(ReadFile(file));
      }
    }
    programs.push_back(std::move(program));
  }
  return programs;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: ocl_probe <amdocl_dir> <device> <threads> <manifest>\n");
    return 2;
  }
  SetUnhandledExceptionFilter(ReportCrash);
  const std::string driver_dir = argv[1];
  const std::string wanted     = argv[2];
  const int         threads    = std::atoi(argv[3]);
  std::fprintf(stderr, "ACP=%u\n", GetACP());

  SetDllDirectoryA(driver_dir.c_str());
  HMODULE amdocl = LoadLibraryA((driver_dir + "\\amdocl64.dll").c_str());
  if (amdocl == nullptr) {
    std::fprintf(stderr, "LoadLibrary amdocl64.dll failed: %lu\n", GetLastError());
    return 3;
  }
  auto get_platforms = reinterpret_cast<clIcdGetPlatformIDsKHR_fn>(
      GetProcAddress(amdocl, "clIcdGetPlatformIDsKHR"));
  if (get_platforms == nullptr) {
    std::fprintf(stderr, "amdocl64.dll has no clIcdGetPlatformIDsKHR\n");
    return 3;
  }
  cl_uint platform_count = 0;
  get_platforms(0, nullptr, &platform_count);
  if (platform_count == 0) {
    std::fprintf(stderr, "no AMD platform\n");
    return 4;
  }
  cl_platform_id platform = nullptr;
  get_platforms(1, &platform, nullptr);
  auto* cl            = Dispatch(platform);
  char  version[256] = {};
  cl->clGetPlatformInfo(platform, CL_PLATFORM_VERSION, sizeof version, version, nullptr);
  std::fprintf(stderr, "platform: %s\n", version);

  cl_context_properties properties[] = {CL_CONTEXT_PLATFORM,
                                        reinterpret_cast<cl_context_properties>(platform),
                                        CL_CONTEXT_OFFLINE_DEVICES_AMD, 1, 0};
  cl_int     error                   = CL_SUCCESS;
  cl_context context =
      cl->clCreateContextFromType(properties, CL_DEVICE_TYPE_ALL, nullptr, nullptr, &error);
  if (error != CL_SUCCESS) {
    std::fprintf(stderr, "offline context failed: %d\n", error);
    return 5;
  }
  size_t bytes = 0;
  cl->clGetContextInfo(context, CL_CONTEXT_DEVICES, 0, nullptr, &bytes);
  std::vector<cl_device_id> devices(bytes / sizeof(cl_device_id));
  cl->clGetContextInfo(context, CL_CONTEXT_DEVICES, bytes, devices.data(), nullptr);
  cl_device_id device = nullptr;
  std::string  names;
  for (auto candidate : devices) {
    char name[128] = {};
    cl->clGetDeviceInfo(candidate, CL_DEVICE_NAME, sizeof name, name, nullptr);
    names += std::string(name) + " ";
    if (wanted == name) {
      device = candidate;
    }
  }
  if (device == nullptr) {
    std::fprintf(stderr, "device %s not found; offline devices: %s\n", wanted.c_str(),
                 names.c_str());
    return 6;
  }
  char driver[128] = {};
  cl->clGetDeviceInfo(device, CL_DRIVER_VERSION, sizeof driver, driver, nullptr);
  std::fprintf(stderr, "device: %s driver=%s\n", wanted.c_str(), driver);

  const auto       programs = ReadManifest(argv[4]);
  std::atomic<int> failures{0};
  if (threads <= 1) {
    for (const auto& program : programs) {
      std::fprintf(stderr, "BUILD %s [%s]\n", program.name.c_str(), program.options.c_str());
      std::fflush(stderr);
      const bool ok = Build(context, device, program);
      std::fprintf(stderr, "  -> %s\n", ok ? "OK" : "FAIL");
      std::fflush(stderr);
      if (!ok) {
        ++failures;
      }
    }
  } else {
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
      workers.emplace_back([&, t] {
        for (size_t i = t; i < programs.size(); i += threads) {
          if (!Build(context, device, programs[i])) {
            ++failures;
          }
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    std::fprintf(stderr, "concurrent builds done, failures=%d\n", failures.load());
  }
  return failures.load() == 0 ? 0 : 1;
}
