cmake_minimum_required(VERSION 3.20)
project(sco_cpp_hello CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/sco-plugin.cmake")

sco_add_plugin(cpp_hello SOURCES cpp_hello.cpp INI plugin.ini)
