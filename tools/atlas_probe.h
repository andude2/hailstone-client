#pragma once

// playprobe's world atlas crawl (included by tools/playprobe.cpp; docs/graphics.md "World atlas"):
//   --atlas-server ID   writes a visit (app/world_atlas.h: the zone's id, whether it is outdoor, its zone points) for
//                       each zone entered into the atlas of the server folder ID (the client's servers/ID), and
//                       measures the zone's record from the asset folder when the atlas has none for these files
//   --atlas-data DIR    the client's data folder the atlas is written in (else HAILSTONE_DATA_DIR, else playprobe's
//                       own, beside it)
//   --atlas-crawl LIST  then zones to each of LIST in turn with #zone (a GM character on a test server), comma
//                       separated short names or @FILE (one a line, '#' comments), and camps after the last
// A zone #zone cannot reach within 60 seconds is skipped. Do not combine with --quest: both answer the server's
// zone requests.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "app/world_atlas.h"
#include "assets/zone.h"
#include "core/atomic_file.h"
#include "core/game_dir.h"
#include "game/world_atlas.h"
#include "game/zoning.h"
#include "net/game_connection.h"
#include "platform/paths.h"

namespace hailstone::probe {

class AtlasProbe {
public:
    using Clock = std::chrono::steady_clock;

    bool option(const char* name, const char* value) {
        if (!std::strcmp(name, "--atlas-server")) {
            server_ = value;
            return true;
        }
        if (!std::strcmp(name, "--atlas-data")) {
            platform::use_data_dir(value, "--atlas-data");
            return true;
        }
        if (std::strcmp(name, "--atlas-crawl") != 0) return false;
        crawl_.clear();
        std::string list = value;
        if (!list.empty() && list[0] == '@') {
            std::ifstream in(list.substr(1));
            if (!in) std::printf("  ATLAS: cannot read %s\n", list.c_str() + 1);
            std::string line;
            while (std::getline(in, line)) add(line);
        } else {
            std::size_t at = 0;
            while (at <= list.size()) {
                const std::size_t end = list.find(',', at);
                add(list.substr(at, end == std::string::npos ? std::string::npos : end - at));
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
        crawling_ = !crawl_.empty();
        return true;
    }

    bool active() const { return !server_.empty(); }
    bool busy() const { return active() && (pending_ || (crawling_ && next_ < crawl_.size()) || zoning_); }

    void zone_in(net::GameConnection& conn) {
        if (!active()) return;
        if (!handler_set_ && crawling_) {
            handler_set_ = true;
            // #zone's request: MoveToZone, as the client's DoTeleportB does for another zone.
            conn.set_teleport_handler([&conn](const game::TeleportBArgs& a) {
                if ((static_cast<std::uint32_t>(a.zone) & 0xffffu) == (conn.current_zone_dword() & 0xffffu)) return;
                game::MoveToZoneCall m;
                m.zone = a.zone;
                m.x = a.x;
                m.y = a.y;
                m.z = a.z;
                m.heading = static_cast<std::int32_t>(a.heading);
                conn.move_to_zone(m);
            });
        }
        pending_ = true;
        zoning_ = false;
        since_ = Clock::now();
    }

    void step(net::GameConnection& conn, const GameDir& game) {
        if (!active()) return;
        const auto now = Clock::now();
        if (zoning_) {
            if (now - since_ > std::chrono::seconds(60)) {
                std::printf("  ATLAS: %s not reached, skipped\n", crawl_[next_ - 1].c_str());
                ++skipped_;
                zoning_ = false;
            }
            return;
        }
        // The zone points come just after the zone: a few seconds in, the visit is written.
        if (pending_ && now - since_ > std::chrono::seconds(3)) {
            pending_ = false;
            record(conn, game);
        }
        if (pending_ || !crawling_ || next_ >= crawl_.size()) return;
        const std::string& to = crawl_[next_++];
        if (to == conn.world().zone_short_name) return;
        std::printf("  ATLAS: #zone %s (%zu of %zu)\n", to.c_str(), next_, crawl_.size());
        conn.say("#zone " + to);
        zoning_ = true;
        since_ = now;
    }

    void finish() const {
        if (!active()) return;
        std::printf("ATLAS SUMMARY: %d visits written, %d records measured, %d zones skipped\n", visits_, measured_,
                    skipped_);
    }

private:
    void add(std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        const std::size_t start = s.find_first_not_of(" \t");
        if (start == std::string::npos || s[start] == '#') return;
        crawl_.push_back(s.substr(start));
    }

    void record(net::GameConnection& conn, const GameDir& game) {
        game::AtlasVisit v;
        v.zone = conn.world().zone_short_name;
        v.zone_id = static_cast<std::uint32_t>(conn.zone_id());
        const auto& z = conn.zone_info();
        v.outdoor = z && z->sky != 0 && (z->time_type == 1 || z->time_type == 2 || z->time_type == 5);
        for (const auto& p : conn.zone_points()) v.points.push_back({p.number, p.zone_id, p.x, p.y, p.z});
        app::save_atlas_visit(server_, v);
        ++visits_;
        std::printf("  ATLAS: %s: %s, %zu zone points\n", v.zone.c_str(), v.outdoor ? "outdoor" : "indoor",
                    v.points.size());
        if (!v.outdoor || app::load_atlas_zone(game, v.zone)) return;
        const std::uint64_t key = app::atlas_zone_key(game, v.zone);
        if (key == 0) {
            std::printf("  ATLAS: %s not measured: its files are not in the asset folder\n", v.zone.c_str());
            return;
        }
        try {
            auto zone = assets::load_zone(game, v.zone);
            const auto t0 = Clock::now();
            const auto rec = app::measure_atlas_zone(zone, key);
            if (!rec.valid()) return;
            const auto bytes = game::write_atlas_zone(rec);
            write_file_atomic(app::atlas_zone_path(v.zone),
                              std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            ++measured_;
            std::printf("  ATLAS: %s measured, %dx%d squares, %.0f%% open sky, %zu line cells, %.0f ms\n",
                        v.zone.c_str(), rec.nx, rec.ny, rec.open_sky * 100.0f, rec.lines.size(),
                        std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        } catch (const std::exception& e) {
            std::printf("  ATLAS: %s not measured: %s\n", v.zone.c_str(), e.what());
        }
    }

    std::string server_;
    std::vector<std::string> crawl_;
    std::size_t next_ = 0;
    bool crawling_ = false, handler_set_ = false, pending_ = false, zoning_ = false;
    Clock::time_point since_{};
    int visits_ = 0, measured_ = 0, skipped_ = 0;
};

} // namespace hailstone::probe
