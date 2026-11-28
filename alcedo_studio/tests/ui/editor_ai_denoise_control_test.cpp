//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_ai_denoise_control_test.cpp
/// @brief The AI Denoise switch of the editor (integration plan, DN7): the write of the switch,
///        the missing model, the platform check, and the sensor layout of the open photo.

#include "ui/alcedo_main/album_backend/editor_ai_denoise_control.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <atomic>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "app/editor_parameter_write.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/input/ai_denoise_error.hpp"

namespace alcedo::ui {
namespace {

const std::string        kVersion(64, 'd');

class RecordingSubmitter final : public IEditorAdjustmentSubmitter {
 public:
  struct Call {
    QString              field_key;
    EditorParameterWrite write;
    bool                 settled = false;
  };

  auto submitWrite(QString fieldKey, EditorParameterWrite write, bool settled) -> bool override {
    if (!accepts) {
      return false;
    }
    calls.push_back({std::move(fieldKey), std::move(write), settled});
    return true;
  }
  auto submitPatch(QString /*fieldKey*/, QString /*paramsJson*/, bool /*settled*/)
      -> bool override {
    return false;
  }
  auto              canEdit() const -> bool override { return accepts; }

  std::vector<Call> calls;
  bool              accepts = true;
};

/// The AI Denoise value of the raw_decode update of @p call; fails the test for another write.
auto SwitchOf(const RecordingSubmitter::Call& call)
    -> std::optional<std::optional<DevelopAiDenoise>> {
  EXPECT_EQ(call.field_key, QStringLiteral("raw_decode"));
  EXPECT_TRUE(call.settled);
  const auto* update = std::get_if<DevelopRawDecodeUpdate>(&call.write);
  if (update == nullptr) {
    ADD_FAILURE() << "the write is not a raw_decode update";
    return std::nullopt;
  }
  // Only the switch: the method and the highlight reconstruction are not written.
  EXPECT_FALSE(update->demosaic_method.has_value());
  EXPECT_FALSE(update->highlights_reconstruct.has_value());
  return update->ai_denoise;
}

/// Waits (event loop) until the sensor layout of @p control is @p layout. QTRY_COMPARE does not
/// fail a gtest on its timeout.
auto LayoutBecomes(const EditorAiDenoiseControl& control, const char* layout) -> bool {
  return QTest::qWaitFor([&] { return control.sensor_layout() == QLatin1String(layout); }, 5000);
}

auto Services(std::function<std::string()> version) -> EditorAiDenoiseControl::Services {
  EditorAiDenoiseControl::Services services;
  services.platform_available      = [] { return true; };
  services.installed_model_version = std::move(version);
  return services;
}

TEST(EditorAiDenoiseControlTest, TurnOnWritesTheInstalledVersionAndTurnOffWritesNull) {
  RecordingSubmitter     submitter;
  EditorAiDenoiseControl control(Services([] { return kVersion; }), &submitter);

  EXPECT_EQ(control.turnOn(), QStringLiteral("on"));
  ASSERT_EQ(submitter.calls.size(), 1U);
  const auto on = SwitchOf(submitter.calls[0]);
  ASSERT_TRUE(on.has_value() && on->has_value());
  EXPECT_EQ((*on)->model_version, kVersion);

  EXPECT_TRUE(control.turnOff());
  ASSERT_EQ(submitter.calls.size(), 2U);
  const auto off = SwitchOf(submitter.calls[1]);
  ASSERT_TRUE(off.has_value());
  EXPECT_FALSE(off->has_value());
}

TEST(EditorAiDenoiseControlTest, TurnOnWithoutAModelRequestsTheAiModelsPageAndWritesNothing) {
  RecordingSubmitter     submitter;
  EditorAiDenoiseControl control(Services([]() -> std::string {
                                   throw AiDenoiseError(AiDenoiseErrorKind::ModelNotInstalled,
                                                        "AI Denoise: the model is not installed");
                                 }),
                                 &submitter);
  EXPECT_EQ(control.turnOn(), QStringLiteral("model_not_installed"));
  EXPECT_TRUE(submitter.calls.empty());
}

TEST(EditorAiDenoiseControlTest, TurnOnIsRefusedWithoutAPlatformOrAReadableModel) {
  RecordingSubmitter submitter;
  auto               no_platform = Services([] { return kVersion; });
  no_platform.platform_available = [] { return false; };
  EditorAiDenoiseControl without_platform(std::move(no_platform), &submitter);
  EXPECT_FALSE(without_platform.available());
  EXPECT_EQ(without_platform.turnOn(), QStringLiteral("refused"));

  EditorAiDenoiseControl unreadable(Services([]() -> std::string {
                                      throw AiDenoiseError(AiDenoiseErrorKind::RunFailed,
                                                           "AI Denoise: the graph cannot be read");
                                    }),
                                    &submitter);
  EXPECT_EQ(unreadable.turnOn(), QStringLiteral("refused"));

  // The session refuses input (no image, or not Interactive).
  submitter.accepts = false;
  EditorAiDenoiseControl refused(Services([] { return kVersion; }), &submitter);
  EXPECT_EQ(refused.turnOn(), QStringLiteral("refused"));
  EXPECT_FALSE(refused.turnOff());
  EXPECT_TRUE(submitter.calls.empty());
}

// DN13: the write of `denoise.set` of alcedo-cli comes from the control, without a submit.
TEST(EditorAiDenoiseControlTest, PrepareTurnOnGivesWriteOfInstalledVersionWithoutSubmit) {
  RecordingSubmitter     submitter;
  EditorAiDenoiseControl control(Services([] { return kVersion; }), &submitter);

  auto                   preparation = control.PrepareTurnOn();
  EXPECT_FALSE(preparation.refusal.has_value());
  ASSERT_TRUE(preparation.write.has_value());
  const RecordingSubmitter::Call call{QStringLiteral("raw_decode"), *preparation.write, true};
  const auto                     on = SwitchOf(call);
  ASSERT_TRUE(on.has_value() && on->has_value());
  EXPECT_EQ((*on)->model_version, kVersion);

  const RecordingSubmitter::Call off_call{QStringLiteral("raw_decode"),
                                          EditorAiDenoiseControl::TurnOffWrite(), true};
  const auto                     off = SwitchOf(off_call);
  ASSERT_TRUE(off.has_value());
  EXPECT_FALSE(off->has_value());
  EXPECT_TRUE(submitter.calls.empty());
}

TEST(EditorAiDenoiseControlTest, PrepareTurnOnRefusesPlatformBeforeReadingModel) {
  RecordingSubmitter submitter;
  int                reads    = 0;
  auto               services = Services([&reads] {
    ++reads;
    return kVersion;
  });
  services.platform_available = [] { return false; };
  EditorAiDenoiseControl control(std::move(services), &submitter);

  const auto             preparation = control.PrepareTurnOn();
  ASSERT_TRUE(preparation.refusal.has_value());
  EXPECT_EQ(*preparation.refusal, EditorAiDenoiseControl::TurnOnRefusal::PlatformUnavailable);
  EXPECT_FALSE(preparation.write.has_value());
  EXPECT_FALSE(preparation.reason.empty());
  EXPECT_EQ(reads, 0);
  // The installed model is still readable for denoise.status.
  EXPECT_EQ(control.ReadInstalledModel().version, kVersion);
  EXPECT_EQ(reads, 1);
}

TEST(EditorAiDenoiseControlTest, ReadInstalledModelReportsMissingAndUnreadableModels) {
  RecordingSubmitter     submitter;
  EditorAiDenoiseControl missing(Services([]() -> std::string {
                                   throw AiDenoiseError(AiDenoiseErrorKind::ModelNotInstalled,
                                                        "AI Denoise: the model is not installed");
                                 }),
                                 &submitter);
  const auto             not_installed = missing.ReadInstalledModel();
  ASSERT_TRUE(not_installed.refusal.has_value());
  EXPECT_EQ(*not_installed.refusal, EditorAiDenoiseControl::TurnOnRefusal::ModelNotInstalled);
  EXPECT_EQ(not_installed.reason, "AI Denoise: the model is not installed");
  EXPECT_TRUE(not_installed.version.empty());
  EXPECT_EQ(missing.PrepareTurnOn().refusal,
            std::optional{EditorAiDenoiseControl::TurnOnRefusal::ModelNotInstalled});

  EditorAiDenoiseControl unreadable(
      Services([]() -> std::string { throw std::runtime_error("disk error"); }), &submitter);
  const auto failed = unreadable.ReadInstalledModel();
  ASSERT_TRUE(failed.refusal.has_value());
  EXPECT_EQ(*failed.refusal, EditorAiDenoiseControl::TurnOnRefusal::ModelUnreadable);
  EXPECT_EQ(failed.reason, "AI Denoise: disk error");
  EXPECT_TRUE(submitter.calls.empty());
}

TEST(EditorAiDenoiseControlTest, PlatformIsCheckedOnce) {
  RecordingSubmitter submitter;
  int                checks   = 0;
  auto               services = Services([] { return kVersion; });
  services.platform_available = [&checks] {
    ++checks;
    return true;
  };
  EditorAiDenoiseControl control(std::move(services), &submitter);
  EXPECT_TRUE(control.available());
  EXPECT_TRUE(control.available());
  EXPECT_EQ(control.turnOn(), QStringLiteral("on"));
  EXPECT_EQ(checks, 1);
}

TEST(EditorAiDenoiseControlTest, SensorLayoutOfTheOpenPhotoIsReadOnAWorkerThread) {
  RecordingSubmitter submitter;
  auto               services = Services([] { return kVersion; });
  services.image_path         = [](std::uint32_t image_id) -> std::filesystem::path {
    switch (image_id) {
      case 1:
        return "bayer.ARW";
      case 2:
        return "xtrans.RAF";
      case 3:
        return "unreadable.ARW";
      default:
        return {};
    }
  };
  services.read_sensor_layout = [](const std::filesystem::path& path) {
    if (path == "bayer.ARW") return RawSensorLayout::Bayer;
    if (path == "xtrans.RAF") return RawSensorLayout::XTrans;
    throw std::runtime_error("LibRaw cannot open " + path.string());
  };
  EditorAiDenoiseControl control(std::move(services), &submitter);
  EXPECT_TRUE(control.sensor_layout().isEmpty());

  control.SetImage(2);
  ASSERT_TRUE(LayoutBecomes(control, "xtrans"));
  control.SetImage(1);
  // Until the layout of the new photo arrives, the layout is unknown (the switch is enabled).
  EXPECT_TRUE(control.sensor_layout().isEmpty());
  ASSERT_TRUE(LayoutBecomes(control, "bayer"));

  // A file that cannot be read keeps the layout unknown.
  QSignalSpy changed(&control, &EditorAiDenoiseControl::SensorChanged);
  control.SetImage(3);
  EXPECT_TRUE(control.sensor_layout().isEmpty());
  QTest::qWait(200);
  EXPECT_TRUE(control.sensor_layout().isEmpty());
  EXPECT_EQ(changed.count(), 1);
}

TEST(EditorAiDenoiseControlTest, LayoutOfAPhotoThatIsNoLongerOpenIsIgnored) {
  RecordingSubmitter submitter;
  std::atomic<bool>  release{false};
  auto               services = Services([] { return kVersion; });
  services.image_path         = [](std::uint32_t image_id) -> std::filesystem::path {
    return image_id == 1 ? "slow_xtrans.RAF" : "bayer.ARW";
  };
  services.read_sensor_layout = [&release](const std::filesystem::path& path) {
    if (path == "slow_xtrans.RAF") {
      while (!release.load()) {
        QThread::msleep(5);
      }
      return RawSensorLayout::XTrans;
    }
    return RawSensorLayout::Bayer;
  };
  EditorAiDenoiseControl control(std::move(services), &submitter);
  control.SetImage(1);
  control.SetImage(4);
  ASSERT_TRUE(LayoutBecomes(control, "bayer"));
  release = true;
  QTest::qWait(200);
  EXPECT_EQ(control.sensor_layout(), QStringLiteral("bayer"));
}

}  // namespace
}  // namespace alcedo::ui
