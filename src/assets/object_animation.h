#pragma once

// A classic (WLD) placed object's own animation: a skeletal actor (0x14 -> 0x11 -> 0x10) whose node tracks (0x13 ->
// 0x12) have more than one key, such as Ssraeshza Temple's turning rings or City of Mist's swaying tree branches.
//
// The original plays these on its own (EQGraphicsDX9's hierarchical model definition builds a D3DX keyframed
// animation set for them); the rules below are the ones OpenEQ's docs/WLD_OBJECT_ANIMATION.md (MIT OR Apache-2.0)
// established, checked against RoF2's EQGraphicsDX9.dll where noted (docs/formats/wld.md, "Placed-object animation"):
// - A track's key i stands at i x interval ms. The interval is the 0x13 timing word when its flag 1 is set, else 100
//   (RoF2 0x1001b0bc, 0x1001b116). It is halved for more than 15 keys only when the archive was opened with a byte
//   the zone's object archives leave 0 (0x1001b26e; CDisplay::StartWorldDisplay's requests), so never here.
// - A track with more than one key gets a closing key, its first again, at keys x interval: the animation loops.
//   All the tracks of a definition are one set whose period is the first animated track's keys x interval.
// - Rotation: the two keys' quaternions blended component by component (signs chosen for continuity, the seam
//   included) and normalised; translation and scale blended linearly.
// - Every placement of a definition shares one controller at speed 1 from phase 0: they all show the same pose.
//   Its clock is milliseconds since the controller was made (here: since the zone was loaded).
//
// Positions are object space; the object's mesh (Zone::objects) is built in the first key's pose, so a node's motion
// is how far it has moved from there.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace hailstone::assets {

struct ObjectTrackKey {
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // as stored, signs made continuous (not normalised)
    glm::vec3 translation{0.0f};
    float scale = 1.0f;
};

struct ObjectAnimationNode {
    std::int32_t parent = -1;            // node index, -1 for a root
    std::vector<ObjectTrackKey> keys;    // one (a still node) or more; with more, keys.back() is the closing key
    std::uint32_t interval_ms = 100;     // between keys
    bool moves = false;                  // it or a node above it has more than one key
};

// A node's binding for a vertex that follows no moving node.
inline constexpr std::uint16_t kStillVertex = 0xffff;

struct ObjectAnimation {
    std::vector<ObjectAnimationNode> nodes; // skeleton order
    std::vector<glm::mat4> rest_inverse;    // the inverse of each node's first-key world matrix
    // One per vertex of the object's mesh: the node it follows, or kStillVertex when that node never moves (or the
    // vertex is not the skeleton's).
    std::vector<std::uint16_t> vertex_node;
    std::uint32_t period_ms = 0; // the set's period (> 0)

    // Each node's motion `ms` milliseconds into the animation: its world matrix then times the inverse of its first
    // key's, so a vertex of the mesh bound to node n is at out[n] * v. Still nodes give the identity.
    void pose(std::uint64_t ms, std::vector<glm::mat4>& out) const;
    // Whether any vertex moves (the loader keeps only such animations).
    bool any_moving_vertex() const;
};

// Whether triangle (a, b, c) of an animated object's mesh moves (any corner follows a moving node), and the node it goes
// with: its first moving corner's (a triangle across two moving nodes is carried whole by that one). kStillVertex when
// it does not move.
std::uint16_t moving_triangle_node(const std::vector<std::uint16_t>& vertex_node, std::uint32_t a, std::uint32_t b,
                                   std::uint32_t c);

// One track's local matrix (T * R * S) `ms` into a set of period `period_ms` (wrapped by it).
glm::mat4 sample_object_track(const ObjectAnimationNode& node, std::uint64_t ms, std::uint32_t period_ms);

} // namespace hailstone::assets
