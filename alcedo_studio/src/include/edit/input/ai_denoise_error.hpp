//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace alcedo {

/// The kind of an AI Denoise render error (integration plan, 2.4), for the text and the controls
/// of the editor (DN7) and the export (DN8). Each kind has the control `Turn Off AI Denoise`.
enum class AiDenoiseErrorKind : std::uint8_t {
  /// The graph file is not in the model folder: the control to the AI Models page too.
  ModelNotInstalled,
  /// The projected time of a photo is above `slow_device_limit` (kept for the device).
  DeviceTooSlow,
  /// The runtime failed: ONNX Runtime, DirectML, Direct3D 12, the video memory, an output value
  /// that is not finite, a platform without forward code, or input that the model cannot take.
  RunFailed,
  /// The photo is not a Bayer RAW file (D14): X-Trans, monochrome, or decoded RGB.
  SensorNotSupported,
};

/// The token of @p kind in a render message: `model_not_installed`, `device_too_slow`,
/// `run_failed`, `sensor_not_supported`.
[[nodiscard]] inline auto AiDenoiseErrorKindToken(AiDenoiseErrorKind kind) -> std::string_view {
  switch (kind) {
    case AiDenoiseErrorKind::ModelNotInstalled:
      return "model_not_installed";
    case AiDenoiseErrorKind::DeviceTooSlow:
      return "device_too_slow";
    case AiDenoiseErrorKind::RunFailed:
      return "run_failed";
    case AiDenoiseErrorKind::SensorNotSupported:
      return "sensor_not_supported";
  }
  return "run_failed";
}

namespace ai_denoise_detail {

// Header-only: the editor session reads the error from the render message without the loader
// library (EditInput) and its dependencies.
inline constexpr std::string_view kRenderErrorMarker     = "AI Denoise error (";
inline constexpr std::string_view kRenderErrorAfterToken = "): ";

inline auto RenderMessage(AiDenoiseErrorKind kind, const std::string& text) -> std::string {
  std::string message{kRenderErrorMarker};
  message += AiDenoiseErrorKindToken(kind);
  message += kRenderErrorAfterToken;
  message += text;
  return message;
}

}  // namespace ai_denoise_detail

/**
 * @brief A render error of AI Denoise.
 *
 * `what()` is `AI Denoise error (<kind token>): <text>`, so the kind stays in the render message
 * that the scheduler, the render coordinator, and the export pass on as text. @ref Text is the
 * text alone.
 */
class AiDenoiseError : public std::runtime_error {
 public:
  AiDenoiseError(AiDenoiseErrorKind kind, const std::string& text)
      : std::runtime_error(ai_denoise_detail::RenderMessage(kind, text)),
        kind_(kind),
        text_(text) {}

  [[nodiscard]] auto Kind() const -> AiDenoiseErrorKind { return kind_; }
  [[nodiscard]] auto Text() const -> const std::string& { return text_; }

 private:
  AiDenoiseErrorKind kind_;
  std::string        text_;
};

/// The kind and the text of an AI Denoise error in a render message.
struct AiDenoiseRenderError {
  AiDenoiseErrorKind kind = AiDenoiseErrorKind::RunFailed;
  std::string        text;
};

inline auto operator==(const AiDenoiseRenderError& a, const AiDenoiseRenderError& b) -> bool {
  return a.kind == b.kind && a.text == b.text;
}

/**
 * @brief The AI Denoise error in @p message, the `what()` of an @ref AiDenoiseError anywhere in
 *        it (a caller can put text before it).
 * @return std::nullopt when the message has no AI Denoise error.
 */
[[nodiscard]] inline auto ParseAiDenoiseRenderError(std::string_view message)
    -> std::optional<AiDenoiseRenderError> {
  using ai_denoise_detail::kRenderErrorAfterToken;
  using ai_denoise_detail::kRenderErrorMarker;
  const auto marker = message.find(kRenderErrorMarker);
  if (marker == std::string_view::npos) {
    return std::nullopt;
  }
  const auto token_start = marker + kRenderErrorMarker.size();
  const auto token_end   = message.find(kRenderErrorAfterToken, token_start);
  if (token_end == std::string_view::npos) {
    return std::nullopt;
  }
  const auto token = message.substr(token_start, token_end - token_start);
  for (const auto kind :
       std::array{AiDenoiseErrorKind::ModelNotInstalled, AiDenoiseErrorKind::DeviceTooSlow,
                  AiDenoiseErrorKind::RunFailed, AiDenoiseErrorKind::SensorNotSupported}) {
    if (AiDenoiseErrorKindToken(kind) == token) {
      return AiDenoiseRenderError{
          kind, std::string(message.substr(token_end + kRenderErrorAfterToken.size()))};
    }
  }
  return std::nullopt;
}

}  // namespace alcedo
