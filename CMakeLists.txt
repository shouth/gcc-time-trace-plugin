cmake_minimum_required(VERSION 3.0)
project(gcc-time-trace-plugin)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if (NOT TIME_TRACE_PLUGIN_TARGET_GCC)
  set(TIME_TRACE_PLUGIN_TARGET_GCC ${CMAKE_CXX_COMPILER})
endif()

execute_process(
  COMMAND ${TIME_TRACE_PLUGIN_TARGET_GCC} -print-file-name=plugin
  OUTPUT_VARIABLE GCC_PLUGIN_SOURCE_DIR
  RESULT_VARIABLE GCC_PLUGIN_SOURCE_DIR_FOUND
  OUTPUT_STRIP_TRAILING_WHITESPACE)

if(${GCC_PLUGIN_SOURCE_DIR_FOUND} EQUAL 0 AND EXISTS ${GCC_PLUGIN_SOURCE_DIR}/include)
  message(STATUS "GCC_PLUGIN_SOURCE_DIR = ${GCC_PLUGIN_SOURCE_DIR}")
else()
  message(FATAL_ERROR "Could not find GCC plugin development files")
endif()

file(GLOB src src/*.cpp)

add_library(${PROJECT_NAME} SHARED ${src})

target_compile_options(${PROJECT_NAME}
  PRIVATE -fno-rtti -fno-exceptions)

set_target_properties(${PROJECT_NAME}
  PROPERTIES
    CXX_EXTENSIONS ON
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    PREFIX ""
    OUTPUT_NAME "timetrace")

target_include_directories(${PROJECT_NAME}
  PRIVATE ${GCC_PLUGIN_SOURCE_DIR}/include)
