# Alcedo UI shell target ownership.

set(ALCEDO_UI_ROOT "${ALCEDO_SRC_ROOT}/ui")
set(ALCEDO_UI_SHELL_ROOT "${ALCEDO_UI_ROOT}/alcedo_main")

def_library(UiLocalization
    SOURCES
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/i18n.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/language_manager.hpp"
        "${ALCEDO_UI_SHELL_ROOT}/i18n.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/language_manager.cpp"
    PUBLIC_DEPS
        Qt6::Core
        Qt6::Qml
)

def_library(EditorViewerLogic
    SOURCES
        "${ALCEDO_UI_ROOT}/edit_viewer/edit_viewer_overlay_geometry.cpp"
        "${ALCEDO_UI_ROOT}/edit_viewer/viewport_mapper.cpp"
        "${ALCEDO_UI_ROOT}/edit_viewer/crop_geometry.cpp"
        "${ALCEDO_UI_ROOT}/edit_viewer/view_transform_controller.cpp"
        "${ALCEDO_UI_ROOT}/edit_viewer/crop_interaction_controller.cpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/edit_viewer_overlay_geometry.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/viewer_state.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/viewport_mapper.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/crop_geometry.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/view_transform_controller.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/crop_interaction_controller.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/frame_sink.hpp"
    PUBLIC_DEPS
        Qt6::Core
        Qt6::Gui
        opencv_core
)

if(ALCEDO_REAL_WIDGET_EDITOR)
    set(EDIT_VIEWER_COMMON_SRCS
        "${ALCEDO_UI_ROOT}/edit_viewer/edit_viewer.cpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/edit_viewer.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/edit_viewer_surface.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/color_manager.hpp"
    )

    set(EDIT_VIEWER_BACKEND_SRCS)
    set(EDIT_VIEWER_PUBLIC_DEPS EditPipeline EditorViewerLogic Qt6::Widgets)

    if(ALCEDO_REAL_OPENGL_EDITOR)
        list(APPEND EDIT_VIEWER_BACKEND_SRCS
            "${ALCEDO_UI_ROOT}/edit_viewer/frame_mailbox.cpp"
            "${ALCEDO_UI_ROOT}/edit_viewer/gl_edit_viewer_surface.cpp"
            "${ALCEDO_UI_ROOT}/edit_viewer/opengl_viewer_renderer.cpp"
            "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/frame_mailbox.hpp"
            "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/gl_edit_viewer_surface.hpp"
        )
        list(APPEND EDIT_VIEWER_PUBLIC_DEPS OpenGL::GL Qt6::OpenGLWidgets)
    endif()

    if(ALCEDO_RHI_WIDGET_EDITOR)
        list(APPEND EDIT_VIEWER_BACKEND_SRCS
            "${ALCEDO_UI_ROOT}/edit_viewer/color_manager.cpp"
            "${ALCEDO_UI_ROOT}/edit_viewer/rhi_edit_viewer_surface.cpp"
            "${ALCEDO_INCLUDE_ROOT}/ui/edit_viewer/rhi_edit_viewer_surface.hpp"
        )
        if(APPLE)
            list(REMOVE_ITEM EDIT_VIEWER_BACKEND_SRCS
                "${ALCEDO_UI_ROOT}/edit_viewer/color_manager.cpp"
            )
            list(APPEND EDIT_VIEWER_BACKEND_SRCS
                "${ALCEDO_UI_ROOT}/edit_viewer/color_manager.mm"
            )
            set_source_files_properties(
                "${ALCEDO_UI_ROOT}/edit_viewer/color_manager.mm"
                PROPERTIES LANGUAGE OBJCXX
            )
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
                "${ALCEDO_UI_ROOT}/edit_viewer/shaders"
            FILES
                "${ALCEDO_UI_ROOT}/edit_viewer/shaders/rhi_image.vert"
                "${ALCEDO_UI_ROOT}/edit_viewer/shaders/rhi_image.frag"
        )
    endif()
endif()

if(COMMAND alcedo_configure_editor_rhi_legacy_viewer)
    alcedo_configure_editor_rhi_legacy_viewer()
endif()

def_library(BackgroundTaskController
    SOURCES
        "${ALCEDO_UI_SHELL_ROOT}/album_backend/background_task_controller.cpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/background_task_controller.hpp"
    PUBLIC_DEPS Qt6::Core
)

def_library(ImageAnalysisController
    SOURCES
        "${ALCEDO_UI_SHELL_ROOT}/album_backend/image_analysis_controller.cpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/image_analysis_controller.hpp"
    PUBLIC_DEPS ImageAnalysisService AiProviderProfile AiCredentialStore BackgroundTaskController
                UiLocalization Qt6::Core
)

add_library(AlbumBackendLib STATIC
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/application_module_host.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/project_module.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/library_module.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/album_thumbnail_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/interaction_policy_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/project_db_write_barrier.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/project_handler.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/thumbnail_manager.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/folder_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/image_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/image_analysis_environment.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/image_analysis_sink.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/stats_engine.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/search_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/semantic_generation_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/model_download_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/import_export.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/nikon_he_recovery_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_adjustment_models.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_tone_curve_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_tone_curve_item.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_color_temp_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_hls_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_cdl_trackball_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_cdl_trackball_item.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_lut_catalog_model.cpp"
    # Pure tone-curve math shared with the legacy QWidget curve editor.
    "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/curve.cpp"
    # Look-panel shared math / catalog (also used by the legacy QWidget editor).
    "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/color_temp.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/color_wheel.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/hls.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/lut_catalog.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_pipeline_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_task_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_history_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_journal_writer_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_checkpoint_store.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_thumbnail_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_render_scheduler_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/workspace_router.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/adjustment_transfer_controller.cpp"
    # Reusable dialog-free pipeline helpers for the QML open path.
    "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/controllers/image_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/controllers/pipeline_controller.cpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/application_module_host.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/project_module.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/library_module.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/album_catalog.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/ui_status_sink.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/album_types.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/project_db_write_barrier.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/path_utils.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/project_handler.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/thumbnail_manager.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/folder_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/image_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/stats_engine.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/search_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/model_download_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/import_export.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/nikon_he_recovery_types.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_tone_curve_item.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_color_temp_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_hls_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_cdl_trackball_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_cdl_trackball_item.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_lut_catalog_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/curve.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/color_temp.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/color_wheel.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/hls.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/lut_catalog.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_task_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_history_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_journal_writer_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_checkpoint_store.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_thumbnail_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/workspace_router.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/controllers/image_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/controllers/pipeline_controller.hpp"
)

target_include_directories(AlbumBackendLib
    PUBLIC
        "${ALCEDO_INCLUDE_ROOT}"
    PRIVATE
        "${ALCEDO_UI_SHELL_ROOT}"
        "${ALCEDO_UI_SHELL_ROOT}/album_backend"
)

target_link_libraries(AlbumBackendLib
    PUBLIC
        UiLocalization
        BackgroundTaskController
        EditorRhiViewport
        Qt6::Quick
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Widgets
        ProjectService
        ImportService
        ExportService
        PipelineMgmtService
        AdjustmentTransferService
        EditorSessionService
        EditorAdjustmentPipeline
        EditorMiniGitMaterializer
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
        PipelineScheduler
        ImageLoader
        xxHash
        CudaDriverRequirements
)

if(TARGET EditViewer)
    target_link_libraries(AlbumBackendLib PUBLIC EditViewer)
endif()

set(ALCEDO_EDITOR_DIALOG_SRCS
    "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/editor_dialog_stub.cpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/editor_dialog.hpp"
)

if(ALCEDO_REAL_WIDGET_EDITOR)
    set(ALCEDO_EDITOR_DIALOG_SRCS
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/editor_dialog.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/dialog.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/dialog_core.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/dialog_pipeline.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/dialog_ui_state.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/state.cpp"
        # image_controller / pipeline_controller live in AlbumBackendLib.
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/controllers/history_controller.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/controllers/lut_controller.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/controllers/render_controller.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/frame/editor_frame_manager.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/history/editor_history_coordinator.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/render/editor_render_coordinator.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/session/editor_adjustment_session.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/histogram.cpp"
        # color_temp / color_wheel / hls / lut_catalog / curve live in AlbumBackendLib.
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/lens_calib.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/geometry.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/pipeline_io.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/modules/versioning.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/scope/scope_panel.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/scope/scope_plot_widgets.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/scope/scope_renderer.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/display_transform_panel_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/collapsible_section_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/editor_control_panel_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/editor_viewer_pane.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/geometry_panel_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/look_control_panel_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/raw_decode_panel_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/tone_control_panel_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/versioning_panel_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/spinner.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/histogram_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/trackball.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/tone_curve_widget.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/history_cards.cpp"
        "${ALCEDO_UI_SHELL_ROOT}/editor_dialog/widgets/lut_browser_widget.cpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/controllers/lut_controller.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/lut_catalog.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/lut_browser_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/dialog.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/dialog_internal.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/editor_dialog.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/state.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/controllers/image_controller.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/controllers/pipeline_controller.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/controllers/history_controller.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/controllers/render_controller.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/frame/editor_frame_manager.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/history/editor_history_coordinator.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/render/editor_render_coordinator.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/session/adjustment_panel.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/session/adjustment_patch.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/session/adjustment_snapshot.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/session/editor_adjustment_session.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/state/tone_adjustment_state.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/state/color_temp_adjustment_state.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/state/look_adjustment_state.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/state/display_transform_adjustment_state.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/state/geometry_adjustment_state.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/state/raw_decode_adjustment_state.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/pipeline/adjustment_pipeline_adapter.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/pipeline/tone_pipeline_adapter.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/pipeline/color_temp_pipeline_adapter.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/pipeline/look_pipeline_adapter.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/pipeline/display_transform_pipeline_adapter.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/pipeline/geometry_pipeline_adapter.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/pipeline/raw_pipeline_adapter.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/histogram.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/color_temp.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/color_wheel.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/curve.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/lens_calib.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/geometry.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/hls.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/pipeline_io.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/modules/versioning.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/scope/scope_panel.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/scope/scope_plot_widgets.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/scope/scope_renderer.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/display_transform_panel_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/collapsible_section_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/editor_control_panel_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/editor_viewer_pane.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/geometry_panel_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/look_control_panel_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/raw_decode_panel_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/tone_control_panel_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/versioning_panel_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/spinner.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/histogram_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/trackball.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/tone_curve_widget.hpp"
        "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_dialog/widgets/history_cards.hpp"
    )
endif()

set(ALCEDO_MAIN_SOURCES
    "${ALCEDO_UI_SHELL_ROOT}/main.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/app_theme.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/shortcut_registry.cpp"
    "${ALCEDO_SRC_ROOT}/config/resource.qrc"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/app_theme.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/i18n.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/language_manager.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/shortcut_registry.hpp"
    "${ALCEDO_INCLUDE_ROOT}/utils/cuda/cuda_driver_requirements.hpp"
    ${ALCEDO_EDITOR_DIALOG_SRCS}
)
if(APPLE AND ALCEDO_MACOS_BUNDLE)
    add_executable(alcedo_main MACOSX_BUNDLE ${ALCEDO_MAIN_SOURCES})
else()
    add_executable(alcedo_main ${ALCEDO_MAIN_SOURCES})
endif()

if(APPLE)
    set(ALCEDO_MAIN_ICON_PNG "${ALCEDO_SRC_ROOT}/config/ICON/alcedo_icon.png")
    set(ALCEDO_MAIN_ICONSET_DIR "${ALCEDO_BINARY_ROOT}/alcedo_icon.iconset")
    set(ALCEDO_MAIN_ICON_ICNS "${ALCEDO_BINARY_ROOT}/alcedo_icon.icns")

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
    set(ALCEDO_MAIN_ICON_RC "${ALCEDO_SRC_ROOT}/config/ICON/alcedo_main.rc")
    if(EXISTS "${ALCEDO_MAIN_ICON_RC}")
        target_sources(alcedo_main PRIVATE "${ALCEDO_MAIN_ICON_RC}")
    else()
        message(WARNING "Windows app icon resource not found: ${ALCEDO_MAIN_ICON_RC}")
    endif()
endif()

target_include_directories(alcedo_main
    PRIVATE
        "${ALCEDO_INCLUDE_ROOT}"
        "${ALCEDO_UI_SHELL_ROOT}"
        "${ALCEDO_UI_SHELL_ROOT}/album_backend"
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

if(ALCEDO_REAL_WIDGET_EDITOR)
    target_link_libraries(alcedo_main PRIVATE EditHistoryMgmtService)
endif()

if(WIN32 AND ALCEDO_OPENCL_ENABLED)
    target_link_libraries(alcedo_main PRIVATE OpenGL::GL)
endif()

set(ALCEDO_MAIN_QML_MODULE_OPTIONS)
if(NOT ALCEDO_GENERATE_QMLTYPES)
    list(APPEND ALCEDO_MAIN_QML_MODULE_OPTIONS NO_GENERATE_QMLTYPES)
endif()

set(ALCEDO_MAIN_QML_FILES
    "${ALCEDO_UI_SHELL_ROOT}/qml/Main.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ExportQueueState.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AlbumExportDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdjustmentTransferDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SettingDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/WelcomeDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/NikonHeRecoveryDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SemanticGenerationDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdvancedContentAnalysisDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/DialogActionButton.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/IconButton.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/IconActionButton.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/CollapsibleSection.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ActivateModelDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SemanticGenerationSettingsPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AiProviderSettingsPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SeveritySegmentedSlider.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/util/SearchComboBox.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AboutPage.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ImportProgressRing.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/InspectorPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AlbumInspectorPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ImageInspectorPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/CollectionsPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/GlobalSearchDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/StatsCard.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/StarRatingFilter.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ImageContextMenu.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ThumbnailGridView.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/BackgroundTaskBar.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/BackgroundTaskPopover.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/WorkspaceHost.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/LibraryWorkspace.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorWorkspace.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorWorkspaceNavigation.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorAdjustmentTransferActions.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorFilmstrip.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorHistoryVersionsRail.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorAdjustmentStack.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorTonePanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorLookPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdjustmentSlider.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdjustmentField.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdjustmentToggle.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdjustmentCombo.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdjustmentGroup.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdjustmentResetButton.qml"
)
set(ALCEDO_MAIN_QML_RESOURCES
    "${ALCEDO_UI_SHELL_ROOT}/qml/util/qmldir"
)
foreach(ALCEDO_MAIN_QML_FILE IN LISTS ALCEDO_MAIN_QML_FILES ALCEDO_MAIN_QML_RESOURCES)
    file(RELATIVE_PATH ALCEDO_MAIN_QML_ALIAS "${ALCEDO_UI_SHELL_ROOT}" "${ALCEDO_MAIN_QML_FILE}")
    set_source_files_properties("${ALCEDO_MAIN_QML_FILE}"
        PROPERTIES QT_RESOURCE_ALIAS "${ALCEDO_MAIN_QML_ALIAS}"
    )
endforeach()

qt_add_qml_module(alcedo_main
    URI Alcedo.Main
    VERSION 1.0
    ${ALCEDO_MAIN_QML_MODULE_OPTIONS}
    QML_FILES ${ALCEDO_MAIN_QML_FILES}
    RESOURCES ${ALCEDO_MAIN_QML_RESOURCES}
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
    "${ALCEDO_UI_SHELL_ROOT}/i18n/alcedo_main_en.ts"
    "${ALCEDO_UI_SHELL_ROOT}/i18n/alcedo_main_zh_CN.ts"
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
        "${ALCEDO_BINARY_ROOT}"
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

    set(ALCEDO_MIND_BUILD_STAMP "${ALCEDO_BINARY_ROOT}/alcedo_mind_sidecar.stamp")
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

set(ALCEDO_MAIN_FONT_SRC "${ALCEDO_SRC_ROOT}/config/fonts/main_Inter.ttf")
set(ALCEDO_MAIN_ZH_FONT_SRC "${ALCEDO_SRC_ROOT}/config/fonts/main_NotoSans_zh.ttf")
if(APPLE AND ALCEDO_MACOS_BUNDLE)
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

# aria2c is bundled next to alcedo_main for accelerated model downloads.
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
