#pragma once
/** --------------------------------------------------------------------------------------------------------- Vulkan Allocation
 * @file ba_vulkan.h
 * @brief Defines the private Vulkan allocation and staging interface.
 */
#include <vulkan/vulkan_core.h>
#include <stddef.h>
#include <stdint.h>

int ba_vk_initialize(void);
int ba_vk_supported(uint32_t properties);
uint64_t ba_vk_budget(uint32_t properties);
VkDevice ba_vk_device(void);
void* ba_vk_allocate(size_t bytes, uint32_t properties);
void ba_vk_free(void* allocation);
void* ba_vk_host(void* allocation);
VkBuffer ba_vk_buffer(void* allocation);
int ba_vk_transfer(void* allocation, size_t offset, size_t bytes, int to_device);
int ba_vk_zero(void* allocation, size_t offset, size_t bytes);
