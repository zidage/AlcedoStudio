//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <string>

#include "decoders/processor/raw_color_context.hpp"
#include "image/metadata.hpp"

/// LibRaw fields of an opened RAW file to RawRuntimeColorContext and ExifDisplayMetaData.
namespace alcedo::metadata_detail {

/// EXIF date text of a RAW file, read while LibRaw parses the TIFF and EXIF IFDs of the file.
///
/// The text is the camera clock time and has no time zone. LibRaw also converts it to
/// `imgdata.other.timestamp` with `mktime()`, in the time zone of this machine. That conversion
/// cannot be reversed in all cases: a camera clock time in the hour skipped when daylight saving
/// time starts has no `time_t`, and `mktime()` moves it by one hour (issue #381). Reading the text
/// gives the camera clock time in any time zone.
struct LibRawCaptureTimeText {
  std::string date_time_original_;   ///< EXIF 0x9003 DateTimeOriginal, `YYYY-MM-DD HH:MM:SS`.
  std::string date_time_digitized_;  ///< EXIF 0x9004 DateTimeDigitized, `YYYY-MM-DD HH:MM:SS`.
  std::string date_time_;            ///< TIFF 0x0132 DateTime, `YYYY-MM-DD HH:MM:SS`.
};

/// LibRaw `exif_cb` handler: stores the date tags of the file in a `LibRawCaptureTimeText`.
/// LibRaw calls it for each tag with `ifp` at the tag data and restores the read position after.
void CollectLibRawCaptureTimeText(void* context, int tag, int type, int len, unsigned int ord,
                                  void* ifp, INT64 base);

/// Populate a RawRuntimeColorContext from LibRaw's opened state. open_file sets every field read
/// here: imgdata.color (cam_mul, pre_mul, cam_xyz, rgb_cam), imgdata.idata, imgdata.other and
/// imgdata.lens. unpack() only copies imgdata.color into imgdata.rawdata.color after it decodes
/// the sensor data, so the metadata path never needs it.
void PopulateMetadataRuntimeContext(LibRaw& raw_processor, RawRuntimeColorContext& ctx);

/// Populate ExifDisplayMetaData from a RawRuntimeColorContext + libraw other/sizes data.
void PopulateDisplayMetadataFromLibRaw(LibRaw& raw_processor, const RawRuntimeColorContext& ctx,
                                       const LibRawCaptureTimeText& capture_time_text,
                                       ExifDisplayMetaData&         display);

/// DNG fast path: replace the Exiv2 values of @p display and @p ctx with the camera, lens and
/// exposure values that LibRaw open_file read, where LibRaw has a value.
void PopulateDngMetadataHintFromOpenLibRaw(LibRaw& raw_processor, ExifDisplayMetaData& display,
                                           RawRuntimeColorContext& ctx);

/// Fill the fields of @p ctx that are still empty or zero from @p metadata_hint. No-op when
/// @p metadata_hint is null.
void MergeMetadataHint(const ExifDisplayMetaData* metadata_hint, RawRuntimeColorContext& ctx);

}  // namespace alcedo::metadata_detail
