//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <qfontdatabase.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleValidator>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QIntValidator>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <json.hpp>
#include <memory>
#include <opencv2/opencv.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/export_service.hpp"
#include "app/history_mgmt_service.hpp"
#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/render_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "app/thumbnail_service.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"
#include "renderer/pipeline_task.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "type/supported_file_type.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/edit_viewer.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
using namespace std::chrono_literals;

static QString FsPathToQString(const std::filesystem::path& path) {
#if defined(_WIN32)
  return QString::fromStdWString(path.wstring());
#else
  return QString::fromUtf8(path.string().c_str());
#endif
}

static std::optional<std::string_view> FindArgValue(int argc, char** argv,
                                                    std::string_view opt_name) {
  const std::string opt_eq = std::string(opt_name) + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i] ? argv[i] : "");
    if (arg == opt_name) {
      if (i + 1 < argc && argv[i + 1]) {
        return std::string_view(argv[i + 1]);
      }
      return std::nullopt;
    }
    if (arg.rfind(opt_eq, 0) == 0) {
      return arg.substr(opt_eq.size());
    }
  }
  return std::nullopt;
}

static void ApplyExternalAppFont(QApplication& app, int argc, char** argv) {
  std::vector<std::filesystem::path> candidates;

  if (const auto arg = FindArgValue(argc, argv, "--font"); arg.has_value()) {
    candidates.emplace_back(std::string(arg.value()));
  }
  if (const auto env = qEnvironmentVariable("ALCEDO_FONT_PATH"); !env.isEmpty()) {
    candidates.emplace_back(env.toStdString());
  }

  const auto app_dir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString());
  candidates.emplace_back(app_dir / "fonts" / "main_IBM.ttf");

#if defined(ALCEDO_SOURCE_DIR)
  candidates.emplace_back(std::filesystem::path(ALCEDO_SOURCE_DIR) / "alcedo" / "src" /
                          "config" / "fonts" / "main_IBM.ttf");
#endif

  for (const auto& path : candidates) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      continue;
    }

    const int font_id = QFontDatabase::addApplicationFont(FsPathToQString(path));
    if (font_id < 0) {
      continue;
    }

    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty()) {
      continue;
    }

    app.setFont(QFont(families.front()));
    return;
  }
}

static void ApplyFontRenderingTuning(QApplication& app, int argc, char** argv) {
  // Controls:
  //   --font-aa=[on|off]
  //   --font-hinting=[default|none|vertical|full]
  // env fallback:
  //   ALCEDO_FONT_AA
  //   ALCEDO_FONT_HINTING
  auto aa = FindArgValue(argc, argv, "--font-aa");
  if (!aa.has_value()) {
    const auto env = qEnvironmentVariable("ALCEDO_FONT_AA");
    if (!env.isEmpty()) {
      aa = std::string_view(env.toStdString());
    }
  }

  auto hinting = FindArgValue(argc, argv, "--font-hinting");
  if (!hinting.has_value()) {
    const auto env = qEnvironmentVariable("ALCEDO_FONT_HINTING");
    if (!env.isEmpty()) {
      hinting = std::string_view(env.toStdString());
    }
  }

  QFont tuned     = app.font();

  // Default to AA on for finer rendering.
  bool  enable_aa = true;
  if (aa.has_value()) {
    std::string aa_s(aa.value());
    std::transform(aa_s.begin(), aa_s.end(), aa_s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (aa_s == "off" || aa_s == "0" || aa_s == "false") {
      enable_aa = false;
    }
  }

  if (enable_aa) {
    tuned.setStyleStrategy(QFont::PreferAntialias);
  } else {
    tuned.setStyleStrategy(QFont::NoAntialias);
  }

  QFont::HintingPreference pref = QFont::PreferVerticalHinting;
  if (hinting.has_value()) {
    std::string h(hinting.value());
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (h == "none") {
      pref = QFont::PreferNoHinting;
    } else if (h == "full") {
      pref = QFont::PreferFullHinting;
    } else if (h == "default") {
      pref = QFont::HintingPreference::PreferDefaultHinting;
    } else {
      pref = QFont::PreferVerticalHinting;
    }
  }
  tuned.setHintingPreference(pref);

  app.setFont(tuned);
}

static void ApplyMaterialLikeTheme(QApplication& app) {
  app.setStyle(QStyleFactory::create("Fusion"));

  QPalette p;
  p.setColor(QPalette::Window, QColor(0x0E, 0x13, 0x1A));
  p.setColor(QPalette::WindowText, QColor(0xE8, 0xEE, 0xFA));
  p.setColor(QPalette::Base, QColor(0x12, 0x18, 0x22));
  p.setColor(QPalette::AlternateBase, QColor(0x17, 0x1F, 0x2B));
  p.setColor(QPalette::ToolTipBase, QColor(0x18, 0x20, 0x2D));
  p.setColor(QPalette::ToolTipText, QColor(0xEB, 0xF1, 0xFE));
  p.setColor(QPalette::Text, QColor(0xE8, 0xEE, 0xFA));
  p.setColor(QPalette::Button, QColor(0x17, 0x20, 0x2D));
  p.setColor(QPalette::ButtonText, QColor(0xE8, 0xEE, 0xFA));
  p.setColor(QPalette::Link, QColor(0x6C, 0xB0, 0xFF));
  p.setColor(QPalette::Highlight, QColor(0x5A, 0xA2, 0xFF));
  p.setColor(QPalette::HighlightedText, QColor(0x08, 0x0D, 0x14));
  app.setPalette(p);

  app.setStyleSheet(QString::fromUtf8(R"QSS(
QWidget {
  color: #E8EEFA;
  font-size: 13px;
}
QToolTip {
  background: #1A2230;
  color: #EDF3FF;
  border: 1px solid rgba(120, 151, 195, 0.45);
  border-radius: 10px;
  padding: 8px 10px;
}
QFrame#CardSurface {
  background: #151D28;
  border: 1px solid #27354A;
  border-radius: 16px;
}
QFrame#GlassSurface {
  background: rgba(21, 29, 40, 0.88);
  border: 1px solid rgba(64, 83, 112, 0.65);
  border-radius: 14px;
}
QLabel#SectionTitle {
  font-size: 17px;
  font-weight: 650;
  color: #F2F7FF;
}
QLabel#MetaText {
  color: #9CADE0;
  font-size: 12px;
}
QPushButton, QToolButton {
  background: #1A2331;
  color: #E8EEFA;
  border: 1px solid #344760;
  border-radius: 12px;
  padding: 7px 12px;
  min-height: 30px;
}
QPushButton:hover, QToolButton:hover {
  background: #223044;
  border-color: #5A78A6;
}
QPushButton:pressed, QToolButton:pressed {
  background: #263952;
}
QPushButton#accent, QToolButton#accent {
  background: #4F9BFF;
  color: #0A1018;
  border: 1px solid #4F9BFF;
  font-weight: 600;
}
QPushButton#accent:hover, QToolButton#accent:hover {
  background: #6CB0FF;
  border-color: #6CB0FF;
}
QLineEdit, QComboBox, QSpinBox {
  background: #111924;
  border: 1px solid #344861;
  color: #E8EEFA;
  border-radius: 11px;
  padding: 6px 10px;
  min-height: 28px;
  selection-background-color: #4F9BFF;
}
QLineEdit:hover, QComboBox:hover, QSpinBox:hover {
  border-color: #5C80B6;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus,
QPushButton:focus, QToolButton:focus, QListView:focus {
  border: 1px solid #5AA2FF;
  outline: none;
}
QCheckBox {
  spacing: 8px;
}
QCheckBox::indicator {
  width: 18px;
  height: 18px;
  border-radius: 6px;
  border: 1px solid #5A6E8E;
  background: #111924;
}
QCheckBox::indicator:hover {
  border-color: #5AA2FF;
}
QCheckBox::indicator:checked {
  background: #5AA2FF;
  border-color: #5AA2FF;
}
QSlider::groove:horizontal {
  height: 6px;
  border-radius: 3px;
  background: #2A3B52;
}
QSlider::sub-page:horizontal {
  background: #5AA2FF;
  border-radius: 3px;
}
QSlider::handle:horizontal {
  width: 18px;
  height: 18px;
  margin: -6px 0;
  border-radius: 9px;
  background: #E8F2FF;
  border: 2px solid #5AA2FF;
}
QListWidget {
  background: transparent;
  border: none;
}
QListWidget::item {
  border: 1px solid #2D3E55;
  border-radius: 14px;
  padding: 8px;
  margin: 4px;
  background: #131C29;
}
QListWidget::item:selected {
  background: #21344D;
  border-color: #5AA2FF;
}
QProgressBar {
  background: #132033;
  border: 1px solid #314763;
  border-radius: 10px;
  min-height: 16px;
  text-align: center;
}
QProgressBar::chunk {
  border-radius: 9px;
  background: #5AA2FF;
}
QMenu {
  background: rgba(18, 26, 37, 0.95);
  border: 1px solid rgba(62, 80, 109, 0.85);
  border-radius: 12px;
  padding: 6px;
}
QMenu::item {
  padding: 7px 12px;
  border-radius: 8px;
}
QMenu::item:selected {
  background: rgba(90, 162, 255, 0.18);
}
)QSS"));
}

enum class FilterValueKind { String, Int64, Double, DateTime };

static FilterValueKind KindForField(FilterField field) {
  switch (field) {
    case FilterField::ExifISO:
    case FilterField::Rating:
      return FilterValueKind::Int64;
    case FilterField::ExifFocalLength:
    case FilterField::ExifAperture:
      return FilterValueKind::Double;
    case FilterField::CaptureDate:
    case FilterField::ImportDate:
      return FilterValueKind::DateTime;
    default:
      return FilterValueKind::String;
  }
}

static void PopulateCompareOps(QComboBox* combo, FilterField field) {
  combo->clear();

  const auto kind   = KindForField(field);
  const auto add_op = [combo](CompareOp op, const char* label) {
    combo->addItem(QString::fromUtf8(label), static_cast<int>(op));
  };

  if (kind == FilterValueKind::String) {
    add_op(CompareOp::CONTAINS, "contains");
    add_op(CompareOp::NOT_CONTAINS, "not contains");
    add_op(CompareOp::EQUALS, "=");
    add_op(CompareOp::NOT_EQUALS, "!=");
    add_op(CompareOp::STARTS_WITH, "starts with");
    add_op(CompareOp::ENDS_WITH, "ends with");
    add_op(CompareOp::REGEX, "regex");
  } else if (kind == FilterValueKind::Int64 || kind == FilterValueKind::Double) {
    add_op(CompareOp::EQUALS, "=");
    add_op(CompareOp::NOT_EQUALS, "!=");
    add_op(CompareOp::GREATER_THAN, "> ");
    add_op(CompareOp::LESS_THAN, "< ");
    add_op(CompareOp::GREATER_EQUAL, ">=");
    add_op(CompareOp::LESS_EQUAL, "<=");
    add_op(CompareOp::BETWEEN, "between");
  } else {
    // Date/time
    add_op(CompareOp::GREATER_THAN, "> ");
    add_op(CompareOp::LESS_THAN, "< ");
    add_op(CompareOp::GREATER_EQUAL, ">=");
    add_op(CompareOp::LESS_EQUAL, "<=");
    add_op(CompareOp::BETWEEN, "between");
  }
}

static std::optional<std::tm> ParseDateTimeYmd(const QString& text) {
  // Accept: YYYY-MM-DD
  const auto trimmed = text.trimmed();
  const auto parts   = trimmed.split('-', Qt::SkipEmptyParts);
  if (parts.size() != 3) {
    return std::nullopt;
  }
  bool      ok_y = false, ok_m = false, ok_d = false;
  const int year  = parts[0].toInt(&ok_y);
  const int month = parts[1].toInt(&ok_m);
  const int day   = parts[2].toInt(&ok_d);
  if (!ok_y || !ok_m || !ok_d) {
    return std::nullopt;
  }
  if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31) {
    return std::nullopt;
  }

  std::tm t{};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  return t;
}

static std::optional<FilterValue> ParseFilterValue(FilterField field, const QString& text) {
  const auto kind = KindForField(field);
  if (kind == FilterValueKind::String) {
    return FilterValue{text.trimmed().toStdWString()};
  }
  if (kind == FilterValueKind::Int64) {
    bool       ok = false;
    const auto v  = text.trimmed().toLongLong(&ok);
    if (!ok) {
      return std::nullopt;
    }
    return FilterValue{static_cast<int64_t>(v)};
  }
  if (kind == FilterValueKind::Double) {
    bool       ok = false;
    const auto v  = text.trimmed().toDouble(&ok);
    if (!ok) {
      return std::nullopt;
    }
    return FilterValue{v};
  }

  const auto tm_opt = ParseDateTimeYmd(text);
  if (!tm_opt.has_value()) {
    return std::nullopt;
  }
  return FilterValue{tm_opt.value()};
}

class FilterDrawer final : public QWidget {
 public:
  explicit FilterDrawer(QWidget* parent = nullptr) : QWidget(parent) {
    setObjectName("FilterDrawer");
    setMinimumWidth(360);
    setMaximumWidth(420);
    setStyleSheet(DrawerStyleSheet());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(12, 12, 12, 0);
    header->setSpacing(8);

    auto* title = new QLabel("Filters", this);
    title->setObjectName("DrawerTitle");

    collapse_btn_ = new QToolButton(this);
    collapse_btn_->setObjectName("CollapseButton");
    collapse_btn_->setCheckable(true);
    collapse_btn_->setChecked(true);
    collapse_btn_->setToolTip("Collapse / expand filter builder");
    collapse_btn_->setArrowType(Qt::DownArrow);
    collapse_btn_->setAutoRaise(true);

    header->addWidget(title, 0);
    header->addStretch(1);
    header->addWidget(collapse_btn_, 0);
    root->addLayout(header, 0);

    content_             = new QWidget(this);
    auto* content_layout = new QVBoxLayout(content_);
    content_layout->setContentsMargins(12, 0, 12, 12);
    content_layout->setSpacing(10);

    // Quick search card.
    auto* quick_card = new QFrame(this);
    quick_card->setObjectName("Card");
    auto* quick = new QVBoxLayout(quick_card);
    quick->setContentsMargins(12, 12, 12, 12);
    quick->setSpacing(6);

    auto* quick_label = new QLabel("Quick search", this);
    quick_label->setObjectName("CardTitle");

    auto* quick_row = new QHBoxLayout();
    quick_row->setSpacing(6);

    quick_search_ = new QLineEdit(this);
    quick_search_->setObjectName("QuickSearch");
    quick_search_->setPlaceholderText("Search filename, camera model, tags…");
    quick_search_->setClearButtonEnabled(true);

    inline_apply_ = new QToolButton(this);
    inline_apply_->setObjectName("InlineApply");
    inline_apply_->setAutoRaise(true);
    inline_apply_->setToolTip("Apply filters");
    inline_apply_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));

    quick_row->addWidget(quick_search_, 1);
    quick_row->addWidget(inline_apply_, 0);

    quick->addWidget(quick_label, 0);
    quick->addLayout(quick_row, 0);
    content_layout->addWidget(quick_card, 0);

    // Rules card.
    auto* rules_card = new QFrame(this);
    rules_card->setObjectName("Card");
    auto* rules = new QVBoxLayout(rules_card);
    rules->setContentsMargins(12, 12, 12, 12);
    rules->setSpacing(8);

    auto* rules_top   = new QHBoxLayout();
    auto* rules_title = new QLabel("Rules", this);
    rules_title->setObjectName("CardTitle");

    join_op_ = new QComboBox(this);
    join_op_->setObjectName("JoinOp");
    join_op_->addItem("Match all rules", static_cast<int>(FilterOp::AND));
    join_op_->addItem("Match any rule", static_cast<int>(FilterOp::OR));
    join_op_->setToolTip("How multiple rules are combined");

    rules_top->addWidget(rules_title, 0);
    rules_top->addStretch(1);
    rules_top->addWidget(join_op_, 0);
    rules->addLayout(rules_top, 0);

    rows_scroll_ = new QScrollArea(this);
    rows_scroll_->setObjectName("RowsScroll");
    rows_scroll_->setWidgetResizable(true);
    rows_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rows_scroll_->setFrameShape(QFrame::NoFrame);

    rows_widget_    = new QWidget(this);
    rows_container_ = new QVBoxLayout(rows_widget_);
    rows_container_->setContentsMargins(0, 0, 0, 0);
    rows_container_->setSpacing(8);
    rows_container_->addStretch(1);
    rows_scroll_->setWidget(rows_widget_);
    rules->addWidget(rows_scroll_, 1);

    btn_add_ = new QPushButton("Add rule", this);
    btn_add_->setObjectName("AddRule");
    btn_add_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    rules->addWidget(btn_add_, 0, Qt::AlignLeft);

    content_layout->addWidget(rules_card, 1);

    // Footer.
    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    btn_clear_ = new QPushButton("Clear", this);
    btn_clear_->setObjectName("ClearButton");
    btn_clear_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    btn_apply_ = new QPushButton("Apply", this);
    btn_apply_->setObjectName("ApplyButton");
    btn_apply_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    actions->addWidget(btn_clear_, 0);
    actions->addStretch(1);
    actions->addWidget(btn_apply_, 0);
    content_layout->addLayout(actions, 0);

    info_ = new QLabel("No images loaded.", this);
    info_->setObjectName("FilterInfo");
    content_layout->addWidget(info_, 0);

    sql_preview_ = new QLabel("", this);
    sql_preview_->setObjectName("SqlPreview");
    sql_preview_->setWordWrap(true);
    sql_preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sql_preview_->setVisible(false);
    content_layout->addWidget(sql_preview_, 0);

    root->addWidget(content_, 1);

    connect(collapse_btn_, &QToolButton::toggled, this, [this](bool expanded) {
      content_->setVisible(expanded);
      collapse_btn_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });

    connect(btn_add_, &QPushButton::clicked, this, [this]() { AddRuleRow(); });
    connect(btn_clear_, &QPushButton::clicked, this, [this]() {
      ClearAllUi();
      if (on_clear_) {
        on_clear_();
      }
    });

    const auto apply_now = [this]() {
      auto node_opt = BuildFilterNode(true);
      if (!node_opt.has_value()) {
        return;
      }
      SetSqlPreview(QString::fromStdString(FilterSQLCompiler::Compile(node_opt.value()).sql_));
      if (on_apply_) {
        on_apply_(node_opt.value());
      }
    };

    connect(btn_apply_, &QPushButton::clicked, this, apply_now);
    connect(inline_apply_, &QToolButton::clicked, this, apply_now);
    connect(quick_search_, &QLineEdit::returnPressed, this, apply_now);

    AddRuleRow();
  }

  void SetOnApply(std::function<void(const FilterNode&)> fn) { on_apply_ = std::move(fn); }
  void SetOnClear(std::function<void()> fn) { on_clear_ = std::move(fn); }

  void SetResultsSummary(int shown, int total) {
    if (total <= 0) {
      info_->setText("No images loaded.");
      return;
    }
    if (shown == total) {
      info_->setText(QString("Showing %1 images").arg(total));
      return;
    }
    info_->setText(QString("Showing %1 of %2").arg(shown).arg(total));
  }

 private:
  struct Row {
    QWidget*     container_ = nullptr;
    QComboBox*   field_     = nullptr;
    QComboBox*   op_        = nullptr;
    QLineEdit*   value_     = nullptr;
    QLabel*      between_   = nullptr;
    QLineEdit*   value2_    = nullptr;
    QToolButton* remove_    = nullptr;
  };

  static QString DrawerStyleSheet() {
    return QString::fromUtf8(R"QSS(
#FilterDrawer { background: transparent; }
#FilterDrawer QLabel#DrawerTitle { font-size: 17px; font-weight: 650; color: #EFF5FF; }
#FilterDrawer QFrame#Card { background: #182231; border: 1px solid #2D3D55; border-radius: 14px; }
#FilterDrawer QLabel#CardTitle { font-weight: 620; color: #EAF1FF; }
#FilterDrawer QLineEdit#QuickSearch {
  padding: 8px 10px;
  border: 1px solid #344A66;
  border-radius: 11px;
  background: #111924;
}
#FilterDrawer QComboBox, #FilterDrawer QLineEdit {
  padding: 6px 9px;
  border: 1px solid #344A66;
  border-radius: 10px;
  background: #111924;
}
#FilterDrawer QPushButton {
  padding: 7px 12px;
  border-radius: 11px;
  background: #1A2638;
  border: 1px solid #344A66;
}
#FilterDrawer QPushButton#ApplyButton { background: #5AA2FF; color: #0A1018; border: 1px solid #5AA2FF; }
#FilterDrawer QPushButton#ApplyButton:hover { background: #6CB0FF; }
#FilterDrawer QToolButton#InlineApply {
  background: rgba(90, 162, 255, 0.14);
  border: 1px solid rgba(90, 162, 255, 0.30);
  border-radius: 10px;
  padding: 5px;
}
#FilterDrawer QToolButton#CollapseButton {
  border-radius: 9px;
  border: 1px solid #344A66;
  padding: 5px;
}
#FilterDrawer QToolButton#RemoveRule {
  background: transparent;
  border: 1px solid transparent;
  padding: 6px;
}
#FilterDrawer QToolButton#RemoveRule:hover {
  background: #213047;
  border-radius: 10px;
  border-color: #3A4F6D;
}
#FilterDrawer QLabel#FilterInfo { color: #9CADCC; }
#FilterDrawer QLabel#SqlPreview {
  color: #E3EEFF;
  font-family: 'Consolas','Courier New',monospace;
  font-size: 11px;
  padding: 8px 10px;
  background: #111923;
  border: 1px solid #2D3D55;
  border-radius: 10px;
}
)QSS");
  }

  static QString PlaceholderForField(FilterField field) {
    switch (field) {
      case FilterField::CaptureDate:
      case FilterField::ImportDate:
        return "YYYY-MM-DD";
      case FilterField::ExifISO:
      case FilterField::Rating:
        return "number";
      case FilterField::ExifAperture:
      case FilterField::ExifFocalLength:
        return "number";
      default:
        return "type to filter…";
    }
  }

  Row* FindRow(QWidget* container) {
    for (auto& r : rows_) {
      if (r.container_ == container) {
        return &r;
      }
    }
    return nullptr;
  }

  void ConfigureEditorsForRow(Row& row, FilterField field) {
    PopulateCompareOps(row.op_, field);
    row.value_->setPlaceholderText(PlaceholderForField(field));
    row.value2_->setPlaceholderText(PlaceholderForField(field));

    row.value_->setValidator(nullptr);
    row.value2_->setValidator(nullptr);

    const auto kind = KindForField(field);
    if (kind == FilterValueKind::Int64) {
      row.value_->setValidator(new QIntValidator(row.value_));
      row.value2_->setValidator(new QIntValidator(row.value2_));
    } else if (kind == FilterValueKind::Double) {
      auto* v1 = new QDoubleValidator(row.value_);
      v1->setNotation(QDoubleValidator::StandardNotation);
      row.value_->setValidator(v1);
      auto* v2 = new QDoubleValidator(row.value2_);
      v2->setNotation(QDoubleValidator::StandardNotation);
      row.value2_->setValidator(v2);
    }
  }

  void UpdateRowBetweenUi(Row& row) {
    const auto op         = static_cast<CompareOp>(row.op_->currentData().toInt());
    const bool is_between = (op == CompareOp::BETWEEN);
    row.between_->setVisible(is_between);
    row.value2_->setVisible(is_between);
  }

  void AddRuleRow() {
    Row r;
    r.container_ = new QWidget(this);
    auto* h      = new QHBoxLayout(r.container_);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);

    r.field_ = new QComboBox(this);
    r.field_->addItem("Filename", static_cast<int>(FilterField::FileName));
    r.field_->addItem("Camera Model", static_cast<int>(FilterField::ExifCameraModel));
    r.field_->addItem("File Extension", static_cast<int>(FilterField::FileExtension));
    r.field_->addItem("ISO", static_cast<int>(FilterField::ExifISO));
    r.field_->addItem("Aperture", static_cast<int>(FilterField::ExifAperture));
    r.field_->addItem("Focal Length", static_cast<int>(FilterField::ExifFocalLength));
    r.field_->addItem("Capture Date", static_cast<int>(FilterField::CaptureDate));
    r.field_->addItem("Import Date", static_cast<int>(FilterField::ImportDate));
    r.field_->addItem("Rating", static_cast<int>(FilterField::Rating));
    r.field_->addItem("Tags", static_cast<int>(FilterField::SemanticTags));

    r.op_      = new QComboBox(this);

    r.value_   = new QLineEdit(this);
    r.value2_  = new QLineEdit(this);
    r.between_ = new QLabel("to", this);

    r.remove_  = new QToolButton(this);
    r.remove_->setObjectName("RemoveRule");
    r.remove_->setAutoRaise(true);
    r.remove_->setIcon(style()->standardIcon(QStyle::SP_DockWidgetCloseButton));
    r.remove_->setToolTip("Remove rule");

    h->addWidget(r.field_, 1);
    h->addWidget(r.op_, 0);
    h->addWidget(r.value_, 1);
    h->addWidget(r.between_, 0);
    h->addWidget(r.value2_, 1);
    h->addWidget(r.remove_, 0);

    const auto initial_field = static_cast<FilterField>(r.field_->currentData().toInt());
    ConfigureEditorsForRow(r, initial_field);
    UpdateRowBetweenUi(r);

    connect(r.field_, &QComboBox::currentIndexChanged, this, [this, c = r.container_]() {
      auto* row = FindRow(c);
      if (!row) {
        return;
      }
      const auto field = static_cast<FilterField>(row->field_->currentData().toInt());
      {
        QSignalBlocker block(*row->op_);
        ConfigureEditorsForRow(*row, field);
      }
      UpdateRowBetweenUi(*row);
    });
    connect(r.op_, &QComboBox::currentIndexChanged, this, [this, c = r.container_]() {
      auto* row = FindRow(c);
      if (!row) {
        return;
      }
      UpdateRowBetweenUi(*row);
    });

    connect(r.remove_, &QToolButton::clicked, this, [this, c = r.container_]() {
      auto it = std::find_if(rows_.begin(), rows_.end(),
                             [c](const Row& rr) { return rr.container_ == c; });
      if (it == rows_.end()) {
        return;
      }
      delete it->container_;
      rows_.erase(it);
    });

    const int stretch_index = rows_container_->count() - 1;
    rows_container_->insertWidget(stretch_index, r.container_);
    rows_.push_back(r);
  }

  void ClearAllUi() {
    quick_search_->clear();
    SetSqlPreview(QString());

    for (auto& row : rows_) {
      delete row.container_;
    }
    rows_.clear();
    AddRuleRow();
  }

  void SetSqlPreview(const QString& sql) {
    const auto trimmed = sql.trimmed();
    if (trimmed.isEmpty()) {
      sql_preview_->clear();
      sql_preview_->setVisible(false);
      return;
    }
    sql_preview_->setText(trimmed);
    sql_preview_->setVisible(true);
  }

  std::optional<FilterNode> BuildFilterNode(bool show_dialogs) {
    std::optional<FilterNode> quick_node;
    const auto                q = quick_search_->text().trimmed();
    if (!q.isEmpty()) {
      const FilterValue       v{q.toStdWString()};
      std::vector<FilterNode> clauses;
      clauses.reserve(4);
      clauses.push_back(FilterNode{FilterNode::Type::Condition,
                                   {},
                                   {},
                                   FieldCondition{.field_        = FilterField::FileName,
                                                  .op_           = CompareOp::CONTAINS,
                                                  .value_        = v,
                                                  .second_value_ = std::nullopt},
                                   std::nullopt});
      clauses.push_back(FilterNode{FilterNode::Type::Condition,
                                   {},
                                   {},
                                   FieldCondition{.field_        = FilterField::ExifCameraModel,
                                                  .op_           = CompareOp::CONTAINS,
                                                  .value_        = v,
                                                  .second_value_ = std::nullopt},
                                   std::nullopt});
      clauses.push_back(FilterNode{FilterNode::Type::Condition,
                                   {},
                                   {},
                                   FieldCondition{.field_        = FilterField::FileExtension,
                                                  .op_           = CompareOp::CONTAINS,
                                                  .value_        = v,
                                                  .second_value_ = std::nullopt},
                                   std::nullopt});

      quick_node =
          FilterNode{FilterNode::Type::Logical, FilterOp::OR, std::move(clauses), {}, std::nullopt};
    }

    std::optional<FilterNode> rules_node;
    std::vector<FilterNode>   conditions;
    conditions.reserve(rows_.size());

    for (const auto& row : rows_) {
      const auto field   = static_cast<FilterField>(row.field_->currentData().toInt());
      const auto op      = static_cast<CompareOp>(row.op_->currentData().toInt());

      const auto v1_text = row.value_->text().trimmed();
      if (v1_text.isEmpty()) {
        continue;
      }

      const auto v1_opt = ParseFilterValue(field, v1_text);
      if (!v1_opt.has_value()) {
        if (show_dialogs) {
          QMessageBox::warning(this, "Filter", "Invalid value. Check field type and input.");
        }
        return std::nullopt;
      }

      FieldCondition cond{
          .field_ = field, .op_ = op, .value_ = v1_opt.value(), .second_value_ = std::nullopt};

      if (op == CompareOp::BETWEEN) {
        const auto v2_text = row.value2_->text().trimmed();
        if (v2_text.isEmpty()) {
          if (show_dialogs) {
            QMessageBox::warning(this, "Filter", "BETWEEN needs two values.");
          }
          return std::nullopt;
        }
        const auto v2_opt = ParseFilterValue(field, v2_text);
        if (!v2_opt.has_value()) {
          if (show_dialogs) {
            QMessageBox::warning(this, "Filter", "Invalid second value for BETWEEN.");
          }
          return std::nullopt;
        }
        cond.second_value_ = v2_opt.value();
      }

      conditions.push_back(
          FilterNode{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt});
    }

    if (!conditions.empty()) {
      if (conditions.size() == 1) {
        rules_node = conditions.front();
      } else {
        const auto join = static_cast<FilterOp>(join_op_->currentData().toInt());
        rules_node =
            FilterNode{FilterNode::Type::Logical, join, std::move(conditions), {}, std::nullopt};
      }
    }

    if (!quick_node.has_value() && !rules_node.has_value()) {
      if (show_dialogs) {
        QMessageBox::information(this, "Filter", "No filters set.");
      }
      return std::nullopt;
    }
    if (quick_node.has_value() && !rules_node.has_value()) {
      return quick_node;
    }
    if (!quick_node.has_value() && rules_node.has_value()) {
      return rules_node;
    }

    std::vector<FilterNode> children;
    children.reserve(2);
    children.push_back(std::move(quick_node.value()));
    children.push_back(std::move(rules_node.value()));
    return FilterNode{
        FilterNode::Type::Logical, FilterOp::AND, std::move(children), {}, std::nullopt};
  }

  QToolButton*                           collapse_btn_   = nullptr;
  QWidget*                               content_        = nullptr;

  QLineEdit*                             quick_search_   = nullptr;
  QToolButton*                           inline_apply_   = nullptr;

  QComboBox*                             join_op_        = nullptr;
  QScrollArea*                           rows_scroll_    = nullptr;
  QWidget*                               rows_widget_    = nullptr;
  QVBoxLayout*                           rows_container_ = nullptr;
  QPushButton*                           btn_add_        = nullptr;
  QPushButton*                           btn_apply_      = nullptr;
  QPushButton*                           btn_clear_      = nullptr;

  QLabel*                                info_           = nullptr;
  QLabel*                                sql_preview_    = nullptr;

  std::vector<Row>                       rows_;

  std::function<void(const FilterNode&)> on_apply_;
  std::function<void()>                  on_clear_;
};

static std::vector<std::filesystem::path> ListCubeLutsInDir(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> results;
  std::error_code                    ec;
  if (!std::filesystem::exists(dir, ec) || ec || !std::filesystem::is_directory(dir, ec)) {
    return results;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }

    const auto  ext = entry.path().extension().string();
    std::string ext_lower;
    ext_lower.reserve(ext.size());
    for (const char c : ext) {
      ext_lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (ext_lower == ".cube") {
      results.push_back(entry.path());
    }
  }

  std::sort(results.begin(), results.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return a.filename().string() < b.filename().string();
            });

  return results;
}

static QImage MatRgba32fToQImageCopy(const cv::Mat& rgba32f_or_u8) {
  if (rgba32f_or_u8.empty()) {
    return {};
  }

  cv::Mat rgba8;
  if (rgba32f_or_u8.type() == CV_32FC4) {
    rgba32f_or_u8.convertTo(rgba8, CV_8UC4, 255.0);
  } else if (rgba32f_or_u8.type() == CV_8UC4) {
    rgba8 = rgba32f_or_u8;
  } else {
    cv::Mat tmp;
    rgba32f_or_u8.convertTo(tmp, CV_8UC4);
    rgba8 = tmp;
  }

  if (!rgba8.isContinuous()) {
    rgba8 = rgba8.clone();
  }

  QImage img(rgba8.data, rgba8.cols, rgba8.rows, static_cast<int>(rgba8.step),
             QImage::Format_RGBA8888);
  return img.copy();
}

static QIcon MakeSkeletonThumbnailIcon(int width, int height, const QString& label) {
  QPixmap px(width, height);
  px.fill(QColor(0x11, 0x18, 0x23));

  QPainter painter(&px);
  painter.setRenderHint(QPainter::Antialiasing, true);

  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(0x1A, 0x25, 0x35));
  painter.drawRoundedRect(QRectF(10.0, 10.0, width - 20.0, height - 20.0), 14.0, 14.0);

  painter.setBrush(QColor(0x2A, 0x3B, 0x51));
  painter.drawRoundedRect(QRectF(18.0, height - 44.0, width - 36.0, 10.0), 5.0, 5.0);
  painter.drawRoundedRect(QRectF(18.0, height - 28.0, width - 80.0, 8.0), 4.0, 4.0);

  painter.setPen(QColor(0x9A, 0xAF, 0xCE));
  painter.drawText(QRectF(0.0, 0.0, width, height), Qt::AlignCenter, label);
  painter.end();

  return QIcon(px);
}

static std::string ExtensionForExportFormat(ImageFormatType fmt) {
  switch (fmt) {
    case ImageFormatType::JPEG:
      return ".jpg";
    case ImageFormatType::PNG:
      return ".png";
    case ImageFormatType::TIFF:
      return ".tiff";
    case ImageFormatType::WEBP:
      return ".webp";
    case ImageFormatType::EXR:
      return ".exr";
    default:
      return ".jpg";
  }
}

static QString FormatName(ImageFormatType fmt) {
  switch (fmt) {
    case ImageFormatType::JPEG:
      return "JPEG";
    case ImageFormatType::PNG:
      return "PNG";
    case ImageFormatType::TIFF:
      return "TIFF";
    case ImageFormatType::WEBP:
      return "WEBP";
    case ImageFormatType::EXR:
      return "EXR";
    default:
      return "JPEG";
  }
}

static ImageFormatType FormatFromName(const QString& s) {
  const auto u = s.trimmed().toUpper();
  if (u == "PNG") {
    return ImageFormatType::PNG;
  }
  if (u == "TIFF") {
    return ImageFormatType::TIFF;
  }
  if (u == "WEBP") {
    return ImageFormatType::WEBP;
  }
  if (u == "EXR") {
    return ImageFormatType::EXR;
  }
  return ImageFormatType::JPEG;
}

class CardFrame final : public QFrame {
 public:
  explicit CardFrame(const QString& title = {}, QWidget* parent = nullptr) : QFrame(parent) {
    setObjectName("CardSurface");
    setFrameShape(QFrame::NoFrame);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    header_ = new QHBoxLayout();
    header_->setContentsMargins(0, 0, 0, 0);
    header_->setSpacing(8);
    root->addLayout(header_, 0);

    title_ = new QLabel(title, this);
    title_->setObjectName("SectionTitle");
    title_->setVisible(!title.isEmpty());
    header_->addWidget(title_, 0);
    header_->addStretch(1);

    body_ = new QVBoxLayout();
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(12);
    root->addLayout(body_, 1);

    footer_ = new QHBoxLayout();
    footer_->setContentsMargins(0, 0, 0, 0);
    footer_->setSpacing(8);
    footer_->setAlignment(Qt::AlignRight);
    root->addLayout(footer_, 0);
    footer_->setEnabled(false);
    footer_->setSpacing(0);
    footer_->setContentsMargins(0, 0, 0, 0);
  }

  auto HeaderLayout() const -> QHBoxLayout* { return header_; }
  auto BodyLayout() const -> QVBoxLayout* { return body_; }
  auto FooterLayout() const -> QHBoxLayout* {
    footer_->setEnabled(true);
    if (footer_->spacing() == 0) {
      footer_->setSpacing(8);
    }
    return footer_;
  }
  auto TitleLabel() const -> QLabel* { return title_; }

 private:
  QHBoxLayout* header_ = nullptr;
  QVBoxLayout* body_   = nullptr;
  QHBoxLayout* footer_ = nullptr;
  QLabel*      title_  = nullptr;
};

class ImportDialog final : public QDialog {
 public:
  explicit ImportDialog(QWidget* parent = nullptr) : QDialog(parent) {
    setModal(true);
    setWindowTitle("Import");
    resize(760, 560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(12);

    auto* intro = new QLabel(
        "Import RAW/JPEG assets into the library. Files are ingested asynchronously and metadata "
        "is progressively extracted.",
        this);
    intro->setWordWrap(true);
    intro->setObjectName("MetaText");
    root->addWidget(intro);

    auto* body_split = new QSplitter(Qt::Horizontal, this);
    body_split->setChildrenCollapsible(false);
    body_split->setHandleWidth(8);
    root->addWidget(body_split, 1);

    auto* sources_card = new CardFrame("Sources", body_split);
    auto* files_top    = new QHBoxLayout();
    files_top->setContentsMargins(0, 0, 0, 0);
    files_top->setSpacing(8);
    auto* add_files_btn    = new QPushButton("Add files…", sources_card);
    auto* remove_files_btn = new QPushButton("Remove selected", sources_card);
    auto* clear_files_btn  = new QPushButton("Clear", sources_card);
    files_top->addWidget(add_files_btn, 0);
    files_top->addWidget(remove_files_btn, 0);
    files_top->addWidget(clear_files_btn, 0);
    files_top->addStretch(1);
    sources_card->BodyLayout()->addLayout(files_top);

    files_list_ = new QListWidget(sources_card);
    files_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    files_list_->setAlternatingRowColors(false);
    files_list_->setMinimumHeight(300);
    sources_card->BodyLayout()->addWidget(files_list_, 1);

    summary_ = new QLabel("No files selected.", sources_card);
    summary_->setObjectName("MetaText");
    sources_card->BodyLayout()->addWidget(summary_);

    auto* options_card = new CardFrame("Import options", body_split);
    auto* form         = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    options_card->BodyLayout()->addLayout(form, 0);

    sort_mode_ = new QComboBox(options_card);
    sort_mode_->addItem("File name", static_cast<int>(ImportSortMode::FILE_NAME));
    sort_mode_->addItem("Full path", static_cast<int>(ImportSortMode::FULL_PATH));
    sort_mode_->addItem("As selected", static_cast<int>(ImportSortMode::NONE));
    form->addRow("Sort mode", sort_mode_);

    persist_placeholders_ = new QCheckBox("Persist placeholders immediately", options_card);
    persist_placeholders_->setChecked(false);
    options_card->BodyLayout()->addWidget(persist_placeholders_, 0);

    write_sequence_ = new QCheckBox("Record deterministic import sequence", options_card);
    write_sequence_->setChecked(true);
    options_card->BodyLayout()->addWidget(write_sequence_, 0);

    auto* hints = new QLabel(
        "Unsupported files are ignored. Metadata extraction runs after placeholders are created, "
        "so thumbnails appear progressively.",
        options_card);
    hints->setObjectName("MetaText");
    hints->setWordWrap(true);
    options_card->BodyLayout()->addWidget(hints, 0);
    options_card->BodyLayout()->addStretch(1);

    body_split->setStretchFactor(0, 3);
    body_split->setStretchFactor(1, 2);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText("Start Import");
    buttons->button(QDialogButtonBox::Ok)->setProperty("class", "accent");
    buttons->button(QDialogButtonBox::Ok)->setObjectName("accent");
    root->addWidget(buttons, 0);

    connect(add_files_btn, &QPushButton::clicked, this, [this]() { AddFiles(); });
    connect(remove_files_btn, &QPushButton::clicked, this, [this]() { RemoveSelected(); });
    connect(clear_files_btn, &QPushButton::clicked, this, [this]() { ClearAll(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() { accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  }

  auto SelectedPaths() const -> const std::vector<image_path_t>& { return paths_; }

  auto SelectedOptions() const -> ImportOptions {
    ImportOptions options;
    options.sort_mode_ = static_cast<ImportSortMode>(sort_mode_->currentData().toInt());
    options.persist_placeholders_immediately_ = persist_placeholders_->isChecked();
    options.write_import_sequence_            = write_sequence_->isChecked();
    return options;
  }

 protected:
  void accept() override {
    if (paths_.empty()) {
      QMessageBox::information(this, "Import", "Add at least one supported image.");
      return;
    }
    QDialog::accept();
  }

 private:
  void AddFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Select images", QString(),
        "Images (*.dng *.nef *.cr2 *.cr3 *.arw *.rw2 *.raf *.tif *.tiff *.jpg *.jpeg *.png *.fff);;All "
        "Files (*)");
    if (files.isEmpty()) {
      return;
    }

    int unsupported = 0;
    for (const auto& f : files) {
      const auto path = std::filesystem::path(f.toStdWString());
      if (!is_supported_file(path)) {
        ++unsupported;
        continue;
      }
      const auto key = path.wstring();
      if (seen_.contains(key)) {
        continue;
      }
      seen_.insert(key);
      paths_.push_back(path);

      auto* item = new QListWidgetItem(style()->standardIcon(QStyle::SP_FileIcon),
                                       QString::fromStdWString(path.filename().wstring()));
      item->setToolTip(QString::fromStdWString(path.wstring()));
      item->setData(Qt::UserRole + 1, QString::fromStdWString(path.wstring()));
      files_list_->addItem(item);
    }

    UpdateSummary();
    if (unsupported > 0) {
      QMessageBox::information(this, "Import",
                               QString("Ignored %1 unsupported file(s).").arg(unsupported));
    }
  }

  void RemoveSelected() {
    const auto selected = files_list_->selectedItems();
    if (selected.isEmpty()) {
      return;
    }
    std::unordered_set<std::wstring> remove_keys;
    remove_keys.reserve(static_cast<size_t>(selected.size()));

    for (auto* item : selected) {
      if (!item) {
        continue;
      }
      remove_keys.insert(item->data(Qt::UserRole + 1).toString().toStdWString());
      delete item;
    }

    paths_.erase(std::remove_if(paths_.begin(), paths_.end(),
                                [&remove_keys](const image_path_t& p) {
                                  return remove_keys.contains(p.wstring());
                                }),
                 paths_.end());

    for (const auto& k : remove_keys) {
      seen_.erase(k);
    }
    UpdateSummary();
  }

  void ClearAll() {
    paths_.clear();
    seen_.clear();
    files_list_->clear();
    UpdateSummary();
  }

  void UpdateSummary() {
    if (paths_.empty()) {
      summary_->setText("No files selected.");
      return;
    }
    summary_->setText(QString("%1 file(s) queued").arg(static_cast<int>(paths_.size())));
  }

  QListWidget*                     files_list_           = nullptr;
  QLabel*                          summary_              = nullptr;
  QComboBox*                       sort_mode_            = nullptr;
  QCheckBox*                       persist_placeholders_ = nullptr;
  QCheckBox*                       write_sequence_       = nullptr;
  std::vector<image_path_t>        paths_;
  std::unordered_set<std::wstring> seen_;
};

class SettingsPage final : public QWidget {
 public:
  explicit SettingsPage(QWidget* parent = nullptr) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* title = new QLabel("Settings", this);
    title->setObjectName("SectionTitle");
    root->addWidget(title, 0, Qt::AlignLeft);

    auto* subtitle =
        new QLabel("Appearance and performance preferences for this workstation.", this);
    subtitle->setObjectName("MetaText");
    root->addWidget(subtitle, 0, Qt::AlignLeft);

    auto* content = new QScrollArea(this);
    content->setFrameShape(QFrame::NoFrame);
    content->setWidgetResizable(true);
    root->addWidget(content, 1);

    auto* pane = new QWidget(content);
    auto* body = new QVBoxLayout(pane);
    body->setContentsMargins(2, 2, 2, 2);
    body->setSpacing(12);
    content->setWidget(pane);

    auto* appearance = new CardFrame("Appearance", pane);
    auto* app_form   = new QFormLayout();
    app_form->setContentsMargins(0, 0, 0, 0);
    app_form->setHorizontalSpacing(12);
    app_form->setVerticalSpacing(10);
    appearance->BodyLayout()->addLayout(app_form);

    auto* theme = new QComboBox(appearance);
    theme->addItems({"Light", "System"});
    auto* accent = new QComboBox(appearance);
    accent->addItems({"Ocean Blue", "Emerald", "Graphite"});
    auto* ui_scale = new QSpinBox(appearance);
    ui_scale->setRange(90, 150);
    ui_scale->setValue(100);
    ui_scale->setSuffix("%");
    app_form->addRow("Theme", theme);
    app_form->addRow("Accent", accent);
    app_form->addRow("UI scale", ui_scale);

    auto* focus_rings = new QCheckBox("Always show strong keyboard focus rings", appearance);
    focus_rings->setChecked(true);
    auto* reduce_motion = new QCheckBox("Reduce non-essential motion", appearance);
    appearance->BodyLayout()->addWidget(focus_rings);
    appearance->BodyLayout()->addWidget(reduce_motion);
    body->addWidget(appearance);

    auto* perf      = new CardFrame("Performance", pane);
    auto* perf_form = new QFormLayout();
    perf_form->setContentsMargins(0, 0, 0, 0);
    perf_form->setHorizontalSpacing(12);
    perf_form->setVerticalSpacing(10);
    perf->BodyLayout()->addLayout(perf_form);

    auto* decode_threads = new QSpinBox(perf);
    decode_threads->setRange(1, 16);
    decode_threads->setValue(8);
    auto* thumb_threads = new QSpinBox(perf);
    thumb_threads->setRange(1, 16);
    thumb_threads->setValue(6);
    auto* cache_gb = new QSpinBox(perf);
    cache_gb->setRange(1, 64);
    cache_gb->setValue(8);
    cache_gb->setSuffix(" GB");
    perf_form->addRow("Decode workers", decode_threads);
    perf_form->addRow("Thumbnail workers", thumb_threads);
    perf_form->addRow("Cache budget", cache_gb);

    auto* gpu_decode = new QCheckBox("Prefer GPU path when available", perf);
    gpu_decode->setChecked(true);
    auto* background_render = new QCheckBox("Allow background preview rendering", perf);
    background_render->setChecked(true);
    perf->BodyLayout()->addWidget(gpu_decode);
    perf->BodyLayout()->addWidget(background_render);
    body->addWidget(perf);

    auto* shortcuts = new CardFrame("Shortcuts", pane);
    auto* hints     = new QLabel(
        "Ctrl+I Import    Ctrl+E Export    Ctrl+F Focus Search    Ctrl+, Settings", shortcuts);
    hints->setObjectName("MetaText");
    shortcuts->BodyLayout()->addWidget(hints);
    body->addWidget(shortcuts);

    body->addStretch(1);
  }
};

class ExportDialog final : public QDialog {
 public:
  struct Item {
    sl_element_id_t sleeve_id_ = 0;
    image_id_t      image_id_  = 0;
  };

  ExportDialog(std::shared_ptr<ImagePoolService> image_pool,
               std::shared_ptr<ExportService> export_service, std::vector<Item> items,
               QWidget* parent = nullptr)
      : QDialog(parent),
        image_pool_(std::move(image_pool)),
        export_service_(std::move(export_service)),
        items_(std::move(items)) {
    if (!image_pool_ || !export_service_) {
      throw std::runtime_error("ExportDialog: missing services");
    }

    setModal(true);
    setWindowTitle("Export");
    resize(740, 560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(12);

    auto* intro = new QLabel(
        "Export selected images using the current edit pipeline. Processing runs in the "
        "background and updates progress for each finished image.",
        this);
    intro->setWordWrap(true);
    intro->setObjectName("MetaText");
    root->addWidget(intro);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(8);
    root->addWidget(split, 1);

    auto* options_card = new CardFrame("Format options", split);
    auto* form         = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    options_card->BodyLayout()->addLayout(form);

    auto* out_row = new QWidget(options_card);
    auto* out_h   = new QHBoxLayout(out_row);
    out_h->setContentsMargins(0, 0, 0, 0);
    out_h->setSpacing(8);

    out_dir_ = new QLineEdit(options_card);
    out_dir_->setPlaceholderText("Select output directory");
    const auto default_dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    out_dir_->setText(default_dir.isEmpty() ? QDir::currentPath() : default_dir);

    auto* browse = new QPushButton("Browse…", options_card);
    out_h->addWidget(out_dir_, 1);
    out_h->addWidget(browse, 0);
    form->addRow("Output", out_row);

    connect(browse, &QPushButton::clicked, this, [this]() {
      const auto picked =
          QFileDialog::getExistingDirectory(this, "Select export folder", out_dir_->text());
      if (!picked.isEmpty()) {
        out_dir_->setText(picked);
      }
    });

    format_ = new QComboBox(options_card);
    for (auto f : {ImageFormatType::JPEG, ImageFormatType::PNG, ImageFormatType::TIFF,
                   ImageFormatType::WEBP, ImageFormatType::EXR}) {
      format_->addItem(FormatName(f));
    }
    form->addRow("Format", format_);

    resize_enabled_ = new QComboBox(options_card);
    resize_enabled_->addItems({"No", "Yes"});
    form->addRow("Resize", resize_enabled_);

    max_side_ = new QSpinBox(options_card);
    max_side_->setRange(256, 16384);
    max_side_->setValue(4096);
    form->addRow("Max side", max_side_);

    quality_ = new QSpinBox(options_card);
    quality_->setRange(1, 100);
    quality_->setValue(95);
    form->addRow("Quality", quality_);

    bit_depth_ = new QComboBox(options_card);
    bit_depth_->addItems({"8", "16", "32"});
    bit_depth_->setCurrentText("16");
    form->addRow("Bit depth", bit_depth_);

    png_compress_ = new QSpinBox(options_card);
    png_compress_->setRange(0, 9);
    png_compress_->setValue(5);
    form->addRow("PNG level", png_compress_);

    tiff_compress_ = new QComboBox(options_card);
    tiff_compress_->addItems({"NONE", "LZW", "ZIP"});
    form->addRow("TIFF comp", tiff_compress_);

    option_widgets_  = {out_dir_,   format_,       resize_enabled_, max_side_, quality_,
                        bit_depth_, png_compress_, tiff_compress_,  browse};

    auto* queue_hint = new QLabel(
        QString("Queued %1 image(s). Select rows in the browser before opening export to limit "
                "the batch.")
            .arg(static_cast<int>(items_.size())),
        options_card);
    queue_hint->setObjectName("MetaText");
    queue_hint->setWordWrap(true);
    options_card->BodyLayout()->addWidget(queue_hint, 0);

    auto* progress_card = new CardFrame("Progress", split);
    status_             = new QLabel("Ready to export.", progress_card);
    status_->setWordWrap(true);
    progress_card->BodyLayout()->addWidget(status_, 0);

    progress_ = new QProgressBar(progress_card);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_card->BodyLayout()->addWidget(progress_, 0);

    queue_preview_ = new QListWidget(progress_card);
    queue_preview_->setSelectionMode(QAbstractItemView::NoSelection);
    queue_preview_->setMinimumHeight(220);
    const int preview_count = std::min<int>(12, static_cast<int>(items_.size()));
    for (int i = 0; i < preview_count; ++i) {
      queue_preview_->addItem(QString("Image #%1  Sleeve #%2")
                                  .arg(static_cast<qulonglong>(items_[i].image_id_))
                                  .arg(static_cast<qulonglong>(items_[i].sleeve_id_)));
    }
    if (items_.size() > static_cast<size_t>(preview_count)) {
      queue_preview_->addItem(
          QString("… and %1 more")
              .arg(static_cast<int>(items_.size()) - static_cast<int>(preview_count)));
    }
    progress_card->BodyLayout()->addWidget(queue_preview_, 1);

    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    export_btn_   = buttons->button(QDialogButtonBox::Ok);
    cancel_btn_   = buttons->button(QDialogButtonBox::Cancel);
    export_btn_->setText("Export");
    export_btn_->setObjectName("accent");
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() { StartExport(); });
  }

 private:
  ExportFormatOptions BuildOptionsForOne(const std::filesystem::path& src_path,
                                         sl_element_id_t sleeve_id, image_id_t image_id) const {
    ExportFormatOptions opt;

    const auto          fmt = FormatFromName(format_->currentText());
    opt.format_             = fmt;

    opt.resize_enabled_     = (resize_enabled_->currentIndex() == 1);
    opt.max_length_side_    = opt.resize_enabled_ ? max_side_->value() : 0;

    opt.quality_            = quality_->value();

    const auto bd           = bit_depth_->currentText().trimmed();
    if (bd == "8") {
      opt.bit_depth_ = ExportFormatOptions::BIT_DEPTH::BIT_8;
    } else if (bd == "32") {
      opt.bit_depth_ = ExportFormatOptions::BIT_DEPTH::BIT_32;
    } else {
      opt.bit_depth_ = ExportFormatOptions::BIT_DEPTH::BIT_16;
    }

    opt.compression_level_ = png_compress_->value();

    const auto tc          = tiff_compress_->currentText().trimmed().toUpper();
    if (tc == "LZW") {
      opt.tiff_compress_ = ExportFormatOptions::TIFF_COMPRESS::LZW;
    } else if (tc == "ZIP") {
      opt.tiff_compress_ = ExportFormatOptions::TIFF_COMPRESS::ZIP;
    } else {
      opt.tiff_compress_ = ExportFormatOptions::TIFF_COMPRESS::NONE;
    }

    std::filesystem::path out_dir(out_dir_->text().toStdWString());
    std::filesystem::path stem = src_path.stem();
    if (stem.empty()) {
      stem = std::filesystem::path("image");
    }
    const auto suffix = std::string("_") + std::to_string(static_cast<uint64_t>(sleeve_id)) + "_" +
                        std::to_string(static_cast<uint64_t>(image_id));
    const auto ext   = ExtensionForExportFormat(fmt);
    opt.export_path_ = out_dir / (stem.wstring() + std::wstring(suffix.begin(), suffix.end()) +
                                  std::wstring(ext.begin(), ext.end()));

    return opt;
  }

  void StartExport() {
    if (exporting_) {
      return;
    }
    if (items_.empty()) {
      status_->setText("Nothing to export.");
      return;
    }

    const auto out_dir = out_dir_->text().trimmed();
    if (out_dir.isEmpty()) {
      QMessageBox::warning(this, "Export", "Please select an output directory.");
      return;
    }

    export_service_->ClearAllExportTasks();
    size_t queued_count = 0;
    int    skipped_count = 0;
    QString first_error;
    for (const auto& it : items_) {
      try {
        const auto src_path = image_pool_->Read<std::filesystem::path>(
            it.image_id_, [](std::shared_ptr<Image> img) { return img->image_path_; });
        ExportTask task;
        task.sleeve_id_ = it.sleeve_id_;
        task.image_id_  = it.image_id_;
        task.options_   = BuildOptionsForOne(src_path, it.sleeve_id_, it.image_id_);
        export_service_->EnqueueExportTask(task);
        ++queued_count;
      } catch (const std::exception& e) {
        ++skipped_count;
        if (first_error.isEmpty()) {
          first_error = QString::fromUtf8(e.what());
        }
      } catch (...) {
        ++skipped_count;
        if (first_error.isEmpty()) {
          first_error = "Unknown error while preparing export task.";
        }
      }
    }

    if (queued_count == 0) {
      QString msg = "No valid export tasks could be created.";
      if (!first_error.isEmpty()) {
        msg += QString("\n\nFirst error: %1").arg(first_error);
      }
      QMessageBox::warning(this, "Export", msg);
      status_->setText("No export tasks were queued.");
      return;
    }

    QString busy_message = "Exporting. Rendering pipeline output and writing files...";
    if (skipped_count > 0) {
      busy_message = QString("Exporting %1 image(s). Skipped %2 invalid item(s).")
                         .arg(static_cast<int>(queued_count))
                         .arg(skipped_count);
    }
    SetBusy(true, busy_message);
    progress_->setRange(0, static_cast<int>(std::max<size_t>(queued_count, 1)));
    progress_->setValue(0);

    QPointer<ExportDialog> self(this);
    export_service_->ExportAll(
        [self](const ExportProgress& progress) {
          if (!self) {
            return;
          }
          QMetaObject::invokeMethod(
              self,
              [self, progress]() {
                if (!self) {
                  return;
                }
                bool is_stale = false;
                if (self->progress_) {
                  const int range_max = static_cast<int>(std::max<size_t>(progress.total_, 1));
                  const int completed =
                      static_cast<int>(std::min(progress.completed_, progress.total_));
                  self->progress_->setRange(
                      0, range_max);
                  is_stale = completed < self->progress_->value();
                  if (!is_stale) {
                    self->progress_->setValue(completed);
                  }
                }
                if (is_stale) {
                  return;
                }
                if (self->status_) {
                  self->status_->setText(
                      QString("Exporting... processed %1/%2, written %3, failed %4.")
                          .arg(static_cast<qulonglong>(progress.completed_))
                          .arg(static_cast<qulonglong>(progress.total_))
                          .arg(static_cast<qulonglong>(progress.succeeded_))
                          .arg(static_cast<qulonglong>(progress.failed_)));
                }
              },
              Qt::QueuedConnection);
        },
        [self, skipped_count](std::shared_ptr<std::vector<ExportResult>> results) {
          if (!self) {
            return;
          }
          QMetaObject::invokeMethod(
              self,
              [self, results, skipped_count]() {
                if (!self) {
                  return;
                }
                self->SetBusy(false, "Export complete.");
                self->progress_->setRange(0, 100);
                self->progress_->setValue(100);
                int         ok   = 0;
                int         fail = 0;
                QStringList errors;
                if (results) {
                  for (const auto& r : *results) {
                    if (r.success_) {
                      ok++;
                    } else {
                      fail++;
                      if (!r.message_.empty()) {
                        errors << QString::fromUtf8(r.message_.c_str());
                      }
                    }
                  }
                }

                const int total = ok + fail;
                if (self->status_) {
                  self->status_->setText(
                      QString("Export complete. Written %1/%2 image(s), failed %3.")
                          .arg(ok)
                          .arg(total)
                          .arg(fail));
                }

                if (fail == 0) {
                  QString info = QString("Done: %1 file(s)").arg(ok);
                  if (skipped_count > 0) {
                    info += QString("\nSkipped %1 invalid item(s) before export.")
                                .arg(skipped_count);
                  }
                  QMessageBox::information(self, "Export", info);
                } else {
                  QString warning =
                      QString("Done: %1 ok, %2 failed").arg(ok).arg(fail);
                  if (skipped_count > 0) {
                    warning += QString(", %1 skipped").arg(skipped_count);
                  }
                  if (!errors.isEmpty()) {
                    warning += QString("\n\n%1").arg(errors.join("\n"));
                  }
                  QMessageBox::warning(self, "Export", warning);
                }

                self->accept();
              },
              Qt::QueuedConnection);
        });
  }

  void SetBusy(bool busy, const QString& message) {
    exporting_ = busy;
    for (auto* widget : option_widgets_) {
      if (widget) {
        widget->setEnabled(!busy);
      }
    }
    if (queue_preview_) {
      queue_preview_->setEnabled(!busy);
    }
    if (export_btn_) {
      export_btn_->setEnabled(!busy);
    }
    if (cancel_btn_) {
      cancel_btn_->setEnabled(!busy);
    }
    if (status_) {
      status_->setText(message);
    }
  }

  std::shared_ptr<ImagePoolService> image_pool_;
  std::shared_ptr<ExportService>    export_service_;
  std::vector<Item>                 items_;

  QLineEdit*                        out_dir_        = nullptr;
  QComboBox*                        format_         = nullptr;
  QComboBox*                        resize_enabled_ = nullptr;
  QSpinBox*                         max_side_       = nullptr;
  QSpinBox*                         quality_        = nullptr;
  QComboBox*                        bit_depth_      = nullptr;
  QSpinBox*                         png_compress_   = nullptr;
  QComboBox*                        tiff_compress_  = nullptr;
  QLabel*                           status_         = nullptr;
  QProgressBar*                     progress_       = nullptr;
  QListWidget*                      queue_preview_  = nullptr;
  QPushButton*                      export_btn_     = nullptr;
  QPushButton*                      cancel_btn_     = nullptr;
  std::vector<QWidget*>             option_widgets_;
  bool                              exporting_ = false;
};

[[maybe_unused]] static void SetPipelineTemplateToGlobalParams(
    std::shared_ptr<PipelineExecutor> executor) {
  auto&          global_params = executor->GetGlobalParams();

  nlohmann::json to_ws_params;
  to_ws_params["ocio"] = {
      {"src", "ACES2065-1"}, {"dst", "ACEScc"}, {"normalize", true}, {"transform_type", 0}};
  (void)executor->GetStage(PipelineStageName::Basic_Adjustment);
  // basic.SetOperator(OperatorType::TO_WS, to_ws_params, global_params);

  nlohmann::json output_params;
  auto&          output_stage = executor->GetStage(PipelineStageName::Output_Transform);
  output_params["ocio"]       = {
      {"src", "ACEScc"}, {"dst", "Camera Rec.709"}, {"limit", true}, {"transform_type", 1}};
  output_params.update(pipeline_defaults::MakeDefaultODTParams());
  output_stage.SetOperator(OperatorType::ODT, output_params, global_params);
}

class SpinnerWidget final : public QWidget {
 public:
  explicit SpinnerWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setFixedSize(22, 22);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);

    timer_ = new QTimer(this);
    timer_->setInterval(16);
    QObject::connect(timer_, &QTimer::timeout, this, [this]() {
      angle_deg_ = (angle_deg_ + 18) % 360;
      update();
    });
    hide();
  }

  void Start() {
    show();
    raise();
    if (!timer_->isActive()) {
      timer_->start();
    }
  }

  void Stop() {
    if (timer_->isActive()) {
      timer_->stop();
    }
    hide();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(2.5, 2.5, width() - 5.0, height() - 5.0);

    // Subtle background ring.
    {
      QPen pen(QColor(0x30, 0x31, 0x34, 200));
      pen.setWidthF(2.0);
      pen.setCapStyle(Qt::RoundCap);
      painter.setPen(pen);
      painter.drawArc(r, 0 * 16, 360 * 16);
    }

    // Foreground arc.
    {
      QPen pen(QColor(0x8a, 0xb4, 0xf8, 230));
      pen.setWidthF(2.2);
      pen.setCapStyle(Qt::RoundCap);
      painter.setPen(pen);
      painter.drawArc(r, (90 - angle_deg_) * 16, 100 * 16);
    }
  }

 private:
  QTimer* timer_     = nullptr;
  int     angle_deg_ = 0;
};

class HistoryLaneWidget final : public QWidget {
 public:
  HistoryLaneWidget(QColor dot, QColor line, bool draw_top, bool draw_bottom,
                    QWidget* parent = nullptr)
      : QWidget(parent),
        dot_(std::move(dot)),
        line_(std::move(line)),
        draw_top_(draw_top),
        draw_bottom_(draw_bottom) {
    setFixedWidth(18);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }

  void SetConnectors(bool draw_top, bool draw_bottom) {
    draw_top_    = draw_top;
    draw_bottom_ = draw_bottom;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int cx = width() / 2;
    const int cy = height() / 2;

    // Vertical lane.
    {
      QPen pen(line_);
      pen.setWidthF(2.0);
      pen.setCapStyle(Qt::RoundCap);
      p.setPen(pen);

      if (draw_top_) {
        p.drawLine(QPointF(cx, 2.0), QPointF(cx, cy - 6.0));
      }
      if (draw_bottom_) {
        p.drawLine(QPointF(cx, cy + 6.0), QPointF(cx, height() - 2.0));
      }
    }

    // Node.
    {
      p.setPen(Qt::NoPen);
      p.setBrush(dot_);
      p.drawEllipse(QPointF(cx, cy), 4.4, 4.4);
      p.setBrush(QColor(0x12, 0x12, 0x12));
      p.drawEllipse(QPointF(cx, cy), 2.0, 2.0);
    }
  }

 private:
  QColor dot_;
  QColor line_;
  bool   draw_top_    = false;
  bool   draw_bottom_ = false;
};

class HistoryCardWidget final : public QFrame {
 public:
  explicit HistoryCardWidget(QWidget* parent = nullptr) : QFrame(parent) {
    setObjectName("HistoryCard");
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setProperty("selected", false);

    setStyleSheet(
        "QFrame#HistoryCard {"
        "  background: #16181A;"
        "  border: 1px solid #303134;"
        "  border-radius: 10px;"
        "}"
        "QFrame#HistoryCard:hover {"
        "  background: #1E2124;"
        "}"
        "QFrame#HistoryCard[selected=\"true\"] {"
        "  background: rgba(138, 180, 248, 0.14);"
        "  border: 1px solid rgba(138, 180, 248, 0.55);"
        "}");
  }

  void SetSelected(bool selected) {
    if (property("selected").toBool() == selected) {
      return;
    }
    setProperty("selected", selected);
    style()->unpolish(this);
    style()->polish(this);
    update();
  }
};

static QLabel* MakePillLabel(const QString& text, const QString& fg, const QString& bg,
                             const QString& border, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setStyleSheet(QString("QLabel {"
                           "  color: %1;"
                           "  background: %2;"
                           "  border: 1px solid %3;"
                           "  border-radius: 10px;"
                           "  padding: 1px 7px;"
                           "  font-size: 11px;"
                           "}")
                       .arg(fg, bg, border));
  return l;
}

class EditorDialog final : public QDialog {
 public:
  enum class WorkingMode : int { Incremental = 0, Plain = 1 };

  EditorDialog(std::shared_ptr<ImagePoolService>       image_pool,
               std::shared_ptr<PipelineGuard>          pipeline_guard,
               std::shared_ptr<EditHistoryMgmtService> history_service,
               std::shared_ptr<EditHistoryGuard> history_guard, sl_element_id_t element_id,
               image_id_t image_id, QWidget* parent = nullptr)
      : QDialog(parent),
        image_pool_(std::move(image_pool)),
        pipeline_guard_(std::move(pipeline_guard)),
        history_service_(std::move(history_service)),
        history_guard_(std::move(history_guard)),
        element_id_(element_id),
        image_id_(image_id),
        scheduler_(RenderService::GetPreviewScheduler()) {
    if (!image_pool_ || !pipeline_guard_ || !pipeline_guard_->pipeline_ || !history_service_ ||
        !history_guard_ || !history_guard_->history_ || !scheduler_) {
      throw std::runtime_error("EditorDialog: missing services");
    }

    setModal(true);
    setSizeGripEnabled(true);
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    setWindowFlag(Qt::MSWindowsFixedSizeDialogHint, false);
    setWindowTitle(QString("Editor - element #%1").arg(static_cast<qulonglong>(element_id_)));
    setMinimumSize(1080, 680);
    resize(1500, 1000);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(12);

    viewer_ = new QtEditViewer(this);
    viewer_->setMinimumSize(560, 360);
    viewer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    viewer_->setStyleSheet(
        "QOpenGLWidget {"
        "  background: #121212;"
        "  border: 1px solid #303134;"
        "  border-radius: 12px;"
        "}");

    // Viewer container allows a small overlay spinner during rendering.
    viewer_container_ = new QWidget(this);
    auto* viewer_grid = new QGridLayout(viewer_container_);
    viewer_grid->setContentsMargins(0, 0, 0, 0);
    viewer_grid->setSpacing(0);
    viewer_grid->addWidget(viewer_, 0, 0);

    spinner_ = new SpinnerWidget(viewer_container_);
    viewer_grid->addWidget(spinner_, 0, 0, Qt::AlignRight | Qt::AlignBottom);
    viewer_grid->setRowStretch(0, 1);
    viewer_grid->setColumnStretch(0, 1);

    controls_scroll_ = new QScrollArea(this);
    controls_scroll_->setFrameShape(QFrame::NoFrame);
    controls_scroll_->setWidgetResizable(true);
    controls_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    controls_scroll_->setMinimumWidth(380);
    controls_scroll_->setMaximumWidth(540);
    controls_scroll_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    controls_scroll_->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical {"
        "  background: #101822;"
        "  width: 10px;"
        "  margin: 2px;"
        "  border-radius: 5px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #2E415A;"
        "  border-radius: 5px;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; "
        "}");

    controls_             = new QWidget(controls_scroll_);
    auto* controls_layout = new QVBoxLayout(controls_);
    controls_layout->setContentsMargins(16, 16, 16, 16);
    controls_layout->setSpacing(12);
    controls_->setMinimumWidth(380);
    controls_->setObjectName("EditorControlsPanel");
    controls_->setStyleSheet(
        "#EditorControlsPanel {"
        "  background: #161E2A;"
        "  border: 1px solid #2C3D55;"
        "  border-radius: 14px;"
        "}"
        "#EditorControlsPanel QFrame#EditorSection {"
        "  background: #111923;"
        "  border: 1px solid #2B3B51;"
        "  border-radius: 12px;"
        "}"
        "#EditorControlsPanel QLabel#EditorSectionTitle {"
        "  color: #EAF2FF;"
        "  font-size: 13px;"
        "  font-weight: 620;"
        "}"
        "#EditorControlsPanel QLabel#EditorSectionSub {"
        "  color: #9DB0D0;"
        "  font-size: 11px;"
        "}");
    controls_scroll_->setWidget(controls_);

    root->addWidget(viewer_container_, 1);
    root->addWidget(controls_scroll_, 0);

    auto* controls_header = new QLabel("Adjustments", controls_);
    controls_header->setObjectName("SectionTitle");
    auto* controls_sub = new QLabel("Editing controls are scoped to the active image.", controls_);
    controls_sub->setObjectName("MetaText");
    controls_sub->setWordWrap(true);
    controls_layout->addWidget(controls_header, 0);
    controls_layout->addWidget(controls_sub, 0);

    // Prefer LUTs next to the executable (installed layout), fall back to source tree.
    const auto app_luts_dir =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) / "LUTs";
    const auto src_luts_dir = std::filesystem::path(CONFIG_PATH) / "LUTs";
    const auto luts_dir = std::filesystem::is_directory(app_luts_dir) ? app_luts_dir : src_luts_dir;
    const auto lut_files = ListCubeLutsInDir(luts_dir);

    lut_paths_.push_back("");  // index 0 => None
    lut_names_.push_back("None");
    for (const auto& p : lut_files) {
      lut_paths_.push_back(p.generic_string());
      lut_names_.push_back(QString::fromStdString(p.filename().string()));
    }

    auto addSection = [&](const QString& title, const QString& subtitle) {
      auto* frame = new QFrame(controls_);
      frame->setObjectName("EditorSection");
      auto* v = new QVBoxLayout(frame);
      v->setContentsMargins(12, 10, 12, 10);
      v->setSpacing(2);

      auto* t = new QLabel(title, frame);
      t->setObjectName("EditorSectionTitle");
      auto* s = new QLabel(subtitle, frame);
      s->setObjectName("EditorSectionSub");
      s->setWordWrap(true);
      v->addWidget(t, 0);
      v->addWidget(s, 0);
      controls_layout->addWidget(frame, 0);
    };

    controls_layout->addStretch();

    int default_lut_index = 0;
    for (int i = 1; i < static_cast<int>(lut_paths_.size()); ++i) {
      if (std::filesystem::path(lut_paths_[i]).filename() == "5207.cube") {
        default_lut_index = i;
        break;
      }
    }

    // If the pipeline already has operator params (loaded from PipelineMapper/storage),
    // initialize UI state from those params rather than overwriting them.
    const bool loaded_state_from_pipeline = LoadStateFromPipelineIfPresent();
    if (!loaded_state_from_pipeline) {
      // Demo-friendly default: apply a LUT only for brand-new pipelines with no saved params.
      state_.lut_path_ = lut_paths_[default_lut_index];
    }
    committed_state_ = state_;

    // Seed a working version from the latest committed one (if any).
    try {
      const auto parent_id = history_guard_->history_->GetLatestVersion().ver_ref_.GetVersionID();
      working_version_     = Version(element_id_, parent_id);
    } catch (...) {
      working_version_ = Version(element_id_);
    }
    working_version_.SetBasePipelineExecutor(pipeline_guard_->pipeline_);

    int initial_lut_index = 0;
    if (!state_.lut_path_.empty()) {
      auto it = std::find(lut_paths_.begin(), lut_paths_.end(), state_.lut_path_);
      if (it != lut_paths_.end()) {
        initial_lut_index = static_cast<int>(std::distance(lut_paths_.begin(), it));
      } else {
        // Keep UI consistent even if LUT path comes from an external/custom location.
        lut_paths_.push_back(state_.lut_path_);
        lut_names_.push_back(
            QString::fromStdString(std::filesystem::path(state_.lut_path_).filename().string()));
        initial_lut_index = static_cast<int>(lut_paths_.size() - 1);
      }
    }

    auto addComboBox = [&](const QString& name, const QStringList& items, int initial_index,
                           auto&& onChange) {
      auto* label = new QLabel(name, controls_);
      label->setStyleSheet(
          "QLabel {"
          "  color: #E8EAED;"
          "  font-size: 14px;"
          "  font-weight: 400;"
          "}");

      auto* combo = new QComboBox(controls_);
      combo->addItems(items);
      combo->setCurrentIndex(initial_index);
      combo->setMinimumWidth(240);
      combo->setFixedHeight(32);
      combo->setStyleSheet(
          "QComboBox {"
          "  background: #202124;"
          "  border: 1px solid #303134;"
          "  border-radius: 8px;"
          "  padding: 4px 8px;"
          "}"
          "QComboBox::drop-down {"
          "  border: 0px;"
          "  width: 24px;"
          "}"
          "QComboBox QAbstractItemView {"
          "  background: #202124;"
          "  border: 1px solid #303134;"
          "  selection-background-color: #8ab4f8;"
          "  selection-color: #080A0C;"
          "}"
          "QComboBox QAbstractItemView::item:hover {"
          "  background: #2B2F33;"
          "  color: #E8EAED;"
          "}"
          "QComboBox QAbstractItemView::item:selected {"
          "  background: #8ab4f8;"
          "  color: #080A0C;"
          "}");

      QObject::connect(
          combo, QOverload<int>::of(&QComboBox::currentIndexChanged), controls_,
          [onChange = std::forward<decltype(onChange)>(onChange)](int idx) { onChange(idx); });

      auto* row       = new QWidget(controls_);
      auto* rowLayout = new QHBoxLayout(row);
      rowLayout->setContentsMargins(0, 0, 0, 0);
      rowLayout->addWidget(label, /*stretch*/ 1);
      rowLayout->addWidget(combo);

      controls_layout->insertWidget(controls_layout->count() - 1, row);
      return combo;
    };

    auto addSlider = [&](const QString& name, int min, int max, int value, auto&& onChange,
                         auto&& onRelease, auto&& formatter) {
      auto* info = new QLabel(QString("%1: %2").arg(name).arg(formatter(value)), controls_);
      info->setStyleSheet(
          "QLabel {"
          "  color: #E8EAED;"
          "  font-size: 14px;"
          "  font-weight: 400;"
          "}");

      auto* slider = new QSlider(Qt::Horizontal, controls_);
      slider->setRange(min, max);
      slider->setValue(value);
      slider->setSingleStep(1);
      slider->setPageStep(std::max(1, (max - min) / 20));
      slider->setMinimumWidth(240);
      slider->setFixedHeight(32);

      QObject::connect(
          slider, &QSlider::valueChanged, controls_,
          [info, name, formatter, onChange = std::forward<decltype(onChange)>(onChange)](int v) {
            info->setText(QString("%1: %2").arg(name).arg(formatter(v)));
            onChange(v);
          });

      QObject::connect(
          slider, &QSlider::sliderReleased, controls_,
          [onRelease = std::forward<decltype(onRelease)>(onRelease)]() { onRelease(); });

      auto* row       = new QWidget(controls_);
      auto* rowLayout = new QHBoxLayout(row);
      rowLayout->setContentsMargins(0, 0, 0, 0);
      rowLayout->addWidget(info, /*stretch*/ 1);
      rowLayout->addWidget(slider);

      controls_layout->insertWidget(controls_layout->count() - 1, row);
      return slider;
    };

    addSection("Pipeline", "Choose LUT and render behavior.");

    lut_combo_ = addComboBox("LUT", lut_names_, initial_lut_index, [&](int idx) {
      if (idx < 0 || idx >= static_cast<int>(lut_paths_.size())) {
        return;
      }
      state_.lut_path_ = lut_paths_[idx];
      CommitAdjustment(AdjustmentField::Lut);
    });

    addSection("Tone", "Primary tonal shaping controls.");

    exposure_slider_ = addSlider(
        "Exposure", -1000, 1000, static_cast<int>(std::lround(state_.exposure_ * 100.0f)),
        [&](int v) {
          state_.exposure_ = static_cast<float>(v) / 100.0f;
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Exposure); },
        [](int v) { return QString::number(v / 100.0, 'f', 2); });

    contrast_slider_ = addSlider(
        "Contrast", -100, 100, static_cast<int>(std::lround(state_.contrast_)),
        [&](int v) {
          state_.contrast_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Contrast); },
        [](int v) { return QString::number(v, 'f', 2); });

    addSection("Color", "Color balance and saturation.");

    saturation_slider_ = addSlider(
        "Saturation", -100, 100, static_cast<int>(std::lround(state_.saturation_)),
        [&](int v) {
          state_.saturation_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Saturation); },
        [](int v) { return QString::number(v, 'f', 2); });

    vibrance_slider_ = addSlider(
        "Vibrance", -100, 100, static_cast<int>(std::lround(state_.vibrance_)),
        [&](int v) {
          state_.vibrance_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Vibrance); },
        [](int v) { return QString::number(v, 'f', 2); });

    tint_slider_ = addSlider(
        "Tint", -100, 100, static_cast<int>(std::lround(state_.tint_)),
        [&](int v) {
          state_.tint_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Tint); },
        [](int v) { return QString::number(v, 'f', 2); });

    blacks_slider_ = addSlider(
        "Blacks", -100, 100, static_cast<int>(std::lround(state_.blacks_)),
        [&](int v) {
          state_.blacks_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Blacks); },
        [](int v) { return QString::number(v, 'f', 2); });

    whites_slider_ = addSlider(
        "Whites", -100, 100, static_cast<int>(std::lround(state_.whites_)),
        [&](int v) {
          state_.whites_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Whites); },
        [](int v) { return QString::number(v, 'f', 2); });

    shadows_slider_ = addSlider(
        "Shadows", -100, 100, static_cast<int>(std::lround(state_.shadows_)),
        [&](int v) {
          state_.shadows_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Shadows); },
        [](int v) { return QString::number(v, 'f', 2); });

    highlights_slider_ = addSlider(
        "Highlights", -100, 100, static_cast<int>(std::lround(state_.highlights_)),
        [&](int v) {
          state_.highlights_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Highlights); },
        [](int v) { return QString::number(v, 'f', 2); });

    addSection("Detail", "Micro-contrast and sharpen controls.");

    sharpen_slider_ = addSlider(
        "Sharpen", -100, 100, static_cast<int>(std::lround(state_.sharpen_)),
        [&](int v) {
          state_.sharpen_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Sharpen); },
        [](int v) { return QString::number(v, 'f', 2); });

    clarity_slider_ = addSlider(
        "Clarity", -100, 100, static_cast<int>(std::lround(state_.clarity_)),
        [&](int v) {
          state_.clarity_ = static_cast<float>(v);
          RequestRender();
        },
        [this]() { CommitAdjustment(AdjustmentField::Clarity); },
        [](int v) { return QString::number(v, 'f', 2); });

    addSection("Versioning", "Commit and inspect edit history.");

    // Edit-history commit controls.
    {
      auto* row       = new QWidget(controls_);
      auto* rowLayout = new QHBoxLayout(row);
      rowLayout->setContentsMargins(0, 0, 0, 0);
      rowLayout->setSpacing(10);

      version_status_ = new QLabel(row);
      version_status_->setStyleSheet(
          "QLabel {"
          "  color: #AAB0B6;"
          "  font-size: 12px;"
          "}");

      commit_version_btn_ = new QPushButton("Commit Version", row);
      commit_version_btn_->setFixedHeight(32);
      commit_version_btn_->setStyleSheet(
          "QPushButton {"
          "  background: #2B2F33;"
          "  border: 1px solid #303134;"
          "  border-radius: 8px;"
          "  padding: 6px 10px;"
          "}"
          "QPushButton:hover {"
          "  background: #34383C;"
          "}"
          "QPushButton:disabled {"
          "  color: #6B7075;"
          "  background: #1E1E1E;"
          "}");

      rowLayout->addWidget(version_status_, /*stretch*/ 1);
      rowLayout->addWidget(commit_version_btn_, /*stretch*/ 0);
      controls_layout->insertWidget(controls_layout->count() - 1, row);

      QObject::connect(commit_version_btn_, &QPushButton::clicked, this,
                       [this]() { CommitWorkingVersion(); });
    }

    // Edit-history visualization ("git log"-like) + working version mode.
    {
      auto* frame = new QFrame(controls_);
      frame->setStyleSheet(
          "QFrame {"
          "  background: transparent;"
          "  border: 1px solid #303134;"
          "  border-radius: 12px;"
          "}");

      auto* layout = new QVBoxLayout(frame);
      layout->setContentsMargins(10, 10, 10, 10);
      layout->setSpacing(8);

      auto* mode_row    = new QWidget(frame);
      auto* mode_layout = new QHBoxLayout(mode_row);
      mode_layout->setContentsMargins(0, 0, 0, 0);
      mode_layout->setSpacing(8);

      auto* mode_label = new QLabel("Working version:", mode_row);
      mode_label->setStyleSheet(
          "QLabel {"
          "  color: #AAB0B6;"
          "  font-size: 12px;"
          "}");

      working_mode_combo_ = new QComboBox(mode_row);
      working_mode_combo_->addItem("Incremental (from latest)",
                                   static_cast<int>(WorkingMode::Incremental));
      working_mode_combo_->addItem("Plain (no parent)", static_cast<int>(WorkingMode::Plain));
      working_mode_combo_->setFixedHeight(28);
      working_mode_combo_->setStyleSheet(
          "QComboBox {"
          "  background: #202124;"
          "  border: 1px solid #303134;"
          "  border-radius: 8px;"
          "  padding: 4px 8px;"
          "}"
          "QComboBox::drop-down {"
          "  border: 0px;"
          "  width: 24px;"
          "}"
          "QComboBox QAbstractItemView {"
          "  background: #202124;"
          "  border: 1px solid #303134;"
          "  selection-background-color: #8ab4f8;"
          "  selection-color: #080A0C;"
          "}");

      new_working_btn_ = new QPushButton("New", mode_row);
      new_working_btn_->setFixedHeight(28);
      new_working_btn_->setStyleSheet(
          "QPushButton {"
          "  background: #2B2F33;"
          "  border: 1px solid #303134;"
          "  border-radius: 8px;"
          "  padding: 4px 10px;"
          "}"
          "QPushButton:hover {"
          "  background: #34383C;"
          "}");

      mode_layout->addWidget(mode_label, /*stretch*/ 0);
      mode_layout->addWidget(working_mode_combo_, /*stretch*/ 1);
      mode_layout->addWidget(new_working_btn_, /*stretch*/ 0);

      layout->addWidget(mode_row);

      auto* versions_label = new QLabel("Versions", frame);
      versions_label->setStyleSheet(
          "QLabel {"
          "  color: #E8EAED;"
          "  font-size: 12px;"
          "  font-weight: 500;"
          "}");
      layout->addWidget(versions_label);

      version_log_ = new QListWidget(frame);
      version_log_->setSelectionMode(QAbstractItemView::SingleSelection);
      version_log_->setSpacing(6);
      version_log_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
      version_log_->setMinimumHeight(150);
      version_log_->setStyleSheet(
          "QListWidget {"
          "  background: #121212;"
          "  border: 1px solid #303134;"
          "  border-radius: 10px;"
          "  padding: 6px;"
          "}"
          "QListWidget::item {"
          "  padding: 2px;"
          "}"
          "QListWidget::item:selected {"
          "  background: transparent;"
          "}");
      layout->addWidget(version_log_);

      QObject::connect(version_log_, &QListWidget::itemSelectionChanged, this,
                       [this]() { RefreshVersionLogSelectionStyles(); });

      auto* tx_label = new QLabel("Uncommitted transactions (stack)", frame);
      tx_label->setStyleSheet(
          "QLabel {"
          "  color: #E8EAED;"
          "  font-size: 12px;"
          "  font-weight: 500;"
          "}");
      layout->addWidget(tx_label);

      tx_stack_ = new QListWidget(frame);
      tx_stack_->setSelectionMode(QAbstractItemView::NoSelection);
      tx_stack_->setSpacing(6);
      tx_stack_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
      tx_stack_->setMinimumHeight(170);
      tx_stack_->setStyleSheet(
          "QListWidget {"
          "  background: #121212;"
          "  border: 1px solid #303134;"
          "  border-radius: 10px;"
          "  padding: 6px;"
          "}"
          "QListWidget::item {"
          "  padding: 2px;"
          "}");
      layout->addWidget(tx_stack_, /*stretch*/ 1);

      controls_layout->insertWidget(controls_layout->count() - 1, frame);

      QObject::connect(new_working_btn_, &QPushButton::clicked, this,
                       [this]() { StartNewWorkingVersionFromUi(); });
      QObject::connect(working_mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                       this, [this](int) { UpdateVersionUi(); });
    }

    UpdateVersionUi();

    SetupPipeline();

    // Requirement (3): init state render via singleShot.
    QTimer::singleShot(0, this, [this]() {
      state_.type_ = RenderType::FULL_RES_PREVIEW;
      RequestRender();
      state_.type_ = RenderType::FAST_PREVIEW;
    });
  }

 private:
  enum class AdjustmentField {
    Exposure,
    Contrast,
    Saturation,
    Vibrance,
    Tint,
    Blacks,
    Whites,
    Shadows,
    Highlights,
    Sharpen,
    Clarity,
    Lut,
  };

  struct AdjustmentState {
    float       exposure_   = 2.0f;
    float       contrast_   = 0.0f;
    float       saturation_ = 0.0f;
    float       vibrance_   = 0.0f;
    float       tint_       = 0.0f;
    float       blacks_     = 0.0f;
    float       whites_     = 0.0f;
    float       shadows_    = 0.0f;
    float       highlights_ = 0.0f;
    float       sharpen_    = 0.0f;
    float       clarity_    = 0.0f;
    std::string lut_path_;
    RenderType  type_ = RenderType::FAST_PREVIEW;
  };

  static bool NearlyEqual(float a, float b) { return std::abs(a - b) <= 1e-6f; }

  void        RefreshVersionLogSelectionStyles() {
    if (!version_log_) {
      return;
    }
    for (int i = 0; i < version_log_->count(); ++i) {
      auto* item = version_log_->item(i);
      if (!item) {
        continue;
      }
      auto* w = version_log_->itemWidget(item);
      if (!w) {
        continue;
      }
      if (auto* card = dynamic_cast<HistoryCardWidget*>(w)) {
        card->SetSelected(item->isSelected());
      }
    }
  }

  void UpdateVersionUi() {
    if (!version_status_ || !commit_version_btn_) {
      return;
    }

    const size_t tx_count = working_version_.GetAllEditTransactions().size();
    QString      label    = QString("Uncommitted: %1 tx").arg(static_cast<qulonglong>(tx_count));

    if (working_version_.HasParentVersion()) {
      label += QString(" • parent: %1")
                   .arg(QString::fromStdString(
                       working_version_.GetParentVersionID().ToString().substr(0, 8)));
    } else {
      label += " • plain";
    }

    if (working_mode_combo_) {
      const auto mode = static_cast<WorkingMode>(working_mode_combo_->currentData().toInt());
      label += (mode == WorkingMode::Plain) ? " • mode: plain" : " • mode: incremental";
    }

    if (history_guard_ && history_guard_->history_) {
      try {
        const auto latest_id = history_guard_->history_->GetLatestVersion().ver_ref_.GetVersionID();
        label +=
            QString(" • Latest: %1").arg(QString::fromStdString(latest_id.ToString().substr(0, 8)));
      } catch (...) {
      }
    }

    version_status_->setText(label);
    commit_version_btn_->setEnabled(tx_count > 0);

    if (tx_stack_) {
      tx_stack_->clear();
      const auto&  txs   = working_version_.GetAllEditTransactions();
      const size_t total = txs.size();
      size_t       i     = 0;
      for (const auto& tx : txs) {
        const QString title = QString::fromStdString(tx.Describe(true, 110));

        auto*         item  = new QListWidgetItem(tx_stack_);
        item->setToolTip(QString::fromStdString(tx.ToJSON().dump(2)));
        item->setSizeHint(QSize(0, 58));

        auto* card = new HistoryCardWidget(tx_stack_);
        auto* row  = new QHBoxLayout(card);
        row->setContentsMargins(10, 8, 10, 8);
        row->setSpacing(10);

        const QColor dot  = QColor(0xF2, 0xC0, 0x5C);
        const QColor line = QColor(0x30, 0x31, 0x34);
        auto*        lane = new HistoryLaneWidget(dot, line, /*draw_top*/ i > 0,
                                                  /*draw_bottom*/ (i + 1) < total, card);
        row->addWidget(lane, 0);

        auto* body = new QVBoxLayout();
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(2);

        auto* title_l = new QLabel(title, card);
        title_l->setWordWrap(true);
        title_l->setStyleSheet(
            "QLabel {"
            "  color: #E8EAED;"
            "  font-size: 12px;"
            "  font-weight: 500;"
            "}");

        auto* meta_l =
            new QLabel(QString("uncommitted • #%1").arg(static_cast<qulonglong>(i + 1)), card);
        meta_l->setStyleSheet(
            "QLabel {"
            "  color: #AAB0B6;"
            "  font-size: 11px;"
            "}");

        body->addWidget(title_l);
        body->addWidget(meta_l);
        row->addLayout(body, 1);

        tx_stack_->setItemWidget(item, card);
        ++i;
      }
    }

    if (version_log_) {
      QString prev_selected_id;
      if (auto* cur = version_log_->currentItem()) {
        prev_selected_id = cur->data(Qt::UserRole).toString();
      }

      version_log_->clear();
      if (history_guard_ && history_guard_->history_) {
        const auto& tree = history_guard_->history_->GetCommitTree();
        Hash128     latest_id{};
        try {
          latest_id = history_guard_->history_->GetLatestVersion().ver_ref_.GetVersionID();
        } catch (...) {
        }

        const Hash128 base_parent = working_version_.GetParentVersionID();

        int           row_index   = 0;
        const int     total_rows  = static_cast<int>(tree.size());

        for (auto it = tree.rbegin(); it != tree.rend(); ++it, ++row_index) {
          const auto& ver      = it->ver_ref_;
          const auto  ver_id   = ver.GetVersionID();
          const auto  short_id = QString::fromStdString(ver_id.ToString().substr(0, 8));
          const auto  when =
              QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ver.GetLastModifiedTime()))
                  .toString("yyyy-MM-dd HH:mm:ss");
          const auto committed_tx_count =
              static_cast<qulonglong>(ver.GetAllEditTransactions().size());

          QString     msg;
          const auto& txs = ver.GetAllEditTransactions();
          if (!txs.empty()) {
            msg = QString::fromStdString(txs.front().Describe(true, 70));
          } else {
            msg = "(empty)";
          }

          const bool is_head  = (ver_id == latest_id);
          const bool is_base  = (base_parent == ver_id && working_version_.HasParentVersion());
          const bool is_plain = !ver.HasParentVersion();

          auto*      item     = new QListWidgetItem(version_log_);
          item->setData(Qt::UserRole, QString::fromStdString(ver_id.ToString()));
          item->setToolTip(QString("version=%1\nparent=%2\ntx=%3")
                               .arg(QString::fromStdString(ver_id.ToString()))
                               .arg(QString::fromStdString(ver.GetParentVersionID().ToString()))
                               .arg(committed_tx_count));
          item->setSizeHint(QSize(0, 74));

          auto* card = new HistoryCardWidget(version_log_);
          auto* row  = new QHBoxLayout(card);
          row->setContentsMargins(10, 9, 10, 9);
          row->setSpacing(10);

          const QColor dot  = is_head
                                  ? QColor(0x8a, 0xb4, 0xf8)
                                  : (is_base ? QColor(0x81, 0xC9, 0x95) : QColor(0x9A, 0x9E, 0xA3));
          const QColor line = QColor(0x30, 0x31, 0x34);
          auto*        lane = new HistoryLaneWidget(dot, line, /*draw_top*/ row_index > 0,
                                                    /*draw_bottom*/ (row_index + 1) < total_rows, card);
          row->addWidget(lane, 0);

          auto* body = new QVBoxLayout();
          body->setContentsMargins(0, 0, 0, 0);
          body->setSpacing(4);

          auto* top = new QHBoxLayout();
          top->setContentsMargins(0, 0, 0, 0);
          top->setSpacing(8);

          const QFont mono   = QFontDatabase::systemFont(QFontDatabase::FixedFont);
          auto*       hash_l = new QLabel(short_id, card);
          hash_l->setFont(mono);
          hash_l->setStyleSheet(
              "QLabel {"
              "  color: #E8EAED;"
              "  font-size: 12px;"
              "  font-weight: 600;"
              "}");

          top->addWidget(hash_l, 0);

          if (is_head) {
            top->addWidget(MakePillLabel("HEAD", "#0B1A2B", "rgba(138, 180, 248, 0.95)",
                                         "rgba(138, 180, 248, 0.95)", card),
                           0);
          }
          if (is_base) {
            top->addWidget(MakePillLabel("BASE", "#071C12", "rgba(129, 201, 149, 0.90)",
                                         "rgba(129, 201, 149, 0.90)", card),
                           0);
          }
          if (is_plain) {
            top->addWidget(MakePillLabel("PLAIN", "#202124", "rgba(170, 176, 182, 0.20)",
                                         "rgba(170, 176, 182, 0.30)", card),
                           0);
          } else {
            const auto parent_short =
                QString::fromStdString(ver.GetParentVersionID().ToString().substr(0, 8));
            top->addWidget(
                MakePillLabel(QString("PARENT %1").arg(parent_short), "#AAB0B6",
                              "rgba(170, 176, 182, 0.08)", "rgba(170, 176, 182, 0.18)", card),
                0);
          }

          top->addStretch(1);

          auto* tx_pill =
              MakePillLabel(QString("tx %1").arg(committed_tx_count), "#AAB0B6",
                            "rgba(170, 176, 182, 0.08)", "rgba(170, 176, 182, 0.18)", card);
          top->addWidget(tx_pill, 0);

          auto* msg_l = new QLabel(msg, card);
          msg_l->setWordWrap(true);
          msg_l->setStyleSheet(
              "QLabel {"
              "  color: #E8EAED;"
              "  font-size: 12px;"
              "}");

          auto* meta_l = new QLabel(when, card);
          meta_l->setStyleSheet(
              "QLabel {"
              "  color: #AAB0B6;"
              "  font-size: 11px;"
              "}");

          body->addLayout(top);
          body->addWidget(msg_l);
          body->addWidget(meta_l);
          row->addLayout(body, 1);

          version_log_->setItemWidget(item, card);

          const QString ver_id_str = QString::fromStdString(ver_id.ToString());
          if (!prev_selected_id.isEmpty() && ver_id_str == prev_selected_id) {
            version_log_->setCurrentItem(item);
            item->setSelected(true);
          } else if (prev_selected_id.isEmpty() && is_head) {
            version_log_->setCurrentItem(item);
            item->setSelected(true);
          }
        }
      }
      RefreshVersionLogSelectionStyles();
    }
  }

  void CommitWorkingVersion() {
    if (!history_service_ || !history_guard_ || !history_guard_->history_) {
      QMessageBox::warning(this, "History", "Edit history service not available.");
      return;
    }

    const size_t tx_count = working_version_.GetAllEditTransactions().size();
    if (tx_count == 0) {
      QMessageBox::information(this, "History", "No uncommitted transactions.");
      return;
    }

    history_id_t committed_id{};
    try {
      committed_id = history_service_->CommitVersion(history_guard_, std::move(working_version_));
    } catch (const std::exception& e) {
      QMessageBox::warning(this, "History", QString("Commit failed: %1").arg(e.what()));
      working_version_ = Version(element_id_);
      working_version_.SetBasePipelineExecutor(pipeline_guard_->pipeline_);
      UpdateVersionUi();
      return;
    }

    // Start a fresh working version chained from the committed one.
    StartNewWorkingVersionFromCommit(committed_id);
    UpdateVersionUi();
  }

  auto CurrentWorkingMode() const -> WorkingMode {
    if (!working_mode_combo_) {
      return WorkingMode::Incremental;
    }
    return static_cast<WorkingMode>(working_mode_combo_->currentData().toInt());
  }

  void StartNewWorkingVersionFromUi() {
    if (CurrentWorkingMode() == WorkingMode::Plain) {
      working_version_ = Version(element_id_);
      working_version_.SetBasePipelineExecutor(pipeline_guard_->pipeline_);
      UpdateVersionUi();
      return;
    }

    // Incremental: seed from latest committed version (if any).
    try {
      if (history_guard_ && history_guard_->history_) {
        const auto parent_id = history_guard_->history_->GetLatestVersion().ver_ref_.GetVersionID();
        working_version_     = Version(element_id_, parent_id);
      } else {
        working_version_ = Version(element_id_);
      }
    } catch (...) {
      working_version_ = Version(element_id_);
    }
    working_version_.SetBasePipelineExecutor(pipeline_guard_->pipeline_);
    UpdateVersionUi();
  }

  void StartNewWorkingVersionFromCommit(const Hash128& committed_id) {
    if (CurrentWorkingMode() == WorkingMode::Plain) {
      working_version_ = Version(element_id_);
    } else {
      working_version_ = Version(element_id_, committed_id);
    }
    working_version_.SetBasePipelineExecutor(pipeline_guard_->pipeline_);
  }

  std::pair<PipelineStageName, OperatorType> FieldSpec(AdjustmentField field) const {
    switch (field) {
      case AdjustmentField::Exposure:
        return {PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE};
      case AdjustmentField::Contrast:
        return {PipelineStageName::Basic_Adjustment, OperatorType::CONTRAST};
      case AdjustmentField::Saturation:
        return {PipelineStageName::Color_Adjustment, OperatorType::SATURATION};
      case AdjustmentField::Vibrance:
        return {PipelineStageName::Color_Adjustment, OperatorType::VIBRANCE};
      case AdjustmentField::Tint:
        return {PipelineStageName::Color_Adjustment, OperatorType::TINT};
      case AdjustmentField::Blacks:
        return {PipelineStageName::Basic_Adjustment, OperatorType::BLACK};
      case AdjustmentField::Whites:
        return {PipelineStageName::Basic_Adjustment, OperatorType::WHITE};
      case AdjustmentField::Shadows:
        return {PipelineStageName::Basic_Adjustment, OperatorType::SHADOWS};
      case AdjustmentField::Highlights:
        return {PipelineStageName::Basic_Adjustment, OperatorType::HIGHLIGHTS};
      case AdjustmentField::Sharpen:
        return {PipelineStageName::Detail_Adjustment, OperatorType::SHARPEN};
      case AdjustmentField::Clarity:
        return {PipelineStageName::Detail_Adjustment, OperatorType::CLARITY};
      case AdjustmentField::Lut:
        return {PipelineStageName::Color_Adjustment, OperatorType::LMT};
    }
    return {PipelineStageName::Basic_Adjustment, OperatorType::EXPOSURE};
  }

  nlohmann::json ParamsForField(AdjustmentField field, const AdjustmentState& s) const {
    switch (field) {
      case AdjustmentField::Exposure:
        return {{"exposure", s.exposure_}};
      case AdjustmentField::Contrast:
        return {{"contrast", s.contrast_}};
      case AdjustmentField::Saturation:
        return {{"saturation", s.saturation_}};
      case AdjustmentField::Vibrance:
        return {{"vibrance", s.vibrance_}};
      case AdjustmentField::Tint:
        return {{"tint", s.tint_}};
      case AdjustmentField::Blacks:
        return {{"black", s.blacks_}};
      case AdjustmentField::Whites:
        return {{"white", s.whites_}};
      case AdjustmentField::Shadows:
        return {{"shadows", s.shadows_}};
      case AdjustmentField::Highlights:
        return {{"highlights", s.highlights_}};
      case AdjustmentField::Sharpen:
        return {{"sharpen", {{"offset", s.sharpen_}}}};
      case AdjustmentField::Clarity:
        return {{"clarity", s.clarity_}};
      case AdjustmentField::Lut:
        return {{"ocio_lmt", s.lut_path_}};
    }
    return {};
  }

  bool FieldChanged(AdjustmentField field) const {
    switch (field) {
      case AdjustmentField::Exposure:
        return !NearlyEqual(state_.exposure_, committed_state_.exposure_);
      case AdjustmentField::Contrast:
        return !NearlyEqual(state_.contrast_, committed_state_.contrast_);
      case AdjustmentField::Saturation:
        return !NearlyEqual(state_.saturation_, committed_state_.saturation_);
      case AdjustmentField::Vibrance:
        return !NearlyEqual(state_.vibrance_, committed_state_.vibrance_);
      case AdjustmentField::Tint:
        return !NearlyEqual(state_.tint_, committed_state_.tint_);
      case AdjustmentField::Blacks:
        return !NearlyEqual(state_.blacks_, committed_state_.blacks_);
      case AdjustmentField::Whites:
        return !NearlyEqual(state_.whites_, committed_state_.whites_);
      case AdjustmentField::Shadows:
        return !NearlyEqual(state_.shadows_, committed_state_.shadows_);
      case AdjustmentField::Highlights:
        return !NearlyEqual(state_.highlights_, committed_state_.highlights_);
      case AdjustmentField::Sharpen:
        return !NearlyEqual(state_.sharpen_, committed_state_.sharpen_);
      case AdjustmentField::Clarity:
        return !NearlyEqual(state_.clarity_, committed_state_.clarity_);
      case AdjustmentField::Lut:
        return state_.lut_path_ != committed_state_.lut_path_;
    }
    return false;
  }

  void CommitAdjustment(AdjustmentField field) {
    if (!FieldChanged(field) || !pipeline_guard_ || !pipeline_guard_->pipeline_) {
      // Still fulfill the "full res on release/change" behavior.
      state_.type_ = RenderType::FULL_RES_PREVIEW;
      RequestRender();
      state_.type_ = RenderType::FAST_PREVIEW;
      return;
    }

    const auto [stage_name, op_type] = FieldSpec(field);
    const auto            old_params = ParamsForField(field, committed_state_);
    const auto            new_params = ParamsForField(field, state_);

    auto                  exec       = pipeline_guard_->pipeline_;
    auto&                 stage      = exec->GetStage(stage_name);
    const auto            op         = stage.GetOperator(op_type);
    const TransactionType tx_type =
        (op.has_value() && op.value() != nullptr) ? TransactionType::_EDIT : TransactionType::_ADD;

    EditTransaction tx{tx_type, op_type, stage_name, new_params};
    tx.SetLastOperatorParams(old_params);
    (void)tx.ApplyTransaction(*exec);

    working_version_.AppendEditTransaction(std::move(tx));
    pipeline_guard_->dirty_ = true;

    committed_state_        = state_;
    UpdateVersionUi();

    state_.type_ = RenderType::FULL_RES_PREVIEW;
    RequestRender();
    state_.type_ = RenderType::FAST_PREVIEW;
  }

  bool LoadStateFromPipelineIfPresent() {
    auto exec = pipeline_guard_ ? pipeline_guard_->pipeline_ : nullptr;
    if (!exec) {
      return false;
    }

    auto ReadFloat = [](const PipelineStage& stage, OperatorType type,
                        const char* key) -> std::optional<float> {
      const auto op = stage.GetOperator(type);
      if (!op.has_value() || op.value() == nullptr) {
        return std::nullopt;
      }
      const auto j = op.value()->ExportOperatorParams();
      if (!j.contains("params")) {
        return std::nullopt;
      }
      const auto& params = j["params"];
      if (!params.contains(key)) {
        return std::nullopt;
      }
      try {
        return params[key].get<float>();
      } catch (...) {
        return std::nullopt;
      }
    };

    auto ReadNestedFloat = [](const PipelineStage& stage, OperatorType type, const char* key1,
                              const char* key2) -> std::optional<float> {
      const auto op = stage.GetOperator(type);
      if (!op.has_value() || op.value() == nullptr) {
        return std::nullopt;
      }
      const auto j = op.value()->ExportOperatorParams();
      if (!j.contains("params")) {
        return std::nullopt;
      }
      const auto& params = j["params"];
      if (!params.contains(key1)) {
        return std::nullopt;
      }
      const auto& inner = params[key1];
      if (!inner.contains(key2)) {
        return std::nullopt;
      }
      try {
        return inner[key2].get<float>();
      } catch (...) {
        return std::nullopt;
      }
    };

    auto ReadString = [](const PipelineStage& stage, OperatorType type,
                         const char* key) -> std::optional<std::string> {
      const auto op = stage.GetOperator(type);
      if (!op.has_value() || op.value() == nullptr) {
        return std::nullopt;
      }
      const auto j = op.value()->ExportOperatorParams();
      if (!j.contains("params")) {
        return std::nullopt;
      }
      const auto& params = j["params"];
      if (!params.contains(key)) {
        return std::nullopt;
      }
      try {
        return params[key].get<std::string>();
      } catch (...) {
        return std::nullopt;
      }
    };

    const auto& basic  = exec->GetStage(PipelineStageName::Basic_Adjustment);
    const auto& color  = exec->GetStage(PipelineStageName::Color_Adjustment);
    const auto& detail = exec->GetStage(PipelineStageName::Detail_Adjustment);

    // If there is no exposure operator, treat this as a fresh pipeline without saved params.
    if (!basic.GetOperator(OperatorType::EXPOSURE).has_value()) {
      return false;
    }

    if (const auto v = ReadFloat(basic, OperatorType::EXPOSURE, "exposure"); v.has_value()) {
      state_.exposure_ = v.value();
    }
    if (const auto v = ReadFloat(basic, OperatorType::CONTRAST, "contrast"); v.has_value()) {
      state_.contrast_ = v.value();
    }
    if (const auto v = ReadFloat(basic, OperatorType::BLACK, "black"); v.has_value()) {
      state_.blacks_ = v.value();
    }
    if (const auto v = ReadFloat(basic, OperatorType::WHITE, "white"); v.has_value()) {
      state_.whites_ = v.value();
    }
    if (const auto v = ReadFloat(basic, OperatorType::SHADOWS, "shadows"); v.has_value()) {
      state_.shadows_ = v.value();
    }
    if (const auto v = ReadFloat(basic, OperatorType::HIGHLIGHTS, "highlights"); v.has_value()) {
      state_.highlights_ = v.value();
    }

    if (const auto v = ReadFloat(color, OperatorType::SATURATION, "saturation"); v.has_value()) {
      state_.saturation_ = v.value();
    }

    if (const auto v = ReadFloat(color, OperatorType::VIBRANCE, "vibrance"); v.has_value()) {
      state_.vibrance_ = v.value();
    }

    if (const auto v = ReadFloat(color, OperatorType::TINT, "tint"); v.has_value()) {
      state_.tint_ = v.value();
    }

    if (const auto v = ReadNestedFloat(detail, OperatorType::SHARPEN, "sharpen", "offset");
        v.has_value()) {
      state_.sharpen_ = v.value();
    }
    if (const auto v = ReadFloat(detail, OperatorType::CLARITY, "clarity"); v.has_value()) {
      state_.clarity_ = v.value();
    }

    // LUT (LMT): if a path exists, treat it as enabled.
    const auto lut = ReadString(color, OperatorType::LMT, "ocio_lmt");
    if (lut.has_value() && !lut->empty()) {
      state_.lut_path_ = *lut;
    } else {
      // Fall back to global flag, but keep empty path if none is specified.
      state_.lut_path_.clear();
    }

    return true;
  }

  void SetupPipeline() {
    auto img_desc = image_pool_->Read<std::shared_ptr<Image>>(
        image_id_, [](const std::shared_ptr<Image>& img) { return img; });
    auto bytes = ByteBufferLoader::LoadFromImage(img_desc);
    if (!bytes) {
      throw std::runtime_error("EditorDialog: failed to load image bytes");
    }

    base_task_.input_             = std::make_shared<ImageBuffer>(std::move(*bytes));
    base_task_.pipeline_executor_ = pipeline_guard_->pipeline_;

    auto           exec           = pipeline_guard_->pipeline_;
    // exec->SetPreviewMode(true);

    // auto& global_params = exec->GetGlobalParams();
    auto&          loading        = exec->GetStage(PipelineStageName::Image_Loading);
    nlohmann::json decode_params;
#ifdef HAVE_CUDA
    decode_params["raw"]["cuda"] = true;
#else
    decode_params["raw"]["cuda"] = false;
#endif
    decode_params["raw"]["highlights_reconstruct"] = true;
    decode_params["raw"]["use_camera_wb"]          = true;
    decode_params["raw"]["user_wb"]                = 7600.f;
    decode_params["raw"]["backend"]                = "alcedo";
    loading.SetOperator(OperatorType::RAW_DECODE, decode_params);

    // auto& basic         = exec->GetStage(PipelineStageName::Basic_Adjustment);
    // basic.SetOperator(OperatorType::EXPOSURE, {{"exposure", 0.0f}}, global_params);
    // basic.SetOperator(OperatorType::CONTRAST, {{"contrast", 1.0f}}, global_params);
    // basic.SetOperator(OperatorType::BLACK, {{"black", 0.0f}}, global_params);
    // basic.SetOperator(OperatorType::WHITE, {{"white", 0.0f}}, global_params);
    // basic.SetOperator(OperatorType::SHADOWS, {{"shadows", 0.0f}}, global_params);
    // basic.SetOperator(OperatorType::HIGHLIGHTS, {{"highlights", 0.0f}}, global_params);

    // auto& color = exec->GetStage(PipelineStageName::Color_Adjustment);
    // color.SetOperator(OperatorType::SATURATION, {{"saturation", 0.0f}}, global_params);
    // color.SetOperator(OperatorType::TINT, {{"tint", 0.0f}}, global_params);

    // auto& detail = exec->GetStage(PipelineStageName::Detail_Adjustment);
    // detail.SetOperator(OperatorType::SHARPEN, {{"sharpen", {{"offset", 0.0f}}}}, global_params);
    // detail.SetOperator(OperatorType::CLARITY, {{"clarity", 0.0f}}, global_params);

    exec->SetExecutionStages(viewer_);

    // Cached pipelines can clear transient GPU resources when returned to the service.
    // PipelineMgmtService now resyncs global params on load, so we no longer need a
    // per-dialog LMT rebind hack here.
    last_applied_lut_path_.clear();
  }

  void ApplyStateToPipeline() {
    auto  exec          = pipeline_guard_->pipeline_;
    auto& global_params = exec->GetGlobalParams();

    auto& basic         = exec->GetStage(PipelineStageName::Basic_Adjustment);
    basic.SetOperator(OperatorType::EXPOSURE, {{"exposure", state_.exposure_}}, global_params);
    basic.SetOperator(OperatorType::CONTRAST, {{"contrast", state_.contrast_}}, global_params);
    basic.SetOperator(OperatorType::BLACK, {{"black", state_.blacks_}}, global_params);
    basic.SetOperator(OperatorType::WHITE, {{"white", state_.whites_}}, global_params);
    basic.SetOperator(OperatorType::SHADOWS, {{"shadows", state_.shadows_}}, global_params);
    basic.SetOperator(OperatorType::HIGHLIGHTS, {{"highlights", state_.highlights_}},
                      global_params);

    auto& color = exec->GetStage(PipelineStageName::Color_Adjustment);
    color.SetOperator(OperatorType::SATURATION, {{"saturation", state_.saturation_}},
                      global_params);
    color.SetOperator(OperatorType::TINT, {{"tint", state_.tint_}}, global_params);
    color.SetOperator(OperatorType::VIBRANCE, {{"vibrance", state_.vibrance_}}, global_params);

    // LUT (LMT): rebind only when the path changes. The operator's SetGlobalParams now
    // derives lmt_enabled_/dirty state from the path, and PipelineMgmtService resyncs on load.
    if (state_.lut_path_ != last_applied_lut_path_) {
      color.SetOperator(OperatorType::LMT, {{"ocio_lmt", state_.lut_path_}}, global_params);
      last_applied_lut_path_ = state_.lut_path_;
    }

    auto& detail = exec->GetStage(PipelineStageName::Detail_Adjustment);
    detail.SetOperator(OperatorType::SHARPEN, {{"sharpen", {{"offset", state_.sharpen_}}}},
                       global_params);
    detail.SetOperator(OperatorType::CLARITY, {{"clarity", state_.clarity_}}, global_params);
  }

  void RequestRender() {
    pending_     = state_;
    has_pending_ = true;
    if (!inflight_) {
      StartNext();
    }
  }

  void EnsurePollTimer() {
    if (poll_timer_) {
      return;
    }
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(16);
    QObject::connect(poll_timer_, &QTimer::timeout, this, [this]() { PollInflight(); });
  }

  void PollInflight() {
    if (!inflight_future_.has_value()) {
      if (poll_timer_ && poll_timer_->isActive() && !inflight_) {
        poll_timer_->stop();
      }
      return;
    }

    if (inflight_future_->wait_for(0ms) != std::future_status::ready) {
      return;
    }

    try {
      (void)inflight_future_->get();
    } catch (...) {
    }
    inflight_future_.reset();
    OnRenderFinished();
  }

  void StartNext() {
    if (!has_pending_) {
      return;
    }

    state_       = pending_;
    has_pending_ = false;

    if (spinner_) {
      spinner_->Start();
      // Ensure the spinner paints before any potentially blocking work.
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    ApplyStateToPipeline();
    pipeline_guard_->dirty_                 = true;

    PipelineTask task                       = base_task_;
    task.options_.render_desc_.render_type_ = state_.type_;
    task.options_.is_callback_              = false;
    task.options_.is_seq_callback_          = false;
    task.options_.is_blocking_              = true;

    auto promise = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
    auto fut     = promise->get_future();
    task.result_ = promise;

    inflight_    = true;
    scheduler_->ScheduleTask(std::move(task));

    inflight_future_ = std::move(fut);
    EnsurePollTimer();
    if (poll_timer_ && !poll_timer_->isActive()) {
      poll_timer_->start();
    }
  }

  void OnRenderFinished() {
    inflight_ = false;

    if (spinner_) {
      spinner_->Stop();
    }

    if (has_pending_) {
      StartNext();
    } else if (poll_timer_ && poll_timer_->isActive()) {
      poll_timer_->stop();
    }
  }

  std::shared_ptr<ImagePoolService>                        image_pool_;
  std::shared_ptr<PipelineGuard>                           pipeline_guard_;
  std::shared_ptr<EditHistoryMgmtService>                  history_service_;
  std::shared_ptr<EditHistoryGuard>                        history_guard_;
  sl_element_id_t                                          element_id_ = 0;
  image_id_t                                               image_id_   = 0;

  std::shared_ptr<PipelineScheduler>                       scheduler_;
  PipelineTask                                             base_task_{};

  QtEditViewer*                                            viewer_             = nullptr;
  QWidget*                                                 viewer_container_   = nullptr;
  QScrollArea*                                             controls_scroll_    = nullptr;
  SpinnerWidget*                                           spinner_            = nullptr;
  QWidget*                                                 controls_           = nullptr;
  QComboBox*                                               lut_combo_          = nullptr;
  QSlider*                                                 exposure_slider_    = nullptr;
  QSlider*                                                 contrast_slider_    = nullptr;
  QSlider*                                                 saturation_slider_  = nullptr;
  QSlider*                                                 vibrance_slider_    = nullptr;
  QSlider*                                                 tint_slider_        = nullptr;
  QSlider*                                                 blacks_slider_      = nullptr;
  QSlider*                                                 whites_slider_      = nullptr;
  QSlider*                                                 shadows_slider_     = nullptr;
  QSlider*                                                 highlights_slider_  = nullptr;
  QSlider*                                                 sharpen_slider_     = nullptr;
  QSlider*                                                 clarity_slider_     = nullptr;
  QLabel*                                                  version_status_     = nullptr;
  QPushButton*                                             commit_version_btn_ = nullptr;
  QComboBox*                                               working_mode_combo_ = nullptr;
  QPushButton*                                             new_working_btn_    = nullptr;
  QListWidget*                                             version_log_        = nullptr;
  QListWidget*                                             tx_stack_           = nullptr;
  QTimer*                                                  poll_timer_         = nullptr;
  std::optional<std::future<std::shared_ptr<ImageBuffer>>> inflight_future_{};

  std::vector<std::string>                                 lut_paths_{};
  QStringList                                              lut_names_{};

  std::string                                              last_applied_lut_path_{};
  AdjustmentState                                          state_{};
  AdjustmentState                                          committed_state_{};
  Version                                                  working_version_{};
  AdjustmentState                                          pending_{};
  bool                                                     inflight_    = false;
  bool                                                     has_pending_ = false;
};

class AlbumWindow final : public QWidget {
 public:
  AlbumWindow(std::shared_ptr<ProjectService> project, std::filesystem::path meta_path,
              std::shared_ptr<ThumbnailService>    thumbnail_service,
              std::shared_ptr<ImagePoolService>    image_pool,
              std::shared_ptr<PipelineMgmtService> pipeline_service, QWidget* parent = nullptr)
      : QWidget(parent),
        project_(std::move(project)),
        meta_path_(std::move(meta_path)),
        thumbnails_(std::move(thumbnail_service)),
        image_pool_(std::move(image_pool)),
        pipeline_service_(std::move(pipeline_service)),
        import_service_(project_ ? project_->GetSleeveService() : nullptr,
                        project_ ? project_->GetImagePoolService() : nullptr) {
    if (!project_ || !thumbnails_ || !image_pool_ || !pipeline_service_) {
      throw std::runtime_error("AlbumWindow: missing services");
    }

    export_service_  = std::make_shared<ExportService>(project_->GetSleeveService(), image_pool_,
                                                       pipeline_service_);
    filter_service_  = std::make_unique<SleeveFilterService>(project_->GetStorage());
    history_service_ = std::make_shared<EditHistoryMgmtService>(project_->GetStorage());

    auto* root       = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto* top_bar = new QFrame(this);
    top_bar->setObjectName("GlassSurface");
    auto* top = new QHBoxLayout(top_bar);
    top->setContentsMargins(14, 12, 14, 12);
    top->setSpacing(8);

    auto* app_title = new QLabel("Alcedo", top_bar);
    app_title->setObjectName("SectionTitle");
    top->addWidget(app_title, 0);

    library_page_btn_ = new QToolButton(top_bar);
    library_page_btn_->setText("Library");
    library_page_btn_->setCheckable(true);
    library_page_btn_->setChecked(true);
    settings_page_btn_ = new QToolButton(top_bar);
    settings_page_btn_->setText("Settings");
    settings_page_btn_->setCheckable(true);
    top->addWidget(library_page_btn_, 0);
    top->addWidget(settings_page_btn_, 0);

    global_search_ = new QLineEdit(top_bar);
    global_search_->setPlaceholderText("Search photos");
    global_search_->setClearButtonEnabled(true);
    top->addWidget(global_search_, 1);

    import_btn_ = new QPushButton("Import…", top_bar);
    import_btn_->setObjectName("accent");
    export_btn_  = new QPushButton("Export…", top_bar);
    filters_btn_ = new QToolButton(top_bar);
    filters_btn_->setText("Inspector");
    filters_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    filters_btn_->setCheckable(true);
    filters_btn_->setChecked(true);
    filters_btn_->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
    top->addWidget(import_btn_, 0);
    top->addWidget(export_btn_, 0);
    top->addWidget(filters_btn_, 0);

    quick_actions_btn_ = new QToolButton(top_bar);
    quick_actions_btn_->setText("Quick");
    quick_actions_btn_->setPopupMode(QToolButton::InstantPopup);
    auto* quick_menu = new QMenu(quick_actions_btn_);
    quick_menu->addAction("Import  (Ctrl+I)", this, [this]() { BeginImport(); });
    quick_menu->addAction("Export  (Ctrl+E)", this, [this]() { OpenExport(); });
    quick_menu->addAction("Library  (Ctrl+1)", this, [this]() { ShowPage(/*settings=*/false); });
    quick_menu->addAction("Settings  (Ctrl+,)", this, [this]() { ShowPage(/*settings=*/true); });
    quick_actions_btn_->setMenu(quick_menu);
    top->addWidget(quick_actions_btn_, 0);
    root->addWidget(top_bar, 0);

    auto* shell_split = new QSplitter(Qt::Horizontal, this);
    shell_split->setChildrenCollapsible(false);
    shell_split->setHandleWidth(8);
    root->addWidget(shell_split, 1);

    auto* side_card = new CardFrame("Library", shell_split);
    side_card->setMinimumWidth(220);
    side_card->setMaximumWidth(280);
    sidebar_search_ = new QLineEdit(side_card);
    sidebar_search_->setPlaceholderText("Search folders/collections");
    side_card->BodyLayout()->addWidget(sidebar_search_, 0);

    sidebar_list_ = new QListWidget(side_card);
    sidebar_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    sidebar_list_->addItem("All Photos");
    sidebar_list_->addItem("Recent Imports");
    sidebar_list_->addItem("Collections");
    sidebar_list_->addItem("Settings");
    sidebar_list_->setCurrentRow(0);
    side_card->BodyLayout()->addWidget(sidebar_list_, 1);

    auto* center      = new QWidget(shell_split);
    auto* center_root = new QVBoxLayout(center);
    center_root->setContentsMargins(0, 0, 0, 0);
    center_root->setSpacing(12);

    auto* library_toolbar = new QFrame(center);
    library_toolbar->setObjectName("GlassSurface");
    auto* lib_top = new QHBoxLayout(library_toolbar);
    lib_top->setContentsMargins(12, 10, 12, 10);
    lib_top->setSpacing(8);
    auto* browser_title = new QLabel("Browser", library_toolbar);
    browser_title->setObjectName("SectionTitle");
    lib_top->addWidget(browser_title, 0);
    auto* browser_meta = new QLabel("Responsive thumbnail grid", library_toolbar);
    browser_meta->setObjectName("MetaText");
    lib_top->addWidget(browser_meta, 0);
    lib_top->addStretch(1);
    grid_view_btn_ = new QToolButton(library_toolbar);
    grid_view_btn_->setText("Grid");
    grid_view_btn_->setCheckable(true);
    grid_view_btn_->setChecked(true);
    list_view_btn_ = new QToolButton(library_toolbar);
    list_view_btn_->setText("List");
    list_view_btn_->setCheckable(true);
    list_view_btn_->setChecked(false);
    lib_top->addWidget(grid_view_btn_, 0);
    lib_top->addWidget(list_view_btn_, 0);
    center_root->addWidget(library_toolbar, 0);

    page_stack_ = new QStackedWidget(center);
    center_root->addWidget(page_stack_, 1);

    auto* library_page = new QWidget(page_stack_);
    auto* lib_root     = new QVBoxLayout(library_page);
    lib_root->setContentsMargins(0, 0, 0, 0);
    lib_root->setSpacing(12);

    auto* browser_card = new CardFrame("Thumbnails", library_page);
    status_            = new QLabel("No images loaded.", browser_card);
    status_->setObjectName("MetaText");
    browser_card->HeaderLayout()->addWidget(status_, 0, Qt::AlignRight);

    browser_state_stack_ = new QStackedWidget(browser_card);
    auto* list_page      = new QWidget(browser_card);
    auto* list_layout    = new QVBoxLayout(list_page);
    list_layout->setContentsMargins(0, 0, 0, 0);
    list_layout->setSpacing(0);

    list_ = new QListWidget(list_page);
    list_->setViewMode(QListView::IconMode);
    list_->setResizeMode(QListWidget::Adjust);
    list_->setMovement(QListView::Static);
    list_->setIconSize(QSize(220, 160));
    list_->setGridSize(QSize(250, 214));
    list_->setSpacing(10);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_layout->addWidget(list_, 1);

    auto* empty_page   = new QWidget(browser_card);
    auto* empty_layout = new QVBoxLayout(empty_page);
    empty_layout->setContentsMargins(36, 36, 36, 36);
    empty_layout->setSpacing(12);
    auto* empty_title = new QLabel("No Photos Yet", empty_page);
    empty_title->setObjectName("SectionTitle");
    auto* empty_text = new QLabel(
        "Import your first folder to start thumbnail generation and RAW adjustments.", empty_page);
    empty_text->setObjectName("MetaText");
    empty_text->setWordWrap(true);
    auto* empty_cta = new QPushButton("Import Photos…", empty_page);
    empty_cta->setObjectName("accent");
    empty_layout->addStretch(1);
    empty_layout->addWidget(empty_title, 0, Qt::AlignHCenter);
    empty_layout->addWidget(empty_text, 0, Qt::AlignHCenter);
    empty_layout->addWidget(empty_cta, 0, Qt::AlignHCenter);
    empty_layout->addStretch(1);

    browser_state_stack_->addWidget(list_page);
    browser_state_stack_->addWidget(empty_page);
    browser_card->BodyLayout()->addWidget(browser_state_stack_, 1);
    lib_root->addWidget(browser_card, 1);
    page_stack_->addWidget(library_page);

    settings_page_ = new SettingsPage(page_stack_);
    page_stack_->addWidget(settings_page_);

    inspector_host_        = new QWidget(shell_split);
    auto* inspector_layout = new QVBoxLayout(inspector_host_);
    inspector_layout->setContentsMargins(0, 0, 0, 0);
    inspector_layout->setSpacing(12);

    auto* inspector_scroll = new QScrollArea(inspector_host_);
    inspector_scroll->setFrameShape(QFrame::NoFrame);
    inspector_scroll->setWidgetResizable(true);
    inspector_layout->addWidget(inspector_scroll, 1);

    auto* inspector_panel = new QWidget(inspector_scroll);
    auto* panel_layout    = new QVBoxLayout(inspector_panel);
    panel_layout->setContentsMargins(0, 0, 0, 0);
    panel_layout->setSpacing(12);
    inspector_scroll->setWidget(inspector_panel);

    filter_drawer_ = new FilterDrawer(inspector_panel);
    panel_layout->addWidget(filter_drawer_, 0);

    auto* inspector_note = new QFrame(inspector_panel);
    inspector_note->setObjectName("CardSurface");
    auto* note_layout = new QVBoxLayout(inspector_note);
    note_layout->setContentsMargins(12, 12, 12, 12);
    note_layout->setSpacing(8);
    auto* note_title = new QLabel("Inspector", inspector_note);
    note_title->setObjectName("SectionTitle");
    auto* note_text = new QLabel(
        "Image adjustments are now available in EditorDialog to keep the browser focused on "
        "library triage and filtering.",
        inspector_note);
    note_text->setObjectName("MetaText");
    note_text->setWordWrap(true);
    note_layout->addWidget(note_title, 0);
    note_layout->addWidget(note_text, 0);
    panel_layout->addWidget(inspector_note, 0);
    panel_layout->addStretch(1);

    shell_split->setStretchFactor(0, 0);
    shell_split->setStretchFactor(1, 1);
    shell_split->setStretchFactor(2, 0);
    shell_split->setSizes({220, 860, 340});

    auto* task_card = new CardFrame("Background Tasks", this);
    auto* task_row  = new QHBoxLayout();
    task_row->setContentsMargins(0, 0, 0, 0);
    task_row->setSpacing(10);
    task_status_ = new QLabel("No background tasks", task_card);
    task_status_->setObjectName("MetaText");
    task_row->addWidget(task_status_, 1);
    task_progress_ = new QProgressBar(task_card);
    task_progress_->setRange(0, 100);
    task_progress_->setValue(0);
    task_progress_->setFixedWidth(260);
    task_row->addWidget(task_progress_, 0);
    task_cancel_btn_ = new QPushButton("Cancel", task_card);
    task_cancel_btn_->setVisible(false);
    task_row->addWidget(task_cancel_btn_, 0);
    task_card->BodyLayout()->addLayout(task_row);
    root->addWidget(task_card, 0);

    connect(import_btn_, &QPushButton::clicked, this, [this]() { BeginImport(); });
    connect(export_btn_, &QPushButton::clicked, this, [this]() { OpenExport(); });
    connect(filters_btn_, &QToolButton::toggled, this,
            [this](bool on) { inspector_host_->setVisible(on && !showing_settings_); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
      if (!item) {
        return;
      }
      const auto element_id = static_cast<sl_element_id_t>(item->data(Qt::UserRole + 1).toUInt());
      const auto image_id   = static_cast<image_id_t>(item->data(Qt::UserRole + 2).toUInt());
      OpenEditor(element_id, image_id);
    });
    connect(empty_cta, &QPushButton::clicked, this, [this]() { BeginImport(); });

    connect(library_page_btn_, &QToolButton::clicked, this, [this]() { ShowPage(false); });
    connect(settings_page_btn_, &QToolButton::clicked, this, [this]() { ShowPage(true); });
    connect(sidebar_list_, &QListWidget::currentRowChanged, this,
            [this](int row) { ShowPage(row == 3); });
    connect(grid_view_btn_, &QToolButton::clicked, this, [this]() {
      list_view_btn_->setChecked(false);
      grid_view_btn_->setChecked(true);
      if (!list_) {
        return;
      }
      list_->setViewMode(QListView::IconMode);
      list_->setIconSize(QSize(220, 160));
      list_->setGridSize(QSize(250, 214));
      list_->setSpacing(10);
    });
    connect(list_view_btn_, &QToolButton::clicked, this, [this]() {
      grid_view_btn_->setChecked(false);
      list_view_btn_->setChecked(true);
      if (!list_) {
        return;
      }
      list_->setViewMode(QListView::ListMode);
      list_->setIconSize(QSize(96, 72));
      list_->setGridSize(QSize(0, 88));
      list_->setSpacing(6);
    });
    connect(task_cancel_btn_, &QPushButton::clicked, this, [this]() { CancelImport(); });

    auto* sc_import = new QShortcut(QKeySequence("Ctrl+I"), this);
    connect(sc_import, &QShortcut::activated, this, [this]() { BeginImport(); });
    auto* sc_export = new QShortcut(QKeySequence("Ctrl+E"), this);
    connect(sc_export, &QShortcut::activated, this, [this]() { OpenExport(); });
    auto* sc_settings = new QShortcut(QKeySequence("Ctrl+,"), this);
    connect(sc_settings, &QShortcut::activated, this, [this]() { ShowPage(true); });
    auto* sc_library = new QShortcut(QKeySequence("Ctrl+1"), this);
    connect(sc_library, &QShortcut::activated, this, [this]() { ShowPage(false); });
    auto* sc_search = new QShortcut(QKeySequence("Ctrl+F"), this);
    connect(sc_search, &QShortcut::activated, this, [this]() {
      if (global_search_) {
        global_search_->setFocus();
        global_search_->selectAll();
      }
    });

    if (filter_drawer_) {
      filter_drawer_->SetOnApply([this](const FilterNode& node) { ApplyFilter(node, true); });
      filter_drawer_->SetOnClear([this]() { ClearFilter(); });
      filter_drawer_->SetResultsSummary(/*shown=*/0, /*total=*/0);
    }
    ShowPage(false);
    RefreshEmptyState();
    SetBusyUi(false, "No background tasks");
  }

 private:
  void ShowPage(bool settings) {
    showing_settings_ = settings;
    if (page_stack_) {
      page_stack_->setCurrentIndex(settings ? 1 : 0);
    }
    if (library_page_btn_) {
      library_page_btn_->setChecked(!settings);
    }
    if (settings_page_btn_) {
      settings_page_btn_->setChecked(settings);
    }
    if (inspector_host_ && filters_btn_) {
      inspector_host_->setVisible(filters_btn_->isChecked() && !settings);
    }
    if (sidebar_list_) {
      const int target = settings ? 3 : 0;
      if (sidebar_list_->currentRow() != target) {
        QSignalBlocker blocker(*sidebar_list_);
        sidebar_list_->setCurrentRow(target);
      }
    }
  }

  void RefreshEmptyState() {
    if (!browser_state_stack_ || !list_) {
      return;
    }
    browser_state_stack_->setCurrentIndex(list_->count() == 0 ? 1 : 0);
  }

  void CancelImport() {
    if (!current_import_job_) {
      return;
    }
    current_import_job_->canceled_.store(true);
    SetBusyUi(true, "Cancelling import…");
    if (task_cancel_btn_) {
      task_cancel_btn_->setEnabled(false);
    }
  }

  void ShowToast(const QString& message, int timeout_ms = 2400) {
    if (message.trimmed().isEmpty()) {
      return;
    }
    if (!toast_) {
      toast_ = new QLabel(this);
      toast_->setStyleSheet(
          "QLabel { background: rgba(24, 32, 48, 0.9); color: #FFFFFF; border-radius: 12px; "
          "padding: 8px 12px; }");
      toast_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    toast_->setText(message);
    toast_->adjustSize();
    const int margin = 20;
    toast_->move(std::max(margin, width() - toast_->width() - margin),
                 std::max(margin, height() - toast_->height() - margin));
    toast_->show();
    toast_->raise();
    const uint64_t token = ++toast_token_;
    QTimer::singleShot(timeout_ms, this, [this, token]() {
      if (!toast_ || token != toast_token_) {
        return;
      }
      toast_->hide();
    });
  }

  bool event(QEvent* e) override {
    const bool rc = QWidget::event(e);
    if (e && e->type() == QEvent::Resize && toast_ && toast_->isVisible()) {
      const int margin = 20;
      toast_->move(std::max(margin, width() - toast_->width() - margin),
                   std::max(margin, height() - toast_->height() - margin));
    }
    return rc;
  }

  int VisibleItemCount() const {
    if (!list_) {
      return 0;
    }
    int shown = 0;
    for (int i = 0; i < list_->count(); ++i) {
      auto* it = list_->item(i);
      if (it && !it->isHidden()) {
        ++shown;
      }
    }
    return shown;
  }

  void UpdateStatusText() {
    if (!status_) {
      return;
    }
    const int total = list_ ? list_->count() : 0;
    if (total <= 0) {
      status_->setText("No images loaded.");
      if (filter_drawer_) {
        filter_drawer_->SetResultsSummary(0, 0);
      }
      return;
    }

    const int shown = VisibleItemCount();
    status_->setText(QString("Showing %1 of %2 images").arg(shown).arg(total));
    if (filter_drawer_) {
      filter_drawer_->SetResultsSummary(shown, total);
    }
  }

  void SetBusyUi(bool busy, const QString& label) {
    if (import_btn_) {
      import_btn_->setEnabled(!busy);
    }
    if (export_btn_) {
      export_btn_->setEnabled(!busy);
    }
    if (list_) {
      list_->setEnabled(!busy);
    }
    if (task_status_) {
      task_status_->setText(label);
    }
    if (task_progress_) {
      if (!busy) {
        task_progress_->setRange(0, 100);
        task_progress_->setValue(0);
      }
    }
    if (task_cancel_btn_) {
      task_cancel_btn_->setVisible(busy && import_inflight_);
      task_cancel_btn_->setEnabled(import_inflight_);
    }
  }

  void BeginImport() {
    if (import_inflight_) {
      return;
    }

    ImportDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) {
      return;
    }

    const auto& paths   = dlg.SelectedPaths();
    const auto  options = dlg.SelectedOptions();

    if (paths.empty()) {
      QMessageBox::information(this, "Import", "No supported images selected.");
      return;
    }

    import_inflight_ = true;
    SetBusyUi(true, QString("Importing %1 file(s)…").arg(paths.size()));
    if (task_progress_) {
      task_progress_->setRange(0, static_cast<int>(std::max<size_t>(paths.size(), 1)));
      task_progress_->setValue(0);
    }
    ShowToast("Import started");

    auto job            = std::make_shared<ImportJob>();
    current_import_job_ = job;

    QPointer<AlbumWindow> self(this);
    job->on_progress_ = [self](const ImportProgress& p) {
      if (!self) {
        return;
      }
      const uint32_t total        = p.total_;
      const uint32_t placeholders = p.placeholders_created_.load();
      const uint32_t meta_done    = p.metadata_done_.load();
      const uint32_t failed       = p.failed_.load();

      QMetaObject::invokeMethod(
          self,
          [self, total, placeholders, meta_done, failed]() {
            if (!self) {
              return;
            }
            if (self->task_progress_) {
              self->task_progress_->setRange(0, static_cast<int>(std::max<uint32_t>(total, 1)));
              self->task_progress_->setValue(static_cast<int>(std::max(placeholders, meta_done)));
            }
            self->SetBusyUi(true, QString("Importing… %1/%2 (meta %3, failed %4)")
                                      .arg(placeholders)
                                      .arg(total)
                                      .arg(meta_done)
                                      .arg(failed));
          },
          Qt::QueuedConnection);
    };

    job->on_finished_ = [self](const ImportResult& r) {
      if (!self) {
        return;
      }
      QMetaObject::invokeMethod(
          self,
          [self, r]() {
            if (!self) {
              return;
            }
            self->FinishImport(r);
          },
          Qt::QueuedConnection);
    };

    current_import_job_ = import_service_.ImportToFolder(paths, image_path_t{}, options, job);
  }

  void FinishImport(const ImportResult& result) {
    import_inflight_ = false;
    SetBusyUi(false, "No background tasks");
    if (task_progress_) {
      task_progress_->setRange(0, 100);
      task_progress_->setValue(100);
    }

    if (!current_import_job_ || !current_import_job_->import_log_) {
      QMessageBox::warning(this, "Import", "Import finished but no log snapshot available.");
      return;
    }

    const auto snapshot = current_import_job_->import_log_->Snapshot();

    import_service_.SyncImports(snapshot, image_path_t{});
    project_->GetSleeveService()->Sync();
    project_->GetImagePoolService()->SyncWithStorage();
    project_->SaveProject(meta_path_);

    if (result.failed_ != 0) {
      QMessageBox::warning(this, "Import",
                           QString("Import finished: %1 imported, %2 failed.")
                               .arg(result.imported_)
                               .arg(result.failed_));
    }

    for (const auto& c : snapshot.created_) {
      AddAlbumItem(c.element_id_, c.image_id_, c.file_name_);
    }

    if (active_filter_node_.has_value()) {
      ApplyFilter(active_filter_node_.value(), /*show_dialogs=*/false);
    } else {
      UpdateStatusText();
    }
    RefreshEmptyState();
    ShowToast(QString("Import complete: %1 imported, %2 failed")
                  .arg(result.imported_)
                  .arg(result.failed_));
  }

  void AddAlbumItem(sl_element_id_t element_id, image_id_t image_id, file_name_t file_name) {
    if (!list_ || items_by_element_.contains(element_id)) {
      return;
    }
    auto* item = new QListWidgetItem();
    item->setText(QString::fromStdWString(file_name));
    item->setData(Qt::UserRole + 1, static_cast<uint32_t>(element_id));
    item->setData(Qt::UserRole + 2, static_cast<uint32_t>(image_id));
    item->setSizeHint(QSize(250, 214));
    item->setIcon(MakeSkeletonThumbnailIcon(220, 160, "Loading"));
    list_->addItem(item);

    items_by_element_[element_id]    = item;

    // Requirement (1): clickable cells; thumbnails load async.
    CallbackDispatcher ui_dispatcher = [](std::function<void()> fn) {
      auto* obj = QCoreApplication::instance();
      if (!obj) {
        fn();
        return;
      }
      QMetaObject::invokeMethod(obj, std::move(fn), Qt::QueuedConnection);
    };

    RefreshThumbnailForItem(element_id, image_id, item, /*invalidate=*/false);
    RefreshEmptyState();
  }

  void ClearFilter() {
    if (active_filter_id_.has_value() && filter_service_) {
      try {
        filter_service_->RemoveFilterCombo(active_filter_id_.value());
      } catch (...) {
      }
      active_filter_id_.reset();
    }
    active_filter_node_.reset();

    if (!list_) {
      return;
    }
    for (int i = 0; i < list_->count(); ++i) {
      auto* it = list_->item(i);
      if (it) {
        it->setHidden(false);
      }
    }
    list_->clearSelection();
    UpdateStatusText();
    ShowToast("Filters cleared");
  }

  void ApplyFilter(const FilterNode& node, bool show_dialogs) {
    if (!filter_service_ || !list_) {
      return;
    }
    if (list_->count() == 0) {
      if (show_dialogs) {
        QMessageBox::information(this, "Filter", "No images loaded.");
      }
      return;
    }

    // Filters are immutable and results are cached by filter_id_. Always create a new filter.
    const filter_id_t new_filter_id = filter_service_->CreateFilterCombo(node);
    const auto        filter_opt    = filter_service_->GetFilterCombo(new_filter_id);
    if (!filter_opt.has_value() || !filter_opt.value()) {
      if (show_dialogs) {
        QMessageBox::warning(this, "Filter", "Filter creation failed.");
      }
      return;
    }

    // Replace previous filter to avoid unbounded growth of in-memory caches.
    if (active_filter_id_.has_value()) {
      try {
        filter_service_->RemoveFilterCombo(active_filter_id_.value());
      } catch (...) {
      }
    }
    active_filter_id_   = new_filter_id;
    active_filter_node_ = node;

    std::vector<sl_element_id_t> filtered_ids;
    try {
      // Root folder id is 0.
      const auto ids_opt = filter_service_->ApplyFilterOn(new_filter_id, /*parent_id=*/0);
      if (!ids_opt.has_value()) {
        if (show_dialogs) {
          QMessageBox::warning(this, "Filter", "Unknown filter id.");
        }
        return;
      }
      filtered_ids = std::move(ids_opt.value());
    } catch (const std::exception& e) {
      if (show_dialogs) {
        QMessageBox::warning(this, "Filter", QString("Filter failed: %1").arg(e.what()));
      }
      return;
    }

    std::unordered_set<sl_element_id_t> allow;
    allow.reserve(filtered_ids.size() * 2 + 1);
    for (const auto id : filtered_ids) {
      allow.insert(id);
    }

    int shown = 0;
    for (int i = 0; i < list_->count(); ++i) {
      auto* it = list_->item(i);
      if (!it) {
        continue;
      }
      const auto element_id = static_cast<sl_element_id_t>(it->data(Qt::UserRole + 1).toUInt());
      const bool keep       = allow.contains(element_id);
      it->setHidden(!keep);
      if (keep) {
        ++shown;
      }
    }

    if (show_dialogs && shown == 0) {
      QMessageBox::information(this, "Filter", "No matches.");
    }

    list_->clearSelection();
    list_->scrollToTop();
    UpdateStatusText();
    ShowToast(QString("Filter applied: %1 match(es)").arg(shown));
  }

  void RefreshThumbnailForItem(sl_element_id_t element_id, image_id_t image_id,
                               QListWidgetItem* item, bool invalidate) {
    if (!item || !thumbnails_) {
      return;
    }

    // Requirement: allow redraw after editor closes.
    if (invalidate) {
      try {
        thumbnails_->InvalidateThumbnail(element_id);
      } catch (...) {
      }
    }

    CallbackDispatcher ui_dispatcher = [](std::function<void()> fn) {
      auto* obj = QCoreApplication::instance();
      if (!obj) {
        fn();
        return;
      }
      QMetaObject::invokeMethod(obj, std::move(fn), Qt::QueuedConnection);
    };

    auto                  svc = thumbnails_;
    QPointer<AlbumWindow> self(this);
    thumbnails_->GetThumbnail(
        element_id, image_id,
        [self, svc, element_id, item, ui_dispatcher](std::shared_ptr<ThumbnailGuard> guard) {
          if (!guard || !guard->thumbnail_buffer_) {
            return;
          }

          std::thread([self, svc, element_id, item, ui_dispatcher,
                       guard = std::move(guard)]() mutable {
            QImage scaled;
            try {
              auto* buf = guard->thumbnail_buffer_.get();
              if (!buf) {
                throw std::runtime_error("null buffer");
              }
              if (!buf->cpu_data_valid_ && buf->gpu_data_valid_) {
                buf->SyncToCPU();
              }
              if (buf->cpu_data_valid_) {
                QImage img = MatRgba32fToQImageCopy(buf->GetCPUData());
                scaled     = img.scaled(220, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
              }
            } catch (...) {
            }

            ui_dispatcher([self, item, scaled]() mutable {
              if (!self || !item) {
                return;
              }
              if (!self->list_ || self->list_->row(item) < 0) {
                return;
              }
              if (!scaled.isNull()) {
                item->setIcon(QIcon(QPixmap::fromImage(scaled)));
              }
            });

            try {
              if (svc) {
                svc->ReleaseThumbnail(element_id);
              }
            } catch (...) {
            }
          }).detach();
        },
        /*pin_if_found=*/true, ui_dispatcher);
  }

  void OpenEditor(sl_element_id_t element_id, image_id_t image_id) {
    if (element_id == 0) {
      return;
    }

    // Requirement (2): create a new preview scheduler (in dialog ctor) and pass pipeline guard.
    std::shared_ptr<PipelineGuard> guard;
    try {
      guard = pipeline_service_->LoadPipeline(element_id);
    } catch (const std::exception& e) {
      QMessageBox::warning(this, "Editor", QString("LoadPipeline failed: %1").arg(e.what()));
      return;
    }

    std::shared_ptr<EditHistoryGuard> history_guard;
    try {
      history_guard = history_service_ ? history_service_->LoadHistory(element_id) : nullptr;
    } catch (const std::exception& e) {
      QMessageBox::warning(this, "Editor", QString("LoadHistory failed: %1").arg(e.what()));
      return;
    }

    try {
      EditorDialog dlg(image_pool_, guard, history_service_, history_guard, element_id, image_id,
                       this);
      dlg.exec();
    } catch (const std::exception& e) {
      QMessageBox::warning(this, "Editor", QString("Editor failed: %1").arg(e.what()));
    }

    // Requirement (4): return PipelineGuard back to cell / persist to DB.
    pipeline_service_->SavePipeline(guard);
    pipeline_service_->Sync();

    if (history_service_) {
      history_service_->SaveHistory(history_guard);
      history_service_->Sync();
    }
    project_->SaveProject(meta_path_);

    // Refresh thumbnail for this cell after edits.
    auto it = items_by_element_.find(element_id);
    if (it != items_by_element_.end()) {
      RefreshThumbnailForItem(element_id, image_id, it->second, /*invalidate=*/true);
    }
  }

  void OpenExport() {
    if (!export_service_) {
      QMessageBox::warning(this, "Export", "ExportService not available.");
      return;
    }

    std::vector<ExportDialog::Item> items;

    const auto                      selected = list_->selectedItems();
    if (!selected.isEmpty()) {
      items.reserve(static_cast<size_t>(selected.size()));
      for (auto* it : selected) {
        if (!it) {
          continue;
        }
        ExportDialog::Item e;
        e.sleeve_id_ = static_cast<sl_element_id_t>(it->data(Qt::UserRole + 1).toUInt());
        e.image_id_  = static_cast<image_id_t>(it->data(Qt::UserRole + 2).toUInt());
        if (e.sleeve_id_ != 0 && e.image_id_ != 0) {
          items.push_back(e);
        }
      }
    } else {
      items.reserve(static_cast<size_t>(list_->count()));
      for (int i = 0; i < list_->count(); ++i) {
        auto* it = list_->item(i);
        if (!it) {
          continue;
        }
        ExportDialog::Item e;
        e.sleeve_id_ = static_cast<sl_element_id_t>(it->data(Qt::UserRole + 1).toUInt());
        e.image_id_  = static_cast<image_id_t>(it->data(Qt::UserRole + 2).toUInt());
        if (e.sleeve_id_ != 0 && e.image_id_ != 0) {
          items.push_back(e);
        }
      }
    }

    if (items.empty()) {
      QMessageBox::information(this, "Export", "No images to export.");
      return;
    }

    try {
      ExportDialog dlg(image_pool_, export_service_, std::move(items), this);
      if (dlg.exec() == QDialog::Accepted) {
        ShowToast("Export complete");
      }
    } catch (const std::exception& e) {
      QMessageBox::warning(this, "Export", QString("Export dialog failed: %1").arg(e.what()));
    }
  }

  std::shared_ptr<ProjectService>                       project_;
  std::filesystem::path                                 meta_path_;
  std::shared_ptr<ThumbnailService>                     thumbnails_;
  std::shared_ptr<ImagePoolService>                     image_pool_;
  std::shared_ptr<PipelineMgmtService>                  pipeline_service_;
  std::shared_ptr<EditHistoryMgmtService>               history_service_;
  std::shared_ptr<ExportService>                        export_service_;
  ImportServiceImpl                                     import_service_;

  std::unique_ptr<SleeveFilterService>                  filter_service_;
  std::optional<filter_id_t>                            active_filter_id_;
  std::optional<FilterNode>                             active_filter_node_;

  QPushButton*                                          import_btn_          = nullptr;
  QPushButton*                                          export_btn_          = nullptr;
  QToolButton*                                          filters_btn_         = nullptr;
  QToolButton*                                          quick_actions_btn_   = nullptr;
  QToolButton*                                          library_page_btn_    = nullptr;
  QToolButton*                                          settings_page_btn_   = nullptr;
  QToolButton*                                          grid_view_btn_       = nullptr;
  QToolButton*                                          list_view_btn_       = nullptr;
  QLabel*                                               status_              = nullptr;
  QLabel*                                               task_status_         = nullptr;
  QLineEdit*                                            global_search_       = nullptr;
  QLineEdit*                                            sidebar_search_      = nullptr;
  QListWidget*                                          sidebar_list_        = nullptr;
  QListWidget*                                          list_                = nullptr;
  QStackedWidget*                                       page_stack_          = nullptr;
  QStackedWidget*                                       browser_state_stack_ = nullptr;
  QWidget*                                              inspector_host_      = nullptr;
  SettingsPage*                                         settings_page_       = nullptr;
  QProgressBar*                                         task_progress_       = nullptr;
  QPushButton*                                          task_cancel_btn_     = nullptr;
  FilterDrawer*                                         filter_drawer_       = nullptr;
  QLabel*                                               toast_               = nullptr;

  std::unordered_map<sl_element_id_t, QListWidgetItem*> items_by_element_{};

  bool                                                  import_inflight_  = false;
  bool                                                  showing_settings_ = false;
  uint64_t                                              toast_token_      = 0;
  std::shared_ptr<ImportJob>                            current_import_job_{};
};

}  // namespace
}  // namespace alcedo

int main(int argc, char* argv[]) {
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
  alcedo::TimeProvider::Refresh();
  alcedo::RegisterAllOperators();

  QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

  QApplication app(argc, argv);
  alcedo::ApplyExternalAppFont(app, argc, argv);
  alcedo::ApplyFontRenderingTuning(app, argc, argv);
  alcedo::ApplyMaterialLikeTheme(app);

  try {
    const auto db_path   = std::filesystem::temp_directory_path() / "album_editor_demo.db";
    const auto meta_path = std::filesystem::temp_directory_path() / "album_editor_demo.json";

    if (std::filesystem::exists(db_path)) {
      std::filesystem::remove(db_path);
    }
    if (std::filesystem::exists(meta_path)) {
      std::filesystem::remove(meta_path);
    }

    auto project = std::make_shared<alcedo::ProjectService>(db_path, meta_path);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project->GetStorage());
    auto thumb_service = std::make_shared<alcedo::ThumbnailService>(
        project->GetSleeveService(), project->GetImagePoolService(), pipeline_service);

    auto* w = new alcedo::AlbumWindow(project, meta_path, thumb_service,
                                        project->GetImagePoolService(), pipeline_service);
    w->setWindowTitle("alcedo - Album + Editor (Qt Demo)");
    w->resize(1400, 900);
    w->show();

    const int rc = app.exec();

    pipeline_service->Sync();
    project->GetImagePoolService()->SyncWithStorage();
    project->SaveProject(meta_path);
    return rc;
  } catch (const std::exception& e) {
    std::cerr << "[AlbumEditorQtDemo] Fatal: " << e.what() << std::endl;
    return 1;
  }
}
