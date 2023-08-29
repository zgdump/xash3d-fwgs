#pragma once

#include "vk_common.h"

enum {
	LogModule_Vulkan = (1<<0),
	LogModule_Textures = (1<<1),
	LogModule_Brush = (1<<2),
	LogModule_Lights = (1<<3),
	LogModule_Studio = (1<<4),
	LogModule_Patch = (1<<5),
	LogModule_Material = (1<<6),
};

extern uint32_t g_log_debug_bits;

// TODO:
// - load bits early at startup somehow. cvar is empty at init for some reason
// - module name in message
// - file:line in message

#define DEBUG(msg, ...) \
	do { \
		if (g_log_debug_bits & (LOG_MODULE)) { \
			gEngine.Con_Reportf("vk: " msg "\n", ##__VA_ARGS__); \
		} \
	} while(0)

#define WARN(msg, ...) \
	do { \
		gEngine.Con_Printf(S_WARN "vk: " msg "\n", ##__VA_ARGS__); \
	} while(0)

#define ERR(msg, ...) \
	do { \
		gEngine.Con_Printf(S_ERROR "vk: " msg "\n", ##__VA_ARGS__); \
	} while(0)

#define INFO(msg, ...) \
	do { \
		gEngine.Con_Printf("vk: " msg "\n", ##__VA_ARGS__); \
	} while(0)

// Read debug-enabled modules from cvar
void VK_LogsReadCvar(void);
