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

if(ALCEDO_OPENCL_ENABLED)
  add_executable(OpenClRuntimeTest ${ALCEDO_TEST_ROOT}/opencl/opencl_runtime_test.cpp)
  target_include_directories(OpenClRuntimeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClRuntimeTest PRIVATE GTest::gtest_main OpenClProgramLibrary)
  target_compile_definitions(OpenClRuntimeTest PRIVATE HAVE_OPENCL)
  gtest_discover_tests(OpenClRuntimeTest TEST_PREFIX "OpenClRuntimeTest.")
  alcedo_register_test_target(OpenClRuntimeTest gpu)

  add_executable(OpenClDemosaicNetTest ${ALCEDO_TEST_ROOT}/opencl/opencl_demosaicnet_test.cpp)
  target_include_directories(OpenClDemosaicNetTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClDemosaicNetTest PRIVATE
    GTest::gtest_main
    OpenClDemosaicNet
    OpenClProgramLibrary
    NnSafetensors
  )
  target_compile_definitions(OpenClDemosaicNetTest PRIVATE
    ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_SOURCE_DIR}/alcedo_studio/src/config/models"
  )
  gtest_discover_tests(OpenClDemosaicNetTest TEST_PREFIX "OpenClDemosaicNetTest.")
  alcedo_register_test_target(OpenClDemosaicNetTest gpu)
endif()
