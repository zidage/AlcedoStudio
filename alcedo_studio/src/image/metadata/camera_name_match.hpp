//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <vector>

/// Camera name normalization and tokens, for the camera color matrix lookup and the DNG camera
/// model choice.
namespace alcedo::metadata_detail {

/// Lower-case alphanumeric words of @p input, with a space at each letter and digit boundary
/// ("Z5" becomes "z 5").
auto NormalizeCameraName(const std::string& input) -> std::string;
auto CompactNormalizedCameraName(std::string normalized) -> std::string;
auto CompactCameraName(const std::string& input) -> std::string;
auto TokenizeCameraNameLoose(const std::string& input) -> std::vector<std::string>;
auto UniqueMeaningfulCameraTokens(const std::vector<std::string>& tokens)
    -> std::vector<std::string>;
auto ContainsCameraToken(const std::vector<std::string>& tokens, const std::string& token) -> bool;
auto ContainsCameraTokenFragment(const std::vector<std::string>& tokens,
                                 const std::string&              fragment) -> bool;
auto HasMeaningfulCameraModelToken(const std::string& camera_model) -> bool;

}  // namespace alcedo::metadata_detail
