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
# Pure logic, no main(). The gardner binary and every tool links this.
add_library(gardener_core STATIC
    src/platform/platform_posix.c
    src/platform/log.c
)
target_include_directories(gardener_core PUBLIC include)

# ---- gardner executable ---------------------------------------------
add_executable(gardener src/main.c)
target_link_libraries(gardener PRIVATE gardener_core)

# ---- tests ----------------------------------------------------------
enable_testing()

add_executable(test_platform tests/test_platform.c)
target_link_libraries(test_platform PRIVATE gardener_core)
add_test(NAME platform COMMAND test_platform)