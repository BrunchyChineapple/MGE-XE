
#include "proxydx/d3d8header.h"
#include "mgedinput.h"
#include "configuration.h"
#include "distantland.h"
#include "distantshader.h"
#include "postshaders.h"
#include "mwbridge.h"
#include "../../../source/d3d8.hpp"

#ifdef MGE_RTX
#include "remix_api_test.h"
#include "retained_world.h"
#define REMIX_ALLOW_X86
#include "remix_c.h"
#include "sky_config.h"   // skyConfig() — live MCM/env master toggles (distant fog, constellations, meteors)
#include <cmath>
#include <cstdio>
#include <cstdint>
#endif



using std::string;
using std::unordered_map;

// The shared update vector remains host-owned until an acknowledged RPC completion.
// These flags prevent a later frame from truncating or replacing an in-flight batch.
static bool s_dynVisUpdatePending = false;
static bool s_dynVisPendingBatchComplete = true;
static bool s_dynVisPendingRetainedOnly = false;
static const void* s_dynVisPendingTransitionToken = nullptr;

#ifdef MGE_RTX
// File-scope state for syncRemixSky's self-healing watchdog and the
// weather-blender re-push gate. Both live outside the function so the
// interior-gating block (which early-returns) can poke s_*lastPushedTarget
// to force the next exterior frame to re-issue the WeatherBlender target.
//
// volatile because the interior-gate write and the main-body read happen
// from the same thread but the compiler should not be free to hoist or
// elide the read across the early-return path.
volatile DWORD s_syncRemixSky_lastPushedTarget = 0xFFFFFFFFu;
static  DWORD s_syncRemixSky_watchdogFrames    = 0u;

// Watchdog cadence. Every kSyncRemixSky_WatchdogPeriod frames in exterior,
// force a full re-push of all master toggles + the WeatherBlender target.
// 240 frames ≈ 4s at 60fps, ≈ 2.7s at 90fps — fast enough to self-heal a
// transient API-state desync without spamming SetConfigVariable when
// nothing's wrong. The watchdog only runs in exterior cells; interior is
// already self-correcting because the interior-gate block clears the
// last-pushed-target sentinel on every interior frame.
static const DWORD kSyncRemixSky_WatchdogPeriod = 240u;

// ----------------------------------------------------------------------------
// Sky reliability guard (2026-06-11): make syncRemixSky "nuke-proof" against
// scenegraph teardown/rebuild during loads, teleports, and interior<->exterior
// transitions. Cost is a couple of cheap reads + isfinite-style checks per frame
// plus edge-triggered resets; nothing new runs heavy continuously.
//
//  * Freeze: while a load screen is up the scenegraph is mid-rebuild, so all
//    reads (sun/moon nodes, weather, daysPassed) are stale/garbage. We push
//    nothing and let Remix hold its last-good Derived state (no garbage in).
//  * Last-good cache: every position/phase value is validated (finite, sane)
//    before it is pushed; the last validated value is cached so a transition
//    can re-assert a correct sky immediately instead of waiting for a fresh read.
//  * Edge resync: on a load-screen falling edge OR a cell-token change we force
//    a full re-push (clear the weather sentinel + re-assert cached sun/moon/phase
//    the SAME frame), so the sky can't stay latched in a half-updated state
//    (the "sun stuck below horizon / moons black or frozen until reload" class).
//  * The 240-frame time-watchdog above remains as a final backstop.
struct SkySyncCache {
    bool  haveSun   = false; float sunElev    = 0.0f,  sunRot    = 0.0f;
    bool  haveMoon0 = false; float moon0Elev  = -20.0f, moon0Rot = 180.0f;
    bool  haveMoon1 = false; float moon1Elev  = -20.0f, moon1Rot = 180.0f;
    bool  havePhase = false; float moon0Phase = 0.5f,  moon1Phase = 0.5f;
};
static SkySyncCache s_skyCache;
static DWORD s_syncRemixSky_lastCellToken = 0xFFFFFFFFu;
static bool  s_syncRemixSky_wasLoading    = false;

// Finite + sane-magnitude guard: self-compare catches NaN, the bound catches inf.
static inline bool skySane(float v) { return (v == v) && v > -1.0e18f && v < 1.0e18f; }

// Sync Morrowind's sun position to Remix's Hillaire physical sky.
static void syncRemixSky() {
    remixapi_Interface* api = RemixAPITest::getInterface();
    if (!api || !api->SetConfigVariable) return;

    // Live master toggles (MGE Sky MCM / mge_sky.cfg, polled ~500ms; env fallback). The wrapper
    // honors these when driving the Derived layer, so the toggles are not stomped by our own writes.
    const SkyConfig& sky = skyConfig();

    auto mwBridge = MWBridge::get();
    if (!mwBridge->IsLoaded()) return;

    // --- Sky reliability guard: freeze during scenegraph rebuild ---
    // While a load screen is up, Morrowind is tearing down / rebuilding the
    // cell + sky scenegraph; sun/moon node reads, weather, and daysPassed are
    // stale or garbage on these frames. Push nothing and hold Remix's last-good
    // Derived state. The falling edge (below) then forces one clean resync.
    if (mwBridge->IsLoadScreen()) {
        s_syncRemixSky_wasLoading = true;
        return;
    }

    // --- Sky reliability guard: edge-triggered full resync ---
    // Fire on a load-screen falling edge (any load/teleport just finished) OR a
    // cell-identity change (interior<->exterior, different interior, load-into-
    // cell). All exterior cells share one token so ordinary border crossings
    // don't thrash. On resync we clear the weather sentinel so the target
    // re-fires, and (in the exterior path below) re-assert cached sun/moon/phase
    // the same frame. Cheap: 1-2 reads, edge-triggered.
    const DWORD cellToken = mwBridge->IsExterior()
        ? 0xE0000000u
        : (0x10000000u | (mwBridge->IntCurCellAddr() & 0x0FFFFFFFu));
    bool forceResync = false;
    if (s_syncRemixSky_wasLoading) { forceResync = true; s_syncRemixSky_wasLoading = false; }
    if (cellToken != s_syncRemixSky_lastCellToken) { forceResync = true; s_syncRemixSky_lastCellToken = cellToken; }
    if (forceResync) {
        s_syncRemixSky_lastPushedTarget = 0xFFFFFFFFu;  // re-fire weather target below
    }

    // Volumetric fog: the legacy D3D fog REMAP is RE-ENABLED in exteriors. Disabling it was a mistake:
    // the remap is what supplies the flat in-scatter floor (multiScatteringEstimate = fogColor *
    // fogRemapColorMultiscatteringScale) AND the medium density. That flat floor is what makes fog
    // actually VISIBLE without the sun-lit fogSunVisibilityGain term (which blows out over water).
    // With gain=0 + remap on you get flat fog glow + extinction and no over-water blowout.
    // (2026-06-13 — reverted the remap-off experiment.)
    bool isExteriorWeather = mwBridge->CellHasWeather();
    bool isExterior = mwBridge->IsExterior();
    api->SetConfigVariable("rtx.volumetrics.enableFogRemap", isExterior ? "True" : "False");
    api->SetConfigVariable("rtx.volumetrics.enableFogColorRemap", isExterior ? "True" : "False");

    // Forward the water-surface world Z so Remix can split the volumetric sun-visibility gain at the
    // water plane: fog below the surface uses rtx.volumetrics.fogSunVisibilityGainUnderwater, fog
    // above it uses the regular gain. Standing on shore, raising the above-water gain for sun shafts
    // otherwise blows the fog seen through the water into a white wall. A very-low sentinel (no water
    // in cell) leaves the split inert -- mirrors the BelowWaterFog sentinel used for distant-land fog.
    const float waterPlaneZ = mwBridge->CellHasWater() ? mwBridge->WaterLevel() : -1.0e9f;
    char waterBuf[32];
    snprintf(waterBuf, sizeof(waterBuf), "%.3f", waterPlaneZ);
    api->SetConfigVariable("rtx.volumetrics.waterPlaneWorldZ", waterBuf);

    // Interior sky gating: when entering a true interior (no weather flag),
    // suppress all sky illumination so light doesn't leak through cracks.
    // The sun position from the last exterior frame is otherwise stuck at a
    // high angle and bleeds onto walls/floors via Remix's atmosphere.
    //
    // We push -90deg sun + zero intensity + disable clouds/moons/stars/milkyway.
    // Cells flagged 'behaves as exterior' (caves with weather, grottos, etc.)
    // pass CellHasWeather() so they never hit this branch and render normally.
    //
    // All target options are flagged NoSave in rtx_options.h so these writes
    // route to the Derived layer and never pollute user.conf / rtx.conf.
    if (!isExteriorWeather) {
        api->SetConfigVariable("rtx.atmosphere.sunElevation", "-90.0");
        api->SetConfigVariable("rtx.atmosphere.sunIntensity", "0.0");
        api->SetConfigVariable("rtx.atmosphere.cloudEnabled", "False");
        api->SetConfigVariable("rtx.atmosphere.moon0.enabled0", "False");
        api->SetConfigVariable("rtx.atmosphere.moon1.enabled1", "False");
        api->SetConfigVariable("rtx.atmosphere.starBrightness", "0.0");
        api->SetConfigVariable("rtx.atmosphere.nightSkyBrightness", "0.0");
        api->SetConfigVariable("rtx.atmosphere.milkyWayEnabled", "False");
        // Constellation overlay is night-only but bypasses starBrightness;
        // explicitly suppress it so it doesn't bleed through interior cracks.
        api->SetConfigVariable("rtx.atmosphere.constellationsEnabled", "False");
        // Meteors run independent of starBrightness too — sporadic background
        // (meteorBaseRate) plus calendar showers both need a master gate.
        api->SetConfigVariable("rtx.atmosphere.meteorsEnabled", "False");
        // No exterior volumetric fog medium bleeding into true interiors.
        api->SetConfigVariable("rtx.volumetrics.enable", "False");

        // Force the WeatherBlender into dormant mode while we're in an
        // interior. Without this, the blender keeps lerping the last
        // exterior preset and writes nightSkyBrightness (~0.008) to the
        // Derived layer every frame via writeBlendedToDerivedLayer,
        // overwriting our nightSkyBrightness="0.0" interior write —
        // observable as faint airglow leaking through cell-edge cracks.
        // Same shape as the cloudWindSpeed regression in QUICKSTART
        // override #4. Clearing __weather.target trips the dormancy gate
        // in WeatherBlender::update so it stops writing entirely. On
        // exterior return, the s_lastPushedTarget reset above guarantees
        // a fresh SetGameValue("__weather.target", ...) below, which
        // re-arms the blender naturally — no extra restore code needed.
        api->SetGameValue("__weather.target", "");

        // Force the weather blender to re-push its target on the first
        // exterior frame after this interior one. Without this, when the
        // player transitions interior → exterior to the same weather they
        // had before going inside, s_lastPushedTarget below matches the
        // current target and we skip the SetGameValue. The blender then
        // stays frozen at its pre-interior blended state and overwrites
        // our exterior-restore writes via writeBlendedToDerivedLayer —
        // observable as "stars missing, moons unlit, sun never rises"
        // after teleporting around.
        s_syncRemixSky_lastPushedTarget = 0xFFFFFFFFu;
        return;
    }

    // Exterior return: restore the defaults the wrapper drove down for
    // interior gating. The actual sun/moon positions are written below.
    api->SetConfigVariable("rtx.atmosphere.sunIntensity", "1.0");
    api->SetConfigVariable("rtx.atmosphere.cloudEnabled", "True");
    api->SetConfigVariable("rtx.atmosphere.moon0.enabled0", "True");
    api->SetConfigVariable("rtx.atmosphere.moon1.enabled1", "True");
    api->SetConfigVariable("rtx.atmosphere.starBrightness", "1.0");
    api->SetConfigVariable("rtx.atmosphere.nightSkyBrightness", "0.008");
    // Constellation overlay + meteors + distant fog: gated by the live MGE Sky toggles. The per-frame
    // writes were stomping the Derived-layer UI checkboxes; now the wrapper honors the user's master
    // toggle instead. The interior gating block above already forces all three off (bleed-through).
    api->SetConfigVariable("rtx.atmosphere.constellationsEnabled", sky.constellations ? "True" : "False");
    api->SetConfigVariable("rtx.atmosphere.meteorsEnabled", sky.meteors ? "True" : "False");
    api->SetConfigVariable("rtx.volumetrics.enable", sky.distantFog ? "True" : "False");
    // milkyWayEnabled: drive True on exterior return to match the
    // source-level default we flipped to true for this project. The
    // interior block writes False; the symmetrical write here keeps
    // the Derived layer in sync as the player crosses cell boundaries.
    // user.conf can still override (User layer wins over Derived).
    api->SetConfigVariable("rtx.atmosphere.milkyWayEnabled", "True");

    // Watchdog: every kWatchdogPeriod exterior frames, force a full
    // resync — clear the WeatherBlender's last-pushed target sentinel
    // so the SetGameValue below re-fires unconditionally. This is a
    // belt-and-suspenders self-heal for any racy API-state desync we
    // haven't caught explicitly (e.g. teleport-induced blender freeze).
    // Cheap: ~5 SetConfigVariable + 1 SetGameValue every 4 seconds.
    if ((++s_syncRemixSky_watchdogFrames % kSyncRemixSky_WatchdogPeriod) == 0u) {
        s_syncRemixSky_lastPushedTarget = 0xFFFFFFFFu;
    }

    // On a resync edge, re-assert the last-good sun/moon/phase immediately so the
    // freshly-restored exterior sky is correct THIS frame, before new scenegraph
    // reads settle. This closes the post-transition gap that otherwise shows as
    // "sun stuck below horizon", "moons black" (moon enabled while the stale sun
    // sits at nadir), or "moon frozen". Fresh valid reads below overwrite these.
    if (forceResync) {
        char rb[64];
        if (s_skyCache.haveSun) {
            snprintf(rb, sizeof(rb), "%.2f", s_skyCache.sunElev);
            api->SetConfigVariable("rtx.atmosphere.sunElevation", rb);
            snprintf(rb, sizeof(rb), "%.2f", s_skyCache.sunRot);
            api->SetConfigVariable("rtx.atmosphere.sunRotation", rb);
        }
        if (s_skyCache.haveMoon0) {
            snprintf(rb, sizeof(rb), "%.2f", s_skyCache.moon0Elev);
            api->SetConfigVariable("rtx.atmosphere.moon0.elevation0", rb);
            snprintf(rb, sizeof(rb), "%.2f", s_skyCache.moon0Rot);
            api->SetConfigVariable("rtx.atmosphere.moon0.rotation0", rb);
        }
        if (s_skyCache.haveMoon1) {
            snprintf(rb, sizeof(rb), "%.2f", s_skyCache.moon1Elev);
            api->SetConfigVariable("rtx.atmosphere.moon1.elevation1", rb);
            snprintf(rb, sizeof(rb), "%.2f", s_skyCache.moon1Rot);
            api->SetConfigVariable("rtx.atmosphere.moon1.rotation1", rb);
        }
        if (s_skyCache.havePhase) {
            snprintf(rb, sizeof(rb), "%.4f", s_skyCache.moon0Phase);
            api->SetConfigVariable("rtx.atmosphere.moon0.phase0", rb);
            snprintf(rb, sizeof(rb), "%.4f", s_skyCache.moon1Phase);
            api->SetConfigVariable("rtx.atmosphere.moon1.phase1", rb);
        }
    }

    float sx, sy, sz;
    mwBridge->GetSunDir(sx, sy, sz);
    float len = sqrtf(sx * sx + sy * sy + sz * sz);
    bool haveSun = (len >= 0.001f);
    if (haveSun) {
        sx /= len; sy /= len; sz /= len;
    }

    // Only push sun position when the scenegraph gives us a valid direction.
    // On a freshly-loaded cell the sun node can briefly read all zeroes;
    // skip just this push and let the rest of the sky update proceed so
    // we don't lose moons/weather/clouds for the same frame. The previous
    // tick's sun position remains in Remix's Derived layer, which is fine
    // because the bad reading clears within a frame or two.
    if (haveSun) {
        float elevation = asinf(std::max(-1.0f, std::min(1.0f, sz))) * (180.0f / 3.14159265f);
        float rotation = atan2f(sx, sy) * (180.0f / 3.14159265f);

        // Morrowind's scenegraph bounces the sun at the horizon — use game hour
        // to detect night and push the sun below the horizon for Remix.
        float hour = mwBridge->getGameHour();
        if (hour < 6.0f || hour > 20.0f) {
            elevation = -elevation;
        } else if (hour < 8.0f) {
            float t = (hour - 6.0f) / 2.0f;
            elevation = elevation * (2.0f * t - 1.0f);
        } else if (hour > 18.0f) {
            float t = (hour - 18.0f) / 2.0f;
            elevation = elevation * (1.0f - 2.0f * t);
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", elevation);
        api->SetConfigVariable("rtx.atmosphere.sunElevation", buf);

        snprintf(buf, sizeof(buf), "%.2f", rotation);
        api->SetConfigVariable("rtx.atmosphere.sunRotation", buf);

        // Cache the validated sun position for transition re-assertion.
        if (skySane(elevation) && skySane(rotation)) {
            s_skyCache.haveSun = true;
            s_skyCache.sunElev = elevation;
            s_skyCache.sunRot  = rotation;
        }
    }
    char buf[64];

    // ================================================================
    // Moon positions: read directly from Morrowind's scenegraph.
    // GetMoonDir returns the normalized world-space direction to the
    // vanilla billboard moon. Morrowind uses Z-up (x=east, y=north,
    // z=up), so elevation = asin(z), rotation = atan2(x, y).
    // This gives us pixel-perfect match with vanilla moon positions.
    //
    // Per the 2026-05-06 merge with RemixProjGroup Kim's fork, Remix
    // now uses an indexed moon array (rtx.atmosphere.moon0/moon1/...).
    // Secunda = moon0, Masser = moon1.
    // ================================================================
    float mx, my, mz;

    // Camera position for moon direction calculation. The moon billboard
    // is parented to the sky root which tracks the camera, so its world
    // position = cameraPos + skyDirection * domeRadius. Subtracting the
    // camera (not the player) eliminates parallax when crouching or
    // orbiting in third person.
    float camX = DistantLand::eyePos.x;
    float camY = DistantLand::eyePos.y;
    float camZ = DistantLand::eyePos.z;

    // Secunda (smaller, brighter moon) — moon0
    if (mwBridge->GetMoonDir(true, mx, my, mz)) {
        float dx = mx - camX, dy = my - camY, dz = mz - camZ;
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 0.001f) {
            dx /= len; dy /= len; dz /= len;
            float moonElev = asinf(std::max(-1.0f, std::min(1.0f, dz))) * (180.0f / 3.14159265f);
            float moonRot = atan2f(dx, dy) * (180.0f / 3.14159265f);

            snprintf(buf, sizeof(buf), "%.2f", moonElev);
            api->SetConfigVariable("rtx.atmosphere.moon0.elevation0", buf);

            snprintf(buf, sizeof(buf), "%.2f", moonRot);
            api->SetConfigVariable("rtx.atmosphere.moon0.rotation0", buf);

            if (skySane(moonElev) && skySane(moonRot)) {
                s_skyCache.haveMoon0 = true;
                s_skyCache.moon0Elev = moonElev;
                s_skyCache.moon0Rot  = moonRot;
            }
        }
    } else {
        // Moon not in scenegraph (loading screen, interior, etc.) — hide it
        api->SetConfigVariable("rtx.atmosphere.moon0.elevation0", "-20.0");
        api->SetConfigVariable("rtx.atmosphere.moon0.rotation0", "180.0");
        // Cache the hidden state so a resync re-asserts "hidden" (correct for a
        // moon that has genuinely set), not a stale above-horizon position.
        s_skyCache.haveMoon0 = true;
        s_skyCache.moon0Elev = -20.0f;
        s_skyCache.moon0Rot  = 180.0f;
    }

    // Masser (larger, red moon) — moon1, completely independent orbit
    if (mwBridge->GetMoonDir(false, mx, my, mz)) {
        float dx = mx - camX, dy = my - camY, dz = mz - camZ;
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 0.001f) {
            dx /= len; dy /= len; dz /= len;
            float masserElev = asinf(std::max(-1.0f, std::min(1.0f, dz))) * (180.0f / 3.14159265f);
            float masserRot = atan2f(dx, dy) * (180.0f / 3.14159265f);

            snprintf(buf, sizeof(buf), "%.2f", masserElev);
            api->SetConfigVariable("rtx.atmosphere.moon1.elevation1", buf);

            snprintf(buf, sizeof(buf), "%.2f", masserRot);
            api->SetConfigVariable("rtx.atmosphere.moon1.rotation1", buf);

            if (skySane(masserElev) && skySane(masserRot)) {
                s_skyCache.haveMoon1 = true;
                s_skyCache.moon1Elev = masserElev;
                s_skyCache.moon1Rot  = masserRot;
            }
        }
    } else {
        api->SetConfigVariable("rtx.atmosphere.moon1.elevation1", "-20.0");
        api->SetConfigVariable("rtx.atmosphere.moon1.rotation1", "180.0");
        s_skyCache.haveMoon1 = true;
        s_skyCache.moon1Elev = -20.0f;
        s_skyCache.moon1Rot  = 180.0f;
    }

    // ================================================================
    // Moon phases: synced to Morrowind's actual lunar cycles.
    // Morrowind uses 8 discrete phases per moon:
    //   0=new, 1=waxing crescent, 2=first quarter, 3=waxing gibbous,
    //   4=full, 5=waning gibbous, 6=third quarter, 7=waning crescent
    //
    // Our shader's getPhaseIllumination() maps phase [0..1] as:
    //   0.0 = new (dark), 0.5 = full (bright), 1.0 = new again
    //
    // So vanilla phase index 0 (new) → 0.0, index 4 (full) → 0.5.
    // Direct mapping: shaderPhase = phaseIndex / 8.0
    // We add 0.5/8 to center within each 2-day (or 3-day) step.
    //
    // Secunda: 16-day cycle (2 days per phase)
    // Masser:  24-day cycle (3 days per phase)
    // ================================================================
    int daysPassed = mwBridge->getDaysPassed();

    int secundaPhaseIndex = ((int)daysPassed % 16) / 2;  // 0..7
    float secundaPhase = ((float)secundaPhaseIndex + 0.5f) / 8.0f;

    int masserPhaseIndex = ((int)daysPassed % 24) / 3;  // 0..7
    float masserPhase = ((float)masserPhaseIndex + 0.5f) / 8.0f;

    if (daysPassed >= 0) {
        // Valid calendar read — push and cache.
        snprintf(buf, sizeof(buf), "%.4f", secundaPhase);
        api->SetConfigVariable("rtx.atmosphere.moon0.phase0", buf);

        snprintf(buf, sizeof(buf), "%.4f", masserPhase);
        api->SetConfigVariable("rtx.atmosphere.moon1.phase1", buf);

        s_skyCache.havePhase  = true;
        s_skyCache.moon0Phase = secundaPhase;
        s_skyCache.moon1Phase = masserPhase;
    } else if (s_skyCache.havePhase) {
        // Garbage daysPassed read (mid-transition): re-assert last-good phase
        // instead of letting it collapse to new-moon (phase 0 = unlit = the
        // "moons are 100% black" symptom).
        snprintf(buf, sizeof(buf), "%.4f", s_skyCache.moon0Phase);
        api->SetConfigVariable("rtx.atmosphere.moon0.phase0", buf);
        snprintf(buf, sizeof(buf), "%.4f", s_skyCache.moon1Phase);
        api->SetConfigVariable("rtx.atmosphere.moon1.phase1", buf);
    }

    // ================================================================
    // Weather presets: drive Kim's WeatherBlender via SetGameValue.
    // Maps Morrowind's 10 weather IDs to Remix weather preset names.
    // The blender handles smooth interpolation internally — we only
    // fire SetGameValue when the target weather changes, not every
    // frame. Morrowind's own transition ratio drives blend_seconds so
    // the visual blend tracks the game's weather transition timing.
    // ================================================================
    static const char* weatherPresetMap[] = {
        "clear",         // 0 = Clear
        "partlyCloudy",  // 1 = Cloudy
        "foggy",         // 2 = Foggy
        "overcast",      // 3 = Overcast
        "rainstorm",     // 4 = Rain
        "thunderstorm",  // 5 = Thunder
        "sandstorm",     // 6 = Ash
        "sandstorm",     // 7 = Blight
        "snow",          // 8 = Snow
        "blizzard",      // 9 = Blizzard
    };
    constexpr int kWeatherPresetCount = 10;

    DWORD curW = mwBridge->GetCurrentWeather();
    DWORD nxtW = mwBridge->GetNextWeather();
    float ratio = mwBridge->GetWeatherRatio();  // 0 = fully current, 1 = fully next
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    // Determine the effective target: if a transition is in progress
    // (ratio > 0), the target is the next weather; otherwise current.
    DWORD effectiveTarget = (ratio > 0.01f && nxtW < (DWORD)kWeatherPresetCount)
                            ? nxtW : curW;
    if (effectiveTarget >= (DWORD)kWeatherPresetCount)
        effectiveTarget = 0;

    const char* targetPreset = weatherPresetMap[effectiveTarget];

    // Only push SetGameValue when the target actually changes to avoid
    // resetting the blender's internal lerp state every frame. The
    // file-scope volatile is referenced by the interior-gating block
    // above to force a re-push on exterior return (so the blender
    // doesn't stay frozen across interior teleports). Watchdog below
    // also clears it periodically as a self-healing mechanism.
    if (effectiveTarget != s_syncRemixSky_lastPushedTarget) {
        s_syncRemixSky_lastPushedTarget = effectiveTarget;

        // Blend duration: Morrowind transitions take ~20-30 seconds
        // depending on weather pair. Use a fixed 20s default that
        // feels natural; the blender handles mid-blend retargeting
        // cleanly if weather changes again before completion.
        api->SetGameValue("__weather.blend_seconds", "20.0");
        api->SetGameValue("__weather.target", targetPreset);
    }

    // Volumetric fog density: for foggy (2) and rain (4) weather, set
    // both legacy max distance bounds to 40m (forces thick fog). All
    // other weathers use defaults (min=1, max=40) which lets the fog
    // remap system scale naturally with Morrowind's D3D fog distance.
    bool isThickFogWeather = (effectiveTarget == 2 || effectiveTarget == 4);
    api->SetConfigVariable("rtx.volumetrics.fogRemapMaxDistanceMinMeters",
                           isThickFogWeather ? "40.0" : "1.0");
    api->SetConfigVariable("rtx.volumetrics.fogRemapMaxDistanceMaxMeters", "40.0");

    // Cloud wind: derive speed and direction from Morrowind's wind state
    // per-frame. windScaling (0–1) is already blended between current and
    // next weather by adjustFog() which runs before syncRemixSky().
    // Both options are NoSave in rtx_options.h so these writes go to the
    // Derived layer and never pollute rtx.conf or user.conf.
    //
    // Speed range: 0.078 (calm) → 0.108 (storm). The earlier 0.110–0.155
    // band felt too fast across the board — clouds were visibly racing
    // even on clear days. Scaled the whole curve down ~30% (kept the
    // calm/storm dynamic range proportional, just shifted slower) so
    // the user-perceived "120-ish in normal weather" sits closer to ~85
    // and storms top out at 108 instead of 155. Tunable here without a
    // rebake; if it ever needs to be a runtime knob, expose two
    // RTX_OPTIONs (NoSave) for base + slope and read them in.
    {
        float cloudSpeed = 0.078f + DistantLand::windScaling * 0.030f;
        if (cloudSpeed > 0.108f) cloudSpeed = 0.108f;
        snprintf(buf, sizeof(buf), "%.4f", cloudSpeed);
        api->SetConfigVariable("rtx.atmosphere.cloudWindSpeed", buf);

        const float* wind = mwBridge->GetWindVector();
        float windMag = sqrtf(wind[0] * wind[0] + wind[1] * wind[1]);
        if (windMag > 0.001f) {
            float windDir = atan2f(wind[1], wind[0]) * (180.0f / 3.14159265f);
            if (windDir < 0.0f) windDir += 360.0f;
            snprintf(buf, sizeof(buf), "%.1f", windDir);
            api->SetConfigVariable("rtx.atmosphere.cloudWindDirection", buf);
        }
    }

    // ================================================================
    // Meteor shower scheduler.
    //
    // Combines two activity sources:
    //   1) Six canonical Morrowind meteor events scheduled by in-game date
    //      (Wisp's Tongue, Vampire Star, Sun's Tears, Lunar Dance,
    //       Tower's Spark, End-of-Year Burst). Each has a peak day, hour,
    //       duration, radiant point, and per-shower peak rate multiplier.
    //   2) Random unscheduled showers — deterministic per-day hash, rolls
    //      ~1/45 chance per in-game day (~8/year), randomized radiant +
    //      intensity. Caught surprise events on top of the canon list.
    //
    // Each frame we compute the activity envelope (triangular over day-of-
    // year * gaussian over hour-of-night) for both sources and pick the
    // higher one, pushing meteorShowerActivity / meteorRadiantElevation /
    // meteorRadiantRotation. All three options are NoSave in Remix so the
    // writes go to the Derived layer.
    //
    // Meteors render via Remix's atmosphere shooting-star path; activity=0
    // means baseline rate only, activity=1 means full peak rate.
    // ================================================================
    {
        // Morrowind calendar: 12 months, 365 days/year, Earth-like month lengths.
        // First-of-month day-of-year offsets (0-indexed, Jan 1 = day 0).
        static const int kMonthStart[12] = {
            0,    // Morning Star (Jan)
            31,   // Sun's Dawn   (Feb)
            59,   // First Seed   (Mar)
            90,   // Rain's Hand  (Apr)
            120,  // Second Seed  (May)
            151,  // Mid Year     (Jun)
            181,  // Sun's Height (Jul)
            212,  // Last Seed    (Aug)
            243,  // Hearthfire   (Sep)
            273,  // Frost Fall   (Oct)
            304,  // Sun's Dusk   (Nov)
            334   // Evening Star (Dec)
        };

        struct MeteorEvent {
            int month;          // 0=Morning Star ... 11=Evening Star
            int dayOfMonth;     // 1-based
            float hourPeak;     // 24h
            float hourSpread;   // gaussian sigma in hours
            int durationDays;   // total days the event spans (centered on peak day)
            float peakRate;     // 0..1 multiplier
            float radiantElev;  // degrees
            float radiantRot;   // degrees azimuth
        };
        static const MeteorEvent kEvents[] = {
            { 1,  14, 0.0f,  2.5f, 5, 0.5f, 50.0f, 200.0f }, // Wisp's Tongue   - Sun's Dawn 14
            { 5,  21, 2.0f,  2.0f, 3, 0.3f, 65.0f,  90.0f }, // Vampire Star    - Mid Year 21
            { 7,  12, 4.0f,  2.5f, 7, 1.0f, 45.0f, 180.0f }, // Sun's Tears     - Last Seed 12
            { 9,  18, 0.0f,  2.0f, 5, 0.4f, 70.0f, 270.0f }, // Lunar Dance     - Frost Fall 18
            { 10,  7, 22.0f, 2.5f, 7, 0.6f, 55.0f,   0.0f }, // Tower's Spark   - Sun's Dusk 7
            { 11, 31, 0.0f,  2.0f, 3, 1.0f, 60.0f, 150.0f }, // End-of-Year     - Evening Star 31 (clamped to 30 below)
        };
        constexpr int kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);

        int daysPassed = mwBridge->getDaysPassed();
        int doy = daysPassed % 365;       // 0..364
        if (doy < 0) doy += 365;
        float hour = mwBridge->getGameHour();

        auto envelope = [](float center, float spread, float x) -> float {
            // Gaussian centered at 'center' with stddev 'spread'
            float dx = x - center;
            float g = expf(-(dx * dx) / (2.0f * spread * spread));
            return g;
        };

        auto eventActivity = [&](const MeteorEvent& e) -> float {
            int eventDoy = kMonthStart[e.month] + (e.dayOfMonth - 1);
            if (eventDoy >= 365) eventDoy = 364;

            // Triangular envelope over day-of-year, half-width = durationDays/2
            float halfWidth = float(e.durationDays) / 2.0f;
            int dDay = doy - eventDoy;
            // Year-wrap shortest distance
            if (dDay >  182) dDay -= 365;
            if (dDay < -182) dDay += 365;
            float dayEnv = 1.0f - fabsf(float(dDay)) / halfWidth;
            if (dayEnv <= 0.0f) return 0.0f;

            // Gaussian envelope over hour, but only at night (peak at hourPeak).
            // Hour wrap for nights crossing midnight.
            float dh = hour - e.hourPeak;
            if (dh >  12.0f) dh -= 24.0f;
            if (dh < -12.0f) dh += 24.0f;
            float hourEnv = expf(-(dh * dh) / (2.0f * e.hourSpread * e.hourSpread));

            return e.peakRate * dayEnv * hourEnv;
        };

        // Find the highest-activity scheduled event
        float bestActivity = 0.0f;
        float bestRadElev = 60.0f;
        float bestRadRot  = 180.0f;
        for (int i = 0; i < kEventCount; ++i) {
            float a = eventActivity(kEvents[i]);
            if (a > bestActivity) {
                bestActivity = a;
                bestRadElev = kEvents[i].radiantElev;
                bestRadRot  = kEvents[i].radiantRot;
            }
        }

        // Random unscheduled shower: deterministic per-day roll (cubed bias
        // toward minor showers, occasional moderate, rare major). Same day
        // always produces the same outcome, no frame jitter.
        {
            uint32_t seed = uint32_t(daysPassed) * 2654435761u;
            seed ^= seed >> 16; seed *= 0x85ebca6bu;
            seed ^= seed >> 13; seed *= 0xc2b2ae35u;
            seed ^= seed >> 16;
            // Probability ~ 1/45 per in-game day (~8/year)
            uint32_t rollProb = seed % 45u;
            if (rollProb == 0u) {
                // Fire — pull more entropy bytes for the shower's attributes.
                uint32_t s2 = seed * 1664525u + 1013904223u;
                uint32_t s3 = s2 * 1664525u + 1013904223u;
                uint32_t s4 = s3 * 1664525u + 1013904223u;
                uint32_t s5 = s4 * 1664525u + 1013904223u;

                // Intensity: cubed roll biased to minor (~0.15-0.3 typical).
                float u   = float(s2 & 0xFFFFu) / 65535.0f;
                float intensity = u * u * u;     // ~0.0..1.0, cubed bias
                if (intensity < 0.05f) intensity = 0.05f;

                // Peak hour: 20:00 .. 04:00 (most-active part of night)
                float hOff = float(s3 & 0xFFFFu) / 65535.0f;
                float peakHour = 20.0f + hOff * 8.0f;
                if (peakHour >= 24.0f) peakHour -= 24.0f;

                float hourSpread = 1.5f + (float(s4 & 0xFFFFu) / 65535.0f) * 1.0f;
                float radElev    = 30.0f + (float(s5 & 0xFFFFu) / 65535.0f) * 50.0f;
                float radRot     = (float(seed & 0xFFFFu) / 65535.0f) * 360.0f;

                float dh = hour - peakHour;
                if (dh >  12.0f) dh -= 24.0f;
                if (dh < -12.0f) dh += 24.0f;
                float hourEnv = expf(-(dh * dh) / (2.0f * hourSpread * hourSpread));
                float randActivity = intensity * hourEnv;

                if (randActivity > bestActivity) {
                    bestActivity = randActivity;
                    bestRadElev  = radElev;
                    bestRadRot   = radRot;
                }
            }
        }

        // Push the picked activity + radiant. Always push so that off-night
        // frames cleanly drive activity back to 0.
        snprintf(buf, sizeof(buf), "%.4f", bestActivity);
        api->SetConfigVariable("rtx.atmosphere.meteorShowerActivity", buf);
        snprintf(buf, sizeof(buf), "%.2f", bestRadElev);
        api->SetConfigVariable("rtx.atmosphere.meteorRadiantElevation", buf);
        snprintf(buf, sizeof(buf), "%.2f", bestRadRot);
        api->SetConfigVariable("rtx.atmosphere.meteorRadiantRotation", buf);
    }

    // ================================================================
    // Lore-accurate constellation month gating (fork — 2026-05-24)
    // ================================================================
    //
    // Drive rtx.atmosphere.constellationCurrentMonth from the in-game
    // calendar so the shader can highlight the player's birthsign-month
    // constellation. Morrowind's months are 28 days each; doy/28 + 1 gives
    // the 1..12 month index. Push every frame (cheap, NoSave field, never
    // pollutes user.conf). 0 = no highlight if the bridge can't resolve.
    {
        int daysPassed = mwBridge->getDaysPassed();
        int doy = daysPassed % 365;
        if (doy < 0) doy += 365;
        // Morrowind's calendar: 12 months × 28 days = 336 days; the engine
        // wraps daysPassed at 365 but in-fiction month rotation tracks the
        // 12-month structure. Use the standard "month = doy/28 + 1, clamp
        // to 12 on the spillover days" mapping.
        int month = (doy / 28) + 1;
        if (month < 1)  month = 1;
        if (month > 12) month = 12;
        char monthBuf[16];
        snprintf(monthBuf, sizeof(monthBuf), "%d", month);
        api->SetConfigVariable("rtx.atmosphere.constellationCurrentMonth", monthBuf);
    }
}
#endif

// renderStage0 - Render distant land at beginning of scene 0, after sky
void DistantLand::renderStage0() {
    auto mwBridge = MWBridge::get();
    IDirect3DStateBlock9* stateSaved;
    UINT passes;

    // A composite batch may still own the single IPC lane from the prior frame. Poll it
    // before selectDistantCell(), which can itself issue worldspace and catalog commands.
#ifdef MGE_RTX
    const bool refreshDistantVisibility = pollCompositeStreamBatch();
#else
    const bool refreshDistantVisibility = true;
#endif
    if (refreshDistantVisibility) {
        selectDistantCell();
    }

    // Get Morrowind camera matrices
    device->GetTransform(D3DTS_VIEW, &mwView);
    device->GetTransform(D3DTS_PROJECTION, &mwProj);

    // Set variables derived from current game state and camera configuration
    setView(&mwView);
#ifdef MGE_RTX
    RetainedWorld::setupCamera(
        eyePos,
        mwView,
        mwProj,
        kDistantNearPlane,
        Configuration.DL.DrawDist * kCellSize);
    if (mwBridge->IsExterior()) {
        RetainedWorld::prepareCompositeTransition(eyePos, mwView, compositeCache);
    }
    streamAndReconcileComposites();
    const bool drawDistantVisibility = canDrawDistantVisibility();
#endif
    adjustFog();
#ifdef MGE_RTX
    syncRemixSky();
#endif
    setupCommonEffect(&mwView, &mwProj);
    FixedFunctionShader::updateLighting(lightSunMult, lightAmbMult);

    isRenderCached &= (Configuration.MGEFlags & USE_MENU_CACHING) && mwBridge->IsMenu();
    isPPLActive = (Configuration.MGEFlags & USE_FFESHADER) && !(Configuration.PerPixelLightFlags == 1 && !mwBridge->IntCurCellAddr());

    if (!isRenderCached) {
        ///LOG::logline("Sky prims: %d", recordSky.size());

        if (isDistantCell()) {
            // Save state block manually since we can change FVF/decl
            device->CreateStateBlock(D3DSBT_ALL, &stateSaved);
            effect->BeginPass(PASS_SETUP);
            effect->EndPass();

            // Shadow culls share the same IPC lane and visibility vectors as the main pass.
            // Keep the previous shadow map while a composite batch owns that lane.
            if (refreshDistantVisibility &&
                (Configuration.MGEFlags & USE_SHADOWS)) {
                if (mwBridge->CellHasWeather() && !mwBridge->IsMenu()) {
                    effectShadow->Begin(&passes, D3DXFX_DONOTSAVESTATE);
                    renderShadowMap();
                    effectShadow->End();
                }
            }

            // Distant everything; bias the projection matrix such that
            // distant land gets drawn behind anything Morrowind would draw
            D3DXMATRIX distProj = mwProj;
            editProjectionZ(&distProj, kDistantNearPlane - 1e-2, Configuration.DL.DrawDist * kCellSize);
            effect->SetMatrix(ehProj, &distProj);

            effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);

            if (!mwBridge->IsUnderwater(eyePos.z)) {
#ifdef MGE_RTX
                // RTX Remix: Use fixed-function pipeline for distant land rendering.
                // Remix can't extract proper textures from D3DX effect shaders.
                if (mwBridge->IsExterior() && drawDistantVisibility) {
                    RetainedWorld::reconcile(eyePos, mwView, compositeCache);
                    // Distant land terrain is sourced from the heightmap and always
                    // renders here; this draw also couples culling (visLand) with the
                    // draw and feeds the depth pass.
                    renderDistantLandFFP(refreshDistantVisibility);
                }

                if (Configuration.MGEFlags & USE_DISTANT_STATICS) {
                    if (drawDistantVisibility) {
                        if (refreshDistantVisibility) {
                            cullDistantStatics(&mwView, &distProj);
                        }
                        renderDistantStaticsFFP();
                    }
                }
                else {
                    visDistant.RemoveAll();
                }
#else
                // Draw distant landscape
                if (mwBridge->IsExterior()) {
                    effect->BeginPass(PASS_RENDERLAND);
                    renderDistantLand(effect, &mwView, &distProj);
                    effect->EndPass();
                }

                // Draw distant statics, with alpha dissolve as they pass the near view boundary
                if (Configuration.MGEFlags & USE_DISTANT_STATICS) {
                    DWORD p = mwBridge->CellHasWeather() ? PASS_RENDERSTATICSEXTERIOR : PASS_RENDERSTATICSINTERIOR;
                    effect->BeginPass(p);
                    vsr.beginAlphaToCoverage(device);

                    cullDistantStatics(&mwView, &distProj);
                    renderDistantStatics();

                    vsr.endAlphaToCoverage(device);
                    effect->EndPass();
                }
                else {
                    visDistant.RemoveAll();
                }
#endif
            }

            // Sky scattering and sky objects (should be drawn late as possible)
            if ((Configuration.MGEFlags & USE_ATM_SCATTER) && mwBridge->CellHasWeather()) {
                renderSky();
            }

            // Update reflection
#ifndef MGE_RTX
            if (mwBridge->CellHasWater()) {
                renderWaterReflection(&mwView, &distProj);
            }
#else
            // Perf (RTX): skip the raster water-reflection pass entirely. texReflection
            // is sampled ONLY by renderWaterPlane() (renderStageWater, #ifndef MGE_RTX);
            // under RTX water is drawn by renderStageWaterFFP() which never reads it, and
            // Remix ray-traces the reflections. So rendering reflected land + statics +
            // sky into a 1024^2 RT every frame near water is pure dead work here.
#endif

            // Update water simulation
#ifndef MGE_RTX
            if (Configuration.MGEFlags & DYNAMIC_RIPPLES) {
                simulateDynamicWaves();
            }
#else
            // Perf (RTX): skip the dynamic-wave simulation. Its outputs (texRain/texRipples)
            // are sampled ONLY by renderWaterPlane() (renderStageWater, #ifndef MGE_RTX), which
            // does not run under RTX, so the per-frame wave-step / ripple render passes are dead.
#endif

            effect->End();

            // Reset matrices
            effect->SetMatrix(ehView, &mwView);
            effect->SetMatrix(ehProj, &mwProj);

            // Save distant land only frame to texture
            if (~Configuration.MGEFlags & NO_MW_MGE_BLEND) {
                texDistantBlend = PostShaders::borrowBuffer(1);
            }

            // Restore render state
            stateSaved->Apply();
            stateSaved->Release();
        } else {
            // Clear water reflection to avoid seeing previous cell environment reflected
            // Must be done every frame to react to lighting changes
#ifndef MGE_RTX
            clearReflection();
#else
            // Perf (RTX): texReflection is sampled only by renderWaterPlane (#ifndef MGE_RTX),
            // so clearing it every frame is dead work under RTX. (Same reason the reflection
            // render pass above is skipped.)
#endif

            // Update water simulation
#ifndef MGE_RTX
            if (Configuration.MGEFlags & DYNAMIC_RIPPLES) {
                // Save state block manually since we can change FVF/decl
                device->CreateStateBlock(D3DSBT_ALL, &stateSaved);

                effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
                simulateDynamicWaves();
                effect->End();

                // Restore render state
                stateSaved->Apply();
                stateSaved->Release();
            }
#endif
        }
    }

    // Clear stray recordings
    recordMW.clear();
    recordSky.clear();
}

// renderStage1 - Render grass and shadows over near features, and write depth texture for scene 0
void DistantLand::renderStage1() {
    auto mwBridge = MWBridge::get();
    IDirect3DStateBlock9* stateSaved;
    UINT passes;

    ///LOG::logline("Stage 1 prims: %d", recordMW.size());

    if (!isRenderCached) {
        // Save state block manually since we can change FVF/decl
        device->CreateStateBlock(D3DSBT_ALL, &stateSaved);

        // TODO: Locate this properly
        if (isDistantCell()) {
            cullGrass(&mwView, &mwProj);
        }

        if (isDistantCell()) {
            // Render over Morrowind domain
            effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);

            // Draw grass with shadows
            if (Configuration.MGEFlags & USE_GRASS) {
                effect->BeginPass(PASS_RENDERGRASSINST);
                vsr.beginAlphaToCoverage(device);
                renderGrassInst();
                vsr.endAlphaToCoverage(device);
                effect->EndPass();
            }

            // Overlay shadow onto Morrowind objects
            if ((Configuration.MGEFlags & USE_SHADOWS) && mwBridge->CellHasWeather()) {
                effect->BeginPass(isPPLActive ? PASS_RENDERSHADOWFFE : PASS_RENDERSHADOW);
                renderShadow();
                effect->EndPass();
            }

            effect->End();
        }

        // Depth texture from recorded renders and distant land
        effectDepth->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        renderDepth();
        effectDepth->End();

        // Restore render state
        stateSaved->Apply();
        stateSaved->Release();
    }

    recordMW.clear();
}

// renderStage2 - Render shadows and depth texture for scenes 1+ (post-stencil redraw/alpha/1st person)
void DistantLand::renderStage2() {
    auto mwBridge = MWBridge::get();
    IDirect3DStateBlock9* stateSaved;
    UINT passes;

    ///LOG::logline("Stage 2 prims: %d", recordMW.size());

    // Early out if nothing is happening
    if (recordMW.empty()) {
        return;
    }

    if (!isRenderCached) {
        // Save state block manually since we can change FVF/decl
        device->CreateStateBlock(D3DSBT_ALL, &stateSaved);

        if (isDistantCell()) {
            // Shadowing onto recorded renders
            if ((Configuration.MGEFlags & USE_SHADOWS) && mwBridge->CellHasWeather()) {
                effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
                effect->BeginPass(isPPLActive ? PASS_RENDERSHADOWFFE : PASS_RENDERSHADOW);
                renderShadow();
                effect->EndPass();
                effect->End();
            }
        }

        // Depth texture from recorded renders
        effectDepth->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        renderDepthAdditional();
        effectDepth->End();

        // Restore state
        stateSaved->Apply();
        stateSaved->Release();
    }

    recordMW.clear();
}


// renderStageBlend - Blend between MGE distant land and Morrowind, rendering caustics first so it blends out
void DistantLand::renderStageBlend() {
    auto mwBridge = MWBridge::get();
    IDirect3DStateBlock9* stateSaved;
    UINT passes;

    if (isRenderCached) {
        return;
    }

    // Save state block manually since we can change FVF/decl
    device->CreateStateBlock(D3DSBT_ALL, &stateSaved);
    effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);

    // Render caustics
#ifndef MGE_RTX
    // Perf+visual (RTX): skip MGE's screen-space caustic overlay. PASS_RENDERCAUSTICS
    // blends an animated caustic pattern (texWater + backbuffer) onto the frame every
    // frame near water. Under RTX, Remix owns water lighting and this overlay both costs
    // a fullscreen blend and shows as a moving shader pattern on the water. The water
    // plane (renderStageWaterFFP) and Remix's own water are unaffected.
    if (mwBridge->IsExterior() && Configuration.DL.WaterCaustics > 0) {
        D3DXMATRIX m;
        IDirect3DTexture9* tex = PostShaders::borrowBuffer(0);
        D3DXMatrixTranslation(&m, eyePos.x, eyePos.y, mwBridge->WaterLevel());

        effect->SetTexture(ehTex0, tex);
        effect->SetTexture(ehTex1, texWater);
        effect->SetTexture(ehTex3, texDepthFrame);
        effect->SetMatrix(ehWorld, &m);
        effect->SetFloat(ehAlphaRef, Configuration.DL.WaterCaustics);
        effect->CommitChanges();

        effect->BeginPass(PASS_RENDERCAUSTICS);
        PostShaders::applyBlend();
        effect->EndPass();
    }
#endif

    // Blend MW/MGE
    if (isDistantCell() && (~Configuration.MGEFlags & NO_MW_MGE_BLEND)) {
        effect->SetTexture(ehTex0, texDistantBlend);
        effect->SetTexture(ehTex3, texDepthFrame);
        effect->CommitChanges();

        effect->BeginPass(PASS_BLENDMGE);
        PostShaders::applyBlend();
        effect->EndPass();
    }

    effect->End();
    stateSaved->Apply();
    stateSaved->Release();
}

// renderStageWater - Render replacement water plane
void DistantLand::renderStageWater() {
    auto mwBridge = MWBridge::get();
    IDirect3DStateBlock9* stateSaved;
    UINT passes;

    if (isRenderCached) {
        return;
    }

    if (mwBridge->CellHasWater()) {
        // Save state block manually since we can change FVF/decl
        device->CreateStateBlock(D3DSBT_ALL, &stateSaved);
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);

        // Draw water plane
        bool u = mwBridge->IsUnderwater(eyePos.z);
        bool i = !mwBridge->IsExterior();

        if (u || i) {
            // Set up clip plane at fog end for certain environments to save fillrate
            float clipAt = Configuration.DL.InteriorFogEnd * kCellSize;
            D3DXPLANE clipPlane(0, 0, -clipAt, mwProj._33 * clipAt + mwProj._43);
            device->SetClipPlane(0, clipPlane);
            device->SetRenderState(D3DRS_CLIPPLANEENABLE, 1);
        }

        // Switch to appropriate shader and render
        effect->BeginPass(u ? PASS_RENDERUNDERWATER : PASS_RENDERWATER);
        renderWaterPlane();
        effect->EndPass();

        effect->End();
        stateSaved->Apply();
        stateSaved->Release();
    }
}

// setupCommonEffect - Set shared shader variables for this frame
void DistantLand::setupCommonEffect(const D3DXMATRIX* view, const D3DXMATRIX* proj) {
    auto mwBridge = MWBridge::get();

    // View position
    effect->SetMatrix(ehView, view);
    effect->SetMatrix(ehProj, proj);
    effect->SetFloatArray(ehEyePos, eyePos, 3);

    // Sunlight
    D3DXVECTOR3 sunVecView;
    RGBVECTOR totalAmb = sunAmb + ambCol;
    D3DXVec3TransformNormal(&sunVecView, (const D3DXVECTOR3*)&sunVec, view);

    effect->SetFloatArray(ehSunVec, sunVec, 3);
    effect->SetFloatArray(ehSunVecView, sunVecView, 3);
    effect->SetFloatArray(ehSunCol, sunCol, 3);
    effect->SetFloatArray(ehSunAmb, totalAmb, 3);
    effect->SetFloatArray(ehSunPos, sunPos, 3);
    effect->SetFloat(ehSunVis, sunVis);

    if (isPPLActive) {
        // Apply light multiplier settings to distant land
        RGBVECTOR s = lightSunMult * sunCol, a = lightAmbMult * totalAmb;
        effect->SetFloatArray(ehSunCol, s, 3);
        effect->SetFloatArray(ehSunAmb, a, 3);
    }

    // Sky/fog
    bool isExpFog = (Configuration.MGEFlags & EXP_FOG) != 0;
    const RGBVECTOR* skyCol = mwBridge->CellHasWeather() ?  mwBridge->getCurrentWeatherSkyCol() : &horizonCol;
    effect->SetFloat(ehFogStart, isExpFog ? fogExpStart : fogStart);
    effect->SetFloat(ehFogRange, isExpFog ? fogExpDivisor : fogEnd);
    effect->SetFloat(ehFogNearStart, fogNearStart);
    effect->SetFloat(ehFogNearRange, fogNearEnd);
    effect->SetFloatArray(ehSkyCol, *skyCol, 3);
    effect->SetFloatArray(ehFogColNear, nearFogCol, 3);
    effect->SetFloatArray(ehFogColFar, horizonCol, 3);
    effect->SetFloat(ehNearViewRange, nearViewRange);
    effect->SetFloat(ehNiceWeather, niceWeather);

    if (ehOutscatter) {
        effect->SetFloatArray(ehOutscatter, atmOutscatter, 3);
        effect->SetFloatArray(ehInscatter, atmInscatter, 3);
        effect->SetFloatArray(ehSkyScatterFar, atmSkylightScatter, 4);
    }

    // Wind, requires smoothing as it is very noisy
    static float smoothWind[2];
    if (!mwBridge->IsMenu()) {
        const float f = 0.02;
        const float* wind = mwBridge->GetWindVector();
        smoothWind[0] += f * (windScaling * wind[0] - smoothWind[0]);
        smoothWind[1] += f * (windScaling * wind[1] - smoothWind[1]);
        effect->SetFloatArray(ehWindVec, smoothWind, 2);
    }

    // Other
    effect->SetFloatArray(ehFootPos, (float*)mwBridge->PlayerPositionPointer(), 3);
    effect->SetFloat(ehTime, mwBridge->simulationTime());
}

// setScattering - Set scattering coefficients for atmospheric scattering shader
void DistantLand::setScattering(const RGBVECTOR& out, const RGBVECTOR& in) {
    atmOutscatter = out;
    atmInscatter = in;
}

static double lerp(double x0, double x1, double t) {
    return (1.0 - t) * x0 + t * x1;
}

static double saturate(double x) {
    return std::min(std::max(0.0, x), 1.0);
}

// adjustFog - Set fog distance, wind speed adjust and fog colour for this frame
void DistantLand::adjustFog() {
    auto mwBridge = MWBridge::get();

    nearViewRange = mwBridge->GetViewDistance();

    // Morrowind does not update weather during menu mode, except when time is changed
    // Therefore always run adjustment during menu mode, except if background caching is used
    if (isRenderCached) {
        return;
    }

    // Get fog cell ranges based on environment and weather
    if (mwBridge->IsUnderwater(eyePos.z)) {
        fogStart = Configuration.DL.BelowWaterFogStart;
        fogEnd = Configuration.DL.BelowWaterFogEnd;
    } else if (mwBridge->CellHasWeather()) {
        int wthr1 = mwBridge->GetCurrentWeather(), wthr2 = mwBridge->GetNextWeather();
        float ratio = mwBridge->GetWeatherRatio(), ff = 1.0, fo = 0.0, ws = 0.0;

        if (ratio != 0 && wthr2 >= 0 && wthr2 <= 9) {
            ff = float(lerp(Configuration.DL.FogD[wthr1], Configuration.DL.FogD[wthr2], ratio));
            fo = float(0.01 * lerp(Configuration.DL.FgOD[wthr1], Configuration.DL.FgOD[wthr2], ratio));
            ws = float(lerp(Configuration.DL.Wind[wthr1], Configuration.DL.Wind[wthr2], ratio));
            niceWeather = float(lerp((wthr1 <= 1) ? 1.0 : 0.0, (wthr2 <= 1) ? 1.0 : 0.0, ratio));
            niceWeather *= niceWeather;
            lightSunMult = float(lerp(Configuration.Lighting.SunMult[wthr1], Configuration.Lighting.SunMult[wthr2], ratio));
            lightAmbMult = float(lerp(Configuration.Lighting.AmbMult[wthr1], Configuration.Lighting.AmbMult[wthr2], ratio));
        } else if (wthr1 >= 0 && wthr1 <= 9) {
            ff = Configuration.DL.FogD[wthr1];
            fo = Configuration.DL.FgOD[wthr1] / 100.0f;
            ws = Configuration.DL.Wind[wthr1];
            niceWeather = (wthr1 <= 1) ? 1.0f : 0.0f;
            lightSunMult = Configuration.Lighting.SunMult[wthr1];
            lightAmbMult = Configuration.Lighting.AmbMult[wthr1];
        }

        // Fog distance scale calculation, ensure fogEnd does not scale closer than vanilla Morrowind
        fogEnd = std::max(0.875f, ff * Configuration.DL.AboveWaterFogEnd);
        fogStart = ff * Configuration.DL.AboveWaterFogStart - fo * fogEnd;
        windScaling = ws;

        // For exp fog, adjust start distance so that starting fog approximately equals fo, to retain near visibility comparable to vanilla
        if ((Configuration.MGEFlags & USE_DISTANT_LAND) && (Configuration.MGEFlags & EXP_FOG)) {
            float lg = log(1.0f - 0.25f * fo);
            float expCorrection = lg / (1 + lg);
            fogStart = ff * Configuration.DL.AboveWaterFogStart + expCorrection * fogEnd;
        }
    } else {
        // Avoid density == 0, as when fogStart and fogEnd are equal, the fog equation denominator goes to infinity
        float density = std::max(0.01f, mwBridge->getInteriorFogDens());
        fogStart = float(lerp(Configuration.DL.InteriorFogEnd, Configuration.DL.InteriorFogStart, density));
        fogEnd = Configuration.DL.InteriorFogEnd;
        niceWeather = 0;
        windScaling = 0;
        lightSunMult = 1.0;
        lightAmbMult = 1.0;
    }

    // Convert from cells to in-game units
    fogStart *= kCellSize;
    fogEnd *= kCellSize;

    if ((Configuration.MGEFlags & USE_DISTANT_LAND) && isDistantCell()) {
        // Set hardware fog for Morrowind's use
        if (Configuration.MGEFlags & EXP_FOG) {
            // Exponential fog mode
            // Adjust exp curve so that at the fog end boundary, the same fog value is reached for all values of fogStart
            constexpr float expFogDistScale = 4.4f;
            fogExpStart = fogStart / expFogDistScale;
            fogExpDivisor = (fogEnd - fogExpStart) / expFogDistScale;

            if (mwBridge->IsUnderwater(eyePos.z) || !mwBridge->CellHasWeather()) {
                // Leave fog ranges as set, shaders use all linear fogging in this case
                fogNearStart = fogStart;
                fogNearEnd = fogEnd;
            } else {
                // Adjust near region linear Morrowind fogging to approximation of exp fog curve
                // Linear density matched to exp fog at dist = 1280 and dist = viewrange (or fog end if closer)
                // Note to self: Don't use saturate here or the denominators can become zero.
                float farIntercept = std::min(fogEnd, nearViewRange);
                float expFogNear = exp(-(1280.0f - fogExpStart) / fogExpDivisor);
                float expFogFar = exp(-(farIntercept - fogExpStart) / fogExpDivisor);
                fogNearStart = 1280.0f + (farIntercept - 1280.0f) * (1.0f - expFogNear) / (expFogFar - expFogNear);
                fogNearEnd = 1280.0f + (farIntercept - 1280.0f) * -expFogNear / (expFogFar - expFogNear);
            }
        } else {
            // Linear mode
            fogNearStart = fogStart;
            fogNearEnd = fogEnd;
        }

        device->SetRenderState(D3DRS_FOGSTART, *(DWORD*)&fogNearStart);
        device->SetRenderState(D3DRS_FOGEND, *(DWORD*)&fogNearEnd);
    } else {
        // Update fog when near render distance changes, and on startup when fogNearEnd == 0
        bool doFogUpdate = fogNearEnd != nearViewRange;

        // Read Morrowind-set fog range
        fogNearEnd = nearViewRange;
        fogNearStart = fogNearEnd * std::min(1.0f - mwBridge->getScenegraphFogDensity(), 0.99f);
        fogStart = fogNearStart;
        fogEnd = fogNearEnd;

        if (doFogUpdate) {
            device->SetRenderState(D3DRS_FOGSTART, *(DWORD*)&fogNearStart);
            device->SetRenderState(D3DRS_FOGEND, *(DWORD*)&fogNearEnd);
        }
    }

    // Adjust Morrowind fog colour towards scatter colour if necessary
    if ((Configuration.MGEFlags & USE_DISTANT_LAND) && (Configuration.MGEFlags & USE_ATM_SCATTER) && mwBridge->CellHasWeather() && !mwBridge->IsUnderwater(eyePos.z)) {
        // Read unadjusted colour, as the scenegraph fog colour may not be updated during menu transitions
        RGBVECTOR c0 = *mwBridge->getCurrentWeatherFogCol();
        RGBVECTOR c1 = c0;

        // Simplified version of scattering from the shader
        const RGBVECTOR* skyCol = mwBridge->getCurrentWeatherSkyCol();
        const D3DXVECTOR3 newSkyCol = {
            float(lerp(skyCol->r, atmSkylightScatter.x, atmSkylightScatter.w)),
            float(lerp(skyCol->g, atmSkylightScatter.y, atmSkylightScatter.w)),
            float(lerp(skyCol->b, atmSkylightScatter.z, atmSkylightScatter.w))
        };
        const float sunaltitude = powf(1 + sunPos.z, 10);
        const float sunaltitude_a = 2.8 + 4.3 / sunaltitude;
        const float sunaltitude_b = saturate(1.0 - exp2(-1.9 * sunaltitude));
        const float sunaltitude_c = saturate(exp(-4.0 * sunPos.z)) * saturate(sunaltitude);

        // Calculate scatter colour at Morrowind draw distance boundary
        float fogdist = (nearViewRange - fogExpStart) / fogExpDivisor;
        float fog = saturate(exp(-fogdist));
        fogdist = saturate(0.224 * fogdist);

        D3DXVECTOR2 horizonDir(eyeVec.x, eyeVec.y);
        D3DXVec2Normalize(&horizonDir, &horizonDir);
        float suncos =  horizonDir.x * sunPos.x + horizonDir.y * sunPos.y;
        float mie = (1.58 / (1.24 - suncos)) * sunaltitude_c;
        float rayl = 1.0 - 0.09 * mie;
        float atmdep = 1.33;

        D3DXVECTOR3 scatter;
        float scatterT = 0.5 * (1 + suncos);
        scatter.x = float(lerp(atmInscatter.r, atmOutscatter.r, scatterT));
        scatter.y = float(lerp(atmInscatter.g, atmOutscatter.g, scatterT));
        scatter.z = float(lerp(atmInscatter.b, atmOutscatter.b, scatterT));

        D3DXVECTOR3 att = atmdep * scatter * (sunaltitude_a + 0.7 * mie);
        att.x = (1 - exp(-fogdist * att.x)) / att.x;
        att.y = (1 - exp(-fogdist * att.y)) / att.y;
        att.z = (1 - exp(-fogdist * att.z)) / att.z;

        D3DXVECTOR3 k0 = mie * D3DXVECTOR3(0.125, 0.125, 0.125) + rayl * newSkyCol;
        D3DXVECTOR3 k1 = att * (1.17 * atmdep + 0.89) * sunaltitude_b;
        c1.r = k0.x * k1.x;
        c1.g = k0.y * k1.y;
        c1.b = k0.z * k1.z;

        // Convert from additive inscatter to Direct3D fog model
        // The correction factor is clamped to avoid creating infinities
        c1 /= std::max(0.02f, 1.0f - fog);

        // Scattering fog only occurs in nice weather
        c0 = (1.0f - niceWeather) * c0 + niceWeather * c1;

        // Save colour for matching near fog in shaders
        nearFogCol = c0;

        // Alter Morrowind's fog colour through its scenegraph
        // This way it automatically restores the correct colour if it has to switch fog modes mid-frame
        DWORD fc = (DWORD)nearFogCol;
        mwBridge->setScenegraphFogCol(fc);

        // Set device fog colour to propagate change immediately
        device->SetRenderState(D3DRS_FOGCOLOR, fc);
    } else {
        // Save current fog colour for matching near fog in shaders
        nearFogCol = RGBVECTOR(mwBridge->getScenegraphFogCol());
    }
}

// postProcess - Calls post process module, or captures and applies frame cache to avoid rendering
void DistantLand::postProcess() {
    if (!isRenderCached) {
        auto mwBridge = MWBridge::get();

        // Save state block
        IDirect3DStateBlock9* stateSaved;
        device->CreateStateBlock(D3DSBT_ALL, &stateSaved);

        if (Configuration.MGEFlags & USE_HW_SHADER) {
            // Set flags to reflect cell environment
            int envFlags = 0;

            if (!mwBridge->CellHasWeather()) {
                envFlags |= 1;
            }
            if (mwBridge->IsExterior()) {
                envFlags |= 2;
            }
            if (mwBridge->IntLikeExterior()) {
                envFlags |= 4;
            }
            if (mwBridge->IsUnderwater(eyePos.z)) {
                envFlags |= 8;
            } else {
                envFlags |= 16;
            }
            if (sunVis >= 0.001) {
                envFlags |= 32;
            } else {
                envFlags |= 64;
            }

            // Run all shaders (with callback to set changed vars)
            PostShaders::shaderTime(&updatePostShader, envFlags, mwBridge->frameTime());
        }

        // Capture pre-UI screenshots here
        checkCaptureScreenshot(false);

        // Cache render for first frame of menu mode
        if ((Configuration.MGEFlags & USE_MENU_CACHING) && mwBridge->IsMenu()) {
            texDistantBlend = PostShaders::borrowBuffer(0);
            isRenderCached = true;
        }

        // Shadow map inset
        ///if(!mwBridge->IsMenu()) { renderShadowDebug(); }

        // Restore state
        stateSaved->Apply();
        stateSaved->Release();
    } else {
        // Blit cached frame to screen
        IDirect3DSurface9* backbuffer, *surfDistant;
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
        texDistantBlend->GetSurfaceLevel(0, &surfDistant);
        device->StretchRect(surfDistant, 0, backbuffer, 0, D3DTEXF_NONE);
        surfDistant->Release();
        backbuffer->Release();

        // Cache expires for frame after mouse click, so as not to affect click response time
        isRenderCached &= !MGEProxyDirectInput::mouseClick;
    }
}

// updatePostShader - callback for setting post shader variables based on environment
void DistantLand::updatePostShader(MGEShader* shader) {
    auto mwBridge = MWBridge::get();

    // Internal textures
    // TODO: Should be set once at init time
    shader->SetTexture(EV_depthframe, texDepthFrame);
    shader->SetTexture(EV_watertexture, texWater);

    // View position
    float zoom = (Configuration.MGEFlags & ZOOM_ASPECT) ? Configuration.CameraEffects.zoom : 1.0f;
    shader->SetMatrix(EV_mview, &mwView);
    shader->SetMatrix(EV_mproj, &mwProj);
    shader->SetFloatArray(EV_eyevec, eyeVec, 3);
    shader->SetFloatArray(EV_eyepos, eyePos, 3);
    shader->SetFloat(EV_fov, Configuration.ScreenFOV / zoom);

    // Lighting
    RGBVECTOR totalAmb = sunAmb + ambCol;
    shader->SetFloatArray(EV_sunvec, sunVec, 3);
    shader->SetFloatArray(EV_suncol, sunCol, 3);
    shader->SetFloatArray(EV_sunamb, totalAmb, 3);
    shader->SetFloatArray(EV_sunpos, sunPos, 3);
    shader->SetFloat(EV_sunvis, float(lerp(sunVis, 1.0, 0.333 * niceWeather)));

    // Sky/fog
    bool isExpFog = (Configuration.MGEFlags & EXP_FOG) != 0;
    shader->SetFloatArray(EV_fogcol, horizonCol, 3);
    shader->SetFloatArray(EV_fognearcol, nearFogCol, 3);
    shader->SetFloat(EV_fogstart, isExpFog ? fogExpStart : fogStart);
    shader->SetFloat(EV_fogrange, isExpFog ? fogExpDivisor : fogEnd);
    shader->SetFloat(EV_fognearstart, fogNearStart);
    shader->SetFloat(EV_fognearrange, fogNearEnd);

    // Other
    // In cells without water, set very low waterlevel for shaders that clip against water
    float water = mwBridge->CellHasWater() ? mwBridge->WaterLevel() : -1e9f;
    shader->SetFloat(EV_time, mwBridge->simulationTime());
    shader->SetFloat(EV_waterlevel, water);
    shader->SetBool(EV_isinterior, !mwBridge->CellHasWeather());
    shader->SetBool(EV_isunderwater, mwBridge->IsUnderwater(eyePos.z));
}

//------------------------------------------------------------

// selectDistantCell - Select the correct set of distant land meshes for the current cell
bool DistantLand::selectDistantCell() {
    auto mwBridge = MWBridge::get();

    if (Configuration.MGEFlags & USE_DISTANT_LAND) {
        // Preserve legacy generic/grass visibility cadence on cell transitions.
        // Retained non-grass members are polled separately every frame below.
        const auto playerCell = mwBridge->getPlayerCell();
        const bool cellChanged = lastDistantVisCell != playerCell;
        bool retainedCatalogChanged = false;
        bool scanComplete = true;
        if (cellChanged) {
            retainedCatalogChanged = scanDynamicVisGroups(
                false, playerCell, scanComplete);
            if (scanComplete) {
                lastDistantVisCell = playerCell;
            }
        }
#ifdef MGE_RTX
        // The generic scan already includes retained members. Avoid reusing its
        // in-flight IPC vector until the server has consumed it.
        if (!cellChanged) {
            retainedCatalogChanged = scanDynamicVisGroups(
                true, nullptr, scanComplete);
        }
        if (retainedCatalogChanged) {
            RetainedWorld::requestCatalogRefresh();
        }
#else
        (void)retainedCatalogChanged;
#endif
        // A completion for another mode/cell may have been drained above.
        // Do not activate this cell until its own generic visibility batch is
        // acknowledged, or one frame can render with stale generic/grass state.
        if (cellChanged && !scanComplete) {
            return DistantLandShare::hasCurrentWorldSpace;
        }
        if (s_dynVisUpdatePending) {
            return DistantLandShare::hasCurrentWorldSpace;
        }

        // Get worldspace key
        string cellname;
        if (mwBridge->IsExterior()) {
            cellname = string();
        }
        else {
            cellname = mwBridge->getInteriorName();
        }

        if (Configuration.UseSharedMemory) {
            DistantLandShare::hasCurrentWorldSpace = ipcClient.setWorldSpaceBlocking(cellname);
#ifdef MGE_RTX
            RetainedWorld::selectWorldspace(
                ipcClient,
                cellname,
                DistantLandShare::hasCurrentWorldSpace,
                mwBridge->IsExterior());
#endif
            if (DistantLandShare::hasCurrentWorldSpace) {
                return true;
            }
        } else {
            const auto iWS = DistantLandShare::mapWorldSpaces.find(cellname);
            if (iWS != DistantLandShare::mapWorldSpaces.end()) {
                DistantLandShare::currentWorldSpace = &iWS->second;
                DistantLandShare::hasCurrentWorldSpace = true;
                return true;
            }
        }
    }

#ifdef MGE_RTX
    if (Configuration.UseSharedMemory) {
        RetainedWorld::selectWorldspace(ipcClient, string(), false, false);
    }
#endif
    DistantLandShare::currentWorldSpace = nullptr;
    DistantLandShare::hasCurrentWorldSpace = false;
    return false;
}

// isDistantCell - Check if there is distant land selected for this cell
bool DistantLand::isDistantCell() {
    return DistantLandShare::hasCurrentWorldSpace;
}

// resolveDynamicVisGroups - Resolve pointers to game objects on load/reload
void DistantLand::resolveDynamicVisGroups() {
    auto mwBridge = MWBridge::get();
    const DynamicVisGroup *lastDVG = nullptr;

    for (auto& vis : dynamicVisGroups) {
        // Clear previous pointer
        vis.gameObject = nullptr;

        // Re-use previous result if the id matches
        if (lastDVG && vis.id == lastDVG->id) {
            vis.gameObject = lastDVG->gameObject;
            continue;
        }
        else {
            lastDVG = &vis;
        }

        // Resolve IDs to pointers
        switch (vis.source) {
        case DynamicVisGroup::DataSource::Journal:
            vis.gameObject = mwBridge->getDialogue(vis.id.c_str());
            break;
        case DynamicVisGroup::DataSource::Global:
            vis.gameObject = mwBridge->getGlobalVar(vis.id.c_str());
            break;
        case DynamicVisGroup::DataSource::UniqueObject:
            vis.gameObject = mwBridge->findFirstReferenceById(vis.id.c_str());
            break;
        }
    }

    // Ensure reloading into the same cell still triggers updates
    lastDistantVisCell = nullptr;
}

// resetDynamicVisState - Detach state belonging to a terminated IPC host
void DistantLand::resetDynamicVisState() {
    s_dynVisUpdatePending = false;
    s_dynVisPendingBatchComplete = true;
    s_dynVisPendingRetainedOnly = false;
    s_dynVisPendingTransitionToken = nullptr;
    lastDistantVisCell = nullptr;

    // startServer has synchronously stopped the previous host before this is
    // called, so releasing its view cannot race a host read.
    dynVisFlagsShared = IPC::VecView<IPC::DynVisFlag>();
    dynVisFlagsSharedId = IPC::InvalidVector;
}

// scanDynamicVisGroups - Scan through game data for visibility changes
bool DistantLand::scanDynamicVisGroups(
    bool retainedOnly,
    const void* transitionToken,
    bool& batchComplete) {
    batchComplete = true;

    const auto finishSharedUpdate = [&]() {
        bool accepted = false;
        const auto result = ipcClient.awaitDynVis(accepted);
        if (result != IPC::WakeReason::Complete) {
            if (result == IPC::WakeReason::ServerLost) {
                s_dynVisUpdatePending = false;
                s_dynVisPendingTransitionToken = nullptr;
                dynVisFlagsShared.clear();
            }
            batchComplete = false;
            return false;
        }

        const bool requestMatchesPending =
            s_dynVisPendingRetainedOnly == retainedOnly &&
            (retainedOnly || s_dynVisPendingTransitionToken == transitionToken);
        s_dynVisUpdatePending = false;
        bool changed = false;
        if (accepted) {
            for (std::uint32_t i = 0; i < dynVisFlagsShared.size(); ++i) {
                const auto update = dynVisFlagsShared[i];
                if (update.groupIndex >= dynamicVisGroups.size()) {
                    continue;
                }
                auto& group = dynamicVisGroups[update.groupIndex];
                if (update.retainedOnly) {
                    group.retainedEnabled = update.enable;
                } else {
                    group.enabled = update.enable;
                    group.retainedEnabled = update.enable;
                }
                changed = true;
            }
        }

        batchComplete = accepted && s_dynVisPendingBatchComplete && requestMatchesPending;
        s_dynVisPendingTransitionToken = nullptr;
        dynVisFlagsShared.clear();
        return accepted && changed;
    };

    if (Configuration.UseSharedMemory && s_dynVisUpdatePending) {
        return finishSharedUpdate();
    }

    auto mwBridge = MWBridge::get();
    if (Configuration.UseSharedMemory) {
        dynVisFlagsShared.clear();
    }
    bool localRetainedChanged = false;
    bool allChangesQueued = true;

    std::uint16_t i = 0;
    for (auto& vis : dynamicVisGroups) {
        int value;
        const auto groupIndex = i++;

        if (!vis.gameObject) {
            continue;
        }

        switch (vis.source) {
        case DynamicVisGroup::DataSource::Journal:
            value = mwBridge->getJournalIndex(vis.gameObject);
            break;
        case DynamicVisGroup::DataSource::Global:
            value = int(mwBridge->getGlobalVarValue(vis.gameObject));
            break;
        case DynamicVisGroup::DataSource::UniqueObject:
            const int disabledRecordFlag = 0x800;
            value = (mwBridge->getRecordFlags(vis.gameObject) & disabledRecordFlag) == 0;
            break;
        }

        bool enable = false;
        for (const auto& r : vis.ranges) {
            if (r.begin <= value && value < r.end) {
                enable = true;
                break;
            }
        }

        const bool cachedEnable = retainedOnly ? vis.retainedEnabled : vis.enabled;
        if (enable == cachedEnable) {
            continue;
        }
        if (Configuration.UseSharedMemory) {
            if (!dynVisFlagsShared.push_back({ groupIndex, enable, retainedOnly })) {
                allChangesQueued = false;
            }
            continue;
        }

        bool changedRetainedMesh = false;
        for (auto* mesh : vis.references) {
            if (mesh == nullptr ||
                (retainedOnly && mesh->retainedPlacementIdentity == 0) ||
                mesh->enabled == enable) {
                continue;
            }
            mesh->enabled = enable;
            changedRetainedMesh = changedRetainedMesh ||
                mesh->retainedPlacementIdentity != 0;
        }
        localRetainedChanged = localRetainedChanged || changedRetainedMesh;
        if (retainedOnly) {
            vis.retainedEnabled = enable;
        } else {
            vis.enabled = enable;
            vis.retainedEnabled = enable;
        }
    }

    if (!Configuration.UseSharedMemory) {
        return localRetainedChanged;
    }
    if (dynVisFlagsShared.empty()) {
        batchComplete = allChangesQueued;
        return false;
    }
    if (!ipcClient.updateDynVis(dynVisFlagsSharedId)) {
        batchComplete = false;
        return false;
    }

    s_dynVisUpdatePending = true;
    s_dynVisPendingBatchComplete = allChangesQueued;
    s_dynVisPendingRetainedOnly = retainedOnly;
    s_dynVisPendingTransitionToken = retainedOnly ? nullptr : transitionToken;
    return finishSharedUpdate();
}

// setView - Called once per frame to setup view dependent data
void DistantLand::setView(const D3DMATRIX* m) {
    auto mwBridge = MWBridge::get();

    // Calculate eyePos, eyeVec for shaders
    D3DXVECTOR4 origin(0.0, 0.0, 0.0, 1.0);
    D3DXMATRIX invView, view = *m;

    D3DXMatrixInverse(&invView, 0, &view);
    D3DXVec4Transform(&eyePos, &origin, &invView);
    eyeVec.x = m->_13;
    eyeVec.y = m->_23;
    eyeVec.z = m->_33;

    // Set sun disc position
    if (mwBridge->IsLoaded() && mwBridge->CellHasWeather()) {
        mwBridge->GetSunDir(sunPos.x, sunPos.y, sunPos.z);
        sunPos.w = 1;
        sunPos /= sqrt(sunPos.x * sunPos.x + sunPos.y * sunPos.y + sunPos.z * sunPos.z);

        // Sun position "bounces" at the horizon to follow night lighting instead of setting
        // Sun visibility goes to zero at night, so use this to correct the sun position so it sets
        sunVis = mwBridge->GetSunVis() / 255.0f;
        if (sunVis == 0) {
            sunPos.z = -sunPos.z;
        }
    } else {
        sunPos = D3DXVECTOR4(0, 0, -1, 1);
        sunVis = 0;
    }
}

// setProjection - Called when a D3D projection matrix is set, and edits it
void DistantLand::setProjection(D3DMATRIX* proj) {
    // Move near plane from 1.0 to 4.0 for more z accuracy
    // Move far plane back to edge of draw distance
    if (Configuration.MGEFlags & USE_DISTANT_LAND) {
        editProjectionZ(proj, kDistantNearPlane, Configuration.DL.DrawDist * kCellSize);
    }
}

// editProjectionZ - Alter the near and far clip planes of a projection matrix
void DistantLand::editProjectionZ(D3DMATRIX* m, float zn, float zf) {
    // Override near and far clip planes
    m->_33 = zf / (zf - zn);
    m->_43 = -zn * zf / (zf - zn);
}

void DistantLand::setHorizonColour(const RGBVECTOR& c) {
    horizonCol = c;
}

void DistantLand::setAmbientColour(const RGBVECTOR& c) {
    ambCol = c;
}

void DistantLand::setSunLight(const D3DLIGHT8* s) {
    // Sun is used for both interiors and exteriors; the sun in interiors is a fixed light
    sunVec.x = s->Direction.x;
    sunVec.y = s->Direction.y;
    sunVec.z = s->Direction.z;
    D3DXVec3Normalize((D3DXVECTOR3*)&sunVec, (D3DXVECTOR3*)&sunVec);

    sunCol = s->Diffuse;
    sunAmb = s->Ambient;
}

// inspectIndexedPrimitive
// Filters and records DIP calls for later use; returning false should cause the draw call to be skipped
// Can also replace selected fixed function calls with an augmented shader
bool DistantLand::inspectIndexedPrimitive(int sceneCount, const RenderedState* rs, const FragmentState* frs, LightState* lightrs) {
    auto mwBridge = MWBridge::get();

    // Avoid recording landscape alpha blend drawcalls, a form of multi-pass splatting
    static IDirect3DVertexBuffer9* lastVB = nullptr;
    bool isLandSplat = sceneCount == 0 && rs->vb == lastVB && rs->blendEnable && (rs->fvf & D3DFVF_DIFFUSE) && mwBridge->IsExterior();
    lastVB = rs->vb;

    // Avoid recording decal passes from UV sets >0, shadow rendering only samples alpha from texture 0 with UV 0
    const auto& stage0 = frs->stage[0];
    bool isDecal = stage0.texcoordIndex != 0 && (stage0.colorArg1 == D3DTA_TEXTURE || stage0.colorArg2 == D3DTA_TEXTURE);

    // Capture all writes to z-buffer, except detectable second passes of multi-pass rendering
    if (rs->zWrite && !isLandSplat && !isDecal) {
        recordMW.emplace_back(*rs);

        // Unify alpha test operator/reference to be equivalent to GREATEREQUAL
        if (rs->alphaFunc == D3DCMP_GREATER) {
            recordMW.back().alphaRef++;
        }
    }

    // Special case, capture sky
    if (recordMW.empty() && rs->blendEnable && sceneCount == 0 && mwBridge->CellHasWeather()) {
        recordSky.emplace_back(*rs);

        // Check for moon geometry, and mark those records by setting lighting off
        if (frs->material.emissive.a == kMoonTag) {
            recordSky.back().useLighting = false;
        }

        // If using atmosphere scattering, draw sky later in stage 0
        if ((Configuration.MGEFlags & USE_DISTANT_LAND) && (Configuration.MGEFlags & USE_ATM_SCATTER)) {
            return false;
        }
    } else if (isPPLActive) {
        // Render Morrowind with replacement shaders
        FixedFunctionShader::renderMorrowind(rs, frs, lightrs);
        return false;
    }

    return true;
}

// requestCapture - Set a function to be called with a screen capture
// Either before the UI is drawn, or after UI and before MGE messages
void DistantLand::requestCapture(std::function<void(IDirect3DSurface9*)> handler, bool captureWithUI) {
    captureScreenHandler = handler;
    captureScreenWithUI = captureWithUI;
}

void DistantLand::checkCaptureScreenshot(bool isUIDrawn) {
    if (bool(captureScreenHandler) && captureScreenWithUI == isUIDrawn) {
        IDirect3DSurface9* surface = captureScreenshot();
        captureScreenHandler(surface);
        if (surface) {
            surface->Release();
        }
        captureScreenHandler = nullptr;
    }
}

// captureScreen - Capture a screenshot
IDirect3DSurface9* DistantLand::captureScreenshot() {
    IDirect3DTexture9* t;
    IDirect3DSurface9* s;

    // Resolve multisampled back buffer
    t = PostShaders::borrowBuffer(0);
    t->GetSurfaceLevel(0, &s);

    // Cancel render cache, borrowBuffer just overwrote it
    isRenderCached = false;

    // Copy buffer to system memory surface
    IDirect3DSurface9* surfSS;
    D3DSURFACE_DESC desc;

    s->GetDesc(&desc);
    DWORD hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surfSS, NULL);
    if (hr != D3D_OK) {
        s->Release();
        return nullptr;
    }

    hr = device->GetRenderTargetData(s, surfSS);
    s->Release();
    if (hr != D3D_OK) {
        surfSS->Release();
        return nullptr;
    }

    return surfSS;
}


// ------------------------------------
// DistantLand::RecordedState

DistantLand::RecordedState::RecordedState(const RenderedState& state)
    : RenderedState(state) {
    vb->AddRef();
    ib->AddRef();
    if (texture) {
        texture->AddRef();
    }
}

DistantLand::RecordedState::~RecordedState() {
    if (vb) {
        vb->Release();
    }
    if (ib) {
        ib->Release();
    }
    if (texture) {
        texture->Release();
    }
}

DistantLand::RecordedState::RecordedState(RecordedState&& source) noexcept
    : RenderedState(source) {
    source.vb = nullptr;
    source.ib = nullptr;
    source.texture = nullptr;
}


// ------------------------------------
// RenderTargetSwitcher

// RenderTargetSwitcher - Switch to a render target, restoring state at end of scope
RenderTargetSwitcher::RenderTargetSwitcher(IDirect3DSurface9* target, IDirect3DSurface9* targetDepthStencil) {
    init(target, targetDepthStencil);
}

// RenderTargetSwitcher - Switch to a render surface belonging to a texture, restoring state at end of scope
RenderTargetSwitcher::RenderTargetSwitcher(IDirect3DTexture9* targetTex, IDirect3DSurface9* targetDepthStencil) {
    // Note the device still holds a reference to the target while it's active
    IDirect3DSurface9* target;
    targetTex->GetSurfaceLevel(0, &target);
    init(target, targetDepthStencil);
    target->Release();
}

void RenderTargetSwitcher::init(IDirect3DSurface9* target, IDirect3DSurface9* targetDepthStencil) {
    DistantLand::device->GetRenderTarget(0, &savedTarget);
    DistantLand::device->GetDepthStencilSurface(&savedDepthStencil);

    DistantLand::device->SetRenderTarget(0, target);
    DistantLand::device->SetDepthStencilSurface(targetDepthStencil);
}

RenderTargetSwitcher::~RenderTargetSwitcher() {
    DistantLand::device->SetRenderTarget(0, savedTarget);
    DistantLand::device->SetDepthStencilSurface(savedDepthStencil);

    if (savedTarget) {
        savedTarget->Release();
    }
    if (savedDepthStencil) {
        savedDepthStencil->Release();
    }
}
