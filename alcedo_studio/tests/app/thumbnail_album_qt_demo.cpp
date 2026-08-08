//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <qfontdatabase.h>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleValidator>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QValidator>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "app/thumbnail_service.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "type/supported_file_type.hpp"

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
  candidates.emplace_back(std::filesystem::path(ALCEDO_SOURCE_DIR) / "alcedo" / "src" / "config" /
                          "fonts" / "main_IBM.ttf");
#endif

  for (const auto& path : candidates) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      continue;
    }

    const int font_id = QFontDatabase::addApplicationFont(FsPathToQString(path));
    if (font_id < 0) {
      std::cerr << "[ThumbnailAlbumQtDemo] Failed to load font: " << path.string() << std::endl;
      continue;
    }

    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty()) {
      std::cerr << "[ThumbnailAlbumQtDemo] Loaded font but no families reported: " << path.string()
                << std::endl;
      continue;
    }

    QFont f(families.front());
    app.setFont(f);
    return;
  }

  std::cerr << "[ThumbnailAlbumQtDemo] No external font applied. Provide `--font <path>` or set "
               "`ALCEDO_FONT_PATH`."
            << std::endl;
}

enum class FilterValueKind { String, Int64, Double, DateTime };

static FilterValueKind KindForField(FilterField field) {
  switch (field) {
    case FilterField::ExifISO:
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
    add_op(CompareOp::EQUALS, "=");
    add_op(CompareOp::NOT_EQUALS, "!=");
    add_op(CompareOp::CONTAINS, "contains");
    add_op(CompareOp::NOT_CONTAINS, "not contains");
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
  std::tm out{};
  out.tm_year = year - 1900;
  out.tm_mon  = month - 1;
  out.tm_mday = day;
  out.tm_hour = 0;
  out.tm_min  = 0;
  out.tm_sec  = 0;
  return out;
}

static std::optional<FilterValue> ParseFilterValue(FilterField field, const QString& text) {
  const auto kind = KindForField(field);
  if (kind == FilterValueKind::String) {
    return FilterValue{text.toStdWString()};
  }
  if (kind == FilterValueKind::Int64) {
    bool            ok = false;
    const qlonglong v  = text.trimmed().toLongLong(&ok);
    if (!ok) {
      return std::nullopt;
    }
    return FilterValue{static_cast<int64_t>(v)};
  }
  if (kind == FilterValueKind::Double) {
    bool         ok = false;
    const double v  = text.trimmed().toDouble(&ok);
    if (!ok) {
      return std::nullopt;
    }
    return FilterValue{v};
  }

  // DateTime
  auto tm_opt = ParseDateTimeYmd(text);
  if (!tm_opt.has_value()) {
    return std::nullopt;
  }
  return FilterValue{tm_opt.value()};
}

class FilterPanel final : public QGroupBox {
 public:
  explicit FilterPanel(QWidget* parent = nullptr) : QGroupBox("Filter", parent) {
    setObjectName("FilterPanel");
    setStyleSheet(FilterPanelStyleSheet());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 18, 12, 12);
    root->setSpacing(12);

    join_op_ = new QComboBox(this);
    join_op_->setToolTip("How to join multiple rules.");
    join_op_->addItem("AND", static_cast<int>(FilterOp::AND));
    join_op_->addItem("OR", static_cast<int>(FilterOp::OR));

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setContentsMargins(0, 0, 0, 0);

    form->addRow("Join", join_op_);
    root->addLayout(form, 1);

    rows_scroll_ = new QScrollArea(this);
    rows_scroll_->setWidgetResizable(true);
    rows_scroll_->setFrameShape(QFrame::NoFrame);
    rows_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    rows_widget_    = new QWidget(rows_scroll_);
    rows_container_ = new QVBoxLayout(rows_widget_);
    rows_container_->setContentsMargins(0, 0, 0, 0);
    rows_container_->setSpacing(8);
    rows_container_->setAlignment(Qt::AlignTop);
    rows_scroll_->setWidget(rows_widget_);
    root->addWidget(rows_scroll_, 3);

    auto* buttons = new QVBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);

    btn_add_   = new QPushButton("+ Add rule", this);
    btn_apply_ = new QPushButton("Apply", this);
    btn_clear_ = new QPushButton("Clear", this);
    btn_add_->setObjectName("AddButton");
    btn_apply_->setObjectName("ApplyButton");
    btn_clear_->setObjectName("ClearButton");

    btn_apply_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    btn_clear_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));

    buttons->addWidget(btn_add_);
    buttons->addWidget(btn_apply_);
    buttons->addWidget(btn_clear_);
    buttons->addStretch(1);

    rules_hint_ = new QLabel(this);
    rules_hint_->setObjectName("RulesHint");
    rules_hint_->setTextFormat(Qt::PlainText);
    buttons->addWidget(rules_hint_);

    info_ = new QLabel(this);
    info_->setObjectName("FilterInfo");
    info_->setTextFormat(Qt::PlainText);
    buttons->addWidget(info_);

    sql_preview_ = new QLabel(this);
    sql_preview_->setObjectName("SqlPreview");
    sql_preview_->setTextFormat(Qt::PlainText);
    sql_preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sql_preview_->setWordWrap(true);
    sql_preview_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    buttons->addWidget(sql_preview_);

    root->addLayout(buttons, 0);

    connect(btn_add_, &QPushButton::clicked, this, [this]() { AddRow(); });
    connect(btn_clear_, &QPushButton::clicked, this, [this]() {
      if (on_clear_) {
        on_clear_();
      }
    });
    connect(btn_apply_, &QPushButton::clicked, this, [this]() {
      auto root_node_opt = BuildFilterNode();
      if (!root_node_opt.has_value()) {
        return;
      }
      try {
        sql_preview_->setText(
            QString::fromStdString(FilterSQLCompiler::Compile(root_node_opt.value()).sql_));
      } catch (...) {
        sql_preview_->setText("(SQL compile failed)");
      }

      if (on_apply_) {
        on_apply_(root_node_opt.value());
      }
    });

    AddRow();
    UpdateControlsState();
  }

  void SetOnApply(std::function<void(const FilterNode&)> fn) { on_apply_ = std::move(fn); }
  void SetOnClear(std::function<void()> fn) { on_clear_ = std::move(fn); }

  void SetInfoText(const QString& text) { info_->setText(text); }
  void ClearSqlPreview() { sql_preview_->setText({}); }

 private:
  static QString FilterPanelStyleSheet() {
    // A small, self-contained dark "card" style for the demo UI.
    return QString::fromUtf8(R"QSS(
      #FilterPanel {
        background: #111315;
        border: 1px solid #2b2f33;
        border-radius: 12px;
        margin-top: 18px;
      }
      #FilterPanel::title {
        subcontrol-origin: margin;
        left: 12px;
        top: 6px;
        padding: 0 6px;
        color: #e8eaed;
        font-weight: 600;
      }
      #FilterPanel QLabel {
        color: #bdc1c6;
        font-weight: 400;
      }
      #FilterPanel QComboBox,
      #FilterPanel QLineEdit {
        background: #202124;
        border: 1px solid #3c4043;
        border-radius: 8px;
        padding: 6px 10px;
        color: #e8eaed;
        min-height: 28px;
      }
      #FilterPanel QComboBox::drop-down {
        border: none;
        width: 22px;
      }
      #FilterPanel QPushButton {
        background: #303134;
        border: 1px solid #3c4043;
        border-radius: 8px;
        padding: 6px 10px;
        color: #e8eaed;
        min-height: 30px;
      }
      #FilterPanel QPushButton:hover { background: #35363a; }
      #FilterPanel QPushButton:pressed { background: #2a2b2e; }
      #FilterPanel QPushButton#ApplyButton {
        background: #8ab4f8;
        color: #202124;
        border: 1px solid #8ab4f8;
        font-weight: 600;
      }
      #FilterPanel QPushButton#ApplyButton:hover { background: #a3c2ff; }
      #FilterPanel QWidget#FilterRow {
        background: #171a1d;
        border: 1px solid #2b2f33;
        border-radius: 10px;
      }
      #FilterPanel QToolButton#RemoveButton {
        background: transparent;
        border: 1px solid transparent;
        border-radius: 6px;
        padding: 4px;
        color: #bdc1c6;
        min-width: 28px;
        min-height: 28px;
      }
      #FilterPanel QToolButton#RemoveButton:hover {
        background: rgba(255, 255, 255, 0.06);
        border: 1px solid #3c4043;
      }
      #FilterPanel QLabel#FilterInfo { color: #9aa0a6; }
      #FilterPanel QLabel#RulesHint { color: #9aa0a6; }
      #FilterPanel QLabel#SqlPreview {
        color: #8ab4f8;
        background: #0f1113;
        border: 1px solid #2b2f33;
        border-radius: 10px;
        padding: 8px 10px;
      }
    )QSS");
  }

  static void ReplaceValidator(QLineEdit* edit, QValidator* next) {
    if (edit == nullptr) {
      return;
    }
    const QValidator* old = edit->validator();
    if (old != nullptr && old->parent() == edit) {
      const_cast<QValidator*>(old)->deleteLater();
    }
    edit->setValidator(next);
  }

  static QString PlaceholderForField(FilterField field) {
    switch (field) {
      case FilterField::FileExtension:
        return ".jpg";
      case FilterField::ExifISO:
        return "e.g. 800";
      case FilterField::ExifAperture:
        return "e.g. 2.8";
      case FilterField::ExifFocalLength:
        return "e.g. 50";
      case FilterField::CaptureDate:
      case FilterField::ImportDate:
        return "YYYY-MM-DD";
      case FilterField::ExifCameraModel:
      default:
        return "type to filter…";
    }
  }

  struct Row {
    QWidget*     container_ = nullptr;
    QComboBox*   field_     = nullptr;
    QComboBox*   op_        = nullptr;
    QLineEdit*   value_     = nullptr;
    QLineEdit*   value2_    = nullptr;
    QToolButton* remove_    = nullptr;
  };

  void ConfigureEditorsForRow(Row& row, FilterField field) {
    const auto kind = KindForField(field);

    row.value_->setInputMask({});
    row.value2_->setInputMask({});
    ReplaceValidator(row.value_, nullptr);
    ReplaceValidator(row.value2_, nullptr);

    row.value_->setPlaceholderText(PlaceholderForField(field));
    row.value2_->setPlaceholderText("and …");

    if (kind == FilterValueKind::Int64) {
      ReplaceValidator(row.value_, new QIntValidator(0, 1'000'000, row.value_));
      ReplaceValidator(row.value2_, new QIntValidator(0, 1'000'000, row.value2_));
    } else if (kind == FilterValueKind::Double) {
      auto* v1 = new QDoubleValidator(row.value_);
      v1->setNotation(QDoubleValidator::StandardNotation);
      v1->setDecimals(6);
      ReplaceValidator(row.value_, v1);

      auto* v2 = new QDoubleValidator(row.value2_);
      v2->setNotation(QDoubleValidator::StandardNotation);
      v2->setDecimals(6);
      ReplaceValidator(row.value2_, v2);
    } else if (kind == FilterValueKind::DateTime) {
      row.value_->setInputMask("0000-00-00");
      row.value2_->setInputMask("0000-00-00");
    }
  }

  void UpdateControlsState() {
    btn_add_->setEnabled(rows_.size() < 6);

    const bool can_remove = rows_.size() > 1;
    for (auto& row : rows_) {
      row.remove_->setEnabled(can_remove);
    }

    rules_hint_->setText(QString("%1 / 6 rules").arg(static_cast<int>(rows_.size())));
  }

  void AddRow() {
    if (rows_.size() >= 6) {
      return;
    }

    Row r;
    r.container_ = new QWidget(rows_widget_);
    r.container_->setObjectName("FilterRow");
    auto* h = new QHBoxLayout(r.container_);
    h->setContentsMargins(10, 8, 10, 8);
    h->setSpacing(8);

    r.field_ = new QComboBox(r.container_);
    r.field_->addItem("Camera Model", static_cast<int>(FilterField::ExifCameraModel));
    r.field_->addItem("File Extension", static_cast<int>(FilterField::FileExtension));
    r.field_->addItem("ISO", static_cast<int>(FilterField::ExifISO));
    r.field_->addItem("Aperture", static_cast<int>(FilterField::ExifAperture));
    r.field_->addItem("Focal Length", static_cast<int>(FilterField::ExifFocalLength));
    r.field_->addItem("Capture Date (YYYY-MM-DD)", static_cast<int>(FilterField::CaptureDate));
    r.field_->addItem("Import Date (YYYY-MM-DD)", static_cast<int>(FilterField::ImportDate));
    r.op_ = new QComboBox(r.container_);
    PopulateCompareOps(r.op_, FilterField::ExifCameraModel);

    r.value_ = new QLineEdit(r.container_);
    r.value_->setClearButtonEnabled(true);
    r.value2_ = new QLineEdit(r.container_);
    r.value2_->setClearButtonEnabled(true);
    r.value2_->setPlaceholderText("and …");
    r.value2_->setVisible(false);

    r.remove_ = new QToolButton(r.container_);
    r.remove_->setObjectName("RemoveButton");
    r.remove_->setAutoRaise(true);
    r.remove_->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    r.remove_->setToolTip("Remove rule");

    h->addWidget(r.field_, 2);
    h->addWidget(r.op_, 1);
    h->addWidget(r.value_, 2);
    h->addWidget(r.value2_, 2);
    h->addWidget(r.remove_, 0);

    rows_container_->addWidget(r.container_);
    rows_.push_back(r);

    auto* row_container = r.container_;
    auto* field_box     = r.field_;
    auto* op_box        = r.op_;
    auto* value2_edit   = r.value2_;

    ConfigureEditorsForRow(rows_.back(), FilterField::ExifCameraModel);

    const auto find_row_index = [this, row_container]() -> std::optional<size_t> {
      for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].container_ == row_container) {
          return i;
        }
      }
      return std::nullopt;
    };

    connect(field_box, &QComboBox::currentIndexChanged, this, [this, find_row_index](int) {
      const auto idx_opt = find_row_index();
      if (!idx_opt.has_value()) {
        return;
      }
      auto&      row   = rows_[idx_opt.value()];
      const auto field = static_cast<FilterField>(row.field_->currentData().toInt());
      PopulateCompareOps(row.op_, field);
      ConfigureEditorsForRow(row, field);

      const auto op = static_cast<CompareOp>(row.op_->currentData().toInt());
      row.value2_->setVisible(op == CompareOp::BETWEEN);
      if (op != CompareOp::BETWEEN) {
        row.value2_->clear();
      }
    });

    connect(op_box, &QComboBox::currentIndexChanged, this,
            [this, find_row_index, value2_edit](int) {
              const auto idx_opt = find_row_index();
              if (!idx_opt.has_value()) {
                return;
              }
              auto&      row = rows_[idx_opt.value()];
              const auto op  = static_cast<CompareOp>(row.op_->currentData().toInt());
              value2_edit->setVisible(op == CompareOp::BETWEEN);
              if (op != CompareOp::BETWEEN) {
                value2_edit->clear();
              }
            });

    // Initialize BETWEEN visibility.
    {
      const auto op = static_cast<CompareOp>(r.op_->currentData().toInt());
      r.value2_->setVisible(op == CompareOp::BETWEEN);
    }

    connect(r.value_, &QLineEdit::returnPressed, this, [this]() { btn_apply_->click(); });
    connect(r.value2_, &QLineEdit::returnPressed, this, [this]() { btn_apply_->click(); });

    connect(r.remove_, &QToolButton::clicked, this, [this, row_container, find_row_index]() {
      if (rows_.size() <= 1) {
        return;
      }
      const auto idx_opt = find_row_index();
      if (!idx_opt.has_value()) {
        return;
      }
      rows_.erase(rows_.begin() + static_cast<long long>(idx_opt.value()));
      delete row_container;
      UpdateControlsState();
    });

    UpdateControlsState();
  }

  std::optional<FilterNode> BuildFilterNode() {
    std::vector<FilterNode> conditions;
    conditions.reserve(rows_.size());

    for (const auto& row : rows_) {
      const auto field  = static_cast<FilterField>(row.field_->currentData().toInt());
      const auto op     = static_cast<CompareOp>(row.op_->currentData().toInt());

      const auto v1_opt = ParseFilterValue(field, row.value_->text());
      if (!v1_opt.has_value()) {
        QMessageBox::warning(this, "Filter", "Invalid value. Check field type and input.");
        return std::nullopt;
      }

      FieldCondition cond{
          .field_ = field, .op_ = op, .value_ = v1_opt.value(), .second_value_ = std::nullopt};

      if (op == CompareOp::BETWEEN) {
        const auto v2_opt = ParseFilterValue(field, row.value2_->text());
        if (!v2_opt.has_value()) {
          QMessageBox::warning(this, "Filter", "Invalid second value for BETWEEN.");
          return std::nullopt;
        }
        cond.second_value_ = v2_opt.value();
      }

      conditions.push_back(
          FilterNode{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt});
    }

    if (conditions.empty()) {
      QMessageBox::information(this, "Filter", "No conditions.");
      return std::nullopt;
    }

    if (conditions.size() == 1) {
      return conditions.front();
    }

    const auto join = static_cast<FilterOp>(join_op_->currentData().toInt());
    return FilterNode{FilterNode::Type::Logical, join, std::move(conditions), {}, std::nullopt};
  }

  QComboBox*                             join_op_        = nullptr;
  QScrollArea*                           rows_scroll_    = nullptr;
  QWidget*                               rows_widget_    = nullptr;
  QVBoxLayout*                           rows_container_ = nullptr;
  QPushButton*                           btn_add_        = nullptr;
  QPushButton*                           btn_apply_      = nullptr;
  QPushButton*                           btn_clear_      = nullptr;
  QLabel*                                rules_hint_     = nullptr;
  QLabel*                                info_           = nullptr;
  QLabel*                                sql_preview_    = nullptr;
  std::vector<Row>                       rows_;

  std::function<void(const FilterNode&)> on_apply_;
  std::function<void()>                  on_clear_;
};

class BackgroundExecutor final {
 public:
  explicit BackgroundExecutor(size_t thread_count) : stop_(false), threads_() {
    thread_count = std::max<size_t>(1, thread_count);
    threads_.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
      threads_.emplace_back([this]() { WorkerLoop(); });
    }
  }

  ~BackgroundExecutor() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  BackgroundExecutor(const BackgroundExecutor&)            = delete;
  BackgroundExecutor& operator=(const BackgroundExecutor&) = delete;

  void                Post(std::function<void()> fn) {
    if (!fn) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
  }

 private:
  void WorkerLoop() {
    for (;;) {
      std::function<void()> fn;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
        if (stop_ && queue_.empty()) {
          return;
        }
        fn = std::move(queue_.front());
        queue_.pop_front();
      }
      try {
        fn();
      } catch (...) {
      }
    }
  }

  std::mutex                        mu_;
  std::condition_variable           cv_;
  std::deque<std::function<void()>> queue_;
  bool                              stop_;
  std::vector<std::thread>          threads_;
};

static BackgroundExecutor& Bg() {
  static BackgroundExecutor exec(std::max<unsigned>(2u, std::thread::hardware_concurrency() / 2));
  return exec;
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
    // Best-effort conversion.
    cv::Mat tmp;
    rgba32f_or_u8.convertTo(tmp, CV_8UC4);
    rgba8 = tmp;
  }

  if (!rgba8.isContinuous()) {
    rgba8 = rgba8.clone();
  }

  // Assume RGBA byte order.
  QImage img(rgba8.data, rgba8.cols, rgba8.rows, static_cast<int>(rgba8.step),
             QImage::Format_RGBA8888);
  return img.copy();
}

struct AlbumIds {
  std::vector<std::pair<sl_element_id_t, image_id_t>> ids;
};

static AlbumIds ImportBatchToTempProject(const std::filesystem::path& db_path,
                                         const std::filesystem::path& meta_path) {
  ProjectService            project(db_path, meta_path);
  auto                      fs_service = project.GetSleeveService();
  auto                      img_pool   = project.GetImagePoolService();
  ImportServiceImpl         import_service(fs_service, img_pool);

  std::filesystem::path     img_root_path = {TEST_IMG_PATH "/raw/batch_import"};
  std::vector<image_path_t> paths;
  for (const auto& entry : std::filesystem::directory_iterator(img_root_path)) {
    if (entry.is_regular_file() && is_supported_file(entry.path())) {
      paths.push_back(entry.path());
    }
  }
  if (paths.empty()) {
    throw std::runtime_error("No supported images found under TEST_IMG_PATH/raw/batch_import");
  }

  auto                       import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> done;
  auto                       fut = done.get_future();
  import_job->on_finished_       = [&done](const ImportResult& r) { done.set_value(r); };

  import_job                     = import_service.ImportToFolder(paths, L"", {}, import_job);
  if (!import_job) {
    throw std::runtime_error("ImportToFolder returned null job");
  }
  if (fut.wait_for(120s) != std::future_status::ready) {
    throw std::runtime_error("Import did not finish in time");
  }

  const auto result = fut.get();
  if (result.failed_ != 0u) {
    throw std::runtime_error("Import failed for some images");
  }

  if (!import_job->import_log_) {
    throw std::runtime_error("Import log is null");
  }
  auto snapshot = import_job->import_log_->Snapshot();
  if (snapshot.created_.empty()) {
    throw std::runtime_error("No created entries in import snapshot");
  }

  AlbumIds     out;
  const size_t count = std::min<size_t>(256, snapshot.created_.size());
  out.ids.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.ids.push_back({snapshot.created_[i].element_id_, snapshot.created_[i].image_id_});
  }

  import_service.SyncImports(snapshot, L"");
  project.GetSleeveService()->Sync();
  project.GetImagePoolService()->SyncWithStorage();
  project.SaveProject(meta_path);

  return out;
}

class AlbumWidget final : public QWidget {
 public:
  AlbumWidget(std::shared_ptr<ThumbnailService>                   thumbnail_service,
              std::shared_ptr<SleeveServiceImpl>                  sleeve_service,
              std::shared_ptr<StorageService>                     storage_service,
              std::vector<std::pair<sl_element_id_t, image_id_t>> ids, QWidget* parent = nullptr)
      : QWidget(parent),
        service_(std::move(thumbnail_service)),
        sleeve_(std::move(sleeve_service)),
        filter_service_(std::move(storage_service)),
        base_ids_(std::move(ids)),
        ids_(base_ids_) {
    if (!service_) {
      throw std::runtime_error("ThumbnailService is null");
    }
    if (!sleeve_) {
      throw std::runtime_error("SleeveService is null");
    }
    if (base_ids_.empty()) {
      throw std::runtime_error("No ids to display");
    }

    element_to_image_.reserve(base_ids_.size());
    for (const auto& p : base_ids_) {
      element_to_image_.insert({p.first, p.second});
    }

    auto* root    = new QHBoxLayout(this);

    filter_panel_ = new FilterPanel(this);
    filter_panel_->SetInfoText(
        QString("Showing %1 images").arg(static_cast<qulonglong>(ids_.size())));
    filter_panel_->SetOnApply([this](const FilterNode& node) { ApplyFilter(node); });
    filter_panel_->SetOnClear([this]() { ClearFilter(); });
    root->addWidget(filter_panel_, 0);

    scrollbar_ = new QScrollBar(Qt::Vertical, this);
    connect(scrollbar_, &QScrollBar::valueChanged, this, [this](int v) {
      start_ = static_cast<size_t>(std::max(0, v));
      RebindAllCells();
      Prefetch();
    });

    auto* content = new QWidget(this);
    grid_         = new QGridLayout(content);
    grid_->setSpacing(6);
    grid_->setContentsMargins(6, 6, 6, 6);

    auto* h = new QHBoxLayout();
    h->addWidget(content, 1);
    h->addWidget(scrollbar_, 0);
    root->addLayout(h, 1);

    InitCells();
    UpdateScrollbar();
    RebindAllCells();
    Prefetch();
  }

  ~AlbumWidget() override { ReleaseAllVisible(); }

 protected:
  void wheelEvent(QWheelEvent* event) override {
    const int steps = event->angleDelta().y() / 120;
    if (steps != 0) {
      const int delta = -steps * static_cast<int>(columns_);  // scroll by a row
      const int next =
          std::clamp(scrollbar_->value() + delta, scrollbar_->minimum(), scrollbar_->maximum());
      scrollbar_->setValue(next);
    }
    event->accept();
  }

 private:
  struct Cell {
    QLabel*                         label            = nullptr;
    size_t                          bound_idx        = static_cast<size_t>(-1);
    sl_element_id_t                 bound_element_id = 0;
    uint64_t                        generation       = 0;
    std::shared_ptr<ThumbnailGuard> guard;
  };

  void SetDisplayedIds(std::vector<std::pair<sl_element_id_t, image_id_t>> next) {
    ReleaseAllVisible();
    prefetch_inflight_idx_.clear();

    ids_   = std::move(next);
    start_ = 0;

    {
      QSignalBlocker block(*scrollbar_);
      UpdateScrollbar();
      scrollbar_->setValue(0);
    }
    RebindAllCells();
    Prefetch();

    if (filter_panel_) {
      filter_panel_->SetInfoText(
          QString("Showing %1 images").arg(static_cast<qulonglong>(ids_.size())));
    }
  }

  void ClearFilter() {
    if (filter_panel_) {
      filter_panel_->ClearSqlPreview();
    }

    if (active_filter_id_.has_value()) {
      filter_service_.RemoveFilterCombo(active_filter_id_.value());
      active_filter_id_.reset();
    }
    SetDisplayedIds(base_ids_);
  }

  void ApplyFilter(const FilterNode& node) {
    if (!sleeve_) {
      return;
    }

    // Filters are immutable and results are cached by filter_id_. Always create a new filter.
    const filter_id_t new_filter_id = filter_service_.CreateFilterCombo(node);
    const auto        filter_opt    = filter_service_.GetFilterCombo(new_filter_id);
    if (!filter_opt.has_value() || !filter_opt.value()) {
      QMessageBox::warning(this, "Filter", "Filter creation failed.");
      return;
    }

    // Replace previous filter to avoid unbounded growth of in-memory caches.
    if (active_filter_id_.has_value()) {
      filter_service_.RemoveFilterCombo(active_filter_id_.value());
    }
    active_filter_id_ = new_filter_id;

    std::vector<sl_element_id_t> filtered_ids;
    try {
      // Root folder id is 0.
      const auto ids_opt = filter_service_.ApplyFilterOn(new_filter_id, /*parent_id=*/0);
      if (!ids_opt.has_value()) {
        QMessageBox::warning(this, "Filter", "Unknown filter id.");
        return;
      }
      filtered_ids = std::move(ids_opt.value());
    } catch (const std::exception& e) {
      QMessageBox::warning(this, "Filter", QString("Filter failed: %1").arg(e.what()));
      return;
    }

    std::vector<std::pair<sl_element_id_t, image_id_t>> next_ids;
    next_ids.reserve(filtered_ids.size());
    for (const auto& element_id : filtered_ids) {
      const auto it = element_to_image_.find(element_id);
      if (it == element_to_image_.end()) {
        continue;
      }
      next_ids.push_back({element_id, it->second});
    }

    if (next_ids.empty()) {
      QMessageBox::information(this, "Filter", "No matches.");
    }
    SetDisplayedIds(std::move(next_ids));
  }

  void InitCells() {
    cells_.resize(view_size_);

    for (size_t i = 0; i < view_size_; ++i) {
      auto* label = new QLabel(this);
      label->setMinimumSize(cell_w_, cell_h_);
      label->setAlignment(Qt::AlignCenter);
      label->setStyleSheet(
          "QLabel { background: #202124; color: #bdbdbd; border: 1px solid #3c4043; }");
      label->setText("(empty)");
      cells_[i].label = label;

      const int r     = static_cast<int>(i / columns_);
      const int c     = static_cast<int>(i % columns_);
      grid_->addWidget(label, r, c);
    }
  }

  void UpdateScrollbar() {
    const size_t window    = std::min(view_size_, ids_.size());
    const size_t max_start = (ids_.size() > window) ? (ids_.size() - window) : 0;
    scrollbar_->setRange(0, static_cast<int>(max_start));
    scrollbar_->setPageStep(static_cast<int>(columns_));
    scrollbar_->setSingleStep(1);
  }

  void ReleaseAllVisible() {
    for (auto& cell : cells_) {
      ReleaseCell(cell);
    }
  }

  void ReleaseCell(Cell& cell) {
    if (cell.guard && cell.bound_element_id != 0) {
      try {
        service_->ReleaseThumbnail(cell.bound_element_id);
      } catch (...) {
      }
    }
    cell.guard.reset();
    cell.bound_idx        = static_cast<size_t>(-1);
    cell.bound_element_id = 0;
    cell.generation++;
  }

  void RebindAllCells() {
    const size_t window    = std::min(view_size_, ids_.size());
    const size_t max_start = (ids_.size() > window) ? (ids_.size() - window) : 0;
    start_                 = std::min(start_, max_start);

    for (size_t pos = 0; pos < window; ++pos) {
      auto&        cell     = cells_[pos];
      const size_t want_idx = start_ + pos;

      if (cell.bound_idx == want_idx) {
        continue;
      }

      ReleaseCell(cell);
      cell.bound_idx        = want_idx;
      cell.bound_element_id = ids_[want_idx].first;
      cell.label->setText(QString("Loading: #%1").arg(static_cast<qulonglong>(want_idx)));
      cell.label->setPixmap({});

      RequestForCell(pos, want_idx, /*pin=*/true);
    }

    // If ids_ smaller than view, clear remaining.
    for (size_t pos = window; pos < view_size_; ++pos) {
      auto& cell = cells_[pos];
      ReleaseCell(cell);
      cell.label->setText("(empty)");
      cell.label->setPixmap({});
    }
  }

  void RequestForCell(size_t cell_pos, size_t idx, bool pin) {
    if (cell_pos >= cells_.size()) {
      return;
    }

    auto                  svc = service_;
    QPointer<AlbumWidget> self(this);

    auto&                 cell          = cells_[cell_pos];
    const auto            element_id    = ids_[idx].first;
    const auto            image_id      = ids_[idx].second;
    const uint64_t        gen           = ++cell.generation;

    // Marshal work to the Qt UI thread.
    CallbackDispatcher    ui_dispatcher = [](std::function<void()> fn) {
      auto* obj = QCoreApplication::instance();
      if (!obj) {
        fn();
        return;
      }
      QMetaObject::invokeMethod(obj, std::move(fn), Qt::QueuedConnection);
    };

    svc->GetThumbnail(
        element_id, image_id,
        [self, svc, cell_pos, idx, element_id, gen,
         ui_dispatcher](std::shared_ptr<ThumbnailGuard> guard) {
          // UI thread callback must stay tiny: validate, store guard, then offload heavy work.
          if (!guard) {
            return;
          }

          if (!self) {
            try {
              svc->ReleaseThumbnail(element_id);
            } catch (...) {
            }
            return;
          }

          if (cell_pos >= self->cells_.size()) {
            try {
              svc->ReleaseThumbnail(element_id);
            } catch (...) {
            }
            return;
          }

          auto&      cell       = self->cells_[cell_pos];
          const bool still_same = (cell.bound_idx == idx) &&
                                  (cell.bound_element_id == element_id) && (cell.generation == gen);
          if (!still_same) {
            try {
              svc->ReleaseThumbnail(element_id);
            } catch (...) {
            }
            return;
          }

          cell.guard = guard;
          if (!guard->thumbnail_buffer_) {
            cell.label->setText("(no buffer)");
            return;
          }

          const int target_w = self->cell_w_;
          const int target_h = self->cell_h_;

          // Do CPU sync + Mat->QImage + scaling off the UI thread.
          Bg().Post([self, svc, cell_pos, idx, element_id, gen, guard, ui_dispatcher, target_w,
                     target_h]() mutable {
            QImage scaled;

            try {
              auto* buffer = guard->thumbnail_buffer_.get();
              if (!buffer) {
                return;
              }

              if (!buffer->cpu_data_valid_ && buffer->gpu_data_valid_) {
                buffer->SyncToCPU();
              }

              if (!buffer->cpu_data_valid_) {
                // No CPU data to render.
                ui_dispatcher([self, svc, cell_pos, idx, element_id, gen, guard]() {
                  if (!self || cell_pos >= self->cells_.size()) {
                    return;
                  }
                  auto&      cell       = self->cells_[cell_pos];
                  const bool still_same = (cell.bound_idx == idx) &&
                                          (cell.bound_element_id == element_id) &&
                                          (cell.generation == gen);
                  if (!still_same) {
                    return;
                  }
                  cell.label->setText("(no CPU data)");
                });
                return;
              }

              auto&  mat = buffer->GetCPUData();
              QImage img = MatRgba32fToQImageCopy(mat);
              if (!img.isNull()) {
                scaled =
                    img.scaled(target_w, target_h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
              }
            } catch (...) {
              ui_dispatcher([self, cell_pos, idx, element_id, gen]() {
                if (!self || cell_pos >= self->cells_.size()) {
                  return;
                }
                auto&      cell       = self->cells_[cell_pos];
                const bool still_same = (cell.bound_idx == idx) &&
                                        (cell.bound_element_id == element_id) &&
                                        (cell.generation == gen);
                if (!still_same) {
                  return;
                }
                cell.label->setText("(render failed)");
              });
              return;
            }

            // Final UI update: keep it to (check + setPixmap).
            ui_dispatcher([self, svc, cell_pos, idx, element_id, gen, guard,
                           scaled = std::move(scaled)]() mutable {
              if (!self) {
                try {
                  svc->ReleaseThumbnail(element_id);
                } catch (...) {
                }
                return;
              }
              if (cell_pos >= self->cells_.size()) {
                try {
                  svc->ReleaseThumbnail(element_id);
                } catch (...) {
                }
                return;
              }

              auto&      cell       = self->cells_[cell_pos];
              const bool still_same = (cell.bound_idx == idx) &&
                                      (cell.bound_element_id == element_id) &&
                                      (cell.generation == gen);
              if (!still_same) {
                // Cell was rebound; let normal rebind/release path handle pins.
                return;
              }

              if (scaled.isNull()) {
                cell.label->setText("(empty image)");
                return;
              }

              cell.label->setPixmap(QPixmap::fromImage(std::move(scaled)));
            });
          });
        },
        pin, ui_dispatcher);
  }

  void Prefetch() {
    const size_t window    = std::min(view_size_, ids_.size());
    const size_t max_start = (ids_.size() > window) ? (ids_.size() - window) : 0;
    start_                 = std::min(start_, max_start);

    const size_t begin     = (start_ > prefetch_each_side_) ? (start_ - prefetch_each_side_) : 0;
    const size_t end       = std::min(ids_.size(), start_ + window + prefetch_each_side_);

    // Prefetch: request non-visible indices with pin=false.
    // IMPORTANT: do NOT blindly ReleaseThumbnail() on completion, because the same in-flight
    // request can later become visible and be joined via pending_ (sharing the guard).
    for (size_t idx = begin; idx < end; ++idx) {
      if (idx >= start_ && idx < (start_ + window)) {
        continue;
      }

      if (prefetch_inflight_idx_.size() >= max_prefetch_inflight_) {
        break;
      }

      if (prefetch_inflight_idx_.contains(idx)) {
        continue;
      }

      const auto            element_id = ids_[idx].first;
      const auto            image_id   = ids_[idx].second;

      auto                  svc        = service_;
      QPointer<AlbumWidget> self(this);

      prefetch_inflight_idx_.insert(idx);

      CallbackDispatcher dispatcher = [](std::function<void()> fn) {
        auto* obj = QCoreApplication::instance();
        if (!obj) {
          fn();
          return;
        }
        QMetaObject::invokeMethod(obj, std::move(fn), Qt::QueuedConnection);
      };

      svc->GetThumbnail(
          element_id, image_id,
          [self, svc, idx, element_id](std::shared_ptr<ThumbnailGuard> guard) {
            (void)guard;
            if (!self) {
              try {
                svc->ReleaseThumbnail(element_id);
              } catch (...) {
              }
              return;
            }

            self->prefetch_inflight_idx_.erase(idx);

            const size_t window      = std::min(self->view_size_, self->ids_.size());
            const bool   visible_now = idx >= self->start_ && idx < (self->start_ + window);
            if (!visible_now) {
              try {
                svc->ReleaseThumbnail(element_id);
              } catch (...) {
              }
            }
          },
          /*pin_if_found=*/false, dispatcher);
    }
  }

  std::shared_ptr<ThumbnailService>                   service_;
  std::shared_ptr<SleeveServiceImpl>                  sleeve_;

  SleeveFilterService                                 filter_service_;
  std::optional<filter_id_t>                          active_filter_id_;

  std::vector<std::pair<sl_element_id_t, image_id_t>> base_ids_;
  std::vector<std::pair<sl_element_id_t, image_id_t>> ids_;

  std::unordered_map<sl_element_id_t, image_id_t>     element_to_image_;

  FilterPanel*                                        filter_panel_ = nullptr;

  QGridLayout*                                        grid_         = nullptr;
  QScrollBar*                                         scrollbar_    = nullptr;
  std::vector<Cell>                                   cells_;

  size_t                                              start_     = 0;

  const size_t                                        columns_   = 4;
  const size_t                                        view_size_ = 12;  // 3x4
  const size_t               prefetch_each_side_                 = 7;   // match fuzz test idea

  const size_t               max_prefetch_inflight_              = 12;
  std::unordered_set<size_t> prefetch_inflight_idx_;

  const int                  cell_w_ = 220;
  const int                  cell_h_ = 160;
};

}  // namespace
}  // namespace alcedo

int main(int argc, char** argv) {
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
  alcedo::RegisterAllOperators();

  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
      Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

  QApplication app(argc, argv);
  alcedo::ApplyExternalAppFont(app, argc, argv);

  try {
    const auto db_path   = std::filesystem::temp_directory_path() / "thumbnail_album_demo.db";
    const auto meta_path = std::filesystem::temp_directory_path() / "thumbnail_album_demo.json";

    // Fresh temp project each run.
    if (std::filesystem::exists(db_path)) {
      std::filesystem::remove(db_path);
    }
    if (std::filesystem::exists(meta_path)) {
      std::filesystem::remove(meta_path);
    }

    auto                     imported = alcedo::ImportBatchToTempProject(db_path, meta_path);

    alcedo::ProjectService project(db_path, meta_path);
    auto                     img_pool = project.GetImagePoolService();
    auto                     pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorageService());

    auto thumbnail_service = std::make_shared<alcedo::ThumbnailService>(
        project.GetSleeveService(), img_pool, pipeline_service);

    auto* w = new alcedo::AlbumWidget(thumbnail_service, project.GetSleeveService(),
                                        project.GetStorageService(), std::move(imported.ids));
    w->setWindowTitle("alcedo - Thumbnail Album Qt Demo");
    w->resize(1400, 900);
    w->show();

    const int rc = app.exec();

    pipeline_service->Sync();
    img_pool->SyncWithStorage();
    project.SaveProject(meta_path);

    return rc;
  } catch (const std::exception& e) {
    std::cerr << "[ThumbnailAlbumQtDemo] Fatal: " << e.what() << std::endl;
    return 1;
  }
}
