//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/curve/curve_op.hpp"

#include "../op_test_fixation.hpp"
#include "sleeve/sleeve_manager.hpp"

using namespace alcedo;
static void GetWaveform(cv::Mat& input, cv::Mat& wave) {
  cv::Mat input_resized;
  cv::resize(input, input_resized, cv::Size(256, 256));
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
      int   y           = 255 - cvRound(value * 255.0f);
      y                 = std::clamp(y, 0, 255);
      cv::Vec3b& target = wave.at<cv::Vec3b>(y, x);
      target[c]         = 255;
    }
  });
}

TEST_F(OperationTests, DISABLED_CurveGrayGradientAdjustmentTest) {
  {
    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 8, 0);
    image_path_t              path = TEST_IMG_PATH "/sample_images/jpg";
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
    auto img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();
    std::vector<cv::Point2f> curve_points = {
        {0.0f, 0.0f}, {0.25f, 0.2f}, {0.75f, 0.8f}, {1.0f, 1.0f}};
    CurveOp     color_wheels{curve_points};
    // A teal and orange look

    ImageBuffer orignal{img->GetThumbnailData().clone()};
    cv::Mat     gradient_1d(1, 256, CV_32FC1);
    for (int i = 0; i < 256; ++i) gradient_1d.at<float>(0, i) = i / 256.0f;

    cv::Mat gradient_2d;
    cv::resize(gradient_1d, gradient_2d, cv::Size(256, 256), 0, 0, cv::INTER_LINEAR);
    cv::Mat gradient_2d_3c(256, 256, CV_32FC3);
    cv::cvtColor(gradient_2d, gradient_2d_3c, cv::COLOR_GRAY2BGR);

    cv::Mat     waveform_input(256, 256, CV_8UC3, cv::Scalar(0.0f));
    cv::Mat     waveform_output(256, 256, CV_8UC3, cv::Scalar(0, 0, 0));

    ImageBuffer input{gradient_2d_3c};
    ImageBuffer result = color_wheels.Apply(input);

    GetWaveform(gradient_2d_3c, waveform_input);
    GetWaveform(result.GetCPUData(), waveform_output);

    cv::imshow("Original", gradient_2d);
    cv::imshow("OriginalWave", waveform_input);
    cv::imshow("After", result.GetCPUData());
    cv::imshow("AfterWave", waveform_output);
    cv::waitKey(0);
  }
}

TEST_F(OperationTests, CurveAnimationAdjustmentTest1) {
  {
    SleeveManager manager{db_path_};
    ImageLoader   image_loader(128, 8, 0);
    image_path_t  path = TEST_IMG_PATH "/sample_images/original_jpg";
    L"lab\\tests\\resources\\sample_images\\original_jpg";
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
    auto img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();
    std::vector<cv::Point2f> curve_points = {
        {0.0f, 0.0f}, {0.25f, 0.2f}, {0.75f, 0.8f}, {1.0f, 1.0f}};
    CurveOp     curve{curve_points};
    // A teal and orange look

    ImageBuffer orignal{img->GetThumbnailData().clone()};
    cv::Mat     gradient_1d(1, 256, CV_32FC1);
    for (int i = 0; i < 256; ++i) gradient_1d.at<float>(0, i) = i / 256.0f;

    cv::Mat gradient_2d;
    cv::resize(gradient_1d, gradient_2d, cv::Size(256, 256), 0, 0, cv::INTER_LINEAR);
    cv::Mat gradient_2d_3c(256, 256, CV_32FC3);
    cv::cvtColor(gradient_2d, gradient_2d_3c, cv::COLOR_GRAY2BGR);

    for (float i = 0.0f; i <= 1.0f; i += 0.001f) {
      curve_points[3].y = i;

      curve.SetCtrlPts(curve_points);

      cv::Mat     waveform_input(256, 256, CV_8UC3, cv::Scalar(0.0f));
      cv::Mat     waveform_output(256, 256, CV_8UC3, cv::Scalar(0, 0, 0));

      ImageBuffer input{gradient_2d_3c};

      ImageBuffer result = curve.Apply(input);

      GetWaveform(result.GetCPUData(), waveform_output);

      cv::imshow("After", result.GetCPUData());
      cv::imshow("AfterWave", waveform_output);
      cv::waitKey(10);
    }
    cv::waitKey(0);
  }
}

TEST_F(OperationTests, DISABLED_CurveAnimationAdjustmentTest2) {
  {
    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 8, 0);
    image_path_t              path = TEST_IMG_PATH "/sample_images/jpg";
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
    auto        img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();

    ImageBuffer original{img->GetThumbnailData()};
    cv::Mat&    original_data = original.GetCPUData();
    // Resize the image
    int         maxWidth      = 2048;
    cv::Mat     resized_img;
    float       scale     = static_cast<float>(maxWidth) / original_data.cols;
    int         newWidth  = maxWidth;
    int         newHeight = static_cast<int>(original_data.rows * scale);
    cv::resize(original_data, resized_img, cv::Size(newWidth, newHeight), 0, 0, cv::INTER_AREA);

    std::vector<cv::Point2f> curve_points = {
        {0.0f, 0.0f}, {0.25f, 0.2f}, {0.75f, 0.8f}, {1.0f, 1.0f}};
    CurveOp curve{curve_points};
    // A teal and orange look

    for (float i = 0.0f; i <= 1.0f; i += 0.1) {
      curve_points[3].y = i;
      curve.SetCtrlPts(curve_points);

      cv::Mat waveform_output(256, 256, CV_8UC3, cv::Scalar(0, 0, 0));

      ImageBuffer input{resized_img};


      ImageBuffer result = curve.Apply(input);


      GetWaveform(result.GetCPUData(), waveform_output);


      cv::imshow("After", result.GetCPUData());
      cv::imshow("AfterWave", waveform_output);

      cv::waitKey(1);
    }
    cv::waitKey(1);
  }

}