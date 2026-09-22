# Static source checks for the legacy pipeline removal (GPU DAG Phase G10).
#
# Usage: cmake -DALCEDO_SOURCE_ROOT=<alcedo_studio/src> -DCHECK=<name> -P legacy_removal_source_checks.cmake
#
# CHECK=NoProductCodeReadsStageTableOutsideMirror
#   Fails when a first-party product file calls a stage-table read API (GetStage(,
#   GetGlobalParams(, GetOperator() and is not one of the files that still host the stage
#   mirror or declare those APIs. Later G10 phases remove entries until the list is empty.

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED ALCEDO_SOURCE_ROOT OR NOT IS_DIRECTORY "${ALCEDO_SOURCE_ROOT}")
  message(FATAL_ERROR "ALCEDO_SOURCE_ROOT must name the alcedo_studio/src directory")
endif()
if(NOT DEFINED CHECK)
  message(FATAL_ERROR "CHECK must name the source check to run")
endif()

# Relative paths under ALCEDO_SOURCE_ROOT, as regular expressions.
set(_stage_mirror_hosts
  "^app/editor_adjustment_pipeline\\.cpp$"
  "^app/pipeline_service\\.cpp$"
  "^app/import_service\\.cpp$"
  "^edit/pipeline/pipeline_cpu\\.cpp$"
  "^ui/alcedo_main/editor_support/controllers/pipeline_controller\\.cpp$"
  "^ui/alcedo_main/album_backend/editor_history_shared_helpers\\.cpp$"
  "^edit/pipeline/pipeline_stage\\.cpp$"
  "^edit/history/edit_transaction\\.cpp$"
  "^edit/operators/"
  # Headers that declare the stage-table API itself.
  "^include/edit/pipeline/pipeline_stage\\.hpp$"
  "^include/edit/pipeline/pipeline\\.hpp$"
  "^include/edit/pipeline/pipeline_cpu\\.hpp$"
  "^include/edit/operators/"
)

function(_alcedo_is_allowed relative_path out_var)
  set(${out_var} FALSE PARENT_SCOPE)
  foreach(pattern IN LISTS _stage_mirror_hosts)
    if(relative_path MATCHES "${pattern}")
      set(${out_var} TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

if(CHECK STREQUAL "NoProductCodeReadsStageTableOutsideMirror")
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
    file(STRINGS "${ALCEDO_SOURCE_ROOT}/${relative_path}" _hits
      REGEX "(GetStage|GetGlobalParams|GetOperator)\\(")
    if(NOT _hits)
      continue()
    endif()
    _alcedo_is_allowed("${relative_path}" _allowed)
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
    message(FATAL_ERROR
      "Product code reads the stage table outside the stage mirror hosts:\n  ${_report}")
  endif()
  message(STATUS "Scanned ${_scanned} files; no stage-table read outside the mirror hosts.")
else()
  message(FATAL_ERROR "Unknown source check: ${CHECK}")
endif()
