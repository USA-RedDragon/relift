cmake_minimum_required(VERSION 3.20)
project(n64oracle C CXX)
if(NOT ARES_SOURCE)
  message(FATAL_ERROR "set ARES_SOURCE to the ares checkout")
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
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/ares.cpp.gen.cpp)
file(WRITE ${GEN} "#include <ares/ares.hpp>\n#include <ares/debug/debug.cpp>\n#include <nall/gdb/server.cpp>\n#include <ares/node/node.cpp>\n\nnamespace ares {\nPlatform* platform = nullptr;\natomic<bool> _runAhead = false;\nconst string Name = \"ares\";\nconst string Version = \"oracle\";\nconst string Copyright = \"ISC\";\nconst string License = \"ISC\";\nconst string LicenseURI = \"https://opensource.org/licenses/ISC\";\nconst string Website = \"ares-emu.net\";\nconst string WebsiteURI = \"https://ares-emu.net/\";\nconst u32 SerializerSignature = 0x31545342;\n}\n")
set(N ${A}/ares/n64)
set(PR ${N}/vulkan/parallel-rdp)

add_library(libco STATIC ${A}/libco/libco.c ${A}/libco/amd64.c)
add_library(sljit STATIC ${A}/thirdparty/sljit/sljit_src/sljitLir.c)
target_include_directories(sljit PUBLIC ${A}/thirdparty)
target_compile_definitions(sljit PUBLIC SLJIT_HAVE_CONFIG_PRE=1 SLJIT_HAVE_CONFIG_POST=1)

set(COMMON_FLAGS -fno-strict-aliasing -fwrapv -fno-char8_t -w -msse4.1 -mavx2 -mbmi2 -mpopcnt)
set(COMMON_DEFS PROFILE_PERFORMANCE CORE_N64 VULKAN BUILD_RELEASE BUILD_LOCAL)

add_library(nallobj OBJECT ${A}/nall/nall/nall.cpp ${A}/nall/nall/sljitAllocator.cpp)

set(N64_SRC
  memory/memory.cpp system/system.cpp cartridge/cartridge.cpp cic/cic.cpp controller/controller.cpp
  dd/dd.cpp mi/mi.cpp vi/vi.cpp ai/ai.cpp pi/pi.cpp pif/pif.cpp ri/ri.cpp si/si.cpp rdram/rdram.cpp
  cpu/cpu.cpp rsp/rsp.cpp rdp/rdp.cpp aleck64/aleck64.cpp)
set(PRDP_SRC vulkan/vulkan.cpp
  vulkan/parallel-rdp/parallel-rdp/command_ring.cpp vulkan/parallel-rdp/parallel-rdp/rdp_device.cpp
  vulkan/parallel-rdp/parallel-rdp/rdp_dump_write.cpp vulkan/parallel-rdp/parallel-rdp/rdp_renderer.cpp
  vulkan/parallel-rdp/parallel-rdp/video_interface.cpp
  vulkan/parallel-rdp/vulkan/buffer.cpp vulkan/parallel-rdp/vulkan/buffer_pool.cpp vulkan/parallel-rdp/vulkan/command_buffer.cpp
  vulkan/parallel-rdp/vulkan/command_pool.cpp vulkan/parallel-rdp/vulkan/context.cpp vulkan/parallel-rdp/vulkan/cookie.cpp
  vulkan/parallel-rdp/vulkan/descriptor_set.cpp vulkan/parallel-rdp/vulkan/device.cpp vulkan/parallel-rdp/vulkan/event_manager.cpp
  vulkan/parallel-rdp/vulkan/fence.cpp vulkan/parallel-rdp/vulkan/fence_manager.cpp vulkan/parallel-rdp/vulkan/image.cpp
  vulkan/parallel-rdp/vulkan/indirect_layout.cpp vulkan/parallel-rdp/vulkan/memory_allocator.cpp
  vulkan/parallel-rdp/vulkan/pipeline_event.cpp vulkan/parallel-rdp/vulkan/query_pool.cpp vulkan/parallel-rdp/vulkan/render_pass.cpp
  vulkan/parallel-rdp/vulkan/sampler.cpp vulkan/parallel-rdp/vulkan/semaphore.cpp vulkan/parallel-rdp/vulkan/semaphore_manager.cpp
  vulkan/parallel-rdp/vulkan/shader.cpp vulkan/parallel-rdp/vulkan/texture/texture_format.cpp
  vulkan/parallel-rdp/util/arena_allocator.cpp vulkan/parallel-rdp/util/logging.cpp vulkan/parallel-rdp/util/thread_id.cpp
  vulkan/parallel-rdp/util/aligned_alloc.cpp vulkan/parallel-rdp/util/timer.cpp vulkan/parallel-rdp/util/timeline_trace_file.cpp
  vulkan/parallel-rdp/util/thread_name.cpp vulkan/parallel-rdp/util/environment.cpp)
list(TRANSFORM N64_SRC PREPEND ${N}/)
list(TRANSFORM PRDP_SRC PREPEND ${N}/)

add_library(aresn64 STATIC ${GEN} ${A}/ares/ares/memory/fixed-allocator.cpp
  ${A}/ares/component/processor/sm5k/sm5k.cpp ${A}/ares/component/arcade/dip-switches.cpp
  ${N64_SRC} ${PRDP_SRC} ${PR}/volk/volk.c)
set_source_files_properties(${PR}/volk/volk.c PROPERTIES LANGUAGE C)
target_include_directories(aresn64 PUBLIC ${A}/ares ${A} ${A}/nall ${A}/thirdparty
  ${PR} ${PR}/vulkan ${PR}/vulkan-headers/include ${PR}/volk ${PR}/util ${PR}/parallel-rdp ${N}/vulkan)
target_compile_definitions(aresn64 PUBLIC ${COMMON_DEFS})
target_compile_options(aresn64 PUBLIC ${COMMON_FLAGS})
target_link_libraries(aresn64 PUBLIC libco sljit pthread dl)
target_sources(aresn64 PRIVATE $<TARGET_OBJECTS:nallobj>)
target_include_directories(nallobj PRIVATE ${A} ${A}/nall ${A}/thirdparty)
target_compile_definitions(nallobj PRIVATE ${COMMON_DEFS} SLJIT_HAVE_CONFIG_PRE=1 SLJIT_HAVE_CONFIG_POST=1)
target_compile_options(nallobj PRIVATE ${COMMON_FLAGS})

add_executable(n64-oracle ${CMAKE_CURRENT_SOURCE_DIR}/driver.cpp $<TARGET_OBJECTS:nallobj>)
target_link_libraries(n64-oracle PRIVATE aresn64)
