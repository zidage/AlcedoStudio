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

if(ALCEDO_METAL_ENABLED)
  # Boundary test kernels: compiled to a standalone metallib next to the test binary.
  find_program(ALCEDO_XCRUN_EXECUTABLE xcrun REQUIRED)
  set(ALCEDO_TEST_BOUNDARY_METAL_SRC
      "${ALCEDO_TEST_ROOT}/ml_ops/metal_demosaicnet_boundary.metal")
  set(ALCEDO_TEST_BOUNDARY_METAL_AIR
      "${CMAKE_CURRENT_BINARY_DIR}/metal_demosaicnet_boundary.air")
  set(ALCEDO_TEST_BOUNDARY_METALLIB
      "${CMAKE_CURRENT_BINARY_DIR}/metal_demosaicnet_boundary.metallib")
  set(ALCEDO_TEST_BOUNDARY_METAL_FLAGS -fno-color-diagnostics)
  if(DEFINED ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET AND
     NOT "${ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET}" STREQUAL "")
    list(APPEND ALCEDO_TEST_BOUNDARY_METAL_FLAGS
         "-mmacosx-version-min=${ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET}")
  endif()
  add_custom_command(
    OUTPUT "${ALCEDO_TEST_BOUNDARY_METAL_AIR}"
    COMMAND ${ALCEDO_XCRUN_EXECUTABLE} --sdk macosx metal
            ${ALCEDO_TEST_BOUNDARY_METAL_FLAGS}
            -c "${ALCEDO_TEST_BOUNDARY_METAL_SRC}"
            -o "${ALCEDO_TEST_BOUNDARY_METAL_AIR}"
    DEPENDS "${ALCEDO_TEST_BOUNDARY_METAL_SRC}"
    COMMENT "Compiling Metal DemosaicNet boundary test shader"
    VERBATIM
    COMMAND_EXPAND_LISTS)
  add_custom_command(
    OUTPUT "${ALCEDO_TEST_BOUNDARY_METALLIB}"
    COMMAND ${ALCEDO_XCRUN_EXECUTABLE} --sdk macosx metallib
            "${ALCEDO_TEST_BOUNDARY_METAL_AIR}"
            -o "${ALCEDO_TEST_BOUNDARY_METALLIB}"
    DEPENDS "${ALCEDO_TEST_BOUNDARY_METAL_AIR}"
    COMMENT "Linking Metal DemosaicNet boundary test metallib"
    VERBATIM)
  add_custom_target(MetalDemosaicNetBoundaryTestShaders
    DEPENDS "${ALCEDO_TEST_BOUNDARY_METALLIB}")

  add_executable(MetalDemosaicNetModuleTest
    ${ALCEDO_TEST_ROOT}/ml_ops/metal_demosaicnet_module_test.mm
    ${ALCEDO_TEST_ROOT}/ml_ops/metal_demosaicnet_graph_test.mm
    ${ALCEDO_TEST_ROOT}/ml_ops/metal_demosaicnet_io_test.mm)
  target_include_directories(MetalDemosaicNetModuleTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalDemosaicNetModuleTest PRIVATE
    GTest::gtest_main
    MetalDemosaicNet
    MetalDemosaicNetEntry
    MetalContext
    NnSafetensors
  )
  target_compile_definitions(MetalDemosaicNetModuleTest PRIVATE
    HAVE_METAL
    ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_SOURCE_DIR}/alcedo_studio/src/config/models"
    ALCEDO_METAL_DEMOSAICNET_BOUNDARY_METALLIB_PATH="${ALCEDO_TEST_BOUNDARY_METALLIB}"
  )
  set_source_files_properties(
    ${ALCEDO_TEST_ROOT}/ml_ops/metal_demosaicnet_module_test.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc" LANGUAGE OBJCXX)
  add_dependencies(MetalDemosaicNetModuleTest MetalDemosaicNetBoundaryTestShaders)
  gtest_discover_tests(MetalDemosaicNetModuleTest TEST_PREFIX "MetalDemosaicNetModuleTest.")
  alcedo_register_test_target(MetalDemosaicNetModuleTest gpu)
endif()

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
