# Automation protocol, command registry, and local socket server tests.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")
include(GoogleTest)

# LABEL replaces the ctest label "automation"; CATEGORIES adds test categories to "ui".
function(alcedo_add_automation_test target_name)
  cmake_parse_arguments(ARG "" "LABEL" "SOURCES;LIBRARIES;CATEGORIES" ${ARGN})
  add_executable(${target_name}
      ${ARG_SOURCES}
  )
  target_include_directories(${target_name} PRIVATE ${ALCEDO_TEST_DIR})
  target_link_libraries(${target_name}
      PRIVATE AlcedoInProcessTestMain ${ARG_LIBRARIES} GTest::gtest Qt6::Core Qt6::Test)
  # gtest_discover_tests keeps only the first entry of a label list (it splits the list into
  # separate properties), so each target has one label: LABEL, or "automation" without it.
  set(_label "automation")
  if(ARG_LABEL)
    set(_label "${ARG_LABEL}")
  endif()
  gtest_discover_tests(${target_name}
      TEST_PREFIX "${target_name}."
      DISCOVERY_MODE PRE_TEST
      DISCOVERY_TIMEOUT 120
      PROPERTIES LABELS "${_label}")
  alcedo_register_test_target(${target_name} ui ${ARG_CATEGORIES})
endfunction()

alcedo_add_automation_test(AutomationProtocolTest
    SOURCES automation_protocol_test.cpp
    LIBRARIES AutomationProtocol)

alcedo_add_automation_test(AutomationCommandRegistryTest
    SOURCES automation_command_registry_test.cpp
    LIBRARIES AutomationHostLib)

# The exclusive create of command output files (issue #401).
alcedo_add_automation_test(AutomationNewOutputFileTest
    SOURCES automation_new_output_file_test.cpp
    LIBRARIES AutomationHostLib Qt6::Gui
    LABEL ci_automation_flow
    CATEGORIES ci_automation)

alcedo_add_automation_test(AutomationServerTest
    SOURCES automation_server_test.cpp
    LIBRARIES AutomationHostLib Qt6::Network)

alcedo_add_automation_test(AutomationSessionFileTest
    SOURCES automation_session_file_test.cpp
    LIBRARIES AutomationProtocol)

# A small session host that `alcedo-cli session start --host-binary` starts in place of
# alcedo_main. It answers session.ping and exits on session.shutdown.
add_executable(alcedo_cli_test_host alcedo_cli_test_host.cpp)
target_include_directories(alcedo_cli_test_host PRIVATE ${ALCEDO_TEST_DIR})
target_link_libraries(alcedo_cli_test_host PRIVATE AutomationHostLib Qt6::Core Qt6::Network)

# Runs the real alcedo-cli program against servers inside the test process, and against
# alcedo_cli_test_host for `session start`.
alcedo_add_automation_test(AlcedoCliTest
    SOURCES alcedo_cli_test.cpp
    LIBRARIES AutomationHostLib Qt6::Network)
target_compile_definitions(AlcedoCliTest PRIVATE
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>"
    ALCEDO_CLI_TEST_HOST_PATH="$<TARGET_FILE:alcedo_cli_test_host>"
    ALCEDO_APP_VERSION="${PROJECT_VERSION}")
add_dependencies(AlcedoCliTest alcedo_cli alcedo_cli_test_host)

alcedo_add_automation_test(HeadlessFrameSinkTest
    SOURCES headless_frame_sink_test.cpp
    LIBRARIES AutomationHostLib)

# Parses the headless options in this process and starts the real alcedo_main --headless.
alcedo_add_automation_test(HeadlessHostTest
    SOURCES headless_host_test.cpp
    LIBRARIES AutomationHostLib)
target_compile_definitions(HeadlessHostTest PRIVATE
    ALCEDO_MAIN_PATH="$<TARGET_FILE:alcedo_main>")
add_dependencies(HeadlessHostTest alcedo_main)

# Starts, queries, and stops real headless sessions through alcedo-cli, and opens a CI RAW file
# in an in-process host with the headless sink. Needs the GPU editor backend.
alcedo_add_automation_test(AutomationHeadlessSessionTest
    SOURCES automation_headless_session_test.cpp
    LIBRARIES AutomationHostLib)
target_compile_definitions(AutomationHeadlessSessionTest PRIVATE
    ALCEDO_MAIN_PATH="$<TARGET_FILE:alcedo_main>"
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>")
add_dependencies(AutomationHeadlessSessionTest alcedo_main alcedo_cli)

# A gui session in this process: agent control, the user adapters, and the notifications of a
# watching socket client on a CI RAW file in the editor. Needs the GPU editor backend.
alcedo_add_automation_test(AutomationGuiSessionTest
    SOURCES automation_gui_session_test.cpp
    LIBRARIES AutomationHostLib Qt6::Network)

# The alcedo skill and its command reference live in https://github.com/zidage/alcedo-skills.
# This repository does not pin a commit of it. Set ALCEDO_SKILLS_DIR to a local checkout to
# compare its commands.json and editor_catalog.json with the schemas of a GUI session of this
# build, and to check its generated reference blocks.
set(ALCEDO_SKILLS_DIR "" CACHE PATH "Local checkout of the alcedo-skills repository")
if(ALCEDO_SKILLS_DIR)
  alcedo_add_automation_test(AutomationReferenceDocsTest
      SOURCES automation_reference_docs_test.cpp
      LIBRARIES AutomationHostLib Qt6::Network)
  target_compile_definitions(AutomationReferenceDocsTest PRIVATE
      ALCEDO_SKILLS_DIR="${ALCEDO_SKILLS_DIR}"
      ALCEDO_REFERENCE_DOCS_DIR="${ALCEDO_SKILLS_DIR}/skills/alcedo/references/reference"
      ALCEDO_REFERENCE_OUT_DIR="${CMAKE_BINARY_DIR}/automation_reference")

  # The generated blocks of the command reference are up to date and every command has a
  # section.
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME AutomationReferenceDocsGenerated
        COMMAND Python3::Interpreter
            "${CMAKE_SOURCE_DIR}/scripts/automation/generate_cli_reference.py"
            --skills-dir "${ALCEDO_SKILLS_DIR}" --check)
    set_tests_properties(AutomationReferenceDocsGenerated PROPERTIES LABELS "automation")
  endif()
endif()

# project.* commands through the command registry of an in-process headless session.
alcedo_add_automation_test(AutomationProjectCommandsTest
    SOURCES automation_project_commands_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)

# library.* and tasks.* parameter and state rules in an in-process headless session.
alcedo_add_automation_test(ImportSourceReplacementRenderingTest
    SOURCES import_source_replacement_rendering_test.cpp
    LIBRARIES AutomationHostLib OpenImageIO::OpenImageIO
    LABEL ci_automation_flow
    CATEGORIES ci_automation)

alcedo_add_automation_test(AutomationLibraryCommandsTest
    SOURCES automation_library_commands_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)

# adjustments.* parameter rules and the background Paste of library photos in an in-process
# headless session.
alcedo_add_automation_test(AutomationAdjustmentCommandsTest
    SOURCES automation_adjustment_commands_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)

# lut.list through the command registry, with a temporary LUT library root.
alcedo_add_automation_test(AutomationLutCommandsTest
    SOURCES automation_lut_commands_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)

# editor.catalog and film.list through the command registry. With the film print resources the
# balance lights of film.list are checked against the stock bundle.
alcedo_add_automation_test(AutomationEditorCommandsTest
    SOURCES automation_editor_commands_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)
if(ALCEDO_FILM_PRINT_RESOURCE_DIR)
  target_compile_definitions(AutomationEditorCommandsTest PRIVATE
      ALCEDO_FILM_PRINT_RESOURCE_DIR="${ALCEDO_FILM_PRINT_RESOURCE_DIR}")
endif()

# The library workflow through real alcedo-cli and alcedo_main --headless processes. A failed
# test copies the session logs to automation-failures/ in the build tree; CI uploads them.
alcedo_add_automation_test(AutomationLibraryE2ETest
    SOURCES automation_library_e2e_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)
target_compile_definitions(AutomationLibraryE2ETest PRIVATE
    ALCEDO_MAIN_PATH="$<TARGET_FILE:alcedo_main>"
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>"
    ALCEDO_AUTOMATION_FAILURE_DIR="${CMAKE_BINARY_DIR}/automation-failures")
add_dependencies(AutomationLibraryE2ETest alcedo_main alcedo_cli)

# Editor commands and render.preview through real alcedo-cli and alcedo_main --headless
# processes on a CI RAW file. Needs the GPU editor backend.
alcedo_add_automation_test(AutomationEditorE2ETest
    SOURCES automation_editor_e2e_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)
target_compile_definitions(AutomationEditorE2ETest PRIVATE
    ALCEDO_MAIN_PATH="$<TARGET_FILE:alcedo_main>"
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>"
    ALCEDO_AUTOMATION_FAILURE_DIR="${CMAKE_BINARY_DIR}/automation-failures")
add_dependencies(AutomationEditorE2ETest alcedo_main alcedo_cli)

# Version commands and commit attribution through real alcedo-cli and alcedo_main --headless
# processes on a CI RAW file. Needs the GPU editor backend.
alcedo_add_automation_test(AutomationVersionsE2ETest
    SOURCES automation_versions_e2e_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)
target_compile_definitions(AutomationVersionsE2ETest PRIVATE
    ALCEDO_MAIN_PATH="$<TARGET_FILE:alcedo_main>"
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>"
    ALCEDO_AUTOMATION_FAILURE_DIR="${CMAKE_BINARY_DIR}/automation-failures")
add_dependencies(AutomationVersionsE2ETest alcedo_main alcedo_cli)

# adjustments.copy and adjustments.paste through real alcedo-cli and alcedo_main --headless
# processes on two CI RAW files. Needs the GPU editor backend.
alcedo_add_automation_test(AutomationAdjustmentsE2ETest
    SOURCES automation_adjustments_e2e_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)
target_compile_definitions(AutomationAdjustmentsE2ETest PRIVATE
    ALCEDO_MAIN_PATH="$<TARGET_FILE:alcedo_main>"
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>"
    ALCEDO_AUTOMATION_FAILURE_DIR="${CMAKE_BINARY_DIR}/automation-failures")
add_dependencies(AutomationAdjustmentsE2ETest alcedo_main alcedo_cli)

# export.* commands through real alcedo-cli and alcedo_main --headless processes on a CI RAW
# file. The written files are read back with OpenImageIO. Needs the GPU editor backend.
alcedo_add_automation_test(AutomationExportE2ETest
    SOURCES automation_export_e2e_test.cpp
    LIBRARIES AutomationHostLib OpenImageIO::OpenImageIO
    LABEL ci_automation_flow
    CATEGORIES ci_automation)
target_compile_definitions(AutomationExportE2ETest PRIVATE
    ALCEDO_MAIN_PATH="$<TARGET_FILE:alcedo_main>"
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>"
    ALCEDO_AUTOMATION_FAILURE_DIR="${CMAKE_BINARY_DIR}/automation-failures")
add_dependencies(AutomationExportE2ETest alcedo_main alcedo_cli)

# search.query through real alcedo-cli and alcedo_main --headless processes on a CI RAW file.
alcedo_add_automation_test(AutomationSearchE2ETest
    SOURCES automation_search_e2e_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)
target_compile_definitions(AutomationSearchE2ETest PRIVATE
    ALCEDO_MAIN_PATH="$<TARGET_FILE:alcedo_main>"
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>"
    ALCEDO_AUTOMATION_FAILURE_DIR="${CMAKE_BINARY_DIR}/automation-failures")
add_dependencies(AutomationSearchE2ETest alcedo_main alcedo_cli)
