#!/bin/bash
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O3 -march=native -flto \
    main.cpp \
    src/parser.cpp \
    src/machine_state.cpp \
    src/scheduler.cpp \
    src/strategy.cpp \
    src/constraint_checker.cpp \
    src/scorer.cpp \
    src/optimizer.cpp \
    src/timeline.cpp \
    src/output.cpp \
    -o main
