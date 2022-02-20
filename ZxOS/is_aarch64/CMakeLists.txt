# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)


find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(hello_world)

#zephyr_code_relocate(src/*.c SRAM)

file(GLOB SRC_FILES
    "src/*.c"
)

target_sources( app PRIVATE ${SRC_FILES})
