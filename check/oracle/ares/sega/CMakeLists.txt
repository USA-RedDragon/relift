cmake_minimum_required(VERSION 3.20)
project(sega_oracle C CXX)
if(NOT ARES_SOURCE)
  message(FATAL_ERROR "set ARES_SOURCE to the patched ares tree")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(A ${ARES_SOURCE})
set(C ${A}/ares/component)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)

add_library(libco STATIC ${A}/libco/libco.c ${A}/libco/amd64.c)
add_library(sljit STATIC ${A}/thirdparty/sljit/sljit_src/sljitLir.c)
target_include_directories(sljit PUBLIC ${A}/thirdparty)
target_compile_definitions(sljit PUBLIC SLJIT_HAVE_CONFIG_PRE=1 SLJIT_HAVE_CONFIG_POST=1)

set(COMMON_FLAGS -fno-strict-aliasing -fwrapv -fno-char8_t -w)
set(COMMON_DEFS PROFILE_PERFORMANCE BUILD_RELEASE BUILD_LOCAL CORE_MS CORE_SG)

add_library(nallobj OBJECT ${A}/nall/nall/nall.cpp ${A}/nall/nall/sljitAllocator.cpp)
target_include_directories(nallobj PRIVATE ${A} ${A}/nall ${A}/thirdparty)
target_compile_definitions(nallobj PRIVATE ${COMMON_DEFS} SLJIT_HAVE_CONFIG_PRE=1 SLJIT_HAVE_CONFIG_POST=1)
target_compile_options(nallobj PRIVATE ${COMMON_FLAGS})

add_library(aressega STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../ares.gen.cpp ${A}/ares/ares/memory/fixed-allocator.cpp
  ${A}/thirdparty/qon/qon.cpp
  ${C}/processor/z80/z80.cpp
  ${C}/audio/sn76489/sn76489.cpp ${C}/video/tms9918/tms9918.cpp ${C}/audio/ym2413/ym2413.cpp
  ${C}/io/i8255/i8255.cpp ${C}/arcade/dip-switches.cpp
  ${A}/ares/ms/ms.cpp ${A}/ares/sg/sg.cpp)
target_include_directories(aressega PUBLIC ${A}/ares ${A} ${A}/nall ${A}/thirdparty)
target_compile_definitions(aressega PUBLIC ${COMMON_DEFS})
target_compile_options(aressega PUBLIC ${COMMON_FLAGS})
target_link_libraries(aressega PUBLIC libco sljit pthread dl)

add_executable(sega-oracle main.cpp ms.cpp sg.cpp $<TARGET_OBJECTS:nallobj>)
target_link_libraries(sega-oracle PRIVATE aressega z)
