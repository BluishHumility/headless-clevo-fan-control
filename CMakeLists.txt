cmake_minimum_required(VERSION 3.17)

project(ClevoFanControl C)

set(CMAKE_C_STANDARD 99)

add_executable(clevo-fan-control src/main.c)

find_package(PkgConfig REQUIRED)

# Link only math library (still needed for round(), etc.)
target_link_libraries(clevo-fan-control m)

install(
        TARGETS clevo-fan-control
        DESTINATION bin
)
