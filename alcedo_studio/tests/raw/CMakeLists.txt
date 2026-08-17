# RAW decode/processor test target ownership.

include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

# --- Unconditional raw targets ---

add_executable(SingleRawLoad ${ALCEDO_TEST_ROOT}/raw/single_raw_decode_test.cpp)
target_include_directories(SingleRawLoad PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SingleRawLoad PRIVATE GTest::gtest_main libraw::raw ${ALCEDO_TEST_OPENCV_MIN_DEPS})
target_compile_definitions(SingleRawLoad PRIVATE LIBRAW_NODLL)
alcedo_register_test_target(SingleRawLoad raw)

add_executable(SingleThumbnailLoad ${ALCEDO_TEST_ROOT}/raw/single_raw_thumbnail_test.cpp)
target_include_directories(SingleThumbnailLoad PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SingleThumbnailLoad PRIVATE GTest::gtest_main libraw::raw ${ALCEDO_TEST_OPENCV_MIN_DEPS})
target_compile_definitions(SingleThumbnailLoad PRIVATE LIBRAW_NODLL)
alcedo_register_test_target(SingleThumbnailLoad raw)

add_executable(ColorTempCudaSanityTest ${ALCEDO_TEST_ROOT}/raw/color_temp_cuda_sanity_test.cpp)
target_include_directories(ColorTempCudaSanityTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ColorTempCudaSanityTest PRIVATE GTest::gtest_main Operators RawProcessor)
alcedo_register_test_target(ColorTempCudaSanityTest raw)

add_executable(RawProcessorCropTest ${ALCEDO_TEST_ROOT}/raw/raw_processor_crop_test.cpp)
target_include_directories(RawProcessorCropTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(RawProcessorCropTest PRIVATE GTest::gtest_main RawProcessor ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(RawProcessorCropTest TEST_PREFIX "RawProcessorCropTest.")
alcedo_register_test_target(RawProcessorCropTest raw)

if(ALCEDO_METAL_ENABLED)
  add_executable(MetalRawOpsTest ${ALCEDO_TEST_ROOT}/raw/metal_raw_ops_test.cpp)
  target_include_directories(MetalRawOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalRawOpsTest PRIVATE GTest::gtest_main RawProcessorOp
                                                MetalImage MetalContext ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(MetalRawOpsTest TEST_PREFIX "MetalRawOpsTest.")
  alcedo_register_test_target(MetalRawOpsTest raw gpu)
endif()

# --- CUDA-guarded raw targets ---

if(ALCEDO_CUDA_ENABLED AND ALCEDO_ENABLE_CUDA_IMAGE_GEOMETRY_OPS_TEST)
  add_executable(CudaImageGeometryOpsTest ${ALCEDO_TEST_ROOT}/raw/cuda_image_geometry_ops_test.cpp)
  target_include_directories(CudaImageGeometryOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(CudaImageGeometryOpsTest PRIVATE GTest::gtest_main RawProcessorOp Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(CudaImageGeometryOpsTest TEST_PREFIX "CudaImageGeometryOpsTest.")
  alcedo_register_test_target(CudaImageGeometryOpsTest raw gpu)
endif()

if(ALCEDO_CUDA_ENABLED)
  add_executable(CudaRawOpsTest ${ALCEDO_TEST_ROOT}/raw/cuda_raw_ops_test.cpp)
  target_include_directories(CudaRawOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(CudaRawOpsTest PRIVATE GTest::gtest_main RawProcessor RawProcessorOp
                                               ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(CudaRawOpsTest TEST_PREFIX "CudaRawOpsTest.")
  alcedo_register_test_target(CudaRawOpsTest raw gpu)
endif()

# --- OpenCL + CUDA guarded raw targets ---

if(ALCEDO_CUDA_ENABLED AND ALCEDO_OPENCL_ENABLED)
  add_executable(OpenClCudaToLinearRefCompareTest ${ALCEDO_TEST_ROOT}/raw/opencl_cuda_to_linear_ref_compare_test.cpp)
  target_include_directories(OpenClCudaToLinearRefCompareTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaToLinearRefCompareTest PRIVATE GTest::gtest_main RawProcessorOp ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClCudaToLinearRefCompareTest TEST_PREFIX "OpenClCudaCompare.")
  alcedo_register_test_target(OpenClCudaToLinearRefCompareTest raw gpu)

  add_executable(OpenClCudaRcdCompareTest ${ALCEDO_TEST_ROOT}/raw/opencl_cuda_rcd_compare_test.cpp)
  target_include_directories(OpenClCudaRcdCompareTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaRcdCompareTest PRIVATE GTest::gtest_main RawProcessorOp ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClCudaRcdCompareTest TEST_PREFIX "OpenClCudaCompare.")
  alcedo_register_test_target(OpenClCudaRcdCompareTest raw gpu)

  add_executable(OpenClCudaPipelineCompareTest ${ALCEDO_TEST_ROOT}/raw/opencl_cuda_pipeline_compare_test.cpp)
  target_include_directories(OpenClCudaPipelineCompareTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaPipelineCompareTest PRIVATE GTest::gtest_main Operators RawProcessor OpenClGeometryUtils OpenClProgramLibrary ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClCudaPipelineCompareTest TEST_PREFIX "OpenClCudaCompare.")
  alcedo_register_test_target(OpenClCudaPipelineCompareTest raw gpu)

  add_executable(OpenClCudaFullPipelineBenchmark ${ALCEDO_TEST_ROOT}/raw/opencl_cuda_full_pipeline_benchmark.cpp)
  target_include_directories(OpenClCudaFullPipelineBenchmark PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaFullPipelineBenchmark PRIVATE GTest::gtest_main Operators RawProcessor OpenClProgramLibrary EditPipeline PipelineScheduler ThreadPool ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClCudaFullPipelineBenchmark TEST_PREFIX "OpenClCudaBenchmark.")
  alcedo_register_test_target(OpenClCudaFullPipelineBenchmark raw gpu)
endif()
