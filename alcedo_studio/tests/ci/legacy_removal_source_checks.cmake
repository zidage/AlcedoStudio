# Static source checks for the legacy pipeline removal (GPU DAG Phase G10).
#
# Usage: cmake -DALCEDO_SOURCE_ROOT=<alcedo_studio/src> -DCHECK=<name> -P legacy_removal_source_checks.cmake
#
# CHECK=NoProductCodeUsesStageTable
#   Fails when a first-party product file uses the stage table (GetStage(, GetGlobalParams(,
#   GetOperator(, SetOperator(, or a PipelineStageName value). G10.7 removed the stage table from
#   the executor, the services, and the controllers; G10.9 archived the stage and the legacy
#   operators, so no file may use it.
#
# CHECK=NoProductCodeUsesStageJsonOutsideLegacyOwners
#   Fails when a first-party product file calls ExportPipelineParams( or ImportPipelineParams(.
#   G10.3 removed every stage-JSON rollback, G10.4 removed the legacy history store, G10.7 removed
#   the executor API and the mapper import, and G10.9 archived the stage that owned the rest.
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
#   header (pipeline_executor.hpp), a path under edit/operators/GPU_kernels/ or
#   edit/operators/CPU_kernels/, or any other header under edit/operators/ except the Model,
#   utility, and shared data headers. G10.6 moved the lens resolver and kernels, the CUDA detail and
#   film grain helpers, the Metal PRNG, and the local-tone and apply-request headers out of them.
#
# G10.9 archived the stage, the legacy operators, and the legacy importer to
# alcedo_studio/deprecated/legacy_pipeline/. The checks below keep them out of the build.
#
# CHECK=DeprecatedLegacyArchiveIsOutsideCompileGraph
#   Needs REPO_ROOT (the repository root) instead of ALCEDO_SOURCE_ROOT; COMPILE_COMMANDS (a
#   compile database) is optional. Fails when a CMakeLists.txt or *.cmake file in the repository
#   root, alcedo_studio, scripts, or vcpkg-overlays (third_party excluded) names the archive
#   directory, or when the compile database lists a file in it. This script is the only CMake file
#   that may spell the archive path.
#
# CHECK=NoPipelineStageTypeRemainsInFirstPartySource
#   Fails when a source file under ALCEDO_SOURCE_ROOT, or under ALCEDO_TEST_SOURCE_ROOT when it is
#   given, names PipelineStage or PipelineStageName.
#
# CHECK=NoOperatorParamsAggregateRemainsInFirstPartySource
#   Fails when a source file names the OperatorParams or GPUOperatorParams aggregate. Longer
#   identifiers that contain the name, such as loadFromOperatorParams, do not match. Same roots as
#   the stage check.
#
# CHECK=AllBuiltInOperatorModelsHaveNoImageApplyEntryPoint
#   Fails when a file under edit/operators/models/ or include/edit/operators/models/ names Apply(
#   or ApplyGPU(. A Model holds parameters; the GPU DAG passes process images.
#
# CHECK=NoLegacyParameterImporterOrStageAdapterRemainsInProductPath
#   Fails when a product source file names LegacyPipelineImporter, legacy_stage_adapter, or
#   MirrorsLegacyStageAdapter.
#
# CHECK=NoLegacyOperatorTypeEnumRemains
#   Fails when a source file uses an OperatorType:: value or declares enum class OperatorType.
#   OperatorTypeId, the Model type identifier, does not match. Same roots as the stage check.
#
# G10.10 archived the OpenCL and Metal legacy programs and shaders, RawProcessor, RawDecoder, the
# CPU RAW operators, and the RAW GPU wrappers that only RawProcessor called.
#
# CHECK=NoLegacyOpenClPipelineFactoryRemains
#   Fails when a source or CMake file names the legacy OpenCL pipeline (CreateOpenCLGPUPipeline,
#   OpenCLGPUPipeline, OpenClFusedParams, OpenClFusedParamUploader, OpenClStage), its program
#   manifest (RegisterOpenClEditPipelinePrograms, opencl_pipeline_programs), or its program
#   directory (edit/pipeline/opencl_shader). Same roots as the stage check.
#
# CHECK=NoLegacyMetalPipelineFactoryRemains
#   Fails when a source, shader, or CMake file names the legacy Metal pipeline
#   (CreateMetalGPUPipeline, MetalGPUPipeline, MetalFusedParams, MetalStage), the fused pipeline
#   shader or its target (fused_pipeline.metal, METAL_FUSED_PIPELINE, EditPipelineMetalShaders), or
#   the legacy operator shader directory (edit/operators/GPU_kernels). Same roots as the stage
#   check.
#
# CHECK=NoRawProcessorEntryRemainsInProductBuild
#   Fails when a source file under ALCEDO_SOURCE_ROOT constructs or declares RawProcessor, names
#   RawDecoder, RawParams, or RawGpuBackend, or includes raw_processor.hpp,
#   raw_processor_internal.hpp, or raw_decoder.hpp; or when a CMake file under ALCEDO_SOURCE_ROOT
#   or ALCEDO_TEST_SOURCE_ROOT names the RawProcessor target or a RawProcessor, RawDecoder, or CPU
#   RAW operator source. The OpenCL::RawProcessor program-name namespace, RawProcessorOp, and
#   raw_processor_pattern.hpp do not match.
#
# CHECK=NoLegacyMetalMetallibIsPackaged
#   Needs METAL_RUNTIME_LIBS (the ALCEDO_METAL_RUNTIME_LIBS list with "|" as the separator);
#   METALLIB_DIR (an installed bundle's metallib directory) is optional. Fails when the runtime
#   list or the directory holds the fused pipeline metallib, or when the runtime list misses a
#   GPU DAG metallib.

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CHECK)
  message(FATAL_ERROR "CHECK must name the source check to run")
endif()
if(CHECK STREQUAL "DeprecatedLegacyArchiveIsOutsideCompileGraph")
  if(NOT DEFINED REPO_ROOT OR NOT IS_DIRECTORY "${REPO_ROOT}")
    message(FATAL_ERROR "REPO_ROOT must name the repository root")
  endif()
elseif(CHECK STREQUAL "NoLegacyMetalMetallibIsPackaged")
  if(NOT DEFINED METAL_RUNTIME_LIBS)
    message(FATAL_ERROR "METAL_RUNTIME_LIBS must list the Metal runtime libraries")
  endif()
elseif(NOT DEFINED ALCEDO_SOURCE_ROOT OR NOT IS_DIRECTORY "${ALCEDO_SOURCE_ROOT}")
  message(FATAL_ERROR "ALCEDO_SOURCE_ROOT must name the alcedo_studio/src directory")
endif()
if(DEFINED ALCEDO_TEST_SOURCE_ROOT AND NOT IS_DIRECTORY "${ALCEDO_TEST_SOURCE_ROOT}")
  message(FATAL_ERROR "ALCEDO_TEST_SOURCE_ROOT must name the alcedo_studio/tests directory")
endif()

# Relative paths under ALCEDO_SOURCE_ROOT that may use an API, as regular expressions. G10.9
# archived the stage and the legacy operators, so no file owns the stage table or stage JSON.
set(_stage_table_owners "")
set(_stage_json_owners "")
set(_no_owners "")

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

# Scan every first-party source under ALCEDO_SOURCE_ROOT, and under ALCEDO_TEST_SOURCE_ROOT when it
# is given, for api_regex and fail with one line per file that is not matched by allowed_var.
# Files under the test root are reported with a "tests:" prefix. Extra glob patterns (relative to
# each root) may follow.
function(_alcedo_scan api_regex allowed_var failure_text success_text)
  set(_roots "${ALCEDO_SOURCE_ROOT}")
  if(DEFINED ALCEDO_TEST_SOURCE_ROOT)
    list(APPEND _roots "${ALCEDO_TEST_SOURCE_ROOT}")
  endif()
  set(_violations "")
  set(_scanned 0)
  foreach(_root IN LISTS _roots)
    set(_label "")
    if(NOT "${_root}" STREQUAL "${ALCEDO_SOURCE_ROOT}")
      set(_label "tests:")
    endif()
    set(_extra_globs "")
    foreach(_pattern IN LISTS ARGN)
      list(APPEND _extra_globs "${_root}/${_pattern}")
    endforeach()
    file(GLOB_RECURSE _sources RELATIVE "${_root}"
      "${_root}/*.cpp" "${_root}/*.hpp" "${_root}/*.h" "${_root}/*.cu" "${_root}/*.cuh"
      "${_root}/*.mm" "${_root}/*.inl" "${_root}/*.qml" ${_extra_globs})
    foreach(relative_path IN LISTS _sources)
      if(relative_path MATCHES "^third_party/")
        continue()
      endif()
      math(EXPR _scanned "${_scanned} + 1")
      file(STRINGS "${_root}/${relative_path}" _hits REGEX "${api_regex}")
      if(NOT _hits)
        continue()
      endif()
      _alcedo_matches_any("${relative_path}" ${allowed_var} _allowed)
      if(NOT _allowed)
        list(GET _hits 0 _first_hit)
        string(STRIP "${_first_hit}" _first_hit)
        list(APPEND _violations "${_label}${relative_path}: ${_first_hit}")
      endif()
    endforeach()
  endforeach()
  if(_scanned EQUAL 0)
    message(FATAL_ERROR "No source files found under ${_roots}")
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
      if(_target MATCHES "^edit/pipeline/" AND NOT _target STREQUAL "edit/pipeline/pipeline_executor.hpp")
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

set(_model_scope
  "^edit/operators/models/"
  "^include/edit/operators/models/"
)

# A path to the deprecated archive: alcedo_studio/deprecated, ${...}/deprecated, or
# deprecated/legacy_pipeline, with forward or backward slashes.
set(_archive_path_regex
  "(alcedo_studio|})[/\\\\]+deprecated([/\\\\]|$)|deprecated[/\\\\]+legacy_pipeline")

# Report every CMake file that names the archive or lives inside it, and every compile-database
# line that names an archive path.
function(_alcedo_scan_archive_references)
  set(_files "")
  if(EXISTS "${REPO_ROOT}/CMakeLists.txt")
    list(APPEND _files "CMakeLists.txt")
  endif()
  foreach(_dir IN ITEMS alcedo_studio scripts vcpkg-overlays)
    if(IS_DIRECTORY "${REPO_ROOT}/${_dir}")
      file(GLOB_RECURSE _found RELATIVE "${REPO_ROOT}"
        "${REPO_ROOT}/${_dir}/CMakeLists.txt" "${REPO_ROOT}/${_dir}/*.cmake")
      list(APPEND _files ${_found})
    endif()
  endforeach()
  set(_violations "")
  set(_scanned 0)
  foreach(relative_path IN LISTS _files)
    if(relative_path MATCHES "(^|/)third_party/" OR
       relative_path STREQUAL "alcedo_studio/tests/ci/legacy_removal_source_checks.cmake")
      continue()
    endif()
    math(EXPR _scanned "${_scanned} + 1")
    if(relative_path MATCHES "^alcedo_studio/deprecated/")
      list(APPEND _violations "${relative_path}: CMake file inside the archive")
      continue()
    endif()
    file(STRINGS "${REPO_ROOT}/${relative_path}" _hits REGEX "${_archive_path_regex}")
    if(_hits)
      list(GET _hits 0 _first_hit)
      string(STRIP "${_first_hit}" _first_hit)
      list(APPEND _violations "${relative_path}: ${_first_hit}")
    endif()
  endforeach()
  if(_scanned EQUAL 0)
    message(FATAL_ERROR "No CMake files found under ${REPO_ROOT}")
  endif()
  if(DEFINED COMPILE_COMMANDS AND NOT COMPILE_COMMANDS STREQUAL "")
    if(NOT EXISTS "${COMPILE_COMMANDS}")
      message(FATAL_ERROR "Compile database not found: ${COMPILE_COMMANDS}")
    endif()
    file(STRINGS "${COMPILE_COMMANDS}" _db_hits REGEX "${_archive_path_regex}")
    foreach(_hit IN LISTS _db_hits)
      string(STRIP "${_hit}" _hit)
      list(APPEND _violations "compile database: ${_hit}")
    endforeach()
  endif()
  if(_violations)
    list(JOIN _violations "\n  " _report)
    message(FATAL_ERROR "The deprecated legacy archive is referenced by the build:\n  ${_report}")
  endif()
  message(STATUS "Scanned ${_scanned} CMake files and the compile database; no archive reference.")
endfunction()

# Scan CMakeLists.txt and *.cmake files under ALCEDO_SOURCE_ROOT and ALCEDO_TEST_SOURCE_ROOT for
# api_regex. The script and the file that registers its fixtures are not scanned.
function(_alcedo_scan_cmake api_regex failure_text success_text)
  set(_roots "${ALCEDO_SOURCE_ROOT}")
  if(DEFINED ALCEDO_TEST_SOURCE_ROOT)
    list(APPEND _roots "${ALCEDO_TEST_SOURCE_ROOT}")
  endif()
  set(_violations "")
  set(_scanned 0)
  foreach(_root IN LISTS _roots)
    set(_label "")
    if(NOT "${_root}" STREQUAL "${ALCEDO_SOURCE_ROOT}")
      set(_label "tests:")
    endif()
    file(GLOB_RECURSE _cmake_files RELATIVE "${_root}" "${_root}/CMakeLists.txt" "${_root}/*.cmake")
    foreach(relative_path IN LISTS _cmake_files)
      if(relative_path MATCHES "^third_party/")
        continue()
      endif()
      _alcedo_matches_any("${relative_path}" _legacy_history_check_hosts _is_host)
      if(_is_host)
        continue()
      endif()
      math(EXPR _scanned "${_scanned} + 1")
      file(STRINGS "${_root}/${relative_path}" _hits REGEX "${api_regex}")
      if(_hits)
        list(GET _hits 0 _first_hit)
        string(STRIP "${_first_hit}" _first_hit)
        list(APPEND _violations "${_label}${relative_path}: ${_first_hit}")
      endif()
    endforeach()
  endforeach()
  if(_violations)
    list(JOIN _violations "\n  " _report)
    message(FATAL_ERROR "${failure_text}:\n  ${_report}")
  endif()
  message(STATUS "Scanned ${_scanned} CMake files; ${success_text}.")
endfunction()

if(CHECK STREQUAL "NoProductCodeUsesStageTable")
  _alcedo_scan("(GetStage|GetGlobalParams|GetOperator|SetOperator)\\(|PipelineStageName::"
    _stage_table_owners
    "Product code uses the stage table"
    "no stage-table use")
elseif(CHECK STREQUAL "NoProductCodeUsesStageJsonOutsideLegacyOwners")
  # The leading class excludes accessors such as Document::GetImportPipelineParams().
  _alcedo_scan("(^|[^A-Za-z0-9_])(Export|Import)PipelineParams\\(" _stage_json_owners
    "Product code exports or imports stage JSON"
    "no stage-JSON export or import")
elseif(CHECK STREQUAL "DeprecatedLegacyArchiveIsOutsideCompileGraph")
  _alcedo_scan_archive_references()
elseif(CHECK STREQUAL "NoPipelineStageTypeRemainsInFirstPartySource")
  _alcedo_scan("PipelineStage" _no_owners
    "Source names the archived pipeline stage type"
    "no PipelineStage or PipelineStageName")
elseif(CHECK STREQUAL "NoOperatorParamsAggregateRemainsInFirstPartySource")
  _alcedo_scan("(^|[^A-Za-z0-9_])(GPU)?OperatorParams([^A-Za-z0-9_]|$)" _no_owners
    "Source names the archived operator parameter aggregate"
    "no OperatorParams or GPUOperatorParams")
elseif(CHECK STREQUAL "AllBuiltInOperatorModelsHaveNoImageApplyEntryPoint")
  _alcedo_scan_within("(^|[^A-Za-z0-9_])Apply(GPU)?[ \t]*\\(" _model_scope
    "Operator Model has an image apply entry point"
    "no operator Model has Apply( or ApplyGPU(")
elseif(CHECK STREQUAL "NoLegacyParameterImporterOrStageAdapterRemainsInProductPath")
  _alcedo_scan("LegacyPipelineImporter|legacy_stage_adapter|MirrorsLegacyStageAdapter" _no_owners
    "Product source names the archived legacy importer or stage adapter"
    "no legacy importer or stage adapter")
elseif(CHECK STREQUAL "NoLegacyOperatorTypeEnumRemains")
  _alcedo_scan(
    "(^|[^A-Za-z0-9_])OperatorType::|enum[ \t]+class[ \t]+OperatorType([^A-Za-z0-9_]|$)"
    _no_owners
    "Source uses the archived legacy OperatorType enum"
    "no legacy OperatorType enum")
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
elseif(CHECK STREQUAL "NoLegacyOpenClPipelineFactoryRemains")
  _alcedo_scan(
    "(^|[^A-Za-z0-9_])(CreateOpenCLGPUPipeline|OpenCLGPUPipeline|OpenClFusedParams|OpenClFusedParamUploader|OpenClStage|RegisterOpenClEditPipelinePrograms)([^A-Za-z0-9_]|$)|opencl_pipeline_programs|edit/pipeline/opencl_shader"
    _legacy_history_check_hosts
    "Source names the archived legacy OpenCL pipeline"
    "no legacy OpenCL pipeline, program manifest, or program directory"
    "*.cl" "*.txt" "*.cmake")
elseif(CHECK STREQUAL "NoLegacyMetalPipelineFactoryRemains")
  _alcedo_scan(
    "(^|[^A-Za-z0-9_])(CreateMetalGPUPipeline|MetalGPUPipeline|MetalFusedParams|MetalStage|EditPipelineMetalShaders)([^A-Za-z0-9_]|$)|fused_pipeline\\.metal|METAL_FUSED_PIPELINE|edit/operators/GPU_kernels"
    _legacy_history_check_hosts
    "Source names the archived legacy Metal pipeline"
    "no legacy Metal pipeline, fused shader, or legacy operator shader"
    "*.metal" "*.txt" "*.cmake")
elseif(CHECK STREQUAL "NoRawProcessorEntryRemainsInProductBuild")
  # The source half scans the product source only; a test helper may use a name such as RawParams.
  set(_test_source_root "${ALCEDO_TEST_SOURCE_ROOT}")
  unset(ALCEDO_TEST_SOURCE_ROOT CACHE)
  unset(ALCEDO_TEST_SOURCE_ROOT)
  _alcedo_scan(
    "(^|[^A-Za-z0-9_])RawProcessor[ \t]+[a-z_][A-Za-z0-9_]*[ \t]*[({;]|class[ \t]+RawProcessor([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])(RawDecoder|RawParams|RawGpuBackend)([^A-Za-z0-9_]|$)|include[ \t]*[<\"]([^\">]*/)?(raw_processor|raw_processor_internal|raw_decoder)\\.hpp"
    _no_owners
    "Product source uses the archived RawProcessor or RawDecoder"
    "no RawProcessor, RawDecoder, or RAW parameter type")
  if(_test_source_root)
    set(ALCEDO_TEST_SOURCE_ROOT "${_test_source_root}")
  endif()
  _alcedo_scan_cmake(
    "(^|[ \t(])RawProcessor([ \t)]|$)|RAW_PROCESSOR_BACKEND_SRCS|raw_processor(_cuda|_metal|_opencl)?\\.cpp|raw_decoder\\.cpp|processor/operators/cpu/"
    "A CMake file builds or links the archived RawProcessor"
    "no RawProcessor target or source")
elseif(CHECK STREQUAL "NoLegacyMetalMetallibIsPackaged")
  string(REPLACE "|" ";" _metal_libs "${METAL_RUNTIME_LIBS}")
  set(_names "")
  foreach(_lib IN LISTS _metal_libs)
    get_filename_component(_name "${_lib}" NAME)
    list(APPEND _names "${_name}")
  endforeach()
  set(_violations "")
  if("fused_pipeline.metallib" IN_LIST _names)
    list(APPEND _violations "runtime list: fused_pipeline.metallib")
  endif()
  foreach(_dag_lib IN ITEMS geometry_resample camera_color primary_grade local_tone mask drt)
    if(NOT "${_dag_lib}.metallib" IN_LIST _names)
      list(APPEND _violations "runtime list misses ${_dag_lib}.metallib")
    endif()
  endforeach()
  if(DEFINED METALLIB_DIR AND EXISTS "${METALLIB_DIR}/fused_pipeline.metallib")
    list(APPEND _violations "bundle: ${METALLIB_DIR}/fused_pipeline.metallib")
  endif()
  if(_violations)
    list(JOIN _violations "\n  " _report)
    message(FATAL_ERROR "The Metal runtime package is wrong:\n  ${_report}")
  endif()
  list(LENGTH _names _count)
  message(STATUS "Checked ${_count} Metal runtime libraries; no fused pipeline metallib.")
else()
  message(FATAL_ERROR "Unknown source check: ${CHECK}")
endif()
