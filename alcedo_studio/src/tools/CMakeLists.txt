# Runs from the install tree, with the same OpenCL registry as the product.
if(ALCEDO_OPENCL_ENABLED OR ALCEDO_METAL_ENABLED)
    qt_add_executable(alcedo_gpu_check gpu_kernel_check.cpp)
    # All packaged executables share one build-tree DLL directory. CMake's
    # dependency scan must see one path for each DLL, as it does after install.
    set_target_properties(alcedo_gpu_check PROPERTIES
        OUTPUT_NAME "alcedo-gpu-check"
        RUNTIME_OUTPUT_DIRECTORY "${ALCEDO_BINARY_ROOT}")
    target_include_directories(alcedo_gpu_check PRIVATE "${ALCEDO_INCLUDE_ROOT}")
    target_link_libraries(alcedo_gpu_check PRIVATE Qt6::Core)
    if(ALCEDO_OPENCL_ENABLED)
        target_link_libraries(alcedo_gpu_check PRIVATE OpenClProgramLibrary)
        target_compile_definitions(alcedo_gpu_check PRIVATE
            ALCEDO_OPENCL_SHADER_SOURCE_ROOT="${ALCEDO_SRC_ROOT}")
    endif()
    if(ALCEDO_METAL_ENABLED)
        target_link_libraries(alcedo_gpu_check PRIVATE MetalContext alcedo::metal_cpp)
    endif()
    alcedo_copy_linked_runtime_dlls(alcedo_gpu_check)
endif()
