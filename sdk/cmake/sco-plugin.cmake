# sco-plugin.cmake: build helpers for sco plugins. Part of the sco SDK.
#
#   include(<sdk>/cmake/sco-plugin.cmake)
#   sco_add_plugin(<id> SOURCES plugin.c [more.c ...] INI plugin.ini)   # native DLL plugin (C11)
#   sco_add_plugin(<id> SOURCES plugin.cpp INI plugin.ini)              # native DLL plugin (C++20,
#                                                                       # include/scosdk/; enable CXX)
#   sco_add_pack(<id> DIR <folder>)                                     # data pack or Lua plugin
#
# `cmake --install <build> --prefix <out>` then lays every plugin out the way the game reads
# it: <out>/data/plugins/<id>/plugin.ini plus the DLL or the pack's files. Copy that
# data/plugins/<id> folder into sc-offline's data/plugins/.
#
# The plugin file is always named <id>.dll, even on macOS and Linux, so plugin.ini's
# `entry = <id>.dll` is right everywhere. Only the Windows build runs in the game; the
# macOS and Linux builds exist so tools/sco-plugin-check can load them on a dev machine.

include_guard(GLOBAL)

set(_SCO_SDK_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The unpacked SDK zip has include/ next to cmake/. Inside the sco-core repository the SDK
# lives in sdk/ and the header in ../include/.
if(NOT SCO_SDK_INCLUDE_DIR)
  foreach(_dir "${_SCO_SDK_CMAKE_DIR}/../include" "${_SCO_SDK_CMAKE_DIR}/../../include")
    if(EXISTS "${_dir}/sco_api.h")
      get_filename_component(SCO_SDK_INCLUDE_DIR "${_dir}" ABSOLUTE)
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${SCO_SDK_INCLUDE_DIR}/sco_api.h")
  message(FATAL_ERROR "sco SDK: sco_api.h not found. Set SCO_SDK_INCLUDE_DIR to the SDK's include folder.")
endif()

# Only with a compiled language enabled: a pack-only project (project(... NONE), like the Lua
# and data-pack examples) has no pointer size and builds nothing. sco_add_plugin checks again.
if(DEFINED CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "sco plugins are 64-bit only. On Windows configure with -A x64.")
endif()

function(_sco_check_id id)
  string(LENGTH "${id}" _len)
  if(NOT id MATCHES "^[a-z0-9_]+$" OR _len GREATER 31)
    message(FATAL_ERROR "sco plugin id '${id}': use 1-31 characters from a-z, 0-9 and _")
  endif()
  if(id STREQUAL "sco" OR id STREQUAL "host" OR id STREQUAL "menu" OR id STREQUAL "game")
    message(FATAL_ERROR "sco plugin id '${id}' is reserved")
  endif()
endfunction()

function(sco_add_plugin id)
  cmake_parse_arguments(P "" "INI" "SOURCES" ${ARGN})
  _sco_check_id("${id}")
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "sco_add_plugin(${id}): sco plugins are 64-bit only. Enable C, and on Windows configure with -A x64.")
  endif()
  if(NOT P_SOURCES)
    message(FATAL_ERROR "sco_add_plugin(${id}): SOURCES is empty")
  endif()
  if(NOT P_INI)
    set(P_INI plugin.ini)
  endif()
  get_filename_component(_ini "${P_INI}" ABSOLUTE)
  if(NOT EXISTS "${_ini}")
    message(FATAL_ERROR "sco_add_plugin(${id}): ${P_INI} not found")
  endif()

  add_library(${id} MODULE ${P_SOURCES})
  target_include_directories(${id} PRIVATE "${SCO_SDK_INCLUDE_DIR}")
  set_target_properties(${id} PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    # C++ sources (the scosdk/ headers): C++20, and only the three sco_plugin_* exports visible.
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # Static C runtime, like sc-offline's dinput8.dll: players need no VC++ redistributable.
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
  if(MSVC)
    target_compile_options(${id} PRIVATE /W4 /WX)
  else()
    target_compile_options(${id} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  install(TARGETS ${id} LIBRARY DESTINATION "data/plugins/${id}")
  install(FILES "${_ini}" DESTINATION "data/plugins/${id}" RENAME plugin.ini)
endfunction()

function(sco_add_pack id)
  cmake_parse_arguments(P "" "DIR" "" ${ARGN})
  _sco_check_id("${id}")
  if(NOT P_DIR)
    set(P_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  get_filename_component(_dir "${P_DIR}" ABSOLUTE)
  if(NOT EXISTS "${_dir}/plugin.ini")
    message(FATAL_ERROR "sco_add_pack(${id}): ${_dir}/plugin.ini not found")
  endif()
  # A build folder inside the pack (cmake -B build in the pack's folder) is not part of it.
  # Folders named build or out are always left out. Any other build folder is found by comparing
  # real paths: Windows can spell one folder two ways (C:/Users/RUNNER~1 and C:/Users/runneradmin).
  file(REAL_PATH "${_dir}" _dir)
  file(REAL_PATH "${CMAKE_BINARY_DIR}" _bin)
  set(_skip_build PATTERN "build" EXCLUDE PATTERN "out" EXCLUDE)
  file(RELATIVE_PATH _rel "${_dir}" "${_bin}")
  if(NOT _rel STREQUAL "" AND NOT IS_ABSOLUTE "${_rel}" AND NOT _rel MATCHES "^\\.\\.(/|$)")
    string(REGEX REPLACE "/.*" "" _top "${_rel}")
    set(_esc "${_dir}/${_top}")
    foreach(_c "\\" "." "+" "*" "?" "^" "$" "(" ")" "[" "]" "|")
      string(REPLACE "${_c}" "\\${_c}" _esc "${_esc}")
    endforeach()
    list(APPEND _skip_build REGEX "^${_esc}(/.*)?$" EXCLUDE)
  endif()
  # Nothing to compile: the pack's folder is the plugin. Installed from the real path, the same
  # spelling the REGEX above was built from.
  install(DIRECTORY "${_dir}/" DESTINATION "data/plugins/${id}"
          PATTERN "CMakeLists.txt" EXCLUDE ${_skip_build})
endfunction()
