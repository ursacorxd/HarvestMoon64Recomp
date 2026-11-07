cmake_minimum_required(VERSION 3.20)
project(spirv_cross_dksh CXX)
set(CMAKE_CXX_STANDARD 17)

# Ruta al SPIRV-Cross fijado por RT64 (lib/rt64/src/contrib/spirv-cross)
set(SPIRV_CROSS_DIR "" CACHE PATH "Codigo fuente de SPIRV-Cross")
if (NOT SPIRV_CROSS_DIR)
    message(FATAL_ERROR "Define -DSPIRV_CROSS_DIR=<ruta a spirv-cross>")
endif()

set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(${SPIRV_CROSS_DIR} ${CMAKE_BINARY_DIR}/spirv-cross)

add_executable(spirv_cross_dksh spirv_cross_dksh.cpp)
target_include_directories(spirv_cross_dksh PRIVATE ${SPIRV_CROSS_DIR})
target_link_libraries(spirv_cross_dksh spirv-cross-core spirv-cross-glsl)
