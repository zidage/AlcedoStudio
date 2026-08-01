//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

/// @file editor_adjustment_transfer_real_project_e2e_test.cpp
/// @brief Replays Copy/Paste through production Main.qml on a packed project.

#include "ui/main_qml_test_fixture.hpp"

#include <gtest/gtest.h>

#include <QPoint>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>

namespace alcedo::ui::test {
namespace {

auto WaitUntil(const std::function<bool()>& predicate, int timeout_ms) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(50);
  }
  return predicate();
}

auto ProjectPathFromEnvironment() -> std::filesystem::path {
  const auto value = qEnvironmentVariable("ALCEDO_REAL_ADJUSTMENT_PROJECT");
  if (value.isEmpty()) {
    return {};
  }
  return std::filesystem::path(value.toStdWString());
}

auto FindCheckedKeys(const QVariantList& rows) -> QVariantList {
  QVariantList keys;
  for (const auto& value : rows) {
    const auto row = value.toMap();
    if (row.value(QStringLiteral("checked")).toBool()) {
      keys.push_back(row.value(QStringLiteral("key")).toString());
    }
  }
  return keys;
}

auto RowForKey(const QVariantList& rows, const QString& key) -> QVariantMap {
  for (const auto& value : rows) {
    const auto row = value.toMap();
    if (row.value(QStringLiteral("key")).toString() == key) {
      return row;
    }
  }
  return {};
}

auto ThumbnailPoint(const QQuickItem& thumbnail_grid, int index) -> QPoint {
  const auto columns = std::max(1, thumbnail_grid.property("columns").toInt());
  const auto cell_width = std::max(72, static_cast<int>(thumbnail_grid.width() / columns));
  const auto column = index % columns;
  const auto row = index / columns;
  return thumbnail_grid
      .mapToScene(QPointF(cell_width * column + 48, 48 + row * 96))
      .toPoint();
}

auto CenterOfItem(const QQuickItem& item) -> QPoint {
  return item.mapToScene(QPointF(item.width() * 0.5, item.height() * 0.5)).toPoint();
}

auto FindVisibleThumbnailGrid(QQuickWindow* window, int timeout_ms) -> QQuickItem* {
  QQuickItem* thumbnail_grid = nullptr;
  const auto found = WaitUntil(
      [&] {
        thumbnail_grid =
            window->findChild<QQuickItem*>(QStringLiteral("libraryThumbnailGridView"));
        return thumbnail_grid != nullptr && thumbnail_grid->isVisible() &&
               thumbnail_grid->width() > 0 && thumbnail_grid->height() > 0;
      },
      timeout_ms);
  return found ? thumbnail_grid : nullptr;
}

auto ClickEnabledItem(QQuickWindow* window, const QString& object_name) -> bool {
  auto* item = window->findChild<QQuickItem*>(object_name);
  if (item == nullptr || !item->isVisible() || !item->property("enabled").toBool()) {
    return false;
  }
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(*item));
  return true;
}

TEST_F(MainQmlTestFixture, RealPackedProjectCopyPasteReloadsToneSnapshot) {
  const auto source_path = ProjectPathFromEnvironment();
  if (source_path.empty() || !std::filesystem::is_regular_file(source_path)) {
    GTEST_SKIP() << "Set ALCEDO_REAL_ADJUSTMENT_PROJECT to the packed project used for this flow";
  }

  auto loaded = LoadMainWindowWithPackedProject(source_path);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_GE(loaded->host.library()->Thumbnails().size(), 2);

  // This regression exercises persisted adjustment state through the production
  // QML workflow. Frame contents are covered by the dedicated GPU E2E tests;
  // provide a lightweight presentation frame here so session close cannot wait
  // on an unrelated RAW decode after the panel assertions have completed.
  auto* render_scheduler = loaded->host.editor_session_scheduler();
  ASSERT_NE(render_scheduler, nullptr);
  render_scheduler->SetTestFrameProducer(
      [](alcedo::IFrameSink* sink, const alcedo::EditorRenderRequest&) {
        if (sink == nullptr) {
          return false;
        }
        const auto mapping = sink->MapResourceForWrite(alcedo::FrameMemoryDomain::HostVisible);
        if (mapping) {
          sink->UnmapResource();
        }
        sink->NotifyFrameReady(alcedo::FrameCompletionSubmission{});
        return true;
      });

  const auto first  = loaded->host.library()->Thumbnails().at(0).toMap();
  const auto second = loaded->host.library()->Thumbnails().at(1).toMap();
  const uint source_element = first.value(QStringLiteral("elementId")).toUInt();
  const uint source_image   = first.value(QStringLiteral("imageId")).toUInt();
  const uint target_element = second.value(QStringLiteral("elementId")).toUInt();
  const uint target_image   = second.value(QStringLiteral("imageId")).toUInt();
  ASSERT_GT(source_element, 0u);
  ASSERT_GT(source_image, 0u);
  ASSERT_GT(target_element, 0u);
  ASSERT_GT(target_image, 0u);
  ASSERT_NE(source_element, target_element);

  auto* window  = loaded->window;
  auto* session = loaded->host.editor_session();
  auto* context_menu = window->findChild<QObject*>(QStringLiteral("imageContextMenu"));
  ASSERT_NE(session, nullptr);
  ASSERT_NE(context_menu, nullptr);

  // Open the first image in the editor, then use the production Copy dialog.
  auto* source_grid = FindVisibleThumbnailGrid(window, 30000);
  ASSERT_NE(source_grid, nullptr);
  ProcessEvents(100);  // Let GridView lay out the first two real project delegates.
  QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, ThumbnailPoint(*source_grid, 0));
  ASSERT_TRUE(WaitUntil([&] {
    return session->has_image() && session->element_id() == source_element;
  }, 30000));
  QObject* source_exposure_model = nullptr;
  QObject* source_shadows_model = nullptr;
  QObject* source_highlights_model = nullptr;
  ASSERT_TRUE(WaitUntil([&] {
    source_exposure_model = window->findChild<QObject*>(QStringLiteral("toneExposureModel"));
    source_shadows_model = window->findChild<QObject*>(QStringLiteral("toneShadowsModel"));
    source_highlights_model =
        window->findChild<QObject*>(QStringLiteral("toneHighlightsModel"));
    return !session->adjustment_snapshot().isEmpty() &&
           source_exposure_model != nullptr && source_shadows_model != nullptr &&
           source_highlights_model != nullptr;
  }, 30000));
  const double source_panel_exposure = source_exposure_model->property("value").toDouble();
  const double source_panel_shadows = source_shadows_model->property("value").toDouble();
  const double source_panel_highlights = source_highlights_model->property("value").toDouble();

  // The transfer menu is owned by the library grid. Finish the source editor
  // session before replaying the grid's Copy/Paste context-menu workflow.
  auto* library_nav = window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  ASSERT_NE(library_nav, nullptr);
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(*library_nav));
  ASSERT_TRUE(WaitUntil([&] { return !session->has_image(); }, 30000));

  ASSERT_TRUE(window->property("backendInteractive").toBool());
  auto* thumbnail_grid = FindVisibleThumbnailGrid(window, 30000);
  ASSERT_NE(thumbnail_grid, nullptr);
  QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, ThumbnailPoint(*thumbnail_grid, 0));
  ASSERT_TRUE(WaitUntil([&] {
    return window->property("pendingAdjustmentSource").toMap().value("elementId").toUInt() ==
           source_element;
  }, 5000));
  ASSERT_TRUE(ClickEnabledItem(window, QStringLiteral("imageContextAction_copy-adjustments")));

  auto* dialog = window->findChild<QObject*>(QStringLiteral("adjustmentTransferDialog"));
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(dialog->property("visible").toBool() || dialog->property("opened").toBool());
  const auto source_versions = dialog->property("sourceVersions").toList();
  ASSERT_FALSE(source_versions.isEmpty());
  EXPECT_FALSE(dialog->property("selectedSourceVersionId").toString().isEmpty());

  const auto source_rows = dialog->property("adjustmentRows").toList();
  const auto source_exposure = RowForKey(source_rows, QStringLiteral("exposure"));
  const auto source_shadows = RowForKey(source_rows, QStringLiteral("shadows"));
  const auto source_highlights = RowForKey(source_rows, QStringLiteral("highlights"));
  ASSERT_FALSE(source_exposure.isEmpty());
  ASSERT_FALSE(source_shadows.isEmpty());
  ASSERT_FALSE(source_highlights.isEmpty());
  EXPECT_NE(source_exposure.value(QStringLiteral("value")).toString(),
            QStringLiteral("0.00"));
  EXPECT_NEAR(source_panel_exposure,
              source_exposure.value(QStringLiteral("value")).toDouble(), 0.02);
  EXPECT_DOUBLE_EQ(source_panel_shadows,
                   source_shadows.value(QStringLiteral("value")).toDouble());
  EXPECT_DOUBLE_EQ(source_panel_highlights,
                   source_highlights.value(QStringLiteral("value")).toDouble());

  ASSERT_FALSE(FindCheckedKeys(source_rows).isEmpty());
  ASSERT_TRUE(ClickEnabledItem(window, QStringLiteral("adjustmentTransferAcceptButton")));
  ASSERT_TRUE(WaitUntil([&] {
    return !dialog->property("visible").toBool() && !dialog->property("opened").toBool();
  }, 5000));

  auto* transfer = loaded->host.adjustment_transfer();
  ASSERT_NE(transfer, nullptr);
  ASSERT_TRUE(transfer->package_available());
  const auto copied_rows = transfer->package_summary();
  const auto copied_exposure = RowForKey(copied_rows, QStringLiteral("exposure"));
  ASSERT_FALSE(copied_exposure.isEmpty());
  EXPECT_EQ(copied_exposure.value(QStringLiteral("value")),
            source_exposure.value(QStringLiteral("value")));

  // Use the second image's actual context-menu path for Paste as well.
  QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, ThumbnailPoint(*thumbnail_grid, 1));
  ASSERT_TRUE(WaitUntil([&] {
    return window->property("pendingAdjustmentSource").toMap().value("elementId").toUInt() ==
           target_element;
  }, 5000));
  ASSERT_TRUE(ClickEnabledItem(window, QStringLiteral("imageContextAction_paste-adjustments")));
  ASSERT_TRUE(dialog->property("visible").toBool() || dialog->property("opened").toBool());
  EXPECT_EQ(dialog->property("mode").toString(), QStringLiteral("paste"));
  EXPECT_EQ(dialog->property("adjustmentRows").toList(), copied_rows);
  ASSERT_TRUE(ClickEnabledItem(window, QStringLiteral("adjustmentTransferAcceptButton")));
  ASSERT_TRUE(WaitUntil([&] {
    return !dialog->property("visible").toBool() && !dialog->property("opened").toBool();
  }, 5000));

  // Open the pasted target, then close it so the next open reads the persisted
  // target pipeline through the same history/session path as a user reopening it.
  ProcessEvents(100);
  QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, ThumbnailPoint(*thumbnail_grid, 1));
  ASSERT_TRUE(WaitUntil([&] {
    return session->has_image() && session->element_id() == target_element;
  }, 30000));
  ASSERT_TRUE(WaitUntil([&] {
    const auto history = session->history_snapshot();
    return std::ranges::any_of(history.versions, [](const auto& version) {
      return version.active && version.display_name == "Pasted Adjustments";
    });
  }, 5000))
      << "Library Paste did not publish its root-relative Version to the reopened editor";

  auto* target_exposure_model = window->findChild<QObject*>(QStringLiteral("toneExposureModel"));
  auto* target_shadows_model = window->findChild<QObject*>(QStringLiteral("toneShadowsModel"));
  auto* target_highlights_model =
      window->findChild<QObject*>(QStringLiteral("toneHighlightsModel"));
  ASSERT_NE(target_exposure_model, nullptr);
  ASSERT_NE(target_shadows_model, nullptr);
  ASSERT_NE(target_highlights_model, nullptr);
  ASSERT_TRUE(WaitUntil([&] {
    return std::abs(target_exposure_model->property("value").toDouble() - source_panel_exposure) <
               0.02 &&
           target_shadows_model->property("value").toDouble() == source_panel_shadows &&
           target_highlights_model->property("value").toDouble() == source_panel_highlights;
  }, 30000));
  library_nav = window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  ASSERT_NE(library_nav, nullptr);
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(*library_nav));
  ASSERT_TRUE(WaitUntil([&] { return !session->has_image(); }, 30000));
  auto* reopened_grid = FindVisibleThumbnailGrid(window, 30000);
  ASSERT_NE(reopened_grid, nullptr);
  ProcessEvents(100);
  QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, ThumbnailPoint(*reopened_grid, 1));
  ASSERT_TRUE(WaitUntil([&] {
    return session->has_image() && session->element_id() == target_element;
  }, 30000));

  auto* exposure_model = window->findChild<QObject*>(QStringLiteral("toneExposureModel"));
  auto* shadows_model = window->findChild<QObject*>(QStringLiteral("toneShadowsModel"));
  auto* highlights_model = window->findChild<QObject*>(QStringLiteral("toneHighlightsModel"));
  ASSERT_NE(exposure_model, nullptr);
  ASSERT_NE(shadows_model, nullptr);
  ASSERT_NE(highlights_model, nullptr);

  ASSERT_TRUE(WaitUntil([&] { return !session->adjustment_snapshot().isEmpty(); }, 5000));
  const auto reopened_snapshot = session->adjustment_snapshot();
  ASSERT_FALSE(reopened_snapshot.isEmpty())
      << "The reopened session published no adjustment snapshot";
  ASSERT_TRUE(reopened_snapshot.contains(QStringLiteral("exposure")));
  ASSERT_TRUE(reopened_snapshot.contains(QStringLiteral("shadows")));
  ASSERT_TRUE(reopened_snapshot.contains(QStringLiteral("highlights")));
  EXPECT_DOUBLE_EQ(
      reopened_snapshot.value(QStringLiteral("exposure")).toMap().value("exposure").toDouble(),
                   exposure_model->property("value").toDouble());
  EXPECT_DOUBLE_EQ(
      reopened_snapshot.value(QStringLiteral("shadows")).toMap().value("shadows").toDouble(),
                   shadows_model->property("value").toDouble());
  EXPECT_DOUBLE_EQ(
      reopened_snapshot.value(QStringLiteral("highlights")).toMap().value("highlights").toDouble(),
      highlights_model->property("value").toDouble());
  EXPECT_NEAR(exposure_model->property("value").toDouble(),
              source_exposure.value(QStringLiteral("value")).toDouble(), 0.02);
  EXPECT_DOUBLE_EQ(shadows_model->property("value").toDouble(),
                   source_shadows.value(QStringLiteral("value")).toDouble());
  EXPECT_DOUBLE_EQ(highlights_model->property("value").toDouble(),
                   source_highlights.value(QStringLiteral("value")).toDouble());

  const auto version_count_before_editor_paste = session->history_snapshot().versions.size();
  const auto editor_paste = transfer->PasteIntoEditor(session);
  ASSERT_TRUE(editor_paste.value(QStringLiteral("success")).toBool())
      << editor_paste.value(QStringLiteral("message")).toString().toStdString();
  ASSERT_TRUE(WaitUntil([&] {
    return session->can_edit() &&
           session->history_snapshot().versions.size() ==
               version_count_before_editor_paste + 1;
  }, 30000));
  EXPECT_FALSE(session->last_history_failed())
      << session->last_history_message().toStdString();
  EXPECT_FALSE(session->last_error().contains(
      QStringLiteral("mini-Git journal fold does not match")));
}

}  // namespace
}  // namespace alcedo::ui::test
