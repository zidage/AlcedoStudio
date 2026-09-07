//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QVariantMap>
#include <cstdint>

#include "app/editor_panel_projection.hpp"

namespace alcedo::ui {

/**
 * @brief Map typed panel values onto the existing QML loadFromSnapshot keys.
 *
 * Nested maps match the current panel loaders (for example snapshot.exposure.exposure).
 * This does not parse Model JSON. Values are already copied from Model getters.
 */
[[nodiscard]] auto PanelProjectionToVariantMap(const alcedo::EditorPanelProjection& projection)
    -> QVariantMap;

/**
 * @brief Apply @p projection to @p dest when the session generation matches.
 *
 * Same-session deliveries merge only the fields present in @p projection so an
 * ordinary edit does not rebuild every panel's values. A new session generation
 * replaces the map. Stale generations leave @p dest unchanged.
 *
 * @return true when @p dest was written.
 */
auto ApplyPanelProjectionToSnapshotMap(const alcedo::EditorPanelProjection& projection,
                                       std::uint64_t session_generation, QVariantMap* dest) -> bool;

}  // namespace alcedo::ui
