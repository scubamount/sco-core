cmake_minimum_required(VERSION 3.20)
project(sco_travel_pack NONE)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/sco-plugin.cmake")

# Nothing to compile; install copies plugin.ini and lists/ into data/plugins/travel_pack/.
sco_add_pack(travel_pack DIR "${CMAKE_CURRENT_LIST_DIR}")
