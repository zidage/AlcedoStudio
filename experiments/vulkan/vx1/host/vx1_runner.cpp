//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX1 experiment runner (vulkan_compute_backend_plan.md, section 6): one input, the OpenCL kernel
// of the product source, and the Slang kernel as SPIR-V on Vulkan. Prints the largest difference
// and the time of each kernel. Experiment code: it does not merge.
//
// vx1_runner --case grade|film_print|film_light_table|film_blur
//            --vk-device <part of name> --cl-device <part of name>
//            --spv <dir> --src <alcedo_studio/src> [--width N --height N] [--no-validation]

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/film/film_print_math.h"
#include "vx1_opencl.hpp"
#include "vx1_vulkan.hpp"

namespace vx1 {
namespace {

// --- input ----------------------------------------------------------------------------------------

class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  float Uniform(float low, float high) {
    return low + (high - low) * float(Next() >> 40) / float(1ull << 24);
  }

 private:
  std::uint64_t state_;
};

struct Options {
  std::string   test_case = "grade";
  std::string   vk_device = "NVIDIA";
  std::string   cl_device = "NVIDIA";
  std::string   spv_dir;
  std::string   grade_spv = "primary_grade.spv";
  std::string   grade_entry = "primary_grade_pointwise_rgba32f";
  bool          photo_input = false;  // --input photo: the value ranges of a developed photo
  std::string   src_dir;
  std::uint32_t width      = 2048;
  std::uint32_t height     = 1536;
  bool          validation = true;
  int           repeats    = 5;
};

// --- comparison -----------------------------------------------------------------------------------

struct Difference {
  double      largest   = 0.0;
  double      p999      = 0.0;
  std::size_t index     = 0;
  std::size_t over      = 0;
  std::size_t nan_a     = 0;
  std::size_t nan_b     = 0;
  std::size_t compared  = 0;
};

/// Difference of channel values of @p a and @p b (stride @p stride, channels [0, @p channels)),
/// after @p map, scaled by @p scale. A value that is NaN on one side only counts as over.
template <class Map>
Difference Compare(const std::vector<float>& a, const std::vector<float>& b, std::size_t stride, std::size_t first,
                   std::size_t last, double scale, double tolerance, Map map) {
  Difference          result;
  std::vector<double> all;
  all.reserve(a.size());
  for (std::size_t i = 0; i < a.size(); i += stride) {
    for (std::size_t c = first; c < last; ++c) {
      const float va = a[i + c];
      const float vb = b[i + c];
      const bool  na = std::isnan(va);
      const bool  nb = std::isnan(vb);
      result.nan_a += na;
      result.nan_b += nb;
      if (na || nb) {
        if (na != nb) ++result.over;
        continue;
      }
      const double d = std::abs(map(va) - map(vb)) * scale;
      all.push_back(d);
      if (d > tolerance) ++result.over;
      if (d > result.largest) {
        result.largest = d;
        result.index   = i / stride;
      }
    }
  }
  result.compared = all.size();
  if (!all.empty()) {
    const auto k = static_cast<std::size_t>(double(all.size() - 1) * 0.999);
    std::nth_element(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(k), all.end());
    result.p999 = all[k];
  }
  return result;
}

double Identity(float v) { return v; }
double Log10Exposure(float v) { return std::log10(std::max(double(v), 1e-30)); }

void Report(const std::string& what, const Difference& d, double tolerance, const char* unit) {
  std::printf("  %-34s largest %.3e  p99.9 %.3e  over %zu of %zu  NaN %zu/%zu  (tol %.1e %s, "
              "worst pixel %zu)\n",
              what.c_str(), d.largest, d.p999, d.over, d.compared, d.nan_a, d.nan_b, tolerance,
              unit, d.index);
}

struct Timing {
  double best = std::numeric_limits<double>::infinity();
  void   Add(double ms) { best = std::min(best, ms); }
};

// --- primary grade --------------------------------------------------------------------------------

struct GradeAdjustmentParams {
  std::uint32_t behavior = 0;
  std::uint32_t count    = 0;
  float         values[48]{};
  std::uint32_t reserved[2]{};
};
static_assert(sizeof(GradeAdjustmentParams) == 208);

struct PrimaryGradeDispatchParams {
  std::uint32_t command_count;
  std::uint32_t command_offset;
  std::uint32_t lut_edge;
  float         local_reference;
  std::uint32_t width;
  std::uint32_t pad[3];
};
static_assert(sizeof(PrimaryGradeDispatchParams) == 32);

struct PrimaryGradePush {
  std::uint64_t parameter_base;
  std::uint64_t commands;
  std::uint64_t lut;
  std::uint32_t command_count;
  std::uint32_t command_offset;
  std::uint32_t lut_edge;
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t pad;
};
static_assert(sizeof(PrimaryGradePush) == 48);

struct GradeInput {
  std::vector<std::uint8_t>  arena;
  std::vector<std::uint32_t> commands;
  std::vector<float>         lut;  // float4 per node
  std::uint32_t              lut_edge = 33;
};

std::uint32_t Append(GradeInput& in, const GradeAdjustmentParams& record) {
  const auto offset = static_cast<std::uint32_t>(in.arena.size());
  const auto bytes  = reinterpret_cast<const std::uint8_t*>(&record);
  in.arena.insert(in.arena.end(), bytes, bytes + sizeof(record));
  return offset;
}

GradeAdjustmentParams Record(std::uint32_t behavior, float value) {
  GradeAdjustmentParams r;
  r.behavior  = behavior;
  r.values[0] = value;
  return r;
}

GradeAdjustmentParams CurveRecord(std::vector<float> xs, std::vector<float> ys,
                                  std::vector<float> ms) {
  GradeAdjustmentParams r = Record(7, 0.0f);
  r.count                 = static_cast<std::uint32_t>(xs.size());
  for (std::size_t i = 0; i < xs.size(); ++i) {
    r.values[i]      = xs[i];
    r.values[16 + i] = ys[i];
    r.values[32 + i] = ms[i];
  }
  return r;
}

/// Every behavior of primary_grade.cl, with values in the ranges of the controls.
GradeInput MakeGradeInput() {
  GradeInput in;
  {  // 0: CAT02 white balance (row-major adaptation in linear AP1)
    auto r                = Record(0, 1.0f);
    const float m[9]      = {1.06f, -0.04f, -0.02f, 0.01f, 0.99f, 0.0f, 0.0f, 0.03f, 0.84f};
    for (int i = 0; i < 9; ++i) r.values[1 + i] = m[i];
    in.commands.push_back(Append(in, r));
  }
  in.commands.push_back(Append(in, Record(1, 0.5f)));     // exposure
  in.commands.push_back(Append(in, Record(2, 40.0f)));    // contrast
  in.commands.push_back(Append(in, Record(3, 60.0f)));    // whites
  in.commands.push_back(Append(in, Record(4, -45.0f)));   // blacks
  {  // 7: curves, four records: RGB, red, green (none), blue
    const auto first = Append(in, CurveRecord({0.0f, 0.25f, 0.5f, 0.75f, 1.0f},
                                              {0.0f, 0.21f, 0.5f, 0.81f, 1.0f},
                                              {0.8f, 1.05f, 1.25f, 1.05f, 0.8f}));
    Append(in, CurveRecord({0.1f, 0.5f, 0.9f}, {0.12f, 0.52f, 0.88f}, {1.0f, 1.0f, 0.9f}));
    Append(in, CurveRecord({}, {}, {}));
    Append(in, CurveRecord({0.0f, 0.3f, 0.6f, 1.0f}, {0.02f, 0.28f, 0.62f, 0.97f},
                           {0.9f, 1.1f, 1.05f, 0.85f}));
    in.commands.push_back(first);
  }
  {  // 8: HLS, 8 bins
    auto r = Record(8, 0.0f);
    for (int i = 0; i < 8; ++i) {
      r.values[i]          = 45.0f * float(i);
      r.values[8 + i * 3]  = (i % 3 == 0) ? 0.15f : (i % 3 == 1 ? -0.1f : 0.0f);
      r.values[9 + i * 3]  = (i % 2 == 0) ? 0.02f : -0.015f;
      r.values[10 + i * 3] = (i % 4 == 0) ? 0.2f : -0.12f;
      r.values[32 + i]     = 30.0f;
    }
    in.commands.push_back(Append(in, r));
  }
  in.commands.push_back(Append(in, Record(9, 1.2f)));     // saturation
  in.commands.push_back(Append(in, Record(10, 30.0f)));   // vibrance
  {  // 11: tone wheels
    auto        r      = Record(11, 0.0f);
    const float v[14] = {-0.01f, 0.0f,  0.012f, 0.05f, -0.02f, -0.02f, 1.1f,
                         0.95f,  1.0f,  -0.03f, 0.02f, 0.02f,  0.1f,   0.6f};
    for (int i = 0; i < 14; ++i) r.values[i] = v[i];
    in.commands.push_back(Append(in, r));
  }
  {  // 12: 3D LUT at strength 0.8
    in.commands.push_back(Append(in, Record(12, 0.8f)));
    const auto e = in.lut_edge;
    in.lut.resize(std::size_t(e) * e * e * 4);
    for (std::uint32_t z = 0; z < e; ++z)
      for (std::uint32_t y = 0; y < e; ++y)
        for (std::uint32_t x = 0; x < e; ++x) {
          const float r  = float(x) / float(e - 1);
          const float g  = float(y) / float(e - 1);
          const float b  = float(z) / float(e - 1);
          const auto  i  = (std::size_t(z) * e + y) * e + x;
          in.lut[i * 4 + 0] = std::pow(r, 0.92f) + 0.04f * std::sin(6.0f * g);
          in.lut[i * 4 + 1] = g + 0.03f * (b - r);
          in.lut[i * 4 + 2] = std::pow(b, 1.08f) - 0.02f * r;
          in.lut[i * 4 + 3] = 1.0f;
        }
  }
  return in;
}

/// ACEScc image: a grey ramp in the first 8 rows, neutral pixels, ties, and random colours.
std::vector<float> MakeGradeImage(std::uint32_t width, std::uint32_t height, bool photo) {
  // stress: ACEScc -0.05 to 1.15 and channel spreads of +-0.25 (+-4.4 stops). photo: 0.25 to 0.65
  // (2^-5.3 to 2^1.7 linear) and spreads of +-0.06 (+-1 stop).
  const float low = photo ? 0.25f : -0.05f, high = photo ? 0.65f : 1.15f;
  const float spread = photo ? 0.06f : 0.25f;
  std::vector<float> image(std::size_t(width) * height * 4);
  Random             random(0x5eed1234abcdULL);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      float*      p = &image[(std::size_t(y) * width + x) * 4];
      const float t = float(x) / float(width - 1);
      if (y < 8) {
        p[0] = p[1] = p[2] = -0.1f + 1.3f * t;
      } else {
        const float base = random.Uniform(low, high);
        const auto  kind = random.Next() % 100;
        if (kind == 0) {
          p[0] = p[1] = p[2] = base;
        } else if (kind == 1) {
          p[0] = p[1] = base;
          p[2]        = base - 0.2f;
        } else {
          p[0] = base + random.Uniform(-spread, spread);
          p[1] = base + random.Uniform(-spread, spread);
          p[2] = base + random.Uniform(-spread, spread);
        }
      }
      p[3] = random.Uniform(0.0f, 1.0f);
    }
  return image;
}

// ApplyOkLabContrast of primary_grade.cl in double precision, with the ACEScc curve of
// color_encoding_math.h.
double AcesccEncodeD(double v) {
  const double a = 9.72, b = 17.52, floor = (-16.0 + a) / b;
  if (v < 0.0) return floor + v;
  if (v < 0.000030517578125) return (std::log2(0.0000152587890625 + v * 0.5) + a) / b;
  return (std::log2(v) + a) / b;
}
double AcesccDecodeD(double v) {
  const double a = 9.72, b = 17.52, floor = (-16.0 + a) / b, threshold = (-15.0 + a) / b;
  if (v < floor) return v - floor;
  if (v <= threshold) return (std::exp2(v * b - a) - 0.0000152587890625) * 2.0;
  return std::exp2(v * b - a);
}
std::array<double, 3> ContrastReference(const float* acescc, double contrast) {
  const double r = AcesccDecodeD(acescc[0]), g = AcesccDecodeD(acescc[1]),
               b = AcesccDecodeD(acescc[2]);
  const double l  = std::cbrt(0.6341104672 * r + 0.3489495087 * g + 0.0169400240 * b);
  const double m  = std::cbrt(0.2754060131 * r + 0.6327713632 * g + 0.0918226237 * b);
  const double s  = std::cbrt(0.1056775254 * r + 0.1971481306 * g + 0.6971743440 * b);
  double       L  = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  double       A  = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  double       B  = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
  const double slope = std::exp2(contrast * 0.01);
  const double shape = L > 0.0 ? std::tanh(3.0 * std::log2(L / 0.5646216) / 2.5) : -1.0;
  const double gain  = std::exp2((slope - 1.0) * 2.5 * shape / 3.0);
  const double cs    = std::sqrt(slope) * std::min(gain, 1.0);
  L *= gain;
  A *= cs;
  B *= cs;
  const double l_ = L + 0.3963377774 * A + 0.2158037573 * B;
  const double m_ = L - 0.1055613458 * A - 0.0638541728 * B;
  const double s_ = L - 0.0894841775 * A - 1.2914855480 * B;
  const double l3 = l_ * l_ * l_, m3 = m_ * m_ * m_, s3 = s_ * s_ * s_;
  return {AcesccEncodeD(2.0693822907 * l3 - 1.1736821545 * m3 + 0.1042998638 * s3),
          AcesccEncodeD(-0.8917480677 * l3 + 2.1537429245 * m3 - 0.2619948569 * s3),
          AcesccEncodeD(-0.0615064732 * l3 - 0.4311325686 * m3 + 1.4926390418 * s3)};
}

/// The full chain of the grade on OpenCL device @p device.
std::vector<float> GradeOpenCl(const Options& o, const std::string& device, const GradeInput& in,
                               const std::vector<float>& image) {
  OpenClCompute     cl(device);
  const std::string sh      = o.src_dir + "/edit/runtime/opencl/shader/";
  auto              program = cl.Build({o.src_dir + "/include/color/color_encoding_math.h",
                                        sh + "oklab.cl", sh + "primary_grade.cl"},
                                       "-cl-std=CL1.2");
  auto   kernel = cl.Kernel(program, "primary_grade_pointwise_rgba32f");
  cl_mem src    = cl.Image(o.width, o.height, image.data(), CL_MEM_READ_ONLY);
  cl_mem dst    = cl.Image(o.width, o.height, nullptr, CL_MEM_WRITE_ONLY);
  const PrimaryGradeDispatchParams dispatch{static_cast<std::uint32_t>(in.commands.size()), 0,
                                            in.lut_edge, 0.0f, o.width, {0, 0, 0}};
  OpenClCompute::Arg(kernel, 0, src);
  OpenClCompute::Arg(kernel, 1, dst);
  OpenClCompute::Arg(kernel, 2, cl.Buffer(in.arena.size(), in.arena.data()));
  OpenClCompute::Arg(kernel, 3, cl.Buffer(in.commands.size() * 4, in.commands.data()));
  OpenClCompute::Arg(kernel, 4, dispatch);
  OpenClCompute::Arg(kernel, 5, cl.Buffer(in.lut.size() * 4, in.lut.data()));
  cl.Run(kernel, o.width, o.height);
  return cl.ReadImage(dst, o.width, o.height);
}

/// The full chain of the grade on Vulkan device @p device.
std::vector<float> GradeVulkan(const Options& o, const std::string& device, const GradeInput& in,
                               const std::vector<float>& image) {
  VulkanCompute vk(device, o.validation);
  const auto    k = vk.CreateKernel(o.spv_dir + "/" + o.grade_spv,
                                    o.grade_entry, sizeof(PrimaryGradePush), 2);
  const auto    v_src   = vk.CreateImage(o.width, o.height, image.data());
  const auto    v_dst   = vk.CreateImage(o.width, o.height);
  const auto    v_arena = vk.CreateBuffer(in.arena.size(), in.arena.data());
  const auto    v_cmds  = vk.CreateBuffer(in.commands.size() * 4, in.commands.data());
  const auto    v_lut   = vk.CreateBuffer(in.lut.size() * 4, in.lut.data());
  const PrimaryGradePush push{v_arena.address,
                              v_cmds.address,
                              v_lut.address,
                              static_cast<std::uint32_t>(in.commands.size()),
                              0,
                              in.lut_edge,
                              o.width,
                              o.height,
                              0};
  vk.Dispatch(k, &push, {v_src, v_dst}, o.width, o.height);
  if (vk.ValidationErrors() != 0) throw std::runtime_error("Vulkan validation errors");
  return vk.DownloadImage(v_dst);
}

/// The same backend on the two devices: OpenCL NVIDIA - OpenCL Intel, Vulkan NVIDIA - Vulkan Intel.
int RunGradeCross(const Options& o) {
  const auto in    = MakeGradeInput();
  const auto image = MakeGradeImage(o.width, o.height, o.photo_input);
  const auto cl_n  = GradeOpenCl(o, "NVIDIA", in, image);
  const auto cl_i  = GradeOpenCl(o, "Intel", in, image);
  const auto vk_n  = GradeVulkan(o, "NVIDIA", in, image);
  const auto vk_i  = GradeVulkan(o, "Intel", in, image);
  constexpr double kTolerance = 2.0e-4;
  std::printf("case grade_cross  %u x %u  input %s\n", o.width, o.height,
              o.photo_input ? "photo" : "stress");
  Report("OpenCL NVIDIA - OpenCL Intel", Compare(cl_n, cl_i, 4, 0, 3, 1.0, kTolerance, Identity),
         kTolerance, "ACEScc");
  Report("Vulkan NVIDIA - Vulkan Intel", Compare(vk_n, vk_i, 4, 0, 3, 1.0, kTolerance, Identity),
         kTolerance, "ACEScc");
  Report("Vulkan NVIDIA - OpenCL NVIDIA", Compare(vk_n, cl_n, 4, 0, 3, 1.0, kTolerance, Identity),
         kTolerance, "ACEScc");
  Report("Vulkan Intel - OpenCL Intel", Compare(vk_i, cl_i, 4, 0, 3, 1.0, kTolerance, Identity),
         kTolerance, "ACEScc");
  return 0;
}

int RunGrade(const Options& o) {
  const auto in    = MakeGradeInput();
  const auto image = MakeGradeImage(o.width, o.height, o.photo_input);
  const auto count = static_cast<std::uint32_t>(in.commands.size());

  // OpenCL
  std::fprintf(stderr, "[stage] OpenCL grade\n");
  OpenClCompute cl(o.cl_device);
  const std::string sh  = o.src_dir + "/edit/runtime/opencl/shader/";
  auto        program   = cl.Build({o.src_dir + "/include/color/color_encoding_math.h",
                                    sh + "oklab.cl", sh + "primary_grade.cl"},
                                   "-cl-std=CL1.2");
  auto        kernel    = cl.Kernel(program, "primary_grade_pointwise_rgba32f");
  cl_mem      src       = cl.Image(o.width, o.height, image.data(), CL_MEM_READ_ONLY);
  cl_mem      dst       = cl.Image(o.width, o.height, nullptr, CL_MEM_WRITE_ONLY);
  cl_mem      arena     = cl.Buffer(in.arena.size(), in.arena.data());
  cl_mem      commands  = cl.Buffer(in.commands.size() * 4, in.commands.data());
  cl_mem      lut       = cl.Buffer(in.lut.size() * 4, in.lut.data());
  const PrimaryGradeDispatchParams dispatch{count, 0, in.lut_edge, 0.0f, o.width, {0, 0, 0}};
  OpenClCompute::Arg(kernel, 0, src);
  OpenClCompute::Arg(kernel, 1, dst);
  OpenClCompute::Arg(kernel, 2, arena);
  OpenClCompute::Arg(kernel, 3, commands);
  OpenClCompute::Arg(kernel, 4, dispatch);
  OpenClCompute::Arg(kernel, 5, lut);
  Timing cl_time;
  for (int i = 0; i <= o.repeats; ++i) {
    const double ms = cl.Run(kernel, o.width, o.height);
    if (i > 0) cl_time.Add(ms);
  }
  const auto cl_out = cl.ReadImage(dst, o.width, o.height);

  // Vulkan
  std::fprintf(stderr, "[stage] Vulkan grade\n");
  VulkanCompute vk(o.vk_device, o.validation);
  const auto    k_index = vk.CreateKernel(o.spv_dir + "/" + o.grade_spv,
                                          o.grade_entry,
                                          sizeof(PrimaryGradePush), 2);
  std::fprintf(stderr, "[stage] Vulkan grade pipeline created\n");
  const auto    v_src   = vk.CreateImage(o.width, o.height, image.data());
  const auto    v_dst   = vk.CreateImage(o.width, o.height);
  const auto    v_arena = vk.CreateBuffer(in.arena.size(), in.arena.data());
  const auto    v_cmds  = vk.CreateBuffer(in.commands.size() * 4, in.commands.data());
  const auto    v_lut   = vk.CreateBuffer(in.lut.size() * 4, in.lut.data());
  const PrimaryGradePush push{v_arena.address, v_cmds.address, v_lut.address, count, 0,
                              in.lut_edge,     o.width,        o.height,      0};
  Timing vk_time;
  for (int i = 0; i <= o.repeats; ++i) {
    const double ms = vk.Dispatch(k_index, &push, {v_src, v_dst}, o.width, o.height);
    if (i > 0) vk_time.Add(ms);
  }
  const auto vk_out = vk.DownloadImage(v_dst);

  std::printf("case grade  %u x %u  %u adjustments  input %s\n", o.width, o.height, count,
              o.photo_input ? "photo" : "stress");
  std::printf("  OpenCL  %s  (%s)\n", cl.DeviceName().c_str(), cl.Driver().c_str());
  std::printf("  Vulkan  %s  (%s)  validation %s\n", vk.DeviceName().c_str(),
              vk.DriverInfo().c_str(), vk.ValidationEnabled() ? "on" : "off");
  constexpr double kTolerance = 2.0e-4;  // opencl_grade_test.cpp, primary grade against the CPU
  const auto       rgb        = Compare(cl_out, vk_out, 4, 0, 3, 1.0, kTolerance, Identity);
  const auto       alpha      = Compare(cl_out, vk_out, 4, 3, 4, 1.0, 0.0, Identity);
  Report("ACEScc RGB, Vulkan - OpenCL", rgb, kTolerance, "ACEScc");
  Report("RGBA (alpha exact)", alpha, 0.0, "");
  {
    const auto i = rgb.index;
    std::printf("  worst: in (%.6f %.6f %.6f)  cl (%.6f %.6f %.6f)  vk (%.6f %.6f %.6f)\n",
                image[i * 4], image[i * 4 + 1], image[i * 4 + 2], cl_out[i * 4],
                cl_out[i * 4 + 1], cl_out[i * 4 + 2], vk_out[i * 4], vk_out[i * 4 + 1],
                vk_out[i * 4 + 2]);
  }
  std::printf("  time    OpenCL %.3f ms  Vulkan %.3f ms (best of %d)\n", cl_time.best,
              vk_time.best, o.repeats);

  // Each adjustment alone on the same input: command_offset i, command_count 1.
  const char* names[] = {"cat02",      "exposure", "contrast",   "whites",
                         "blacks",     "curves",   "hls",        "saturation",
                         "vibrance",   "tone_wheels", "lut"};
  for (std::uint32_t i = 0; i < count; ++i) {
    const PrimaryGradeDispatchParams one{1, i, in.lut_edge, 0.0f, o.width, {0, 0, 0}};
    OpenClCompute::Arg(kernel, 4, one);
    cl.Run(kernel, o.width, o.height);
    const auto             cl_one = cl.ReadImage(dst, o.width, o.height);
    const PrimaryGradePush push_one{v_arena.address, v_cmds.address, v_lut.address, 1, i,
                                    in.lut_edge,     o.width,        o.height,      0};
    vk.Dispatch(k_index, &push_one, {v_src, v_dst}, o.width, o.height);
    const auto vk_one = vk.DownloadImage(v_dst);
    const auto d = Compare(cl_one, vk_one, 4, 0, 3, 1.0, kTolerance, Identity);
    Report(std::string("only ") + names[i], d, kTolerance, "ACEScc");
    if (std::string(names[i]) == "contrast") {
      std::vector<float> reference(cl_one.size());
      for (std::size_t p = 0; p < reference.size() / 4; ++p) {
        const auto v         = ContrastReference(&image[p * 4], 40.0);
        reference[p * 4]     = float(v[0]);
        reference[p * 4 + 1] = float(v[1]);
        reference[p * 4 + 2] = float(v[2]);
        reference[p * 4 + 3] = image[p * 4 + 3];
      }
      Report("  contrast, OpenCL - double host",
             Compare(cl_one, reference, 4, 0, 3, 1.0, kTolerance, Identity), kTolerance, "ACEScc");
      Report("  contrast, Vulkan - double host",
             Compare(vk_one, reference, 4, 0, 3, 1.0, kTolerance, Identity), kTolerance, "ACEScc");
    }
    if (d.over > 0) {
      int shown = 0;
      for (std::size_t p = 0; p < cl_one.size() / 4 && shown < 6; ++p) {
        float worst = 0.0f;
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(cl_one[p * 4 + c] - vk_one[p * 4 + c]));
        if (worst <= kTolerance) continue;
        ++shown;
        std::printf("    in (%.7f %.7f %.7f)  cl (%.7f %.7f %.7f)  vk (%.7f %.7f %.7f)\n",
                    image[p * 4], image[p * 4 + 1], image[p * 4 + 2], cl_one[p * 4],
                    cl_one[p * 4 + 1], cl_one[p * 4 + 2], vk_one[p * 4], vk_one[p * 4 + 1],
                    vk_one[p * 4 + 2]);
      }
    }
  }

  std::printf("  validation errors %u, warnings %u\n", vk.ValidationErrors(),
              vk.ValidationWarnings());
  const bool pass = rgb.over == 0 && rgb.nan_a == rgb.nan_b && vk.ValidationErrors() == 0;
  std::printf("RESULT grade(%s) %s %s largest %.3e tol %.1e\n", o.photo_input ? "photo" : "stress",
              vk.DeviceName().c_str(), pass ? "PASS" : "FAIL", rgb.largest, kTolerance);
  return pass ? 0 : 1;
}

// --- film print -----------------------------------------------------------------------------------

static_assert(sizeof(FilmKernelParams) == 416);

struct FilmPush {
  std::uint64_t data;
  std::uint64_t values;
  std::uint64_t q;
  std::uint64_t a;
  std::uint64_t print_rgba;
  std::uint32_t params_offset;
  std::uint32_t q_offset;
  std::uint32_t a_offset;
  std::int32_t  width;
  std::int32_t  height;
  std::int32_t  inhibitor_blur;
};
static_assert(sizeof(FilmPush) == 64);

struct FilmMatrix {
  float m[9];
};

struct FilmInput {
  FilmKernelParams   params{};
  std::vector<float> values;
  std::vector<float> print_rgba;
  int                inhibitor_blur = 0;
};

float Logistic(float t, float centre, float width) {
  return 1.0f / (1.0f + std::exp(-(t - centre) / width));
}

/// A synthetic selection in the layout of film_print_kernel_tables.hpp. The tables are smooth
/// functions with the value ranges of a real stock; every branch of FP1, FP4/FP5, and FP6 that the
/// three kernels have is reached.
FilmInput MakeFilmInput(const std::string& variant) {
  FilmInput         in;
  FilmKernelParams& p = in.params;
  const float ap1_to_xyz[9] = {0.6624542f,  0.1340042f, 0.1561877f, 0.2722287f, 0.6740818f,
                               0.0536895f, -0.0055746f, 0.0040607f, 1.0103391f};
  std::copy(ap1_to_xyz, ap1_to_xyz + 9, p.ap1_to_xyz);
  p.locus_white[0] = 0.95047f;
  p.locus_white[1] = 1.08883f;
  p.locus_keep     = 0.98f;

  // Layout: uv | curves | inhibition | normals | room.
  const std::uint32_t nodes = 64, facets = 24, curve_stored = 280, inhibition_stored = 300;
  p.uv_nodes                = nodes;
  p.uv_scale[0]             = float(nodes - 1) / 0.65f;
  p.uv_scale[1]             = float(nodes - 1) / 0.62f;
  const std::size_t uv_values = std::size_t(nodes) * nodes * 3;
  p.curves_offset             = static_cast<std::uint32_t>(uv_values);
  p.inhibition_values_offset  = p.curves_offset + 3 * curve_stored;
  p.normals_offset            = p.inhibition_values_offset + 3 * inhibition_stored;
  p.room_offset               = p.normals_offset + 2 * facets;
  in.values.resize(p.room_offset + facets);

  const float centres[3][2] = {{0.45f, 0.52f}, {0.12f, 0.57f}, {0.17f, 0.16f}};
  for (std::uint32_t iu = 0; iu < nodes; ++iu)
    for (std::uint32_t iv = 0; iv < nodes; ++iv) {
      const float u = float(iu) * 0.65f / float(nodes - 1);
      const float v = float(iv) * 0.62f / float(nodes - 1);
      for (int k = 0; k < 3; ++k) {
        const float du = u - centres[k][0], dv = v - centres[k][1];
        in.values[(std::size_t(iu) * nodes + iv) * 3 + k] =
            0.6f + 0.4f * std::exp(-(du * du + dv * dv) / 0.05f);
      }
    }
  const float layer_gain[3] = {1.1f, 1.0f, 0.9f};
  std::copy(layer_gain, layer_gain + 3, p.layer_gain);

  p.curve_stored     = curve_stored;
  p.curve_lengths[0] = 280;
  p.curve_lengths[1] = 270;
  p.curve_lengths[2] = 260;
  p.curve_step       = 0.025f;
  for (int k = 0; k < 3; ++k) {
    p.curve_offset[k] = 5.0f + 0.1f * float(k);
    for (std::uint32_t i = 0; i < curve_stored; ++i) {
      in.values[p.curves_offset + k * curve_stored + i] =
          0.15f + 2.6f * Logistic(float(i), 140.0f + 5.0f * float(k), 30.0f);
    }
  }
  p.inhibition_stored = inhibition_stored;
  for (int k = 0; k < 3; ++k) {
    p.inhibition_lengths[k] = inhibition_stored;
    p.inhibition_offset[k]  = 5.5f;
    for (std::uint32_t i = 0; i < inhibition_stored; ++i) {
      in.values[p.inhibition_values_offset + k * inhibition_stored + i] =
          0.12f + 2.8f * Logistic(float(i), 150.0f, 32.0f);
    }
  }
  p.inhibition_step = 0.025f;
  const float k_total[9] = {0.25f, 0.05f, 0.03f, 0.04f, 0.22f, 0.05f, 0.03f, 0.06f, 0.2f};
  for (int i = 0; i < 9; ++i) {
    p.inhibition_k[i]       = k_total[i];
    p.inhibition_image_k[i] = 0.4f * k_total[i];
    p.inhibition_point_k[i] = 0.6f * k_total[i];
  }
  p.inhibition_has_image = 1;
  p.inhibition_has_point = 1;

  float min_room = 1e9f;
  p.locus_facets = facets;
  for (std::uint32_t f = 0; f < facets; ++f) {
    const float angle = 6.2831853f * float(f) / float(facets);
    in.values[p.normals_offset + 2 * f]     = std::cos(angle);
    in.values[p.normals_offset + 2 * f + 1] = std::sin(angle);
    const float room = 1.2f + 0.5f * std::sin(3.0f * angle + 0.4f);
    in.values[p.room_offset + f]            = room;
    min_room                                = std::min(min_room, room);
  }
  p.locus_inner_squared = 0.9f * min_room * min_room;

  // Print table: 4 values per node, red fastest; v = cube root of XYZ of the transmittances.
  const std::uint32_t edge = 33;
  p.print_edge             = edge;
  for (int k = 0; k < 3; ++k) {
    p.print_start[k] = 0.1f;
    p.print_step[k]  = 3.0f / float(edge - 1);
  }
  in.print_rgba.resize(std::size_t(edge) * edge * edge * 4);
  for (std::uint32_t ib = 0; ib < edge; ++ib)
    for (std::uint32_t ig = 0; ig < edge; ++ig)
      for (std::uint32_t ir = 0; ir < edge; ++ir) {
        const float tr = std::pow(10.0f, -(0.1f + p.print_step[0] * float(ir)));
        const float tg = std::pow(10.0f, -(0.1f + p.print_step[1] * float(ig)));
        const float tb = std::pow(10.0f, -(0.1f + p.print_step[2] * float(ib)));
        const float x  = 0.40f * tr + 0.35f * tg + 0.18f * tb;
        const float y  = 0.20f * tr + 0.70f * tg + 0.08f * tb;
        const float z  = 0.02f * tr + 0.10f * tg + 0.95f * tb;
        float*      n  = &in.print_rgba[((std::size_t(ib) * edge + ig) * edge + ir) * 4];
        n[0]           = std::cbrt(x);
        n[1]           = std::cbrt(y);
        n[2]           = std::cbrt(z);
        n[3]           = 1.0f;
      }
  const float xyz_to_display[9] = {3.2404542f,  -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
                                   0.0415560f,  0.0556434f,  -0.2040259f, 1.0572252f};
  std::copy(xyz_to_display, xyz_to_display + 9, p.xyz_to_display);
  for (int k = 0; k < 3; ++k) {
    p.gamut_threshold[k] = 0.75f;
    p.gamut_scale[k]     = 0.2f;
  }
  p.gamut_power            = 1.2f;
  p.negative_grade_grey[0] = 1.0f;
  p.negative_grade_grey[1] = 1.05f;
  p.negative_grade_grey[2] = 1.1f;
  p.display_luminance[0]   = 0.2126f;
  p.display_luminance[1]   = 0.7152f;
  p.display_luminance[2]   = 0.0722f;

  if (variant == "film_light_table") {
    // Colour reversal on a light table, PQ display curve.
    p.light_table            = 1;
    p.display_surround_power = 0.12f;
    p.display_encoding       = CE_TF_ST2084;
  } else {
    // Print with the negative grade and the cinema look, sRGB display curve.
    p.cinema_look      = 1;
    p.negative_grade   = 1;
    p.display_encoding = CE_TF_SRGB;
  }
  in.inhibitor_blur = variant == "film_blur" ? 1 : 0;
  return in;
}

/// Linear AP1: luminance from 2^-12 to 2^4, out-of-gamut colours, and a row of zeros.
std::vector<float> MakeFilmImage(std::uint32_t width, std::uint32_t height) {
  std::vector<float> image(std::size_t(width) * height * 4);
  Random             random(0xf11a5eedULL);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      float* p = &image[(std::size_t(y) * width + x) * 4];
      if (y == 0) {
        p[0] = p[1] = p[2] = 0.0f;
      } else if (y < 8) {
        p[0] = p[1] = p[2] = std::exp2(-12.0f + 16.0f * float(x) / float(width - 1));
      } else {
        const float lum = std::exp2(random.Uniform(-12.0f, 4.0f));
        for (int c = 0; c < 3; ++c) p[c] = lum * (1.0f + random.Uniform(-0.9f, 1.5f));
        if (random.Next() % 50 == 0) p[random.Next() % 3] = -0.05f * lum;
      }
      p[3] = random.Uniform(0.0f, 1.0f);
    }
  return image;
}

/// The three kernels on the host, from the same headers (float32 C++ form).
void FilmCpu(const FilmInput& in, const std::vector<float>& image, std::vector<float>& q,
             std::vector<float>& a, std::vector<float>& out) {
  const auto  pixels = image.size() / 4;
  const auto* p      = &in.params;
  const float* values = in.values.data();
  q.resize(pixels * 3);
  a.resize(pixels * 3);
  out.resize(pixels * 4);
  for (std::size_t i = 0; i < pixels; ++i) {
    const AcesRgcRgb    c  = AcesReferenceGamutCompress(image[i * 4], image[i * 4 + 1],
                                                        image[i * 4 + 2]);
    const FilmKernelRgb qv = FilmKernelLayerExposure(p, values, c.r, c.g, c.b);
    FilmKernelRgb       av;
    FilmKernelRgb       qs = qv;
    if (in.inhibitor_blur != 0) {
      const auto first = FilmKernelFirstAmountsWithPointPart(p, values, qv);
      qs               = first.log_q;
      av               = first.amounts;
    } else {
      av = FilmKernelPointAmounts(p, values, qv);
    }
    q[i * 3] = qs.r, q[i * 3 + 1] = qs.g, q[i * 3 + 2] = qs.b;
    a[i * 3] = av.r, a[i * 3 + 1] = av.g, a[i * 3 + 2] = av.b;
    FilmKernelRgb linear;
    if (p->light_table != 0U) {
      linear = FilmKernelFinishDisplay(
          p, FilmKernelDisplaySurround(p, FilmKernelPrintDisplay(p, in.print_rgba.data(), av)));
    } else {
      FilmKernelRgb record = av;
      if (p->negative_grade != 0U) record = FilmKernelNegativeGrade(p, record, record);
      linear = FilmKernelFinishDisplay(p, FilmKernelPrintDisplay(p, in.print_rgba.data(), record));
      if (p->cinema_look != 0U) linear = FilmKernelCinemaLook(linear);
    }
    const int tf   = int(p->display_encoding);
    out[i * 4]     = CeDisplayEncodeChannel(tf, linear.r, 1.0f);
    out[i * 4 + 1] = CeDisplayEncodeChannel(tf, linear.g, 1.0f);
    out[i * 4 + 2] = CeDisplayEncodeChannel(tf, linear.b, 1.0f);
    out[i * 4 + 3] = image[i * 4 + 3];
  }
}

int RunFilm(const Options& o) {
  const auto        in     = MakeFilmInput(o.test_case);
  const auto        image  = MakeFilmImage(o.width, o.height);
  const std::size_t pixels = std::size_t(o.width) * o.height;
  std::vector<std::uint8_t> data(sizeof(FilmKernelParams));
  std::memcpy(data.data(), &in.params, sizeof(FilmKernelParams));
  const int w = int(o.width), h = int(o.height);

  // OpenCL: the program of kFilmPrintProgramName (opencl_gpu_dag_programs.cpp).
  OpenClCompute     cl(o.cl_device);
  const std::string inc = o.src_dir + "/include/";
  const std::string sh  = o.src_dir + "/edit/runtime/opencl/shader/";
  auto program = cl.Build({inc + "color/color_encoding_math.h",
                           inc + "edit/runtime/aces_reference_gamut_compression.h",
                           inc + "edit/runtime/film/film_print_math.h",
                           inc + "edit/runtime/film/film_grain_field.h",
                           inc + "edit/runtime/film/film_grain_tile.h",
                           inc + "edit/runtime/film/film_grain_negative.h", sh + "film_print.cl",
                           sh + "film_grain_negative.cl"},
                          "-cl-std=CL1.2");
  auto   k_exposure = cl.Kernel(program, "film_print_exposure");
  auto   k_first    = cl.Kernel(program, "film_print_first_amounts");
  auto   k_display  = cl.Kernel(program, "film_print_display");
  cl_mem scene      = cl.Image(o.width, o.height, image.data(), CL_MEM_READ_ONLY);
  cl_mem dst        = cl.Image(o.width, o.height, nullptr, CL_MEM_WRITE_ONLY);
  const float one_texel[4] = {0, 0, 0, 0};
  cl_mem flare      = cl.Image(1, 1, one_texel, CL_MEM_READ_ONLY);
  cl_mem dummy      = cl.Buffer(64);
  cl_mem c_data     = cl.Buffer(data.size(), data.data());
  cl_mem c_values   = cl.Buffer(in.values.size() * 4, in.values.data());
  cl_mem c_print    = cl.Buffer(in.print_rgba.size() * 4, in.print_rgba.data());
  cl_mem c_q        = cl.Buffer(pixels * 3 * 4);
  cl_mem c_a        = cl.Buffer(pixels * 3 * 4);
  const cl_uint zero = 0;
  const cl_int  izero = 0, ione = 1;
  using A = OpenClCompute;
  A::Arg(k_exposure, 0, scene);
  A::Arg(k_exposure, 1, dummy);
  A::Arg(k_exposure, 2, izero);
  A::Arg(k_exposure, 3, c_q);
  A::Arg(k_exposure, 4, zero);
  A::Arg(k_exposure, 5, c_values);
  A::Arg(k_exposure, 6, c_data);
  A::Arg(k_exposure, 7, zero);
  A::Arg(k_exposure, 8, w);
  A::Arg(k_exposure, 9, h);
  A::Arg(k_first, 0, c_q);
  A::Arg(k_first, 1, zero);
  A::Arg(k_first, 2, c_a);
  A::Arg(k_first, 3, zero);
  A::Arg(k_first, 4, cl_int(in.inhibitor_blur));
  A::Arg(k_first, 5, c_values);
  A::Arg(k_first, 6, c_data);
  A::Arg(k_first, 7, zero);
  A::Arg(k_first, 8, w);
  A::Arg(k_first, 9, h);
  const FilmMatrix identity{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
  const float      fzero = 0.0f;
  A::Arg(k_display, 0, c_a);
  A::Arg(k_display, 1, zero);
  A::Arg(k_display, 2, scene);
  A::Arg(k_display, 3, dummy);
  A::Arg(k_display, 4, izero);
  A::Arg(k_display, 5, dst);
  A::Arg(k_display, 6, dummy);
  A::Arg(k_display, 7, izero);
  A::Arg(k_display, 8, w);
  A::Arg(k_display, 9, h);
  A::Arg(k_display, 10, c_print);
  A::Arg(k_display, 11, flare);
  A::Arg(k_display, 12, izero);  // has_flare
  A::Arg(k_display, 13, ione);
  A::Arg(k_display, 14, ione);
  A::Arg(k_display, 15, identity);
  A::Arg(k_display, 16, fzero);
  A::Arg(k_display, 17, dummy);  // soft
  A::Arg(k_display, 18, zero);
  A::Arg(k_display, 19, izero);  // has_soft
  A::Arg(k_display, 20, izero);
  A::Arg(k_display, 21, izero);
  A::Arg(k_display, 22, izero);
  A::Arg(k_display, 23, c_data);
  A::Arg(k_display, 24, zero);
  std::array<Timing, 3> cl_time;
  const cl_kernel       cl_kernels[3] = {k_exposure, k_first, k_display};
  for (int i = 0; i <= o.repeats; ++i) {
    for (int k = 0; k < 3; ++k) {
      const double ms = cl.Run(cl_kernels[k], o.width, o.height);
      if (i > 0) cl_time[k].Add(ms);
    }
  }
  const auto cl_q   = cl.ReadBuffer(c_q, pixels * 3);
  const auto cl_a   = cl.ReadBuffer(c_a, pixels * 3);
  const auto cl_out = cl.ReadImage(dst, o.width, o.height);

  // Vulkan
  VulkanCompute     vk(o.vk_device, o.validation);
  const std::string spv = o.spv_dir + "/film_print.spv";
  const std::size_t v_kernels[3] = {
      vk.CreateKernel(spv, "film_print_exposure", sizeof(FilmPush), 2),
      vk.CreateKernel(spv, "film_print_first_amounts", sizeof(FilmPush), 2),
      vk.CreateKernel(spv, "film_print_display", sizeof(FilmPush), 2)};
  const auto v_scene  = vk.CreateImage(o.width, o.height, image.data());
  const auto v_dst    = vk.CreateImage(o.width, o.height);
  const auto v_data   = vk.CreateBuffer(data.size(), data.data());
  const auto v_values = vk.CreateBuffer(in.values.size() * 4, in.values.data());
  const auto v_print  = vk.CreateBuffer(in.print_rgba.size() * 4, in.print_rgba.data());
  const auto v_q      = vk.CreateBuffer(pixels * 3 * 4);
  const auto v_a      = vk.CreateBuffer(pixels * 3 * 4);
  const FilmPush push{v_data.address, v_values.address, v_q.address, v_a.address,
                      v_print.address, 0, 0, 0, w, h, in.inhibitor_blur};
  std::array<Timing, 3> vk_time;
  for (int i = 0; i <= o.repeats; ++i) {
    for (int k = 0; k < 3; ++k) {
      const double ms = vk.Dispatch(v_kernels[k], &push, {v_scene, v_dst}, o.width, o.height);
      if (i > 0) vk_time[k].Add(ms);
    }
  }
  std::vector<float> vk_q(pixels * 3), vk_a(pixels * 3);
  vk.Download(v_q, vk_q.data(), pixels * 3 * 4);
  vk.Download(v_a, vk_a.data(), pixels * 3 * 4);
  const auto vk_out = vk.DownloadImage(v_dst);

  std::vector<float> cpu_q, cpu_a, cpu_out;
  FilmCpu(in, image, cpu_q, cpu_a, cpu_out);

  std::printf("case %s  %u x %u  inhibitor_blur %d  display_encoding %u\n", o.test_case.c_str(),
              o.width, o.height, in.inhibitor_blur, in.params.display_encoding);
  std::printf("  OpenCL  %s  (%s)\n", cl.DeviceName().c_str(), cl.Driver().c_str());
  std::printf("  Vulkan  %s  (%s)  validation %s\n", vk.DeviceName().c_str(),
              vk.DriverInfo().c_str(), vk.ValidationEnabled() ? "on" : "off");
  constexpr double kCodes = 0.5;  // kRampLimitCodes of opencl_film_print_test.cpp
  const auto q_map = in.inhibitor_blur != 0 ? Identity : Log10Exposure;
  const auto q_vc  = Compare(cl_q, vk_q, 3, 0, 3, 1.0, 1e-4, q_map);
  const auto a_vc  = Compare(cl_a, vk_a, 3, 0, 3, 1.0, 1e-4, Identity);
  const auto o_vc  = Compare(cl_out, vk_out, 4, 0, 3, 255.0, kCodes, Identity);
  const auto o_vh  = Compare(cpu_out, vk_out, 4, 0, 3, 255.0, kCodes, Identity);
  const auto o_ch  = Compare(cpu_out, cl_out, 4, 0, 3, 255.0, kCodes, Identity);
  const auto alpha = Compare(cl_out, vk_out, 4, 3, 4, 1.0, 0.0, Identity);
  Report(in.inhibitor_blur ? "FP1/FP4 log Q, Vulkan - OpenCL" : "FP1 log10 Q, Vulkan - OpenCL",
         q_vc, 1e-4, "log10");
  Report("FP4/5 amounts, Vulkan - OpenCL", a_vc, 1e-4, "density");
  Report("FP6 output, Vulkan - OpenCL", o_vc, kCodes, "codes");
  Report("FP6 output, Vulkan - host", o_vh, kCodes, "codes");
  Report("FP6 output, OpenCL - host", o_ch, kCodes, "codes");
  Report("RGBA (alpha exact)", alpha, 0.0, "");
  {
    const auto i = o_vc.index;
    std::printf("  worst: in (%.4e %.4e %.4e)  cl (%.6f %.6f %.6f)  vk (%.6f %.6f %.6f)  host "
                "(%.6f %.6f %.6f)\n",
                image[i * 4], image[i * 4 + 1], image[i * 4 + 2], cl_out[i * 4],
                cl_out[i * 4 + 1], cl_out[i * 4 + 2], vk_out[i * 4], vk_out[i * 4 + 1],
                vk_out[i * 4 + 2], cpu_out[i * 4], cpu_out[i * 4 + 1], cpu_out[i * 4 + 2]);
  }
  const char* names[3] = {"exposure", "first_amounts", "display"};
  for (int k = 0; k < 3; ++k) {
    std::printf("  time    %-14s OpenCL %.3f ms  Vulkan %.3f ms (best of %d)\n", names[k],
                cl_time[k].best, vk_time[k].best, o.repeats);
  }
  std::printf("  validation errors %u, warnings %u\n", vk.ValidationErrors(),
              vk.ValidationWarnings());
  const bool pass = o_vc.over == 0 && o_vc.nan_a == o_vc.nan_b && vk.ValidationErrors() == 0;
  std::printf("RESULT %s %s %s largest %.2e codes tol %.1f\n", o.test_case.c_str(),
              vk.DeviceName().c_str(), pass ? "PASS" : "FAIL", o_vc.largest, kCodes);
  return pass ? 0 : 1;
}

}  // namespace
}  // namespace vx1

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  vx1::Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string arg  = argv[i];
    auto              next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
      return argv[++i];
    };
    try {
      if (arg == "--case") o.test_case = next();
      else if (arg == "--vk-device") o.vk_device = next();
      else if (arg == "--cl-device") o.cl_device = next();
      else if (arg == "--spv") o.spv_dir = next();
      else if (arg == "--src") o.src_dir = next();
      else if (arg == "--width") o.width = static_cast<std::uint32_t>(std::stoul(next()));
      else if (arg == "--height") o.height = static_cast<std::uint32_t>(std::stoul(next()));
      else if (arg == "--repeats") o.repeats = std::stoi(next());
      else if (arg == "--grade-spv") o.grade_spv = next();
      else if (arg == "--grade-entry") o.grade_entry = next();
      else if (arg == "--input") o.photo_input = next() == "photo";
      else if (arg == "--no-validation") o.validation = false;
      else throw std::runtime_error("unknown argument " + arg);
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      return 2;
    }
  }
  try {
    if (o.test_case == "pipeline") {
      // Pipeline creation only: --grade-spv names the SPIR-V file of the grade kernel.
      vx1::VulkanCompute vk(o.vk_device, o.validation);
      const auto         start = std::chrono::steady_clock::now();
      vk.CreateKernel(o.spv_dir + "/" + o.grade_spv, o.grade_entry, 48, 2);
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      std::printf("PIPELINE OK %s %s %.1f ms (no pipeline cache)\n", o.grade_spv.c_str(),
                  vk.DeviceName().c_str(), ms);
      return 0;
    }
    if (o.test_case == "grade_cross") return vx1::RunGradeCross(o);
    if (o.test_case == "grade") return vx1::RunGrade(o);
    if (o.test_case == "grade_photo") {
      o.photo_input = true;
      return vx1::RunGrade(o);
    }
    if (o.test_case.rfind("film", 0) == 0) return vx1::RunFilm(o);
    std::cerr << "unknown case " << o.test_case << "\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 3;
  }
}
