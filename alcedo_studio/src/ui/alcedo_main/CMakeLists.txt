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
        "${ALCEDO_UI_SHELL_ROOT}/album_backend/thumbnail_image_provider.cpp"
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
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_scope_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_scope_item.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_operation_publisher.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_adjustment_models.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_models.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_commit_presentation.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_geometry_math.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_lens_catalog_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_tone_curve_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_tone_curve_item.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_color_temp_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_hls_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_cdl_trackball_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_cdl_trackball_item.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_lut_catalog_model.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/modules/curve.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/modules/geometry.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/modules/lens_calib.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/modules/color_temp.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/modules/color_wheel.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/modules/hls.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/modules/lut_catalog.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_pipeline_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_task_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_history_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_state_detail.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_shared_helpers.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_projection.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_mutation.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_version_refs.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_transfer.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_history_checkpoint.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_journal_writer_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_checkpoint_store.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_thumbnail_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/editor_session_render_scheduler_port.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/workspace_router.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/album_backend/adjustment_transfer_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/controllers/image_controller.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/editor_support/controllers/pipeline_controller.cpp"
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
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/thumbnail_image_provider.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/folder_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/image_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/stats_engine.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/search_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/model_download_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/import_export.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/nikon_he_recovery_types.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_action_availability_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_scope_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_scope_item.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_operation_publisher.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_models.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_commit_presentation.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_geometry_math.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_lens_catalog_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_tone_curve_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_tone_curve_item.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_color_temp_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_hls_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_cdl_trackball_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_cdl_trackball_item.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_lut_catalog_model.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/modules/curve.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/modules/color_temp.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/modules/color_wheel.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/modules/geometry.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/modules/hls.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/modules/lens_calib.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/modules/lut_catalog.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/cdl_wheel_state.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_task_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_history_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_state_detail.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_projection.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_mutation.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_version_refs.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_transfer.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_history_checkpoint.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_journal_writer_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_checkpoint_store.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_thumbnail_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/workspace_router.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/controllers/image_controller.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/editor_support/controllers/pipeline_controller.hpp"
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
        UpdateService
        SleeveFilterService
        FSService
        EditPipeline
        PipelineScheduler
        ImageLoader
        xxHash
        CudaDriverRequirements
)

# The editor shell is QML-only. Widgets remains private to the album backend for
# the native project and Nikon recovery file dialogs.
target_link_libraries(AlbumBackendLib PRIVATE Qt6::Widgets)

set(ALCEDO_MAIN_SOURCES
    "${ALCEDO_UI_SHELL_ROOT}/main.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/application_module_qml_types.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/app_theme.cpp"
    "${ALCEDO_UI_SHELL_ROOT}/shortcut_registry.cpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/application_module_qml_types.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/app_theme.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/i18n.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/language_manager.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/shortcut_registry.hpp"
    "${ALCEDO_INCLUDE_ROOT}/utils/cuda/cuda_driver_requirements.hpp"
)
if(WIN32)
    configure_file(
        "${ALCEDO_SRC_ROOT}/config/ICON/alcedo_version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/alcedo_version.rc"
        @ONLY
    )
    list(APPEND ALCEDO_MAIN_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/alcedo_version.rc")
endif()
if(APPLE AND ALCEDO_MACOS_BUNDLE)
    add_executable(alcedo_main MACOSX_BUNDLE ${ALCEDO_MAIN_SOURCES})
else()
    add_executable(alcedo_main ${ALCEDO_MAIN_SOURCES})
endif()

add_executable(alcedo_update_installer
    "${ALCEDO_SRC_ROOT}/app/update_installer_main.cpp"
)
target_link_libraries(alcedo_update_installer PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(alcedo_update_installer PRIVATE UpdateInstallerWindows)
endif()
set_target_properties(alcedo_update_installer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ALCEDO_BINARY_ROOT}"
)
if(APPLE)
    set_property(TARGET alcedo_update_installer APPEND PROPERTY
        INSTALL_RPATH "@loader_path/../Frameworks")
endif()
alcedo_copy_linked_runtime_dlls(alcedo_update_installer)

add_executable(alcedo_update_signer
    "${ALCEDO_SRC_ROOT}/app/update_signer_main.cpp"
)
target_link_libraries(alcedo_update_signer PRIVATE Qt6::Core unofficial::ed25519::ed25519)
set_target_properties(alcedo_update_signer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ALCEDO_BINARY_ROOT}"
)
alcedo_copy_linked_runtime_dlls(alcedo_update_signer)

if(WIN32)
    # Keep the development executable at the historical, stable location.
    # CMake Tools and developers launching from the build tree must use the
    # same executable that receives the first-party CUDA runtime DLLs.
    set_target_properties(alcedo_main PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${ALCEDO_BINARY_ROOT}"
    )

    # Make the build-tree executable directly runnable without a shell-specific
    # Qt environment. Packaging generates its own deployment configuration.
    get_filename_component(_alcedo_qt_runtime_prefix
        "${Qt6_DIR}/../../.." ABSOLUTE
    )
    file(GENERATE
        OUTPUT "${ALCEDO_BINARY_ROOT}/qt.conf"
        CONTENT
"[Paths]
Prefix = ${_alcedo_qt_runtime_prefix}
Plugins = plugins
QmlImports = qml
"
    )
endif()

if(APPLE)
    # Brand icon pipeline (Windows + macOS):
    #   scripts/build_app_icons.ps1  ->  cargo tauri icon
    # writes config/ICON/alcedo_icon.{png,ico,icns}. Prefer the committed ICNS
    # so Dock / Finder / DMG match the Windows mark without re-slicing on Apple
    # hosts; fall back to sips+iconutil when the ICNS asset is missing.
    set(ALCEDO_MAIN_ICON_PNG "${ALCEDO_SRC_ROOT}/config/ICON/alcedo_icon.png")
    set(ALCEDO_MAIN_ICON_ICNS_SRC "${ALCEDO_SRC_ROOT}/config/ICON/alcedo_icon.icns")
    set(ALCEDO_MAIN_ICONSET_DIR "${ALCEDO_BINARY_ROOT}/alcedo_icon.iconset")
    # Keep the bundled resource name distinct from legacy PuerhLab builds.
    # Launch Services caches application icons by bundle metadata and resource
    # identity; reusing alcedo_icon.icns allowed an old bird icon registered
    # under the same bundle identifier to remain visible after an upgrade.
    set(ALCEDO_MAIN_ICON_ICNS "${ALCEDO_BINARY_ROOT}/alcedo_studio_or.icns")

    if(EXISTS "${ALCEDO_MAIN_ICON_ICNS_SRC}")
        add_custom_command(
            OUTPUT "${ALCEDO_MAIN_ICON_ICNS}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ALCEDO_MAIN_ICON_ICNS_SRC}"
                "${ALCEDO_MAIN_ICON_ICNS}"
            DEPENDS "${ALCEDO_MAIN_ICON_ICNS_SRC}"
            COMMENT "Staging macOS app icon from ${ALCEDO_MAIN_ICON_ICNS_SRC}"
            VERBATIM
        )
    else()
        find_program(ALCEDO_SIPS_EXECUTABLE sips REQUIRED)
        find_program(ALCEDO_ICONUTIL_EXECUTABLE iconutil REQUIRED)

        if(NOT EXISTS "${ALCEDO_MAIN_ICON_PNG}")
            message(FATAL_ERROR
                "macOS app icon missing. Expected ${ALCEDO_MAIN_ICON_ICNS_SRC} "
                "or ${ALCEDO_MAIN_ICON_PNG}. Generate with: "
                "powershell -File scripts/build_app_icons.ps1 "
                "(or cargo tauri icon <source.png>).")
        endif()

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
    endif()

    add_custom_target(AlcedoStudioMainIcon DEPENDS "${ALCEDO_MAIN_ICON_ICNS}")
    add_dependencies(alcedo_main AlcedoStudioMainIcon)
    set_source_files_properties("${ALCEDO_MAIN_ICON_ICNS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(alcedo_main PRIVATE "${ALCEDO_MAIN_ICON_ICNS}")
    # CFBundleIconFile — Finder, Dock, and Launchpad use this for the .app.
    set_target_properties(alcedo_main PROPERTIES
        MACOSX_BUNDLE_ICON_FILE "alcedo_studio_or.icns"
    )
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
        # QApplication remains the process host; this is not an editor dependency.
        Qt6::Widgets
        Qt6::Svg
        Qt6::QuickEffects
)
target_compile_definitions(alcedo_main PRIVATE
    ALCEDO_APP_VERSION="${PROJECT_VERSION}"
)

alcedo_copy_linked_runtime_dlls(alcedo_main)

if(WIN32 AND ALCEDO_OPENCL_ENABLED)
    target_link_libraries(alcedo_main PRIVATE OpenGL::GL)
endif()

set(ALCEDO_MAIN_QML_MODULE_OPTIONS)
if(NOT ALCEDO_GENERATE_QMLTYPES)
    list(APPEND ALCEDO_MAIN_QML_MODULE_OPTIONS NO_GENERATE_QMLTYPES)
endif()

set(ALCEDO_MAIN_QML_FILES
    "${ALCEDO_UI_SHELL_ROOT}/qml/Main.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/TopToolbar.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/WindowCaptionButtons.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/InspectorToggleButton.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/WindowResizeHandles.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/NotificationSnackbar.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/DeleteConfirmDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/BlurredOverlay.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AcceleratorPreparationOverlay.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ProjectLoadingOverlay.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ImportProgressOverlay.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SelectionState.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ImageActionsController.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/WindowAnimations.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ProjectLaunchController.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ShellSignals.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AppDialogs.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/FolderImportConfirmDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorCloseConfirmDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ExportQueueState.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ExportInspectorPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ExportNamingEditor.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ThemedProgressBar.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdjustmentTransferDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SettingDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/WelcomeDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/UpdateNotice.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/UpdatesSettingsPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/CacheSettingsPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/NikonHeRecoveryDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SemanticGenerationDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AdvancedContentAnalysisDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/DialogActionButton.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ThemeCheckBox.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/IconButton.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/IconActionButton.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SlidingIconNav.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/CollapsibleSection.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/SegmentedCardSwitcher.qml"
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
    "${ALCEDO_UI_SHELL_ROOT}/qml/AppContextMenu.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AppMenuItem.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/AppMenuSeparator.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ImageContextMenu.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/ThumbnailGridView.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/BackgroundTaskBar.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/BackgroundTasksDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/WorkspaceHost.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/LibraryWorkspace.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorWorkspace.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorWorkspaceNavigation.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorSaveRecoveryBar.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorAdjustmentTransferActions.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorFilmstrip.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorFilmstripThumbnailCard.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorWorkspaceRail.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorHistoryTransactionsPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorHistoryTransactionEntry.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorHistoryCommitGraphNode.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorVersionsPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorMergeDialog.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorMergeChoiceButton.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorMergeConflictCard.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorMergeJsonValue.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorAdjustmentStack.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorScopePanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorTonePanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorLookPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/LUTPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorDisplayTransformPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorGeometryPanel.qml"
    "${ALCEDO_UI_SHELL_ROOT}/qml/EditorRawDecodePanel.qml"
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

add_library(AlcedoMainQml STATIC
    "${ALCEDO_SRC_ROOT}/config/resource.qrc"
)
target_include_directories(AlcedoMainQml PUBLIC "${ALCEDO_INCLUDE_ROOT}")
target_link_libraries(AlcedoMainQml
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
        Qt6::QuickEffects
        Qt6::Svg
)

set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
qt_add_qml_module(AlcedoMainQml
    URI Alcedo.Main
    VERSION 1.0
    ${ALCEDO_MAIN_QML_MODULE_OPTIONS}
    QML_FILES ${ALCEDO_MAIN_QML_FILES}
    RESOURCES ${ALCEDO_MAIN_QML_RESOURCES}
)
set_target_properties(AlcedoMainQml PROPERTIES
    QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode"
    QT_QML_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Alcedo/Main"
)

if(COMMAND qt_finalize_target)
    qt_finalize_target(AlcedoMainQml)
endif()

target_link_libraries(alcedo_main PRIVATE AlcedoMainQml AlcedoMainQmlplugin)

set(ALCEDO_TEST_HOST_SOURCES
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/main.cpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/probe_json.cpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/probe_json.hpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/probe_item_tree.cpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/probe_item_tree.hpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/probe_input_injector.cpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/probe_input_injector.hpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/probe_property_wait.cpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/probe_property_wait.hpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/test_probe.cpp"
    "${ALCEDO_UI_ROOT}/alcedo_studio_test_host/test_probe.hpp"
    "${ALCEDO_UI_SHELL_ROOT}/application_module_qml_types.cpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/application_module_qml_types.hpp"
    "${ALCEDO_UI_SHELL_ROOT}/app_theme.cpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/app_theme.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/i18n.hpp"
    "${ALCEDO_INCLUDE_ROOT}/ui/alcedo_main/language_manager.hpp"
    "${ALCEDO_INCLUDE_ROOT}/utils/cuda/cuda_driver_requirements.hpp"
)
add_executable(alcedo_studio_test_host ${ALCEDO_TEST_HOST_SOURCES})
set_target_properties(alcedo_studio_test_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ALCEDO_BINARY_ROOT}"
)
target_include_directories(alcedo_studio_test_host
    PRIVATE
        "${ALCEDO_INCLUDE_ROOT}"
        "${ALCEDO_UI_ROOT}/alcedo_studio_test_host"
        "${ALCEDO_UI_SHELL_ROOT}"
        "${ALCEDO_UI_SHELL_ROOT}/album_backend"
)
target_link_libraries(alcedo_studio_test_host
    PRIVATE
        AlbumBackendLib
        AlcedoMainQml
        AlcedoMainQmlplugin
        UiLocalization
        Qt6::Quick
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::QuickDialogs2
        Qt6::Widgets
        Qt6::Network
        Qt6::Svg
        Qt6::QuickEffects
)
if(WIN32 AND ALCEDO_OPENCL_ENABLED)
    target_link_libraries(alcedo_studio_test_host PRIVATE OpenGL::GL)
endif()
alcedo_copy_linked_runtime_dlls(alcedo_studio_test_host)
if(COMMAND qt_finalize_target)
    qt_finalize_target(alcedo_studio_test_host)
elseif(COMMAND qt_finalize_executable)
    qt_finalize_executable(alcedo_studio_test_host)
endif()
if(COMMAND qt_import_qml_plugins)
    qt_import_qml_plugins(alcedo_main)
    qt_import_qml_plugins(alcedo_studio_test_host)
endif()

if(APPLE)
    set_target_properties(alcedo_main PROPERTIES
        MACOSX_BUNDLE ${ALCEDO_MACOS_BUNDLE}
        MACOSX_BUNDLE_BUNDLE_NAME "${ALCEDO_MACOS_BUNDLE_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${ALCEDO_MACOS_BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${ALCEDO_BUILD_NUMBER}"
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
        UpdateService
    # QML_FILES are embedded by qt_add_qml_module(), but are not guaranteed to
    # be visible to lupdate through SOURCE_TARGETS when the module is changed
    # without a full configure. Keep this list derived from the QML module list
    # above so every user-visible qsTr() call is part of the normal update flow.
    SOURCES
        ${ALCEDO_MAIN_QML_FILES}
    TS_FILES
        ${ALCEDO_TRANSLATION_TS_FILES}
    LUPDATE_TARGET
        alcedo_main_lupdate
    LRELEASE_TARGET
        alcedo_main_lrelease
    LUPDATE_OPTIONS
        -no-obsolete
        -locations relative
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
)
target_link_libraries(alcedo_main PRIVATE AlcedoVersion)

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

    # Ensure every packaged metallib is built before POST_BUILD copy, even when
    # an intermediate static lib is not linked into a particular configuration.
    add_dependencies(alcedo_main
        MetalUtilsShaders
        MetalGeometryUtilsShaders
        LensCalibOpMetalShaders
        EditPipelineMetalShaders
        ScopeAnalyzerMetalShaders
        MetalToLinearRefShaders
        MetalDebayerRcdShaders
        MetalHighlightReconstructShaders
        MetalXTransInterpolateShaders
        MetalCvtRefSpaceShaders
        MetalDemosaicNetIoShaders
    )

    add_custom_command(TARGET alcedo_main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_alcedo_metallib_runtime_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ALCEDO_METAL_RUNTIME_LIBS}
            "${_alcedo_metallib_runtime_dir}"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

# Ship DemosaicNet weights next to the executable for build-tree and installed
# bundles. Runtime resolves config/models/{bayer,xtrans}.safetensors relative
# to the executable (see Metal/OpenCL/CUDA demosaicnet caches).
set(_alcedo_demosaicnet_model_src_dir "${ALCEDO_SRC_ROOT}/config/models")
set(_alcedo_demosaicnet_model_files
    "${_alcedo_demosaicnet_model_src_dir}/bayer.safetensors"
    "${_alcedo_demosaicnet_model_src_dir}/xtrans.safetensors"
)
if(APPLE AND ALCEDO_MACOS_BUNDLE)
    set(_alcedo_demosaicnet_model_dest_dir
        "$<TARGET_BUNDLE_CONTENT_DIR:alcedo_main>/MacOS/config/models")
else()
    set(_alcedo_demosaicnet_model_dest_dir "$<TARGET_FILE_DIR:alcedo_main>/config/models")
endif()
add_custom_command(TARGET alcedo_main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_alcedo_demosaicnet_model_dest_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${_alcedo_demosaicnet_model_files}
        "${_alcedo_demosaicnet_model_dest_dir}"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

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

# The linker signs the executable before the bundle resources above are copied.
# Re-sign the complete build-tree bundle last so Launch Services and Dock see a
# valid application identity during direct development launches as well as from
# installed/package builds.
if(APPLE AND ALCEDO_MACOS_BUNDLE AND
   NOT "${ALCEDO_MACOS_CODESIGN_IDENTITY}" STREQUAL "")
    find_program(ALCEDO_CODESIGN_EXECUTABLE codesign REQUIRED)
    if(ALCEDO_MACOS_CODESIGN_TIMESTAMP)
        set(_alcedo_build_codesign_timestamp --timestamp)
    else()
        set(_alcedo_build_codesign_timestamp --timestamp=none)
    endif()
    add_custom_command(TARGET alcedo_main POST_BUILD
        # Incremental builds do not clear files that were copied by an older
        # target definition. Remove the legacy icon resource before sealing
        # the bundle so only CFBundleIconFile's current resource can be used.
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_BUNDLE_CONTENT_DIR:alcedo_main>/Resources/alcedo_icon.icns"
        COMMAND "${ALCEDO_CODESIGN_EXECUTABLE}"
            --force
            --deep
            ${ALCEDO_MACOS_CODESIGN_OPTIONS}
            --sign "${ALCEDO_MACOS_CODESIGN_IDENTITY}"
            ${_alcedo_build_codesign_timestamp}
            "$<TARGET_BUNDLE_DIR:alcedo_main>"
        COMMENT "Signing complete macOS build-tree app bundle"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()
