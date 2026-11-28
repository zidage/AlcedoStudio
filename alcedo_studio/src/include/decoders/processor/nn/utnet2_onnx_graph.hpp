//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <onnxruntime_cxx_api.h>

#include <filesystem>
#include <string>

// The check of the FP16 UtNet2 graph that both platform sessions run after they load it
// (integration plan, DN2 and DN10). Only the platform session libraries include this header: it
// needs the ONNX Runtime headers of their dependency (alcedo::onnxruntime_directml on Windows,
// alcedo::onnxruntime_macos on macOS).

namespace alcedo::nn {

/// The UTF-8 text of `path`, for error texts.
[[nodiscard]] auto UtNet2PathText(const std::filesystem::path& path) -> std::string;

/// Checks the graph of `session`, loaded from `path`, against `UtNet2Spec`: one input and one
/// output with the names and the float16 type of the specification and 4 dimensions with the
/// channel counts; the metadata values `architecture`, `precision`, and
/// `input_preparation_version`; a `weights_sha256` of 64 lower-case hex digits; a finite
/// `output_scale` above 0. Returns the `output_scale`. Throws `std::runtime_error` that names
/// `path` and the first difference.
[[nodiscard]] auto CheckUtNet2Graph(Ort::Session& session, const std::filesystem::path& path)
    -> float;

}  // namespace alcedo::nn
