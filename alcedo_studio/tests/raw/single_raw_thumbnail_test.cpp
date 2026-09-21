//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include <libraw/libraw.h>
#include <libraw/libraw_types.h>

#include <vector>
#include <filesystem>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "type/type.hpp"

TEST(SingleThumbnailLoad, BasicAssertions) {
  // std::cout << std::filesystem::current_path() << std::endl;
  // Set the path of the test image, which has to be an absolute path, didn't know why...
  image_path_t test_img = std::string(TEST_IMG_PATH) + std::string("/raw/dng_100.dng");
  LibRaw raw_processor;

  // Try to open the file
  ASSERT_EQ(raw_processor.open_file(test_img.c_str()), LIBRAW_SUCCESS);

  // Try to unpack the image -> to metadata and
  ASSERT_EQ(raw_processor.unpack(), LIBRAW_SUCCESS);

  ASSERT_EQ(raw_processor.unpack_thumb(), LIBRAW_SUCCESS);

  libraw_processed_image_t *thumbnail = raw_processor.dcraw_make_mem_thumb();
  if (!thumbnail) {
    std::cerr << "Error generating RGB image!" << std::endl;
    ASSERT_TRUE(false);
  }

  // Decide the format of the thumbnail
  cv::Mat img;
  if (thumbnail->type == LIBRAW_IMAGE_JPEG) {
      // For JPEG format, decode directly
      std::vector<uchar> jpegData(thumbnail->data, thumbnail->data + thumbnail->data_size);
      img = cv::imdecode(jpegData, cv::IMREAD_COLOR);
  } else if (thumbnail->type == LIBRAW_IMAGE_BITMAP) {
      // For Bmp file, manually decode
      img = cv::Mat(thumbnail->height, thumbnail->width, CV_8UC3, thumbnail->data);
      cv::cvtColor(img, img, cv::COLOR_RGB2BGR);  // 转换颜色通道
  } else {
      std::cerr << "Unsupported thumbnail format." << std::endl;
      LibRaw::dcraw_clear_mem(thumbnail);
      return;
  }

  // if (!img.empty()) {
  //   imshow("RAW Thumbnail", img);
  //   cv::waitKey(0);
  // }


  LibRaw::dcraw_clear_mem(thumbnail);
  raw_processor.recycle();
}
