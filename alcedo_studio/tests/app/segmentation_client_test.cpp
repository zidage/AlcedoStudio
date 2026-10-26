//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM3.A to SM3.C: the gRPC segmentation client against an in-process SegmentationService.

#include "sidecar_client/segmentation_client.hpp"

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ai_common.pb.h"
#include "segmentation.grpc.pb.h"

namespace alcedo::sidecar_client {
namespace {

constexpr int  kMaxMessageBytes = 64 * 1024 * 1024;
constexpr auto kTimeout         = std::chrono::milliseconds(10000);

void           SetOkHeader(alcedo::ai::AiResponseHeader* header) {
  header->set_status(alcedo::ai::AI_STATUS_OK);
  header->set_error_code(alcedo::ai::AI_ERROR_NONE);
}

/// Records what the client sends; answers with fixed values.
/// The Sky answer of the fake service: not found, the sky head alpha (ONNX Runtime), or the
/// region union (Core ML).
enum class FakeSkyResponse { kNotFound, kSkyHeadAlpha, kRegionUnion };

/// Size of the sky head alpha grid: the encoder input, 1024 x 1024.
constexpr uint32_t                 kSkyGridSide = 1024;

class RecordingSegmentationService final : public alcedo::ai::SegmentationService::Service {
 public:
  auto LoadSegmentationModels(grpc::ServerContext*                             context,
                              const alcedo::ai::LoadSegmentationModelsRequest* request,
                              alcedo::ai::LoadSegmentationModelsResponse*      response)
      -> grpc::Status override {
    Record(context);
    auto* header = response->mutable_header();
    header->set_status(alcedo::ai::AI_STATUS_INVALID_ARGUMENT);
    header->set_error_code(alcedo::ai::AI_ERROR_INVALID_INPUT);
    header->set_error_message("segmentation device \"" + request->device() + "\" is not supported");
    return grpc::Status::OK;
  }

  auto GetSegmentationStatus(grpc::ServerContext* context,
                             const alcedo::ai::GetSegmentationStatusRequest* /*request*/,
                             alcedo::ai::GetSegmentationStatusResponse* response)
      -> grpc::Status override {
    Record(context);
    SetOkHeader(response->mutable_header());
    response->set_loaded(true);
    response->set_model_set_id("alcedo-ai-mask-v1");
    response->set_backend("onnxruntime-directml");
    auto* model = response->add_models();
    model->set_role("encoder");
    model->set_device("directml:0");
    model->set_load_ms(2900);
    response->set_session_count(1);
    response->set_cached_image_count(3);
    return grpc::Status::OK;
  }

  auto StartSegmentationSession(grpc::ServerContext*                               context,
                                const alcedo::ai::StartSegmentationSessionRequest* request,
                                alcedo::ai::StartSegmentationSessionResponse*      response)
      -> grpc::Status override {
    Record(context);
    {
      std::lock_guard lock(mutex_);
      received_width_       = request->width();
      received_height_      = request->height();
      received_pixel_bytes_ = request->rgb8_pixels().size();
      received_image_key_   = request->image_key();
    }
    // A key-only start: "cached" is a cached image, any other key is not cached.
    if (request->rgb8_pixels().empty() && request->image_key() != "cached") {
      auto* header = response->mutable_header();
      header->set_status(alcedo::ai::AI_STATUS_INVALID_ARGUMENT);
      header->set_error_code(alcedo::ai::AI_ERROR_IMAGE_NOT_CACHED);
      header->set_error_message("the segmentation image is not cached; send its pixels");
      return grpc::Status::OK;
    }
    SetOkHeader(response->mutable_header());
    response->set_session_id("session-1");
    response->set_encoder_ms(request->rgb8_pixels().empty() ? 0 : 70);
    response->set_from_cache(request->rgb8_pixels().empty());
    // The cached image entry of the key "cached" is 4096 x 2731.
    response->set_image_width(request->rgb8_pixels().empty() ? 4096 : request->width());
    response->set_image_height(request->rgb8_pixels().empty() ? 2731 : request->height());
    return grpc::Status::OK;
  }

  auto DecodeSegmentationPrompt(grpc::ServerContext*                               context,
                                const alcedo::ai::DecodeSegmentationPromptRequest* request,
                                alcedo::ai::DecodeSegmentationPromptResponse*      response)
      -> grpc::Status override {
    Record(context);
    {
      std::lock_guard lock(mutex_);
      decode_request_ = *request;
    }
    SetOkHeader(response->mutable_header());
    for (uint32_t token : {2U, 3U, 1U}) {
      auto* level = response->add_masks();
      level->mutable_logits()->Resize(256 * 256, static_cast<float>(token));
      level->set_score(0.25F * static_cast<float>(token));
      level->set_token(token);
      level->set_area(100 * token);
    }
    response->set_selected_index(2);
    response->set_logit_size(256);
    response->set_decoder_ms(12);
    return grpc::Status::OK;
  }

  auto RunSegmentationTask(grpc::ServerContext*                          context,
                           const alcedo::ai::RunSegmentationTaskRequest* request,
                           alcedo::ai::RunSegmentationTaskResponse*      response)
      -> grpc::Status override {
    Record(context);
    {
      std::lock_guard lock(mutex_);
      task_request_ = *request;
    }
    SetOkHeader(response->mutable_header());
    // Sky: see FakeSkyResponse. People and Person: an alpha grid of 8 x 4 values and a list of
    // two people; Person: the grid covers the crop rectangle (0.25, 0.1)-(0.75, 0.9).
    if (request->task() == alcedo::ai::SEGMENTATION_TASK_SKY) {
      FillSkyResponse(response);
      return grpc::Status::OK;
    }
    const bool person_task = request->task() == alcedo::ai::SEGMENTATION_TASK_PERSON;
    response->set_found(true);
    response->set_mask_kind(alcedo::ai::SEGMENTATION_TASK_MASK_KIND_ALPHA);
    response->set_mask_width(8);
    response->set_mask_height(4);
    response->mutable_alpha_values()->Resize(8 * 4, 0.25F);
    response->set_alpha_values(9, 0.75F);
    auto* mask_box = response->mutable_mask_box();
    mask_box->set_x0(person_task ? 0.25F : 0.0F);
    mask_box->set_y0(person_task ? 0.1F : 0.0F);
    mask_box->set_x1(person_task ? 0.75F : 1.0F);
    mask_box->set_y1(person_task ? 0.9F : 1.0F);
    response->set_presence(0.99F);
    response->set_subject_score(0.93F);
    response->set_mask_area(1);
    response->set_model_ms(21);
    for (uint32_t index : {0U, 1U}) {
      auto* person = response->add_people();
      person->set_index(index);
      person->mutable_box()->set_x0(0.1F + 0.5F * static_cast<float>(index));
      person->mutable_box()->set_y0(0.2F);
      person->mutable_box()->set_x1(0.3F + 0.5F * static_cast<float>(index));
      person->mutable_box()->set_y1(0.9F);
      person->set_score(0.8F - 0.1F * static_cast<float>(index));
      person->set_subject_fraction(index == 0 ? 0.6F : 0.05F);
      person->set_in_subject(index == 0);
    }
    return grpc::Status::OK;
  }

  void SetSkyResponse(FakeSkyResponse sky_response) {
    std::lock_guard lock(mutex_);
    sky_response_ = sky_response;
  }

  [[nodiscard]] auto Peers() -> std::set<std::string> {
    std::lock_guard lock(mutex_);
    return peers_;
  }
  [[nodiscard]] auto ReceivedPixelBytes() -> size_t {
    std::lock_guard lock(mutex_);
    return received_pixel_bytes_;
  }
  [[nodiscard]] auto ReceivedImageKey() -> std::string {
    std::lock_guard lock(mutex_);
    return received_image_key_;
  }
  [[nodiscard]] auto ReceivedSize() -> std::pair<uint32_t, uint32_t> {
    std::lock_guard lock(mutex_);
    return {received_width_, received_height_};
  }
  [[nodiscard]] auto DecodeRequest() -> alcedo::ai::DecodeSegmentationPromptRequest {
    std::lock_guard lock(mutex_);
    return decode_request_;
  }
  [[nodiscard]] auto TaskRequest() -> alcedo::ai::RunSegmentationTaskRequest {
    std::lock_guard lock(mutex_);
    return task_request_;
  }

 private:
  void FillSkyResponse(alcedo::ai::RunSegmentationTaskResponse* response) {
    FakeSkyResponse sky_response = FakeSkyResponse::kNotFound;
    {
      std::lock_guard lock(mutex_);
      sky_response = sky_response_;
    }
    switch (sky_response) {
      case FakeSkyResponse::kNotFound:
        response->set_message("No sky found: sky presence 0.312 is below 0.423.");
        response->set_presence(0.312F);
        response->set_presence_threshold(0.423F);
        return;
      case FakeSkyResponse::kSkyHeadAlpha: {
        // The alpha of the stretched image: 0.25 everywhere, 1 in the top row, 0.5 at the last
        // value.
        response->set_found(true);
        response->set_mask_kind(alcedo::ai::SEGMENTATION_TASK_MASK_KIND_ALPHA);
        response->set_mask_width(kSkyGridSide);
        response->set_mask_height(kSkyGridSide);
        auto* values = response->mutable_alpha_values();
        values->Resize(static_cast<int>(kSkyGridSide * kSkyGridSide), 0.25F);
        for (uint32_t column = 0; column < kSkyGridSide; ++column) {
          values->Set(static_cast<int>(column), 1.0F);
        }
        values->Set(values->size() - 1, 0.5F);
        auto* mask_box = response->mutable_mask_box();
        mask_box->set_x0(0.0F);
        mask_box->set_y0(0.0F);
        mask_box->set_x1(1.0F);
        mask_box->set_y1(1.0F);
        response->set_presence(0.002F);
        response->set_presence_threshold(5e-4F);
        response->set_mask_area(kSkyGridSide + 1);
        response->set_model_ms(54);
        return;
      }
      case FakeSkyResponse::kRegionUnion: {
        // A 6 x 4 session image: the top row is sky.
        response->set_found(true);
        response->set_mask_kind(alcedo::ai::SEGMENTATION_TASK_MASK_KIND_BINARY);
        response->set_mask_width(6);
        response->set_mask_height(4);
        std::string mask(6 * 4, '\0');
        std::fill_n(mask.begin(), 6, '\xff');
        response->set_mask_r8(mask);
        response->set_presence(0.8F);
        response->set_presence_threshold(0.423F);
        response->set_region_count(2);
        response->set_mask_area(6);
        response->set_model_ms(40);
        return;
      }
    }
  }

  void Record(grpc::ServerContext* context) {
    std::lock_guard lock(mutex_);
    peers_.insert(context->peer());
  }

  std::mutex                                  mutex_;
  std::set<std::string>                       peers_;
  uint32_t                                    received_width_       = 0;
  uint32_t                                    received_height_      = 0;
  size_t                                      received_pixel_bytes_ = 0;
  std::string                                 received_image_key_;
  alcedo::ai::DecodeSegmentationPromptRequest decode_request_;
  alcedo::ai::RunSegmentationTaskRequest      task_request_;
  FakeSkyResponse                             sky_response_ = FakeSkyResponse::kNotFound;
};

class SegmentationClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    int                 port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
    builder.SetMaxSendMessageSize(kMaxMessageBytes);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    ASSERT_GT(port, 0);
    client_ = MakeGrpcSegmentationClient("127.0.0.1:" + std::to_string(port));
  }

  void TearDown() override {
    client_.reset();
    if (server_) {
      server_->Shutdown();
    }
  }

  RecordingSegmentationService        service_;
  std::unique_ptr<grpc::Server>       server_;
  std::unique_ptr<SegmentationClient> client_;
};

TEST_F(SegmentationClientTest, StartSessionSendsImageLargerThanDefaultLimit) {
  SegmentationRgb8Image image;
  image.width  = 4096;
  image.height = 2731;
  image.pixels.assign(static_cast<size_t>(image.width) * image.height * 3, 127);

  const auto result = client_->StartSession(image, "photo-a", kTimeout);

  ASSERT_TRUE(result.status.ok) << result.status.error;
  EXPECT_EQ(result.session_id, "session-1");
  EXPECT_EQ(result.encoder_ms, 70);
  EXPECT_FALSE(result.from_cache);
  EXPECT_EQ(service_.ReceivedImageKey(), "photo-a");
  EXPECT_EQ(service_.ReceivedPixelBytes(), image.pixels.size());
  EXPECT_EQ(service_.ReceivedSize(), (std::pair<uint32_t, uint32_t>{4096, 2731}));
}

TEST_F(SegmentationClientTest, SequentialCallsUseOneConnection) {
  for (int call = 0; call < 20; ++call) {
    const auto status = client_->GetStatus(kTimeout);
    ASSERT_TRUE(status.status.ok) << status.status.error;
    EXPECT_TRUE(status.loaded);
    ASSERT_EQ(status.models.size(), 1U);
    EXPECT_EQ(status.models[0].device, "directml:0");
    EXPECT_EQ(status.models[0].load_ms, 2900);
    EXPECT_EQ(status.cached_image_count, 3U);
  }
  EXPECT_EQ(service_.Peers().size(), 1U);
}

TEST_F(SegmentationClientTest, StartSessionSendsImageKeyWithoutPixels) {
  const auto result = client_->StartSession(SegmentationRgb8Image{}, "cached", kTimeout);

  ASSERT_TRUE(result.status.ok) << result.status.error;
  EXPECT_TRUE(result.from_cache);
  EXPECT_FALSE(result.image_not_cached);
  EXPECT_EQ(result.encoder_ms, 0);
  EXPECT_EQ(result.session_id, "session-1");
  EXPECT_EQ(service_.ReceivedImageKey(), "cached");
  EXPECT_EQ(service_.ReceivedPixelBytes(), 0U);
  EXPECT_EQ(service_.ReceivedSize(), (std::pair<uint32_t, uint32_t>{0, 0}));
}

TEST_F(SegmentationClientTest, StartSessionResultCarriesImageSize) {
  SegmentationRgb8Image image;
  image.width  = 64;
  image.height = 48;
  image.pixels.assign(static_cast<size_t>(image.width) * image.height * 3, 90);

  const auto with_pixels = client_->StartSession(image, "photo-a", kTimeout);
  const auto key_only    = client_->StartSession(SegmentationRgb8Image{}, "cached", kTimeout);
  const auto not_cached  = client_->StartSession(SegmentationRgb8Image{}, "photo-b", kTimeout);

  ASSERT_TRUE(with_pixels.status.ok) << with_pixels.status.error;
  EXPECT_EQ(with_pixels.image_width, 64U);
  EXPECT_EQ(with_pixels.image_height, 48U);
  ASSERT_TRUE(key_only.status.ok) << key_only.status.error;
  EXPECT_EQ(key_only.image_width, 4096U);
  EXPECT_EQ(key_only.image_height, 2731U);
  EXPECT_FALSE(not_cached.status.ok);
  EXPECT_EQ(not_cached.image_width, 0U);
  EXPECT_EQ(not_cached.image_height, 0U);
}

TEST_F(SegmentationClientTest, ImageNotCachedHeaderSetsImageNotCached) {
  const auto result = client_->StartSession(SegmentationRgb8Image{}, "photo-b", kTimeout);

  EXPECT_FALSE(result.status.ok);
  EXPECT_TRUE(result.image_not_cached);
  EXPECT_FALSE(result.from_cache);
  EXPECT_TRUE(result.session_id.empty());
  EXPECT_EQ(result.status.error_code, static_cast<int>(alcedo::ai::AI_ERROR_IMAGE_NOT_CACHED));
  EXPECT_EQ(result.status.error, "the segmentation image is not cached; send its pixels");
}

TEST_F(SegmentationClientTest, FailureHeaderMapsToFailedStatus) {
  const auto status = client_->LoadModels("D:/models/alcedo-ai-mask-v1", "cuda", kTimeout);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(status.status, static_cast<int>(alcedo::ai::AI_STATUS_INVALID_ARGUMENT));
  EXPECT_EQ(status.error_code, static_cast<int>(alcedo::ai::AI_ERROR_INVALID_INPUT));
  EXPECT_EQ(status.error, "segmentation device \"cuda\" is not supported");
}

TEST_F(SegmentationClientTest, UnreachableEndpointReportsTransportError) {
  // Port 1 has no listener on the test machine.
  auto       unreachable = MakeGrpcSegmentationClient("127.0.0.1:1");
  const auto status      = unreachable->UnloadModels(std::chrono::milliseconds(500));

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(status.status, static_cast<int>(alcedo::ai::AI_STATUS_UNSPECIFIED));
  EXPECT_FALSE(status.error.empty());
}

TEST_F(SegmentationClientTest, DecodeRequestCarriesPromptAndMaskInput) {
  SegmentationPrompt prompt;
  prompt.points = {{.x = 0.25F, .y = 0.5F, .label = SegmentationPointLabel::kAdd},
                   {.x = 0.75F, .y = 0.5F, .label = SegmentationPointLabel::kSubtract}};
  prompt.box    = SegmentationBox{.x0 = 0.1F, .y0 = 0.2F, .x1 = 0.9F, .y1 = 0.8F};
  prompt.mask_input.assign(256 * 256, -3.0F);
  prompt.mask_input[17] = 5.0F;

  const auto result     = client_->Decode("session-1", prompt, kTimeout);

  ASSERT_TRUE(result.status.ok) << result.status.error;
  const auto request = service_.DecodeRequest();
  EXPECT_EQ(request.session_id(), "session-1");
  ASSERT_EQ(request.points_size(), 2);
  EXPECT_FLOAT_EQ(request.points(0).x(), 0.25F);
  EXPECT_EQ(request.points(0).label(), alcedo::ai::SEGMENTATION_POINT_LABEL_ADD);
  EXPECT_EQ(request.points(1).label(), alcedo::ai::SEGMENTATION_POINT_LABEL_SUBTRACT);
  ASSERT_TRUE(request.has_box());
  EXPECT_FLOAT_EQ(request.box().x1(), 0.9F);
  ASSERT_EQ(request.mask_input_size(), 256 * 256);
  EXPECT_FLOAT_EQ(request.mask_input(17), 5.0F);

  ASSERT_EQ(result.masks.size(), 3U);
  EXPECT_EQ(result.masks[0].token, 2U);
  EXPECT_EQ(result.masks[2].area, 100U);
  EXPECT_EQ(result.masks[1].logits.size(), 256U * 256U);
  EXPECT_FLOAT_EQ(result.masks[1].logits[0], 3.0F);
  EXPECT_EQ(result.selected_index, 2U);
  EXPECT_EQ(result.logit_size, 256U);
  EXPECT_EQ(result.decoder_ms, 12);
}

TEST_F(SegmentationClientTest, RunTaskCarriesTaskAndPersonIndex) {
  const auto person = client_->RunTask("session-1", SegmentationTask::kPerson, 1U, kTimeout);

  ASSERT_TRUE(person.status.ok) << person.status.error;
  auto request = service_.TaskRequest();
  EXPECT_EQ(request.session_id(), "session-1");
  EXPECT_EQ(request.task(), alcedo::ai::SEGMENTATION_TASK_PERSON);
  ASSERT_TRUE(request.has_person_index());
  EXPECT_EQ(request.person_index(), 1U);

  const auto people =
      client_->RunTask("session-1", SegmentationTask::kPeople, std::nullopt, kTimeout);

  ASSERT_TRUE(people.status.ok) << people.status.error;
  request = service_.TaskRequest();
  EXPECT_EQ(request.task(), alcedo::ai::SEGMENTATION_TASK_PEOPLE);
  EXPECT_FALSE(request.has_person_index());
}

TEST_F(SegmentationClientTest, RunTaskMapsAlphaMaskBoxAndSubjectFraction) {
  const auto result = client_->RunTask("session-1", SegmentationTask::kPerson, 0U, kTimeout);

  ASSERT_TRUE(result.status.ok) << result.status.error;
  EXPECT_TRUE(result.found);
  EXPECT_TRUE(result.message.empty());
  EXPECT_EQ(result.mask_kind, SegmentationTaskMaskKind::kAlpha);
  EXPECT_EQ(result.mask_width, 8U);
  EXPECT_EQ(result.mask_height, 4U);
  ASSERT_EQ(result.alpha_values.size(), 8U * 4U);
  EXPECT_FLOAT_EQ(result.alpha_values[9], 0.75F);
  EXPECT_FLOAT_EQ(result.alpha_values[8], 0.25F);
  EXPECT_TRUE(result.mask_r8.empty());
  EXPECT_FLOAT_EQ(result.mask_box.x0, 0.25F);
  EXPECT_FLOAT_EQ(result.mask_box.y0, 0.1F);
  EXPECT_FLOAT_EQ(result.mask_box.x1, 0.75F);
  EXPECT_FLOAT_EQ(result.mask_box.y1, 0.9F);
  EXPECT_FLOAT_EQ(result.subject_score, 0.93F);
  EXPECT_EQ(result.mask_area, 1U);
  EXPECT_EQ(result.model_ms, 21);
  ASSERT_EQ(result.people.size(), 2U);
  EXPECT_EQ(result.people[1].index, 1U);
  EXPECT_FLOAT_EQ(result.people[1].box.x0, 0.6F);
  EXPECT_FLOAT_EQ(result.people[1].box.y1, 0.9F);
  EXPECT_FLOAT_EQ(result.people[1].score, 0.7F);
  EXPECT_FLOAT_EQ(result.people[0].subject_fraction, 0.6F);
  EXPECT_TRUE(result.people[0].in_subject);
  EXPECT_FLOAT_EQ(result.people[1].subject_fraction, 0.05F);
  EXPECT_FALSE(result.people[1].in_subject);

  const auto people =
      client_->RunTask("session-1", SegmentationTask::kPeople, std::nullopt, kTimeout);
  ASSERT_TRUE(people.status.ok) << people.status.error;
  EXPECT_FLOAT_EQ(people.mask_box.x0, 0.0F);
  EXPECT_FLOAT_EQ(people.mask_box.y1, 1.0F);
}

TEST_F(SegmentationClientTest, RunTaskNotFoundIsSuccessWithMessage) {
  const auto result = client_->RunTask("session-1", SegmentationTask::kSky, std::nullopt, kTimeout);

  ASSERT_TRUE(result.status.ok) << result.status.error;
  EXPECT_EQ(service_.TaskRequest().task(), alcedo::ai::SEGMENTATION_TASK_SKY);
  EXPECT_FALSE(service_.TaskRequest().has_person_index());
  EXPECT_FALSE(result.found);
  EXPECT_EQ(result.message, "No sky found: sky presence 0.312 is below 0.423.");
  EXPECT_EQ(result.mask_kind, SegmentationTaskMaskKind::kNone);
  EXPECT_TRUE(result.mask_r8.empty());
  EXPECT_TRUE(result.alpha_values.empty());
  EXPECT_FLOAT_EQ(result.presence_threshold, 0.423F);
}

TEST_F(SegmentationClientTest, RunTaskMapsSkyHeadAlphaWithWholeImageBox) {
  service_.SetSkyResponse(FakeSkyResponse::kSkyHeadAlpha);

  const auto result = client_->RunTask("session-1", SegmentationTask::kSky, std::nullopt, kTimeout);

  ASSERT_TRUE(result.status.ok) << result.status.error;
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.mask_kind, SegmentationTaskMaskKind::kAlpha);
  EXPECT_EQ(result.mask_width, kSkyGridSide);
  EXPECT_EQ(result.mask_height, kSkyGridSide);
  ASSERT_EQ(result.alpha_values.size(), std::size_t{kSkyGridSide} * kSkyGridSide);
  EXPECT_FLOAT_EQ(result.alpha_values[0], 1.0F);
  EXPECT_FLOAT_EQ(result.alpha_values[kSkyGridSide - 1], 1.0F);
  EXPECT_FLOAT_EQ(result.alpha_values[kSkyGridSide], 0.25F);
  EXPECT_FLOAT_EQ(result.alpha_values.back(), 0.5F);
  EXPECT_TRUE(result.mask_r8.empty());
  EXPECT_FLOAT_EQ(result.mask_box.x0, 0.0F);
  EXPECT_FLOAT_EQ(result.mask_box.y0, 0.0F);
  EXPECT_FLOAT_EQ(result.mask_box.x1, 1.0F);
  EXPECT_FLOAT_EQ(result.mask_box.y1, 1.0F);
  EXPECT_FLOAT_EQ(result.presence, 0.002F);
  EXPECT_FLOAT_EQ(result.presence_threshold, 5e-4F);
  EXPECT_EQ(result.region_count, 0U);
  EXPECT_EQ(result.mask_area, kSkyGridSide + 1);
  EXPECT_EQ(result.model_ms, 54);
}

TEST_F(SegmentationClientTest, RunTaskMapsSkyRegionUnionAsBinaryMask) {
  service_.SetSkyResponse(FakeSkyResponse::kRegionUnion);

  const auto result = client_->RunTask("session-1", SegmentationTask::kSky, std::nullopt, kTimeout);

  ASSERT_TRUE(result.status.ok) << result.status.error;
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.mask_kind, SegmentationTaskMaskKind::kBinary);
  EXPECT_EQ(result.mask_width, 6U);
  EXPECT_EQ(result.mask_height, 4U);
  ASSERT_EQ(result.mask_r8.size(), 6U * 4U);
  EXPECT_EQ(result.mask_r8[5], 255);
  EXPECT_EQ(result.mask_r8[6], 0);
  EXPECT_TRUE(result.alpha_values.empty());
  EXPECT_EQ(result.region_count, 2U);
  EXPECT_FLOAT_EQ(result.presence, 0.8F);
  EXPECT_EQ(result.mask_area, 6U);
}

}  // namespace
}  // namespace alcedo::sidecar_client
