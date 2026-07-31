# OpenCL target ownership.
if(ALCEDO_OPENCL_ENABLED)
    set(ALCEDO_OPENCL_SHADER_SOURCE_ROOT "${ALCEDO_SRC_ROOT}")
    set(OPENCL_RAW_UTILS_CL "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_shader/raw_utils_opencl.cl")
    set(OPENCL_RAW_TO_LINEAR_REF_CL "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_shader/to_linear_ref.cl")
    set(OPENCL_DEBAYER_RCD_CL "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_shader/debayer_rcd.cl")
    set(OPENCL_HIGHLIGHT_RECONSTRUCT_CL "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_shader/highlight_reconstruct.cl")
    set(OPENCL_CVT_REF_SPACE_CL "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_shader/cvt_ref_space.cl")
    set(OPENCL_PRNG_CL "${ALCEDO_SRC_ROOT}/opencl/opencl_shader/prng.cl")
    set(OPENCL_EDIT_PIPELINE_FUSED_PARAMS_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/fused_params.cl")
    set(OPENCL_EDIT_PIPELINE_FUSED_PARAMS_VALIDATION_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/fused_params_validation.cl")
    set(OPENCL_EDIT_PIPELINE_FUSED_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/edit_pipeline_fused.cl")
    set(OPENCL_EDIT_PIPELINE_DETAIL_COMMON_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/detail_common.cl")
    set(OPENCL_EDIT_PIPELINE_FILM_GRAIN_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/film_grain.cl")
    set(OPENCL_EDIT_PIPELINE_HALATION_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/halation.cl")
    set(OPENCL_EDIT_PIPELINE_DETAIL_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/edit_pipeline_detail.cl")
    set(OPENCL_EDIT_PIPELINE_COMMON_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/common.cl")
    set(OPENCL_EDIT_PIPELINE_BASIC_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/basic.cl")
    set(OPENCL_EDIT_PIPELINE_TONE_MAPPING_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/tone_mapping.cl")
    set(OPENCL_EDIT_PIPELINE_COLOR_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/color.cl")
    set(OPENCL_EDIT_PIPELINE_CST_CL "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_shader/cst.cl")
    set(OPENCL_XTRANS_INTERPOLATE_CL "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_shader/xtrans_interpolate.cl")
    set(OPENCL_DEMOSAICNET_CONV_CL "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_shader/demosaicnet_conv.cl")
    set(OPENCL_DEMOSAICNET_STRUCTURAL_CL "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_shader/demosaicnet_structural.cl")
    set(OPENCL_GEOMETRY_UTILS_CL "${ALCEDO_SRC_ROOT}/opencl/opencl_shader/geometry_utils.cl")
    set(OPENCL_EDIT_GEOMETRY_LENS_CALIB_CL "${ALCEDO_SRC_ROOT}/edit/operators/geometry/opencl_shader/lens_calib.cl")
    set(OPENCL_SCOPE_ANALYZER_CL "${ALCEDO_SRC_ROOT}/edit/scope/opencl_shader/scope_analyzer.cl")

    # On Windows, first-party CUDA pipeline targets (RawProcessorOp, Operators,
    # EditPipeline, …) are SHARED by default. If OpenCL context/program runtime
    # stays STATIC, each DLL and alcedo_main gets its own Meyers singleton for
    # OpenClContext / OpenClProgramLibrary / OpenClBackendProgramRegistry.
    # editor_startup then registers programs in the EXE copy while GetProgram
    # from RawProcessorOp.dll sees programs_.size() == 0.
    # Mirror def_cuda_library: one process-wide OpenCL runtime DLL pair.
    set(_alcedo_opencl_shared OFF)
    if(WIN32 AND ALCEDO_CUDA_ENABLED AND ALCEDO_BUILD_CUDA_DLLS)
        set(_alcedo_opencl_shared ON)
    endif()

    set(_alcedo_opencl_context_sources
        "${ALCEDO_SRC_ROOT}/opencl/opencl_context.cpp"
        "${ALCEDO_SRC_ROOT}/opencl/opencl_api_counters.cpp"
    )
    set(_alcedo_opencl_program_library_sources
        "${ALCEDO_SRC_ROOT}/opencl/opencl_backend_program_registry.cpp"
        "${ALCEDO_SRC_ROOT}/opencl/opencl_program_library.cpp"
        "${ALCEDO_SRC_ROOT}/opencl/opencl_runtime.cpp"
        "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_raw_programs.cpp"
        "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_demosaicnet_programs.cpp"
        "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_pipeline_programs.cpp"
        "${ALCEDO_SRC_ROOT}/edit/scope/opencl_scope_programs.cpp"
        "${ALCEDO_SRC_ROOT}/opencl/opencl_geometry_programs.cpp"
    )

    if(_alcedo_opencl_shared)
        def_library(OpenClContext
            TYPE SHARED
            SOURCES ${_alcedo_opencl_context_sources}
            PUBLIC_DEPS OpenCL::OpenCL
        )
        def_library(OpenClProgramLibrary
            TYPE SHARED
            SOURCES ${_alcedo_opencl_program_library_sources}
            PUBLIC_DEPS
                OpenClContext
                OpenCL::OpenCL
            PRIVATE_DEPS
                StrConv
        )
        set_target_properties(OpenClContext OpenClProgramLibrary PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS ON
        )
        # Same /GL- note as def_cuda_library: CMake export scan needs regular COFF.
        foreach(_alcedo_opencl_shared_target IN ITEMS OpenClContext OpenClProgramLibrary)
            target_compile_options(${_alcedo_opencl_shared_target} PRIVATE
                $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:/GL->
            )
        endforeach()
        install(TARGETS OpenClContext OpenClProgramLibrary
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        )
    else()
        def_library(OpenClContext
            SOURCES ${_alcedo_opencl_context_sources}
            PUBLIC_DEPS OpenCL::OpenCL
        )
        def_library(OpenClProgramLibrary
            SOURCES ${_alcedo_opencl_program_library_sources}
            PUBLIC_DEPS
                OpenClContext
                OpenCL::OpenCL
            PRIVATE_DEPS
                StrConv
        )
    endif()

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
