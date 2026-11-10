//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Production QML of the Library inspector button row (#731): LibraryInspectorActions.qml with
// an InspectorPanel and a SelectionState, loaded from the source tree against a real packed
// project. The marks control above the row (#772, LibraryMarksControl.qml) is in the same
// column. The tests emit the buttons' clicked signals or call the control's functions; they do
// not deliver synthetic pointer input.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QString>
#include <QUrl>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "image/color_label.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/export_queue.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/library_selection.hpp"
#include "ui/alcedo_main/album_backend/library_selection_marks.hpp"
#include "ui/alcedo_main/album_backend/library_view_preferences.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

using LibraryInspectorActionsQmlTests = ApplicationModuleHostTestFixture;

/// The first item named @p object_name in the visual tree under @p root. Repeater delegates
/// have no QObject parent, so QObject::findChild does not see them.
auto FindItem(QQuickItem* root, const QString& object_name) -> QQuickItem* {
  if (root == nullptr) {
    return nullptr;
  }
  if (root->objectName() == object_name) {
    return root;
  }
  for (auto* child : root->childItems()) {
    if (auto* found = FindItem(child, object_name)) {
      return found;
    }
  }
  return nullptr;
}

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
    property alias marks: marksControl

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

        LibraryMarksControl {
            id: marksControl
            Layout.fillWidth: true
            visible: !inspectorPanel.exportPageActive
            selectionState: selectionStateObj
            selectedCount: selectionStateObj.selectedCount
            backendInteractive: appModules.project.serviceReady
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
  auto marks() -> QQuickItem* { return qobject_cast<QQuickItem*>(Property("marks")); }
  auto MarksItem(const QString& object_name) -> QQuickItem* {
    return FindItem(marks(), object_name);
  }
  /// "all", "some", or "none" of the code circle @p code.
  auto CodeState(int code) -> QString {
    auto* item = MarksItem(QStringLiteral("libraryMarksColorLabel_%1").arg(code));
    return item ? item->property("markState").toString() : QString{};
  }
  auto StarText(int star) -> QString {
    auto* item = MarksItem(QStringLiteral("libraryMarksStar_%1").arg(star));
    return item ? item->property("text").toString() : QString{};
  }
  auto DeleteButton() -> QQuickItem* {
    return MarksItem(QStringLiteral("libraryMarksDeleteButton"));
  }
  auto ExportButton() -> QQuickItem* { return Button("libraryInspectorExportButton"); }

  void ShowPage(int page) {
    ASSERT_NE(inspector(), nullptr);
    ASSERT_TRUE(inspector()->setProperty("currentPage", page));
    ProcessEvents(10);
  }

  auto warnings() const -> const std::vector<std::string>& { return warnings_; }
  /// Warnings that name the button row file or the marks control file.
  auto ActionWarnings() const -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& warning : warnings_) {
      if (warning.find("LibraryInspectorActions.qml") != std::string::npos ||
          warning.find("LibraryMarksControl.qml") != std::string::npos) {
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

/// Three photos: 0 Red + Green, rating 2; 1 Red, rating 4, Delete mark; 2 no mark, rating 0.
auto MarkedSpecs() -> std::vector<LibraryPhotoSpec> {
  std::vector<LibraryPhotoSpec> specs(3);
  specs[0].color_labels_ = ColorLabelBit(ColorLabel::kRed) | ColorLabelBit(ColorLabel::kGreen);
  specs[0].rating_       = 2;
  specs[1].color_labels_ = ColorLabelBit(ColorLabel::kRed);
  specs[1].rating_       = 4;
  specs[1].delete_mark_  = true;
  return specs;
}

/// The loaded library row of @p element_id, or null.
auto RowOf(ApplicationModuleHost& backend, sl_element_id_t element_id) -> const AlbumItem* {
  for (const auto& item : backend.library()->model().items()) {
    if (item.element_id == element_id) {
      return &item;
    }
  }
  return nullptr;
}

/// Wait until the selection summary holds @p count photos.
auto WaitForSummary(ApplicationModuleHost& backend, int count) -> bool {
  return WaitUntil([&]() { return backend.library()->selection_marks()->Count() == count; });
}

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

// #772: per code, filled when all selected photos have it, a ring with a dash when some have it,
// a ring when none has it; hollow stars and "Mixed" for different ratings; "Mark" while not all
// photos have the Delete mark. Without a selection the control is disabled.
TEST_F(LibraryInspectorActionsQmlTests, MarksControlShowsTriStateCodesMixedRatingAndMark) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, MarkedSpecs());
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  InspectorColumn column(backend);
  ASSERT_TRUE(WaitUntil([&]() { return column.DeleteButton() != nullptr; }))
      << Joined(column.warnings());
  column.ShowPage(kAlbumPage);
  EXPECT_TRUE(IsShown(column.marks()));
  EXPECT_FALSE(column.marks()->property("controlsEnabled").toBool());
  EXPECT_FALSE(column.DeleteButton()->isEnabled());
  for (int code = 0; code < kColorLabelCount; ++code) {
    EXPECT_EQ(column.CodeState(code), QStringLiteral("none")) << code;
  }

  SelectTwoImages(backend, *packed);
  ASSERT_TRUE(WaitForSummary(backend, 2));
  ProcessEvents(10);
  EXPECT_TRUE(column.marks()->property("controlsEnabled").toBool());
  EXPECT_TRUE(column.DeleteButton()->isEnabled());

  EXPECT_EQ(column.CodeState(static_cast<int>(ColorLabel::kRed)), QStringLiteral("all"));
  EXPECT_EQ(column.CodeState(static_cast<int>(ColorLabel::kGreen)), QStringLiteral("some"));
  for (const auto code : {ColorLabel::kOrange, ColorLabel::kYellow, ColorLabel::kBlue,
                          ColorLabel::kPurple, ColorLabel::kGray}) {
    EXPECT_EQ(column.CodeState(static_cast<int>(code)), QStringLiteral("none"))
        << static_cast<int>(code);
  }
  EXPECT_TRUE(IsShown(column.MarksItem(QStringLiteral("libraryMarksColorLabelDash_3"))));
  EXPECT_FALSE(IsShown(column.MarksItem(QStringLiteral("libraryMarksColorLabelDash_0"))));
  EXPECT_FALSE(IsShown(column.MarksItem(QStringLiteral("libraryMarksColorLabelDash_1"))));

  // Ratings 2 and 4: hollow stars and "Mixed".
  EXPECT_TRUE(column.marks()->property("ratingMixed").toBool());
  EXPECT_TRUE(IsShown(column.MarksItem(QStringLiteral("libraryMarksRatingMixed"))));
  for (int star = 1; star <= 5; ++star) {
    EXPECT_EQ(column.StarText(star), QString::fromUtf8("☆")) << star;
  }

  // One of the two photos has the Delete mark.
  EXPECT_EQ(column.DeleteButton()->property("text").toString(), QStringLiteral("Mark"));
  EXPECT_FALSE(column.DeleteButton()->property("marked").toBool());

  // The tooltip of a code is its user name.
  QVariant name;
  ASSERT_TRUE(QMetaObject::invokeMethod(column.marks(), "codeName", Q_RETURN_ARG(QVariant, name),
                                        Q_ARG(QVariant, 0)));
  EXPECT_EQ(name.toString(), backend.library_view()->color_label_names().value(0));
  EXPECT_TRUE(column.ActionWarnings().empty()) << Joined(column.ActionWarnings());
}

// #772: a click on a code that some photos have adds it to all; a click on a code that all have
// removes it from all. The other codes of each photo stay.
TEST_F(LibraryInspectorActionsQmlTests, MarksControlCodeClickAddsToAllOrRemovesFromAll) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, MarkedSpecs());
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  SelectTwoImages(backend, *packed);
  ASSERT_TRUE(WaitForSummary(backend, 2));
  const auto&     keys = packed->images_;

  InspectorColumn column(backend);
  ASSERT_TRUE(WaitUntil([&]() { return column.DeleteButton() != nullptr; }))
      << Joined(column.warnings());
  column.ShowPage(kAlbumPage);
  QSignalSpy finished(backend.images(), &ImageController::ImageMarksFinished);

  const int  green = static_cast<int>(ColorLabel::kGreen);
  const int  red   = static_cast<int>(ColorLabel::kRed);
  ASSERT_EQ(column.CodeState(green), QStringLiteral("some"));
  ASSERT_TRUE(
      QMetaObject::invokeMethod(column.marks(), "toggleColorLabel", Q_ARG(QVariant, green)));
  // The control shows the change before the write ends.
  EXPECT_EQ(column.CodeState(green), QStringLiteral("all"));
  ASSERT_TRUE(WaitUntil([&]() { return finished.count() == 1; }));
  EXPECT_TRUE(finished.takeFirst().at(0).toMap().value("success").toBool());
  for (std::size_t i = 0; i < 2; ++i) {
    const auto* row = RowOf(backend, keys[i].file_id_);
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(HasColorLabel(row->color_labels, ColorLabel::kGreen)) << i;
    EXPECT_TRUE(HasColorLabel(row->color_labels, ColorLabel::kRed)) << i;
  }
  // The photo that is not selected keeps its marks.
  ASSERT_NE(RowOf(backend, keys[2].file_id_), nullptr);
  EXPECT_EQ(RowOf(backend, keys[2].file_id_)->color_labels, 0);
  // The stored marks, read again after the write, give the same state.
  ProcessEvents(300);
  EXPECT_EQ(backend.library()->selection_marks()->ColorLabelCount(ColorLabel::kGreen), 2);
  EXPECT_EQ(column.CodeState(green), QStringLiteral("all"));

  // All have Green: a click removes it from all; Red stays.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(column.marks(), "toggleColorLabel", Q_ARG(QVariant, green)));
  EXPECT_EQ(column.CodeState(green), QStringLiteral("none"));
  ASSERT_TRUE(WaitUntil([&]() { return finished.count() == 1; }));
  finished.clear();
  for (std::size_t i = 0; i < 2; ++i) {
    const auto* row = RowOf(backend, keys[i].file_id_);
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(HasColorLabel(row->color_labels, ColorLabel::kGreen)) << i;
    EXPECT_TRUE(HasColorLabel(row->color_labels, ColorLabel::kRed)) << i;
  }

  // All have Red: a click removes it from all.
  ProcessEvents(300);
  ASSERT_EQ(column.CodeState(red), QStringLiteral("all"));
  ASSERT_TRUE(QMetaObject::invokeMethod(column.marks(), "toggleColorLabel", Q_ARG(QVariant, red)));
  ASSERT_TRUE(WaitUntil([&]() { return finished.count() == 1; }));
  ProcessEvents(300);
  EXPECT_EQ(column.CodeState(red), QStringLiteral("none"));
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(RowOf(backend, keys[i].file_id_)->color_labels, 0) << i;
  }
  EXPECT_TRUE(column.ActionWarnings().empty()) << Joined(column.ActionWarnings());
}

// #772: a star sets the rating of all selected photos (Mixed goes away); the star of the common
// rating clears it. "Mark" sets the Delete mark on all photos and turns into "Marked"; "Marked"
// clears it.
TEST_F(LibraryInspectorActionsQmlTests, MarksControlRatesAllPhotosAndTogglesTheDeleteMark) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, MarkedSpecs());
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  SelectTwoImages(backend, *packed);
  ASSERT_TRUE(WaitForSummary(backend, 2));
  const auto&     keys = packed->images_;

  InspectorColumn column(backend);
  ASSERT_TRUE(WaitUntil([&]() { return column.DeleteButton() != nullptr; }))
      << Joined(column.warnings());
  column.ShowPage(kAlbumPage);
  ASSERT_TRUE(column.marks()->property("ratingMixed").toBool());

  QSignalSpy ratings(backend.images(), &ImageController::ImageRatingsFinished);
  ASSERT_TRUE(QMetaObject::invokeMethod(column.marks(), "setRating", Q_ARG(QVariant, 3)));
  // The control shows the change before the write ends.
  EXPECT_FALSE(column.marks()->property("ratingMixed").toBool());
  ASSERT_TRUE(WaitUntil([&]() { return ratings.count() == 1; }));
  ProcessEvents(300);
  EXPECT_FALSE(column.marks()->property("ratingMixed").toBool());
  EXPECT_EQ(column.marks()->property("commonRating").toInt(), 3);
  EXPECT_FALSE(IsShown(column.MarksItem(QStringLiteral("libraryMarksRatingMixed"))));
  EXPECT_EQ(column.StarText(3), QString::fromUtf8("★"));
  EXPECT_EQ(column.StarText(4), QString::fromUtf8("☆"));
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(RowOf(backend, keys[i].file_id_)->rating, 3) << i;
  }

  // The star of the common rating clears it.
  QVariant cleared;
  ASSERT_TRUE(QMetaObject::invokeMethod(column.marks(), "ratingForStar",
                                        Q_RETURN_ARG(QVariant, cleared), Q_ARG(QVariant, 3)));
  EXPECT_EQ(cleared.toInt(), 0);
  ASSERT_TRUE(QMetaObject::invokeMethod(column.marks(), "setRating", Q_ARG(QVariant, cleared)));
  ASSERT_TRUE(WaitUntil([&]() { return ratings.count() == 2; }));
  ProcessEvents(300);
  EXPECT_EQ(column.marks()->property("commonRating").toInt(), 0);
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(RowOf(backend, keys[i].file_id_)->rating, 0) << i;
  }

  // Mark: all selected photos get the Delete mark.
  QSignalSpy marks(backend.images(), &ImageController::ImageMarksFinished);
  ASSERT_EQ(column.DeleteButton()->property("text").toString(), QStringLiteral("Mark"));
  ASSERT_TRUE(QMetaObject::invokeMethod(column.DeleteButton(), "clicked"));
  EXPECT_EQ(column.DeleteButton()->property("text").toString(), QStringLiteral("Marked"));
  ASSERT_TRUE(WaitUntil([&]() { return marks.count() == 1; }));
  ProcessEvents(300);
  EXPECT_EQ(column.DeleteButton()->property("text").toString(), QStringLiteral("Marked"));
  EXPECT_TRUE(column.DeleteButton()->property("marked").toBool());
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_TRUE(RowOf(backend, keys[i].file_id_)->delete_mark) << i;
  }
  EXPECT_FALSE(RowOf(backend, keys[2].file_id_)->delete_mark);

  // Marked: a click clears the mark of all selected photos.
  ASSERT_TRUE(QMetaObject::invokeMethod(column.DeleteButton(), "clicked"));
  ASSERT_TRUE(WaitUntil([&]() { return marks.count() == 2; }));
  ProcessEvents(300);
  EXPECT_EQ(column.DeleteButton()->property("text").toString(), QStringLiteral("Mark"));
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_FALSE(RowOf(backend, keys[i].file_id_)->delete_mark) << i;
  }
  EXPECT_TRUE(column.ActionWarnings().empty()) << Joined(column.ActionWarnings());
}

}  // namespace alcedo::ui::test
