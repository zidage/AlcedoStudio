//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"

#include <gtest/gtest.h>

#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace {

using alcedo::CreateDefaultPipelineDocument;
using alcedo::EditorNodeGraphProjection;
using alcedo::NodeId;
using alcedo::ui::EditorNodeLayoutMetrics;
using alcedo::ui::EditorNodeLayoutStore;
using alcedo::ui::NodeIdToQString;

auto MakeMetrics() -> EditorNodeLayoutMetrics {
  EditorNodeLayoutMetrics metrics;
  metrics.origin_x             = 10;
  metrics.origin_y             = 20;
  metrics.vertical_gap         = 30;
  metrics.node_width           = 220;
  metrics.endpoint_height      = 40;
  metrics.name_row_height      = 32;
  metrics.drawer_header_height = 28;
  metrics.mask_row_height      = 28;
  metrics.panel_width_min      = 260;
  metrics.panel_width_max      = 460;
  metrics.panel_width_default  = 320;
  return metrics;
}

TEST(EditorNodeLayoutStore, NewKeyStartsWithDefaultViewZoomAndOpenDrawers) {
  EditorNodeLayoutStore store(MakeMetrics());
  store.activate("p", 1, 2, "version-a");
  EXPECT_EQ(store.preferred_panel_width(), 320);
  EXPECT_DOUBLE_EQ(store.zoom(), 1.0);
  EXPECT_EQ(store.view_position(), QPointF());
  EXPECT_TRUE(store.DrawerOpen(NodeId{"grade.primary"}));
  EXPECT_FALSE(store.hasNodePosition(QStringLiteral("grade.primary")));
}

TEST(EditorNodeLayoutStore, TwoVersionsKeepSeparatePositionsAndDrawerState) {
  EditorNodeLayoutStore store(MakeMetrics());
  store.activate("p", 1, 2, "version-a");
  store.SetNodePosition(NodeId{"grade.primary"}, QPointF(11, 22));
  store.SetDrawerOpen(NodeId{"grade.primary"}, false);
  store.set_zoom(1.5);
  store.set_view_position(QPointF(-4, 8));
  store.set_selected_node_id(NodeId{"develop"});

  store.activate("p", 1, 2, "version-b");
  EXPECT_FALSE(store.hasNodePosition(QStringLiteral("grade.primary")));
  EXPECT_TRUE(store.DrawerOpen(NodeId{"grade.primary"}));
  EXPECT_DOUBLE_EQ(store.zoom(), 1.0);
  EXPECT_TRUE(store.selected_node_id().Empty());

  store.SetNodePosition(NodeId{"grade.primary"}, QPointF(90, 91));

  store.activate("p", 1, 2, "version-a");
  EXPECT_EQ(store.NodePosition(NodeId{"grade.primary"}), QPointF(11, 22));
  EXPECT_FALSE(store.DrawerOpen(NodeId{"grade.primary"}));
  EXPECT_DOUBLE_EQ(store.zoom(), 1.5);
  EXPECT_EQ(store.view_position(), QPointF(-4, 8));
  EXPECT_EQ(store.selected_node_id(), NodeId{"develop"});
}

TEST(EditorNodeLayoutStore, RemovedNodeIdKeepsPriorPositionAndDrawerState) {
  EditorNodeLayoutStore store(MakeMetrics());
  store.activate("p", 3, 4, "v");
  store.SetNodePosition(NodeId{"grade.extra"}, QPointF(40, 50));
  store.SetDrawerOpen(NodeId{"grade.extra"}, false);

  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 1, 1, 1);
  store.EnsureDefaultPositions(snapshot);

  EXPECT_EQ(store.NodePosition(NodeId{"grade.extra"}), QPointF(40, 50));
  EXPECT_FALSE(store.DrawerOpen(NodeId{"grade.extra"}));
}

TEST(EditorNodeLayoutStore, EnsureDefaultPositionsDoNotOverwriteStoredValues) {
  EditorNodeLayoutStore store(MakeMetrics());
  store.activate("p", 1, 2, "v");
  store.SetNodePosition(NodeId{"develop"}, QPointF(5, 6));
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 1, 1, 1);
  store.EnsureDefaultPositions(snapshot);
  EXPECT_EQ(store.NodePosition(NodeId{"develop"}), QPointF(5, 6));
  EXPECT_TRUE(store.hasNodePosition(QStringLiteral("grade.primary")));
  EXPECT_TRUE(store.hasNodePosition(QStringLiteral("drt")));
}

TEST(EditorNodeLayoutStore, PreferredPanelWidthStaysInsideSidePanelRange) {
  EditorNodeLayoutStore store(MakeMetrics());
  store.activate("p", 1, 2, "v");
  store.set_preferred_panel_width(100);
  EXPECT_EQ(store.preferred_panel_width(), 260);
  store.set_preferred_panel_width(900);
  EXPECT_EQ(store.preferred_panel_width(), 460);
}

TEST(EditorNodeLayoutStore, AssignStagingPositionPlacesNewNodeBelowTheBackbone) {
  const auto            metrics = MakeMetrics();
  EditorNodeLayoutStore store(metrics);
  store.activate("p", 1, 2, "v");
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 1, 1, 1);
  store.EnsureDefaultPositions(snapshot);

  const auto develop = store.NodePosition(NodeId{"develop"});
  const auto primary = store.NodePosition(NodeId{"grade.primary"});
  const auto drt     = store.NodePosition(NodeId{"drt"});
  ASSERT_TRUE(develop.has_value());
  ASSERT_TRUE(primary.has_value());
  ASSERT_TRUE(drt.has_value());

  const NodeId draft{"grade.draft"};
  store.AssignStagingPosition(draft, snapshot);
  const auto staged = store.NodePosition(draft);
  ASSERT_TRUE(staged.has_value());
  EXPECT_DOUBLE_EQ(staged->x(), static_cast<qreal>(metrics.origin_x));
  EXPECT_GT(staged->y(), drt->y());
  EXPECT_EQ(store.NodePosition(NodeId{"develop"}), develop);
  EXPECT_EQ(store.NodePosition(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(store.NodePosition(NodeId{"drt"}), drt);
}

TEST(EditorNodeLayoutStore, AssignStagingPositionStacksLaterDraftsDownwardOnTheSameX) {
  const auto            metrics = MakeMetrics();
  EditorNodeLayoutStore store(metrics);
  store.activate("p", 1, 2, "v");
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 1, 1, 1);

  const NodeId first{"grade.draft-a"};
  const NodeId second{"grade.draft-b"};
  store.AssignStagingPosition(first, snapshot);
  store.AssignStagingPosition(second, snapshot);

  const auto a = store.NodePosition(first);
  const auto b = store.NodePosition(second);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_DOUBLE_EQ(a->x(), static_cast<qreal>(metrics.origin_x));
  EXPECT_DOUBLE_EQ(b->x(), a->x());
  EXPECT_GT(b->y(), a->y());
}

}  // namespace
