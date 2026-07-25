//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include <QVariantList>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/editor_dialog/modules/lut_catalog.hpp"

namespace alcedo::ui {

/// Phase 6D LUT catalog model. Lists cube LUTs from the resolved LUT directory,
/// tracks selection, and submits operator-shaped params matching ParamsForField
/// (Lut): {"ocio_lmt":"<path-or-empty>"}. Relative selection supports Look-panel
/// keyboard shortcuts (prev/next). Load via setSelectedPath does not submit.
class EditorLutCatalogModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
  Q_PROPERTY(QString selectedPath READ selectedPath WRITE setSelectedPath NOTIFY selectedPathChanged)
  Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedPathChanged)
  Q_PROPERTY(QString directoryText READ directoryText NOTIFY catalogChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY catalogChanged)
  Q_PROPERTY(bool canOpenDirectory READ canOpenDirectory NOTIFY catalogChanged)
  Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)

 public:
  explicit EditorLutCatalogModel(QObject* parent = nullptr);

  [[nodiscard]] auto entries() const -> QVariantList { return entries_; }
  [[nodiscard]] auto selectedPath() const -> QString { return selectedPath_; }
  /// Plain load setter. Does not submit. Refreshes selection highlight.
  void setSelectedPath(const QString& path);
  [[nodiscard]] auto selectedIndex() const -> int { return selectedIndex_; }
  [[nodiscard]] auto directoryText() const -> QString { return directoryText_; }
  [[nodiscard]] auto statusText() const -> QString { return statusText_; }
  [[nodiscard]] auto canOpenDirectory() const -> bool { return canOpenDirectory_; }
  [[nodiscard]] auto filterText() const -> QString { return filterText_; }
  void setFilterText(const QString& text);

  /// Rescan the LUT directory. force=true invalidates the catalog cache.
  Q_INVOKABLE void refresh(bool force = false);
  /// User selection: commit one settled LUT transaction when the path changes.
  Q_INVOKABLE void selectPath(const QString& path);
  /// Move selection by step among selectable entries; commits when path changes.
  Q_INVOKABLE bool selectRelative(int step);
  Q_INVOKABLE void clearSelection();
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;
  [[nodiscard]] Q_INVOKABLE QString defaultLutPath() const;
  /// Absolute filesystem directory for "Open folder". Empty when unavailable.
  [[nodiscard]] Q_INVOKABLE QString directoryPath() const;

 signals:
  void entriesChanged();
  void selectedPathChanged();
  void catalogChanged();
  void filterTextChanged();
  void settledCommitted();
  void openFolderRequested(const QString& path);

 private:
  void rebuildEntriesView();
  void applySelectionHighlight();
  void submitSettled();
  [[nodiscard]] auto buildParamsJson() const -> QString;

  lut_catalog::LutCatalog catalog_{};
  QVariantList            entries_;
  QString                 selectedPath_;
  int                     selectedIndex_     = -1;
  QString                 directoryText_;
  QString                 statusText_;
  bool                    canOpenDirectory_  = false;
  QString                 filterText_;
  std::string             selectedPathUtf8_;
};

}  // namespace alcedo::ui
