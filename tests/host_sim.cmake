# CTest host_sim_examples: runs sco-host-sim over the SDK examples laid out under ROOT and
# fails unless it exits 0 and hello.wave answers OK.
#   cmake -DSIM=<sco-host-sim> -DROOT=<plugin root> -P tests/host_sim.cmake
execute_process(COMMAND "${SIM}" "${ROOT}" --ticks 3 --invoke hello.wave "Pilot One"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc STREQUAL "0")
  message("host_sim_examples: FAIL (sco-host-sim exit ${rc})")
  message(FATAL_ERROR "sco-host-sim exited with ${rc}")
endif()
if(NOT out MATCHES "hello\\.wave -> OK \"Hello, Pilot One\"")
  message("host_sim_examples: FAIL (no reply from hello.wave)")
  message(FATAL_ERROR "hello.wave did not answer")
endif()
