# Decoders target ownership (RAW processor, DemosaicNet, image decoding).
# 3. Processors & Operators
# ==============================================================================

# --- RawProcessorOp Configuration (CPU/GPU Split) ---
set(RAW_OP_CPU_SRCS
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/color_space_conv.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/debayer_ahd.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/debayer_amaze.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/debayer_rcd.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/highlight_reconstruct.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/white_balance.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/raw_proc_utils.cpp
)

set(RAW_OP_GPU_SRCS)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_color_space_conv.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_debayer_ahd.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_debayer_rcd.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_dng_warp.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_downsample.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_highlight_reconstruct.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_image_ops.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_rotate.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_white_balance.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_xtrans_interpolate.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_cvt_ref_space.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_debayer_rcd.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_highlight_reconstruct.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_to_linear_ref.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_xtrans_interpolate.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_to_linear_ref.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_debayer_rcd.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_xtrans_interpolate.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_highlight_reconstruct.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_cvt_ref_space.cpp
    )
endif()

# RawProcessorOp Target
def_cuda_library(RawProcessorOp
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
    ${ALCEDO_SRC_ROOT}/edit/operators/operator_factory.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/basic/exposure_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/basic/exposure_op_vec.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/basic/contrast_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/basic/black_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/basic/color_temp_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/basic/highlight_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/basic/shadow_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/basic/white_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/color/tint_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/color/conversion/Oklab_cvt.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/color/saturation_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/color/vibrance_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/color/HLS_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/detail/clarity_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/detail/sharpen_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/wheel/color_wheel_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/curve/curve_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/cst/cst_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/cst/lmt_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/cst/aces_odt_cpu.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/cst/open_drt_cpu.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/cst/odt_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/cst/film_grain_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/cst/halation_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/raw/raw_decode_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/geometry/resize_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/geometry/crop_rotate_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/geometry/lens_calib_op.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/operator_registeration.cpp
)

if(ALCEDO_CUDA_ENABLED)
    list(APPEND OPERATORS_SRCS
        ${ALCEDO_SRC_ROOT}/edit/operators/geometry/cuda_geometry_ops.cu
        ${ALCEDO_SRC_ROOT}/edit/operators/geometry/cuda_lens_calib_ops.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND OPERATORS_SRCS
        ${ALCEDO_SRC_ROOT}/edit/operators/basic/highlight_shadow_local_tone_metal.cpp
        ${ALCEDO_SRC_ROOT}/edit/operators/geometry/metal_lens_calib.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND OPERATORS_SRCS
        ${ALCEDO_SRC_ROOT}/edit/operators/basic/highlight_shadow_local_tone_opencl.cpp
        ${ALCEDO_SRC_ROOT}/edit/operators/geometry/opencl_lens_calib_ops.cpp
    )
endif()

def_cuda_library(Operators
    SOURCES ${OPERATORS_SRCS}
    PUBLIC_DEPS ImageBuffer JSON OpenColorIO::OpenColorIO hwy::hwy lensfun::lensfun
    # Image provides RawColorContextToJson/FromJson used by RawDecodeOp durable params.
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS} RawProcessor Image EditGraph
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
    SOURCES ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_accelerator.cpp
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
target_link_libraries(Operators PRIVATE AcceleratorBackend OpenMP::OpenMP_CXX)

# --- Edit Pipeline ---
set(PIPELINE_SRCS
    ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_cpu.cpp
    ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_stage.cpp
    ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_gpu_wrapper.cpp
    ${ALCEDO_INCLUDE_ROOT}/edit/pipeline/highlight_shadow_local_tone.hpp
    ${ALCEDO_INCLUDE_ROOT}/edit/pipeline/local_tone_mapping.hpp
    ${ALCEDO_INCLUDE_ROOT}/edit/pipeline/metal_kernel_dispatch.hpp
    ${ALCEDO_INCLUDE_ROOT}/edit/pipeline/metal_pipeline_stats.hpp
    ${ALCEDO_INCLUDE_ROOT}/edit/pipeline/opencl_kernel_dispatch.hpp
    ${ALCEDO_SRC_ROOT}/edit/scope/scope_analyzer.cpp
    ${ALCEDO_SRC_ROOT}/edit/scope/final_display_frame_tap.cpp
)

if(ALCEDO_CUDA_ENABLED)
    set(GPU_PIPELINE_SRCS
        ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_gpu_impl.cu
        ${ALCEDO_SRC_ROOT}/edit/scope/cuda_scope_analyzer.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND GPU_PIPELINE_SRCS
        ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_metal_impl.cpp
        ${ALCEDO_SRC_ROOT}/edit/scope/metal_scope_analyzer.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND GPU_PIPELINE_SRCS
        ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_opencl_impl.cpp
        ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_opencl_param.cpp
        ${ALCEDO_SRC_ROOT}/edit/scope/opencl_scope_analyzer.cpp
    )
endif()

def_cuda_library(EditPipeline
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

set(RENDERER_SRCS ${ALCEDO_SRC_ROOT}/renderer/pipeline_scheduler.cpp)
def_library(PipelineScheduler
    SOURCES ${RENDERER_SRCS}
    PUBLIC_DEPS EditPipeline ThreadPool ImageLoader)

# --- Raw Processor Wrapper ---
set(RAW_PROCESSOR_BACKEND_SRCS)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS ${ALCEDO_SRC_ROOT}/decoders/processor/raw_processor_cuda.cpp)
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS ${ALCEDO_SRC_ROOT}/decoders/processor/raw_processor_metal.cpp)
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS ${ALCEDO_SRC_ROOT}/decoders/processor/raw_processor_opencl.cpp)
endif()

# Hard-coded DemosaicNet modules + lazy model cache. Owned by the RAW CUDA backend path
# (raw_processor_cuda.cpp / cuda_demosaicnet.cu), NOT by RawProcessorOp (legacy op facade only).
if(ALCEDO_CUDA_ENABLED)
    def_cuda_library(DemosaicNet
        SOURCES
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_bayer.cu
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_xtrans.cu
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_cache.cpp
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_profiler.cpp
        PUBLIC_DEPS
            CudaUtils
    )
    set_target_properties(DemosaicNet PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(DemosaicNet PUBLIC HAVE_CUDA)
    target_compile_definitions(DemosaicNet PRIVATE
        ALCEDO_DEMOASICNET_MODEL_DIR="${ALCEDO_SRC_ROOT}/config/models"
    )

    # Product-path Neural preprocess (phase-align + gamma) + GpuMat demosaic entry.
    # Stays on CudaDemosaicNetEntry (not generic DemosaicNet modules) so LibRaw CFA types link.
    def_cuda_library(CudaDemosaicNetEntry
        SOURCES
            ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_demosaicnet.cu
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_preprocess.cu
        PUBLIC_DEPS
            DemosaicNet
            libraw::raw_r
    )
    set_target_properties(CudaDemosaicNetEntry PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(CudaDemosaicNetEntry PUBLIC HAVE_CUDA)
endif()

def_library(RawProcessor
    SOURCES ${ALCEDO_SRC_ROOT}/decoders/processor/raw_processor.cpp ${RAW_PROCESSOR_BACKEND_SRCS}
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
    ${ALCEDO_SRC_ROOT}/decoders/decoder_scheduler.cpp
    ${ALCEDO_SRC_ROOT}/decoders/raw_decoder.cpp
    ${ALCEDO_SRC_ROOT}/decoders/thumbnail_decoder.cpp
    ${ALCEDO_SRC_ROOT}/decoders/metadata_decoder.cpp
)

def_library(ImageDecoder
    SOURCES ${DECODER_SRCS}
    PUBLIC_DEPS Image ThreadPool libraw::raw_r RawProcessor
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS}
)

def_library(ImageLoader 
    SOURCES ${ALCEDO_SRC_ROOT}/io/image/image_loader.cpp
    PUBLIC_DEPS ImageDecoder
)

# ==============================================================================
