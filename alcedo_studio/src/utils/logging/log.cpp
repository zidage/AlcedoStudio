//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/logging/log.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>

namespace alcedo::log {
namespace {

constexpr auto LevelBit(Level level) noexcept -> std::uint8_t {
  return static_cast<std::uint8_t>(1u << static_cast<unsigned>(level));
}

constexpr std::uint8_t kDefaultEnabledLevels =
    LevelBit(Level::Info) | LevelBit(Level::Warn) | LevelBit(Level::Error);

/// One bit per level for each category, stored as the difference from kDefaultEnabledLevels so
/// that the zero initialization of a static is the default state: no initialization order
/// dependency for messages from static constructors. Written by SetEnabled (AppDiagnostics applies
/// the Qt rules here), read by IsEnabled on every message at or above the compile-time floor.
std::array<std::atomic<std::uint8_t>, kCategoryCount> g_changed_levels{};

auto EnabledLevels(std::size_t index) noexcept -> std::uint8_t {
  return static_cast<std::uint8_t>(g_changed_levels[index].load(std::memory_order_relaxed) ^
                                   kDefaultEnabledLevels);
}

std::atomic<Handler> g_handler{&WriteToStderr};
std::mutex           g_stderr_lock;

auto                 CategoryIndex(Category category) noexcept -> std::size_t {
  return static_cast<std::size_t>(category);
}

auto FileBaseName(const char* file) noexcept -> std::string_view {
  if (file == nullptr) {
    return {};
  }
  const std::string_view path(file);
  const auto             separator = path.find_last_of("/\\");
  return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

}  // namespace

auto IsEnabled(Level level, Category category) noexcept -> bool {
  const auto index = CategoryIndex(category);
  if (index >= kCategoryCount) {
    return false;
  }
  return (EnabledLevels(index) & LevelBit(level)) != 0;
}

void SetEnabled(Category category, Level level, bool enabled) noexcept {
  const auto index = CategoryIndex(category);
  if (index >= kCategoryCount) {
    return;
  }
  const auto bit             = LevelBit(level);
  const bool enabled_default = (kDefaultEnabledLevels & bit) != 0;
  if (enabled != enabled_default) {
    g_changed_levels[index].fetch_or(bit, std::memory_order_relaxed);
  } else {
    g_changed_levels[index].fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_relaxed);
  }
}

void ResetEnabledLevels() noexcept {
  for (auto& entry : g_changed_levels) {
    entry.store(0, std::memory_order_relaxed);
  }
}

void SetHandler(Handler handler) noexcept {
  g_handler.store(handler != nullptr ? handler : &WriteToStderr, std::memory_order_release);
}

auto LevelName(Level level) noexcept -> const char* {
  switch (level) {
    case Level::Debug:
      return "DEBUG";
    case Level::Info:
      return "INFO";
    case Level::Warn:
      return "WARN";
    case Level::Error:
      return "ERROR";
  }
  return "LOG";
}

void WriteToStderr(const Record& record) noexcept {
  try {
    const auto  category = CategoryName(record.category);
    const auto  file     = FileBaseName(record.file);
    std::string line;
    line.reserve(record.message.size() + category.size() + file.size() + 32);
    line += '[';
    line += LevelName(record.level);
    line += "] [";
    line += category;
    line += "] ";
    line += record.message;
    if (!file.empty() && record.line > 0) {
      line += " (";
      line += file;
      line += ':';
      line += std::to_string(record.line);
      line += ')';
    }
    line += '\n';

    std::lock_guard lock(g_stderr_lock);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
  } catch (...) {
    // A log line that cannot be formatted is dropped; logging must not throw into its caller.
  }
}

Message::Message(Level level, Category category, const char* file, int line,
                 const char* function) noexcept
    : level_(level), category_(category), file_(file), line_(line), function_(function) {}

Message::~Message() {
  try {
    const std::string text = stream_.str();
    const Record      record{
             .level    = level_,
             .category = category_,
             .file     = file_,
             .line     = line_,
             .function = function_,
             .message  = text,
    };
    g_handler.load(std::memory_order_acquire)(record);
  } catch (...) {
    // See WriteToStderr: a message that cannot be completed is dropped.
  }
}

}  // namespace alcedo::log
