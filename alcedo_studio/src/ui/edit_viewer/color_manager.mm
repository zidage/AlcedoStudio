//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#import <AppKit/AppKit.h>
#include <CoreGraphics/CGColorSpace.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

#include "ui/edit_viewer/color_manager.hpp"

#include <QtCore/qlogging.h>
#include <QDebug>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace alcedo {
namespace {

auto FindMetalLayerInLayerHierarchy(CALayer* layer) -> CAMetalLayer* {
  if (!layer) {
    return nil;
  }
  if ([layer isKindOfClass:[CAMetalLayer class]]) {
    return (CAMetalLayer*)layer;
  }
  for (CALayer* sublayer in layer.sublayers) {
    if (CAMetalLayer* metal_layer = FindMetalLayerInLayerHierarchy(sublayer)) {
      return metal_layer;
    }
  }
  return nil;
}

auto FindMetalLayerInViewHierarchy(NSView* view) -> CAMetalLayer* {
  if (!view) {
    return nil;
  }
  if (CAMetalLayer* metal_layer = FindMetalLayerInLayerHierarchy(view.layer)) {
    return metal_layer;
  }
  for (NSView* subview in view.subviews) {
    if (CAMetalLayer* metal_layer = FindMetalLayerInViewHierarchy(subview)) {
      return metal_layer;
    }
  }
  return nil;
}

auto ResolveLinearColorSpace(ColorUtils::ColorSpace encoding_space, bool* exact_match)
    -> CFStringRef {
  if (exact_match) {
    *exact_match = true;
  }

  switch (encoding_space) {
    case ColorUtils::ColorSpace::REC709:
      return kCGColorSpaceLinearSRGB;
    case ColorUtils::ColorSpace::P3_D65:
      return kCGColorSpaceExtendedLinearDisplayP3;
    case ColorUtils::ColorSpace::P3_D60:
    case ColorUtils::ColorSpace::P3_DCI:
      if (exact_match) {
        *exact_match = false;
      }
      return kCGColorSpaceExtendedLinearDisplayP3;
    case ColorUtils::ColorSpace::REC2020:
      return kCGColorSpaceExtendedLinearITUR_2020;
    case ColorUtils::ColorSpace::XYZ:
      if (exact_match) {
        *exact_match = false;
      }
      return kCGColorSpaceGenericXYZ;
    default:
      if (exact_match) {
        *exact_match = false;
      }
      return kCGColorSpaceExtendedLinearSRGB;
  }
}

auto ResolveNamedColorSpace(const ViewerDisplayConfig& config, bool* exact_match) -> CFStringRef {
  if (config.encoding_eotf == ColorUtils::EOTF::LINEAR) {
    return ResolveLinearColorSpace(config.encoding_space, exact_match);
  }

  if (exact_match) {
    *exact_match = true;
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
          return kCGColorSpaceSRGB;
        default:
          if (exact_match) {
            *exact_match = false;
          }
          return kCGColorSpaceSRGB;
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
          if (exact_match) {
            *exact_match = false;
          }
          return kCGColorSpaceDisplayP3;
      }
    case ColorUtils::ColorSpace::P3_D60:
      if (exact_match) {
        *exact_match = false;
      }
      return kCGColorSpaceDisplayP3;
    case ColorUtils::ColorSpace::P3_DCI:
      if (config.encoding_eotf != ColorUtils::EOTF::GAMMA_2_6 && exact_match) {
        *exact_match = false;
      }
      return kCGColorSpaceDCIP3;
    case ColorUtils::ColorSpace::REC2020:
      switch (config.encoding_eotf) {
        case ColorUtils::EOTF::ST2084:
          return kCGColorSpaceITUR_2100_PQ;
        case ColorUtils::EOTF::HLG:
          return kCGColorSpaceITUR_2100_HLG;
        default:
          return kCGColorSpaceITUR_2020;
      }
    case ColorUtils::ColorSpace::XYZ:
      return kCGColorSpaceGenericXYZ;
    default:
      if (exact_match) {
        *exact_match = false;
      }
      return kCGColorSpaceSRGB;
  }
}

void LogFallbackColorSpace(const ViewerDisplayConfig& config, CFStringRef resolved_name) {
  QString resolved = QStringLiteral("<null>");
  if (resolved_name) {
    char buffer[256] = {};
    if (CFStringGetCString(resolved_name, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
      resolved = QString::fromUtf8(buffer);
    }
  }
  qWarning().noquote()
      << QStringLiteral("ColorManager: falling back to %1 for encoding_space=%2 encoding_eotf=%3")
             .arg(resolved,
                  QString::fromStdString(ColorUtils::ColorSpaceToString(config.encoding_space)),
                  QString::fromStdString(ColorUtils::EOTFToString(config.encoding_eotf)));
}

auto ClampHdrPeakLuminance(float peak_luminance) -> float {
  if (!std::isfinite(peak_luminance)) {
    return 100.0f;
  }
  return std::clamp(peak_luminance, 100.0f, 10000.0f);
}

auto ResolveEDRMetadata(const ViewerDisplayConfig& config) -> CAEDRMetadata* {
  if (@available(macOS 10.15, *)) {
    if (config.encoding_eotf == ColorUtils::EOTF::ST2084) {
      const float peak_luminance = ClampHdrPeakLuminance(config.peak_luminance);
      return [CAEDRMetadata HDR10MetadataWithMinLuminance:0.0001f
                                             maxLuminance:peak_luminance
                                       opticalOutputScale:10000.0f];
    }
    if (config.encoding_eotf == ColorUtils::EOTF::HLG) {
      return [CAEDRMetadata HLGMetadata];
    }
  }
  return nil;
}

}  // namespace

auto ColorManager::ApplyWindowColorSpace(void*                      native_view_or_window,
                                         const ViewerDisplayConfig& config) -> bool {
  const auto native_address = reinterpret_cast<std::uintptr_t>(native_view_or_window);
  // Qt offscreen `QWindow::winId()` can be a non-zero dummy such as 1. Sending
  // -isKindOfClass: to that pointer is a SIGSEGV, not a clean ObjC miss.
  if (native_address < 4096u) {
    return false;
  }

  id        object = (__bridge id)native_view_or_window;

  NSWindow* window = nil;
  NSView*   view   = nil;
  if ([object isKindOfClass:[NSWindow class]]) {
    window = (NSWindow*)object;
    view   = window.contentView;
  } else if ([object isKindOfClass:[NSView class]]) {
    view   = (NSView*)object;
    window = view.window;
  } else {
    return false;
  }

  CAMetalLayer* metal_layer = FindMetalLayerInViewHierarchy(view);
  if (!metal_layer && window) {
    metal_layer = FindMetalLayerInViewHierarchy(window.contentView);
  }
  if (!metal_layer && window.contentView.superview) {
    metal_layer = FindMetalLayerInViewHierarchy(window.contentView.superview);
  }
  if (!metal_layer) {
    return false;
  }

  bool        exact_match = true;
  CFStringRef color_name  = ResolveNamedColorSpace(config, &exact_match);
  if (!color_name) {
    return false;
  }

  const bool uses_hdr_transfer = config.encoding_eotf == ColorUtils::EOTF::ST2084 ||
                                 config.encoding_eotf == ColorUtils::EOTF::HLG;
  metal_layer.wantsExtendedDynamicRangeContent = uses_hdr_transfer;
  if (@available(macOS 10.15, *)) {
    metal_layer.EDRMetadata = uses_hdr_transfer ? ResolveEDRMetadata(config) : nil;
  }

  CGColorSpaceRef color_space = CGColorSpaceCreateWithName(color_name);
  if (!color_space) {
    return false;
  }

  metal_layer.colorspace = color_space;

  CGColorSpaceRelease(color_space);

  if (!exact_match) {
    LogFallbackColorSpace(config, color_name);
  }
  return true;
}

}  // namespace alcedo
