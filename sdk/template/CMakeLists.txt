cmake_minimum_required(VERSION 3.20)
project(my_plugin C)

# Path to the unpacked SDK. The default works while this folder sits in the SDK's template/;
# after you copy it elsewhere, pass -DSCO_SDK=<path to the unpacked sco-sdk folder>.
set(SCO_SDK "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "Unpacked sco SDK")
include("${SCO_SDK}/cmake/sco-plugin.cmake")

sco_add_plugin(my_plugin SOURCES plugin.c INI plugin.ini)
