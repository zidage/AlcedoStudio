# CUDA target ownership.
if(ALCEDO_CUDA_ENABLED)
    def_cuda_library(CudaUtils
        SOURCES
            "${ALCEDO_SRC_ROOT}/cuda/prng.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/relu.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/mul.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/concat.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/slice.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/crop.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/layout.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/conv2d.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/cutlass_conv2d.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/demosaicnet_nhwc.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/conv_transpose2d.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/fused_post_output.cu"
            "${ALCEDO_SRC_ROOT}/cuda/nn/safetensors.cpp"
        PUBLIC_DEPS
            CUDA::cudart
            NnSafetensors
            ${ALCEDO_OPENCV_CORE_DEPS}
    )
    set_target_properties(CudaUtils PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(CudaUtils PUBLIC HAVE_CUDA)
    target_include_directories(CudaUtils PRIVATE
        "${ALCEDO_SRC_ROOT}/third_party/cutlass/include"
    )
    set_source_files_properties("${ALCEDO_SRC_ROOT}/cuda/nn/cutlass_conv2d.cu" PROPERTIES
        COMPILE_OPTIONS "-Xcompiler=/Zc:__cplusplus;--expt-relaxed-constexpr"
    )
endif()
