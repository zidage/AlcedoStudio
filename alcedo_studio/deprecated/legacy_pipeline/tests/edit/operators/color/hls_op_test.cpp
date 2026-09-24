//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/color/HLS_op.hpp"

#include "../op_test_fixation.hpp"
#include "sleeve/sleeve_manager.hpp"


using namespace alcedo;
TEST_F(OperationTests, HLSAdjustmentTest) {
  {
    SleeveManager manager{db_path_};
    ImageLoader   image_loader(128, 8, 0);
    image_path_t  path =
        TEST_IMG_PATH "/sample_images/jpg";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      imgs.push_back(img.path());
    }

    auto display_callback = [](size_t, std::weak_ptr<Image>) {};

    manager.LoadToPath(imgs, L"");
    auto view = manager.GetView();
    view->UpdateView(L"");
    // Load thumbnail for these images, do nothing with the callback
    view->LoadPreview(0, 20, display_callback);
    // For now, adjust the thumbnail only
    auto img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();

    cv::namedWindow("HLS Animation", cv::WINDOW_AUTOSIZE);
    HLSOp       op{};
    auto        param = op.GetParams();
    ImageBuffer to_adjust{img->GetThumbnailData()};

    // Pick a color
    cv::Vec3f   sky_blue_bgr(210.0f / 255.0f, 150.0f / 255.0f, 30.0f / 255.0f);

    op.SetRanges(20.0f, 0.4f, 0.4f);
    op.SetTargetColor(sky_blue_bgr);
    // Delay between two adjustments
    int delay = 10;
    // Hue adjustment
    for (float value = -100.0f; value <= 100.0f; value += 1.0f) {
      op.SetAdjustment(cv::Vec3f(value / 100.0f, 0.0f, 0.0f));

      ImageBuffer to_display = op.Apply(to_adjust);
      cv::imshow("H Animation", to_display.GetCPUData());
      to_adjust = {img->GetThumbnailData()};

      if (cv::waitKey(delay) == 27) break;  // Press ESC to exit
    }
    cv::waitKey(10);
    // Lightness adjustment
    for (float value = -100.0f; value <= 100.0f; value += 1.0f) {
      op.SetAdjustment(cv::Vec3f(0.0f, value / 1000.0f, 0.0f));

      ImageBuffer to_display = op.Apply(to_adjust);
      cv::imshow("L Animation", to_display.GetCPUData());
      to_adjust = {img->GetThumbnailData()};

      if (cv::waitKey(delay) == 27) break;  // Press ESC to exit
    }
    cv::waitKey(10);
    // Saturation adjustment
    for (float value = -100.0f; value <= 100.0f; value += 1.0f) {
      op.SetAdjustment(cv::Vec3f(0.0f, 0.0f, value / 100.0f));

      ImageBuffer to_display = op.Apply(to_adjust);
      cv::imshow("S Animation", to_display.GetCPUData());
      to_adjust = {img->GetThumbnailData()};

      if (cv::waitKey(delay) == 27) break;  // Press ESC to exit
    }
    op.SetAdjustment(cv::Vec3f(0.0f, 0.0f, 5.0f));
    // Hue range adjustment
    for (float value = 0.0f; value <= 100.0f; value += 1.0f) {
      op.SetRanges(value / 100.0f, 0.4f, 0.4f);

      ImageBuffer to_display = op.Apply(to_adjust);
      cv::imshow("Hue range Animation", to_display.GetCPUData());
      to_adjust = {img->GetThumbnailData()};

      if (cv::waitKey(delay) == 27) break;  // Press ESC to exit
    }
    // Lightness range adjustment
    for (float value = 0.0f; value <= 100.0f; value += 1.0f) {
      op.SetRanges(20.0f, value / 100.0f, 0.4f);

      ImageBuffer to_display = op.Apply(to_adjust);
      cv::imshow("Lightness range Animation", to_display.GetCPUData());
      to_adjust = {img->GetThumbnailData()};

      if (cv::waitKey(delay) == 27) break;  // Press ESC to exit
    }
    // Saturation range adjustment
    for (float value = 0.0f; value <= 100.0f; value += 1.0f) {
      op.SetRanges(20.0f, 0.0f, value / 100.0f);

      ImageBuffer to_display = op.Apply(to_adjust);
      cv::imshow("Saturation range Animation", to_display.GetCPUData());
      to_adjust = {img->GetThumbnailData()};

      if (cv::waitKey(delay) == 27) break;  // Press ESC to exit
    }
    cv::waitKey(1);
  }
}