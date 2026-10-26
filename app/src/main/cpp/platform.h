// Cartucho - small platform layer shared by the core host and its users.
#pragma once

#include "libretro.h"

namespace cartucho {

// Logs one line (no trailing newline needed). `level` is a retro_log_level.
// Android: logcat under the "Cartucho" tag. Elsewhere: stderr.
void PlatformLog(int level, const char* text);

// Looks up an OpenGL ES function for a hardware-rendered core.
retro_proc_address_t PlatformGetProcAddress(const char* sym);

}  // namespace cartucho
