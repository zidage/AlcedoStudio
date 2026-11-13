//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_ai_mask_model_set_test.cpp
/// @brief AI Models plan, AM5: the AI Mask tool reads the installed AI Mask model set before it
///        starts the sidecar.
///
/// The production EditorAiMaskTool runs on the production EditorSessionController and session
/// facade. Without a complete installed set of a supported version, Open shows the state of the
/// set (not installed, update required, repair) and requests no sidecar runtime and no AI Mask
/// edit. A usable set opens the edit; each Open reads the set again.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <system_error>

#include "app/ai_model_package_install.hpp"
#include "app/ai_model_package_service.hpp"
#include "app/ai_sidecar_runtime_service.hpp"
#include "app/editor_mask_coverage_preview_test_support.hpp"
#include "ui/alcedo_main/album_backend/editor_ai_mask_tool.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::ui {
namespace {

namespace fs = std::filesystem;
using namespace coverage_preview_test;

/// Feed trust state and statement acceptance in memory: the readiness read uses neither.
class MemoryPreferences final : public AiModelPackagePreferences {
 public:
  [[nodiscard]] auto LoadTrustedSequence() const -> quint64 override { return 1; }
  void               SaveTrustedSequence(quint64 /*sequence*/) override {}
  [[nodiscard]] auto LoadAcceptedStatementVersion() const -> quint64 override { return 0; }
  void               SaveAcceptedStatementVersion(quint64 /*version*/) override {}
};

class EditorAiMaskModelSetTest : public EditorMaskCoveragePreviewTest {
 protected:
  void SetUp() override {
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("EditorAiMaskModelSetTest"));
    EditorMaskCoveragePreviewTest::SetUp();
    std::random_device device;
    models_ = fs::current_path() / "editor_ai_mask_model_set_test" / std::to_string(device());
    fs::create_directories(models_);
    models_     = fs::weakly_canonical(models_);
    controller_ = std::make_unique<EditorSessionController>(service_);
    // The AI Model package service of the application over the test model directory. Without a
    // feed URL it is disabled: it reads the local installations only.
    AiModelPackageServiceOptions options;
    options.model_directory = [models = models_] { return models; };
    options.preferences     = std::make_unique<MemoryPreferences>();
    packages_               = std::make_unique<AiModelPackageService>(std::move(options));
    read_model_set_         = [this] { return packages_->ReadAiMaskModelSet(); };
    tool_                   = std::make_unique<EditorAiMaskTool>(
        controller_.get(), EditorAiMaskTool::Services{[this] {
                                                        ++runtime_requests_;
                                                        return ai_runtime_;
                                                      },
                                                                        {},
                                                      [this] { return read_model_set_(); }});
  }

  void TearDown() override {
    if (tool_) {
      QCoreApplication::processEvents();
      tool_->cancel();
      if (service_->ai_mask_edit_open()) {
        (void)service_->CancelAiMaskEdit();
      }
      CompleteFrames();
      QCoreApplication::processEvents();
      tool_->Shutdown();
      tool_.reset();
    }
    packages_.reset();
    nodes_.set_editor_session(nullptr);
    controller_.reset();
    EditorMaskCoveragePreviewTest::TearDown();
    std::error_code error;
    fs::remove_all(models_.parent_path(), error);
  }

  /// Open the image in the editor and select its Color Grade node, as the header AI Mask button
  /// requires.
  void OpenImageWithSelectedGrade() {
    controller_->Open(kElementId, kImageId);
    CompleteFrames();
    QCoreApplication::processEvents();
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
    nodes_.set_editor_session(controller_.get());
    nodes_.selectNode(QString::fromStdString(std::string{GradeId().Value()}));
    QCoreApplication::processEvents();
    ASSERT_EQ(nodes_.selected_node_kind(), QStringLiteral("colorGrade"));
  }

  /// Opening stopped before the sidecar: the panel shows @p state, nothing else started.
  void ExpectStoppedBeforeTheSidecar(const QString& state) {
    EXPECT_TRUE(tool_->active()) << "the panel shows the model set state";
    EXPECT_EQ(tool_->model_set_state(), state);
    EXPECT_FALSE(tool_->ready());
    EXPECT_FALSE(tool_->busy());
    EXPECT_TRUE(tool_->error_text().isEmpty()) << tool_->error_text().toStdString();
    EXPECT_EQ(runtime_requests_, 0) << "no sidecar runtime was requested";
    Drain();
    QCoreApplication::processEvents();
    EXPECT_FALSE(service_->ai_mask_edit_open()) << "no AI Mask edit opened";
    EXPECT_TRUE(controller_->can_edit()) << "no AI Mask edit lease holds the document";
  }

  fs::path                                 models_;
  std::unique_ptr<EditorSessionController> controller_;
  EditorNodeController                     nodes_;
  std::unique_ptr<AiModelPackageService>   packages_;
  std::function<AiMaskModelSetReadiness()> read_model_set_;
  int                                      runtime_requests_ = 0;
  std::shared_ptr<AiSidecarRuntimeService> ai_runtime_ =
      std::make_shared<AiSidecarRuntimeService>();
  std::unique_ptr<EditorAiMaskTool> tool_;
};

TEST_F(EditorAiMaskModelSetTest, NoInstalledSetShowsNotInstalledAndStartsNoSidecar) {
  OpenImageWithSelectedGrade();

  tool_->open();
  ExpectStoppedBeforeTheSidecar(QStringLiteral("notInstalled"));

  // Close dismisses the state; the tool is ready for the next Open.
  tool_->cancel();
  EXPECT_FALSE(tool_->active());
  EXPECT_TRUE(tool_->model_set_state().isEmpty());
  EXPECT_EQ(runtime_requests_, 0);
}

TEST_F(EditorAiMaskModelSetTest, SetBelowTheMinimumShowsUpdateRequiredAndStartsNoSidecar) {
  OpenImageWithSelectedGrade();
  // A build that supports versions 2-3 finds only version 1 in the model directory.
  fs::create_directories(models_ / AiModelPackageDirectoryName(kAiMaskModelPackageId, 1));
  read_model_set_ = [this] {
    return AiMaskModelSetReadinessFor(ReadAiModelInstallation(models_, kAiMaskModelPackageId, 2, 3),
                                      AiModelPackageStatus::kUpdateRequired);
  };

  tool_->open();
  ExpectStoppedBeforeTheSidecar(QStringLiteral("updateRequired"));
}

TEST_F(EditorAiMaskModelSetTest, IncompleteSetShowsRepairAndEachOpenReadsTheSetAgain) {
  OpenImageWithSelectedGrade();
  // A supported version directory without its receipt and inventory (a developer copy, or an
  // interrupted manual change): the set is not complete.
  fs::create_directories(
      models_ / AiModelPackageDirectoryName(kAiMaskModelPackageId, kAiMaskModelSetMinimumVersion));

  tool_->open();
  ExpectStoppedBeforeTheSidecar(QStringLiteral("repairRequired"));
  tool_->cancel();

  // A usable set: the next Open passes its directory on and opens the AI Mask edit.
  const fs::path installed = models_ / "installed-set";
  read_model_set_          = [installed] {
    return AiMaskModelSetReadiness{AiMaskModelSetAvailability::kReady, installed};
  };
  tool_->open();
  EXPECT_TRUE(tool_->active());
  EXPECT_TRUE(tool_->model_set_state().isEmpty());
  EXPECT_EQ(runtime_requests_, 1);
  Drain();
  EXPECT_TRUE(service_->ai_mask_edit_open());
}

}  // namespace
}  // namespace alcedo::ui
