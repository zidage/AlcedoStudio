//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/preset_controller.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/adjustment_transfer_catalog.hpp"
#include "app/adjustment_transfer_package_builder.hpp"
#include "app/preset_file.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

constexpr auto kBrowserHeightKey   = "presets/browserHeight";
constexpr auto kBrowserViewModeKey = "presets/browserViewMode";

auto           IsBrowserViewMode(const QString& view_mode) -> bool {
  return view_mode == QStringLiteral("grid") || view_mode == QStringLiteral("list");
}

auto           ErrorResult(const QString& message) -> QVariantMap {
  return {{"success", false}, {"message", message}};
}

auto SuccessResult(const QString& message = {}) -> QVariantMap {
  QVariantMap result{{"success", true}};
  if (!message.isEmpty()) {
    result.insert("message", message);
  }
  return result;
}

auto OperationName(alcedo::PresetLibraryService::Operation operation) -> QString {
  using Operation = alcedo::PresetLibraryService::Operation;
  switch (operation) {
    case Operation::kScan:
      return QStringLiteral("scan");
    case Operation::kUseRoot:
      return QStringLiteral("useRoot");
    case Operation::kSave:
      return QStringLiteral("save");
    case Operation::kAdd:
      return QStringLiteral("add");
    case Operation::kRename:
      return QStringLiteral("rename");
    case Operation::kDelete:
      return QStringLiteral("delete");
    case Operation::kNone:
      break;
  }
  return {};
}

/// The local path of a QML file value: a `file:` URL, a QUrl, or a native path.
auto LocalPath(const QVariant& value) -> std::filesystem::path {
  QString text;
  if (value.metaType().id() == QMetaType::QUrl) {
    text = value.toUrl().toLocalFile();
  } else {
    text = value.toString();
    if (text.startsWith(QStringLiteral("file:"))) {
      text = QUrl(text).toLocalFile();
    }
  }
  if (text.isEmpty()) {
    return {};
  }
  return std::filesystem::path(text.toStdWString());
}

}  // namespace

PresetController::PresetController(alcedo::PresetLibraryService*  library,
                                   AdjustmentTransferDialogModel* copy_dialog, OpenUrl open_url,
                                   QObject* parent)
    : QObject(parent),
      library_(library),
      copy_dialog_(copy_dialog),
      open_url_(std::move(open_url)),
      presets_(new PresetListModel(library, this)) {
  if (!open_url_) {
    open_url_ = [](const QUrl& url) { return QDesktopServices::openUrl(url); };
  }
  browser_height_ = QSettings().value(QString::fromLatin1(kBrowserHeightKey), 0.0).toDouble();
  browser_view_mode_ = QSettings().value(QString::fromLatin1(kBrowserViewModeKey)).toString();
  if (!IsBrowserViewMode(browser_view_mode_)) {
    browser_view_mode_ = QStringLiteral("grid");
  }
  if (library_ != nullptr) {
    connect(library_, &alcedo::PresetLibraryService::OperationFinished, this,
            &PresetController::PublishOperation);
  }
  connect(this, &PresetController::OperationFinished, this, &PresetController::operationFinished);
}

void PresetController::SetBrowserHeight(double height) {
  if (height < 0.0 || height == browser_height_) {
    return;
  }
  browser_height_ = height;
  QSettings().setValue(QString::fromLatin1(kBrowserHeightKey), height);
  emit BrowserHeightChanged();
}

void PresetController::SetBrowserViewMode(const QString& view_mode) {
  if (!IsBrowserViewMode(view_mode) || view_mode == browser_view_mode_) {
    return;
  }
  browser_view_mode_ = view_mode;
  QSettings().setValue(QString::fromLatin1(kBrowserViewModeKey), view_mode);
  emit BrowserViewModeChanged();
}

void PresetController::SetEditorSession(EditorSessionController* editor_session) {
  editor_session_ = editor_session;
}

void PresetController::SetAdjustmentTransfer(AdjustmentTransferController* adjustment_transfer) {
  adjustment_transfer_ = adjustment_transfer;
}

auto PresetController::SaveVersionAsPreset(const QString& versionId) -> QVariantMap {
  if (library_ == nullptr) {
    return ErrorResult(Tr("The preset library is unavailable."));
  }
  if (editor_session_ == nullptr || !editor_session_->has_image()) {
    return ErrorResult(Tr("No photo is open."));
  }
  alcedo::version_ref_id_t version_id;
  try {
    version_id = alcedo::version_ref_id_t::FromString(versionId.toStdString());
  } catch (...) {
    return ErrorResult(Tr("Source Version identity is invalid."));
  }
  std::shared_ptr<const alcedo::CommitGraph>      graph;
  std::shared_ptr<const alcedo::PipelineDocument> root_document;
  std::string                                     error;
  if (!editor_session_->SnapshotHistorySource(&graph, &root_document, &error)) {
    return ErrorResult(QString::fromStdString(error));
  }
  // The replay reads the Version from the immutable history; the active Version stays.
  auto read = alcedo::AdjustmentTransferCatalogService::ReadVersion(*graph, *root_document,
                                                                    version_id, &error);
  if (!read.has_value()) {
    return ErrorResult(QString::fromStdString(error));
  }
  const auto selection = alcedo::SelectPresetItems(read->document);
  if (selection.nodes.empty()) {
    return ErrorResult(Tr("No transferable adjustments."));
  }
  try {
    const auto package = alcedo::AdjustmentTransferPackageBuilder::Build(read->document, selection);
    return SavePackage(package, QString::fromStdString(read->version.display_name));
  } catch (const std::exception& e) {
    return ErrorResult(QString::fromUtf8(e.what()));
  }
}

auto PresetController::SaveDialogSelectionAsPreset() -> QVariantMap {
  if (copy_dialog_ == nullptr) {
    return ErrorResult(Tr("No source Version is loaded."));
  }
  std::string error;
  const auto  package = copy_dialog_->BuildPackage(&error);
  if (!package.has_value()) {
    return ErrorResult(error.empty() ? Tr("No transferable adjustments.")
                                     : QString::fromStdString(error));
  }
  return SavePackage(*package, copy_dialog_->selected_version_name());
}

auto PresetController::SavePackage(const alcedo::AdjustmentTransferPackage& package,
                                   const QString&                           label) -> QVariantMap {
  if (library_ == nullptr) {
    return ErrorResult(Tr("The preset library is unavailable."));
  }
  const auto name   = alcedo::PresetNameFromLabel(label.toStdString());
  const auto status = library_->Save(package, name);
  if (status != alcedo::PresetLibraryService::Status::kOk) {
    return ErrorResult(QString::fromStdString(library_->LastResult().message));
  }
  QVariantMap result = SuccessResult();
  result.insert("pending", true);
  return result;
}

auto PresetController::AddFiles(const QVariantList& urls) -> QVariantMap {
  if (library_ == nullptr) {
    return ErrorResult(Tr("The preset library is unavailable."));
  }
  std::vector<std::filesystem::path> paths;
  for (const auto& url : urls) {
    auto path = LocalPath(url);
    if (!path.empty()) {
      paths.push_back(std::move(path));
    }
  }
  if (paths.empty()) {
    return ErrorResult(Tr("No preset file selected."));
  }
  library_->Add(std::move(paths));
  QVariantMap result = SuccessResult();
  result.insert("pending", true);
  return result;
}

auto PresetController::ListedPackage(const QString& presetId, QString* name, QString* error)
    -> std::shared_ptr<const alcedo::AdjustmentTransferPackage> {
  if (library_ == nullptr) {
    *error = Tr("The preset library is unavailable.");
    return nullptr;
  }
  const auto  id    = presetId.toStdString();
  const auto* entry = library_->FindEntry(id);
  if (entry == nullptr) {
    *error = Tr("The preset is not in the preset folder.");
    return nullptr;
  }
  if (!entry->available) {
    *error = QString::fromStdString(entry->reason);
    return nullptr;
  }
  *name              = QString::fromStdString(entry->name);
  const auto package = library_->PackageFor(id);
  if (!package.Ok()) {
    *error = QString::fromStdString(package.message);
    return nullptr;
  }
  return package.package;
}

auto PresetController::ApplyToEditor(const QString& presetId) -> QVariantMap {
  QString    name;
  QString    error;
  const auto package = ListedPackage(presetId, &name, &error);
  if (!package) {
    return ErrorResult(error);
  }
  return PasteIntoEditor(*package, name);
}

auto PresetController::ApplyFileToEditor(const QVariant& url) -> QVariantMap {
  const auto path = LocalPath(url);
  if (path.empty()) {
    return ErrorResult(Tr("No preset file selected."));
  }
  // The file is read and pasted only: it does not enter the preset folder or the cache (R8).
  auto read = alcedo::ReadPresetFile(path);
  if (!read.Ok()) {
    return ErrorResult(QString::fromStdString(read.message));
  }
  return PasteIntoEditor(*read.package, QString::fromStdWString(path.stem().wstring()));
}

auto PresetController::ApplyToTargets(const QString& presetId, const QVariantList& targetEntries)
    -> QVariantMap {
  QString    name;
  QString    error;
  // One read for every target (plan 6.2): the multi-target Paste takes this one package.
  const auto package = ListedPackage(presetId, &name, &error);
  if (!package) {
    return ErrorResult(error);
  }
  return PasteIntoTargets(*package, targetEntries, name);
}

auto PresetController::ApplyFileToTargets(const QVariant& url, const QVariantList& targetEntries)
    -> QVariantMap {
  const auto path = LocalPath(url);
  if (path.empty()) {
    return ErrorResult(Tr("No preset file selected."));
  }
  auto read = alcedo::ReadPresetFile(path);
  if (!read.Ok()) {
    return ErrorResult(QString::fromStdString(read.message));
  }
  return PasteIntoTargets(*read.package, targetEntries,
                          QString::fromStdWString(path.stem().wstring()));
}

auto PresetController::PasteIntoTargets(const alcedo::AdjustmentTransferPackage& package,
                                        const QVariantList& targetEntries, const QString& name)
    -> QVariantMap {
  if (adjustment_transfer_ == nullptr) {
    return ErrorResult(Tr("Paste is unavailable."));
  }
  return adjustment_transfer_->PastePackageToTargets(package, targetEntries, name);
}

auto PresetController::PasteIntoEditor(const alcedo::AdjustmentTransferPackage& package,
                                       const QString& name) -> QVariantMap {
  if (editor_session_ == nullptr || !editor_session_->has_image()) {
    return ErrorResult(Tr("No photo is open."));
  }
  const auto result  = editor_session_->PasteAdjustmentPackage(package, name);
  const bool success = result.kind != alcedo::EditorSessionResultKind::Rejected &&
                       result.kind != alcedo::EditorSessionResultKind::Failed;
  return {{"success", success}, {"message", QString::fromStdString(result.message)}};
}

auto PresetController::Rename(const QString& presetId, const QString& name) -> QVariantMap {
  if (library_ == nullptr) {
    return ErrorResult(Tr("The preset library is unavailable."));
  }
  if (library_->Rename(presetId.toStdString(), name.toStdString()) !=
      alcedo::PresetLibraryService::Status::kOk) {
    return ErrorResult(QString::fromStdString(library_->LastResult().message));
  }
  QVariantMap result = SuccessResult();
  result.insert("pending", true);
  return result;
}

auto PresetController::Delete(const QString& presetId) -> QVariantMap {
  if (library_ == nullptr) {
    return ErrorResult(Tr("The preset library is unavailable."));
  }
  if (library_->Delete(presetId.toStdString()) != alcedo::PresetLibraryService::Status::kOk) {
    return ErrorResult(QString::fromStdString(library_->LastResult().message));
  }
  QVariantMap result = SuccessResult();
  result.insert("pending", true);
  return result;
}

auto PresetController::ShowInFolder(const QString& presetId) -> QVariantMap {
  if (library_ == nullptr || library_->FindEntry(presetId.toStdString()) == nullptr) {
    return ErrorResult(Tr("The preset is not in the preset folder."));
  }
  const QFileInfo file(QDir(library_->root_path()).filePath(presetId));
  return OpenFolder(std::filesystem::path(file.absolutePath().toStdWString()));
}

auto PresetController::ShowFolder() -> QVariantMap {
  if (library_ == nullptr) {
    return ErrorResult(Tr("The preset library is unavailable."));
  }
  return OpenFolder(library_->Root());
}

auto PresetController::OpenFolder(const std::filesystem::path& folder) -> QVariantMap {
  const QString path = QString::fromStdWString(folder.wstring());
  if (path.isEmpty() || !open_url_(QUrl::fromLocalFile(path))) {
    return ErrorResult(Tr("The folder cannot be opened: %1").arg(QDir::toNativeSeparators(path)));
  }
  return SuccessResult();
}

void PresetController::Refresh() {
  if (library_ != nullptr) {
    library_->Refresh();
  }
}

void PresetController::PublishOperation() {
  if (library_ == nullptr) {
    return;
  }
  const auto&  last = library_->LastResult();
  QVariantList ids;
  for (const auto& id : last.affected_ids) {
    ids.push_back(QString::fromStdString(id));
  }
  emit OperationFinished({{"operation", OperationName(last.operation)},
                          {"success", last.status == alcedo::PresetLibraryService::Status::kOk},
                          {"message", QString::fromStdString(last.message)},
                          {"presetIds", ids}});
}

}  // namespace alcedo::ui
