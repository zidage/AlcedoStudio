//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Windows directory watcher of the mounts (Mounted Folder Plan, MF7).
//
// Each mount has one change notification handle on its root directory that also reports the
// directories below it. Windows refuses to rename a directory while a handle is open in a
// directory below it, so a handle for each directory would stop the user from renaming any
// mounted directory that has subdirectories. With one handle on the root, only the directories
// above the root are held, as with any directory watcher.
//
// A handle also holds the volume. A message-only window receives the device notifications of
// the volume of each handle. When the user ejects the volume, the watcher closes the handles of
// its mounts and keeps the device notification. When the eject completes or is canceled, the
// watch ends and the watcher reports the removal (#1020): the scan finds the mount offline or
// watches it again. The window also receives the arrival of each volume (the device interface
// class GUID_DEVINTERFACE_VOLUME), so an offline mount gets a scan when its drive is connected.
//
// The new mount points (#1076) and the mount points that are gone (#1087) come from a comparison
// of the drive list: Windows can assign the drive letter after the device interface
// notification, so the watcher reads the list at each arrival and removal notification and every
// kMountTablePollInterval, as the QFileSystemWatcher form reads the mount table.

#include <Windows.h>
#include <dbt.h>

#include <QMetaObject>
#include <QObject>
#include <QTimer>
#include <QWinEventNotifier>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "ui/alcedo_main/album_backend/mount_directory_watcher.hpp"

namespace alcedo::ui {
namespace {

constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr wchar_t kWindowClassName[]    = L"AlcedoMountDirectoryWatcher";
/// GUID_DEVINTERFACE_VOLUME of <winioctl.h>. The header declares it without a definition unless
/// INITGUID is set.
constexpr GUID    kVolumeInterfaceClass = {
    0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

class WinMountDirectoryWatcher final : public MountDirectoryWatcher {
 public:
  WinMountDirectoryWatcher() {
    poll_timer_.setInterval(kMountTablePollInterval);
    QObject::connect(&poll_timer_, &QTimer::timeout, &poll_timer_,
                     [this] { ReportMountPointChanges(); });
    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.lpfnWndProc   = &WinMountDirectoryWatcher::WindowProc;
    window_class.hInstance     = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClassName;
    // A second watcher finds the class registered; that is not an error.
    (void)::RegisterClassExW(&window_class);
    window_ = ::CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                window_class.hInstance, nullptr);
    if (window_ == nullptr) {
      return;
    }
    ::SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size       = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid  = kVolumeInterfaceClass;
    // Without it, a connected drive leaves its mount offline until the next activation scan.
    volume_arrival_notification_ =
        ::RegisterDeviceNotificationW(window_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
  }

  ~WinMountDirectoryWatcher() override {
    UnwatchAll();
    if (volume_arrival_notification_ != nullptr) {
      ::UnregisterDeviceNotification(volume_arrival_notification_);
    }
    if (window_ != nullptr) {
      ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
      ::DestroyWindow(window_);
    }
  }

  WinMountDirectoryWatcher(const WinMountDirectoryWatcher&)                    = delete;
  auto operator=(const WinMountDirectoryWatcher&) -> WinMountDirectoryWatcher& = delete;

  void SetChangeCallback(ChangeCallback callback) override { on_change_ = std::move(callback); }

  void SetVolumeCallbacks(VolumeRemovedCallback removed, VolumeArrivedCallback arrived,
                          MountPointArrivedCallback mount_point_arrived,
                          MountPointRemovedCallback mount_point_removed) override {
    on_volume_removed_      = std::move(removed);
    on_volume_arrived_      = std::move(arrived);
    on_mount_point_arrived_ = std::move(mount_point_arrived);
    on_mount_point_removed_ = std::move(mount_point_removed);
    if (!on_mount_point_arrived_ && !on_mount_point_removed_) {
      poll_timer_.stop();
      return;
    }
    // The drive list of this call is the state that the first comparison uses.
    drive_table_.Start();
    poll_timer_.start();
  }

  auto Watch(mount_id_t mount_id, const std::filesystem::path& root,
             std::span<const std::wstring> directories) -> bool override {
    (void)directories;  // The handle of the root reports the whole tree.
    Unwatch(mount_id);
    if (window_ == nullptr) {
      return false;
    }
    auto entry       = std::make_unique<Entry>();
    entry->directory = ::CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (entry->directory == INVALID_HANDLE_VALUE) {
      return false;
    }
    entry->change = ::FindFirstChangeNotificationW(root.c_str(), TRUE, kNotifyFilter);
    if (entry->change == INVALID_HANDLE_VALUE) {
      CloseEntry(*entry);
      return false;
    }
    DEV_BROADCAST_HANDLE filter{};
    filter.dbch_size       = sizeof(filter);
    filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
    filter.dbch_handle     = entry->directory;
    entry->device_notification =
        ::RegisterDeviceNotificationW(window_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (entry->device_notification == nullptr) {
      // Without the device notification the handles would block an eject of the volume.
      CloseEntry(*entry);
      return false;
    }
    entry->notifier = std::make_unique<QWinEventNotifier>(entry->change);
    QObject::connect(entry->notifier.get(), &QWinEventNotifier::activated, entry->notifier.get(),
                     [this, mount_id](HANDLE) { HandleChange(mount_id); });
    entries_[mount_id] = std::move(entry);
    return true;
  }

  void Unwatch(mount_id_t mount_id) override {
    const auto it = entries_.find(mount_id);
    if (it == entries_.end()) {
      return;
    }
    // A queued activation of the notifier refers to the mount id only, so it finds no entry.
    auto entry = std::move(it->second);
    entries_.erase(it);
    CloseEntry(*entry);
  }

  void UnwatchAll() override {
    while (!entries_.empty()) {
      Unwatch(entries_.begin()->first);
    }
  }

 private:
  struct Entry {
    HANDLE                             directory           = INVALID_HANDLE_VALUE;
    HANDLE                             change              = INVALID_HANDLE_VALUE;
    HDEVNOTIFY                         device_notification = nullptr;
    std::unique_ptr<QWinEventNotifier> notifier;
  };

  /// Close the handles that hold the volume. The device notification stays, so the end of the
  /// removal still arrives.
  static void CloseHandles(Entry& entry) {
    if (entry.notifier) {
      // Unwatch can run inside the activated signal of this notifier.
      entry.notifier->setEnabled(false);
      entry.notifier.release()->deleteLater();
    }
    if (entry.change != INVALID_HANDLE_VALUE) {
      ::FindCloseChangeNotification(entry.change);
      entry.change = INVALID_HANDLE_VALUE;
    }
    if (entry.directory != INVALID_HANDLE_VALUE) {
      ::CloseHandle(entry.directory);
      entry.directory = INVALID_HANDLE_VALUE;
    }
  }

  static void CloseEntry(Entry& entry) {
    CloseHandles(entry);
    if (entry.device_notification != nullptr) {
      ::UnregisterDeviceNotification(entry.device_notification);
      entry.device_notification = nullptr;
    }
  }

  void HandleChange(mount_id_t mount_id) {
    const auto it = entries_.find(mount_id);
    if (it == entries_.end()) {
      return;
    }
    if (!::FindNextChangeNotification(it->second->change)) {
      // The root directory is gone. The scan that this notification starts finds the mount
      // offline or watches it again.
      Unwatch(mount_id);
    }
    if (on_change_) {
      on_change_(mount_id);
    }
  }

  /// Report each drive that is gone from the drive list and each drive that is new in it. A
  /// volume that replaced another at the same drive letter between two reads is in both lists,
  /// so it is not reported; the card checks the volume before Import and Mount.
  void ReportMountPointChanges() {
    if (!on_mount_point_arrived_ && !on_mount_point_removed_) {
      return;
    }
    const auto changes = drive_table_.Poll();
    if (on_mount_point_removed_) {
      for (const auto& mount_point : changes.removed) {
        on_mount_point_removed_(mount_point);
      }
    }
    if (on_mount_point_arrived_) {
      for (const auto& mount_point : changes.arrived) {
        on_mount_point_arrived_(ReadArrivedVolume(mount_point));
      }
    }
  }

  /// The mount whose device notification is @p notification.
  auto MountOfNotification(HDEVNOTIFY notification) const -> std::optional<mount_id_t> {
    for (const auto& [mount_id, entry] : entries_) {
      if (entry->device_notification == notification) {
        return mount_id;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief The removal of the volume of a handle, and the arrival of a volume.
   *
   * Windows asks each holder of a handle before the removal (DBT_DEVICEQUERYREMOVE); an open
   * handle refuses the removal, so the handles close in this call. DBT_DEVICEREMOVEPENDING
   * comes when the removal cannot be refused. DBT_DEVICEREMOVECOMPLETE ends the removal, also
   * the removal of a drive that the user pulls out without an eject. DBT_DEVICEQUERYREMOVEFAILED
   * ends a canceled removal: the volume stays, and the scan watches the mount again.
   *
   * The callbacks run after this window procedure returns, because Windows waits for the
   * procedure before it sends the next notification.
   */
  void HandleDeviceEvent(WPARAM event, LPARAM data) {
    const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(data);
    if (header == nullptr) {
      return;
    }
    if (event == DBT_DEVICEARRIVAL && header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
      QMetaObject::invokeMethod(
          &callback_context_,
          [this] {
            if (on_volume_arrived_) {
              on_volume_arrived_();
            }
            ReportMountPointChanges();
          },
          Qt::QueuedConnection);
      return;
    }
    if (event == DBT_DEVICEREMOVECOMPLETE &&
        header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
      // An ejected volume that holds no watched mount: its card closes now rather than at the
      // next poll.
      QMetaObject::invokeMethod(
          &callback_context_, [this] { ReportMountPointChanges(); }, Qt::QueuedConnection);
      return;
    }
    if (header->dbch_devicetype != DBT_DEVTYP_HANDLE) {
      return;
    }
    const auto* handle = reinterpret_cast<const DEV_BROADCAST_HANDLE*>(header);
    const auto  found  = MountOfNotification(handle->dbch_hdevnotify);
    if (!found.has_value()) {
      return;
    }
    const mount_id_t mount_id = *found;
    switch (event) {
      case DBT_DEVICEQUERYREMOVE:
      case DBT_DEVICEREMOVEPENDING:
        CloseHandles(*entries_.at(mount_id));
        return;
      case DBT_DEVICEREMOVECOMPLETE:
      case DBT_DEVICEQUERYREMOVEFAILED:
        Unwatch(mount_id);
        QMetaObject::invokeMethod(
            &callback_context_,
            [this, mount_id] {
              if (on_volume_removed_) {
                on_volume_removed_(mount_id);
              }
            },
            Qt::QueuedConnection);
        return;
      default:
        return;
    }
  }

  static auto CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
      -> LRESULT {
    if (message == WM_DEVICECHANGE) {
      auto* self =
          reinterpret_cast<WinMountDirectoryWatcher*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
      if (self != nullptr) {
        self->HandleDeviceEvent(w_param, l_param);
      }
      return TRUE;
    }
    return ::DefWindowProcW(window, message, w_param, l_param);
  }

  HWND                                                   window_                      = nullptr;
  HDEVNOTIFY                                             volume_arrival_notification_ = nullptr;
  /// The receiver of the posted callbacks; its destruction with the watcher drops a posted call.
  QObject                                                callback_context_;
  ChangeCallback                                         on_change_;
  VolumeRemovedCallback                                  on_volume_removed_;
  VolumeArrivedCallback                                  on_volume_arrived_;
  MountPointArrivedCallback                              on_mount_point_arrived_;
  MountPointRemovedCallback                              on_mount_point_removed_;
  /// The drive list for the new mount points; ListMountPointPaths.
  MountTableComparison                                   drive_table_;
  QTimer                                                 poll_timer_;
  std::unordered_map<mount_id_t, std::unique_ptr<Entry>> entries_;
};

}  // namespace

auto CreatePlatformMountDirectoryWatcher() -> std::unique_ptr<MountDirectoryWatcher> {
  return std::make_unique<WinMountDirectoryWatcher>();
}

}  // namespace alcedo::ui
