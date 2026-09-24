# CI workflow test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(CiRawWorkflowTest ${ALCEDO_TEST_ROOT}/ci/ci_raw_workflow_test.cpp)
target_include_directories(CiRawWorkflowTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(CiRawWorkflowTest
  PRIVATE
    GTest::gtest_main
    ProjectService
    ImportService
    Image
    EditPipeline
    PipelineScheduler
    ThreadPool
    ${ALCEDO_TEST_OPENCV_MIN_DEPS}
)
gtest_discover_tests(CiRawWorkflowTest TEST_PREFIX "CiRawWorkflowTest."
  PROPERTIES LABELS "ci_raw_flow")
alcedo_register_test_target(CiRawWorkflowTest ci_raw)

add_executable(SleeveFilesystemCiTest ${ALCEDO_TEST_ROOT}/ci/sleeve_filesystem_ci_test.cpp)
target_include_directories(SleeveFilesystemCiTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFilesystemCiTest PRIVATE GTest::gtest_main SleeveFS StrConv)
gtest_discover_tests(SleeveFilesystemCiTest TEST_PREFIX "SleeveFilesystemCiTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(SleeveFilesystemCiTest ci_core)

# G10 legacy removal: static source checks (cmake -P, no build dependency).
# G10.7 removed the stage table from product code; G10.9 archived its last owners.
add_test(NAME NoProductCodeUsesStageTable
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_ROOT_DIR}/src
          -DCHECK=NoProductCodeUsesStageTable
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(NoProductCodeUsesStageTable
  PROPERTIES LABELS "ci_core_flow;legacy_removal")

# The same check must reject stage-table use in the executor and a service. The fixture is
# generated in the build tree so no test source outside the product tree is formatted or tidied.
set(_stage_table_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/stage_table_violation_fixture")
file(WRITE "${_stage_table_fixture_root}/edit/pipeline/pipeline_executor.cpp"
  "void Configure(Executor& e) { e.SetOperator(Kind::Exposure, {}); }\n")
file(WRITE "${_stage_table_fixture_root}/app/pipeline_service.cpp"
  "auto Name() { return PipelineStageName::Image_Loading; }\n")
add_test(NAME StageTableCheckRejectsExecutorAndServiceUse
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_stage_table_fixture_root}
          -DCHECK=NoProductCodeUsesStageTable
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(StageTableCheckRejectsExecutorAndServiceUse
  PROPERTIES PASS_REGULAR_EXPRESSION "app/pipeline_service\\.cpp: auto Name.*edit/pipeline/pipeline_executor\\.cpp: void Configure"
             LABELS "ci_core_flow;legacy_removal")

# G10.3 removed the stage-JSON rollback, G10.7 the executor API and the mapper import, and G10.9
# archived the stage. No file may copy stage JSON again.
add_test(NAME NoProductCodeUsesStageJsonOutsideLegacyOwners
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_ROOT_DIR}/src
          -DCHECK=NoProductCodeUsesStageJsonOutsideLegacyOwners
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(NoProductCodeUsesStageJsonOutsideLegacyOwners
  PROPERTIES LABELS "ci_core_flow;legacy_removal")

set(_stage_json_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/stage_json_violation_fixture")
file(WRITE "${_stage_json_fixture_root}/app/rollback.cpp"
  "void Save(Executor& e) { auto prior = e.ExportPipelineParams(); }\n")
file(WRITE "${_stage_json_fixture_root}/edit/history/accessor.hpp"
  "auto Read(const History& h) { return h.GetImportPipelineParams(); }\n")
add_test(NAME StageJsonCheckRejectsRollbackOutsideLegacyOwners
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_stage_json_fixture_root}
          -DCHECK=NoProductCodeUsesStageJsonOutsideLegacyOwners
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(StageJsonCheckRejectsRollbackOutsideLegacyOwners
  PROPERTIES PASS_REGULAR_EXPRESSION "app/rollback\.cpp: void Save"
             FAIL_REGULAR_EXPRESSION "accessor\.hpp"
             LABELS "ci_core_flow;legacy_removal")

# G10.4: the legacy history store is archived. No source or CMake file under src or tests may
# name its types, tables, or targets again.
add_test(NAME NoProductSourceReferencesLegacyHistoryStore
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_ROOT_DIR}/src
          -DCHECK=NoSourceReferencesLegacyHistoryStore
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(NoProductSourceReferencesLegacyHistoryStore
  PROPERTIES LABELS "ci_core_flow;legacy_removal")

add_test(NAME NoTestSourceReferencesLegacyHistoryStore
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_TEST_ROOT}
          -DCHECK=NoSourceReferencesLegacyHistoryStore
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(NoTestSourceReferencesLegacyHistoryStore
  PROPERTIES LABELS "ci_core_flow;legacy_removal")

# The same check must reject a source file and a CMake target list that name the legacy store,
# and must not reject Mini-Git names that share a prefix.
set(_legacy_history_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/legacy_history_violation_fixture")
file(WRITE "${_legacy_history_fixture_root}/sleeve/file.hpp"
  "class File { std::shared_ptr<EditTransaction> pending_; };\n")
file(WRITE "${_legacy_history_fixture_root}/app/CMakeLists.txt"
  "target_link_libraries(App EditorJournalWriter)\n")
file(WRITE "${_legacy_history_fixture_root}/edit/mini_git.hpp"
  "struct EditorMiniGitSaveCapture; class PipelineHistoryApplier;\n")
add_test(NAME LegacyHistoryCheckRejectsSourceAndTargetNames
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_legacy_history_fixture_root}
          -DCHECK=NoSourceReferencesLegacyHistoryStore
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(LegacyHistoryCheckRejectsSourceAndTargetNames
  PROPERTIES PASS_REGULAR_EXPRESSION "app/CMakeLists\.txt: target_link_libraries.*sleeve/file\.hpp: class File"
             FAIL_REGULAR_EXPRESSION "mini_git\.hpp"
             LABELS "ci_core_flow;legacy_removal")

# G10.5: DRT resolution and its GPU parameter layouts live in the runtime. No file under
# edit/runtime or include/edit/runtime may include a legacy operator or stage parameter header.
add_test(NAME RuntimeSourcesDoNotIncludeLegacyOperatorHeaders
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_ROOT_DIR}/src
          -DCHECK=RuntimeSourcesDoNotIncludeLegacyOperatorHeaders
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(RuntimeSourcesDoNotIncludeLegacyOperatorHeaders
  PROPERTIES LABELS "ci_core_flow;legacy_removal")

# The same check must reject a runtime file that includes param.cuh, and must not reject a
# runtime file that includes the relocated CUDA DRT header or a legacy file outside the runtime.
set(_runtime_legacy_header_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/runtime_legacy_header_fixture")
file(WRITE "${_runtime_legacy_header_fixture_root}/edit/runtime/cuda/drt_pass.cu"
  "#include \"edit/operators/GPU_kernels/param.cuh\"\n")
file(WRITE "${_runtime_legacy_header_fixture_root}/include/edit/runtime/cuda/drt_state.hpp"
  "#include \"edit/runtime/cuda/cuda_drt_gpu_params.cuh\"\n")
file(WRITE "${_runtime_legacy_header_fixture_root}/edit/pipeline/stage.cpp"
  "#include \"edit/operators/op_base.hpp\"\n")
add_test(NAME RuntimeLegacyHeaderCheckRejectsRuntimeInclude
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_runtime_legacy_header_fixture_root}
          -DCHECK=RuntimeSourcesDoNotIncludeLegacyOperatorHeaders
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(RuntimeLegacyHeaderCheckRejectsRuntimeInclude
  PROPERTIES PASS_REGULAR_EXPRESSION "edit/runtime/cuda/drt_pass\.cu: #include"
             FAIL_REGULAR_EXPRESSION "drt_state\.hpp|stage\.cpp"
             LABELS "ci_core_flow;legacy_removal")

# G10.6: no DAG runtime, graph, or renderer file includes a legacy pipeline or operator path.
add_test(NAME DagSourcesDoNotReferenceLegacyDirectories
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_ROOT_DIR}/src
          -DCHECK=DagSourcesDoNotReferenceLegacyDirectories
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(DagSourcesDoNotReferenceLegacyDirectories
  PROPERTIES LABELS "ci_core_flow;legacy_removal")

# The same check must reject a relative shader include into GPU_kernels, a legacy operator header,
# and a stage header in DAG files, and must accept the executor header, Model and data headers,
# and legacy includes outside the DAG scope.
set(_dag_legacy_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/dag_legacy_directory_fixture")
file(WRITE "${_dag_legacy_fixture_root}/edit/runtime/metal/shader/neighbor.metal"
  "#include \"../../../operators/GPU_kernels/metal_shader/prng.metal\"\n")
file(WRITE "${_dag_legacy_fixture_root}/edit/graph/node.cpp"
  "#include \"edit/operators/geometry/lens_calib_op.hpp\"\n")
file(WRITE "${_dag_legacy_fixture_root}/include/renderer/task.hpp"
  "#include \"edit/pipeline/pipeline.hpp\"\n#include \"edit/pipeline/pipeline_executor.hpp\"\n")
file(WRITE "${_dag_legacy_fixture_root}/edit/runtime/allowed.cpp"
  "#include \"edit/operators/models/sharpen_model.hpp\"\n#include \"edit/operators/basic/planckian_locus_table.hpp\"\n#include \"edit/runtime/local_tone_mapping.hpp\"\n")
file(WRITE "${_dag_legacy_fixture_root}/edit/pipeline/stage.cpp"
  "#include \"edit/operators/GPU_kernels/param.cuh\"\n")
add_test(NAME DagLegacyDirectoryCheckRejectsLegacyIncludes
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_dag_legacy_fixture_root}
          -DCHECK=DagSourcesDoNotReferenceLegacyDirectories
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(DagLegacyDirectoryCheckRejectsLegacyIncludes
  PROPERTIES PASS_REGULAR_EXPRESSION "edit/graph/node\.cpp: #include.*edit/runtime/metal/shader/neighbor\.metal: #include.*include/renderer/task\.hpp: #include \"edit/pipeline/pipeline\.hpp\""
             FAIL_REGULAR_EXPRESSION "allowed\.cpp|stage\.cpp|pipeline_executor\.hpp"
             LABELS "ci_core_flow;legacy_removal")

# G10.6: a DAG frame presenter test links the scope analyzer from EditScope, without the legacy
# pipeline libraries. The check reads the generated Ninja link statement.
if(CMAKE_GENERATOR MATCHES "Ninja" AND TARGET GpuDagCudaDrtProductTest)
  add_test(NAME FramePresenterLinksScopeWithoutEditPipeline
    COMMAND ${CMAKE_COMMAND} -DNINJA_FILE=${CMAKE_BINARY_DIR}/build.ninja
            -DTARGET_NAME=GpuDagCudaDrtProductTest
            -DCHECK=FramePresenterLinksScopeWithoutEditPipeline
            -P ${ALCEDO_TEST_ROOT}/ci/target_link_checks.cmake)
  set_tests_properties(FramePresenterLinksScopeWithoutEditPipeline
    PROPERTIES LABELS "ci_core_flow;legacy_removal")
endif()

# The link check must reject a link statement that names EditPipeline and lacks EditScope.
set(_link_fixture_ninja "${CMAKE_CURRENT_BINARY_DIR}/frame_presenter_link_fixture/build.ninja")
file(WRITE "${_link_fixture_ninja}"
  "build tests/edit/PresenterTest.exe: CXX_EXECUTABLE_LINKER__PresenterTest_Debug a.obj\n  LINK_LIBRARIES = src/edit/EditRuntimeCuda.lib src/decoders/EditPipeline.lib\n")
add_test(NAME FramePresenterLinkCheckRejectsLegacyPipeline
  COMMAND ${CMAKE_COMMAND} -DNINJA_FILE=${_link_fixture_ninja} -DTARGET_NAME=PresenterTest
          -DCHECK=FramePresenterLinksScopeWithoutEditPipeline
          -P ${ALCEDO_TEST_ROOT}/ci/target_link_checks.cmake)
set_tests_properties(FramePresenterLinkCheckRejectsLegacyPipeline
  PROPERTIES PASS_REGULAR_EXPRESSION "PresenterTest does not link EditScope; links legacy library EditPipeline"
             LABELS "ci_core_flow;legacy_removal")

# G10.9: the stage, the legacy operators, and the legacy importer are archived outside the build.
# No CMake file may name the archive, and the compile database may not list a file in it.
add_test(NAME DeprecatedLegacyArchiveIsOutsideCompileGraph
  COMMAND ${CMAKE_COMMAND} -DREPO_ROOT=${CMAKE_SOURCE_DIR}
          -DCOMPILE_COMMANDS=$<$<BOOL:${CMAKE_EXPORT_COMPILE_COMMANDS}>:${CMAKE_BINARY_DIR}/compile_commands.json>
          -DCHECK=DeprecatedLegacyArchiveIsOutsideCompileGraph
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(DeprecatedLegacyArchiveIsOutsideCompileGraph
  PROPERTIES LABELS "ci_core_flow;legacy_removal")

# The same check must reject a subdirectory into the archive, a CMake file inside the archive, and
# an archive path in the compile database, and must not report a third_party file. The archive
# directory name is assembled here so this file never spells the path itself.
string(CONCAT _archive_dir "depre" "cated")
set(_archive_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/legacy_archive_reference_fixture")
file(WRITE "${_archive_fixture_root}/CMakeLists.txt"
  "add_subdirectory(\${ALCEDO_ROOT_DIR}/${_archive_dir}/legacy_pipeline/src)\n")
file(WRITE "${_archive_fixture_root}/alcedo_studio/${_archive_dir}/legacy_pipeline/CMakeLists.txt"
  "project(archive)\n")
file(WRITE "${_archive_fixture_root}/alcedo_studio/src/third_party/lib/CMakeLists.txt"
  "# vendored: ../../${_archive_dir}/legacy_pipeline\n")
file(WRITE "${_archive_fixture_root}/scripts/clean.cmake" "set(X 1)\n")
file(WRITE "${_archive_fixture_root}/compile_commands.json"
  "[{\"file\": \"D:/repo/alcedo_studio/${_archive_dir}/legacy_pipeline/src/edit/stage.cpp\"}]\n")
add_test(NAME ArchiveReferenceCheckRejectsBuildAndCompileDatabaseUse
  COMMAND ${CMAKE_COMMAND} -DREPO_ROOT=${_archive_fixture_root}
          -DCOMPILE_COMMANDS=${_archive_fixture_root}/compile_commands.json
          -DCHECK=DeprecatedLegacyArchiveIsOutsideCompileGraph
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(ArchiveReferenceCheckRejectsBuildAndCompileDatabaseUse
  PROPERTIES PASS_REGULAR_EXPRESSION "CMakeLists\.txt: add_subdirectory.*legacy_pipeline/CMakeLists\.txt: CMake file inside the archive.*compile database: .*stage\.cpp"
             FAIL_REGULAR_EXPRESSION "third_party|clean\.cmake"
             LABELS "ci_core_flow;legacy_removal")

# G10.9 type and entry-point checks over src and tests.
foreach(_g10_9_check IN ITEMS
    NoPipelineStageTypeRemainsInFirstPartySource
    NoOperatorParamsAggregateRemainsInFirstPartySource
    NoLegacyOperatorTypeEnumRemains)
  add_test(NAME ${_g10_9_check}
    COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_ROOT_DIR}/src
            -DALCEDO_TEST_SOURCE_ROOT=${ALCEDO_TEST_ROOT}
            -DCHECK=${_g10_9_check}
            -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
  set_tests_properties(${_g10_9_check} PROPERTIES LABELS "ci_core_flow;legacy_removal")
endforeach()
foreach(_g10_9_check IN ITEMS
    AllBuiltInOperatorModelsHaveNoImageApplyEntryPoint
    NoLegacyParameterImporterOrStageAdapterRemainsInProductPath)
  add_test(NAME ${_g10_9_check}
    COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_ROOT_DIR}/src
            -DCHECK=${_g10_9_check}
            -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
  set_tests_properties(${_g10_9_check} PROPERTIES LABELS "ci_core_flow;legacy_removal")
endforeach()

# Negative fixtures for the G10.9 checks. Each check must report the violating file and must not
# report a file whose identifier only contains the legacy name.
set(_g10_9_fixture_src "${CMAKE_CURRENT_BINARY_DIR}/g10_9_violation_fixture/src")
set(_g10_9_fixture_tests "${CMAKE_CURRENT_BINARY_DIR}/g10_9_violation_fixture/tests")
file(WRITE "${_g10_9_fixture_src}/app/stage_user.cpp"
  "void Configure(PipelineStage& stage);\n")
file(WRITE "${_g10_9_fixture_src}/app/params_user.cpp"
  "void Resolve(OperatorParams& params);\nenum class OperatorType : int { EXPOSURE };\n")
file(WRITE "${_g10_9_fixture_src}/ui/panel_model.cpp"
  "void loadFromOperatorParams(const Map& m);\nauto t = OperatorTypeId::Exposure();\n")
file(WRITE "${_g10_9_fixture_src}/edit/operators/models/exposure_model.hpp"
  "class ExposureModel { void ApplyGPU(Buffer& b); };\n")
file(WRITE "${_g10_9_fixture_src}/edit/operators/models/tint_model.hpp"
  "class TintModel { void ApplyPatch(const Json& j); };\n")
file(WRITE "${_g10_9_fixture_src}/app/import.cpp"
  "auto doc = LegacyPipelineImporter::Import(json);\n")
file(WRITE "${_g10_9_fixture_tests}/raw/gpu_params_test.cpp"
  "GPUOperatorParams gpu{};\nauto k = OperatorType::ODT;\n")

add_test(NAME PipelineStageCheckRejectsStageType
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_g10_9_fixture_src}
          -DALCEDO_TEST_SOURCE_ROOT=${_g10_9_fixture_tests}
          -DCHECK=NoPipelineStageTypeRemainsInFirstPartySource
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(PipelineStageCheckRejectsStageType
  PROPERTIES PASS_REGULAR_EXPRESSION "app/stage_user\.cpp: void Configure"
             FAIL_REGULAR_EXPRESSION "params_user|panel_model|gpu_params_test"
             LABELS "ci_core_flow;legacy_removal")

add_test(NAME OperatorParamsCheckRejectsAggregateInSourceAndTests
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_g10_9_fixture_src}
          -DALCEDO_TEST_SOURCE_ROOT=${_g10_9_fixture_tests}
          -DCHECK=NoOperatorParamsAggregateRemainsInFirstPartySource
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(OperatorParamsCheckRejectsAggregateInSourceAndTests
  PROPERTIES PASS_REGULAR_EXPRESSION "app/params_user\.cpp: void Resolve.*tests:raw/gpu_params_test\.cpp: GPUOperatorParams"
             FAIL_REGULAR_EXPRESSION "panel_model"
             LABELS "ci_core_flow;legacy_removal")

add_test(NAME OperatorTypeCheckRejectsEnumAndValues
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_g10_9_fixture_src}
          -DALCEDO_TEST_SOURCE_ROOT=${_g10_9_fixture_tests}
          -DCHECK=NoLegacyOperatorTypeEnumRemains
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(OperatorTypeCheckRejectsEnumAndValues
  PROPERTIES PASS_REGULAR_EXPRESSION "app/params_user\.cpp: enum class OperatorType.*tests:raw/gpu_params_test\.cpp: auto k"
             FAIL_REGULAR_EXPRESSION "panel_model"
             LABELS "ci_core_flow;legacy_removal")

add_test(NAME ModelApplyCheckRejectsImageEntryPoint
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_g10_9_fixture_src}
          -DCHECK=AllBuiltInOperatorModelsHaveNoImageApplyEntryPoint
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(ModelApplyCheckRejectsImageEntryPoint
  PROPERTIES PASS_REGULAR_EXPRESSION "edit/operators/models/exposure_model\.hpp: class ExposureModel"
             FAIL_REGULAR_EXPRESSION "tint_model"
             LABELS "ci_core_flow;legacy_removal")

add_test(NAME LegacyImporterCheckRejectsImporterUse
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_g10_9_fixture_src}
          -DCHECK=NoLegacyParameterImporterOrStageAdapterRemainsInProductPath
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(LegacyImporterCheckRejectsImporterUse
  PROPERTIES PASS_REGULAR_EXPRESSION "app/import\.cpp: auto doc"
             FAIL_REGULAR_EXPRESSION "stage_user|params_user"
             LABELS "ci_core_flow;legacy_removal")
