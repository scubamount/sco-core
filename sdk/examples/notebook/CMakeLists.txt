cmake_minimum_required(VERSION 3.20)
project(sco_notebook_lua NONE)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/sco-plugin.cmake")

# Nothing to compile; install copies plugin.ini and main.lua into data/plugins/notebook/.
sco_add_pack(notebook DIR "${CMAKE_CURRENT_LIST_DIR}")
