#include "render/cloud_renderer.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/vk_util.h"

namespace hailstone::render {

namespace {

// shaders/clouds.comp's uniform block (std140: mat4s, vec4s and an ivec4 only).
struct CloudUniforms {
    glm::mat4 inverse_view_proj;
    glm::mat4 last_view_proj;
    glm::vec4 to_light;
    glm::vec4 light;
    glm::vec4 ambient;
    glm::vec4 params;
    glm::vec4 wind;
    glm::ivec4 extent;
};

constexpr std::uint32_t kShapeSize = 128;
constexpr std::uint32_t kDetailSize = 32;
constexpr std::uint32_t kWeatherSize = 512;
constexpr VkFormat kNoiseFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kTargetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// The set's bindings (shaders/clouds.comp): 0 the shape, 1 the detail, 2 the weather map, 3 the last frame's clouds,
// 4 this frame's (written), 5 the uniforms.
constexpr std::uint32_t kBindings = 6;

void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst;
    b.dstAccessMask = dst_access;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkSampler make_sampler(VkDevice device, VkSamplerAddressMode mode) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = mode;
    VkSampler s = VK_NULL_HANDLE;
    vk_check(vkCreateSampler(device, &si, nullptr, &s), "vkCreateSampler (clouds)");
    return s;
}

std::uint32_t groups(std::uint32_t size, std::uint32_t local) { return (size + local - 1) / local; }

} // namespace

CloudRenderer::CloudRenderer(VkContext& vk, const std::filesystem::path& shader_dir) : vk_(vk) {
    VkDevice d = vk_.device();
    repeat_sampler_ = make_sampler(d, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    clamp_sampler_ = make_sampler(d, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    for (std::size_t i = 0; i < uniforms_.size(); ++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(CloudUniforms);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        vk_check(vmaCreateBuffer(vk_.allocator(), &bi, &ai, &uniforms_[i].buffer, &uniforms_[i].allocation, &out),
                 "create cloud uniform buffer");
        uniforms_[i].size = bi.size;
        uniforms_mapped_[i] = out.pMappedData;
    }

    VkDescriptorSetLayoutBinding b[kBindings]{};
    for (std::uint32_t i = 0; i < kBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i == 4   ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                              : i == 5 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                       : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = kBindings;
    li.pBindings = b;
    vk_check(vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_), "vkCreateDescriptorSetLayout (clouds)");

    constexpr auto kSets = static_cast<std::uint32_t>(VkContext::kFramesInFlight * 2);
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kSets},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kSets;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    vk_check(vkCreateDescriptorPool(d, &pi, nullptr, &pool_), "vkCreateDescriptorPool (clouds)");
    std::array<VkDescriptorSetLayout, kSets> layouts;
    layouts.fill(set_layout_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSets;
    ai.pSetLayouts = layouts.data();
    vk_check(vkAllocateDescriptorSets(d, &ai, sets_.data()), "vkAllocateDescriptorSets (clouds)");

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    vk_check(vkCreatePipelineLayout(d, &pli, nullptr, &layout_), "vkCreatePipelineLayout (clouds)");
    VkShaderModule m = create_shader_module(d, shader_dir / "clouds.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = layout_;
    const VkResult r = vkCreateComputePipelines(d, vk_.pipeline_cache(), 1, &ci, nullptr, &pipeline_);
    vkDestroyShaderModule(d, m, nullptr);
    vk_check(r, "vkCreateComputePipelines (clouds)");

    make_noise(shader_dir);
}

CloudRenderer::~CloudRenderer() {
    VkDevice d = vk_.device();
    vk_.wait_idle();
    for (Image* i : {&shape_, &detail_, &weather_, &targets_[0], &targets_[1]}) free_image(*i);
    if (pipeline_) vkDestroyPipeline(d, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
    for (VkSampler s : {repeat_sampler_, clamp_sampler_})
        if (s) vkDestroySampler(d, s, nullptr);
    for (auto& u : uniforms_) u.destroy(vk_.allocator());
}

CloudRenderer::Image CloudRenderer::make_image(VkImageType type, VkFormat format, glm::uvec3 size) {
    Image out;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = type;
    ii.format = format;
    ii.extent = {size.x, size.y, size.z};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    vk_check(vmaCreateImage(vk_.allocator(), &ii, &ai, &out.image, &out.allocation, nullptr), "create cloud image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(vk_.device(), &vi, nullptr, &out.view), "create cloud image view");
    return out;
}

void CloudRenderer::free_image(Image& image) {
    if (image.view) vkDestroyImageView(vk_.device(), image.view, nullptr);
    if (image.image) vmaDestroyImage(vk_.allocator(), image.image, image.allocation);
    image = {};
}

void CloudRenderer::make_noise(const std::filesystem::path& shader_dir) {
    VkDevice d = vk_.device();
    shape_ = make_image(VK_IMAGE_TYPE_3D, kNoiseFormat, {kShapeSize, kShapeSize, kShapeSize});
    detail_ = make_image(VK_IMAGE_TYPE_3D, kNoiseFormat, {kDetailSize, kDetailSize, kDetailSize});
    weather_ = make_image(VK_IMAGE_TYPE_2D, kNoiseFormat, {kWeatherSize, kWeatherSize, 1});

    // A pipeline of its own, used once: shaders/cloud_noise.comp writes the three images.
    VkDescriptorSetLayoutBinding b[3]{};
    for (std::uint32_t i = 0; i < 3; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = b;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    vk_check(vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout), "vkCreateDescriptorSetLayout (cloud noise)");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vk_check(vkCreateDescriptorPool(d, &pi, nullptr, &pool), "vkCreateDescriptorPool (cloud noise)");
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    vk_check(vkAllocateDescriptorSets(d, &ai, &set), "vkAllocateDescriptorSets (cloud noise)");
    VkDescriptorImageInfo images[3] = {{VK_NULL_HANDLE, shape_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, detail_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, weather_.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(d, 3, w, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vk_check(vkCreatePipelineLayout(d, &pli, nullptr, &layout), "vkCreatePipelineLayout (cloud noise)");
    VkShaderModule m = create_shader_module(d, shader_dir / "cloud_noise.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateComputePipelines(d, vk_.pipeline_cache(), 1, &ci, nullptr, &pipeline);
    vkDestroyShaderModule(d, m, nullptr);
    vk_check(r, "vkCreateComputePipelines (cloud noise)");

    vk_.immediate_submit([&](VkCommandBuffer cmd) {
        for (const Image* i : {&shape_, &detail_, &weather_})
            image_barrier(cmd, i->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const glm::uvec3 sizes[3] = {glm::uvec3(kShapeSize), glm::uvec3(kDetailSize), {kWeatherSize, kWeatherSize, 1}};
        for (std::uint32_t mode = 0; mode < 3; ++mode) {
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mode), &mode);
            vkCmdDispatch(cmd, groups(sizes[mode].x, 4), groups(sizes[mode].y, 4), groups(sizes[mode].z, 4));
        }
        for (const Image* i : {&shape_, &detail_, &weather_})
            image_barrier(cmd, i->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    vkDestroyPipeline(d, pipeline, nullptr);
    vkDestroyPipelineLayout(d, layout, nullptr);
    vkDestroyDescriptorPool(d, pool, nullptr);
    vkDestroyDescriptorSetLayout(d, set_layout, nullptr);
    log::info("clouds: noise made ({}^3 shape, {}^3 detail, {}^2 weather)", kShapeSize, kDetailSize, kWeatherSize);
}

void CloudRenderer::resize(glm::uvec2 extent) {
    vk_.wait_idle(); // the old targets may still be read by a frame in flight
    for (auto& t : targets_) free_image(t);
    for (auto& t : targets_) t = make_image(VK_IMAGE_TYPE_2D, kTargetFormat, {extent.x, extent.y, 1});
    extent_ = extent;
    targets_fresh_ = true;

    VkDevice d = vk_.device();
    for (std::uint32_t slot = 0; slot < VkContext::kFramesInFlight; ++slot)
        for (std::uint32_t target = 0; target < 2; ++target) {
            VkDescriptorImageInfo images[5] = {
                {repeat_sampler_, shape_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {repeat_sampler_, detail_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {repeat_sampler_, weather_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {clamp_sampler_, targets_[target ^ 1u].view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, targets_[target].view, VK_IMAGE_LAYOUT_GENERAL},
            };
            VkDescriptorBufferInfo buffer{uniforms_[slot].buffer, 0, sizeof(CloudUniforms)};
            VkWriteDescriptorSet w[kBindings]{};
            for (std::uint32_t i = 0; i < kBindings; ++i) {
                w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[i].dstSet = sets_[slot * 2 + target];
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                if (i == 5) {
                    w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    w[i].pBufferInfo = &buffer;
                } else {
                    w[i].descriptorType =
                        i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    w[i].pImageInfo = &images[i];
                }
            }
            vkUpdateDescriptorSets(d, kBindings, w, 0, nullptr);
        }
}

void CloudRenderer::record(const FrameInfo& frame, const CloudFrame& clouds) {
    const glm::uvec2 extent = cloud_extent(frame.extent.width, frame.extent.height);
    if (extent != extent_) resize(extent);
    VkCommandBuffer cmd = frame.cmd;
    bool reset = false;
    if (targets_fresh_) {
        for (const Image& t : targets_)
            image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        targets_fresh_ = false;
        reset = true;
    }

    // The wind, added up from each frame's step.
    const double dt = last_seconds_ < 0.0 ? 0.0 : clouds.seconds - last_seconds_;
    last_seconds_ = clouds.seconds;
    wind_ += cloud_wind_step(static_cast<float>(dt), clouds.storm);
    // Kept within a few tiles of the weather map (shaders/clouds.comp kWeatherTile, 60 km): the textures repeat.
    wind_ = glm::mod(wind_, glm::vec2(600.0f));

    const CloudHistoryKey key{clouds.to_light, clouds.storm, wind_};
    if (frame_ == 0 || cloud_history_stale(last_key_, key)) reset = true;
    last_key_ = key;

    const std::uint32_t target = current_ ^ 1u;
    CloudUniforms u{};
    u.inverse_view_proj = glm::inverse(clouds.sky_view_proj);
    u.last_view_proj = reset ? clouds.sky_view_proj : last_view_proj_;
    u.to_light = glm::vec4(clouds.to_light, 0.0f);
    u.light = glm::vec4(clouds.light, clouds.cover.darkness);
    u.ambient = glm::vec4(clouds.ambient, clouds.cover.coverage);
    u.params = glm::vec4(clouds.cover.density, clouds.cover.type,
                         static_cast<float>(std::fmod(clouds.seconds, 4096.0)), static_cast<float>(frame_ % 64));
    u.wind = glm::vec4(wind_, static_cast<float>(cloud_update_slot(frame_)), reset ? 1.0f : 0.0f);
    u.extent = glm::ivec4(static_cast<int>(extent.x), static_cast<int>(extent.y), 0, 0);
    std::memcpy(uniforms_mapped_[frame.frame_slot], &u, sizeof(u));
    vmaFlushAllocation(vk_.allocator(), uniforms_[frame.frame_slot].allocation, 0, VK_WHOLE_SIZE);

    // The target written now was read two frames ago by the sky and last frame as the history.
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                            &sets_[frame.frame_slot * 2 + target], 0, nullptr);
    vkCmdDispatch(cmd, groups(extent.x, 8), groups(extent.y, 8), 1);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    current_ = target;
    last_view_proj_ = clouds.sky_view_proj;
    ++frame_;
}

} // namespace hailstone::render
