#pragma once

#include <box2d/b2_math.h>
#include <entt/entt.hpp>

namespace naval {
    class Audio;
    class CameraShake;
    class Terrain;
    enum class Faction;

    // Aiming and firing, driven by each ship's FireOrder. A weapon acquires
    // nothing on its own: it tracks the contact its ship has designated whenever
    // that contact is inside its arc and range, and shoots it only while the
    // order says to fire and the gun is off cooldown. So a gun the target drifts
    // into range of joins in unprompted, and one that cannot bear holds. Free
    // fire is the sole exception, and the only thing here that lets a gun pick
    // its own mark: it engages the nearest foe it can bear on, though never
    // before the designated contact if that one is also in reach.
    //
    // This is also where an order ends: a designated contact that has died or
    // left the registry clears the order outright, which is what stops a ship
    // firing at a wreck. A salvo ends here too — it is consumed and cleared on
    // the first update after it is ordered, so it costs the battery one round
    // rather than becoming a state anything has to turn back off. Also advances
    // cooldowns and records which weapons bear (for rendering). Each shot is
    // heard, and felt, at its gun's mount. `dt` is the tick length in seconds.
    void UpdateWeapons(entt::registry& registry, Audio& audio, CameraShake& shake, float dt);

    // The hull of `faction` under a world point, or entt::null if none is there
    // — how a click designates a target. `pickRadiusM` fattens the point into a
    // disc so a distant contact stays clickable when zoomed out; the caller sets
    // it from the zoom, since what is a comfortable click is a matter of pixels
    // and only the camera knows the scale. Ties go to the nearest hull centre.
    entt::entity ContactAt(entt::registry& registry, b2Vec2 point, Faction faction, float pickRadiusM);

    // Where `shooter` believes `target` is, from its own knowledge — what the guns
    // lay on, and what the Target readout reports, rather than the hull's truth. A
    // ship with no picture is omniscient (the true hull); a ship with one lays on
    // what it holds: a radar or visual fix is the true hull (tracked accurately), a
    // passive contact only its TMA estimate (so a shot on a soft solution misses), a
    // stale fix its frozen last-known position, and a bare bearing or unheld contact
    // no aim at all (ok false). `estimate` marks a point solution — bear and lay on
    // the point rather than a known silhouette.
    struct AimBelief {
        bool ok = false;
        b2Vec2 pos{ 0.0f, 0.0f };
        b2Vec2 vel{ 0.0f, 0.0f };
        bool estimate = false;
    };
    AimBelief KnownAim(entt::registry& registry, entt::entity shooter, entt::entity target);

    // Advances projectiles (ballistic shells and guided munitions) and destroys
    // those that have run their course. A shot that expires without striking a
    // hull leaves a splash where it fell — except a waterborne munition (a
    // torpedo), which makes no surface splash and also beaches quietly if it
    // swims onto land, which `terrain` is passed in to detect. Shots are heard
    // where they arrive — an impact on the hull they strike, a splash where they
    // fall — and a hull destroyed by one is heard exploding. An impact and an
    // explosion also knock the camera; a splash does not, having hit nothing.
    void UpdateProjectiles(entt::registry& registry, Audio& audio, CameraShake& shake,
                           Terrain const& terrain, float dt);

    // Ages the splashes left by spent shots and removes those fully faded.
    void UpdateSplashes(entt::registry& registry, float dt);

    // Ages each destroyed hull's death sequence and removes the wreck once it
    // has fully sunk. Hulls enter this state when their health reaches zero.
    void UpdateSinking(entt::registry& registry, float dt);
}
