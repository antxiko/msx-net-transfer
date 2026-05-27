//=============================================================================
// msxgl_config.h — NHGET 2.0 (navegador HTTP NetTransfer)
//=============================================================================
#pragma once

//-----------------------------------------------------------------------------
// BIOS
//-----------------------------------------------------------------------------
#define BIOS_CALL_MAINROM          BIOS_CALL_DIRECT
#define BIOS_USE_MAINROM           TRUE
#define BIOS_USE_VDP               TRUE
#define BIOS_USE_PSG               FALSE
#define BIOS_USE_SUBROM            FALSE
#define BIOS_USE_DISKROM           FALSE

//-----------------------------------------------------------------------------
// VDP — SCREEN 0 ancho 40 y 80 columnas (T1 + T2)
//-----------------------------------------------------------------------------
#define VDP_VRAM_ADDR              VDP_VRAM_ADDR_17
#define VDP_INIT_50HZ              VDP_INIT_OFF
#define VDP_UNIT                   VDP_UNIT_U8

#define VDP_USE_MODE_T1            TRUE
#define VDP_USE_MODE_T2            TRUE
#define VDP_USE_MODE_G1            FALSE
#define VDP_USE_MODE_G2            FALSE
#define VDP_USE_MODE_MC            FALSE
#define VDP_USE_MODE_G3            FALSE
#define VDP_USE_MODE_G4            FALSE
#define VDP_USE_MODE_G5            FALSE
#define VDP_USE_MODE_G6            FALSE
#define VDP_USE_MODE_G7            FALSE

#define VDP_USE_VRAM16K            FALSE
#define VDP_USE_SPRITE             FALSE
#define VDP_USE_COMMAND            FALSE
#define VDP_USE_CUSTOM_CMD         FALSE
#define VDP_AUTO_INIT              FALSE
#define VDP_USE_UNDOCUMENTED       FALSE
#define VDP_USE_VALIDATOR          FALSE

//-----------------------------------------------------------------------------
// PRINT — no se usa (escribimos via DOS_CharOutput y secuencias VT-52)
//-----------------------------------------------------------------------------
#define PRINT_USE_TEXT             FALSE
#define PRINT_USE_VALIDATOR        FALSE

//-----------------------------------------------------------------------------
// INPUT — KEY_xxx macros necesarias en bios.h aunque no usemos input module
//-----------------------------------------------------------------------------
#define INPUT_USE_KEYBOARD         TRUE
#define INPUT_USE_JOYSTICK         FALSE
#define INPUT_USE_MANAGER          FALSE
#define INPUT_JOY_UPDATE           FALSE
#define INPUT_KB_UPDATE            FALSE
#define INPUT_KB_UPDATE_MIN        0
#define INPUT_KB_UPDATE_MAX        0

//-----------------------------------------------------------------------------
// MEMORY
//-----------------------------------------------------------------------------
#define MEMORY_USE_VALIDATOR       FALSE

//-----------------------------------------------------------------------------
// SYSTEM
//-----------------------------------------------------------------------------
#define SYSTEM_USE_MSX_VERSION     FALSE
#define SYSTEM_USE_SLOT            FALSE

//-----------------------------------------------------------------------------
// DOS — handle-based, sin FCB (DOS-2)
//-----------------------------------------------------------------------------
#define DOS_USE_FCB                FALSE
#define DOS_USE_HANDLE             TRUE
#define DOS_USE_UTILITIES          TRUE
// IMPORTANTE: TRUE para que DOS_FindFirstEntry devuelva NULL si el fichero
// no existe. Sin esto MSXgl siempre devuelve g_DOS_LastFIB (con datos del
// FFIRST anterior) y servimos contenido equivocado bajo paths inexistentes.
#define DOS_USE_VALIDATOR          TRUE
#define DOS_USE_ERROR_HANDLER      FALSE
#define DOS_USE_BIOSCALL           TRUE
