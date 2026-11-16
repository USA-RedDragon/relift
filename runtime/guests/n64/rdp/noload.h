/**
 * @file noload.h
 * @brief Included first in every parallel-rdp source relift builds: points
 * its own Vulkan loader lookups (taken only when it is given no
 * vkGetInstanceProcAddr, and prdp.cpp always gives it hostlib's) at
 * functions in prdp.cpp that open nothing, so a release opens libraries
 * only through hostlib; and renames volk's global vkGetInstanceProcAddr, a
 * function pointer, which would otherwise take the place of the loader's
 * function of that name where the runtime links the loader (MoltenVK).
 */
#ifndef RELIFT_N64_NOLOAD_H
#define RELIFT_N64_NOLOAD_H

#define dlopen rl_n64_prdp_no_dlopen
#define LoadLibraryA rl_n64_prdp_no_load_library
#define vkGetInstanceProcAddr rl_n64_prdp_vkGetInstanceProcAddr

#endif
