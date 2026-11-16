#pragma once

// The maths of the temporal upscaler (docs/graphics.md "Temporal upscaling"): the sub-pixel jitter put on the
// scene's projection, the reprojection from this frame's view to the last one's that the motion vectors come from,
// and when the history is thrown away. No Vulkan here: render/vk_context.cpp runs the passes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::render {

// How the 3D view is brought up to its size (display.upscaler). Fsr1: spatial (FSR 1's EASU + RCAS) below a render
// scale of 100, nothing at 100, MSAA as display.msaa says. Taa: temporal, the scene jittered each frame and
// accumulated into a history at the view's size (shaders/taa_resolve.frag), at every render scale; MSAA is off.
// Fsr3: the same jittered frames and motion, resolved by AMD FSR 3.1's upscaler (render/fsr3_upscaler.h); Taa where
// the device lacks what it needs. External: an upscaler provider's (render/upscaler_provider.h, an add-on in
// upscalers/<name>/ that display.upscaler names) on the same frames, in FSR 3.1's place; Fsr3, then Taa, without it.
enum class Upscaler : std::uint8_t { Fsr1, Taa, Fsr3, External };

// The render scale's range (display.render_scale, percent): down to a third of the view on each side (Ultra
// Performance, 3x).
inline constexpr int kMinRenderScalePercent = 33;
inline constexpr float kMinRenderScale = 0.33f;

// The upscale quality presets most games offer (display_option "upscale_quality"), each a render scale: Native AA
// draws at the view's size (the temporal upscalers then only anti-alias), the others at AMD's ratios for FSR's modes
// (1.5x, 1.7x, 2x, 3x on each side). Custom is any other render scale (the slider's).
enum class UpscaleQuality : std::uint8_t { NativeAa, Quality, Balanced, Performance, UltraPerformance, Custom };
inline constexpr int kUpscaleQualityPercent[] = {100, 67, 59, 50, 33};
// The preset a render scale (percent) is, Custom when it is none of theirs.
UpscaleQuality upscale_quality_for(int percent);
// A preset's render scale in percent; Custom has none (nullopt-like: 0).
int upscale_quality_percent(UpscaleQuality q);

// display.upscaler's values: "fsr1", "taa", "fsr3"; anything else is Fsr1 ("off" too, below), but for the name of
// an installed upscaler provider, which the client loads at the start (External).
Upscaler parse_upscaler(std::string_view name);
std::string_view upscaler_name(Upscaler u);

// display.upscaler = "off": no upscaling and no frame generation at all. The view is drawn at its full size whatever
// display.render_scale says (Fsr1 at 100 draws as it is, MSAA as display.msaa says), and display.frame_gen is
// ignored; both settings are kept for when an upscaler is chosen again.
inline constexpr std::string_view kUpscalerOff = "off";
bool upscaling_off(std::string_view name);

// The Options window's Upscaler choices (display_option "upscaler"): 0 Off, then 1 FSR 1, 2 Temporal, 3 FSR 3.1
// (each built-in Upscaler's value + 1), then 4 on the installed upscaler providers in `providers`' order (their
// names; render/upscaler_provider.h installed_upscaler_providers). upscaler_choice gives the choice display.upscaler's
// value is (a provider's name that is not installed is FSR 1, as any unknown value); upscaler_choice_name the value a
// choice saves (out of range is FSR 1).
int upscaler_choice(std::string_view name, const std::vector<std::string>& providers = {});
std::string upscaler_choice_name(int choice, const std::vector<std::string>& providers = {});
inline constexpr int kFirstProviderChoice = 4;

// The radical inverse of `index` in `base` (the Halton sequence): 1 -> 1/2, 2 -> 1/4, 3 -> 3/4 in base 2.
float halton(std::uint32_t index, std::uint32_t base);

// How many jitter positions a cycle holds at render scale `scale` (kMinRenderScale..1 of the view on each side): 8 at 1, more as
// each scene pixel covers more of the view's, so every view pixel still sees a sample near it (FSR 2's
// ffxFsr2GetJitterPhaseCount, 8 x the area ratio, rounded up).
std::uint32_t jitter_phases(float scale);

// Frame `frame`'s jitter in scene pixels, each axis in -0.5..0.5 (x right, y down): Halton (2, 3) from its second
// point on, repeating every `phases` frames. All geometry is drawn shifted by it.
glm::vec2 jitter_offset(std::uint64_t frame, std::uint32_t phases);

// That jitter as an offset in normalised device coordinates for a target of `width` x `height` pixels. The scene's
// viewport is flipped (vk_context.cpp begin_scene_rendering): NDC y is up, pixel y down.
glm::vec2 jitter_ndc(glm::vec2 jitter_px, std::uint32_t width, std::uint32_t height);

// `proj` with its output moved by `ndc` (an NDC offset): a perspective or orthographic projection alike.
glm::mat4 jittered(const glm::mat4& proj, glm::vec2 ndc);

// The matrix from this frame's clip space (unjittered, `view` and `proj`) to the last frame's: a point's NDC and
// depth in, its place a frame ago out (divide by w). Worked in double precision and through the cameras' relative
// motion, so world coordinates in the tens of thousands keep the sub-pixel precision the history needs.
glm::mat4 reprojection(const glm::mat4& prev_view, const glm::mat4& prev_proj, const glm::mat4& view,
                       const glm::mat4& proj);

// A jump of the camera between two frames that the history cannot follow (a teleport, a camera switch, a cut):
// it moved more than `max_step` world units, or turned more than `max_turn` radians. The step is past the chase
// camera's whole reach (display.max_camera_distance, at most 150): pushed in by a tree and out again, or zoomed with
// the wheel, it moves tens of units in a frame, and the motion follows that exactly. Cut there, the upscaler dropped
// its history and frame generation the frame, over and over in a forest (a flash of the raw picture and a hitch).
inline constexpr float kHistoryMaxStep = 200.0f;
inline constexpr float kHistoryMaxTurn = 0.5f;
bool camera_cut(const glm::mat4& prev_view, const glm::mat4& view, float max_step = kHistoryMaxStep,
                float max_turn = kHistoryMaxTurn);

} // namespace hailstone::render
