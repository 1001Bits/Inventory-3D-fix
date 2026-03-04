#pragma once

#define NOMMNOSOUND
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// CommonLibF4 must be included first (before Windows.h)
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include <REL/Relocation.h>

// Windows headers (after CommonLib, needed for VirtualProtect)
#include <Windows.h>
#include <ShlObj.h>

// Direct3D 11 (for D3D11 alpha fixup in mode 16+)
#include <d3d11.h>
#include <d3dcompiler.h>

using namespace std::literals;

// F4VRCommonFramework
#include "Logger.h"

using namespace f4cf;

#define DLLEXPORT __declspec(dllexport)

// Standard library
#include <cstdint>
#include <string>
