//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Film sources of the Tone Curve presets (tone_curve_preset_study.md, section 3.4): the neutral
// tone of Kodak 2383 through the reference negative 5207, Velvia 50, and Ektachrome E100, from
// the point stages of the film print render (FilmPrintPointReference) on an AP1 neutral ramp.
//
// The study script (scripts/fit_tone_curve_presets.py) runs this test and reads the lines that
// start with `tone_curve_film_tone`. The test also checks that the film resources still give the
// tone that the study fitted.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "edit/runtime/film/film_print_tables.hpp"
#include "edit/runtime/film/film_stock_bundle.hpp"
#include "film_print_reference.hpp"
#include "tone_curve_preset_study_data.hpp"

namespace alcedo {
namespace {

#if defined(ALCEDO_FILM_PRINT_RESOURCE_DIR)

namespace fs = std::filesystem;

/// One film source: the preset id, the negative or colour reversal film, and the print.
struct FilmSource {
  std::string_view preset_id;
  std::string_view negative;
  std::string_view print_material;  ///< Empty for a colour reversal film
};

constexpr std::array<FilmSource, 3> kFilmSources{{
    {"kodak_2383", "kodak_5207", "kodak_2383"},
    {"fuji_velvia_50", "fuji_velvia_50", ""},
    {"kodak_ektachrome_e100", "kodak_ektachrome_e100", ""},
}};

auto                                SrgbDecode(double code) -> double {
  return code <= 0.04045 ? code / 12.92 : std::pow((code + 0.055) / 1.055, 2.4);
}

/// Display luminance (Rec. 709 Y of the decoded display RGB, white 1) of the AP1 neutral
/// @p scene.
auto NeutralTone(const film_print_reference::FilmPrintPointReference& reference, double scene)
    -> double {
  const auto value   = static_cast<float>(scene);
  const auto encoded = reference.Srgb(value, value, value);
  return 0.2126 * SrgbDecode(encoded.r) + 0.7152 * SrgbDecode(encoded.g) +
         0.0722 * SrgbDecode(encoded.b);
}

class ToneCurvePresetFilmSourceTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    bundle_ = FilmStockBundle::Open(fs::path(ALCEDO_FILM_PRINT_RESOURCE_DIR));
    tables_ = std::make_unique<FilmPrintTables>(bundle_);
  }

  static void TearDownTestSuite() {
    tables_.reset();
    bundle_.reset();
  }

  static inline std::shared_ptr<const FilmStockBundle> bundle_;
  static inline std::unique_ptr<FilmPrintTables>       tables_;
};

TEST_F(ToneCurvePresetFilmSourceTest, NeutralToneOfEachFilmIsTheToneOfTheStudy) {
  // Scene light `reference`: the grey anchor of each film, so the neutral ramp stays neutral. As
  // Shot without a RAW white renders with this light too.
  const std::string light = "reference";
  for (const auto& source : kFilmSources) {
    SCOPED_TRACE(std::string{source.preset_id});
    const std::string negative{source.negative};
    const std::string print{source.print_material};
    const auto        tables = tables_->Resolve(negative, print, light);
    const film_print_reference::FilmPrintPointReference reference(*bundle_, *tables, negative,
                                                                  print, light);
    const auto* study         = tone_curve_study::FindFilmSource(source.preset_id);
    double      largest_stops = 0.0;
    for (int i = 0; i < tone_curve_study::kFilmSampleCount; ++i) {
      const double ev =
          tone_curve_study::kFilmSampleFirstEv + i * tone_curve_study::kFilmSampleStepEv;
      const double scene = 0.18 * std::exp2(ev);
      const double tone  = NeutralTone(reference, scene);
      std::cout << "tone_curve_film_tone " << source.preset_id << " " << std::setprecision(9) << ev
                << " " << tone << "\n";
      if (study != nullptr) {
        largest_stops =
            std::max(largest_stops,
                     std::abs(std::log2(std::max(tone, 1e-9) /
                                        std::max(study->tone[static_cast<std::size_t>(i)], 1e-9))));
      }
    }
    EXPECT_NE(study, nullptr) << "The study has no tone of this film.";
    // The study keeps 9 significant digits of each tone.
    EXPECT_LT(largest_stops, 1e-6);
  }
}

#else

TEST(ToneCurvePresetFilmSourceTest, FilmResourcesAreConfigured) {
  GTEST_SKIP() << "ALCEDO_FILM_PRINT_RESOURCE_DIR is not configured, so the film sources of the "
                  "Tone Curve presets were not checked.";
}

#endif

}  // namespace
}  // namespace alcedo
