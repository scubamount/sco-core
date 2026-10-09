cmake_minimum_required(VERSION 3.20)
project(sco_quantum_pack NONE)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/sco-plugin.cmake")

# Nothing to compile; install copies plugin.ini and datacore/ into data/plugins/quantum_pack/.
sco_add_pack(quantum_pack DIR "${CMAKE_CURRENT_LIST_DIR}")
