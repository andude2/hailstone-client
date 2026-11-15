#pragma once

// The browser tabs' images as ImGui textures (docs/browser.md): one BGRA8 texture per tab and layer (the page, its
// popup), kept in step with web::BrowserHost by copying only the changed rectangles into the frame's command buffer.
// A texture that goes (the tab closed, or a new size) is retired, not destroyed, until the frames that may still draw
// it are done: the Lua windows take this frame's texture before record() runs, and ImGui draws it after.

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

#include "render/gpu.h"
#include "web/browser_protocol.h"

namespace hailstone::render { class VkContext; struct FrameInfo; }
namespace hailstone::web { class BrowserHost; }

namespace hailstone::ui {

class WebTextures {
public:
    explicit WebTextures(render::VkContext& vk);
    ~WebTextures();
    WebTextures(const WebTextures&) = delete;
    WebTextures& operator=(const WebTextures&) = delete;

    struct View {
        void* id = nullptr; // ImTextureID; null until the tab's first image is up
        int width = 0, height = 0;
    };
    View texture(std::uint32_t tab, web::Layer layer = web::Layer::View) const;

    // Uploads what changed in each tab since the last frame and retires the textures of tabs that are gone. Between
    // VkContext::begin_frame_commands and begin_scene.
    void record(const render::FrameInfo& frame, web::BrowserHost& host);

    // Destroys the retired textures whose frames are done (record does this too). Called each frame record is not,
    // so a texture retired before the tabs stopped being recorded does not stay until shutdown.
    void collect();

private:
    struct Entry {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::uint16_t width = 0, height = 0;
        bool ready = false; // uploaded once: in the shader-read layout
    };
    using Key = std::pair<std::uint32_t, web::Layer>;

    Entry make(std::uint16_t width, std::uint16_t height);
    void destroy(Entry& e);
    void retire(Entry e);
    void upload(VkCommandBuffer cmd, std::uint32_t slot, Entry& e, const std::vector<std::byte>& bgra,
                const std::vector<VkBufferImageCopy>& regions, std::size_t bytes);

    render::VkContext& vk_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::map<Key, Entry> entries_;
    std::vector<std::pair<std::uint64_t, Entry>> retired_; // with the frame (VkContext::frames_begun) they may go at
    struct Staging {
        render::Buffer buffer;
        void* mapped = nullptr;
    };
    std::vector<Staging> staging_; // one per frame slot, grown as needed
};

} // namespace hailstone::ui
