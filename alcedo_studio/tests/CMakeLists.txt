# Not used by now, but kept for future test cases
add_library(MemLeakDect INTERFACE leak_detector/memory_leak_detector.hpp)
target_link_libraries(MemLeakDect INTERFACE GTest::gtest_main)

set(ALCEDO_TEST_OPENCV_MIN_DEPS
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
)

set(ALCEDO_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(ALCEDO_SRC_DIR "${ALCEDO_ROOT_DIR}/src")
set(ALCEDO_INCLUDE_DIR "${ALCEDO_SRC_DIR}/include")
set(ALCEDO_TEST_DIR "${ALCEDO_ROOT_DIR}/tests")

include(GoogleTest)
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

option(ALCEDO_BUILD_TESTS_BY_DEFAULT
       "Build enabled test targets as part of the default all target." OFF)
option(ALCEDO_BUILD_CORE_TESTS "Enable small core/unit test category targets." ON)
option(ALCEDO_BUILD_IMAGE_TESTS "Enable image and ImageBuffer test category targets." ON)
option(ALCEDO_BUILD_IO_TESTS "Enable IO/export/metadata test category targets." ON)
option(ALCEDO_BUILD_RAW_TESTS "Enable RAW decode/processor test category targets." ON)
option(ALCEDO_BUILD_GPU_TESTS "Enable CUDA/Metal GPU test category targets." ON)
option(ALCEDO_BUILD_EDIT_TESTS "Enable edit operator and pipeline test category targets." ON)
option(ALCEDO_BUILD_UI_TESTS "Enable Qt/UI test category targets." ON)
option(ALCEDO_BUILD_APP_TESTS "Enable application service test category targets." ON)
option(ALCEDO_BUILD_STORAGE_TESTS "Enable storage/Sleeve test category targets." ON)
option(ALCEDO_BUILD_DEMO_TARGETS "Enable GUI/demo target category targets." OFF)
option(ALCEDO_BUILD_CI_TESTS "Enable zero-extra-cost GitHub CI test targets." OFF)
option(ALCEDO_ENABLE_CUDA_IMAGE_GEOMETRY_OPS_TEST
       "Enable CudaImageGeometryOpsTest discovery and execution." ON)
option(ALCEDO_ENABLE_CUDA_PREVIEW_VRAM_RECLAMATION_TEST
       "Enable CudaPreviewVramReclamationTest discovery and execution." ON)

add_custom_target(alcedo_tests_all)
add_custom_target(alcedo_tests_core)
add_custom_target(alcedo_tests_image)
add_custom_target(alcedo_tests_io)
add_custom_target(alcedo_tests_raw)
add_custom_target(alcedo_tests_gpu)
add_custom_target(alcedo_tests_edit)
add_custom_target(alcedo_tests_ui)
add_custom_target(alcedo_tests_app)
add_custom_target(alcedo_tests_storage)
add_custom_target(alcedo_tests_demos)
add_custom_target(alcedo_tests_ci_core)
add_custom_target(alcedo_tests_ci_raw)
add_custom_target(alcedo_tests_ci_metal)

function(alcedo_assign_test_category category option_name category_target)
  foreach(test_target IN LISTS ARGN)
    if(TARGET ${test_target})
      if(NOT ${option_name} OR NOT ALCEDO_BUILD_TESTS_BY_DEFAULT)
        set_target_properties(${test_target} PROPERTIES
          EXCLUDE_FROM_ALL TRUE
          EXCLUDE_FROM_DEFAULT_BUILD TRUE
        )
      endif()
      if(${option_name})
        if(ALCEDO_BUILD_TESTS_BY_DEFAULT)
          set_target_properties(${test_target} PROPERTIES
            EXCLUDE_FROM_ALL FALSE
            EXCLUDE_FROM_DEFAULT_BUILD FALSE
          )
        endif()
        add_dependencies(${category_target} ${test_target})
        add_dependencies(alcedo_tests_all ${test_target})
      endif()
    endif()
  endforeach()
endfunction()

add_executable(SampleTest sample_test.cpp)
target_link_libraries(SampleTest PRIVATE GTest::gtest_main)

add_executable(EditViewerLogicTest
  ui/edit_viewer_logic_test.cpp
  ${ALCEDO_SRC_DIR}/ui/edit_viewer/viewport_mapper.cpp
  ${ALCEDO_SRC_DIR}/ui/edit_viewer/crop_geometry.cpp
  ${ALCEDO_SRC_DIR}/ui/edit_viewer/view_transform_controller.cpp
  ${ALCEDO_SRC_DIR}/ui/edit_viewer/crop_interaction_controller.cpp
)
target_include_directories(EditViewerLogicTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditViewerLogicTest PRIVATE GTest::gtest_main Qt6::Widgets opencv_core)
gtest_discover_tests(EditViewerLogicTest TEST_PREFIX "EditViewerLogicTest.")

add_executable(ToneCurveWidgetTest
  ui/tone_curve_widget_test.cpp
  ui/widget_test_main.cpp
  ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
  ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
  ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/modules/curve.cpp
  ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/widgets/tone_curve_widget.cpp
)
target_include_directories(ToneCurveWidgetTest
  PUBLIC ${ALCEDO_INCLUDE_DIR}
  PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(ToneCurveWidgetTest
  PRIVATE
    GTest::gtest
    JSON
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
)
gtest_discover_tests(ToneCurveWidgetTest TEST_PREFIX "ToneCurveWidgetTest.")

add_executable(SingleRawLoad raw/single_raw_decode_test.cpp)
target_include_directories(SingleRawLoad PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SingleRawLoad PRIVATE GTest::gtest_main libraw::raw ${ALCEDO_TEST_OPENCV_MIN_DEPS})
target_compile_definitions(SingleRawLoad PRIVATE LIBRAW_NODLL)

add_executable(SingleThumbnailLoad raw/single_raw_thumbnail_test.cpp)
target_include_directories(SingleThumbnailLoad PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SingleThumbnailLoad PRIVATE GTest::gtest_main libraw::raw ${ALCEDO_TEST_OPENCV_MIN_DEPS})
target_compile_definitions(SingleThumbnailLoad PRIVATE LIBRAW_NODLL)

add_executable(ColorTempCudaSanityTest raw/color_temp_cuda_sanity_test.cpp)
target_include_directories(ColorTempCudaSanityTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ColorTempCudaSanityTest PRIVATE GTest::gtest_main Operators RawProcessor)

if(ALCEDO_CUDA_ENABLED AND ALCEDO_ENABLE_CUDA_IMAGE_GEOMETRY_OPS_TEST)
  add_executable(CudaImageGeometryOpsTest raw/cuda_image_geometry_ops_test.cpp)
  target_include_directories(CudaImageGeometryOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(CudaImageGeometryOpsTest PRIVATE GTest::gtest_main RawProcessorOp Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(CudaImageGeometryOpsTest TEST_PREFIX "CudaImageGeometryOpsTest.")
endif()

if(ALCEDO_CUDA_ENABLED)
  add_executable(CudaPrngTest cuda/prng_test.cu)
  target_include_directories(CudaPrngTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(CudaPrngTest PRIVATE GTest::gtest_main CudaUtils CUDA::cudart)
  set_target_properties(CudaPrngTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  gtest_discover_tests(CudaPrngTest TEST_PREFIX "CudaPrngTest.")

  if(WIN32)
    add_executable(D3DCudaInteropUtilsTest ui/d3d_cuda_interop_utils_test.cpp)
    target_include_directories(D3DCudaInteropUtilsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
    target_link_libraries(D3DCudaInteropUtilsTest PRIVATE GTest::gtest_main CUDA::cudart)
    gtest_discover_tests(D3DCudaInteropUtilsTest TEST_PREFIX "D3DCudaInteropUtilsTest.")
  endif()

  add_executable(CudaRawOpsTest raw/cuda_raw_ops_test.cpp)
  target_include_directories(CudaRawOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(CudaRawOpsTest PRIVATE GTest::gtest_main RawProcessorOp ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(CudaRawOpsTest TEST_PREFIX "CudaRawOpsTest.")
endif()

if(ALCEDO_CUDA_ENABLED AND ALCEDO_OPENCL_ENABLED)
  add_executable(OpenClCudaToLinearRefCompareTest raw/opencl_cuda_to_linear_ref_compare_test.cpp)
  target_include_directories(OpenClCudaToLinearRefCompareTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaToLinearRefCompareTest PRIVATE GTest::gtest_main RawProcessorOp ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClCudaToLinearRefCompareTest TEST_PREFIX "OpenClCudaCompare.")

  add_executable(OpenClCudaRcdCompareTest raw/opencl_cuda_rcd_compare_test.cpp)
  target_include_directories(OpenClCudaRcdCompareTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaRcdCompareTest PRIVATE GTest::gtest_main RawProcessorOp ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClCudaRcdCompareTest TEST_PREFIX "OpenClCudaCompare.")

  add_executable(OpenClCudaPipelineCompareTest raw/opencl_cuda_pipeline_compare_test.cpp)
  target_include_directories(OpenClCudaPipelineCompareTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaPipelineCompareTest PRIVATE GTest::gtest_main Operators RawProcessor OpenClGeometryUtils OpenClProgramLibrary ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  if(WIN32 AND TARGET alcedo_lensfun)
    add_custom_command(TARGET OpenClCudaPipelineCompareTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:alcedo_lensfun>"
              "$<TARGET_FILE_DIR:OpenClCudaPipelineCompareTest>/lensfun.dll"
      VERBATIM
    )
  endif()
  gtest_discover_tests(OpenClCudaPipelineCompareTest TEST_PREFIX "OpenClCudaCompare.")

  add_executable(OpenClCudaFullPipelineBenchmark raw/opencl_cuda_full_pipeline_benchmark.cpp)
  target_include_directories(OpenClCudaFullPipelineBenchmark PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaFullPipelineBenchmark PRIVATE GTest::gtest_main Operators RawProcessor OpenClProgramLibrary EditPipeline PipelineScheduler ThreadPool ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  if(WIN32 AND TARGET alcedo_lensfun)
    add_custom_command(TARGET OpenClCudaFullPipelineBenchmark POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:alcedo_lensfun>"
              "$<TARGET_FILE_DIR:OpenClCudaFullPipelineBenchmark>/lensfun.dll"
      VERBATIM
    )
  endif()
  gtest_discover_tests(OpenClCudaFullPipelineBenchmark TEST_PREFIX "OpenClCudaBenchmark.")
endif()

if(ALCEDO_METAL_ENABLED)
  add_executable(ImageBufferMetalTest image/image_buffer_metal_test.cpp)
  target_include_directories(ImageBufferMetalTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(ImageBufferMetalTest PRIVATE GTest::gtest_main ImageBuffer MetalImage ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(ImageBufferMetalTest TEST_PREFIX "ImageBufferMetalTest.")

  add_executable(MetalUtilsTest metal/metal_utils_test.cpp)
  target_include_directories(MetalUtilsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalUtilsTest PRIVATE GTest::gtest_main MetalImage MetalUtils ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(MetalUtilsTest TEST_PREFIX "MetalUtilsTest."
    PROPERTIES LABELS "ci_metal_runtime")

  add_executable(MetalRawOpsTest raw/metal_raw_ops_test.cpp)
  target_include_directories(MetalRawOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalRawOpsTest PRIVATE GTest::gtest_main RawProcessorOp ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(MetalRawOpsTest TEST_PREFIX "MetalRawOpsTest."
    PROPERTIES LABELS "ci_metal_runtime")

  add_executable(MetalRawStagePreviewTest raw/metal_raw_stage_preview_test.cpp)
  target_include_directories(MetalRawStagePreviewTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalRawStagePreviewTest PRIVATE GTest::gtest_main EditPipeline RawProcessor ThreadPool opencv_highgui ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(MetalRawStagePreviewTest TEST_PREFIX "MetalRawStagePreviewTest."
    PROPERTIES LABELS "ci_metal_runtime")

  add_executable(MetalFullPipelinePreviewTest raw/metal_full_pipeline_preview_test.cpp)
  target_include_directories(MetalFullPipelinePreviewTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalFullPipelinePreviewTest PRIVATE GTest::gtest_main EditPipeline RawProcessor PipelineScheduler ThreadPool opencv_highgui ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(MetalFullPipelinePreviewTest TEST_PREFIX "MetalFullPipelinePreviewTest."
    PROPERTIES LABELS "ci_metal_runtime")

  add_executable(MetalFastPreviewProfile raw/metal_fast_preview_profile.cpp)
  target_include_directories(MetalFastPreviewProfile PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalFastPreviewProfile PRIVATE GTest::gtest_main EditPipeline RawProcessor PipelineScheduler ThreadPool opencv_highgui ${ALCEDO_TEST_OPENCV_MIN_DEPS})
endif()

if(ALCEDO_OPENCL_ENABLED)
  add_executable(ImageBufferOpenClTest image/image_buffer_opencl_test.cpp)
  target_include_directories(ImageBufferOpenClTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(ImageBufferOpenClTest PRIVATE GTest::gtest_main ImageBuffer ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(ImageBufferOpenClTest TEST_PREFIX "ImageBufferOpenClTest.")

  add_executable(OpenClRawOpsTest raw/opencl_raw_ops_test.cpp)
  target_include_directories(OpenClRawOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClRawOpsTest PRIVATE GTest::gtest_main RawProcessorOp ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClRawOpsTest TEST_PREFIX "OpenClRawOpsTest.")

  add_executable(OpenCLRawPreview gui_pocs/opencl_raw_preview.cpp)
  target_include_directories(OpenCLRawPreview PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenCLRawPreview PRIVATE RawProcessor Qt6::Widgets ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  set(ALCEDO_QT_PLATFORMS_DIR "${Qt6_DIR}/../../../plugins/platforms")
  add_custom_command(TARGET OpenCLRawPreview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OpenCLRawPreview>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ALCEDO_QT_PLATFORMS_DIR}"
            "$<TARGET_FILE_DIR:OpenCLRawPreview>/platforms"
    VERBATIM
  )

  add_executable(OpenCLRawHLRPreview gui_pocs/opencl_raw_hlr_preview.cpp)
  target_include_directories(OpenCLRawHLRPreview PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenCLRawHLRPreview PRIVATE RawProcessor Qt6::Widgets ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  add_custom_command(TARGET OpenCLRawHLRPreview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OpenCLRawHLRPreview>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ALCEDO_QT_PLATFORMS_DIR}"
            "$<TARGET_FILE_DIR:OpenCLRawHLRPreview>/platforms"
    VERBATIM
  )

  if(ALCEDO_CUDA_ENABLED)
    add_executable(OpenClCudaPipelinePreview gui_pocs/opencl_cuda_pipeline_preview.cpp)
    target_include_directories(OpenClCudaPipelinePreview PUBLIC ${ALCEDO_INCLUDE_DIR})
    target_link_libraries(OpenClCudaPipelinePreview PRIVATE Operators RawProcessor OpenClGeometryUtils OpenClProgramLibrary Qt6::Widgets ${ALCEDO_TEST_OPENCV_MIN_DEPS})
    if(WIN32 AND TARGET alcedo_lensfun)
      add_custom_command(TARGET OpenClCudaPipelinePreview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:alcedo_lensfun>"
                "$<TARGET_FILE_DIR:OpenClCudaPipelinePreview>/lensfun.dll"
        VERBATIM
      )
    endif()
    add_custom_command(TARGET OpenClCudaPipelinePreview POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OpenClCudaPipelinePreview>/platforms"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${ALCEDO_QT_PLATFORMS_DIR}"
              "$<TARGET_FILE_DIR:OpenClCudaPipelinePreview>/platforms"
      VERBATIM
    )
  endif()
endif()

# add_executable(ImageDecoderTest decoders/image_decoder_test.cpp)
# target_include_directories(ImageDecoderTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(ImageDecoderTest PRIVATE GTest::gtest_main ImageDecoder Exiv2)

# add_executable(ImageLoaderTest image/image_loader_test.cpp)
# target_include_directories(ImageLoaderTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(ImageLoaderTest PRIVATE GTest::gtest_main ImageDecoder ImageLoader Exiv2)

# FIXME: Due to the introduction of Application layer, these tests need to be reworked
# But since they are using the low-level storage APIs, they can still be useful for testing
# However, some unit tests will be moved to App. layer tests later.

# add_executable(ImagePoolTest storage/image_pool_test.cpp)
# target_include_directories(ImagePoolTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(ImagePoolTest PRIVATE GTest::gtest_main Image ImagePool)

# add_executable(SleeveMapperTest storage/sleeve_mapper_test.cpp)
# target_include_directories(SleeveMapperTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(SleeveMapperTest PRIVATE GTest::gtest_main SleeveFS SleeveManager ImagePool)
add_executable(SleeveFSTest sleeve/sleeve_fs_test.cpp)
target_include_directories(SleeveFSTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFSTest PRIVATE GTest::gtest_main SleeveFS StrConv)

# add_executable(SleeveFilterTest sleeve/sleeve_filter_test.cpp)
# target_include_directories(SleeveFilterTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(SleeveFilterTest PRIVATE GTest::gtest_main SleeveFS SleeveManager StrConv)

# add_executable(SleeveViewTest sleeve/sleeve_view_test.cpp)
# target_include_directories(SleeveViewTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(SleeveViewTest PRIVATE GTest::gtest_main Image ImagePool TimeProvider SleeveView SleeveFS SleeveManager)

# Operation tests
# add_executable(ExposureOPTest edit/operators/basic/exposure_op_test.cpp)
# target_include_directories(ExposureOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(ExposureOPTest GTest::gtest_main Operators SleeveManager)

if(ALCEDO_CUDA_ENABLED)
  add_executable(SharedToneCurveTest edit/operators/basic/shared_tone_curve_test.cpp)
  target_include_directories(SharedToneCurveTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(SharedToneCurveTest PRIVATE GTest::gtest_main Operators CUDA::cudart)
  gtest_discover_tests(SharedToneCurveTest TEST_PREFIX "SharedToneCurveTest.")
endif()

add_executable(ToneMappingOwnershipTest edit/operators/tone/tone_mapping_ownership_test.cpp)
target_compile_definitions(ToneMappingOwnershipTest
    PRIVATE ALCEDO_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/alcedo_studio/src")
target_link_libraries(ToneMappingOwnershipTest PRIVATE GTest::gtest_main)
gtest_discover_tests(ToneMappingOwnershipTest TEST_PREFIX "ToneMappingOwnershipTest.")

add_executable(LocalToneMappingContractTest edit/operators/tone/local_tone_mapping_contract_test.cpp)
target_compile_definitions(LocalToneMappingContractTest
    PRIVATE ALCEDO_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/alcedo_studio/src")
target_include_directories(LocalToneMappingContractTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(LocalToneMappingContractTest PRIVATE GTest::gtest_main)
gtest_discover_tests(LocalToneMappingContractTest TEST_PREFIX "LocalToneMappingContractTest.")

add_executable(ToneMappingFacadeTest edit/operators/tone/tone_mapping_facade_test.cpp)
target_include_directories(ToneMappingFacadeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ToneMappingFacadeTest PRIVATE GTest::gtest_main Operators)
gtest_discover_tests(ToneMappingFacadeTest TEST_PREFIX "ToneMappingFacadeTest.")

# add_executable(ContrastOPTest edit/operators/basic/contrast_op_test.cpp)
# target_include_directories(ContrastOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(ContrastOPTest GTest::gtest_main Operators SleeveManager)

# add_executable(TintOPTest edit/operators/color/tint_op_test.cpp)
# target_include_directories(TintOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(TintOPTest GTest::gtest_main Operators SleeveManager)

# add_executable(SaturationOPTest edit/operators/color/saturation_op_test.cpp)
# target_include_directories(SaturationOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(SaturationOPTest GTest::gtest_main Operators SleeveManager)

# add_executable(VibranceOPTest edit/operators/color/vibrance_op_test.cpp)
# target_include_directories(VibranceOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(VibranceOPTest GTest::gtest_main Operators SleeveManager)

# add_executable(HLSOPTest edit/operators/color/hls_op_test.cpp)
# target_include_directories(HLSOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(HLSOPTest GTest::gtest_main Operators SleeveManager)

# add_executable(CWOPTest edit/operators/wheel/color_wheel_op_test.cpp)
# target_include_directories(CWOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(CWOPTest GTest::gtest_main Operators SleeveManager)

# add_executable(CurveOPTest edit/operators/curve/curve_op_test.cpp)
# target_include_directories(CurveOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(CurveOPTest GTest::gtest_main Operators SleeveManager)
# if (WIN32)
# target_link_libraries(CurveOPTest easy_profiler)
# endif()

# add_executable(ClarityOPTest edit/operators/detail/clarity_op_test.cpp)
# target_include_directories(ClarityOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(ClarityOPTest GTest::gtest_main Operators SleeveManager)

# add_executable(SharpenOPTest edit/operators/detail/sharpen_op_test.cpp)
# target_include_directories(SharpenOPTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(SharpenOPTest GTest::gtest_main Operators SleeveManager)

# add_executable(CSTTest edit/operators/cst/cst_op_test.cpp)
# target_include_directories(CSTTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(CSTTest GTest::gtest_main Operators SleeveManager ImageDecoder easy_profiler)

add_executable(ODTOpTest edit/operators/cst/odt_op_test.cpp)
target_include_directories(ODTOpTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ODTOpTest PRIVATE GTest::gtest_main Operators)
gtest_discover_tests(ODTOpTest TEST_PREFIX "ODTOpTest.")

add_executable(FilmGrainOpTest edit/operators/cst/film_grain_op_test.cpp)
target_include_directories(FilmGrainOpTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(FilmGrainOpTest PRIVATE GTest::gtest_main Operators)
gtest_discover_tests(FilmGrainOpTest TEST_PREFIX "FilmGrainOpTest.")

add_executable(HalationOpTest edit/operators/cst/halation_op_test.cpp)
target_include_directories(HalationOpTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(HalationOpTest PRIVATE GTest::gtest_main Operators)
gtest_discover_tests(HalationOpTest TEST_PREFIX "HalationOpTest.")

add_executable(LutCatalogTest
    ui/lut_catalog_test.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/modules/lut_catalog.cpp
)
target_include_directories(LutCatalogTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
)
target_link_libraries(LutCatalogTest PRIVATE GTest::gtest_main Qt6::Core)
gtest_discover_tests(LutCatalogTest TEST_PREFIX "LutCatalogTest.")

add_executable(EditorPipelineIoTest
    ui/editor_pipeline_io_test.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/modules/pipeline_io.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/modules/color_temp.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/modules/color_wheel.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/modules/curve.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/modules/geometry.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/modules/hls.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/editor_dialog/state.cpp
)
target_include_directories(EditorPipelineIoTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorPipelineIoTest
    PRIVATE
        GTest::gtest_main
        EditPipeline
        Operators
        JSON
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
gtest_discover_tests(EditorPipelineIoTest TEST_PREFIX "EditorPipelineIoTest.")

add_executable(CropRotateOpTest edit/operators/geometry/crop_rotate_op_test.cpp)
target_include_directories(CropRotateOpTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(CropRotateOpTest PRIVATE GTest::gtest_main Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(CropRotateOpTest TEST_PREFIX "CropRotateOpTest."
  PROPERTIES LABELS "ci_core_flow")

if(ALCEDO_CUDA_ENABLED)
  add_executable(ODTCudaSmokeTest edit/operators/cst/odt_cuda_smoke_test.cu)
  target_include_directories(ODTCudaSmokeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(ODTCudaSmokeTest PRIVATE GTest::gtest_main Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS} CUDA::cudart)
  set_target_properties(ODTCudaSmokeTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  gtest_discover_tests(ODTCudaSmokeTest TEST_PREFIX "ODTCudaSmokeTest.")

  add_executable(FilmGrainCudaStageTest edit/operators/cst/film_grain_cuda_stage_test.cu)
  target_include_directories(FilmGrainCudaStageTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(FilmGrainCudaStageTest PRIVATE GTest::gtest_main Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS} CUDA::cudart)
  set_target_properties(FilmGrainCudaStageTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  gtest_discover_tests(FilmGrainCudaStageTest TEST_PREFIX "FilmGrainCudaStageTest.")

  add_executable(HalationCudaStageTest edit/operators/cst/halation_cuda_stage_test.cu)
  target_include_directories(HalationCudaStageTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(HalationCudaStageTest PRIVATE GTest::gtest_main Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS} CUDA::cudart)
  set_target_properties(HalationCudaStageTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  gtest_discover_tests(HalationCudaStageTest TEST_PREFIX "HalationCudaStageTest.")

  if(ALCEDO_ENABLE_CUDA_PREVIEW_VRAM_RECLAMATION_TEST)
    add_executable(CudaPreviewVramReclamationTest edit/pipeline/cuda_preview_vram_reclamation_test.cu)
    target_include_directories(CudaPreviewVramReclamationTest PUBLIC ${ALCEDO_INCLUDE_DIR})
    target_link_libraries(CudaPreviewVramReclamationTest PRIVATE GTest::gtest_main EditPipeline PipelineScheduler Operators ThreadPool ${ALCEDO_TEST_OPENCV_MIN_DEPS} CUDA::cudart)
    set_target_properties(CudaPreviewVramReclamationTest PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    gtest_discover_tests(CudaPreviewVramReclamationTest TEST_PREFIX "CudaPreviewVramReclamationTest.")
  endif()
endif()

# add_executable(TileTest edit/pipeline/tile_test.cpp)
# target_include_directories(TileTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(TileTest GTest::gtest_main Operators SleeveManager ImageDecoder EditPipeline OpenColorIO lcms2::lcms2)

# add_executable(PipelineTest edit/pipeline/cpu_pipeline_test.cpp)
# target_include_directories(PipelineTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(PipelineTest GTest::gtest_main Operators SleeveManager ImageDecoder EditPipeline OpenColorIO lcms2::lcms2)
# if (WIN32)
# target_link_libraries(PipelineTest easy_profiler)
# endif()

# add_executable(PipelineSchedulerTest edit/pipeline/pipeline_scheduler_test.cpp)
# target_include_directories(PipelineSchedulerTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(PipelineSchedulerTest GTest::gtest_main Operators SleeveManager ImageDecoder EditPipeline PipelineScheduler OpenColorIO lcms2::lcms2)
# if (WIN32)
# target_link_libraries(PipelineSchedulerTest easy_profiler)
# endif()

# add_executable(EditTransactionTest edit/history/transaction_test.cpp)
# target_include_directories(EditTransactionTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(EditTransactionTest GTest::gtest_main Operators SleeveManager ImageDecoder EditPipeline PipelineScheduler OpenColorIO lcms2::lcms2)
# if (WIN32)
# target_link_libraries(EditTransactionTest easy_profiler)
# endif()

# add_executable(EditVersionTest edit/history/version_test.cpp)
# target_include_directories(EditVersionTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(EditVersionTest GTest::gtest_main Operators SleeveManager ImageDecoder EditPipeline PipelineScheduler OpenColorIO lcms2::lcms2)
# if (WIN32)
# target_link_libraries(EditVersionTest easy_profiler)
# endif()

# add_executable(EditHistoryTest edit/history/history_test.cpp)
# target_include_directories(EditHistoryTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(EditHistoryTest GTest::gtest_main Operators SleeveManager ImageDecoder EditPipeline PipelineScheduler OpenColorIO lcms2::lcms2)
# if (WIN32)
# target_link_libraries(EditHistoryTest easy_profiler)
# endif()

# add_executable(PipelineSerialTest edit/pipeline/pipeline_serial_test.cpp)
# target_include_directories(PipelineSerialTest PUBLIC ${CMAKE_SOURCE_DIR}/alcedo/src/include)
# target_link_libraries(PipelineSerialTest GTest::gtest_main Operators SleeveManager ImageDecoder EditPipeline PipelineScheduler OpenColorIO lcms2::lcms2)
# if (WIN32)
# target_link_libraries(PipelineSerialTest easy_profiler)
# endif()

add_executable(PipelineFrameSinkTest edit/pipeline/pipeline_frame_sink_test.cpp)
target_include_directories(PipelineFrameSinkTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(PipelineFrameSinkTest PRIVATE GTest::gtest_main Operators EditPipeline
                                                    PipelineScheduler ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(PipelineFrameSinkTest TEST_PREFIX "PipelineFrameSinkTest.")
if(TARGET EditViewer)
  add_executable(ImagePreview gui_pocs/image_preview_cpu.cpp)
  target_include_directories(ImagePreview PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(ImagePreview Image ImageLoader EditPipeline EditViewer Operators PipelineScheduler OpenColorIO::OpenColorIO lcms2::lcms2 Qt6::Widgets)

  if(WIN32)
    target_link_libraries(ImagePreview FSService)
  endif()

  target_compile_definitions(ImagePreview PRIVATE ALCEDO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

  set(ALCEDO_IMAGE_PREVIEW_FONT_SRC "${ALCEDO_SRC_DIR}/config/fonts/main_IBM.ttf")
  add_custom_command(TARGET ImagePreview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ImagePreview>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_IMAGE_PREVIEW_FONT_SRC}" "$<TARGET_FILE_DIR:ImagePreview>/fonts/main_IBM.ttf"
    VERBATIM
  )
endif()

add_executable(ImportServiceTest app/import_service_test.cpp)
target_include_directories(ImportServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImportServiceTest ImportService ProjectService GTest::gtest_main)

if(WIN32)
  target_link_libraries(ImportServiceTest)
endif()

add_executable(SleeveServiceTest app/sleeve_service_test.cpp)
target_include_directories(SleeveServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveServiceTest
  ProjectService
  PipelineMgmtService
  EditHistoryMgmtService
  GTest::gtest_main)

if(WIN32)
  target_link_libraries(SleeveServiceTest)
endif()

add_executable(FilterServiceTest app/filter_service_test.cpp)
target_include_directories(FilterServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(FilterServiceTest ImportService ProjectService SleeveFilterService GTest::gtest_main)
gtest_discover_tests(FilterServiceTest TEST_PREFIX "FilterServiceTest.")

add_executable(SearchQueryClassifierTest app/search_query_classifier_test.cpp)
target_include_directories(SearchQueryClassifierTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SearchQueryClassifierTest SleeveFilterService GTest::gtest_main)
gtest_discover_tests(SearchQueryClassifierTest TEST_PREFIX "SearchQueryClassifierTest.")

if(WIN32)
  target_link_libraries(FilterServiceTest)
endif()

add_executable(PipelineServiceTest app/pipeline_service_test.cpp)
target_include_directories(PipelineServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(PipelineServiceTest ProjectService PipelineMgmtService GTest::gtest_main)
gtest_discover_tests(PipelineServiceTest TEST_PREFIX "PipelineServiceTest."
  PROPERTIES LABELS "ci_core_flow")

if(WIN32)
  target_link_libraries(PipelineServiceTest)
endif()

add_executable(AdjustmentTransferServiceTest app/adjustment_transfer_service_test.cpp)
target_include_directories(AdjustmentTransferServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(AdjustmentTransferServiceTest
  AdjustmentTransferService
  ProjectService
  PipelineMgmtService
  EditHistoryMgmtService
  GTest::gtest_main
)
gtest_discover_tests(AdjustmentTransferServiceTest TEST_PREFIX "AdjustmentTransferServiceTest."
  PROPERTIES LABELS "ci_core_flow")

if(WIN32)
  target_link_libraries(AdjustmentTransferServiceTest)
endif()

add_executable(EditHistoryMgmtServiceTest app/history_mgmt_service_test.cpp)
target_include_directories(EditHistoryMgmtServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditHistoryMgmtServiceTest ProjectService EditHistoryMgmtService GTest::gtest_main)
gtest_discover_tests(EditHistoryMgmtServiceTest TEST_PREFIX "EditHistoryMgmtServiceTest."
  PROPERTIES LABELS "ci_core_flow")

if(WIN32)
  target_link_libraries(EditHistoryMgmtServiceTest)
  if(TARGET alcedo_lensfun)
    add_custom_command(TARGET EditHistoryMgmtServiceTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:alcedo_lensfun>"
              "$<TARGET_FILE_DIR:EditHistoryMgmtServiceTest>/lensfun.dll"
      VERBATIM)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll")
    add_custom_command(TARGET EditHistoryMgmtServiceTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll"
              "$<TARGET_FILE_DIR:EditHistoryMgmtServiceTest>/duckdb.dll"
      VERBATIM)
  endif()
endif()

add_executable(VersionHashTest edit/history/version_hash_test.cpp)
target_include_directories(VersionHashTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(VersionHashTest EditHistory GTest::gtest_main)
gtest_discover_tests(VersionHashTest TEST_PREFIX "VersionHashTest."
  PROPERTIES LABELS "ci_core_flow")

add_executable(ProjectServiceTest app/project_service_test.cpp)
target_include_directories(ProjectServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ProjectServiceTest ProjectService GTest::gtest_main)
gtest_discover_tests(ProjectServiceTest TEST_PREFIX "ProjectServiceTest."
  PROPERTIES LABELS "ci_core_flow")

if(WIN32)
  target_link_libraries(ProjectServiceTest)
  if(TARGET alcedo_lensfun)
    add_custom_command(TARGET ProjectServiceTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:alcedo_lensfun>"
              "$<TARGET_FILE_DIR:ProjectServiceTest>/lensfun.dll"
      VERBATIM)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll")
    add_custom_command(TARGET ProjectServiceTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll"
              "$<TARGET_FILE_DIR:ProjectServiceTest>/duckdb.dll"
      VERBATIM)
  endif()
endif()

add_executable(SemanticFakeRuntime app/semantic_fake_runtime_main.cpp)

add_executable(SemanticRuntimeServiceTest app/semantic_runtime_service_test.cpp)
target_include_directories(SemanticRuntimeServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_compile_definitions(SemanticRuntimeServiceTest PRIVATE
  ALCEDO_SEMANTIC_FAKE_RUNTIME_PATH="$<TARGET_FILE:SemanticFakeRuntime>")
target_link_libraries(SemanticRuntimeServiceTest SemanticRuntimeService ProjectService GTest::gtest_main)
add_dependencies(SemanticRuntimeServiceTest SemanticFakeRuntime)
gtest_discover_tests(SemanticRuntimeServiceTest TEST_PREFIX "SemanticRuntimeServiceTest."
  PROPERTIES LABELS "ci_core_flow")

add_executable(SemanticGrpcDependencySmokeTest app/semantic_grpc_dependency_smoke_test.cpp)
target_include_directories(SemanticGrpcDependencySmokeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SemanticGrpcDependencySmokeTest
  puerhlab::grpc++
  puerhlab::protobuf
  GTest::gtest_main)
gtest_discover_tests(SemanticGrpcDependencySmokeTest TEST_PREFIX "SemanticGrpcDependencySmokeTest."
  PROPERTIES LABELS "ci_core_flow")

add_executable(SemanticStorageControllerTest storage/semantic_storage_controller_test.cpp)
target_include_directories(SemanticStorageControllerTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SemanticStorageControllerTest ProjectService GTest::gtest_main)
gtest_discover_tests(SemanticStorageControllerTest TEST_PREFIX "SemanticStorageControllerTest."
  PROPERTIES LABELS "ci_core_flow")
if(WIN32)
  target_link_libraries(SemanticStorageControllerTest)
  if(TARGET alcedo_lensfun)
    add_custom_command(TARGET SemanticStorageControllerTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:alcedo_lensfun>"
              "$<TARGET_FILE_DIR:SemanticStorageControllerTest>/lensfun.dll"
      VERBATIM)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll")
    add_custom_command(TARGET SemanticStorageControllerTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll"
              "$<TARGET_FILE_DIR:SemanticStorageControllerTest>/duckdb.dll"
      VERBATIM)
  endif()
  if(ALCEDO_DUCKDB_VSS_EXTENSION)
    add_custom_command(TARGET SemanticStorageControllerTest POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_FILE_DIR:SemanticStorageControllerTest>/duckdb_extensions"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${ALCEDO_DUCKDB_VSS_EXTENSION}"
              "$<TARGET_FILE_DIR:SemanticStorageControllerTest>/duckdb_extensions/vss.duckdb_extension"
      VERBATIM)
  endif()
endif()

add_executable(SemanticGenerationServiceTest app/semantic_generation_service_test.cpp)
target_include_directories(SemanticGenerationServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SemanticGenerationServiceTest
  SemanticGenerationService
  ProjectService
  ImportService
  DuckDB
  GTest::gtest_main)
gtest_discover_tests(SemanticGenerationServiceTest TEST_PREFIX "SemanticGenerationServiceTest."
  PROPERTIES LABELS "ci_raw_flow")

add_executable(ThumbnailServiceTest app/thumbnail_service_test.cpp)
target_include_directories(ThumbnailServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ThumbnailServiceTest ThumbnailService ProjectService ImportService GTest::gtest_main)

if(WIN32)
  target_link_libraries(ThumbnailServiceTest)
endif()

add_executable(ThumbnailDiskCacheServiceTest app/thumbnail_disk_cache_service_test.cpp)
target_include_directories(ThumbnailDiskCacheServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ThumbnailDiskCacheServiceTest ThumbnailDiskCacheService GTest::gtest_main)
gtest_discover_tests(ThumbnailDiskCacheServiceTest TEST_PREFIX "ThumbnailDiskCacheServiceTest.")

add_executable(ExportServiceTest app/export_service_test.cpp)
target_include_directories(ExportServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ExportServiceTest ExportService ProjectService ImportService GTest::gtest_main ImageWriter ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(ExportServiceTest TEST_PREFIX "ExportServiceTest.")

if(WIN32)
  target_link_libraries(ExportServiceTest)
endif()

add_executable(HsResearchExportTool app/hs_research_export_tool.cpp)
target_include_directories(HsResearchExportTool PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(HsResearchExportTool PRIVATE ExportService ProjectService ImportService ImageWriter ${ALCEDO_TEST_OPENCV_MIN_DEPS})

if(WIN32)
  target_link_libraries(HsResearchExportTool)
  if(TARGET alcedo_lensfun)
    add_custom_command(TARGET HsResearchExportTool POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:alcedo_lensfun>"
              "$<TARGET_FILE_DIR:HsResearchExportTool>/lensfun.dll"
      VERBATIM)
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll")
    add_custom_command(TARGET HsResearchExportTool POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_SOURCE_DIR}/alcedo_studio/third_party/libduckdb-windows/duckdb.dll"
              "$<TARGET_FILE_DIR:HsResearchExportTool>/duckdb.dll"
      VERBATIM)
  endif()
endif()

add_executable(ImageWriterTest io/image_writer_test.cpp)
target_include_directories(ImageWriterTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImageWriterTest PRIVATE GTest::gtest_main ImageWriter ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(ImageWriterTest TEST_PREFIX "ImageWriterTest."
  PROPERTIES LABELS "ci_core_flow")

add_executable(MetadataExtractorTest image/metadata_extractor_test.cpp)
target_include_directories(MetadataExtractorTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(MetadataExtractorTest PRIVATE GTest::gtest_main Image Operators)
gtest_discover_tests(MetadataExtractorTest TEST_PREFIX "MetadataExtractorTest.")

add_executable(BatchImportDngMetadataTest image/batch_import_dng_metadata_test.cpp)
target_include_directories(BatchImportDngMetadataTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(BatchImportDngMetadataTest PRIVATE GTest::gtest_main Image Operators)
gtest_discover_tests(BatchImportDngMetadataTest TEST_PREFIX "BatchImportDngMetadataTest.")

add_executable(DngMetadataBenchmark image/dng_metadata_benchmark.cpp)
target_include_directories(DngMetadataBenchmark PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DngMetadataBenchmark PRIVATE OpenImageIO::OpenImageIO Exiv2 libraw::raw_r)

add_executable(ExportIccProfileResolverTest io/export_icc_profile_resolver_test.cpp)
target_include_directories(ExportIccProfileResolverTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ExportIccProfileResolverTest PRIVATE GTest::gtest_main ExportIccProfileResolver)
gtest_discover_tests(ExportIccProfileResolverTest TEST_PREFIX "ExportIccProfileResolverTest.")

add_executable(CudaDriverRequirementsTest
    utils/cuda_driver_requirements_test.cpp
    ${ALCEDO_SRC_DIR}/utils/cuda/cuda_driver_requirements.cpp
)
target_include_directories(CudaDriverRequirementsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(CudaDriverRequirementsTest PRIVATE GTest::gtest_main)
gtest_discover_tests(CudaDriverRequirementsTest TEST_PREFIX "CudaDriverRequirementsTest.")

if(ALCEDO_OPENCL_ENABLED)
  add_executable(OpenClProgramLibraryTest opencl/opencl_program_library_test.cpp)
  target_include_directories(OpenClProgramLibraryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClProgramLibraryTest PRIVATE GTest::gtest_main OpenClProgramLibrary)
  gtest_discover_tests(OpenClProgramLibraryTest TEST_PREFIX "OpenClProgramLibraryTest.")

  add_executable(OpenClRuntimeTest opencl/opencl_runtime_test.cpp)
  target_include_directories(OpenClRuntimeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClRuntimeTest PRIVATE GTest::gtest_main OpenClProgramLibrary)
  gtest_discover_tests(OpenClRuntimeTest TEST_PREFIX "OpenClRuntimeTest.")

  add_executable(OpenClFusedEditPipelineTest opencl/opencl_fused_edit_pipeline_test.cpp)
  target_include_directories(OpenClFusedEditPipelineTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClFusedEditPipelineTest
    PRIVATE
      GTest::gtest_main
      EditPipeline
      OpenClProgramLibrary
      ${ALCEDO_TEST_OPENCV_MIN_DEPS}
  )
  gtest_discover_tests(OpenClFusedEditPipelineTest TEST_PREFIX "OpenClFusedEditPipelineTest.")
endif()

if(ALCEDO_CUDA_ENABLED AND ALCEDO_OPENCL_ENABLED)
  add_executable(OpenClGeometryUtilsTest opencl/opencl_geometry_utils_test.cpp)
  target_include_directories(OpenClGeometryUtilsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClGeometryUtilsTest PRIVATE GTest::gtest_main Operators RawProcessorOp OpenClGeometryUtils ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClGeometryUtilsTest TEST_PREFIX "OpenClGeometryUtilsTest.")
endif()

add_executable(ImagePathEncodingTest io/image_path_encoding_test.cpp)
target_include_directories(ImagePathEncodingTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImagePathEncodingTest PRIVATE GTest::gtest_main ImageLoader StorageService)
gtest_discover_tests(ImagePathEncodingTest TEST_PREFIX "ImagePathEncodingTest.")

add_executable(CiRawWorkflowTest ci/ci_raw_workflow_test.cpp)
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

add_executable(SleeveFilesystemCiTest ci/sleeve_filesystem_ci_test.cpp)
target_include_directories(SleeveFilesystemCiTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFilesystemCiTest PRIVATE GTest::gtest_main SleeveFS StrConv)
gtest_discover_tests(SleeveFilesystemCiTest TEST_PREFIX "SleeveFilesystemCiTest."
  PROPERTIES LABELS "ci_core_flow")

add_executable(ThumbnailAlbumQtDemo app/thumbnail_album_qt_demo.cpp)
target_include_directories(ThumbnailAlbumQtDemo PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ThumbnailAlbumQtDemo PRIVATE ThumbnailService ProjectService ImportService Qt6::Widgets SleeveFilterService)
target_compile_definitions(ThumbnailAlbumQtDemo PRIVATE ALCEDO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set(ALCEDO_DEMO_FONT_SRC "${ALCEDO_SRC_DIR}/config/fonts/main_IBM.ttf")
add_custom_command(TARGET ThumbnailAlbumQtDemo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ThumbnailAlbumQtDemo>/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_DEMO_FONT_SRC}" "$<TARGET_FILE_DIR:ThumbnailAlbumQtDemo>/fonts/main_IBM.ttf"
  VERBATIM
)

# CompleteUIDemo disabled — album_editor_qt_demo.cpp is no longer maintained.
# if(TARGET EditViewer)
#   add_executable(CompleteUIDemo gui_pocs/album_editor_qt_demo.cpp)
#   target_include_directories(CompleteUIDemo PUBLIC ${ALCEDO_INCLUDE_DIR})
#   target_link_libraries(CompleteUIDemo PRIVATE ThumbnailService ProjectService ImportService ExportService PipelineMgmtService EditHistoryMgmtService FSService Qt6::Widgets SleeveFilterService EditPipeline EditViewer)
#   target_compile_definitions(CompleteUIDemo PRIVATE ALCEDO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
#   add_custom_command(TARGET CompleteUIDemo POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:CompleteUIDemo>/fonts"
#     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_DEMO_FONT_SRC}" "$<TARGET_FILE_DIR:CompleteUIDemo>/fonts/main_IBM.ttf"
#     VERBATIM
#   )
# endif()

include(GoogleTest)

# set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
# gtest_discover_tests(SampleTest)
# gtest_discover_tests(SingleRawLoad)
# gtest_discover_tests(SingleThumbnailLoad)
# gtest_discover_tests(ImageDecoderTest)
# gtest_discover_tests(ImageLoaderTest)
# # gtest_discover_tests(SleeveOperationTest)
# gtest_discover_tests(ImagePoolTest)
# # gtest_discover_tests(SleeveViewTest)
# # gtest_discover_tests(SleeveMapperTest)
# gtest_discover_tests(SleeveFSTest)
# gtest_discover_tests(ExposureOPTest)
# gtest_discover_tests(ContrastOPTest)
# gtest_discover_tests(TintOPTest)
# gtest_discover_tests(SaturationOPTest)
# gtest_discover_tests(VibranceOPTest)
# gtest_discover_tests(ClarityOPTest)
# gtest_discover_tests(SharpenOPTest)
# gtest_discover_tests(PipelineTest)

# ==============================================================================
# UI Tests — AlbumBackend (GTest + QSignalSpy)
# ==============================================================================
# Uses a custom main() that creates QCoreApplication, so we link GTest::gtest
# (not gtest_main) and compile ui_test_main.cpp into each target.

# Macro to reduce boilerplate for UI test targets.
macro(def_ui_test TARGET_NAME SOURCE_FILE)
    add_executable(${TARGET_NAME}
        ui/${SOURCE_FILE}
        ui/ui_test_main.cpp
        ui/editor_dialog_stub.cpp
    )
    target_include_directories(${TARGET_NAME}
        PUBLIC ${ALCEDO_INCLUDE_DIR}
        PRIVATE ${ALCEDO_TEST_DIR}
    )
    target_link_libraries(${TARGET_NAME}
        PRIVATE
            AlbumBackendLib
            GTest::gtest
            Qt6::Test
            Qt6::Core
    )
    include(GoogleTest)
    if(NOT "${ARGN}" STREQUAL "")
        gtest_discover_tests(${TARGET_NAME} TEST_PREFIX "${TARGET_NAME}."
            PROPERTIES LABELS ${ARGN})
    else()
        gtest_discover_tests(${TARGET_NAME} TEST_PREFIX "${TARGET_NAME}.")
    endif()
endmacro()

def_ui_test(AlbumBackendImportTest   album_backend_import_test.cpp)
def_ui_test(AlbumBackendProjectTest  album_backend_project_test.cpp)
def_ui_test(AlbumBackendFolderTest   album_backend_folder_test.cpp)
def_ui_test(AlbumBackendImageDeleteTest album_backend_image_delete_test.cpp)
def_ui_test(AlbumBackendImageDetailsTest album_backend_image_details_test.cpp)
def_ui_test(AlbumBackendRatingTest album_backend_rating_test.cpp "ci_raw_flow")
def_ui_test(AlbumBackendThumbnailTest album_backend_thumbnail_test.cpp)
def_ui_test(AlbumBackendI18nTest album_backend_i18n_test.cpp)
def_ui_test(AlbumBackendCiWorkflowTest album_backend_ci_workflow_test.cpp "ci_raw_flow")

add_executable(GlobalSearchDialogQmlTest
    ui/global_search_dialog_qml_test.cpp
    ui/widget_test_main.cpp
    ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(GlobalSearchDialogQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(GlobalSearchDialogQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(GlobalSearchDialogQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(GlobalSearchDialogQmlTest TEST_PREFIX "GlobalSearchDialogQmlTest.")

add_executable(LanguageManagerTest
    ui/language_manager_test.cpp
    ui/ui_test_main.cpp
)
target_include_directories(LanguageManagerTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(LanguageManagerTest
    PRIVATE
        UiLocalization
        GTest::gtest
        Qt6::Test
        Qt6::Core
        Qt6::Qml
)
gtest_discover_tests(LanguageManagerTest TEST_PREFIX "LanguageManagerTest.")

alcedo_assign_test_category(core ALCEDO_BUILD_CORE_TESTS alcedo_tests_core
  SampleTest
  CudaDriverRequirementsTest
  OpenClProgramLibraryTest
  OpenClRuntimeTest
)

alcedo_assign_test_category(image ALCEDO_BUILD_IMAGE_TESTS alcedo_tests_image
  ImageBufferMetalTest
  ImageBufferOpenClTest
  MetadataExtractorTest
  BatchImportDngMetadataTest
)

alcedo_assign_test_category(io ALCEDO_BUILD_IO_TESTS alcedo_tests_io
  ImageWriterTest
  ExportIccProfileResolverTest
  ImagePathEncodingTest
)

alcedo_assign_test_category(raw ALCEDO_BUILD_RAW_TESTS alcedo_tests_raw
  SingleRawLoad
  SingleThumbnailLoad
  ColorTempCudaSanityTest
  OpenClRawOpsTest
  MetalRawStagePreviewTest
  MetalFullPipelinePreviewTest
)

alcedo_assign_test_category(gpu ALCEDO_BUILD_GPU_TESTS alcedo_tests_gpu
  CudaPrngTest
  CudaImageGeometryOpsTest
  D3DCudaInteropUtilsTest
  CudaRawOpsTest
  MetalUtilsTest
  MetalRawOpsTest
  ODTCudaSmokeTest
  FilmGrainCudaStageTest
  HalationCudaStageTest
  CudaPreviewVramReclamationTest
  OpenClCudaToLinearRefCompareTest
  OpenClCudaPipelineCompareTest
  OpenClCudaFullPipelineBenchmark
  OpenClFusedEditPipelineTest
  OpenClGeometryUtilsTest
)

alcedo_assign_test_category(edit ALCEDO_BUILD_EDIT_TESTS alcedo_tests_edit
  SharedToneCurveTest
  ToneMappingOwnershipTest
  LocalToneMappingContractTest
  ToneMappingFacadeTest
  ODTOpTest
  FilmGrainOpTest
  HalationOpTest
  CropRotateOpTest
  PipelineFrameSinkTest
)

alcedo_assign_test_category(ui ALCEDO_BUILD_UI_TESTS alcedo_tests_ui
  EditViewerLogicTest
  ToneCurveWidgetTest
  LutCatalogTest
  EditorPipelineIoTest
  AlbumBackendImportTest
  AlbumBackendProjectTest
  AlbumBackendFolderTest
  AlbumBackendImageDeleteTest
  AlbumBackendImageDetailsTest
  AlbumBackendThumbnailTest
  AlbumBackendI18nTest
  GlobalSearchDialogQmlTest
  LanguageManagerTest
)

alcedo_assign_test_category(app ALCEDO_BUILD_APP_TESTS alcedo_tests_app
  ImportServiceTest
  SleeveServiceTest
  FilterServiceTest
  PipelineServiceTest
  EditHistoryMgmtServiceTest
  SemanticGenerationServiceTest
  ThumbnailServiceTest
  ThumbnailDiskCacheServiceTest
  ExportServiceTest
)

alcedo_assign_test_category(storage ALCEDO_BUILD_STORAGE_TESTS alcedo_tests_storage
  SleeveFSTest
)

alcedo_assign_test_category(demos ALCEDO_BUILD_DEMO_TARGETS alcedo_tests_demos
  ImagePreview
  OpenCLRawPreview
  OpenCLRawHLRPreview
  OpenClCudaPipelinePreview
  ThumbnailAlbumQtDemo
  # CompleteUIDemo  # disabled
  DngMetadataBenchmark
)

alcedo_assign_test_category(ci_core ALCEDO_BUILD_CI_TESTS alcedo_tests_ci_core
  ImageWriterTest
  SleeveFilesystemCiTest
  PipelineServiceTest
  EditHistoryMgmtServiceTest
  CropRotateOpTest
  SemanticGrpcDependencySmokeTest
  SemanticRuntimeServiceTest
  SemanticStorageControllerTest
)

alcedo_assign_test_category(ci_raw ALCEDO_BUILD_CI_TESTS alcedo_tests_ci_raw
  CiRawWorkflowTest
  AlbumBackendRatingTest
  AlbumBackendCiWorkflowTest
  SemanticGenerationServiceTest
)

alcedo_assign_test_category(ci_metal ALCEDO_BUILD_CI_TESTS alcedo_tests_ci_metal
  MetalUtilsTest
  MetalRawOpsTest
  MetalRawStagePreviewTest
  MetalFullPipelinePreviewTest
)
