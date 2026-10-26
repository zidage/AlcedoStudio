//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sidecar_client/segmentation_client.hpp"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>

#include <QUuid>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ai_common.pb.h"
#include "segmentation.grpc.pb.h"

namespace alcedo::sidecar_client {
namespace {

/// Send and receive limit of the segmentation channel; a 4096 x 4096 RGB8 image is 50.3 MB.
constexpr int kSegmentationMaxMessageBytes = 64 * 1024 * 1024;

auto DeadlineFromNow(std::chrono::milliseconds timeout) -> std::chrono::system_clock::time_point {
  return std::chrono::system_clock::now() + timeout;
}

void FillHeader(alcedo::ai::AiRequestHeader* header, const char* task_id,
                std::chrono::milliseconds timeout) {
  header->set_request_id(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
  header->set_task_id(task_id);
  header->set_timeout_ms(timeout.count());
}

auto TransportFailure(const grpc::Status& status) -> SegmentationCallStatus {
  SegmentationCallStatus result;
  result.ok = false;
  result.error =
      status.error_message().empty()
          ? "gRPC call failed with code " + std::to_string(static_cast<int>(status.error_code()))
          : status.error_message();
  return result;
}

auto FromHeader(const alcedo::ai::AiResponseHeader& header) -> SegmentationCallStatus {
  SegmentationCallStatus result;
  result.status     = static_cast<int>(header.status());
  result.error_code = static_cast<int>(header.error_code());
  result.ok         = header.status() == alcedo::ai::AI_STATUS_OK;
  result.error      = header.error_message();
  return result;
}

auto ToProtoLabel(SegmentationPointLabel label) -> alcedo::ai::SegmentationPointLabel {
  return label == SegmentationPointLabel::kAdd ? alcedo::ai::SEGMENTATION_POINT_LABEL_ADD
                                               : alcedo::ai::SEGMENTATION_POINT_LABEL_SUBTRACT;
}

auto ToProtoTask(SegmentationTask task) -> alcedo::ai::SegmentationTask {
  switch (task) {
    case SegmentationTask::kSubject:
      return alcedo::ai::SEGMENTATION_TASK_SUBJECT;
    case SegmentationTask::kBackground:
      return alcedo::ai::SEGMENTATION_TASK_BACKGROUND;
    case SegmentationTask::kSky:
      return alcedo::ai::SEGMENTATION_TASK_SKY;
    case SegmentationTask::kPeople:
      return alcedo::ai::SEGMENTATION_TASK_PEOPLE;
    case SegmentationTask::kPerson:
      return alcedo::ai::SEGMENTATION_TASK_PERSON;
  }
  return alcedo::ai::SEGMENTATION_TASK_UNSPECIFIED;
}

auto FromProtoMaskKind(int kind) -> SegmentationTaskMaskKind {
  switch (kind) {
    case alcedo::ai::SEGMENTATION_TASK_MASK_KIND_ALPHA:
      return SegmentationTaskMaskKind::kAlpha;
    default:
      return SegmentationTaskMaskKind::kNone;
  }
}

class GrpcSegmentationClient final : public SegmentationClient {
 public:
  explicit GrpcSegmentationClient(const std::string& endpoint)
      : stub_(alcedo::ai::SegmentationService::NewStub(MakeChannel(endpoint))) {}

  auto LoadModels(const std::string& model_directory, const std::string& device,
                  std::chrono::milliseconds timeout) -> SegmentationCallStatus override {
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::LoadSegmentationModelsRequest request;
    FillHeader(request.mutable_header(), "segmentation.load_models", timeout);
    request.set_model_directory(model_directory);
    request.set_device(device);
    alcedo::ai::LoadSegmentationModelsResponse response;
    const auto status = stub_->LoadSegmentationModels(&context, request, &response);
    return status.ok() ? FromHeader(response.header()) : TransportFailure(status);
  }

  auto UnloadModels(std::chrono::milliseconds timeout) -> SegmentationCallStatus override {
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::UnloadSegmentationModelsRequest request;
    FillHeader(request.mutable_header(), "segmentation.unload_models", timeout);
    alcedo::ai::UnloadSegmentationModelsResponse response;
    const auto status = stub_->UnloadSegmentationModels(&context, request, &response);
    return status.ok() ? FromHeader(response.header()) : TransportFailure(status);
  }

  auto GetStatus(std::chrono::milliseconds timeout) -> SegmentationStatus override {
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::GetSegmentationStatusRequest request;
    FillHeader(request.mutable_header(), "segmentation.status", timeout);
    alcedo::ai::GetSegmentationStatusResponse response;
    const auto         status = stub_->GetSegmentationStatus(&context, request, &response);
    SegmentationStatus result;
    if (!status.ok()) {
      result.status = TransportFailure(status);
      return result;
    }
    result.status          = FromHeader(response.header());
    result.loaded          = response.loaded();
    result.model_set_id    = response.model_set_id();
    result.model_directory = response.model_directory();
    result.device          = response.device();
    result.backend         = response.backend();
    result.session_count      = response.session_count();
    result.cached_image_count = response.cached_image_count();
    result.models.reserve(static_cast<size_t>(response.models_size()));
    for (const auto& model : response.models()) {
      result.models.push_back(SegmentationModelResidency{
          .role = model.role(), .device = model.device(), .load_ms = model.load_ms()});
    }
    return result;
  }

  auto StartSession(const SegmentationRgb8Image& image, const std::string& image_key,
                    std::chrono::milliseconds timeout) -> SegmentationSessionResult override {
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::StartSegmentationSessionRequest request;
    FillHeader(request.mutable_header(), "segmentation.start_session", timeout);
    request.set_width(image.width);
    request.set_height(image.height);
    request.set_rgb8_pixels(reinterpret_cast<const char*>(image.pixels.data()),
                            image.pixels.size());
    request.set_image_key(image_key);
    alcedo::ai::StartSegmentationSessionResponse response;
    const auto status = stub_->StartSegmentationSession(&context, request, &response);
    SegmentationSessionResult result;
    if (!status.ok()) {
      result.status = TransportFailure(status);
      return result;
    }
    result.status     = FromHeader(response.header());
    result.session_id = response.session_id();
    result.encoder_ms = response.encoder_ms();
    result.from_cache = response.from_cache();
    result.image_width  = response.image_width();
    result.image_height = response.image_height();
    result.image_not_cached =
        response.header().error_code() == alcedo::ai::AI_ERROR_IMAGE_NOT_CACHED;
    return result;
  }

  auto Decode(const std::string& session_id, const SegmentationPrompt& prompt,
              std::chrono::milliseconds timeout) -> SegmentationDecodeResult override {
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::DecodeSegmentationPromptRequest request;
    FillHeader(request.mutable_header(), "segmentation.decode_prompt", timeout);
    request.set_session_id(session_id);
    for (const auto& point : prompt.points) {
      auto* proto_point = request.add_points();
      proto_point->set_x(point.x);
      proto_point->set_y(point.y);
      proto_point->set_label(ToProtoLabel(point.label));
    }
    if (prompt.box.has_value()) {
      auto* box = request.mutable_box();
      box->set_x0(prompt.box->x0);
      box->set_y0(prompt.box->y0);
      box->set_x1(prompt.box->x1);
      box->set_y1(prompt.box->y1);
    }
    request.mutable_mask_input()->Assign(prompt.mask_input.begin(), prompt.mask_input.end());
    alcedo::ai::DecodeSegmentationPromptResponse response;
    const auto               status = stub_->DecodeSegmentationPrompt(&context, request, &response);
    SegmentationDecodeResult result;
    if (!status.ok()) {
      result.status = TransportFailure(status);
      return result;
    }
    result.status         = FromHeader(response.header());
    result.selected_index = response.selected_index();
    result.logit_size     = response.logit_size();
    result.decoder_ms     = response.decoder_ms();
    result.masks.reserve(static_cast<size_t>(response.masks_size()));
    for (const auto& level : response.masks()) {
      result.masks.push_back(
          SegmentationMaskLevel{.logits = {level.logits().begin(), level.logits().end()},
                                .score  = level.score(),
                                .token  = level.token(),
                                .area   = level.area()});
    }
    return result;
  }

  auto EndSession(const std::string& session_id, std::chrono::milliseconds timeout)
      -> SegmentationCallStatus override {
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::EndSegmentationSessionRequest request;
    FillHeader(request.mutable_header(), "segmentation.end_session", timeout);
    request.set_session_id(session_id);
    alcedo::ai::EndSegmentationSessionResponse response;
    const auto status = stub_->EndSegmentationSession(&context, request, &response);
    return status.ok() ? FromHeader(response.header()) : TransportFailure(status);
  }

  auto RunTask(const std::string& session_id, SegmentationTask task,
               std::optional<uint32_t> person_index, std::chrono::milliseconds timeout)
      -> SegmentationTaskResult override {
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::RunSegmentationTaskRequest request;
    FillHeader(request.mutable_header(), "segmentation.run_task", timeout);
    request.set_session_id(session_id);
    request.set_task(ToProtoTask(task));
    if (person_index.has_value()) {
      request.set_person_index(*person_index);
    }
    alcedo::ai::RunSegmentationTaskResponse response;
    const auto             status = stub_->RunSegmentationTask(&context, request, &response);
    SegmentationTaskResult result;
    if (!status.ok()) {
      result.status = TransportFailure(status);
      return result;
    }
    result.status             = FromHeader(response.header());
    result.found              = response.found();
    result.message            = response.message();
    result.mask_kind          = FromProtoMaskKind(response.mask_kind());
    result.mask_width         = response.mask_width();
    result.mask_height        = response.mask_height();
    result.alpha_values       = {response.alpha_values().begin(), response.alpha_values().end()};
    result.mask_box           = {.x0 = response.mask_box().x0(),
                                 .y0 = response.mask_box().y0(),
                                 .x1 = response.mask_box().x1(),
                                 .y1 = response.mask_box().y1()};
    result.presence           = response.presence();
    result.presence_threshold = response.presence_threshold();
    result.subject_score      = response.subject_score();
    result.mask_area          = response.mask_area();
    result.model_ms           = response.model_ms();
    result.people.reserve(static_cast<size_t>(response.people_size()));
    for (const auto& person : response.people()) {
      result.people.push_back(SegmentationPerson{.index            = person.index(),
                                                 .box              = {.x0 = person.box().x0(),
                                                                      .y0 = person.box().y0(),
                                                                      .x1 = person.box().x1(),
                                                                      .y1 = person.box().y1()},
                                                 .score            = person.score(),
                                                 .subject_fraction = person.subject_fraction(),
                                                 .in_subject       = person.in_subject()});
    }
    return result;
  }

 private:
  static auto MakeChannel(const std::string& endpoint) -> std::shared_ptr<grpc::Channel> {
    grpc::ChannelArguments arguments;
    arguments.SetMaxReceiveMessageSize(kSegmentationMaxMessageBytes);
    arguments.SetMaxSendMessageSize(kSegmentationMaxMessageBytes);
    return grpc::CreateCustomChannel(endpoint, grpc::InsecureChannelCredentials(), arguments);
  }

  std::unique_ptr<alcedo::ai::SegmentationService::Stub> stub_;
};

}  // namespace

auto MakeGrpcSegmentationClient(std::string endpoint) -> std::unique_ptr<SegmentationClient> {
  return std::make_unique<GrpcSegmentationClient>(endpoint);
}

}  // namespace alcedo::sidecar_client
