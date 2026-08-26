#pragma once

#include "gbb/rom_library.hpp"

// Source compatibility for the existing Game Boy core and third-party users.
// New application code should use the system-neutral gbb namespace.
namespace gameboy {
using RomPlatform = gbb::RomPlatform;
using RomMetadata = gbb::RomMetadata;
using RomLibraryEntry = gbb::RomLibraryEntry;
using RomLibrary = gbb::RomLibrary;
using gbb::cover_system_name;
using gbb::inspect_rom;
using gbb::inspect_rom_file;
using gbb::platform_name;
} // namespace gameboy
