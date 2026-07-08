#pragma once

#include "game/noise.h"

#include <box2d/box2d.h>
#include <moth_ui/utils/vector.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace moth_graphics::graphics {
    class IGraphics;
}

namespace naval {
    struct Camera;

    // Procedural land. The world is divided into fixed square chunks; each is
    // generated on demand from Perlin noise as the camera reveals it and torn
    // down once it falls well outside view, so the sea is effectively endless
    // while only the visible neighbourhood is ever resident.
    //
    // Generation runs marching squares over the noise field: cells whose corners
    // straddle the sea-level threshold become coastline. Each chunk caches the
    // land interior as triangles (for drawing) and installs the coastline as
    // static Box2D edges (for collision), so ships simply cannot cross a shore.
    class Terrain {
    public:
        Terrain(b2World& world, uint32_t seed);

        // Stream chunks in/out to cover the camera's view (plus a margin).
        void Update(Camera const& camera);

        // Draw all resident land, mapping world metres to screen via the camera.
        void Draw(moth_graphics::graphics::IGraphics& graphics, Camera const& camera) const;

    private:
        // One resident chunk: its grid coords, its static collision body, plus
        // cached land triangles in world metres (three vertices per triangle).
        struct Chunk {
            int cx = 0;
            int cy = 0;
            b2Body* body = nullptr;
            std::vector<moth_ui::FloatVec2> triangles;
        };

        // Continuous terrain height at a world point; land is height > 0.
        float Height(float worldX, float worldY) const;
        void Generate(int cx, int cy);

        static int64_t Key(int cx, int cy) {
            return (static_cast<int64_t>(cx) << 32) | static_cast<uint32_t>(cy);
        }

        b2World& m_world;
        Noise m_noise;
        std::unordered_map<int64_t, Chunk> m_chunks;
    };
}
