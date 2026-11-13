//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <opencv2/core/mat.hpp>

#ifdef HAVE_CUDA
#include <opencv2/cudaimgproc.hpp>
#endif
#include <opencv2/highgui.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "edit/history/edit_transaction.hpp"
#include "edit/history/version.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "history_test_fixation.hpp"
#include "image/image_buffer.hpp"
#include "sleeve/sleeve_manager.hpp"
#include "type/supported_file_type.hpp"

using namespace alcedo;

TEST_F(EditHistoryTests, DISABLED_ApplyAddTransaction) {
  CPUPipelineExecutor pipeline = CPUPipelineExecutor();

  Version             version(1);
  EditTransaction     transaction(TransactionType::_ADD, OperatorType::EXPOSURE,
                                  PipelineStageName::Basic_Adjustment, {{"exposure", 0.5}});

  EXPECT_TRUE(transaction.ApplyTransaction(pipeline));
}

TEST_F(EditHistoryTests, DISABLED_ApplyDeleteTransaction) {
  CPUPipelineExecutor pipeline = CPUPipelineExecutor();

  // First, add an operator to delete later.
  EditTransaction     add_transaction(TransactionType::_ADD, OperatorType::EXPOSURE,
                                      PipelineStageName::Basic_Adjustment, {{"exposure", 0.5}});
  EXPECT_TRUE(add_transaction.ApplyTransaction(pipeline));

  // Now, create a delete transaction for the same operator.
  EditTransaction delete_transaction(TransactionType::_DELETE, OperatorType::EXPOSURE,
                                     PipelineStageName::Basic_Adjustment, {},
                                     add_transaction.GetTransactionID());

  EXPECT_TRUE(delete_transaction.ApplyTransaction(pipeline));
}

TEST_F(EditHistoryTests, DISABLED_ApplyEditTransaction) {
  CPUPipelineExecutor pipeline = CPUPipelineExecutor();

  // First, add an operator to edit later.
  EditTransaction     add_transaction(TransactionType::_ADD, OperatorType::EXPOSURE,
                                      PipelineStageName::Basic_Adjustment, {{"exposure", 0.5}});
  EXPECT_TRUE(add_transaction.ApplyTransaction(pipeline));

  // Now, create an edit transaction for the same operator.
  EditTransaction edit_transaction(TransactionType::_EDIT, OperatorType::EXPOSURE,
                                   PipelineStageName::Basic_Adjustment, {{"exposure", 1.0}},
                                   add_transaction.GetTransactionID());

  EXPECT_TRUE(edit_transaction.ApplyTransaction(pipeline));
}

void SetPipelineTemplate(std::shared_ptr<PipelineExecutor> executor) {
  auto&          raw_stage     = executor->GetStage(PipelineStageName::Image_Loading);
  auto&          global_params = executor->GetGlobalParams();
  nlohmann::json decode_params;
#ifdef HAVE_CUDA
  decode_params["raw"]["cuda"] = false;
#else
  decode_params["raw"]["cuda"] = false;
#endif
  decode_params["raw"]["highlights_reconstruct"] = false;
  decode_params["raw"]["use_camera_wb"]          = true;
  decode_params["raw"]["backend"]                = "alcedo";
  nlohmann::json to_ws_params;
  to_ws_params["ocio"] = {{"src", "ACES2065-1"},
                          {"dst", "ACEScc"},
                          {"normalize", true},
                          {"transform_type", 0}};
  raw_stage.SetOperator(OperatorType::RAW_DECODE, decode_params);
  // raw_stage.SetOperator(OperatorType::CST, to_ws_params);

  auto& to_ws = executor->GetStage(PipelineStageName::Basic_Adjustment);
  to_ws.SetOperator(OperatorType::TO_WS, to_ws_params, global_params);

  nlohmann::json output_params;
  auto&          output_stage = executor->GetStage(PipelineStageName::Output_Transform);
  output_params["ocio"]       = {
      {"src", "ACEScc"}, {"dst", "Camera Rec.709"}, {"limit", true}, {"transform_type", 1}};
  output_stage.SetOperator(OperatorType::TO_OUTPUT, output_params, global_params);
}

TEST_F(EditHistoryTests, DISABLED_TestWithImage) {
  {
    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 8, 0);
    image_path_t              path = std::string(TEST_IMG_PATH) + "/raw/building";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      if (!img.is_directory() && is_supported_file(img.path())) imgs.push_back(img.path());
    }

    manager.LoadToPath(imgs, L"");

    // Read image data
    auto              img_pool = manager.GetPool()->GetPool();

    PipelineScheduler scheduler{};

    for (const auto& img : img_pool) {
      auto         img_ptr = img.second;
      PipelineTask task1;

      auto         buffer    = ByteBufferLoader::LoadFromImage(img_ptr);
      task1.input_           = buffer ? std::make_shared<ImageBuffer>(std::move(*buffer)) : nullptr;

      auto pipeline_executor = std::make_shared<CPUPipelineExecutor>();
      pipeline_executor->SetPreviewMode(true);

      task1.pipeline_executor_ = pipeline_executor;
      SetPipelineTemplate(task1.pipeline_executor_);

      task1.options_.is_blocking_ = false;
      task1.options_.is_callback_ = true;
      auto save_callback          = [img_ptr](ImageBuffer&) {
        // gpu_data.convertTo(gpu_data, CV_16UC3, 65535.0f);
        // cv::cuda::cvtColor(gpu_data, gpu_data, cv::COLOR_RGB2BGR);

        // output.SyncToCPU();

        // std::string           file_name =
        // conv::ToBytes(img_ptr->_image_path.filename().wstring()); std::string           time =
        // TimeProvider::TimePointToString(TimeProvider::Now());

        // static constexpr auto save_path = TEST_IMG_PATH "/my_pipeline/batch_results/{}.tif";
        // std::string           save_name = file_name + "_" + time;
        // cv::imwrite(std::format(save_path, save_name), output.GetCPUData());
      };

      auto empty_callback         = [](ImageBuffer&) {};

      task1.callback_             = empty_callback;
      task1.options_.is_blocking_ = true;
      task1.result_               = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
      auto future_task1           = task1.result_->get_future();

      auto task2                  = task1;  // Make a copy of task1 for task2
      task2.result_               = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
      ;  // Clear the promise for task2 to avoid issues
      auto future_task2           = task2.result_->get_future();
      task2.options_.is_blocking_ = true;  // Make task2 non-blocking

      scheduler.ScheduleTask(std::move(task1));

      future_task1.get();  // Wait for task1 to complete

      // Create and apply an edit transaction
      EditTransaction tx1(TransactionType::_ADD, OperatorType::EXPOSURE,
                          PipelineStageName::Basic_Adjustment, {{"exposure", 0.5}});
      tx1.ApplyTransaction(*pipeline_executor);

      auto task3                  = task2;
      task3.result_               = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
      auto future_task3           = task3.result_->get_future();
      task3.options_.is_blocking_ = true;  // Make task3 non-blocking

      scheduler.ScheduleTask(std::move(task2));
      future_task2.get();  // Wait for task2 to complete

      // Create and apply another edit transaction
      EditTransaction tx2(TransactionType::_ADD, OperatorType::CONTRAST,
                          PipelineStageName::Basic_Adjustment, {{"contrast", 50}},
                          tx1.GetTransactionID());
      tx2.ApplyTransaction(*pipeline_executor);

      scheduler.ScheduleTask(std::move(task3));
      future_task3.get();  // Wait for task3 to complete
    }
  }
}

TEST_F(EditHistoryTests, DISABLED_TestWithImage_Animated) {
  {
    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 8, 0);
    image_path_t              path = std::string(TEST_IMG_PATH) + "/raw/building";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      if (!img.is_directory() && is_supported_file(img.path())) imgs.push_back(img.path());
    }

    manager.LoadToPath(imgs, L"");

    // Read image data
    auto              img_pool = manager.GetPool()->GetPool();

    PipelineScheduler scheduler{};

    auto              img_ptr = img_pool.begin()->second;

    // Animation loop for the same image
    cv::namedWindow("preview");
    cv::resizeWindow("preview", 800, 600);

    PipelineTask task;
    auto         buffer    = ByteBufferLoader::LoadFromImage(img_ptr);
    task.input_            = buffer ? std::make_shared<ImageBuffer>(std::move(*buffer)) : nullptr;

    auto pipeline_executor = std::make_shared<CPUPipelineExecutor>();
    pipeline_executor->SetPreviewMode(true);

    task.pipeline_executor_ = pipeline_executor;
    SetPipelineTemplate(task.pipeline_executor_);

    task.options_.is_callback_     = false;
    task.options_.is_seq_callback_ = true;
    auto display_callback          = [](ImageBuffer&, uint32_t id) {
      auto time = TimeProvider::TimePointToString(TimeProvider::Now());
      std::cout << "New frame " << id << " rendered at " << time << "." << std::endl;

      // cv::cvtColor(output.GetCPUData(), output.GetCPUData(), cv::COLOR_RGB2BGR);
    };
    task.seq_callback_         = display_callback;
    task.options_.is_blocking_ = true;

    nlohmann::json exposure_params;
    exposure_params["exposure"] = 0.0f;

    auto& basic_stage = task.pipeline_executor_->GetStage(PipelineStageName::Basic_Adjustment);
    basic_stage.SetOperator(OperatorType::EXPOSURE, exposure_params);

    pipeline_executor->SetExecutionStages();

    for (float exposure = -2.0f; exposure <= 2.0f; exposure += 0.05f) {
      PipelineTask task1 = task;  // Make a copy of task for task1
      auto& basic_stage  = task1.pipeline_executor_->GetStage(PipelineStageName::Basic_Adjustment);
      exposure_params["exposure"] = exposure;
      basic_stage.SetOperator(OperatorType::EXPOSURE, exposure_params);
      task1.options_.is_blocking_ = true;
      task1.result_               = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
      auto future_task1           = task1.result_->get_future();

      scheduler.ScheduleTask(std::move(task1));
      auto result = future_task1.get();  // Wait for task1 to complete
      cv::cvtColor(result->GetCPUData(), result->GetCPUData(), cv::COLOR_RGB2BGR);
      cv::imshow("preview", result->GetCPUData());
      cv::waitKey(1);
    }
  }
}

TEST_F(EditHistoryTests, TestWithPreviewPipeline) {
  {
    SleeveManager             manager{db_path_};
    ImageLoader               image_loader(128, 1, 0);
    image_path_t              path = std::string(TEST_IMG_PATH) + "/raw/building";
    std::vector<image_path_t> imgs;
    for (const auto& img : std::filesystem::directory_iterator(path)) {
      if (!img.is_directory() && is_supported_file(img.path())) imgs.push_back(img.path());
    }

    manager.LoadToPath(imgs, L"");

    // Read image data
    auto              img_pool = manager.GetPool()->GetPool();

    PipelineScheduler scheduler{};

    auto              img_ptr = img_pool.begin()->second;

    // Animation loop for the same image

    PipelineTask      task;
    auto              buffer = ByteBufferLoader::LoadFromImage(img_ptr);
    task.input_              = buffer ? std::make_shared<ImageBuffer>(std::move(*buffer)) : nullptr;

    auto pipeline_executor   = std::make_shared<CPUPipelineExecutor>(true);
    pipeline_executor->SetPreviewMode(true);

    task.pipeline_executor_ = pipeline_executor;
    SetPipelineTemplate(task.pipeline_executor_);

    auto& global_params            = task.pipeline_executor_->GetGlobalParams();

    task.options_.is_callback_     = false;
    task.options_.is_seq_callback_ = true;
    auto display_callback          = [](ImageBuffer& output, uint32_t id) {
      std::cout << "New frame " << id << std::endl;
    };
    task.seq_callback_         = display_callback;
    task.options_.is_blocking_ = true;

    nlohmann::json exposure_params;
    exposure_params["exposure"] = 0.0f;

    auto& basic_stage = task.pipeline_executor_->GetStage(PipelineStageName::Basic_Adjustment);
    basic_stage.SetOperator(OperatorType::EXPOSURE, exposure_params, global_params);

    pipeline_executor->SetExecutionStages();

    for (float exposure = -5.0f; exposure <= 5.0f; exposure += .5f) {
      PipelineTask task1 = task;  // Make a copy of task for task1
      auto& basic_stage  = task1.pipeline_executor_->GetStage(PipelineStageName::Basic_Adjustment);

      // Update multiple parameters
      exposure_params["exposure"] = exposure;
      basic_stage.SetOperator(OperatorType::EXPOSURE, exposure_params, global_params);

      // Set up blocking and result promise
      task1.options_.is_blocking_ = true;
      task1.result_               = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
      auto future_task1           = task1.result_->get_future();

      scheduler.ScheduleTask(std::move(task1));

      auto result = future_task1.get();  // Wait for task1 to complete

      cv::cvtColor(result->GetCPUData(), result->GetCPUData(), cv::COLOR_RGB2BGR);
      cv::imshow("preview", result->GetCPUData());

      cv::waitKey(0);
    }
  }
}
