# naval_moth — Plan

Top-down naval combat game. Ships are heavy, momentum-driven vessels that
maneuver around a sea and fight with automatic weapons. Built on the moth
workspace: **moth_graphics** for rendering and UI, **Box2D** for physics and
collision, **EnTT** for the entity/component model.

This document describes intent and scope — the *what*, not the *how*. It is the
source of truth for direction; implementation notes live in code and commit
messages.

## Design pillars

- **Momentum matters.** Ships are weighty. They turn slowly, carry speed, and
  cannot stop on a dime. Positioning is a skill, not a given.
- **Command, don't pilot.** The player issues intent (a destination) and the
  ship works out how to get there. Combat is automatic once weapons have valid
  targets — the player's job is maneuver and positioning.
- **Composable ships.** A ship is a hull plus attached components (chiefly
  weapons). Loadouts differ; behaviour follows from the components present.

## Movement & control

The player clicks a point in the play space. The selected ship automatically
turns toward that point and powers toward it, easing off as it approaches and
coming to rest within a set arrival distance. Momentum is deliberately visible:
the ship may drift past the point before settling, and sharp reversals are not
possible.

Click distance scales effort. A destination far away commands fast, hard power;
a nearby destination commands a small, careful adjustment. This gives the player
one gesture that covers both "charge across the map" and "nudge into position."

## Ships & components

Ships are composed rather than hard-coded. A hull carries structural identity and
health; **propulsion** and **weapons** are attached components. This keeps room
for varied ship classes and, later, submarines.

Propulsion describes how a ship moves — available power (how hard it can drive),
how it turns, and the handling that follows (top speed, acceleration). Turning is
rudder-like: a ship has little turning authority dead in the water and comes
about faster the more way it has on, so maneuvering is bound up with managing
speed. The movement system reads these values rather than assuming fixed
constants, so two hulls with different propulsion handle differently: a nimble
cutter and a ponderous ship of the line share the same control model but feel
distinct.

## Weapons

Weapons are components mounted on a ship. Each weapon carries:

- a **projectile type** (cannonball, missile, torpedo, …),
- a **muzzle velocity**,
- a **damage** value,
- a **shot cooldown**, and
- **targeting parameters**: firing arc, a range band (below), and which enemy
  types it may engage.

Weapons acquire targets on their own and fire automatically, respecting their
cooldown, whenever a valid target sits inside their arc and firing range.
Different weapons on the same ship target independently.

### Per-weapon controls

Automatic fire is the default, but the player can take hands-on command of each
gun the active ship carries. The control panel lists every weapon and, per
weapon, offers:

- a toggle for whether it **draws its firing arc**, so the display can be kept
  clean or a single gun's coverage highlighted;
- a toggle for **automatic fire** — on, it engages on its own as above; off, it
  holds fire until told;
- a **fire button** to shoot on command (still bound by cooldown and a valid
  target in arc and range); and
- a **target readout** for whatever contact the gun has locked: range, speed,
  bearing, heading, and contact type — the tactical picture for that weapon.

Controls are independent per weapon, so one gun can free-fire while another is
held for a manual salvo.

### Range band

Range is not a single number but a band with four zones, so distance shapes both
*whether* a weapon can shoot and *how likely* it is to land. Each weapon carries
three thresholds — **min**, **effective**, and **max** — that divide the line of
fire into:

- **Too close (below min).** Inside the minimum range the target cannot be
  engaged at all — the weapon can't depress or traverse onto something right
  alongside. Some weapons set min to zero and have no dead zone.
- **Guaranteed (min → effective).** A target between the minimum and effective
  ranges is hit every time it is fired on (100%).
- **Falloff (effective → max).** The weapon may still fire past its effective
  range, but accuracy decays across this band: hit chance starts at 100% at the
  effective edge and drops to 0% right at the maximum range. A shot rolls against
  that chance; a miss splashes harmlessly.
- **Out of range (beyond max).** The target is not engageable and does not count
  for target acquisition.

Targeting acquires anything inside max range (the outer edge of the falloff
band); the roll only decides whether an individual shot connects. This gives
weapons distinct personalities — a short, sure-fire close weapon versus a
long-reach gun that can harass at distance but only reliably kills within its
effective range.

## Projectiles

Distinct projectile types with their own behaviour — starting with a simple
straight-shot cannonball (flat travel, fuzed to detonate at the target's range so
a miss splashes near the target rather than flying on to the weapon's maximum
range). A projectile carries no range of its own yet — the firing weapon sets
both its reach and, per shot, the fuze. Missiles and torpedoes follow later, with
homing/guidance behaviour — and a self-contained run distance independent of the
launcher — of their own.

## Data & content

Ships, weapons, projectiles, and enemies are defined as data, not code. Each is a
named definition authored in a JSON file and loaded at startup into an in-memory
registry; the game instantiates entities from these definitions through a spawn
factory rather than from hard-coded constants. This keeps the composable-ships
pillar honest — a loadout is just a different set of references — and lets content
and balance change without a recompile.

Five definition sets, cross-referenced by id:

- **Hulls** — structural identity: propulsion handling, hull dimensions and render
  spec, base health, and the weapon mount points a hull carries.
- **Weapons** — a projectile reference, muzzle velocity, damage, shot cooldown, and
  targeting parameters (firing arc, the min/effective/max range band, engageable
  enemy types).
- **Projectiles** — a shot's visuals (draw size and colour) and, later, its flight
  behaviour (straight-shot now, homing for missiles and torpedoes). Speed and
  damage live on the firing weapon. A straight shot's travel is bounded by the
  weapon's range; self-guided munitions gain their own run distance when they
  arrive.
- **Enemies** — a hull plus a weapon loadout and an AI behaviour profile; what the
  game spawns as an opponent.
- **Player** — the player's own ship: a hull (and later a chosen loadout). A single
  definition rather than a table, since there is only ever one player.

References resolve by id — an enemy names a hull and weapons, a weapon names a
projectile — and are validated when the definitions load, so a bad reference fails
loudly at startup rather than at spawn time. Definitions are read-only content: a
spawned entity's mutable state (position, health, cooldown timers) lives in its
components, never back in the definition. The registry and factory arrive with the
hull table; the weapon, projectile, and enemy tables come online through the same
machinery as the milestones that consume them land.

## Camera & world

- **Phase 1:** open sea, camera locked. No land, no scrolling.
- **Later:** a larger map with land masses to navigate around, and a camera that
  follows the active ship on a spring (smooth lag rather than a rigid lock).

## Enemies & multiplayer

- **Phase 1:** a single AI enemy ship to fight and destroy.
- **Later:** multiple AI ships positioned around the map.
- **Later:** networked player-controlled ships fighting alongside or against AI.

## Stretch

- Submarines as a ship class (dive/surface state).
- Torpedo weapons.
- Sonar mechanics for detecting submerged contacts.
- **Pathfinding around land.** Today a move order steers straight at the
  destination, so a course that crosses land just grinds the hull against the
  shore. A ship could instead route around intervening land — finding a path to
  the destination and following it as a series of waypoints, while keeping the
  momentum-driven handling that makes maneuvering a skill. Not needed while the
  seas are mostly open, but a natural fit once land is common enough to get in
  the way.

## Milestones

Each milestone is a playable increment, ordered so the game feels like something
as early as possible.

- **M0 — Scaffold.** Project builds and links moth_graphics + Box2D + EnTT with
  an empty entry point. *(done)*
- **M1 — A ship that sails.** One player ship on open sea, camera locked.
  Click-to-move with momentum-driven turn/power and arrival easing. *(done)*
- **M2 — Ships from data.** A definition registry loads hull definitions from
  JSON; the player ship is spawned from a named hull through a factory instead of
  inline constants. Same ship, now data-driven — the groundwork the other tables
  reuse. *(current)*
- **M3 — First fight.** Weapon components fire automatically at a target; one AI
  enemy that can be damaged and destroyed. Health and win/lose state. Weapon,
  projectile, and enemy definitions come online here.
- **M4 — Weapon depth.** Full targeting parameters (arcs, the min/effective/max
  range band with accuracy falloff, enemy types) and multiple weapons per ship
  engaging independently. Additional projectile types.
- **M5 — A world to sail.** Map with navigable land and a spring-follow camera.
- **M6 — A sea of enemies.** Multiple AI ships around the map with independent
  behaviour.
- **M7 — Other captains.** Networked player ships.
- **M8 — Beneath the surface.** Submarines, torpedoes, and sonar.

## Musings

*Undirected ideas, not committed scope — a place to think out loud.*

**Hunt-and-sink as information warfare.** A multiplayer mode where two players
share a large sea and the whole game is *finding* each other before you can
fight. Sinking the enemy is the goal, but the hard part is knowing where they
are. Position is hidden by default; you build a picture of the map from
imperfect, decaying sources rather than seeing the other ship outright.

The fun would live in the detection layer, borrowing from modern combat:

- **Radar** — active sweeps reveal contacts but announce your own position to
  anyone listening.
- **Radio / signals intelligence** — intercepting emissions to bearing-fix an
  enemy who's being noisy, trading silence for stealth.
- **Scouting assets** — drones, helicopters, spotter aircraft that extend your
  sensor reach out beyond the horizon at some cost or risk.
- **Intelligence reports & area knowledge** — periodic or purchased hints,
  terrain and shipping-lane knowledge that narrows the search.

The tension is emissions control versus information: every way of learning where
the enemy is tends to give away where *you* are. A match becomes a stalking
game — go loud and find them fast but get found, or stay dark and hunt patiently.
Fits naturally on top of the momentum-driven handling and the composable-ship
model, and would lean hard on the later networked-play and sensor work.
