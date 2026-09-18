//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/raw_decoder.hpp"

#include <libraw/libraw_const.h>
#include <opencv2/core/hal/interface.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/matx.hpp>
#include <opencv2/opencv.hpp>
#include <stdexcept>

#include "decoders/libraw_unpack_guard.hpp"
#include "decoders/dng_default_crop.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata_extractor.hpp"
#include "type/type.hpp"

namespace alcedo {
/**
 * @brief A callback used to decode a raw file
 *
 * @param file
 * @param file_path
 * @param id
 */
void RawDecoder::Decode(std::vector<char> buffer, std::filesystem::path file_path,
                        std::shared_ptr<BufferQueue> result, image_id_t id,
                        std::shared_ptr<std::promise<image_id_t>> promise) {
  // TODO: Add Implementation
}

void RawDecoder::Decode(std::vector<char>&& buffer, std::shared_ptr<Image> source_img) {
  // TODO: Add Implementation

  // LibRaw is too large for ASan-instrumented worker-thread stacks on macOS.
  auto raw_processor = std::make_unique<LibRaw>();
  int  ret           = raw_processor->open_buffer((void*)buffer.data(), buffer.size());
  if (ret != LIBRAW_SUCCESS) {
    throw std::runtime_error("RawDecoder: Unable to read raw file using LibRAW");
  }

  // Default set output color space to ACES2065-1 (AP0)
  raw_processor->imgdata.params.output_color   = 6;
  raw_processor->imgdata.params.output_bps     = 16;
  raw_processor->imgdata.params.gamm[0]        = 1.0;  // Linear gamma
  raw_processor->imgdata.params.gamm[1]        = 1.0;
  raw_processor->imgdata.params.no_auto_bright = 0;  // Disable auto brightness
  raw_processor->imgdata.params.use_camera_wb  = 1;

  raw_processor->imgdata.rawparams.use_dngsdk  = 1;
  libraw_guard::Unpack(*raw_processor);

  // Build a pre-populated context from the Image or extract from LibRaw.
  RawRuntimeColorContext ctx;
  if (source_img && source_img->HasRawColorContext()) {
    ctx = MetadataExtractor::ReadRawColorContextForRender(*source_img);
  } else {
    MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(*raw_processor, ctx);
    if (source_img) {
      MetadataExtractor::MergeMetadataHint(&source_img->exif_display_, ctx);
    }
  }

  RawParams raw_params;
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
  raw_params.gpu_backend_ = RawGpuBackend::GPU;
#else
  raw_params.gpu_backend_ = RawGpuBackend::CPU;
#endif
  raw_params.highlights_reconstruct_ = false;
  raw_params.use_camera_wb_          = true;
  raw_params.user_wb_                = 0;

  const auto dng_metadata = dng::ExtractMetadata(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size()));
  ctx.dng_warp_rectilinear_present_ = dng_metadata.warp_rectilinear.has_value();
  RawProcessor processor{raw_params, raw_processor->imgdata.rawdata, *raw_processor, ctx,
                         dng_metadata.default_crop.data(), dng_metadata.warp_rectilinear};

  auto         processed = processor.Process();

  raw_processor->recycle();
  source_img->LoadOriginalData(std::move(processed));
}

void RawDecoder::Decode(std::vector<char> buffer, std::shared_ptr<Image> source_img,
                        std::shared_ptr<BufferQueue>              result,
                        std::shared_ptr<std::promise<image_id_t>> promise) {
  throw std::runtime_error("RawDecoder: Not implemented");
}

};  // namespace alcedo
