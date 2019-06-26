cmake_minimum_required(VERSION 2.8)

# project
project(pybind11_examples)

# required packages
find_package(pybind11 REQUIRED)  # or add_subdirectory(pybind11)
message(STATUS "pybind11_INCLUDE_DIRS=${pybind11_INCLUDE_DIRS}")

# macros
set(MUJIN_INSTALL_INCLUDE_DIR $ENV{MUJIN_INSTALL_DIR}/include)
#set(MUJIN_INSTALL_LIBRARY_DIR $ENV{MUJIN_INSTALL_DIR}/lib)

# dependency
set(example_files example.cpp)

# include
include_directories(${MUJIN_INSTALL_INCLUDE_DIR})
set(USE_PYTHON3 no CACHE BOOL "switch between python and python3")
if(USE_PYTHON3)
  include_directories(${MUJIN_INSTALL_INCLUDE_DIR}/python3.6m)
else()
  include_directories(/usr/include/python2.7)
endif()

# archive
add_library(example SHARED ${example_files})

# prefix of library (no "lib")
set_target_properties(example PROPERTIES PREFIX "")

