# CTest dcb_tool: runs sco-dcb over the fixtures test_datacore writes into DIR and checks the exit
# codes and output: info on a valid file exits 0 with "layout: OK", on a truncated one 1, on a
# missing file 2; records lists the fixture's records.
#   cmake -DDCB=<sco-dcb> -DDIR=<fixture dir> -P tests/dcb_tool.cmake
function(run expect_rc pattern)
  execute_process(COMMAND "${DCB}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message("$ sco-dcb ${ARGN}\n${out}${err}")
  if(NOT rc STREQUAL "${expect_rc}")
    message("dcb_tool: FAIL (sco-dcb ${ARGN}: exit ${rc}, expected ${expect_rc})")
    message(FATAL_ERROR "unexpected exit code")
  endif()
  if(NOT "${out}${err}" MATCHES "${pattern}")
    message("dcb_tool: FAIL (sco-dcb ${ARGN}: no match for ${pattern})")
    message(FATAL_ERROR "unexpected output")
  endif()
endfunction()

run(0 "record size: 36 bytes.*layout: OK" info "${DIR}/datacore_36.dcb")
run(0 "record size: 32 bytes.*layout: OK" info "${DIR}/datacore_32.dcb")
run(1 "layout refused \\(totals\\)" info "${DIR}/datacore_bad.dcb")
run(2 "datacore_missing.dcb" info "${DIR}/datacore_missing.dcb")
run(2 "usage" info)
run(0 "\tShipA\tShip\t0x00001111\t0\t139\tlibs/foundry/records/test/ships.xml" records "${DIR}/datacore_36.dcb")
message("dcb_tool: OK")
