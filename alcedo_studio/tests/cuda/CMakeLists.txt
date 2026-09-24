# CUDA test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

if(ALCEDO_CUDA_ENABLED)
  add_executable(CudaPrngTest ${ALCEDO_TEST_ROOT}/cuda/prng_test.cu)
  target_include_directories(CudaPrngTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(CudaPrngTest PRIVATE GTest::gtest_main CudaUtils CUDA::cudart)
  set_target_properties(CudaPrngTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  gtest_discover_tests(CudaPrngTest TEST_PREFIX "CudaPrngTest.")
  alcedo_register_test_target(CudaPrngTest gpu)

  # Forward-only CNN / ML operator suite (ReLU, Mul, structural ops, Conv2d, ...)
  # plus hard-coded DemosaicNet modules + lazy cache (Phase 5).
  add_executable(MlOpsTest
    ${ALCEDO_TEST_ROOT}/ml_ops/relu_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/device_buffer_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/workspace_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/mul_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/concat_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/slice_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/crop_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/layout_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/structural_compose_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/conv2d_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/conv_transpose2d_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/safetensors_test.cpp
    ${ALCEDO_TEST_ROOT}/ml_ops/demosaicnet_module_test.cu
    ${ALCEDO_TEST_ROOT}/ml_ops/cuda_tile_jobs_test.cpp
  )
  target_include_directories(MlOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MlOpsTest PRIVATE GTest::gtest_main CudaUtils DemosaicNet
                        CudaDemosaicNetEntry CUDA::cudart JSON ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  set_target_properties(MlOpsTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  target_compile_definitions(MlOpsTest PRIVATE
      ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_SOURCE_DIR}/alcedo_studio/src/config/models"
  )
  gtest_discover_tests(MlOpsTest TEST_PREFIX "MlOpsTest.")
  alcedo_register_test_target(MlOpsTest gpu)

  if(WIN32)
    add_executable(D3DCudaInteropUtilsTest ${ALCEDO_TEST_ROOT}/ui/d3d_cuda_interop_utils_test.cpp)
    target_include_directories(D3DCudaInteropUtilsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
    target_link_libraries(D3DCudaInteropUtilsTest PRIVATE GTest::gtest_main CUDA::cudart)
    gtest_discover_tests(D3DCudaInteropUtilsTest TEST_PREFIX "D3DCudaInteropUtilsTest.")
    alcedo_register_test_target(D3DCudaInteropUtilsTest gpu)
  endif()
endif()
