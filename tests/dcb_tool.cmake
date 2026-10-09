# CTest dcb_tool: runs sco-dcb over the fixtures test_datacore writes into DIR and checks the exit
# codes and output: info on a valid file exits 0 with "layout: OK", on a truncated one 1, on a
# missing file 2; records lists the fixture's records; patch adds a record and writes a file that
# info accepts and records lists, and refuses a missing field with exit 1.
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
run(0 "\tShipA\tShip\t0x[0-9a-f]+\t0\t139\tlibs/foundry/records/test/ships.xml\tShips" records "${DIR}/datacore_36.dcb")
file(REMOVE "${DIR}/datacore_36_patched.dcb" "${DIR}/datacore_36_refused.dcb")
run(0 "op 2: OK AddRecord record \"ShipC\".*Emit: OK" patch "${DIR}/datacore_36.dcb" "${DIR}/datacore_36_patched.dcb"
    --seed 1 set ShipA speed 2.5 add-record Ship ShipC ShipA - set ShipB maker record:ShipC)
run(0 "capabilities: datacore.patch on, datacore.add_record on.*layout: OK" info "${DIR}/datacore_36_patched.dcb")
run(0 "\n5\t[^\t]+\tShipC\tShip\t0x[0-9a-f]+\t2\t139\tlibs/foundry/records/sco/sco-dcb/ShipC.xml\tShips"
    records "${DIR}/datacore_36_patched.dcb")
run(1 "REFUSED \\(field not found\\).*nothing written" patch "${DIR}/datacore_36.dcb" "${DIR}/datacore_36_refused.dcb"
    set ShipA speedX 1)
if(EXISTS "${DIR}/datacore_36_refused.dcb")
  message("dcb_tool: FAIL (a refused patch wrote a file)")
  message(FATAL_ERROR "unexpected output file")
endif()
message("dcb_tool: OK")
