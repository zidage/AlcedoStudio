//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QString>
#include <memory>
#include <optional>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/project_service.hpp"
#include "edit/history/edit_commit_attribution.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

/**
 * @file
 * @brief The Paste of a package onto photos by the agent: the shared part of `adjustments.paste`
 * and `presets.apply` (edit presets plan, PS4B).
 */
namespace alcedo::automation {

/**
 * @brief The photos of a paste command: `element_ids` of @p params, or without it the photos
 *        selected in the library.
 *
 * The library selection exists only in the GUI host (@p selection_available). Sends the error
 * (-32602 for an unknown id or an empty selection, -32001 without a selection) and returns
 * nothing when the photos cannot be resolved.
 */
[[nodiscard]] auto AgentPasteTargets(ui::ApplicationModuleHost* host, ProjectService& project,
                                     const QJsonObject& params, bool selection_available,
                                     AutomationReply& reply)
    -> std::optional<std::vector<sl_element_id_t>>;

/**
 * @brief The name of the new Versions: `version_name` of @p params normalized with the Version
 *        name rules, or @p default_name without it. Sends -32602 and returns nothing for a name
 *        that the rules refuse.
 */
[[nodiscard]] auto AgentPasteVersionName(const QJsonObject& params, const QString& default_name,
                                         AutomationReply& reply) -> std::optional<QString>;

/// The `version_name` and `note` parameter schemas of a paste command.
[[nodiscard]] auto AgentPasteVersionNameSchema() -> QJsonObject;
[[nodiscard]] auto AgentPasteNoteSchema() -> QJsonObject;
/// The result schema of a paste command: `{task_id, target_count, editor?}`.
[[nodiscard]] auto AgentPasteResultSchema() -> QJsonObject;

/**
 * @brief Paste @p package onto @p ids as one new Version named @p version_name each, by the
 *        agent, and answer @p reply.
 *
 * The routing of Paste Adjustments: the photo open in the editor pastes first through the editor
 * session (EditorSessionController::RequestPasteAdjustmentPackage), and the answer reports it in
 * `editor`; every other photo pastes in the AdjustmentPaste background task of the apply
 * coordinator, whose id the answer carries in `task_id`. Each Paste commit has @p attribution.
 * Answers Busy while another Paste runs or the policy refuses Paste. The package stays
 * alive until the last stage that reads it ends.
 *
 * @pre @p ids are photos of the entered project (AgentPasteTargets).
 */
void StartAgentPaste(AutomationCommandRegistry* registry, ui::ApplicationModuleHost* host,
                     AutomationReply                                  reply,
                     std::shared_ptr<const AdjustmentTransferPackage> package,
                     std::vector<sl_element_id_t> ids, const QString& version_name,
                     const EditCommitAttribution& attribution);

}  // namespace alcedo::automation
