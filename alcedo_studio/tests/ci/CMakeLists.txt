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
