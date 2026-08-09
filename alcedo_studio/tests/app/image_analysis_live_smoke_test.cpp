//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Env-gated live smoke for the Phase 5d/5e/5f image-analysis module. Opens a real packed `.alcd`
// project, materializes one k1024 thumbnail, starts the real Rust sidecar (no CLIP model —
// `describe`/`score` use the HTTP provider path), registers a real provider credential read from
// `.env.test`, and asks the live LLM to describe AND score the image through `ImageAnalysisService`.
//
// Phase 5f extension: the live describe result is persisted through `AiStore` (the
// duckorm ORM layer — no raw SQL) and round-tripped back, then a caption token is shown to be
// searchable via `SleeveFilterService` (the search-document AI-understanding contribution) only
// after persistence. A second test scores the image live, persists the 1..=5 integer rating
// (Phase 5f rating contract: integer 1..=5, no confidence), round-trips it, and proves the
// rating `reasons` are NOT part of full-text search.
//
// Skipped unless ALL of these are set:
//   ALCEDO_IA_LIVE_RUNTIME_PATH     - absolute path to alcedo_mind(.exe)
//   ALCEDO_TEST_PACKED_PROJECT_PATH - absolute path to a packed .alcd project
//   ALCEDO_IA_LIVE_ENV_TEST_PATH    - absolute path to rust/puerh_mind/.env.test
// Optional:
//   ALCEDO_IA_LIVE_PROVIDER_ID      - "openrouter" (default) or "volcengine_ark" / "volcengine_ark_coding"
//
// The API key is read from `.env.test` (ALCEDO_OPENROUTER_API_KEY / ALCEDO_VOLCENGINE_ARK_API_KEY)
// and is never printed, logged, or written to settings. The image itself is never inspected; the
// caption is printed to stdout only (NOT recorded in the plan doc).

#include "ai/ai_description.hpp"
#include "ai/ai_rating.hpp"
#include "app/ai_sidecar_runtime_service.hpp"
#include "app/album_browse_service.hpp"
#include "app/image_analysis_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_package_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "app/thumbnail_service.hpp"
#include "app/thumbnail_types.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/ai/ai_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QString>

namespace alcedo {
namespace {

// Minimal .env reader: returns the value for `KEY=VALUE` lines, ignoring blanks / `#`
// comments, a leading `export `, and surrounding quotes. Used only to read the API key the
// user pre-placed in .env.test; the value never leaves this test except into RegisterCredential.
auto EnvFileValue(const std::filesystem::path& path, const std::string& key) -> std::string {
  std::ifstream in(path);
  if (!in.is_open()) {
    return {};
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    auto key_pos = first;
    if (line.compare(first, 7, "export ") == 0) {
      key_pos = first + 7;
    }
    if (line.compare(key_pos, key.size(), key) != 0) {
      continue;
    }
    if (line.size() <= key_pos + key.size() || line[key_pos + key.size()] != '=') {
      continue;
    }
    std::string v = line.substr(key_pos + key.size() + 1);
    const auto a = v.find_first_not_of(" \t");
    const auto b = v.find_last_not_of(" \t");
    if (a == std::string::npos) {
      return {};
    }
    v = v.substr(a, b - a + 1);
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                          (v.front() == '\'' && v.back() == '\''))) {
      v = v.substr(1, v.size() - 2);
    }
    return v;
  }
  return {};
}

auto EnvOr(const char* name, const char* fallback) -> std::string {
  const char* v = std::getenv(name);
  return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string(fallback);
}

// A pure-ASCII-alpha token of length >= 3, lowercased; returns "" if the input has any
// non-[a-zA-Z] character or is too short. The folded search path strips separators and
// lowercases, so a clean alpha token matches deterministically on both the query and the
// folded document. Tokens containing digits/hyphens/non-ASCII are rejected so the search
// assertion can never false-fail on folding edge cases.
auto NormalizeToken(const std::string& raw) -> std::string {
  if (raw.size() < 3) {
    return {};
  }
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    if (!std::isalpha(static_cast<unsigned char>(c))) {
      return {};
    }
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

auto SplitWords(const std::string& text) -> std::vector<std::string> {
  std::vector<std::string> words;
  std::string              cur;
  for (char c : text) {
    if (std::isalpha(static_cast<unsigned char>(c))) {
      cur.push_back(c);
    } else {
      if (!cur.empty()) {
        words.push_back(cur);
        cur.clear();
      }
    }
  }
  if (!cur.empty()) {
    words.push_back(cur);
  }
  return words;
}

// Collect every usable search token, preferring tags (distinctive nouns), then caption words,
// then scene words — in priority order, deduped. Each token is a clean lowercase ASCII-alpha
// string of length >= 3 (see NormalizeToken). Used to find a token that isolates the AI-caption
// contribution to search (one not already matching the file's filename/metadata).
auto CollectSearchTokens(const std::string& caption, const std::vector<std::string>& tags,
                         const std::string& scene) -> std::vector<std::string> {
  std::vector<std::string> out;
  auto                     push = [&](const std::string& raw) {
    const auto n = NormalizeToken(raw);
    if (n.empty()) {
      return;
    }
    if (std::find(out.begin(), out.end(), n) == out.end()) {
      out.push_back(n);
    }
  };
  for (const auto& t : tags) {
    push(t);
  }
  for (const auto& w : SplitWords(caption)) {
    push(w);
  }
  for (const auto& w : SplitWords(scene)) {
    push(w);
  }
  return out;
}

auto ToWideAscii(const std::string& token) -> std::wstring {
  return {token.begin(), token.end()};
}

// True if `element_id` appears in the search results for `token` within folder 0 (root, where
// the analyzed image lives). A fresh SleeveFilterService is constructed per call so no in-memory
// result cache can stale the before/after comparison across persistence.
auto SearchReturnsFile(const std::shared_ptr<Storage>& storage, const std::string& token,
                       sl_element_id_t element_id) -> bool {
  if (token.empty()) {
    return false;
  }
  SleeveFilterService filter(storage);
  const auto          matches = filter.SearchFolder(0, ToWideAscii(token), 0, 64);
  return std::any_of(matches.begin(), matches.end(),
                     [element_id](const FuzzySearchMatch& m) { return m.file_id_ == element_id; });
}

// Walk `candidates` in order and return the first that does NOT currently match `element_id`
// in search — i.e. a token not already in the file's filename/metadata, so any post-persistence
// hit is attributable to the AI annotation. Call this BEFORE persisting the AI row (when the AI
// table is empty, only filename/metadata can match). Returns "" if every candidate already
// matches (then the attribution cannot be isolated and the caller skips the search sub-check
// rather than false-failing on a filename coincidence).
auto PickIsolatingToken(const std::shared_ptr<Storage>& storage,
                        const std::vector<std::string>& candidates, sl_element_id_t element_id)
    -> std::string {
  for (const auto& tok : candidates) {
    if (!SearchReturnsFile(storage, tok, element_id)) {
      return tok;
    }
  }
  return {};
}

// Map a live describe result onto the Phase 5f domain object. The foreign key is file_id
// (= element id = the inode the CLIP embeddings already bind to), NOT image_id.
auto BuildAiDescription(const ImageAnalysisUnderstandingResult& u, sl_element_id_t element_id,
                        const std::string& task_id) -> AiDescription {
  AiDescription d;
  d.file_id_           = element_id;
  d.task_id_           = task_id;
  d.provider_id_       = u.provider;
  d.model_id_          = u.model_id;
  d.prompt_profile_id_ = u.prompt_profile_id;
  d.rendition_kind_    = u.rendition.kind;
  d.caption_           = u.caption;
  d.scene_             = u.scene;
  d.confidence_        = u.confidence;
  d.active_            = true;
  d.SetTags(u.tags);
  return d;
}

auto BuildAiRating(const ImageAnalysisRatingResult& r, sl_element_id_t element_id,
                   const std::string& task_id) -> AiRating {
  AiRating a;
  a.file_id_           = element_id;
  a.task_id_           = task_id;
  a.provider_id_       = r.provider;
  a.model_id_          = r.model_id;
  a.prompt_profile_id_ = r.prompt_profile_id;
  a.rendition_kind_    = r.rendition.kind;
  a.rating_            = r.rating;  // 1..=5 on success; AiRating::IsValid() rejects 0 (unset)
  a.rubric_id_         = r.rubric_id;
  a.rubric_version_    = r.rubric_version;
  a.reasons_           = r.reasons;
  a.active_            = true;
  return a;
}

// Owns every resource for one live run (project, thumbnail stack, sidecar, wired analysis seams)
// plus the temp paths that must be cleaned up. Destroyed in reverse order; the sidecar is
// stopped in the dtor so a test that returns early still tears the process down.
struct LiveSmokeEnv {
  std::filesystem::path                        workspace_dir;
  std::filesystem::path                        db_path;
  std::filesystem::path                        meta_path;
  std::filesystem::path                        thumbnail_cache_root;
  std::filesystem::path                        model_root;
  std::filesystem::path                        enc_temp_dir;
  std::shared_ptr<PipelineMgmtService>         pipeline_service;
  std::shared_ptr<ThumbnailService>           thumbnail_service;
  std::shared_ptr<AiSidecarRuntimeService>     runtime;
  std::shared_ptr<ThumbnailServiceImageAnalysisProvider> thumb_provider;
  std::shared_ptr<AiSidecarRuntimeImageAnalysisClient>    analysis_client;
  std::shared_ptr<ImageAnalysisInFlightGate>             gate;
  std::unique_ptr<ProjectService>                        project;
  ImageAnalysisItem                                      view_item{};  // the analyzed image

  ~LiveSmokeEnv() {
    if (runtime) {
      runtime->Stop();
    }
    std::error_code ec;
    std::filesystem::remove_all(workspace_dir, ec);
    std::filesystem::remove_all(thumbnail_cache_root, ec);
    std::filesystem::remove_all(model_root, ec);
    std::filesystem::remove_all(enc_temp_dir, ec);
  }
};

// Builds the full live environment. Returns null on failure with `*err` set; the caller decides
// whether that is a skip (missing env vars are checked before calling this) or a FAIL. No
// ASSERT/GTEST_* inside — those macros `return;` void and cannot be used in a value-returning
// helper.
auto BuildLiveSmokeEnv(const char* runtime_env, const char* project_env,
                       std::string* err) -> std::unique_ptr<LiveSmokeEnv> {
  auto env = std::make_unique<LiveSmokeEnv>();

  // (1) Unpack the packed .alcd into a temp workspace.
  ProjectPackageService package_service;
  if (!package_service.IsPackedProjectPath(project_env)) {
    *err = std::string("not a packed .alcd project: ") + project_env;
    return nullptr;
  }
  QString workspace_err;
  if (!package_service.CreateProjectWorkspace(QString::fromUtf8("ia_live_smoke"),
                                              &env->workspace_dir, &workspace_err)) {
    *err = workspace_err.toStdString();
    return nullptr;
  }
  QString unpack_err;
  if (!package_service.UnpackProjectToWorkspace(project_env, env->workspace_dir,
                                                QString::fromUtf8("ia_live_smoke"), &env->db_path,
                                                &env->meta_path, &unpack_err)) {
    *err = unpack_err.toStdString();
    return nullptr;
  }

  // (2) Open the unpacked project and enumerate one image from the root folder.
  env->project = std::make_unique<ProjectService>(env->db_path, env->meta_path,
                                                  ProjectOpenMode::kLoadExisting);
  auto browse = env->project->GetAlbumBrowseService();
  if (browse == nullptr) {
    *err = "AlbumBrowseService unavailable";
    return nullptr;
  }
  const auto files = browse->ListFilesInFolderById(0);  // root
  if (files.empty()) {
    *err = "no image files in the packed project";
    return nullptr;
  }
  const auto view = files.front();
  if (view.image_id_ == 0u) {
    *err = "first entry is not a real image";
    return nullptr;
  }
  env->view_item = ImageAnalysisItem{view.element_id_, view.image_id_};

  // (3) Thumbnail materialization stack (mirrors thumbnail_service_test.cpp:753-755).
  env->thumbnail_cache_root =
      std::filesystem::temp_directory_path() / "alcedo_ia_live_smoke_thumbcache";
  std::filesystem::create_directories(env->thumbnail_cache_root);
  env->pipeline_service = std::make_shared<PipelineMgmtService>(env->project->GetStorage());
  env->thumbnail_service = std::make_shared<ThumbnailService>(
      env->project->GetSleeveService(), env->project->GetImagePoolService(), env->pipeline_service,
      nullptr, env->project->GetProjectUUID(), env->thumbnail_cache_root);

  // (4) Start the real sidecar without a CLIP model: `describe`/`score` are served over the
  // HTTP provider path, so require_model_info=false is sufficient.
  env->runtime = std::make_shared<AiSidecarRuntimeService>();
  AiSidecarRuntimeOptions options;
  options.runtime_binary        = std::filesystem::path(runtime_env);
  options.model_root            = std::filesystem::temp_directory_path() / "alcedo_ia_live_smoke_modelroot";
  std::filesystem::create_directories(options.model_root);
  env->model_root                = options.model_root;
  options.model_id              = "plhery/mobileclip2-onnx:s2";
  options.device                 = "cpu";
  options.batch_cap             = 8;
  options.batch_wait_ms         = 2;
  options.max_message_bytes     = 16 * 1024 * 1024;
  options.allow_download        = false;
  options.require_model_info    = false;
  options.startup_timeout       = std::chrono::milliseconds(120000);
  options.health_poll_interval  = std::chrono::milliseconds(100);
  options.graceful_stop_timeout = std::chrono::milliseconds(2000);
  options.kill_timeout          = std::chrono::milliseconds(3000);
  if (!env->runtime->StartAndWait(options)) {
    *err = env->runtime->Status().message;
    return nullptr;
  }

  // (5) Wire the image-analysis module: real thumbnail provider + real sidecar client + gate.
  env->thumb_provider   = std::make_shared<ThumbnailServiceImageAnalysisProvider>(env->thumbnail_service);
  env->analysis_client  = std::make_shared<AiSidecarRuntimeImageAnalysisClient>(env->runtime);
  env->gate             = std::make_shared<ImageAnalysisInFlightGate>();
  return env;
}

// Shared options for one analysis run. `task` selects describe vs score.
auto MakeAnalysisOptions(ImageAnalysisTask task, const std::string& provider_id,
                         const std::string& api_key, const std::filesystem::path& enc_temp_dir)
    -> ImageAnalysisOptions {
  ImageAnalysisOptions opts;
  opts.task                   = task;
  opts.thumbnail_resolution   = ThumbnailResolution::k1024;
  opts.jpeg_quality           = 90;
  opts.timeout                = std::chrono::milliseconds(120000);
  opts.provider_id            = provider_id;
  opts.credential.provider_id = provider_id;
  opts.credential.secret      = api_key;  // consumed by RegisterCredential, then cleared
  opts.temp_dir               = enc_temp_dir;
  opts.prefetch               = 1;
  return opts;
}

// Reads the three required env vars plus the provider id and the provider's API key (from
// .env.test, falling back to the process env). Returns std::nullopt when the env vars are not
// set — the caller GTEST_SKIPs in that case (this is a smoke test; CI without creds skips).
// The API key is read here so the caller can ASSERT it non-empty in the test body itself
// (ASSERT_* in a helper would return from the helper, not the test). The key value is never
// logged.
struct LiveSmokeInputs {
  const char* runtime_env;
  const char* project_env;
  const char* envtest_env;
  std::string provider_id;
  std::string api_key;
};

auto ReadLiveSmokeInputs() -> std::optional<LiveSmokeInputs> {
  const char* runtime_env = std::getenv("ALCEDO_IA_LIVE_RUNTIME_PATH");
  const char* project_env = std::getenv("ALCEDO_TEST_PACKED_PROJECT_PATH");
  const char* envtest_env = std::getenv("ALCEDO_IA_LIVE_ENV_TEST_PATH");
  if (runtime_env == nullptr || project_env == nullptr || envtest_env == nullptr) {
    return std::nullopt;
  }
  LiveSmokeInputs out;
  out.runtime_env  = runtime_env;
  out.project_env = project_env;
  out.envtest_env = envtest_env;
  out.provider_id = EnvOr("ALCEDO_IA_LIVE_PROVIDER_ID", "openrouter");
  const std::string key_var = (out.provider_id.starts_with("volcengine"))
                                  ? "ALCEDO_VOLCENGINE_ARK_API_KEY"
                                  : "ALCEDO_OPENROUTER_API_KEY";
  out.api_key = EnvFileValue(envtest_env, key_var);
  if (out.api_key.empty()) {
    if (const char* e = std::getenv(key_var.c_str())) {
      out.api_key = e;
    }
  }
  return out;
}

TEST(ImageAnalysisLiveSmokeTest, DescribesOneImageFromPackedProject) {
  auto inputs = ReadLiveSmokeInputs();
  if (!inputs.has_value()) {
    GTEST_SKIP() << "Set ALCEDO_IA_LIVE_RUNTIME_PATH, ALCEDO_TEST_PACKED_PROJECT_PATH, and "
                    "ALCEDO_IA_LIVE_ENV_TEST_PATH to run the live image-analysis smoke.";
  }
  ASSERT_FALSE(inputs->api_key.empty())
      << "No API key for provider '" << inputs->provider_id << "' in .env.test at "
      << inputs->envtest_env;
  const auto& provider_id = inputs->provider_id;
  const auto& api_key     = inputs->api_key;

  // Populate the global OperatorFactory singleton, exactly as main.cpp:142 and every
  // pipeline-using test fixture do. Without this, ThumbnailService's render path builds a
  // CPUPipelineExecutor whose InitDefaultPipeline() calls OperatorFactory::Create() for each
  // default operator — and Create() returns nullptr for unregistered types. SetOperator()'s
  // 3-arg overload then dereferences the null op_ in SetGlobalParams, crashing the process
  // (0xC0000005). This is a test-harness requirement, not a Phase 5e concern.
  RegisterAllOperators();

  std::string build_err;
  auto        env = BuildLiveSmokeEnv(inputs->runtime_env, inputs->project_env, &build_err);
  ASSERT_NE(env, nullptr) << build_err;

  ImageAnalysisService ia_service(env->thumb_provider, env->analysis_client, env->gate);
  env->enc_temp_dir = std::filesystem::temp_directory_path() / "alcedo_ia_live_smoke_enc";
  std::filesystem::create_directories(env->enc_temp_dir);
  auto opts = MakeAnalysisOptions(ImageAnalysisTask::kDescribe, provider_id, api_key, env->enc_temp_dir);

  auto job = ia_service.StartAnalysis({env->view_item}, opts, {}, {});
  job->Wait();
  env->runtime->Stop();  // stop ASAP; the persisted-result assertions below don't need the sidecar

  auto results = job->Results();
  ASSERT_EQ(results.size(), 1u);
  const auto& r = results[0];
  ASSERT_EQ(r.status, ImageAnalysisItemStatus::kAnalyzed)
      << "item status not analyzed; error: " << r.error;
  ASSERT_TRUE(r.understanding.ok)
      << "understanding not ok; status=" << r.understanding.status
      << " error=" << r.understanding.error;
  ASSERT_FALSE(r.understanding.caption.empty())
      << "provider returned an empty caption; error=" << r.understanding.error;

  // Print the description (NOT the image). The secret must never appear in any field.
  std::cout << "\n[LIVE DESCRIBE] provider=" << provider_id
            << " model=" << r.understanding.model_id
            << " scene=\"" << r.understanding.scene << "\""
            << " tags=" << r.understanding.tags.size()
            << " confidence=" << r.understanding.confidence
            << " elapsed_ms=" << r.understanding.elapsed_ms
            << " tokens=" << r.understanding.usage.total_tokens
            << "\n  caption: " << r.understanding.caption << "\n  tags:";
  for (const auto& t : r.understanding.tags) {
    std::cout << " \"" << t << "\"";
  }
  std::cout << "\n";

  EXPECT_EQ(r.understanding.caption.find(api_key), std::string::npos);
  EXPECT_EQ(r.understanding.error.find(api_key), std::string::npos);
  EXPECT_EQ(r.understanding.scene.find(api_key), std::string::npos);
  for (const auto& t : r.understanding.tags) {
    EXPECT_EQ(t.find(api_key), std::string::npos);
  }

  // ---- Phase 5f: persist the live understanding through the ORM layer and round-trip it ----
  auto storage = env->project->GetStorage();
  auto& ai_ctrl = storage->GetAiStore();
  const auto file_id = env->view_item.element_id;

  // Before persistence the AI table is empty, so search only matches filename/metadata. Find a
  // caption/tag token NOT already matching the file, so any post-persist search hit is
  // attributable to the AI understanding rather than a filename coincidence.
  const auto candidates = CollectSearchTokens(r.understanding.caption, r.understanding.tags,
                                              r.understanding.scene);
  const auto isolating  = PickIsolatingToken(storage, candidates, file_id);

  const auto persisted = BuildAiDescription(r.understanding, file_id, "describe");
  ASSERT_TRUE(ai_ctrl.UpsertUnderstanding(persisted))
      << "UpsertUnderstanding rejected a valid live result";

  const auto got = ai_ctrl.GetActiveUnderstanding(file_id);
  ASSERT_TRUE(got.has_value()) << "GetActiveUnderstanding returned nothing after upsert";
  EXPECT_EQ(got->file_id_, file_id);
  EXPECT_EQ(got->caption_, r.understanding.caption);
  EXPECT_EQ(got->scene_, r.understanding.scene);
  EXPECT_EQ(got->provider_id_, r.understanding.provider);
  EXPECT_EQ(got->model_id_, r.understanding.model_id);
  EXPECT_DOUBLE_EQ(got->confidence_, r.understanding.confidence);
  EXPECT_TRUE(got->active_);
  const auto got_tags = got->Tags();
  ASSERT_EQ(got_tags.size(), r.understanding.tags.size())
      << "tag count changed across the JSON serialize/round-trip";
  for (size_t i = 0; i < got_tags.size(); ++i) {
    EXPECT_EQ(got_tags[i], r.understanding.tags[i]);
  }
  // The persisted row must not have leaked the API key into any stored text field.
  EXPECT_EQ(got->caption_.find(api_key), std::string::npos);
  EXPECT_EQ(got->scene_.find(api_key), std::string::npos);
  EXPECT_EQ(got->tags_json_.find(api_key), std::string::npos);

  // ---- Phase 5f: the live caption makes the file searchable (search-document AI contribution) ----
  if (candidates.empty()) {
    std::cout << "[LIVE SEARCH] caption had no clean ASCII-alpha token; skipping searchability "
                 "sub-check (persistence/round-trip still validated above)\n";
  } else if (isolating.empty()) {
    std::cout << "[LIVE SEARCH] every caption/tag token already matched filename/metadata before "
                 "persistence; cannot isolate the AI-caption contribution, skipping attribution "
                 "proof (persistence/round-trip still validated above)\n";
  } else {
    // `isolating` did NOT match before persistence (AI table empty); it MUST match after, since
    // the persisted caption is now part of this file's search document.
    EXPECT_TRUE(SearchReturnsFile(storage, isolating, file_id))
        << "file not searchable for caption token '" << isolating
        << "' after persisting the AI understanding — search document does not include the "
           "live caption";
    std::cout << "[LIVE SEARCH] caption token '" << isolating
              << "' searchable only after AI persistence (attribution proven)\n";
  }
}

TEST(ImageAnalysisLiveSmokeTest, RatesOneImageFromPackedProject) {
  auto inputs = ReadLiveSmokeInputs();
  if (!inputs.has_value()) {
    GTEST_SKIP() << "Set ALCEDO_IA_LIVE_RUNTIME_PATH, ALCEDO_TEST_PACKED_PROJECT_PATH, and "
                    "ALCEDO_IA_LIVE_ENV_TEST_PATH to run the live image-analysis smoke.";
  }
  ASSERT_FALSE(inputs->api_key.empty())
      << "No API key for provider '" << inputs->provider_id << "' in .env.test at "
      << inputs->envtest_env;
  const auto& provider_id = inputs->provider_id;
  const auto& api_key     = inputs->api_key;

  RegisterAllOperators();

  std::string build_err;
  auto        env = BuildLiveSmokeEnv(inputs->runtime_env, inputs->project_env, &build_err);
  ASSERT_NE(env, nullptr) << build_err;

  ImageAnalysisService ia_service(env->thumb_provider, env->analysis_client, env->gate);
  env->enc_temp_dir = std::filesystem::temp_directory_path() / "alcedo_ia_live_smoke_enc_score";
  std::filesystem::create_directories(env->enc_temp_dir);
  auto opts = MakeAnalysisOptions(ImageAnalysisTask::kScore, provider_id, api_key, env->enc_temp_dir);

  auto job = ia_service.StartAnalysis({env->view_item}, opts, {}, {});
  job->Wait();
  env->runtime->Stop();

  auto results = job->Results();
  ASSERT_EQ(results.size(), 1u);
  const auto& r = results[0];
  ASSERT_EQ(r.status, ImageAnalysisItemStatus::kAnalyzed)
      << "item status not analyzed; error: " << r.error;

  std::cout << "\n[LIVE SCORE] provider=" << provider_id << " ok=" << r.rating.ok
            << " status=" << r.rating.status << " rating=" << r.rating.rating
            << " rubric=" << r.rating.rubric_id << "/" << r.rating.rubric_version
            << " elapsed_ms=" << r.rating.elapsed_ms << "\n  reasons: " << r.rating.reasons
            << "\n";

  // If the live provider cannot score, that is a provider limitation, not a code defect —
  // record it and stop (the sidecar is already stopped; the env dtor cleans temp dirs). The
  // 1..=5 contract is still unit-validated by AiStoreTest.
  if (!r.rating.ok) {
    std::cout << "[LIVE SCORE] provider returned an error for ScoreImage (status=" << r.rating.status
              << " error=\"" << r.rating.error << "\"); skipping persistence assertions\n";
    GTEST_SKIP() << "live provider did not return a successful score; rating persistence not "
                    "exercised (see stdout for the provider error)";
  }

  // Phase 5f rating contract: a successful score is a 1..=5 integer (no confidence).
  ASSERT_GE(r.rating.rating, 1) << "live rating below the 1..=5 contract: " << r.rating.rating;
  ASSERT_LE(r.rating.rating, 5) << "live rating above the 1..=5 contract: " << r.rating.rating;
  ASSERT_FALSE(r.rating.reasons.empty()) << "live rating returned no reasons";

  EXPECT_EQ(r.rating.reasons.find(api_key), std::string::npos);
  EXPECT_EQ(r.rating.error.find(api_key), std::string::npos);

  // ---- Phase 5f: persist the live rating through the ORM layer and round-trip it ----
  auto storage = env->project->GetStorage();
  auto& ai_ctrl = storage->GetAiStore();
  const auto file_id = env->view_item.element_id;

  // Before persistence the AI tables are empty, so search only matches filename/metadata. Find a
  // reasons token NOT already matching the file, so we can prove the rating does NOT make it
  // searchable (rating is excluded from the search document).
  const auto candidates = CollectSearchTokens(r.rating.reasons, {}, "");
  const auto isolating  = PickIsolatingToken(storage, candidates, file_id);

  const auto persisted = BuildAiRating(r.rating, file_id, "rate");
  ASSERT_TRUE(ai_ctrl.UpsertRating(persisted))
      << "UpsertRating rejected a valid live result";

  const auto got = ai_ctrl.GetActiveRating(file_id);
  ASSERT_TRUE(got.has_value()) << "GetActiveRating returned nothing after upsert";
  EXPECT_EQ(got->file_id_, file_id);
  EXPECT_EQ(got->rating_, r.rating.rating);
  EXPECT_EQ(got->reasons_, r.rating.reasons);
  EXPECT_EQ(got->rubric_id_, r.rating.rubric_id);
  EXPECT_EQ(got->rubric_version_, r.rating.rubric_version);
  EXPECT_TRUE(got->active_);
  EXPECT_EQ(got->reasons_.find(api_key), std::string::npos);

  // ---- Phase 5f: rating reasons are NOT part of full-text search ----
  if (candidates.empty()) {
    std::cout << "[LIVE SEARCH] rating reasons had no clean ASCII-alpha token; skipping the "
                 "rating-excluded-from-search sub-check\n";
  } else if (isolating.empty()) {
    std::cout << "[LIVE SEARCH] every reasons token already matched filename/metadata before "
                 "persistence; cannot isolate, skipping the rating-excluded-from-search proof\n";
  } else {
    // `isolating` did NOT match before persistence (AI tables empty); it must STILL not match
    // after, because the rating is NOT part of the search document.
    EXPECT_FALSE(SearchReturnsFile(storage, isolating, file_id))
        << "file became searchable for rating-reasons token '" << isolating
        << "' after persisting the rating — rating reasons must NOT enter the search document";
    std::cout << "[LIVE SEARCH] rating-reasons token '" << isolating
              << "' correctly NOT searchable after rating persistence\n";
  }
}

}  // namespace
}  // namespace alcedo