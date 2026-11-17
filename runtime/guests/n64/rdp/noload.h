/**
 * @file noload.h
 * @brief Included first in every parallel-rdp source relift builds: points
 * its own Vulkan loader lookups (taken only when it is given no
 * vkGetInstanceProcAddr, and prdp.cpp always gives it hostlib's) at
 * functions in prdp.cpp that open nothing, so a release opens libraries
 * only through hostlib. relift build also includes prdp_names.h, which
 * gives volk's global function pointers names of their own, apart from the
 * functions of a loader the runtime links (MoltenVK).
 */
#ifndef RELIFT_N64_NOLOAD_H
#define RELIFT_N64_NOLOAD_H

#define dlopen rl_n64_prdp_no_dlopen
#define LoadLibraryA rl_n64_prdp_no_load_library

#endif
