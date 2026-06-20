#pragma once

#include <QSet>
#include <QWidget>
#include <vector>

#include "ui/alcedo_main/editor_dialog/modules/lut_catalog.hpp"

class QAction;
class QLabel;
class QListWidget;
class QLineEdit;
class QToolButton;

namespace alcedo::ui {

class ElidedLabel;

class LutBrowserWidget final : public QWidget {
  Q_OBJECT

 public:
  explicit LutBrowserWidget(QWidget* parent = nullptr);

  void RetranslateUi();
  void SetDirectoryInfo(const QString& directory_text, const QString& status_text,
                        bool can_open_directory);
  void SetEntries(const std::vector<lut_catalog::LutCatalogEntry>& entries,
                  const QString& selected_path, bool preserve_scroll_position = false);
  auto SelectRelativeEntry(int step) -> bool;

 signals:
  void OpenFolderRequested();
  void RefreshRequested();
  void LutPathActivated(const QString& path);

 private:
  enum class SortField {
    Name,
    ModifiedTime,
  };

  enum class SortOrder {
    Ascending,
    Descending,
  };

  enum class EntryFilter {
    Favorites,
    All,
  };

  void         RebuildVisibleEntries(const QString& preferred_selected_path,
                                     bool           preserve_scroll_position = false);
  void         UpdateSearchResultSummary();
  auto         CurrentSortField() const -> SortField;
  auto         CurrentSortOrder() const -> SortOrder;
  void         RefreshSelectionStyles();
  void         SetEntryFilter(EntryFilter filter);
  void         RefreshFilterButtonStyles();
  void         LoadFavoriteSettings();
  void         SaveFavoriteSettings() const;
  auto         IsFavoritePath(const QString& path) const -> bool;
  auto         IsFavoriteEntry(const lut_catalog::LutCatalogEntry& entry) const -> bool;
  void         ToggleFavoritePath(const QString& path);

  QLabel*      title_label_            = nullptr;
  ElidedLabel* directory_label_        = nullptr;
  QLabel*      status_label_           = nullptr;
  QLabel*      search_summary_label_   = nullptr;
  QLineEdit*   search_edit_            = nullptr;
  QToolButton* favorites_filter_btn_   = nullptr;
  QToolButton* all_filter_btn_         = nullptr;
  QToolButton* sort_btn_               = nullptr;
  QToolButton* folder_btn_             = nullptr;
  QAction*     sort_field_name_action_ = nullptr;
  QAction*     sort_field_time_action_ = nullptr;
  QAction*     sort_order_asc_action_  = nullptr;
  QAction*     sort_order_desc_action_ = nullptr;
  QAction*     refresh_action_         = nullptr;
  QListWidget* entries_list_           = nullptr;
  std::vector<lut_catalog::LutCatalogEntry> source_entries_{};
  std::vector<lut_catalog::LutCatalogEntry> visible_entries_{};
  QSet<QString>                             favorite_lut_paths_{};
  EntryFilter                               active_filter_    = EntryFilter::All;
  bool                                      updating_entries_ = false;
};

}  // namespace alcedo::ui
