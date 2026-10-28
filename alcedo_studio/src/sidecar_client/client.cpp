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
#include "ai_runtime.grpc.pb.h"
#include "image_analysis.grpc.pb.h"
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
                         const std::string& credential_ref = {},
                         const std::string& trace_id = {}) -> void {
  header->set_request_id(request_id);
  header->set_task_id(task_id);
  header->set_timeout_ms(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
  if (!credential_ref.empty()) {
    header->set_credential_ref(credential_ref);
  }
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

auto ToCapability(const alcedo::ai::AiCapability& capability) -> AiSidecarCapability {
  AiSidecarCapability out;
  out.task_id     = capability.task_id();
  out.provider_id = capability.provider_id();
  out.model_id    = capability.model_id();
  out.input_kinds.reserve(static_cast<size_t>(capability.input_kinds_size()));
  for (const auto kind : capability.input_kinds()) {
    out.input_kinds.push_back(static_cast<int>(kind));
  }
  out.output_kinds.reserve(static_cast<size_t>(capability.output_kinds_size()));
  for (const auto kind : capability.output_kinds()) {
    out.output_kinds.push_back(static_cast<int>(kind));
  }
  out.supports_batch      = capability.supports_batch();
  out.supports_cancel     = capability.supports_cancel();
  out.requires_credential = capability.requires_credential();
  out.max_payload_bytes   = capability.max_payload_bytes();
  return out;
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

auto ToRendition(const alcedo::ai::RenditionMetadata& rendition) -> ImageAnalysisRendition {
  ImageAnalysisRendition out;
  out.kind     = rendition.kind();
  out.width    = rendition.width();
  out.height   = rendition.height();
  out.bytes    = rendition.bytes();
  out.max_edge = std::max(rendition.width(), rendition.height());
  return out;
}

auto ToUsage(const alcedo::ai::UsageMetadata& usage) -> ImageAnalysisUsage {
  ImageAnalysisUsage out;
  out.input_tokens  = usage.input_tokens();
  out.output_tokens = usage.output_tokens();
  out.total_tokens  = usage.total_tokens();
  return out;
}

void FillImageAnalysisHeader(alcedo::ai::AiRequestHeader* header, const std::string& task_id,
                             const ImageAnalysisRequest& request,
                             std::chrono::milliseconds timeout) {
  FillAiRequestHeader(header, request.request_id, task_id, timeout, request.credential_ref,
                      request.request_id);
}

void FillRendition(alcedo::ai::RenditionMetadata* proto,
                   const ImageAnalysisRendition& rendition) {
  proto->set_kind(rendition.kind);
  proto->set_width(rendition.width);
  proto->set_height(rendition.height);
  proto->set_bytes(rendition.bytes);
}

auto ToUnderstandingResult(const alcedo::ai::DescribeImageResponse& response,
                           const std::string& fallback_request_id,
                           const std::string& transport_error = {})
    -> ImageAnalysisUnderstandingResult {
  ImageAnalysisUnderstandingResult result;
  const auto& header = response.header();
  result.request_id          = header.request_id().empty() ? fallback_request_id : header.request_id();
  result.status              = static_cast<int>(header.status());
  result.error_code          = static_cast<int>(header.error_code());
  result.error               = transport_error.empty() ? header.error_message() : transport_error;
  result.provider            = header.provider();
  result.model_id            = header.model_id();
  result.elapsed_ms          = static_cast<uint64_t>(header.elapsed_ms());
  result.provider_request_id = response.provider_request_id();
  result.prompt_profile_id   = response.prompt_profile_id();
  result.rendition           = ToRendition(response.rendition());
  if (response.has_usage()) {
    result.usage = ToUsage(response.usage());
  }
  result.ok = header.status() == alcedo::ai::AI_STATUS_OK;
  if (result.ok && response.has_result()) {
    const auto& body = response.result();
    result.caption    = body.caption();
    result.tags.assign(body.tags().begin(), body.tags().end());
    result.scene      = body.scene();
    result.confidence = body.confidence();
  }
  return result;
}

auto ToRatingResult(const alcedo::ai::ScoreImageResponse& response,
                    const std::string& fallback_request_id,
                    const std::string& transport_error = {}) -> ImageAnalysisRatingResult {
  ImageAnalysisRatingResult result;
  const auto& header = response.header();
  result.request_id          = header.request_id().empty() ? fallback_request_id : header.request_id();
  result.status              = static_cast<int>(header.status());
  result.error_code          = static_cast<int>(header.error_code());
  result.error               = transport_error.empty() ? header.error_message() : transport_error;
  result.provider            = header.provider();
  result.model_id            = header.model_id();
  result.elapsed_ms          = static_cast<uint64_t>(header.elapsed_ms());
  result.provider_request_id = response.provider_request_id();
  result.prompt_profile_id   = response.prompt_profile_id();
  result.rendition           = ToRendition(response.rendition());
  if (response.has_usage()) {
    result.usage = ToUsage(response.usage());
  }
  result.ok = header.status() == alcedo::ai::AI_STATUS_OK;
  if (result.ok && response.has_result()) {
    const auto& body = response.result();
    result.rating         = body.rating();
    result.rubric_id      = body.rubric_id();
    result.rubric_version = body.rubric_version();
    result.reasons        = body.reasons();
  }
  return result;
}

auto ToCombinedResult(const alcedo::ai::AnalyzeImageResponse& response,
                      const std::string& fallback_request_id,
                      const std::string& transport_error = {}) -> ImageAnalysisCombinedResult {
  ImageAnalysisCombinedResult result;
  const auto& header = response.header();
  result.request_id          = header.request_id().empty() ? fallback_request_id : header.request_id();
  result.status              = static_cast<int>(header.status());
  result.error_code          = static_cast<int>(header.error_code());
  result.error               = transport_error.empty() ? header.error_message() : transport_error;
  result.provider            = header.provider();
  result.model_id            = header.model_id();
  result.elapsed_ms          = static_cast<uint64_t>(header.elapsed_ms());
  result.provider_request_id = response.provider_request_id();
  result.prompt_profile_id   = response.prompt_profile_id();
  result.rendition           = ToRendition(response.rendition());
  result.has_understanding   = response.has_understanding();
  result.has_rating          = response.has_rating();
  if (response.has_usage()) {
    result.usage = ToUsage(response.usage());
  }
  result.ok = header.status() == alcedo::ai::AI_STATUS_OK;
  if (result.ok && result.has_understanding && response.has_understanding()) {
    const auto& body = response.understanding();
    result.understanding.request_id          = result.request_id;
    result.understanding.ok                  = true;
    result.understanding.status              = result.status;
    result.understanding.error_code          = result.error_code;
    result.understanding.provider            = result.provider;
    result.understanding.model_id            = result.model_id;
    result.understanding.provider_request_id = result.provider_request_id;
    result.understanding.prompt_profile_id   = result.prompt_profile_id;
    result.understanding.rendition           = result.rendition;
    result.understanding.usage               = result.usage;
    result.understanding.elapsed_ms          = result.elapsed_ms;
    result.understanding.caption             = body.caption();
    result.understanding.tags.assign(body.tags().begin(), body.tags().end());
    result.understanding.scene      = body.scene();
    result.understanding.confidence = body.confidence();
  }
  if (result.ok && result.has_rating && response.has_rating()) {
    const auto& body = response.rating();
    result.rating.request_id          = result.request_id;
    result.rating.ok                  = true;
    result.rating.status              = result.status;
    result.rating.error_code          = result.error_code;
    result.rating.provider            = result.provider;
    result.rating.model_id            = result.model_id;
    result.rating.provider_request_id = result.provider_request_id;
    result.rating.prompt_profile_id   = result.prompt_profile_id;
    result.rating.rendition           = result.rendition;
    result.rating.usage               = result.usage;
    result.rating.elapsed_ms          = result.elapsed_ms;
    result.rating.rating              = body.rating();
    result.rating.rubric_id           = body.rubric_id();
    result.rating.rubric_version      = body.rubric_version();
    result.rating.reasons             = body.reasons();
  }
  return result;
}

auto ToCombinedResult(const alcedo::ai::BatchAnalyzeImageItemResponse& response,
                      const std::string& fallback_request_id) -> ImageAnalysisCombinedResult {
  ImageAnalysisCombinedResult result;
  const auto& header = response.header();
  result.request_id          = header.request_id().empty() ? fallback_request_id : header.request_id();
  result.status              = static_cast<int>(header.status());
  result.error_code          = static_cast<int>(header.error_code());
  result.error               = header.error_message();
  result.provider            = header.provider();
  result.model_id            = header.model_id();
  result.elapsed_ms          = static_cast<uint64_t>(header.elapsed_ms());
  result.provider_request_id = response.provider_request_id();
  result.prompt_profile_id   = response.prompt_profile_id();
  result.rendition           = ToRendition(response.rendition());
  result.has_understanding   = response.has_understanding();
  result.has_rating          = response.has_rating();
  if (response.has_usage()) {
    result.usage = ToUsage(response.usage());
  }
  result.ok = header.status() == alcedo::ai::AI_STATUS_OK;
  if (result.ok && result.has_understanding) {
    const auto& body = response.understanding();
    result.understanding.request_id          = result.request_id;
    result.understanding.ok                  = true;
    result.understanding.status              = result.status;
    result.understanding.error_code          = result.error_code;
    result.understanding.provider            = result.provider;
    result.understanding.model_id            = result.model_id;
    result.understanding.provider_request_id = result.provider_request_id;
    result.understanding.prompt_profile_id   = result.prompt_profile_id;
    result.understanding.rendition           = result.rendition;
    result.understanding.usage               = result.usage;
    result.understanding.elapsed_ms          = result.elapsed_ms;
    result.understanding.caption             = body.caption();
    result.understanding.tags.assign(body.tags().begin(), body.tags().end());
    result.understanding.scene      = body.scene();
    result.understanding.confidence = body.confidence();
  }
  if (result.ok && result.has_rating) {
    const auto& body = response.rating();
    result.rating.request_id          = result.request_id;
    result.rating.ok                  = true;
    result.rating.status              = result.status;
    result.rating.error_code          = result.error_code;
    result.rating.provider            = result.provider;
    result.rating.model_id            = result.model_id;
    result.rating.provider_request_id = result.provider_request_id;
    result.rating.prompt_profile_id   = result.prompt_profile_id;
    result.rating.rendition           = result.rendition;
    result.rating.usage               = result.usage;
    result.rating.elapsed_ms          = result.elapsed_ms;
    result.rating.rating              = body.rating();
    result.rating.rubric_id           = body.rubric_id();
    result.rating.rubric_version      = body.rubric_version();
    result.rating.reasons             = body.reasons();
  }
  return result;
}

auto ToListModelsResult(const alcedo::ai::ListModelsResponse& response,
                        const std::string& fallback_request_id,
                        const std::string& transport_error = {}) -> ImageAnalysisListModelsResult {
  ImageAnalysisListModelsResult result;
  const auto& header = response.header();
  result.request_id = header.request_id().empty() ? fallback_request_id : header.request_id();
  result.status     = static_cast<int>(header.status());
  result.error_code = static_cast<int>(header.error_code());
  result.error      = transport_error.empty() ? header.error_message() : transport_error;
  result.provider   = header.provider();
  result.elapsed_ms = static_cast<uint64_t>(header.elapsed_ms());
  result.ok         = header.status() == alcedo::ai::AI_STATUS_OK;
  if (result.ok) {
    result.models.reserve(static_cast<size_t>(response.models_size()));
    for (const auto& model : response.models()) {
      result.models.push_back(AiDiscoveredModel{.model_id           = model.model_id(),
                                                .display_name       = model.display_name(),
                                                .source_provider_id = model.source_provider_id()});
    }
  }
  return result;
}

class GrpcRuntimeControlClient final : public RuntimeControlClient {
 public:
  explicit GrpcRuntimeControlClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

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

  auto ListCapabilities(std::chrono::milliseconds timeout,
                        std::vector<AiSidecarCapability>* capabilities,
                        std::string* error) -> bool override {
    auto stub = alcedo::ai::AiRuntimeService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::ListCapabilitiesRequest request;
    const auto request_id = MakeRequestId();
    FillAiRequestHeader(request.mutable_header(), request_id, "runtime.list_capabilities", timeout,
                        "", request_id);
    alcedo::ai::ListCapabilitiesResponse response;
    const auto status = stub->ListCapabilities(&context, request, &response);
    if (!status.ok()) {
      if (error) *error = GrpcErrorMessage(status);
      return false;
    }
    if (response.header().status() != alcedo::ai::AI_STATUS_OK) {
      if (error) *error = response.header().error_message();
      return false;
    }
    if (capabilities) {
      capabilities->clear();
      capabilities->reserve(static_cast<size_t>(response.capabilities_size()));
      for (const auto& capability : response.capabilities()) {
        capabilities->push_back(ToCapability(capability));
      }
    }
    return true;
  }

  auto CancelTask(const std::string& request_id, std::chrono::milliseconds timeout,
                  bool* cancelled, std::string* error) -> bool override {
    auto stub = alcedo::ai::AiRuntimeService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::CancelTaskRequest request;
    const auto cancel_request_id = MakeRequestId();
    FillAiRequestHeader(request.mutable_header(), cancel_request_id, "runtime.cancel_task", timeout,
                        "", cancel_request_id);
    request.set_request_id(request_id);
    alcedo::ai::CancelTaskResponse response;
    const auto status = stub->CancelTask(&context, request, &response);
    if (!status.ok()) {
      if (error) *error = GrpcErrorMessage(status);
      return false;
    }
    if (response.header().status() != alcedo::ai::AI_STATUS_OK) {
      if (error) *error = response.header().error_message();
      return false;
    }
    if (cancelled) *cancelled = response.cancelled();
    return true;
  }

 private:
  std::string endpoint_;
};

class GrpcCredentialClient final : public CredentialClient {
 public:
  explicit GrpcCredentialClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

  auto RegisterCredential(const std::string& provider_id, const std::string& secret, int64_t ttl_ms,
                          std::chrono::milliseconds timeout, std::string* handle,
                          std::string* error) -> bool override {
    auto stub = alcedo::ai::AiRuntimeService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::RegisterCredentialRequest request;
    const auto request_id = MakeRequestId();
    FillAiRequestHeader(request.mutable_header(), request_id, "credential.register", timeout, "",
                        request_id);
    request.set_provider_id(provider_id);
    request.set_secret(secret);
    request.set_ttl_ms(ttl_ms);
    alcedo::ai::RegisterCredentialResponse response;
    const auto status = stub->RegisterCredential(&context, request, &response);
    if (!status.ok()) {
      if (error) *error = GrpcErrorMessage(status);
      return false;
    }
    if (response.header().status() != alcedo::ai::AI_STATUS_OK) {
      if (error) *error = response.header().error_message();
      return false;
    }
    if (handle) *handle = response.credential_handle();
    return true;
  }

  auto RevokeCredential(const std::string& handle, std::chrono::milliseconds timeout,
                        bool* revoked, std::string* error) -> bool override {
    auto stub = alcedo::ai::AiRuntimeService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::RevokeCredentialRequest request;
    const auto request_id = MakeRequestId();
    FillAiRequestHeader(request.mutable_header(), request_id, "credential.revoke", timeout, "",
                        request_id);
    request.set_credential_handle(handle);
    alcedo::ai::RevokeCredentialResponse response;
    const auto status = stub->RevokeCredential(&context, request, &response);
    if (!status.ok()) {
      if (error) *error = GrpcErrorMessage(status);
      return false;
    }
    if (response.header().status() != alcedo::ai::AI_STATUS_OK) {
      if (error) *error = response.header().error_message();
      return false;
    }
    if (revoked) *revoked = response.revoked();
    return true;
  }

 private:
  std::string endpoint_;
};

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
    FillAiRequestHeader(request.mutable_header(), request_id, "semantic.embed_text", timeout, "",
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
                        "", batch_id);
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
    FillAiRequestHeader(request.mutable_header(), request_id, "semantic.embed_image", timeout, "",
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
                        "", batch_id);
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

class GrpcImageAnalysisClient final : public ImageAnalysisClient {
 public:
  explicit GrpcImageAnalysisClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

  auto DescribeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisUnderstandingResult override {
    auto stub = alcedo::ai::ImageAnalysisService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::DescribeImageRequest req;
    FillImageAnalysisHeader(req.mutable_header(), "image_understanding.describe", request,
                            timeout);
    req.set_image_bytes(reinterpret_cast<const char*>(request.image_bytes.data()),
                        request.image_bytes.size());
    req.set_image_format_hint(request.image_format_hint);
    FillRendition(req.mutable_rendition(), request.rendition);
    req.set_provider_id(request.provider_id);
    req.set_model_id(request.model_id);
    req.set_prompt_profile_id(request.prompt_profile_id);
    req.set_output_language(request.output_language);
    alcedo::ai::DescribeImageResponse response;
    const auto status = stub->DescribeImage(&context, req, &response);
    if (status.ok()) {
      return ToUnderstandingResult(response, request.request_id);
    }
    ImageAnalysisUnderstandingResult result;
    result.request_id = request.request_id;
    result.ok         = false;
    result.status     = static_cast<int>(alcedo::ai::AI_STATUS_UNIMPLEMENTED);
    result.error      = GrpcErrorMessage(status);
    return result;
  }

  auto ScoreImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisRatingResult override {
    auto stub = alcedo::ai::ImageAnalysisService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::ScoreImageRequest req;
    FillImageAnalysisHeader(req.mutable_header(), "image_rating.score", request, timeout);
    req.set_image_bytes(reinterpret_cast<const char*>(request.image_bytes.data()),
                        request.image_bytes.size());
    req.set_image_format_hint(request.image_format_hint);
    FillRendition(req.mutable_rendition(), request.rendition);
    req.set_provider_id(request.provider_id);
    req.set_model_id(request.model_id);
    req.set_prompt_profile_id(request.prompt_profile_id);
    req.set_rubric_id(request.rubric_id);
    req.set_output_language(request.output_language);
    req.set_rating_severity(request.rating_severity);
    req.set_camera_context(request.camera_context);
    req.set_include_rating_reasons(request.include_rating_reasons);
    alcedo::ai::ScoreImageResponse response;
    const auto status = stub->ScoreImage(&context, req, &response);
    if (status.ok()) {
      return ToRatingResult(response, request.request_id);
    }
    ImageAnalysisRatingResult result;
    result.request_id = request.request_id;
    result.ok         = false;
    result.status     = static_cast<int>(alcedo::ai::AI_STATUS_UNIMPLEMENTED);
    result.error      = GrpcErrorMessage(status);
    return result;
  }

  auto AnalyzeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisCombinedResult override {
    auto stub = alcedo::ai::ImageAnalysisService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::AnalyzeImageRequest req;
    FillImageAnalysisHeader(req.mutable_header(), "image_analysis.analyze", request, timeout);
    req.set_image_bytes(reinterpret_cast<const char*>(request.image_bytes.data()),
                        request.image_bytes.size());
    req.set_image_format_hint(request.image_format_hint);
    FillRendition(req.mutable_rendition(), request.rendition);
    req.set_provider_id(request.provider_id);
    req.set_model_id(request.model_id);
    req.set_prompt_profile_id(request.prompt_profile_id);
    req.set_include_understanding(request.include_understanding);
    req.set_include_rating(request.include_rating);
    req.set_include_rating_reasons(request.include_rating_reasons);
    req.set_rubric_id(request.rubric_id);
    req.set_output_language(request.output_language);
    req.set_rating_severity(request.rating_severity);
    req.set_camera_context(request.camera_context);
    alcedo::ai::AnalyzeImageResponse response;
    const auto status = stub->AnalyzeImage(&context, req, &response);
    if (status.ok()) {
      return ToCombinedResult(response, request.request_id);
    }
    ImageAnalysisCombinedResult result;
    result.request_id = request.request_id;
    result.ok         = false;
    result.status     = static_cast<int>(alcedo::ai::AI_STATUS_UNIMPLEMENTED);
    result.error      = GrpcErrorMessage(status);
    return result;
  }

  auto BatchAnalyzeImage(const std::vector<ImageAnalysisRequest>& requests,
                         std::chrono::milliseconds timeout)
      -> std::vector<ImageAnalysisCombinedResult> override {
    if (requests.empty()) {
      return {};
    }
    auto stub = alcedo::ai::ImageAnalysisService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::BatchAnalyzeImageRequest req;
    const auto batch_id = MakeRequestId();
    FillAiRequestHeader(req.mutable_header(), batch_id, "image_analysis.batch_analyze", timeout,
                        requests.front().credential_ref, batch_id);
    req.set_provider_id(requests.front().provider_id);
    req.set_model_id(requests.front().model_id);
    req.set_prompt_profile_id(requests.front().prompt_profile_id);
    req.set_include_understanding(requests.front().include_understanding);
    req.set_include_rating(requests.front().include_rating);
    req.set_include_rating_reasons(requests.front().include_rating_reasons);
    req.set_rubric_id(requests.front().rubric_id);
    req.set_output_language(requests.front().output_language);
    req.set_rating_severity(requests.front().rating_severity);
    for (const auto& request : requests) {
      auto* item = req.add_items();
      item->set_request_id(request.request_id);
      item->set_image_bytes(reinterpret_cast<const char*>(request.image_bytes.data()),
                            request.image_bytes.size());
      item->set_image_format_hint(request.image_format_hint);
      FillRendition(item->mutable_rendition(), request.rendition);
      item->set_camera_context(request.camera_context);
    }
    alcedo::ai::BatchAnalyzeImageResponse response;
    const auto status = stub->BatchAnalyzeImage(&context, req, &response);
    if (status.ok()) {
      std::vector<ImageAnalysisCombinedResult> results;
      results.reserve(static_cast<size_t>(response.items_size()));
      for (int i = 0; i < response.items_size(); ++i) {
        const auto fallback =
            static_cast<size_t>(i) < requests.size() ? requests[static_cast<size_t>(i)].request_id
                                                     : std::string{};
        results.push_back(ToCombinedResult(response.items(i), fallback));
      }
      return results;
    }
    std::vector<ImageAnalysisCombinedResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      ImageAnalysisCombinedResult result;
      result.request_id = request.request_id;
      result.ok         = false;
      result.status     = static_cast<int>(alcedo::ai::AI_STATUS_UNIMPLEMENTED);
      result.error      = GrpcErrorMessage(status);
      results.push_back(std::move(result));
    }
    return results;
  }

  auto ListModels(const std::string& provider_id, const std::string& credential_ref,
                  std::chrono::milliseconds timeout) -> ImageAnalysisListModelsResult override {
    auto stub = alcedo::ai::ImageAnalysisService::NewStub(MakeChannel(endpoint_));
    grpc::ClientContext context;
    context.set_deadline(DeadlineFromNow(timeout));
    alcedo::ai::ListModelsRequest req;
    const auto request_id = MakeRequestId();
    FillAiRequestHeader(req.mutable_header(), request_id, "image_analysis.list_models", timeout,
                        credential_ref, request_id);
    req.set_provider_id(provider_id);
    alcedo::ai::ListModelsResponse response;
    const auto status = stub->ListModels(&context, req, &response);
    if (status.ok()) {
      return ToListModelsResult(response, request_id);
    }
    ImageAnalysisListModelsResult result;
    result.request_id = request_id;
    result.ok         = false;
    result.status     = static_cast<int>(alcedo::ai::AI_STATUS_UNIMPLEMENTED);
    result.error      = GrpcErrorMessage(status);
    return result;
  }

 private:
  std::string endpoint_;
};

class GrpcClient final : public Client {
 public:
  explicit GrpcClient(std::string endpoint)
      : endpoint_(std::move(endpoint)),
        runtime_(endpoint_),
        credentials_(endpoint_),
        models_(endpoint_),
        semantic_(endpoint_),
        image_analysis_(endpoint_) {}

  auto endpoint() const -> const std::string& override { return endpoint_; }
  auto runtime() -> RuntimeControlClient& override { return runtime_; }
  auto credentials() -> CredentialClient& override { return credentials_; }
  auto models() -> ModelManagerClient& override { return models_; }
  auto semantic() -> SemanticEmbeddingClient& override { return semantic_; }
  auto image_analysis() -> ImageAnalysisClient& override { return image_analysis_; }

 private:
  std::string                 endpoint_;
  GrpcRuntimeControlClient    runtime_;
  GrpcCredentialClient        credentials_;
  GrpcModelManagerClient      models_;
  GrpcSemanticEmbeddingClient semantic_;
  GrpcImageAnalysisClient     image_analysis_;
};

}  // namespace

auto MakeGrpcClient(std::string endpoint) -> std::shared_ptr<Client> {
  return std::make_shared<GrpcClient>(std::move(endpoint));
}

}  // namespace alcedo::sidecar_client
