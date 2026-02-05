cmake_minimum_required(VERSION 3.20)

project(recomp_mod_native_lib)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (DEFINED ENV{LIB_NAME} AND NOT "$ENV{LIB_NAME}" STREQUAL "")
    set(LIB_NAME "$ENV{LIB_NAME}" CACHE STRING "Extlib name" FORCE)
endif()
if (NOT DEFINED LIB_NAME OR LIB_NAME STREQUAL "")
    set(LIB_NAME "mm_recomp_http_bridge_extlib" CACHE STRING "Extlib name")
endif()

set(TARGET_NAME "${LIB_NAME}")

include_directories("./offline_build")
include_directories("./include/extlib")
include_directories("./include/shared")
include_directories("./src/extlib")

add_library(${TARGET_NAME} SHARED)

add_subdirectory("./src/extlib")

if (WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE ws2_32)
endif()

set_target_properties(${TARGET_NAME}
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "./arc/"
    LIBRARY_OUTPUT_DIRECTORY "./lib/"
    RUNTIME_OUTPUT_DIRECTORY "./lib/"
)
