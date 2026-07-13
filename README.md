# Naval Moth

A top-down naval combat game built on the [moth](https://github.com/instinkt900) C++ workspace — [moth_graphics](https://github.com/instinkt900/moth_graphics) for rendering and UI, [Box2D](https://box2d.org/) for physics and collision, and [EnTT](https://github.com/skypjack/entt) for the entity/component model.

Ships are heavy, momentum-driven vessels: they turn slowly, carry their speed, and cannot stop on a dime. The player issues intent — click a point and the selected ship works out how to turn and power toward it, easing off as it arrives — while combat plays out automatically once weapons have valid targets. This is an early-development project.

## Building

Uses Conan 2.x + CMake presets, and requires a C++17 compiler.

### Add the Conan remote

moth_graphics (and its `moth_ui` dependency) are published to an Artifactory remote rather than Conan Center, so Conan can't resolve them from the default `conancenter` remote. Register the remote once before installing:

```bash
conan remote add moth https://artifactory.matthewcotton.net/artifactory/api/conan/conan-local
```

The remote is publicly readable, so no login is required to install.

### Prerequisites

Set up a Python virtual environment and install Conan:

```bash
# Linux / macOS
python3 -m venv .venv
source .venv/bin/activate
pip install conan

# Windows (PowerShell)
python3 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install conan
```

A `.conan/profile` is provided that sets `compiler.cppstd=17` and configures Conan to install system packages automatically. It can be used directly or as a reference when building locally.

### Linux

On Linux, SDL2, SDL_image, SDL_ttf, GLFW, FreeType, and HarfBuzz must come from the system package manager (mixing Conan-built and system copies of these libraries causes runtime conflicts). Using `.conan/profile`, Conan installs them automatically via `apt`:

```bash
conan install . -pr .conan/profile -s build_type=Release --build=missing
cmake --preset conan-release
cmake --build --preset conan-release
```

For a Debug build, replace `Release` / `conan-release` with `Debug` / `conan-debug`.

### Windows

```bash
conan install . -pr .conan/profile -s build_type=Release --build=missing
cmake --preset conan-default
cmake --build --preset conan-release
```

## Dependencies

| Dependency | Source |
|---|---|
| moth_graphics ≥ 1.1.0 | Conan (moth remote) |
| box2d 2.4.1 | Conan |
| entt 3.13.2 | Conan |

## License

MIT
