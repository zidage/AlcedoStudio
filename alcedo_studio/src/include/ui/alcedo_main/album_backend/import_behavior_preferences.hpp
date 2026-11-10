// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>

namespace alcedo::ui {
/// Application-wide import defaults, owned for the application lifetime and used on the GUI
/// thread only. Settings stages edits separately; only Apply invokes the persistent setter.
class ImportBehaviorPreferences final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool skipDuplicateImports READ SkipDuplicateImports WRITE SetSkipDuplicateImports
                 NOTIFY SkipDuplicateImportsChanged)
 public:
  static constexpr auto kSettingKey = "import/skipDuplicates";
  explicit ImportBehaviorPreferences(QObject* parent = nullptr);
  auto             SkipDuplicateImports() const -> bool { return skip_duplicates_; }
  /// Persists the default for subsequent jobs; never changes a running request.
  Q_INVOKABLE void SetSkipDuplicateImports(bool enabled);
 signals:
  void SkipDuplicateImportsChanged();

 private:
  bool skip_duplicates_ = true;
};
}  // namespace alcedo::ui
