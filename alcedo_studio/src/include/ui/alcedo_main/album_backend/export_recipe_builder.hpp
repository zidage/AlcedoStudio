//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <optional>
#include <variant>

#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"

namespace alcedo::ui {

/// The option values of one export run, in the units of the export panel and of the
/// `export.run` command. The map form (ExportRecipeBuilder::FromVariantMap and ToVariantMap)
/// uses the member names as keys.
struct ExportRecipeOptions {
  /// JPEG, PNG, TIFF, or EXR. An HDR image always exports as an UltraHDR JPEG.
  QString format            = QStringLiteral("JPEG");
  int     bit_depth         = 8;
  /// JPEG quality, 1..100.
  int     quality           = 95;
  /// PNG compression level, 0..9.
  int     png_level         = 5;
  /// NONE, LZW, or ZIP.
  QString tiff_compression  = QStringLiteral("NONE");
  /// Writes the EXIF, XMP, and IPTC metadata of the photo.
  bool    include_metadata  = true;
  bool    embed_icc_profile = true;
  /// Copies the GPS IFD of the source file. Applies only when include_metadata is true.
  bool    include_gps       = true;
  /// UltraHDR JPEG quality of HDR images, 1..100.
  int     ultra_hdr_quality = 95;
  bool    ultra_hdr_dither  = true;
  /// The longest edge of HDR images, in pixels, 256..8192. HDR images also follow resize_mode.
  double  ultra_hdr_long_edge = 8192;
  QString out_dir;
  /// When true, the files go to `out_dir/subfolder`.
  bool    subfolder_enabled = false;
  QString subfolder         = QStringLiteral("Processed");
  /// original, long_edge, bounds, or print.
  QString resize_mode       = QStringLiteral("original");
  /// The pixel sizes are numbers so that a value that is not an integer (for example text from
  /// the panel) is rejected by Validate instead of being truncated.
  double  long_edge         = 2048;
  double  width             = 2048;
  double  height            = 2048;
  double  print_width       = 210;
  double  print_height      = 297;
  /// mm, cm, or in.
  QString print_unit        = QStringLiteral("mm");
  double  dpi               = 300;
  QString file_name_pattern = QStringLiteral("{source}");
};

/// A named set of export options. A preset sets every option except the destination (out_dir,
/// subfolder_enabled, and subfolder), which stays as the user chose it.
struct ExportRecipePreset {
  /// `builtin:<key>` for a built-in preset, `user:<name>` for a preset the user saved.
  QString             id;
  /// The English name of a built-in preset (the panel shows its translation), or the user name.
  QString             name;
  bool                built_in = false;
  ExportRecipeOptions options;
};

/// The first option that Validate rejects.
struct ExportRecipeError {
  /// The option name, for example `long_edge`.
  QString field;
  /// The rule, for example `long_edge must be in [256, 16384]`.
  QString message;
};

/// The export option defaults, limits, bit-depth rules, output folder rule, file name pattern
/// check, and the assembly of the ImportExportHandler export arguments. The export panel and
/// the `export.*` commands use the same rules. Owned by ApplicationModuleHost. GUI thread only.
class ExportRecipeBuilder final : public QObject {
  Q_OBJECT
  /// Every preset as `{id, name, builtIn}`: the built-in presets, then the user presets.
  Q_PROPERTY(QVariantList presets READ PresetList NOTIFY PresetsChanged)

 public:
  static constexpr int    kMinLongEdge         = 256;
  static constexpr int    kMaxLongEdge         = 16384;
  static constexpr int    kMinPixelBound       = 1;
  static constexpr int    kMaxPixelBound       = 100000;
  static constexpr double kMaxPrintSize        = 100000.0;
  static constexpr int    kMinDpi              = 1;
  static constexpr int    kMaxDpi              = 10000;
  static constexpr int    kMinQuality          = 1;
  static constexpr int    kMaxQuality          = 100;
  static constexpr int    kMinPngLevel         = 0;
  static constexpr int    kMaxPngLevel         = 9;
  /// The long edge limit of an UltraHDR export.
  static constexpr int    kUltraHdrMaxLongEdge = 8192;
  static constexpr int    kMaxUserPresetCount  = 64;
  static constexpr int    kMaxPresetNameSize   = 80;

  explicit ExportRecipeBuilder(ImportExportHandler* import_export, QObject* parent = nullptr);

  [[nodiscard]] static auto Defaults() -> ExportRecipeOptions;
  [[nodiscard]] static auto Formats() -> QStringList;
  /// The bit depths that @p format can write: JPEG 8; PNG 8, 16; TIFF 8, 16, 32; EXR 16, 32.
  /// Empty for an unknown format.
  [[nodiscard]] static auto AllowedBitDepths(const QString& format) -> QList<int>;
  /// The bit depth that replaces a depth @p format cannot write: 8 for JPEG, 16 otherwise.
  [[nodiscard]] static auto PreferredBitDepth(const QString& format) -> int;
  [[nodiscard]] static auto TiffCompressions() -> QStringList;
  [[nodiscard]] static auto ResizeModes() -> QStringList;
  [[nodiscard]] static auto PrintUnits() -> QStringList;
  /// The parser message when ParseExportFileNamePattern rejects @p pattern, otherwise nothing.
  [[nodiscard]] static auto CheckFileNamePattern(const QString& pattern) -> std::optional<QString>;
  /// The first option of @p options that breaks a rule. Only the size values of the selected
  /// resize mode are checked. The output folder is not checked; the export start reports it.
  [[nodiscard]] static auto Validate(const ExportRecipeOptions& options)
      -> std::optional<ExportRecipeError>;
  /// `out_dir`, or `out_dir/subfolder` when the subfolder is enabled and not blank.
  [[nodiscard]] static auto OutputFolder(const ExportRecipeOptions& options) -> QString;
  /// Reads the keys of @p values over the defaults. A size value can be a number or text: text
  /// is read like the panel fields (trimmed; the print sizes in the default locale). Text that
  /// is not a number gives NaN, which Validate rejects.
  [[nodiscard]] static auto FromVariantMap(const QVariantMap& values) -> ExportRecipeOptions;
  [[nodiscard]] static auto ToVariantMap(const ExportRecipeOptions& options) -> QVariantMap;

  /// The built-in presets: Standard, Web, Social Media, Email, Maximum Quality JPEG, 16-bit TIFF,
  /// and EXR Archive.
  [[nodiscard]] static auto BuiltInPresets() -> QList<ExportRecipePreset>;
  /// The presets that the user saved, read from QSettings `export/recipePresets`. Entries that
  /// fail Validate or have an invalid name are skipped.
  [[nodiscard]] static auto UserPresets() -> QList<ExportRecipePreset>;
  /// BuiltInPresets, then UserPresets.
  [[nodiscard]] static auto Presets() -> QList<ExportRecipePreset>;
  /// True when @p a and @p b export the same files: equal options that a preset sets, comparing
  /// only the codec values of the selected format and the size values of the selected resize mode.
  [[nodiscard]] static auto HasSamePresetOptions(const ExportRecipeOptions& a,
                                                 const ExportRecipeOptions& b) -> bool;
  /// The id of the first preset whose options HasSamePresetOptions with @p options, or empty.
  [[nodiscard]] static auto MatchingPresetId(const ExportRecipeOptions& options) -> QString;
  /// @p current with every option of @p preset except the destination.
  [[nodiscard]] static auto ApplyPreset(const ExportRecipeOptions& current,
                                        const ExportRecipePreset&  preset) -> ExportRecipeOptions;
  /// Stores @p options as the user preset @p name and returns its id. A preset with the same
  /// name (ignoring case) or with the id @p replaced_id is replaced. Rejects an empty or too long
  /// name, invalid options, and a new preset beyond kMaxUserPresetCount.
  auto SaveUserPreset(const QString& name, const ExportRecipeOptions& options,
                      const QString& replaced_id) -> std::variant<QString, ExportRecipeError>;
  /// Removes the user preset @p id. Returns false for a built-in or unknown id.
  auto DeleteUserPreset(const QString& id) -> bool;
  /// The options of the last export of the panel (QSettings `export/lastOptions`) over the
  /// defaults; the defaults when nothing valid is stored. The destination is not stored.
  [[nodiscard]] static auto LastUsedOptions() -> ExportRecipeOptions;
  static void               SaveLastUsedOptions(const ExportRecipeOptions& options);

  /// Validates @p options and starts the export of @p targets (maps with elementId and
  /// imageId) through ImportExportHandler::StartExportWithRecipeOptionsForTargets. Returns the
  /// rejected option, or the handler status (field `export`) when the handler did not start
  /// the export. Returns nothing when the export runs.
  auto                      Start(const ExportRecipeOptions& options, const QVariantList& targets)
      -> std::optional<ExportRecipeError>;

  // QML forms of the rules above.
  Q_INVOKABLE QVariantMap  DefaultOptions() const;
  Q_INVOKABLE QVariantList AllowedBitDepthsFor(const QString& format) const;
  Q_INVOKABLE int          PreferredBitDepthFor(const QString& format) const;
  Q_INVOKABLE bool         IsFileNamePatternValid(const QString& pattern) const;
  /// `{valid, field, message}` of Validate for the map form of the options.
  Q_INVOKABLE QVariantMap  ValidateOptions(const QVariantMap& options) const;
  Q_INVOKABLE QString      OutputFolderFor(const QVariantMap& options) const;
  [[nodiscard]] auto       PresetList() const -> QVariantList;
  /// MatchingPresetId for the map form of the options.
  Q_INVOKABLE QString      MatchingPresetIdFor(const QVariantMap& options) const;
  /// The map form of ApplyPreset; @p options unchanged when @p preset_id is unknown.
  Q_INVOKABLE QVariantMap  OptionsWithPreset(const QVariantMap& options,
                                             const QString&     preset_id) const;
  /// `{saved, id, field, message}` of SaveUserPreset for the map form of the options.
  Q_INVOKABLE QVariantMap  SavePreset(const QString& name, const QVariantMap& options,
                                      const QString& replaced_id);
  Q_INVOKABLE bool         DeletePreset(const QString& id);
  /// The map form of LastUsedOptions.
  Q_INVOKABLE QVariantMap  LastUsedOptionsMap() const;
  Q_INVOKABLE void         SaveLastUsedOptionsMap(const QVariantMap& options);
  /// The policy whose agent control refuses StartExport. Null refuses nothing.
  void                     SetInteractionPolicy(InteractionPolicyController* interaction_policy);
  /// The user export of the export panel: `{started, field, message}` of Start for the map form
  /// of the options. Refused with `started: false` and the reason in `message` while an
  /// automation agent holds control. The automation command calls Start.
  Q_INVOKABLE QVariantMap  StartExport(const QVariantMap& options, const QVariantList& targets);

 signals:
  void PresetsChanged();

 private:
  QPointer<ImportExportHandler> import_export_;
  InteractionPolicyController*  interaction_policy_ = nullptr;
};

}  // namespace alcedo::ui
