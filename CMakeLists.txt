cmake_minimum_required(VERSION 3.20)

# CMP0141 and the debug-format variable must be set before project(): MSVC debug flags are chosen
# when the compiler is enabled. Embedded debug info (/Z7) avoids vc140.pdb locking and enables
# compiler caching. Harmless for other compilers; a caller-set value wins.
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()
if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

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
if(SCO_GAME_SC AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/game")
  message(FATAL_ERROR "This source kit omits the game pack. Configure with -DSCO_GAME_SC=OFF.")
endif()
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
    if(UNIX AND NOT APPLE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
      # DSOs with --no-undefined need the shared sanitizer runtime. Use the same
      # runtime in their host executables; mixing static and shared ASan fails.
      target_link_options(${target} PUBLIC -shared-libsan)
      execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-runtime-dir
        OUTPUT_VARIABLE _sco_sanitizer_dir OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
      set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${_sco_sanitizer_dir}")
    endif()
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
  list(FILTER SCO_GAME_SOURCES EXCLUDE REGEX "/(pak_hooks|diagnostics|host|private_session|admission)\\.cpp$")
  add_library(sco_game_sc STATIC ${SCO_GAME_SOURCES})
  target_link_libraries(sco_game_sc PUBLIC sco_core)
  add_library(sco_game_diagnostics STATIC src/game/diagnostics.cpp)
  target_link_libraries(sco_game_diagnostics PUBLIC sco_host)
  sco_project_target(sco_game_diagnostics)
  sco_project_target(sco_game_sc)
endif()

# Portable guarded game-pack boundary; no engine adapter enabled by default.
if(SCO_GAME_SC)
  add_library(sco_offline_missions STATIC src/game/services/offline_missions.cpp)
  target_include_directories(sco_offline_missions PUBLIC "${SCO_INCLUDE_DIR}")
  target_link_libraries(sco_offline_missions PUBLIC sco_host)
  sco_project_target(sco_offline_missions)
  if(SCO_BUILD_TESTS)
    add_executable(test_offline_missions tests/test_offline_missions.cpp)
    target_link_libraries(test_offline_missions PRIVATE sco_offline_missions)
    sco_project_target(test_offline_missions)
    add_test(NAME offline_missions COMMAND test_offline_missions)
    add_library(sco_abi_offline_missions OBJECT tests/abi_offline_missions.c)
    target_include_directories(sco_abi_offline_missions PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(sco_abi_offline_missions)
    add_dependencies(test_offline_missions sco_abi_offline_missions)
  endif()
endif()

# Tasks, events and commands: the cross-thread runtime under the plugin ABI.
add_library(sco_runtime STATIC src/api/sco_tasks.cpp src/api/sco_events.cpp src/api/sco_commands.cpp
  src/api/sco_services.cpp src/api/sco_raw.cpp)
target_include_directories(sco_runtime PUBLIC "${SCO_INCLUDE_DIR}")
target_link_libraries(sco_runtime PUBLIC Threads::Threads)
sco_project_target(sco_runtime)

# Capability registry and the host's sco_api table.
add_library(sco_host STATIC src/api/sco_caps.cpp src/host/sco_host.cpp src/host/sco_diagnostics.cpp
  src/host/sco_diagnostics_publisher.cpp)
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
add_library(sco_vfs STATIC src/vfs/compose.cpp src/vfs/table.cpp src/vfs/files.cpp)
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
  src/plugins/manifest.cpp src/plugins/settings_ini.cpp src/plugins/discover.cpp src/plugins/loader.cpp src/plugins/content.cpp)
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
# host-owned service. No renderer: the product draws what is registered. sco.ui.surfaces
# (sco/surfaces.h, include/sco_ui_surfaces.h) lives here too and shares sco.ui's release hook.
add_library(sco_ui STATIC src/ui/ui.cpp src/ui/surfaces.cpp)
target_link_libraries(sco_ui PUBLIC sco_host)
sco_project_target(sco_ui)

# Renderer-neutral retained UI is always part of the kernel. Products optionally
# compile the ImGui adapter against their own host-side ImGui implementation.
add_library(sco_widgets STATIC src/widgets/widgets.cpp)
target_link_libraries(sco_widgets PUBLIC sco_host)
sco_project_target(sco_widgets)
option(SCO_WIDGETS_IMGUI "Build the host-only ImGui widget adapter" OFF)
set(SCO_WIDGETS_IMGUI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk/third_party/imgui" CACHE PATH
    "Directory containing the host's imgui.h and imconfig.h")
if(SCO_WIDGETS_IMGUI)
  add_library(sco_widgets_imgui STATIC src/widgets/imgui_adapter.cpp)
  target_link_libraries(sco_widgets_imgui PUBLIC sco_widgets)
  target_include_directories(sco_widgets_imgui PRIVATE "${SCO_WIDGETS_IMGUI_INCLUDE_DIR}")
  sco_project_target(sco_widgets_imgui)
endif()

# sco.settings (sco/settings.h, include/sco_settings.h): the typed [settings] of plugin.ini, their
# values (kept in the plugin's sco.storage namespace) and what the product's menu draws; a host-owned
# service, read-only to plugins.
add_library(sco_settings STATIC src/settings/settings.cpp)
target_link_libraries(sco_settings PUBLIC sco_host sco_plugins sco_storage sco_ui)
sco_project_target(sco_settings)

# Read-only startup list/script snapshots; kernel-neutral public sco.content.
add_library(sco_content STATIC src/content/content.cpp)
target_link_libraries(sco_content PUBLIC sco_host sco_plugins)
sco_project_target(sco_content)

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

# Default-off host-private diagnostics; deliberately does not link sco.net or sockets.
# Reuse project-authored SHA/HMAC primitives only, with independent session keys.
add_library(sco_diagnostics_ipc STATIC src/ipc/diagnostics_ipc.cpp src/ipc/diagnostics_json.cpp src/ipc/diagnostics_audit.cpp src/net/sha2.c)
if(WIN32)
  target_sources(sco_diagnostics_ipc PRIVATE src/ipc/diagnostics_platform_win.cpp)
  target_link_libraries(sco_diagnostics_ipc PRIVATE advapi32 bcrypt wevtapi)
else()
  target_sources(sco_diagnostics_ipc PRIVATE src/ipc/diagnostics_platform_posix.cpp)
endif()
target_link_libraries(sco_diagnostics_ipc PUBLIC sco_ipc Threads::Threads)
sco_project_target(sco_diagnostics_ipc)
option(SCO_BUILD_RUNTIME_MCP "Build separately packaged native runtime MCP peer" OFF)
if(SCO_BUILD_RUNTIME_MCP)
  # All dependencies are local project sources/system APIs; no compilation at launcher runtime.
  add_library(sco_core_mcp_runtime_peer SHARED tools/sco-core-mcp-runtime/native_adapter.cpp)
  target_include_directories(sco_core_mcp_runtime_peer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(sco_core_mcp_runtime_peer PRIVATE sco_diagnostics_ipc sco_plugins)
  set_target_properties(sco_core_mcp_runtime_peer PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  foreach(_rt_pic sco_diagnostics_ipc sco_ipc sco_host sco_runtime sco_core sco_plugins)
    set_target_properties(${_rt_pic} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endforeach()
  sco_project_target(sco_core_mcp_runtime_peer)
endif()

# sco.net core (sco/net/*.h): SHA-256/HMAC/PBKDF2 (sha2.c, plain C), the wire framing, reliable
# streams and the session over an abstract datagram transport. Platform-free (docs/net-wire.md).
add_library(sco_net STATIC src/net/sha2.c src/net/wire.cpp src/net/reliable.cpp src/net/core.cpp)
target_include_directories(sco_net PUBLIC "${SCO_INCLUDE_DIR}")
sco_project_target(sco_net)

# sco.net service (sco/net/session.h, include/sco_net.h): the network thread, channel ownership,
# quotas, the LAN rule (scope.cpp) and UDP, a host-owned service. The platform half is udp_win.cpp
# (Winsock, the real target) or udp_posix.cpp (the host tests).
add_library(sco_net_service STATIC src/net/service.cpp src/net/scope.cpp src/net/control.cpp)
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
target_link_libraries(sco_app PUBLIC sco_host sco_plugins sco_core sco_storage sco_ui sco_widgets sco_settings sco_content sco_ipc sco_net_service sco_diagnostics_ipc)
if(SCO_GAME_SC)
  # sco.datacore (Platform::dataCore) and the Star Citizen tables a product passes as
  # Platform::registerSignatures (sco::game::RegisterGameSignatures).
  add_library(sco_private_session STATIC src/game/private_session.cpp)
  target_link_libraries(sco_private_session PUBLIC sco_net_service)
  sco_project_target(sco_private_session)
  target_link_libraries(sco_app PUBLIC sco_datacore_service sco_game_sc sco_game_diagnostics sco_private_session)
endif()
# The game pack's services (sco/game/services.h, docs/design/game-services.md): teleport.spatial
# today, published under the owner "game" when the product sets Platform::gameServices. They read
# game memory under SEH (__try), so Windows only; SCO_GAME_SERVICES tells sco_app they're there.
if(SCO_GAME_SC AND WIN32)
  add_library(sco_game_services STATIC src/game/services/reads.cpp src/game/services/spatial.cpp src/game/services/relocation.cpp
                                       src/game/services/relocation_engine.cpp
                                       src/game/services/spawn.cpp src/game/services/missions.cpp
                                       src/game/services/actors.cpp src/game/services/actor_stats.cpp src/game/services/vehicles.cpp
                                       src/game/services/world.cpp src/game/services/world_control.cpp src/game/services/world_free_cam.cpp src/game/services/gamerules.cpp src/game/services/inventory.cpp
                                       src/game/services/inventory_walk.cpp src/game/services/inventory_engine.cpp src/game/services/inventory_host.cpp
                                       src/game/services/events.cpp
                                       src/game/services/entities.cpp src/game/services/creative.cpp
                                       src/game/services/owned.cpp src/game/services/prefabs.cpp
                                       src/game/services/loadout.cpp src/game/services/cvars.cpp src/game/services/build.cpp src/game/services/input.cpp
                                       src/game/services/hangar.cpp src/game/services/hangar_engine.cpp
                                       src/game/services/reputation.cpp src/game/services/reputation_engine.cpp
                                       src/game/services/asop_service.cpp src/game/services/asop_engine.cpp
                                       src/game/services/contracts_service.cpp src/game/services/contracts_engine.cpp
                                       src/game/services/quantum_engine.cpp)
  target_link_libraries(sco_game_services PUBLIC sco_game_sc sco_host sco_engine sco_hook sco_quantum sco_offline_missions sco_mining sco_game_admission)
  target_compile_definitions(sco_game_services PUBLIC SCO_GAME_SERVICES=1)
  sco_project_target(sco_game_services)
  target_link_libraries(sco_app PUBLIC sco_game_services)
endif()
sco_project_target(sco_app)

# Offline game host composition is a separate host-source target, never a plugin API.
if(SCO_GAME_SC)
  add_library(sco_game_host STATIC src/game/host.cpp)
  target_link_libraries(sco_game_host PUBLIC sco_app sco_game_sc sco_hook sco_pak sco_datacore_service sco_game_admission)
  sco_project_target(sco_game_host)
endif()

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

# sco-lua: the sandboxed runtime for kind = lua plugins. Game-neutral: sco_lua.c includes only
# sco_api.h, the generic host services and Lua. Its optional adapters (plugins/lua/sco_lua_ext.h)
# are separate targets a host links and registers.
add_library(sco_lua STATIC plugins/lua/sco_lua.c)
target_include_directories(sco_lua PUBLIC "${SCO_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/plugins/lua")
target_link_libraries(sco_lua PUBLIC sco_lua_vendor)
sco_project_target(sco_lua)
if(NOT MSVC)
  target_compile_options(sco_lua PRIVATE -Wpedantic)
endif()

# The Star Citizen adapter: sco.datacore, the read-only sco.game.* tables and the game.* event
# payloads. A host registers it: sco_lua_add_extension(&sco_lua_game_extension).
if(SCO_GAME_SC)
  add_library(sco_lua_game STATIC plugins/lua/sco_lua_game.c)
  target_link_libraries(sco_lua_game PUBLIC sco_lua)
  sco_project_target(sco_lua_game)
  if(NOT MSVC)
    target_compile_options(sco_lua_game PRIVATE -Wpedantic)
  endif()
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
  target_link_libraries(sco-host-sim PRIVATE sco_datacore sco_lua_game)   # the pack lint; sco.game for scripts
endif()
sco_project_target(sco-host-sim)

# DataCore files on disk (docs/datacore.md): info and records.
if(SCO_GAME_SC)
add_executable(sco-dcb tools/sco-dcb.cpp)
target_link_libraries(sco-dcb PRIVATE sco_datacore)
sco_project_target(sco-dcb)
endif()

if(SCO_GAME_SC)
  add_library(sco_game_admission STATIC src/game/admission.cpp)
  target_include_directories(sco_game_admission PUBLIC "${SCO_INCLUDE_DIR}")
  sco_project_target(sco_game_admission)
  add_library(sco_mining STATIC src/game/services/mining.cpp)
  target_link_libraries(sco_mining PUBLIC sco_host sco_game_admission)
  if(WIN32)
    target_sources(sco_mining PRIVATE src/game/services/mining_native.cpp)
    target_link_libraries(sco_mining PUBLIC sco_game_sc sco_hook)
  endif()
  sco_project_target(sco_mining)
  if(SCO_BUILD_TESTS)
    add_executable(test_mining tests/test_mining.cpp)
    target_link_libraries(test_mining PRIVATE sco_mining)
    sco_project_target(test_mining)
    enable_testing()
    add_test(NAME mining COMMAND test_mining)
  endif()
  add_library(sco_quantum STATIC src/game/services/quantum.cpp src/game/services/quantum_owned.cpp)
  target_link_libraries(sco_quantum PUBLIC sco_host sco_hook)
  sco_project_target(sco_quantum)
endif()
if(SCO_GAME_SC AND SCO_BUILD_TESTS)
  add_executable(test_quantum tests/test_quantum.cpp)
  target_link_libraries(test_quantum PRIVATE sco_quantum)
  sco_project_target(test_quantum)
  enable_testing()
  add_test(NAME quantum COMMAND test_quantum)
  add_test(NAME quantum_cleanup_fault COMMAND test_quantum --cleanup-fault)
  add_executable(test_quantum_owned tests/test_quantum_owned.cpp)
  target_link_libraries(test_quantum_owned PRIVATE sco_quantum)
  sco_project_target(test_quantum_owned)
  add_test(NAME quantum_owned COMMAND test_quantum_owned)
  add_test(NAME quantum_owned_restore_fault COMMAND test_quantum_owned --restore-fault)
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/abi_game_quantum.cpp"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_quantum.c\"\n")
  add_executable(abi_quantum_c tests/abi_game_quantum.c)
  add_executable(abi_quantum_cpp "${CMAKE_CURRENT_BINARY_DIR}/abi_game_quantum.cpp")
  foreach(t abi_quantum_c abi_quantum_cpp)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(${t})
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()

# Host-only offline fleet composition: never linked into the neutral kernel.
if(SCO_GAME_SC)
  add_library(sco_asop_host STATIC src/game/asop_host/config.cpp src/game/asop_host/handoff.cpp)
  target_link_libraries(sco_asop_host PUBLIC sco_game_sc Threads::Threads)
  if(WIN32)
    target_sources(sco_asop_host PRIVATE src/game/asop_host/catalog_win.cpp
      src/game/asop_host/support_win.cpp src/game/asop_host/fleet_win.cpp src/game/asop_host/hangar_win.cpp)
    target_link_libraries(sco_asop_host PUBLIC sco_game_services sco_hook)
  endif()
  sco_project_target(sco_asop_host)
  if(SCO_BUILD_TESTS)
    enable_testing()
    add_executable(test_asop_host src/game/asop_host/test_asop_host.cpp)
    target_link_libraries(test_asop_host PRIVATE sco_asop_host)
    sco_project_target(test_asop_host)
    add_test(NAME asop_host COMMAND test_asop_host)
  endif()
endif()

# ---- tests ------------------------------------------------------------------------------------

if(SCO_BUILD_TESTS)
  enable_testing()
  if(SCO_GAME_SC)
    add_executable(test_input tests/test_input.cpp src/game/services/input.cpp)
    target_link_libraries(test_input PRIVATE sco_host)
    sco_project_target(test_input)
    add_test(NAME input_registry COMMAND test_input)
    add_executable(test_gamerules_params tests/test_gamerules_params.cpp src/game/services/gamerules.cpp)
    target_link_libraries(test_gamerules_params PRIVATE sco_host)
    sco_project_target(test_gamerules_params)
    add_test(NAME gamerules_params COMMAND test_gamerules_params)
    add_executable(abi_input_c tests/abi_game_input.c)
    target_include_directories(abi_input_c PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(abi_input_c)
    add_test(NAME abi_input_c COMMAND abi_input_c)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/abi_game_input.cpp"
         CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_input.c\"\n")
    add_executable(abi_input_cpp "${CMAKE_CURRENT_BINARY_DIR}/abi_game_input.cpp")
    target_include_directories(abi_input_cpp PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(abi_input_cpp)
    add_test(NAME abi_input_cpp COMMAND abi_input_cpp)
    if(NOT MSVC)
      add_executable(abi_input_short_enums tests/abi_game_input.c)
      target_include_directories(abi_input_short_enums PRIVATE "${SCO_INCLUDE_DIR}")
      target_compile_options(abi_input_short_enums PRIVATE -fshort-enums)
      sco_project_target(abi_input_short_enums)
      add_test(NAME abi_input_short_enums COMMAND abi_input_short_enums)
    endif()
  endif()
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

  # The ABI freeze (tools/abi_snapshot.py): the compiled surface of include/sco_api.h against
  # tests/abi/sco_api-v1.1.golden, plus the sha256 of the header as released at sdk-v1.1.0. The
  # probe is built here (any compiler, MSVC included) and only run by this test, so `cmake --build`
  # alone stays green and ctest carries the verdict. Header-only, so the kernel-only build runs it
  # too. Needs python3 (CI has it) and the git tag (fetched by the tool when missing).
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    set(_freeze_probe "${CMAKE_CURRENT_BINARY_DIR}/abi_freeze_probe.c")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tools/abi_snapshot.py" gen "${_freeze_probe}"
                    RESULT_VARIABLE _freeze_gen)
    if(NOT _freeze_gen EQUAL 0)
      message(FATAL_ERROR "tools/abi_snapshot.py gen failed")
    endif()
    add_executable(abi_freeze_probe "${_freeze_probe}")
    target_include_directories(abi_freeze_probe PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(abi_freeze_probe)
    add_test(NAME abi_freeze
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/abi_snapshot.py"
                     check --bin "$<TARGET_FILE:abi_freeze_probe>")
  else()
    message(WARNING "python3 not found: the abi_freeze test is not registered "
                    "(run tools/test.sh, which uses it too)")
  endif()

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

  # The sco.settings table pin (tests/abi_settings.c), the same three ways.
  set(_abi_settings_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_settings.cpp")
  file(CONFIGURE OUTPUT "${_abi_settings_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_settings.c\"\n")
  add_library(sco_abi_settings_c OBJECT tests/abi_settings.c)
  add_library(sco_abi_settings_cxx OBJECT "${_abi_settings_cxx}")
  set(_abi_settings_targets sco_abi_settings_c sco_abi_settings_cxx)
  if(NOT MSVC)
    add_library(sco_abi_settings_short_enums OBJECT tests/abi_settings.c)
    target_compile_options(sco_abi_settings_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_settings_targets sco_abi_settings_short_enums)
  endif()
  foreach(t IN LISTS _abi_settings_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_settings
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_settings_targets})

  if(SCO_GAME_SC)
  # The game.vehicles table pin (tests/abi_game_vehicles.c), the same three ways. Game-pack only.
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

  # The game.entities table pin (tests/abi_game_entities.c, sc_entities.h), the same three ways. Game-pack only.
  set(_abi_ge_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_entities.cpp")
  file(CONFIGURE OUTPUT "${_abi_ge_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_entities.c\"\n")
  add_library(sco_abi_game_entities_c OBJECT tests/abi_game_entities.c)
  add_library(sco_abi_game_entities_cxx OBJECT "${_abi_ge_cxx}")
  set(_abi_ge_targets sco_abi_game_entities_c sco_abi_game_entities_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_entities_short_enums OBJECT tests/abi_game_entities.c)
    target_compile_options(sco_abi_game_entities_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_ge_targets sco_abi_game_entities_short_enums)
  endif()
  foreach(t IN LISTS _abi_ge_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_game_entities
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_ge_targets})

  # The game.actor_stats table pin (tests/abi_game_actor_stats.c, sc_actor_stats.h), the same three ways. Game-pack only.
  set(_abi_gas_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_actor_stats.cpp")
  file(CONFIGURE OUTPUT "${_abi_gas_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_actor_stats.c\"\n")
  add_library(sco_abi_game_actor_stats_c OBJECT tests/abi_game_actor_stats.c)
  add_library(sco_abi_game_actor_stats_cxx OBJECT "${_abi_gas_cxx}")
  set(_abi_gas_targets sco_abi_game_actor_stats_c sco_abi_game_actor_stats_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_actor_stats_short_enums OBJECT tests/abi_game_actor_stats.c)
    target_compile_options(sco_abi_game_actor_stats_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_gas_targets sco_abi_game_actor_stats_short_enums)
  endif()
  foreach(t IN LISTS _abi_gas_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_game_actor_stats
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_gas_targets})

  # The game.actor_stats_write table pin (tests/abi_game_actor_stats_write.c, sc_actor_stats_write.h), the same three ways. Game-pack only.
  set(_abi_gasw_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_actor_stats_write.cpp")
  file(CONFIGURE OUTPUT "${_abi_gasw_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_actor_stats_write.c\"\n")
  add_library(sco_abi_game_actor_stats_write_c OBJECT tests/abi_game_actor_stats_write.c)
  add_library(sco_abi_game_actor_stats_write_cxx OBJECT "${_abi_gasw_cxx}")
  set(_abi_gasw_targets sco_abi_game_actor_stats_write_c sco_abi_game_actor_stats_write_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_actor_stats_write_short_enums OBJECT tests/abi_game_actor_stats_write.c)
    target_compile_options(sco_abi_game_actor_stats_write_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_gasw_targets sco_abi_game_actor_stats_write_short_enums)
  endif()
  foreach(t IN LISTS _abi_gasw_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_game_actor_stats_write
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_gasw_targets})

  # The game.gamerules table pin (tests/abi_game_gamerules.c, sc_gamerules.h), the same three ways. Game-pack only.
  set(_abi_ggr_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_gamerules.cpp")
  file(CONFIGURE OUTPUT "${_abi_ggr_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_gamerules.c\"\n")
  add_library(sco_abi_game_gamerules_c OBJECT tests/abi_game_gamerules.c)
  add_library(sco_abi_game_gamerules_cxx OBJECT "${_abi_ggr_cxx}")
  set(_abi_ggr_targets sco_abi_game_gamerules_c sco_abi_game_gamerules_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_gamerules_short_enums OBJECT tests/abi_game_gamerules.c)
    target_compile_options(sco_abi_game_gamerules_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_ggr_targets sco_abi_game_gamerules_short_enums)
  endif()
  foreach(_t IN LISTS _abi_ggr_targets)
    target_include_directories(${_t} PRIVATE "${SCO_INCLUDE_DIR}")
  endforeach()
  add_executable(abi_gamerules_c tests/abi_game_gamerules.c)
  add_executable(abi_gamerules_cpp "${_abi_ggr_cxx}")
  foreach(_t abi_gamerules_c abi_gamerules_cpp)
    target_include_directories(${_t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(${_t})
    add_test(NAME ${_t} COMMAND ${_t})
  endforeach()

  # The game.build table pin (tests/abi_game_build.c, sc_game_build.h), the same three ways. Game-pack only.
  set(_abi_gb_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_build.cpp")
  file(CONFIGURE OUTPUT "${_abi_gb_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_build.c\"\n")
  add_library(sco_abi_game_build_c OBJECT tests/abi_game_build.c)
  add_library(sco_abi_game_build_cxx OBJECT "${_abi_gb_cxx}")
  set(_abi_gb_targets sco_abi_game_build_c sco_abi_game_build_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_build_short_enums OBJECT tests/abi_game_build.c)
    target_compile_options(sco_abi_game_build_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_gb_targets sco_abi_game_build_short_enums)
  endif()
  foreach(_t IN LISTS _abi_gb_targets)
    target_include_directories(${_t} PRIVATE "${SCO_INCLUDE_DIR}")
  endforeach()
  add_test(NAME abi_game_build
           COMMAND ${CMAKE_COMMAND} -E echo "layout pinned by object-library compile")

  # The game.inventory table pin (tests/abi_game_inventory.c, sc_inventory.h), the same three ways. Game-pack only.
  set(_abi_ginv_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_inventory.cpp")
  file(CONFIGURE OUTPUT "${_abi_ginv_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_inventory.c\"\n")
  add_library(sco_abi_game_inventory_c OBJECT tests/abi_game_inventory.c)
  add_library(sco_abi_game_inventory_cxx OBJECT "${_abi_ginv_cxx}")
  set(_abi_ginv_targets sco_abi_game_inventory_c sco_abi_game_inventory_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_inventory_short_enums OBJECT tests/abi_game_inventory.c)
    target_compile_options(sco_abi_game_inventory_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_ginv_targets sco_abi_game_inventory_short_enums)
  endif()
  foreach(_t IN LISTS _abi_ginv_targets)
    target_include_directories(${_t} PRIVATE "${SCO_INCLUDE_DIR}")
  endforeach()
  add_test(NAME abi_game_inventory
           COMMAND ${CMAKE_COMMAND} -E echo "layout pinned by object-library compile")

  # The game.creative table pin (tests/abi_game_creative.c, sc_creative.h), the same three ways. Game-pack only.
  set(_abi_gc_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_game_creative.cpp")
  file(CONFIGURE OUTPUT "${_abi_gc_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_game_creative.c\"\n")
  add_library(sco_abi_game_creative_c OBJECT tests/abi_game_creative.c)
  add_library(sco_abi_game_creative_cxx OBJECT "${_abi_gc_cxx}")
  set(_abi_gc_targets sco_abi_game_creative_c sco_abi_game_creative_cxx)
  if(NOT MSVC)
    add_library(sco_abi_game_creative_short_enums OBJECT tests/abi_game_creative.c)
    target_compile_options(sco_abi_game_creative_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_gc_targets sco_abi_game_creative_short_enums)
  endif()
  foreach(t IN LISTS _abi_gc_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_game_creative
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_gc_targets})

  endif()

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

  # The sco.widgets table pin (tests/abi_widgets.c), the same three ways.
  set(_abi_widgets_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_widgets.cpp")
  file(CONFIGURE OUTPUT "${_abi_widgets_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_widgets.c\"\n")
  add_library(sco_abi_widgets_c OBJECT tests/abi_widgets.c)
  add_library(sco_abi_widgets_cxx OBJECT "${_abi_widgets_cxx}")
  set(_abi_widgets_targets sco_abi_widgets_c sco_abi_widgets_cxx)
  if(NOT MSVC)
    add_library(sco_abi_widgets_short_enums OBJECT tests/abi_widgets.c)
    target_compile_options(sco_abi_widgets_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_widgets_targets sco_abi_widgets_short_enums)
  endif()
  foreach(t IN LISTS _abi_widgets_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_widgets
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_widgets_targets})

  # The sco.ui.surfaces table and payload pins (tests/abi_ui_surfaces.c), the same three ways.
  set(_abi_ui_surfaces_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_ui_surfaces.cpp")
  file(CONFIGURE OUTPUT "${_abi_ui_surfaces_cxx}"
       CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_ui_surfaces.c\"\n")
  add_library(sco_abi_ui_surfaces_c OBJECT tests/abi_ui_surfaces.c)
  add_library(sco_abi_ui_surfaces_cxx OBJECT "${_abi_ui_surfaces_cxx}")
  set(_abi_ui_surfaces_targets sco_abi_ui_surfaces_c sco_abi_ui_surfaces_cxx)
  if(NOT MSVC)
    add_library(sco_abi_ui_surfaces_short_enums OBJECT tests/abi_ui_surfaces.c)
    target_compile_options(sco_abi_ui_surfaces_short_enums PRIVATE -fshort-enums)
    list(APPEND _abi_ui_surfaces_targets sco_abi_ui_surfaces_short_enums)
  endif()
  foreach(t IN LISTS _abi_ui_surfaces_targets)
    target_include_directories(${t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${t})
    if(NOT MSVC)
      target_compile_options(${t} PRIVATE -Wpedantic)
    endif()
  endforeach()
  add_test(NAME abi_ui_surfaces
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" ${_abi_config} --target ${_abi_ui_surfaces_targets})

  if(SCO_GAME_SC)
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

  endif()

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
  if(SCO_GAME_SC)
    # One portable G3 fixture; it links real shared model code, not engine reads.
    add_executable(sco_test_game_spatial tests/test_game_spatial.cpp tests/abi_game_spatial.c)
    target_link_libraries(sco_test_game_spatial PRIVATE sco_engine)
    sco_project_target(sco_test_game_spatial)
    add_test(NAME game_spatial COMMAND sco_test_game_spatial)
    add_executable(sco_test_relocation tests/test_relocation.cpp tests/abi_relocation.c)
    target_include_directories(sco_test_relocation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SCO_INCLUDE_DIR}")
    sco_project_target(sco_test_relocation)
    add_test(NAME relocation COMMAND sco_test_relocation)
    set(_relocation_cxx "${CMAKE_CURRENT_BINARY_DIR}/abi_relocation.cpp")
    file(CONFIGURE OUTPUT "${_relocation_cxx}" CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_relocation.c\"\n")
    add_library(sco_abi_relocation_cxx OBJECT "${_relocation_cxx}")
    target_include_directories(sco_abi_relocation_cxx PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(sco_abi_relocation_cxx)
    add_dependencies(sco_test_relocation sco_abi_relocation_cxx)
    if(NOT MSVC)
      add_library(sco_abi_relocation_short_enums OBJECT tests/abi_relocation.c)
      target_include_directories(sco_abi_relocation_short_enums PRIVATE "${SCO_INCLUDE_DIR}")
      target_compile_options(sco_abi_relocation_short_enums PRIVATE -fshort-enums)
      sco_warnings(sco_abi_relocation_short_enums)
      add_dependencies(sco_test_relocation sco_abi_relocation_short_enums)
    endif()
  endif()

  # The MIT bridge layouts (tests/abi_titanlink.c, tests/abi_voxel_bridge.c: sc_titanlink.h and
  # sc_voxel_bridge.h, every size, offset and constant), and the game pack's game.world table and
  # game.* event payloads (tests/abi_game_world.c, tests/abi_game_events.c), the same three ways.
  set(_abi_bridge_pins titanlink voxel_bridge)
  if(SCO_GAME_SC)
    list(APPEND _abi_bridge_pins game_world game_events game_spatial world_control prefabs game_loadout game_hangar game_reputation game_asop game_contracts)
    add_executable(test_prefabs tests/test_prefabs.cpp src/game/services/prefabs.cpp)
    target_include_directories(test_prefabs PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(test_prefabs)
    add_test(NAME prefabs COMMAND test_prefabs)
    add_executable(test_world_control tests/test_world_control.cpp src/game/services/world_control.cpp)
    target_include_directories(test_world_control PRIVATE "${SCO_INCLUDE_DIR}")
    target_link_libraries(test_world_control PRIVATE sco_runtime)
    sco_project_target(test_world_control)
    add_test(NAME world_control COMMAND test_world_control)
    add_executable(test_hangar tests/test_hangar.cpp src/game/services/hangar.cpp)
    target_include_directories(test_hangar PRIVATE "${SCO_INCLUDE_DIR}")
    target_link_libraries(test_hangar PRIVATE sco_host)
    sco_project_target(test_hangar)
    add_test(NAME hangar COMMAND test_hangar)
    add_executable(test_reputation tests/test_reputation.cpp src/game/services/reputation.cpp)
    target_include_directories(test_reputation PRIVATE "${SCO_INCLUDE_DIR}")
    target_link_libraries(test_reputation PRIVATE sco_host)
    sco_project_target(test_reputation)
    add_test(NAME reputation COMMAND test_reputation)
    add_executable(test_inventory tests/test_inventory.cpp src/game/services/inventory.cpp src/game/services/inventory_walk.cpp)
    target_include_directories(test_inventory PRIVATE "${SCO_INCLUDE_DIR}")
    target_link_libraries(test_inventory PRIVATE sco_host)
    sco_project_target(test_inventory)
    add_test(NAME inventory COMMAND test_inventory)
    if(WIN32)
      add_executable(test_inventory_engine tests/test_inventory_engine.cpp src/game/services/inventory_engine.cpp
                                           src/game/services/inventory_walk.cpp)
      target_include_directories(test_inventory_engine PRIVATE "${SCO_INCLUDE_DIR}")
      sco_project_target(test_inventory_engine)
      add_test(NAME inventory_engine COMMAND test_inventory_engine)
    endif()
    add_executable(test_asop tests/test_asop.cpp src/game/services/asop_service.cpp)
    target_include_directories(test_asop PRIVATE "${SCO_INCLUDE_DIR}")
    target_link_libraries(test_asop PRIVATE sco_host)
    sco_project_target(test_asop)
    add_test(NAME asop COMMAND test_asop)
    add_executable(test_contracts tests/test_contracts.cpp src/game/services/contracts_service.cpp)
    target_include_directories(test_contracts PRIVATE "${SCO_INCLUDE_DIR}")
    target_link_libraries(test_contracts PRIVATE sco_host)
    sco_project_target(test_contracts)
    add_test(NAME contracts COMMAND test_contracts)
    add_executable(test_world_free_cam tests/test_world_free_cam.cpp src/game/services/world_free_cam.cpp)
    target_include_directories(test_world_free_cam PRIVATE "${SCO_INCLUDE_DIR}")
    target_link_libraries(test_world_free_cam PRIVATE sco_host)
    sco_project_target(test_world_free_cam)
    add_test(NAME world_free_cam COMMAND test_world_free_cam)
  endif()
  foreach(_b IN LISTS _abi_bridge_pins)
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
  if(SCO_GAME_SC)
    add_dependencies(sco_test_game_spatial sco_abi_game_spatial_cxx)
    if(NOT MSVC)
      add_dependencies(sco_test_game_spatial sco_abi_game_spatial_short_enums)
    endif()
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

  endif()

  if(SCO_GAME_SC)
    add_executable(test_owned_removal tests/test_owned_removal.cpp tests/abi_game_owned.c
                   src/game/services/owned.cpp)
    target_link_libraries(test_owned_removal PRIVATE sco_host)
    sco_project_target(test_owned_removal)
    add_test(NAME test_owned_removal COMMAND test_owned_removal)
  endif()

  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE sco_core)
  if(SCO_GAME_SC)
    target_link_libraries(test_core PRIVATE sco_game_sc)   # the system.quit, pak.* and asop.* rows
  endif()
  sco_project_target(test_core)
  add_test(NAME test_core COMMAND test_core)

  add_executable(test_runtime_diagnostics_policy tests/test_runtime_diagnostics_policy.cpp)
  if(SCO_BUILD_RUNTIME_MCP)
    add_executable(test_runtime_diagnostics_integration tests/test_runtime_diagnostics_integration.cpp)
    target_include_directories(test_runtime_diagnostics_integration PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_runtime_diagnostics_integration PRIVATE sco_diagnostics_ipc sco_plugins)
    add_dependencies(test_runtime_diagnostics_integration sco_core_mcp_runtime_peer)
    sco_project_target(test_runtime_diagnostics_integration)
    add_test(NAME test_runtime_diagnostics_integration
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/sco-core-mcp-runtime/test_integration.py"
        --host "$<TARGET_FILE:test_runtime_diagnostics_integration>"
        --adapter "$<TARGET_FILE:sco_core_mcp_runtime_peer>" --root "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(test_runtime_diagnostics_integration PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME test_runtime_server
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/sco-core-mcp-runtime/test_server.py")
  endif()
  target_include_directories(test_runtime_diagnostics_policy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_runtime_diagnostics_policy PRIVATE sco_app sco_diagnostics_ipc)
  sco_project_target(test_runtime_diagnostics_policy)
  add_test(NAME test_runtime_diagnostics_policy COMMAND test_runtime_diagnostics_policy
    "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
  add_executable(test_runtime_diagnostics_audit_win tests/test_runtime_diagnostics_audit_win.cpp)
  target_include_directories(test_runtime_diagnostics_audit_win PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_runtime_diagnostics_audit_win PRIVATE sco_diagnostics_ipc)
  sco_project_target(test_runtime_diagnostics_audit_win)
  add_executable(test_runtime tests/test_runtime.cpp)
  target_link_libraries(test_runtime PRIVATE sco_runtime)
  sco_project_target(test_runtime)
  add_test(NAME test_runtime COMMAND test_runtime)

  add_executable(test_host tests/test_host.cpp)
  target_link_libraries(test_host PRIVATE sco_host)
  sco_project_target(test_host)
  add_test(NAME test_host COMMAND test_host)

  target_compile_definitions(sco_runtime PRIVATE SCO_RUNTIME_DIAGNOSTICS_FIXTURE=1)
  target_compile_definitions(sco_diagnostics_ipc PRIVATE SCO_RUNTIME_DIAGNOSTICS_FIXTURE=1)
  add_executable(test_runtime_diagnostics_ipc tests/test_runtime_diagnostics_ipc.cpp tests/test_diagnostics_json.cpp)
  target_include_directories(test_runtime_diagnostics_ipc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_runtime_diagnostics_ipc PRIVATE sco_diagnostics_ipc)
  sco_project_target(test_runtime_diagnostics_ipc)
  add_test(NAME test_runtime_diagnostics_ipc COMMAND test_runtime_diagnostics_ipc "${SCO_TEST_OUT}" "${CMAKE_CURRENT_SOURCE_DIR}")
  add_executable(test_diagnostics_platform tests/test_diagnostics_platform.cpp)
  target_link_libraries(test_diagnostics_platform PRIVATE sco_diagnostics_ipc)
  sco_project_target(test_diagnostics_platform)
  add_test(NAME test_diagnostics_platform COMMAND test_diagnostics_platform)
  set_tests_properties(test_diagnostics_platform PROPERTIES ENVIRONMENT "SCO_DIAGNOSTICS_TEST_TMPDIR=${SCO_TEST_OUT}")
  # These fixtures use the same per-user/session diagnostics mapping and peer lease.
  # Keep that resource exclusive without serializing unrelated tests.
  set_tests_properties(test_runtime_diagnostics_policy test_runtime_diagnostics_ipc
    test_diagnostics_platform PROPERTIES RESOURCE_LOCK sco_diagnostics_transport)
  # Independently compiled source publisher: only copied public SDK declarations.
  set(_diagnostic_sdk "${CMAKE_CURRENT_BINARY_DIR}/diagnostics-sdk/include")
  file(MAKE_DIRECTORY "${_diagnostic_sdk}")
  foreach(_h sco_api.h sco_diagnostics_publisher.h)
    configure_file("${SCO_INCLUDE_DIR}/${_h}" "${_diagnostic_sdk}/${_h}" COPYONLY)
  endforeach()
  add_library(diagnostics_publisher_fixture MODULE sdk/examples/diagnostics_publisher/diagnostics_publisher.c)
  target_include_directories(diagnostics_publisher_fixture PRIVATE "${_diagnostic_sdk}")
  sco_project_target(diagnostics_publisher_fixture)
  if(APPLE)
    target_link_options(diagnostics_publisher_fixture PRIVATE "-Wl,-undefined,error")
  elseif(UNIX)
    target_link_options(diagnostics_publisher_fixture PRIVATE "-Wl,--no-undefined")
  endif()
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/abi_diagnostics_publisher.cpp"
    CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_diagnostics_publisher.c\"\n")
  add_library(sco_abi_diagnostics_publisher_c OBJECT tests/abi_diagnostics_publisher.c)
  add_library(sco_abi_diagnostics_publisher_cxx OBJECT "${CMAKE_CURRENT_BINARY_DIR}/abi_diagnostics_publisher.cpp")
  set(_diagnostic_pins sco_abi_diagnostics_publisher_c sco_abi_diagnostics_publisher_cxx)
  if(NOT MSVC)
    add_library(sco_abi_diagnostics_publisher_short OBJECT tests/abi_diagnostics_publisher.c)
    target_compile_options(sco_abi_diagnostics_publisher_short PRIVATE -fshort-enums -Wpedantic)
    list(APPEND _diagnostic_pins sco_abi_diagnostics_publisher_short)
  endif()
  foreach(_t IN LISTS _diagnostic_pins)
    target_include_directories(${_t} PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(${_t})
  endforeach()
  add_executable(test_diagnostics_publishers tests/test_diagnostics_publishers.cpp)
  target_link_libraries(test_diagnostics_publishers PRIVATE sco_app)
  add_dependencies(test_diagnostics_publishers diagnostics_publisher_fixture ${_diagnostic_pins})
  sco_project_target(test_diagnostics_publishers)
  add_test(NAME test_diagnostics_publishers COMMAND test_diagnostics_publishers
    "$<TARGET_FILE:diagnostics_publisher_fixture>" "${CMAKE_CURRENT_BINARY_DIR}/tests/out")

  add_executable(test_runtime_diagnostics_snapshot
    tests/test_runtime_diagnostics_snapshot.cpp tests/test_runtime_diagnostics_services.cpp)
  target_link_libraries(test_runtime_diagnostics_snapshot PRIVATE sco_app)
  sco_project_target(test_runtime_diagnostics_snapshot)
  add_test(NAME test_runtime_diagnostics_snapshot COMMAND test_runtime_diagnostics_snapshot)

  # The death hook's one-owner rule over the real bus: game.actors.died for every death,
  # game.player.died only for your player's, each gated by its own capability. The rule lives in
  # src/game/services/death_events.h, which is portable on purpose (the hook itself is Windows-only),
  # so this runs on any host without a game. It is game pack code (sc_game_events.h), so a
  # kernel-only build (SCO_GAME_SC=OFF) leaves it out.
  if(SCO_GAME_SC)
    add_executable(test_death_events tests/test_death_events.cpp)
    target_include_directories(test_death_events PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_death_events PRIVATE sco_host)
    sco_project_target(test_death_events)
    add_test(NAME test_death_events COMMAND test_death_events)
  endif()

  if(SCO_GAME_SC)
    add_library(sco_loadout STATIC src/game/services/loadout.cpp)
    target_link_libraries(sco_loadout PUBLIC sco_host sco_game_admission)
    sco_project_target(sco_loadout)
    set(_loadout_inc "${SCO_TEST_OUT}/loadout/include")
    file(MAKE_DIRECTORY "${_loadout_inc}")
    configure_file(include/sco_api.h "${_loadout_inc}/sco_api.h" COPYONLY)
    configure_file(include/sc_loadout.h "${_loadout_inc}/sc_loadout.h" COPYONLY)
    set(_loadout_root "${SCO_TEST_OUT}/loadout/plugins")
    if(WIN32)
      set(_loadout_ext dll)
    else()
      set(_loadout_ext so)
    endif()
    file(WRITE "${_loadout_root}/loadout_native/plugin.ini"
         "id = loadout_native\nname = Synthetic loadout\nversion = 1.0.0\napi = 1.1\nkind = native\nentry = loadout_native.${_loadout_ext}\n")
    add_library(sco_loadout_native SHARED tests/game_contracts/loadout/consumer.c)
    target_include_directories(sco_loadout_native PRIVATE "${_loadout_inc}")
    set_target_properties(sco_loadout_native PROPERTIES PREFIX "" OUTPUT_NAME loadout_native SUFFIX ".${_loadout_ext}"
      LIBRARY_OUTPUT_DIRECTORY "$<1:${_loadout_root}/loadout_native>"
      RUNTIME_OUTPUT_DIRECTORY "$<1:${_loadout_root}/loadout_native>")
    if(UNIX AND NOT APPLE)
      target_link_options(sco_loadout_native PRIVATE "-Wl,--no-undefined")
    endif()
    sco_warnings(sco_loadout_native)
    add_executable(test_loadout tests/game_contracts/loadout/test_loadout.cpp tests/game_contracts/loadout/routes.cpp
      tests/game_contracts/loadout/native.cpp)
    add_dependencies(test_loadout sco_loadout_native)
    target_include_directories(test_loadout PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_loadout PRIVATE sco_loadout sco_plugins)
    sco_project_target(test_loadout)
    add_test(NAME test_loadout COMMAND test_loadout "${_loadout_root}")

    add_library(sco_relocation_provider STATIC src/game/services/relocation.cpp)
    target_link_libraries(sco_relocation_provider PUBLIC sco_host sco_game_admission)
    sco_project_target(sco_relocation_provider)
    add_executable(test_relocation_provider tests/game_contracts/relocation/test_provider.cpp)
    target_include_directories(test_relocation_provider PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_relocation_provider PRIVATE sco_relocation_provider)
    sco_project_target(test_relocation_provider)
    add_test(NAME test_relocation_provider COMMAND test_relocation_provider)
  endif()

  # The C++20 SDK layer (include/scosdk/): two SDK plugins over the real host table.
  add_executable(test_sdk tests/test_sdk.cpp)
  target_link_libraries(test_sdk PRIVATE sco_host)
  sco_project_target(test_sdk)
  add_test(NAME test_sdk COMMAND test_sdk)

  if(SCO_GAME_SC)
    # Synthetic C recipes and typed C++ actor/entity pointer-tail negatives.
    # Contract declarations only; never a game-runtime certification.
    add_executable(test_language_routes_c tests/test_language_routes.c)
    target_include_directories(test_language_routes_c PRIVATE "${SCO_INCLUDE_DIR}")
    sco_warnings(test_language_routes_c)
    add_test(NAME test_language_routes_c COMMAND test_language_routes_c)
    add_executable(test_language_routes tests/test_language_routes.cpp)
    target_include_directories(test_language_routes PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(test_language_routes)
    add_test(NAME test_language_routes COMMAND test_language_routes)
  endif()

  # sco.storage over the real host table and SQLite; spawns itself to test crash safety.
  add_executable(test_storage tests/test_storage.cpp)
  target_link_libraries(test_storage PRIVATE sco_storage)
  sco_project_target(test_storage)
  add_test(NAME test_storage COMMAND test_storage "${SCO_TEST_OUT}")

  # sco.settings: the declarations' values, the service over the real host table and storage,
  # persistence across a restart, settings.changed. Host build, no game.
  add_executable(test_settings tests/test_settings.cpp)
  add_executable(test_content tests/test_content.cpp tests/abi_content.c)
  set(_content_inc "${CMAKE_CURRENT_BINARY_DIR}/content_public_include")
  file(MAKE_DIRECTORY "${_content_inc}")
  foreach(h sco_api.h sco_content.h)
    configure_file("${SCO_INCLUDE_DIR}/${h}" "${_content_inc}/${h}" COPYONLY)
  endforeach()
  add_library(content_reader_fixture MODULE sdk/examples/content_reader/content_reader.c)
  target_include_directories(content_reader_fixture PRIVATE "${_content_inc}")
  sco_warnings(content_reader_fixture)
  if(UNIX AND NOT APPLE)
    target_link_options(content_reader_fixture PRIVATE -Wl,--no-undefined)
  endif()
  add_dependencies(test_content content_reader_fixture)
  if(NOT MSVC)
    add_library(sco_abi_content_short_enums OBJECT tests/abi_content.c)
    target_include_directories(sco_abi_content_short_enums PRIVATE "${SCO_INCLUDE_DIR}")
    target_compile_options(sco_abi_content_short_enums PRIVATE -fshort-enums -Wpedantic)
    sco_warnings(sco_abi_content_short_enums)
    add_dependencies(test_content sco_abi_content_short_enums)
  endif()
  target_link_libraries(test_content PRIVATE sco_app)
  sco_project_target(test_content)
  add_test(NAME test_content COMMAND test_content "${SCO_TEST_OUT}" "$<TARGET_FILE:content_reader_fixture>")
  target_link_libraries(test_settings PRIVATE sco_settings)
  sco_project_target(test_settings)
  add_test(NAME test_settings COMMAND test_settings "${SCO_TEST_OUT}")

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
  # Host control uses only an injected synthetic IPv4 transport: no live sockets.
  add_executable(test_net_control tests/test_net_control.cpp
    src/net/service.cpp src/net/scope.cpp src/net/control.cpp)
  # Same production backend, but UDP is replaced by failing socket traps in the fixture.
  target_link_libraries(test_net_control PRIVATE sco_net sco_host Threads::Threads)
  if(SCO_GAME_SC)
    target_sources(test_net_control PRIVATE src/game/private_session.cpp tests/abi_private_session.c)
    target_compile_definitions(test_net_control PRIVATE SCO_TEST_PRIVATE_SESSION=1)
  endif()
  sco_project_target(test_net_control)
  add_test(NAME test_net_control COMMAND test_net_control)

  # sco.ui over the real host table, with built-in plugins under the loader's crash guard.
  add_executable(test_ui tests/test_ui.cpp)
  target_link_libraries(test_ui PRIVATE sco_ui sco_plugins)
  sco_project_target(test_ui)
  add_test(NAME test_ui COMMAND test_ui)

  # sco.ui.surfaces over the real host table, the release hook it shares with sco.ui, and
  # scosdk/ui_surfaces.hpp.
  add_executable(test_surfaces tests/test_surfaces.cpp)
  target_link_libraries(test_surfaces PRIVATE sco_ui)
  sco_project_target(test_surfaces)
  add_test(NAME test_surfaces COMMAND test_surfaces)

  if(SCO_GAME_SC)
    add_executable(test_cvars tests/test_cvars.cpp src/game/services/cvars.cpp)
    target_link_libraries(test_cvars PRIVATE sco_host sco_plugins)
    add_library(cvars_probe MODULE tests/fixtures/cvars_probe.c)
    target_include_directories(cvars_probe PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(cvars_probe)
    # Explicit config placement prevents multi-config generators from putting the
    # module below an implicit Release/Debug directory beside a stale manifest.
    set(_cvars_plugin_root "${CMAKE_CURRENT_BINARY_DIR}/cvars_plugins/$<CONFIG>")
    set_target_properties(cvars_probe PROPERTIES PREFIX "" SUFFIX ".so"
      # Import libraries/exports also share OUTPUT_NAME on MSVC. Isolate all
      # artifacts so parallel read/write probe links cannot race on one .exp.
      ARCHIVE_OUTPUT_DIRECTORY "${_cvars_plugin_root}/cvars_probe"
      LIBRARY_OUTPUT_DIRECTORY "${_cvars_plugin_root}/cvars_probe"
      RUNTIME_OUTPUT_DIRECTORY "${_cvars_plugin_root}/cvars_probe")
    file(GENERATE OUTPUT "${_cvars_plugin_root}/cvars_probe/plugin.ini"
      CONTENT "id=cvars_probe\nname=Cvars probe\nversion=1.0.0\napi=1.1\nkind=native\nentry=$<TARGET_FILE_NAME:cvars_probe>\n")
    add_dependencies(test_cvars cvars_probe)
    target_compile_definitions(test_cvars PRIVATE CVARS_PLUGIN_ROOT="${_cvars_plugin_root}")
    add_executable(test_cvars_write tests/test_cvars_write.cpp src/game/services/cvars.cpp)
    target_link_libraries(test_cvars_write PRIVATE sco_host sco_plugins)
    add_library(cvars_write_probe MODULE tests/fixtures/cvars_probe.c)
    target_include_directories(cvars_write_probe PRIVATE "${SCO_INCLUDE_DIR}")
    target_compile_definitions(cvars_write_probe PRIVATE CVARS_PROBE_WRITE)
    sco_project_target(cvars_write_probe)
    set(_cvars_write_plugin_root "${CMAKE_CURRENT_BINARY_DIR}/cvars_write_plugins/$<CONFIG>")
    set_target_properties(cvars_write_probe PROPERTIES PREFIX "" OUTPUT_NAME "cvars_probe" SUFFIX ".so"
      ARCHIVE_OUTPUT_DIRECTORY "${_cvars_write_plugin_root}/cvars_probe"
      LIBRARY_OUTPUT_DIRECTORY "${_cvars_write_plugin_root}/cvars_probe"
      RUNTIME_OUTPUT_DIRECTORY "${_cvars_write_plugin_root}/cvars_probe")
    file(GENERATE OUTPUT "${_cvars_write_plugin_root}/cvars_probe/plugin.ini"
      CONTENT "id=cvars_probe\nname=Cvars probe\nversion=1.0.0\napi=1.1\nkind=native\nentry=$<TARGET_FILE_NAME:cvars_write_probe>\n")
    target_compile_definitions(test_cvars_write PRIVATE CVARS_PLUGIN_ROOT="${_cvars_write_plugin_root}")
    add_dependencies(test_cvars_write cvars_write_probe)
    sco_project_target(test_cvars_write)
    add_test(NAME test_cvars_write COMMAND test_cvars_write)
    add_test(NAME test_cvars_write_reentry COMMAND test_cvars_write cleanup-reentry)
    add_test(NAME test_cvars_write_fault COMMAND test_cvars_write write-fault)
    add_test(NAME test_cvars_write_mismatch COMMAND test_cvars_write write-mismatch)
    add_executable(test_cvars_extent tests/test_cvars_extent.cpp)
    target_include_directories(test_cvars_extent PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(test_cvars_extent)
    add_test(NAME test_cvars_extent COMMAND test_cvars_extent)
    add_library(abi_game_cvars_write OBJECT tests/abi_game_cvars_write.c)
    target_include_directories(abi_game_cvars_write PRIVATE "${SCO_INCLUDE_DIR}")
    add_library(abi_game_cvars OBJECT tests/abi_game_cvars.c)
    target_include_directories(abi_game_cvars PRIVATE "${SCO_INCLUDE_DIR}")
    sco_project_target(test_cvars)
    add_test(NAME test_cvars COMMAND test_cvars)
  endif()

  add_executable(test_widgets tests/test_widgets.cpp)
  target_link_libraries(test_widgets PRIVATE sco_widgets)
  sco_project_target(test_widgets)
  add_test(NAME test_widgets COMMAND test_widgets)

  add_executable(test_widgets11 tests/test_widgets11.cpp)
  target_link_libraries(test_widgets11 PRIVATE sco_widgets)
  sco_project_target(test_widgets11)
  add_test(NAME test_widgets11 COMMAND test_widgets11)

  add_executable(test_widgets_app tests/test_widgets_app.cpp)
  target_link_libraries(test_widgets_app PRIVATE sco_app)
  sco_project_target(test_widgets_app)
  add_test(NAME test_widgets_app COMMAND test_widgets_app)

  add_executable(test_widgets_sdk tests/test_widgets_sdk.cpp src/widgets/imgui_adapter.cpp)
  target_link_libraries(test_widgets_sdk PRIVATE sco_widgets)
  target_include_directories(test_widgets_sdk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk/third_party/imgui")
  target_compile_definitions(test_widgets_sdk PRIVATE SCO_TEST_IMGUI_ADAPTER)
  sco_project_target(test_widgets_sdk)
  add_test(NAME test_widgets_sdk COMMAND test_widgets_sdk)

  add_executable(test_menu_builder tests/test_menu_builder.cpp)
  target_link_libraries(test_menu_builder PRIVATE sco_widgets)
  sco_project_target(test_menu_builder)
  add_test(NAME test_menu_builder COMMAND test_menu_builder)

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
  # Scope::AllFiles (persistent slot swaps, files/ replacement mounts) over a fake ICryPak; no
  # loader detour, so it runs on every platform.
  add_executable(test_pak_files tests/test_pak_files.cpp)
  target_link_libraries(test_pak_files PRIVATE sco_pak)
  sco_project_target(test_pak_files)
  add_test(NAME test_pak_files COMMAND test_pak_files)
  endif()

  add_executable(test_vfs tests/test_vfs.cpp)
  target_link_libraries(test_vfs PRIVATE sco_vfs)
  sco_project_target(test_vfs)
  add_test(NAME test_vfs COMMAND test_vfs)

  if(SCO_GAME_SC)
  add_executable(test_game_host_data tests/game_host_data.cpp)
  target_link_libraries(test_game_host_data PRIVATE sco_game_host)
  sco_project_target(test_game_host_data)
  add_test(NAME game_host_data COMMAND test_game_host_data "${SCO_TEST_OUT}/host-data")
  add_executable(test_game_host tests/game_host.cpp)
  target_link_libraries(test_game_host PRIVATE sco_game_host)
  sco_project_target(test_game_host)
  add_test(NAME game_host COMMAND test_game_host)
  add_test(NAME game_host_loss COMMAND test_game_host loss)
  add_test(NAME game_host_quit COMMAND test_game_host quit)
  add_test(NAME game_host_quit_missing COMMAND test_game_host missing-quit)
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
  # Service blame and pin tests: four providers (mode 11) and a caller (mode 12).
  list(APPEND _fakes "svc_provider:11" "svc_prov_b:11" "svc_prov_c:11" "svc_prov_d:11" "svc_caller:12")
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

  # The ABI freeze's old binary: tests/fixtures/plugins/native/frozen_v11_plugin.c built against
  # include/sco_api.h exactly as released at sdk-v1.1.0 (the tag's header, written here by
  # tools/abi_snapshot.py; no binary is committed) and loaded by test_plugins. Keep in step with
  # tools/test.sh and tools/test-win.sh.
  set(_frozen "${SCO_TEST_OUT}/frozen_v11/v11")
  set(_frozen_inc "${SCO_TEST_OUT}/frozen_v11_include")
  file(REMOVE_RECURSE "${SCO_TEST_OUT}/frozen_v11" "${_frozen_inc}")
  file(WRITE "${_frozen}/plugin.ini"
       "id = v11\nname = Frozen 1.1\nversion = 1.1.0\napi = 1.1\nkind = native\nentry = v11.${_ext}\n")
  if(Python3_Interpreter_FOUND)
    execute_process(COMMAND "${Python3_EXECUTABLE}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/tools/abi_snapshot.py"
                            released-header "${_frozen_inc}/sco_api.h"
                    RESULT_VARIABLE _frozen_hdr)
    if(NOT _frozen_hdr EQUAL 0)
      message(FATAL_ERROR "tools/abi_snapshot.py released-header failed "
                          "(the sdk-v1.1.0 tag must be reachable: git fetch origin tag sdk-v1.1.0)")
    endif()
    add_library(sco_frozen_v11 SHARED tests/fixtures/plugins/native/frozen_v11_plugin.c)
    target_include_directories(sco_frozen_v11 PRIVATE "${_frozen_inc}")
    set_target_properties(sco_frozen_v11 PROPERTIES
      OUTPUT_NAME v11 PREFIX "" SUFFIX ".${_ext}"
      LIBRARY_OUTPUT_DIRECTORY "$<1:${_frozen}>"
      RUNTIME_OUTPUT_DIRECTORY "$<1:${_frozen}>")
    sco_warnings(sco_frozen_v11)
  else()
    message(WARNING "python3 not found: the frozen sdk-v1.1.0 plugin is not built; "
                    "test_plugins' frozen-plugin test will report it missing")
  endif()

  add_executable(test_manifest tests/test_manifest.cpp)
  target_link_libraries(test_manifest PRIVATE sco_plugins)
  sco_project_target(test_manifest)
  add_test(NAME test_manifest COMMAND test_manifest)

  add_executable(test_plugins tests/test_plugins.cpp)
  target_link_libraries(test_plugins PRIVATE sco_plugins)
  add_dependencies(test_plugins ${_fake_targets})
  if(TARGET sco_frozen_v11)
    add_dependencies(test_plugins sco_frozen_v11)
  endif()
  sco_project_target(test_plugins)
  add_test(NAME test_plugins
           COMMAND test_plugins "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plugins" "${SCO_TEST_OUT}")

  # sco-plugin-check's stand-in host (built as sdk/CMakeLists.txt builds it) on one plugin per
  # PROBE_MODE of tests/fixtures/plugins/native/checker_probe.c: release_service on a borrowed
  # name, --short-tables (sco_api and sco.widgets at their 1.0 extents), and sco.widgets 1.1.
  # Cases in tests/plugin_check.cmake.
  add_executable(sco-plugin-check sdk/tools/sco-plugin-check.c)
  target_include_directories(sco-plugin-check PRIVATE "${SCO_INCLUDE_DIR}")
  # gnu11 like sdk/CMakeLists.txt (the root sets C_EXTENSIONS OFF); the checker uses POSIX lstat.
  set_target_properties(sco-plugin-check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
  if(MSVC)
    target_compile_options(sco-plugin-check PRIVATE /W4 /WX /wd5105 /D_CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(sco-plugin-check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(sco-plugin-check PRIVATE ${CMAKE_DL_LIBS})
  endif()
  set(_chk "${SCO_TEST_OUT}/plugin_check")
  file(REMOVE_RECURSE "${_chk}")
  foreach(probe IN ITEMS chk_release_own:0 chk_release_borrowed:1 chk_tail_unchecked:2 chk_tail_checked:3
                      chk_w11_unchecked:4 chk_w11_checked:5)
    string(REPLACE ":" ";" probe "${probe}")
    list(GET probe 0 id)
    list(GET probe 1 mode)
    file(WRITE "${_chk}/${id}/plugin.ini"
         "id = ${id}\nname = Checker probe ${id}\nversion = 1.0.0\napi = 1.0\nkind = native\nentry = ${id}.${_ext}\n")
    add_library(sco_chk_${id} SHARED tests/fixtures/plugins/native/checker_probe.c)
    target_include_directories(sco_chk_${id} PRIVATE "${SCO_INCLUDE_DIR}")
    target_compile_definitions(sco_chk_${id} PRIVATE PROBE_MODE=${mode} "PROBE_ID=\"${id}\"")
    set_target_properties(sco_chk_${id} PROPERTIES
      OUTPUT_NAME ${id} PREFIX "" SUFFIX ".${_ext}"
      LIBRARY_OUTPUT_DIRECTORY "$<1:${_chk}/${id}>"
      RUNTIME_OUTPUT_DIRECTORY "$<1:${_chk}/${id}>")
    sco_warnings(sco_chk_${id})
    add_dependencies(sco-plugin-check sco_chk_${id})
  endforeach()
  add_test(NAME plugin_check_services
           COMMAND "${CMAKE_COMMAND}" "-DCHECK=$<TARGET_FILE:sco-plugin-check>" "-DROOT=${_chk}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_check.cmake")
  set_tests_properties(plugin_check_services PROPERTIES FAIL_REGULAR_EXPRESSION "plugin_check_services: FAIL")

  if(SCO_GAME_SC)
    # Public game CONTRACT consumers on the neutral host, not game implementation.
    # Kept outside core-source; no game library is linked into this fixture.
    set(_conformance "${SCO_TEST_OUT}/sdk_conformance/plugins")
    foreach(id conformance conformance_peer)
      file(WRITE "${_conformance}/${id}/plugin.ini"
           "id = ${id}\nname = SDK conformance\nversion = 1.0.0\napi = 1.1\nkind = native\nentry = ${id}.${_ext}\n")
      add_library(sco_sdk_${id} SHARED tests/game_contracts/conformance/consumer.c)
      target_include_directories(sco_sdk_${id} PRIVATE "${SCO_INCLUDE_DIR}")
      target_compile_definitions(sco_sdk_${id} PRIVATE "CONSUMER_ID=\"${id}\"")
      set_target_properties(sco_sdk_${id} PROPERTIES
        OUTPUT_NAME ${id} PREFIX "" SUFFIX ".${_ext}"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${_conformance}/${id}>"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${_conformance}/${id}>")
      sco_project_target(sco_sdk_${id})
    endforeach()
    add_executable(test_sdk_conformance tests/game_contracts/conformance/host.cpp)
    target_link_libraries(test_sdk_conformance PRIVATE sco_host sco_plugins)
    add_dependencies(test_sdk_conformance sco_sdk_conformance sco_sdk_conformance_peer sco_fake_svc_provider)
    sco_project_target(test_sdk_conformance)
    add_test(NAME sdk_conformance
             COMMAND test_sdk_conformance "${_conformance}" "${_plug}")
    set_tests_properties(sdk_conformance PROPERTIES LABELS "sdk;game-contract;neutral-host")
  endif()

  add_executable(test_lua tests/test_lua.cpp)
  target_link_libraries(test_lua PRIVATE sco_plugins sco_host sco_lua sco_ui sco_storage sco_settings sco_widgets)
  if(SCO_GAME_SC)
    target_link_libraries(test_lua PRIVATE sco_datacore_service sco_lua_game)   # TestDataCore, the sco.game tests
  endif()
  sco_project_target(test_lua)
  add_test(NAME test_lua COMMAND test_lua "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${SCO_TEST_OUT}")
  add_test(NAME test_lua_widgets COMMAND test_lua "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${SCO_TEST_OUT}/widgets" widgets)

  # The host kit with built-ins, two fake plugins, greeter and travel_pack.
  add_executable(test_app tests/test_app.cpp)
  target_link_libraries(test_app PRIVATE sco_app sco_lua)
  if(SCO_GAME_SC)
    target_link_libraries(test_app PRIVATE sco_lua_game)
  endif()
  add_dependencies(test_app ${_fake_targets})
  sco_project_target(test_app)
  add_test(NAME test_app COMMAND test_app "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${SCO_TEST_OUT}")
  add_test(NAME test_shutdown_drain COMMAND test_app "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${SCO_TEST_OUT}" shutdown-only)

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
  add_library(sco_sim_cpp_hello SHARED sdk/examples/cpp_hello/cpp_hello.cpp)
  target_include_directories(sco_sim_cpp_hello PRIVATE "${SCO_INCLUDE_DIR}")
  set_target_properties(sco_sim_cpp_hello PROPERTIES
    OUTPUT_NAME cpp_hello PREFIX "" SUFFIX ".${_ext}"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${_sim}/cpp_hello>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${_sim}/cpp_hello>")
  sco_project_target(sco_sim_cpp_hello)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/cpp_hello/plugin.ini" _cpp_hello_ini)
  if(NOT WIN32)
    string(REPLACE "entry = cpp_hello.dll" "entry = cpp_hello.so" _cpp_hello_ini "${_cpp_hello_ini}")
  endif()
  file(WRITE "${_sim}/cpp_hello/plugin.ini" "${_cpp_hello_ini}")
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/greeter" DESTINATION "${_sim}")
  if(SCO_GAME_SC)
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/travel_pack"
              "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/quantum_pack" DESTINATION "${_sim}")
  endif()
  add_dependencies(sco-host-sim sco_sim_hello sco_sim_cpp_hello)
  # tests/host_sim.cmake runs the sim and prints "host_sim_examples: FAIL" unless it exits 0 and
  # answers the invoke; CTest needs the reply and no FAIL line.
  add_test(NAME host_sim_examples
           COMMAND "${CMAKE_COMMAND}" "-DSIM=$<TARGET_FILE:sco-host-sim>" "-DROOT=${_sim}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/host_sim.cmake")
  set_tests_properties(host_sim_examples PROPERTIES
    PASS_REGULAR_EXPRESSION "hello\\.wave -> OK \"Hello, Pilot One\""
    FAIL_REGULAR_EXPRESSION "host_sim_examples: FAIL")
endif()
