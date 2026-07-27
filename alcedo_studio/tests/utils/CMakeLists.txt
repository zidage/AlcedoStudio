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

if(ALCEDO_CUDA_ENABLED)
  add_executable(SharedToneCurveTest ${ALCEDO_TEST_ROOT}/edit/operators/basic/shared_tone_curve_test.cpp)
  target_include_directories(SharedToneCurveTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(SharedToneCurveTest PRIVATE GTest::gtest_main Operators CUDA::cudart)
  gtest_discover_tests(SharedToneCurveTest TEST_PREFIX "SharedToneCurveTest.")
  alcedo_register_test_target(SharedToneCurveTest edit)
endif()
