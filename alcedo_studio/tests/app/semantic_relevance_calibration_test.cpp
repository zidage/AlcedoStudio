//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Calibration of the natural-language relevance rule against a real project and the real
// semantic sidecar (global_search_redesign_plan.md, Phase GS1, Step 9). It prints the query
// similarity c, the relevance r, and the z value of each listed file and of the 20 most relevant
// files of each query, and checks the current constants of SemanticVectorSearch:
//   - every nonsense query keeps no file;
//   - every positive query keeps all of its listed files.
//
// Environment variables (the test is skipped when one of the first two is absent):
//   ALCEDO_SEMANTIC_CALIBRATION_PROJECT  a packed .alcd project with generated embeddings
//   ALCEDO_SEMANTIC_CALIBRATION_QUERIES  a JSON file:
//     {"positive": [{"query": "red car", "files": ["DSC_0001.NEF", ...]}, ...],
//      "nonsense": ["quantum spreadsheet invoice", ...]}
//   ALCEDO_SEMANTIC_CALIBRATION_MODEL_DIR  the semantic model directory (the folder that holds
//                                          one folder per model profile)
//   ALCEDO_MIND_BINARY                     the release alcedo_mind executable
//   ALCEDO_SEMANTIC_CALIBRATION_SCORES_CSV a CSV file that receives every scored file of every
//                                          query, for a search of the constant values offline
// The file names are Element names of the project. The scope is the whole library.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <json.hpp>
#include <set>
#include <string>
#include <vector>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "storage/store/semantic/semantic_vector_search.hpp"
#include "utils/string/convert.hpp"
#include "support/test_temp_directory.hpp"

namespace alcedo {
namespace {

constexpr size_t kTopRowCount = 20;

struct PositiveQuery {
  std::string           query_{};
  std::set<std::string> files_{};
};

struct CalibrationQueries {
  std::vector<PositiveQuery> positive_{};
  std::vector<std::string>   nonsense_{};
};

auto ReadCalibrationQueries(const std::filesystem::path& path) -> CalibrationQueries {
  std::ifstream input(path);
  EXPECT_TRUE(input.is_open()) << path.string();
  const auto         json = nlohmann::json::parse(input);
  CalibrationQueries queries;
  for (const auto& entry : json.at("positive")) {
    PositiveQuery positive{.query_ = entry.at("query").get<std::string>()};
    for (const auto& file : entry.at("files")) {
      positive.files_.insert(file.get<std::string>());
    }
    queries.positive_.push_back(std::move(positive));
  }
  for (const auto& entry : json.at("nonsense")) {
    queries.nonsense_.push_back(entry.get<std::string>());
  }
  return queries;
}

auto FormatScore(const SemanticRelevanceScore& score) -> std::string {
  const auto z = score.relevance_z_.has_value() ? std::format("{:7.3f}", *score.relevance_z_)
                                                : std::string("    n/a");
  return std::format("    {:<40} c {:7.4f}  b {:7.4f}  r {:7.4f}  z {}  {}", score.file_name_,
                     score.query_similarity_, score.neutral_similarity_, score.relevance_,
                     z, score.kept_ ? "kept" : "-");
}

/// Prints the listed files and the top rows of one query; returns the kept count.
auto ReportQuery(const std::string& kind, const std::string& query,
                 const std::vector<SemanticRelevanceScore>& scores,
                 const std::set<std::string>&               listed_files) -> size_t {
  const auto kept = static_cast<size_t>(
      std::count_if(scores.begin(), scores.end(), [](const auto& s) { return s.kept_; }));
  std::cout << std::format("\n[{}] \"{}\": {} scored, {} kept\n", kind, query, scores.size(),
                           kept);
  if (!listed_files.empty()) {
    std::cout << "  listed files:\n";
    for (const auto& score : scores) {
      if (listed_files.contains(score.file_name_)) {
        std::cout << FormatScore(score) << "\n";
      }
    }
  }
  if (kept > 0 && listed_files.empty()) {
    std::cout << "  kept files:\n";
    for (const auto& score : scores) {
      if (score.kept_) {
        std::cout << FormatScore(score) << "\n";
      }
    }
  }
  std::cout << std::format("  top {} files:\n", kTopRowCount);
  for (size_t i = 0; i < std::min(kTopRowCount, scores.size()); ++i) {
    std::cout << FormatScore(scores[i]) << "\n";
  }
  return kept;
}

TEST(SemanticRelevanceCalibrationTest, LiveRelevanceKeepsListedPhotosAndRejectsNonsenseQueries) {
  const test::TestTempDirectory temp_dir;
  const char* project_env = std::getenv("ALCEDO_SEMANTIC_CALIBRATION_PROJECT");
  const char* queries_env = std::getenv("ALCEDO_SEMANTIC_CALIBRATION_QUERIES");
  if (project_env == nullptr || *project_env == '\0' || queries_env == nullptr ||
      *queries_env == '\0') {
    GTEST_SKIP() << "Set ALCEDO_SEMANTIC_CALIBRATION_PROJECT and "
                    "ALCEDO_SEMANTIC_CALIBRATION_QUERIES to run the calibration.";
  }

  QCoreApplication::setOrganizationName(QStringLiteral("AlcedoStudioTests"));
  QCoreApplication::setApplicationName(QStringLiteral("SemanticRelevanceCalibrationTest"));
  if (const char* model_dir = std::getenv("ALCEDO_SEMANTIC_CALIBRATION_MODEL_DIR")) {
    QSettings settings;
    settings.setValue(QStringLiteral("semantic/modelDirectory"), QString::fromUtf8(model_dir));
  }

  const auto queries  = ReadCalibrationQueries(std::filesystem::path(queries_env));
  const auto work_dir = temp_dir.Path() / "semantic_relevance_calibration";
  std::error_code ec;
  std::filesystem::remove_all(work_dir, ec);
  std::filesystem::create_directories(work_dir);

  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  QString               unpack_error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(std::filesystem::path(project_env), work_dir,
                                                     QStringLiteral("calibration"), &db_path,
                                                     &meta_path, &unpack_error))
      << unpack_error.toStdString();

  {
    ProjectService project(db_path, meta_path);
    auto           filter_service = project.GetSleeveFilterService();
    ASSERT_NE(filter_service, nullptr);
    ASSERT_TRUE(filter_service->HasSemanticSearchProvider());
    const auto& search = project.GetStorage()->GetSemanticVectorSearch();

    std::cout << std::format(
        "\nConstants: kMinQuerySimilarity {}, kMinRelevanceZ {}, kMinScopeStatisticsCount {}, "
        "neutral text \"{}\"\n",
        SemanticVectorSearch::kMinQuerySimilarity, SemanticVectorSearch::kMinRelevanceZ,
        SemanticVectorSearch::kMinScopeStatisticsCount, SemanticVectorSearch::kNeutralText);

    std::ofstream csv;
    if (const char* csv_path = std::getenv("ALCEDO_SEMANTIC_CALIBRATION_SCORES_CSV")) {
      csv.open(std::filesystem::path(csv_path), std::ios::binary);
      ASSERT_TRUE(csv.is_open()) << csv_path;
      csv << "kind,query,listed,file,query_similarity,neutral_similarity,relevance,relevance_z\n";
    }
    const auto scores_of = [&](const std::string& kind, const std::string& query,
                               const std::set<std::string>& listed_files) {
      const auto  embedding = filter_service->EmbedSemanticQuery(conv::FromBytes(query));
      std::string error;
      auto        scores = search.ReadRelevanceScores(0, embedding, &error);
      EXPECT_TRUE(error.empty()) << error;
      if (csv.is_open()) {
        for (const auto& score : scores) {
          const auto z = score.relevance_z_.has_value()
                             ? std::format("{:.6f}", *score.relevance_z_)
                             : std::string();
          csv << std::format("{},\"{}\",{},\"{}\",{:.6f},{:.6f},{:.6f},{}\n", kind, query,
                             listed_files.contains(score.file_name_) ? 1 : 0, score.file_name_,
                             score.query_similarity_, score.neutral_similarity_, score.relevance_,
                             z);
        }
      }
      return scores;
    };

    for (const auto& positive : queries.positive_) {
      const auto scores = scores_of("positive", positive.query_, positive.files_);
      ReportQuery("positive", positive.query_, scores, positive.files_);
      std::set<std::string> found;
      for (const auto& score : scores) {
        if (positive.files_.contains(score.file_name_)) {
          found.insert(score.file_name_);
          EXPECT_TRUE(score.kept_) << positive.query_ << ": " << score.file_name_;
        }
      }
      EXPECT_EQ(found, positive.files_) << positive.query_ << ": listed files without a ready "
                                                              "embedding in the project";
    }
    for (const auto& nonsense : queries.nonsense_) {
      const auto scores = scores_of("nonsense", nonsense, {});
      EXPECT_EQ(ReportQuery("nonsense", nonsense, scores, {}), 0U) << nonsense;
    }
    std::cout.flush();
  }
  std::filesystem::remove_all(work_dir, ec);
}

}  // namespace
}  // namespace alcedo
