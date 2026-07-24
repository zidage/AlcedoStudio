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

  add_executable(ODTCudaSmokeTest ${ALCEDO_TEST_ROOT}/edit/operators/cst/odt_cuda_smoke_test.cu)
  target_include_directories(ODTCudaSmokeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(ODTCudaSmokeTest PRIVATE GTest::gtest_main Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS} CUDA::cudart)
  set_target_properties(ODTCudaSmokeTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  gtest_discover_tests(ODTCudaSmokeTest TEST_PREFIX "ODTCudaSmokeTest.")
  alcedo_register_test_target(ODTCudaSmokeTest gpu)

  add_executable(FilmGrainCudaStageTest ${ALCEDO_TEST_ROOT}/edit/operators/cst/film_grain_cuda_stage_test.cu)
  target_include_directories(FilmGrainCudaStageTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(FilmGrainCudaStageTest PRIVATE GTest::gtest_main Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS} CUDA::cudart)
  set_target_properties(FilmGrainCudaStageTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  gtest_discover_tests(FilmGrainCudaStageTest TEST_PREFIX "FilmGrainCudaStageTest.")
  alcedo_register_test_target(FilmGrainCudaStageTest gpu)

  add_executable(HalationCudaStageTest ${ALCEDO_TEST_ROOT}/edit/operators/cst/halation_cuda_stage_test.cu)
  target_include_directories(HalationCudaStageTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(HalationCudaStageTest PRIVATE GTest::gtest_main Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS} CUDA::cudart)
  set_target_properties(HalationCudaStageTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  gtest_discover_tests(HalationCudaStageTest TEST_PREFIX "HalationCudaStageTest.")
  alcedo_register_test_target(HalationCudaStageTest gpu)

  if(ALCEDO_ENABLE_CUDA_PREVIEW_VRAM_RECLAMATION_TEST)
    add_executable(CudaPreviewVramReclamationTest ${ALCEDO_TEST_ROOT}/edit/pipeline/cuda_preview_vram_reclamation_test.cu)
    target_include_directories(CudaPreviewVramReclamationTest PUBLIC ${ALCEDO_INCLUDE_DIR})
    target_link_libraries(CudaPreviewVramReclamationTest PRIVATE GTest::gtest_main EditPipeline PipelineScheduler Operators ThreadPool ${ALCEDO_TEST_OPENCV_MIN_DEPS} CUDA::cudart)
    set_target_properties(CudaPreviewVramReclamationTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    gtest_discover_tests(CudaPreviewVramReclamationTest TEST_PREFIX "CudaPreviewVramReclamationTest.")
    alcedo_register_test_target(CudaPreviewVramReclamationTest gpu)
  endif()
endif()
