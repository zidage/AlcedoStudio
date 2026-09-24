//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/cst_op.hpp"

#include <opencv2/core/hal/interface.h>

#include <opencv2/core/types.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "../op_test_fixation.hpp"
#include "concurrency/thread_pool.hpp"
#include "decoders/raw_decoder.hpp"
#include "edit/operators/basic/contrast_op.hpp"
#include "edit/operators/basic/exposure_op.hpp"
#include "edit/operators/basic/tone_region_op.hpp"
#include "edit/operators/color/saturation_op.hpp"
#include "edit/operators/color/tint_op.hpp"
#include "edit/operators/color/vibrance_op.hpp"
#include "edit/operators/detail/clarity_op.hpp"
#include "edit/operators/wheel/color_wheel_op.hpp"
#include "image/image_buffer.hpp"
#include "sleeve/sleeve_manager.hpp"
#include "utils/string/convert.hpp"

using namespace alcedo;
TEST_F(OperationTests, DISABLED_ColorSpaceTransformTest) {
  {
    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 8, 0);
    image_path_t              path = TEST_IMG_PATH "/sample_images/cst/adobe_rgb";
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
    // For now, adjust the thumbnails'RGBsRGB
    auto img = manager.GetPool()->AccessElement(0, AccessType::THUMB).value().lock();

    OCIO_ACES_Transform_Op op{"Gamma 2.4 Encoded Rec.709", "sRGB Encoded Rec.709 (sRGB)",
                              CONFIG_PATH "OCIO/config.ocio"};
    ImageBuffer            to_adjust{img->GetThumbnailData()};
    ImageBuffer            to_display = op.Apply(to_adjust);
    cv::namedWindow("Adobe RGB", cv::WINDOW_KEEPRATIO);
    cv::imshow("Adobe RGB", img->GetThumbnailData());
    cv::Mat display_8u;
    to_display.GetCPUData().convertTo(display_8u, CV_8UC3);
    cv::namedWindow("sRGB", cv::WINDOW_KEEPRATIO);
    cv::imshow("sRGB", display_8u);

    cv::waitKey(0);
  }
}

TEST_F(OperationTests, DISABLED_ACESWorkflowTest1) {
  {
    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 8, 0);
    image_path_t              path = TEST_IMG_PATH "/sample_images/raw/building";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      if (!img.is_directory()) imgs.push_back(img.path());
    }

    manager.LoadToPath(imgs, L"");

    // Read image data
    manager.GetPool()->RecordAccess(0, AccessType::FULL_IMG);
    auto            img = manager.GetPool()->AccessElement(0, AccessType::FULL_IMG).value().lock();
    RawDecoder      decoder;
    std::ifstream   file(img->_image_path, std::ios::binary | std::ios::ate);
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(fileSize);
    if (!file.read(buffer.data(), fileSize)) {
      FAIL();
    }
    file.close();

    decoder.Decode(std::move(buffer), img);

    OCIO_ACES_Transform_Op IDT{"", "ACEScct", CONFIG_PATH "OCIO/config.ocio"};
    ImageBuffer            source{img->GetImageData()};
    ImageBuffer            to_adjust = IDT.Apply(source);
    // OCIO_ACES_Transform_Op     LMT{"ACEScct", OCIO::ROLE_SCENE_LINEAR};
    // ImageBuffer intermediate = LMT.Apply(to_adjust);
    // OCIO_ACES_Transform_Op ODT(OCIO::ROLE_SCENE_LINEAR, "Rec.1886 Rec.709 - Display");
    // ImageBuffer to_display = ODT.Apply(intermediate, "ACES 1.0 - SDR Video");
    // cv::namedWindow("ACES2065-1", cv::WINDOW_KEEPRATIO);
    // cv::imshow("ACES2065-1", img->GetImageData());
    // cv::namedWindow("sRGB", cv::WINDOW_KEEPRATIO);
    // cv::imshow("sRGB", to_display.GetCPUData());

    cv::Mat                to_save_acescct;
    to_adjust.GetCPUData().convertTo(to_save_acescct, CV_16UC3, 65535.0f);
    static constexpr auto save_path = TEST_IMG_PATH "/my_pipeline/batch_results/{}.tif";
    cv::imwrite(std::format(save_path, "acescct"), to_save_acescct);

    cv::Mat to_save_aces2065;
    img->GetImageData().convertTo(to_save_aces2065, CV_16UC3, 65535.0f);
    cv::imwrite(std::format(save_path, "aces2065"), to_save_aces2065);
    cv::waitKey(0);
  }
}

TEST_F(OperationTests, DISABLED_ACESWorkflowTest2) {
  {
    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 8, 0);
    image_path_t              path = TEST_IMG_PATH "/sample_images/raw/camera/nikon/z8";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      if (!img.is_directory()) imgs.push_back(img.path());
    }

    manager.LoadToPath(imgs, L"");

    // Read image data
    manager.GetPool()->RecordAccess(0, AccessType::FULL_IMG);
    auto            img = manager.GetPool()->AccessElement(0, AccessType::FULL_IMG).value().lock();
    RawDecoder      decoder;
    std::ifstream   file(img->_image_path, std::ios::binary | std::ios::ate);
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(fileSize);
    if (!file.read(buffer.data(), fileSize)) {
      FAIL();
    }
    file.close();

    decoder.Decode(std::move(buffer), img);


    ImageBuffer source{img->GetImageData()};

    OCIO_ACES_Transform_Op IDT{"ACES - ACES2065-1", "ACEScct", CONFIG_PATH "OCIO/config.ocio"};
    ImageBuffer            to_adjust = IDT.Apply(source);



    // ExposureOp EV{0.2f};
    // to_adjust = EV.Apply(to_adjust);

    // TintOp TTOP{+3.0f};
    // to_adjust = TTOP.Apply(to_adjust);
    // ToneRegionOp WTOP{10.0f, ToneRegion::WHITE};
    // to_adjust = WTOP.Apply(to_adjust);

    // ToneRegionOp HLOP{-50.0f, ToneRegion::HIGHLIGHTS};
    // to_adjust = HLOP.Apply(to_adjust);

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
                                         {"luminance_offset", 0.2}}},
                                       {"gain",
                                        {{"color_offset.x", 1.00},
                                         {"color_offset.y", 1.0},
                                         {"color_offset.z", 1.0},
                                         {"luminance_offset", -0.25}}},
                                       {"crossovers", {{"lift", 0.3}, {"gain", 0.7}}}};
    nlohmann::json params;
    params[color_wheels.script_name_] = teal_orange_look;
    color_wheels.SetParams(params);
    to_adjust = color_wheels.Apply(to_adjust);
    // ToneRegionOp SDOP{10.0f, ToneRegion::SHADOWS};
    // to_adjust = SDOP.Apply(to_adjust);

    // // ToneRegionOp BLOP{5.0f, ToneRegion::BLACK};
    // // to_adjust = BLOP.Apply(to_adjust);

    // // SaturationOp VIB{30.0f};
    // // to_adjust = VIB.Apply(to_adjust);

    // // ClarityOp CLRT{20.0f};
    // // to_adjust = CLRT.Apply(to_adjust);

    std::filesystem::path  look_path{CONFIG_PATH "LUTs/ACES CCT 2383 D65.cube"};
    OCIO_ACES_Transform_Op LMT{look_path};
    to_adjust = LMT.ApplyLMT(to_adjust);


    OCIO_ACES_Transform_Op pre_RRT{"ACEScct", ""};
    ImageBuffer            intermediate = pre_RRT.Apply(to_adjust);

    OCIO_ACES_Transform_Op ODT("", "Camera Rec.709");
    ImageBuffer            to_display = ODT.Apply(intermediate);

    cv::Mat to_save_rec709;
    to_display.GetCPUData().convertTo(to_save_rec709, CV_16UC3, 65535.0f);

    cv::cvtColor(to_save_rec709, to_save_rec709, cv::COLOR_RGB2BGR);
    static constexpr auto save_path = TEST_IMG_PATH "/my_pipeline/batch_results/{}.tif";
    cv::imwrite(std::format(save_path, conv::ToBytes(img->_image_name)), to_save_rec709);

  }

}

TEST_F(OperationTests, BatchProcessTest) {
  {

    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 8, 0);
    image_path_t              path = std::string(TEST_IMG_PATH) + std::string("/real_tests");
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      if (!img.is_directory()) imgs.push_back(img.path());
    }

    manager.LoadToPath(imgs, L"");

    // Read image data
    ThreadPool thread_pool{8};
    auto       img_pool = manager.GetPool()->GetPool();
    for (auto& pair : img_pool) {
      auto task = [pair, img_pool]() {
        auto            img = pair.second;
        RawDecoder      decoder;
        std::ifstream   file(img->_image_path, std::ios::binary | std::ios::ate);
        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> buffer(fileSize);
        if (!file.read(buffer.data(), fileSize)) {
          FAIL();
        }
        file.close();

        decoder.Decode(std::move(buffer), img);

        ImageBuffer            source{img->GetImageData()};
        OCIO_ACES_Transform_Op IDT{"", "ACEScct"};
        ImageBuffer            to_adjust = IDT.Apply(source);

        ContrastOp             CONT{35.0f};

        ClarityOp              CRT{25.0f};
        to_adjust = CRT.Apply(to_adjust);


        std::filesystem::path  look_path{CONFIG_PATH "LUTs/ACES CCT 2383 D65.cube"};
        OCIO_ACES_Transform_Op LMT{look_path};
        to_adjust = LMT.ApplyLMT(to_adjust);
        OCIO_ACES_Transform_Op pre_RRT{"ACEScct", ""};
        ImageBuffer            intermediate = pre_RRT.Apply(to_adjust);
        OCIO_ACES_Transform_Op ODT("", "Camera Rec.709");
        ImageBuffer            to_display = ODT.Apply(intermediate);

        cv::Mat                to_save_rec709;
        to_display.GetCPUData().convertTo(to_save_rec709, CV_16UC3, 65535.0f);
        cv::cvtColor(to_save_rec709, to_save_rec709, cv::COLOR_RGB2BGR);
        static constexpr auto save_path = TEST_IMG_PATH "/my_pipeline/batch_results/{}.tif";
        cv::imwrite(std::format(save_path, conv::ToBytes(img->_image_name)), to_save_rec709);

        img->ClearData();
        to_save_rec709.release();
        to_adjust.ReleaseCPUData();
      };
      thread_pool.Submit(task);
    }
  }
}
