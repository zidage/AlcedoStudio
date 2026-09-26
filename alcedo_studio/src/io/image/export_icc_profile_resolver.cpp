//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/export_icc_profile_resolver.hpp"

#include <fstream>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGColorSpace.h>
#endif

namespace alcedo {
namespace {

auto CanonicalPath(const std::filesystem::path& path) -> std::filesystem::path {
  if (path.empty()) {
    return {};
  }

  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  return ec ? path.lexically_normal() : canonical;
}

auto IsRegularFile(const std::filesystem::path& path) -> bool {
  if (path.empty()) {
    return false;
  }

  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec &&
         std::filesystem::is_regular_file(path, ec) && !ec;
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
      return CanonicalPath(std::filesystem::path(buffer).parent_path());
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
    return {};
  }
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return CanonicalPath(std::filesystem::path(buffer.c_str()).parent_path());
#else
  return {};
#endif
}

auto ResolveConfigProfileFileName(const ExportColorProfileConfig& config) -> std::string_view {
  using ColorSpace = ColorUtils::ColorSpace;
  using EOTF = ColorUtils::EOTF;

  switch (config.encoding_space) {
    case ColorSpace::REC709:
      switch (config.encoding_eotf) {
        case EOTF::BT1886:
          return "rec709_bt1886.icc";
        case EOTF::GAMMA_2_2:
          return "rec709_gamma22.icc";
        default:
          return {};
      }
    case ColorSpace::P3_D65:
      switch (config.encoding_eotf) {
        case EOTF::GAMMA_2_2:
          return "p3_d65_gamma22.icc";
        case EOTF::ST2084:
          return "p3_d65_pq.icc";
        default:
          return {};
      }
    case ColorSpace::P3_D60:
      return config.encoding_eotf == EOTF::GAMMA_2_6 ? "p3_d60_gamma26.icc" : std::string_view{};
    case ColorSpace::P3_DCI:
      return config.encoding_eotf == EOTF::GAMMA_2_6 ? "p3_dci_gamma26.icc" : std::string_view{};
    case ColorSpace::XYZ:
      return config.encoding_eotf == EOTF::GAMMA_2_6 ? "xyz_gamma26.icc" : std::string_view{};
    case ColorSpace::REC2020:
      switch (config.encoding_eotf) {
        case EOTF::ST2084:
          return "rec2020_pq.icc";
        case EOTF::HLG:
          return "rec2020_hlg.icc";
        default:
          return {};
      }
    default:
      return {};
  }
}

auto ResolveConfigSearchRoots() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> roots;

  // A packaged ICC directory next to the executable is authoritative: a profile missing from it
  // must not be silently read from the build machine's source checkout.
  const auto exe_dir = GetExecutableDir();
  if (!exe_dir.empty()) {
    for (const auto& packaged_root : {exe_dir / "config" / "icc", exe_dir / "icc"}) {
      std::error_code ec;
      if (std::filesystem::is_directory(packaged_root, ec) && !ec) {
        roots.emplace_back(packaged_root);
      }
    }
    if (!roots.empty()) {
      return roots;
    }
  }

#ifdef CONFIG_PATH
  roots.emplace_back(std::filesystem::path(CONFIG_PATH) / "icc");
#endif
  roots.emplace_back(std::filesystem::path("config/icc"));
  roots.emplace_back(std::filesystem::path("src/config/icc"));
  roots.emplace_back(std::filesystem::path("alcedo/src/config/icc"));

  return roots;
}

#if defined(__APPLE__)
auto ResolveLinearAppleColorSpace(ColorUtils::ColorSpace encoding_space) -> CFStringRef {
  switch (encoding_space) {
    case ColorUtils::ColorSpace::REC709:
      return kCGColorSpaceLinearSRGB;
    case ColorUtils::ColorSpace::P3_D65:
    case ColorUtils::ColorSpace::P3_D60:
    case ColorUtils::ColorSpace::P3_DCI:
      return kCGColorSpaceExtendedLinearDisplayP3;
    case ColorUtils::ColorSpace::REC2020:
      return kCGColorSpaceExtendedLinearITUR_2020;
    case ColorUtils::ColorSpace::XYZ:
      return kCGColorSpaceGenericXYZ;
    default:
      return nullptr;
  }
}

auto ResolveAppleColorSpace(const ExportColorProfileConfig& config) -> CFStringRef {
  if (config.encoding_eotf == ColorUtils::EOTF::LINEAR) {
    return ResolveLinearAppleColorSpace(config.encoding_space);
  }

  switch (config.encoding_space) {
    case ColorUtils::ColorSpace::REC709:
      switch (config.encoding_eotf) {
        case ColorUtils::EOTF::ST2084:
          return kCGColorSpaceITUR_709_PQ;
        case ColorUtils::EOTF::HLG:
          return kCGColorSpaceITUR_709_HLG;
        case ColorUtils::EOTF::BT1886:
        case ColorUtils::EOTF::GAMMA_2_2:
          return kCGColorSpaceITUR_709;
        default:
          return nullptr;
      }
    case ColorUtils::ColorSpace::P3_D65:
      switch (config.encoding_eotf) {
        case ColorUtils::EOTF::ST2084:
          return kCGColorSpaceDisplayP3_PQ;
        case ColorUtils::EOTF::HLG:
          return kCGColorSpaceDisplayP3_HLG;
        case ColorUtils::EOTF::GAMMA_2_2:
          return kCGColorSpaceDisplayP3;
        default:
          return nullptr;
      }
    case ColorUtils::ColorSpace::P3_D60:
      return config.encoding_eotf == ColorUtils::EOTF::GAMMA_2_6 ? kCGColorSpaceDisplayP3
                                                                  : nullptr;
    case ColorUtils::ColorSpace::P3_DCI:
      return config.encoding_eotf == ColorUtils::EOTF::GAMMA_2_6 ? kCGColorSpaceDCIP3 : nullptr;
    case ColorUtils::ColorSpace::REC2020:
      switch (config.encoding_eotf) {
        case ColorUtils::EOTF::ST2084:
          return kCGColorSpaceITUR_2100_PQ;
        case ColorUtils::EOTF::HLG:
          return kCGColorSpaceITUR_2100_HLG;
        default:
          return nullptr;
      }
    case ColorUtils::ColorSpace::XYZ:
      return config.encoding_eotf == ColorUtils::EOTF::GAMMA_2_6 ? kCGColorSpaceGenericXYZ
                                                                  : nullptr;
    default:
      return nullptr;
  }
}

auto ResolveAppleIccProfileBytes(const ExportColorProfileConfig& config) -> std::vector<uint8_t> {
  std::vector<uint8_t> bytes;
  CFStringRef color_name = ResolveAppleColorSpace(config);
  if (!color_name) {
    return bytes;
  }

  CGColorSpaceRef color_space = CGColorSpaceCreateWithName(color_name);
  if (!color_space) {
    return bytes;
  }

  CFDataRef icc_data = CGColorSpaceCopyICCData(color_space);
  CGColorSpaceRelease(color_space);
  if (!icc_data) {
    return bytes;
  }

  const auto size = static_cast<size_t>(CFDataGetLength(icc_data));
  bytes.resize(size);
  if (size > 0) {
    CFDataGetBytes(icc_data, CFRangeMake(0, static_cast<CFIndex>(size)), bytes.data());
  }
  CFRelease(icc_data);
  return bytes;
}
#endif

auto ReadFileBytes(const std::filesystem::path& path) -> std::vector<uint8_t> {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

}  // namespace

auto ExportIccProfileResolver::ResolveIccProfileBytes(const ExportColorProfileConfig& config)
    -> std::vector<uint8_t> {
#if defined(__APPLE__)
  if (const auto apple_bytes = ResolveAppleIccProfileBytes(config); !apple_bytes.empty()) {
    return apple_bytes;
  }
#endif

  const auto path = ResolveConfigProfilePath(config);
  return path.has_value() ? ReadFileBytes(*path) : std::vector<uint8_t>{};
}

auto ExportIccProfileResolver::ResolveConfigProfilePath(const ExportColorProfileConfig& config)
    -> std::optional<std::filesystem::path> {
  const std::string_view file_name = ResolveConfigProfileFileName(config);
  if (file_name.empty()) {
    return std::nullopt;
  }

  for (const auto& root : ResolveConfigSearchRoots()) {
    const auto candidate = root / std::filesystem::path(std::string(file_name));
    if (IsRegularFile(candidate)) {
      return CanonicalPath(candidate);
    }
  }

  return std::nullopt;
}

}  // namespace alcedo
