# CTest dcb_pack: sco-dcb's pack commands over the golden files, the sample pack
# (sdk/examples/quantum_pack) and the fixture test_datacore_pack writes into DIR with the record and
# field names the sample uses: lint and check exit codes and output, show, and diff whose output
# applies back (the round trip).
#   cmake -DDCB=<sco-dcb> -DROOT=<repo root> -DDIR=<fixture dir> -P tests/dcb_pack.cmake
function(run expect_rc pattern)
  execute_process(COMMAND "${DCB}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message("$ sco-dcb ${ARGN}\n${out}${err}")
  if(NOT rc STREQUAL "${expect_rc}")
    message("dcb_pack: FAIL (sco-dcb ${ARGN}: exit ${rc}, expected ${expect_rc})")
    message(FATAL_ERROR "unexpected exit code")
  endif()
  if(NOT "${out}${err}" MATCHES "${pattern}")
    message("dcb_pack: FAIL (sco-dcb ${ARGN}: no match for ${pattern})")
    message(FATAL_ERROR "unexpected output")
  endif()
endfunction()

set(G "${ROOT}/tests/fixtures/datacore/golden")
set(SAMPLE "${ROOT}/sdk/examples/quantum_pack")
set(EOS "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem")

run(0 "lint: 1 file, 0 failed" lint "${SAMPLE}")
run(0 "OK +[^\n]*good_all.toml: 13 operations \\(atomic = false\\).*lint: 2 files, 0 failed" lint "${G}/good_all.toml" "${G}/good_minimal.toml")
run(1 "FAIL [^\n]*bad_unknown_key.toml:6: \\[\\[set\\]\\]: unknown key \"feild\".*lint: 2 files, 1 failed" lint "${G}/bad_unknown_key.toml" "${G}/good_minimal.toml")
run(1 "bad_syntax.toml:3: TOML:" lint "${G}/bad_syntax.toml")
run(2 "no datacore folder" lint "${G}")
run(2 "usage" lint)

run(0 "APPLIED 6/6.*emit: OK, [0-9]+ splices.*\\[datacore\\] 1 pack: quantum_pack 6/6 applied" check "${DIR}/quantum_fixture.dcb" "${SAMPLE}")
run(1 "line 9: SKIP [^\n]*no property \"spoolTime\" in SQuantumDriveParams.*missing_field refused" check "${DIR}/quantum_fixture.dcb" "${ROOT}/tests/fixtures/datacore/missing_field")
run(1 "REFUSED \\(doesn't parse\\)" check "${DIR}/quantum_fixture.dcb" "${G}/bad_set_twice.toml")
run(2 "missing.dcb" check "${DIR}/missing.dcb" "${SAMPLE}")

run(0 "Components\\[1\\]\\.params\\.spoolUpTime = 5\\.1" show "${DIR}/quantum_fixture.dcb" "${EOS}")
run(0 "params\\.spoolUpTime = 3\\.5" show "${DIR}/quantum_fixture_patched.dcb" "guid:08a5bfdb-1972-421f-83fe-be03b7ac5222" "Components[SCItemQuantumDriveParams].params")
run(1 "record Nope not found" show "${DIR}/quantum_fixture.dcb" Nope)
run(1 "no property" show "${DIR}/quantum_fixture.dcb" "${EOS}" "Components[SCItemQuantumDriveParams].params.nope")

# diff: the sample's changes come back as a pack that applies to the base again.
execute_process(COMMAND "${DCB}" diff "${DIR}/quantum_fixture.dcb" "${DIR}/quantum_fixture_patched.dcb"
                RESULT_VARIABLE rc OUTPUT_FILE "${DIR}/quantum_diff.toml" ERROR_VARIABLE err)
file(READ "${DIR}/quantum_diff.toml" diff)
message("$ sco-dcb diff (exit ${rc})\n${diff}${err}")
if(NOT rc STREQUAL "0" OR NOT diff MATCHES "spoolUpTime\"\nvalue = 3\\.5" OR NOT diff MATCHES "\\[\\[instance\\]\\]")
  message("dcb_pack: FAIL (diff)")
  message(FATAL_ERROR "diff")
endif()
run(0 "APPLIED" check "${DIR}/quantum_fixture.dcb" "${DIR}/quantum_diff.toml")
run(0 "# sco-dcb diff.*format = 1" diff "${DIR}/quantum_fixture.dcb" "${DIR}/quantum_fixture.dcb")
message("dcb_pack: OK")
