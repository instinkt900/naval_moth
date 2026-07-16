#pragma once

#include "game/audio.h" // kNoSound
#include "game/hull_shape.h"

#include <box2d/box2d.h>
#include <entt/entt.hpp>
#include <moth_ui/utils/color.h>

#include <string>
#include <vector>

namespace naval {
    // The entity's Box2D body. The body's transform is the source of truth for
    // position and heading; other systems read it, never a duplicate.
    struct Physics {
        b2Body* body = nullptr;
    };

    // Engine characteristics — how hard a ship can drive and how it turns. Form
    // drag balances thrust at maxSpeed, so the hull has a definite top speed.
    // Turning is rudder-like: yaw authority follows a hump in forward speed —
    // near nil at a standstill, peaking below top speed, washing out at flank —
    // with turnRate as the peak. The hump's shape is shared handling character
    // living in the propulsion system; per hull only the peak turn rate and the
    // top speed differ, so different hulls still handle differently.
    struct Propulsion {
        float maxThrust = 0.0f;     // full-power forward force (newtons)
        float maxSpeed = 0.0f;      // top speed (m/s); drag balances thrust here
        float turnRate = 0.0f;      // peak yaw rate (radians / second), at the best turning speed
        float powerDistance = 0.0f; // distance (metres) beyond which throttle is full
        float rudderRate = 0.0f;    // how fast the rudder swings toward its command (1/second)
    };

    // The single commanded destination. Each click replaces it — targets are
    // never queued. Cleared on arrival so the ship coasts to a stop. While a
    // target is active the autopilot has the helm; clearing it hands control to
    // the manual Helm inputs below.
    struct MoveTarget {
        b2Vec2 point{ 0.0f, 0.0f }; // world space (metres)
        bool active = false;        // true while a destination is set; false = coast/idle
        float maxThrottle = 1.0f;   // cap on autopilot throttle; 1 = full power, <1 = cruise
    };

    // Direct manual control of a ship, an alternative to the waypoint autopilot.
    // Throttle is signed — negative drives astern. The rudder has a commanded
    // angle the helm turns toward and an actual angle that slews to it over time
    // at the hull's rudderRate, so "hard a-starboard" swings the rudder across
    // rather than snapping it. Both are normalised to [-1, 1]; +rudder turns to
    // starboard. Used only while MoveTarget is inactive.
    struct Helm {
        float throttle = 0.0f;  // commanded thrust, [-1, 1]; negative = astern
        float rudderCmd = 0.0f; // commanded rudder, [-1, 1]; + = starboard
        float rudder = 0.0f;    // actual rudder position, slews toward rudderCmd
    };

    // Idle patrol AI. A ship carrying this is handed a long random waypoint on
    // open water whenever it has no active MoveTarget, and runs to it in a
    // straight line through the same autopilot the player's clicks drive. Legs
    // are kilometres, so a ship holds a heading for minutes before it arrives and
    // picks the next one — a slow patrol, not a nervous one. The only other way a
    // leg ends is a periodic progress check: if the hull has made almost no way
    // since the last check (it has run up against a shore) it re-rolls, so a
    // stuck ship doesn't grind there. Placeholder behaviour until aggro ranges
    // and gunnery drive enemy movement.
    struct Wander {
        b2Vec2 lastPos{ 0.0f, 0.0f }; // ship position at the last progress check
        float sinceCheck = 0.0f;      // s accumulated toward the next progress check
    };

    // Aggro state for an enemy AI. While `target` is null the ship patrols under
    // the Wander/autopilot; once a foe comes inside aggro range it locks on here,
    // breaks off, and the aggro system takes the helm to manoeuvre a weapon onto
    // it. `weaponIndex` is the armament slot of the arc it is working to present,
    // remembered between ticks so the ship commits to one battery rather than
    // flipping between port and starboard as the target drifts across the bow.
    struct Aggro {
        entt::entity target = entt::null; // the contact being engaged, or null while patrolling
        int weaponIndex = -1;             // armament index of the arc being presented, or -1
    };

    // How to draw the hull. The long axis runs bow-to-stern along local +x
    // (the body's forward direction).
    struct Renderable {
        moth_ui::Color color;     // hull fill colour
        float halfLengthM = 0.0f; // bow-stern half extent, metres
        float halfBeamM = 0.0f;   // port-starboard half extent, metres
        float foreShoulder = kHullShoulder;         // fore shoulder position, factor of the half-length
        float foreShoulderBeam = kHullShoulderBeam; // beam at the fore shoulder, factor of the half-beam
        float aftShoulder = kHullShoulder;          // aft shoulder position, factor of the half-length
        float aftShoulderBeam = kHullShoulderBeam;  // beam at the aft shoulder, factor of the half-beam
    };

    // How long a wake mark lingers before it has fully faded. Shared by the wake
    // system (which expires marks past this age) and the renderer (which fades
    // each mark over it), so the two never disagree.
    inline constexpr float kWakeLifetimeS = 40.0f;

    // A fading wake trailing a moving hull. Marks are dropped at the ship's
    // centre as it makes way, spaced by distance so the trail stays even at any
    // speed, and fade out over kWakeLifetimeS. Purely cosmetic: the wake system
    // maintains it, the renderer draws it.
    struct Wake {
        struct Mark {
            b2Vec2 position{ 0.0f, 0.0f }; // world point (m) where the mark was dropped
            float age = 0.0f;              // seconds since it was dropped
        };
        std::vector<Mark> marks;         // oldest first
        b2Vec2 lastDrop{ 0.0f, 0.0f };   // centre position of the last mark, for distance spacing
        bool seeded = false;             // true once lastDrop holds a real position
    };

    // A single weapon mounted on a ship. Static fields are resolved from the
    // database at spawn; the runtime fields update as it engages. bearing/arc
    // are relative to the bow — the arc's world centre is bodyAngle + bearing.
    // mountOffset places the weapon on the hull in local metres (+x toward the
    // bow, +y toward starboard); it is the origin for aiming, firing, and the
    // drawn arc.
    struct Weapon {
        std::string name;                   // weapon def id, for display
        float bearing = 0.0f;               // rad, mount direction relative to bow
        b2Vec2 mountOffset{ 0.0f, 0.0f };   // hull-local mount position (m)
        float arcHalfAngle = 0.0f;          // rad, half-width of the firing arc
        float range = 0.0f;                 // m, engagement range; also how far its shots travel
        float spread = 0.0f;                // rad, half-angle of the spread disc over the target (radius = range * tan)
        float cooldown = 0.0f;              // s between shots

        // Shot stats, copied from the database so firing needs no lookup. Speed
        // and damage are the weapon's; the draw radius, colour and what it does
        // on arrival come from its projectile. The projectile's arrival effects
        // are stamped onto each shot as it is fired and travel with it, so they
        // are held here only to be handed over.
        float muzzleVelocity = 0.0f;    // m/s the shot leaves the barrel at
        float damage = 0.0f;            // hit points removed on impact
        float projectileRadiusM = 0.0f; // shot draw radius, metres
        moth_ui::Color projectileColor; // shot draw colour
        int projectileImpactSound = kNoSound; // heard where a shot strikes a hull
        int projectileSplashSound = kNoSound; // heard where a shot falls in the sea
        float projectileImpactShakeM = 0.0f;  // camera shake where a shot strikes a hull (m)

        // What the gun itself does as it fires, felt and heard at the mount.
        // The sound handle is resolved at spawn for the same reason as the stats
        // above: firing is a hot path and shouldn't be hashing strings. kNoSound
        // fires silently, and a shake of 0 fires without moving the camera.
        int fireSound = kNoSound;
        float fireShakeM = 0.0f;

        float cooldownRemaining = 0.0f; // s until it can fire again
        bool hasTarget = false;         // the ship's designated contact bears this tick

        // Player-facing controls, one set per weapon.
        bool showArc = true;     // draw this weapon's firing arc
        bool showSpread = false; // draw a debug preview of the spread disc over the current target

        // The ship's designated contact while this weapon bears on it, else
        // entt::null — a weapon does not choose a target, it only answers
        // whether the one the ship named is inside its arc and range (see
        // FireOrder). Refreshed every update; read it only after registry.valid,
        // as the target may be destroyed between updates.
        entt::entity target = entt::null;

        // The current world aim point (the target's centre, led for its motion)
        // and the radius of the spread disc around it — refreshed each tick a
        // target is held. Firing samples a random point in this disc; the debug
        // draw previews it. Meaningful only while `target` is valid.
        b2Vec2 aimWorld{ 0.0f, 0.0f };
        float spreadRadiusM = 0.0f;
    };

    // Every weapon a ship carries. A vector (rather than one component per
    // weapon) because an entity holds at most one component of a given type.
    struct Armament {
        std::vector<Weapon> weapons;
    };

    // A ship's engagement order: the one contact it is fighting, and whether it
    // is actually shooting at it. Designating and firing are deliberately
    // separate — you pick a contact, read what it is, and only then commit — so
    // an order with `firing` false is a ship tracking its mark with the guns
    // silent, arcs lit and aim solutions live.
    //
    // The order lives on the ship rather than on each weapon because that is the
    // decision being made: "engage that one", not "gun three, engage that one".
    // Every weapon then answers only whether the contact is inside its own arc
    // and range. A battery that cannot bear simply holds, and starts firing the
    // moment the target drifts into its arc, with no further order — which is
    // why the order carries a contact and not a list of guns.
    //
    // The weapons system clears the whole order once the target is dead or gone
    // from the registry; that is what makes "fire until it is dead" terminate,
    // and it is why nothing else needs to watch for a target's death.
    //
    // The player's order comes from clicking a contact and the Target window's
    // Fire/Hold. An enemy's is issued by the aggro system, which fires as soon
    // as it has locked something. One component either way, so both sides shoot
    // down the same path rather than the AI having a private one.
    struct FireOrder {
        entt::entity target = entt::null; // the designated contact, or null for none
        bool firing = false;              // true = shoot it whenever a gun bears
    };

    // Which side a ship fights for. Weapons engage hulls of a different
    // faction; a projectile strikes only hulls of the faction it was fired at.
    enum class Faction {
        Player,
        Enemy,
    };

    // The side an armed entity belongs to. Every ship carries one so weapons
    // can tell friend from foe — any hull may fire on any hull not its own.
    struct Combatant {
        Faction faction = Faction::Player;
    };

    // A human-readable class name (the hull id), for labelling a contact in the
    // controls readout.
    struct Identity {
        std::string name;
    };

    // The sounds a hull itself makes, as opposed to the ones its guns and their
    // shots make (those live on Weapon and Projectile). Handles are resolved
    // from the hull definition at spawn; kNoSound is silent.
    //
    // It is a component rather than a field on Health because the hull is what
    // owns these sounds, not its destructibility — the death of a ship is simply
    // the first of them to be needed, and an engine rumble or a damage groan
    // would belong here beside it.
    struct Sounds {
        int explosion = kNoSound; // played once as the hull is destroyed
    };

    // What the hull itself does to the camera, as opposed to what its guns and
    // their shots do (those live on Weapon and Projectile). Amplitudes are
    // metres of shake at full effect (see camera_shake.h); 0 is no shake.
    //
    // Its own component rather than a field on Sounds beside the explosion it
    // fires with, because the two are the same *moment* and not the same thing —
    // one is a noise and one is a blow, they are authored separately, and a hull
    // may well want one without the other. Kept apart, each stays a coherent
    // answer to "what does this hull sound like" and "what does this hull do to
    // the picture".
    struct Shake {
        float explosionM = 0.0f; // felt once as the hull is destroyed
    };

    // Hit points. A hull loses `current` when a projectile strikes it and is
    // removed once it reaches zero. Only ships that can be destroyed carry this.
    struct Health {
        float current = 0.0f;
        float max = 0.0f;
    };

    // A destroyed hull's death sequence, shared by the sinking system (which
    // ages the wreck and finally removes it) and the renderer (which greys the
    // hull, then fades it). The wreck burns charred grey for kSinkBurnS, then
    // alpha-fades under the sea over kSinkDurationS; total life is their sum.
    inline constexpr float kSinkBurnS = 5.0f;      // grey/burning before it starts to go down
    inline constexpr float kSinkDurationS = 10.0f; // fade-out once it begins sinking

    // Marks a hull that has been destroyed and is playing out its death
    // sequence. It is no longer a combatant and carries no armament; the sinking
    // system ages it and removes it once it has fully gone under.
    struct Sinking {
        float age = 0.0f; // seconds since it was destroyed
    };

    // A projectile in flight. Straight-shot: constant velocity, detonates once it
    // has travelled to its fuzed range — set when fired to the target's distance
    // (plus a little random spread) so a miss splashes near the target rather
    // than flying on to the weapon's max range. Kept out of Box2D — it has no
    // collision yet.
    struct Projectile {
        b2Vec2 position{ 0.0f, 0.0f }; // world space (metres)
        b2Vec2 velocity{ 0.0f, 0.0f }; // m/s
        float remaining = 0.0f;        // m of travel left before the fuze detonates it
        float radiusM = 0.0f;          // draw radius, metres
        float damage = 0.0f;           // hit points removed from the hull it strikes
        moth_ui::Color color;          // draw colour
        Faction target = Faction::Enemy; // the faction this shot may strike

        // What the shot does when it arrives, carried on the shot itself because
        // the weapon that fired it may be gone — sunk — by the time it lands.
        // Exactly one of the sounds plays: a shot either strikes a hull or falls
        // in the sea. kNoSound is silent.
        int impactSound = kNoSound;
        int splashSound = kNoSound;

        // Metres of camera shake at full effect where it strikes a hull; 0 = no
        // shake. There is no splash equivalent on purpose: a shot that falls in
        // the sea has hit nothing, and shaking the camera for it would tell the
        // player a near miss landed when it did not.
        float impactShakeM = 0.0f;
    };

    // How long a splash lingers before it has fully faded. Shared by the splash
    // system (which removes splashes past this age) and the renderer (which
    // expands and fades each over it), so the two never disagree.
    inline constexpr float kSplashLifetimeS = 0.6f;

    // A brief splash where a spent shot fell into the sea — spawned when a
    // projectile travels its full range without striking a hull. It expands and
    // fades over kSplashLifetimeS, then is removed. Its own entity, not attached
    // to anything, since the projectile that spawned it is already gone. Purely
    // cosmetic: the splash system ages it, the renderer draws it.
    struct Splash {
        b2Vec2 position{ 0.0f, 0.0f }; // world point (m) where the shot fell
        float age = 0.0f;              // seconds since it appeared
        float radiusM = 0.0f;          // the shot's radius; the splash grows from it
    };
}
