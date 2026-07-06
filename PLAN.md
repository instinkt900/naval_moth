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
- a **damage** value,
- a **shot cooldown**, and
- **targeting parameters**: firing arc, range, and which enemy types it may
  engage.

Weapons acquire targets on their own and fire automatically, respecting their
cooldown, whenever a valid target sits inside their arc and range. Different
weapons on the same ship target independently.

## Projectiles

Distinct projectile types with their own behaviour — starting with a simple
straight-shot cannonball (flat travel with a maximum range, splashing out at the
end). Missiles and torpedoes follow later, with homing/guidance behaviour of
their own.

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

## Milestones

Each milestone is a playable increment, ordered so the game feels like something
as early as possible.

- **M0 — Scaffold.** Project builds and links moth_graphics + Box2D + EnTT with
  an empty entry point. *(current)*
- **M1 — A ship that sails.** One player ship on open sea, camera locked.
  Click-to-move with momentum-driven turn/power and arrival easing.
- **M2 — First fight.** Weapon components fire automatically at a target; one AI
  enemy that can be damaged and destroyed. Health and win/lose state.
- **M3 — Weapon depth.** Full targeting parameters (arcs, ranges, enemy types)
  and multiple weapons per ship engaging independently. Additional projectile
  types.
- **M4 — A world to sail.** Map with navigable land and a spring-follow camera.
- **M5 — A sea of enemies.** Multiple AI ships around the map with independent
  behaviour.
- **M6 — Other captains.** Networked player ships.
- **M7 — Beneath the surface.** Submarines, torpedoes, and sonar.
