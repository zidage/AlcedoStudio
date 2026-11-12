//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_camera_profile_list_model.hpp"

#include <QVariantList>
#include <utility>

#include "app/editor_parameter_write.hpp"
#include "image/dcp_profile_library.hpp"
#include "json.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::ui {
namespace {

constexpr const char* kFieldKey = "camera_profile_file";

auto Text(const std::string& value) -> QString { return QString::fromStdString(value); }

/// True when @p identity names the stored reference: the camera model by its compact key, the
/// profile name exactly (the identity rule of DcpProfileLibrary::Find).
auto SameIdentity(const DcpProfileIdentity& identity, const std::string& camera_model,
                  const std::string& profile_name) -> bool {
  return identity.profile_name == profile_name &&
         DcpUniqueCameraModelKey(identity.unique_camera_model) ==
             DcpUniqueCameraModelKey(camera_model);
}

auto Entry(const QString& label, const std::string& camera_model, const std::string& profile_name,
           bool is_default, bool is_missing) -> QVariantMap {
  QVariantMap entry;
  // `value` keeps the rows distinct for EditorAdjustmentEnumModel::currentValue.
  entry.insert(QStringLiteral("value"),
               is_default ? QString() : Text(camera_model) + QChar(0x1F) + Text(profile_name));
  entry.insert(QStringLiteral("label"), label);
  entry.insert(QStringLiteral("cameraModel"), Text(camera_model));
  entry.insert(QStringLiteral("profileName"), Text(profile_name));
  entry.insert(QStringLiteral("isDefault"), is_default);
  entry.insert(QStringLiteral("isMissing"), is_missing);
  return entry;
}

}  // namespace

EditorCameraProfileListModel::EditorCameraProfileListModel(QObject* parent)
    : EditorAdjustmentEnumModel(parent) {
  setFieldKey(QString::fromLatin1(kFieldKey));
  setDefaultIndex(0);
}

auto EditorCameraProfileListModel::library() const -> QObject* { return library_.data(); }

void EditorCameraProfileListModel::setLibrary(QObject* library) {
  auto* service = qobject_cast<CameraProfileLibraryService*>(library);
  if (service == library_) {
    return;
  }
  if (library_ != nullptr) {
    disconnect(library_, nullptr, this, nullptr);
  }
  library_ = service;
  if (library_ != nullptr) {
    connect(library_, &CameraProfileLibraryService::ScanFinished, this,
            &EditorCameraProfileListModel::ReadMatchedProfiles);
  }
  emit libraryChanged();
  ReadMatchedProfiles();
}

void EditorCameraProfileListModel::setCameraMake(const QString& make) {
  if (camera_make_ == make) {
    return;
  }
  camera_make_ = make;
  emit cameraChanged();
  ReadMatchedProfiles();
}

void EditorCameraProfileListModel::setCameraModel(const QString& model) {
  if (camera_model_ == model) {
    return;
  }
  camera_model_ = model;
  emit cameraChanged();
  ReadMatchedProfiles();
}

void EditorCameraProfileListModel::loadFromSnapshot(const QVariantMap& snapshot) {
  // The panel presentation nests the odt value under "odt" too.
  if (const auto odt = snapshot.value(QStringLiteral("odt")); odt.isValid()) {
    auto value = odt.toMap();
    if (value.contains(QStringLiteral("odt"))) {
      value = value.value(QStringLiteral("odt")).toMap();
    }
    const bool film_print =
        value.value(QStringLiteral("method")).toString() == QStringLiteral("film_print");
    if (film_print != film_print_method_) {
      film_print_method_ = film_print;
      emit filmPrintMethodChanged();
    }
  }
  const auto field = snapshot.value(QString::fromLatin1(kFieldKey));
  if (!field.isValid()) {
    return;
  }
  // The panel presentation nests the value under the field key; accept the bare value too.
  auto value = field.toMap();
  if (value.contains(QString::fromLatin1(kFieldKey))) {
    value = value.value(QString::fromLatin1(kFieldKey)).toMap();
  }
  StoredReference stored;
  stored.raster           = value.value(QStringLiteral("raster")).toBool();
  stored.embedded_default = value.value(QStringLiteral("embedded_default")).toBool();
  stored.selected         = value.value(QStringLiteral("selected")).toBool();
  stored.resolved         = value.value(QStringLiteral("resolved")).toBool();
  if (stored.selected) {
    stored.has_tone_curve = value.value(QStringLiteral("has_tone_curve")).toBool();
    stored.profile_tone   = value.value(QStringLiteral("profile_tone")).toBool();
    stored.camera_model   = value.value(QStringLiteral("camera_model")).toString().toStdString();
    stored.profile_name   = value.value(QStringLiteral("profile_name")).toString().toStdString();
  }
  const bool changed =
      stored.raster != stored_.raster || stored.embedded_default != stored_.embedded_default ||
      stored.selected != stored_.selected || stored.resolved != stored_.resolved ||
      stored.has_tone_curve != stored_.has_tone_curve ||
      stored.profile_tone != stored_.profile_tone || stored.camera_model != stored_.camera_model ||
      stored.profile_name != stored_.profile_name;
  stored_ = std::move(stored);
  ComposeEntries();
  if (changed) {
    emit storedReferenceChanged();
  }
}

auto EditorCameraProfileListModel::submitSelection() -> bool {
  const int   row = currentIndex();
  std::string error;
  auto        write =
      ParseEditorParameterWrite(kFieldKey, nlohmann::json::parse(RowWriteJson(row)), &error);
  bool accepted = false;
  if (write.has_value()) {
    accepted = submitNow(std::move(*write), true);
    if (!accepted) {
      error = "the editor session does not accept edits";
    }
  }
  if (!accepted) {
    qCWarning(alcedo::diag::editorLog).noquote()
        << "[Camera Profile] selection" << entries().value(row).toMap().value("label").toString()
        << "is not applied:" << QString::fromStdString(error);
    setCurrentIndex(StoredRow());
  }
  return accepted;
}

void EditorCameraProfileListModel::ReadMatchedProfiles() {
  matched_.clear();
  // Match waits for a running scan; the GUI thread does not. The rows wait for ScanFinished,
  // and the combo is disabled while the scan runs (plan section 2.2).
  if (library_ != nullptr && !library_->scanning() && !camera_model_.isEmpty()) {
    matched_ = library_->library().Match(camera_make_.toStdString(), camera_model_.toStdString());
  }
  ComposeEntries();
}

void EditorCameraProfileListModel::ComposeEntries() {
  QVariantList entries;
  if (!stored_.raster) {
    entries.push_back(
        Entry(stored_.embedded_default ? tr("Embedded") : tr("Built-in"), {}, {}, true, false));
    bool stored_matched = false;
    for (const auto& identity : matched_) {
      stored_matched =
          stored_matched ||
          (stored_.selected && SameIdentity(identity, stored_.camera_model, stored_.profile_name));
      entries.push_back(Entry(Text(identity.profile_name), identity.unique_camera_model,
                              identity.profile_name, false, false));
    }
    if (stored_.selected && !stored_matched) {
      entries.push_back(Entry(Text(stored_.profile_name), stored_.camera_model,
                              stored_.profile_name, false, true));
    }
  }
  setEntries(entries);
  setCurrentIndex(StoredRow());
}

auto EditorCameraProfileListModel::StoredRow() const -> int {
  if (!stored_.selected) {
    return 0;
  }
  const auto rows = entries();
  for (int row = 1; row < rows.size(); ++row) {
    const auto               entry = rows[row].toMap();
    const DcpProfileIdentity identity{
        entry.value(QStringLiteral("cameraModel")).toString().toStdString(),
        entry.value(QStringLiteral("profileName")).toString().toStdString()};
    if (SameIdentity(identity, stored_.camera_model, stored_.profile_name)) {
      return row;
    }
  }
  return 0;
}

auto EditorCameraProfileListModel::RowWriteJson(int row) const -> std::string {
  const auto entry = entries().value(row).toMap();
  if (entry.isEmpty() || entry.value(QStringLiteral("isDefault")).toBool()) {
    return "null";
  }
  nlohmann::json write{
      {"camera_model", entry.value(QStringLiteral("cameraModel")).toString().toStdString()},
      {"profile_name", entry.value(QStringLiteral("profileName")).toString().toStdString()}};
  // A switch that the user turned off stays off for another profile.
  if (stored_.selected && !stored_.profile_tone) {
    write["profile_tone"] = false;
  }
  return write.dump();
}

bool EditorCameraProfileListModel::submitProfileTone(bool on) {
  std::string error;
  bool        accepted = false;
  if (!stored_.selected) {
    error = "no camera profile is selected";
  } else if (on == stored_.profile_tone) {
    error = "the switch already has this value";
  } else {
    auto write = ParseEditorParameterWrite(kFieldKey,
                                           {{"camera_model", stored_.camera_model},
                                            {"profile_name", stored_.profile_name},
                                            {"profile_tone", on}},
                                           &error);
    if (write.has_value()) {
      accepted = submitNow(std::move(*write), true);
      if (!accepted) {
        error = "the editor session does not accept edits";
      }
    }
  }
  if (!accepted) {
    qCWarning(alcedo::diag::editorLog).noquote()
        << "[Camera Profile] Tone Curve" << (on ? "on" : "off")
        << "is not applied:" << QString::fromStdString(error);
  }
  return accepted;
}

}  // namespace alcedo::ui
