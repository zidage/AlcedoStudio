# Automation protocol, command registry, and local socket server tests.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")
include(GoogleTest)

function(alcedo_add_automation_test target_name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
  add_executable(${target_name}
      ${ARG_SOURCES}
      ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
  )
  target_include_directories(${target_name} PRIVATE ${ALCEDO_TEST_DIR})
  target_link_libraries(${target_name} PRIVATE ${ARG_LIBRARIES} GTest::gtest Qt6::Core Qt6::Test)
  gtest_discover_tests(${target_name}
      TEST_PREFIX "${target_name}."
      DISCOVERY_MODE PRE_TEST
      DISCOVERY_TIMEOUT 120
      PROPERTIES LABELS "automation")
  alcedo_register_test_target(${target_name} ui)
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
