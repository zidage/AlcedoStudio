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
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/library_selection.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

struct FilmstripRow {
  int     element_id = 0;
  int     image_id   = 0;
  QString file_name;
  int     rating = 0;
};

auto DefaultFilmstripRows() -> std::vector<FilmstripRow> {
  std::vector<FilmstripRow> rows;
  for (int i = 0; i < 8; ++i) {
    rows.push_back({1000 + i, 2000 + i, QStringLiteral("film_%1.arw").arg(i), i == 0 ? 3 : 0});
  }
  return rows;
}

// The library side of the filmstrip: the production thumbnail model and the production
// selection, so the tests read ratings and selection through the same objects as the app.
class FilmstripLibrary final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject* thumbnailModel READ thumbnailModel CONSTANT)
  Q_PROPERTY(LibrarySelection* selection READ selection CONSTANT)

 public:
  explicit FilmstripLibrary(QObject* parent = nullptr) : QObject(parent) {
    resetRows(DefaultFilmstripRows());
  }

  [[nodiscard]] auto thumbnailModel() -> QObject* { return &model_; }
  [[nodiscard]] auto model() -> AlbumThumbnailModel& { return model_; }
  [[nodiscard]] auto selection() -> LibrarySelection* { return &selection_; }

  void resetRows(const std::vector<FilmstripRow>& rows) {
    std::vector<AlbumItem> items;
    items.reserve(rows.size());
    for (const auto& row : rows) {
      AlbumItem item;
      item.element_id = static_cast<sl_element_id_t>(row.element_id);
      item.file_id    = static_cast<sl_element_id_t>(row.element_id);
      item.image_id   = static_cast<image_id_t>(row.image_id);
      item.file_name  = row.file_name;
      item.rating     = row.rating;
      items.push_back(std::move(item));
    }
    model_.resetModel(items, items.size());
  }

  // Library Select All: every photo of the query.
  Q_INVOKABLE void SelectAllInCurrentView() {
    selection_.Replace(model_.getItemsInRange(0, model_.count() - 1));
  }

  Q_INVOKABLE void SetThumbnailVisible(int element_id, int image_id, bool visible, int max_edge) {
    Q_UNUSED(image_id);
    calls_.push_back({element_id, visible, max_edge});
    last_max_edge_    = max_edge;
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

  [[nodiscard]] int  lastMaxEdge() const { return last_max_edge_; }

  [[nodiscard]] bool allVisibleCallsUseMaxEdge(int expected_max_edge) const {
    for (const auto& call : calls_) {
      if (call.visible && call.max_edge != expected_max_edge) {
        return false;
      }
    }
    return true;
  }

 signals:
  void thumbnailUpdated(int elementId, QString thumbUrl, bool loading, bool missingSource,
                        QString errorText);

 private:
  struct Call {
    int  element_id = 0;
    bool visible    = false;
    int  max_edge   = 0;
  };

  AlbumThumbnailModel model_;
  LibrarySelection    selection_{nullptr};
  QSet<QString>       pinned_;
  std::vector<Call>   calls_;
  int                 last_max_edge_ = 0;
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
  Q_PROPERTY(
      QString sessionState READ sessionState WRITE setSessionState NOTIFY sessionStateChanged)
  Q_PROPERTY(bool renderBusy READ renderBusy WRITE setRenderBusy NOTIFY renderBusyChanged)
  Q_PROPERTY(bool canDiscardCurrentCommit READ canDiscardCurrentCommit WRITE
                 setCanDiscardCurrentCommit NOTIFY canDiscardCurrentCommitChanged)
  Q_PROPERTY(QVariantMap actions READ actions CONSTANT)

 public:
  [[nodiscard]] bool        filmstripCollapsed() const { return filmstrip_collapsed_; }
  [[nodiscard]] double      filmstripExpandedHeight() const { return filmstrip_expanded_height_; }
  [[nodiscard]] double      filmstripScrollPosition() const { return filmstrip_scroll_position_; }
  [[nodiscard]] int         elementId() const { return element_id_; }
  [[nodiscard]] int         imageId() const { return image_id_; }
  [[nodiscard]] bool        hasImage() const { return true; }
  [[nodiscard]] QString     sessionState() const { return session_state_; }
  [[nodiscard]] bool        renderBusy() const { return render_busy_; }
  [[nodiscard]] bool        canDiscardCurrentCommit() const { return can_discard_; }
  [[nodiscard]] int         discardCount() const { return discard_count_; }
  [[nodiscard]] QVariantMap actions() const { return {{QStringLiteral("canSelectImage"), true}}; }

  void                      setFilmstripCollapsed(bool collapsed) {
    if (filmstrip_collapsed_ == collapsed) return;
    filmstrip_collapsed_ = collapsed;
    emit filmstripCollapsedChanged();
  }
  void setFilmstripExpandedHeight(double height) {
    const double clamped = qBound(168.0, height, 4096.0);
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
  double  filmstrip_expanded_height_ = 168.0;
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
  [[nodiscard]] bool    canSelectEditorImage() const { return can_select_; }
  [[nodiscard]] QString selectEditorImageReason() const { return QStringLiteral("Saving"); }
  void                  setCanSelectEditorImage(bool can_select) {
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
  Q_INVOKABLE void  openEditor(int element_id, int image_id) {
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
  FilmstripSession* session_         = nullptr;
  int               open_count_      = 0;
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
  [[nodiscard]] FilmstripRouter*  workspaceRouter() const { return router_; }
  [[nodiscard]] FilmstripSession* editorSession() const { return session_; }
  [[nodiscard]] FilmstripPolicy*  interactionPolicy() const { return policy_; }

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
            // Same routing as EditorWorkspace: filmstrip selection signals write
            // the shared Library selection.
            item.replaceSelection.connect(function(items) {
                appModules.library.selection.Replace(items)
            })
            item.imageSelectionChanged.connect(function(elementId, imageId, fileName, isHdr,
                                                        selected) {
                appModules.library.selection.SetImageSelected(elementId, imageId, fileName,
                                                             isHdr, selected)
            })
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

auto           QmlDirectory() -> QString {
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
  FilmstripQmlHarness() : router_(&session_), modules_(&library_, &router_, &session_, &policy_) {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Material"));
    // EditorFilmstrip.qml imports Alcedo.Main for the ShortcutRegistry
    // singleton; file-URL loads resolve it through C++ type registration.
    RegisterShortcutRegistryQmlType();
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
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

  FilmstripLibrary      library_;
  FilmstripSession      session_;
  FilmstripPolicy       policy_;
  FilmstripRouter       router_;
  FilmstripModules      modules_;
  QQmlApplicationEngine engine_;
  QQuickWindow*         window_ = nullptr;
  QStringList           warnings_;
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
  // Right is a registered navigation command: it activates the adjacent image
  // through the session router and moves filmstrip focus with it.
  QTest::keyClick(harness.window_, Qt::Key_Right);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 1001);
  EXPECT_EQ(harness.router_.lastImageId(), 2001);
  EXPECT_EQ(filmstrip->property("focusIndex").toInt(), 1);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 1);
  EXPECT_EQ(filmstrip->property("currentFileName").toString(), QStringLiteral("film_1.arw"));
  // Return still activates the focused row through the same session path.
  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 2);
  EXPECT_EQ(harness.router_.lastElementId(), 1001);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 1);
}

TEST(EditorFilmstripQmlTest, LeftAndRightSwitchToAdjacentImagesAndMoveFilmstripFocus) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 0);

  list->forceActiveFocus();
  QTest::keyClick(harness.window_, Qt::Key_Right);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(harness.router_.lastElementId(), 1001);
  EXPECT_EQ(filmstrip->property("focusIndex").toInt(), 1);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 1);

  QTest::keyClick(harness.window_, Qt::Key_Right);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 2);
  EXPECT_EQ(harness.router_.lastElementId(), 1002);
  EXPECT_EQ(filmstrip->property("focusIndex").toInt(), 2);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 2);

  QTest::keyClick(harness.window_, Qt::Key_Left);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 3);
  EXPECT_EQ(harness.router_.lastElementId(), 1001);
  EXPECT_EQ(filmstrip->property("focusIndex").toInt(), 1);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 1);
}

TEST(EditorFilmstripQmlTest, ArrowNavigationStopsAtFilmstripBounds) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);

  list->forceActiveFocus();
  // Already at index 0: Left is a no-op — no activation, no focus move.
  QTest::keyClick(harness.window_, Qt::Key_Left);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(filmstrip->property("focusIndex").toInt(), 0);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 0);

  // Walk to the last row, then Right must also stay inert at the bound.
  for (int i = 0; i < 7; ++i) {
    QTest::keyClick(harness.window_, Qt::Key_Right);
    ProcessEvents();
  }
  EXPECT_EQ(harness.router_.openCount(), 7);
  EXPECT_EQ(filmstrip->property("focusIndex").toInt(), 7);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 7);

  QTest::keyClick(harness.window_, Qt::Key_Right);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 7);
  EXPECT_EQ(filmstrip->property("focusIndex").toInt(), 7);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 7);
}

TEST(EditorFilmstripQmlTest, FilmstripSelectAllAndVersionsCreateMayShareCtrlAByFocus) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);

  list->forceActiveFocus();
  // Ctrl+A resolves through the editor.filmstrip scope while the strip owns
  // focus; the Versions binding on the same key stays out of the path here.
  QTest::keyClick(harness.window_, Qt::Key_A, Qt::ControlModifier);
  ProcessEvents();
  EXPECT_EQ(filmstrip->property("selectionAnchorIndex").toInt(), 0);
  // Select All must not activate another image.
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 0);
}

TEST(EditorFilmstripQmlTest, ModelResetRebindsToNewFilteredRows) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);

  harness.library_.resetRows({{5001, 6001, QStringLiteral("album_only.arw"), 0},
                              {5002, 6002, QStringLiteral("album_two.arw"), 0}});
  ProcessEvents();
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 2, 2000);
  EXPECT_EQ(filmstrip->property("totalCount").toInt(), 2);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), -1);

  harness.session_.setElementId(5002);
  harness.session_.setImageId(6002);
  ProcessEvents();
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 1);
  EXPECT_EQ(filmstrip->property("currentFileName").toString(), QStringLiteral("album_two.arw"));
}

// The strip creates delegates only for visible tiles (cacheBuffer 0): scroll the tile into view
// before reading it.
auto TileAt(QQuickItem* list, int index) -> QQuickItem* {
  constexpr int kContain = 4;  // ListView.Contain
  QMetaObject::invokeMethod(list, "positionViewAtIndex", Qt::DirectConnection, Q_ARG(int, index),
                            Q_ARG(int, kContain));
  ProcessEvents(10);
  QQuickItem* tile = nullptr;
  QMetaObject::invokeMethod(list, "itemAtIndex", Qt::DirectConnection,
                            Q_RETURN_ARG(QQuickItem*, tile), Q_ARG(int, index));
  return tile;
}

auto CardOf(QQuickItem* tile) -> QObject* {
  return tile ? tile->findChild<QObject*>(QStringLiteral("editorFilmstripTileCard")) : nullptr;
}

// Counts the tiles that show the editing state or the selected state.
auto HighlightedTileCount(QQuickItem* list) -> int {
  int count = 0;
  for (int index = 0; index < list->property("count").toInt(); ++index) {
    QObject* card = CardOf(TileAt(list, index));
    if (card && (card->property("editing").toBool() || card->property("selected").toBool())) {
      ++count;
    }
  }
  return count;
}

// Stars (ThumbnailStarRating) under @p root in the visual tree that are visible and show
// @p glyph. Repeater items have no QObject parent in the delegate, so findChildren misses them.
auto VisibleStarCount(QQuickItem* root, const QString& glyph) -> int {
  if (root == nullptr || !root->isVisible()) {
    return 0;
  }
  int count = 0;
  if (root->objectName().startsWith(QStringLiteral("thumbnailStar_")) &&
      root->property("text").toString() == glyph) {
    ++count;
  }
  for (auto* child : root->childItems()) {
    count += VisibleStarCount(child, glyph);
  }
  return count;
}

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

// Issue #589: the caption has one line, the file name and the edit mark; the rating is an
// overlay of only the filled stars at the bottom-right corner of the photo, on a short shade
// over the bottom part of the image. An unrated image has no overlay. The caption never covers
// the image.
TEST(EditorFilmstripQmlTest, OneLineCaptionBelowImageAndFilledStarsOverlayThePhoto) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* list = harness.list();
  ASSERT_NE(list, nullptr);
  ASSERT_TRUE(QTest::qWaitFor([&] { return list->property("count").toInt() == 8; }, 2000));

  QQuickItem* editing_tile = TileAt(list, 0);
  ASSERT_NE(editing_tile, nullptr);
  auto* surface   = FindItem(editing_tile, QStringLiteral("editorFilmstripTileSurface"));
  auto* frame     = FindItem(editing_tile, QStringLiteral("editorFilmstripThumbnailFrame"));
  auto* bar       = FindItem(editing_tile, QStringLiteral("editorFilmstripCaptionBar"));
  auto* image     = FindItem(editing_tile, QStringLiteral("editorFilmstripThumbnailImage"));
  auto* name      = FindItem(editing_tile, QStringLiteral("editorFilmstripFileNameLabel"));
  auto* edit_mark = FindItem(editing_tile, QStringLiteral("editorFilmstripEditMark"));
  auto* overlay   = FindItem(editing_tile, QStringLiteral("editorFilmstripRatingOverlay"));
  ASSERT_NE(surface, nullptr);
  ASSERT_NE(frame, nullptr);
  ASSERT_NE(bar, nullptr);
  ASSERT_NE(image, nullptr);
  ASSERT_NE(name, nullptr);
  ASSERT_NE(edit_mark, nullptr);
  ASSERT_NE(overlay, nullptr);
  // The former caption star line is gone.
  EXPECT_EQ(FindItem(editing_tile, QStringLiteral("editorFilmstripRatingLabel")), nullptr);

  // One caption line on the bottom edge of the frame; the image ends where it starts.
  const auto& theme = AppTheme::Instance();
  EXPECT_EQ(name->parentItem(), bar);
  EXPECT_EQ(bar->parentItem(), frame);
  EXPECT_NEAR(bar->height(), theme.lineHeightCaption() + theme.spaceXs() * 2, 0.5);
  EXPECT_NEAR(bar->y() + bar->height(), frame->height() - 1.0, 0.5);
  EXPECT_LE(image->y() + image->height(), bar->y() + 0.5);
  EXPECT_EQ(name->property("text").toString(), QStringLiteral("film_0.arw"));
  // An unedited image has no edit mark.
  EXPECT_FALSE(edit_mark->isVisible());

  // The rating overlay: three filled white stars and no empty star, on the bottom part of the
  // image only, in its bottom-right corner.
  EXPECT_TRUE(overlay->isVisible());
  EXPECT_EQ(VisibleStarCount(overlay, QStringLiteral("★")), 3);
  EXPECT_EQ(VisibleStarCount(overlay, QStringLiteral("☆")), 0);
  const QRectF image_rect = image->mapRectToScene(QRectF(0, 0, image->width(), image->height()));
  const QRectF overlay_rect =
      overlay->mapRectToScene(QRectF(0, 0, overlay->width(), overlay->height()));
  EXPECT_NEAR(overlay_rect.bottom(), image_rect.bottom(), 0.5);
  EXPECT_NEAR(overlay_rect.right(), image_rect.right(), 0.5);
  EXPECT_LT(overlay_rect.height(), image_rect.height() / 2.0);
  auto* stars = FindItem(overlay, QStringLiteral("editorFilmstripRatingStars"));
  ASSERT_NE(stars, nullptr);
  EXPECT_EQ(stars->property("filledColor").value<QColor>(), QColor(Qt::white));

  // An unrated image has no overlay and the same caption height.
  QQuickItem* unrated_tile = TileAt(list, 1);
  ASSERT_NE(unrated_tile, nullptr);
  auto* unrated_overlay = FindItem(unrated_tile, QStringLiteral("editorFilmstripRatingOverlay"));
  auto* unrated_bar     = FindItem(unrated_tile, QStringLiteral("editorFilmstripCaptionBar"));
  ASSERT_NE(unrated_overlay, nullptr);
  ASSERT_NE(unrated_bar, nullptr);
  EXPECT_FALSE(unrated_overlay->isVisible());
  EXPECT_NEAR(unrated_bar->height(), bar->height(), 0.5);

  // Editing tile: monochrome bone well, and the caption uses the ink of the well.
  ASSERT_TRUE(QTest::qWaitFor(
      [&] {
        return surface->property("color").value<QColor>() == theme.editorListSelectedFillColor();
      },
      2000));
  EXPECT_EQ(bar->property("color").value<QColor>(), theme.editorListSelectedFillColor());
  EXPECT_EQ(name->property("color").value<QColor>(), theme.editorListSelectedInkColor());
}

// A rating change shows on the tile at once (model dataChanged on the rating role, the same
// update that ImageController sends after a rating).
TEST(EditorFilmstripQmlTest, RatingChangeUpdatesTileStarOverlayImmediately) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();

  auto* list = harness.list();
  ASSERT_NE(list, nullptr);
  ASSERT_TRUE(QTest::qWaitFor([&] { return list->property("count").toInt() == 8; }, 2000));
  QQuickItem* tile = TileAt(list, 1);
  ASSERT_NE(tile, nullptr);
  auto* overlay = FindItem(tile, QStringLiteral("editorFilmstripRatingOverlay"));
  ASSERT_NE(overlay, nullptr);
  EXPECT_FALSE(overlay->isVisible());

  ASSERT_TRUE(harness.library_.model().updateRating(1001, 2001, 4));
  EXPECT_TRUE(overlay->isVisible());
  EXPECT_EQ(VisibleStarCount(overlay, QStringLiteral("★")), 4);
  EXPECT_EQ(VisibleStarCount(overlay, QStringLiteral("☆")), 0);
  EXPECT_EQ(tile->property("displayRating").toInt(), 4);

  ASSERT_TRUE(harness.library_.model().updateRating(1001, 2001, 0));
  EXPECT_FALSE(overlay->isVisible());
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// Issue #589: the edit mark of the open photo appears after its first commit and disappears
// after an undo back to the image root, without a reload of the strip. The editor session
// sends both as AlbumThumbnailModel::updateEditState (EditorSessionController and
// LibraryModule::SetAlbumItemEditState); the session side is covered by
// EditorVersionsPanelQmlTest.OpenPhotoEditStateFollowsTheHistoryHeadInTheAlbumCatalog.
TEST(EditorFilmstripQmlTest, EditMarkFollowsTheOpenPhotoEditStateWithoutReload) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();

  auto* list = harness.list();
  ASSERT_NE(list, nullptr);
  ASSERT_TRUE(QTest::qWaitFor([&] { return list->property("count").toInt() == 8; }, 2000));
  QQuickItem* tile = TileAt(list, 0);
  ASSERT_NE(tile, nullptr);
  auto* edit_mark = FindItem(tile, QStringLiteral("editorFilmstripEditMark"));
  ASSERT_NE(edit_mark, nullptr);
  EXPECT_FALSE(edit_mark->isVisible());
  QSignalSpy             resets(&harness.library_.model(), &QAbstractItemModel::modelReset);

  // First commit.
  constexpr std::int64_t kEditedAtNs = 1'788'429'600'000'000'000;
  ASSERT_TRUE(harness.library_.model().updateEditState(1000, 2000, kEditedAtNs,
                                                       QStringLiteral("Base Look")));
  EXPECT_TRUE(edit_mark->isVisible());
  EXPECT_TRUE(tile->property("isEdited").toBool());
  // A chip with a fill on the top-right corner of the photo, not in the caption.
  auto* image = FindItem(tile, QStringLiteral("editorFilmstripThumbnailImage"));
  auto* bar   = FindItem(tile, QStringLiteral("editorFilmstripCaptionBar"));
  ASSERT_NE(image, nullptr);
  ASSERT_NE(bar, nullptr);
  EXPECT_EQ(edit_mark->parentItem(), image->parentItem());
  EXPECT_EQ(edit_mark->property("color").value<QColor>(),
            AppTheme::Instance().thumbnailEditMarkFillColor());
  const QRectF image_rect = image->mapRectToScene(QRectF(0, 0, image->width(), image->height()));
  const QRectF mark_rect =
      edit_mark->mapRectToScene(QRectF(0, 0, edit_mark->width(), edit_mark->height()));
  EXPECT_TRUE(image_rect.contains(mark_rect)) << "mark outside the photo";
  EXPECT_LT(image_rect.right() - mark_rect.right(), 8.0);
  EXPECT_LT(mark_rect.top() - image_rect.top(), 8.0);
  EXPECT_LT(mark_rect.width(), 24.0);
  EXPECT_TRUE(edit_mark->property("tooltipText").toString().endsWith(QStringLiteral("\nBase Look")))
      << edit_mark->property("tooltipText").toString().toStdString();

  // Undo back to the image root.
  ASSERT_TRUE(harness.library_.model().updateEditState(1000, 2000, std::nullopt, {}));
  EXPECT_FALSE(edit_mark->isVisible());

  // The same delegate shows both states: no reload of the strip.
  EXPECT_EQ(resets.count(), 0);
  EXPECT_EQ(TileAt(list, 0), tile);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// Issue 442 item 2: the filmstrip shares the Library selection, but a Library-selected image
// that the editor does not show has its own light gray state. The editing tile keeps the bone
// well, so a click in the Library never moves the editing highlight.
TEST(EditorFilmstripQmlTest, LibrarySelectionShowsGraySelectedStateSeparateFromEditingTile) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();

  auto* list = harness.list();
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);

  // A single click in the Library selects image 3 and does not open it.
  harness.library_.selection()->Replace(harness.library_.model().getItemsInRange(3, 3));
  ProcessEvents();

  {
    QQuickItem* tile = TileAt(list, 0);
    ASSERT_NE(tile, nullptr);
    QObject* card = CardOf(tile);
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(card->property("editing").toBool());
    EXPECT_FALSE(card->property("selected").toBool());
    EXPECT_EQ(card->property("tileState").toString(), QStringLiteral("editing"));
    auto* surface = tile->findChild<QQuickItem*>(QStringLiteral("editorFilmstripTileSurface"));
    ASSERT_NE(surface, nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(surface->property("color").value<QColor>(),
                              AppTheme::Instance().editorListSelectedFillColor(), 2000);
  }
  {
    QQuickItem* tile = TileAt(list, 3);
    ASSERT_NE(tile, nullptr);
    QObject* card = CardOf(tile);
    ASSERT_NE(card, nullptr);
    EXPECT_FALSE(card->property("editing").toBool());
    EXPECT_TRUE(card->property("selected").toBool());
    EXPECT_EQ(card->property("tileState").toString(), QStringLiteral("selected"));
    auto* surface = tile->findChild<QQuickItem*>(QStringLiteral("editorFilmstripTileSurface"));
    ASSERT_NE(surface, nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(surface->property("color").value<QColor>(),
                              AppTheme::Instance().filmstripSelectedFillColor(), 2000);
  }
  EXPECT_NE(AppTheme::Instance().filmstripSelectedFillColor(),
            AppTheme::Instance().editorListSelectedFillColor());
  // The editor did not switch.
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_EQ(harness.session_.elementId(), 1000);

  // Ctrl+click in the filmstrip adds to the shared selection without opening the image.
  ASSERT_TRUE(QMetaObject::invokeMethod(
      harness.filmstrip(), "handleIndexSelection", Q_ARG(QVariant, QVariant(5)),
      Q_ARG(QVariant, QVariant(static_cast<int>(Qt::ControlModifier))),
      Q_ARG(QVariant, QVariant(false))));
  ProcessEvents();
  EXPECT_EQ(harness.library_.selection()->SelectedCount(), 2);
  EXPECT_TRUE(harness.library_.selection()->IsSelected(1005));
  EXPECT_TRUE(CardOf(TileAt(list, 5))->property("selected").toBool());
  EXPECT_EQ(harness.router_.openCount(), 0);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// Issue 442 item 3: after a click on image A and several Left/Right presses, the filmstrip shows
// one highlighted tile: the open image is also the only selected image.
TEST(EditorFilmstripQmlTest, LeftRightNavigationKeepsOneHighlightedTile) {
  FilmstripQmlHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();

  auto* filmstrip = harness.filmstrip();
  auto* list      = harness.list();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(list, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(list->property("count").toInt(), 8, 2000);

  // Plain click on image 1 opens it and selects it.
  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "handleIndexSelection",
                                        Q_ARG(QVariant, QVariant(1)), Q_ARG(QVariant, QVariant(0)),
                                        Q_ARG(QVariant, QVariant(true))));
  ProcessEvents();
  EXPECT_EQ(harness.session_.elementId(), 1001);
  EXPECT_TRUE(harness.library_.selection()->IsSelected(1001));
  EXPECT_EQ(HighlightedTileCount(list), 1);

  list->forceActiveFocus();
  for (int i = 0; i < 3; ++i) {
    QTest::keyClick(harness.window_, Qt::Key_Right);
    ProcessEvents();
    EXPECT_EQ(HighlightedTileCount(list), 1) << "after Right press " << (i + 1);
  }
  EXPECT_EQ(harness.session_.elementId(), 1004);
  EXPECT_EQ(harness.library_.selection()->SelectedCount(), 1);
  EXPECT_TRUE(harness.library_.selection()->IsSelected(1004));

  QTest::keyClick(harness.window_, Qt::Key_Left);
  ProcessEvents();
  EXPECT_EQ(harness.session_.elementId(), 1003);
  EXPECT_EQ(HighlightedTileCount(list), 1);
  EXPECT_TRUE(harness.library_.selection()->IsSelected(1003));
  EXPECT_FALSE(harness.library_.selection()->IsSelected(1001));
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
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
  // Right now activates the adjacent image through the router; Return
  // re-activates the focused row. Neither may disturb the strip's contentX.
  QTest::keyClick(harness.window_, Qt::Key_Right);
  ProcessEvents();
  EXPECT_EQ(harness.router_.openCount(), 1);
  EXPECT_EQ(filmstrip->property("selectedIndex").toInt(), 1);
  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();

  EXPECT_EQ(harness.router_.openCount(), 2);
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
// context menu. This test checks the emitted request payload, including that a
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

  ASSERT_TRUE(QMetaObject::invokeMethod(
      filmstrip, "requestContextMenuForIndex", Q_ARG(QVariant, QVariant(1)),
      Q_ARG(QVariant, QVariant(96.0)), Q_ARG(QVariant, QVariant(24.0))));
  // Out-of-range rows must not emit a request.
  ASSERT_TRUE(QMetaObject::invokeMethod(
      filmstrip, "requestContextMenuForIndex", Q_ARG(QVariant, QVariant(99)),
      Q_ARG(QVariant, QVariant(1.0)), Q_ARG(QVariant, QVariant(1.0))));
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
  ASSERT_TRUE(QMetaObject::invokeMethod(list, "itemAtIndex", Qt::DirectConnection,
                                        Q_RETURN_ARG(QQuickItem*, selected_tile), Q_ARG(int, 0)));
  ASSERT_NE(selected_tile, nullptr);
  const auto saving_badges =
      selected_tile->findChildren<QObject*>(QStringLiteral("editorFilmstripSavingBadge"));
  const auto render_badges =
      selected_tile->findChildren<QObject*>(QStringLiteral("editorFilmstripRenderBadge"));
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

  EXPECT_NEAR(filmstrip->property("minExpandedHeight").toReal(), 168.0, 0.5);
  EXPECT_EQ(filmstrip->property("filmstripThumbnailMaxEdge").toInt(), 512);
  EXPECT_NEAR(filmstrip->property("expandedHeight").toReal(), 168.0, 0.5);

  QQuickItem* tile_before = nullptr;
  ASSERT_TRUE(QMetaObject::invokeMethod(list, "itemAtIndex", Qt::DirectConnection,
                                        Q_RETURN_ARG(QQuickItem*, tile_before), Q_ARG(int, 0)));
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
  EXPECT_NEAR(harness.session_.filmstripExpandedHeight(), 168.0, 0.5);

  QQuickItem* tile_during = nullptr;
  ASSERT_TRUE(QMetaObject::invokeMethod(list, "itemAtIndex", Qt::DirectConnection,
                                        Q_RETURN_ARG(QQuickItem*, tile_during), Q_ARG(int, 0)));
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
  EXPECT_NEAR(harness.session_.filmstripExpandedHeight(), 168.0, 0.5);

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
  auto* handle_focus_fill =
      harness.window_->findChild<QQuickItem*>(QStringLiteral("editorFilmstripHandleFocusFill"));
  ASSERT_NE(handle_focus_fill, nullptr);
  EXPECT_NEAR(handle_focus_fill->property("radius").toReal(), AppTheme::Instance().panelRadius(),
              0.5);
  EXPECT_EQ(
      harness.window_->findChild<QQuickItem*>(QStringLiteral("editorFilmstripActiveTopSquareOff")),
      nullptr);
}

}  // namespace
}  // namespace alcedo::ui::test

#include "editor_filmstrip_qml_test.moc"
