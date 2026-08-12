cmake_minimum_required(VERSION 3.20.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app)

target_sources(app PRIVATE src/main.c)

add_compile_options(-Wall -Wextra -Werror=return-type -Werror=implicit-function-declaration -fdiagnostics-color=always)
