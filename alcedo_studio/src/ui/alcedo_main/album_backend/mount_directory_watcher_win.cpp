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
// the volume of each handle and closes the handles of a volume that the user ejects; the next
// scan finds the mount offline, and a later scan that finds it online watches it again.

#include <Windows.h>
#include <dbt.h>

#include <QWinEventNotifier>
#include <memory>
#include <unordered_map>
#include <utility>

#include "ui/alcedo_main/album_backend/mount_directory_watcher.hpp"

namespace alcedo::ui {
namespace {

constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr wchar_t              kWindowClassName[] = L"AlcedoMountDirectoryWatcher";

class WinMountDirectoryWatcher final : public MountDirectoryWatcher {
 public:
  WinMountDirectoryWatcher() {
    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.lpfnWndProc   = &WinMountDirectoryWatcher::WindowProc;
    window_class.hInstance     = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClassName;
    // A second watcher finds the class registered; that is not an error.
    (void)::RegisterClassExW(&window_class);
    window_ = ::CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                window_class.hInstance, nullptr);
    if (window_ != nullptr) {
      ::SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
  }

  ~WinMountDirectoryWatcher() override {
    UnwatchAll();
    if (window_ != nullptr) {
      ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
      ::DestroyWindow(window_);
    }
  }

  WinMountDirectoryWatcher(const WinMountDirectoryWatcher&)                    = delete;
  auto operator=(const WinMountDirectoryWatcher&) -> WinMountDirectoryWatcher& = delete;

  void SetChangeCallback(ChangeCallback callback) override { on_change_ = std::move(callback); }

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

  static void CloseEntry(Entry& entry) {
    if (entry.notifier) {
      // Unwatch can run inside the activated signal of this notifier.
      entry.notifier->setEnabled(false);
      entry.notifier.release()->deleteLater();
    }
    if (entry.device_notification != nullptr) {
      ::UnregisterDeviceNotification(entry.device_notification);
      entry.device_notification = nullptr;
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

  /// Close the handles of the mount whose volume the user ejects. Windows asks each holder of
  /// a handle before the removal (DBT_DEVICEQUERYREMOVE); an open handle refuses the removal.
  void HandleDeviceEvent(WPARAM event, LPARAM data) {
    if (event != DBT_DEVICEQUERYREMOVE && event != DBT_DEVICEREMOVECOMPLETE &&
        event != DBT_DEVICEREMOVEPENDING) {
      return;
    }
    const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(data);
    if (header == nullptr || header->dbch_devicetype != DBT_DEVTYP_HANDLE) {
      return;
    }
    const auto* handle = reinterpret_cast<const DEV_BROADCAST_HANDLE*>(header);
    for (const auto& [mount_id, entry] : entries_) {
      if (entry->device_notification == handle->dbch_hdevnotify) {
        Unwatch(mount_id);
        return;
      }
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

  HWND                                                   window_ = nullptr;
  ChangeCallback                                         on_change_;
  std::unordered_map<mount_id_t, std::unique_ptr<Entry>> entries_;
};

}  // namespace

auto CreatePlatformMountDirectoryWatcher() -> std::unique_ptr<MountDirectoryWatcher> {
  return std::make_unique<WinMountDirectoryWatcher>();
}

}  // namespace alcedo::ui
