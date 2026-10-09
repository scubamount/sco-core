cmake_minimum_required(VERSION 3.20)
project(sco_sdk C CXX)

# Builds every example, the template and sco-plugin-check in one go. Works from the unpacked
# SDK zip and from sdk/ in the sco-core repository.
#
#   cmake -S . -B build && cmake --build build && cmake --install build --prefix out
#
# out/data/plugins/ then holds hello/, cpp_hello/, travel_pack/, quantum_pack/, greeter/, notebook/ and my_plugin/, laid out the way
# sc-offline reads them, and out/bin/ holds sco-plugin-check.

include("${CMAKE_CURRENT_LIST_DIR}/cmake/sco-plugin.cmake")

add_executable(sco-plugin-check tools/sco-plugin-check.c)
target_include_directories(sco-plugin-check PRIVATE "${SCO_SDK_INCLUDE_DIR}")
set_target_properties(sco-plugin-check PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(MSVC)
  # C5105: older Windows SDK headers trip the C11 preprocessor (winbase.h); not our code.
  target_compile_options(sco-plugin-check PRIVATE /W4 /WX /wd5105 /D_CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(sco-plugin-check PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(sco-plugin-check PRIVATE ${CMAKE_DL_LIBS})
endif()
install(TARGETS sco-plugin-check RUNTIME DESTINATION bin)

sco_add_plugin(hello SOURCES examples/hello/hello.c INI examples/hello/plugin.ini)
sco_add_plugin(cpp_hello SOURCES examples/cpp_hello/cpp_hello.cpp INI examples/cpp_hello/plugin.ini)
sco_add_plugin(my_plugin SOURCES template/plugin.c INI template/plugin.ini)
sco_add_pack(travel_pack DIR examples/travel_pack)
sco_add_pack(quantum_pack DIR examples/quantum_pack)
sco_add_pack(greeter DIR examples/greeter)
sco_add_pack(notebook DIR examples/notebook)
