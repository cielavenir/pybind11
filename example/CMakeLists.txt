cmake_minimum_required(VERSION 2.8)

# project
project(pybind11_examples)
set(CMAKE_CXX_STANDARD 11)

# required packages
find_package(pybind11 REQUIRED)  # or add_subdirectory(pybind11)
message(STATUS "pybind11_INCLUDE_DIRS=${pybind11_INCLUDE_DIRS}")

# macros
if(APPLE)
	set(CMAKE_MACOSX_RPATH 1)
	set(MUJIN_INSTALL_DIR $ENV{HOME}/mujin)
else()
	set(MUJIN_INSTALL_DIR $ENV{MUJIN_INSTALL_DIR})
endif()
set(MUJIN_INSTALL_INCLUDE_DIR ${MUJIN_INSTALL_DIR}/include)
#set(MUJIN_INSTALL_LIBRARY_DIR $ENV{MUJIN_INSTALL_DIR}/lib)

# dependency
set(example_files example.cpp)

# include
include_directories(${MUJIN_INSTALL_INCLUDE_DIR})
message(STATUS "MUJIN_INSTALL_INCLUDE_DIR=${MUJIN_INSTALL_INCLUDE_DIR}")
set(USE_PYTHON3 no CACHE BOOL "switch between python and python3")
if(NOT APPLE)
	if(USE_PYTHON3)
	  include_directories(${MUJIN_INSTALL_INCLUDE_DIR}/python3.6m)
	else()
	  include_directories(/usr/include/python2.7)
	endif()
else()
	set(USE_PYTHON3 yes)
	set(PYTHON_DIR "/usr/local/Cellar/python/3.7.3/Frameworks/Python.framework/Versions/3.7")
	include_directories("${PYTHON_DIR}/include/python3.7m")
	link_directories("${PYTHON_DIR}/lib")
	# 
endif()
link_directories(${BOOST_LIBRARYDIR})	

# archive
add_library(example SHARED ${example_files})
target_link_libraries(example python3.7)

# prefix of library (no "lib")
# python3-config --extension-suffix
if(USE_PYTHON3)
  set_target_properties(example PROPERTIES PREFIX "" SUFFIX ".cpython-37m-darwin.so")
else()
  set_target_properties(example PROPERTIES PREFIX "")
endif()

