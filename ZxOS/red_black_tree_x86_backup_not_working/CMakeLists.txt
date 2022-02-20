# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(LIB_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/lib)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_world)

include_directories(src)

file(GLOB SRC_FILES
    "src/*.c"
)

add_library(other3party_lib STATIC IMPORTED GLOBAL)
set_target_properties(other3party_lib PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/rbtree.a)

target_link_libraries(app PUBLIC other3party_lib)
target_sources( app PRIVATE ${SRC_FILES})
