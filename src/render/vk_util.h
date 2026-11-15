#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <volk.h>

#include "render/spirv_file.h"

namespace hailstone::render {

// Back-face culling of EQ geometry. The original culls counter-clockwise
// triangles (D3DCULL_CCW, set at device init by 0x100916d0 and again by
// CRender::DrawRenderEffectBuckets @0x1008b470); WLD triangles are clockwise
// from the front. The screen winding after this client's projection was
// mapped by testing.
inline constexpr VkFrontFace kEqFrontFace = VK_FRONT_FACE_CLOCKWISE;

inline constexpr VkFrontFace opposite(VkFrontFace f) {
    return f == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
}

// Whether end_frame's screenshot readback can take a swapchain of this format: it copies 4 bytes a pixel and
// reads them as RGBA (swapping BGRA), so only the 8-bit four-channel formats will do; a surface that only offers
// FP16 or 10-bit HDR formats is refused rather than read past its buffer.
inline constexpr bool screenshot_format_supported(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_UNORM ||
           f == VK_FORMAT_R8G8B8A8_SRGB;
}

inline void vk_check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: VkResult " + std::to_string(r));
}

// A shader module from a .spv file (read_spirv_file's checks).
inline VkShaderModule create_shader_module(VkDevice device, const std::filesystem::path& path) {
    const auto code = read_spirv_file(path);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * 4;
    info.pCode = code.data();
    VkShaderModule module;
    vk_check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

inline VkImageView create_image_view(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                     std::uint32_t mip_levels) {
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = format;
    v.subresourceRange = {aspect, 0, mip_levels, 0, 1};
    VkImageView view;
    vk_check(vkCreateImageView(device, &v, nullptr, &view), "vkCreateImageView");
    return view;
}

inline void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                          VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                          VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, std::uint32_t base_mip = 0,
                          std::uint32_t mip_count = VK_REMAINING_MIP_LEVELS) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, base_mip, mip_count, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace hailstone::render
