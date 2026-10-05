# kawac -- the Kawa compiler.
#
# Build:  cmake -B build -G Ninja && cmake --build build
# Test:   ctest --test-dir build --output-on-failure
#
# The classic Makefile stays for now; both drive the same sources. Ninja via
# CMake is the production path: true dependency-graph incrementality, no
# shell re-exec per rule, and CTest for parallel test driving.

cmake_minimum_required(VERSION 3.28)
project(kawac C)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND llvm-config --bindir OUTPUT_VARIABLE LLVM_BINDIR
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(MEMORY_HEADER ${CMAKE_BINARY_DIR}/generated/kawa_memory_bc.h)
set(MEMORY_METRICS_HEADER ${CMAKE_BINARY_DIR}/generated/kawa_memory_metrics_bc.h)
add_custom_command(OUTPUT ${MEMORY_HEADER} ${MEMORY_METRICS_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_runtime.py
        --clang ${LLVM_BINDIR}/clang --source ${CMAKE_SOURCE_DIR}/src/runtime/kawa_memory.c
        --output ${MEMORY_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_runtime.py
        --clang ${LLVM_BINDIR}/clang --source ${CMAKE_SOURCE_DIR}/src/runtime/kawa_memory.c
        --output ${MEMORY_METRICS_HEADER} --symbol kawa_memory_metrics_bc --metrics
    DEPENDS src/runtime/kawa_memory.c src/runtime/kawa_memory.h scripts/embed_runtime.py
    VERBATIM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Mirror the Makefile's flags. -march=native is intentional: kawac's own
# hot loops (lexing, parsing, IR construction) are host-local builds.
set(KAWA_CFLAGS -Wall -Wno-unused-parameter -march=native
	-funroll-loops -D_POSIX_C_SOURCE=200809L)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
	list(APPEND KAWA_CFLAGS -Wno-unknown-warning-option)
endif()

execute_process(COMMAND llvm-config --cflags
	OUTPUT_VARIABLE LLVM_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
	COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(LLVM_CFLAGS NATIVE_COMMAND "${LLVM_CFLAGS}")

execute_process(COMMAND llvm-config --ldflags --link-shared --libs --system-libs
	core executionengine analysis native passes coroutines bitwriter bitreader linker
	OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE
	COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(LLVM_LIBS NATIVE_COMMAND "${LLVM_LIBS}")

file(GLOB KAWA_SRC src/*.c)
file(GLOB KAWA_CODEGEN_SRC src/codegen/*.c)

add_executable(kawac
	${MEMORY_HEADER}
	${MEMORY_METRICS_HEADER}
	${KAWA_SRC}
	${KAWA_CODEGEN_SRC}
	timbr/timbr.c)
target_include_directories(kawac PRIVATE include timbr src/codegen ${CMAKE_BINARY_DIR}/generated)
target_compile_options(kawac PRIVATE ${KAWA_CFLAGS} ${LLVM_CFLAGS}
	$<$<CONFIG:Release>:-O3>)
target_link_libraries(kawac PRIVATE ${LLVM_LIBS})
target_link_options(kawac PRIVATE ${LLVM_CFLAGS})

# Debug variant with runtime checks (bounds traps) -- mirrors `make debug`.
add_executable(kawac_debug EXCLUDE_FROM_ALL
	${MEMORY_HEADER}
	${MEMORY_METRICS_HEADER}
	${KAWA_SRC}
	${KAWA_CODEGEN_SRC}
	timbr/timbr.c)
target_include_directories(kawac_debug PRIVATE include timbr src/codegen ${CMAKE_BINARY_DIR}/generated)
target_compile_options(kawac_debug PRIVATE ${KAWA_CFLAGS} ${LLVM_CFLAGS} -g -O0)
target_compile_definitions(kawac_debug PRIVATE KAWA_DEBUG=1)
target_link_libraries(kawac_debug PRIVATE ${LLVM_LIBS})

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Same implementation as embedded bitcode, with a two-bit generation counter
# to make identity exhaustion executable in CI. ASan/UBSan checks the checker.
add_executable(memory_runtime_test tests/runtime/memory.c src/runtime/kawa_memory.c)
target_include_directories(memory_runtime_test PRIVATE src/runtime)
target_compile_definitions(memory_runtime_test PRIVATE KAWA_GENERATION_MAX=3 KAWA_MEMORY_METRICS)
target_compile_options(memory_runtime_test PRIVATE -UNDEBUG -O2 -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(memory_runtime_test PRIVATE -fsanitize=address,undefined)
find_package(Threads REQUIRED)
target_link_libraries(memory_runtime_test PRIVATE Threads::Threads)
add_test(NAME memory-runtime COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_memory_runtime.py $<TARGET_FILE:memory_runtime_test>)
add_test(NAME check-mode COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_check_mode.py $<TARGET_FILE:kawac>)
add_test(NAME tooling COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_tooling.py $<TARGET_FILE:kawac>)
add_test(NAME optimization-report COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_optimization_report.py $<TARGET_FILE:kawac>)

# Layer 1: smoke test -- the compiler builds and compiles its own test suite
# entry point without crashing.
add_test(NAME smoke
	COMMAND ${CMAKE_SOURCE_DIR}/scripts/smoke.sh $<TARGET_FILE:kawac>)

# Both this runner and the optional lit adapter use scripts/test_support.py.
# No dependency on a particular vendored lit/Python installation.
add_test(NAME unit
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test.py
		--kawac $<TARGET_FILE:kawac> --strict
		--json ${CMAKE_BINARY_DIR}/unit-results.json)
set_tests_properties(unit PROPERTIES TIMEOUT 600)

add_test(NAME harness
	COMMAND ${Python3_EXECUTABLE} -m unittest discover
		-s ${CMAKE_SOURCE_DIR}/scripts/harness_tests -v)

# Benchmarks build every language from source and verify output before timing.
add_custom_target(bench
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench.py
		--kawac $<TARGET_FILE:kawac>
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	USES_TERMINAL
	DEPENDS kawac)

add_custom_target(bench-verify
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench.py
		--kawac $<TARGET_FILE:kawac> --quick --verify-only --sanitize-c
		--json ${CMAKE_BINARY_DIR}/benchmark-verification.json
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	USES_TERMINAL
	DEPENDS kawac)

add_custom_target(bench-memory
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_memory.py
        --kawac $<TARGET_FILE:kawac> --json ${CMAKE_BINARY_DIR}/memory-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS kawac)

add_custom_target(bench-owners
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_owners.py
        --kawac $<TARGET_FILE:kawac> --json ${CMAKE_BINARY_DIR}/owners-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS kawac)

add_custom_target(bench-subobjects
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_memory.py
        --kawac $<TARGET_FILE:kawac> --workload subobject_walk
        --rounds 10000000 --json ${CMAKE_BINARY_DIR}/subobject-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS kawac)

add_custom_target(bench-tools
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_tools.py
        --kawac $<TARGET_FILE:kawac> --json ${CMAKE_BINARY_DIR}/compiler-tools-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS kawac)
