#pragma once

#include "game/audio.h" // kNoSound
#include "game/hull_shape.h"

#include <box2d/box2d.h>
#include <entt/entt.hpp>
#include <moth_ui/utils/color.h>

#include <string>
#include <unordered_map>
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

    // What a mounted weapon is, which decides how it aims and what it fires. Guns
    // and launchers are one component and one Armament so the targeting, fire
    // order, aggro, and UI systems treat every mount uniformly (they read range,
    // arc, bearing and cooldown, all populated at spawn whichever the kind); only
    // aiming and the shot itself branch on this.
    enum class WeaponKind {
        Gun,      // trains a barrel within its arc and fires a ballistic shell along it
        VLS,      // vertical launch: 360 arc, no training; releases a guided munition at rest
        Launcher, // trainable rail: trains within its arc, then launches a guided munition along it
    };

    // A single weapon mounted on a ship. Static fields are resolved from the
    // database at spawn; the runtime fields update as it engages. bearing/arc
    // are relative to the bow — the arc's world centre is bodyAngle + bearing.
    // mountOffset places the weapon on the hull in local metres (+x toward the
    // bow, +y toward starboard); it is the origin for aiming, firing, and the
    // drawn arc.
    struct Weapon {
        WeaponKind kind = WeaponKind::Gun;  // gun vs launcher; branches aiming and firing only

        // A point-defence mount (a CIWS) answers inbound guided air munitions
        // rather than ships, and stands right outside the ship's fire order: it
        // is never handed the designated contact, never salvos, never joins free
        // fire. Its one control is the enable tick below. While enabled it lays
        // on the nearest incoming missile in its arc and knocks it down. Always a
        // Gun kind — a launcher is not a point-defence weapon — and handled on its
        // own branch in the weapons system, so none of the anti-ship targeting
        // touches it. Set from the gun definition at spawn.
        bool pointDefense = false;

        std::string name;                   // weapon display name (the def's name, or its id), for the readout
        std::string munitionName;           // launcher only: the loaded munition's name, for the readout; empty for a gun
        float bearing = 0.0f;               // rad, mount direction relative to bow
        b2Vec2 mountOffset{ 0.0f, 0.0f };   // hull-local mount position (m)
        float arcHalfAngle = 0.0f;          // rad, half-width of the firing arc
        float range = 0.0f;                 // m, engagement range; also how far its shots travel
        float spread = 0.0f;                // rad, half-angle of the spread disc over the target (radius = range * tan)
        float cooldown = 0.0f;              // s between shots
        float turnRate = 0.0f;              // rad/s the barrel trains at within its arc; <= 0 trains instantly
        int barrelCount = 1;                // gun only: barrels fired per trigger, each its own projectile; 1 is a single-barrel gun
        float barrelSeparationM = 0.0f;     // gun only: metres between adjacent barrels, abreast and centred on the mount

        // Shot stats, copied from the database so firing needs no lookup. Speed
        // and damage are the weapon's; the draw radius, colour and what it does
        // on arrival come from its projectile. The projectile's arrival effects
        // are stamped onto each shot as it is fired and travel with it, so they
        // are held here only to be handed over.
        float muzzleVelocity = 0.0f;    // m/s the shot leaves the barrel at (gun only)
        float damage = 0.0f;            // hit points removed on impact
        float projectileRadiusM = 0.0f; // shot draw radius, metres
        moth_ui::Color projectileColor; // shot draw colour
        int projectileImpactSound = kNoSound; // heard where a shot strikes a hull
        int projectileSplashSound = kNoSound; // heard where a shot falls in the sea
        float projectileImpactShakeM = 0.0f;  // camera shake where a shot strikes a hull (m)

        // Munition propulsion, cached from the loaded munition at spawn so a launch
        // needs no lookup — the same trade the projectile stats above make. All
        // zero for a gun; for a launcher/VLS they seed the guided shot's flight.
        // The shot's reach and damage are already carried by range/damage above,
        // populated from the munition for a launcher.
        float munitionMaxSpeed = 0.0f;     // m/s the munition accelerates up to
        float munitionAcceleration = 0.0f; // m/s^2 gained in flight
        float munitionTurnRate = 0.0f;     // rad/s its heading steers toward the target
        float munitionMinRange = 0.0f;     // m the munition must travel to arm; also the drawn dead-zone radius
        float munitionInitialSpeed = 0.0f; // m/s off the rail (Launcher kind); a VLS launches at rest
        bool munitionWaterborne = false;   // true for a torpedo (water medium); it swims and makes no surface splash
        float munitionHealth = 0.0f;       // warhead health stamped on each launched munition, for point defence to whittle down; 0 = the munition cannot be shot down
        // The guided munition's drawn rectangle, cached from the munition; both
        // zero for a gun, whose ballistic shot draws as a circle of
        // projectileRadiusM instead. Cosmetic only — the shot's collision, point
        // defence and splash size all read projectileRadiusM.
        float munitionDrawLengthM = 0.0f;  // m, along travel
        float munitionDrawWidthM = 0.0f;   // m, across the beam

        // Launcher tubes. A launcher fires from a pool of ready tubes rather than
        // on a single cooldown: up to readyTubes launch in quick succession, each
        // spaced by launchInterval, then spent tubes reload one at a time over
        // reloadTime (reloadTimer is the one currently reloading; 0 = none). All
        // zero for a gun, which uses cooldown above instead.
        int tubeCount = 0;           // total tubes the launcher holds
        int readyTubes = 0;          // tubes currently loaded and ready to fire
        float launchInterval = 0.0f; // s enforced between successive launches
        float launchTimer = 0.0f;    // s until the next launch is allowed
        float reloadTime = 0.0f;     // s to reload one spent tube
        float reloadTimer = 0.0f;    // s left on the tube reloading now; 0 = none in progress

        // Rounds still queued from the last salvo, drained one per shot as the
        // weapon ripples them out: for a gun, shots spaced by its cooldown; for a
        // launcher, tubes spaced by launchInterval. Loaded from the unit's
        // FireChannel::salvoSize when a Salvo is ordered (a launcher capping it at
        // its ready tubes).
        int pending = 0;

        // What the gun itself does as it fires, felt and heard at the mount.
        // The sound handle is resolved at spawn for the same reason as the stats
        // above: firing is a hot path and shouldn't be hashing strings. kNoSound
        // fires silently, and a shake of 0 fires without moving the camera.
        int fireSound = kNoSound;
        // Whether fireSound is a looping sample (a minigun's whirr), resolved at
        // spawn alongside the handle so the fire path branches on a bool rather
        // than querying the bank. A looping fire sound is held for as long as the
        // mount bears its mark instead of played once per round (see the
        // point-defence branch of combat_system and Audio::HoldLoop).
        bool fireSoundLoops = false;
        float fireShakeM = 0.0f;

        float cooldownRemaining = 0.0f; // s until it can fire again
        bool hasTarget = false;         // the ship's designated contact bears this tick

        // The fire-control channel (fire unit) this battery mount belongs to: the
        // id of a FireChannel on the ship's FireControl. Every battery mount is
        // always on one — its own lone channel, or a group it shares — so the mount
        // always has an order to answer, and firing is just that channel's `firing`
        // (nothing fires until the player commits a unit). Moving a mount between
        // units (its own → a group, or back) only rewrites this id; the Target
        // window keeps it pointing at a live channel. -1 means "on no channel",
        // which a point-defence mount carries (it ignores this and answers to
        // `enabled` below), and which a battery mount never rests at.
        int channel = -1;

        // Whether a *point-defence* mount is switched on. A point-defence control
        // only now: a CIWS stands outside the group model (it answers inbound
        // missiles on its own), so the enable tick is its whole say. A battery
        // mount ignores this — its participation is `channel` above. Point defence
        // spawns enabled (a hands-off shield), which is why the default stays on.
        bool enabled = true;

        // Player-facing spread-preview toggle, a per-mount debug draw. The firing
        // arc is a separate toggle and lives on the fire unit, not the mount
        // (FireChannel::showArc), so a whole group's arcs switch together; a
        // point-defence arc instead draws whenever the mount is enabled.
        bool showSpread = false; // draw a debug preview of the spread disc over the current target

        // The contact this weapon is laid on, or entt::null if it can reach
        // none — normally its group's designated contact, and under free fire
        // whatever else the gun found when that contact was out of its arc (see
        // FireChannel). A point-defence mount instead lays this on the inbound
        // munition entity it is engaging, which is why the field is a bare entity
        // and not assumed to carry a hull's components — read it only after
        // registry.valid. Distinct from hasTarget above, which stays a strict answer
        // about the *designated* contact: the two agree except under free fire,
        // where a gun may be laid on something the ship never named. That is why
        // the target ring and the "guns bear" count read hasTarget — they are
        // reporting on the designated contact — while the gun's own status reads
        // this. Refreshed every update; read it only after registry.valid, as
        // the target may be destroyed between updates.
        entt::entity target = entt::null;

        // The current world aim point (the target's centre, led for its motion)
        // and the radius of the spread disc around it — refreshed each tick a
        // target is held. Firing samples a random point in this disc; the debug
        // draw previews it. Meaningful only while `target` is valid.
        b2Vec2 aimWorld{ 0.0f, 0.0f };
        float spreadRadiusM = 0.0f;

        // The barrel's current lay, as a bearing relative to the bow — so it
        // rides the hull's heading and stays inside the arc as the ship turns
        // (world bearing is bodyAngle + aimBearing). It trains toward the target
        // at turnRate each tick, clamped to the arc, and simply holds on any
        // tick with no target: a persistent aim, not one that recentres. The
        // shot leaves along it, so a gun still slewing onto a mark throws wide of
        // the lead until it catches up. Seeded to the mount bearing at spawn, so
        // the gun starts centred in its arc.
        float aimBearing = 0.0f;

        // True once the barrel has trained onto the aim solution — within about
        // a degree of it — rather than still slewing toward it. Refreshed each
        // tick a target is held and cleared when there is none. Drives the
        // Target window's per-gun readout only; the shot never waits on it (a
        // gun still slewing fires along the barrel and simply throws wide).
        bool acquired = false;
    };

    // Every weapon a ship carries. A vector (rather than one component per
    // weapon) because an entity holds at most one component of a given type.
    struct Armament {
        std::vector<Weapon> weapons;
    };

    // One fire-control channel — a "fire unit": the contact it is fighting and
    // whether it is shooting. Every battery mount belongs to exactly one channel
    // (Weapon::channel holds its id): a *lone* mount has its own channel (`group`
    // false) and a *group* is a channel several mounts share (`group` true). Either
    // way the order lives on the channel, not the mount — "this unit, engage that
    // one" — and every mount on the channel answers only whether the contact is
    // inside its own arc and range. A mount that cannot bear holds, and starts
    // firing the moment the target drifts into its arc, with no further order, which
    // is why a channel carries a contact and not a list of guns.
    //
    // Designating and firing are deliberately separate — pick a contact, read what
    // it is, and only then commit — so a channel with `firing` false is a unit
    // tracking its mark with the guns silent, arcs lit and aim solutions live.
    //
    // `id` is a stable handle the mounts reference; it is never reused, so a mount's
    // channel resolves even as the channel list is edited. `group` only shapes how
    // the Target window draws the unit (a lone mount vs a group with a member list);
    // the weapons system treats every channel identically. The weapons system clears
    // a channel's target once it is dead or gone from the registry — what makes
    // "fire until it is dead" terminate, per unit.
    //
    // `firing`, `salvo` and `freeFire` are three ways to say shoot, each granting a
    // gun the shot on its own — not a mode dial. `firing` is the standing order on
    // the designated contact; `salvo` is one round from whatever is loaded;
    // `freeFire` releases *this unit's* guns from the designated contact. Free fire
    // is the only one that lets a gun shoot at something the unit never named, which
    // is why it alone changes what a weapon may aim at rather than just when it may
    // pull the trigger — and it is scoped to the unit, so releasing one unit's guns
    // never disturbs another's.
    struct FireChannel {
        int id = 0;                       // stable handle the mounts reference; never reused
        bool group = false;               // false = one lone mount's order; true = a shared group
        entt::entity target = entt::null; // the designated contact, or null for none
        bool firing = false;              // true = shoot it whenever an assigned gun bears

        // A single round from every assigned gun that bears and is loaded, then
        // done. A latch, not a state: the weapons system consumes it the tick after
        // it is set and clears it, so it survives exactly one update however many
        // frames the button is held. Guns still reloading miss it — the salvo is
        // what the unit can fire *now*, not a volley it waits to assemble.
        bool salvo = false;

        // Weapons free: every gun on this unit engages the nearest foe it can bear
        // on, of its own accord and without a designated contact. The unit's
        // designated contact still comes first for any gun that can reach it, so
        // ordering free fire never pulls the unit off the mark chosen — it only
        // finds work for the guns that could not reach it anyway.
        bool freeFire = false;

        // How many rounds a Salvo releases from *each* weapon on this unit — the
        // player-set roller in the Target window. A gun ripples this many shots out
        // over its cooldown; a launcher this many tubes (capped at those ready when
        // it fires). It lives on the unit, not the weapon, so one roller governs a
        // whole group. Each weapon drains its own Weapon::pending from this when the
        // salvo is ordered.
        int salvoSize = 1;

        // Whether this unit's weapons draw their firing arcs. Purely a display
        // toggle — off by default to keep the view uncluttered, and the player
        // switches a unit's on when it wants to see where the guns can bear.
        // Scoped to the unit, so a group's arcs toggle together.
        bool showArc = false;
    };

    // A ship's fire control: its fire units. Every battery mount is on exactly one
    // channel here — its own lone channel or a group it shares. The player's ship
    // starts with one lone channel per mount (each gun its own unit) and forms
    // groups from the Target window; the enemy keeps a single channel its whole
    // battery shares, driven by the aggro system (`channels.front()`). `nextId`
    // hands out a fresh id per channel so a mount's channel is never ambiguous even
    // as units are made and disbanded. One component either way, so both sides shoot
    // down the same path rather than the AI having a private one.
    struct FireControl {
        std::vector<FireChannel> channels; // fire units, in display order
        int nextId = 0;                    // next channel id to hand out
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

    // A human-readable class name (the hull's display name, or its id if the
    // hull gives none), for labelling a contact in the controls readout.
    struct Identity {
        std::string name;
    };

    // A ship's detection reach — how far its own senses carry. Only the ranges
    // the current sensor step implements are populated; the rest of the sensor
    // suite (passive radar) fills these out as those steps land. Authored per
    // hull in data, so a bigger ship with a taller mast and a stronger set can
    // see and reach farther.
    //
    // visualRangeM is the range inside which a contact is simply *seen* — the
    // real hull on the water, full truth, no radar needed (the Visual rung of the
    // detection ladder in PLAN's *Sensors & the tactical view*).
    //
    // activeRangeM is how far an active radar sweep reaches: while activeOn, a
    // contact beyond visual range but within it is held at the Ranged rung — a
    // fixed position, but no identity. Radiating is a choice, not a given, which
    // is why activeOn is a runtime toggle and not a property of the hull: going
    // active buys the ranged picture at the cost (once the enemy can listen) of
    // announcing own position. It starts off — silent is the default posture.
    //
    // passiveRangeM anchors how far the passive ESM hears — but only a contact
    // that is itself *radiating* (its own activeOn), and only as a bearing, never
    // a range. It is not a flat cutoff: it is the range at which an emitter *as
    // loud as this ship's own radar* falls to the hearing floor. A louder emitter
    // (a bigger activeRangeM) is heard proportionally farther, a quieter one only
    // closer, and how strong the cut is conflates the two (see Contact::strength).
    // Passive listening is always on and costs nothing to own emissions, so this
    // is what a dark ship reads the sea with: it hears a careless emitter's
    // direction well out, and the price of ranging that bearing is going active.
    struct Sensors {
        float visualRangeM = 0.0f;  // m; a contact within this is seen outright
        float activeRangeM = 0.0f;  // m; active radar reach — a contact within this (beyond visual) is a ranged blip while radiating
        float passiveRangeM = 0.0f; // m; passive ESM reach — an *emitting* contact within this is heard as a bearing, no range
        bool activeOn = false;      // whether the active radar is radiating; off = emit nothing, hold only visual + passive contacts
    };

    // The rungs of the detection ladder (PLAN's *Sensors & the tactical view*),
    // ordered from least to most certain. A ship's knowledge of a contact climbs
    // these as its sensors resolve it: heard on a bearing, fixed by active radar,
    // that fix held long enough (or seen close enough) to classify, then seen
    // outright. The Identified rung is a Ranged fix whose class has resolved (see
    // Contact::identified); Visual is above it, since seeing the hull is the whole
    // truth at once.
    enum class DetectLevel {
        Bearing,    // passive: a bearing out from own ship, no range or identity
        Ranged,     // active: bearing + range, a fixed position, still unidentified
        Identified, // class, heading and speed resolved
        Visual,     // inside visual range: the real hull, full truth
    };

    // How long a lost contact lingers on the picture before it is forgotten. A
    // positioned contact (Ranged or Visual) that drops out of sensor reach is not
    // erased at once: it is held as a decaying track from its last-known position
    // for this long, fading as it ages, then dropped (PLAN's contact decay).
    // Shared by the sensor system (which ages and expires a track) and the renderer
    // (which fades the mark over it), so the two never disagree.
    inline constexpr float kContactDecayS = 20.0f;

    // What one observing ship knows about one contact, at the moment its picture
    // was last refreshed. Held in a ContactPicture, keyed by the observed hull's
    // entity. The picture is updated in place across ticks (not rebuilt), so a
    // contact carries a little memory: where it was last fixed, and how long since
    // it was last held.
    struct Contact {
        DetectLevel level = DetectLevel::Bearing;

        // World bearing from the observer to the contact (radians) — the one thing
        // a passive detection yields, and all it yields. Meaningful only at the
        // Bearing rung; a Ranged or Visual contact carries its true position on the
        // hull instead and is drawn from that. Held here rather than re-derived at
        // draw time precisely so the range is *not* available to the renderer: a
        // bearing-only contact must not be placeable, or it would leak the range
        // passive listening never gave.
        float bearing = 0.0f;

        // Normalised passive signal strength in [0, 1] — 0 at the edge of hearing,
        // 1 for a loud, near cut. Meaningful only at the Bearing rung. It is a
        // function of the *received* signal alone (emitter power over distance
        // squared), which is what makes it deliberately ambiguous: a large emitter
        // far off and a small one close can read the same, so strength hints at
        // "big or near" without ever giving the range apart. The renderer shows it
        // as the wedge's brightness and how tight a cut it is.
        float strength = 0.0f;

        // The last position the contact was *fixed* at, and whether it holds one.
        // Set from the hull's true position each tick a Ranged or Visual detection
        // lands; a bare bearing yields no fix and leaves hasPos false. This is what
        // a lost track decays from — the mark stays put at the last-known point
        // while the real hull steams on unseen — and what the radar blip is drawn
        // at, so a stale contact freezes rather than tracking a hull it no longer
        // holds.
        b2Vec2 lastPos{ 0.0f, 0.0f };
        bool hasPos = false;

        // The contact's estimated velocity, built up by tracking rather than read
        // off the hull — an active fix gives a position each tick, and a run of
        // them differenced over time betrays the course and speed (an alpha-beta
        // tracker in UpdateSensors, converging over a few tens of seconds, so a
        // freshly acquired contact's vector starts soft and firms as it is held).
        // It drives three things: the dead reckoning that carries a positioned
        // ghost forward once its fix goes stale (lastPos drifts on it each tick, so
        // a lost radar contact coasts on its last-known course instead of freezing),
        // the firing solution's lead, and the course tick/readout — whose heading is
        // the velocity's own direction, the only "heading" a radar ever measures
        // (aspect off the track, not the bow). Reading it off the snapshot rather
        // than the entity is what stops a radar-off contact leaking the truth as it
        // fades.
        b2Vec2 velocity{ 0.0f, 0.0f };

        // Whether the velocity estimate has been tracked long enough to trust — the
        // gate the course tick and the Target list's motion read behind, set once
        // the contact has been positionally held for kMotionResolveS. Sticky like
        // `identified`, and reached well before it: a radar track yields a course
        // and speed within tens of seconds, long before the minutes of hold that
        // classify the hull, so the plot shows where a contact is going before it
        // shows what it is.
        bool motionKnown = false;

        // Seconds since the contact was last detected at *any* rung (bearing,
        // ranged or visual). Zero on any tick a detection refreshes it; it climbs
        // once the contact drops out of all reach. Governs whether the track is
        // still held at all — a contact neither detected nor still carrying a
        // visible position ghost is forgotten. SeesHull also reads it: a visual
        // contact is only "seen" while this is zero.
        float staleness = 0.0f;

        // Seconds since the contact was last *positionally fixed* — a Ranged or
        // Visual detection, the rungs that give a real position. A bearing does not
        // reset it: direction is not a fix. So a contact that drops from an active
        // fix down to a passive bearing keeps a live bearing (staleness zero) while
        // its last-known position goes stale (this climbing) — the blip decays as a
        // ghost from lastPos over kContactDecayS even as the bearing line stays
        // fresh. Governs the radar blip and its fade, where staleness governs
        // retention.
        float fixStaleness = 0.0f;

        // Class-classification state. `identified` is sticky once earned: seen
        // inside visual range it resolves at once, else it takes kIdentifyDwellS of
        // radar hold — minutes, not the tens of seconds motion takes — and then
        // stays known for as long as the track lives, so a brief downgrade to a
        // bearing does not un-learn what it is. `dwell` is the seconds of positional
        // hold accumulated toward both that and `motionKnown`, counting only the
        // ticks the contact is actually fixed (Ranged or Visual) — a bearing gives
        // no position and does not advance it. Until identified the Target window
        // reads the contact's class as Unknown and the plot marks it with an open
        // rather than a filled blip; its course, though, shows as soon as
        // motionKnown, ahead of the class (see velocity).
        float dwell = 0.0f;
        bool identified = false;
    };

    // A ship's own picture of the sea: every contact it holds and how well it
    // holds each. Per observer, not one global truth the game reveals — two ships
    // can legitimately disagree about what is out there, which is exactly what a
    // shared fleet picture (see PLAN's *Fleet command*) will later merge. While
    // there is a single player ship only it carries a picture; the enemy is left
    // omniscient (the aggro system still scans every hull directly) until the
    // fight is made two-sided. The sensor system rebuilds this each tick; the
    // renderer and the target-picking both read it, so what the player cannot
    // detect it can neither see nor designate.
    struct ContactPicture {
        std::unordered_map<entt::entity, Contact> contacts;
    };

    // Whether `picture` holds `contact` at the Visual rung *this tick* — the
    // observer actually sees the hull right now, as opposed to holding only a
    // ranged blip, a decaying last-known track, or nothing. This is the gate for
    // drawing a contact as its literal hull, with its wake and firing arcs; a
    // ranged or stale contact draws as a bare radar mark instead (see
    // DrawContacts), so those hull visuals must not leak from it. The freshness
    // test is what keeps a *lost* visual contact from still drawing its live hull:
    // once it goes stale it ghosts from its last-known position like any other.
    inline bool SeesHull(ContactPicture const& picture, entt::entity contact) {
        auto const it = picture.contacts.find(contact);
        return it != picture.contacts.end() && it->second.level == DetectLevel::Visual &&
               it->second.staleness == 0.0f;
    }

    // One passive contact's target-motion-analysis track: the rolling history of
    // bearing cuts and the own-ship position each was taken from, plus the
    // solution the solver has fit to them so far. This is how a range is wrung out
    // of a passive contact *without going active* — a single bearing carries no
    // range, but the way a run of them walks against own ship's own manoeuvre
    // betrays one (PLAN's *Active versus passive*). So the track has to persist
    // across ticks and accumulate, unlike the ContactPicture that is rebuilt each
    // tick — history is the whole point.
    //
    // The solution is a constant-velocity estimate: where the contact is and how
    // it is moving. It is observable only once own ship has manoeuvred — on a
    // single straight leg the geometry is ambiguous (a near slow contact and a far
    // fast one trace the same bearings), which the solver reports as a near-zero
    // `observability` and this system pays deliberate manoeuvre by resolving. Fed
    // only own positions and measured bearings, never the contact's true range.
    struct TmaTrack {
        struct Sample {
            float t = 0.0f;                // s since the track opened
            b2Vec2 ownPos{ 0.0f, 0.0f };   // own-ship world position at the cut (m)
            float bearing = 0.0f;          // measured world bearing to the contact (rad)
        };
        std::vector<Sample> samples;       // rolling window, oldest first
        float age = 0.0f;                  // s since the track opened
        float sinceSample = 0.0f;          // s since the last cut was appended

        // The solver's current fit, referenced to the latest sample. Meaningful
        // only while `solved`; confidence 0 means the geometry has not yet earned
        // a usable solution (too few cuts, too short a baseline, or a straight leg
        // that leaves range unobservable).
        bool solved = false;
        b2Vec2 position{ 0.0f, 0.0f };     // estimated contact position (m), at the latest cut
        b2Vec2 velocity{ 0.0f, 0.0f };     // estimated contact velocity (m/s)
        float confidence = 0.0f;           // [0,1]; grows with own-ship manoeuvre and a tight fit

        // Solver diagnostics, for the TMA debug readout and for tuning the gates.
        float observability = 0.0f;        // scaled smallest pivot of the fit; ~0 on a straight leg
        float residualRad = 0.0f;          // RMS angular misfit of the bearings to the fit
    };

    // An observing ship's file of passive tracks, keyed by the observed hull's
    // entity — the persistent counterpart to its ContactPicture. Held only by a
    // ship that carries a picture (the player, for now). The TMA system opens a
    // track when a contact is first heard on a bearing, feeds it cuts as own ship
    // moves, and drops it once the contact is ranged, seen, or lost (contact decay
    // — carrying a lost track on from its last fix — arrives with the step after).
    struct TrackFile {
        std::unordered_map<entt::entity, TmaTrack> tracks;
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

    // How a projectile flies. A ballistic shot holds a constant velocity; a
    // guided munition steers toward its homing target and accelerates as it goes.
    enum class Guidance {
        Ballistic,
        Guided,
    };

    // A projectile in flight. Kept out of Box2D — it has no collision; hits are
    // resolved analytically against hull rectangles.
    //
    // Ballistic (a gun's shell): constant velocity, detonates once it has
    // travelled to its fuzed `remaining` range — set when fired to the target's
    // distance (plus a little random spread) so a miss splashes near the target
    // rather than flying on to the weapon's max range.
    //
    // Guided (a launcher's munition): each tick it turns its heading toward the
    // homing target at `turnRate` and ramps its speed toward `maxSpeed` by
    // `acceleration`, so it leaves the cell slow and accelerates in. `remaining`
    // is its self-contained run distance: it flies until it strikes a hull or
    // runs that out and splashes. Losing its target (the hull sank or left) it
    // holds its heading and coasts on to the end of that run.
    struct Projectile {
        b2Vec2 position{ 0.0f, 0.0f }; // world space (metres)
        b2Vec2 velocity{ 0.0f, 0.0f }; // m/s
        float remaining = 0.0f;        // m of travel left: fuze range (ballistic) or run distance (guided)
        float radiusM = 0.0f;          // metres: draw radius when ballistic; always the collision / point-defence / splash size
        // A guided munition draws as a rectangle laid along its travel rather than
        // a circle (see DrawProjectiles); these are its length and width in metres.
        // Both zero for a ballistic shot, which draws as a circle of radiusM.
        float drawLengthM = 0.0f;      // m, along travel
        float drawWidthM = 0.0f;       // m, across the beam
        float damage = 0.0f;           // hit points removed from the hull it strikes
        moth_ui::Color color;          // draw colour
        Faction target = Faction::Enemy; // the faction this shot may strike

        // Warhead health, for point defence to knock a guided munition down before
        // it strikes. A CIWS chips this each burst; at zero the munition is
        // destroyed in flight. Meaningful only for a guided air munition (a shell
        // is gone the instant it lands, and a torpedo runs below the reach of
        // these guns); 0 leaves the munition impervious to point defence, which is
        // a deliberate content lever, not merely the ballistic default.
        float health = 0.0f;

        // Guidance and the munition fields it needs; all inert for a ballistic
        // shot. homingTarget is the hull the munition steers toward, cleared to
        // null once it is no longer a valid live contact.
        Guidance guidance = Guidance::Ballistic;
        entt::entity homingTarget = entt::null; // hull steered toward, or null once lock is lost
        float maxSpeed = 0.0f;                  // m/s the munition accelerates up to
        float acceleration = 0.0f;              // m/s^2 gained in flight
        float turnRate = 0.0f;                  // rad/s the heading steers toward the target

        // Warhead arming: distance (m) the munition must still travel before it is
        // live. Counts down with the run; a strike while it is above zero is a dud
        // — the munition is spent but does no damage and detonates nothing, which
        // is what gives a launcher a minimum range. Zero (a ballistic shot, or a
        // munition with no minimum range) is armed from the muzzle.
        float armDistance = 0.0f;

        // A waterborne munition (a torpedo) is already in the sea, so when it runs
        // out its range it simply stops rather than throwing up a surface splash.
        // False for shells and air-launched missiles, which splash where they fall.
        bool waterborne = false;

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
