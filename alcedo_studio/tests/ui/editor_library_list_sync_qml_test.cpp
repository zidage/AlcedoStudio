//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The editor follows the filtered Library list (ImageActionsController.qml). When the accepted
// list does not contain the open image, the editor closes it through the router's empty-editor
// route (WorkspaceRouter::openEditor(0, 0) -> EditorSessionController::Open(0, 0), a persisted
// session close). The close waits for the same save check as an image switch
// (actions.canSelectImage) and for the accepted library query.
//
// Issue 747: the check runs no database read on the GUI thread. A photo outside the loaded rows
// is found with LibraryModule::RequestEditorListPosition on the query worker; the fake library
// has no synchronous lookup, and the tests answer each request explicitly.
// Issue 746: the check is about the route target, and waits while the session still shows the
// previous photo (the router publishes the route before the queued Switch runs).
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

// EditorSessionController: identity and state notify through StateChanged. With a queued
// executor (the production EditorSessionThreadedCommandExecutor), Open only queues the command;
// CompleteQueuedOpen runs it.
class FakeSession final : public QObject {
  Q_OBJECT
  Q_PROPERTY(uint elementId READ elementId NOTIFY StateChanged)
  Q_PROPERTY(uint imageId READ imageId NOTIFY StateChanged)
  Q_PROPERTY(bool hasImage READ hasImage NOTIFY StateChanged)
  Q_PROPERTY(QString sessionState READ sessionState NOTIFY StateChanged)
  Q_PROPERTY(QObject* actions READ actions CONSTANT)

 public:
  [[nodiscard]] auto elementId() const -> uint { return element_id_; }
  [[nodiscard]] auto imageId() const -> uint { return image_id_; }
  [[nodiscard]] auto hasImage() const -> bool { return element_id_ > 0 && image_id_ > 0; }
  [[nodiscard]] auto sessionState() const -> QString { return session_state_; }
  [[nodiscard]] auto actions() -> QObject* { return &actions_; }
  [[nodiscard]] auto fakeActions() -> FakeActions& { return actions_; }

  void               SetQueued(bool queued) { queued_ = queued; }
  void               Open(uint element_id, uint image_id) {
    if (queued_) {
      queued_open_ = {element_id, image_id};
      has_queued_  = true;
      return;
    }
    element_id_ = element_id;
    image_id_   = image_id;
    emit StateChanged();
  }
  void CompleteQueuedOpen() {
    if (!has_queued_) {
      return;
    }
    has_queued_ = false;
    element_id_ = queued_open_.first;
    image_id_   = queued_open_.second;
    emit StateChanged();
  }

 signals:
  void StateChanged();

 private:
  FakeActions           actions_;
  uint                  element_id_    = 1001;
  uint                  image_id_      = 2001;
  QString               session_state_ = QStringLiteral("Interactive");
  bool                  queued_        = false;
  bool                  has_queued_    = false;
  std::pair<uint, uint> queued_open_{};
};

// WorkspaceRouter: the workspace and the route target notify through RouteChanged. Like the
// production router, the route is published before the session receives Open.
class FakeRouter final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString workspace READ workspace NOTIFY RouteChanged)
  Q_PROPERTY(uint elementId READ elementId NOTIFY RouteChanged)
  Q_PROPERTY(uint imageId READ imageId NOTIFY RouteChanged)

 public:
  explicit FakeRouter(FakeSession* session) : session_(session) {}

  [[nodiscard]] auto workspace() const -> QString { return workspace_; }
  [[nodiscard]] auto elementId() const -> uint { return element_id_; }
  [[nodiscard]] auto imageId() const -> uint { return image_id_; }
  [[nodiscard]] auto openCount() const -> int { return open_count_; }
  [[nodiscard]] auto lastElementId() const -> int { return last_element_id_; }

  Q_INVOKABLE bool   openEditor(uint element_id, uint image_id) {
    ++open_count_;
    last_element_id_ = static_cast<int>(element_id);
    workspace_       = QStringLiteral("editor");
    element_id_      = element_id;
    image_id_        = image_id;
    emit RouteChanged();
    session_->Open(element_id, image_id);
    return true;
  }
  Q_INVOKABLE void openLibrary() {
    workspace_  = QStringLiteral("library");
    element_id_ = 0;
    image_id_   = 0;
    emit RouteChanged();
  }
  // The route back to the editor with the image that the session still holds.
  void ReturnToEditor() {
    workspace_  = QStringLiteral("editor");
    element_id_ = session_->elementId();
    image_id_   = session_->imageId();
    emit RouteChanged();
  }
  // The empty editor, as a setup step (not counted as a user route).
  void ShowEmptyEditor() {
    workspace_  = QStringLiteral("editor");
    element_id_ = 0;
    image_id_   = 0;
    emit RouteChanged();
    session_->Open(0, 0);
  }

 signals:
  void RouteChanged();

 private:
  FakeSession* session_         = nullptr;
  QString      workspace_       = QStringLiteral("editor");
  uint         element_id_      = 1001;
  uint         image_id_        = 2001;
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

// LibraryModule: queryUpdating notifies through QueryStateChanged. The position read of the
// editor list check only records the request; the test answers it (the query worker).
class FakeLibrary final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool queryUpdating READ queryUpdating NOTIFY QueryStateChanged)
  Q_PROPERTY(QObject* thumbnailModel READ thumbnailModel CONSTANT)

 public:
  [[nodiscard]] auto queryUpdating() const -> bool { return query_updating_; }
  [[nodiscard]] auto thumbnailModel() -> QObject* { return &model_; }
  [[nodiscard]] auto model() -> FakeThumbnailModel& { return model_; }
  [[nodiscard]] auto requestCount() const -> int { return request_count_; }
  [[nodiscard]] auto lastRequestId() const -> qulonglong { return last_request_id_; }
  [[nodiscard]] auto lastRequestFileId() const -> uint { return last_request_file_id_; }

  void               SetQueryUpdating(bool updating) {
    query_updating_ = updating;
    emit QueryStateChanged();
  }

  Q_INVOKABLE qulonglong RequestEditorListPosition(uint file_id, bool load_page) {
    Q_UNUSED(load_page);
    ++request_count_;
    last_request_id_      = ++next_request_id_;
    last_request_file_id_ = file_id;
    return last_request_id_;
  }
  // The worker answer of request @p request_id (the newest request when 0).
  void Answer(qint64 occurrence, qulonglong request_id = 0) {
    emit editorListPositionReady(request_id == 0 ? last_request_id_ : request_id,
                                 last_request_file_id_, occurrence);
  }

 signals:
  void QueryStateChanged();
  void editorListPositionReady(qulonglong requestId, uint fileId, qint64 occurrence);

 private:
  FakeThumbnailModel model_;
  bool               query_updating_       = false;
  int                request_count_        = 0;
  qulonglong         next_request_id_      = 0;
  qulonglong         last_request_id_      = 0;
  uint               last_request_file_id_ = 0;
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

  // The toolbar return to the editor with the last edited photo (EditorWorkspaceNavigation).
  void OpenLastEditorImage(uint element_id, uint image_id) {
    QMetaObject::invokeMethod(controller_.get(), "openLastEditorImage",
                              Q_ARG(QVariant, QVariant(element_id)),
                              Q_ARG(QVariant, QVariant(image_id)));
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
  // The open image is not a loaded row: the check asks the query worker.
  EXPECT_EQ(harness.library_.requestCount(), 1);
  EXPECT_EQ(harness.library_.lastRequestFileId(), 1001u);
  harness.library_.Answer(-1);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.session_.elementId(), 1001u);

  // The session admits the switch again: the pending close runs once, to the empty editor,
  // with the answer that it already has.
  harness.session_.fakeActions().SetCanSelectImage(true);
  ProcessEvents();
  EXPECT_EQ(harness.library_.requestCount(), 1);
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
  EXPECT_EQ(harness.library_.requestCount(), 0);

  // The new result without the open image is accepted.
  harness.SetFilteredList({1000});
  harness.library_.SetQueryUpdating(false);
  ProcessEvents();
  EXPECT_EQ(harness.library_.requestCount(), 1);
  harness.library_.Answer(-1);
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
  // A loaded row answers the check at once, with no position read.
  EXPECT_EQ(harness.library_.requestCount(), 0);
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
  // The return itself does not decide: the answer of the query worker does.
  EXPECT_EQ(harness.library_.requestCount(), 1);
  EXPECT_EQ(harness.router_.openCount(), 0);
  harness.library_.Answer(-1);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 0);
  EXPECT_EQ(harness.session_.elementId(), 0u);
  // The return does not scroll the filmstrip; it keeps its own position.
  EXPECT_EQ(harness.host_->property("filmstripRevealCount").toInt(), 0);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// Issue 747: the open image is outside the loaded rows (the Library scrolled far away, or the
// filter changed) and the list still holds it. The return keeps it open.
TEST(EditorLibraryListSyncQmlTest, ReturnToEditorKeepsUnloadedImageThatListHolds) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.router_.openLibrary();
  ProcessEvents();
  harness.SetFilteredList({1000, 1002});
  harness.router_.ReturnToEditor();
  ProcessEvents();
  ASSERT_EQ(harness.library_.requestCount(), 1);
  harness.library_.Answer(57);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.session_.elementId(), 1001u);
  EXPECT_EQ(harness.router_.elementId(), 1001u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// An answer to a check that a newer accepted result replaced decides nothing.
TEST(EditorLibraryListSyncQmlTest, AnswerOfReplacedCheckIsIgnored) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.SetFilteredList({1000, 1002});
  harness.RequestSync();
  ASSERT_EQ(harness.library_.requestCount(), 1);
  const qulonglong first_request = harness.library_.lastRequestId();

  harness.library_.SetQueryUpdating(true);
  harness.library_.SetQueryUpdating(false);
  ProcessEvents();
  ASSERT_EQ(harness.library_.requestCount(), 2);

  harness.library_.Answer(-1, first_request);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.session_.elementId(), 1001u);

  harness.library_.Answer(3);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.session_.elementId(), 1001u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// Issue 746: open X from folder A, return to the Library, select folder B, and double-click Y.
// The router publishes the route to Y before the session runs the queued Switch, so the session
// still shows X. The check waits for the switch and then keeps Y, which folder B holds.
TEST(EditorLibraryListSyncQmlTest, OpenInNewFolderKeepsRouteTargetWhileSwitchIsQueued) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.router_.openLibrary();
  ProcessEvents();
  harness.SetFilteredList({3000, 3001, 3002});
  ProcessEvents();
  harness.session_.SetQueued(true);
  harness.router_.openEditor(3001, 4001);
  ProcessEvents();
  EXPECT_EQ(harness.session_.elementId(), 1001u);  // the Switch still waits in the queue
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.elementId(), 3001u);
  EXPECT_EQ(harness.library_.requestCount(), 0);

  harness.session_.CompleteQueuedOpen();
  ProcessEvents();
  EXPECT_EQ(harness.session_.elementId(), 3001u);
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 3001);
  EXPECT_EQ(harness.router_.elementId(), 3001u);
  EXPECT_EQ(harness.library_.requestCount(), 0);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(EditorLibraryListSyncQmlTest, EmptyEditorDoesNotOpenAnImageAfterFilterChange) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.router_.ShowEmptyEditor();
  harness.SetFilteredList({1000, 1002});
  harness.RequestSync();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.library_.requestCount(), 0);
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
  ASSERT_EQ(harness.library_.requestCount(), 1);
  harness.library_.Answer(-1);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 0);
  EXPECT_EQ(harness.session_.elementId(), 0u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// The toolbar return with the last photo that the session still has open routes to it at once;
// the check then closes it when the list does not hold it.
TEST(EditorLibraryListSyncQmlTest, ToolbarReturnWithOpenImageRoutesToItAndChecksOnWorker) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.router_.openLibrary();
  harness.SetFilteredList({1000, 1002});
  ProcessEvents();
  harness.OpenLastEditorImage(1001, 2001);
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 1001);
  ASSERT_EQ(harness.library_.requestCount(), 1);
  harness.library_.Answer(-1);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 2);
  EXPECT_EQ(harness.router_.lastElementId(), 0);
  EXPECT_EQ(harness.session_.elementId(), 0u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// The toolbar return with a last photo that is no longer open: the editor opens empty and opens
// the photo only when the list holds it, so a filtered-out photo is never loaded.
TEST(EditorLibraryListSyncQmlTest, ToolbarReturnOpensClosedLastImageOnlyWhenListHoldsIt) {
  EditorLibraryListSyncHarness harness;
  ASSERT_NE(harness.controller_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.router_.ShowEmptyEditor();
  harness.router_.openLibrary();
  harness.SetFilteredList({1000, 1002});
  ProcessEvents();
  harness.OpenLastEditorImage(1001, 2001);
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 0);
  ASSERT_EQ(harness.library_.requestCount(), 1);
  EXPECT_EQ(harness.library_.lastRequestFileId(), 1001u);
  harness.library_.Answer(-1);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.session_.elementId(), 0u);

  harness.router_.openLibrary();
  ProcessEvents();
  harness.OpenLastEditorImage(1001, 2001);
  ASSERT_EQ(harness.library_.requestCount(), 2);
  harness.library_.Answer(40);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 3);
  EXPECT_EQ(harness.router_.lastElementId(), 1001);
  EXPECT_EQ(harness.session_.elementId(), 1001u);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test

#include "editor_library_list_sync_qml_test.moc"
