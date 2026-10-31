cmake_minimum_required(VERSION 3.20)
project(RenderHeight VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FH2MOD_PLATFORM_DIR "" CACHE PATH "Path to the FH2 Mod Platform source tree")
if(NOT FH2MOD_PLATFORM_DIR OR NOT EXISTS "${FH2MOD_PLATFORM_DIR}/include/fh2mod.h")
    message(FATAL_ERROR "Set FH2MOD_PLATFORM_DIR to the FH2 Mod Platform source directory")
endif()

add_subdirectory("${FH2MOD_PLATFORM_DIR}" "${CMAKE_BINARY_DIR}/fh2-mod-platform" EXCLUDE_FROM_ALL)

add_library(RenderHeight SHARED src/render_height.cpp)
target_link_libraries(RenderHeight PRIVATE fh2mod_api)
target_compile_definitions(RenderHeight PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(RenderHeight PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fms-extensions)
target_link_options(RenderHeight PRIVATE -static)
set_target_properties(RenderHeight PROPERTIES PREFIX "" OUTPUT_NAME "RenderHeight")
