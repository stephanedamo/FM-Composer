cmake_minimum_required(VERSION 3.5)
# TODO: Determine actual required CMake version

# Set default build type to Release
if(NOT DEFINED CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build (Debug or Release)")
endif()

project(FMComposer)

# Detect and enable C++11 support
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
CHECK_CXX_COMPILER_FLAG("-std=c++0x" COMPILER_SUPPORTS_CXX0X)
if(COMPILER_SUPPORTS_CXX11)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
elseif(COMPILER_SUPPORTS_CXX0X)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
else()
	message(STATUS "FMComposer requires C++11, but ${CMAKE_CXX_COMPILER} does not appear to support C++11. Please use a different compiler or update ${CMAKE_CXX_COMPILER}.")
endif()

include_directories(src)

# Add source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.c")

# Set executable output path
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Set executable name
set(EXECUTABLE_NAME "FMComposer")
add_executable(${EXECUTABLE_NAME} ${SOURCES})

# Add CMake modules
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

# Detect and add SFML
find_package(SFML 2 REQUIRED graphics window system)
if(SFML_FOUND)
	include_directories(${SFML_INCLUDE_DIR})
	target_link_libraries(${EXECUTABLE_NAME} ${SFML_LIBRARIES} ${SFML_DEPENDENCIES})
endif()

# Detect and add OpenGL
find_package(OpenGL REQUIRED)
if(OPENGL_FOUND)
	include_directories(${OPENGL_INCLUDE_DIRS})
	target_link_libraries(${EXECUTABLE_NAME} ${OPENGL_LIBRARIES})
endif()

# Detect and add FLAC
find_package(FLAC REQUIRED)
if(FLAC_FOUND)
	include_directories(${FLAC_INCLUDE_DIR})
	target_link_libraries(${EXECUTABLE_NAME} ${FLAC_LIBRARY})
endif()

# Detect and add PortAudio
find_package(PortAudio REQUIRED)
if(PORTAUDIO_FOUND)
	include_directories(${PORTAUDIO_INCLUDE_DIRS})
	target_link_libraries(${EXECUTABLE_NAME} ${PORTAUDIO_LIBRARIES})
endif()

# Detect and add Lame
find_library(LAME_LIBRARY "mp3lame")
find_path(LAME_INCLUDE_DIR "lame.h" PATH_SUFFIXES "lame")
if(NOT LAME_LIBRARY)
	message(STATUS "Cannot find mp3lame library")
elseif(NOT LAME_INCLUDE_DIR)
	message(STATUS "Cannot find mp3lame includes")
else()
	message(STATUS "Found mp3lame includes: ${LAME_INCLUDE_DIR}")
	message(STATUS "Found mp3lame library: ${LAME_LIBRARY}")
	include_directories(${LAME_INCLUDE_DIR})
	target_link_libraries(${EXECUTABLE_NAME} ${LAME_LIBRARY})
endif()

# Link POSIX Thread library if necessary
if(CMAKE_COMPILER_IS_GNUCC)
	target_link_libraries(${EXECUTABLE_NAME} pthread)
endif(CMAKE_COMPILER_IS_GNUCC)
