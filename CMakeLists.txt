cmake_minimum_required(VERSION 3.22)
project(teamXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto")

add_executable(main
    main.cpp
    src/parser.cpp
    src/machine_state.cpp
    src/scheduler.cpp
    src/strategy.cpp
    src/constraint_checker.cpp
    src/scorer.cpp
    src/optimizer.cpp
    src/output.cpp
)
