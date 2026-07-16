# Sound effect licensing

Applies to the `.wav` files in this directory only. The rest of naval_moth is
under the project's own terms; nothing here is ours.

## Source

Sounds from **[GFX Sounds](https://gfxsounds.com)**, downloaded on a **free
account** under their
[Standard License](https://gfxsounds.com/licensing/standard-license/).

## Terms that apply to us

Quoting the license as it read when these were downloaded (2026-07-16):

- **Attribution is required.** *"you must credit us in the form of a link back to
  our website."* This is waived only for Creator or Pro plans — *"If you have a
  Creator or Pro active plan, attribution is not required"* — and these were
  taken on the free plan, so it applies.

  **The credit: sound effects from [GFX Sounds](https://gfxsounds.com).**

- **Non-commercial only.** *"If you will get money from your videos through ads
  or you will get money from your projects, you need a Commercial License."* The
  free plan does not include one — *"Our Pro plan include Commercial License"* —
  so this game must not be sold or monetised while these files are in it.

## Known gaps

The Standard License does not say anything about **redistributing** the raw
sound files, either standalone or inside a source repository. These files are
committed to a public repo, which is a decision taken with that silence in mind
rather than on any permission the license actually grants. If naval_moth ever
becomes something people are asked to pay for, or the sounds need to be shipped
under clearer terms, both of these want revisiting — the sounds are referenced
by id in `assets/data/sounds.json`, so swapping the files out is a content
change, not a code one.

## Files

| File | Sound id |
|---|---|
| `Pirate-ship-cannon-single-fire-332.wav` | `gun_heavy` |
| `Armored-tank-cannon-fire-single-shot-288.wav` | `gun_medium` |
| `Large-explosion-or-cannon-in-a-close-perspective-450.wav` | `impact_large`, `explosion` |
| `Swimming-pool-dive-in-with-a-splash-414.wav` | `splash_large` |
