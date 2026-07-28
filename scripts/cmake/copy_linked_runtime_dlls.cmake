if(NOT DEFINED ALCEDO_RUNTIME_DEST OR "${ALCEDO_RUNTIME_DEST}" STREQUAL "")
  message(FATAL_ERROR "ALCEDO_RUNTIME_DEST is required")
endif()

file(MAKE_DIRECTORY "${ALCEDO_RUNTIME_DEST}")

if(DEFINED ALCEDO_RUNTIME_DLLS AND NOT "${ALCEDO_RUNTIME_DLLS}" STREQUAL "")
  string(REPLACE "," ";" _alcedo_runtime_dlls "${ALCEDO_RUNTIME_DLLS}")
  foreach(_alcedo_runtime_dll IN LISTS _alcedo_runtime_dlls)
    if(EXISTS "${_alcedo_runtime_dll}")
      get_filename_component(_alcedo_runtime_dll_name "${_alcedo_runtime_dll}" NAME)
      file(COPY_FILE
        "${_alcedo_runtime_dll}"
        "${ALCEDO_RUNTIME_DEST}/${_alcedo_runtime_dll_name}"
        ONLY_IF_DIFFERENT
      )
    else()
      message(FATAL_ERROR "Linked runtime DLL does not exist: ${_alcedo_runtime_dll}")
    endif()
  endforeach()
endif()

if(DEFINED ALCEDO_RUNTIME_EXECUTABLE AND EXISTS "${ALCEDO_RUNTIME_EXECUTABLE}")
  string(REPLACE "," ";" _alcedo_runtime_search_dirs "${ALCEDO_RUNTIME_SEARCH_DIRS}")
  file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${ALCEDO_RUNTIME_EXECUTABLE}"
    DIRECTORIES ${_alcedo_runtime_search_dirs}
    RESOLVED_DEPENDENCIES_VAR _alcedo_resolved_runtime_dlls
    UNRESOLVED_DEPENDENCIES_VAR _alcedo_unresolved_runtime_dlls
    PRE_EXCLUDE_REGEXES
      "api-ms-win-.*"
      "ext-ms-.*"
    POST_EXCLUDE_REGEXES
      ".*[\\\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\\\/].*"
      ".*[\\\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\/].*"
      ".*[\\\\/][Ss][Yy][Ss][Ww][Oo][Ww]64[\\\\/].*"
  )

  foreach(_alcedo_runtime_dll IN LISTS _alcedo_resolved_runtime_dlls)
    get_filename_component(_alcedo_runtime_dll_name "${_alcedo_runtime_dll}" NAME)
    file(COPY_FILE
      "${_alcedo_runtime_dll}"
      "${ALCEDO_RUNTIME_DEST}/${_alcedo_runtime_dll_name}"
      ONLY_IF_DIFFERENT
    )
  endforeach()

  if(_alcedo_unresolved_runtime_dlls)
    list(JOIN _alcedo_unresolved_runtime_dlls "\n  " _alcedo_unresolved_text)
    message(FATAL_ERROR "Unresolved linked runtime DLLs:\n  ${_alcedo_unresolved_text}")
  endif()
endif()
