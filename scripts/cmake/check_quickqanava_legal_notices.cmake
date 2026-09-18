# Assert source-tree legal notices contain the QuickQanava BSD-3-Clause binary
# redistribution clause and the vendored bezier MIT text. Invoke with:
#   cmake -DALCEDO_SOURCE_DIR=<repo> -P scripts/cmake/check_quickqanava_legal_notices.cmake

if(NOT ALCEDO_SOURCE_DIR)
  get_filename_component(ALCEDO_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

function(alcedo_assert_file_contains path needle)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Required legal notice is missing: ${path}")
  endif()
  file(READ "${path}" _text)
  string(FIND "${_text}" "${needle}" _found)
  if(_found EQUAL -1)
    message(FATAL_ERROR
      "Legal notice ${path} does not contain required text: ${needle}")
  endif()
endfunction()

alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/third_party_licenses/QuickQanava-licence.txt"
  "Redistributions in binary form must reproduce")
alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/third_party_licenses/QuickQanava-licence.txt"
  "Benoit AUTHEMAN")
alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/third_party_licenses/QuickQanava-bezier-LICENSE.txt"
  "MIT License")
alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/third_party_licenses/QuickQanava-bezier-LICENSE.txt"
  "Permission is hereby granted")
alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/THIRD_PARTY_NOTICE.txt"
  "QuickQanava")
alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/THIRD_PARTY_NOTICE.txt"
  "bezier")
alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/NOTICE"
  "QuickQanava")
alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/LICENSE"
  "GNU GENERAL PUBLIC LICENSE")
alcedo_assert_file_contains(
  "${ALCEDO_SOURCE_DIR}/LICENSE"
  "Version 3")

message(STATUS
  "QuickQanava BSD-3-Clause and bezier MIT notice texts are present.")
