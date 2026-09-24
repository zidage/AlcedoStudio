# Utility test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(CudaDriverRequirementsTest
    ${ALCEDO_TEST_ROOT}/utils/cuda_driver_requirements_test.cpp
    ${ALCEDO_SRC_DIR}/utils/cuda/cuda_driver_requirements.cpp
)
target_include_directories(CudaDriverRequirementsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(CudaDriverRequirementsTest PRIVATE GTest::gtest_main)
gtest_discover_tests(CudaDriverRequirementsTest TEST_PREFIX "CudaDriverRequirementsTest.")
alcedo_register_test_target(CudaDriverRequirementsTest core)

# Phase 7A R0/R1: presentation logging policy (disabled-by-default present
# category, buffered info/debug writes, immediate warning/critical/fatal flush).
add_executable(EditorAppLoggingTest
    ${ALCEDO_TEST_ROOT}/utils/editor_app_logging_test.cpp
    ${ALCEDO_SRC_DIR}/utils/diagnostics/app_logging.cpp)
target_include_directories(EditorAppLoggingTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorAppLoggingTest PRIVATE GTest::gtest_main Qt6::Core)
gtest_discover_tests(EditorAppLoggingTest TEST_PREFIX "EditorAppLoggingTest."
  PROPERTIES LABELS "ci_core_flow;editor_logging")
alcedo_register_test_target(EditorAppLoggingTest core)

add_executable(PreviewPerformanceTest
    ${ALCEDO_TEST_ROOT}/utils/preview_performance_test.cpp)
target_include_directories(PreviewPerformanceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(PreviewPerformanceTest
    PRIVATE PreviewPerformance EditorPendingInput GTest::gtest_main)
gtest_discover_tests(PreviewPerformanceTest TEST_PREFIX "PreviewPerformanceTest."
  PROPERTIES LABELS "ci_core_flow;preview_performance")
alcedo_register_test_target(PreviewPerformanceTest core)
