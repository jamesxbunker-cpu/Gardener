cmake_minimum_required(VERSION 3.16)
project(gardener C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

# ---- core library ---------------------------------------------------
add_library(gardener_core STATIC
    src/platform/platform_posix.c
    src/ring.c
    src/registry.c
    src/ingest.c
    src/packet.c
    src/log_writer.c
    src/log_reader.c
    src/replay.c
    src/baseline.c                      # C1
)
target_include_directories(gardener_core PUBLIC include)

# ---- gardener executable ---------------------------------------------
add_executable(gardener src/main.c)
target_link_libraries(gardener PRIVATE gardener_core)
target_link_libraries(gardener PRIVATE m)       # strtod, isfinite

# ---- tools ----------------------------------------------------------
add_executable(gardener-dump tools/gardener_dump.c)
target_link_libraries(gardener-dump PRIVATE gardener_core)

add_executable(gardener-log tools/gardener_log.c)
target_link_libraries(gardener-log PRIVATE gardener_core)

# ---- tests ----------------------------------------------------------
enable_testing()

add_executable(test_platform tests/test_platform.c)
target_link_libraries(test_platform PRIVATE gardener_core)
add_test(NAME platform COMMAND test_platform)

add_executable(test_ring tests/test_ring.c)
target_link_libraries(test_ring PRIVATE gardener_core)
add_test(NAME ring COMMAND test_ring)

add_executable(test_registry tests/test_registry.c)
target_link_libraries(test_registry PRIVATE gardener_core)
add_test(NAME registry COMMAND test_registry)

add_executable(test_ingest tests/test_ingest.c)
target_link_libraries(test_ingest PRIVATE gardener_core)
add_test(NAME ingest COMMAND test_ingest)

add_executable(test_packet_json tests/test_packet_json.c)   # A4
target_link_libraries(test_packet_json PRIVATE gardener_core m)
add_test(NAME packet_json COMMAND test_packet_json)

add_executable(test_packet_bin tests/test_packet_bin.c)
target_link_libraries(test_packet_bin PRIVATE gardener_core m)
add_test(NAME packet_bin COMMAND test_packet_bin)

add_executable(test_log_format tests/test_log_format.c)
target_link_libraries(test_log_format PRIVATE gardener_core)
add_test(NAME log_format COMMAND test_log_format)

add_executable(test_log_writer tests/test_log_writer.c)     # B1
target_link_libraries(test_log_writer PRIVATE gardener_core)
add_test(NAME log_writer COMMAND test_log_writer)

add_executable(test_log_reader tests/test_log_reader.c)
target_link_libraries(test_log_reader PRIVATE gardener_core)
add_test(NAME log_reader COMMAND test_log_reader)

add_executable(test_replay tests/test_replay.c)     # B4
target_link_libraries(test_replay PRIVATE gardener_core)
add_test(NAME replay COMMAND test_replay)

add_executable(test_main_args tests/test_main_args.c)     # B5
target_link_libraries(test_main_args PRIVATE gardener_core)
add_test(NAME main_args COMMAND test_main_args)

add_executable(test_baseline_format tests/test_baseline_format.c)
target_link_libraries(test_baseline_format PRIVATE gardener_core)
add_test(NAME baseline_format COMMAND test_baseline_format)

add_executable(test_baseline tests/test_baseline.c)     # C1
target_link_libraries(test_baseline PRIVATE gardener_core m)
add_test(NAME baseline COMMAND test_baseline)

add_executable(test_baseline_regimes tests/test_baseline_regimes.c)     # C2
target_link_libraries(test_baseline_regimes PRIVATE gardener_core m)
add_test(NAME baseline_regimes COMMAND test_baseline_regimes)
