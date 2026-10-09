cmake_minimum_required(VERSION 3.20)
project(sco_hello C)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/sco-plugin.cmake")

sco_add_plugin(hello SOURCES hello.c INI plugin.ini)
