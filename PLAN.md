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
- **Command, don't pilot.** The player issues intent — a destination, a contact
  to engage — and the ship works out how to carry it out. Gunnery is automatic
  once fire is ordered: which guns bear is the ship's problem, not the player's.
  The player's job is manoeuvre, positioning, and choosing the fight.
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

Weapons acquire nothing on their own. The ship designates one contact and orders
fire; each weapon then only answers whether that contact is inside its own arc
and range, and shoots it while it is. Guns are not commanded individually — the
order is the captain's, and the batteries that can reach obey it.

### Designating a target

The player designates a contact by **clicking it**; clicking open water is a helm
order as before, so the two never collide. Designating does not open fire — you
pick a contact, read what it is, and only then commit.

A **ring** marks the designated contact on the water: green while no gun bears on
it, red the moment one does. So "am I in a position to hit that?" is answerable
from the sea itself, without reading the panel — and since the ring reads the
guns' own bearing test, it turns red exactly when the guns would actually shoot.

The **Target window** shows the designated contact's picture — type, range,
speed, bearing, heading and health — along with how much of the battery bears on
it, and carries the ship's fire orders. **Fire** becomes **Hold** once fire is
ordered. It is available whether or not anything bears: ordering fire while
still manoeuvring onto the target is the point, and each gun joins in of its own
accord as its arc comes onto the contact.

Fire continues until the contact is dead or Hold is clicked. Nothing else ends
it, and a target that sinks clears the order outright.

**Salvo** is the same order without the commitment: one round from every gun that
bears and is loaded, then done. Guns still reloading miss it — a salvo is what the
battery can throw at that moment, not a volley it waits to assemble. It leaves
nothing set behind it, so it is the way to try the range without opening a
sustained engagement.

**Free fire** releases the guns from the designated contact: each engages the
nearest foe it can bear on, of its own accord, and it needs no designated contact
at all. It is the one order that lets a weapon choose what to shoot — Fire and
Salvo only decide *when* the trigger comes, never *what* is under it. The
designated contact still takes precedence for any gun that can reach it, so
releasing the guns never pulls the battery off the mark you chose; it only finds
work for the guns that could not reach that mark anyway.

The three are not a mode dial — each grants the shot on its own, and they compose:
free fire with a target designated and Fire ordered means the guns that bear on it
concentrate, and the rest fight the ships they can see. All three act only on the
guns ticked into the battery (see Per-weapon controls); a gun switched out holds
through every one of them.

### Per-weapon controls

The battery list sits at the foot of the Target window and names every gun the
active ship carries. A tick beside each gun's name **switches it in or out of the
fire orders**: unticked, it holds through Fire, Salvo and free fire alike while
still tracking the contact and drawing its arc — a gun's one say in the engagement
is whether it takes part at all. Per weapon there is also a toggle for whether it
**draws its firing arc** (so the display can be kept clean or one gun's coverage
highlighted), the same for its **spread preview**, and a read-only **status** —
bearing, holding, firing, reloading, or disabled.

The enemy AI issues the same order through the same path — its aggro lock *is* a
fire order — so both sides shoot by identical rules rather than the AI having a
private one.

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

Once missiles and torpedoes exist as threats in their own right — objects that
travel through the world and take time to arrive, rather than instant hits — point
defence becomes possible: a CIWS-class weapon should be able to engage *them*, not
only ships. Such a mount would lay onto an inbound missile or torpedo inside its
arc and try to destroy it before it strikes, so close-in guns answer standoff
weapons rather than being just another short-ranged anti-ship battery. That makes
the range band a real attack/defence exchange — reach out with the main guns and
missiles, hold off what comes back with the CIWS. It also asks two things of the
weapons system that anti-ship fire does not: a weapon needs to know which *kinds*
of thing it may target (hulls, projectiles, or both), and targeting has to consider
in-flight ordnance as candidates alongside ships.

## Sound

Sound is there to give weight to things that already happen — a gun firing, a shell
landing, a hull going up — not to be a system in its own right. It stays deliberately
shallow: no doppler, no true 3D panning, no filters, no music. A sound is named in
data by whatever makes it, and plays where that thing happened.

Three things shape a sound at the moment it plays, all of them fixed as it starts
rather than tracked while it runs.

**Distance** across the water from the camera sets its volume, fading to nothing well
out from the view, so a battle across the map doesn't shout as loudly as your own guns.
**Zoom** scales it again: a sound plays as authored only with the camera right down on
the action, and pulling back to survey the map quietens everything — including whatever
sits dead centre — down to a floor that keeps a distant battle audible rather than
silent. The two are separate knobs because they answer different questions, and because
zoom is a multiplier the wheel moves in steps: it earns a response measured against the
zoom limits, not against a distance in metres. **Pitch** is varied slightly per playing,
which is what stops a rapid-firing gun from sounding like a loop of one sample.

Sounds are also **panned** left or right by where they sit across the view, which is a
stereo placement and not the beginning of spatialisation — no doppler, no distance
filtering, no height. It tracks the picture: a gun at the edge of the screen is at the
edge of the stereo image whatever the zoom.

Volume and pitch variance are authored per sound, since how loud a recording is and how
much it can be bent are properties of the recording.

Missing audio is not an error. A weapon that names no sound is silently quiet, which
is how content stays playable while sounds are still being found for it; only a
*named* sound that isn't there is worth complaining about.

## Camera shake

The camera takes a knock when something violent happens in front of it — a gun
firing, a shell striking home, a hull going up. Like the sound, it exists to give
weight to things that already happen rather than to be a system of its own, and it
is authored the same way: whatever makes the violence says in data how hard it
shakes, and an unauthored shake is simply a still camera.

Only a hit shakes. A shot that falls in the sea splashes and is heard, but it has
struck nothing, and shaking for it would tell the player a blow landed when none
did.

**How far a shake reaches is the same question as how far a sound carries**, and
so it is answered once, by one curve, for both. A gun you can barely hear is a gun
that barely moves the picture; something loud enough to fill the speakers is
something that shoves the view around. Tying them is what keeps the two honest —
they cannot drift apart under tuning, because there is only one thing to tune.
Distance and zoom both feed it: a thing fades to nothing over a set reach of open
water, and pulling the camera back to survey the map damps everything down.

What that buys is a jolt measured in metres of sea rather than in fractions of the
screen, which means a shake can arrive from past the edge of the view when zoomed
right in — the guns of a battle you are not looking at still reach you. That is the
point of measuring in metres, not a side effect of it.

Beyond that reach, the two systems share nothing: the shake decides for itself how
a jolt piles up and dies away, and the sound pans and pitches on its own.

Jolts from separate events add together and are capped, so a broadside landing at
once hits harder than a single shell without throwing the view somewhere the player
has to wait to get back. A shake is a knock rather than a rumble: it is most of the
way gone within a fraction of a second, so a gun on a fast cooldown lands its next
one into a camera that has nearly settled.

## Data & content

Ships, weapons, projectiles, and enemies are defined as data, not code. Each is a
named definition authored in a JSON file and loaded at startup into an in-memory
registry; the game instantiates entities from these definitions through a spawn
factory rather than from hard-coded constants. This keeps the composable-ships
pillar honest — a loadout is just a different set of references — and lets content
and balance change without a recompile.

Six definition sets, cross-referenced by id:

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
- **Sounds** — a named sound effect: the audio file, how loud it plays, and how much
  its pitch varies from one playing to the next. Named by the things that make the
  noise rather than owned by them, so several weapons can share one report.

Content is authored with real warships and their armament in mind — hull
dimensions, speeds, and weapon fits should read as plausible for the vessel
they're modelled on, so the fleet feels like a fleet rather than a set of
abstract shapes. Gun ranges are the one deliberate exception: real naval gunnery
reaches far enough that a true-scale engagement is two dots shooting at each
other from off-screen, which is no fun to play. **Divide a real range by 10** when
authoring a weapon, keeping the relative reach of one gun against another intact
while pulling the whole fight into a range where maneuver matters and both ships
are visible at once.

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
- **Programmed munition routes.** Let a guided munition be given a route — a
  series of waypoints it runs before its seeker takes over — instead of homing
  from the instant it launches. A torpedo could be steered around a headland
  rather than swimming into it; a Harpoon could be routed to come in on a bearing
  that skirts a target's point defence. The terminal homing stays exactly as it
  is now; this only adds a pre-seek path in front of it. A natural companion to
  ship pathfinding below — the same waypoint idea, aimed by the captain rather
  than solved by the ship.
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
