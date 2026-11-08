//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Production QML of the Library inspector button row (#731): LibraryInspectorActions.qml with
// an InspectorPanel and a SelectionState, loaded from the source tree against a real packed
// project. The tests emit the buttons' clicked signals; they do not deliver synthetic pointer
// input.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QString>
#include <QUrl>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/export_queue.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/library_selection.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

using LibraryInspectorActionsQmlTests = ApplicationModuleHostTestFixture;

// The Library inspector column of LibraryWorkspace.qml: the InspectorPanel above the button row,
// with the same bindings to the selection and the backend as Main.qml gives LibraryWorkspace.
constexpr char kHarnessQml[]          = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 420
    height: 900
    visible: true
    property alias inspector: inspectorPanel
    property alias actions: inspectorActions

    SelectionState {
        id: selectionStateObj
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        InspectorPanel {
            id: inspectorPanel
            Layout.fillWidth: true
            Layout.fillHeight: true
            interactionPolicy: appModules.interactionPolicy
            selectionState: selectionStateObj
            selectedCount: selectionStateObj.selectedCount
        }

        LibraryInspectorActions {
            id: inspectorActions
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            inspectorPanel: inspectorPanel
            selectionState: selectionStateObj
            selectedCount: selectionStateObj.selectedCount
            backendInteractive: appModules.project.serviceReady
            exportQueueCount: appModules.exportQueue.count
        }
    }
}
)";

constexpr int  kAlbumPage             = 0;
constexpr int  kImagePage             = 1;
constexpr int  kExportPage            = 2;

// A harness URL in the production QML directory, so InspectorPanel, LibraryInspectorActions,
// and SelectionState resolve as sibling files.
auto           HarnessUrl() -> QUrl {
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" /
                    "LibraryInspectorActionsHarness.qml";
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(path.string()));
#endif
}

auto WaitUntil(const std::function<bool()>& predicate, int timeoutMs = 15000) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(10);
  }
  return predicate();
}

auto Joined(const std::vector<std::string>& lines) -> std::string {
  std::string out;
  for (const auto& line : lines) {
    out += line + "\n";
  }
  return out;
}

/// The inspector column of the Library workspace with a loaded project.
class InspectorColumn {
 public:
  explicit InspectorColumn(ApplicationModuleHost& backend) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    AppTheme::RegisterFonts();
    RegisterShortcutRegistryQmlType();
    engine_.addImportPath(QStringLiteral("qrc:/"));
    backend.AttachQmlEngine(&engine_);
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), &backend);
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& list) {
      for (const auto& warning : list) {
        warnings_.push_back(warning.toString().toStdString());
      }
    });
    engine_.loadData(QByteArray{kHarnessQml}, HarnessUrl());
  }

  auto window() -> QObject* {
    return engine_.rootObjects().empty() ? nullptr : engine_.rootObjects().front();
  }
  auto inspector() -> QObject* { return Property("inspector"); }
  auto actions() -> QQuickItem* { return qobject_cast<QQuickItem*>(Property("actions")); }
  auto Button(const char* object_name) -> QQuickItem* {
    auto* row = actions();
    return row ? row->findChild<QQuickItem*>(QString::fromLatin1(object_name)) : nullptr;
  }
  auto AddButton() -> QQuickItem* { return Button("libraryInspectorAddToExportQueueButton"); }
  auto ExportButton() -> QQuickItem* { return Button("libraryInspectorExportButton"); }

  void ShowPage(int page) {
    ASSERT_NE(inspector(), nullptr);
    ASSERT_TRUE(inspector()->setProperty("currentPage", page));
    ProcessEvents(10);
  }

  auto warnings() const -> const std::vector<std::string>& { return warnings_; }
  /// Warnings that name the button row file.
  auto ActionWarnings() const -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& warning : warnings_) {
      if (warning.find("LibraryInspectorActions.qml") != std::string::npos) {
        out.push_back(warning);
      }
    }
    return out;
  }

 private:
  auto Property(const char* name) -> QObject* {
    auto* root = window();
    return root ? qvariant_cast<QObject*>(root->property(name)) : nullptr;
  }

  QQmlApplicationEngine    engine_;
  std::vector<std::string> warnings_;
};

/// Selects the first two images of @p project.
void SelectTwoImages(ApplicationModuleHost& backend, const PopulatedProject& project) {
  ASSERT_GE(project.images_.size(), 2u);
  auto* selection = backend.library()->selection();
  for (std::size_t index = 0; index < 2; ++index) {
    const auto& key = project.images_[index];
    selection->SetImageSelected(key.file_id_, key.image_id_,
                                QStringLiteral("photo_%1.dng").arg(index), false, true);
  }
  ASSERT_EQ(selection->SelectedCount(), 2);
  ProcessEvents(10);
}

auto IsShown(QQuickItem* item) -> bool { return item != nullptr && item->isVisible(); }

}  // namespace

TEST_F(LibraryInspectorActionsQmlTests, AlbumAndImageTabsShowAddToExportQueueAndHideExport) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 3);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  SelectTwoImages(backend, *packed);

  InspectorColumn column(backend);
  ASSERT_TRUE(WaitUntil([&]() { return column.AddButton() != nullptr; }))
      << Joined(column.warnings());
  ASSERT_NE(column.ExportButton(), nullptr);

  for (const int page : {kAlbumPage, kImagePage}) {
    SCOPED_TRACE(page);
    column.ShowPage(page);
    EXPECT_TRUE(IsShown(column.AddButton()));
    EXPECT_EQ(column.AddButton()->property("text").toString(),
              QStringLiteral("Add to Export Queue (2)"));
    EXPECT_TRUE(column.AddButton()->isEnabled());
    EXPECT_FALSE(IsShown(column.ExportButton()));
  }
  EXPECT_TRUE(column.ActionWarnings().empty()) << Joined(column.ActionWarnings());
}

TEST_F(LibraryInspectorActionsQmlTests, AddClickQueuesSelectionClearsItAndOpensExportTab) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 3);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  SelectTwoImages(backend, *packed);
  ASSERT_EQ(backend.export_queue()->Count(), 0);

  InspectorColumn column(backend);
  ASSERT_TRUE(WaitUntil([&]() { return column.AddButton() != nullptr; }))
      << Joined(column.warnings());
  column.ShowPage(kAlbumPage);
  ASSERT_TRUE(column.AddButton()->isEnabled());

  ASSERT_TRUE(QMetaObject::invokeMethod(column.AddButton(), "clicked"));
  ProcessEvents(10);

  EXPECT_EQ(backend.export_queue()->Count(), 2);
  EXPECT_EQ(backend.library()->selection()->SelectedCount(), 0);
  EXPECT_EQ(column.inspector()->property("currentPage").toInt(), kExportPage);
  EXPECT_TRUE(column.inspector()->property("exportPageActive").toBool());
  EXPECT_TRUE(column.ActionWarnings().empty()) << Joined(column.ActionWarnings());
}

TEST_F(LibraryInspectorActionsQmlTests, ExportTabShowsOnlyTheExportButtonAtFullRowWidth) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 3);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  SelectTwoImages(backend, *packed);

  InspectorColumn column(backend);
  ASSERT_TRUE(WaitUntil([&]() { return column.AddButton() != nullptr; }))
      << Joined(column.warnings());
  column.ShowPage(kExportPage);

  EXPECT_FALSE(IsShown(column.AddButton()));
  EXPECT_TRUE(IsShown(column.ExportButton()));
  auto* row = column.actions();
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(WaitUntil([&]() {
    return row->width() > 0 && std::abs(column.ExportButton()->width() - row->width()) < 0.5;
  })) << "export button width "
      << column.ExportButton()->width() << ", row width " << row->width();
  EXPECT_TRUE(column.ActionWarnings().empty()) << Joined(column.ActionWarnings());
}

TEST_F(LibraryInspectorActionsQmlTests, AddButtonIsDisabledWithoutSelectedImages) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 3);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_EQ(backend.library()->selection()->SelectedCount(), 0);

  InspectorColumn column(backend);
  ASSERT_TRUE(WaitUntil([&]() { return column.AddButton() != nullptr; }))
      << Joined(column.warnings());
  column.ShowPage(kAlbumPage);

  EXPECT_TRUE(IsShown(column.AddButton()));
  EXPECT_FALSE(column.AddButton()->isEnabled());
  EXPECT_EQ(column.AddButton()->property("text").toString(),
            QStringLiteral("Add to Export Queue (0)"));

  // A clicked signal of the disabled button changes nothing.
  ASSERT_TRUE(QMetaObject::invokeMethod(column.AddButton(), "clicked"));
  ProcessEvents(10);
  EXPECT_EQ(backend.export_queue()->Count(), 0);
  EXPECT_EQ(column.inspector()->property("currentPage").toInt(), kAlbumPage);
  EXPECT_TRUE(column.ActionWarnings().empty()) << Joined(column.ActionWarnings());
}

}  // namespace alcedo::ui::test
