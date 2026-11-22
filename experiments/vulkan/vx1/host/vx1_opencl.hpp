//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX1 experiment: the OpenCL reference. The programs are the product .cl sources, concatenated in
// the order of opencl_gpu_dag_programs.cpp, built with -cl-std=CL1.2. Experiment code: it does not
// merge.
#pragma once

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace vx1 {

inline void ClCheck(cl_int result, const char* what) {
  if (result != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL: ") + what + " failed, error " +
                             std::to_string(result));
  }
}

inline std::string ReadText(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("source file not found: " + path);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

class OpenClCompute {
 public:
  explicit OpenClCompute(const std::string& device_filter) {
    cl_uint platform_count = 0;
    ClCheck(clGetPlatformIDs(0, nullptr, &platform_count), "clGetPlatformIDs");
    std::vector<cl_platform_id> platforms(platform_count);
    ClCheck(clGetPlatformIDs(platform_count, platforms.data(), nullptr), "clGetPlatformIDs");
    std::string seen;
    for (auto platform : platforms) {
      cl_uint count = 0;
      if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &count) != CL_SUCCESS) continue;
      std::vector<cl_device_id> devices(count);
      clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, count, devices.data(), nullptr);
      for (auto device : devices) {
        const std::string name = Info(device, CL_DEVICE_NAME);
        seen += "'" + name + "' ";
        if (name.find(device_filter) == std::string::npos) continue;
        device_      = device;
        device_name_ = name;
        driver_      = Info(device, CL_DRIVER_VERSION);
        break;
      }
      if (device_) break;
    }
    if (!device_) throw std::runtime_error("OpenCL: no GPU matches '" + device_filter + "'. Devices: " + seen);
    cl_int error = CL_SUCCESS;
    context_     = clCreateContext(nullptr, 1, &device_, nullptr, nullptr, &error);
    ClCheck(error, "clCreateContext");
    queue_ = clCreateCommandQueue(context_, device_, CL_QUEUE_PROFILING_ENABLE, &error);
    ClCheck(error, "clCreateCommandQueue");
  }

  ~OpenClCompute() {
    for (auto k : kernels_) clReleaseKernel(k);
    for (auto p : programs_) clReleaseProgram(p);
    for (auto m : mems_) clReleaseMemObject(m);
    if (queue_) clReleaseCommandQueue(queue_);
    if (context_) clReleaseContext(context_);
  }

  OpenClCompute(const OpenClCompute&)            = delete;
  OpenClCompute& operator=(const OpenClCompute&) = delete;

  [[nodiscard]] const std::string& DeviceName() const { return device_name_; }
  [[nodiscard]] const std::string& Driver() const { return driver_; }

  /// Builds one program from @p paths, concatenated in order, and returns its kernels by name.
  cl_program Build(const std::vector<std::string>& paths, const std::string& options) {
    std::string source;
    for (const auto& path : paths) {
      source += ReadText(path);
      source += "\n";
    }
    const char* text  = source.c_str();
    cl_int      error = CL_SUCCESS;
    cl_program  program = clCreateProgramWithSource(context_, 1, &text, nullptr, &error);
    ClCheck(error, "clCreateProgramWithSource");
    error = clBuildProgram(program, 1, &device_, options.c_str(), nullptr, nullptr);
    if (error != CL_SUCCESS) {
      std::size_t size = 0;
      clGetProgramBuildInfo(program, device_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
      std::string log(size, '\0');
      clGetProgramBuildInfo(program, device_, CL_PROGRAM_BUILD_LOG, size, log.data(), nullptr);
      throw std::runtime_error("OpenCL: build failed:\n" + log);
    }
    programs_.push_back(program);
    return program;
  }

  cl_kernel Kernel(cl_program program, const char* name) {
    cl_int    error  = CL_SUCCESS;
    cl_kernel kernel = clCreateKernel(program, name, &error);
    ClCheck(error, name);
    kernels_.push_back(kernel);
    return kernel;
  }

  cl_mem Buffer(std::size_t size, const void* data = nullptr) {
    cl_int error = CL_SUCCESS;
    cl_mem mem   = clCreateBuffer(context_,
                                  CL_MEM_READ_WRITE | (data ? CL_MEM_COPY_HOST_PTR : 0),
                                  size, const_cast<void*>(data), &error);
    ClCheck(error, "clCreateBuffer");
    mems_.push_back(mem);
    return mem;
  }

  cl_mem Image(std::size_t width, std::size_t height, const float* rgba, cl_mem_flags flags) {
    const cl_image_format format{CL_RGBA, CL_FLOAT};
    cl_image_desc         desc{};
    desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
    desc.image_width  = width;
    desc.image_height = height;
    cl_int error      = CL_SUCCESS;
    cl_mem mem        = clCreateImage(context_, flags | (rgba ? CL_MEM_COPY_HOST_PTR : 0), &format,
                                      &desc, const_cast<float*>(rgba), &error);
    ClCheck(error, "clCreateImage");
    mems_.push_back(mem);
    return mem;
  }

  std::vector<float> ReadImage(cl_mem image, std::size_t width, std::size_t height) {
    std::vector<float> out(width * height * 4);
    const std::size_t  origin[3] = {0, 0, 0};
    const std::size_t  region[3] = {width, height, 1};
    ClCheck(clEnqueueReadImage(queue_, image, CL_TRUE, origin, region, 0, 0, out.data(), 0,
                               nullptr, nullptr),
            "clEnqueueReadImage");
    return out;
  }

  std::vector<float> ReadBuffer(cl_mem buffer, std::size_t floats) {
    std::vector<float> out(floats);
    ClCheck(clEnqueueReadBuffer(queue_, buffer, CL_TRUE, 0, floats * sizeof(float), out.data(), 0,
                                nullptr, nullptr),
            "clEnqueueReadBuffer");
    return out;
  }

  /// Runs @p kernel over (@p width, @p height) with a work-group of 16 x 16 and returns the time in
  /// milliseconds (event profiling).
  double Run(cl_kernel kernel, std::size_t width, std::size_t height) {
    const std::size_t local[2]  = {16, 16};
    const std::size_t global[2] = {(width + 15) / 16 * 16, (height + 15) / 16 * 16};
    cl_event          event     = nullptr;
    ClCheck(clEnqueueNDRangeKernel(queue_, kernel, 2, nullptr, global, local, 0, nullptr, &event),
            "clEnqueueNDRangeKernel");
    ClCheck(clWaitForEvents(1, &event), "clWaitForEvents");
    cl_ulong start = 0, end = 0;
    clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(start), &start, nullptr);
    clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(end), &end, nullptr);
    clReleaseEvent(event);
    return double(end - start) * 1.0e-6;
  }

  template <class T>
  static void Arg(cl_kernel kernel, cl_uint index, const T& value) {
    ClCheck(clSetKernelArg(kernel, index, sizeof(T), &value), "clSetKernelArg");
  }

 private:
  static std::string Info(cl_device_id device, cl_device_info what) {
    std::size_t size = 0;
    clGetDeviceInfo(device, what, 0, nullptr, &size);
    std::string text(size, '\0');
    clGetDeviceInfo(device, what, size, text.data(), nullptr);
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
  }

  cl_device_id            device_  = nullptr;
  cl_context              context_ = nullptr;
  cl_command_queue        queue_   = nullptr;
  std::string             device_name_;
  std::string             driver_;
  std::vector<cl_program> programs_;
  std::vector<cl_kernel>  kernels_;
  std::vector<cl_mem>     mems_;
};

}  // namespace vx1
