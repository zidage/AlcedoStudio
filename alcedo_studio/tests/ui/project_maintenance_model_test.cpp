// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/alcedo_main/album_backend/project_maintenance_model.hpp"

#include <stdexcept>

#include "../support/project_maintenance_fixture.hpp"

namespace alcedo::ui {
namespace {
using ProjectMaintenanceModelTest = alcedo::test::MaintenanceStorageFixture;
TEST_F(ProjectMaintenanceModelTest, ReviewCannotSelectRatedOrEditedRows) {
  Add(1);
  Add(2);
  Add(3);
  Execute(
      "UPDATE Image SET rating=2,metadata='{\"Rating\":2}' WHERE id=2;"
      "UPDATE VersionRef SET display_name='Edited copy' WHERE element_id=3");
  ASSERT_TRUE(service_.Scan(*store_));
  ProjectMaintenanceModel model(service_);
  model.Refresh();
  EXPECT_FALSE(model.SetSelected(2, true));
  EXPECT_FALSE(model.SetSelected(3, true));
  EXPECT_TRUE(model.SetSelected(1, true));
  EXPECT_EQ(model.SelectedCount(), 1);
  model.SetReadOnly(true);
  EXPECT_FALSE(model.SetSelected(1, false));
}
TEST_F(ProjectMaintenanceModelTest, HiddenSelectedRowsRemainInConfirmationCount) {
  for (sl_element_id_t id = 1; id <= 105; ++id) Add(id);
  ASSERT_TRUE(service_.Scan(*store_));
  ProjectMaintenanceModel model(service_);
  model.Refresh();
  EXPECT_EQ(model.rowCount(), 100);
  model.SelectKind(0);
  EXPECT_EQ(model.SelectedCount(), 104);
  model.SetPage(1);
  EXPECT_EQ(model.rowCount(), 5);
  model.SetSearch(QStringLiteral("absent path"));
  EXPECT_EQ(model.rowCount(), 0);
  EXPECT_EQ(model.SelectedCount(), 104);
  EXPECT_EQ(model.SelectedDuplicates(), 104);
  model.SetFilter(1);
  EXPECT_EQ(model.SelectedCount(), 104);
  model.ClearSelection();
  EXPECT_EQ(model.SelectedCount(), 0);
}
TEST_F(ProjectMaintenanceModelTest, IncompleteScanHasNoSelectableRows) {
  Add(1);
  Add(2);
  EXPECT_FALSE(service_.Scan(*store_, {}, {}, [] { return true; }));
  ProjectMaintenanceModel model(service_);
  model.Refresh();
  EXPECT_EQ(model.rowCount(), 0);
  EXPECT_FALSE(model.SetSelected(2, true));
}
TEST_F(ProjectMaintenanceModelTest, ReviewPreservesUnsigned32BitEntryIds) {
  Add(1);
  Add(4294967295U);
  ASSERT_TRUE(service_.Scan(*store_));
  ProjectMaintenanceModel model(service_);
  model.Refresh();
  ASSERT_EQ(model.rowCount(), 2);
  EXPECT_EQ(model.data(model.index(1), ProjectMaintenanceModel::EntryId).toULongLong(),
            4294967295ULL);
  ASSERT_TRUE(model.SetSelected(4294967295U, true));
  ASSERT_EQ(model.SelectedIds().size(), 1);
  EXPECT_EQ(model.SelectedIds().front(), 4294967295U);
}
TEST_F(ProjectMaintenanceModelTest, CommandPagesDoNotChangeVisibleReviewOrSelection) {
  for (sl_element_id_t id = 1; id <= 105; ++id) Add(id);
  ASSERT_TRUE(service_.Scan(*store_));
  ProjectMaintenanceModel model(service_);
  model.Refresh();
  model.SelectKind(0);
  model.SetSearch(QStringLiteral("hidden"));
  int        count = -1;
  const auto rows  = model.ReadFindingsPage(1, -1, {}, count);
  EXPECT_EQ(rows.size(), 5);
  EXPECT_EQ(count, 105);
  EXPECT_EQ(model.rowCount(), 0);
  EXPECT_EQ(model.Search(), QStringLiteral("hidden"));
  EXPECT_EQ(model.SelectedCount(), 104);
  EXPECT_THROW(model.ReadFindingsPage(2, -1, {}, count), std::out_of_range);
}
}  // namespace
}  // namespace alcedo::ui
