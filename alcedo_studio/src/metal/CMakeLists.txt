# Metal target ownership (shader compilation infrastructure, GPU utilities, DemosaicNet).
if(ALCEDO_METAL_ENABLED)
    if(POLICY CMP0116)
        cmake_policy(SET CMP0116 NEW)
    endif()

    # Metallib/air outputs live under ALCEDO_BINARY_ROOT (src/ binary tree), not
    # CMAKE_CURRENT_BINARY_DIR (this domain's metal/ subdir). Paths must also be
    # CACHE INTERNAL so sibling domains (decoders, edit) can bake them into
    # compile definitions — ordinary set() is directory-local after Phase 3.
    set(METAL_UTILS_SRC "${ALCEDO_SRC_ROOT}/metal/metal_utils/metal_convert.metal")
    set(METAL_UTILS_AIR "${ALCEDO_BINARY_ROOT}/metal/metal_utils/metal_convert.air")
    set(METAL_UTILS_METALLIB "${ALCEDO_BINARY_ROOT}/metal/metal_utils/metal_convert.metallib"
        CACHE INTERNAL "Metal convert utils metallib path" FORCE)
    set(METAL_GEOMETRY_UTILS_SRC "${ALCEDO_SRC_ROOT}/metal/metal_utils/geometry_utils.metal")
    set(METAL_GEOMETRY_UTILS_AIR "${ALCEDO_BINARY_ROOT}/metal/metal_utils/geometry_utils.air")
    set(METAL_GEOMETRY_UTILS_METALLIB "${ALCEDO_BINARY_ROOT}/metal/metal_utils/geometry_utils.metallib"
        CACHE INTERNAL "Metal geometry utils metallib path" FORCE)
    set(METAL_LENS_CALIB_SRC "${ALCEDO_SRC_ROOT}/edit/operators/geometry/metal_shader/lens_calib.metal")
    set(METAL_LENS_CALIB_AIR "${ALCEDO_BINARY_ROOT}/edit/operators/geometry/metal_shader/lens_calib.air")
    set(METAL_LENS_CALIB_METALLIB "${ALCEDO_BINARY_ROOT}/edit/operators/geometry/metal_shader/lens_calib.metallib"
        CACHE INTERNAL "Metal lens calibration metallib path" FORCE)
    set(METAL_FUSED_PIPELINE_SRC "${ALCEDO_SRC_ROOT}/edit/pipeline/metal_shader/fused_pipeline.metal")
    set(METAL_FUSED_PIPELINE_AIR "${ALCEDO_BINARY_ROOT}/edit/pipeline/metal_shader/fused_pipeline.air")
    set(METAL_FUSED_PIPELINE_METALLIB "${ALCEDO_BINARY_ROOT}/edit/pipeline/metal_shader/fused_pipeline.metallib"
        CACHE INTERNAL "Metal fused pipeline metallib path" FORCE)
    set(METAL_SCOPE_ANALYZER_SRC "${ALCEDO_SRC_ROOT}/edit/scope/metal_shader/scope_analyzer.metal")
    set(METAL_SCOPE_ANALYZER_AIR "${ALCEDO_BINARY_ROOT}/edit/scope/metal_shader/scope_analyzer.air")
    set(METAL_SCOPE_ANALYZER_METALLIB "${ALCEDO_BINARY_ROOT}/edit/scope/metal_shader/scope_analyzer.metallib"
        CACHE INTERNAL "Metal scope analyzer metallib path" FORCE)
    set(METAL_FUSED_PIPELINE_DEPS
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/common.metal"
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/basic.metal"
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/tone_mapping.metal"
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/color.metal"
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/cst.metal"
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/detail.metal"
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/prng.metal"
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/film_grain.metal"
        "${ALCEDO_SRC_ROOT}/edit/operators/GPU_kernels/metal_shader/halation.metal"
    )
    set(METAL_TO_LINEAR_REF_SRC "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_shader/to_linear_ref.metal")
    set(METAL_TO_LINEAR_REF_AIR "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/to_linear_ref.air")
    set(METAL_TO_LINEAR_REF_METALLIB "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/to_linear_ref.metallib"
        CACHE INTERNAL "Metal ToLinearRef metallib path" FORCE)
    set(METAL_DEBAYER_RCD_SRC "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_shader/debayer_rcd.metal")
    set(METAL_DEBAYER_RCD_AIR "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/debayer_rcd.air")
    set(METAL_DEBAYER_RCD_METALLIB "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/debayer_rcd.metallib"
        CACHE INTERNAL "Metal Debayer RCD metallib path" FORCE)
    set(METAL_CVT_REF_SPACE_SRC "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_shader/cvt_ref_space.metal")
    set(METAL_CVT_REF_SPACE_AIR "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/cvt_ref_space.air")
    set(METAL_CVT_REF_SPACE_METALLIB "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/cvt_ref_space.metallib"
        CACHE INTERNAL "Metal ConvertToRefSpace metallib path" FORCE)
    set(METAL_HIGHLIGHT_RECONSTRUCT_SRC "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_shader/highlight_reconstruct.metal")
    set(METAL_HIGHLIGHT_RECONSTRUCT_AIR "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/highlight_reconstruct.air")
    set(METAL_HIGHLIGHT_RECONSTRUCT_METALLIB "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/highlight_reconstruct.metallib"
        CACHE INTERNAL "Metal HighlightReconstruct metallib path" FORCE)
    set(METAL_XTRANS_INTERPOLATE_SRC "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_shader/xtrans_interpolate.metal")
    set(METAL_XTRANS_INTERPOLATE_AIR "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/xtrans_interpolate.air")
    set(METAL_XTRANS_INTERPOLATE_METALLIB "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/xtrans_interpolate.metallib"
        CACHE INTERNAL "Metal X-Trans interpolate metallib path" FORCE)
    set(METAL_DEMOSAICNET_IO_SRC "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_shader/demosaicnet_io.metal")
    set(METAL_DEMOSAICNET_IO_AIR "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/demosaicnet_io.air")
    set(METAL_DEMOSAICNET_IO_METALLIB "${ALCEDO_BINARY_ROOT}/decoders/processor/operators/gpu/metal_shader/demosaicnet_io.metallib"
        CACHE INTERNAL "Metal DemosaicNet IO metallib path" FORCE)
    set(METAL_GEOMETRY_RESAMPLE_SRC "${ALCEDO_SRC_ROOT}/edit/runtime/metal/shader/geometry_resample.metal")
    set(METAL_GEOMETRY_RESAMPLE_AIR "${ALCEDO_BINARY_ROOT}/edit/runtime/metal/shader/geometry_resample.air")
    set(METAL_GEOMETRY_RESAMPLE_METALLIB "${ALCEDO_BINARY_ROOT}/edit/runtime/metal/shader/geometry_resample.metallib"
        CACHE INTERNAL "Metal DAG geometry resample metallib path" FORCE)
    set(METAL_CAMERA_COLOR_SRC "${ALCEDO_SRC_ROOT}/edit/runtime/metal/shader/camera_color.metal")
    set(METAL_CAMERA_COLOR_AIR "${ALCEDO_BINARY_ROOT}/edit/runtime/metal/shader/camera_color.air")
    set(METAL_CAMERA_COLOR_METALLIB "${ALCEDO_BINARY_ROOT}/edit/runtime/metal/shader/camera_color.metallib"
        CACHE INTERNAL "Metal DAG camera color metallib path" FORCE)
    set(METAL_PRIMARY_GRADE_SRC "${ALCEDO_SRC_ROOT}/edit/runtime/metal/shader/primary_grade.metal")
    set(METAL_PRIMARY_GRADE_AIR "${ALCEDO_BINARY_ROOT}/edit/runtime/metal/shader/primary_grade.air")
    set(METAL_PRIMARY_GRADE_METALLIB "${ALCEDO_BINARY_ROOT}/edit/runtime/metal/shader/primary_grade.metallib"
        CACHE INTERNAL "Metal DAG primary grade metallib path" FORCE)
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
    set(ALCEDO_METAL_MODULE_CACHE_DIR "${ALCEDO_BINARY_ROOT}/metal/module-cache")
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

    alcedo_add_metal_library(
        GpuDagGeometryResampleShaders
        "${METAL_GEOMETRY_RESAMPLE_SRC}"
        "${METAL_GEOMETRY_RESAMPLE_AIR}"
        "${METAL_GEOMETRY_RESAMPLE_METALLIB}"
        "Compiling Metal DAG geometry resample shader"
        "Linking Metal DAG geometry resample library"
    )

    alcedo_add_metal_library(
        GpuDagCameraColorShaders
        "${METAL_CAMERA_COLOR_SRC}"
        "${METAL_CAMERA_COLOR_AIR}"
        "${METAL_CAMERA_COLOR_METALLIB}"
        "Compiling Metal DAG camera color shader"
        "Linking Metal DAG camera color library"
    )

    alcedo_add_metal_library(
        GpuDagPrimaryGradeShaders
        "${METAL_PRIMARY_GRADE_SRC}"
        "${METAL_PRIMARY_GRADE_AIR}"
        "${METAL_PRIMARY_GRADE_METALLIB}"
        "Compiling Metal DAG primary grade shader"
        "Linking Metal DAG primary grade library"
    )

    add_custom_target(GpuDagMetalShaders DEPENDS
        "${METAL_GEOMETRY_RESAMPLE_METALLIB}"
        "${METAL_CAMERA_COLOR_METALLIB}"
        "${METAL_PRIMARY_GRADE_METALLIB}"
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
        "${METAL_GEOMETRY_RESAMPLE_METALLIB}"
        "${METAL_CAMERA_COLOR_METALLIB}"
        "${METAL_PRIMARY_GRADE_METALLIB}"
    )
    set(ALCEDO_METAL_RUNTIME_LIBS "${ALCEDO_METAL_RUNTIME_LIBS}" CACHE INTERNAL
        "Metal shader libraries required at runtime." FORCE
    )

    # Source paths are relative to ALCEDO_SRC_ROOT (this file lives under metal/).
    def_library(MetalContext SOURCES "${ALCEDO_SRC_ROOT}/metal/metal_context.cpp")
    target_link_libraries(MetalContext PUBLIC alcedo::metal_cpp)
    def_library(MetalImage SOURCES "${ALCEDO_SRC_ROOT}/image/metal_image.cpp" PUBLIC_DEPS MetalContext alcedo::metal_cpp ${ALCEDO_OPENCV_CORE_DEPS})
    def_library(MetalUtils
        SOURCES "${ALCEDO_SRC_ROOT}/metal/metal_utils/metal_convert_utils.cpp"
        PUBLIC_DEPS MetalContext MetalImage alcedo::metal_cpp ${ALCEDO_OPENCV_CORE_DEPS}
    )
    add_dependencies(MetalUtils MetalUtilsShaders)
    target_compile_definitions(MetalUtils PRIVATE ALCEDO_METAL_UTILS_METALLIB_PATH="${METAL_UTILS_METALLIB}")
    def_library(MetalGeometryUtils
        SOURCES "${ALCEDO_SRC_ROOT}/metal/metal_utils/geometry_utils.cpp"
        PUBLIC_DEPS MetalImage alcedo::metal_cpp ${ALCEDO_OPENCV_CORE_DEPS} libraw::raw_r
    )
    add_dependencies(MetalGeometryUtils MetalGeometryUtilsShaders)
    target_compile_definitions(MetalGeometryUtils PRIVATE ALCEDO_METAL_GEOMETRY_UTILS_METALLIB_PATH="${METAL_GEOMETRY_UTILS_METALLIB}")

    # Fixed Metal MPSGraph DemosaicNet modules and lazy model cache.
    def_library(MetalDemosaicNet
        SOURCES
            "${ALCEDO_SRC_ROOT}/decoders/processor/nn/metal_demosaicnet_module.mm"
            "${ALCEDO_SRC_ROOT}/decoders/processor/nn/metal_demosaicnet_cache.mm"
        PUBLIC_DEPS
            MetalContext
            NnSafetensors
            alcedo::metal_cpp
    )
    target_compile_definitions(MetalDemosaicNet PUBLIC HAVE_METAL)
    target_compile_definitions(MetalDemosaicNet PRIVATE
        ALCEDO_DEMOASICNET_MODEL_DIR="${ALCEDO_SRC_ROOT}/config/models"
        ALCEDO_METAL_DEMOSAICNET_IO_METALLIB_PATH="${METAL_DEMOSAICNET_IO_METALLIB}"
    )
    target_compile_options(MetalDemosaicNet PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    set_source_files_properties(
        "${ALCEDO_SRC_ROOT}/decoders/processor/nn/metal_demosaicnet_module.mm"
        "${ALCEDO_SRC_ROOT}/decoders/processor/nn/metal_demosaicnet_cache.mm"
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
            "${ALCEDO_SRC_ROOT}/decoders/processor/nn/metal_demosaicnet_tiled.mm"
            "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_demosaicnet.cpp"
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
        ALCEDO_DEMOASICNET_MODEL_DIR="${ALCEDO_SRC_ROOT}/config/models"
    )
    target_compile_options(MetalDemosaicNetEntry PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    set_source_files_properties(
        "${ALCEDO_SRC_ROOT}/decoders/processor/nn/metal_demosaicnet_tiled.mm"
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
