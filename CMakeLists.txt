cmake_minimum_required(VERSION 3.20)
project(sco_core C CXX)

# sco-core libraries, sco-sigcheck and the unit tests. tools/test.sh and tools/test-win.sh stay the
# reference scripts (they add the TSan runs and the x86_64-pc-windows-msvc ABI pin); this is the
# same build for any CMake toolchain, MSVC included.
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
#
# Multi-config generators (Visual Studio): add --config <cfg> to the build and -C <cfg> to ctest.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_sco_top_level ON)
else()
  set(_sco_top_level OFF)
endif()

option(SCO_BUILD_TESTS "Build the sco-core unit tests and register them with CTest" ${_sco_top_level})
option(SCO_WERROR "Treat warnings in sco-core's own code as errors" ON)
# The kernel (sco_api, runtime, host, plugins, app, storage, ui, ipc, net, hook, vfs, engine, the
# signature engine, sco-lua) is game-agnostic. The Star Citizen game pack is everything else: the
# signature rows in src/game (sco_game_sc), the CryPak adapter (sco_pak), DataCore (sco_datacore,
# sco_datacore_service) and their tools (sco-sigcheck, sco-dcb). OFF builds and tests the kernel
# alone, with SCO_KERNEL_ONLY defined; CI's kernel-only job does, so nothing in the kernel can
# depend on the game pack without that job failing.
option(SCO_GAME_SC "Build the Star Citizen game pack (signature rows, CryPak adapter, DataCore)" ON)
if(NOT SCO_GAME_SC)
  add_compile_definitions(SCO_KERNEL_ONLY=1)
endif()
set(SCO_SANITIZE "" CACHE STRING "Sanitizers for project code, e.g. address,undefined (not MSVC)")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "sco-core is 64-bit only. On Windows configure with -A x64.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# Warnings (and sanitizers) for sco-core's own code. Vendored Lua gets neither.
function(sco_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8 $<$<BOOL:${SCO_WERROR}>:/WX>)
    # C4996 flags every standard fopen/strcpy as "unsafe" in favour of the non-portable _s
    # variants; the code stays ISO C/C++, so turn off that one CRT deprecation, nothing else.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra $<$<BOOL:${SCO_WERROR}>:-Werror>)
  endif()
endfunction()

function(sco_project_target target)
  sco_warnings(${target})
  if(SCO_SANITIZE AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=${SCO_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(${target} PUBLIC -fsanitize=${SCO_SANITIZE})
  endif()
endfunction()

set(SCO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ---- libraries --------------------------------------------------------------------------------

# Pattern scanning, the signature engine, log/status and PE files (kernel).
add_library(sco_core STATIC
  src/sco_scan.cpp src/sco_signatures.cpp src/sco_log_status.cpp src/sco_pe_file.cpp)
if(WIN32)
  target_sources(sco_core PRIVATE src/sco_image_win.cpp)
endif()
target_include_directories(sco_core PUBLIC "${SCO_INCLUDE_DIR}")
target_link_libraries(sco_core PUBLIC Threads::Threads)
sco_project_target(sco_core)

# The Star Citizen signature tables (game pack): every src/game/*_sigs.cpp row and
# sco::game::RegisterGameSignatures. The CryPak hooks are their own library (sco_pak): the rows and
# sco-sigcheck don't need hooks.
if(SCO_GAME_SC)
  file(GLOB SCO_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.cpp")
  list(FILTER SCO_GAME_SOURCES EXCLUDE REGEX "/pak_hooks\\.cpp$")
  add_library(sco_game_sc STATIC ${SCO_GAME_SOURCES})
  target_link_libraries(sco_game_sc PUBLIC sco_core)
  sco_project_target(sco_game_sc)
endif()

# Tasks, events and commands: the cross-thread runtime under the plugin ABI.
add_library(sco_runtime STATIC src/api/sco_tasks.cpp src/api/sco_events.cpp src/api/sco_commands.cpp
  src/api/sco_services.cpp src/api/sco_raw.cpp)
target_include_directories(sco_runtime PUBLIC "${SCO_INCLUDE_DIR}")
target_link_libraries(sco_runtime PUBLIC Threads::Threads)
sco_project_target(sco_runtime)

# Capability registry and the host's sco_api table.
add_library(sco_host STATIC src/api/sco_caps.cpp src/host/sco_host.cpp)
target_link_libraries(sco_host PUBLIC sco_runtime sco_core)
sco_project_target(sco_host)

# Plugin discovery, plugin.ini, the native loader (with the SEH guard on Windows), content index.
# Detours and near-code memory (sco/hook.h). Products link it; nothing else in sco-core needs it.
add_library(sco_hook STATIC src/hook/sco_hook.cpp)
target_include_directories(sco_hook PUBLIC "${SCO_INCLUDE_DIR}")
target_link_libraries(sco_hook PUBLIC Threads::Threads)
sco_project_target(sco_hook)

# Engine-agnostic spatial math (sco/engine/types.h, header-only) and the zone tree (sco/engine/zone.h)
# the host feeds each tick. No game offsets, no game memory.
add_library(sco_engine STATIC src/engine/zone.cpp)
target_include_directories(sco_engine PUBLIC "${SCO_INCLUDE_DIR}")
target_link_libraries(sco_engine PUBLIC Threads::Threads)
sco_project_target(sco_engine)

# Game-file overrides (sco/vfs.h): virtual files from splices, the reader arithmetic and the mount
# table. Engine-agnostic: no engine, no Windows; the CryPak adapter comes separately.
add_library(sco_vfs STATIC src/vfs/compose.cpp src/vfs/table.cpp)
target_include_directories(sco_vfs PUBLIC "${SCO_INCLUDE_DIR}")
target_link_libraries(sco_vfs PUBLIC Threads::Threads)
sco_project_target(sco_vfs)

# The CryPak adapter (sco/game/pak.h, game pack): the loader detour and the vtable slot swaps that
# serve sco::vfs mounts to the engine. Its rows are in sco_game_sc; the vfs.pak capability in sco_host.
if(SCO_GAME_SC)
  add_library(sco_pak STATIC src/game/pak_hooks.cpp)
  target_link_libraries(sco_pak PUBLIC sco_game_sc sco_core sco_hook sco_vfs sco_host)
  sco_project_target(sco_pak)
endif()

# The DataCore parser, validation and patcher (sco/datacore.h) and data packs (sco/datacore_pack.h):
# bytes in, tables and sco::vfs splices out. No engine, no Windows. Vendored toml++
# (third_party/tomlplusplus, see its README) is a private SYSTEM include of this library only; its
# warnings are also off around the include (src/datacore/toml.h).
if(SCO_GAME_SC)   # DataCore is game pack
add_library(sco_datacore STATIC src/datacore/datacore.cpp src/datacore/patch.cpp src/datacore/pack.cpp)
target_include_directories(sco_datacore PUBLIC "${SCO_INCLUDE_DIR}")
target_include_directories(sco_datacore SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlplusplus")
target_link_libraries(sco_datacore PUBLIC sco_vfs)
sco_project_target(sco_datacore)
endif()

add_library(sco_plugins STATIC
  src/plugins/manifest.cpp src/plugins/discover.cpp src/plugins/loader.cpp src/plugins/content.cpp)
if(WIN32)
  target_sources(sco_plugins PRIVATE src/plugins/guard_win.cpp)
endif()
target_link_libraries(sco_plugins PUBLIC sco_runtime sco_core ${CMAKE_DL_LIBS})
sco_project_target(sco_plugins)

# Vendored SQLite (third_party/sqlite, see its README): the amalgamation as-is, with conservative
# options, its own warnings off for this target only. Multi-thread mode: sco::storage serializes
# each connection itself.
set(SCO_SQLITE_OPTIONS
  SQLITE_THREADSAFE=2 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0
  SQLITE_TEMP_STORE=2 SQLITE_TRUSTED_SCHEMA=0 SQLITE_USE_URI=0 SQLITE_OMIT_SHARED_CACHE
  SQLITE_OMIT_DEPRECATED SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_ENABLE_API_ARMOR)
add_library(sco_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sco_sqlite SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
target_compile_definitions(sco_sqlite PRIVATE ${SCO_SQLITE_OPTIONS})
# -std=gnu11: the amalgamation uses POSIX functions that strict C11 headers hide.
set_target_properties(sco_sqlite PROPERTIES C_EXTENSIONS ON)
if(MSVC)
  target_compile_options(sco_sqlite PRIVATE /w)
else()
  target_compile_options(sco_sqlite PRIVATE -w)
endif()
if(SCO_SANITIZE AND NOT MSVC)
  target_compile_options(sco_sqlite PRIVATE -fsanitize=${SCO_SANITIZE})
  # SQLite calls through function pointers cast to a common type (its destructors), which
  # clang's -fsanitize=function (part of undefined) would flag inside SQLite itself.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(sco_sqlite PRIVATE -fno-sanitize=function)
  endif()
endif()
target_link_libraries(sco_sqlite PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(sco_sqlite PUBLIC m)
endif()

# sco.storage (sco/storage.h, include/sco_storage.h): per-plugin databases, a host-owned service.
add_library(sco_storage STATIC src/storage/storage.cpp)
target_link_libraries(sco_storage PUBLIC sco_host sco_sqlite)
sco_project_target(sco_storage)

# sco.ui (sco/ui.h, include/sco_ui.h): plugin tabs, overlays and badges and the hotkey table, a
# host-owned service. No renderer: the product draws what is registered.
add_library(sco_ui STATIC src/ui/ui.cpp)
target_link_libraries(sco_ui PUBLIC sco_host)
sco_project_target(sco_ui)

# sco.ipc (sco/ipc.h, include/sco_ipc.h over the MIT wire include/sc_ipc.h): named shared-memory
# channels to another program on the same PC, a host-owned service. The platform half is
# shm_win.cpp (the real target) or shm_posix.cpp (the host tests); the ring logic is platform-free.
add_library(sco_ipc STATIC src/ipc/ipc.cpp)
if(WIN32)
  target_sources(sco_ipc PRIVATE src/ipc/shm_win.cpp)
  target_link_libraries(sco_ipc PRIVATE advapi32)
else()
  target_sources(sco_ipc PRIVATE src/ipc/shm_posix.cpp)
  if(NOT APPLE)
    target_link_libraries(sco_ipc PRIVATE rt)
  endif()
endif()
target_link_libraries(sco_ipc PUBLIC sco_host)
sco_project_target(sco_ipc)

# sco.net core (sco/net/*.h): SHA-256/HMAC/PBKDF2 (sha2.c, plain C), the wire framing, reliable
# streams and the session over an abstract datagram transport. Platform-free (docs/net-wire.md).
add_library(sco_net STATIC src/net/sha2.c src/net/wire.cpp src/net/reliable.cpp src/net/core.cpp)
target_include_directories(sco_net PUBLIC "${SCO_INCLUDE_DIR}")
sco_project_target(sco_net)

# sco.net service (sco/net/session.h, include/sco_net.h): the network thread, channel ownership,
# quotas, the LAN rule (scope.cpp) and UDP, a host-owned service. The platform half is udp_win.cpp
# (Winsock, the real target) or udp_posix.cpp (the host tests).
add_library(sco_net_service STATIC src/net/service.cpp src/net/scope.cpp)
if(WIN32)
  target_sources(sco_net_service PRIVATE src/net/udp_win.cpp)
  target_link_libraries(sco_net_service PRIVATE ws2_32)
else()
  target_sources(sco_net_service PRIVATE src/net/udp_posix.cpp)
endif()
target_link_libraries(sco_net_service PUBLIC sco_net sco_host Threads::Threads)
sco_project_target(sco_net_service)

# sco.datacore (sco/datacore_service.h, include/sco_datacore.h): plugins queue DataCore overrides; the
# DataCore load applies data packs, saved patches and queued patches in plugin order.
if(SCO_GAME_SC)   # sco.datacore is game pack
add_library(sco_datacore_service STATIC src/datacore/service.cpp)
target_link_libraries(sco_datacore_service PUBLIC sco_datacore sco_host sco_plugins)
sco_project_target(sco_datacore_service)
endif()

# The host kit (sco/app.h): Start / Tick / Stop over the host table, caps, signatures and loader.
add_library(sco_app STATIC src/app/sco_app.cpp)
target_link_libraries(sco_app PUBLIC sco_host sco_plugins sco_core sco_storage sco_ui sco_ipc sco_net_service)
if(SCO_GAME_SC)
  # sco.datacore (Platform::dataCore) and the Star Citizen tables a product passes as
  # Platform::registerSignatures (sco::game::RegisterGameSignatures).
  target_link_libraries(sco_app PUBLIC sco_datacore_service sco_game_sc)
endif()
# The game pack's services (sco/game/services.h, docs/design/game-services.md): teleport.spatial
# today, published under the owner "game" when the product sets Platform::gameServices. They read
# game memory under SEH (__try), so Windows only; SCO_GAME_SERVICES tells sco_app they're there.
if(SCO_GAME_SC AND WIN32)
  add_library(sco_game_services STATIC src/game/services/reads.cpp src/game/services/spatial.cpp
                                       src/game/services/spawn.cpp src/game/services/missions.cpp
                                       src/game/services/actors.cpp src/game/services/vehicles.cpp)
  target_link_libraries(sco_game_services PUBLIC sco_game_sc sco_host sco_engine)
  target_compile_definitions(sco_game_services PUBLIC SCO_GAME_SERVICES=1)
  sco_project_target(sco_game_services)
  target_link_libraries(sco_app PUBLIC sco_game_services)
endif()
sco_project_target(sco_app)

# Vendored Lua 5.4 (plugins/lua/third_party/lua): built as-is, its own warnings off.
file(GLOB SCO_LUA_VENDOR_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/lua/third_party/lua/src/*.c")
list(FILTER SCO_LUA_VENDOR_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(sco_lua_vendor STATIC ${SCO_LUA_VENDOR_SOURCES})
target_include_directories(sco_lua_vendor PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/lua/third_party/lua/src")
if(MSVC)
  target_compile_options(sco_lua_vendor PRIVATE /w)
else()
  target_compile_options(sco_lua_vendor PRIVATE -w)
endif()
if(SCO_SANITIZE AND NOT MSVC)
  target_compile_options(sco_lua_vendor PRIVATE -fsanitize=${SCO_SANITIZE})
endif()
if(UNIX)
  target_link_libraries(sco_lua_vendor PUBLIC m)
endif()

# sco-lua: the sandboxed runtime for kind = lua plugins.
add_library(sco_lua STATIC plugins/lua/sco_lua.c)
target_include_directories(sco_lua PUBLIC "${SCO_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/plugins/lua")
target_link_libraries(sco_lua PUBLIC sco_lua_vendor)
sco_project_target(sco_lua)
if(NOT MSVC)
  target_compile_options(sco_lua PRIVATE -Wpedantic)
endif()

# ---- tools ------------------------------------------------------------------------------------

if(SCO_GAME_SC)
  add_executable(sco-sigcheck tools/sco-sigcheck.cpp)
  target_link_libraries(sco-sigcheck PRIVATE sco_game_sc)
  sco_project_target(sco-sigcheck)
endif()

# The real host kit, loader and sco-lua outside the game.
add_executable(sco-host-sim tools/sco-host-sim.cpp)
target_link_libraries(sco-host-sim PRIVATE sco_app sco_lua)
if(SCO_GAME_SC)
  target_link_libraries(sco-host-sim PRIVATE sco_datacore)   # the pack lint
endif()
sco_project_target(sco-host-sim)

# DataCore files on disk (docs/datacore.md): info and records.
if(SCO_GAME_SC)
add_executable(sco-dcb tools/sco-dcb.cpp)
target_link_libraries(sco-dcb PRIVATE sco_datacore)
sco_project_target(sco-dcb)
endif()

# ---- tests ------------------------------------------------------------------------------------

if(SCO_BUILD_TESTS)
  enable_testing()
  set(SCO_TEST_OUT "${CMAKE_CURRENT_BINARY_DIR}/tests/out")
  file(MAKE_DIRECTORY "${SCO_TEST_OUT}")

  # Plugin ABI pin (tests/abi_v1.c): compile-only, as C11 and as C++20, plus -fshort-enums where
  # the compiler has it. The abi_v1 test rebuilds these objects, so a broken pin fails ctest.
  set(_abi_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_v1.cpp")
  file(CONFIGURE OUTPUT "${_abi_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_v1.c\"\n")
  add_library(sco_abi_v1_c OBJECT tests/abi_v1.c)
  add_library(sco_abi_v1_cxx OBJECT "${_abi_cxx}")
  set(_abi_targets sco_abi_v1_c sco_abi_v1_cxx)
  if(NOT MSVC)
    add_library(sco_abi_v1_short_enums OBJECT tests/abi_v1.c)
    target_compile_options(sco_abi_v1_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_targets sco_abi_v1_short_enums)
  endif()
  foreach(t IN LISTS _abi_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(_multi_config)
    set(_abi_config --config $<CONFIG>)
  else()
    set(_abi_config)
  endif()
  add_test(NAME abi_v1
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_targets})

  # The sco.storage table pin (tests/abi_storage.c), the same three ways.
  set(_abi_storage_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_storage.cpp")
  file(CONFIGURE OUTPUT "${_abi_storage_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_storage.c\"\n")
  add_library(sco_abi_storage_c OBJECT tests/abi_storage.c)
  add_library(sco_abi_storage_cxx OBJECT "${_abi_storage_cxx}")
  set(_abi_storage_targets sco_abi_storage_c sco_abi_storage_cxx)
  if(NOT MSVC)
    add_library(sco_abi_storage_short_enums OBJECT tests/abi_storage.c)
    target_compile_options(sco_abi_storage_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_storage_targets sco_abi_storage_short_enums)
  endif()
  foreach(t IN LISTS _abi_storage_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_storage
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_storage_targets})

  # The game.vehicles table pin (tests/abi_game_vehicles.c), the same three ways. Header only, so the
  # kernel-only build checks it too.
  set(_abi_vehicles_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_vehicles.cpp")
  file(CONFIGURE OUTPUT "${_abi_vehicles_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_vehicles.c\"\n")
  add_library(sco_abi_game_vehicles_c OBJECT tests/abi_game_vehicles.c)
  add_library(sco_abi_game_vehicles_cxx OBJECT "${_abi_vehicles_cxx}")
  set(_abi_vehicles_targets sco_abi_game_vehicles_c sco_abi_game_vehicles_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_vehicles_short_enums OBJECT tests/abi_game_vehicles.c)
    target_compile_options(sco_abi_game_vehicles_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_vehicles_targets sco_abi_game_vehicles_short_enums)
  endif()
  foreach(t IN LISTS _abi_vehicles_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_game_vehicles
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_vehicles_targets})

  # The sco.ui table pin (tests/abi_ui.c), the same three ways.
  set(_abi_ui_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_ui.cpp")
  file(CONFIGURE OUTPUT "${_abi_ui_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_ui.c\"\n")
  add_library(sco_abi_ui_c OBJECT tests/abi_ui.c)
  add_library(sco_abi_ui_cxx OBJECT "${_abi_ui_cxx}")
  set(_abi_ui_targets sco_abi_ui_c sco_abi_ui_cxx)
  if(NOT MSVC)
    add_library(sco_abi_ui_short_enums OBJECT tests/abi_ui.c)
    target_compile_options(sco_abi_ui_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_ui_targets sco_abi_ui_short_enums)
  endif()
  foreach(t IN LISTS _abi_ui_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_ui
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_ui_targets})

  # The sco.datacore table pin (tests/abi_datacore.c), the same three ways.
  set(_abi_datacore_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_datacore.cpp")
  file(CONFIGURE OUTPUT "${_abi_datacore_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_datacore.c\"\n")
  add_library(sco_abi_datacore_c OBJECT tests/abi_datacore.c)
  add_library(sco_abi_datacore_cxx OBJECT "${_abi_datacore_cxx}")
  set(_abi_datacore_targets sco_abi_datacore_c sco_abi_datacore_cxx)
  if(NOT MSVC)
    add_library(sco_abi_datacore_short_enums OBJECT tests/abi_datacore.c)
    target_compile_options(sco_abi_datacore_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_datacore_targets sco_abi_datacore_short_enums)
  endif()
  foreach(t IN LISTS _abi_datacore_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_datacore
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_datacore_targets})

  # The sco.ipc table pin (tests/abi_ipc.c, which also compiles the static_asserts of sc_ipc.h), the
  # same three ways.
  set(_abi_ipc_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_ipc.cpp")
  file(CONFIGURE OUTPUT "${_abi_ipc_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_ipc.c\"\n")
  add_library(sco_abi_ipc_c OBJECT tests/abi_ipc.c)
  add_library(sco_abi_ipc_cxx OBJECT "${_abi_ipc_cxx}")
  set(_abi_ipc_targets sco_abi_ipc_c sco_abi_ipc_cxx)
  if(NOT MSVC)
    add_library(sco_abi_ipc_short_enums OBJECT tests/abi_ipc.c)
    target_compile_options(sco_abi_ipc_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_ipc_targets sco_abi_ipc_short_enums)
  endif()
  foreach(t IN LISTS _abi_ipc_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_ipc
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_ipc_targets})

  # The MIT wire of sco.net packets (tests/abi_sc_net.c, sc_net.h's constants, structs and helper
  # signatures), the same three ways.
  set(_abi_sc_net_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_sc_net.cpp")
  file(CONFIGURE OUTPUT "${_abi_sc_net_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_sc_net.c\"\n")
  add_library(sco_abi_sc_net_c OBJECT tests/abi_sc_net.c)
  add_library(sco_abi_sc_net_cxx OBJECT "${_abi_sc_net_cxx}")
  set(_abi_sc_net_targets sco_abi_sc_net_c sco_abi_sc_net_cxx)
  if(NOT MSVC)
    add_library(sco_abi_sc_net_short_enums OBJECT tests/abi_sc_net.c)
    target_compile_options(sco_abi_sc_net_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_sc_net_targets sco_abi_sc_net_short_enums)
  endif()
  foreach(t IN LISTS _abi_sc_net_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_sc_net
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_sc_net_targets})

  # The sco.net table pin (tests/abi_net.c), the same three ways.
  set(_abi_net_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_net.cpp")
  file(CONFIGURE OUTPUT "${_abi_net_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_net.c\"\n")
  add_library(sco_abi_net_c OBJECT tests/abi_net.c)
  add_library(sco_abi_net_cxx OBJECT "${_abi_net_cxx}")
  set(_abi_net_targets sco_abi_net_c sco_abi_net_cxx)
  if(NOT MSVC)
    add_library(sco_abi_net_short_enums OBJECT tests/abi_net.c)
    target_compile_options(sco_abi_net_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_net_targets sco_abi_net_short_enums)
  endif()
  foreach(t IN LISTS _abi_net_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_net
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_net_targets})
  # The MIT bridge layouts (tests/abi_titanlink.c, tests/abi_voxel_bridge.c: sc_titanlink.h and
  # sc_voxel_bridge.h, every size, offset and constant), the same three ways.
  foreach(_b IN ITEMS titanlink voxel_bridge)
    set(_abi_b_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_${_b}.cpp")
    file(CONFIGURE OUTPUT "${_abi_b_cxx}"
         CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_${_b}.c\"\n")
    add_library(sco_abi_${_b}_c OBJECT tests/abi_${_b}.c)
    add_library(sco_abi_${_b}_cxx OBJECT "${_abi_b_cxx}")
    set(_abi_b_targets sco_abi_${_b}_c sco_abi_${_b}_cxx)
    if(NOT MSVC)
      add_library(sco_abi_${_b}_short_enums OBJECT tests/abi_${_b}.c)
      target_compile_options(sco_abi_${_b}_short_enums PRIVATE -fshort-enums)
      list(APPEND _abi_b_targets sco_abi_${_b}_short_enums)
    endif()
    foreach(t IN LISTS _abi_b_targets)
      target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
      sco_warnings(${t})
      if(NOT MSVC)
        target_compile_options(${t} PRIVATE -Wpedantic)
      endif()
    endforeach()
    add_test(NAME abi_${_b}
             COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_b_targets})
  endforeach()
  # The game.actors table pin (tests/abi_game_actors.c, sc_actors.h), the same three ways.
  set(_abi_ga_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_actors.cpp")
  file(CONFIGURE OUTPUT "${_abi_ga_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_actors.c\"\n")
  add_library(sco_abi_game_actors_c OBJECT tests/abi_game_actors.c)
  add_library(sco_abi_game_actors_cxx OBJECT "${_abi_ga_cxx}")
  set(_abi_ga_targets sco_abi_game_actors_c sco_abi_game_actors_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_actors_short_enums OBJECT tests/abi_game_actors.c)
    target_compile_options(sco_abi_game_actors_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_ga_targets sco_abi_game_actors_short_enums)
  endif()
  foreach(t IN LISTS _abi_ga_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_game_actors
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_ga_targets})

  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE sco_core)
  if(SCO_GAME_SC)
    target_link_libraries(test_core PRIVATE sco_game_sc)   # the system.quit, pak.* and asop.* rows
  endif()
  sco_project_target(test_core)
  add_test(NAME test_core COMMAND test_core)

  add_executable(test_runtime tests/test_runtime.cpp)
  target_link_libraries(test_runtime PRIVATE sco_runtime)
  sco_project_target(test_runtime)
  add_test(NAME test_runtime COMMAND test_runtime)

  add_executable(test_host tests/test_host.cpp)
  target_link_libraries(test_host PRIVATE sco_host)
  sco_project_target(test_host)
  add_test(NAME test_host COMMAND test_host)

  # The C++20 SDK layer (include/scosdk/): two SDK plugins over the real host table.
  add_executable(test_sdk tests/test_sdk.cpp)
  target_link_libraries(test_sdk PRIVATE sco_host)
  sco_project_target(test_sdk)
  add_test(NAME test_sdk COMMAND test_sdk)

  # sco.storage over the real host table and SQLite; spawns itself to test crash safety.
  add_executable(test_storage tests/test_storage.cpp)
  target_link_libraries(test_storage PRIVATE sco_storage)
  sco_project_target(test_storage)
  add_test(NAME test_storage COMMAND test_storage "${SCO_TEST_OUT}")

  # sc_ipc.h over plain memory (wrap, rollover, hostile peers) and sco.ipc over real shared memory,
  # with a second mapping and a second process (it spawns itself) as the peer.
  add_executable(test_ipc tests/test_ipc.cpp)
  target_link_libraries(test_ipc PRIVATE sco_ipc)
  sco_project_target(test_ipc)
  add_test(NAME test_ipc COMMAND test_ipc)

  # sco.net's core: crypto vectors, the parser against hostile input, replay, reliable delivery over
  # a seeded lossy in-memory network, several sessions in one process (virtual clock, no sleeps).
  add_executable(test_net tests/test_net.cpp)
  target_link_libraries(test_net PRIVATE sco_net)
  sco_project_target(test_net)
  add_test(NAME test_net COMMAND test_net)

  # The sco.net service over the in-memory network (ownership, quotas, unload, delivery on the game
  # thread), two Cores over UDP loopback, and the service's network thread over UDP loopback.
  add_executable(test_net_service tests/test_net_service.cpp)
  target_link_libraries(test_net_service PRIVATE sco_net_service)
  sco_project_target(test_net_service)
  add_test(NAME test_net_service COMMAND test_net_service)

  # sco.ui over the real host table, with built-in plugins under the loader's crash guard.
  add_executable(test_ui tests/test_ui.cpp)
  target_link_libraries(test_ui PRIVATE sco_ui sco_plugins)
  sco_project_target(test_ui)
  add_test(NAME test_ui COMMAND test_ui)

  add_executable(test_hook tests/test_hook.cpp)
  target_link_libraries(test_hook PRIVATE sco_hook)
  add_test(NAME test_hook COMMAND test_hook)
  sco_project_target(test_hook)

  add_executable(test_spatial tests/test_spatial.cpp)
  target_link_libraries(test_spatial PRIVATE sco_engine)
  sco_project_target(test_spatial)
  add_test(NAME test_spatial COMMAND test_spatial)

  if(SCO_GAME_SC)
  # sco::game::pak over a fake ICryPak (a real vtable) and a fake loader; x86-64 only (the loader
  # detour), skipped elsewhere.
  add_executable(test_pak tests/test_pak.cpp)
  target_link_libraries(test_pak PRIVATE sco_pak)
  sco_project_target(test_pak)
  add_test(NAME test_pak COMMAND test_pak)
  endif()

  add_executable(test_vfs tests/test_vfs.cpp)
  target_link_libraries(test_vfs PRIVATE sco_vfs)
  sco_project_target(test_vfs)
  add_test(NAME test_vfs COMMAND test_vfs)

  if(SCO_GAME_SC)
  # The DataCore parser over tests/dcb_builder.h fixtures; it also writes the fixtures dcb_tool
  # runs sco-dcb on.
  add_executable(test_datacore tests/test_datacore.cpp)
  target_link_libraries(test_datacore PRIVATE sco_datacore)
  sco_project_target(test_datacore)
  add_test(NAME test_datacore COMMAND test_datacore "${SCO_TEST_OUT}")
  set_tests_properties(test_datacore PROPERTIES FIXTURES_SETUP dcb_fixtures)
  # The patcher: each operation, drift across moved tables and both record sizes, refusals by
  # category, per-batch atomicity, truncated and corrupted inputs.
  add_executable(test_datacore_patch tests/test_datacore_patch.cpp)
  target_link_libraries(test_datacore_patch PRIVATE sco_datacore)
  sco_project_target(test_datacore_patch)
  add_test(NAME test_datacore_patch COMMAND test_datacore_patch)
  # Data packs: golden parses of tests/fixtures/datacore/golden/*.toml, ordering, conflicts,
  # atomicity, every refusal; it writes the fixture the sample pack is checked against.
  add_executable(test_datacore_pack tests/test_datacore_pack.cpp)
  target_link_libraries(test_datacore_pack PRIVATE sco_datacore)
  sco_project_target(test_datacore_pack)
  add_test(NAME test_datacore_pack COMMAND test_datacore_pack "${CMAKE_CURRENT_SOURCE_DIR}" "${SCO_TEST_OUT}")
  set_tests_properties(test_datacore_pack PROPERTIES FIXTURES_SETUP dcb_pack_fixtures)
  # sco-dcb lint / check / show / diff over the golden files, the sample pack and that fixture.
  add_test(NAME dcb_pack
           COMMAND "${CMAKE_COMMAND}" "-DDCB=$<TARGET_FILE:sco-dcb>" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DDIR=${SCO_TEST_OUT}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dcb_pack.cmake")
  set_tests_properties(dcb_pack PROPERTIES
    FIXTURES_REQUIRED dcb_pack_fixtures
    PASS_REGULAR_EXPRESSION "dcb_pack: OK"
    FAIL_REGULAR_EXPRESSION "dcb_pack: FAIL")
  # The sco.datacore service over the real host table: a simulated launch sequence (load, a plugin
  # commits after it, the next launch applies the saved patch in plugin order), patches committed
  # before a load, release, atomicity, reports, datacore.applied, and the scosdk wrapper.
  add_executable(test_datacore_service tests/test_datacore_service.cpp)
  target_link_libraries(test_datacore_service PRIVATE sco_datacore_service)
  sco_project_target(test_datacore_service)
  add_test(NAME test_datacore_service COMMAND test_datacore_service "${SCO_TEST_OUT}")
  add_test(NAME dcb_tool
           COMMAND "${CMAKE_COMMAND}" "-DDCB=$<TARGET_FILE:sco-dcb>" "-DDIR=${SCO_TEST_OUT}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/dcb_tool.cmake")
  set_tests_properties(dcb_tool PROPERTIES
    FIXTURES_REQUIRED dcb_fixtures
    PASS_REGULAR_EXPRESSION "dcb_tool: OK"
    FAIL_REGULAR_EXPRESSION "dcb_tool: FAIL")
  endif()

  # Fake native plugins for test_plugins, laid out like tools/test.sh (tools/test-win.sh on
  # Windows): <out>/plugins/m<mode>/{plugin.ini,m<mode>.<ext>}, m11 a second clean plugin, and
  # text/ an entry that is not a library.
  if(WIN32)
    set(_ext dll)
  else()
    set(_ext so)
  endif()
  set(_plug "${SCO_TEST_OUT}/plugins")
  file(REMOVE_RECURSE "${_plug}")
  # One plugin m<n> per FAKE_MODE n in fake_plugin.c; keep in step with tools/test.sh.
  set(SCO_FAKE_PLUGIN_MODES 0 1 2 3 4 5 6 7 8 9 10)
  set(_fakes)
  foreach(m IN LISTS SCO_FAKE_PLUGIN_MODES)
    list(APPEND _fakes "m${m}:${m}")
  endforeach()
  list(APPEND _fakes "m11:0")   # a second clean plugin
  set(_fake_targets)
  foreach(fake IN LISTS _fakes)
    string(REPLACE ":" ";" fake "${fake}")
    list(GET fake 0 id)
    list(GET fake 1 mode)
    file(WRITE "${_plug}/${id}/plugin.ini"
         "id = ${id}\nname = Fake ${id}\nversion = 1.0.0\napi = 1.0\nkind = native\nentry = ${id}.${_ext}\n")
    add_library(sco_fake_${id} SHARED tests/fixtures/plugins/native/fake_plugin.c)
    target_include_directories(sco_fake_${id} PRIVATE "${SCO_INCLUDE_DIR}")
    target_compile_definitions(sco_fake_${id} PRIVATE FAKE_MODE=${mode} "FAKE_ID=\"${id}\"")
    # $<1:...> keeps multi-config generators from adding a per-config subfolder.
    set_target_properties(sco_fake_${id} PROPERTIES
      OUTPUT_NAME ${id} PREFIX "" SUFFIX ".${_ext}"
      LIBRARY_OUTPUT_DIRECTORY "$<1:${_plug}/${id}>"
      RUNTIME_OUTPUT_DIRECTORY "$<1:${_plug}/${id}>")
    sco_warnings(sco_fake_${id})
    list(APPEND _fake_targets sco_fake_${id})
  endforeach()
  file(WRITE "${_plug}/text/plugin.ini"
       "id = text\nname = Not a library\nversion = 1\napi = 1.0\nkind = native\nentry = text.${_ext}\n")
  if(WIN32)
    file(WRITE "${_plug}/text/text.dll" "not a DLL\n")
  else()
    file(WRITE "${_plug}/text/text.so" "not a shared library\n")
  endif()

  add_executable(test_plugins tests/test_plugins.cpp)
  target_link_libraries(test_plugins PRIVATE sco_plugins)
  add_dependencies(test_plugins ${_fake_targets})
  sco_project_target(test_plugins)
  add_test(NAME test_plugins
           COMMAND test_plugins "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plugins" "${SCO_TEST_OUT}")

  add_executable(test_lua tests/test_lua.cpp)
  target_link_libraries(test_lua PRIVATE sco_plugins sco_host sco_lua sco_ui sco_storage)
  if(SCO_GAME_SC)
    target_link_libraries(test_lua PRIVATE sco_datacore_service)   # TestDataCore
  endif()
  sco_project_target(test_lua)
  add_test(NAME test_lua COMMAND test_lua "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${SCO_TEST_OUT}")

  # The host kit with built-ins, two fake plugins, greeter and travel_pack.
  add_executable(test_app tests/test_app.cpp)
  target_link_libraries(test_app PRIVATE sco_app sco_lua)
  add_dependencies(test_app ${_fake_targets})
  sco_project_target(test_app)
  add_test(NAME test_app COMMAND test_app "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${SCO_TEST_OUT}")

  # sco-host-sim over the SDK examples, laid out like data/plugins: <out>/sim/plugins/
  # {hello/{hello.<ext>,plugin.ini}, greeter/, travel_pack/}. Keep in step with tools/test.sh.
  set(_sim "${SCO_TEST_OUT}/sim/plugins")
  file(REMOVE_RECURSE "${SCO_TEST_OUT}/sim")
  add_library(sco_sim_hello SHARED sdk/examples/hello/hello.c)
  target_include_directories(sco_sim_hello PRIVATE "${SCO_INCLUDE_DIR}")
  set_target_properties(sco_sim_hello PROPERTIES
    OUTPUT_NAME hello PREFIX "" SUFFIX ".${_ext}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${_sim}/hello>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${_sim}/hello>")
  sco_warnings(sco_sim_hello)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/hello/plugin.ini" _hello_ini)
  if(NOT WIN32)
    string(REPLACE "entry = hello.dll" "entry = hello.so" _hello_ini "${_hello_ini}")
  endif()
  file(WRITE "${_sim}/hello/plugin.ini" "${_hello_ini}")
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/greeter" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/travel_pack"
            "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/quantum_pack"
       DESTINATION "${_sim}")
  add_dependencies(sco-host-sim sco_sim_hello)
  # tests/host_sim.cmake runs the sim and prints "host_sim_examples: FAIL" unless it exits 0 and
  # answers the invoke; CTest needs the reply and no FAIL line.
  add_test(NAME host_sim_examples
           COMMAND "${CMAKE_COMMAND}" "-DSIM=$<TARGET_FILE:sco-host-sim>" "-DROOT=${_sim}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/host_sim.cmake")
  set_tests_properties(host_sim_examples PROPERTIES
    PASS_REGULAR_EXPRESSION "hello\\.wave -> OK \"Hello, Pilot One\""
    FAIL_REGULAR_EXPRESSION "host_sim_examples: FAIL")
endif()
