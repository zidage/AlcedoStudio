//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <exiv2/exiv2.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/export_service.hpp"
#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/runtime/drt_display.hpp"
#include "type/supported_file_type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

using namespace std::chrono_literals;

struct Options {
  std::filesystem::path              out_dir = std::filesystem::path("build") / "diagnostics" /
                                  "hs_reference_exports";
  std::vector<std::filesystem::path> raw_paths;
  float                              shadow_slider = 100.0f;
  float                              highlight_slider = -100.0f;
  float                              saturation_slider = pipeline_defaults::kCleanBaselineSaturation;
  bool                               keep_project = false;
  bool                               use_default_lut = false;
};

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

auto ToWide(std::string_view text) -> std::wstring {
  return std::wstring(text.begin(), text.end());
}

auto FindArgValue(int argc, wchar_t** argv, std::wstring_view opt_name)
    -> std::optional<std::wstring_view> {
  const std::wstring opt_eq = std::wstring(opt_name) + L"=";
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i] ? std::wstring_view(argv[i]) : std::wstring_view();
    if (arg == opt_name) {
      if (i + 1 < argc && argv[i + 1]) {
        return std::wstring_view(argv[i + 1]);
      }
      return std::nullopt;
    }
    if (arg.rfind(opt_eq, 0) == 0) {
      return arg.substr(opt_eq.size());
    }
  }
  return std::nullopt;
}

auto HasFlag(int argc, wchar_t** argv, std::wstring_view flag) -> bool {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::wstring_view(argv[i]) == flag) {
      return true;
    }
  }
  return false;
}

auto ParseFloatArg(int argc, wchar_t** argv, std::wstring_view opt_name, float fallback) -> float {
  const auto value = FindArgValue(argc, argv, opt_name);
  if (!value.has_value() || value->empty()) {
    return fallback;
  }
  try {
    return std::stof(std::wstring(*value));
  } catch (...) {
    throw std::runtime_error("Failed to parse numeric command-line argument.");
  }
}

auto BuildOptions(int argc, wchar_t** argv) -> Options {
  Options options;
  if (const auto out = FindArgValue(argc, argv, L"--out-dir"); out.has_value() && !out->empty()) {
    options.out_dir = std::filesystem::path(std::wstring(*out));
  }
  options.shadow_slider = ParseFloatArg(argc, argv, L"--shadow", options.shadow_slider);
  options.highlight_slider =
      ParseFloatArg(argc, argv, L"--highlight", options.highlight_slider);
  options.saturation_slider =
      ParseFloatArg(argc, argv, L"--saturation", options.saturation_slider);
  options.keep_project    = HasFlag(argc, argv, L"--keep-project");
  options.use_default_lut = HasFlag(argc, argv, L"--default-lut");

  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i] ? std::wstring_view(argv[i]) : std::wstring_view();
    if (arg.empty()) {
      continue;
    }
    if (arg[0] == L'-') {
      if (arg == L"--out-dir" || arg == L"--shadow" || arg == L"--highlight" ||
          arg == L"--saturation") {
        ++i;
      }
      continue;
    }
    options.raw_paths.emplace_back(std::wstring(arg));
  }
  return options;
}

void PrintUsage() {
  std::cout << "Usage:\n"
            << "  HsResearchExportTool [--out-dir DIR] [--shadow 100] [--highlight -100]\n"
            << "                      [--saturation 30]\n"
            << "                      [--default-lut] [--keep-project] RAW1 [RAW2 ...]\n";
}

auto SanitizeSliderToken(float value) -> std::string {
  const bool positive = value >= 0.0f;
  const int  rounded  = static_cast<int>(std::lround(std::abs(value)));
  std::ostringstream oss;
  oss << (positive ? "plus_" : "minus_") << rounded;
  return oss.str();
}

auto BuildOutputName(const std::filesystem::path& raw_path, float shadow_slider,
                     float highlight_slider) -> std::filesystem::path {
  const std::string stem = PathToUtf8(raw_path.stem());
  return std::filesystem::path(stem + "_shadow_" + SanitizeSliderToken(shadow_slider) +
                               "_highlight_" + SanitizeSliderToken(highlight_slider) + ".png");
}

auto FindDefaultLutPath() -> std::string {
  std::vector<std::filesystem::path> candidates;
#ifdef CONFIG_PATH
  candidates.emplace_back(std::filesystem::path(CONFIG_PATH) / "LUTs" / "5207.cube");
#endif
  candidates.emplace_back(std::filesystem::path("alcedo_studio") / "src" / "config" / "LUTs" /
                          "5207.cube");

  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && !ec) {
      return PathToUtf8(candidate);
    }
  }
  return {};
}

auto MakeProjectPath(const std::filesystem::path& out_dir, std::wstring_view suffix)
    -> std::filesystem::path {
  const auto tick =
      std::chrono::steady_clock::now().time_since_epoch().count();
  std::wostringstream oss;
  oss << L"hs_research_" << tick << suffix;
  return out_dir / std::filesystem::path(oss.str());
}

auto ImportBlocking(ImportServiceImpl& import_service,
                    const std::vector<std::filesystem::path>& raw_paths) -> ImportLogSnapshot {
  std::vector<image_path_t> inputs;
  inputs.reserve(raw_paths.size());
  for (const auto& path : raw_paths) {
    inputs.push_back(path);
  }

  auto                       import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> done;
  auto                       done_future = done.get_future();
  import_job->on_finished_ = [&done](const ImportResult& result) { done.set_value(result); };

  import_job = import_service.ImportToFolder(inputs, L"", {}, import_job);
  if (!import_job) {
    throw std::runtime_error("ImportService returned a null job.");
  }
  if (done_future.wait_for(300s) != std::future_status::ready) {
    throw std::runtime_error("Timed out waiting for RAW import.");
  }

  const ImportResult result = done_future.get();
  if (result.failed_ != 0 || result.imported_ != raw_paths.size()) {
    throw std::runtime_error("RAW import did not finish cleanly: imported=" +
                             std::to_string(result.imported_) + " failed=" +
                             std::to_string(result.failed_));
  }

  if (!import_job->import_log_) {
    throw std::runtime_error("Import log is unavailable after import.");
  }

  const auto snapshot = import_job->import_log_->Snapshot();
  import_service.SyncImports(snapshot, L"");
  return snapshot;
}

auto FindImportedEntry(const ImportLogSnapshot& snapshot, const std::filesystem::path& raw_path)
    -> const ImportLogEntry* {
  std::error_code canonical_ec;
  const auto normalized_input = std::filesystem::weakly_canonical(raw_path, canonical_ec);
  for (const auto& entry : snapshot.created_) {
    const auto normalized_source = std::filesystem::weakly_canonical(entry.source_path_, canonical_ec);
    if (!canonical_ec && normalized_source == normalized_input) {
      return &entry;
    }
    if (entry.source_path_ == raw_path) {
      return &entry;
    }
  }
  return nullptr;
}

void ApplyReferenceStudyAdjustments(CPUPipelineExecutor& exec, float shadow_slider,
                                    float highlight_slider, float saturation_slider,
                                    const std::string& lut_path) {
  exec.ResetToCleanBaselineAdjustments();

  auto& global_params = exec.GetGlobalParams();
  auto& basic_stage   = exec.GetStage(PipelineStageName::Basic_Adjustment);
  auto& color_stage   = exec.GetStage(PipelineStageName::Color_Adjustment);

  basic_stage.SetOperator(OperatorType::EXPOSURE,
                          {{"exposure", pipeline_defaults::kCleanBaselineExposure}}, global_params);
  basic_stage.SetOperator(OperatorType::CONTRAST, {{"contrast", 0.0f}}, global_params);
  basic_stage.SetOperator(OperatorType::BLACK, {{"black", 0.0f}}, global_params);
  basic_stage.SetOperator(OperatorType::WHITE, {{"white", 0.0f}}, global_params);
  basic_stage.SetOperator(OperatorType::SHADOWS, {{"shadows", shadow_slider}}, global_params);
  basic_stage.SetOperator(OperatorType::HIGHLIGHTS, {{"highlights", highlight_slider}},
                          global_params);

  color_stage.SetOperator(OperatorType::SATURATION,
                          {{"saturation", saturation_slider}},
                          global_params);
  color_stage.SetOperator(OperatorType::TINT, {{"tint", 0.0f}}, global_params);
  color_stage.SetOperator(OperatorType::LMT, {{"ocio_lmt", lut_path}}, global_params);
}

void RemoveIfExists(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::remove(path, ec);
  }
}

auto LoadWideArgsFromProcess(int argc, char** argv) -> std::vector<std::wstring> {
#if defined(_WIN32)
  int     wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  if (wargv == nullptr || wargc <= 0) {
    throw std::runtime_error("CommandLineToArgvW failed.");
  }

  std::vector<std::wstring> args;
  args.reserve(static_cast<size_t>(wargc));
  for (int i = 0; i < wargc; ++i) {
    args.emplace_back(wargv[i] ? wargv[i] : L"");
  }
  LocalFree(wargv);
  return args;
#else
  std::vector<std::wstring> args;
  args.reserve(static_cast<size_t>(std::max(argc, 0)));
  for (int i = 0; i < argc; ++i) {
    const char* raw = (argv != nullptr && argv[i] != nullptr) ? argv[i] : "";
    args.emplace_back(raw, raw + std::char_traits<char>::length(raw));
  }
  return args;
#endif
}

}  // namespace

auto RunHsResearchExportTool(int argc, char** argv) -> int {
  try {
    auto                       wide_args_storage = LoadWideArgsFromProcess(argc, argv);
    std::vector<wchar_t*>      wide_argv;
    wide_argv.reserve(wide_args_storage.size());
    for (auto& item : wide_args_storage) {
      wide_argv.push_back(item.data());
    }

    const int     wide_argc = static_cast<int>(wide_argv.size());
    wchar_t**     wide_argv_ptr = wide_argv.empty() ? nullptr : wide_argv.data();
    const Options options = BuildOptions(wide_argc, wide_argv_ptr);
    if (options.raw_paths.empty()) {
      PrintUsage();
      return 1;
    }

    for (const auto& path : options.raw_paths) {
      if (!std::filesystem::is_regular_file(path) || !is_supported_file(path)) {
        throw std::runtime_error("Unsupported or missing RAW input: " + PathToUtf8(path));
      }
    }

    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    RegisterAllOperators();

    std::filesystem::create_directories(options.out_dir);
    const std::filesystem::path db_path   = MakeProjectPath(options.out_dir, L".db");
    const std::filesystem::path meta_path = MakeProjectPath(options.out_dir, L".json");
    RemoveIfExists(db_path);
    RemoveIfExists(meta_path);

    const std::string default_lut_path = options.use_default_lut ? FindDefaultLutPath() : "";
    if (options.use_default_lut) {
      std::cout << "[HsResearchExportTool] default LUT: "
                << (default_lut_path.empty() ? "(none)" : default_lut_path) << '\n';
    }

    {
      ProjectService project(db_path, meta_path, ProjectOpenMode::kCreateNew);
      auto           sleeve_service   = project.GetSleeveService();
      auto           image_pool       = project.GetImagePoolService();
      auto           pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());
      pipeline_service->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);

      ImportServiceImpl import_service(sleeve_service, image_pool);
      const auto        snapshot = ImportBlocking(import_service, options.raw_paths);

      ExportService export_service(sleeve_service, image_pool, pipeline_service);

      for (const auto& raw_path : options.raw_paths) {
        const auto* entry = FindImportedEntry(snapshot, raw_path);
        if (!entry) {
          throw std::runtime_error("Failed to find imported entry for " + PathToUtf8(raw_path));
        }

        auto pipeline_guard = pipeline_service->LoadPipeline(entry->element_id_);
        if (!pipeline_guard || !pipeline_guard->pipeline_) {
          throw std::runtime_error("Failed to load pipeline for " + PathToUtf8(raw_path));
        }

        ApplyReferenceStudyAdjustments(*pipeline_guard->pipeline_, options.shadow_slider,
                                       options.highlight_slider, options.saturation_slider,
                                       default_lut_path);
        pipeline_guard->dirty_ = true;

        ExportTask task;
        task.sleeve_id_              = entry->element_id_;
        task.image_id_               = entry->image_id_;
        task.options_.format_        = ImageFormatType::PNG;
        task.options_.bit_depth_     = ExportFormatOptions::BIT_DEPTH::BIT_16;
        task.options_.compression_level_ = 4;
        task.options_.export_path_   = options.out_dir /
                                     BuildOutputName(raw_path, options.shadow_slider,
                                                     options.highlight_slider);
        task.recipe_ = ExportRecipe::FromLegacyOptions(task.options_);
        if (pipeline_guard->document_ && pipeline_guard->document_->Drt()) {
          std::lock_guard<std::mutex> lock(pipeline_guard->pipeline_->GetRenderLock());
          task.recipe_->output_color_ =
              ExportColorProfileFromDrt(pipeline_guard->document_->Drt()->Params().Params());
        }
        pipeline_service->SavePipeline(pipeline_guard);
        export_service.EnqueueExportTask(task);

        std::cout << "[HsResearchExportTool] queued " << PathToUtf8(raw_path) << " -> "
                  << PathToUtf8(task.options_.export_path_) << '\n';
      }

      pipeline_service->Sync();

      std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
      auto                                                     future = done.get_future();
      export_service.ExportAll(
          [&done](std::shared_ptr<std::vector<ExportResult>> results) { done.set_value(results); });

      if (future.wait_for(1800s) != std::future_status::ready) {
        throw std::runtime_error("Timed out waiting for export tasks.");
      }

      const auto results = future.get();
      if (!results || results->size() != options.raw_paths.size()) {
        throw std::runtime_error("Unexpected export result count.");
      }

      for (size_t i = 0; i < results->size(); ++i) {
        if (!(*results)[i].success_) {
          throw std::runtime_error("Export failed: " + (*results)[i].message_);
        }
      }

      project.SaveProject(meta_path);
    }

    std::cout << "[HsResearchExportTool] exports written to " << PathToUtf8(options.out_dir)
              << '\n';

    if (!options.keep_project) {
      RemoveIfExists(db_path);
      RemoveIfExists(meta_path);
    } else {
      std::cout << "[HsResearchExportTool] kept project files:\n"
                << "  " << PathToUtf8(db_path) << '\n'
                << "  " << PathToUtf8(meta_path) << '\n';
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "[HsResearchExportTool] ERROR: " << e.what() << '\n';
    return 1;
  }
}

}  // namespace alcedo

auto main(int argc, char** argv) -> int {
  return alcedo::RunHsResearchExportTool(argc, argv);
}
