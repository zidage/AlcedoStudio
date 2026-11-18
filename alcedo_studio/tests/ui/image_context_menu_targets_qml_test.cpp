//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Issue 957: the image context menu resolves its targets with one rule for the Library grid and
// the editor filmstrip (ImageActionsController.openImageContextMenu ->
// LibraryMutationOperations::ResolveTargets). A menu on a selected image acts on every selected
// image: Paste, Delete, Rating, and "Add to" receive the whole selection. A menu on an image
// outside the selection acts on that image alone.
//
// The controller runs with the production LibraryMutationOperations and LibrarySelection; the
// other appModules objects are stand-ins that these tests do not drive. ImageController has
// upper-case invokables, which a QML object cannot declare, so its stand-in is a C++ object.

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QList>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <filesystem>
#include <memory>

#include "ui/alcedo_main/album_backend/library_mutation_operations.hpp"
#include "ui/alcedo_main/album_backend/library_selection.hpp"

namespace alcedo::ui::test {
namespace {

// ImageController: the rating and the inspection that the menu reads for the clicked image.
class FakeImages final : public QObject {
  Q_OBJECT

 public:
  Q_INVOKABLE QVariantMap GetImageRating(uint element_id, uint image_id) const {
    Q_UNUSED(element_id);
    Q_UNUSED(image_id);
    return {{QStringLiteral("success"), true}, {QStringLiteral("rating"), 2}};
  }
  Q_INVOKABLE QVariantMap GetFocusedImageInspection(uint element_id, uint image_id) const {
    Q_UNUSED(element_id);
    Q_UNUSED(image_id);
    return {{QStringLiteral("success"), false}};
  }
};

constexpr char kFakeModulesQml[] = R"(
import QtQuick

QtObject {
    property QtObject editorSession: null
    property QtObject workspaceRouter: QtObject { property string workspace: "editor" }
    property QtObject library: QtObject {
        property bool queryUpdating: false
        property QtObject thumbnailModel: QtObject {
            property bool loading: false
            property int count: 0
        }
    }
    property QtObject images: null
    property QtObject interactionPolicy: QtObject {
        property int focusedElementId: 0
        property var pendingDeleteTargets: []
    }
    property QtObject folders: QtObject { property int currentFolderId: 0 }
    property QtObject libraryMutations: null
    property QtObject adjustmentTransfer: QtObject {}
}
)";

constexpr char kHostQml[]        = R"(
import QtQuick

QtObject {
    property bool backendInteractive: true
    function showSnackbar(message) {}
}
)";

constexpr char kMenuQml[]        = R"(
import QtQuick

QtObject {
    property int openCount: 0
    function openAt(x, y) { openCount += 1 }
}
)";

auto           QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int milliseconds = 30) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto ElementIds(const QVariantList& targets) -> QList<uint> {
  QList<uint> ids;
  for (const QVariant& target : targets) {
    ids.push_back(target.toMap().value(QStringLiteral("elementId")).toUInt());
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

auto ClickedItem(uint element_id) -> QVariantMap {
  return {{QStringLiteral("elementId"), element_id},
          {QStringLiteral("fileId"), element_id},
          {QStringLiteral("imageId"), element_id + 1000},
          {QStringLiteral("fileName"), QStringLiteral("film_%1.arw").arg(element_id)},
          {QStringLiteral("rating"), 0}};
}

class ImageContextMenuHarness {
 public:
  ImageContextMenuHarness() {
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    modules_ = Create(QByteArray{kFakeModulesQml}, QStringLiteral("file:///FakeModules.qml"));
    host_    = Create(QByteArray{kHostQml}, QStringLiteral("file:///FakeHost.qml"));
    menu_    = Create(QByteArray{kMenuQml}, QStringLiteral("file:///FakeMenu.qml"));
    if (!modules_ || !host_ || !menu_) {
      return;
    }
    modules_->setProperty("libraryMutations", QVariant::fromValue<QObject*>(&mutations_));
    modules_->setProperty("images", QVariant::fromValue<QObject*>(&images_));
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), modules_.get());
    QQmlComponent component(
        &engine_,
        QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/ImageActionsController.qml")));
    controller_.reset(component.create());
    for (const auto& error : component.errors()) warnings_.push_back(error.toString());
    if (controller_) {
      controller_->setProperty("host", QVariant::fromValue<QObject*>(host_.get()));
      controller_->setProperty("imageContextMenu", QVariant::fromValue<QObject*>(menu_.get()));
    }
    ProcessEvents();
  }

  void Select(QList<uint> element_ids) {
    for (const uint element_id : element_ids) {
      selection_.SetImageSelected(element_id, element_id + 1000,
                                  QStringLiteral("film_%1.arw").arg(element_id), false, true);
    }
  }

  void OpenMenu(uint element_id, const QString& origin) {
    QMetaObject::invokeMethod(controller_.get(), "openImageContextMenu",
                              Q_ARG(QVariant, QVariant(ClickedItem(element_id))),
                              Q_ARG(QVariant, QVariant(12.0)), Q_ARG(QVariant, QVariant(34.0)),
                              Q_ARG(QVariant, QVariant(origin)));
    ProcessEvents();
  }

  [[nodiscard]] auto Targets(const char* property) const -> QVariantList {
    return controller_->property(property).toList();
  }

  // Members are destroyed in reverse order: the QML objects go before the engine, and the
  // engine before the C++ objects that the QML objects reference.
  LibrarySelection          selection_{nullptr};
  LibraryMutationOperations mutations_{nullptr, &selection_, nullptr, nullptr, nullptr};
  FakeImages                images_;
  QQmlEngine                engine_;
  QStringList               warnings_;
  std::unique_ptr<QObject>  modules_;
  std::unique_ptr<QObject>  host_;
  std::unique_ptr<QObject>  menu_;
  std::unique_ptr<QObject>  controller_;

 private:
  [[nodiscard]] auto Create(const QByteArray& source, const QString& url)
      -> std::unique_ptr<QObject> {
    QQmlComponent component(&engine_);
    component.setData(source, QUrl(url));
    std::unique_ptr<QObject> object(component.create());
    for (const auto& error : component.errors()) warnings_.push_back(error.toString());
    return object;
  }
};

// The issue steps: three images selected in the filmstrip, the menu opened on one of them from
// the filmstrip. Paste, Delete, and Rating each target every selected image.
TEST(ImageContextMenuTargetsQmlTest, FilmstripMenuOnSelectedImageTargetsWholeSelection) {
  ImageContextMenuHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.Select({1001, 1002, 1003});
  harness.OpenMenu(1002, QStringLiteral("editor-filmstrip"));

  const QList<uint> selected{1001u, 1002u, 1003u};
  EXPECT_EQ(harness.menu_->property("openCount").toInt(), 1);
  EXPECT_EQ(harness.controller_->property("menuOrigin").toString(),
            QStringLiteral("editor-filmstrip"));
  EXPECT_EQ(ElementIds(harness.Targets("pendingAdjustmentPasteTargets")), selected);
  EXPECT_EQ(ElementIds(harness.Targets("pendingDeleteTargets")), selected);
  EXPECT_EQ(ElementIds(harness.Targets("pendingRatingTargets")), selected);
  // Copy and the menu rating read the clicked image.
  EXPECT_EQ(harness.controller_->property("pendingAdjustmentSource")
                .toMap()
                .value(QStringLiteral("elementId"))
                .toUInt(),
            1002u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// A menu on an image outside the selection acts on that image alone, from the filmstrip and
// from the Library grid, and never on the hidden selection.
TEST(ImageContextMenuTargetsQmlTest, MenuOnUnselectedImageTargetsClickedImageOnly) {
  ImageContextMenuHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.Select({1001, 1002, 1003});
  harness.OpenMenu(1005, QStringLiteral("editor-filmstrip"));
  EXPECT_EQ(ElementIds(harness.Targets("pendingAdjustmentPasteTargets")), QList<uint>{1005u});
  EXPECT_EQ(ElementIds(harness.Targets("pendingDeleteTargets")), QList<uint>{1005u});

  harness.OpenMenu(1005, QStringLiteral("library"));
  EXPECT_EQ(harness.controller_->property("menuOrigin").toString(), QStringLiteral("library"));
  EXPECT_EQ(ElementIds(harness.Targets("pendingAdjustmentPasteTargets")), QList<uint>{1005u});
  EXPECT_EQ(ElementIds(harness.Targets("pendingRatingTargets")), QList<uint>{1005u});
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// The Library grid uses the same rule: a menu on a selected image targets the selection.
TEST(ImageContextMenuTargetsQmlTest, LibraryMenuOnSelectedImageTargetsWholeSelection) {
  ImageContextMenuHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.Select({1001, 1003});
  harness.OpenMenu(1003, QStringLiteral("library"));
  const QList<uint> selected{1001u, 1003u};
  EXPECT_EQ(ElementIds(harness.Targets("pendingAdjustmentPasteTargets")), selected);
  EXPECT_EQ(ElementIds(harness.Targets("pendingDeleteTargets")), selected);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test

#include "image_context_menu_targets_qml_test.moc"
