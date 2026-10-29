cmake_minimum_required(VERSION 3.13)
project(uds_server C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(uds_server
    src/can_if.c
    src/isotp.c
    src/main.c
    src/timeutil.c
    src/uds.c
)
target_include_directories(uds_server PRIVATE include)
target_compile_options(uds_server PRIVATE -Wall -Wextra -Wpedantic)
