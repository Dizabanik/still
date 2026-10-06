# still -- the Whisky compiler.
#
# Build:  cmake -B build -G Ninja && cmake --build build
# Test:   ctest --test-dir build --output-on-failure
#
# The classic Makefile stays for now; both drive the same sources. Ninja via
# CMake is the production path: true dependency-graph incrementality, no
# shell re-exec per rule, and CTest for parallel test driving.

cmake_minimum_required(VERSION 3.28)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" STILL_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
if(NOT STILL_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "VERSION must contain a semantic version")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" STILL_CORE_VERSION "${STILL_VERSION}")
project(still VERSION "${STILL_CORE_VERSION}" LANGUAGES C)
option(STILL_NATIVE_CPU "Optimize the compiler and embedded runtimes for this CPU" ON)
include(GNUInstallDirs)
include(CTest)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
find_program(LLVM_CONFIG_EXECUTABLE NAMES llvm-config llvm-config-21 REQUIRED)
execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --version OUTPUT_VARIABLE LLVM_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT LLVM_VERSION MATCHES "^21\\.")
    message(FATAL_ERROR "Whisky requires LLVM 21; ${LLVM_CONFIG_EXECUTABLE} reports ${LLVM_VERSION}. Put LLVM 21's bin directory first on PATH.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/timbr/timbr.c")
    message(FATAL_ERROR "Missing timbr submodule. Run: git submodule update --init --recursive")
endif()
execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --bindir OUTPUT_VARIABLE LLVM_BINDIR
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --libdir OUTPUT_VARIABLE LLVM_LIBDIR
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(RUNTIME_CPU_ARGS)
if(STILL_NATIVE_CPU)
    list(APPEND RUNTIME_CPU_ARGS --native)
endif()
set(IO_HEADER ${CMAKE_BINARY_DIR}/generated/wky_runtime_bc.h)
set(MEMORY_HEADER ${CMAKE_BINARY_DIR}/generated/wky_memory_bc.h)
set(MEMORY_METRICS_HEADER ${CMAKE_BINARY_DIR}/generated/wky_memory_metrics_bc.h)
add_custom_command(OUTPUT ${IO_HEADER} ${MEMORY_HEADER} ${MEMORY_METRICS_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_runtime.py
        --clang ${LLVM_BINDIR}/clang --source ${CMAKE_SOURCE_DIR}/src/runtime/wky_runtime.c
        --output ${IO_HEADER} --symbol wky_runtime_bc ${RUNTIME_CPU_ARGS}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_runtime.py
        --clang ${LLVM_BINDIR}/clang --source ${CMAKE_SOURCE_DIR}/src/runtime/wky_memory.c
        --output ${MEMORY_HEADER} ${RUNTIME_CPU_ARGS}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_runtime.py
        --clang ${LLVM_BINDIR}/clang --source ${CMAKE_SOURCE_DIR}/src/runtime/wky_memory.c
        --output ${MEMORY_METRICS_HEADER} --symbol wky_memory_metrics_bc --metrics ${RUNTIME_CPU_ARGS}
    DEPENDS src/runtime/wky_runtime.c src/runtime/wky_memory.c src/runtime/wky_memory.h scripts/embed_runtime.py
    VERBATIM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Keep native optimization for local work; distribution builds opt out.
set(STILL_CFLAGS -Wall -Wno-unused-parameter
	-funroll-loops -D_POSIX_C_SOURCE=200809L)
if(STILL_NATIVE_CPU)
    list(APPEND STILL_CFLAGS -march=native)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
	list(APPEND STILL_CFLAGS -Wno-unknown-warning-option)
endif()

execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --cflags
	OUTPUT_VARIABLE LLVM_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
	COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(LLVM_CFLAGS NATIVE_COMMAND "${LLVM_CFLAGS}")

execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --ldflags --link-shared --libs --system-libs
	core executionengine analysis native passes coroutines bitwriter bitreader linker
	OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE
	COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(LLVM_LIBS NATIVE_COMMAND "${LLVM_LIBS}")

file(GLOB STILL_SRC CONFIGURE_DEPENDS src/*.c)
file(GLOB STILL_CODEGEN_SRC CONFIGURE_DEPENDS src/codegen/*.c)

add_executable(still
	${IO_HEADER}
	${MEMORY_HEADER}
	${MEMORY_METRICS_HEADER}
	${STILL_SRC}
	${STILL_CODEGEN_SRC}
	timbr/timbr.c)
target_include_directories(still PRIVATE ${CMAKE_BINARY_DIR}/generated include timbr src/codegen)
target_compile_definitions(still PRIVATE STILL_VERSION="${STILL_VERSION}")
target_compile_options(still PRIVATE ${STILL_CFLAGS} ${LLVM_CFLAGS}
	$<$<CONFIG:Release>:-O3>)
target_link_libraries(still PRIVATE ${LLVM_LIBS})
set_target_properties(still PROPERTIES BUILD_RPATH "${LLVM_LIBDIR}" INSTALL_RPATH "${LLVM_LIBDIR}")
install(TARGETS still RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE NOTICE third_party/timbr.LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/still)
install(FILES README.md INSTALL.md IMPLEMENTATION.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Debug variant with runtime checks (bounds traps) -- mirrors `make debug`.
add_executable(still_debug EXCLUDE_FROM_ALL
	${IO_HEADER}
	${MEMORY_HEADER}
	${MEMORY_METRICS_HEADER}
	${STILL_SRC}
	${STILL_CODEGEN_SRC}
	timbr/timbr.c)
target_include_directories(still_debug PRIVATE ${CMAKE_BINARY_DIR}/generated include timbr src/codegen)
target_compile_options(still_debug PRIVATE ${STILL_CFLAGS} ${LLVM_CFLAGS} -g -O0)
target_compile_definitions(still_debug PRIVATE STILL_DEBUG=1 STILL_VERSION="${STILL_VERSION}")
target_link_libraries(still_debug PRIVATE ${LLVM_LIBS})
set_target_properties(still_debug PROPERTIES BUILD_RPATH "${LLVM_LIBDIR}")

if(BUILD_TESTING)

# Same implementation as embedded bitcode, with a two-bit generation counter
# to make identity exhaustion executable in CI. ASan/UBSan checks the checker.
add_executable(memory_runtime_test tests/runtime/memory.c src/runtime/wky_memory.c)
target_include_directories(memory_runtime_test PRIVATE src/runtime)
target_compile_definitions(memory_runtime_test PRIVATE WKY_GENERATION_MAX=3 WKY_MEMORY_METRICS)
target_compile_options(memory_runtime_test PRIVATE -UNDEBUG -O2 -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(memory_runtime_test PRIVATE -fsanitize=address,undefined)
find_package(Threads REQUIRED)
target_link_libraries(memory_runtime_test PRIVATE Threads::Threads)
add_test(NAME memory-runtime COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_memory_runtime.py $<TARGET_FILE:memory_runtime_test>)
add_executable(print_runtime_test tests/runtime/printing.c src/runtime/wky_runtime.c)
target_compile_options(print_runtime_test PRIVATE -UNDEBUG -O2 -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(print_runtime_test PRIVATE -fsanitize=address,undefined)
add_test(NAME print-runtime COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_print_runtime.py $<TARGET_FILE:print_runtime_test>)
add_test(NAME check-mode COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_check_mode.py $<TARGET_FILE:still>)
add_test(NAME tooling COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_tooling.py $<TARGET_FILE:still>)
add_test(NAME branding COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_branding.py $<TARGET_FILE:still>)
add_test(NAME optimization-report COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/scripts/test_optimization_report.py $<TARGET_FILE:still>)

# Layer 1: smoke test -- the compiler builds and compiles its own test suite
# entry point without crashing.
add_test(NAME smoke
	COMMAND ${CMAKE_SOURCE_DIR}/scripts/smoke.sh $<TARGET_FILE:still>)

# Both this runner and the optional lit adapter use scripts/test_support.py.
# No dependency on a particular vendored lit/Python installation.
add_test(NAME unit
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test.py
		--still $<TARGET_FILE:still> --strict
		--json ${CMAKE_BINARY_DIR}/unit-results.json)
set(STILL_UNIT_TEST_TIMEOUT 600 CACHE STRING "Regression suite timeout in seconds; increase for CPU emulation")
set_tests_properties(unit PROPERTIES TIMEOUT ${STILL_UNIT_TEST_TIMEOUT})

add_test(NAME harness
	COMMAND ${Python3_EXECUTABLE} -m unittest discover
		-s ${CMAKE_SOURCE_DIR}/scripts/harness_tests -v)
endif()

# Benchmarks build every language from source and verify output before timing.
add_custom_target(bench
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench.py
		--still $<TARGET_FILE:still>
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	USES_TERMINAL
	DEPENDS still)

add_custom_target(bench-verify
	COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench.py
		--still $<TARGET_FILE:still> --quick --verify-only --sanitize-c
		--json ${CMAKE_BINARY_DIR}/benchmark-verification.json
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	USES_TERMINAL
	DEPENDS still)

add_custom_target(bench-memory
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_memory.py
        --still $<TARGET_FILE:still> --json ${CMAKE_BINARY_DIR}/memory-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS still)

add_custom_target(bench-owners
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_owners.py
        --still $<TARGET_FILE:still> --json ${CMAKE_BINARY_DIR}/owners-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS still)

add_custom_target(bench-owned-values
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_owners.py
        --still $<TARGET_FILE:still> --workload owned_values --trials 64
        --json ${CMAKE_BINARY_DIR}/owned-values-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS still)

add_custom_target(bench-subobjects
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_memory.py
        --still $<TARGET_FILE:still> --workload subobject_walk
        --rounds 10000000 --json ${CMAKE_BINARY_DIR}/subobject-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS still)

add_custom_target(bench-tools
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_tools.py
        --still $<TARGET_FILE:still> --json ${CMAKE_BINARY_DIR}/compiler-tools-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS still)

add_custom_target(bench-semantics
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_semantics.py
        --still $<TARGET_FILE:still> --json ${CMAKE_BINARY_DIR}/semantics-benchmark.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS still)

add_custom_target(bench-semantics-verify
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/bench_semantics.py
        --still $<TARGET_FILE:still> --rounds 2048 --result-rounds 4096 --verify-only
        --json ${CMAKE_BINARY_DIR}/semantics-verification.json
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL DEPENDS still)
