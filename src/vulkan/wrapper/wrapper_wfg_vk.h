// SPDX-FileCopyrightText: Copyright 2026 qwertypower (DEVAR Entertainment LLC)
// SPDX-License-Identifier: MIT
//
// Function-pointer dispatch for WFG's Vulkan calls (from WinLite's wfg/src/vk_dispatch.h).
//
// In the wrapper every call goes straight to the underlying driver: wrapper_fg.c fills the
// table from the wrapper device's own dispatch tables (wrapper_wfg_bind), and the handles WFG
// gets are the driver's. Only the entry points WFG calls are in the table.
// See src/vulkan/wrapper/LICENSE.wfg.

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <stdbool.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPushConstants CmdPushConstants;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdSetViewport CmdSetViewport;
    PFN_vkCmdSetScissor CmdSetScissor;
    PFN_vkCmdDraw CmdDraw;
} VkDispatch;

extern VkDispatch vkd;   /* defined in wrapper_fg.c */

// Filled once, by wrapper_wfg_bind() (wrapper_fg.c), before the first vkr_dis_create.

#ifdef __cplusplus
}
#endif

// Redirect bare `vkFoo` names to the dispatch table (not in wrapper_fg.c, which only fills it).
#ifndef WRAPPER_WFG_VK_NO_REDIRECT

#define vkGetInstanceProcAddr vkd.GetInstanceProcAddr
#define vkGetPhysicalDeviceProperties vkd.GetPhysicalDeviceProperties
#define vkGetPhysicalDeviceMemoryProperties vkd.GetPhysicalDeviceMemoryProperties
#define vkGetPhysicalDeviceFormatProperties vkd.GetPhysicalDeviceFormatProperties
#define vkGetDeviceProcAddr vkd.GetDeviceProcAddr
#define vkAllocateMemory vkd.AllocateMemory
#define vkFreeMemory vkd.FreeMemory
#define vkCreateImage vkd.CreateImage
#define vkDestroyImage vkd.DestroyImage
#define vkBindImageMemory vkd.BindImageMemory
#define vkGetImageMemoryRequirements vkd.GetImageMemoryRequirements
#define vkCreateImageView vkd.CreateImageView
#define vkDestroyImageView vkd.DestroyImageView
#define vkCreateSampler vkd.CreateSampler
#define vkDestroySampler vkd.DestroySampler
#define vkCreateDescriptorSetLayout vkd.CreateDescriptorSetLayout
#define vkDestroyDescriptorSetLayout vkd.DestroyDescriptorSetLayout
#define vkCreateDescriptorPool vkd.CreateDescriptorPool
#define vkDestroyDescriptorPool vkd.DestroyDescriptorPool
#define vkAllocateDescriptorSets vkd.AllocateDescriptorSets
#define vkUpdateDescriptorSets vkd.UpdateDescriptorSets
#define vkCreatePipelineLayout vkd.CreatePipelineLayout
#define vkDestroyPipelineLayout vkd.DestroyPipelineLayout
#define vkCreateComputePipelines vkd.CreateComputePipelines
#define vkDestroyPipeline vkd.DestroyPipeline
#define vkCreateShaderModule vkd.CreateShaderModule
#define vkDestroyShaderModule vkd.DestroyShaderModule
#define vkCmdBindPipeline vkd.CmdBindPipeline
#define vkCmdBindDescriptorSets vkd.CmdBindDescriptorSets
#define vkCmdPushConstants vkd.CmdPushConstants
#define vkCmdPipelineBarrier vkd.CmdPipelineBarrier
#define vkCmdBlitImage vkd.CmdBlitImage
#define vkCmdDispatch vkd.CmdDispatch
#define vkCreateRenderPass vkd.CreateRenderPass
#define vkDestroyRenderPass vkd.DestroyRenderPass
#define vkCreateGraphicsPipelines vkd.CreateGraphicsPipelines
#define vkCmdBeginRenderPass vkd.CmdBeginRenderPass
#define vkCmdEndRenderPass vkd.CmdEndRenderPass
#define vkCmdSetViewport vkd.CmdSetViewport
#define vkCmdSetScissor vkd.CmdSetScissor
#define vkCmdDraw vkd.CmdDraw
#endif
