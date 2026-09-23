cmake_minimum_required(VERSION 3.13)            # CMake version check
project(uatom VERSION 0.0.1 LANGUAGES CXX)      # Create project

# Header-only library target
add_library(uatom INTERFACE)
target_include_directories(uatom INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(uatom INTERFACE cxx_std_17)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})

# only build the tests when uatom is the top level project
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()
    add_subdirectory(tests)
endif()
