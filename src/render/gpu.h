#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace hailstone::assets { struct Image; }

namespace hailstone::render {

class VkContext;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void destroy(VmaAllocator allocator);
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    void destroy(VkDevice device, VmaAllocator allocator);
};

// What a texture holds. Colour textures of the world get their mip chain built on the CPU (assets::build_color_mips:
// linear filtering, alpha-weighted colour, cut-out coverage kept); data (normal maps, UI art) keeps the plain chain.
// OpaqueColor is a colour texture whose alpha its material never reads (MaterialState::alpha_unused): made opaque
// first (assets::make_opaque), so the colour under its transparent texels is kept, as the original draws it; a Color
// texture painted on a colour key has it taken out first (assets::remove_color_key: no cyan fringe on the leaves).
enum class TextureUse : std::uint8_t { Data, Color, OpaqueColor };

// Collects uploads of device-local buffers and textures and performs them in
// one submission. submit() does not wait for it (VkContext::submit_upload): the
// frames submitted after it see the results, and its staging memory is freed
// once the GPU is done with it. A target destroyed soon after goes through
// VkContext::retire (or after VkContext::wait_idle).
// The CPU half of UploadBatch::texture for a colour texture: the image with its mip chain built (and its colour key
// taken out, or made opaque), or nullopt when the image goes up as it is. Safe on any thread; an image it returns
// uploads unchanged as TextureUse::Data.
std::optional<assets::Image> prepare_texture_image(const assets::Image& image, TextureUse use);
// Drops the image's largest mip levels until it is at most `max_size` texels across (0: none dropped), keeping at
// least one level.
void drop_large_mips(assets::Image& image, std::uint32_t max_size);

class UploadBatch {
public:
    explicit UploadBatch(VkContext& vk) : vk_(vk) {}
    ~UploadBatch();
    UploadBatch(const UploadBatch&) = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;

    Buffer buffer(std::span<const std::byte> data, VkBufferUsageFlags usage);
    // A buffer filled in pieces: the target is created once, and each piece is staged and copied on its own, so
    // the staging holds one piece at a time. A zone's vertex buffer is as big as the zone's geometry, and staging
    // it whole would double that while the copy is made.
    Buffer buffer_pieces(std::span<const std::byte> data, VkBufferUsageFlags usage, std::size_t piece_bytes);

    // The staging this batch holds (host memory the GPU has not read yet).
    std::size_t staging_bytes() const;

    // BCn images are uploaded compressed when the device supports it and
    // decompressed otherwise. Single-level RGBA8 images get a full mip chain; single-level colour images of any format
    // get theirs from assets::build_color_mips.
    Texture texture(const assets::Image& image, TextureUse use = TextureUse::Data);

    void submit();

private:
    struct PendingBuffer {
        Buffer staging;
        Buffer target;
        VkDeviceSize target_offset = 0; // where in the target the staging goes
        VkDeviceSize size = 0;           // how many bytes (0: the staging's whole size)
    };
    struct PendingTexture {
        Buffer staging;
        VkImage image;
        std::vector<VkBufferImageCopy> regions;
        std::uint32_t mip_levels;
        std::uint32_t width, height;
        bool generate_mips;
    };

    Buffer staging(std::span<const std::byte> data);

    VkContext& vk_;
    std::vector<PendingBuffer> buffers_;
    std::vector<PendingTexture> textures_;
};

} // namespace hailstone::render
