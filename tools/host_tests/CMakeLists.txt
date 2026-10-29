# Host-side unit tests for the parts of the firmware that have nothing to do
# with the ESP32: the robot ID format and the key derivation. They run on a
# laptop with no ESP-IDF, no board and no network, which is what lets CI catch
# an algorithm change before it reaches a robot.
#
#   cmake -S tools/host_tests -B build/host_tests
#   cmake --build build/host_tests
#   ctest --test-dir build/host_tests --output-on-failure
#
# or just: idf.py host-test   (see idf_ext.py)

cmake_minimum_required(VERSION 3.16)
project(mpx_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

enable_testing()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(MPX_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
get_filename_component(MPX_REPO_ROOT "${MPX_REPO_ROOT}" ABSOLUTE)

set(MPX_ID_TOOL "${MPX_REPO_ROOT}/tools/mpx_id.py")
set(MPX_VECTORS_JSON "${MPX_REPO_ROOT}/tools/vectors/identity.json")
set(MPX_SEAL_TOOL "${MPX_REPO_ROOT}/tools/mpx_seal.py")
set(MPX_SEAL_JSON "${MPX_REPO_ROOT}/tools/vectors/seal.json")
set(MPX_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MPX_VECTORS_HEADER "${MPX_GENERATED_DIR}/mpx_vectors.h")
set(MPX_SEAL_HEADER "${MPX_GENERATED_DIR}/mpx_seal_vectors.h")

# ── Golden vectors, as a C header ───────────────────────────────────────────
#
# tools/mpx_id.py is the reference implementation; it regenerates the JSON and
# the header from the same code, so the C tests can never drift from the Python
# one. The header only carries data, so there is no second implementation to
# keep in sync until mpx_id.c exists.
add_custom_command(
    OUTPUT "${MPX_VECTORS_HEADER}" "${MPX_SEAL_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MPX_GENERATED_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${MPX_ID_TOOL}" c-header "${MPX_VECTORS_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${MPX_SEAL_TOOL}" c-header "${MPX_SEAL_HEADER}"
    DEPENDS "${MPX_ID_TOOL}" "${MPX_VECTORS_JSON}" "${MPX_SEAL_TOOL}" "${MPX_SEAL_JSON}"
    COMMENT "Generating mpx_vectors.h from tools/vectors/*.json"
    VERBATIM
)

# ── Sources under test ──────────────────────────────────────────────────────
#
# ESP-free only. The rule for components/mpx_identity: if a file includes an
# esp_*.h header it does not belong here, and if the algorithm lives in such a
# file it cannot be tested without a board. mpx_id.c and mpx_derive.c are kept
# free of ESP headers for exactly this reason; mpx_identity.cc adapts them.
set(MPX_IDENTITY_SOURCES
    ${MPX_REPO_ROOT}/components/mpx_identity/mpx_id.c
    ${MPX_REPO_ROOT}/components/mpx_identity/mpx_derive.c
    ${MPX_REPO_ROOT}/components/mpx_identity/mpx_seal.c
    ${MPX_REPO_ROOT}/components/mpx_identity/mpx_efuse_record.c
    # The provisioning line parser and the §6.6 decision table: what a bench
    # operator triggers with a cable, so the only place it can be tested is here.
    ${MPX_REPO_ROOT}/components/mpx_identity/mpx_prov_json.c
    ${MPX_REPO_ROOT}/components/mpx_identity/mpx_prov_sm.c
)

# ── Optional real HMAC ─────────────────────────────────────
#
# The firmware hashes with the ESP32 HMAC peripheral, and the host has no such
# thing. The tests inject a callback instead (see mpx_derive.h), so the key
# derivation can be checked without mbedTLS. OpenSSL is only needed to prove
# that the injected callback and the vectors agree on a real HMAC-SHA256 -- and
# to check the derived key against the frozen vector. It is optional because a
# laptop without libssl headers should still be able to run the ID tests.
find_package(OpenSSL COMPONENTS Crypto QUIET)

file(GLOB MPX_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c")

add_executable(mpx_host_tests
    main.c
    test_harness.c
    ${MPX_TEST_SOURCES}
    ${MPX_IDENTITY_SOURCES}
    "${MPX_VECTORS_HEADER}"
    "${MPX_SEAL_HEADER}"
)

target_include_directories(mpx_host_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MPX_GENERATED_DIR}"
    "${MPX_REPO_ROOT}/components/mpx_identity"
    "${MPX_REPO_ROOT}/components/mpx_identity/include"
)

if(OpenSSL_FOUND)
    target_compile_definitions(mpx_host_tests PRIVATE MPX_HAVE_OPENSSL=1)
    target_link_libraries(mpx_host_tests PRIVATE OpenSSL::Crypto)
    message(STATUS "OpenSSL found: full key-derivation vectors enabled")
else()
    message(STATUS "OpenSSL not found: key-derivation vectors skipped")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mpx_host_tests PRIVATE -Wall -Wextra -Wshadow -Wconversion)
endif()

add_test(NAME unit COMMAND mpx_host_tests)

# ── Reference implementation checks ────────────────────────────────────────
#
# Three separate tests so a failure says which one broke: the algorithm
# properties (§11 test 1), the vectors file being stale, and the JSON agreeing
# with the Python code.
add_test(NAME python_selftest
    COMMAND "${Python3_EXECUTABLE}" "${MPX_ID_TOOL}" selftest)

add_test(NAME vectors_up_to_date
    COMMAND "${Python3_EXECUTABLE}" "${MPX_ID_TOOL}" vectors --check)

add_test(NAME seal_selftest
    COMMAND "${Python3_EXECUTABLE}" "${MPX_SEAL_TOOL}" selftest)

add_test(NAME seal_vectors_up_to_date
    COMMAND "${Python3_EXECUTABLE}" "${MPX_SEAL_TOOL}" vectors --check)

add_test(NAME id_cli_rejects_bad_ids
    COMMAND "${Python3_EXECUTABLE}" "${MPX_ID_TOOL}" check
            MPX-AJZ7J8Y0YTDS MPX-DOG-01 MPX-A1Z7J8Y0YTDS)
set_tests_properties(id_cli_rejects_bad_ids PROPERTIES WILL_FAIL TRUE)
