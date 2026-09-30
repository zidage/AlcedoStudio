//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointer>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <optional>
#include <string>

#include "app/lut_library_service.hpp"
#include "edit/operators/models/lut_reference.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/editor_support/modules/lut_catalog.hpp"

namespace alcedo::ui {

/// Phase 6D LUT catalog model. Lists the entries of the application's
/// LutLibraryService, tracks selection, and submits a typed LUT selection: the
/// official package/LUT ID of package-owned official files, else the library path
/// (plan L4). A selection keeps the configured strength. Relative selection supports
/// Look-panel keyboard shortcuts (prev/next). Loading (setSelectedPath, loadSelection)
/// does not submit. Favorites are stored by the library as root-relative entry paths;
/// this model exposes them as absolute paths.
class EditorLutCatalogModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(alcedo::LutLibraryService* library READ library WRITE setLibrary NOTIFY libraryChanged)
  Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
  Q_PROPERTY(
      QString selectedPath READ selectedPath WRITE setSelectedPath NOTIFY selectedPathChanged)
  Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedPathChanged)
  Q_PROPERTY(QString directoryText READ directoryText NOTIFY catalogChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY catalogChanged)
  Q_PROPERTY(bool canOpenDirectory READ canOpenDirectory NOTIFY catalogChanged)
  Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
  Q_PROPERTY(QStringList favoritePaths READ favoritePaths NOTIFY favoritePathsChanged)

 public:
  explicit EditorLutCatalogModel(QObject* parent = nullptr);
  ~EditorLutCatalogModel() override;

  [[nodiscard]] auto                library() const -> alcedo::LutLibraryService* { return library_; }
  /// Bind the application library. Rebuilds the list and follows its changes.
  void                              setLibrary(alcedo::LutLibraryService* library);
  [[nodiscard]] auto                entries() const -> QVariantList { return entries_; }
  [[nodiscard]] auto                selectedPath() const -> QString { return selectedPath_; }
  /// Load-only selection: updates selectedPath/selectedIndex without submitting.
  void                              setSelectedPath(const QString& path);
  /// Load-only selection from the panel's `lut` field (referenceKind, packageId, lutId,
  /// libraryPath, path, lutName). The reference is resolved through the library to its
  /// current file; an unresolved reference is shown as a missing row with its last known name.
  Q_INVOKABLE void                  loadSelection(const QVariantMap& lutField);
  [[nodiscard]] auto                selectedIndex() const -> int { return selectedIndex_; }
  [[nodiscard]] auto                directoryText() const -> QString { return directoryText_; }
  [[nodiscard]] auto                statusText() const -> QString { return statusText_; }
  [[nodiscard]] auto                canOpenDirectory() const -> bool { return canOpenDirectory_; }
  [[nodiscard]] auto                filterText() const -> QString { return filterText_; }
  void                              setFilterText(const QString& text);
  [[nodiscard]] auto                favoritePaths() const -> QStringList;

  /// force=true asks the library for a user-requested rescan; the list follows
  /// when the new inventory is published. force=false rebuilds the list from the
  /// current inventory and checks a missing current LUT through the library.
  Q_INVOKABLE void                  refresh(bool force = false);
  /// User selection: commit one settled LUT transaction when the path changes.
  /// Does not emit entriesChanged (selection is via selectedPathChanged only).
  Q_INVOKABLE void                  selectPath(const QString& path);
  /// Move selection by step among selectable entries; commits when path changes.
  Q_INVOKABLE bool                  selectRelative(int step);
  Q_INVOKABLE void                  clearSelection();
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;
  /// Open the library folder in the platform file manager. On failure the
  /// library's error is shown through statusText and openFolderFailed.
  Q_INVOKABLE bool                  openDirectory();
  /// Toggle the favorite state of a library entry given by its absolute path.
  Q_INVOKABLE void                  toggleFavoritePath(const QString& path);
  /// True when path is present in the persisted favoritePaths list.
  Q_INVOKABLE bool                  isFavoritePath(const QString& path) const;

 signals:
  void entriesChanged();
  void selectedPathChanged();
  void catalogChanged();
  void filterTextChanged();
  void settledCommitted();
  void openFolderFailed(const QString& message);
  void favoritePathsChanged();
  void libraryChanged();

 private:
  void                    rebuildEntriesView();
  void                    applySelectionHighlight();
  void                    submitSettled();
  [[nodiscard]] auto      buildParamsJson() const -> QString;
  [[nodiscard]] auto      relativePathOf(const QString& path) const -> std::optional<std::string>;
  /// Reference that selecting the list row at @p path submits.
  [[nodiscard]] auto      referenceForPath(const QString& path) const -> alcedo::LutReference;
  void setSelection(const QString& path, alcedo::LutReference reference, std::string name);

  QPointer<alcedo::LutLibraryService> library_;
  lut_catalog::LutCatalog catalog_{};
  QVariantList            entries_;
  QString                 selectedPath_;
  int                     selectedIndex_ = -1;
  QString                 directoryText_;
  QString                 statusText_;
  bool                    canOpenDirectory_ = false;
  QString                 filterText_;
  std::string             selectedPathUtf8_;
  alcedo::LutReference                selectedReference_;
  std::string                         selectedName_;
};

}  // namespace alcedo::ui
