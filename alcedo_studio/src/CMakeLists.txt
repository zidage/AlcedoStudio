# ==============================================================================
# Helper Macro to reduce boilerplate
# ==============================================================================
# Usage: def_library(Name SOURCES src1 src2 PUBLIC_DEPS lib1 PRIVATE_DEPS lib2)
macro(def_library target_name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    
    add_library(${target_name} ${ARG_SOURCES})
    target_include_directories(${target_name} PUBLIC include)
    
    if(ARG_PUBLIC_DEPS)
        target_link_libraries(${target_name} PUBLIC ${ARG_PUBLIC_DEPS})
    endif()
    
    if(ARG_PRIVATE_DEPS)
        target_link_libraries(${target_name} PRIVATE ${ARG_PRIVATE_DEPS})
    endif()
endmacro()

# ==============================================================================
# 1. Utilities
# ==============================================================================

set(ALCEDO_OPENCV_CORE_DEPS
    opencv_core
)
set(ALCEDO_OPENCV_IMAGE_DEPS
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    opencv_calib3d
    opencv_highgui
)

def_library(ThreadPool SOURCES concurrency/thread_pool.cpp)
if(ALCEDO_METAL_ENABLED)
    target_link_libraries(ThreadPool PUBLIC alcedo::metal_cpp)
endif()

if(ALCEDO_OPENCL_ENABLED)
    set(ALCEDO_OPENCL_SHADER_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    set(OPENCL_RAW_UTILS_CL "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/opencl_shader/raw_utils_opencl.cl")
    set(OPENCL_RAW_TO_LINEAR_REF_CL "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/opencl_shader/to_linear_ref.cl")
    set(OPENCL_DEBAYER_RCD_CL "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/opencl_shader/debayer_rcd.cl")
    set(OPENCL_HIGHLIGHT_RECONSTRUCT_CL "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/opencl_shader/highlight_reconstruct.cl")
    set(OPENCL_CVT_REF_SPACE_CL "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/opencl_shader/cvt_ref_space.cl")
    set(OPENCL_PRNG_CL "${CMAKE_CURRENT_SOURCE_DIR}/opencl/opencl_shader/prng.cl")
    set(OPENCL_EDIT_PIPELINE_FUSED_PARAMS_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/fused_params.cl")
    set(OPENCL_EDIT_PIPELINE_FUSED_PARAMS_VALIDATION_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/fused_params_validation.cl")
    set(OPENCL_EDIT_PIPELINE_FUSED_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/edit_pipeline_fused.cl")
    set(OPENCL_EDIT_PIPELINE_DETAIL_COMMON_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/detail_common.cl")
    set(OPENCL_EDIT_PIPELINE_FILM_GRAIN_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/film_grain.cl")
    set(OPENCL_EDIT_PIPELINE_HALATION_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/halation.cl")
    set(OPENCL_EDIT_PIPELINE_DETAIL_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/edit_pipeline_detail.cl")
    set(OPENCL_EDIT_PIPELINE_COMMON_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/common.cl")
    set(OPENCL_EDIT_PIPELINE_BASIC_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/basic.cl")
    set(OPENCL_EDIT_PIPELINE_TONE_MAPPING_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/tone_mapping.cl")
    set(OPENCL_EDIT_PIPELINE_COLOR_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/color.cl")
    set(OPENCL_EDIT_PIPELINE_CST_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/opencl_shader/cst.cl")
    set(OPENCL_XTRANS_INTERPOLATE_CL "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/opencl_shader/xtrans_interpolate.cl")
    set(OPENCL_DEMOSAICNET_CONV_CL "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/opencl_shader/demosaicnet_conv.cl")
    set(OPENCL_DEMOSAICNET_STRUCTURAL_CL "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/opencl_shader/demosaicnet_structural.cl")
    set(OPENCL_GEOMETRY_UTILS_CL "${CMAKE_CURRENT_SOURCE_DIR}/opencl/opencl_shader/geometry_utils.cl")
    set(OPENCL_EDIT_GEOMETRY_LENS_CALIB_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/geometry/opencl_shader/lens_calib.cl")
    set(OPENCL_SCOPE_ANALYZER_CL "${CMAKE_CURRENT_SOURCE_DIR}/edit/scope/opencl_shader/scope_analyzer.cl")
    def_library(OpenClContext
        SOURCES
            opencl/opencl_context.cpp
            opencl/opencl_api_counters.cpp
        PUBLIC_DEPS OpenCL::OpenCL
    )
    def_library(OpenClProgramLibrary
        SOURCES
            opencl/opencl_backend_program_registry.cpp
            opencl/opencl_program_library.cpp
            opencl/opencl_runtime.cpp
            decoders/processor/operators/gpu/opencl_raw_programs.cpp
            decoders/processor/operators/gpu/opencl_demosaicnet_programs.cpp
            edit/pipeline/opencl_pipeline_programs.cpp
            edit/scope/opencl_scope_programs.cpp
            opencl/opencl_geometry_programs.cpp
        PUBLIC_DEPS
            OpenClContext
            OpenCL::OpenCL
        PRIVATE_DEPS
            StrConv
    )
    target_compile_definitions(OpenClProgramLibrary PRIVATE
        ALCEDO_OPENCL_RAW_UTILS_CL="${OPENCL_RAW_UTILS_CL}"
        ALCEDO_OPENCL_RAW_TO_LINEAR_REF_CL="${OPENCL_RAW_TO_LINEAR_REF_CL}"
        ALCEDO_OPENCL_DEBAYER_RCD_CL="${OPENCL_DEBAYER_RCD_CL}"
        ALCEDO_OPENCL_HIGHLIGHT_RECONSTRUCT_CL="${OPENCL_HIGHLIGHT_RECONSTRUCT_CL}"
        ALCEDO_OPENCL_CVT_REF_SPACE_CL="${OPENCL_CVT_REF_SPACE_CL}"
        ALCEDO_OPENCL_PRNG_CL="${OPENCL_PRNG_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_FUSED_PARAMS_CL="${OPENCL_EDIT_PIPELINE_FUSED_PARAMS_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_FUSED_PARAMS_VALIDATION_CL="${OPENCL_EDIT_PIPELINE_FUSED_PARAMS_VALIDATION_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_FUSED_CL="${OPENCL_EDIT_PIPELINE_FUSED_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_COMMON_CL="${OPENCL_EDIT_PIPELINE_DETAIL_COMMON_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_FILM_GRAIN_CL="${OPENCL_EDIT_PIPELINE_FILM_GRAIN_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_HALATION_CL="${OPENCL_EDIT_PIPELINE_HALATION_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_CL="${OPENCL_EDIT_PIPELINE_DETAIL_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_COMMON_CL="${OPENCL_EDIT_PIPELINE_COMMON_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_BASIC_CL="${OPENCL_EDIT_PIPELINE_BASIC_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_TONE_MAPPING_CL="${OPENCL_EDIT_PIPELINE_TONE_MAPPING_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_COLOR_CL="${OPENCL_EDIT_PIPELINE_COLOR_CL}"
        ALCEDO_OPENCL_EDIT_PIPELINE_CST_CL="${OPENCL_EDIT_PIPELINE_CST_CL}"
        ALCEDO_OPENCL_XTRANS_INTERPOLATE_CL="${OPENCL_XTRANS_INTERPOLATE_CL}"
        ALCEDO_OPENCL_DEMOSAICNET_CONV_CL="${OPENCL_DEMOSAICNET_CONV_CL}"
        ALCEDO_OPENCL_DEMOSAICNET_STRUCTURAL_CL="${OPENCL_DEMOSAICNET_STRUCTURAL_CL}"
        ALCEDO_OPENCL_GEOMETRY_UTILS_CL="${OPENCL_GEOMETRY_UTILS_CL}"
        ALCEDO_OPENCL_EDIT_GEOMETRY_LENS_CALIB_CL="${OPENCL_EDIT_GEOMETRY_LENS_CALIB_CL}"
        ALCEDO_OPENCL_SCOPE_ANALYZER_CL="${OPENCL_SCOPE_ANALYZER_CL}"
        ALCEDO_OPENCL_SHADER_SOURCE_ROOT="${ALCEDO_OPENCL_SHADER_SOURCE_ROOT}"
    )
endif()

def_library(TimeProvider SOURCES utils/clock/time_provider.cpp)

def_library(AppDiagnostics
    SOURCES
        utils/diagnostics/app_logging.cpp
        include/utils/diagnostics/app_logging.hpp
    PUBLIC_DEPS Qt6::Core
)

def_library(CudaDriverRequirements
    SOURCES utils/cuda/cuda_driver_requirements.cpp
)

# Backend-neutral host safetensors parser (no CUDA / OpenCL).
def_library(NnSafetensors
    SOURCES nn/safetensors.cpp
    PRIVATE_DEPS JSON
)

if(ALCEDO_CUDA_ENABLED)
    def_library(CudaUtils
        SOURCES
            cuda/prng.cu
            cuda/nn/relu.cu
            cuda/nn/mul.cu
            cuda/nn/concat.cu
            cuda/nn/slice.cu
            cuda/nn/crop.cu
            cuda/nn/layout.cu
            cuda/nn/conv2d.cu
            cuda/nn/cutlass_conv2d.cu
            cuda/nn/demosaicnet_nhwc.cu
            cuda/nn/conv_transpose2d.cu
            cuda/nn/fused_post_output.cu
            cuda/nn/safetensors.cpp
        PUBLIC_DEPS
            CUDA::cudart
            NnSafetensors
            ${ALCEDO_OPENCV_CORE_DEPS}
    )
    set_target_properties(CudaUtils PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(CudaUtils PUBLIC HAVE_CUDA)
    target_include_directories(CudaUtils PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cutlass/include"
    )
    set_source_files_properties(cuda/nn/cutlass_conv2d.cu PROPERTIES
        COMPILE_OPTIONS "-Xcompiler=/Zc:__cplusplus;--expt-relaxed-constexpr"
    )
endif()

def_library(UiLocalization
    SOURCES
        include/ui/alcedo_main/i18n.hpp
        include/ui/alcedo_main/language_manager.hpp
        ui/alcedo_main/i18n.cpp
        ui/alcedo_main/language_manager.cpp
    PUBLIC_DEPS
        Qt6::Core
        Qt6::Qml
)

def_library(StrConv 
    SOURCES utils/string/converter.cpp 
    PUBLIC_DEPS utfcpp
)

if(ALCEDO_METAL_ENABLED)
    if(POLICY CMP0116)
        cmake_policy(SET CMP0116 NEW)
    endif()

    set(METAL_UTILS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_utils/metal_convert.metal")
    set(METAL_UTILS_AIR "${CMAKE_CURRENT_BINARY_DIR}/metal/metal_utils/metal_convert.air")
    set(METAL_UTILS_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/metal/metal_utils/metal_convert.metallib")
    set(METAL_GEOMETRY_UTILS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_utils/geometry_utils.metal")
    set(METAL_GEOMETRY_UTILS_AIR "${CMAKE_CURRENT_BINARY_DIR}/metal/metal_utils/geometry_utils.air")
    set(METAL_GEOMETRY_UTILS_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/metal/metal_utils/geometry_utils.metallib")
    set(METAL_LENS_CALIB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/geometry/metal_shader/lens_calib.metal")
    set(METAL_LENS_CALIB_AIR "${CMAKE_CURRENT_BINARY_DIR}/edit/operators/geometry/metal_shader/lens_calib.air")
    set(METAL_LENS_CALIB_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/edit/operators/geometry/metal_shader/lens_calib.metallib")
    set(METAL_FUSED_PIPELINE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/edit/pipeline/metal_shader/fused_pipeline.metal")
    set(METAL_FUSED_PIPELINE_AIR "${CMAKE_CURRENT_BINARY_DIR}/edit/pipeline/metal_shader/fused_pipeline.air")
    set(METAL_FUSED_PIPELINE_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/edit/pipeline/metal_shader/fused_pipeline.metallib")
    set(METAL_SCOPE_ANALYZER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/edit/scope/metal_shader/scope_analyzer.metal")
    set(METAL_SCOPE_ANALYZER_AIR "${CMAKE_CURRENT_BINARY_DIR}/edit/scope/metal_shader/scope_analyzer.air")
    set(METAL_SCOPE_ANALYZER_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/edit/scope/metal_shader/scope_analyzer.metallib")
    set(METAL_FUSED_PIPELINE_DEPS
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/common.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/basic.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/tone_mapping.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/color.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/cst.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/detail.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/prng.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/film_grain.metal"
        "${CMAKE_CURRENT_SOURCE_DIR}/edit/operators/GPU_kernels/metal_shader/halation.metal"
    )
    set(METAL_TO_LINEAR_REF_SRC "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/metal_shader/to_linear_ref.metal")
    set(METAL_TO_LINEAR_REF_AIR "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/to_linear_ref.air")
    set(METAL_TO_LINEAR_REF_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/to_linear_ref.metallib")
    set(METAL_DEBAYER_RCD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/metal_shader/debayer_rcd.metal")
    set(METAL_DEBAYER_RCD_AIR "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/debayer_rcd.air")
    set(METAL_DEBAYER_RCD_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/debayer_rcd.metallib")
    set(METAL_CVT_REF_SPACE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/metal_shader/cvt_ref_space.metal")
    set(METAL_CVT_REF_SPACE_AIR "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/cvt_ref_space.air")
    set(METAL_CVT_REF_SPACE_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/cvt_ref_space.metallib")
    set(METAL_HIGHLIGHT_RECONSTRUCT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/metal_shader/highlight_reconstruct.metal")
    set(METAL_HIGHLIGHT_RECONSTRUCT_AIR "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/highlight_reconstruct.air")
    set(METAL_HIGHLIGHT_RECONSTRUCT_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/highlight_reconstruct.metallib")
    set(METAL_XTRANS_INTERPOLATE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/metal_shader/xtrans_interpolate.metal")
    set(METAL_XTRANS_INTERPOLATE_AIR "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/xtrans_interpolate.air")
    set(METAL_XTRANS_INTERPOLATE_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/xtrans_interpolate.metallib")
    set(METAL_DEMOSAICNET_IO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/decoders/processor/operators/gpu/metal_shader/demosaicnet_io.metal")
    set(METAL_DEMOSAICNET_IO_AIR "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/demosaicnet_io.air")
    set(METAL_DEMOSAICNET_IO_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/decoders/processor/operators/gpu/metal_shader/demosaicnet_io.metallib")
    find_program(ALCEDO_XCRUN_EXECUTABLE xcrun REQUIRED)
    execute_process(
        COMMAND ${ALCEDO_XCRUN_EXECUTABLE} --sdk macosx --find metal
        RESULT_VARIABLE ALCEDO_METAL_COMPILER_STATUS
        OUTPUT_VARIABLE ALCEDO_METAL_EXECUTABLE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE ALCEDO_METAL_COMPILER_ERROR
    )
    if(NOT ALCEDO_METAL_COMPILER_STATUS EQUAL 0)
        string(STRIP "${ALCEDO_METAL_COMPILER_ERROR}" ALCEDO_METAL_COMPILER_ERROR)
        message(FATAL_ERROR "Metal compiler is not available via xcrun. ${ALCEDO_METAL_COMPILER_ERROR}")
    endif()
    execute_process(
        COMMAND ${ALCEDO_XCRUN_EXECUTABLE} --sdk macosx --find metallib
        RESULT_VARIABLE ALCEDO_METALLIB_STATUS
        OUTPUT_VARIABLE ALCEDO_METALLIB_EXECUTABLE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE ALCEDO_METALLIB_ERROR
    )
    if(NOT ALCEDO_METALLIB_STATUS EQUAL 0)
        string(STRIP "${ALCEDO_METALLIB_ERROR}" ALCEDO_METALLIB_ERROR)
        message(FATAL_ERROR "Metal library linker is not available via xcrun. ${ALCEDO_METALLIB_ERROR}")
    endif()

    set(ALCEDO_METAL_COMMON_COMPILE_FLAGS
        -MMD
        -fno-color-diagnostics
    )
    if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
        set(ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET_DEFAULT "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET_DEFAULT "13.3")
    endif()
    if(NOT DEFINED ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET OR
       "${ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET}" STREQUAL "" OR
       "${ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET}" STREQUAL "13.0")
        set(ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET
            "${ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET_DEFAULT}"
            CACHE STRING
            "macOS deployment target used when compiling Metal shaders."
            FORCE
        )
    else()
        set(ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET
            "${ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET}"
            CACHE STRING
            "macOS deployment target used when compiling Metal shaders."
        )
    endif()
    set(ALCEDO_METAL_PLATFORM_COMPILE_FLAGS)
    if(NOT "${ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET}" STREQUAL "")
        list(APPEND ALCEDO_METAL_PLATFORM_COMPILE_FLAGS
            "-mmacosx-version-min=${ALCEDO_METAL_SHADER_DEPLOYMENT_TARGET}"
        )
    endif()
    set(ALCEDO_METAL_MODULE_CACHE_DIR "${CMAKE_CURRENT_BINARY_DIR}/metal/module-cache")
    set(ALCEDO_METAL_CONFIG_COMPILE_FLAGS
        $<$<CONFIG:Debug>:-gline-tables-only>
        $<$<CONFIG:RelWithDebInfo>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-gline-tables-only>
        $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-O3>
    )

    function(alcedo_add_metal_library shader_target source_file air_file metallib_file compile_comment
             link_comment)
        set(options)
        set(oneValueArgs)
        set(multiValueArgs DEPENDS)
        cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

        get_filename_component(_air_dir "${air_file}" DIRECTORY)
        set(_depfile "${air_file}.d")

        add_custom_command(
            OUTPUT "${air_file}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_air_dir}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_METAL_MODULE_CACHE_DIR}"
            COMMAND ${ALCEDO_METAL_EXECUTABLE}
                    ${ALCEDO_METAL_COMMON_COMPILE_FLAGS}
                    ${ALCEDO_METAL_PLATFORM_COMPILE_FLAGS}
                    ${ALCEDO_METAL_CONFIG_COMPILE_FLAGS}
                    -fmodules-cache-path="${ALCEDO_METAL_MODULE_CACHE_DIR}"
                    -MF "${_depfile}"
                    -c "${source_file}"
                    -o "${air_file}"
            DEPENDS "${source_file}" ${ARG_DEPENDS}
            DEPFILE "${_depfile}"
            BYPRODUCTS "${_depfile}"
            COMMENT "${compile_comment}"
            VERBATIM
            COMMAND_EXPAND_LISTS
        )
        add_custom_command(
            OUTPUT "${metallib_file}"
            COMMAND ${ALCEDO_METALLIB_EXECUTABLE} "${air_file}" -o "${metallib_file}"
            DEPENDS "${air_file}"
            COMMENT "${link_comment}"
            VERBATIM
        )
        add_custom_target(${shader_target} DEPENDS "${metallib_file}")
    endfunction()

    alcedo_add_metal_library(
        MetalUtilsShaders
        "${METAL_UTILS_SRC}"
        "${METAL_UTILS_AIR}"
        "${METAL_UTILS_METALLIB}"
        "Compiling Metal utility shaders"
        "Linking Metal utility library"
    )

    alcedo_add_metal_library(
        MetalGeometryUtilsShaders
        "${METAL_GEOMETRY_UTILS_SRC}"
        "${METAL_GEOMETRY_UTILS_AIR}"
        "${METAL_GEOMETRY_UTILS_METALLIB}"
        "Compiling Metal geometry utility shaders"
        "Linking Metal geometry utility library"
    )

    alcedo_add_metal_library(
        LensCalibOpMetalShaders
        "${METAL_LENS_CALIB_SRC}"
        "${METAL_LENS_CALIB_AIR}"
        "${METAL_LENS_CALIB_METALLIB}"
        "Compiling Metal lens calibration shader"
        "Linking Metal lens calibration library"
    )

    alcedo_add_metal_library(
        EditPipelineMetalShaders
        "${METAL_FUSED_PIPELINE_SRC}"
        "${METAL_FUSED_PIPELINE_AIR}"
        "${METAL_FUSED_PIPELINE_METALLIB}"
        "Compiling Metal fused pipeline shader"
        "Linking Metal fused pipeline library"
        DEPENDS ${METAL_FUSED_PIPELINE_DEPS}
    )

    alcedo_add_metal_library(
        ScopeAnalyzerMetalShaders
        "${METAL_SCOPE_ANALYZER_SRC}"
        "${METAL_SCOPE_ANALYZER_AIR}"
        "${METAL_SCOPE_ANALYZER_METALLIB}"
        "Compiling Metal scope analyzer shader"
        "Linking Metal scope analyzer library"
    )

    alcedo_add_metal_library(
        MetalToLinearRefShaders
        "${METAL_TO_LINEAR_REF_SRC}"
        "${METAL_TO_LINEAR_REF_AIR}"
        "${METAL_TO_LINEAR_REF_METALLIB}"
        "Compiling Metal ToLinearRef shader"
        "Linking Metal ToLinearRef library"
    )

    alcedo_add_metal_library(
        MetalDebayerRcdShaders
        "${METAL_DEBAYER_RCD_SRC}"
        "${METAL_DEBAYER_RCD_AIR}"
        "${METAL_DEBAYER_RCD_METALLIB}"
        "Compiling Metal Debayer RCD shader"
        "Linking Metal Debayer RCD library"
    )

    alcedo_add_metal_library(
        MetalHighlightReconstructShaders
        "${METAL_HIGHLIGHT_RECONSTRUCT_SRC}"
        "${METAL_HIGHLIGHT_RECONSTRUCT_AIR}"
        "${METAL_HIGHLIGHT_RECONSTRUCT_METALLIB}"
        "Compiling Metal HighlightReconstruct shader"
        "Linking Metal HighlightReconstruct library"
    )

    alcedo_add_metal_library(
        MetalXTransInterpolateShaders
        "${METAL_XTRANS_INTERPOLATE_SRC}"
        "${METAL_XTRANS_INTERPOLATE_AIR}"
        "${METAL_XTRANS_INTERPOLATE_METALLIB}"
        "Compiling Metal X-Trans interpolate shader"
        "Linking Metal X-Trans interpolate library"
    )

    alcedo_add_metal_library(
        MetalCvtRefSpaceShaders
        "${METAL_CVT_REF_SPACE_SRC}"
        "${METAL_CVT_REF_SPACE_AIR}"
        "${METAL_CVT_REF_SPACE_METALLIB}"
        "Compiling Metal ConvertToRefSpace shader"
        "Linking Metal ConvertToRefSpace library"
    )

    alcedo_add_metal_library(
        MetalDemosaicNetIoShaders
        "${METAL_DEMOSAICNET_IO_SRC}"
        "${METAL_DEMOSAICNET_IO_AIR}"
        "${METAL_DEMOSAICNET_IO_METALLIB}"
        "Compiling Metal DemosaicNet IO shader"
        "Linking Metal DemosaicNet IO library"
    )

    add_custom_target(RawProcessorOpMetalShaders DEPENDS
        "${METAL_TO_LINEAR_REF_METALLIB}"
        "${METAL_DEBAYER_RCD_METALLIB}"
        "${METAL_HIGHLIGHT_RECONSTRUCT_METALLIB}"
        "${METAL_XTRANS_INTERPOLATE_METALLIB}"
        "${METAL_CVT_REF_SPACE_METALLIB}"
        "${METAL_DEMOSAICNET_IO_METALLIB}"
    )
    set(ALCEDO_METAL_RUNTIME_LIBS
        "${METAL_UTILS_METALLIB}"
        "${METAL_GEOMETRY_UTILS_METALLIB}"
        "${METAL_LENS_CALIB_METALLIB}"
        "${METAL_FUSED_PIPELINE_METALLIB}"
        "${METAL_SCOPE_ANALYZER_METALLIB}"
        "${METAL_TO_LINEAR_REF_METALLIB}"
        "${METAL_DEBAYER_RCD_METALLIB}"
        "${METAL_HIGHLIGHT_RECONSTRUCT_METALLIB}"
        "${METAL_XTRANS_INTERPOLATE_METALLIB}"
        "${METAL_CVT_REF_SPACE_METALLIB}"
        "${METAL_DEMOSAICNET_IO_METALLIB}"
    )
    set(ALCEDO_METAL_RUNTIME_LIBS "${ALCEDO_METAL_RUNTIME_LIBS}" CACHE INTERNAL
        "Metal shader libraries required at runtime."
    )

    def_library(MetalContext SOURCES metal/metal_context.cpp)
    target_link_libraries(MetalContext PUBLIC alcedo::metal_cpp)
    def_library(MetalImage SOURCES image/metal_image.cpp PUBLIC_DEPS MetalContext alcedo::metal_cpp ${ALCEDO_OPENCV_CORE_DEPS})
    def_library(MetalUtils
        SOURCES metal/metal_utils/metal_convert_utils.cpp
        PUBLIC_DEPS MetalContext MetalImage alcedo::metal_cpp ${ALCEDO_OPENCV_CORE_DEPS}
    )
    add_dependencies(MetalUtils MetalUtilsShaders)
    target_compile_definitions(MetalUtils PRIVATE ALCEDO_METAL_UTILS_METALLIB_PATH="${METAL_UTILS_METALLIB}")
    def_library(MetalGeometryUtils
        SOURCES metal/metal_utils/geometry_utils.cpp
        PUBLIC_DEPS MetalImage alcedo::metal_cpp ${ALCEDO_OPENCV_CORE_DEPS} libraw::raw_r
    )
    add_dependencies(MetalGeometryUtils MetalGeometryUtilsShaders)
    target_compile_definitions(MetalGeometryUtils PRIVATE ALCEDO_METAL_GEOMETRY_UTILS_METALLIB_PATH="${METAL_GEOMETRY_UTILS_METALLIB}")

    # Fixed Metal MPSGraph DemosaicNet modules and lazy model cache.
    def_library(MetalDemosaicNet
        SOURCES
            decoders/processor/nn/metal_demosaicnet_module.mm
            decoders/processor/nn/metal_demosaicnet_cache.mm
        PUBLIC_DEPS
            MetalContext
            NnSafetensors
            alcedo::metal_cpp
    )
    target_compile_definitions(MetalDemosaicNet PUBLIC HAVE_METAL)
    target_compile_definitions(MetalDemosaicNet PRIVATE
        ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config/models"
        ALCEDO_METAL_DEMOSAICNET_IO_METALLIB_PATH="${METAL_DEMOSAICNET_IO_METALLIB}"
    )
    target_compile_options(MetalDemosaicNet PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    set_source_files_properties(
        decoders/processor/nn/metal_demosaicnet_module.mm
        decoders/processor/nn/metal_demosaicnet_cache.mm
        PROPERTIES
            COMPILE_FLAGS "-fobjc-arc"
            LANGUAGE OBJCXX
    )
    add_dependencies(MetalDemosaicNet MetalDemosaicNetIoShaders)
    target_link_libraries(MetalDemosaicNet PUBLIC
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework MetalPerformanceShadersGraph"
    )

    # Tile IO orchestration + product RAW Neural entry (hard-fail).
    def_library(MetalDemosaicNetEntry
        SOURCES
            decoders/processor/nn/metal_demosaicnet_tiled.mm
            decoders/processor/operators/gpu/metal_demosaicnet.cpp
        PUBLIC_DEPS
            MetalDemosaicNet
            MetalImage
            MetalContext
            alcedo::metal_cpp
            ${ALCEDO_OPENCV_CORE_DEPS}
            libraw::raw_r
    )
    target_compile_definitions(MetalDemosaicNetEntry PUBLIC HAVE_METAL)
    target_compile_definitions(MetalDemosaicNetEntry PRIVATE
        ALCEDO_METAL_DEMOSAICNET_IO_METALLIB_PATH="${METAL_DEMOSAICNET_IO_METALLIB}"
        ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config/models"
    )
    target_compile_options(MetalDemosaicNetEntry PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    set_source_files_properties(
        decoders/processor/nn/metal_demosaicnet_tiled.mm
        PROPERTIES
            COMPILE_FLAGS "-fobjc-arc"
            LANGUAGE OBJCXX
    )
    add_dependencies(MetalDemosaicNetEntry MetalDemosaicNetIoShaders)
    target_link_libraries(MetalDemosaicNetEntry PUBLIC
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework MetalPerformanceShadersGraph"
    )
endif()

# ==============================================================================
# 2. Core Image Handling 
# ==============================================================================

# ImageBuffer (Platform specific deps handling)
set(IMAGE_BUFFER_SRCS image/image_buffer.cpp)
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND IMAGE_BUFFER_SRCS image/opencl_image.cpp)
endif()
add_library(ImageBuffer ${IMAGE_BUFFER_SRCS})
target_include_directories(ImageBuffer PUBLIC include)

# Common dependencies
set(IMAGE_BUFFER_DEPS Exiv2 ${ALCEDO_OPENCV_CORE_DEPS} libraw::raw_r TimeProvider JSON StrConv)

# Platform specific dependencies
if (WIN32)
    target_link_libraries(ImageBuffer PUBLIC ${IMAGE_BUFFER_DEPS} xxHash::xxhash)
elseif(ALCEDO_METAL_ENABLED)
    target_link_libraries(ImageBuffer PUBLIC ${IMAGE_BUFFER_DEPS} xxHash MetalImage MetalUtils)
else()
    target_link_libraries(ImageBuffer PUBLIC ${IMAGE_BUFFER_DEPS} xxHash)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_link_libraries(ImageBuffer PUBLIC OpenClContext)
endif()

if(ALCEDO_OPENCL_ENABLED)
    def_library(OpenClGeometryUtils
        SOURCES opencl/opencl_geometry_utils.cpp
        PUBLIC_DEPS ImageBuffer OpenClProgramLibrary ${ALCEDO_OPENCV_CORE_DEPS}
    )
    target_compile_definitions(OpenClGeometryUtils PUBLIC HAVE_OPENCL)

    # Reusable OpenCL NN primitives (device buffer, workspace, NHWC4 conv).
    # No Bayer/X-Trans topology, model paths, or RAW routing — product modules only.
    def_library(OpenClNn
        SOURCES
            opencl/nn/device_buffer.cpp
            opencl/nn/activation_slots.cpp
            opencl/nn/workspace.cpp
            opencl/nn/convolution.cpp
            opencl/nn/demosaicnet_stage_profiler.cpp
        PUBLIC_DEPS
            OpenClContext
            OpenClProgramLibrary
            OpenCL::OpenCL
    )
    target_compile_definitions(OpenClNn PUBLIC HAVE_OPENCL)

    # Fixed OpenCL DemosaicNet modules and lazy model cache.
    def_library(OpenClDemosaicNet
        SOURCES
            decoders/processor/nn/opencl_demosaicnet_module.cpp
            decoders/processor/nn/opencl_demosaicnet_cache.cpp
            decoders/processor/nn/opencl_demosaicnet_tiled.cpp
        PUBLIC_DEPS
            OpenClNn
            OpenClProgramLibrary
            NnSafetensors
            OpenCL::OpenCL
            ${ALCEDO_OPENCV_CORE_DEPS}
    )
    target_compile_definitions(OpenClDemosaicNet PUBLIC HAVE_OPENCL)
    target_compile_definitions(OpenClDemosaicNet PRIVATE
        ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config/models"
    )

    # Product-path OpenCL Neural demosaic entry. Soft-fail API for
    # RawProcessor OpenCL branch; not part of the generic OpenClDemosaicNet module layer.
    def_library(OpenClDemosaicNetEntry
        SOURCES
            decoders/processor/operators/gpu/opencl_demosaicnet.cpp
        PUBLIC_DEPS
            OpenClDemosaicNet
            ImageBuffer
            OpenClProgramLibrary
            OpenCL::OpenCL
            ${ALCEDO_OPENCV_CORE_DEPS}
    )
    target_compile_definitions(OpenClDemosaicNetEntry PUBLIC HAVE_OPENCL)
endif()

# Image Base
def_library(Image
    SOURCES image/image.cpp 
    image/metadata_extractor.cpp
    PUBLIC_DEPS ImageBuffer
)
target_link_libraries(Image PRIVATE OpenImageIO::OpenImageIO)

# Image Pool
def_library(ImagePool
    SOURCES storage/image_pool/image_pool_manager.cpp
    PRIVATE_DEPS Image
)

# ==============================================================================
# 3. Processors & Operators
# ==============================================================================

# --- RawProcessorOp Configuration (CPU/GPU Split) ---
set(RAW_OP_CPU_SRCS
    decoders/processor/operators/cpu/color_space_conv.cpp
    decoders/processor/operators/cpu/debayer_ahd.cpp
    decoders/processor/operators/cpu/debayer_amaze.cpp
    decoders/processor/operators/cpu/debayer_rcd.cpp
    decoders/processor/operators/cpu/highlight_reconstruct.cpp
    decoders/processor/operators/cpu/white_balance.cpp
    decoders/processor/operators/cpu/raw_proc_utils.cpp
)

set(RAW_OP_GPU_SRCS)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        decoders/processor/operators/gpu/cuda_color_space_conv.cu
        decoders/processor/operators/gpu/cuda_debayer_ahd.cu
        decoders/processor/operators/gpu/cuda_debayer_rcd.cu
        decoders/processor/operators/gpu/cuda_dng_warp.cu
        decoders/processor/operators/gpu/cuda_downsample.cu
        decoders/processor/operators/gpu/cuda_highlight_reconstruct.cu
        decoders/processor/operators/gpu/cuda_image_ops.cu
        decoders/processor/operators/gpu/cuda_rotate.cu
        decoders/processor/operators/gpu/cuda_white_balance.cu
        decoders/processor/operators/gpu/cuda_xtrans_interpolate.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        decoders/processor/operators/gpu/metal_cvt_ref_space.cpp
        decoders/processor/operators/gpu/metal_debayer_rcd.cpp
        decoders/processor/operators/gpu/metal_highlight_reconstruct.cpp
        decoders/processor/operators/gpu/metal_to_linear_ref.cpp
        decoders/processor/operators/gpu/metal_xtrans_interpolate.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        decoders/processor/operators/gpu/opencl_to_linear_ref.cpp
        decoders/processor/operators/gpu/opencl_debayer_rcd.cpp
        decoders/processor/operators/gpu/opencl_xtrans_interpolate.cpp
        decoders/processor/operators/gpu/opencl_highlight_reconstruct.cpp
        decoders/processor/operators/gpu/opencl_cvt_ref_space.cpp
    )
endif()

# RawProcessorOp Target
def_library(RawProcessorOp
    SOURCES ${RAW_OP_CPU_SRCS} ${RAW_OP_GPU_SRCS}
    PUBLIC_DEPS ImageBuffer libraw::raw_r Exiv2 hwy::hwy Eigen3::Eigen
)
target_link_libraries(RawProcessorOp PRIVATE OpenMP::OpenMP_CXX)

if(ALCEDO_CUDA_ENABLED)
    set_target_properties(RawProcessorOp PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    set_target_properties(RawProcessorOp PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    target_compile_definitions(RawProcessorOp PUBLIC HAVE_CUDA)
endif()
if(ALCEDO_METAL_ENABLED)
    add_dependencies(RawProcessorOp RawProcessorOpMetalShaders)
    target_compile_definitions(RawProcessorOp PRIVATE
        ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH="${METAL_TO_LINEAR_REF_METALLIB}"
        ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH="${METAL_DEBAYER_RCD_METALLIB}"
        ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH="${METAL_HIGHLIGHT_RECONSTRUCT_METALLIB}"
        ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH="${METAL_XTRANS_INTERPOLATE_METALLIB}"
        ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH="${METAL_CVT_REF_SPACE_METALLIB}"
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_link_libraries(RawProcessorOp PUBLIC OpenClProgramLibrary)
    target_compile_definitions(RawProcessorOp PUBLIC HAVE_OPENCL)
endif()

# --- Edit Operators ---
set(OPERATORS_SRCS
    edit/operators/operator_factory.cpp
    edit/operators/basic/exposure_op.cpp
    edit/operators/basic/exposure_op_vec.cpp
    edit/operators/basic/contrast_op.cpp
    edit/operators/basic/black_op.cpp
    edit/operators/basic/color_temp_op.cpp
    edit/operators/basic/highlight_op.cpp
    edit/operators/basic/shadow_op.cpp
    edit/operators/basic/white_op.cpp
    edit/operators/color/tint_op.cpp
    edit/operators/color/conversion/Oklab_cvt.cpp
    edit/operators/color/saturation_op.cpp
    edit/operators/color/vibrance_op.cpp
    edit/operators/color/HLS_op.cpp
    edit/operators/detail/clarity_op.cpp
    edit/operators/detail/sharpen_op.cpp
    edit/operators/wheel/color_wheel_op.cpp
    edit/operators/curve/curve_op.cpp
    edit/operators/cst/cst_op.cpp
    edit/operators/cst/lmt_op.cpp
    edit/operators/cst/aces_odt_cpu.cpp
    edit/operators/cst/open_drt_cpu.cpp
    edit/operators/cst/odt_op.cpp
    edit/operators/cst/film_grain_op.cpp
    edit/operators/cst/halation_op.cpp
    edit/operators/raw/raw_decode_op.cpp
    edit/operators/geometry/resize_op.cpp
    edit/operators/geometry/crop_rotate_op.cpp
    edit/operators/geometry/lens_calib_op.cpp
    edit/operators/operator_registeration.cpp
)

if(ALCEDO_CUDA_ENABLED)
    list(APPEND OPERATORS_SRCS
        edit/operators/geometry/cuda_geometry_ops.cu
        edit/operators/geometry/cuda_lens_calib_ops.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND OPERATORS_SRCS
        edit/operators/basic/highlight_shadow_local_tone_metal.cpp
        edit/operators/geometry/metal_lens_calib.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND OPERATORS_SRCS
        edit/operators/basic/highlight_shadow_local_tone_opencl.cpp
        edit/operators/geometry/opencl_lens_calib_ops.cpp
    )
endif()

def_library(Operators
    SOURCES ${OPERATORS_SRCS}
    PUBLIC_DEPS ImageBuffer JSON OpenColorIO::OpenColorIO hwy::hwy lensfun::lensfun
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS} RawProcessor
)
if(ALCEDO_CUDA_ENABLED)
    set_target_properties(Operators PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)
endif()
if(ALCEDO_METAL_ENABLED)
    add_dependencies(Operators LensCalibOpMetalShaders EditPipelineMetalShaders)
    target_link_libraries(Operators PRIVATE MetalGeometryUtils MetalImage MetalContext)
    target_compile_definitions(Operators PRIVATE
        ALCEDO_METAL_LENS_CALIB_METALLIB_PATH="${METAL_LENS_CALIB_METALLIB}"
        ALCEDO_METAL_FUSED_PIPELINE_METALLIB_PATH="${METAL_FUSED_PIPELINE_METALLIB}"
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(Operators PUBLIC HAVE_OPENCL)
    target_link_libraries(Operators PRIVATE OpenClProgramLibrary OpenClGeometryUtils)
endif()
if(DEFINED ALCEDO_LENSFUN_INCLUDE_DIR AND NOT "${ALCEDO_LENSFUN_INCLUDE_DIR}" STREQUAL "")
    target_include_directories(Operators SYSTEM BEFORE PUBLIC "${ALCEDO_LENSFUN_INCLUDE_DIR}")
endif()
if(TARGET puerhlab_lensfun_build)
    add_dependencies(Operators puerhlab_lensfun_build)
endif()

# --- Accelerator Backend Selection ---
def_library(AcceleratorBackend
    SOURCES edit/pipeline/pipeline_accelerator.cpp
    PUBLIC_DEPS Image
    PRIVATE_DEPS ${ALCEDO_OPENCV_CORE_DEPS} CudaDriverRequirements
)
if(ALCEDO_CUDA_ENABLED)
    target_compile_definitions(AcceleratorBackend PUBLIC HAVE_CUDA)
    target_include_directories(AcceleratorBackend PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
endif()
if(ALCEDO_METAL_ENABLED)
    target_compile_definitions(AcceleratorBackend PUBLIC HAVE_METAL)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(AcceleratorBackend PUBLIC HAVE_OPENCL)
    target_link_libraries(AcceleratorBackend PRIVATE OpenClProgramLibrary)
endif()
target_link_libraries(Operators PRIVATE AcceleratorBackend)

# --- Edit Pipeline ---
set(PIPELINE_SRCS
    edit/pipeline/pipeline_cpu.cpp
    edit/pipeline/pipeline_stage.cpp
    edit/pipeline/pipeline_gpu_wrapper.cpp
    include/edit/pipeline/highlight_shadow_local_tone.hpp
    include/edit/pipeline/local_tone_mapping.hpp
    include/edit/pipeline/metal_kernel_dispatch.hpp
    include/edit/pipeline/metal_pipeline_stats.hpp
    include/edit/pipeline/opencl_kernel_dispatch.hpp
    edit/scope/scope_analyzer.cpp
    edit/scope/final_display_frame_tap.cpp
)

if(ALCEDO_CUDA_ENABLED)
    set(GPU_PIPELINE_SRCS
        edit/pipeline/pipeline_gpu_impl.cu
        edit/scope/cuda_scope_analyzer.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND GPU_PIPELINE_SRCS
        edit/pipeline/pipeline_metal_impl.cpp
        edit/scope/metal_scope_analyzer.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND GPU_PIPELINE_SRCS
        edit/pipeline/pipeline_opencl_impl.cpp
        edit/pipeline/pipeline_opencl_param.cpp
        edit/scope/opencl_scope_analyzer.cpp
    )
endif()

def_library(EditPipeline
    SOURCES ${PIPELINE_SRCS} ${GPU_PIPELINE_SRCS}
    PUBLIC_DEPS Operators
    PRIVATE_DEPS ThreadPool opencv_imgproc CudaDriverRequirements AcceleratorBackend
)

if(ALCEDO_CUDA_ENABLED)
    set_target_properties(EditPipeline PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    set_target_properties(EditPipeline PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    target_compile_definitions(EditPipeline PUBLIC HAVE_CUDA)
    target_include_directories(EditPipeline PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(EditPipeline PRIVATE CUDA::cudart)
endif()
if(ALCEDO_METAL_ENABLED)
    add_dependencies(EditPipeline EditPipelineMetalShaders ScopeAnalyzerMetalShaders)
    target_compile_definitions(EditPipeline PUBLIC HAVE_METAL)
    target_compile_definitions(EditPipeline PRIVATE
        ALCEDO_METAL_FUSED_PIPELINE_METALLIB_PATH="${METAL_FUSED_PIPELINE_METALLIB}"
        ALCEDO_METAL_SCOPE_ANALYZER_METALLIB_PATH="${METAL_SCOPE_ANALYZER_METALLIB}"
    )
    target_link_libraries(EditPipeline PRIVATE MetalImage MetalContext)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(EditPipeline PUBLIC HAVE_OPENCL)
    target_link_libraries(EditPipeline PRIVATE OpenClProgramLibrary)
endif()

set(RENDERER_SRCS renderer/pipeline_scheduler.cpp)
def_library(PipelineScheduler
    SOURCES ${RENDERER_SRCS}
    PUBLIC_DEPS EditPipeline ThreadPool ImageLoader)

# --- Raw Processor Wrapper ---
set(RAW_PROCESSOR_BACKEND_SRCS)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS decoders/processor/raw_processor_cuda.cpp)
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS decoders/processor/raw_processor_metal.cpp)
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS decoders/processor/raw_processor_opencl.cpp)
endif()

# Hard-coded DemosaicNet modules + lazy model cache. Owned by the RAW CUDA backend path
# (raw_processor_cuda.cpp / cuda_demosaicnet.cu), NOT by RawProcessorOp (legacy op facade only).
if(ALCEDO_CUDA_ENABLED)
    def_library(DemosaicNet
        SOURCES
            decoders/processor/nn/demosaicnet_bayer.cu
            decoders/processor/nn/demosaicnet_xtrans.cu
            decoders/processor/nn/demosaicnet_cache.cpp
            decoders/processor/nn/demosaicnet_profiler.cpp
        PUBLIC_DEPS
            CudaUtils
    )
    set_target_properties(DemosaicNet PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(DemosaicNet PUBLIC HAVE_CUDA)
    target_compile_definitions(DemosaicNet PRIVATE
        ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config/models"
    )

    # Product-path Neural preprocess (phase-align + gamma) + GpuMat demosaic entry.
    # Stays on CudaDemosaicNetEntry (not generic DemosaicNet modules) so LibRaw CFA types link.
    def_library(CudaDemosaicNetEntry
        SOURCES
            decoders/processor/operators/gpu/cuda_demosaicnet.cu
            decoders/processor/nn/demosaicnet_preprocess.cu
        PUBLIC_DEPS
            DemosaicNet
            libraw::raw_r
    )
    set_target_properties(CudaDemosaicNetEntry PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(CudaDemosaicNetEntry PUBLIC HAVE_CUDA)
endif()

def_library(RawProcessor
    SOURCES decoders/processor/raw_processor.cpp ${RAW_PROCESSOR_BACKEND_SRCS}
    PUBLIC_DEPS Image libraw::raw_r Exiv2 RawProcessorOp
    PRIVATE_DEPS opencv_imgproc AcceleratorBackend
)
if(ALCEDO_CUDA_ENABLED)
    target_link_libraries(RawProcessor PRIVATE CudaDemosaicNetEntry)
endif()
if(ALCEDO_METAL_ENABLED)
    target_link_libraries(RawProcessor PRIVATE MetalDemosaicNetEntry MetalGeometryUtils)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_link_libraries(RawProcessor PRIVATE OpenClGeometryUtils OpenClDemosaicNetEntry)
endif()

# ==============================================================================
# 4. Decoding & IO
# ==============================================================================

set(DECODER_SRCS
    decoders/decoder_scheduler.cpp
    decoders/raw_decoder.cpp
    decoders/thumbnail_decoder.cpp
    decoders/metadata_decoder.cpp
)

def_library(ImageDecoder
    SOURCES ${DECODER_SRCS}
    PUBLIC_DEPS Image ThreadPool libraw::raw_r RawProcessor
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS}
)

def_library(ImageLoader 
    SOURCES io/image/image_loader.cpp
    PUBLIC_DEPS ImageDecoder
)

# ==============================================================================
# 5. Business Logic & State
# ==============================================================================

set(HISTORY_SRCS 
    edit/history/edit_history.cpp 
    edit/history/version.cpp 
    edit/history/edit_transaction.cpp
)

def_library(EditHistory
    SOURCES ${HISTORY_SRCS}
    PUBLIC_DEPS Image TimeProvider JSON EditPipeline
)

set(FILTER_SRCS
    sleeve/sleeve_filter/filter_sql.cpp
    sleeve/sleeve_filter/filter_combo.cpp
)

def_library(SleeveFilter
    SOURCES ${FILTER_SRCS}
    PRIVATE_DEPS Image EditHistory JSON
)

set(ELEMENT_SRCS
    sleeve/sleeve_element/sleeve_element_factory.cpp
    sleeve/sleeve_element/sleeve_element.cpp
    sleeve/sleeve_element/sleeve_file.cpp
    sleeve/sleeve_element/sleeve_folder.cpp
)

def_library(SleeveElement
    SOURCES ${ELEMENT_SRCS}
    PUBLIC_DEPS Image ImageLoader SleeveFilter EditHistory ImagePool
)

# ==============================================================================
# 6. Storage & Database
# ==============================================================================

# ORM Layer
def_library(DuckORM
    SOURCES storage/mapper/duckorm/duckdb_orm.cpp storage/mapper/duckorm/duckdb_types.cpp
    PUBLIC_DEPS DuckDB
)

# Mappers
set(MAPPER_SRCS
    storage/mapper/sleeve/element/file_mapper.cpp
    storage/mapper/sleeve/element/folder_mapper.cpp
    storage/mapper/sleeve/element/element_mapper.cpp
    storage/mapper/sleeve/element/element_id_mapper.cpp
    storage/mapper/sleeve/base/base_mapper.cpp
    storage/mapper/image/image_mapper.cpp
    storage/mapper/sleeve/filter/filter_mapper.cpp
    storage/mapper/sleeve/edit_history/history_mapper.cpp
    storage/mapper/pipeline/pipeline_mapper.cpp
)

def_library(SleeveMapper
    SOURCES ${MAPPER_SRCS}
    PUBLIC_DEPS DuckDB DuckORM Image SleeveElement ImagePool EditPipeline PipelineScheduler
)

# AI domain objects (Phase 5f): standalone image understanding/rating objects, FK=file_id
# (Sleeve element id / inode). Not members of Image/SleeveFile. DB ser/deser lives in the
# duckorm-backed `AiStorageController` (storage layer); this lib is JSON-only at the
# object layer so it can be reused without pulling storage deps.
def_library(Ai
    SOURCES ai/ai_description.cpp ai/ai_rating.cpp
    PUBLIC_DEPS JSON
)

# Services
set(SERVICE_SRCS
    sleeve/storage_service.cpp
    storage/controller/controller_types.cpp
    storage/controller/db_controller.cpp
    storage/controller/image/image_controller.cpp
    storage/controller/semantic/semantic_storage_controller.cpp
    storage/controller/ai/ai_storage_controller.cpp
    storage/controller/sleeve/element_controller.cpp
    storage/service/image/image_service.cpp
    storage/service/sleeve/base/base_service.cpp
    storage/service/sleeve/base/root_service.cpp
    storage/service/sleeve/element/element_service.cpp
    storage/service/sleeve/element/element_id_service.cpp
    storage/service/sleeve/element/file_service.cpp
    storage/service/sleeve/element/folder_service.cpp
    storage/service/sleeve/edit_history/history_service.cpp
    storage/service/pipeline/pipeline_service.cpp
)

def_library(StorageService
    SOURCES ${SERVICE_SRCS}
    PUBLIC_DEPS Ai DuckDB DuckORM SleeveMapper StrConv PipelineScheduler
    PRIVATE_DEPS AppDiagnostics
)

# ==============================================================================
# 7. Filesystem & View Manager
# ==============================================================================

set(FS_SRCS
    sleeve/sleeve_base.cpp
    sleeve/path_resolver.cpp
    sleeve/dentry_cache_manager.cpp
    sleeve/sleeve_filesystem.cpp
)

def_library(SleeveFS
    SOURCES ${FS_SRCS}
    PUBLIC_DEPS SleeveElement StorageService
)

def_library(SleeveManager
    SOURCES sleeve/sleeve_manager.cpp
    PUBLIC_DEPS SleeveFS PipelineScheduler
)

# ==============================================================================
# 8. I/O  
# ==============================================================================

def_library(ExportIccProfileResolver
    SOURCES
        include/io/image/export_color_profile_config.hpp
        include/io/image/export_icc_profile_resolver.hpp
        io/image/export_icc_profile_resolver.cpp
    PUBLIC_DEPS
        opencv_core
)

if(APPLE)
    target_link_libraries(ExportIccProfileResolver PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
endif()

if(PUERHLAB_ULTRAHDR_AVAILABLE)
    def_library(UltraHdrWriter
        SOURCES io/image/ultra_hdr_writer.cpp
        PUBLIC_DEPS OpenImageIO::OpenImageIO ImageBuffer Exiv2 puerhlab::uhdr ExportIccProfileResolver
        PRIVATE_DEPS opencv_core
    )

    if(APPLE)
        target_link_libraries(UltraHdrWriter PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
    endif()
endif()

def_library(ImageWriter
    SOURCES io/image/image_writer.cpp
    PUBLIC_DEPS OpenImageIO::OpenImageIO ImageBuffer ExportIccProfileResolver
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS}
)
if(PUERHLAB_ULTRAHDR_AVAILABLE)
    target_link_libraries(ImageWriter PUBLIC UltraHdrWriter)
    target_compile_definitions(ImageWriter PUBLIC ALCEDO_HAS_ULTRAHDR=1)
endif()

if(APPLE)
    target_link_libraries(ImageWriter PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
endif()

# ==============================================================================
# 9. Services
# ==============================================================================
def_library(FSService
    SOURCES app/sleeve_service.cpp
    PUBLIC_DEPS SleeveFS EditHistory
)

def_library(SleeveFilterService
    SOURCES app/sleeve_filter_service.cpp
            app/search_query_classifier.cpp
    PUBLIC_DEPS SleeveElement StorageService SleeveFilter StrConv
)

def_library(AlbumBrowseService
    SOURCES app/album_browse_service.cpp
    PUBLIC_DEPS FSService SleeveElement SleeveFilterService
)


def_library(ImagePoolService
    SOURCES app/image_pool_service.cpp
    PUBLIC_DEPS ImagePool Image StorageService
)

set(ALCEDO_SEMANTIC_PROTO_DIR "${CMAKE_SOURCE_DIR}/rust/puerh_mind/proto")
set(ALCEDO_SEMANTIC_PROTO "${ALCEDO_SEMANTIC_PROTO_DIR}/semantic.proto")
set(ALCEDO_SEMANTIC_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/semantic")
set(ALCEDO_SEMANTIC_PROTO_SRCS
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}/semantic.pb.cc"
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}/semantic.grpc.pb.cc"
)
set(ALCEDO_SEMANTIC_PROTO_HDRS
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}/semantic.pb.h"
    "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}/semantic.grpc.pb.h"
)

# AI proto path vars are hoisted here (ahead of the SemanticProto custom command and library)
# because semantic.proto now imports ai_common.proto (Phase 4): the SemanticProto protoc
# command DEPENDS the ai_common.proto source, semantic.pb.h includes ai_common.pb.h, and the
# SemanticProto library links AiProto. The AI custom commands and AiProto target still live
# below; only these path vars are defined early.
set(ALCEDO_AI_PROTO_DIR "${ALCEDO_SEMANTIC_PROTO_DIR}")
set(ALCEDO_AI_COMMON_PROTO "${ALCEDO_AI_PROTO_DIR}/ai_common.proto")
set(ALCEDO_AI_RUNTIME_PROTO "${ALCEDO_AI_PROTO_DIR}/ai_runtime.proto")
set(ALCEDO_AI_IMAGE_ANALYSIS_PROTO "${ALCEDO_AI_PROTO_DIR}/image_analysis.proto")
set(ALCEDO_AI_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/ai")

add_custom_command(
    OUTPUT ${ALCEDO_SEMANTIC_PROTO_SRCS} ${ALCEDO_SEMANTIC_PROTO_HDRS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}"
    COMMAND ${PUERHLAB_PROTOC_EXECUTABLE}
        --proto_path=${ALCEDO_SEMANTIC_PROTO_DIR}
        --cpp_out=${ALCEDO_SEMANTIC_PROTO_GEN_DIR}
        --grpc_out=${ALCEDO_SEMANTIC_PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${PUERHLAB_GRPC_CPP_PLUGIN_EXECUTABLE}
        "${ALCEDO_SEMANTIC_PROTO}"
    DEPENDS
        "${ALCEDO_SEMANTIC_PROTO}"
        "${ALCEDO_AI_COMMON_PROTO}"
    VERBATIM
)

# ------------------------------------------------------------------------------
# AI sidecar protobuf generation (package alcedo.ai).
#
# Sibling to SemanticProto. The two new protos live in the same proto directory
# (ALCEDO_SEMANTIC_PROTO_DIR), already on the --proto_path, so ai_runtime.proto's
# `import "ai_common.proto";` resolves at generation time. ai_common.proto has
# no service, so it only gets .pb.{h,cc}; ai_runtime.proto also gets a .grpc.pb
# stub. SemanticProto is left frozen — existing consumers are untouched.
# Phase 1 only requires the generated headers and stub to compile; no C++
# service implementation consumes AiProto yet (Phase 2 wires the runtime).
# ------------------------------------------------------------------------------
set(ALCEDO_AI_PROTO_SRCS
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_common.pb.cc"
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.pb.cc"
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.grpc.pb.cc"
    "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.pb.cc"
    "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.grpc.pb.cc"
)
set(ALCEDO_AI_PROTO_HDRS
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_common.pb.h"
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.pb.h"
    "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.grpc.pb.h"
    "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.pb.h"
    "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.grpc.pb.h"
)

# ai_common.proto: messages + enums only (no service -> no grpc stub).
add_custom_command(
    OUTPUT
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_common.pb.cc"
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_common.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_AI_PROTO_GEN_DIR}"
    COMMAND ${PUERHLAB_PROTOC_EXECUTABLE}
        --proto_path=${ALCEDO_AI_PROTO_DIR}
        --cpp_out=${ALCEDO_AI_PROTO_GEN_DIR}
        "${ALCEDO_AI_COMMON_PROTO}"
    DEPENDS
        "${ALCEDO_AI_COMMON_PROTO}"
    VERBATIM
)

# ai_runtime.proto: imports ai_common.proto; emits .pb + .grpc.pb. DEPENDS the
# ai_common.proto *source* (not a generated artifact) so import resolution and
# ordering are correct under Make/Ninja. protoc generates output only for files
# named on the command line, so this command does not regenerate ai_common.*.
add_custom_command(
    OUTPUT
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.pb.cc"
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.pb.h"
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.grpc.pb.h"
        "${ALCEDO_AI_PROTO_GEN_DIR}/ai_runtime.grpc.pb.cc"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_AI_PROTO_GEN_DIR}"
    COMMAND ${PUERHLAB_PROTOC_EXECUTABLE}
        --proto_path=${ALCEDO_AI_PROTO_DIR}
        --cpp_out=${ALCEDO_AI_PROTO_GEN_DIR}
        --grpc_out=${ALCEDO_AI_PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${PUERHLAB_GRPC_CPP_PLUGIN_EXECUTABLE}
        "${ALCEDO_AI_RUNTIME_PROTO}"
    DEPENDS
        "${ALCEDO_AI_RUNTIME_PROTO}"
        "${ALCEDO_AI_COMMON_PROTO}"
    VERBATIM
)

# image_analysis.proto: imports ai_common.proto; emits .pb + .grpc.pb (Phase 5b).
# Same pattern as ai_runtime.proto: protoc generates output only for files named on
# the command line, so this does not regenerate ai_common.*; DEPENDS the ai_common.proto
# *source* (not a generated artifact) so import resolution + ordering are correct
# under Make/Ninja. Shares the single alcedo.ai package, so AiProto picks it up with
# no new package / cross-package extern_path.
add_custom_command(
    OUTPUT
        "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.pb.cc"
        "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.pb.h"
        "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.grpc.pb.h"
        "${ALCEDO_AI_PROTO_GEN_DIR}/image_analysis.grpc.pb.cc"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_AI_PROTO_GEN_DIR}"
    COMMAND ${PUERHLAB_PROTOC_EXECUTABLE}
        --proto_path=${ALCEDO_AI_PROTO_DIR}
        --cpp_out=${ALCEDO_AI_PROTO_GEN_DIR}
        --grpc_out=${ALCEDO_AI_PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${PUERHLAB_GRPC_CPP_PLUGIN_EXECUTABLE}
        "${ALCEDO_AI_IMAGE_ANALYSIS_PROTO}"
    DEPENDS
        "${ALCEDO_AI_IMAGE_ANALYSIS_PROTO}"
        "${ALCEDO_AI_COMMON_PROTO}"
    VERBATIM
)

add_library(AiProto ${ALCEDO_AI_PROTO_SRCS} ${ALCEDO_AI_PROTO_HDRS})
target_include_directories(AiProto PUBLIC "${ALCEDO_AI_PROTO_GEN_DIR}")
target_link_libraries(AiProto PUBLIC puerhlab::grpc++ puerhlab::protobuf)

# SemanticProto links AiProto and adds the AI generated include dir because semantic.proto
# imports ai_common.proto (Phase 4): semantic.pb.h includes ai_common.pb.h and references
# alcedo::ai types. AiProto is defined just above so the link resolves; the target dependency
# also orders the AI custom commands (which generate ai_common.pb.h) before SemanticProto
# compiles semantic.pb.cc.
add_library(SemanticProto ${ALCEDO_SEMANTIC_PROTO_SRCS} ${ALCEDO_SEMANTIC_PROTO_HDRS})
target_include_directories(SemanticProto PUBLIC "${ALCEDO_SEMANTIC_PROTO_GEN_DIR}"
                                           "${ALCEDO_AI_PROTO_GEN_DIR}")
target_link_libraries(SemanticProto PUBLIC puerhlab::grpc++ puerhlab::protobuf AiProto)

def_library(SidecarClient
    SOURCES
        sidecar_client/client.cpp
        include/sidecar_client/client.hpp
        include/sidecar_client/credential_client.hpp
        include/sidecar_client/image_analysis_client.hpp
        include/sidecar_client/model_manager_client.hpp
        include/sidecar_client/proto_dto.hpp
        include/sidecar_client/runtime_control_client.hpp
        include/sidecar_client/semantic_embedding_client.hpp
        include/sidecar_client/dto/image_analysis.hpp
        include/sidecar_client/dto/model_manager.hpp
        include/sidecar_client/dto/runtime.hpp
        include/sidecar_client/dto/semantic_embedding.hpp
    PUBLIC_DEPS Qt6::Core
    PRIVATE_DEPS SemanticProto AiProto AppDiagnostics
)

def_library(AiSidecarRuntimeService
    SOURCES
        app/ai_sidecar_runtime_service.cpp
        include/app/ai_sidecar_runtime_service.hpp
    PUBLIC_DEPS Qt6::Core Qt6::Network SidecarClient
    PRIVATE_DEPS AppDiagnostics
)

def_library(AiCredentialStore
    SOURCES
        app/ai_credential_store.cpp
        include/app/ai_credential_store.hpp
)
if(WIN32)
    target_link_libraries(AiCredentialStore PRIVATE Advapi32 Crypt32)
elseif(APPLE)
    target_link_libraries(AiCredentialStore PRIVATE "-framework Security" "-framework CoreFoundation")
endif()

# Frontend 1b: multi-profile provider settings controller. Profiles persist in
# ai_providers.json and generated sidecar ProviderConfig JSON; raw API keys stay
# only in AiCredentialStore.
def_library(AiProviderProfile
    SOURCES
        app/ai_provider_profile.cpp
        include/app/ai_provider_profile.hpp
    PUBLIC_DEPS AiCredentialStore Qt6::Core
)
def_library(ModelDownloadService
    SOURCES
        app/model_download_service.cpp
        app/model_asset_catalog.cpp
        include/app/model_download_service.hpp
        include/app/model_asset_catalog.hpp
    PUBLIC_DEPS Qt6::Core Qt6::Network
)

def_library(ProjectService
    SOURCES
        app/project_service.cpp
        app/project_package_backend.cpp
        app/project_package_service.cpp
        ui/alcedo_main/album_backend/path_utils.cpp
    PUBLIC_DEPS FSService StorageService ImagePoolService AlbumBrowseService SleeveFilterService
                AiSidecarRuntimeService ModelDownloadService
                StrConv JSON xxHash stduuid
    PRIVATE_DEPS
        UiLocalization
        AppDiagnostics
        Qt6::Core
        Qt6::Gui
        opencv_core
)

def_library(PipelineMgmtService
    SOURCES app/pipeline_service.cpp
    PUBLIC_DEPS ProjectService EditPipeline
    )

def_library(AdjustmentTransferService
    SOURCES app/adjustment_transfer_service.cpp
    PUBLIC_DEPS PipelineMgmtService EditPipeline
)

def_library(EditHistoryMgmtService
    SOURCES app/history_mgmt_service.cpp
    PUBLIC_DEPS ProjectService EditHistory
)

def_library(ImportService
    SOURCES app/import_service.cpp
    PUBLIC_DEPS SleeveManager ImageWriter ImagePoolService FSService
)

def_library(ThumbnailService
    SOURCES app/thumbnail_service.cpp
    PUBLIC_DEPS EditPipeline ThreadPool StrConv ImagePoolService PipelineMgmtService ThumbnailDiskCacheService EditHistoryMgmtService
)

def_library(SemanticGenerationService
    SOURCES
        app/semantic_generation_service.cpp
        include/app/semantic_generation_service.hpp
        include/storage/controller/semantic/semantic_label_config.hpp
    PUBLIC_DEPS AiSidecarRuntimeService ThumbnailService StorageService opencv_core opencv_imgproc
    PRIVATE_DEPS AppDiagnostics
)

# Phase 5d: host-side encoded-rendition encoder (OIIO primary codec, no OpenCV
# imgcodecs in the primary path) + the host ImageAnalysisService that owns rendition
# selection, credential registration, serialized dispatch, and cancellation.
def_library(ImageAnalysisEncoder
    SOURCES
        app/image_analysis_encoder.cpp
        include/app/image_analysis_encoder.hpp
    PUBLIC_DEPS ThumbnailService ImageBuffer OpenImageIO::OpenImageIO opencv_core opencv_imgproc
)

def_library(ImageAnalysisService
    SOURCES
        app/image_analysis_service.cpp
        include/app/image_analysis_service.hpp
    PUBLIC_DEPS AiSidecarRuntimeService AiCredentialStore ThumbnailService ImageAnalysisEncoder ImageBuffer
    PRIVATE_DEPS AppDiagnostics
)

# Phase 6d: standalone QML-exposed album image-analysis controller. Owns NO
# AlbumBackend coupling (the production IImageAnalysisEnvironment lives in
# album_backend.cpp); unit-testable with fakes. AlbumBackendLib links this.
def_library(ImageAnalysisController
    SOURCES
        ui/alcedo_main/album_backend/image_analysis_controller.cpp
        include/ui/alcedo_main/album_backend/image_analysis_controller.hpp
    PUBLIC_DEPS ImageAnalysisService AiProviderProfile AiCredentialStore BackgroundTaskController UiLocalization Qt6::Core
)

def_library(ThumbnailDiskCacheService
    SOURCES app/thumbnail_disk_cache_service.cpp
    PUBLIC_DEPS ImageBuffer JSON xxHash opencv_imgcodecs OpenImageIO::OpenImageIO
)

def_library(ExportService
    SOURCES app/export_service.cpp
    PUBLIC_DEPS ImageWriter ImageLoader ImagePoolService PipelineMgmtService PipelineScheduler ThreadPool
)

# ==============================================================================
# 9. UI Components
# ==============================================================================
if(ALCEDO_REAL_WIDGET_EDITOR)
    set(EDIT_VIEWER_COMMON_SRCS
        ui/edit_viewer/edit_viewer.cpp
        ui/edit_viewer/edit_viewer_overlay_geometry.cpp
        ui/edit_viewer/viewport_mapper.cpp
        ui/edit_viewer/crop_geometry.cpp
        ui/edit_viewer/view_transform_controller.cpp
        ui/edit_viewer/crop_interaction_controller.cpp
        include/ui/edit_viewer/edit_viewer.hpp
        include/ui/edit_viewer/edit_viewer_overlay_geometry.hpp
        include/ui/edit_viewer/edit_viewer_surface.hpp
        include/ui/edit_viewer/color_manager.hpp
        include/ui/edit_viewer/viewer_state.hpp
        include/ui/edit_viewer/viewport_mapper.hpp
        include/ui/edit_viewer/crop_geometry.hpp
        include/ui/edit_viewer/view_transform_controller.hpp
        include/ui/edit_viewer/crop_interaction_controller.hpp
    )

    set(EDIT_VIEWER_BACKEND_SRCS)
    set(EDIT_VIEWER_PUBLIC_DEPS EditPipeline Qt6::Widgets)

    if(ALCEDO_REAL_OPENGL_EDITOR)
        list(APPEND EDIT_VIEWER_BACKEND_SRCS
            ui/edit_viewer/frame_mailbox.cpp
            ui/edit_viewer/gl_edit_viewer_surface.cpp
            ui/edit_viewer/opengl_viewer_renderer.cpp
            include/ui/edit_viewer/frame_mailbox.hpp
            include/ui/edit_viewer/gl_edit_viewer_surface.hpp
        )
        list(APPEND EDIT_VIEWER_PUBLIC_DEPS OpenGL::GL Qt6::OpenGLWidgets)
    endif()

    if(ALCEDO_RHI_WIDGET_EDITOR)
        list(APPEND EDIT_VIEWER_BACKEND_SRCS
            ui/edit_viewer/color_manager.cpp
            ui/edit_viewer/rhi_edit_viewer_surface.cpp
            include/ui/edit_viewer/rhi_edit_viewer_surface.hpp
        )
        if(APPLE)
            list(REMOVE_ITEM EDIT_VIEWER_BACKEND_SRCS ui/edit_viewer/color_manager.cpp)
            list(APPEND EDIT_VIEWER_BACKEND_SRCS ui/edit_viewer/color_manager.mm)
            set_source_files_properties(ui/edit_viewer/color_manager.mm PROPERTIES LANGUAGE OBJCXX)
        endif()
        list(APPEND EDIT_VIEWER_PUBLIC_DEPS Qt6::GuiPrivate Qt6::ShaderTools)
    endif()

    def_library(EditViewer
        SOURCES ${EDIT_VIEWER_COMMON_SRCS} ${EDIT_VIEWER_BACKEND_SRCS}
        PUBLIC_DEPS ${EDIT_VIEWER_PUBLIC_DEPS}
    )

    if(ALCEDO_CUDA_ENABLED)
        set_target_properties(EditViewer PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
        target_compile_definitions(EditViewer PUBLIC HAVE_CUDA)
    endif()
    if(ALCEDO_OPENCL_ENABLED)
        target_compile_definitions(EditViewer PUBLIC HAVE_OPENCL)
        target_link_libraries(EditViewer PRIVATE OpenClProgramLibrary)
    endif()

    if(ALCEDO_RHI_WIDGET_EDITOR AND APPLE)
        target_link_libraries(EditViewer PRIVATE
            "-framework AppKit"
            "-framework QuartzCore"
            "-framework CoreGraphics"
        )
    endif()
    if(ALCEDO_RHI_WIDGET_EDITOR AND WIN32)
        target_link_libraries(EditViewer PRIVATE d3d11 d3d12 dxgi)
    endif()

    if(ALCEDO_RHI_WIDGET_EDITOR)
        qt_add_shaders(EditViewer "editviewer_rhi_shaders"
            PREFIX
                "/shaders/edit_viewer"
            BASE
                "ui/edit_viewer/shaders"
            FILES
                ui/edit_viewer/shaders/rhi_image.vert
                ui/edit_viewer/shaders/rhi_image.frag
        )
    endif()
endif()

# ── BackgroundTaskController: standalone QObject task registry (Phase 1). Kept
# as its own lightweight lib so ImageAnalysisController and other controllers can
# link it without pulling in the full AlbumBackendLib. AlbumBackendLib links it
# transitively for the album shell and the def_ui_test-based controller tests.
def_library(BackgroundTaskController
    SOURCES
        ui/alcedo_main/album_backend/background_task_controller.cpp
        include/ui/alcedo_main/album_backend/background_task_controller.hpp
    PUBLIC_DEPS Qt6::Core
)

# ── AlbumBackendLib: reusable static library for album_backend ──────────────
add_library(AlbumBackendLib STATIC
    ui/alcedo_main/album_backend/album_backend.cpp
    ui/alcedo_main/album_backend/album_thumbnail_model.cpp
    ui/alcedo_main/album_backend/interaction_policy_controller.cpp
    ui/alcedo_main/album_backend/project_db_write_barrier.cpp
    ui/alcedo_main/album_backend/project_handler.cpp
    ui/alcedo_main/album_backend/thumbnail_manager.cpp
    ui/alcedo_main/album_backend/folder_controller.cpp
    ui/alcedo_main/album_backend/image_controller.cpp
    ui/alcedo_main/album_backend/stats_engine.cpp
    ui/alcedo_main/album_backend/search_controller.cpp
    ui/alcedo_main/album_backend/semantic_generation_controller.cpp
    ui/alcedo_main/album_backend/model_download_controller.cpp
    ui/alcedo_main/album_backend/import_export.cpp
    ui/alcedo_main/album_backend/nikon_he_recovery_controller.cpp
    ui/alcedo_main/album_backend/editor_controller.cpp
    ui/alcedo_main/album_backend/adjustment_transfer_controller.cpp
    include/ui/alcedo_main/album_backend/album_backend.hpp
    include/ui/alcedo_main/album_backend/album_thumbnail_model.hpp
    include/ui/alcedo_main/album_backend/album_types.hpp
    include/ui/alcedo_main/album_backend/interaction_policy_controller.hpp
    include/ui/alcedo_main/album_backend/project_db_write_barrier.hpp
    include/ui/alcedo_main/album_backend/path_utils.hpp
    include/ui/alcedo_main/album_backend/project_handler.hpp
    include/ui/alcedo_main/album_backend/thumbnail_manager.hpp
    include/ui/alcedo_main/album_backend/folder_controller.hpp
    include/ui/alcedo_main/album_backend/image_controller.hpp
    include/ui/alcedo_main/album_backend/stats_engine.hpp
    include/ui/alcedo_main/album_backend/search_controller.hpp
    include/ui/alcedo_main/album_backend/semantic_generation_controller.hpp
    include/ui/alcedo_main/album_backend/model_download_controller.hpp
    include/ui/alcedo_main/album_backend/import_export.hpp
    include/ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp
    include/ui/alcedo_main/album_backend/nikon_he_recovery_types.hpp
    include/ui/alcedo_main/album_backend/editor_controller.hpp
    include/ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp
)

target_include_directories(AlbumBackendLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/alcedo_main
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/alcedo_main/album_backend
)

target_link_libraries(AlbumBackendLib
    PUBLIC
        UiLocalization
        BackgroundTaskController
        Qt6::Quick
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Widgets
        ProjectService
        ImportService
        ExportService
        PipelineMgmtService
        AdjustmentTransferService
        EditHistoryMgmtService
        ThumbnailService
        SemanticGenerationService
        ImageAnalysisService
        ImageAnalysisController
        AiProviderProfile
        AiCredentialStore
        ModelDownloadService
        SleeveFilterService
        FSService
        EditPipeline
        xxHash
        CudaDriverRequirements
)

if(TARGET EditViewer)
    target_link_libraries(AlbumBackendLib PUBLIC EditViewer)
endif()

set(ALCEDO_EDITOR_DIALOG_SRCS
    ui/alcedo_main/editor_dialog/editor_dialog_stub.cpp
    include/ui/alcedo_main/editor_dialog/editor_dialog.hpp
)

if(ALCEDO_REAL_WIDGET_EDITOR)
    set(ALCEDO_EDITOR_DIALOG_SRCS
        ui/alcedo_main/editor_dialog/editor_dialog.cpp
        ui/alcedo_main/editor_dialog/dialog.cpp
        ui/alcedo_main/editor_dialog/dialog_core.cpp
        ui/alcedo_main/editor_dialog/dialog_pipeline.cpp
        ui/alcedo_main/editor_dialog/dialog_ui_state.cpp
        ui/alcedo_main/editor_dialog/state.cpp
        ui/alcedo_main/editor_dialog/controllers/image_controller.cpp
        ui/alcedo_main/editor_dialog/controllers/pipeline_controller.cpp
        ui/alcedo_main/editor_dialog/controllers/history_controller.cpp
        ui/alcedo_main/editor_dialog/controllers/lut_controller.cpp
        ui/alcedo_main/editor_dialog/controllers/render_controller.cpp
        ui/alcedo_main/editor_dialog/frame/editor_frame_manager.cpp
        ui/alcedo_main/editor_dialog/history/editor_history_coordinator.cpp
        ui/alcedo_main/editor_dialog/render/editor_render_coordinator.cpp
        ui/alcedo_main/editor_dialog/session/editor_adjustment_session.cpp
        ui/alcedo_main/editor_dialog/modules/histogram.cpp
        ui/alcedo_main/editor_dialog/modules/color_temp.cpp
        ui/alcedo_main/editor_dialog/modules/color_wheel.cpp
        ui/alcedo_main/editor_dialog/modules/curve.cpp
        ui/alcedo_main/editor_dialog/modules/lens_calib.cpp
        ui/alcedo_main/editor_dialog/modules/lut_catalog.cpp
        ui/alcedo_main/editor_dialog/modules/geometry.cpp
        ui/alcedo_main/editor_dialog/modules/hls.cpp
        ui/alcedo_main/editor_dialog/modules/pipeline_io.cpp
        ui/alcedo_main/editor_dialog/modules/versioning.cpp
        ui/alcedo_main/editor_dialog/scope/scope_panel.cpp
        ui/alcedo_main/editor_dialog/scope/scope_plot_widgets.cpp
        ui/alcedo_main/editor_dialog/scope/scope_renderer.cpp
        ui/alcedo_main/editor_dialog/widgets/display_transform_panel_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/collapsible_section_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/editor_control_panel_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/editor_viewer_pane.cpp
        ui/alcedo_main/editor_dialog/widgets/geometry_panel_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/look_control_panel_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/raw_decode_panel_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/tone_control_panel_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/versioning_panel_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/spinner.cpp
        ui/alcedo_main/editor_dialog/widgets/histogram_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/trackball.cpp
        ui/alcedo_main/editor_dialog/widgets/tone_curve_widget.cpp
        ui/alcedo_main/editor_dialog/widgets/history_cards.cpp
        ui/alcedo_main/editor_dialog/widgets/lut_browser_widget.cpp
        include/ui/alcedo_main/editor_dialog/controllers/lut_controller.hpp
        include/ui/alcedo_main/editor_dialog/modules/lut_catalog.hpp
        include/ui/alcedo_main/editor_dialog/widgets/lut_browser_widget.hpp
        include/ui/alcedo_main/editor_dialog/dialog.hpp
        include/ui/alcedo_main/editor_dialog/dialog_internal.hpp
        include/ui/alcedo_main/editor_dialog/editor_dialog.hpp
        include/ui/alcedo_main/editor_dialog/state.hpp
        include/ui/alcedo_main/editor_dialog/controllers/image_controller.hpp
        include/ui/alcedo_main/editor_dialog/controllers/pipeline_controller.hpp
        include/ui/alcedo_main/editor_dialog/controllers/history_controller.hpp
        include/ui/alcedo_main/editor_dialog/controllers/render_controller.hpp
        include/ui/alcedo_main/editor_dialog/frame/editor_frame_manager.hpp
        include/ui/alcedo_main/editor_dialog/history/editor_history_coordinator.hpp
        include/ui/alcedo_main/editor_dialog/render/editor_render_coordinator.hpp
        include/ui/alcedo_main/editor_dialog/session/adjustment_panel.hpp
        include/ui/alcedo_main/editor_dialog/session/adjustment_patch.hpp
        include/ui/alcedo_main/editor_dialog/session/adjustment_snapshot.hpp
        include/ui/alcedo_main/editor_dialog/session/editor_adjustment_session.hpp
        include/ui/alcedo_main/editor_dialog/state/tone_adjustment_state.hpp
        include/ui/alcedo_main/editor_dialog/state/color_temp_adjustment_state.hpp
        include/ui/alcedo_main/editor_dialog/state/look_adjustment_state.hpp
        include/ui/alcedo_main/editor_dialog/state/display_transform_adjustment_state.hpp
        include/ui/alcedo_main/editor_dialog/state/geometry_adjustment_state.hpp
        include/ui/alcedo_main/editor_dialog/state/raw_decode_adjustment_state.hpp
        include/ui/alcedo_main/editor_dialog/pipeline/adjustment_pipeline_adapter.hpp
        include/ui/alcedo_main/editor_dialog/pipeline/tone_pipeline_adapter.hpp
        include/ui/alcedo_main/editor_dialog/pipeline/color_temp_pipeline_adapter.hpp
        include/ui/alcedo_main/editor_dialog/pipeline/look_pipeline_adapter.hpp
        include/ui/alcedo_main/editor_dialog/pipeline/display_transform_pipeline_adapter.hpp
        include/ui/alcedo_main/editor_dialog/pipeline/geometry_pipeline_adapter.hpp
        include/ui/alcedo_main/editor_dialog/pipeline/raw_pipeline_adapter.hpp
        include/ui/alcedo_main/editor_dialog/modules/histogram.hpp
        include/ui/alcedo_main/editor_dialog/modules/color_temp.hpp
        include/ui/alcedo_main/editor_dialog/modules/color_wheel.hpp
        include/ui/alcedo_main/editor_dialog/modules/curve.hpp
        include/ui/alcedo_main/editor_dialog/modules/lens_calib.hpp
        include/ui/alcedo_main/editor_dialog/modules/geometry.hpp
        include/ui/alcedo_main/editor_dialog/modules/hls.hpp
        include/ui/alcedo_main/editor_dialog/modules/pipeline_io.hpp
        include/ui/alcedo_main/editor_dialog/modules/versioning.hpp
        include/ui/alcedo_main/editor_dialog/scope/scope_panel.hpp
        include/ui/alcedo_main/editor_dialog/scope/scope_plot_widgets.hpp
        include/ui/alcedo_main/editor_dialog/scope/scope_renderer.hpp
        include/ui/alcedo_main/editor_dialog/widgets/display_transform_panel_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/collapsible_section_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/editor_control_panel_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/editor_viewer_pane.hpp
        include/ui/alcedo_main/editor_dialog/widgets/geometry_panel_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/look_control_panel_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/raw_decode_panel_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/tone_control_panel_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/versioning_panel_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/spinner.hpp
        include/ui/alcedo_main/editor_dialog/widgets/histogram_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/trackball.hpp
        include/ui/alcedo_main/editor_dialog/widgets/tone_curve_widget.hpp
        include/ui/alcedo_main/editor_dialog/widgets/history_cards.hpp
    )
endif()

# ── alcedo_main executable ────────────────────────────────────────────────
set(ALCEDO_MAIN_SOURCES
    ui/alcedo_main/main.cpp
    ui/alcedo_main/app_theme.cpp
    ui/alcedo_main/shortcut_registry.cpp
    config/resource.qrc
    include/ui/alcedo_main/app_theme.hpp
    include/ui/alcedo_main/i18n.hpp
    include/ui/alcedo_main/language_manager.hpp
    include/ui/alcedo_main/shortcut_registry.hpp
    include/utils/cuda/cuda_driver_requirements.hpp
    ${ALCEDO_EDITOR_DIALOG_SRCS}
)
if(APPLE AND ALCEDO_MACOS_BUNDLE)
    add_executable(alcedo_main MACOSX_BUNDLE ${ALCEDO_MAIN_SOURCES})
else()
    add_executable(alcedo_main ${ALCEDO_MAIN_SOURCES})
endif()

if(APPLE)
    set(ALCEDO_MAIN_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/config/ICON/alcedo_icon.png")
    set(ALCEDO_MAIN_ICONSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/alcedo_icon.iconset")
    set(ALCEDO_MAIN_ICON_ICNS "${CMAKE_CURRENT_BINARY_DIR}/alcedo_icon.icns")

    find_program(ALCEDO_SIPS_EXECUTABLE sips REQUIRED)
    find_program(ALCEDO_ICONUTIL_EXECUTABLE iconutil REQUIRED)

    add_custom_command(
        OUTPUT "${ALCEDO_MAIN_ICON_ICNS}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${ALCEDO_MAIN_ICONSET_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ALCEDO_MAIN_ICONSET_DIR}"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 16 16 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_16x16.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 32 32 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_16x16@2x.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 32 32 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_32x32.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 64 64 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_32x32@2x.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 128 128 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_128x128.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 256 256 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_128x128@2x.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 256 256 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_256x256.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 512 512 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_256x256@2x.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 512 512 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_512x512.png"
        COMMAND ${ALCEDO_SIPS_EXECUTABLE} -z 1024 1024 "${ALCEDO_MAIN_ICON_PNG}" --out "${ALCEDO_MAIN_ICONSET_DIR}/icon_512x512@2x.png"
        COMMAND ${ALCEDO_ICONUTIL_EXECUTABLE} -c icns "${ALCEDO_MAIN_ICONSET_DIR}" -o "${ALCEDO_MAIN_ICON_ICNS}"
        DEPENDS "${ALCEDO_MAIN_ICON_PNG}"
        COMMENT "Generating macOS app icon from ${ALCEDO_MAIN_ICON_PNG}"
        VERBATIM
    )

    add_custom_target(AlcedoStudioMainIcon DEPENDS "${ALCEDO_MAIN_ICON_ICNS}")
    add_dependencies(alcedo_main AlcedoStudioMainIcon)
    set_source_files_properties("${ALCEDO_MAIN_ICON_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(alcedo_main PRIVATE "${ALCEDO_MAIN_ICON_ICNS}")
    set_target_properties(alcedo_main PROPERTIES MACOSX_BUNDLE_ICON_FILE "alcedo_icon.icns")
endif()

if(WIN32)
    set(ALCEDO_MAIN_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/config/ICON/alcedo_main.rc")
    if(EXISTS "${ALCEDO_MAIN_ICON_RC}")
        target_sources(alcedo_main PRIVATE "${ALCEDO_MAIN_ICON_RC}")
    else()
        message(WARNING "Windows app icon resource not found: ${ALCEDO_MAIN_ICON_RC}")
    endif()
endif()

target_include_directories(alcedo_main
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/alcedo_main
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/alcedo_main/album_backend
)

target_link_libraries(alcedo_main
    PRIVATE
        AlbumBackendLib
        AppDiagnostics
        UiLocalization
        Qt6::Quick
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::QuickDialogs2
        Qt6::Widgets
        Qt6::Svg
        Qt6::QuickEffects
)

if(WIN32 AND ALCEDO_OPENCL_ENABLED)
    target_link_libraries(alcedo_main PRIVATE OpenGL::GL)
endif()

set(ALCEDO_MAIN_QML_MODULE_OPTIONS)
if(NOT ALCEDO_GENERATE_QMLTYPES)
    list(APPEND ALCEDO_MAIN_QML_MODULE_OPTIONS NO_GENERATE_QMLTYPES)
endif()

qt_add_qml_module(alcedo_main
    URI Alcedo.Main
    VERSION 1.0
    ${ALCEDO_MAIN_QML_MODULE_OPTIONS}
    QML_FILES
        ui/alcedo_main/qml/Main.qml
        ui/alcedo_main/qml/ExportQueueState.qml
        ui/alcedo_main/qml/AlbumExportDialog.qml
        ui/alcedo_main/qml/AdjustmentTransferDialog.qml
        ui/alcedo_main/qml/SettingDialog.qml
        ui/alcedo_main/qml/WelcomeDialog.qml
        ui/alcedo_main/qml/NikonHeRecoveryDialog.qml
        ui/alcedo_main/qml/SemanticGenerationDialog.qml
        ui/alcedo_main/qml/AdvancedContentAnalysisDialog.qml
        ui/alcedo_main/qml/DialogActionButton.qml
        ui/alcedo_main/qml/IconButton.qml
        ui/alcedo_main/qml/ActivateModelDialog.qml
        ui/alcedo_main/qml/SemanticGenerationSettingsPanel.qml
        ui/alcedo_main/qml/AiProviderSettingsPanel.qml
        ui/alcedo_main/qml/SeveritySegmentedSlider.qml
        ui/alcedo_main/qml/util/SearchComboBox.qml
        ui/alcedo_main/qml/AboutPage.qml
        ui/alcedo_main/qml/ImportProgressRing.qml
        ui/alcedo_main/qml/InspectorPanel.qml
        ui/alcedo_main/qml/AlbumInspectorPanel.qml
        ui/alcedo_main/qml/ImageInspectorPanel.qml
        ui/alcedo_main/qml/CollectionsPanel.qml
        ui/alcedo_main/qml/GlobalSearchDialog.qml
        ui/alcedo_main/qml/StatsCard.qml
        ui/alcedo_main/qml/StarRatingFilter.qml
        ui/alcedo_main/qml/ImageContextMenu.qml
        ui/alcedo_main/qml/ThumbnailGridView.qml
        ui/alcedo_main/qml/ThumbnailListView.qml
        ui/alcedo_main/qml/BackgroundTaskBar.qml
        ui/alcedo_main/qml/BackgroundTaskPopover.qml
    RESOURCES
        ui/alcedo_main/qml/util/qmldir
)
set_target_properties(alcedo_main PROPERTIES
    QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode"
)
if(APPLE)
    set_target_properties(alcedo_main PROPERTIES
        MACOSX_BUNDLE ${ALCEDO_MACOS_BUNDLE}
        MACOSX_BUNDLE_BUNDLE_NAME "${ALCEDO_MACOS_BUNDLE_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${ALCEDO_MACOS_BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
endif()

set(ALCEDO_TRANSLATION_TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/alcedo_main/i18n/alcedo_main_en.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/alcedo_main/i18n/alcedo_main_zh_CN.ts
)

qt_add_translations(TARGETS alcedo_main
    SOURCE_TARGETS
        alcedo_main
        AlbumBackendLib
        UiLocalization
    TS_FILES
        ${ALCEDO_TRANSLATION_TS_FILES}
    RESOURCE_PREFIX
        "/i18n"
    QM_OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}"
)

if(COMMAND qt_finalize_target)
    qt_finalize_target(alcedo_main)
elseif(COMMAND qt_finalize_executable)
    qt_finalize_executable(alcedo_main)
endif()

target_compile_definitions(alcedo_main PRIVATE
    ALCEDO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ALCEDO_APP_VERSION="${PROJECT_VERSION}"
)

if(DEFINED ALCEDO_LENSFUN_LIBRARY_DIR AND NOT "${ALCEDO_LENSFUN_LIBRARY_DIR}" STREQUAL "")
    set_property(TARGET alcedo_main APPEND PROPERTY BUILD_RPATH "${ALCEDO_LENSFUN_LIBRARY_DIR}")
endif()

set(ALCEDO_MIND_SOURCE_DIR "${CMAKE_SOURCE_DIR}/rust/puerh_mind"
    CACHE INTERNAL "Alcedo semantic sidecar source directory.")
set(ALCEDO_MIND_PROMPTS_DIR "${ALCEDO_MIND_SOURCE_DIR}/configs/prompts"
    CACHE INTERNAL "Alcedo semantic sidecar prompt profile config directory.")
set(ALCEDO_MIND_BUILD_PROFILE "release"
    CACHE INTERNAL "Alcedo semantic sidecar cargo profile.")
set(ALCEDO_MIND_TARGET_DIR "${ALCEDO_MIND_SOURCE_DIR}/target/${ALCEDO_MIND_BUILD_PROFILE}"
    CACHE INTERNAL "Alcedo semantic sidecar cargo target directory.")
option(ALCEDO_BUILD_SEMANTIC_SIDECAR
    "Build and copy the Rust semantic runtime sidecar next to alcedo_main."
    ON)

if(WIN32)
    set(ALCEDO_MIND_EXE_NAME "alcedo_mind.exe"
        CACHE INTERNAL "Alcedo semantic sidecar executable name.")
else()
    set(ALCEDO_MIND_EXE_NAME "alcedo_mind"
        CACHE INTERNAL "Alcedo semantic sidecar executable name.")
endif()

set(ALCEDO_MIND_BINARY "${ALCEDO_MIND_TARGET_DIR}/${ALCEDO_MIND_EXE_NAME}"
    CACHE INTERNAL "Alcedo semantic sidecar executable path.")

if(ALCEDO_BUILD_SEMANTIC_SIDECAR)
    set(ALCEDO_MIND_CARGO_MSVC "${ALCEDO_MIND_SOURCE_DIR}/script/cargo_msvc.cmd")
    if(WIN32)
        if(NOT EXISTS "${ALCEDO_MIND_CARGO_MSVC}")
            message(FATAL_ERROR "Semantic sidecar MSVC cargo wrapper was not found: ${ALCEDO_MIND_CARGO_MSVC}")
        endif()
        set(ALCEDO_MIND_CARGO_COMMAND "${ALCEDO_MIND_CARGO_MSVC}" build --release)
    else()
        find_program(ALCEDO_CARGO_EXECUTABLE NAMES cargo REQUIRED)
        set(ALCEDO_MIND_CARGO_COMMAND "${ALCEDO_CARGO_EXECUTABLE}" build --release)
    endif()

    file(GLOB_RECURSE ALCEDO_MIND_RUST_SOURCES CONFIGURE_DEPENDS
        "${ALCEDO_MIND_SOURCE_DIR}/src/*.rs"
    )
    file(GLOB ALCEDO_MIND_PROTO_FILES CONFIGURE_DEPENDS
        "${ALCEDO_MIND_SOURCE_DIR}/proto/*.proto"
    )

    set(ALCEDO_MIND_BYPRODUCTS "${ALCEDO_MIND_BINARY}")
    if(WIN32)
        list(APPEND ALCEDO_MIND_BYPRODUCTS "${ALCEDO_MIND_TARGET_DIR}/DirectML.dll")
    endif()

    set(ALCEDO_MIND_BUILD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/alcedo_mind_sidecar.stamp")
    add_custom_command(
        OUTPUT "${ALCEDO_MIND_BUILD_STAMP}"
        COMMAND ${ALCEDO_MIND_CARGO_COMMAND}
        COMMAND ${CMAKE_COMMAND} -E touch "${ALCEDO_MIND_BUILD_STAMP}"
        WORKING_DIRECTORY "${ALCEDO_MIND_SOURCE_DIR}"
        DEPENDS
            "${ALCEDO_MIND_SOURCE_DIR}/Cargo.toml"
            "${ALCEDO_MIND_SOURCE_DIR}/Cargo.lock"
            "${ALCEDO_MIND_SOURCE_DIR}/build.rs"
            ${ALCEDO_MIND_RUST_SOURCES}
            ${ALCEDO_MIND_PROTO_FILES}
        BYPRODUCTS ${ALCEDO_MIND_BYPRODUCTS}
        COMMENT "Building semantic runtime sidecar"
        VERBATIM
        USES_TERMINAL
    )
    add_custom_target(alcedo_mind_sidecar DEPENDS "${ALCEDO_MIND_BUILD_STAMP}")
    add_dependencies(alcedo_main alcedo_mind_sidecar)
endif()

set(ALCEDO_MAIN_FONT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/config/fonts/main_Inter.ttf")
set(ALCEDO_MAIN_ZH_FONT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/config/fonts/main_NotoSans_zh.ttf")
if(APPLE AND ALCEDO_MACOS_BUNDLE)
    # Inside .app bundle, resources go next to the executable in Contents/MacOS/
    add_custom_command(TARGET alcedo_main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:alcedo_main>/MacOS/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_MAIN_FONT_SRC}" "$<TARGET_BUNDLE_CONTENT_DIR:alcedo_main>/MacOS/fonts/main_Inter.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_MAIN_ZH_FONT_SRC}" "$<TARGET_BUNDLE_CONTENT_DIR:alcedo_main>/MacOS/fonts/main_NotoSans_zh.ttf"
        VERBATIM
    )
else()
    add_custom_command(TARGET alcedo_main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:alcedo_main>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_MAIN_FONT_SRC}" "$<TARGET_FILE_DIR:alcedo_main>/fonts/main_Inter.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_MAIN_ZH_FONT_SRC}" "$<TARGET_FILE_DIR:alcedo_main>/fonts/main_NotoSans_zh.ttf"
        VERBATIM
    )
endif()

if(APPLE AND ALCEDO_METAL_ENABLED AND ALCEDO_METAL_RUNTIME_LIBS)
    if(ALCEDO_MACOS_BUNDLE)
        set(_alcedo_metallib_runtime_dir "$<TARGET_BUNDLE_CONTENT_DIR:alcedo_main>/Resources/metallib")
    else()
        set(_alcedo_metallib_runtime_dir "$<TARGET_FILE_DIR:alcedo_main>/metallib")
    endif()

    add_custom_command(TARGET alcedo_main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_alcedo_metallib_runtime_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ALCEDO_METAL_RUNTIME_LIBS}
            "${_alcedo_metallib_runtime_dir}"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

if(WIN32 AND TARGET alcedo_lensfun)
    add_custom_command(TARGET alcedo_main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:alcedo_lensfun>"
            "$<TARGET_FILE_DIR:alcedo_main>/lensfun.dll"
        VERBATIM
    )
endif()

if(TARGET alcedo_main AND ALCEDO_DUCKDB_VSS_EXTENSION AND ALCEDO_DUCKDB_FTS_EXTENSION)
    if(APPLE AND ALCEDO_MACOS_BUNDLE)
        set(_alcedo_duckdb_extension_runtime_dir
            "$<TARGET_BUNDLE_CONTENT_DIR:alcedo_main>/Resources/duckdb_extensions")
    else()
        set(_alcedo_duckdb_extension_runtime_dir "$<TARGET_FILE_DIR:alcedo_main>/duckdb_extensions")
    endif()
    add_custom_command(TARGET alcedo_main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:alcedo_main>/duckdb_extensions"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_alcedo_duckdb_extension_runtime_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ALCEDO_DUCKDB_VSS_EXTENSION}"
            "${_alcedo_duckdb_extension_runtime_dir}/vss.duckdb_extension"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ALCEDO_DUCKDB_FTS_EXTENSION}"
            "${_alcedo_duckdb_extension_runtime_dir}/fts.duckdb_extension"
        VERBATIM
    )
endif()

if(ALCEDO_BUILD_SEMANTIC_SIDECAR AND TARGET alcedo_mind_sidecar)
    add_custom_target(alcedo_mind_sidecar_runtime_copy ALL
        COMMAND ${CMAKE_COMMAND}
            "-DALCEDO_MIND_BINARY=${ALCEDO_MIND_BINARY}"
            "-DALCEDO_MIND_TARGET_DIR=${ALCEDO_MIND_TARGET_DIR}"
            "-DALCEDO_MIND_PROMPTS_DIR=${ALCEDO_MIND_PROMPTS_DIR}"
            "-DALCEDO_MIND_DEST_DIR=$<TARGET_FILE_DIR:alcedo_main>"
            -P "${CMAKE_SOURCE_DIR}/scripts/cmake/copy_semantic_sidecar.cmake"
        DEPENDS alcedo_mind_sidecar
        COMMENT "Copying semantic runtime sidecar next to alcedo_main"
        VERBATIM
    )
    add_dependencies(alcedo_main alcedo_mind_sidecar_runtime_copy)
endif()

# ── aria2c: bundled next to alcedo_main for accelerated model downloads ──
include("${CMAKE_SOURCE_DIR}/scripts/cmake/fetch_aria2.cmake")
if(ALCEDO_ARIA2C_BINARY AND TARGET alcedo_main)
    if(APPLE AND ALCEDO_MACOS_BUNDLE)
        set(_aria2c_dest_dir "$<TARGET_BUNDLE_CONTENT_DIR:alcedo_main>/MacOS")
    else()
        set(_aria2c_dest_dir "$<TARGET_FILE_DIR:alcedo_main>")
    endif()
    add_custom_command(TARGET alcedo_main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_aria2c_dest_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_ARIA2C_BINARY}" "${_aria2c_dest_dir}"
        VERBATIM
    )
    if(NOT WIN32)
        add_custom_command(TARGET alcedo_main POST_BUILD
            COMMAND chmod +x "${_aria2c_dest_dir}/aria2c"
            VERBATIM
        )
    endif()
endif()
