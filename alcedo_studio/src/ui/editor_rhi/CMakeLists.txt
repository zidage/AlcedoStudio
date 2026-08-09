# Qt Quick RHI viewport and maintained presentation harness.

set(ALCEDO_EDITOR_RHI_ROOT "${ALCEDO_SRC_ROOT}/ui/editor_rhi")

set(EDITOR_RHI_CONTRACT_SRCS
    "${ALCEDO_EDITOR_RHI_ROOT}/editor_backend.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/frame_presentation_lease.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/native_resource_counters.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/harness_fixtures.cpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/editor_backend.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/frame_presentation_lease.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/native_resource_counters.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/harness_fixtures.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/editor_startup.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/cuda_adapter_discovery.hpp"
)
set(EDITOR_RHI_STARTUP_SRCS
    "${ALCEDO_EDITOR_RHI_ROOT}/editor_startup.cpp"
)
if(WIN32 AND ALCEDO_CUDA_ENABLED)
    list(APPEND EDITOR_RHI_STARTUP_SRCS
        "${ALCEDO_EDITOR_RHI_ROOT}/cuda_adapter_discovery.cpp"
    )
endif()

def_library(EditorRhiContracts
    SOURCES ${EDITOR_RHI_CONTRACT_SRCS}
    PUBLIC_DEPS Qt6::Core
)
if(ALCEDO_CUDA_ENABLED)
    target_compile_definitions(EditorRhiContracts PUBLIC HAVE_CUDA)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(EditorRhiContracts PUBLIC HAVE_OPENCL)
endif()
if(ALCEDO_METAL_ENABLED)
    target_compile_definitions(EditorRhiContracts PUBLIC HAVE_METAL)
endif()

set(EDITOR_RHI_UNIFIED_VIEWPORT_SRCS
    ${EDITOR_RHI_STARTUP_SRCS}
    "${ALCEDO_EDITOR_RHI_ROOT}/lease_target_adapters.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/direct_present_queue.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/direct_frame_sink.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/editor_viewport_item.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/editor_viewport_renderer.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/editor_interaction_controller.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/editor_overlay_item.cpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/direct_present_queue.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/direct_frame_sink.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/lease_target_adapters.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/viewer/viewer_view_state.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/editor_viewport_item.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/editor_viewport_renderer.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/editor_interaction_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/editor_overlay_item.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/color_manager.hpp"
)
if(APPLE)
    list(APPEND EDITOR_RHI_UNIFIED_VIEWPORT_SRCS
        "${ALCEDO_SRC_ROOT}/ui/edit_viewer/color_manager.mm"
    )
    set_source_files_properties(
        "${ALCEDO_SRC_ROOT}/ui/edit_viewer/color_manager.mm"
        PROPERTIES LANGUAGE OBJCXX
    )
else()
    list(APPEND EDITOR_RHI_UNIFIED_VIEWPORT_SRCS
        "${ALCEDO_SRC_ROOT}/ui/edit_viewer/color_manager.cpp"
    )
endif()

def_library(EditorRhiViewport
    SOURCES ${EDITOR_RHI_UNIFIED_VIEWPORT_SRCS}
    PUBLIC_DEPS
        EditorRhiContracts
        EditorViewerLogic
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Quick
        Qt6::Qml
        Qt6::OpenGL
        Qt6::ShaderTools
        opencv_core
    PRIVATE_DEPS AppDiagnostics
)
if(ALCEDO_CUDA_ENABLED)
    target_compile_definitions(EditorRhiViewport PUBLIC HAVE_CUDA)
    target_link_libraries(EditorRhiViewport PUBLIC CUDA::cudart)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(EditorRhiViewport PUBLIC HAVE_OPENCL)
    # OpenClProgramLibrary owns runtime init, program registration, and warm-up.
    # Linking only OpenClContext left editor_startup able to call
    # TryInitializeOpenClRuntime via transitive deps, but production warm-up /
    # registration must share the same static library as raw/edit GetProgram.
    target_link_libraries(EditorRhiViewport PUBLIC OpenClProgramLibrary OpenCL::OpenCL)
endif()
if(ALCEDO_METAL_ENABLED)
    target_compile_definitions(EditorRhiViewport PUBLIC HAVE_METAL)
endif()
if(APPLE)
    target_link_libraries(EditorRhiViewport PRIVATE
        "-framework AppKit"
        "-framework QuartzCore"
        "-framework CoreGraphics"
    )
endif()
if(WIN32)
    target_link_libraries(EditorRhiViewport PRIVATE d3d11 dxgi opengl32)
endif()

qt_add_shaders(EditorRhiViewport "editor_rhi_viewport_shaders"
    PREFIX
        "/shaders/editor_rhi"
    BASE
        "${ALCEDO_EDITOR_RHI_ROOT}/shaders"
    FILES
        "${ALCEDO_EDITOR_RHI_ROOT}/shaders/editor_viewport.vert"
        "${ALCEDO_EDITOR_RHI_ROOT}/shaders/editor_viewport.frag"
)

set(EDITOR_RHI_HARNESS_VIEWPORT_SRCS
    "${ALCEDO_EDITOR_RHI_ROOT}/harness_viewport_item.cpp"
    "${ALCEDO_EDITOR_RHI_ROOT}/harness_viewport_renderer.cpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/harness_viewport_item.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/editor_rhi/harness_viewport_renderer.hpp"
)

def_library(EditorRhiHarnessLib
    SOURCES
        ${EDITOR_RHI_STARTUP_SRCS}
        ${EDITOR_RHI_HARNESS_VIEWPORT_SRCS}
    PUBLIC_DEPS
        EditorRhiContracts
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Quick
        Qt6::OpenGL
        Qt6::ShaderTools
)
if(APPLE)
    set_source_files_properties(
        "${ALCEDO_EDITOR_RHI_ROOT}/harness_viewport_renderer.cpp"
        PROPERTIES LANGUAGE OBJCXX
    )
    target_link_libraries(EditorRhiHarnessLib PRIVATE
        "-framework Metal"
        "-framework Foundation"
    )
endif()
if(ALCEDO_CUDA_ENABLED)
    target_compile_definitions(EditorRhiHarnessLib PUBLIC HAVE_CUDA)
    target_link_libraries(EditorRhiHarnessLib PUBLIC CUDA::cudart)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(EditorRhiHarnessLib PUBLIC HAVE_OPENCL)
    target_link_libraries(EditorRhiHarnessLib PUBLIC OpenClProgramLibrary OpenCL::OpenCL)
endif()
if(ALCEDO_METAL_ENABLED)
    target_compile_definitions(EditorRhiHarnessLib PUBLIC HAVE_METAL)
endif()
if(WIN32)
    target_link_libraries(EditorRhiHarnessLib PRIVATE d3d11 dxgi opengl32)
endif()

qt_add_shaders(EditorRhiHarnessLib "editor_rhi_harness_shaders"
    PREFIX
        "/shaders/editor_rhi"
    BASE
        "${ALCEDO_EDITOR_RHI_ROOT}/shaders"
    FILES
        "${ALCEDO_EDITOR_RHI_ROOT}/shaders/harness_image.vert"
        "${ALCEDO_EDITOR_RHI_ROOT}/shaders/harness_image.frag"
)

add_executable(EditorRhiHarness
    "${ALCEDO_EDITOR_RHI_ROOT}/editor_rhi_harness_main.cpp"
)
target_include_directories(EditorRhiHarness PUBLIC "${ALCEDO_INCLUDE_ROOT}")
target_link_libraries(EditorRhiHarness PRIVATE
    EditorRhiHarnessLib
    EditorRhiViewport
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
)
if(COMMAND qt_finalize_target)
    qt_finalize_target(EditorRhiHarness)
elseif(COMMAND qt_finalize_executable)
    qt_finalize_executable(EditorRhiHarness)
endif()

if(WIN32 AND ALCEDO_CUDA_ENABLED)
    add_custom_target(run_editor_rhi_harness_cuda
        COMMAND EditorRhiHarness --editor-backend=cuda --case=direct-presentation
        DEPENDS EditorRhiHarness
        WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
        USES_TERMINAL
    )
endif()
if(WIN32 AND ALCEDO_OPENCL_ENABLED)
    add_custom_target(run_editor_rhi_harness_opencl
        COMMAND EditorRhiHarness --editor-backend=opencl --case=direct-presentation
        DEPENDS EditorRhiHarness
        WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
        USES_TERMINAL
    )
endif()
