//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include <libraw/libraw.h>
#include <libraw/libraw_types.h>

#include <filesystem>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "type/type.hpp"

TEST(SingleRawLoad, BasicAssertions) {
  // std::cout << std::filesystem::current_path() << std::endl;
  // Set the path of the test image, which has to be an absolute path, didn't know why...
  image_path_t test_img = std::string(TEST_IMG_PATH) + std::string("/raw/dng_100.dng");
  LibRaw raw_processor;

  // Try to open the file
  ASSERT_EQ(raw_processor.open_file(test_img.c_str()), LIBRAW_SUCCESS);

  // Try to unpack the image -> to metadata and
  ASSERT_EQ(raw_processor.unpack(), LIBRAW_SUCCESS);

  ASSERT_EQ(raw_processor.dcraw_process(), LIBRAW_SUCCESS);

  libraw_processed_image_t *img = raw_processor.dcraw_make_mem_image();
  if (!img) {
    std::cerr << "Error generating RGB image!" << std::endl;
    ASSERT_TRUE(false);
  }

  // cv::Mat image(img->height, img->width, CV_8UC3, img->data);
  // cv::Size newSize(image.cols / 8, image.rows / 8);
  // cv::Mat imageRGB;
  // cv::cvtColor(image, imageRGB, cv::COLOR_BGR2RGB);
  // cv::Mat resizedImage;
  // cv::resize(imageRGB, resizedImage, newSize, 0, 0, cv::INTER_LINEAR);
  
  // cv::imshow("RAW Image", resizedImage);
  // cv::waitKey(0);


  LibRaw::dcraw_clear_mem(img);
  raw_processor.recycle();
}
