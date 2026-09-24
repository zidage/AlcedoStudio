//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <opencv2/highgui.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "edit/operators/basic/black_op.hpp"
#include "edit/operators/basic/exposure_op.hpp"
#include "edit/operators/basic/white_op.hpp"
#include "edit/operators/color/tint_op.hpp"
#include "edit/operators/cst/cst_op.hpp"
#include "edit/operators/cst/lmt_op.hpp"
#include "edit/operators/op_kernel.hpp"
#include "edit/pipeline/tile_scheduler.hpp"
#include "io/image/image_loader.hpp"
#include "pipeline_test_fixation.hpp"
#include "sleeve/sleeve_manager.hpp"
#include "type/supported_file_type.hpp"

namespace alcedo {
// TEST_F(PipelineTests, DISABLED_TileSchedulerBasic1) {
//   nlohmann::json params;
//   params["exposure"] = 1.0f;
//   ExposureOp   exp_op{params};

//   KernelStream stream;
//   stream.AddToStream(exp_op.ToKernel());

//   cv::Mat bgr8 =
//       cv::imread(std::string(TEST_IMG_PATH) + "/jpeg/tile_tests/test_img.jpg", cv::IMREAD_COLOR);
//   ASSERT_FALSE(bgr8.empty()) << "Failed to read image";
//   cv::resize(bgr8, bgr8, cv::Size(1200, 800));

//   cv::Mat img;
//   bgr8.convertTo(img, CV_32FC3, 1.0 / 255.0);
//   cv::cvtColor(img, img, cv::COLOR_BGR2RGB);

//   auto          img_buffer = std::make_shared<ImageBuffer>(std::move(img));

//   TileScheduler scheduler{img_buffer, stream};

//   using clock                                        = std::chrono::high_resolution_clock;
//   auto                                      start    = clock::now();
//   auto                                      output   = scheduler.ApplyOps();
//   auto                                      end      = clock::now();
//   std::chrono::duration<double, std::milli> duration = end - start;
//   std::cout << "TileScheduler processing time: " << duration.count() << " ms" << std::endl;

//   cv::imshow("TileScheduler Output", output->GetCPUData());
//   cv::waitKey(0);
// }

// TEST_F(PipelineTests, TileSchedulerBasic2) {
//   nlohmann::json params;
//   params["exposure"] = 1.0f;
//   params["black"]    = 10.0f;
//   params["white"]    = -15.0f;
//   params["tint"]     = -15.0f;
//   nlohmann::json lmt_params;
//     lmt_params["ocio_lmt"] = CONFIG_PATH "LUTs/ACES CCT 2383 D65.cube";
  
//   ExposureOp   exp_op{params};
//   BlackOp      black_op{params};
//   WhiteOp      white_op{params};
//   TintOp       tint_op{params};

//   OCIO_LMT_Transform_Op lmt_op{lmt_params};

//   nlohmann::json        input_cst_params;
//   input_cst_params["ocio"] = {{"src", "sRGB Encoded Rec.709 (sRGB)"}, {"dst", "ACEScct"}};
//   OCIO_ACES_Transform_Op input_cst_op{input_cst_params};
  

//   nlohmann::json output_cst_params;
//   output_cst_params["ocio"] = {{"src", "ACEScct"}, {"dst", "Camera Rec.709"}, {"limit", true}};
//   OCIO_ACES_Transform_Op output_cst_op{output_cst_params};

//   KernelStream stream;
//   stream.AddToStream(input_cst_op.ToKernel());
//   stream.AddToStream(exp_op.ToKernel());
//   stream.AddToStream(black_op.ToKernel());
//   stream.AddToStream(white_op.ToKernel());
//   stream.AddToStream(tint_op.ToKernel());
//   stream.AddToStream(lmt_op.ToKernel());
//   stream.AddToStream(output_cst_op.ToKernel());

//   cv::Mat bgr8 =
//       cv::imread(std::string(TEST_IMG_PATH) + "/jpeg/tile_tests/test_img.jpg", cv::IMREAD_COLOR);
//   ASSERT_FALSE(bgr8.empty()) << "Failed to read image";
//   cv::resize(bgr8, bgr8, cv::Size(768, 1024));

//   cv::Mat img;
//   bgr8.convertTo(img, CV_32FC3, 1.0 / 255.0);
//   cv::cvtColor(img, img, cv::COLOR_BGR2RGB);

//   auto          img_buffer = std::make_shared<ImageBuffer>(std::move(img));

//   TileScheduler scheduler{img_buffer, stream, 24};

//   using clock                                        = std::chrono::high_resolution_clock;
//   auto                                      start    = clock::now();
//   auto                                      output   = scheduler.ApplyOps();
//   auto                                      end      = clock::now();
//   std::chrono::duration<double, std::milli> duration = end - start;
//   std::cout << "TileScheduler processing time: " << duration.count() << " ms" << std::endl;

//   cv::cvtColor(output->GetCPUData(), output->GetCPUData(), cv::COLOR_RGB2BGR);

//   cv::namedWindow("TileScheduler Output", cv::WINDOW_NORMAL);
//   cv::resizeWindow("TileScheduler Output", 450, 600);
//   cv::imshow("TileScheduler Output", output->GetCPUData());
//   cv::waitKey(0);
// }
}  // namespace alcedo
