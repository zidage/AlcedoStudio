//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string_view>

#include "utils/logging/log_categories.hpp"

/// Qt-free logging for the core libraries (edit, decoders, image, renderer, gpu), on the model of
/// Exiv2 `LogMsg`:
///
/// - `ALCEDO_LOG_<LEVEL>(Category) << ...;` collects one message in a `Message` object.
/// - The destructor of `Message` gives the complete line to the installed handler in one call, so
///   lines from different threads do not interleave.
/// - A level below the compile-time floor `ALCEDO_LOG_MIN_LEVEL` compiles to
///   `if constexpr (true) {} else ...`: the arguments are type-checked and not evaluated.
/// - A level at or above the floor reads one cached enabled flag of its category before it
///   formats. No `getenv`, lock, or allocation happens before that check.
///
/// Without a Qt application the default handler writes each line to stderr under a mutex.
/// AppDiagnostics installs a handler that forwards each line to the `QLoggingCategory` of the same
/// name, so `QT_LOGGING_RULES` filters it and the application log file receives it.
///
/// The AlcedoLog CMake target sets `ALCEDO_LOG_MIN_LEVEL` from the build type: Debug builds compile
/// in every level; other builds compile out `ALCEDO_LOG_DEBUG`.

#define ALCEDO_LOG_LEVEL_DEBUG 0
#define ALCEDO_LOG_LEVEL_INFO  1
#define ALCEDO_LOG_LEVEL_WARN  2
#define ALCEDO_LOG_LEVEL_ERROR 3

#ifndef ALCEDO_LOG_MIN_LEVEL
#error "ALCEDO_LOG_MIN_LEVEL is a compile definition of the AlcedoLog target; link AlcedoLog."
#endif

namespace alcedo::log {

enum class Level : std::uint8_t {
  Debug = ALCEDO_LOG_LEVEL_DEBUG,
  Info  = ALCEDO_LOG_LEVEL_INFO,
  Warn  = ALCEDO_LOG_LEVEL_WARN,
  Error = ALCEDO_LOG_LEVEL_ERROR,
};

/// The lowest level that this translation unit compiles in. Internal linkage (not `inline`): a
/// translation unit may set its own floor, and an inline variable must be the same in all of them.
constexpr Level kCompiledMinLevel = static_cast<Level>(ALCEDO_LOG_MIN_LEVEL);

/// One complete log message. @ref message, @ref file, and @ref function stay valid only during
/// the handler call.
struct Record {
  Level            level    = Level::Info;
  Category         category = Category::App;
  const char*      file     = nullptr;
  int              line     = 0;
  const char*      function = nullptr;
  std::string_view message;
};

/// Receives each complete message once. Called from any thread, also concurrently; the handler
/// does its own synchronization. Must not throw.
using Handler = void (*)(const Record& record);

/// True when messages of @p level in @p category reach the handler. One relaxed atomic load.
[[nodiscard]] auto IsEnabled(Level level, Category category) noexcept -> bool;

/// Enables or disables @p level in @p category. The defaults enable Info, Warn, and Error, and
/// disable Debug, like the default rules of AppDiagnostics (`*.debug=false`).
void               SetEnabled(Category category, Level level, bool enabled) noexcept;

/// Restores the default enabled levels of every category.
void               ResetEnabledLevels() noexcept;

/// Installs @p handler for all later messages. `nullptr` installs the default stderr handler.
void               SetHandler(Handler handler) noexcept;

/// The default handler: writes `[LEVEL] [category] message (file:line)` and a newline to stderr in
/// one write under a mutex.
void               WriteToStderr(const Record& record) noexcept;

/// `DEBUG`, `INFO`, `WARN`, or `ERROR`.
[[nodiscard]] auto LevelName(Level level) noexcept -> const char*;

/// Collects one message and gives it to the handler when it goes out of scope. Use the
/// `ALCEDO_LOG_*` macros; they make one temporary per statement.
class Message      final {
 public:
  Message(Level level, Category category, const char* file, int line,
               const char* function) noexcept;
  ~Message();

  Message(const Message&)                                  = delete;
  auto               operator=(const Message&) -> Message& = delete;

  [[nodiscard]] auto stream() -> std::ostream& { return stream_; }

 private:
  Level              level_;
  Category           category_;
  const char*        file_;
  int                line_;
  const char*        function_;
  std::ostringstream stream_;
};

}  // namespace alcedo::log

#define ALCEDO_LOG_MESSAGE_(level_id, category_id)                                             \
  ::alcedo::log::Message(::alcedo::log::Level::level_id, ::alcedo::log::Category::category_id, \
                         __FILE__, __LINE__, __func__)                                         \
      .stream()

#define ALCEDO_LOG_ENABLED_(level_id, category_id)                       \
  if (!::alcedo::log::IsEnabled(::alcedo::log::Level::level_id,          \
                                ::alcedo::log::Category::category_id)) { \
  } else                                                                 \
    ALCEDO_LOG_MESSAGE_(level_id, category_id)

#define ALCEDO_LOG_COMPILED_OUT_(level_id, category_id) \
  if constexpr (true) {                                 \
  } else                                                \
    ALCEDO_LOG_MESSAGE_(level_id, category_id)

#if ALCEDO_LOG_MIN_LEVEL <= ALCEDO_LOG_LEVEL_DEBUG
#define ALCEDO_LOG_DEBUG(category_id) ALCEDO_LOG_ENABLED_(Debug, category_id)
#else
#define ALCEDO_LOG_DEBUG(category_id) ALCEDO_LOG_COMPILED_OUT_(Debug, category_id)
#endif

#if ALCEDO_LOG_MIN_LEVEL <= ALCEDO_LOG_LEVEL_INFO
#define ALCEDO_LOG_INFO(category_id) ALCEDO_LOG_ENABLED_(Info, category_id)
#else
#define ALCEDO_LOG_INFO(category_id) ALCEDO_LOG_COMPILED_OUT_(Info, category_id)
#endif

#if ALCEDO_LOG_MIN_LEVEL <= ALCEDO_LOG_LEVEL_WARN
#define ALCEDO_LOG_WARN(category_id) ALCEDO_LOG_ENABLED_(Warn, category_id)
#else
#define ALCEDO_LOG_WARN(category_id) ALCEDO_LOG_COMPILED_OUT_(Warn, category_id)
#endif

#define ALCEDO_LOG_ERROR(category_id) ALCEDO_LOG_ENABLED_(Error, category_id)
