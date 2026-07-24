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
    def_library(OpenClContext
        SOURCES
            "${ALCEDO_SRC_ROOT}/opencl/opencl_context.cpp"
            "${ALCEDO_SRC_ROOT}/opencl/opencl_api_counters.cpp"
        PUBLIC_DEPS OpenCL::OpenCL
    )
    def_library(OpenClProgramLibrary
        SOURCES
            "${ALCEDO_SRC_ROOT}/opencl/opencl_backend_program_registry.cpp"
            "${ALCEDO_SRC_ROOT}/opencl/opencl_program_library.cpp"
            "${ALCEDO_SRC_ROOT}/opencl/opencl_runtime.cpp"
            "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_raw_programs.cpp"
            "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_demosaicnet_programs.cpp"
            "${ALCEDO_SRC_ROOT}/edit/pipeline/opencl_pipeline_programs.cpp"
            "${ALCEDO_SRC_ROOT}/edit/scope/opencl_scope_programs.cpp"
            "${ALCEDO_SRC_ROOT}/opencl/opencl_geometry_programs.cpp"
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
