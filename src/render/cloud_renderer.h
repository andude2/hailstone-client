#pragma once

// The modern sky's volumetric clouds (docs/graphics.md "Volumetric clouds"): shaders/clouds.comp marches a layer of
// cloud along each view ray into an image at half the scene's size, which the atmosphere (shaders/atmosphere.frag)
// blends over the sky. Its noise is made on the GPU when the renderer is made (shaders/cloud_noise.comp). The maths
// it runs on is render/clouds.h.

#include <array>
#include <cstdint>
#include <filesystem>

#include <glm/glm.hpp>

#include "render/clouds.h"
#include "render/gpu.h"
#include "render/vk_context.h"

namespace hailstone::render {

// What the clouds are lit and shaped by this frame.
struct CloudFrame {
    glm::mat4 sky_view_proj{1.0f}; // the projection times the view's rotation (no translation): the sky's
    CloudCover cover;
    glm::vec3 to_light{0.0f, 0.0f, 1.0f}; // render::Daylight::to_light
    glm::vec3 light{0.0f};                // render::Daylight::light (after render::apply_storm)
    glm::vec3 ambient{0.0f};              // render::Daylight::sky (likewise)
    float storm = 0.0f;                   // render::WeatherLook::storm
    double seconds = 0.0;                 // the wall clock (the wind)
};

class CloudRenderer {
public:
    CloudRenderer(VkContext& vk, const std::filesystem::path& shader_dir);
    ~CloudRenderer();
    CloudRenderer(const CloudRenderer&) = delete;
    CloudRenderer& operator=(const CloudRenderer&) = delete;

    // Records this frame's march into frame.cmd: outside rendering, before VkContext::begin_scene. Afterwards view()
    // is this frame's clouds, readable by fragment shaders in VK_IMAGE_LAYOUT_GENERAL.
    void record(const FrameInfo& frame, const CloudFrame& clouds);
    VkImageView view() const { return targets_[current_].view; }
    VkSampler sampler() const { return clamp_sampler_; }

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    Image make_image(VkImageType type, VkFormat format, glm::uvec3 size);
    void free_image(Image& image);
    void make_noise(const std::filesystem::path& shader_dir);
    void resize(glm::uvec2 extent);

    VkContext& vk_;
    Image shape_, detail_, weather_;
    std::array<Image, 2> targets_{}; // this frame's and the last one's, in turn
    glm::uvec2 extent_{0u};
    bool targets_fresh_ = false; // made since the last record: still in VK_IMAGE_LAYOUT_UNDEFINED
    std::uint32_t current_ = 0;
    std::uint64_t frame_ = 0;

    VkSampler repeat_sampler_ = VK_NULL_HANDLE;
    VkSampler clamp_sampler_ = VK_NULL_HANDLE;
    std::array<Buffer, VkContext::kFramesInFlight> uniforms_{};
    std::array<void*, VkContext::kFramesInFlight> uniforms_mapped_{};
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    // One per frame slot and target: [slot * 2 + target written].
    std::array<VkDescriptorSet, VkContext::kFramesInFlight * 2> sets_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    // The last frame's, for the history: its view and what its sky was.
    glm::mat4 last_view_proj_{1.0f};
    CloudHistoryKey last_key_;
    glm::vec2 wind_{0.0f};
    double last_seconds_ = -1.0;
};

} // namespace hailstone::render
