#include "game/terrain.h"

#include "game/camera.h"

#include <moth_graphics/graphics/igraphics.h>
#include <moth_ui/utils/transform.h>

#include <cmath>
#include <utility>

namespace naval {
    namespace {
        // --- noise shaping ---
        constexpr float kNoiseFreq = 0.00006f; // world frequency (1/m); lower = larger landmasses
        constexpr int kOctaves = 4;
        constexpr float kLacunarity = 2.0f;
        constexpr float kGain = 0.5f;
        constexpr float kSeaLevel = 0.15f;      // fBm above this is land; higher = less land
        constexpr float kShallowLevel = -0.18f; // height down to here is shallow water (lower = wider rim)

        // --- world scale ---
        // The marching-squares cell size is derived from the noise scale rather
        // than fixed in metres: a cell spans a constant fraction of the dominant
        // landmass wavelength (~1/kNoiseFreq), so coastline fidelity — and hence
        // the triangle and collision-edge count per landmass — stays constant when
        // the world is re-scaled by changing kNoiseFreq. A cell size fixed in
        // metres instead explodes the cell count as the world grows, which is what
        // tanks performance at low zoom. ~20 cells per wavelength reproduces the
        // fidelity the small-scale world shipped with.
        constexpr float kCellsPerWavelength = 20.0f;
        constexpr float kCellM = 1.0f / (kNoiseFreq * kCellsPerWavelength); // ~167 m
        constexpr int kChunkCells = 12;   // cells per chunk edge
        constexpr float kChunkM = kCellM * static_cast<float>(kChunkCells);

        // Keep a clear harbour around the origin so a ship never spawns embedded.
        constexpr float kSpawnClearM = 45.0f;
        constexpr float kSpawnClearStrength = 1.5f;

        // --- streaming hysteresis (in chunks beyond the view rect) ---
        constexpr int kLoadMargin = 1; // generate this far past the view
        constexpr int kKeepMargin = 2; // unload only once this far past

        const moth_ui::Color kSea{ 0.10f, 0.20f, 0.32f, 1.0f };
        const moth_ui::Color kShallow{ 0.20f, 0.30f, 0.42f, 1.0f };
        const moth_ui::Color kLand{ 0.36f, 0.40f, 0.28f, 1.0f };

        // Point where the height field crosses @p level along the segment a->b,
        // whose ends straddle it; the denominator is non-zero because one end is
        // above the level and the other at or below it.
        b2Vec2 Crossing(b2Vec2 a, float va, b2Vec2 b, float vb, float level) {
            float const t = (va - level) / (va - vb);
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

    bool Terrain::IsWater(b2Vec2 point, float clearanceM) const {
        // Land is height > 0; the shore is the height == 0 contour where the
        // collision edges live. Rejecting any positive height within the ring
        // keeps a spawned hull off the coastline in every direction.
        if (Height(point.x, point.y) > 0.0f) {
            return false;
        }
        constexpr int kSamples = 8;
        for (int i = 0; i < kSamples; ++i) {
            float const angle = (2.0f * b2_pi) * static_cast<float>(i) / static_cast<float>(kSamples);
            if (Height(point.x + (clearanceM * std::cos(angle)),
                       point.y + (clearanceM * std::sin(angle))) > 0.0f) {
                return false;
            }
        }
        return true;
    }

    void Terrain::March(int cx, int cy, float level,
                        std::vector<moth_ui::FloatVec2>& out, b2Body* edgeBody) const {
        auto vec = [](b2Vec2 p) { return moth_ui::FloatVec2{ p.x, p.y }; };
        auto tri = [&](b2Vec2 a, b2Vec2 b, b2Vec2 c) {
            out.push_back(vec(a));
            out.push_back(vec(b));
            out.push_back(vec(c));
        };
        auto quad = [&](b2Vec2 a, b2Vec2 b, b2Vec2 c, b2Vec2 d) { tri(a, b, c); tri(a, c, d); };
        auto penta = [&](b2Vec2 a, b2Vec2 b, b2Vec2 c, b2Vec2 d, b2Vec2 e) {
            tri(a, b, c); tri(a, c, d); tri(a, d, e);
        };
        auto coast = [&](b2Vec2 a, b2Vec2 b) {
            if (edgeBody == nullptr) {
                return;
            }
            b2EdgeShape edge;
            edge.SetTwoSided(a, b);
            edgeBody->CreateFixture(&edge, 0.0f);
        };

        // Marching squares over every cell in the chunk. Corners are sampled from
        // global cell indices, so a chunk shares its border samples with its
        // neighbours and contours line up seamlessly across chunk boundaries.
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

                int const code = (v0 > level ? 1 : 0) | (v1 > level ? 2 : 0) |
                                 (v2 > level ? 4 : 0) | (v3 > level ? 8 : 0);
                if (code == 0) {
                    continue; // wholly below the level
                }

                // Interpolated crossings on the four edges (only the ones the
                // active case references are meaningful).
                b2Vec2 const e01 = Crossing(c0, v0, c1, v1, level);
                b2Vec2 const e12 = Crossing(c1, v1, c2, v2, level);
                b2Vec2 const e23 = Crossing(c2, v2, c3, v3, level);
                b2Vec2 const e30 = Crossing(c3, v3, c0, v0, level);

                switch (code) {
                case 15: quad(c0, c1, c2, c3); break;                          // all in
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
                case 5:  // saddle: c0 and c2 in, treated as two separate corners
                    tri(c0, e01, e30); coast(e01, e30);
                    tri(c2, e23, e12); coast(e12, e23);
                    break;
                case 10: // saddle: c1 and c3 in
                    tri(c1, e12, e01); coast(e01, e12);
                    tri(c3, e30, e23); coast(e23, e30);
                    break;
                default: break;
                }
            }
        }
    }

    void Terrain::Generate(int cx, int cy) {
        Chunk chunk;
        chunk.cx = cx;
        chunk.cy = cy;

        b2BodyDef bodyDef;
        bodyDef.type = b2_staticBody;
        chunk.body = m_world.CreateBody(&bodyDef); // world-space vertices; body at origin

        // Shallow rim first (fill only, no collision), then the land on top of it
        // (fill plus the coastline collision edges). Drawing land over the wider
        // shallow region leaves just a variable-width coastal band showing.
        March(cx, cy, kShallowLevel, chunk.shallowTriangles, nullptr);
        March(cx, cy, 0.0f, chunk.triangles, chunk.body);

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

        // The open sea: clear the whole view to the deep-water colour first, so
        // shallow rims and land draw over it.
        graphics.SetColor(kSea);
        graphics.Clear();

        std::vector<moth_ui::FloatVec2> screen;
        auto drawTriangles = [&](std::vector<moth_ui::FloatVec2> const& triangles) {
            if (triangles.empty()) {
                return;
            }
            screen.clear();
            screen.reserve(triangles.size());
            for (auto const& world : triangles) {
                screen.push_back(camera.WorldToScreen(b2Vec2{ world.x, world.y }));
            }
            graphics.DrawTrianglesF(screen.data(), screen.size());
        };

        // Shallow-water rims under all land, then the land itself, so a chunk's
        // land never lets an adjacent chunk's rim show through.
        graphics.SetColor(kShallow);
        for (auto const& entry : m_chunks) {
            drawTriangles(entry.second.shallowTriangles);
        }
        graphics.SetColor(kLand);
        for (auto const& entry : m_chunks) {
            drawTriangles(entry.second.triangles);
        }
    }
}
