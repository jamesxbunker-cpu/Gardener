# Gardener

A system supervisor: learns what "normal" looks like for a
multi-channel system, flags anomalies, and (later) can act on them.

## Status

Phase A0 — skeleton only. Nothing monitors anything yet.

## Build

    cmake -B build
    cmake --build build

## Run

    ./build/gardener --version
    ./build/gardener --verbose

## Test

    cd build && ctest --output-on-failure

## Layout

    include/gardener/   public headers
    src/                implementation
    src/platform/       the only place OS APIs are called
    tests/              unit tests