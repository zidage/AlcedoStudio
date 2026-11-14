//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

/// The one list of log categories. Each entry is
/// `X(core_id, qt_category_function, "category.name")`:
///
/// - `core_id` is the `alcedo::log::Category` enumerator that the Qt-free macros in
///   `utils/logging/log.hpp` take (`ALCEDO_LOG_WARN(Render)`).
/// - `qt_category_function` is the `QLoggingCategory` function that
///   `utils/diagnostics/app_logging.hpp` declares for the Qt code (`qCWarning(renderLog)`).
/// - `"category.name"` is the name in the log file and in `QT_LOGGING_RULES`.
///
/// AppDiagnostics forwards a core message to the Qt category of the same entry, so both paths
/// write one name and obey one rule set.
#define ALCEDO_LOG_CATEGORY_LIST(X)                           \
  X(App, appLog, "alcedo.app")                                \
  X(Project, projectLog, "alcedo.project")                    \
  X(Library, libraryLog, "alcedo.library")                    \
  X(Import, importLog, "alcedo.import")                       \
  X(Storage, storageLog, "alcedo.storage")                    \
  X(StorageDb, storageDbLog, "alcedo.storage.db")             \
  X(Thumbnail, thumbnailLog, "alcedo.thumbnail")              \
  X(Decode, decodeLog, "alcedo.decode")                       \
  X(Render, renderLog, "alcedo.render")                       \
  X(RenderRoi, renderRoiLog, "alcedo.render.roi")             \
  X(Gpu, gpuLog, "alcedo.gpu")                                \
  X(GpuPool, gpuPoolLog, "alcedo.gpu.pool")                   \
  X(Lens, lensLog, "alcedo.lens")                             \
  X(Editor, editorLog, "alcedo.editor")                       \
  X(EditorPresent, editorPresentLog, "alcedo.editor.present") \
  X(Mask, maskLog, "alcedo.mask")                             \
  X(Semantic, semanticLog, "alcedo.semantic")                 \
  X(SemanticRpc, semanticRpcLog, "alcedo.semantic.rpc")       \
  X(SemanticDb, semanticDbLog, "alcedo.semantic.db")          \
  X(Automation, automationLog, "alcedo.automation")           \
  X(Export, exportLog, "alcedo.export")                       \
  X(Update, updateLog, "alcedo.update")

namespace alcedo::log {

#define ALCEDO_LOG_CATEGORY_ENUMERATOR_(core_id, qt_category_function, name) core_id,
enum class Category : std::uint8_t { ALCEDO_LOG_CATEGORY_LIST(ALCEDO_LOG_CATEGORY_ENUMERATOR_) };
#undef ALCEDO_LOG_CATEGORY_ENUMERATOR_

#define ALCEDO_LOG_CATEGORY_COUNT_(core_id, qt_category_function, name) +1
inline constexpr std::size_t kCategoryCount =
    0 ALCEDO_LOG_CATEGORY_LIST(ALCEDO_LOG_CATEGORY_COUNT_);
#undef ALCEDO_LOG_CATEGORY_COUNT_

/// The name of @p category in the log file and in `QT_LOGGING_RULES`, for example `alcedo.gpu`.
[[nodiscard]] constexpr auto CategoryName(Category category) noexcept -> std::string_view {
  switch (category) {
#define ALCEDO_LOG_CATEGORY_NAME_CASE_(core_id, qt_category_function, name) \
  case Category::core_id:                                                   \
    return name;
    ALCEDO_LOG_CATEGORY_LIST(ALCEDO_LOG_CATEGORY_NAME_CASE_)
#undef ALCEDO_LOG_CATEGORY_NAME_CASE_
  }
  return "alcedo";
}

}  // namespace alcedo::log
