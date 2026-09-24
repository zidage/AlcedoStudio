//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/color/tint_op.hpp"
#include "../op_test_fixation.hpp"

#include "sleeve/sleeve_manager.hpp"

using namespace alcedo;
TEST_F(OperationTests, TintAdjustmentTest) {
  {
    SleeveManager manager{db_path_};
    ImageLoader   image_loader(128, 8, 0);
    image_path_t  path =
        TEST_IMG_PATH "/sample_images/jpg";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      imgs.push_back(img.path());
    }

    auto display_callback = [](size_t idx, std::weak_ptr<Image> img) {
    };

    manager.LoadToPath(imgs, L"");
    auto view = manager.GetView();
    view->UpdateView(L"");
    // Load thumbnail for these images, do nothing with the callback
    view->LoadPreview(0, 20, display_callback);
    // For now, adjust the thumbnail only
    auto img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();

    cv::namedWindow("Tint Animation", cv::WINDOW_AUTOSIZE);
    TintOp op{0.0f};
    auto       param = op.GetParams();
    ImageBuffer to_adjust{img->GetThumbnailData()};
    // Delay between two adjustments
    int delay = 10;
    for (float value = -100.0f; value <= 100.0f; value += 1.0f) {
      param[op.GetScriptName()] = value;
      op.SetParams(param);

      ImageBuffer to_display = op.Apply(to_adjust);
      cv::imshow("Tint Animation", to_display.GetCPUData());
      to_adjust = {img->GetThumbnailData()};

      if (cv::waitKey(delay) == 27) break; // Press ESC to exit
    }

    cv::waitKey(1);
  }
}