# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The workspace-level `../CLAUDE.md` covers working principles, the build system, and
shared code conventions, and applies here too. This file covers only what is specific
to naval_moth.

## Design source of truth

`PLAN.md` describes intent and scope — the *what*, not the *how* — and is the source of
truth for direction, including the milestone ordering (currently M2, ships from data).
Read it before adding a feature: it already specifies the range-band model, per-weapon
controls, projectile types, and the data-driven content design, and says which of those
are deliberately not built yet. Implementation notes live in code and commit messages,
not in PLAN.md.

## Build

Conan 2.x + CMake presets, C++17. Builds and running are the user's job (see the
workspace principles) — this is reference for correct invocations, not a cue to run them.

```bash
conan install . -pr .conan/profile -s build_type=Debug --build=missing
cmake --preset conan-debug          # Windows: conan-default
cmake --build --preset conan-debug
```

- `moth_graphics` resolves from a private Artifactory remote, not Conan Center:
  `conan remote add moth https://artifactory.matthewcotton.net/artifactory/api/conan/conan-local`
- **There is no test suite** — no test target, no CTest registration. Verify by reasoning
  about the code, or by the user running the game.
- The build is `-Wall -Werror` (`/W3 /WX` on MSVC), so an unused variable fails the build.
- `.clangd` points at `./build/Debug` for the compile database, so a Debug configure is
  what makes clangd work.

## Running

The binary loads `assets/data` as a **relative path**, so it must run with the project
root as the working directory. SDL is the default backend; `--vulkan` switches to
GLFW + Vulkan.

## Architecture

`main.cpp` (picks a platform backend, installs an spdlog-backed `moth_ui::ILogger`) →
`NavalApplication` (a `moth_graphics::platform::Application`) → **`GameLayer`**, which owns
essentially the whole game: the `b2World`, the `entt::registry`, the `defs::Database`, the
`Terrain`, the `Camera`, and the `Audio`. There is no separate world/sim class — `GameLayer::Update()`
*is* the simulation loop and `GameLayer::Draw()` *is* the frame. Systems are free functions
over the registry (`src/game/*_system.h`), not classes; the layer calls them in order.

### System ordering is load-bearing

`Update()` runs systems in a specific order, and several of them depend on it. Preserve it
when adding systems, and comment the ordering constraint when you do:

- `UpdateAggro` before `UpdateWander` — aggro claims the helm first, so wander only steers
  ships still on patrol.
- `UpdateAggro` before `UpdateWeapons` — aggro issues each engaging enemy's `FireOrder`, and
  the weapons system is what consumes it; reversed, every enemy fires a tick stale.
- Steering (aggro/wander/propulsion) before `m_world.Step()` — they apply forces the step
  integrates.
- `UpdateWake` after the step — marks drop at the hull's settled position.
- `UpdateSinking` after the step — it destroys Box2D bodies, which must not happen inside
  the world update.
- `Audio::SetListener` after the camera pan but before any system that fires — it takes the
  whole `Camera`, since volume and pan both depend on the zoom as well as the centre. Gain and
  pan are baked in when a sound starts and never updated while it plays.
- `Audio::Update` last — it reclaims the voices of finished sounds, so it must run after
  everything that took one this tick.

`Audio::Load` also has to happen before the first spawn (it's in the `GameLayer` constructor,
after `defs::Database` is up), because a ship resolves its sound handles as it is built.

### Units: metres vs pixels

Box2D simulates in **metres**; moth draws in **pixels**. All world state — hull dimensions,
positions, weapon ranges, speeds — is authored in metres, and `Camera` (`game/camera.h`) is
the single place the two spaces meet (`WorldToScreen` / `ScreenToWorld` / `MToPx`). Don't
convert anywhere else, and don't introduce pixel-valued world state.

A body's Box2D transform is the **sole** source of truth for position and heading. Systems
read it; nothing keeps a duplicate.

### Data-driven content

`assets/data/{hulls,weapons,projectiles,enemies,player,sounds}.json` load once at startup into
`defs::Database`, which validates every cross-reference by id and throws on a dangling one —
so bad content fails loudly at startup, not at spawn. Definitions are **read-only**; a spawned
entity's mutable state (health, cooldowns, position) lives in its components and never writes
back to the definition. Spawn through `ship_factory.h` rather than inline constants — a ship
is built entirely from its hull id.

JSON authoring uses friendlier units than the runtime structs (`maxSpeedKnots`, `bearingDegrees`,
`arcDegrees`, `spreadDegrees`); `defs.cpp` converts on load.

### Things that surprise

- **`hull_shape.h`** — one 8-vertex outline template drives both the physics fixture (in metres)
  and the drawn hull (in pixels), so collision matches the silhouette. The 8-vertex cap is
  Box2D's polygon limit, which is why a hull gets one tunable shoulder per end rather than an
  arbitrary station list.
- **Projectiles are not Box2D bodies.** They're plain components integrated by hand in
  `combat_system.cpp`, with no collision — hits are resolved analytically.
- **Terrain is streamed and effectively endless.** Perlin noise → marching squares per chunk,
  generated as the camera reveals a chunk and torn down when it leaves; coastlines become static
  Box2D edges. `Terrain::IsWater` reads the noise field directly, so spawn placement doesn't
  depend on which chunks are resident.
- **`AggroTuningRef()`** is a deliberate mutable global shared by the aggro system, the ImGui
  debug sliders, and the ring renderer, so all three stay in step while tuning at runtime.
- **Weapons live in an `Armament` vector**, not one component per weapon, because an entity holds
  at most one component of a given type.
- **miniaudio is vendored**, not a Conan package (`external/miniaudio`, a single public-domain
  header). It has no dependencies, so vendoring sidesteps the Linux system-package rule the rest
  of the audio/display stack is bound by, and it owns its own device thread — so audio works the
  same under SDL and under GLFW/Vulkan. `src/game/miniaudio_impl.cpp` exists only to hold its
  implementation and is the one file built with warnings off; the `MA_NO_*` switches trimming it
  are set target-wide in CMake so every TU agrees on what's in the header.
- **Sound ids resolve to `int` handles at spawn** (`kNoSound` = silent), not looked up by string
  when a gun fires — the same trade `Weapon` already makes for its projectile's stats. A shot
  carries its own impact/splash handles because the gun that fired it may have sunk by the time
  it lands.
- **Volume is distance × zoom, kept as two separate curves** (`audio.cpp`). Distance fades over
  metres of water; zoom is a master gain interpolated against `kMinZoom`/`kMaxZoom` (in
  `camera.h` — shared with the wheel handler for exactly this reason) on a **log** scale, since
  the wheel scales zoom multiplicatively. Folding zoom into distance as a listener altitude was
  tried and reverted: it ties the zoom response to `kSilenceM` and flattens it to ~1 dB across
  the near half of the zoom range. Panning is miniaudio's panner, which runs after its
  spatialiser and so costs nothing that `MA_SOUND_FLAG_NO_SPATIALIZATION` turns off.
- **Missing audio degrades, it doesn't throw** — deliberately unlike the rest of `defs::Database`.
  An *unnamed* sound is silent by design; a *named but dangling id* still throws like any other
  reference; a named id whose **file** won't load only warns and goes silent, since `Database`
  never opens the file — `Audio::Load` does.

## Code style

Beyond the workspace conventions: this codebase comments **why**, at length, and in prose —
component and system declarations carry paragraph-level explanations of the design intent and
the invariants behind them (see `components.h`). Match that when adding to headers. Units belong
in the comment on every physical field (metres, radians, m/s, seconds).
