# ML operator / NN test target ownership.

include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

# Phase 1 OpenCL NN plan: backend-neutral safetensors parser, student tile planner,
# CFA alignment / Reflect101 / gamma constants. Must not link CUDA or OpenCL.
add_executable(NnSharedUtilsTest
  ${ALCEDO_TEST_ROOT}/ml_ops/nn_safetensors_test.cpp
  ${ALCEDO_TEST_ROOT}/ml_ops/neural_tile_jobs_test.cpp
  ${ALCEDO_TEST_ROOT}/ml_ops/demosaicnet_preprocess_common_test.cpp
)
target_include_directories(NnSharedUtilsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(NnSharedUtilsTest PRIVATE GTest::gtest_main NnSafetensors JSON
                      libraw::raw_r ${ALCEDO_TEST_OPENCV_MIN_DEPS})
target_compile_definitions(NnSharedUtilsTest PRIVATE
    ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_SOURCE_DIR}/alcedo_studio/src/config/models"
)
gtest_discover_tests(NnSharedUtilsTest TEST_PREFIX "NnSharedUtilsTest.")
alcedo_register_test_target(NnSharedUtilsTest core)
