//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/modules/versioning.hpp"

#include <QColor>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <string>
#include <utility>

#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/history_controller.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/history_cards.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui::versioning {
namespace {

void AddEmptyStateItem(QListWidget* list_widget, const QString& text) {
  if (!list_widget) {
    return;
  }
  auto* item = new QListWidgetItem(list_widget);
  item->setFlags(Qt::NoItemFlags);
  item->setSizeHint(QSize(0, 56));

  auto* label = new QLabel(text, list_widget);
  label->setAlignment(Qt::AlignCenter);
  label->setStyleSheet(AppTheme::EditorLabelStyle(AppTheme::Instance().textMutedColor()));
  AppTheme::MarkFontRole(label, AppTheme::FontRole::UiCaption);
  label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  list_widget->setItemWidget(item, label);
}

auto VersionDisplayName(const Version& version) -> QString {
  const auto name = QString::fromStdString(version.GetDisplayName());
  return name.isEmpty() ? Tr("Untitled") : name;
}

auto BuildVersionCard(const Version& version, bool active, const VersionUiCallbacks& callbacks,
                      QWidget* parent) -> HistoryCardWidget* {
  auto* card = new HistoryCardWidget(parent);
  card->SetSelected(active);

  auto* row = new QHBoxLayout(card);
  row->setContentsMargins(10, 8, 10, 8);
  row->setSpacing(8);

  auto* body = new QVBoxLayout();
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(2);

  auto* title = new ElidedLabel(VersionDisplayName(version), card);
  title->setObjectName(QStringLiteral("HistoryTxTitle"));
  QFont title_font = AppTheme::Font(AppTheme::FontRole::UiBodyStrong);
  title_font.setPointSizeF(9.5);
  title_font.setWeight(QFont::DemiBold);
  title->setFont(title_font);
  title->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  body->addWidget(title);

  const QString when =
      QDateTime::fromSecsSinceEpoch(static_cast<qint64>(version.GetLastModifiedTime()))
          .toString("MM-dd HH:mm");
  auto* subtitle = new ElidedLabel(Tr("Modified %1").arg(when), card);
  subtitle->setObjectName(QStringLiteral("HistoryTxSubtitle"));
  QFont subtitle_font = AppTheme::Font(AppTheme::FontRole::DataCaption);
  subtitle_font.setPointSizeF(8.0);
  subtitle->setFont(subtitle_font);
  subtitle->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  body->addWidget(subtitle);
  row->addLayout(body, 1);

  if (active) {
    row->addWidget(MakePillLabel(Tr("CURRENT"), card), 0, Qt::AlignTop);
  }

  auto* rename_btn = new QToolButton(card);
  rename_btn->setText(QStringLiteral("✎"));
  rename_btn->setToolTip(Tr("Rename version"));
  rename_btn->setCursor(Qt::PointingHandCursor);
  rename_btn->setAutoRaise(true);
  rename_btn->setFixedSize(22, 22);
  rename_btn->setStyleSheet(
      QStringLiteral("QToolButton { color: %1; border: none; background: transparent; }"
                     "QToolButton:hover { color: %2; }")
          .arg(AppTheme::Instance().textMutedColor().name(QColor::HexRgb),
               AppTheme::Instance().textColor().name(QColor::HexRgb)));
  const QString version_id = QString::fromStdString(version.GetVersionID().ToString());
  QObject::connect(rename_btn, &QToolButton::clicked, card, [callbacks, version_id]() {
    if (callbacks.request_rename_version) {
      callbacks.request_rename_version(version_id);
    }
  });
  row->addWidget(rename_btn, 0, Qt::AlignTop);

  return card;
}

}  // namespace

auto MakeHistoryCursorLabel(size_t cursor, size_t total) -> QString {
  if (total == 0) {
    return Tr("No edits yet");
  }
  if (cursor >= total) {
    return Tr("Current");
  }
  return Tr("Returned to step %1 of %2")
      .arg(static_cast<qulonglong>(cursor))
      .arg(static_cast<qulonglong>(total));
}

auto ReconstructPipelineParamsForVersion(Version& version,
                                         const std::shared_ptr<EditHistoryGuard>& history_guard)
    -> std::optional<nlohmann::json> {
  if (!history_guard || !history_guard->history_) {
    return std::nullopt;
  }
  return history_guard->history_->ReconstructPipelineParamsForVersion(version.GetVersionID());
}

auto ResolveSelectedVersion(QListWidgetItem* item,
                            const std::shared_ptr<EditHistoryGuard>& history_guard,
                            ResolvedVersionSelection* out_selection,
                            QString* error) -> bool {
  if (!item || !history_guard || !history_guard->history_) {
    return false;
  }
  return ResolveVersionId(item->data(Qt::UserRole).toString(), history_guard, out_selection, error);
}

auto ResolveVersionId(const QString& version_id_qstr,
                      const std::shared_ptr<EditHistoryGuard>& history_guard,
                      ResolvedVersionSelection* out_selection,
                      QString* error) -> bool {
  if (!history_guard || !history_guard->history_) {
    return false;
  }

  const auto version_id_str = version_id_qstr.toStdString();
  if (version_id_str.empty()) {
    return false;
  }

  Hash128 version_id{};
  try {
    version_id = Hash128::FromString(version_id_str);
  } catch (const std::exception& e) {
    if (error) {
      *error = Tr("Invalid version ID: %1").arg(e.what());
    }
    return false;
  }

  Version* selected_version = nullptr;
  try {
    selected_version = &history_guard->history_->GetVersion(version_id);
  } catch (const std::exception& e) {
    if (error) {
      *error = Tr("Failed to load selected version: %1").arg(e.what());
    }
    return false;
  }

  if (out_selection) {
    out_selection->version_id = version_id;
    out_selection->version    = selected_version;
  }
  return true;
}

auto UndoLastTransaction(WorkingVersion& working_version,
                         const std::shared_ptr<PipelineGuard>& pipeline_guard) -> CursorMoveResult {
  CursorMoveResult result{};
  if (!pipeline_guard || !pipeline_guard->pipeline_) {
    return result;
  }
  if (working_version.GetCursor() == 0) {
    return result;
  }
  try {
    if (!working_version.UndoLastTransaction(*pipeline_guard->pipeline_)) {
      return result;
    }
  } catch (const std::exception& e) {
    result.error = Tr("Undo failed: %1").arg(e.what());
    return result;
  }
  pipeline_guard->dirty_ = true;
  result.moved           = true;
  return result;
}

auto MoveCursorTo(WorkingVersion& working_version, size_t target_cursor,
                  const std::shared_ptr<PipelineGuard>& pipeline_guard) -> CursorMoveResult {
  CursorMoveResult result{};
  if (!pipeline_guard || !pipeline_guard->pipeline_) {
    return result;
  }
  if (target_cursor == working_version.GetCursor()) {
    return result;
  }
  try {
    if (!working_version.MoveCursorTo(target_cursor, *pipeline_guard->pipeline_)) {
      return result;
    }
  } catch (const std::exception& e) {
    result.error = Tr("History jump failed: %1").arg(e.what());
    return result;
  }
  pipeline_guard->dirty_ = true;
  result.moved           = true;
  return result;
}

auto SeedWorkingVersionFromActive(sl_element_id_t element_id,
                                  const std::shared_ptr<EditHistoryGuard>& history_guard)
    -> WorkingVersion {
  return controllers::SeedWorkingVersionFromActive(element_id, history_guard);
}

auto SeedWorkingVersionFromVersion(sl_element_id_t element_id, const Hash128& version_id,
                                   const std::shared_ptr<EditHistoryGuard>& history_guard)
    -> WorkingVersion {
  return controllers::SeedWorkingVersionFromVersion(element_id, version_id, history_guard);
}

void PersistWorkingVersion(const std::shared_ptr<EditHistoryMgmtService>& history_service,
                           const std::shared_ptr<EditHistoryGuard>& history_guard,
                           const WorkingVersion& working_version,
                           const std::shared_ptr<PipelineGuard>& pipeline_guard) {
  if (!history_service || !history_guard || !history_guard->history_ ||
      !working_version.HasVersion()) {
    return;
  }

  if (const auto head_params = working_version.GetHeadPipelineParams(); head_params.has_value()) {
    history_service->UpdateVersion(history_guard, working_version.GetVersionID(), working_version,
                                   *head_params);
    return;
  }

  if (!pipeline_guard || !pipeline_guard->pipeline_) {
    return;
  }
  history_service->UpdateVersion(history_guard, working_version.GetVersionID(), working_version,
                                 pipeline_guard->pipeline_->ExportPipelineParams());
}

void UpdateVersionUi(const VersionUiContext& ui, const VersionUiCallbacks& callbacks,
                     const WorkingVersion& working_version,
                     const std::shared_ptr<EditHistoryGuard>& history_guard,
                     const std::function<void()>& refresh_selection_styles) {
  const size_t total_transactions = working_version.GetAllEditTransactions().size();
  const size_t cursor             = working_version.GetCursor();

  if (ui.history_status) {
    const QString label = MakeHistoryCursorLabel(cursor, total_transactions);
    ui.history_status->setText(label);
    ui.history_status->setToolTip(label);
  }
  if (ui.undo_tx_btn) {
    ui.undo_tx_btn->setEnabled(cursor > 0);
  }

  if (ui.tx_stack) {
    ui.tx_stack->clear();
    const auto& txs = working_version.GetAllEditTransactions();
    if (txs.empty()) {
      AddEmptyStateItem(ui.tx_stack, Tr("No edits yet"));
    }
    for (size_t reverse_index = 0; reverse_index < txs.size(); ++reverse_index) {
      const size_t tx_index      = txs.size() - 1 - reverse_index;
      const size_t target_cursor = tx_index + 1;
      const bool   future        = target_cursor > cursor;
      const bool   current       = cursor > 0 && target_cursor == cursor;

      auto* item = new QListWidgetItem(ui.tx_stack);
      item->setData(Qt::UserRole, static_cast<qulonglong>(target_cursor));
      item->setSizeHint(QSize(0, 56));
      auto* card =
          BuildTxHistoryCard(txs[tx_index], /*draw_top*/ reverse_index > 0,
                             /*draw_bottom*/ (reverse_index + 1) < txs.size(), current, future,
                             ui.tx_stack);
      ui.tx_stack->setItemWidget(item, card);
      if (current) {
        ui.tx_stack->setCurrentItem(item);
      }
    }
  }

  if (ui.version_log) {
    ui.version_log->clear();
    if (history_guard && history_guard->history_) {
      const auto& versions = history_guard->history_->GetVersions();
      const auto active_id = history_guard->history_->GetActiveVersionID();

      for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        const auto& version = it->ver_ref_;
        const bool  active  = version.GetVersionID() == active_id;

        auto* item = new QListWidgetItem(ui.version_log);
        item->setData(Qt::UserRole, QString::fromStdString(version.GetVersionID().ToString()));
        item->setSizeHint(QSize(0, 56));
        auto* card = BuildVersionCard(version, active, callbacks, ui.version_log);
        ui.version_log->setItemWidget(item, card);
        if (active) {
          ui.version_log->setCurrentItem(item);
          item->setSelected(true);
        }
      }
    }
    if (ui.version_log->count() == 0) {
      AddEmptyStateItem(ui.version_log, Tr("No versions"));
    }
    if (refresh_selection_styles) {
      refresh_selection_styles();
    }
  }
}

}  // namespace alcedo::ui::versioning
