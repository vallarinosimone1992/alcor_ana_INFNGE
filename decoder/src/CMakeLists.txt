### @author: Roberto Preghenella
### @email: preghenella@bo.infn.it

cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(alcor-utils)

include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++17" COMPILER_SUPPORTS_CXX17)
CHECK_CXX_COMPILER_FLAG("-std=c++0x" COMPILER_SUPPORTS_CXX0X)
if(COMPILER_SUPPORTS_CXX17)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
elseif(COMPILER_SUPPORTS_CXX0X)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
else()
  message(STATUS "The compiler ${CMAKE_CXX_COMPILER} has no C++17 support. Please use a different C++ compiler.")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

include(FindPackageHandleStandardArgs)
option(ALCOR_BUILD_READOUT "Build readout tools (requires uHAL)" OFF)
find_package(Threads REQUIRED)
find_package(Boost COMPONENTS program_options REQUIRED)
if(ALCOR_BUILD_READOUT)
  find_package(uHAL REQUIRED)
endif()
find_package(ROOT COMPONENTS Core RIO Tree REQUIRED)
execute_process(
  COMMAND ${ROOT_CONFIG_EXECUTABLE} --cflags
  OUTPUT_VARIABLE ROOT_CXX_FLAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(ROOT_CXX_FLAGS_LIST UNIX_COMMAND "${ROOT_CXX_FLAGS}")


set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(src)
