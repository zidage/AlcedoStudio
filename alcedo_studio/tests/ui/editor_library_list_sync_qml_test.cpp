//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The editor follows the filtered Library list (ImageActionsController.qml). When the accepted
// list does not contain the open image, the editor closes it through the router's empty-editor
// route (WorkspaceRouter::openEditor(0, 0) -> EditorSessionController::Open(0, 0), a persisted
// session close). The close waits for the same save check as an image switch
// (actions.canSelectImage) and for the accepted library query.
//
// The fakes are C++ objects with the production property, invokable, and signal names. The
// production signals start with an upper-case letter (RouteChanged, StateChanged,
// QueryStateChanged, AvailabilityChanged); a QML fake with lower-case signals would hide a
// handler that never runs in the application.

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
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <utility>

namespace alcedo::ui::test {
namespace {

// EditorActionAvailabilityModel: canSelectImage notifies through AvailabilityChanged.
class FakeActions final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool canSelectImage READ canSelectImage NOTIFY AvailabilityChanged)

 public:
  [[nodiscard]] auto canSelectImage() const -> bool { return can_select_image_; }
  void               SetCanSelectImage(bool can_select) {
    can_select_image_ = can_select;
    emit AvailabilityChanged();
  }

 signals:
  void AvailabilityChanged();

 private:
  bool can_select_image_ = true;
};

// EditorSessionController: identity and state notify through StateChanged.
class FakeSession final : public QObject {
  Q_OBJECT
  Q_PROPERTY(uint elementId READ elementId NOTIFY StateChanged)
  Q_PROPERTY(uint imageId READ imageId NOTIFY StateChanged)
  Q_PROPERTY(QString sessionState READ sessionState NOTIFY StateChanged)
  Q_PROPERTY(QObject* actions READ actions CONSTANT)

 public:
  [[nodiscard]] auto elementId() const -> uint { return element_id_; }
  [[nodiscard]] auto imageId() const -> uint { return image_id_; }
  [[nodiscard]] auto sessionState() const -> QString { return session_state_; }
  [[nodiscard]] auto actions() -> QObject* { return &actions_; }
  [[nodiscard]] auto fakeActions() -> FakeActions& { return actions_; }

  void               Open(uint element_id, uint image_id) {
    element_id_ = element_id;
    image_id_   = image_id;
    emit StateChanged();
  }

 signals:
  void StateChanged();

 private:
  FakeActions actions_;
  uint        element_id_    = 1001;
  uint        image_id_      = 2001;
  QString     session_state_ = QStringLiteral("Interactive");
};

// WorkspaceRouter: the workspace notifies through RouteChanged.
class FakeRouter final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString workspace READ workspace NOTIFY RouteChanged)

 public:
  explicit FakeRouter(FakeSession* session) : session_(session) {}

  [[nodiscard]] auto workspace() const -> QString { return workspace_; }
  [[nodiscard]] auto openCount() const -> int { return open_count_; }
  [[nodiscard]] auto lastElementId() const -> int { return last_element_id_; }

  Q_INVOKABLE bool   openEditor(uint element_id, uint image_id) {
    ++open_count_;
    last_element_id_ = static_cast<int>(element_id);
    workspace_       = QStringLiteral("editor");
    emit RouteChanged();
    session_->Open(element_id, image_id);
    return true;
  }
  Q_INVOKABLE void openLibrary() {
    workspace_ = QStringLiteral("library");
    emit RouteChanged();
  }
  // The route back to the editor with the image that the session still holds.
  void ReturnToEditor() {
    workspace_ = QStringLiteral("editor");
    emit RouteChanged();
  }

 signals:
  void RouteChanged();

 private:
  FakeSession* session_         = nullptr;
  QString      workspace_       = QStringLiteral("editor");
  int          open_count_      = 0;
  int          last_element_id_ = -1;
};

// AlbumThumbnailModel: the loaded rows of the accepted, filtered list.
class FakeThumbnailModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
  Q_PROPERTY(int count READ count NOTIFY countChanged)

 public:
  [[nodiscard]] auto loading() const -> bool { return false; }
  [[nodiscard]] auto count() const -> int { return static_cast<int>(element_ids_.size()); }

  void               SetElementIds(QList<int> element_ids) {
    element_ids_ = std::move(element_ids);
    emit countChanged();
  }

  Q_INVOKABLE int rowByElementId(uint element_id) const {
    return static_cast<int>(element_ids_.indexOf(static_cast<int>(element_id)));
  }
  Q_INVOKABLE QVariantMap getItemAt(int index) const {
    if (index < 0 || index >= count()) {
      return {};
    }
    const int element_id = element_ids_.at(index);
    return {{QStringLiteral("elementId"), element_id},
            {QStringLiteral("imageId"), element_id + 1000},
            {QStringLiteral("fileName"), QStringLiteral("film_%1.arw").arg(element_id)},
            {QStringLiteral("rating"), 0}};
  }

 signals:
  void loadingChanged();
  void countChanged();

 private:
  QList<int> element_ids_{1000, 1001, 1002};
};

// LibraryModule: queryUpdating notifies through QueryStateChanged.
class FakeLibrary final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool queryUpdating READ queryUpdating NOTIFY QueryStateChanged)
  Q_PROPERTY(QObject* thumbnailModel READ thumbnailModel CONSTANT)

 public:
  [[nodiscard]] auto queryUpdating() const -> bool { return query_updating_; }
  [[nodiscard]] auto thumbnailModel() -> QObject* { return &model_; }
  [[nodiscard]] auto model() -> FakeThumbnailModel& { return model_; }

  void               SetQueryUpdating(bool updating) {
    query_updating_ = updating;
    emit QueryStateChanged();
  }

  Q_INVOKABLE int IndexOfElementInCurrentView(uint element_id) const {
    return model_.rowByElementId(element_id);
  }

 signals:
  void QueryStateChanged();

 private:
  FakeThumbnailModel model_;
  bool               query_updating_ = false;
};

class FakeImages final : public QObject {
  Q_OBJECT

 public:
  Q_INVOKABLE QVariantMap GetFocusedImageInspection(uint element_id, uint image_id) const {
    Q_UNUSED(element_id);
    Q_UNUSED(image_id);
    return {{QStringLiteral("success"), false}};
  }
};

// The remaining appModules objects that ImageActionsController reads but these tests do not
// drive. The C++ fakes are assigned before the controller loads.
constexpr char kFakeModulesQml[] = R"(
import QtQuick

QtObject {
    property QtObject editorSession: null
    property QtObject workspaceRouter: null
    property QtObject library: null
    property QtObject images: null
    property QtObject interactionPolicy: QtObject {
        property int focusedElementId: 0
        property var pendingDeleteTargets: []
    }
    property QtObject folders: QtObject { property int currentFolderId: 0 }
    property QtObject libraryMutations: QtObject { signal deleteTargetsFinished(var result) }
    property QtObject adjustmentTransfer: QtObject {}
}
)";

constexpr char kHostQml[]        = R"(
import QtQuick

QtObject {
    property int filmstripRevealCount: 0
    property int libraryRevealCount: 0
    function requestFilmstripScrollToElement(elementId, index) { filmstripRevealCount += 1 }
    function requestLibraryScrollToElement(elementId, index) { libraryRevealCount += 1 }
    function showSnackbar(message) {}
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

auto AsVariant(QObject* object) -> QVariant { return QVariant::fromValue(object); }

class EditorLibraryListSyncHarness {
 public:
  EditorLibraryListSyncHarness() {
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    modules_ = Create(QByteArray{kFakeModulesQml}, QStringLiteral("file:///FakeModules.qml"));
    host_    = Create(QByteArray{kHostQml}, QStringLiteral("file:///FakeHost.qml"));
    if (!modules_ || !host_) {
      return;
    }
    modules_->setProperty("editorSession", AsVariant(&session_));
    modules_->setProperty("workspaceRouter", AsVariant(&router_));
    modules_->setProperty("library", AsVariant(&library_));
    modules_->setProperty("images", AsVariant(&images_));
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), modules_.get());
    QQmlComponent component(
        &engine_,
        QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/ImageActionsController.qml")));
    controller_.reset(component.create());
    for (const auto& error : component.errors()) warnings_.push_back(error.toString());
    if (controller_) {
      controller_->setProperty("host", AsVariant(host_.get()));
    }
    ProcessEvents();
  }

  void SetFilteredList(QList<int> element_ids) {
    library_.model().SetElementIds(std::move(element_ids));
  }

  void RequestSync() {
    QMetaObject::invokeMethod(controller_.get(), "requestEditorLibraryListSync",
                              Q_ARG(QVariant, QVariant(true)));
    ProcessEvents();
  }

  // Members are destroyed in reverse order: the QML objects go before the engine, and the
  // engine before the C++ fakes that the QML objects reference.
  FakeSession              session_;
  FakeRouter               router_{&session_};
  FakeLibrary              library_;
  FakeImages               images_;
  QQmlEngine               engine_;
  QStringList              warnings_;
  std::unique_ptr<QObject> modules_;
  std::unique_ptr<QObject> host_;
  std::unique_ptr<QObject> controller_;

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

TEST(EditorLibraryListSyncQmlTest, FilterThatRemovesOpenImageClosesItAfterSaveCheck) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  // The save check refuses an image switch (for example, a commit is still being written).
  harness.session_.fakeActions().SetCanSelectImage(false);
  harness.SetFilteredList({1000, 1002});
  harness.RequestSync();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.session_.elementId(), 1001u);

  // The session admits the switch again: the pending close runs once, to the empty editor.
  harness.session_.fakeActions().SetCanSelectImage(true);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 0);
  EXPECT_EQ(harness.session_.elementId(), 0u);
  EXPECT_EQ(harness.session_.imageId(), 0u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(EditorLibraryListSyncQmlTest, CloseWaitsForAcceptedLibraryQuery) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  // A filter change is submitted; the rows still show the previous result.
  harness.library_.SetQueryUpdating(true);
  harness.RequestSync();
  EXPECT_EQ(harness.router_.openCount(), 0);

  // The new result without the open image is accepted.
  harness.SetFilteredList({1000});
  harness.library_.SetQueryUpdating(false);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.session_.elementId(), 0u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(EditorLibraryListSyncQmlTest, OpenImageInFilteredListStaysOpenAndIsRevealed) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.SetFilteredList({1001, 1002});
  harness.RequestSync();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.session_.elementId(), 1001u);
  EXPECT_EQ(harness.host_->property("filmstripRevealCount").toInt(), 1);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// Issue 442 item 1: the Library filter changed while the editor was hidden. The return to the
// editor checks the open image and closes it.
TEST(EditorLibraryListSyncQmlTest, ReturnToEditorClosesImageThatLibraryFilterRemoved) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.router_.openLibrary();
  ProcessEvents();
  harness.SetFilteredList({1000, 1002});
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 0);

  harness.router_.ReturnToEditor();
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 0);
  EXPECT_EQ(harness.session_.elementId(), 0u);
  // The return does not scroll the filmstrip; it keeps its own position.
  EXPECT_EQ(harness.host_->property("filmstripRevealCount").toInt(), 0);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(EditorLibraryListSyncQmlTest, EmptyEditorDoesNotOpenAnImageAfterFilterChange) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.session_.Open(0, 0);
  harness.SetFilteredList({1000, 1002});
  harness.RequestSync();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.session_.elementId(), 0u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// A stats filter, search, or collection change in the editor: the accepted library result alone
// starts the check, with no other request.
TEST(EditorLibraryListSyncQmlTest, AcceptedQueryInEditorClosesFilteredOutImage) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.library_.SetQueryUpdating(true);
  ProcessEvents();
  harness.SetFilteredList({1002});
  harness.library_.SetQueryUpdating(false);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 0);
  EXPECT_EQ(harness.session_.elementId(), 0u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test

#include "editor_library_list_sync_qml_test.moc"
