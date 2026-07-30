# CI workflow test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(CiRawWorkflowTest ${ALCEDO_TEST_ROOT}/ci/ci_raw_workflow_test.cpp)
target_include_directories(CiRawWorkflowTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(CiRawWorkflowTest
  PRIVATE
    GTest::gtest_main
    ProjectService
    ImportService
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
