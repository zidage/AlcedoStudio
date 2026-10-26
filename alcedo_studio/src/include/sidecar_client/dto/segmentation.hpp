//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace alcedo {

/// Outcome of one segmentation call. `status` and `error_code` are the `alcedo.ai`
/// AiResponseStatus and AiErrorCode values; a transport failure sets `ok = false` with the gRPC
/// message and leaves `status` at AI_STATUS_UNSPECIFIED.
struct SegmentationCallStatus {
  bool        ok         = false;
  int         status     = 0;
  int         error_code = 0;
  std::string error;
};

/// An RGB8 image for a segmentation session: row-major, 3 bytes per pixel, long edge at most
/// 4096 px. A start request from the image embedding cache only sends an empty image (width 0,
/// height 0, no pixels).
struct SegmentationRgb8Image {
  uint32_t             width  = 0;
  uint32_t             height = 0;
  std::vector<uint8_t> pixels;
};

enum class SegmentationPointLabel : uint8_t {
  kSubtract = 0,
  kAdd      = 1,
};

/// A prompt point in normalized image coordinates [0, 1].
struct SegmentationPoint {
  float                  x     = 0.0F;
  float                  y     = 0.0F;
  SegmentationPointLabel label = SegmentationPointLabel::kAdd;
};

/// A prompt box in normalized image coordinates; the corners can come in any order.
struct SegmentationBox {
  float x0 = 0.0F;
  float y0 = 0.0F;
  float x1 = 0.0F;
  float y1 = 0.0F;
};

/// The decoder input of one stroke, click, or box: all points and the box of the selection, and
/// the selected logits of the previous decode (empty for the first decode).
struct SegmentationPrompt {
  std::vector<SegmentationPoint> points;
  std::optional<SegmentationBox> box;
  std::vector<float>             mask_input;
};

/// One decoder mask token: logit_size x logit_size logits clipped to +-32.
struct SegmentationMaskLevel {
  std::vector<float> logits;
  float              score = 0.0F;
  uint32_t           token = 0;
  uint32_t           area  = 0;
};

/// Result of a decode. A click returns three levels sorted by area; other prompts return one
/// mask.
struct SegmentationDecodeResult {
  SegmentationCallStatus             status;
  std::vector<SegmentationMaskLevel> masks;
  uint32_t                           selected_index = 0;
  uint32_t                           logit_size     = 0;
  int64_t                            decoder_ms     = 0;
};

struct SegmentationSessionResult {
  SegmentationCallStatus status;
  std::string            session_id;
  /// Time of the preprocessing, the encoder, the head, and the person crops; 0 for a cached
  /// image.
  int64_t                encoder_ms       = 0;
  /// True when the session uses the cached image embedding of the image key.
  bool                   from_cache       = false;
  /// True when a start without pixels names an image key that the sidecar does not cache
  /// (AI_ERROR_IMAGE_NOT_CACHED). The caller renders the image and starts again with the pixels.
  bool                   image_not_cached = false;
  /// Size of the session image: the sent size for a start with pixels, the cached size for a
  /// start from the cache. 0 when the start fails.
  uint32_t               image_width      = 0;
  uint32_t               image_height     = 0;
};

/// A task mask of the sidecar (SM3.C, SM3.D).
enum class SegmentationTask : uint8_t {
  kSubject    = 1,
  kBackground = 2,
  kSky        = 3,
  /// The Subject alpha and the People list.
  kPeople     = 4,
  /// The S3OD alpha of the crop of one person of the People list (needs a person index).
  kPerson     = 5,
};

/// The value kind of a task mask.
enum class SegmentationTaskMaskKind : uint8_t {
  /// No mask (the task found nothing or the call failed).
  kNone   = 0,
  /// Sky of a sidecar without the sky head (Core ML, until #538): `mask_r8` at the session image
  /// size, 255 inside and 0 outside. It is the union of the region masks, each the bilinear
  /// upsampling of its decoder logits with threshold 0.
  kBinary = 1,
  /// Subject, Background, People, Person, and Sky of the sky head (ONNX Runtime): `alpha_values`
  /// in [0, 1]; inside where >= 0.5. The grid covers `mask_box` of the session image: the image
  /// resized to a long edge of 1024, the person crop resized to a long edge of 1024 (Person), or
  /// the image stretched to 1024 x 1024 (Sky; each axis has its own scale).
  kAlpha  = 2,
};

/// One entry of the People list: the normalized box, clipped to the image, the score, and the
/// share of the box in the Subject alpha. The list is sorted from left to right; `index` is the
/// position in the list.
struct SegmentationPerson {
  uint32_t        index = 0;
  SegmentationBox box;
  float           score            = 0.0F;
  /// The share of the Subject alpha grid pixels inside the box with an alpha >= 0.5. kPerson: 0
  /// while the image has no Subject result.
  float           subject_fraction = 0.0F;
  /// The sidecar rule for "the person is included in the Subject alpha".
  bool            in_subject       = false;
};

/// Result of a task mask. A task that finds nothing has `status.ok`, `found == false`, and the
/// reason in `message`.
struct SegmentationTaskResult {
  SegmentationCallStatus          status;
  bool                            found = false;
  std::string                     message;
  SegmentationTaskMaskKind        mask_kind   = SegmentationTaskMaskKind::kNone;
  uint32_t                        mask_width  = 0;
  uint32_t                        mask_height = 0;
  /// kBinary: mask_width x mask_height values, row-major.
  std::vector<uint8_t>            mask_r8;
  /// kAlpha: mask_width x mask_height values, row-major.
  std::vector<float>              alpha_values;
  /// kAlpha: the normalized rectangle of the session image that the alpha grid covers;
  /// (0, 0, 1, 1) except for kPerson, where it is the crop rectangle.
  SegmentationBox                 mask_box;
  /// People and Person: the presence of the people head and its threshold. Sky with the sky
  /// head: the share of the alpha values >= 0.5 and the share that "found" needs; Sky without
  /// the sky head: the sky presence of the people and sky head and its threshold.
  float                           presence           = 0.0F;
  float                           presence_threshold = 0.0F;
  /// Sky without the sky head: the number of sky regions in the union; 0 with the sky head.
  uint32_t                        region_count       = 0;
  /// People and Person: the full People list.
  std::vector<SegmentationPerson> people;
  /// Subject, Background, and People: the S3OD IoU score of the image; Person: of the crop.
  float                           subject_score = 0.0F;
  /// Number of mask values inside.
  uint32_t                        mask_area     = 0;
  /// Time of the model runs of the request; 0 when the sidecar read stored results only.
  int64_t                         model_ms      = 0;
};

struct SegmentationModelResidency {
  std::string role;
  std::string device;
  int64_t     load_ms = 0;
};

struct SegmentationStatus {
  SegmentationCallStatus                  status;
  bool                                    loaded = false;
  std::string                             model_set_id;
  std::string                             model_directory;
  std::string                             device;
  std::string                             backend;
  std::vector<SegmentationModelResidency> models;
  uint32_t                                session_count      = 0;
  uint32_t                                cached_image_count = 0;
};

}  // namespace alcedo
