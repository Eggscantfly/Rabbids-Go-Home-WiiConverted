cmake_minimum_required(VERSION 3.20)

project(RGHLzoNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RGH_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_subdirectory("Pybind11")

add_library(minilzo STATIC "LZO/minilzo/minilzo.c")
target_include_directories(minilzo PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/LZO/minilzo"
    "${CMAKE_CURRENT_LIST_DIR}/LZO/include"
    "${CMAKE_CURRENT_LIST_DIR}/LZO/include/lzo"
)

pybind11_add_module(_lzo_native MODULE "src/native_lzo.cpp")
target_link_libraries(_lzo_native PRIVATE minilzo)
set_target_properties(_lzo_native PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${RGH_ROOT}/rghport/archive"
    RUNTIME_OUTPUT_DIRECTORY "${RGH_ROOT}/rghport/archive"
)

if(MSVC)
    target_compile_options(_lzo_native PRIVATE /W4 /permissive-)
endif()
