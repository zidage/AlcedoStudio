//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <utility>

#include "ui/alcedo_main/album_backend/export_recipe_builder.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_export_commands.hpp"

namespace alcedo::automation {
namespace {

using ui::ExportRecipeBuilder;

auto ToJsonArray(const QStringList& values) -> QJsonArray {
  QJsonArray array;
  for (const QString& value : values) {
    array.push_back(value);
  }
  return array;
}

auto Range(double minimum, double maximum) -> QJsonObject {
  return QJsonObject{{"min", minimum}, {"max", maximum}};
}

auto BitDepthsByFormat() -> QJsonObject {
  QJsonObject depths;
  for (const QString& format : ExportRecipeBuilder::Formats()) {
    QJsonArray allowed;
    for (const int depth : ExportRecipeBuilder::AllowedBitDepths(format)) {
      allowed.push_back(depth);
    }
    depths.insert(format, allowed);
  }
  return depths;
}

/// The option parameters of export.run. Ranges and the bit depth of each format are checked by
/// ExportRecipeBuilder, which reports the allowed values; the schema gives the types and names.
auto ExportOptionProperties() -> QJsonObject {
  const auto integer = [](const char* description) {
    return QJsonObject{{"type", "integer"}, {"description", description}};
  };
  const auto number = [](const char* description) {
    return QJsonObject{{"type", "number"}, {"description", description}};
  };
  const auto boolean = [](const char* description) {
    return QJsonObject{{"type", "boolean"}, {"description", description}};
  };
  const auto one_of = [](const QStringList& values, const char* description) {
    return QJsonObject{
        {"type", "string"}, {"enum", ToJsonArray(values)}, {"description", description}};
  };
  return QJsonObject{
      {"format", one_of(ExportRecipeBuilder::Formats(),
                        "Output format of SDR photos. HDR photos export as UltraHDR JPEG.")},
      {"bit_depth", integer("JPEG 8; PNG 8, 16; TIFF 8, 16, 32; EXR 16, 32.")},
      {"quality", integer("JPEG quality, 1 to 100.")},
      {"png_level", integer("PNG compression level, 0 to 9.")},
      {"tiff_compression", one_of(ExportRecipeBuilder::TiffCompressions(), "TIFF compression.")},
      {"include_metadata", boolean("Write the photo metadata.")},
      {"embed_icc_profile", boolean("Embed the output ICC profile.")},
      {"include_gps", boolean("Write the GPS location of the photo (with include_metadata).")},
      {"ultra_hdr_quality", integer("UltraHDR JPEG quality of HDR photos, 1 to 100.")},
      {"ultra_hdr_dither", boolean("Dither the UltraHDR gain map.")},
      {"ultra_hdr_long_edge", integer("Longest edge of HDR photos in pixels, 256 to 8192.")},
      {"subfolder",
       QJsonObject{{"type", "string"}, {"description", "Write into this subfolder of out_dir."}}},
      {"resize_mode",
       one_of(ExportRecipeBuilder::ResizeModes(),
              "original, long_edge (long_edge), bounds (width, height), or print "
              "(print_width, print_height, print_unit, dpi). Without it, the first size value "
              "given selects the mode.")},
      {"long_edge", integer("Long edge in pixels, 256 to 16384.")},
      {"width", integer("Bounds width in pixels, 1 to 100000.")},
      {"height", integer("Bounds height in pixels, 1 to 100000.")},
      {"print_width", number("Print width, more than 0 and at most 100000.")},
      {"print_height", number("Print height, more than 0 and at most 100000.")},
      {"print_unit", one_of(ExportRecipeBuilder::PrintUnits(), "Unit of the print size.")},
      {"dpi", integer("Print resolution, 1 to 10000.")},
      {"file_name_pattern",
       QJsonObject{{"type", "string"},
                   {"description",
                    "File name pattern, for example {source} or "
                    "{date:yyyy MM dd}-{sequence:0000}. Fields: source, date, sequence, "
                    "cameraMake, cameraModel, lens, iso, aperture, shutter, focal, rating."}}}};
}

/// The export.run parameters in the option map of ExportRecipeBuilder.
auto RecipeValues(const QJsonObject& params) -> QVariantMap {
  QVariantMap values = params.toVariantMap();
  values.remove(QStringLiteral("element_ids"));
  values.insert(QStringLiteral("out_dir"),
                QFileInfo(params.value("out_dir").toString()).absoluteFilePath());
  if (params.contains("subfolder")) {
    values.insert(QStringLiteral("subfolder_enabled"), true);
  }
  if (!params.contains("resize_mode")) {
    if (params.contains("long_edge")) {
      values.insert(QStringLiteral("resize_mode"), QStringLiteral("long_edge"));
    } else if (params.contains("width") || params.contains("height")) {
      values.insert(QStringLiteral("resize_mode"), QStringLiteral("bounds"));
    } else if (params.contains("print_width") || params.contains("print_height") ||
               params.contains("dpi")) {
      values.insert(QStringLiteral("resize_mode"), QStringLiteral("print"));
    }
  }
  return values;
}

auto ExportCounts(const ui::ImportExportHandler& import_export) -> QJsonObject {
  return QJsonObject{{"total", import_export.ExportTotal()},
                     {"completed", import_export.ExportCompleted()},
                     {"succeeded", import_export.ExportSucceeded()},
                     {"failed", import_export.ExportFailed()},
                     {"skipped", import_export.ExportSkipped()}};
}

/// The photos of the last export run and their state (queued, running, succeeded, failed).
auto ExportItems(const ui::ImportExportHandler& import_export) -> QJsonArray {
  QJsonArray items;
  const auto statuses = import_export.ExportItemStatuses();
  for (auto it = statuses.cbegin(); it != statuses.cend(); ++it) {
    const QStringList ids = it.key().split(QLatin1Char(':'));
    if (ids.size() != 2) {
      continue;
    }
    items.push_back(QJsonObject{{"element_id", ids.at(0).toDouble()},
                                {"image_id", ids.at(1).toDouble()},
                                {"state", it.value().toString()}});
  }
  return items;
}

auto QueueRows(const ui::ExportQueue& queue) -> QJsonArray {
  QJsonArray rows;
  for (const auto& entry : queue.Entries()) {
    rows.push_back(QJsonObject{{"element_id", static_cast<double>(entry.element_id)},
                               {"image_id", static_cast<double>(entry.image_id)},
                               {"file_name", entry.file_name},
                               {"is_hdr", entry.is_hdr}});
  }
  return rows;
}

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

}  // namespace

auto RegisterAutomationExportCommands(AutomationCommandRegistry& registry,
                                      ui::ApplicationModuleHost* host,
                                      AutomationTaskTracker* tracker, QString* error) -> bool {
  if (host == nullptr || tracker == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("the export commands need a host and a task tracker");
    }
    return false;
  }
  bool                  ok = true;

  AutomationCommandSpec options;
  options.method      = QStringLiteral("export.options");
  options.description = QStringLiteral(
      "Returns the export defaults, the formats, the bit depths of each format, the TIFF "
      "compressions, the resize modes, the print units, and the limits of the size and quality "
      "values. export.run uses the same rules as the export panel.");
  options.params_schema = AutomationClosedParamsSchema();
  options.result_schema =
      AutomationObjectSchema(QJsonObject{{"defaults", QJsonObject{{"type", "object"}}},
                                         {"formats", QJsonObject{{"type", "array"}}},
                                         {"bit_depths", QJsonObject{{"type", "object"}}},
                                         {"tiff_compressions", QJsonObject{{"type", "array"}}},
                                         {"resize_modes", QJsonObject{{"type", "array"}}},
                                         {"print_units", QJsonObject{{"type", "array"}}},
                                         {"limits", QJsonObject{{"type", "object"}}}},
                             QJsonArray{"defaults", "formats", "bit_depths", "tiff_compressions",
                                        "resize_modes", "print_units", "limits"});
  options.handler = [host](const QJsonObject&, AutomationReply reply) {
    QJsonObject defaults = QJsonObject::fromVariantMap(
        ExportRecipeBuilder::ToVariantMap(ExportRecipeBuilder::Defaults()));
    // The panel options without their automation meaning: the folder comes from out_dir and
    // subfolder.
    defaults.remove("subfolder_enabled");
    defaults.insert("subfolder", "");
    defaults.insert("out_dir", host->import_export()->DefaultExportFolder());
    using B = ExportRecipeBuilder;
    reply.SendResult(QJsonObject{
        {"defaults", defaults},
        {"formats", ToJsonArray(B::Formats())},
        {"bit_depths", BitDepthsByFormat()},
        {"tiff_compressions", ToJsonArray(B::TiffCompressions())},
        {"resize_modes", ToJsonArray(B::ResizeModes())},
        {"print_units", ToJsonArray(B::PrintUnits())},
        {"limits",
         QJsonObject{{"long_edge", Range(B::kMinLongEdge, B::kMaxLongEdge)},
                     {"width", Range(B::kMinPixelBound, B::kMaxPixelBound)},
                     {"height", Range(B::kMinPixelBound, B::kMaxPixelBound)},
                     {"print_width", QJsonObject{{"exclusive_min", 0}, {"max", B::kMaxPrintSize}}},
                     {"print_height", QJsonObject{{"exclusive_min", 0}, {"max", B::kMaxPrintSize}}},
                     {"dpi", Range(B::kMinDpi, B::kMaxDpi)},
                     {"quality", Range(B::kMinQuality, B::kMaxQuality)},
                     {"ultra_hdr_quality", Range(B::kMinQuality, B::kMaxQuality)},
                     {"ultra_hdr_long_edge", Range(B::kMinLongEdge, B::kUltraHdrMaxLongEdge)},
                     {"png_level", Range(B::kMinPngLevel, B::kMaxPngLevel)}}}});
  };
  RegisterOrFail(registry, std::move(options), &ok, error);

  AutomationCommandSpec run;
  run.method      = QStringLiteral("export.run");
  run.description = QStringLiteral(
      "Exports the photos 'element_ids' into 'out_dir' with the given options (see "
      "export.options; an option that is not given has its default). An option that breaks a "
      "rule is rejected with the allowed values and no export starts. Returns the task id for "
      "tasks.wait as soon as the export runs; the finished task has the counts, and "
      "export.status lists the state of each photo.");
  run.changes_state     = true;
  QJsonObject run_props = ExportOptionProperties();
  run_props.insert("element_ids",
                   AutomationElementIdListSchema(QStringLiteral("The photos to export.")));
  run_props.insert("out_dir", QJsonObject{{"type", "string"},
                                          {"description",
                                           "The output folder. It is created "
                                           "when it does not exist."}});
  run.params_schema = AutomationClosedParamsSchema(run_props, QJsonArray{"element_ids", "out_dir"});
  run.result_schema =
      AutomationObjectSchema(QJsonObject{{"task_id", QJsonObject{{"type", "string"}}},
                                         {"queued", QJsonObject{{"type", "integer"}}},
                                         {"skipped", QJsonObject{{"type", "integer"}}}},
                             QJsonArray{"task_id", "queued", "skipped"});
  run.handler = [host, tracker](const QJsonObject& params, AutomationReply reply) {
    const ui::ExportRecipeOptions options =
        ExportRecipeBuilder::FromVariantMap(RecipeValues(params));
    if (const auto rejected = ExportRecipeBuilder::Validate(options)) {
      SendAutomationParamError(reply, QStringLiteral("/") + rejected->field, rejected->message);
      return;
    }
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    if (host->import_export()->export_inflight()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Busy,
                               QStringLiteral("An export is already running."));
      return;
    }
    const auto rows = ResolveAutomationElementRows(*project, params, reply);
    if (!rows.has_value()) {
      return;
    }
    QVariantList targets;
    for (const auto& row : *rows) {
      targets.push_back(QVariantMap{{QStringLiteral("elementId"), static_cast<uint>(row.file_id_)},
                                    {QStringLiteral("imageId"), static_cast<uint>(row.image_id_)}});
    }
    if (const auto failure = host->export_recipe()->Start(options, targets)) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Rejected, failure->message);
      return;
    }
    reply.SendResult(QJsonObject{{"task_id", tracker->RunningExportId()},
                                 {"queued", host->import_export()->ExportTotal()},
                                 {"skipped", host->import_export()->ExportSkipped()}});
  };
  RegisterOrFail(registry, std::move(run), &ok, error);

  AutomationCommandSpec status;
  status.method      = QStringLiteral("export.status");
  status.description = QStringLiteral(
      "Returns the state of the running or last export: the task id, the status message, the "
      "counts, the error summary, and the state of each photo. 'queue' lists the export queue "
      "of the export panel (empty in the headless host).");
  status.params_schema = AutomationClosedParamsSchema();
  status.result_schema = AutomationObjectSchema(
      QJsonObject{{"in_flight", QJsonObject{{"type", "boolean"}}},
                  {"task_id", QJsonObject{{"type", "string"}}},
                  {"message", QJsonObject{{"type", "string"}}},
                  {"counts", QJsonObject{{"type", "object"}}},
                  {"error_summary", QJsonObject{{"type", "string"}}},
                  {"items", QJsonObject{{"type", "array"}}},
                  {"queue", QJsonObject{{"type", "array"}}}},
      QJsonArray{"in_flight", "task_id", "message", "counts", "error_summary", "items", "queue"});
  status.handler = [host, tracker](const QJsonObject&, AutomationReply reply) {
    const ui::ImportExportHandler& import_export = *host->import_export();
    reply.SendResult(QJsonObject{{"in_flight", import_export.ExportInFlight()},
                                 {"task_id", tracker->LastExportId()},
                                 {"message", import_export.ExportStatus()},
                                 {"counts", ExportCounts(import_export)},
                                 {"error_summary", import_export.ExportErrorSummary()},
                                 {"items", ExportItems(import_export)},
                                 {"queue", QueueRows(*host->export_queue())}});
  };
  RegisterOrFail(registry, std::move(status), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
