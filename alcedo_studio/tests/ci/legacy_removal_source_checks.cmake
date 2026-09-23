# Static source checks for the legacy pipeline removal (GPU DAG Phase G10).
#
# Usage: cmake -DALCEDO_SOURCE_ROOT=<alcedo_studio/src> -DCHECK=<name> -P legacy_removal_source_checks.cmake
#
# CHECK=NoProductCodeReadsStageTableOutsideMirror
#   Fails when a first-party product file calls a stage-table read API (GetStage(,
#   GetGlobalParams(, GetOperator() and is not one of the files that still host the stage
#   mirror or declare those APIs. Later G10 phases remove entries until the list is empty.
#
# CHECK=NoProductCodeUsesStageJsonOutsideLegacyOwners
#   Fails when a first-party product file calls ExportPipelineParams( or ImportPipelineParams(
#   outside edit/pipeline/ and the legacy owners that later phases delete (G10.3 removed every
#   stage-JSON rollback; the legacy history store goes in G10.4 and the mapper import in G10.7).

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED ALCEDO_SOURCE_ROOT OR NOT IS_DIRECTORY "${ALCEDO_SOURCE_ROOT}")
  message(FATAL_ERROR "ALCEDO_SOURCE_ROOT must name the alcedo_studio/src directory")
endif()
if(NOT DEFINED CHECK)
  message(FATAL_ERROR "CHECK must name the source check to run")
endif()

# Relative paths under ALCEDO_SOURCE_ROOT, as regular expressions.
set(_stage_mirror_hosts
  "^app/pipeline_service\\.cpp$"
  "^app/import_service\\.cpp$"
  "^edit/pipeline/pipeline_cpu\\.cpp$"
  "^ui/alcedo_main/editor_support/controllers/pipeline_controller\\.cpp$"
  "^edit/pipeline/pipeline_stage\\.cpp$"
  "^edit/history/edit_transaction\\.cpp$"
  "^edit/operators/"
  # Headers that declare the stage-table API itself.
  "^include/edit/pipeline/pipeline_stage\\.hpp$"
  "^include/edit/pipeline/pipeline\\.hpp$"
  "^include/edit/pipeline/pipeline_cpu\\.hpp$"
  "^include/edit/operators/"
)

set(_stage_json_owners
  # The executor that defines the API.
  "^edit/pipeline/"
  "^include/edit/pipeline/"
  # Legacy history store (G10.4).
  "^app/editor_history_materializer\\.cpp$"
  "^edit/history/edit_history\\.cpp$"
  "^edit/history/version\\.cpp$"
  # Stage-JSON import for format_version < 2 (G10.7).
  "^storage/mapper/pipeline/pipeline_mapper\\.cpp$"
)

function(_alcedo_matches_any relative_path patterns_var out_var)
  set(${out_var} FALSE PARENT_SCOPE)
  foreach(pattern IN LISTS ${patterns_var})
    if(relative_path MATCHES "${pattern}")
      set(${out_var} TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

# Scan every first-party source for api_regex and fail with one line per file that is not
# matched by allowed_var.
function(_alcedo_scan api_regex allowed_var failure_text success_text)
  file(GLOB_RECURSE _sources RELATIVE "${ALCEDO_SOURCE_ROOT}"
    "${ALCEDO_SOURCE_ROOT}/*.cpp" "${ALCEDO_SOURCE_ROOT}/*.hpp" "${ALCEDO_SOURCE_ROOT}/*.h"
    "${ALCEDO_SOURCE_ROOT}/*.cu" "${ALCEDO_SOURCE_ROOT}/*.cuh" "${ALCEDO_SOURCE_ROOT}/*.mm"
    "${ALCEDO_SOURCE_ROOT}/*.inl" "${ALCEDO_SOURCE_ROOT}/*.qml")
  set(_violations "")
  set(_scanned 0)
  foreach(relative_path IN LISTS _sources)
    if(relative_path MATCHES "^third_party/")
      continue()
    endif()
    math(EXPR _scanned "${_scanned} + 1")
    file(STRINGS "${ALCEDO_SOURCE_ROOT}/${relative_path}" _hits REGEX "${api_regex}")
    if(NOT _hits)
      continue()
    endif()
    _alcedo_matches_any("${relative_path}" ${allowed_var} _allowed)
    if(NOT _allowed)
      list(GET _hits 0 _first_hit)
      string(STRIP "${_first_hit}" _first_hit)
      list(APPEND _violations "${relative_path}: ${_first_hit}")
    endif()
  endforeach()
  if(_scanned EQUAL 0)
    message(FATAL_ERROR "No source files found under ${ALCEDO_SOURCE_ROOT}")
  endif()
  if(_violations)
    list(JOIN _violations "\n  " _report)
    message(FATAL_ERROR "${failure_text}:\n  ${_report}")
  endif()
  message(STATUS "Scanned ${_scanned} files; ${success_text}.")
endfunction()

if(CHECK STREQUAL "NoProductCodeReadsStageTableOutsideMirror")
  _alcedo_scan("(GetStage|GetGlobalParams|GetOperator)\\(" _stage_mirror_hosts
    "Product code reads the stage table outside the stage mirror hosts"
    "no stage-table read outside the mirror hosts")
elseif(CHECK STREQUAL "NoProductCodeUsesStageJsonOutsideLegacyOwners")
  # The leading class excludes accessors such as EditHistory::GetImportPipelineParams().
  _alcedo_scan("(^|[^A-Za-z0-9_])(Export|Import)PipelineParams\\(" _stage_json_owners
    "Product code exports or imports stage JSON outside its legacy owners"
    "no stage-JSON export or import outside the legacy owners")
else()
  message(FATAL_ERROR "Unknown source check: ${CHECK}")
endif()
