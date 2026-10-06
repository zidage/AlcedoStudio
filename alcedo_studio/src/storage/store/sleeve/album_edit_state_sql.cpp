//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/album_edit_state_sql.hpp"

#include <format>
#include <string>
#include <string_view>

#include "storage/mapper/duckorm/duckdb_expr.hpp"

namespace alcedo::album_edit_state_sql {
namespace expr = duckorm::expr;

auto EditedFileIdsQuery() -> std::string {
  return std::format("SELECT ies.element_id FROM {}", kActiveHeadCommitJoin);
}

auto EditStateRelation(const std::string& time_zone, std::string_view match_set_table)
    -> duckorm::SqlFragment {
  auto relation = expr::raw(
      "edit_state AS (SELECT ies.element_id AS file_id, hc.created_at_ns AS "
      "edited_at_ns, ");
  if (time_zone.empty()) {
    relation.sql_.append("CAST(NULL AS DATE)");
  } else {
    // to_timestamp reads epoch seconds as an instant (TIMESTAMPTZ); timezone(zone, TIMESTAMPTZ)
    // gives the local wall time in that zone without the connection's TimeZone setting.
    relation.sql_.append("CAST(timezone(CAST(");
    relation.append(expr::param(time_zone));
    relation.sql_.append(
        " AS VARCHAR), to_timestamp(CAST(hc.created_at_ns AS DOUBLE) / 1e9)) AS DATE)");
  }
  relation.sql_.append(
      std::format(" AS edit_day FROM {} WHERE ies.element_id IN (SELECT file_id FROM {}))",
                  kActiveHeadCommitJoin, match_set_table));
  return relation;
}

}  // namespace alcedo::album_edit_state_sql
