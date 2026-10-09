# Automation protocol, command registry, and local socket server tests.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")
include(GoogleTest)

# LABEL replaces the ctest label "automation"; CATEGORIES adds test categories to "ui".
function(alcedo_add_automation_test target_name)
  cmake_parse_arguments(ARG "" "LABEL" "SOURCES;LIBRARIES;CATEGORIES" ${ARGN})
  add_executable(${target_name}
      ${ARG_SOURCES}
      ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
  )
  target_include_directories(${target_name} PRIVATE ${ALCEDO_TEST_DIR})
  target_link_libraries(${target_name} PRIVATE ${ARG_LIBRARIES} GTest::gtest Qt6::Core Qt6::Test)
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

alcedo_add_automation_test(AutomationServerTest
    SOURCES automation_server_test.cpp
    LIBRARIES AutomationHostLib Qt6::Network)

alcedo_add_automation_test(AutomationSessionFileTest
    SOURCES automation_session_file_test.cpp
    LIBRARIES AutomationProtocol)

# Runs the real alcedo-cli program against servers inside the test process.
alcedo_add_automation_test(AlcedoCliTest
    SOURCES alcedo_cli_test.cpp
    LIBRARIES AutomationHostLib Qt6::Network)
target_compile_definitions(AlcedoCliTest PRIVATE
    ALCEDO_CLI_PATH="$<TARGET_FILE:alcedo_cli>")
add_dependencies(AlcedoCliTest alcedo_cli)

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

# project.* commands through the command registry of an in-process headless session.
alcedo_add_automation_test(AutomationProjectCommandsTest
    SOURCES automation_project_commands_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)

# library.* and tasks.* parameter and state rules in an in-process headless session.
alcedo_add_automation_test(AutomationLibraryCommandsTest
    SOURCES automation_library_commands_test.cpp
    LIBRARIES AutomationHostLib
    LABEL ci_automation_flow
    CATEGORIES ci_automation)

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
