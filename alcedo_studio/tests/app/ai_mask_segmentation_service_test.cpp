//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM3.A to SM3.C: the AI Mask tool lifecycle of AiMaskSegmentationService over a fake
// segmentation client.

#include "app/ai_mask_segmentation_service.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace alcedo {
namespace {

using CallLog = std::vector<std::string>;

auto Ok() -> SegmentationCallStatus {
  SegmentationCallStatus status;
  status.ok = true;
  return status;
}

auto Failed(std::string message) -> SegmentationCallStatus {
  SegmentationCallStatus status;
  status.ok    = false;
  status.error = std::move(message);
  return status;
}

/// Writes each call into a shared log; answers with configurable results.
class FakeSegmentationClient final : public sidecar_client::SegmentationClient {
 public:
  explicit FakeSegmentationClient(std::shared_ptr<CallLog> log) : log_(std::move(log)) {}

  auto LoadModels(const std::string& model_directory, const std::string& device,
                  std::chrono::milliseconds /*timeout*/) -> SegmentationCallStatus override {
    log_->push_back("load " + model_directory + " " + device);
    return Ok();
  }
  auto UnloadModels(std::chrono::milliseconds /*timeout*/) -> SegmentationCallStatus override {
    log_->push_back("unload");
    return Ok();
  }
  auto GetStatus(std::chrono::milliseconds /*timeout*/) -> SegmentationStatus override {
    log_->push_back("status");
    return SegmentationStatus{.status = Ok()};
  }
  auto StartSession(const SegmentationRgb8Image& image, const std::string& image_key,
                    std::chrono::milliseconds /*timeout*/) -> SegmentationSessionResult override {
    const auto session_id = "s" + std::to_string(++started_);
    log_->push_back("start " + session_id + " " + std::to_string(image.width) + " " +
                    std::to_string(image.pixels.size()) +
                    (image_key.empty() ? std::string{} : " " + image_key));
    SegmentationSessionResult result;
    if (fail_start_) {
      result.status = Failed("encoder failed");
      return result;
    }
    if (image.pixels.empty() && image_key != cached_key_) {
      result.status           = Failed("the segmentation image is not cached");
      result.image_not_cached = true;
      return result;
    }
    result.status     = Ok();
    result.session_id = session_id;
    result.from_cache = image.pixels.empty();
    // The cached image entry is 4096 x 2731.
    result.image_width  = image.pixels.empty() ? 4096 : image.width;
    result.image_height = image.pixels.empty() ? 2731 : image.height;
    return result;
  }
  auto Decode(const std::string& session_id, const SegmentationPrompt& prompt,
              std::chrono::milliseconds /*timeout*/) -> SegmentationDecodeResult override {
    log_->push_back("decode " + session_id + " " + std::to_string(prompt.points.size()));
    return SegmentationDecodeResult{.status = Ok()};
  }
  auto EndSession(const std::string& session_id, std::chrono::milliseconds /*timeout*/)
      -> SegmentationCallStatus override {
    log_->push_back("end " + session_id);
    return fail_end_ ? Failed("transport closed") : Ok();
  }
  auto RunTask(const std::string& session_id, SegmentationTask task,
               std::optional<uint32_t> person_index, std::chrono::milliseconds /*timeout*/)
      -> SegmentationTaskResult override {
    std::string entry = "task " + session_id + " " + std::to_string(static_cast<int>(task));
    if (person_index.has_value()) {
      entry += " person " + std::to_string(*person_index);
    }
    log_->push_back(entry);
    return SegmentationTaskResult{.status = Ok(), .found = true};
  }

  bool        fail_start_ = false;
  bool        fail_end_   = false;
  /// The image key that a key-only start finds in the cache.
  std::string cached_key_ = "cached";

 private:
  std::shared_ptr<CallLog> log_;
  int                      started_ = 0;
};

struct ServiceUnderTest {
  std::shared_ptr<CallLog>                   log    = std::make_shared<CallLog>();
  FakeSegmentationClient*                    client = nullptr;
  std::unique_ptr<AiMaskSegmentationService> service;

  ServiceUnderTest() {
    auto fake                           = std::make_unique<FakeSegmentationClient>(log);
    client                              = fake.get();
    // The lease deleter writes into the same log, so the test sees when the lease is released.
    auto                  log_for_lease = log;
    std::shared_ptr<void> lease(new int(0), [log_for_lease](int* token) {
      delete token;
      log_for_lease->push_back("lease released");
    });
    service = std::make_unique<AiMaskSegmentationService>(std::move(lease), std::move(fake));
  }
};

auto Image(uint32_t width) -> SegmentationRgb8Image {
  SegmentationRgb8Image image;
  image.width  = width;
  image.height = 4;
  image.pixels.assign(static_cast<size_t>(width) * 4 * 3, 0);
  return image;
}

TEST(AiMaskSegmentationServiceTest, StartImageEndsPreviousSession) {
  ServiceUnderTest under_test;
  ASSERT_TRUE(under_test.service->StartImage(Image(8)).status.ok);
  ASSERT_TRUE(under_test.service->StartImage(Image(16)).status.ok);

  EXPECT_EQ(*under_test.log, (CallLog{"start s1 8 96", "end s1", "start s2 16 192"}));
  EXPECT_TRUE(under_test.service->HasSession());
}

TEST(AiMaskSegmentationServiceTest, StartImageReportsEndFailureWithoutStarting) {
  ServiceUnderTest under_test;
  ASSERT_TRUE(under_test.service->StartImage(Image(8)).status.ok);
  under_test.client->fail_end_ = true;

  const auto result            = under_test.service->StartImage(Image(16));

  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.error, "transport closed");
  EXPECT_EQ(*under_test.log, (CallLog{"start s1 8 96", "end s1"}));
  EXPECT_TRUE(under_test.service->HasSession());
}

TEST(AiMaskSegmentationServiceTest, FailedStartKeepsNoSession) {
  ServiceUnderTest under_test;
  under_test.client->fail_start_ = true;

  EXPECT_FALSE(under_test.service->StartImage(Image(8)).status.ok);
  EXPECT_FALSE(under_test.service->HasSession());
  EXPECT_TRUE(under_test.service->EndImage().ok);
  EXPECT_EQ(*under_test.log, (CallLog{"start s1 8 96"}));
}

TEST(AiMaskSegmentationServiceTest, DecodeWithoutSessionFailsWithoutCall) {
  ServiceUnderTest   under_test;
  SegmentationPrompt prompt;
  prompt.points.push_back({.x = 0.5F, .y = 0.5F, .label = SegmentationPointLabel::kAdd});

  const auto result = under_test.service->Decode(prompt);

  EXPECT_FALSE(result.status.ok);
  EXPECT_TRUE(under_test.log->empty());
  ASSERT_TRUE(under_test.service->StartImage(Image(8)).status.ok);
  EXPECT_TRUE(under_test.service->Decode(prompt).status.ok);
  EXPECT_EQ(under_test.log->back(), "decode s1 1");
}

TEST(AiMaskSegmentationServiceTest, CloseEndsSessionUnloadsAndReleasesLease) {
  ServiceUnderTest under_test;
  ASSERT_TRUE(under_test.service->LoadModels("models", "directml").ok);
  ASSERT_TRUE(under_test.service->StartImage(Image(8)).status.ok);

  EXPECT_TRUE(under_test.service->Close().ok);
  EXPECT_EQ(*under_test.log, (CallLog{"load models directml", "start s1 8 96", "end s1", "unload",
                                      "lease released"}));
  EXPECT_TRUE(under_test.service->IsClosed());

  EXPECT_TRUE(under_test.service->Close().ok);
  EXPECT_FALSE(under_test.service->StartImage(Image(8)).status.ok);
  EXPECT_EQ(under_test.log->size(), 5U);
}

TEST(AiMaskSegmentationServiceTest, StatusReadsSidecarUntilClosed) {
  ServiceUnderTest under_test;
  EXPECT_TRUE(under_test.service->Status().status.ok);
  EXPECT_EQ(*under_test.log, (CallLog{"status"}));

  ASSERT_TRUE(under_test.service->Close().ok);
  const auto closed = under_test.service->Status();

  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(closed.status.error, "the AI Mask segmentation service is closed");
  EXPECT_EQ(under_test.log->back(), "lease released");
}

TEST(AiMaskSegmentationServiceTest, DestructorClosesService) {
  ServiceUnderTest under_test;
  ASSERT_TRUE(under_test.service->StartImage(Image(8)).status.ok);

  under_test.client = nullptr;
  under_test.service.reset();

  EXPECT_EQ(*under_test.log, (CallLog{"start s1 8 96", "end s1", "unload", "lease released"}));
}

TEST(AiMaskSegmentationServiceTest, StartImageSendsImageKey) {
  ServiceUnderTest under_test;

  const auto       result = under_test.service->StartImage(Image(8), "photo-a");

  ASSERT_TRUE(result.status.ok);
  EXPECT_FALSE(result.from_cache);
  EXPECT_EQ(*under_test.log, (CallLog{"start s1 8 96 photo-a"}));
}

TEST(AiMaskSegmentationServiceTest, StartCachedImageEndsPreviousSessionAndSendsKeyOnly) {
  ServiceUnderTest under_test;
  ASSERT_TRUE(under_test.service->StartImage(Image(8)).status.ok);

  const auto result = under_test.service->StartCachedImage("cached");

  ASSERT_TRUE(result.status.ok);
  EXPECT_TRUE(result.from_cache);
  EXPECT_TRUE(under_test.service->HasSession());
  EXPECT_EQ(*under_test.log, (CallLog{"start s1 8 96", "end s1", "start s2 0 0 cached"}));
  ASSERT_TRUE(under_test.service->Decode(SegmentationPrompt{}).status.ok);
  EXPECT_EQ(under_test.log->back(), "decode s2 0");
}

TEST(AiMaskSegmentationServiceTest, StartResultsCarryTheSessionImageSize) {
  ServiceUnderTest under_test;

  const auto       with_pixels = under_test.service->StartImage(Image(8), "photo-a");
  const auto       key_only    = under_test.service->StartCachedImage("cached");

  ASSERT_TRUE(with_pixels.status.ok);
  EXPECT_EQ(with_pixels.image_width, 8U);
  EXPECT_EQ(with_pixels.image_height, 4U);
  ASSERT_TRUE(key_only.status.ok);
  EXPECT_EQ(key_only.image_width, 4096U);
  EXPECT_EQ(key_only.image_height, 2731U);
}

TEST(AiMaskSegmentationServiceTest, StartCachedImageMissKeepsNoSession) {
  ServiceUnderTest under_test;

  const auto       miss = under_test.service->StartCachedImage("photo-b");

  EXPECT_FALSE(miss.status.ok);
  EXPECT_TRUE(miss.image_not_cached);
  EXPECT_FALSE(under_test.service->HasSession());
  // The caller renders the image and starts again with the pixels and the same key.
  ASSERT_TRUE(under_test.service->StartImage(Image(8), "photo-b").status.ok);
  EXPECT_EQ(*under_test.log, (CallLog{"start s1 0 0 photo-b", "start s2 8 96 photo-b"}));
  EXPECT_TRUE(under_test.service->HasSession());
}

TEST(AiMaskSegmentationServiceTest, StartCachedImageWithEmptyKeyFailsWithoutCall) {
  ServiceUnderTest under_test;

  EXPECT_FALSE(under_test.service->StartCachedImage("").status.ok);
  EXPECT_TRUE(under_test.log->empty());
}

TEST(AiMaskSegmentationServiceTest, RunTaskWithoutSessionFailsWithoutCall) {
  ServiceUnderTest under_test;

  const auto       result = under_test.service->RunTask(SegmentationTask::kSky, std::nullopt);

  EXPECT_FALSE(result.status.ok);
  EXPECT_FALSE(result.found);
  EXPECT_TRUE(under_test.log->empty());
  ASSERT_TRUE(under_test.service->Close().ok);
  EXPECT_FALSE(under_test.service->RunTask(SegmentationTask::kSky, std::nullopt).status.ok);
}

TEST(AiMaskSegmentationServiceTest, RunTaskUsesCurrentSession) {
  ServiceUnderTest under_test;
  ASSERT_TRUE(under_test.service->StartImage(Image(8)).status.ok);
  ASSERT_TRUE(under_test.service->StartImage(Image(16)).status.ok);

  EXPECT_TRUE(under_test.service->RunTask(SegmentationTask::kPerson, 2U).found);
  EXPECT_TRUE(under_test.service->RunTask(SegmentationTask::kSubject, std::nullopt).status.ok);

  EXPECT_EQ(*under_test.log, (CallLog{"start s1 8 96", "end s1", "start s2 16 192",
                                      "task s2 5 person 2", "task s2 1"}));
}

}  // namespace
}  // namespace alcedo
