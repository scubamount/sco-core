cmake_minimum_required(VERSION 3.20)
project(sco_greeter_lua NONE)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/sco-plugin.cmake")

# Nothing to compile; install copies plugin.ini and main.lua into data/plugins/greeter/.
sco_add_pack(greeter DIR "${CMAKE_CURRENT_LIST_DIR}")
