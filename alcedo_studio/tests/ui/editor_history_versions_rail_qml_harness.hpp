//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_history_versions_rail_qml_harness.hpp
/// @brief Shared fakes and helpers for History / Versions panel QML tests.

#pragma once

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "app/editor_session_service.hpp"
#include "type/hash_type.hpp"
#include "ui/alcedo_main/album_backend/editor_history_models.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace rail_harness {

inline auto StableId(std::uint64_t value) -> Hash128 { return Hash128(value, 0); }

inline auto MakeVersion(const Hash128& id, std::string name, const head_commit_hash_t& head,
                        bool active) -> EditorHistoryVersion {
  EditorHistoryVersion version;
  version.version_id       = id;
  version.display_name     = std::move(name);
  version.head_commit_hash = head;
  version.created_at       = static_cast<std::time_t>(id.low64());
  version.updated_at       = version.created_at;
  version.active           = active;
  return version;
}

inline auto MakeCommit(const Hash128& id, std::string field_key, std::string before_value_json,
                       std::string after_value_json,
                       const head_commit_hash_t&     first_parent  = std::nullopt,
                       const std::optional<Hash128>& second_parent = std::nullopt,
                       EditCommitKind                kind = EditCommitKind::kEdit,
                       EditorHistoryTimelinePosition position =
                           EditorHistoryTimelinePosition::Applied) -> EditorHistoryCommit {
  EditorHistoryCommit commit;
  commit.commit_hash        = id;
  commit.first_parent_hash  = first_parent;
  commit.second_parent_hash = second_parent;
  commit.kind               = kind;
  commit.created_at_ns      = id.low64();
  commit.field_key          = std::move(field_key);
  commit.before_value_json  = std::move(before_value_json);
  commit.after_value_json   = std::move(after_value_json);
  commit.before_enabled     = true;
  commit.after_enabled      = true;
  commit.position           = position;
  return commit;
}

class RecordingEditorSessionBackend final : public IEditorSessionBackend {
 public:
  RecordingEditorSessionBackend() {
    const auto first_version   = StableId(1);
    const auto second_version  = StableId(2);
    const auto first_commit    = StableId(11);
    const auto second_commit   = StableId(12);
    const auto merge_commit    = StableId(13);
    const auto incoming_commit = StableId(14);

    snapshot_.active_version_id = first_version;
    snapshot_.active_head       = merge_commit;
    snapshot_.versions          = {
        MakeVersion(first_version, "Base Look", merge_commit, true),
        MakeVersion(second_version, "Alternate Look", first_commit, false),
    };
    snapshot_.commits = {
        MakeCommit(merge_commit, "merge", std::string{}, std::string{}, second_commit,
                   incoming_commit, EditCommitKind::kMerge,
                   EditorHistoryTimelinePosition::Current),
        MakeCommit(second_commit, "contrast", R"({"contrast":0.0})", R"({"contrast":12.0})",
                   first_commit),
        MakeCommit(first_commit, "exposure", R"({"exposure":0.0})", R"({"exposure":0.35})"),
    };
    snapshot_.recovered_head = true;
    snapshot_.can_undo       = true;
  }

  [[nodiscard]] auto state() const -> EditorSessionState override { return state_; }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override { return identity_; }
  [[nodiscard]] auto active() const -> bool override {
    return state_ != EditorSessionState::NoImage;
  }
  [[nodiscard]] auto has_image() const -> bool override { return has_image_; }
  [[nodiscard]] auto has_pending_recovery() const -> bool override { return recovery_pending_; }
  [[nodiscard]] auto last_error() const -> std::string override { return last_error_; }
  [[nodiscard]] auto action_availability() const -> alcedo::EditorActionAvailability override {
    alcedo::EditorActionAvailability availability;
    auto allow = [&](alcedo::EditorAction action, bool enabled) {
      availability.decisions[static_cast<std::size_t>(action)].allowed = enabled;
    };
    allow(alcedo::EditorAction::Undo, snapshot_.can_undo);
    allow(alcedo::EditorAction::Redo, snapshot_.can_redo);
    allow(alcedo::EditorAction::MoveHead, true);
    allow(alcedo::EditorAction::ApplyPaste, true);
    allow(alcedo::EditorAction::RetrySave, recovery_pending_);
    allow(alcedo::EditorAction::DiscardAndContinue, recovery_pending_);
    allow(alcedo::EditorAction::CancelPendingNavigation, recovery_pending_);
    return availability;
  }

  void SetPresentationSinkId(PresentationSinkId) override {}
  void SetPresentationSize(int, int) override {}

  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    identity_.element_id = element_id;
    identity_.image_id   = image_id;
    has_image_           = true;
    state_               = EditorSessionState::Interactive;
    NotifyHistoryChange();
    return Accepted("Opened");
  }

  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    return Open(element_id, image_id);
  }

  auto CheckoutVersion(const version_ref_id_t& version_id) -> EditorSessionResult override {
    const auto it =
        std::find_if(snapshot_.versions.begin(), snapshot_.versions.end(),
                     [&](const auto& version) { return version.version_id == version_id; });
    if (it == snapshot_.versions.end()) return Rejected("Unknown Version");

    for (auto& version : snapshot_.versions) version.active = version.version_id == version_id;
    snapshot_.active_version_id = version_id;
    snapshot_.active_head       = it->head_commit_hash;
    last_checkout_id_           = version_id;
    ++checkout_count_;
    NotifyHistoryChange();
    return Accepted("Checked out");
  }

  auto CreateRootVersion(std::string display_name) -> EditorSessionResult override {
    if (block_version_ops_) {
      ++blocked_create_count_;
      return Rejected("Version create blocked");
    }
    const auto id = StableId(next_version_id_++);
    for (auto& version : snapshot_.versions) version.active = false;
    snapshot_.versions.push_back(MakeVersion(id, std::move(display_name), std::nullopt, true));
    snapshot_.active_version_id = id;
    snapshot_.active_head       = std::nullopt;
    snapshot_.can_undo          = false;
    snapshot_.can_redo          = false;
    snapshot_.recovered_head    = false;
    last_created_id_            = id;
    ++create_count_;
    NotifyHistoryChange();
    return Accepted("Root Version created");
  }

  auto BranchFromCommit(const commit_hash_t& commit_id, std::string display_name)
      -> EditorSessionResult override {
    last_branch_name_ = display_name;
    const auto id = StableId(next_version_id_++);
    for (auto& version : snapshot_.versions) version.active = false;
    snapshot_.versions.push_back(MakeVersion(id, std::move(display_name), commit_id, true));
    snapshot_.active_version_id = id;
    snapshot_.active_head       = commit_id;
    last_branch_commit_         = commit_id;
    last_created_id_            = id;
    ++branch_count_;
    ++create_count_;
    NotifyHistoryChange();
    return Accepted("Branch created");
  }

  auto RenameVersion(const version_ref_id_t& version_id, std::string display_name)
      -> EditorSessionResult override {
    if (block_version_ops_) {
      ++blocked_rename_count_;
      return Rejected("Version rename blocked");
    }
    const auto it =
        std::find_if(snapshot_.versions.begin(), snapshot_.versions.end(),
                     [&](const auto& version) { return version.version_id == version_id; });
    if (it == snapshot_.versions.end()) return Rejected("Unknown Version");
    it->display_name = std::move(display_name);
    last_rename_id_  = version_id;
    ++rename_count_;
    NotifyHistoryChange();
    return Accepted("Version renamed");
  }

  auto RemoveVersion(const version_ref_id_t& version_id) -> EditorSessionResult override {
    const auto it =
        std::find_if(snapshot_.versions.begin(), snapshot_.versions.end(),
                     [&](const auto& version) { return version.version_id == version_id; });
    if (it == snapshot_.versions.end() || it->active) return Rejected("Version cannot be removed");
    snapshot_.versions.erase(it);
    last_removed_id_ = version_id;
    ++remove_count_;
    NotifyHistoryChange();
    return Accepted("Version removed");
  }

  auto PasteAdjustments(const AdjustmentTransferPackage&, std::string)
      -> EditorSessionResult override {
    ++paste_count_;
    NotifyHistoryChange();
    return Accepted("Adjustments pasted");
  }

  auto BeginMerge(const AdjustmentTransferPackage&, AdjustmentMergePreview*)
      -> EditorSessionResult override {
    ++begin_merge_count_;
    return Accepted("Merge started");
  }

  auto CompleteMerge(const std::vector<AdjustmentMergeResolution>& resolutions)
      -> EditorSessionResult override {
    last_merge_resolution_count_ = resolutions.size();
    ++complete_merge_count_;
    NotifyHistoryChange();
    return Accepted("Merge completed");
  }

  auto CancelMerge() -> EditorSessionResult override {
    NotifyHistoryChange();
    return Accepted("Merge cancelled");
  }

  auto RetrySave() -> EditorSessionResult override {
    ++retry_save_count_;
    recovery_pending_ = false;
    last_error_.clear();
    NotifyHistoryChange();
    return Accepted("Save retried");
  }

  auto DiscardAndContinue() -> EditorSessionResult override {
    ++discard_count_;
    recovery_pending_ = false;
    last_error_.clear();
    NotifyHistoryChange();
    return Accepted("Discarded and continued");
  }

  auto CancelPendingNavigation() -> EditorSessionResult override {
    ++cancel_recovery_count_;
    recovery_pending_ = false;
    last_error_.clear();
    NotifyHistoryChange();
    return Accepted("Pending navigation cancelled");
  }

  auto Close(bool) -> EditorSessionResult override {
    state_     = EditorSessionState::NoImage;
    has_image_ = false;
    NotifyHistoryChange();
    return Accepted("Closed");
  }

  auto Shutdown() -> EditorSessionResult override { return Close(true); }
  auto Discard() -> EditorSessionResult override { return Accepted("Discarded"); }

  auto Undo() -> EditorSessionResult override {
    ++undo_count_;
    snapshot_.can_undo = false;
    snapshot_.can_redo = true;
    NotifyHistoryChange();
    return Accepted("Undone");
  }

  auto Redo() -> EditorSessionResult override {
    ++redo_count_;
    snapshot_.can_undo = true;
    snapshot_.can_redo = false;
    NotifyHistoryChange();
    return Accepted("Redone");
  }

  auto MoveHeadToCommit(const commit_hash_t& commit_id) -> EditorSessionResult override {
    ++move_head_count_;
    last_move_head_commit_ = commit_id;
    snapshot_.active_head  = commit_id;
    for (auto& commit : snapshot_.commits) {
      commit.position = (commit.commit_hash == commit_id)
                            ? EditorHistoryTimelinePosition::Current
                            : EditorHistoryTimelinePosition::Applied;
    }
    NotifyHistoryChange();
    return Accepted("Head moved");
  }

  [[nodiscard]] auto history_snapshot() -> EditorHistorySnapshot override { return snapshot_; }
  [[nodiscard]] auto history_revision() const -> std::uint64_t override { return history_revision_; }

  [[nodiscard]] auto last_checkout_id() const -> Hash128 { return last_checkout_id_; }
  [[nodiscard]] auto last_created_id() const -> Hash128 { return last_created_id_; }
  [[nodiscard]] auto last_rename_id() const -> Hash128 { return last_rename_id_; }
  [[nodiscard]] auto last_removed_id() const -> Hash128 { return last_removed_id_; }
  [[nodiscard]] auto checkout_count() const -> int { return checkout_count_; }
  [[nodiscard]] auto create_count() const -> int { return create_count_; }
  [[nodiscard]] auto rename_count() const -> int { return rename_count_; }
  [[nodiscard]] auto remove_count() const -> int { return remove_count_; }
  [[nodiscard]] auto undo_count() const -> int { return undo_count_; }
  [[nodiscard]] auto redo_count() const -> int { return redo_count_; }
  [[nodiscard]] auto paste_count() const -> int { return paste_count_; }
  [[nodiscard]] auto begin_merge_count() const -> int { return begin_merge_count_; }
  [[nodiscard]] auto complete_merge_count() const -> int { return complete_merge_count_; }
  [[nodiscard]] auto last_merge_resolution_count() const -> std::size_t {
    return last_merge_resolution_count_;
  }
  [[nodiscard]] auto retry_save_count() const -> int { return retry_save_count_; }
  [[nodiscard]] auto discard_count() const -> int { return discard_count_; }
  [[nodiscard]] auto cancel_recovery_count() const -> int { return cancel_recovery_count_; }
  [[nodiscard]] auto move_head_count() const -> int { return move_head_count_; }
  [[nodiscard]] auto branch_count() const -> int { return branch_count_; }
  [[nodiscard]] auto last_move_head_commit() const -> Hash128 { return last_move_head_commit_; }
  [[nodiscard]] auto last_branch_commit() const -> Hash128 { return last_branch_commit_; }
  [[nodiscard]] auto last_branch_name() const -> const std::string& { return last_branch_name_; }
  [[nodiscard]] auto blocked_create_count() const -> int { return blocked_create_count_; }
  [[nodiscard]] auto blocked_rename_count() const -> int { return blocked_rename_count_; }

  void SetRecovery(bool pending, std::string error = {}) {
    recovery_pending_ = pending;
    last_error_       = std::move(error);
    NotifyHistoryChange();
  }

  void SetBlockVersionOps(bool block) { block_version_ops_ = block; }

 private:
  auto Accepted(const char* message) const -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Accepted;
    result.state    = state_;
    result.identity = identity_;
    result.message  = message;
    return result;
  }

  auto Rejected(const char* message) const -> EditorSessionResult {
    auto result = Accepted(message);
    result.kind = EditorSessionResultKind::Rejected;
    return result;
  }

  /// Bump the history revision then notify, mirroring EditorSessionService's
  /// BumpHistoryRevision + NotifyChange on history-display-affecting changes.
  /// Every NotifyChange in this fake follows a real mutation, so the
  /// controller emits exactly one HistoryChanged per mutation.
  void NotifyHistoryChange() {
    ++history_revision_;
    NotifyChange();
  }

  std::uint64_t         history_revision_            = 0;

  EditorSessionState    state_     = EditorSessionState::Interactive;
  bool                  has_image_ = true;
  EditorSessionIdentity identity_{1, 2};
  EditorHistorySnapshot snapshot_;
  std::uint64_t         next_version_id_ = 20;
  Hash128               last_checkout_id_;
  Hash128               last_created_id_;
  Hash128               last_rename_id_;
  Hash128               last_removed_id_;
  Hash128               last_move_head_commit_;
  Hash128               last_branch_commit_;
  std::string           last_branch_name_;
  int                   checkout_count_              = 0;
  int                   create_count_                = 0;
  int                   rename_count_                = 0;
  int                   remove_count_                = 0;
  int                   move_head_count_             = 0;
  int                   branch_count_                = 0;
  int                   undo_count_                  = 0;
  int                   redo_count_                  = 0;
  int                   paste_count_                 = 0;
  int                   begin_merge_count_           = 0;
  int                   complete_merge_count_        = 0;
  std::size_t           last_merge_resolution_count_ = 0;
  bool                  recovery_pending_            = false;
  std::string           last_error_;
  int                   retry_save_count_      = 0;
  int                   discard_count_         = 0;
  int                   cancel_recovery_count_ = 0;
  bool                  block_version_ops_     = false;
  int                   blocked_create_count_  = 0;
  int                   blocked_rename_count_  = 0;
};

class RecordingInteractionPolicy final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool canCheckoutVersion READ canCheckoutVersion CONSTANT)
  Q_PROPERTY(QString checkoutVersionReason READ checkoutVersionReason CONSTANT)

 public:
  [[nodiscard]] auto canCheckoutVersion() const -> bool { return true; }
  [[nodiscard]] auto checkoutVersionReason() const -> QString { return {}; }
};

class RecordingAdjustmentTransfer final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool packageAvailable READ packageAvailable CONSTANT)

 public:
  [[nodiscard]] auto packageAvailable() const -> bool { return true; }

  Q_INVOKABLE QVariantMap PasteIntoEditor(QObject*) {
    ++paste_count_;
    return {{QStringLiteral("success"), true},
            {QStringLiteral("message"), QStringLiteral("Adjustments pasted")}};
  }

  Q_INVOKABLE QVariantMap BeginMergeIntoEditor(QObject*) {
    ++begin_merge_count_;
    const QVariantMap conflict{
        {QStringLiteral("fieldKey"), QStringLiteral("exposure")},
        {QStringLiteral("currentValue"), 0.0},
        {QStringLiteral("incomingValue"), 1.0},
        {QStringLiteral("currentEnabled"), true},
        {QStringLiteral("incomingEnabled"), true},
    };
    return {{QStringLiteral("success"), true},
            {QStringLiteral("hasConflicts"), true},
            {QStringLiteral("conflicts"), QVariantList{conflict}},
            {QStringLiteral("message"), QStringLiteral("Choose every changed field")}};
  }

  Q_INVOKABLE QVariantMap CompleteMergeIntoEditor(QObject*, const QVariant& resolutions) {
    ++complete_merge_count_;
    last_resolution_count_ = resolutions.toList().size();
    if (!resolutions.toList().isEmpty()) {
      last_resolution_ = resolutions.toList().front().toMap();
    }
    return {{QStringLiteral("success"), true},
            {QStringLiteral("message"), QStringLiteral("Merge completed")}};
  }

  Q_INVOKABLE QVariantMap CancelMergeIntoEditor(QObject*) {
    return {{QStringLiteral("success"), true},
            {QStringLiteral("message"), QStringLiteral("Merge cancelled")}};
  }

  [[nodiscard]] auto paste_count() const -> int { return paste_count_; }
  [[nodiscard]] auto begin_merge_count() const -> int { return begin_merge_count_; }
  [[nodiscard]] auto complete_merge_count() const -> int { return complete_merge_count_; }
  [[nodiscard]] auto last_resolution_count() const -> int { return last_resolution_count_; }
  [[nodiscard]] auto last_resolution() const -> QVariantMap { return last_resolution_; }

 private:
  int         paste_count_           = 0;
  int         begin_merge_count_     = 0;
  int         complete_merge_count_  = 0;
  int         last_resolution_count_ = 0;
  QVariantMap last_resolution_;
};

inline constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    objectName: "editorHistoryVersionsRailHarness"
    width: 900
    height: 680
    visible: true

    property alias rail: railLoader.item

    Loader {
        id: railLoader
        anchors.fill: parent
        asynchronous: false
        source: railSourceUrl

        onLoaded: {
            if (!item) {
                return
            }
            item.editorSession = editorSessionFake
            item.interactionPolicy = interactionPolicyFake
            item.adjustmentTransfer = adjustmentTransferFake
        }
    }
}
)";

inline constexpr char kRecoveryHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    objectName: "editorSaveRecoveryHarness"
    width: 1200
    height: 120
    visible: true

    Loader {
        id: recoveryLoader
        anchors.fill: parent
        asynchronous: false
        source: recoverySourceUrl

        onLoaded: {
            if (item) item.editorSession = editorSessionFake
        }
    }
}
)";

inline auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

inline auto RailUrl() -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() + "/EditorWorkspaceRail.qml");
}

inline auto RecoveryBarUrl() -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() + "/EditorSaveRecoveryBar.qml");
}

inline void ProcessEvents() { QCoreApplication::processEvents(QEventLoop::AllEvents); }

inline void Click(QQuickWindow* window, QQuickItem* item, QPointF local = QPointF(-1.0, -1.0)) {
  ASSERT_NE(window, nullptr);
  ASSERT_NE(item, nullptr);
  if (local.x() < 0.0) local = QPointF(item->width() / 2.0, item->height() / 2.0);
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, item->mapToScene(local).toPoint());
  ProcessEvents();
}

inline void TypeText(QQuickWindow* window, const QString& text) {
  for (const QChar character : text) {
    if (character >= QLatin1Char('a') && character <= QLatin1Char('z')) {
      const auto key =
          static_cast<Qt::Key>(Qt::Key_A + character.unicode() - QLatin1Char('a').unicode());
      QTest::keyClick(window, key);
    } else if (character >= QLatin1Char('A') && character <= QLatin1Char('Z')) {
      const auto key =
          static_cast<Qt::Key>(Qt::Key_A + character.unicode() - QLatin1Char('A').unicode());
      QTest::keyClick(window, key, Qt::ShiftModifier);
    } else if (character == QLatin1Char(' ')) {
      QTest::keyClick(window, Qt::Key_Space);
    } else if (character >= QLatin1Char('0') && character <= QLatin1Char('9')) {
      const auto key =
          static_cast<Qt::Key>(Qt::Key_0 + character.unicode() - QLatin1Char('0').unicode());
      QTest::keyClick(window, key);
    }
  }
  ProcessEvents();
}

inline auto FindVisualItem(QQuickItem* parent, const QString& object_name) -> QQuickItem* {
  if (parent == nullptr) return nullptr;
  for (auto* child : parent->childItems()) {
    if (child->objectName() == object_name) return child;
    if (auto* nested = FindVisualItem(child, object_name)) return nested;
  }
  return nullptr;
}

/// Fixture that loads the production history/versions rail with recording fakes.
class RailQmlFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    RegisterEditorHistoryQmlTypes();

    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("editorSessionFake"), &controller_);
    engine_.rootContext()->setContextProperty(QStringLiteral("interactionPolicyFake"), &policy_);
    engine_.rootContext()->setContextProperty(QStringLiteral("adjustmentTransferFake"), &transfer_);
    engine_.rootContext()->setContextProperty(QStringLiteral("railSourceUrl"), RailUrl());
    engine_.rootContext()->setContextProperty(QStringLiteral("recoverySourceUrl"), RecoveryBarUrl());
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& warnings) {
      for (const auto& warning : warnings) warnings_.push_back(warning.toString());
    });

    engine_.loadData(QByteArray{kHarnessQml},
                     QUrl(QStringLiteral("file:///EditorHistoryVersionsRailHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
      if (window_ != nullptr) {
        window_->show();
        window_->requestActivate();
      }
    }
    ProcessEvents();
    if (window_ != nullptr && Find(QStringLiteral("editorHistoryVersionsRail")) == nullptr &&
        !warnings_.isEmpty()) {
      ADD_FAILURE() << warnings_.join('\n').toStdString();
    }
  }

  auto Find(const QString& object_name) -> QQuickItem* {
    if (window_ == nullptr) return nullptr;
    if (auto* direct = window_->findChild<QQuickItem*>(object_name)) return direct;
    return FindVisualItem(window_->contentItem(), object_name);
  }

  auto Cards() -> QList<QQuickItem*> {
    QList<QQuickItem*>               cards;
    std::function<void(QQuickItem*)> visit = [&](QQuickItem* parent) {
      if (parent == nullptr) return;
      for (auto* child : parent->childItems()) {
        if (child->objectName() == QStringLiteral("editorVersionCard")) cards.push_back(child);
        visit(child);
      }
    };
    if (window_ != nullptr) visit(window_->contentItem());
    return cards;
  }

  auto HistoryCards() -> QList<QQuickItem*> {
    QList<QQuickItem*>               delegates;
    std::function<void(QQuickItem*)> visit = [&](QQuickItem* parent) {
      if (parent == nullptr) return;
      for (auto* child : parent->childItems()) {
        if (child->objectName() == QStringLiteral("editorHistoryTransactionDelegate")) {
          delegates.push_back(child);
        }
        visit(child);
      }
    };
    if (window_ != nullptr) visit(window_->contentItem());
    return delegates;
  }

  void OpenVersionsPage() { Click(window_, Find(QStringLiteral("editorVersionsRailButton"))); }

  void OpenHistoryPage() { Click(window_, Find(QStringLiteral("editorHistoryRailButton"))); }

  RecordingEditorSessionBackend backend_;
  EditorSessionController       controller_{&backend_};
  RecordingInteractionPolicy    policy_;
  RecordingAdjustmentTransfer   transfer_;
  QQmlApplicationEngine         engine_;
  QQuickWindow*                 window_ = nullptr;
  QStringList                   warnings_;
};

}  // namespace rail_harness
}  // namespace alcedo::ui::test
