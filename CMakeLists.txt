CMAKE_MINIMUM_REQUIRED(VERSION 2.8)
#if(POLICY CMP0011)
#  cmake_policy(SET CMP0011 NEW)
#endif(POLICY CMP0011)
#if(POLICY CMP0017)
#  cmake_policy(SET CMP0017 OLD)
#endif()

IF(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  MESSAGE(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there. (you may need to remove CMakeCache.txt) ")
endif()

PROJECT(OpenSlide)

# configure the module path of modified modules
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

##### STDBOOL.H ISSUES SHOULD BE RESOLVED ######
# add modules for config.h setup
#include(CheckIncludeFiles)

#check_include_files("stdbool.h" HAVE_STDBOOL_H)
################################################

if(CMAKE_COMPILER_IS_GNUCXX) # also true when the code is compiled as c-code
  add_definitions(-DG_DISABLE_DEPRECATED -DG_DISABLE_SINGLE_INCLUDES)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wnested-externs -Wc++-compat -fvisibility=hidden -fno-common")
endif()

# define the library version
set(OPENSLIDE_WORLD_VERSION "3")
set(OPENSLIDE_MAJOR_VERSION "3")
set(OPENSLIDE_MINOR_VERSION "3")
set(OPENSLIDE_VERSION_NUMBER ${OPENSLIDE_WORLD_VERSION}.${OPENSLIDE_MAJOR_VERSION}.${OPENSLIDE_MINOR_VERSION})

##### SHOULD NO LONGER BE NEEDED #####
## create a config.h header-file
#configure_file(${PROJECT_SOURCE_DIR}/config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h)
#add_definitions(-DHAVE_CONFIG_H)

include_directories(${CMAKE_CURRENT_BINARY_DIR} src)

message("adding src directory")
add_subdirectory(src)
add_subdirectory(test)
add_subdirectory(tools)
