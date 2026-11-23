// Opaque Vulkan types, so the skeleton compiles without the Vulkan SDK.
// Phase 2 (MAKO/lsfg inference) will replace these with real vulkan.h includes.
#pragma once

typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef struct VkExtensionProperties VkExtensionProperties;
typedef void* PFN_vkGetInstanceProcAddr;
typedef void* PFN_vkGetDeviceProcAddr;
