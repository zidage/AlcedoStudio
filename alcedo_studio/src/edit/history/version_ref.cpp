//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/version_ref.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "edit/history/edit_commit.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

auto NowTime() -> std::time_t { return std::chrono::system_clock::to_time_t(TimeProvider::Now()); }

auto NewVersionRefId(sl_element_id_t element_id, std::time_t created_at, std::uint64_t nonce)
    -> version_ref_id_t {
  auto h = Hash128::Compute(&element_id, sizeof(element_id));
  h      = Hash128::Blend(h, Hash128::Compute(&created_at, sizeof(created_at)));
  h      = Hash128::Blend(h, Hash128::Compute(&nonce, sizeof(nonce)));
  return h;
}

auto NewRootId(sl_element_id_t element_id, std::time_t created_at, std::uint64_t nonce)
    -> root_id_t {
  auto h = Hash128::Compute(&element_id, sizeof(element_id));
  // Domain-separate root ids from version ref ids.
  const char domain[] = "alcedo.root_id.v1";
  h                   = Hash128::Blend(h, Hash128::Compute(domain, sizeof(domain) - 1));
  h                   = Hash128::Blend(h, Hash128::Compute(&created_at, sizeof(created_at)));
  h                   = Hash128::Blend(h, Hash128::Compute(&nonce, sizeof(nonce)));
  return h;
}

auto NewNonce() -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

auto MakeDefaultVersionRef(sl_element_id_t element_id, std::string default_display_name,
                           std::time_t now, std::uint64_t nonce) -> VersionRef {
  VersionRef default_ref;
  default_ref.version_id       = NewVersionRefId(element_id, now, nonce);
  default_ref.element_id       = element_id;
  default_ref.display_name     = std::move(default_display_name);
  default_ref.head_commit_hash = std::nullopt;
  default_ref.created_at       = now;
  default_ref.updated_at       = now;
  return default_ref;
}

auto MakeImageEditState(sl_element_id_t element_id, root_id_t root_id,
                        const version_ref_id_t& active_version_id) -> ImageEditState {
  ImageEditState state;
  state.element_id                          = element_id;
  state.root_id                             = root_id;
  state.active_version_id                   = active_version_id;
  state.materialized_head_commit_hash       = std::nullopt;
  state.materialized_transaction_chain_hash = ComputeRootChainHash(root_id);
  state.serialized_pipeline_state           = std::nullopt;
  state.project_schema_version              = kImageEditSchemaVersion;
  return state;
}

}  // namespace

auto VersionRef::ToJSON() const -> nlohmann::json {
  nlohmann::json j;
  j["version_id"]        = version_id.ToString();
  j["element_id"]        = element_id;
  j["display_name"]      = display_name;
  j["head_commit_hash"]  = HeadCommitHashToStorage(head_commit_hash);
  j["created_at"]        = created_at;
  j["updated_at"]        = updated_at;
  return j;
}

auto VersionRef::FromJSON(const nlohmann::json& j) -> VersionRef {
  VersionRef ref;
  ref.version_id       = Hash128::FromString(j.at("version_id").get<std::string>());
  ref.element_id       = j.at("element_id").get<sl_element_id_t>();
  ref.display_name     = j.at("display_name").get<std::string>();
  ref.head_commit_hash = HeadCommitHashFromStorage(j.value("head_commit_hash", std::string{}));
  ref.created_at       = j.at("created_at").get<std::time_t>();
  ref.updated_at       = j.at("updated_at").get<std::time_t>();
  return ref;
}

auto ImageEditState::ToJSON() const -> nlohmann::json {
  nlohmann::json j;
  j["element_id"]                         = element_id;
  j["root_id"]                            = root_id.ToString();
  j["active_version_id"]                  = active_version_id.ToString();
  j["materialized_head_commit_hash"]      = HeadCommitHashToStorage(materialized_head_commit_hash);
  j["materialized_transaction_chain_hash"] = materialized_transaction_chain_hash.ToString();
  j["project_schema_version"]             = project_schema_version;
  if (serialized_pipeline_state.has_value()) {
    j["serialized_pipeline_state"] = *serialized_pipeline_state;
  }
  return j;
}

auto ImageEditState::FromJSON(const nlohmann::json& j) -> ImageEditState {
  ImageEditState state;
  state.element_id = j.at("element_id").get<sl_element_id_t>();
  state.root_id    = Hash128::FromString(j.at("root_id").get<std::string>());
  state.active_version_id =
      Hash128::FromString(j.at("active_version_id").get<std::string>());
  state.materialized_head_commit_hash =
      HeadCommitHashFromStorage(j.value("materialized_head_commit_hash", std::string{}));
  state.materialized_transaction_chain_hash =
      Hash128::FromString(j.at("materialized_transaction_chain_hash").get<std::string>());
  state.project_schema_version = j.at("project_schema_version").get<std::uint32_t>();
  if (state.project_schema_version != kImageEditSchemaVersion) {
    throw std::runtime_error("ImageEditState: unsupported project_schema_version");
  }
  if (j.contains("serialized_pipeline_state")) {
    state.serialized_pipeline_state = j.at("serialized_pipeline_state");
  }
  return state;
}

auto CreateEmptyImageEditState(sl_element_id_t element_id, std::string default_display_name)
    -> std::pair<ImageEditState, VersionRef> {
  const auto now   = NowTime();
  const auto nonce = NewNonce();
  auto       default_ref =
      MakeDefaultVersionRef(element_id, std::move(default_display_name), now, nonce);
  auto state =
      MakeImageEditState(element_id, NewRootId(element_id, now, nonce), default_ref.version_id);
  return {std::move(state), std::move(default_ref)};
}

auto CreateImageEditStateWithRoot(sl_element_id_t element_id, root_id_t root_id,
                                  std::string default_display_name)
    -> std::pair<ImageEditState, VersionRef> {
  const auto now   = NowTime();
  const auto nonce = NewNonce();
  auto       default_ref =
      MakeDefaultVersionRef(element_id, std::move(default_display_name), now, nonce);
  auto state = MakeImageEditState(element_id, root_id, default_ref.version_id);
  return {std::move(state), std::move(default_ref)};
}

void MoveVersionRefHead(VersionRef& version_ref, head_commit_hash_t new_head,
                        std::time_t updated_at) {
  version_ref.head_commit_hash = std::move(new_head);
  version_ref.updated_at       = updated_at != 0 ? updated_at : NowTime();
}

}  // namespace alcedo
