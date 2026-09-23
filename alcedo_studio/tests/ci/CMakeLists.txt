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
add_test(NAME NoProductCodeReadsStageTableOutsideMirror
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${ALCEDO_ROOT_DIR}/src
          -DCHECK=NoProductCodeReadsStageTableOutsideMirror
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(NoProductCodeReadsStageTableOutsideMirror
  PROPERTIES LABELS "ci_core_flow;legacy_removal")

# The same check must reject a product file outside the mirror hosts. The fixture is generated in
# the build tree so no test source outside the product tree is formatted or tidied.
set(_stage_read_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/stage_read_violation_fixture")
file(WRITE "${_stage_read_fixture_root}/renderer/reads_stage.cpp"
  "void Read(Executor& e) { (void)e.GetStage(PipelineStageName::Image_Loading); }\n")
add_test(NAME StageTableReadCheckRejectsProductFileOutsideMirror
  COMMAND ${CMAKE_COMMAND} -DALCEDO_SOURCE_ROOT=${_stage_read_fixture_root}
          -DCHECK=NoProductCodeReadsStageTableOutsideMirror
          -P ${ALCEDO_TEST_ROOT}/ci/legacy_removal_source_checks.cmake)
set_tests_properties(StageTableReadCheckRejectsProductFileOutsideMirror
  PROPERTIES PASS_REGULAR_EXPRESSION "renderer/reads_stage\\.cpp: void Read"
             LABELS "ci_core_flow;legacy_removal")

# G10.3: stage-JSON export and import stay inside the executor and the legacy owners that G10.4
# and G10.7 delete. No rollback path may copy stage JSON again.
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
