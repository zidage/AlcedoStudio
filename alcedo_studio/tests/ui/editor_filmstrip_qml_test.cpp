//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QAbstractListModel>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHash>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <filesystem>
#include <vector>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

struct FilmstripRow {
  int element_id = 0;
  int image_id   = 0;
  QString file_name;
  int rating = 0;
};

class FilmstripModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(int totalCount READ count NOTIFY countChanged)
  Q_PROPERTY(bool hasMore READ hasMore CONSTANT)
  Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)

 public:
  enum Role {
    ElementIdRole = Qt::UserRole + 1,
    ImageIdRole,
    FileNameRole,
    RatingRole,
    ThumbUrlRole,
    ThumbLoadingRole,
    ThumbMissingSourceRole,
    ThumbErrorTextRole,
  };

  FilmstripModel() {
    for (int i = 0; i < 8; ++i) {
      rows_.push_back(
          {1000 + i, 2000 + i, QStringLiteral("film_%1.arw").arg(i), i == 0 ? 3 : 0});
    }
  }

  [[nodiscard]] int count() const { return static_cast<int>(rows_.size()); }
  [[nodiscard]] bool hasMore() const { return false; }
  [[nodiscard]] bool loading() const { return false; }

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
    return parent.isValid() ? 0 : count();
  }

  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
      return {};
    }
    const auto& row = rows_[static_cast<size_t>(index.row())];
    switch (role) {
      case ElementIdRole:
        return row.element_id;
      case ImageIdRole:
        return row.image_id;
      case FileNameRole:
        return row.file_name;
      case RatingRole:
        return row.rating;
      case ThumbUrlRole:
        return QString{};
      case ThumbLoadingRole:
        return false;
      case ThumbMissingSourceRole:
        return false;
      case ThumbErrorTextRole:
        return QString{};
      default:
        return {};
    }
  }

  [[nodiscard]] QHash<int, QByteArray> roleNames() const override {
    return {{ElementIdRole, "elementId"},
            {ImageIdRole, "imageId"},
            {FileNameRole, "fileName"},
            {RatingRole, "rating"},
            {ThumbUrlRole, "thumbUrl"},
            {ThumbLoadingRole, "thumbLoading"},
            {ThumbMissingSourceRole, "thumbMissingSource"},
            {ThumbErrorTextRole, "thumbErrorText"}};
  }

  Q_INVOKABLE QVariantMap getItemAt(int index) const {
    if (index < 0 || index >= count()) {
      return {};
    }
    const auto& row = rows_[static_cast<size_t>(index)];
    return {{QStringLiteral("elementId"), row.element_id},
            {QStringLiteral("imageId"), row.image_id},
            {QStringLiteral("fileName"), row.file_name},
            {QStringLiteral("rating"), row.rating}};
  }

  Q_INVOKABLE int rowByElementId(int element_id) const {
    for (int i = 0; i < count(); ++i) {
      if (rows_[static_cast<size_t>(i)].element_id == element_id) {
        return i;
      }
    }
    return -1;
  }

 signals:
  void countChanged();
  void loadingChanged();

 private:
  std::vector<FilmstripRow> rows_;
};

class FilmstripLibrary final : public QObject {
  Q_OBJECT
  Q_PROPERTY(FilmstripModel* thumbnailModel READ thumbnailModel CONSTANT)

 public:
  explicit FilmstripLibrary(QObject* parent = nullptr) : QObject(parent) {}

  [[nodiscard]] FilmstripModel* thumbnailModel() { return &model_; }

  Q_INVOKABLE void SetThumbnailVisible(int element_id, int image_id, bool visible, int max_edge) {
    Q_UNUSED(image_id);
    calls_.push_back({element_id, visible, max_edge});
    last_max_edge_ = max_edge;
    const QString key = QStringLiteral("%1").arg(element_id);
    if (visible) {
      pinned_.insert(key);
    } else {
      pinned_.remove(key);
    }
  }

  [[nodiscard]] bool isPinned(int element_id) const {
    return pinned_.contains(QStringLiteral("%1").arg(element_id));
  }

  [[nodiscard]] int releaseCount() const {
    int count = 0;
    for (const auto& call : calls_) {
      if (!call.visible) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] int lastMaxEdge() const { return last_max_edge_; }

  [[nodiscard]] bool allVisibleCallsUseMaxEdge(int expected_max_edge) const {
    for (const auto& call : calls_) {
      if (call.visible && call.max_edge != expected_max_edge) {
        return false;
      }
    }
    return true;
  }

 signals:
  void thumbnailUpdated(int elementId, QString dataUrl, bool loading, bool missingSource,
                        QString errorText);

 private:
  struct Call {
    int  element_id = 0;
    bool visible    = false;
    int  max_edge   = 0;
  };

  FilmstripModel    model_;
  QSet<QString>     pinned_;
  std::vector<Call> calls_;
  int               last_max_edge_ = 0;
};

class FilmstripSession final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool filmstripCollapsed READ filmstripCollapsed WRITE setFilmstripCollapsed NOTIFY
                 filmstripCollapsedChanged)
  Q_PROPERTY(double filmstripExpandedHeight READ filmstripExpandedHeight WRITE
                 setFilmstripExpandedHeight NOTIFY filmstripExpandedHeightChanged)
  Q_PROPERTY(double filmstripScrollPosition READ filmstripScrollPosition WRITE
                 setFilmstripScrollPosition NOTIFY filmstripScrollPositionChanged)
  Q_PROPERTY(int elementId READ elementId WRITE setElementId NOTIFY elementIdChanged)
  Q_PROPERTY(int imageId READ imageId WRITE setImageId NOTIFY imageIdChanged)
  Q_PROPERTY(bool hasImage READ hasImage CONSTANT)
  Q_PROPERTY(QString sessionState READ sessionState WRITE setSessionState NOTIFY sessionStateChanged)
  Q_PROPERTY(bool renderBusy READ renderBusy WRITE setRenderBusy NOTIFY renderBusyChanged)
  Q_PROPERTY(bool canDiscardCurrentCommit READ canDiscardCurrentCommit WRITE
                 setCanDiscardCurrentCommit NOTIFY canDiscardCurrentCommitChanged)
  Q_PROPERTY(QVariantMap actions READ actions CONSTANT)

 public:
  [[nodiscard]] bool filmstripCollapsed() const { return filmstrip_collapsed_; }
  [[nodiscard]] double filmstripExpandedHeight() const { return filmstrip_expanded_height_; }
  [[nodiscard]] double filmstripScrollPosition() const { return filmstrip_scroll_position_; }
  [[nodiscard]] int elementId() const { return element_id_; }
  [[nodiscard]] int imageId() const { return image_id_; }
  [[nodiscard]] bool hasImage() const { return true; }
  [[nodiscard]] QString sessionState() const { return session_state_; }
  [[nodiscard]] bool renderBusy() const { return render_busy_; }
  [[nodiscard]] bool canDiscardCurrentCommit() const { return can_discard_; }
  [[nodiscard]] int discardCount() const { return discard_count_; }
  [[nodiscard]] QVariantMap actions() const {
    return {{QStringLiteral("canSelectImage"), true}};
  }

  void setFilmstripCollapsed(bool collapsed) {
    if (filmstrip_collapsed_ == collapsed) return;
    filmstrip_collapsed_ = collapsed;
    emit filmstripCollapsedChanged();
  }
  void setFilmstripExpandedHeight(double height) {
    const double clamped = qBound(128.0, height, 4096.0);
    if (qFuzzyCompare(filmstrip_expanded_height_ + 1.0, clamped + 1.0)) return;
    filmstrip_expanded_height_ = clamped;
    emit filmstripExpandedHeightChanged();
  }
  void setFilmstripScrollPosition(double position) {
    if (qFuzzyCompare(filmstrip_scroll_position_ + 1.0, position + 1.0)) return;
    filmstrip_scroll_position_ = position;
    emit filmstripScrollPositionChanged();
  }
  void setElementId(int element_id) {
    if (element_id_ == element_id) return;
    element_id_ = element_id;
    emit elementIdChanged();
  }
  void setImageId(int image_id) {
    if (image_id_ == image_id) return;
    image_id_ = image_id;
    emit imageIdChanged();
  }
  void setSessionState(const QString& state) {
    if (session_state_ == state) return;
    session_state_ = state;
    emit sessionStateChanged();
  }
  void setRenderBusy(bool busy) {
    if (render_busy_ == busy) return;
    render_busy_ = busy;
    emit renderBusyChanged();
  }
  void setCanDiscardCurrentCommit(bool can_discard) {
    if (can_discard_ == can_discard) return;
    can_discard_ = can_discard;
    emit canDiscardCurrentCommitChanged();
  }

  Q_INVOKABLE void Discard() {
    ++discard_count_;
    setCanDiscardCurrentCommit(false);
  }

 signals:
  void filmstripCollapsedChanged();
  void filmstripExpandedHeightChanged();
  void filmstripScrollPositionChanged();
  void elementIdChanged();
  void imageIdChanged();
  void sessionStateChanged();
  void renderBusyChanged();
  void canDiscardCurrentCommitChanged();

 private:
  bool    filmstrip_collapsed_       = false;
  double  filmstrip_expanded_height_ = 128.0;
  double  filmstrip_scroll_position_ = 0.0;
  int     element_id_                = 1000;
  int     image_id_                  = 2000;
  QString session_state_             = QStringLiteral("Interactive");
  bool    render_busy_               = false;
  bool    can_discard_               = true;
  int     discard_count_             = 0;
};

class FilmstripPolicy final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool canSelectEditorImage READ canSelectEditorImage WRITE setCanSelectEditorImage
                 NOTIFY canSelectEditorImageChanged)
  Q_PROPERTY(QString selectEditorImageReason READ selectEditorImageReason CONSTANT)

 public:
  [[nodiscard]] bool canSelectEditorImage() const { return can_select_; }
  [[nodiscard]] QString selectEditorImageReason() const { return QStringLiteral("Saving"); }
  void setCanSelectEditorImage(bool can_select) {
    if (can_select_ == can_select) return;
    can_select_ = can_select;
    emit canSelectEditorImageChanged();
  }

 signals:
  void canSelectEditorImageChanged();

 private:
  bool can_select_ = true;
};

class FilmstripRouter final : public QObject {
  Q_OBJECT

 public:
  explicit FilmstripRouter(FilmstripSession* session, QObject* parent = nullptr)
      : QObject(parent), session_(session) {}

  [[nodiscard]] int openCount() const { return open_count_; }
  Q_INVOKABLE void openEditor(int element_id, int image_id) {
    ++open_count_;
    last_element_id_ = element_id;
    last_image_id_   = image_id;
    if (session_) {
      session_->setElementId(element_id);
      session_->setImageId(image_id);
    }
  }

  [[nodiscard]] int lastElementId() const { return last_element_id_; }
  [[nodiscard]] int lastImageId() const { return last_image_id_; }

 private:
  FilmstripSession* session_        = nullptr;
  int               open_count_     = 0;
  int               last_element_id_ = 0;
  int               last_image_id_   = 0;
};

class FilmstripModules final : public QObject {
  Q_OBJECT
  Q_PROPERTY(FilmstripLibrary* library READ library CONSTANT)
  Q_PROPERTY(FilmstripRouter* workspaceRouter READ workspaceRouter CONSTANT)
  Q_PROPERTY(FilmstripSession* editorSession READ editorSession CONSTANT)
  Q_PROPERTY(FilmstripPolicy* interactionPolicy READ interactionPolicy CONSTANT)

 public:
  FilmstripModules(FilmstripLibrary* library, FilmstripRouter* router, FilmstripSession* session,
                   FilmstripPolicy* policy, QObject* parent = nullptr)
      : QObject(parent), library_(library), router_(router), session_(session), policy_(policy) {}

  [[nodiscard]] FilmstripLibrary* library() const { return library_; }
  [[nodiscard]] FilmstripRouter* workspaceRouter() const { return router_; }
  [[nodiscard]] FilmstripSession* editorSession() const { return session_; }
  [[nodiscard]] FilmstripPolicy* interactionPolicy() const { return policy_; }

 private:
  FilmstripLibrary* library_ = nullptr;
  FilmstripRouter*  router_  = nullptr;
  FilmstripSession* session_ = nullptr;
  FilmstripPolicy*  policy_  = nullptr;
};

constexpr char kFilmstripHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    objectName: "editorFilmstripHarness"
    width: 520
    height: 640
    visible: true

    property var menuRequests: []

    Loader {
        id: filmstripLoader
        objectName: "filmstripLoader"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: item ? item.dockHeight : 0
        source: filmstripSourceUrl
        onLoaded: {
            item.editorSession = appModules.editorSession
            item.interactionPolicy = appModules.interactionPolicy
            item.theme = null
            item.contextMenuRequested.connect(function(menuItem, sceneX, sceneY) {
                root.menuRequests.push({
                    elementId: Number(menuItem.elementId),
                    imageId: Number(menuItem.imageId),
                    sceneX: Number(sceneX),
                    sceneY: Number(sceneY)
                })
            })
        }
    }
}
)";

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int milliseconds = 30) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

class FilmstripQmlHarness {
 public:
  FilmstripQmlHarness()
      : router_(&session_), modules_(&library_, &router_, &session_, &policy_) {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QObject::connect(&engine_, &QQmlEngine::warnings,
                     [this](const QList<QQmlError>& emitted) {
                       for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), &modules_);
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(
        QStringLiteral("filmstripSourceUrl"),
        QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorFilmstrip.qml")));
    engine_.loadData(QByteArray{kFilmstripHarnessQml},
                     QUrl(QStringLiteral("file:///EditorFilmstripHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
      if (window_) {
        window_->show();
        window_->requestActivate();
      }
    }
    ProcessEvents();
  }

  [[nodiscard]] QQuickItem* filmstrip() const {
    return window_ ? window_->findChild<QQuickItem*>(QStringLiteral("editorFilmstrip")) : nullptr;
  }
  [[nodiscard]] QQuickItem* list() const {
    return window_ ? window_->findChild<QQuickItem*>(QStringLiteral("editorFilmstripListView"))
                   : nullptr;
  }
  [[nodiscard]] QVariantList menuRequests() const {
    return window_ ? window_->property("menuRequests").toList() : QVariantList{};
  }

  FilmstripLibrary  library_;
  FilmstripSession  session_;
  FilmstripPolicy   policy_;
  FilmstripRouter   router_;
  FilmstripModules  modules_;
  QQmlApplicationEngine engine_;
  QQuickWindow*      window_ = nullptr;
  QStringList        warnings_;
};

TEST(EditorFilmstripQmlTest, SharedModelRoutesKeyboardSelectionAndUpdatesCurrentRow) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);
  EXPECT_EQ(filmstrip->property("totalCount").toInt(), 8);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 0);
  EXPECT_EQ(filmstrip->property("currentFileName").toString(), QStringLiteral("film_0.arw"));

  list->forceActiveFocus();
  QTest::keyClick(harness.window_, Qt::Key_Right);
  ProcessEvents();
  EXPECT_EQ(filmstrip->property("focusIndex").toInt(), 1);
  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 1001);
  EXPECT_EQ(harness.router_.lastImageId(), 2001);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 1);
  EXPECT_EQ(filmstrip->property("currentFileName").toString(), QStringLiteral("film_1.arw"));
}

TEST(EditorFilmstripQmlTest, FileNameSitsBelowThumbnailWithMonochromeSelectedTile) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* list = harness.list();
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);

  QQuickItem* selected_tile = nullptr;
  ASSERT_TRUE(QMetaObject::invokeMethod(
      list, "itemAtIndex", Qt::DirectConnection, Q_RETURN_ARG(QQuickItem*, selected_tile),
      Q_ARG(int, 0)));
  ASSERT_NE(selected_tile, nullptr);

  const auto surfaces = selected_tile->findChildren<QObject*>(
      QStringLiteral("editorFilmstripTileSurface"));
  const auto frames = selected_tile->findChildren<QObject*>(
      QStringLiteral("editorFilmstripThumbnailFrame"));
  const auto labels = selected_tile->findChildren<QObject*>(
      QStringLiteral("editorFilmstripFileNameLabel"));
  const auto rating_overlays = selected_tile->findChildren<QObject*>(
      QStringLiteral("editorFilmstripRatingOverlay"));
  const auto rating_labels = selected_tile->findChildren<QObject*>(
      QStringLiteral("editorFilmstripRatingLabel"));
  ASSERT_EQ(surfaces.size(), 1);
  ASSERT_EQ(frames.size(), 1);
  ASSERT_EQ(labels.size(), 1);
  ASSERT_EQ(rating_overlays.size(), 1);
  ASSERT_EQ(rating_labels.size(), 1);

  auto* frame = qobject_cast<QQuickItem*>(frames.front());
  auto* label = qobject_cast<QQuickItem*>(labels.front());
  ASSERT_NE(frame, nullptr);
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(labels.front()->property("text").toString(), QStringLiteral("film_0.arw"));
  EXPECT_TRUE(rating_overlays.front()->property("visible").toBool());
  EXPECT_EQ(rating_labels.front()->property("text").toString(), QStringLiteral("★★★☆☆"));
  // Monochrome rating badge: dark well + light stars (no gold accent).
  EXPECT_EQ(rating_overlays.front()->property("color").value<QColor>(),
            AppTheme::Instance().editorListSelectedInkColor());
  EXPECT_EQ(rating_labels.front()->property("color").value<QColor>(),
            AppTheme::Instance().editorListSelectedFillColor());
  EXPECT_GT(label->y(), frame->y());
  EXPECT_GT(label->height(), 0.0);
  EXPECT_EQ(surfaces.front()->property("color").value<QColor>(),
            AppTheme::Instance().editorListSelectedFillColor());
}

TEST(EditorFilmstripQmlTest, CollapseKeepsThumbnailPinsAndRestoresHorizontalScroll) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);
  ASSERT_TRUE(harness.library_.isPinned(1000));

  list->setProperty("contentX", 160.0);
  ProcessEvents();
  const double saved_scroll = harness.session_.filmstripScrollPosition();
  EXPECT_GT(saved_scroll, 0.0);
  const int releases_before = harness.library_.releaseCount();
  harness.session_.setSessionState(QStringLiteral("Saving"));
  EXPECT_TRUE(filmstrip->property("saving").toBool());
  list->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(list->property("activeFocus").toBool());

  harness.session_.setFilmstripCollapsed(true);
  ProcessEvents();
  EXPECT_TRUE(harness.session_.filmstripCollapsed());
  EXPECT_NEAR(filmstrip->height(), filmstrip->property("handleHeight").toReal(), 1.0);
  EXPECT_NE(harness.list(), nullptr);
  EXPECT_TRUE(harness.library_.isPinned(1000));
  EXPECT_EQ(harness.library_.releaseCount(), releases_before);
  EXPECT_TRUE(filmstrip->property("saving").toBool());

  harness.session_.setFilmstripCollapsed(false);
  ProcessEvents();
  EXPECT_NEAR(list->property("contentX").toReal(), saved_scroll, 2.0);
  EXPECT_TRUE(list->property("activeFocus").toBool());
  EXPECT_TRUE(filmstrip->property("saving").toBool());
}

// Selecting / opening an image from the filmstrip must not reset contentX so
// the selected tile becomes the leftmost item. Cross-view reveals (Library →
// filmstrip) still use applyFilmstripScrollTarget separately.
TEST(EditorFilmstripQmlTest, LocalSelectionKeepsHorizontalScrollUnchanged) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(list->property("contentWidth").toReal() > list->width() + 160.0, 2000);

  list->setProperty("contentX", 160.0);
  ProcessEvents();
  const double content_x_before = list->property("contentX").toReal();
  EXPECT_NEAR(content_x_before, 160.0, 2.0);

  list->forceActiveFocus();
  ProcessEvents();
  QTest::keyClick(harness.window_, Qt::Key_Right);
  ProcessEvents();
  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();

  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 1);
  EXPECT_NEAR(list->property("contentX").toReal(), content_x_before, 2.0);

  // Direct session selection change (same path as openEditor) also must not
  // pin the new row to ListView.Beginning.
  harness.session_.setElementId(1005);
  harness.session_.setImageId(2005);
  ProcessEvents();
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 5);
  EXPECT_NEAR(list->property("contentX").toReal(), content_x_before, 2.0);
}

// The filmstrip no longer owns a local menu: right-click forwards a menu
// request (clicked row + scene point) to Main, which opens the shared image
// context menu. This test pins the emission contract, including that a
// non-current row (row 1 while the session sits on row 0) still produces a
// request targeted at that row.
TEST(EditorFilmstripQmlTest, ContextMenuRequestCarriesClickedRowAndScenePoint) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  ASSERT_NE(filmstrip, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(harness.list() != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(harness.library_.isPinned(1000), 2000);

  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "requestContextMenuForIndex",
                                        Q_ARG(QVariant, QVariant(1)),
                                        Q_ARG(QVariant, QVariant(96.0)),
                                        Q_ARG(QVariant, QVariant(24.0))));
  // Out-of-range rows must not emit a request.
  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "requestContextMenuForIndex",
                                        Q_ARG(QVariant, QVariant(99)),
                                        Q_ARG(QVariant, QVariant(1.0)),
                                        Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents();

  const QVariantList requests = harness.menuRequests();
  ASSERT_EQ(requests.size(), 1);
  const QVariantMap payload = requests.first().toMap();
  EXPECT_EQ(payload.value(QStringLiteral("elementId")).toInt(), 1001);
  EXPECT_EQ(payload.value(QStringLiteral("imageId")).toInt(), 2001);
  EXPECT_DOUBLE_EQ(payload.value(QStringLiteral("sceneX")).toDouble(), 96.0);
  EXPECT_DOUBLE_EQ(payload.value(QStringLiteral("sceneY")).toDouble(), 24.0);
}

TEST(EditorFilmstripQmlTest, SelectedTileShowsSavingAndRenderBadgesWhileSessionBusy) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  ASSERT_NE(filmstrip, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(harness.list() != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(harness.library_.isPinned(1000), 2000);

  harness.session_.setCanDiscardCurrentCommit(false);
  harness.session_.setSessionState(QStringLiteral("Saving"));
  harness.session_.setRenderBusy(true);
  ProcessEvents();
  auto* list = harness.list();
  ASSERT_NE(list, nullptr);
  QQuickItem* selected_tile = nullptr;
  ASSERT_TRUE(QMetaObject::invokeMethod(
      list, "itemAtIndex", Qt::DirectConnection, Q_RETURN_ARG(QQuickItem*, selected_tile),
      Q_ARG(int, 0)));
  ASSERT_NE(selected_tile, nullptr);
  const auto saving_badges = selected_tile->findChildren<QObject*>(
      QStringLiteral("editorFilmstripSavingBadge"));
  const auto render_badges = selected_tile->findChildren<QObject*>(
      QStringLiteral("editorFilmstripRenderBadge"));
  ASSERT_FALSE(saving_badges.isEmpty());
  ASSERT_FALSE(render_badges.isEmpty());
  int visible_saving = 0;
  int visible_render = 0;
  for (QObject* badge : saving_badges) visible_saving += badge->property("visible").toBool();
  for (QObject* badge : render_badges) visible_render += badge->property("visible").toBool();
  EXPECT_EQ(visible_saving, 1);
  EXPECT_EQ(visible_render, 1);
}

// Vertical drag on the control row grows the dock and thumbnail tiles while the
// decode edge stays fixed at 512 (no Library-style dynamic re-request).
TEST(EditorFilmstripQmlTest, HeightResizeGrowsTilesWithoutChangingThumbnailMaxEdge) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);

  EXPECT_NEAR(filmstrip->property("minExpandedHeight").toReal(), 128.0, 0.5);
  EXPECT_EQ(filmstrip->property("filmstripThumbnailMaxEdge").toInt(), 512);
  EXPECT_NEAR(filmstrip->property("expandedHeight").toReal(), 128.0, 0.5);

  QQuickItem* tile_before = nullptr;
  ASSERT_TRUE(QMetaObject::invokeMethod(
      list, "itemAtIndex", Qt::DirectConnection, Q_RETURN_ARG(QQuickItem*, tile_before),
      Q_ARG(int, 0)));
  ASSERT_NE(tile_before, nullptr);
  const qreal tile_height_before = tile_before->height();
  const qreal tile_width_before  = tile_before->width();
  ASSERT_TRUE(harness.library_.isPinned(1000));
  ASSERT_TRUE(harness.library_.allVisibleCallsUseMaxEdge(512));

  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "setExpandedHeightLive",
                                        Q_ARG(QVariant, QVariant(220.0))));
  ProcessEvents();
  EXPECT_NEAR(filmstrip->property("expandedHeight").toReal(), 220.0, 0.5);
  // Live drag does not commit to the session until release.
  EXPECT_NEAR(harness.session_.filmstripExpandedHeight(), 128.0, 0.5);

  QQuickItem* tile_during = nullptr;
  ASSERT_TRUE(QMetaObject::invokeMethod(
      list, "itemAtIndex", Qt::DirectConnection, Q_RETURN_ARG(QQuickItem*, tile_during),
      Q_ARG(int, 0)));
  ASSERT_NE(tile_during, nullptr);
  EXPECT_GT(tile_during->height(), tile_height_before);
  EXPECT_GT(tile_during->width(), tile_width_before);
  EXPECT_EQ(filmstrip->property("filmstripThumbnailMaxEdge").toInt(), 512);
  // ListView may recycle delegates on size change, but every pin request stays
  // at the fixed filmstrip edge (no Library-style dynamic resolution).
  EXPECT_TRUE(harness.library_.allVisibleCallsUseMaxEdge(512));
  EXPECT_EQ(harness.library_.lastMaxEdge(), 512);
  EXPECT_TRUE(harness.library_.isPinned(1000));

  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "commitExpandedHeight"));
  ProcessEvents();
  EXPECT_NEAR(harness.session_.filmstripExpandedHeight(), 220.0, 0.5);
  EXPECT_NEAR(filmstrip->property("expandedHeight").toReal(), 220.0, 0.5);

  // Floor is the default proportion; values below min clamp up.
  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "setExpandedHeightLive",
                                        Q_ARG(QVariant, QVariant(40.0))));
  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "commitExpandedHeight"));
  ProcessEvents();
  EXPECT_NEAR(harness.session_.filmstripExpandedHeight(), 128.0, 0.5);

  // Ceiling is half the harness window (640 → 320).
  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "setExpandedHeightLive",
                                        Q_ARG(QVariant, QVariant(900.0))));
  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "commitExpandedHeight"));
  ProcessEvents();
  EXPECT_NEAR(harness.session_.filmstripExpandedHeight(), 320.0, 1.0);
  EXPECT_EQ(filmstrip->property("filmstripThumbnailMaxEdge").toInt(), 512);
  EXPECT_TRUE(harness.library_.allVisibleCallsUseMaxEdge(512));
  EXPECT_EQ(harness.library_.lastMaxEdge(), 512);
}

// Filmstrip shell keeps a stable panelRadius silhouette (not tied to window
// active/focus). Coupling radius to focus made top corners vanish while working.
TEST(EditorFilmstripQmlTest, ShellKeepsStablePanelRadiusIndependentOfWindowFocus) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  ASSERT_NE(filmstrip, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(harness.list() != nullptr, 2000);

  auto* shell = harness.window_->findChild<QQuickItem*>(QStringLiteral("editorFilmstripShell"));
  ASSERT_NE(shell, nullptr);
  EXPECT_NEAR(shell->property("radius").toReal(), AppTheme::Instance().panelRadius(), 0.5);
  auto* handle_focus_fill = harness.window_->findChild<QQuickItem*>(
      QStringLiteral("editorFilmstripHandleFocusFill"));
  ASSERT_NE(handle_focus_fill, nullptr);
  EXPECT_NEAR(handle_focus_fill->property("radius").toReal(),
              AppTheme::Instance().panelRadius(), 0.5);
  EXPECT_EQ(harness.window_->findChild<QQuickItem*>(
                QStringLiteral("editorFilmstripActiveTopSquareOff")),
            nullptr);
}

}  // namespace
}  // namespace alcedo::ui::test

#include "editor_filmstrip_qml_test.moc"
