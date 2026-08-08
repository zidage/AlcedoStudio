//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace alcedo {

inline constexpr const char* kDefaultSemanticPhotographyPromptConfigHash =
    "photography-labels-v5-en";
inline constexpr const char* kDefaultSemanticPhotographyZhPromptConfigHash =
    "photography-labels-v5-zh";
inline constexpr double kDefaultSemanticLabelConfidenceThreshold = 0.20;
inline constexpr double kDefaultSemanticLabelMarginThreshold     = 0.03;
inline constexpr size_t kMaxSemanticImageLabelCount              = 3;
inline constexpr size_t kDefaultSemanticLabelTopScoreCount       = kMaxSemanticImageLabelCount;
// How many label prototypes to rank per image before the elbow decides which to keep.
// Analog of kSemanticSearchMinCandidatePool on the search side: large enough for the
// elbow to see a knee in the score distribution, independent of the display cap.
inline constexpr size_t kSemanticLabelCandidatePoolSize          = 10;

enum class SemanticLabelLanguage : uint8_t {
  kEnglish = 0,
  kChinese,
};

struct SemanticPhotographyLabelConfig {
  std::string canonical_label{};
  std::string english_label{};
  std::string chinese_label{};
  std::string english_query{};
  std::string chinese_query{};
};

struct SemanticLabelQueryConfig {
  std::string label{};            // Model-facing label text stored with prototypes.
  std::string query{};            // Model-facing prompt/query text.
  std::string canonical_label{};  // Stable English id used for cross-language mapping.
  std::string english_label{};
  std::string chinese_label{};
};

struct SemanticGenerationLabelPrototype {
  std::string        label{};
  std::vector<float> embedding{};
};

inline auto SemanticLabelLanguageForModel(std::string_view profile_id, std::string_view language)
    -> SemanticLabelLanguage {
  auto lower = [](std::string_view value) {
    std::string out(value);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
  };
  const auto profile = lower(profile_id);
  const auto lang    = lower(language);
  if (lang == "zh" || lang == "zh-cn" || profile.ends_with("-zh")) {
    return SemanticLabelLanguage::kChinese;
  }
  return SemanticLabelLanguage::kEnglish;
}

inline auto SemanticPromptConfigHashForLanguage(SemanticLabelLanguage language) -> const char* {
  return language == SemanticLabelLanguage::kChinese ? kDefaultSemanticPhotographyZhPromptConfigHash
                                                     : kDefaultSemanticPhotographyPromptConfigHash;
}

inline auto SemanticSupportedTextLanguagesJson(SemanticLabelLanguage language) -> const char* {
  return language == SemanticLabelLanguage::kChinese ? R"(["zh"])" : R"(["en","zh"])";
}

// The `english_label` / `chinese_label` fields are the human-facing display names and the
// alias keys used for search routing; they are intentionally kept readable (a few are
// established multi-word terms such as "still life" or "long exposure").
//
// The `english_query` / `chinese_query` fields are the text actually embedded as the label
// prototype. Contrastive vision-language models such as SigLIP degrade on long descriptive
// prompts, so each query is reduced to a single word (or, for irreducible technical terms,
// the shortest possible phrase). The label set is also pruned of overlapping / non-photographic
// categories: ceremony (~wedding), fashion (~portrait), performance (~concert/event),
// screenshot (not a photographic subject), and panorama (ambiguous without a stitching feature).
inline auto DefaultSemanticPhotographyLabelDefinitions()
    -> const std::vector<SemanticPhotographyLabelConfig>& {
  static const std::vector<SemanticPhotographyLabelConfig> labels{
      {"portrait", "portrait", "人像", "portrait", "人像"},
      {"group", "group", "合影", "group", "合影"},
      {"family", "family", "家庭", "family", "家庭"},
      {"children", "children", "儿童", "children", "儿童"},
      {"wedding", "wedding", "婚礼", "wedding", "婚礼"},
      {"event", "event", "活动", "event", "活动"},
      {"concert", "concert", "演唱会", "concert", "演唱会"},
      {"sports", "sports", "运动", "sports", "运动"},
      {"street", "street", "街头", "street", "街头"},
      {"cityscape", "cityscape", "城市风光", "cityscape", "城市风光"},
      {"architecture", "architecture", "建筑", "architecture", "建筑"},
      {"interior", "interior", "室内", "interior", "室内"},
      {"landscape", "landscape", "风景", "landscape", "风景"},
      {"mountain", "mountain", "山景", "mountain", "山景"},
      {"forest", "forest", "森林", "forest", "森林"},
      {"desert", "desert", "沙漠", "desert", "沙漠"},
      {"beach", "beach", "海滩", "beach", "海滩"},
      {"lake", "lake", "湖泊", "lake", "湖泊"},
      {"waterfall", "waterfall", "瀑布", "waterfall", "瀑布"},
      {"garden", "garden", "花园", "garden", "花园"},
      {"flower", "flower", "花卉", "flower", "花卉"},
      {"wildlife", "wildlife", "野生动物", "wildlife", "野生动物"},
      {"pet", "pet", "宠物", "pet", "宠物"},
      {"food and drink", "food and drink", "餐饮", "food", "餐饮"},
      {"product", "product", "产品", "product", "产品"},
      {"still life", "still life", "静物", "still life", "静物"},
      {"vehicle", "vehicle", "交通工具", "vehicle", "交通工具"},
      {"artwork", "artwork", "艺术品", "artwork", "艺术品"},
      {"document", "document", "文档", "document", "文档"},
      {"macro", "macro", "微距", "macro", "微距"},
      {"night", "night", "夜景", "night", "夜景"},
      {"sunrise and sunset", "sunrise and sunset", "日出日落", "sunset", "日出日落"},
      {"snow", "snow", "雪景", "snow", "雪景"},
      {"autumn", "autumn", "秋天", "autumn", "秋天"},
      {"fog", "fog", "雾", "fog", "雾"},
      {"black and white", "black and white", "黑白", "monochrome", "黑白"},
      {"silhouette", "silhouette", "剪影", "silhouette", "剪影"},
      {"aerial", "aerial", "航拍", "aerial", "航拍"},
      {"long exposure", "long exposure", "长曝光", "long exposure", "长曝光"},
      {"fireworks", "fireworks", "烟花", "fireworks", "烟花"},
      {"studio", "studio", "影棚", "studio", "影棚"},
  };
  return labels;
}

inline auto MakeSemanticLabelQueryConfigs(SemanticLabelLanguage language)
    -> std::vector<SemanticLabelQueryConfig> {
  std::vector<SemanticLabelQueryConfig> queries;
  const auto&                           definitions = DefaultSemanticPhotographyLabelDefinitions();
  queries.reserve(definitions.size());
  for (const auto& entry : definitions) {
    queries.push_back(SemanticLabelQueryConfig{
        .label =
            language == SemanticLabelLanguage::kChinese ? entry.chinese_label : entry.english_label,
        .query =
            language == SemanticLabelLanguage::kChinese ? entry.chinese_query : entry.english_query,
        .canonical_label = entry.canonical_label,
        .english_label   = entry.english_label,
        .chinese_label   = entry.chinese_label,
    });
  }
  return queries;
}

inline auto DefaultSemanticPhotographyLabelQueries()
    -> const std::vector<SemanticLabelQueryConfig>& {
  static const std::vector<SemanticLabelQueryConfig> labels =
      MakeSemanticLabelQueryConfigs(SemanticLabelLanguage::kEnglish);
  return labels;
}

inline auto DefaultSemanticPhotographyLabelQueries(SemanticLabelLanguage language)
    -> const std::vector<SemanticLabelQueryConfig>& {
  if (language == SemanticLabelLanguage::kChinese) {
    static const std::vector<SemanticLabelQueryConfig> labels =
        MakeSemanticLabelQueryConfigs(SemanticLabelLanguage::kChinese);
    return labels;
  }
  return DefaultSemanticPhotographyLabelQueries();
}

inline auto NormalizeSemanticLabelKey(std::string value) -> std::string {
  const auto first = std::find_if_not(value.begin(), value.end(),
                                      [](unsigned char ch) { return std::isspace(ch) != 0; });
  const auto last  = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
                      return std::isspace(ch) != 0;
                    }).base();
  if (first >= last) {
    return {};
  }
  std::string out(first, last);
  std::ranges::transform(out, out.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return out;
}

inline auto SemanticLabelCanonicalLookup() -> const std::unordered_map<std::string, std::string>& {
  static const std::unordered_map<std::string, std::string> lookup = [] {
    std::unordered_map<std::string, std::string> map;
    for (const auto& entry : DefaultSemanticPhotographyLabelDefinitions()) {
      map.emplace(NormalizeSemanticLabelKey(entry.canonical_label), entry.canonical_label);
      map.emplace(NormalizeSemanticLabelKey(entry.english_label), entry.canonical_label);
      map.emplace(NormalizeSemanticLabelKey(entry.chinese_label), entry.canonical_label);
    }
    return map;
  }();
  return lookup;
}

inline auto SemanticLabelByCanonicalLookup()
    -> const std::unordered_map<std::string, const SemanticPhotographyLabelConfig*>& {
  static const std::unordered_map<std::string, const SemanticPhotographyLabelConfig*> lookup = [] {
    std::unordered_map<std::string, const SemanticPhotographyLabelConfig*> map;
    for (const auto& entry : DefaultSemanticPhotographyLabelDefinitions()) {
      map.emplace(entry.canonical_label, &entry);
    }
    return map;
  }();
  return lookup;
}

inline auto CanonicalSemanticLabel(std::string_view label_text) -> std::optional<std::string> {
  const auto found =
      SemanticLabelCanonicalLookup().find(NormalizeSemanticLabelKey(std::string(label_text)));
  if (found == SemanticLabelCanonicalLookup().end()) {
    return std::nullopt;
  }
  return found->second;
}

inline auto SemanticLabelDisplayText(std::string_view label_text, SemanticLabelLanguage language)
    -> std::string {
  const auto canonical = CanonicalSemanticLabel(label_text);
  if (!canonical.has_value()) {
    return std::string(label_text);
  }
  const auto found = SemanticLabelByCanonicalLookup().find(*canonical);
  if (found == SemanticLabelByCanonicalLookup().end()) {
    return std::string(label_text);
  }
  return language == SemanticLabelLanguage::kChinese ? found->second->chinese_label
                                                     : found->second->english_label;
}

inline auto SemanticLabelAliases(std::string_view label_text) -> std::vector<std::string> {
  const auto canonical = CanonicalSemanticLabel(label_text);
  if (!canonical.has_value()) {
    return {std::string(label_text)};
  }
  const auto found = SemanticLabelByCanonicalLookup().find(*canonical);
  if (found == SemanticLabelByCanonicalLookup().end()) {
    return {std::string(label_text)};
  }
  std::vector<std::string> aliases{found->second->english_label};
  if (found->second->chinese_label != found->second->english_label) {
    aliases.push_back(found->second->chinese_label);
  }
  return aliases;
}

inline auto DefaultSemanticPhotographyLabels(
    SemanticLabelLanguage language = SemanticLabelLanguage::kEnglish) -> std::vector<std::string> {
  std::vector<std::string> labels;
  const auto&              queries = DefaultSemanticPhotographyLabelQueries(language);
  labels.reserve(queries.size());
  for (const auto& query : queries) {
    labels.push_back(query.label);
  }
  return labels;
}

}  // namespace alcedo
