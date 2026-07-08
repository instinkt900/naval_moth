#include "game/terrain.h"

#include "game/camera.h"

#include <moth_graphics/graphics/igraphics.h>
#include <moth_ui/utils/transform.h>

#include <cmath>
#include <utility>

namespace naval {
    namespace {
        // --- world scale ---
        constexpr float kCellM = 6.0f;    // metres per marching-squares cell
        constexpr int kChunkCells = 12;   // cells per chunk edge
        constexpr float kChunkM = kCellM * static_cast<float>(kChunkCells);

        // --- noise shaping ---
        constexpr float kNoiseFreq = 0.012f; // world frequency (1/m)
        constexpr int kOctaves = 4;
        constexpr float kLacunarity = 2.0f;
        constexpr float kGain = 0.5f;
        constexpr float kSeaLevel = 0.15f; // fBm above this is land; higher = less land

        // Keep a clear harbour around the origin so a ship never spawns embedded.
        constexpr float kSpawnClearM = 45.0f;
        constexpr float kSpawnClearStrength = 1.5f;

        // --- streaming hysteresis (in chunks beyond the view rect) ---
        constexpr int kLoadMargin = 1; // generate this far past the view
        constexpr int kKeepMargin = 2; // unload only once this far past

        const moth_ui::Color kLand{ 0.36f, 0.40f, 0.28f, 1.0f };

        // Zero-crossing of the height field along the segment a->b, where va>0
        // and vb<=0 (or vice versa); the denominator is always non-zero because
        // the two ends straddle sea level.
        b2Vec2 Crossing(b2Vec2 a, float va, b2Vec2 b, float vb) {
            float const t = va / (va - vb);
            return { a.x + (t * (b.x - a.x)), a.y + (t * (b.y - a.y)) };
        }
    }

    Terrain::Terrain(b2World& world, uint32_t seed)
        : m_world(world)
        , m_noise(seed) {}

    float Terrain::Height(float worldX, float worldY) const {
        float h = m_noise.Fbm(worldX * kNoiseFreq, worldY * kNoiseFreq, kOctaves, kLacunarity, kGain) - kSeaLevel;
        float const d = std::sqrt((worldX * worldX) + (worldY * worldY));
        if (d < kSpawnClearM) {
            h -= kSpawnClearStrength * (1.0f - (d / kSpawnClearM));
        }
        return h;
    }

    void Terrain::Generate(int cx, int cy) {
        Chunk chunk;
        chunk.cx = cx;
        chunk.cy = cy;

        b2BodyDef bodyDef;
        bodyDef.type = b2_staticBody;
        chunk.body = m_world.CreateBody(&bodyDef); // world-space vertices; body at origin

        auto vec = [](b2Vec2 p) { return moth_ui::FloatVec2{ p.x, p.y }; };
        auto tri = [&](b2Vec2 a, b2Vec2 b, b2Vec2 c) {
            chunk.triangles.push_back(vec(a));
            chunk.triangles.push_back(vec(b));
            chunk.triangles.push_back(vec(c));
        };
        auto quad = [&](b2Vec2 a, b2Vec2 b, b2Vec2 c, b2Vec2 d) { tri(a, b, c); tri(a, c, d); };
        auto penta = [&](b2Vec2 a, b2Vec2 b, b2Vec2 c, b2Vec2 d, b2Vec2 e) {
            tri(a, b, c); tri(a, c, d); tri(a, d, e);
        };
        auto coast = [&](b2Vec2 a, b2Vec2 b) {
            b2EdgeShape edge;
            edge.SetTwoSided(a, b);
            chunk.body->CreateFixture(&edge, 0.0f);
        };

        // Marching squares over every cell in the chunk. Corners are sampled from
        // global cell indices, so a chunk shares its border samples with its
        // neighbours and coastlines line up seamlessly across chunk boundaries.
        for (int iy = 0; iy < kChunkCells; ++iy) {
            for (int ix = 0; ix < kChunkCells; ++ix) {
                int const gx = (cx * kChunkCells) + ix;
                int const gy = (cy * kChunkCells) + iy;
                float const x0 = static_cast<float>(gx) * kCellM;
                float const x1 = static_cast<float>(gx + 1) * kCellM;
                float const y0 = static_cast<float>(gy) * kCellM;
                float const y1 = static_cast<float>(gy + 1) * kCellM;

                b2Vec2 const c0{ x0, y0 };
                b2Vec2 const c1{ x1, y0 };
                b2Vec2 const c2{ x1, y1 };
                b2Vec2 const c3{ x0, y1 };
                float const v0 = Height(x0, y0);
                float const v1 = Height(x1, y0);
                float const v2 = Height(x1, y1);
                float const v3 = Height(x0, y1);

                int const code = (v0 > 0.0f ? 1 : 0) | (v1 > 0.0f ? 2 : 0) |
                                 (v2 > 0.0f ? 4 : 0) | (v3 > 0.0f ? 8 : 0);
                if (code == 0) {
                    continue; // all sea
                }

                // Interpolated crossings on the four edges (only the ones the
                // active case references are meaningful).
                b2Vec2 const e01 = Crossing(c0, v0, c1, v1);
                b2Vec2 const e12 = Crossing(c1, v1, c2, v2);
                b2Vec2 const e23 = Crossing(c2, v2, c3, v3);
                b2Vec2 const e30 = Crossing(c3, v3, c0, v0);

                switch (code) {
                case 15: quad(c0, c1, c2, c3); break;                          // all land
                case 1:  tri(c0, e01, e30);    coast(e01, e30); break;         // c0
                case 2:  tri(c1, e12, e01);    coast(e01, e12); break;         // c1
                case 4:  tri(c2, e23, e12);    coast(e12, e23); break;         // c2
                case 8:  tri(c3, e30, e23);    coast(e23, e30); break;         // c3
                case 3:  quad(c0, c1, e12, e30); coast(e12, e30); break;       // c0,c1
                case 6:  quad(c1, c2, e23, e01); coast(e01, e23); break;       // c1,c2
                case 12: quad(c2, c3, e30, e12); coast(e12, e30); break;       // c2,c3
                case 9:  quad(c3, c0, e01, e23); coast(e01, e23); break;       // c0,c3
                case 7:  penta(c0, c1, c2, e23, e30); coast(e23, e30); break;  // c0,c1,c2
                case 14: penta(c1, c2, c3, e30, e01); coast(e30, e01); break;  // c1,c2,c3
                case 13: penta(c2, c3, c0, e01, e12); coast(e01, e12); break;  // c0,c2,c3
                case 11: penta(c3, c0, c1, e12, e23); coast(e12, e23); break;  // c0,c1,c3
                case 5:  // saddle: c0 and c2 land, treated as two separate corners
                    tri(c0, e01, e30); coast(e01, e30);
                    tri(c2, e23, e12); coast(e12, e23);
                    break;
                case 10: // saddle: c1 and c3 land
                    tri(c1, e12, e01); coast(e01, e12);
                    tri(c3, e30, e23); coast(e23, e30);
                    break;
                default: break;
                }
            }
        }

        m_chunks.emplace(Key(cx, cy), std::move(chunk));
    }

    void Terrain::Update(Camera const& camera) {
        if (camera.pixelsPerMeter <= 0.0f) {
            return;
        }
        float const halfW = (camera.viewSize.x * 0.5f) / camera.pixelsPerMeter;
        float const halfH = (camera.viewSize.y * 0.5f) / camera.pixelsPerMeter;
        auto chunkOf = [](float world) { return static_cast<int>(std::floor(world / kChunkM)); };
        int const minCX = chunkOf(camera.center.x - halfW);
        int const maxCX = chunkOf(camera.center.x + halfW);
        int const minCY = chunkOf(camera.center.y - halfH);
        int const maxCY = chunkOf(camera.center.y + halfH);

        // Generate any chunk within the view plus the load margin.
        for (int cy = minCY - kLoadMargin; cy <= maxCY + kLoadMargin; ++cy) {
            for (int cx = minCX - kLoadMargin; cx <= maxCX + kLoadMargin; ++cx) {
                if (m_chunks.find(Key(cx, cy)) == m_chunks.end()) {
                    Generate(cx, cy);
                }
            }
        }

        // Unload chunks that have fallen beyond the (wider) keep margin.
        for (auto it = m_chunks.begin(); it != m_chunks.end();) {
            Chunk const& chunk = it->second;
            bool const keep = chunk.cx >= (minCX - kKeepMargin) && chunk.cx <= (maxCX + kKeepMargin) &&
                              chunk.cy >= (minCY - kKeepMargin) && chunk.cy <= (maxCY + kKeepMargin);
            if (keep) {
                ++it;
            } else {
                m_world.DestroyBody(chunk.body);
                it = m_chunks.erase(it);
            }
        }
    }

    void Terrain::Draw(moth_graphics::graphics::IGraphics& graphics, Camera const& camera) const {
        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        graphics.SetColor(kLand);
        std::vector<moth_ui::FloatVec2> screen;
        for (auto const& entry : m_chunks) {
            auto const& triangles = entry.second.triangles;
            if (triangles.empty()) {
                continue;
            }
            screen.clear();
            screen.reserve(triangles.size());
            for (auto const& world : triangles) {
                screen.push_back(camera.WorldToScreen(b2Vec2{ world.x, world.y }));
            }
            graphics.DrawTrianglesF(screen.data(), screen.size());
        }
    }
}
