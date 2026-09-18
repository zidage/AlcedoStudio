//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QString>

#include <gtest/gtest.h>

#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui {
namespace {

TEST(ShortcutRegistry, NodesCommandsMatchTheDocumentedGraphKeys) {
  ShortcutRegistry registry;
  RegisterNodesPanelShortcuts(&registry);

  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Plus, Qt::ControlModifier),
            QLatin1String(shortcut_id::kNodesAddColorGrade));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Equal, Qt::ControlModifier),
            QLatin1String(shortcut_id::kNodesAddColorGrade));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier),
            QLatin1String(shortcut_id::kNodesAddColorGrade));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_0, Qt::ControlModifier),
            QLatin1String(shortcut_id::kNodesFitGraph));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_F2, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesRenameColorGrade));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Delete, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesDeleteColorGrade));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_C, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesBeginConnect));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_C, Qt::ControlModifier), QString());
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Return, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesCompleteConnect));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Enter, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesCompleteConnect));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Up, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesSelectPrevious));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Down, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesSelectNext));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Home, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesSelectDevelop));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_End, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesSelectDrt));
  EXPECT_EQ(registry.commandIdForKey(Qt::Key_Escape, Qt::NoModifier),
            QLatin1String(shortcut_id::kNodesCancel));
}

TEST(ShortcutRegistry, DecorateTooltipAppendsTheNativeSequence) {
  ShortcutRegistry registry;
  RegisterNodesPanelShortcuts(&registry);

  const auto decorated =
      registry.decorateTooltip(QStringLiteral("Fit"), QLatin1String(shortcut_id::kNodesFitGraph));
  EXPECT_TRUE(decorated.startsWith(QStringLiteral("Fit (")));
  EXPECT_TRUE(decorated.endsWith(QLatin1Char(')')));
  EXPECT_NE(registry.shortcutText(QLatin1String(shortcut_id::kNodesFitGraph)), QString());
}

}  // namespace
}  // namespace alcedo::ui
