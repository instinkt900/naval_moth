#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Live tuning for the TMA solver's confidence gates, shared by the solver that
    // reads it and the debug panel that edits it (the AggroTuningRef pattern), so
    // the two stay in step while dialling the feature in at runtime.
    //
    // These are conditioning thresholds, not accuracy ones: the passive bearings
    // fed to the solver are exact — Contact::trueBearing, the cut without its
    // display noise — so a fit is the *correct* target state whenever the geometry
    // is observable at all, and observability only measures how well-conditioned
    // that fit is (a slow bearing sweep over tens of km is correct but
    // ill-conditioned). The wandering cut the operator sees (Contact::bearing) is a
    // display layer over this: the solver deliberately works the clean geometry so
    // ranges stay solvable, and its confidence then tightens that display noise (see
    // Contact::offset). Observability spans orders of magnitude, so confidence maps
    // from it on a log scale between minObs and obsFull.
    struct TmaTuning {
        float minObs = 1e-8f;      // numerical floor: below this the fit is treated as unobservable
        float obsFull = 1e-4f;     // observability mapping to full confidence (log scale up from minObs)
        float solvedFloor = 0.05f; // least confidence still shown as a solution
        bool showResolutions = false; // debug: overlay each track's cut geometry and its estimate-vs-truth error
    };

    // The one live TMA tuning block, behind an accessor so the solver and the debug
    // sliders touch the same values.
    TmaTuning& TmaTuningRef();

    // Target motion analysis. For each observing ship (one carrying both a
    // ContactPicture and a TrackFile), maintains a passive track per contact it
    // holds only as a bearing, and solves each for a range without going active.
    //
    // A single passive bearing is a ray with no range; only a run of them, taken
    // as own ship manoeuvres, pins the contact down (PLAN's *Active versus
    // passive*). So this accumulates bearing cuts over time into each track and
    // fits a constant-velocity solution to them, which becomes observable — and
    // the solution's confidence rises — only once own ship has bent its own track.
    // Steam straight and the range stays ambiguous; manoeuvre deliberately and a
    // firing solution falls out that cost no emission.
    //
    // Reads each observer's ContactPicture (built by UpdateSensors) and its own
    // Physics position; mutates its TrackFile. It consumes the picture's bearings
    // rather than recomputing them, so it must run *after* UpdateSensors and, like
    // it, before the world step — the cut and the own-ship position it is paired
    // with must be from the same instant.
    void UpdateTMA(entt::registry& registry, float dt);
}
