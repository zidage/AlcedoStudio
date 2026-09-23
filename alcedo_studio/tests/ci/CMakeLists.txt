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
  "#include \"edit/pipeline/pipeline.hpp\"\n#include \"edit/pipeline/pipeline_cpu.hpp\"\n")
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
             FAIL_REGULAR_EXPRESSION "allowed\.cpp|stage\.cpp|pipeline_cpu\.hpp"
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
