//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sidecar_client/client.hpp"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <QUuid>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ai_common.pb.h"
#include "semantic.grpc.pb.h"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::sidecar_client {
namespace {

auto DeadlineFromNow(std::chrono::milliseconds timeout) -> std::chrono::system_clock::time_point {
  return std::chrono::system_clock::now() + timeout;
}

auto MakeRequestId() -> std::string {
  return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

auto GrpcErrorMessage(const grpc::Status& status) -> std::string {
  if (!status.error_message().empty()) {
    return status.error_message();
  }
  return "gRPC call failed with code " + std::to_string(static_cast<int>(status.error_code()));
}

auto FillAiRequestHeader(alcedo::ai::AiRequestHeader* header, const std::string& request_id,
                         const std::string& task_id, std::chrono::milliseconds timeout,
                         const std::string& trace_id = {}) -> void {
  header->set_request_id(request_id);
  header->set_task_id(task_id);
  header->set_timeout_ms(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
  if (!trace_id.empty()) {
    header->set_trace_id(trace_id);
  }
}

auto MakeChannel(const std::string& endpoint) {
  return grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
}

auto ToRuntimeModelInfo(const semantic::GetModelInfoResponse& response)
    -> AiSidecarRuntimeModelInfo {
  AiSidecarRuntimeModelInfo info;
  info.profile_id                 = response.profile_id();
  info.model_id                   = response.model_id();
  info.revision                   = response.revision();
  info.engine_profile_id          = response.engine_profile_id();
  info.language                   = response.language();
  info.embedding_dimension        = response.embedding_dimension();
  info.native_embedding_dimension = response.native_embedding_dimension();
  info.image_size                 = response.image_size();
  info.embedding_transform        = response.embedding_transform();
  info.provider                   = response.provider();
  info.model_root                 = response.model_root();
  info.prototype_config_hash      = response.prototype_config_hash();
  return info;
}

auto ToRuntimeStatus(const semantic::GetRuntimeStatusResponse& response)
    -> AiSidecarRuntimeRemoteStatus {
  AiSidecarRuntimeRemoteStatus status;
  status.state               = response.state();
  status.provider            = response.provider();
  status.image_batch_cap     = response.image_batch_cap();
  status.image_batch_wait_ms = response.image_batch_wait_ms();
  status.uptime_ms           = response.uptime_ms();
  status.encoders.reserve(static_cast<size_t>(response.encoders_size()));
  for (const auto& encoder : response.encoders()) {
    status.encoders.push_back(AiSidecarEncoderResidency{.encoder = encoder.encoder(),
                                                        .loaded  = encoder.loaded(),
                                                        .device  = encoder.device(),
                                                        .idle_ms = encoder.idle_ms()});
  }
  return status;
}


auto ToAssetInfo(const semantic::ModelAsset& response) -> SemanticModelAssetInfo {
  SemanticModelAssetInfo asset;
  asset.role        = response.role();
  asset.repo_id     = response.repo_id();
  asset.revision    = response.revision();
  asset.remote_path = response.remote_path();
  asset.local_path  = response.local_path();
  asset.size_bytes  = response.size_bytes();
  asset.sha256      = response.sha256();
  return asset;
}

auto ToProfileInfo(const semantic::ModelProfile& response) -> SemanticModelProfileInfo {
  SemanticModelProfileInfo profile;
  profile.profile_id                 = response.profile_id();
  profile.display_name               = response.display_name();
  profile.model_id                   = response.model_id();
  profile.revision                   = response.revision();
  profile.engine_profile_id          = response.engine_profile_id();
  profile.language                   = response.language();
  profile.embedding_dimension        = response.embedding_dimension();
  profile.native_embedding_dimension = response.native_embedding_dimension();
  profile.image_size                 = response.image_size();
  profile.installed                  = response.installed();
  profile.local_root                 = response.local_root();
  profile.status                     = response.status();
  profile.embedding_transform        = response.embedding_transform();
  profile.assets.reserve(static_cast<size_t>(response.assets_size()));
  for (const auto& asset : response.assets()) {
    profile.assets.push_back(ToAssetInfo(asset));
  }
  return profile;
}

auto ToManifest(const semantic::ResolvedModelManifest& response)
    -> SemanticResolvedModelManifest {
  SemanticResolvedModelManifest manifest;
  manifest.profile_id                 = response.profile_id();
  manifest.model_id                   = response.model_id();
  manifest.revision                   = response.revision();
  manifest.engine_profile_id          = response.engine_profile_id();
  manifest.language                   = response.language();
  manifest.embedding_dimension        = response.embedding_dimension();
  manifest.native_embedding_dimension = response.native_embedding_dimension();
  manifest.image_size                 = response.image_size();
  manifest.embedding_transform        = response.embedding_transform();
  manifest.model_root                 = response.model_root();
  manifest.assets.reserve(static_cast<size_t>(response.assets_size()));
  for (const auto& asset : response.assets()) {
    manifest.assets.push_back(ToAssetInfo(asset));
  }
  return manifest;
}

auto ToModelManagerResult(const semantic::ModelManagerResponse& response)
    -> SemanticModelManagerResult {
  SemanticModelManagerResult result;
  result.ok      = response.ok();
  result.status  = response.status();
  result.error   = response.error();
  result.profile = ToProfileInfo(response.profile());
  if (response.has_manifest()) {
    result.manifest = ToManifest(response.manifest());
  }
  return result;
}

auto ToEmbeddingResult(const semantic::EmbeddingResponseV2& response) -> SemanticEmbeddingResult {
  SemanticEmbeddingResult result;
  result.request_id = response.header().request_id();
  result.embedding.assign(response.embedding().begin(), response.embedding().end());
  result.dimension  = response.dimension();
  result.model_name = response.model_name();
  result.elapsed_ms = response.elapsed_ms();
  result.ok         = response.header().status() == alcedo::ai::AI_STATUS_OK;
  result.error      = response.header().error_message();
  return result;
}

auto ToEmbeddingResult(const semantic::EmbeddingBatchItemV2& response) -> SemanticEmbeddingResult {
  SemanticEmbeddingResult result;
  result.request_id = response.request_id();
  result.embedding.assign(response.embedding().begin(), response.embedding().end());
  result.dimension  = response.dimension();
  result.model_name = response.model_name();
  result.elapsed_ms = response.elapsed_ms();
  result.ok         = response.ok();
  result.error      = response.error();
  return result;
}




class GrpcModelManagerClient final : public ModelManagerClient {
 public:
  explicit GrpcModelManagerClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

  auto ListModelProfiles(const std::string& model_root, std::chrono::milliseconds timeout,
                         std::string* error) -> std::vector<SemanticModelProfileInfo> override {
    auto stub = semantic::ModelManagerService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::ListModelProfilesRequest request;
    request.set_model_root(model_root);
    semantic::ListModelProfilesResponse response;
    const auto status = stub->ListModelProfiles(&context, request, &response);
    if (!status.ok()) {
      if (error) *error = GrpcErrorMessage(status);
      return {};
    }
    std::vector<SemanticModelProfileInfo> profiles;
    profiles.reserve(static_cast<size_t>(response.profiles_size()));
    for (const auto& profile : response.profiles()) {
      profiles.push_back(ToProfileInfo(profile));
    }
    return profiles;
  }

  auto ListInstalledModels(const std::string& model_root, std::chrono::milliseconds timeout,
                           std::string* error) -> std::vector<SemanticModelProfileInfo> override {
    auto stub = semantic::ModelManagerService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::ListInstalledModelsRequest request;
    request.set_model_root(model_root);
    semantic::ListInstalledModelsResponse response;
    const auto status = stub->ListInstalledModels(&context, request, &response);
    if (!status.ok()) {
      if (error) *error = GrpcErrorMessage(status);
      return {};
    }
    std::vector<SemanticModelProfileInfo> profiles;
    profiles.reserve(static_cast<size_t>(response.profiles_size()));
    for (const auto& profile : response.profiles()) {
      profiles.push_back(ToProfileInfo(profile));
    }
    return profiles;
  }

  auto ValidateModel(const std::string& profile_id, const std::string& model_root,
                     std::chrono::milliseconds timeout) -> SemanticModelManagerResult override {
    auto stub = semantic::ModelManagerService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::ValidateModelRequest request;
    request.set_profile_id(profile_id);
    request.set_model_root(model_root);
    semantic::ModelManagerResponse response;
    const auto status = stub->ValidateModel(&context, request, &response);
    if (status.ok()) {
      return ToModelManagerResult(response);
    }
    return {.ok = false, .status = "error", .error = GrpcErrorMessage(status)};
  }

  auto DeleteModel(const std::string& profile_id, const std::string& model_root,
                   std::chrono::milliseconds timeout) -> SemanticModelManagerResult override {
    auto stub = semantic::ModelManagerService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::DeleteModelRequest request;
    request.set_profile_id(profile_id);
    request.set_model_root(model_root);
    semantic::ModelManagerResponse response;
    const auto status = stub->DeleteModel(&context, request, &response);
    if (status.ok()) {
      return ToModelManagerResult(response);
    }
    return {.ok = false, .status = "error", .error = GrpcErrorMessage(status)};
  }

 private:
  std::string endpoint_;
};

class GrpcSemanticEmbeddingClient final : public SemanticEmbeddingClient {
 public:
  explicit GrpcSemanticEmbeddingClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

  auto Ping(std::chrono::milliseconds timeout, std::string* error) -> bool override {
    auto stub = semantic::SemanticService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::PingRequest request;
    request.set_request_id("ping");
    semantic::PingResponse response;
    const auto status = stub->Ping(&context, request, &response);
    if (!status.ok()) {
      if (error) *error = GrpcErrorMessage(status);
      return false;
    }
    return true;
  }

  auto GetRuntimeStatus(std::chrono::milliseconds timeout, AiSidecarRuntimeRemoteStatus* status,
                        std::string* error) -> bool override {
    auto stub = semantic::SemanticService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::GetRuntimeStatusRequest request;
    semantic::GetRuntimeStatusResponse response;
    const auto grpc_status = stub->GetRuntimeStatus(&context, request, &response);
    if (!grpc_status.ok()) {
      if (error) *error = GrpcErrorMessage(grpc_status);
      return false;
    }
    if (status) *status = ToRuntimeStatus(response);
    return true;
  }

  auto GetModelInfo(std::chrono::milliseconds timeout, AiSidecarRuntimeModelInfo* info,
                    std::string* error) -> bool override {
    auto stub = semantic::SemanticService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::GetModelInfoRequest request;
    semantic::GetModelInfoResponse response;
    const auto status = stub->GetModelInfo(&context, request, &response);
    if (!status.ok()) {
      if (error) *error = GrpcErrorMessage(status);
      return false;
    }
    if (info) *info = ToRuntimeModelInfo(response);
    return true;
  }

  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override {
    auto stub = semantic::SemanticService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::EmbedTextRequestV2 request;
    FillAiRequestHeader(request.mutable_header(), request_id, "semantic.embed_text", timeout,
                        request_id);
    request.set_text(text);
    semantic::EmbeddingResponseV2 response;
    const auto status = stub->EmbedTextV2(&context, request, &response);
    if (status.ok()) {
      return ToEmbeddingResult(response);
    }
    return {.request_id = request_id, .ok = false, .error = GrpcErrorMessage(status)};
  }

  auto EmbedTextBatch(const std::vector<SemanticTextEmbeddingRequest>& requests,
                      std::chrono::milliseconds timeout)
      -> std::vector<SemanticEmbeddingResult> override {
    auto stub = semantic::SemanticService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    const auto batch_id = MakeRequestId();
    semantic::EmbedTextBatchRequestV2 request;
    FillAiRequestHeader(request.mutable_header(), batch_id, "semantic.embed_text_batch", timeout,
                        batch_id);
    for (const auto& input : requests) {
      auto* item = request.add_items();
      item->set_request_id(input.request_id);
      item->set_text(input.text);
      item->set_model_name(input.model_name);
    }
    semantic::EmbeddingBatchResponseV2 response;
    const auto status = stub->EmbedTextBatchV2(&context, request, &response);
    if (status.ok() && response.header().status() == alcedo::ai::AI_STATUS_OK) {
      std::vector<SemanticEmbeddingResult> results;
      results.reserve(static_cast<size_t>(response.items_size()));
      for (const auto& item : response.items()) {
        results.push_back(ToEmbeddingResult(item));
      }
      return results;
    }
    // A transport failure, or a batch error in the header (for example no active model:
    // AI_ERROR_MODEL_NOT_READY), fails every item with the same reason.
    const std::string error =
        status.ok() ? response.header().error_message() : GrpcErrorMessage(status);
    std::vector<SemanticEmbeddingResult> results;
    results.reserve(requests.size());
    for (const auto& input : requests) {
      results.push_back(
          SemanticEmbeddingResult{.request_id = input.request_id, .ok = false, .error = error});
    }
    return results;
  }

  auto EmbedImage(const std::string& request_id, const std::vector<uint8_t>& rgba8_image,
                  const std::string& format_hint, std::chrono::milliseconds timeout)
      -> SemanticEmbeddingResult override {
    auto stub = semantic::SemanticService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    semantic::EmbedImageRequestV2 request;
    FillAiRequestHeader(request.mutable_header(), request_id, "semantic.embed_image", timeout,
                        request_id);
    request.set_image_bytes(reinterpret_cast<const char*>(rgba8_image.data()), rgba8_image.size());
    request.set_image_format_hint(format_hint);
    semantic::EmbeddingResponseV2 response;
    const auto status = stub->EmbedImageV2(&context, request, &response);
    if (status.ok()) {
      return ToEmbeddingResult(response);
    }
    return {.request_id = request_id, .ok = false, .error = GrpcErrorMessage(status)};
  }

  auto EmbedImageBatch(std::vector<SemanticImageEmbeddingRequest> requests,
                       std::chrono::milliseconds timeout)
      -> std::vector<SemanticEmbeddingResult> override {
    auto stub = semantic::SemanticService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    const auto batch_id = MakeRequestId();
    semantic::EmbedImageBatchRequestV2 request;
    FillAiRequestHeader(request.mutable_header(), batch_id, "semantic.embed_image_batch", timeout,
                        batch_id);
    for (const auto& input : requests) {
      auto* item = request.add_items();
      item->set_request_id(input.request_id);
      item->set_image_bytes(reinterpret_cast<const char*>(input.rgba8_image.data()),
                            input.rgba8_image.size());
      item->set_image_format_hint(input.format_hint);
      item->set_model_name(input.model_name);
    }
    semantic::EmbeddingBatchResponseV2 response;
    const auto status = stub->EmbedImageBatchV2(&context, request, &response);
    if (status.ok() && response.header().status() == alcedo::ai::AI_STATUS_OK) {
      std::vector<SemanticEmbeddingResult> results;
      results.reserve(static_cast<size_t>(response.items_size()));
      for (const auto& item : response.items()) {
        results.push_back(ToEmbeddingResult(item));
      }
      return results;
    }
    // A transport failure, or a batch error in the header (for example no active model:
    // AI_ERROR_MODEL_NOT_READY), fails every item with the same reason.
    const std::string error =
        status.ok() ? response.header().error_message() : GrpcErrorMessage(status);
    std::vector<SemanticEmbeddingResult> results;
    results.reserve(requests.size());
    for (const auto& input : requests) {
      results.push_back(
          SemanticEmbeddingResult{.request_id = input.request_id, .ok = false, .error = error});
    }
    return results;
  }

 private:
  std::string endpoint_;
};


class GrpcClient final : public Client {
 public:
  explicit GrpcClient(std::string endpoint)
      : endpoint_(std::move(endpoint)), models_(endpoint_), semantic_(endpoint_) {}

  auto endpoint() const -> const std::string& override { return endpoint_; }
  auto models() -> ModelManagerClient& override { return models_; }
  auto semantic() -> SemanticEmbeddingClient& override { return semantic_; }

 private:
  std::string                 endpoint_;
  GrpcModelManagerClient      models_;
  GrpcSemanticEmbeddingClient semantic_;
};

}  // namespace

auto MakeGrpcClient(std::string endpoint) -> std::shared_ptr<Client> {
  return std::make_shared<GrpcClient>(std::move(endpoint));
}

}  // namespace alcedo::sidecar_client
