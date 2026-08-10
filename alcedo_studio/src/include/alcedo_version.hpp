//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

// CMake sets ALCEDO_APP_NAME / ALCEDO_APP_VERSION on targets that need them
// (writers, alcedo_main). Defaults keep the header usable in isolated builds.
#ifndef ALCEDO_APP_NAME
#define ALCEDO_APP_NAME "Alcedo Studio"
#endif

#ifndef ALCEDO_APP_VERSION
#define ALCEDO_APP_VERSION "0.0.0"
#endif

namespace alcedo {

/// EXIF IFD0 Software (0x0131) value: "<name> <version>".
inline auto AlcedoSoftwareExifString() -> std::string {
  return std::string(ALCEDO_APP_NAME) + " " + ALCEDO_APP_VERSION;
}

}  // namespace alcedo
