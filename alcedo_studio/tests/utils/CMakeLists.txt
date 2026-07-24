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

if(ALCEDO_CUDA_ENABLED)
  add_executable(SharedToneCurveTest ${ALCEDO_TEST_ROOT}/edit/operators/basic/shared_tone_curve_test.cpp)
  target_include_directories(SharedToneCurveTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(SharedToneCurveTest PRIVATE GTest::gtest_main Operators CUDA::cudart)
  gtest_discover_tests(SharedToneCurveTest TEST_PREFIX "SharedToneCurveTest.")
  alcedo_register_test_target(SharedToneCurveTest edit)
endif()
