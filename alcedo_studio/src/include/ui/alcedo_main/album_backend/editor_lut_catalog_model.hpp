//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QSettings>
#include <QStringList>
#include <QVariantList>
#include <string>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/editor_support/modules/lut_catalog.hpp"

namespace alcedo::ui {

/// Phase 6D LUT catalog model. Lists cube LUTs from the resolved LUT directory,
/// tracks selection, and submits operator-shaped params matching ParamsForField
/// (Lut): {"ocio_lmt":"<path-or-empty>"}. Relative selection supports Look-panel
/// keyboard shortcuts (prev/next). Load via setSelectedPath does not submit.
class EditorLutCatalogModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
  Q_PROPERTY(
      QString selectedPath READ selectedPath WRITE setSelectedPath NOTIFY selectedPathChanged)
  Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedPathChanged)
  Q_PROPERTY(QString directoryText READ directoryText NOTIFY catalogChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY catalogChanged)
  Q_PROPERTY(bool canOpenDirectory READ canOpenDirectory NOTIFY catalogChanged)
  Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
  Q_PROPERTY(QStringList favoritePaths READ favoritePaths WRITE setFavoritePaths NOTIFY
                 favoritePathsChanged)

 public:
  explicit EditorLutCatalogModel(QObject* parent = nullptr);
  ~EditorLutCatalogModel() override;

  [[nodiscard]] auto                entries() const -> QVariantList { return entries_; }
  [[nodiscard]] auto                selectedPath() const -> QString { return selectedPath_; }
  /// Load-only selection: updates selectedPath/selectedIndex without submitting.
  void                              setSelectedPath(const QString& path);
  [[nodiscard]] auto                selectedIndex() const -> int { return selectedIndex_; }
  [[nodiscard]] auto                directoryText() const -> QString { return directoryText_; }
  [[nodiscard]] auto                statusText() const -> QString { return statusText_; }
  [[nodiscard]] auto                canOpenDirectory() const -> bool { return canOpenDirectory_; }
  [[nodiscard]] auto                filterText() const -> QString { return filterText_; }
  void                              setFilterText(const QString& text);
  [[nodiscard]] auto                favoritePaths() const -> QStringList { return favoritePaths_; }
  void                              setFavoritePaths(const QStringList& paths);

  /// Rescan the LUT directory. force=true invalidates the catalog cache.
  Q_INVOKABLE void                  refresh(bool force = false);
  /// User selection: commit one settled LUT transaction when the path changes.
  /// Does not emit entriesChanged (selection is via selectedPathChanged only).
  Q_INVOKABLE void                  selectPath(const QString& path);
  /// Move selection by step among selectable entries; commits when path changes.
  Q_INVOKABLE bool                  selectRelative(int step);
  Q_INVOKABLE void                  clearSelection();
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;
  [[nodiscard]] Q_INVOKABLE QString defaultLutPath() const;
  /// Absolute filesystem directory for "Open folder". Empty when unavailable.
  [[nodiscard]] Q_INVOKABLE QString directoryPath() const;
  /// Toggle a path's favorite status and persist via QSettings.
  Q_INVOKABLE void                  toggleFavoritePath(const QString& path);
  /// True when path is present in the persisted favoritePaths list.
  Q_INVOKABLE bool                  isFavoritePath(const QString& path) const;

 signals:
  void entriesChanged();
  void selectedPathChanged();
  void catalogChanged();
  void filterTextChanged();
  void settledCommitted();
  void openFolderRequested(const QString& path);
  void favoritePathsChanged();

 private:
  void                    rebuildEntriesView();
  void                    applySelectionHighlight();
  void                    submitSettled();
  [[nodiscard]] auto      buildParamsJson() const -> QString;
  void                    loadFavoriteSettings();
  void                    saveFavoriteSettings() const;

  lut_catalog::LutCatalog catalog_{};
  QVariantList            entries_;
  QString                 selectedPath_;
  int                     selectedIndex_ = -1;
  QString                 directoryText_;
  QString                 statusText_;
  bool                    canOpenDirectory_ = false;
  QString                 filterText_;
  std::string             selectedPathUtf8_;
  QStringList             favoritePaths_{};
};

}  // namespace alcedo::ui
