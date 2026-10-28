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
    src/baseline.c
    src/event.c
    src/detect.c                     # D1
    src/policy.c    
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

add_executable(gardener-stats tools/gardener_stats.c)
target_link_libraries(gardener-stats PRIVATE gardener_core)

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

add_executable(test_baseline_decay tests/test_baseline_decay.c)     # C3
target_link_libraries(test_baseline_decay PRIVATE gardener_core m)
add_test(NAME baseline_decay COMMAND test_baseline_decay)

add_executable(test_baseline_persist tests/test_baseline_persist.c)     # C4
target_link_libraries(test_baseline_persist PRIVATE gardener_core m)
add_test(NAME baseline_persist COMMAND test_baseline_persist)

add_executable(test_main_baseline tests/test_main_baseline.c)     # C5
target_link_libraries(test_main_baseline PRIVATE gardener_core m)
add_test(NAME main_baseline COMMAND test_main_baseline)

add_executable(test_event_format tests/test_event_format.c)     # D0
target_link_libraries(test_event_format PRIVATE gardener_core)
add_test(NAME event_format COMMAND test_event_format)

add_executable(test_event_json tests/test_event_json.c)
target_link_libraries(test_event_json PRIVATE gardener_core)
add_test(NAME event_json COMMAND test_event_json)

add_executable(test_detect_range tests/test_detect_range.c)     # D1
target_link_libraries(test_detect_range PRIVATE gardener_core m)
add_test(NAME detect_range COMMAND test_detect_range)

add_executable(test_detect_zscore tests/test_detect_zscore.c)   # D1
target_link_libraries(test_detect_zscore PRIVATE gardener_core m)
add_test(NAME detect_zscore COMMAND test_detect_zscore)

add_executable(test_detect_flatline tests/test_detect_flatline.c)   # D2
target_link_libraries(test_detect_flatline PRIVATE gardener_core m)
add_test(NAME detect_flatline COMMAND test_detect_flatline)

add_executable(test_detect_roc tests/test_detect_roc.c)             # D2
target_link_libraries(test_detect_roc PRIVATE gardener_core m)
add_test(NAME detect_roc COMMAND test_detect_roc)

add_executable(test_detect_correlation tests/test_detect_correlation.c)   # D3
target_link_libraries(test_detect_correlation PRIVATE gardener_core m)
add_test(NAME detect_correlation COMMAND test_detect_correlation)

add_executable(test_detect_mismatch tests/test_detect_mismatch.c)   # D4
target_link_libraries(test_detect_mismatch PRIVATE gardener_core m)
add_test(NAME detect_mismatch COMMAND test_detect_mismatch)

add_executable(test_main_detect tests/test_main_detect.c)   # D5
target_link_libraries(test_main_detect PRIVATE gardener_core)
add_test(NAME main_detect COMMAND test_main_detect)

add_executable(test_policy_format tests/test_policy_format.c)
target_link_libraries(test_policy_format PRIVATE gardener_core)
add_test(NAME policy_format COMMAND test_policy_format)
