cmake_minimum_required(VERSION 3.20)
project(ShortcutRouter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Arrow REQUIRED CONFIG)
find_package(Parquet REQUIRED CONFIG)

find_path(H3_INCLUDE_DIR h3api.h PATH_SUFFIXES h3)
find_library(H3_LIBRARY h3)

if (NOT H3_INCLUDE_DIR OR NOT H3_LIBRARY)
    message(FATAL_ERROR "H3 headers or library not found. Install libh3 and set H3_INCLUDE_DIR/H3_LIBRARY if necessary.")
endif()

add_executable(shortcut_router
    src/main.cpp
    src/shortcut_graph.cpp
    src/h3_utils.cpp
)

target_include_directories(shortcut_router PRIVATE include "${H3_INCLUDE_DIR}")

set(ARROW_TARGET Arrow::arrow_shared)
if (NOT TARGET ${ARROW_TARGET})
    set(ARROW_TARGET Arrow::arrow)
endif()

set(PARQUET_TARGET Parquet::parquet_shared)
if (NOT TARGET ${PARQUET_TARGET})
    set(PARQUET_TARGET Parquet::parquet)
endif()

target_link_libraries(shortcut_router
    PRIVATE
        ${ARROW_TARGET}
        ${PARQUET_TARGET}
        ${H3_LIBRARY}
)