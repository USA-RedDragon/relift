cmake_minimum_required(VERSION 3.24)

project(nba_oracle LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED NBA_SOURCE)
  message(FATAL_ERROR "set NBA_SOURCE to the NanoBoyAdvance checkout")
endif()

find_package(fmt 12.1.0 QUIET)
if(NOT fmt_FOUND)
  add_subdirectory(${NBA_SOURCE}/thirdparty/fmt-12.1.0 fmt)
endif()
add_subdirectory(${NBA_SOURCE}/thirdparty/atom-1.0.1 atom)
add_subdirectory(${NBA_SOURCE}/src/nba nba)

target_compile_options(nba PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-include cstring>)

add_executable(nba-oracle driver.cc)
target_include_directories(nba-oracle PRIVATE ${NBA_SOURCE}/src/nba/src)
target_link_libraries(nba-oracle PRIVATE nba)
