//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <utility>

#include "app/editor_render_coordinator.hpp"
#include "edit/frame_presentation_types.hpp"
#include "support/harness_completing_pipeline_scheduler_port.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"
#include "ui/alcedo_main/application_module_qml_types.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/main_qml_test_fixture.hpp"

namespace alcedo::ui::test {
namespace {

using EditorWorkspaceCropShortcutQmlTest = MainQmlTestFixture;

TEST_F(EditorWorkspaceCropShortcutQmlTest, EnterKeysCommitNumericInputAndReturnFromViewport) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, PathToQString(temp_dir_));
  QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
  RegisterApplicationModuleTypes();

  const auto photos = WritePngPhotos(temp_dir_, "geometry-shortcut-", 1);
  ASSERT_EQ(photos.size(), 1u);
  auto loaded = LoadMainWindowFromModule();
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_TRUE(ImportAndWait(loaded->host, photos));
  const auto thumbnails = loaded->host.library()->Thumbnails();
  ASSERT_EQ(thumbnails.size(), 1);
  const auto photo      = thumbnails.front().toMap();
  const auto element_id = photo.value(QStringLiteral("elementId")).toUInt();
  const auto image_id   = photo.value(QStringLiteral("imageId")).toUInt();
  ASSERT_GT(element_id, 0u);
  ASSERT_GT(image_id, 0u);

  auto* session     = loaded->host.editor_session();
  auto* coordinator = loaded->host.editor_render_coordinator();
  ASSERT_NE(session, nullptr);
  ASSERT_NE(coordinator, nullptr);
  // The interaction test uses the real session and leaves pixel rendering to the render suites.
  auto scheduler = std::make_shared<alcedo::test::HarnessCompletingPipelineSchedulerPort>();
  scheduler->SetSinkResolver([session] { return session->presentation_frame_sink(); });
  scheduler->SetFrameProducer([](IFrameSink* sink, const EditorRenderRequest&) {
    const auto mapping = sink->MapResourceForWrite(FrameMemoryDomain::HostVisible);
    if (mapping) {
      sink->UnmapResource();
    }
    // WritePngPhotos creates a 48 by 32 photograph. Crop validation uses its full source extent.
    sink->NotifyFrameReady(FrameCompletionSubmission{
        .geometry = MaskEditGeometry::MakeIdentityPhotographGeometry({48, 32}),
    });
    return true;
  });
  coordinator->SetPipelineSchedulerPort(std::move(scheduler));

  loaded->host.workspace_router()->OpenEditor(element_id, image_id);
  ASSERT_TRUE(QTest::qWaitFor(
      [&] { return session->element_id() == element_id && session->can_edit(); }, 15000));

  auto* window    = loaded->window;
  auto* workspace = window->findChild<QQuickItem*>(QStringLiteral("editorWorkspace"));
  ASSERT_NE(workspace, nullptr);
  auto* viewport =
      workspace->findChild<QQuickItem*>(QStringLiteral("editorViewportInteractionLayer"));
  auto* slider = workspace->findChild<QQuickItem*>(QStringLiteral("geometryRotationSlider"));
  auto* panel = workspace->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentPanel_geometry"));
  ASSERT_NE(viewport, nullptr);
  ASSERT_NE(slider, nullptr);
  ASSERT_NE(panel, nullptr);
  auto* rotation =
      workspace->findChild<EditorAdjustmentValueModel*>(QStringLiteral("geometryRotationModel"));
  ASSERT_NE(rotation, nullptr);
  QSignalSpy settled(rotation, &EditorAdjustmentValueModel::settledCommitted);
  QSignalSpy entered(slider, SIGNAL(valueEntered(double)));
  ASSERT_TRUE(settled.isValid());
  ASSERT_TRUE(entered.isValid());

  const QString geometry   = QStringLiteral("geometry");
  const QString tone       = QStringLiteral("tone");
  const QString crop_scope = QLatin1String(shortcut_scope::kEditorCropEdit);
  for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
    SCOPED_TRACE(key == Qt::Key_Enter ? "Keypad Enter" : "Return");
    const auto   modifiers = key == Qt::Key_Enter ? Qt::KeypadModifier : Qt::NoModifier;
    const double angle     = key == Qt::Key_Enter ? 10.0 : 5.0;
    session->set_active_adjustment_panel(geometry);
    QQuickItem* numeric_input = nullptr;
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
          numeric_input =
              slider->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderValueInput"));
          return numeric_input && slider->isVisible() && slider->isEnabled() &&
                 panel->property("hasSourceSize").toBool();
        },
        10000))
        << "Numeric input found: " << (numeric_input != nullptr)
        << ", slider visible: " << slider->isVisible()
        << ", slider enabled: " << slider->isEnabled()
        << ", source extent available: " << panel->property("hasSourceSize").toBool();
    ASSERT_EQ(session->active_adjustment_panel(), geometry);

    numeric_input->forceActiveFocus();
    ASSERT_EQ(window->activeFocusItem(), numeric_input);
    ASSERT_TRUE(workspace->property("cropEditShortcutScope").toString().isEmpty());
    ASSERT_TRUE(numeric_input->setProperty("text", QString::number(angle)));
    QSignalSpy accepted(numeric_input, SIGNAL(accepted()));
    ASSERT_TRUE(accepted.isValid());
    entered.clear();
    settled.clear();
    QTest::keyClick(window, key, modifiers);
    ASSERT_EQ(accepted.size(), 1);
    ASSERT_EQ(entered.size(), 1);
    EXPECT_DOUBLE_EQ(entered.first().first().toDouble(), angle);
    EXPECT_EQ(settled.size(), 1);
    ASSERT_EQ(session->active_adjustment_panel(), geometry);
    EXPECT_NE(window->activeFocusItem(), numeric_input);

    viewport->forceActiveFocus();
    ASSERT_EQ(window->activeFocusItem(), viewport);
    ASSERT_EQ(workspace->property("cropEditShortcutScope").toString(), crop_scope);
    QTest::keyClick(window, key, modifiers);
    ASSERT_TRUE(QTest::qWaitFor([&] { return session->active_adjustment_panel() == tone; }, 2000));
    EXPECT_EQ(accepted.size(), 1);
    EXPECT_EQ(entered.size(), 1);
    EXPECT_EQ(settled.size(), 1);
  }

  session->Finalize(false);
  EXPECT_TRUE(QTest::qWaitFor([&] { return !session->has_image(); }, 10000));
}

}  // namespace
}  // namespace alcedo::ui::test
