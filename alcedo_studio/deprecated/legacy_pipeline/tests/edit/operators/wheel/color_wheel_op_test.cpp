//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/wheel/color_wheel_op.hpp"

#include <algorithm>
#include <opencv2/core.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "../op_test_fixation.hpp"
#include "image/image_buffer.hpp"
#include "sleeve/sleeve_manager.hpp"

using namespace alcedo;
TEST_F(OperationTests, GrayGradientAdjustmentTest) {
  {
    SleeveManager manager{db_path_};
    ImageLoader   image_loader(128, 8, 0);
    image_path_t  path =
        TEST_IMG_PATH "/sample_images/original_jpg";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      imgs.push_back(img.path());
    }

    auto display_callback = [](size_t, std::weak_ptr<Image>) {};

    manager.LoadToPath(imgs, L"");
    auto view = manager.GetView();
    view->UpdateView(L"");
    // Load thumbnail for these images, do nothing with the callback
    view->LoadPreview(0, 10, display_callback);
    // For now, adjust the thumbnail only
    auto           img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();

    ColorWheelOp   color_wheels;
    // A teal and orange look
    nlohmann::json teal_orange_look = {{"lift",
                                        {{"color_offset.x", 0.0},
                                         {"color_offset.y", 0.0},
                                         {"color_offset.z", 0.0},
                                         {"luminance_offset", 0.0}}},
                                       {"gamma",
                                        {{"color_offset.x", 1.0},
                                         {"color_offset.y", 1.0},
                                         {"color_offset.z", 1.00},
                                         {"luminance_offset", -0.0}}},
                                       {"gain",
                                        {{"color_offset.x", 1.0},
                                         {"color_offset.y", 1.0},
                                         {"color_offset.z", 1.0},
                                         {"luminance_offset", -1.0}}},
                                       {"crossovers", {{"lift", 0.2}, {"gain", 0.8}}}};
    nlohmann::json params;
    params[color_wheels.script_name_] = teal_orange_look;
    color_wheels.SetParams(params);

    ImageBuffer orignal{img->GetThumbnailData().clone()};
    cv::Mat     gradient_1d(1, 256, CV_32FC1);
    for (int i = 0; i < 256; ++i) gradient_1d.at<float>(0, i) = i / 256.0f;

    cv::Mat gradient_2d;
    cv::resize(gradient_1d, gradient_2d, cv::Size(256, 256), 0, 0, cv::INTER_LINEAR);
    cv::Mat gradient_2d_3c(256, 256, CV_32FC3);
    cv::cvtColor(gradient_2d, gradient_2d_3c, cv::COLOR_GRAY2BGR);

    cv::Mat     waveform_input(256, 256, CV_32FC1, cv::Scalar(0.0f));
    cv::Mat     waveform_output(256, 256, CV_8UC3, cv::Scalar(0, 0, 0));

    ImageBuffer input{gradient_2d_3c};
    ImageBuffer result = color_wheels.Apply(input);

    gradient_2d.forEach<float>([&](float& pixel, const int* pos) {
      waveform_input.at<float>(static_cast<int>((1.0f - pixel) * 255.0f), pos[1]) = 1.0f;
    });

    result.GetCPUData().forEach<cv::Vec3f>([&](cv::Vec3f& pixel, const int* pos) {
      if (pixel[0] > 1.0f) {
        pixel[0] = 1.0f;
      }
      if (pixel[1] > 1.0f) {
        pixel[1] = 1.0f;
      }
      if (pixel[2] > 1.0f) {
        pixel[2] = 1.0f;
      }
      int x = pos[1];
      for (int c = 0; c < 3; ++c) {
        float value       = pixel[c];
        int   y           = 255 - cvRound(value * 255.0f);
        y                 = std::clamp(y, 0, 255);

        cv::Vec3b& target = waveform_output.at<cv::Vec3b>(y, x);
        target[c]         = 255;
      }
    });

    cv::imshow("Original", gradient_2d);
    cv::imshow("OriginalWave", waveform_input);
    cv::imshow("After", result.GetCPUData());
    cv::imshow("AfterWave", waveform_output);
    cv::waitKey(0);
  }
  cv::waitKey(1000);
}

void GetWaveform(cv::Mat& input, cv::Mat& wave) {
  cv::Mat input_resized;
  cv::resize(input, input_resized, cv::Size(1024, 512));
  input_resized.forEach<cv::Vec3f>([&](cv::Vec3f& pixel, const int* pos) {
    if (pixel[0] > 1.0f) {
      pixel[0] = 1.0f;
    }
    if (pixel[1] > 1.0f) {
      pixel[1] = 1.0f;
    }
    if (pixel[2] > 1.0f) {
      pixel[2] = 1.0f;
    }
    int x = pos[1];
    for (int c = 0; c < 3; ++c) {
      float value       = pixel[c];
      int   y           = 511 - cvRound(value * 511.0f);
      y                 = std::clamp(y, 0, 511);
      cv::Vec3b& target = wave.at<cv::Vec3b>(y, x);
      target[c]         = 255;
    }
  });
}

TEST_F(OperationTests, DISABLED_ColorWheelAdjustmentTest) {
  {
    SleeveManager manager{db_path_};
    ImageLoader   image_loader(128, 8, 0);
    image_path_t  path =
        TEST_IMG_PATH "/sample_images/original_jpg";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      imgs.push_back(img.path());
    }

    auto display_callback = [](size_t idx, std::weak_ptr<Image> img) {};

    manager.LoadToPath(imgs, L"");
    auto view = manager.GetView();
    view->UpdateView(L"");
    // Load thumbnail for these images, do nothing with the callback
    view->LoadPreview(0, 10, display_callback);
    // For now, adjust the thumbnail only
    auto           img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();

    ColorWheelOp   color_wheels;
    // A teal and orange look
    nlohmann::json teal_orange_look = {{"lift",
                                        {{"color_offset.x", 0.0},
                                         {"color_offset.y", 0.0},
                                         {"color_offset.z", 0.0},
                                         {"luminance_offset", 0.3}}},
                                       {"gamma",
                                        {{"color_offset.x", 1.0},
                                         {"color_offset.y", 1.0},
                                         {"color_offset.z", 1.00},
                                         {"luminance_offset", -0.0}}},
                                       {"gain",
                                        {{"color_offset.x", 1.0},
                                         {"color_offset.y", 1.0},
                                         {"color_offset.z", 1.0},
                                         {"luminance_offset", 0.0}}},
                                       {"crossovers", {{"lift", 0.2}, {"gain", 0.8}}}};
    nlohmann::json params;
    params[color_wheels.script_name_] = teal_orange_look;
    color_wheels.SetParams(params);

    ImageBuffer original{img->GetThumbnailData().clone()};

    ImageBuffer input{original.GetCPUData()};
    ImageBuffer result = color_wheels.Apply(original);

    cv::Mat     waveform_origin(512, 1024, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat     waveform_result(512, 1024, CV_8UC3, cv::Scalar(0, 0, 0));

    GetWaveform(img->GetThumbnailData(), waveform_origin);
    GetWaveform(result.GetCPUData(), waveform_result);

    cv::imshow("Original", img->GetThumbnailData());
    cv::imshow("OriginalWave", waveform_origin);
    cv::imshow("After", result.GetCPUData());
    cv::imshow("AfterWave", waveform_result);
    cv::waitKey(0);
  }
}

TEST_F(OperationTests, ColorWheelAnimationTest) {
  {
    SleeveManager manager{db_path_};
    ImageLoader   image_loader(128, 8, 0);
    image_path_t  path =
        TEST_IMG_PATH "/sample_images/original_jpg";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      imgs.push_back(img.path());
    }

    auto display_callback = [](size_t, std::weak_ptr<Image>) {};

    manager.LoadToPath(imgs, L"");
    auto view = manager.GetView();
    view->UpdateView(L"");
    // Load thumbnail for these images, do nothing with the callback
    view->LoadPreview(0, 10, display_callback);
    // For now, adjust the thumbnail only
    auto           img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();

    ColorWheelOp   color_wheels;
    // A teal and orange look
    nlohmann::json look = {{"lift",
                            {{"color_offset.x", 0.0},
                             {"color_offset.y", 0.0},
                             {"color_offset.z", 0.0},
                             {"luminance_offset", 0.0}}},
                           {"gamma",
                            {{"color_offset.x", 1.0},
                             {"color_offset.y", 1.0},
                             {"color_offset.z", 1.00},
                             {"luminance_offset", 0.0}}},
                           {"gain",
                            {{"color_offset.x", 1.0},
                             {"color_offset.y", 1.0},
                             {"color_offset.z", 1.0},
                             {"luminance_offset", 0.0}}},
                           {"crossovers", {{"lift", 0.2}, {"gain", 0.8}}}};
    nlohmann::json params;
    params[color_wheels.script_name_] = look;
    color_wheels.SetParams(params);

    for (float i = -1.0f; i <= 1.0f; i += 0.01f) {
      params[color_wheels.script_name_]["gain"]["luminance_offset"] = i;
      color_wheels.SetParams(params);
      ImageBuffer original{img->GetThumbnailData().clone()};

      ImageBuffer input{original.GetCPUData()};
      ImageBuffer result = color_wheels.Apply(input);

      cv::Mat     waveform_result(512, 1024, CV_8UC3, cv::Scalar(0, 0, 0));

      GetWaveform(result.GetCPUData(), waveform_result);
      cv::imshow("After", result.GetCPUData());
      cv::imshow("AfterWave", waveform_result);
      if (cv::waitKey(10) == 27) break;
    }
    cv::waitKey(0);
  }
}
