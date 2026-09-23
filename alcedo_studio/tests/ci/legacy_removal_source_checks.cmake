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
#   stage-JSON rollback, G10.4 removed the legacy history store, and the mapper import goes in
#   G10.7).
#
# CHECK=NoSourceReferencesLegacyHistoryStore
#   Fails when a source or CMake file under ALCEDO_SOURCE_ROOT names a type, table, or target of
#   the legacy history store that G10.4 archived (the edit-history table and object, Version and
#   transaction types, the image journal and its writer port, the history management service, the
#   legacy materializer, and the recovery-metadata table). Run once for src and once for tests.
#
# CHECK=RuntimeSourcesDoNotIncludeLegacyOperatorHeaders
#   Fails when a file under edit/runtime/ or include/edit/runtime/ includes a legacy operator or
#   stage parameter header (op_base.hpp, odt_op.hpp, param.cuh, fused_param.hpp,
#   opencl_param.hpp). G10.5 moved the DRT resolution and parameter layouts out of them.
#
# CHECK=DagSourcesDoNotReferenceLegacyDirectories
#   Fails when a file under edit/runtime/, include/edit/runtime/, edit/graph/, include/edit/graph/,
#   renderer/, or include/renderer/ includes a path under edit/pipeline/ other than the executor
#   header (pipeline_cpu.hpp), a path under edit/operators/GPU_kernels/ or
#   edit/operators/CPU_kernels/, or any other header under edit/operators/ except the Model,
#   utility, and shared data headers. G10.6 moved the lens resolver and kernels, the CUDA detail and
#   film grain helpers, the Metal PRNG, and the local-tone and apply-request headers out of them.

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

# The static-check script and the CMake file that registers its negative fixtures spell the
# legacy names on purpose.
set(_legacy_history_check_hosts
  "^ci/legacy_removal_source_checks\\.cmake$"
  "^ci/CMakeLists\\.txt$"
)

# Scan every first-party source for api_regex and fail with one line per file that is not
# matched by allowed_var. Extra glob patterns (relative to ALCEDO_SOURCE_ROOT) may follow.
function(_alcedo_scan api_regex allowed_var failure_text success_text)
  set(_extra_globs "")
  foreach(_pattern IN LISTS ARGN)
    list(APPEND _extra_globs "${ALCEDO_SOURCE_ROOT}/${_pattern}")
  endforeach()
  file(GLOB_RECURSE _sources RELATIVE "${ALCEDO_SOURCE_ROOT}"
    "${ALCEDO_SOURCE_ROOT}/*.cpp" "${ALCEDO_SOURCE_ROOT}/*.hpp" "${ALCEDO_SOURCE_ROOT}/*.h"
    "${ALCEDO_SOURCE_ROOT}/*.cu" "${ALCEDO_SOURCE_ROOT}/*.cuh" "${ALCEDO_SOURCE_ROOT}/*.mm"
    "${ALCEDO_SOURCE_ROOT}/*.inl" "${ALCEDO_SOURCE_ROOT}/*.qml" ${_extra_globs})
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

# Scan the files matched by scope_var for api_regex and fail with one line per matching file.
function(_alcedo_scan_within api_regex scope_var failure_text success_text)
  file(GLOB_RECURSE _sources RELATIVE "${ALCEDO_SOURCE_ROOT}"
    "${ALCEDO_SOURCE_ROOT}/*.cpp" "${ALCEDO_SOURCE_ROOT}/*.hpp" "${ALCEDO_SOURCE_ROOT}/*.h"
    "${ALCEDO_SOURCE_ROOT}/*.cu" "${ALCEDO_SOURCE_ROOT}/*.cuh" "${ALCEDO_SOURCE_ROOT}/*.mm"
    "${ALCEDO_SOURCE_ROOT}/*.inl" "${ALCEDO_SOURCE_ROOT}/*.cl" "${ALCEDO_SOURCE_ROOT}/*.metal")
  set(_violations "")
  set(_scanned 0)
  foreach(relative_path IN LISTS _sources)
    _alcedo_matches_any("${relative_path}" ${scope_var} _in_scope)
    if(NOT _in_scope)
      continue()
    endif()
    math(EXPR _scanned "${_scanned} + 1")
    file(STRINGS "${ALCEDO_SOURCE_ROOT}/${relative_path}" _hits REGEX "${api_regex}")
    if(_hits)
      list(GET _hits 0 _first_hit)
      string(STRIP "${_first_hit}" _first_hit)
      list(APPEND _violations "${relative_path}: ${_first_hit}")
    endif()
  endforeach()
  if(_scanned EQUAL 0)
    message(FATAL_ERROR "No source files found in the check scope under ${ALCEDO_SOURCE_ROOT}")
  endif()
  if(_violations)
    list(JOIN _violations "
  " _report)
    message(FATAL_ERROR "${failure_text}:
  ${_report}")
  endif()
  message(STATUS "Scanned ${_scanned} files; ${success_text}.")
endfunction()

set(_runtime_scope
  "^edit/runtime/"
  "^include/edit/runtime/"
)

set(_dag_scope
  "^edit/runtime/"
  "^include/edit/runtime/"
  "^edit/graph/"
  "^include/edit/graph/"
  "^renderer/"
  "^include/renderer/"
)

# Headers under edit/operators/ that are Model, utility, or shared data headers, not legacy
# operators. G10.9 keeps them in the build.
set(_dag_allowed_operator_headers
  "^edit/operators/models/"
  "^edit/operators/utils/"
  "^edit/operators/basic/planckian_locus_table\\.hpp$"
  "^edit/operators/basic/camera_matrices\\.hpp$"
  "^edit/operators/geometry/resize_algorithm\\.hpp$"
)

# Report one line per DAG file that includes a legacy pipeline or operator path.
function(_alcedo_scan_dag_includes)
  file(GLOB_RECURSE _sources RELATIVE "${ALCEDO_SOURCE_ROOT}"
    "${ALCEDO_SOURCE_ROOT}/*.cpp" "${ALCEDO_SOURCE_ROOT}/*.hpp" "${ALCEDO_SOURCE_ROOT}/*.h"
    "${ALCEDO_SOURCE_ROOT}/*.cu" "${ALCEDO_SOURCE_ROOT}/*.cuh" "${ALCEDO_SOURCE_ROOT}/*.mm"
    "${ALCEDO_SOURCE_ROOT}/*.inl" "${ALCEDO_SOURCE_ROOT}/*.cl" "${ALCEDO_SOURCE_ROOT}/*.metal")
  set(_violations "")
  set(_scanned 0)
  foreach(relative_path IN LISTS _sources)
    _alcedo_matches_any("${relative_path}" _dag_scope _in_scope)
    if(NOT _in_scope)
      continue()
    endif()
    math(EXPR _scanned "${_scanned} + 1")
    file(STRINGS "${ALCEDO_SOURCE_ROOT}/${relative_path}" _includes
      REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    foreach(_line IN LISTS _includes)
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[<\"]([^\">]*)[\">].*$" "\\1"
             _target "${_line}")
      # Relative shader includes such as ../../../operators/GPU_kernels/x.metal name the same
      # directories; drop leading ../ segments before the match.
      string(REGEX REPLACE "^(\\.\\./)+" "" _target "${_target}")
      if(_target MATCHES "^(edit/)?operators/")
        string(REGEX REPLACE "^(edit/)?operators/" "edit/operators/" _target "${_target}")
      endif()
      set(_legacy FALSE)
      if(_target MATCHES "^edit/pipeline/" AND NOT _target STREQUAL "edit/pipeline/pipeline_cpu.hpp")
        set(_legacy TRUE)
      elseif(_target MATCHES "^edit/operators/(GPU_kernels|CPU_kernels)/")
        set(_legacy TRUE)
      elseif(_target MATCHES "^edit/operators/")
        _alcedo_matches_any("${_target}" _dag_allowed_operator_headers _allowed)
        if(NOT _allowed)
          set(_legacy TRUE)
        endif()
      endif()
      if(_legacy)
        string(STRIP "${_line}" _line)
        list(APPEND _violations "${relative_path}: ${_line}")
      endif()
    endforeach()
  endforeach()
  if(_scanned EQUAL 0)
    message(FATAL_ERROR "No source files found in the check scope under ${ALCEDO_SOURCE_ROOT}")
  endif()
  if(_violations)
    list(JOIN _violations "
  " _report)
    message(FATAL_ERROR "DAG source includes a legacy pipeline or operator path:
  ${_report}")
  endif()
  message(STATUS "Scanned ${_scanned} files; no DAG source includes a legacy pipeline or operator path.")
endfunction()

if(CHECK STREQUAL "NoProductCodeReadsStageTableOutsideMirror")
  _alcedo_scan("(GetStage|GetGlobalParams|GetOperator)\\(" _stage_mirror_hosts
    "Product code reads the stage table outside the stage mirror hosts"
    "no stage-table read outside the mirror hosts")
elseif(CHECK STREQUAL "NoProductCodeUsesStageJsonOutsideLegacyOwners")
  # The leading class excludes accessors such as Document::GetImportPipelineParams().
  _alcedo_scan("(^|[^A-Za-z0-9_])(Export|Import)PipelineParams\\(" _stage_json_owners
    "Product code exports or imports stage JSON outside its legacy owners"
    "no stage-JSON export or import outside the legacy owners")
elseif(CHECK STREQUAL "NoSourceReferencesLegacyHistoryStore")
  _alcedo_scan(
    "(^|[^A-Za-z0-9_])(Edit[H]istory|Edit[H]istoryMgmtService|Edit[H]istoryMapper|EditTransaction|EditorTransactionJournal|EditorJournalWriter|IEditorJournalPort|EditorSessionJournalWriterPort|EditorHistoryMaterializer|EditorRecoveryMetadata)([^A-Za-z0-9_]|$)"
    _legacy_history_check_hosts
    "Source names the archived legacy history store"
    "no reference to the archived legacy history store"
    "*.txt" "*.cmake")
elseif(CHECK STREQUAL "RuntimeSourcesDoNotIncludeLegacyOperatorHeaders")
  _alcedo_scan_within(
    "#[ \t]*include[ \t]*[<\"]([^\">]*/)?(op_base\\.hpp|odt_op\\.hpp|param\\.cuh|fused_param\\.hpp|opencl_param\\.hpp)[\">]"
    _runtime_scope
    "Runtime source includes a legacy operator or stage parameter header"
    "no runtime source includes a legacy operator or stage parameter header")
elseif(CHECK STREQUAL "DagSourcesDoNotReferenceLegacyDirectories")
  _alcedo_scan_dag_includes()
else()
  message(FATAL_ERROR "Unknown source check: ${CHECK}")
endif()
