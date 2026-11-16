// SPDX-License-Identifier: MIT
// The few symbols the ares core expects its frontend to define, and the core
// sources that need no platform layer.
#include <ares/ares.hpp>
#include <ares/debug/debug.cpp>
#include <nall/gdb/server.cpp>
#include <ares/node/node.cpp>

namespace ares {
Platform* platform = nullptr;
atomic<bool> _runAhead = false;
const string Name       = "ares";
const string Version    = "oracle";
const string Copyright  = "ISC";
const string License    = "ISC";
const string LicenseURI = "https://opensource.org/licenses/ISC";
const string Website    = "ares-emu.net";
const string WebsiteURI = "https://ares-emu.net/";
const u32    SerializerSignature = 0x31545342;
}
