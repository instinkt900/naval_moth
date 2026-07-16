# Sound effects

Licensing and attribution for these files: see `LICENSE.md`. **Attribution is
required** — credit is owed to [GFX Sounds](https://gfxsounds.com) — and the
sounds are **non-commercial only**.

Files are named by `assets/data/sounds.json`, which is where volume and pitch
variance are authored too. WAV only: the miniaudio build is trimmed to that
decoder (see `CMakeLists.txt`).

A sound id whose file is missing or won't decode is warned about at startup and
left silent, so the game runs with any subset of these present. That is why the
placeholders below are not a problem — those sounds simply don't play yet.

| Sound id | File | Used by |
|---|---|---|
| `gun_heavy` | `Pirate-ship-cannon-single-fire-332.wav` | `broadside_gun`, `mark-7` |
| `gun_medium` | `Armored-tank-cannon-fire-single-shot-288.wav` | `mark-12` |
| `impact_large` | `Large-explosion-or-cannon-in-a-close-perspective-450.wav` | `large_shell`, `ap_mark_8`, `shell_127` striking a hull |
| `explosion` | `Large-explosion-or-cannon-in-a-close-perspective-450.wav` | `corsair`, `hulk` destroyed |
| `splash_large` | `Swimming-pool-dive-in-with-a-splash-414.wav` | `large_shell`, `ap_mark_8`, `shell_127` falling short |
| `gun_light` | *(placeholder — no file yet)* | `auto_gun` |
| `cannon` | *(placeholder — no file yet)* | `broadside_cannon` |
| `impact_small` | *(placeholder — no file yet)* | `cannonball`, `basic_shell` striking a hull |
| `splash_small` | *(placeholder — no file yet)* | `cannonball`, `basic_shell` falling short |

Keep them short and dry. Volume and pitch variation are applied at playback from
`sounds.json`, and distance and zoom fade from the camera on top of that, so a
sample that is already loud, long-tailed or pre-varied fights all three.
