# Validates the installed CMake package with an independent downstream consumer.
# Invoked by ctest as: cmake -P tests/run_downstream.cmake
#   -DBUILD_DIR=... -DSOURCE_DIR=... -DPREFIX=...
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
                RESULT_VARIABLE r0)
if(NOT r0 EQUAL 0)
  message(FATAL_ERROR "install failed: ${r0}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/downstream"
                           -B "${BUILD_DIR}/downstream"
                           -DCMAKE_PREFIX_PATH=${PREFIX}
                           -DCMAKE_BUILD_TYPE=Release
                RESULT_VARIABLE r1)
if(NOT r1 EQUAL 0)
  message(FATAL_ERROR "consumer configure failed: ${r1}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}/downstream" --config Release
                RESULT_VARIABLE r2)
if(NOT r2 EQUAL 0)
  message(FATAL_ERROR "consumer build failed: ${r2}")
endif()
execute_process(COMMAND "${BUILD_DIR}/downstream/consumer.exe" RESULT_VARIABLE r3 OUTPUT_VARIABLE out)
if(NOT r3 EQUAL 0)
  message(FATAL_ERROR "consumer run failed: ${r3}")
endif()
message(STATUS "downstream: ${out}")