# Hyphae Implementation Plan

This file is the working implementation plan for `hyphae` v0.1. It replaces the earlier prompt-style draft with a repo-local plan that tracks the current codebase, the locked product decisions, and the documentation upkeep required as the implementation becomes concrete.

## Current Baseline

The repository is still at the JUCE template stage.

- `CMakeLists.txt` now uses the Hyphae identity: project `Hyphae`, product name `Hyphae`, manufacturer `Lucian Par`, manufacturer code `LUCP`, plugin code `Hyph`.
- [`src/PluginProcessor.cpp`](/Users/lucian/projects/hyphae/src/PluginProcessor.cpp:1) now includes an APVTS parameter layout and versioned parameter-state serialization, but DSP is still placeholder/template logic.
- [`src/PluginEditor.cpp`](/Users/lucian/projects/hyphae/src/PluginEditor.cpp:1) is still a lightweight placeholder editor, but it now reflects the Hyphae identity and M1 state skeleton.
- `README.md` is still the generic JUCE kickstart readme.
- `docs/DesignV1.md` is the main design reference right now; the future doc split described below has not been created yet.

## Product Lock For v0.1

These are the implementation constraints we should treat as fixed unless we explicitly record a divergence.

- Effect only.
- Delay buffer is mono-sum write, sampled by the grain engine.
- Delay memory is bounded to `8.0` seconds maximum.
- Grain synthesis uses a Hann window.
- Grain rate drift stays subtle and centered around `1.0`.
- Grain size range is locked in v0.1 once finalized.
- Max active grain voices is `12` with a fixed pool.
- Spawn policy is weather-like / Poisson-ish with a hard cap of `2` spawns per block.
- When the voice pool is full, spawns are dropped rather than stolen.
- Stereo motion is cluster-based, with subtle mid/side shaping on top.
- Conduction bed is audible and multi-tap, with no feedback in v0.1.
- Freeze ducks writes using ramps rather than hard-stopping delay writes.
- Spore Burst reseeds topology without a density gust and must remain click-free.
- State restore must resume the exact simulation timeline and scheduler/RNG state.
- Delay buffer audio contents do not persist across reload.
- Reset/Clear button is included.
- Transport behavior is free-run only.
- Output must be realtime-safe, click-free, bounded, and run around a `-12 dBFS` operating target.

## Realtime-Safety Rules

These are non-negotiable for `processBlock()`.

- No allocations.
- No locks or mutexes.
- No logging.
- No file I/O.
- All delay reads bounds-checked.
- All exposed or audible controls smoothed.
- All grain events windowed.
- All derived values clamped.
- Non-finite samples guarded and zeroed safely.
- Output protection includes DC/rumble control plus soft clipping or limiting.

## Documentation Workflow

The docs need to evolve with the code, not after the fact.

### Existing docs

- `docs/DesignV1.md` is the current design spec and should be updated until the repo is reorganized.
- `docs/planning.md` is this implementation tracker.

### Intended doc split

As the project settles, create or migrate toward:

- `docs/design/hyphae_v0_1_design.md`
- `docs/agent/implementation_plan.md`
- `docs/agent/mismatch_ledger.md`
- `docs/agent/agent_log.md`

Until that split exists, keep changes coherent in the current docs and avoid duplicating contradictory specs.

### Required updates after each milestone

- Final parameter IDs, ranges, and defaults.
- Exact state serialization fields and versioning policy.
- Safety caps such as spawn caps, delay caps, and grain size bounds.
- Confirmed bus support behavior, especially mono-in to stereo-out handling.

### Decision logging rule

If we make a decision that is not already locked in the design:

- Pick a sensible default and continue.
- Record the decision in `agent_log.md` once that file exists.
- If the decision changes user-facing behavior or future extensibility, also record it in `mismatch_ledger.md` as a new decision or divergence.

## Milestone Plan

Status keys: `not started`, `in progress`, `done`.

### M0. Identity + Build Sanity

Status: `done`

Goals:

- Rename plugin identifiers in `CMakeLists.txt`.
- Replace template project, product, and manufacturer strings.
- Confirm effect-only configuration.
- Confirm intended bus layout support for v0.1.

Deliverables:

- Plugin no longer builds as `KICKSTART`.
- Design docs record the final product identity and plugin codes.
- Initial bus support is confirmed as matched mono/stereo layouts in the current processor skeleton.

Notes:

- This is the highest-priority first step because every later host test and state blob depends on stable identity.
- Chosen identity for M0: project/product `Hyphae`, company `Lucian Par`, manufacturer code `LUCP`, plugin code `Hyph`.
- Explicit bundle ID: `com.lucianpar.hyphae`.

### M1. Parameters + State Skeleton

Status: `done`

Goals:

- Introduce `juce::AudioProcessorValueTreeState`.
- Define stable parameter IDs for mix, spores, conduction, and actions.
- Add a state version field.
- Implement `getStateInformation()` and `setStateInformation()`.

Deliverables:

- APVTS-backed parameter layout.
- Versioned state payload.
- Docs updated with exact ranges/defaults and ID strings.

Notes:

- APVTS root type is `HyphaeState`.
- Current state payload stores parameter values plus root property `stateVersion = 1`.
- Full exact sim-state resume is still deferred to M8; M1 covers stable parameter persistence only.

### M2. Delay Buffer

Status: `done`

Goals:

- Implement a bounded single-channel float delay/ring buffer.
- Write mono-sum input into the buffer.
- Add smoothed write gain for Freeze ducking.
- Implement interpolated reads.

Safety checks:

- No allocations in the audio thread.
- Bounds checks on reads and writes.
- Non-finite guards.

Deliverables:

- Delay buffer sized in `prepareToPlay()`.
- Memory formula and cap documented.

Notes:

- Current implementation uses a single-channel float ring buffer allocated in `prepareToPlay()`.
- Buffer sizing is `clamp (ceil (sampleRate * 8.0), 1, 1536000)`.
- Freeze currently ducks writes with a smoothed `30 ms` write-gain ramp.
- A bounded linear-interpolated `250 ms` preview read is available for verification in the placeholder editor.

### M3. Grain Voice Pool + Hann Window

Status: `done`

Goals:

- Implement fixed `12`-voice grain pool.
- Add Hann windowing per voice.
- Lock the v0.1 grain size range.
- Implement subtle rate drift policy.
- Enforce spawn cap and drop-on-full behavior.

Deliverables:

- Audible spores from delay memory.
- Docs updated with final grain size and drift bounds.

Notes:

- Current implementation uses a fixed processor-owned pool of `12` grain voices.
- Grain size range is now locked to `20.0..180.0 ms`.
- Rate drift range is currently `0.97..1.03`.
- Scheduling is block-driven and Poisson-ish, with a hard cap of `2` spawns per block.
- When all voices are active, new spawns are dropped rather than stolen.

### M4. Mycelium Control Model

Status: `not started`

Goals:

- Implement compact cluster-center model for density, delay targeting, and stereo placement.
- Support branch and merge behavior.
- Implement click-free Spore Burst reseeding.

Deliverables:

- Cluster count and branch/merge rules written down.
- Smooth topology reconfiguration behavior.

### M5. Conduction Bed

Status: `not started`

Goals:

- Implement audible multi-tap delay bed.
- Keep topology in the `3` to `6` tap range.
- Add damping and stable gain staging.

Deliverables:

- Conduction texture is clearly audible and level-safe.
- Docs capture tap strategy and mapping.

### M6. Stereo Shaping

Status: `not started`

Goals:

- Make cluster panning the primary spatial cue.
- Add subtle mid/side shaping.
- Bias spores slightly more toward side as Spread increases.
- Keep conduction bed more mid-anchored.

Deliverables:

- Exact M/S mapping and smoothing policy documented.

### M7. Output Safety + Headroom

Status: `not started`

Goals:

- Add DC blocker or HPF.
- Add soft clipper or limiter.
- Add smoothed wet normalization, such as `1 / sqrt(activeVoices)`.
- Verify nominal operating level around `-12 dBFS`.

Deliverables:

- Output is bounded under stress.
- Limiter or clipper choice and thresholds documented.

### M8. Exact Sim-State Resume

Status: `not started`

Goals:

- Serialize simulation state, RNG state, scheduler accumulators, and any smoothing state required for deterministic resume.
- Ensure reload restores the same ongoing evolution, except for non-persisted delay audio contents.

Deliverables:

- Exact serialized field list and version policy documented.

### M9. Minimal UI

Status: `not started`

Goals:

- Replace template editor.
- Add parameter attachments.
- Add `Spore Burst`, `Freeze`, and `Reset/Clear` controls.

Deliverables:

- Minimal but usable v0.1 editor with all major controls wired.

### M10. Validation + README

Status: `not started`

Goals:

- Perform quick host validation where possible.
- Stress-test rapid automation, repeated Spore Burst presses, and Freeze toggling.
- Replace the generic readme with build and audition guidance for Hyphae.

Deliverables:

- Acceptance notes and known limitations recorded.
- README reflects the real plugin.

## Recommended Build Order

1. M0, because project identity should stabilize before host testing and saved-state iteration.
2. M1, because parameter and state scaffolding define the control surface for the DSP.
3. M2 and M3, because the plugin needs memory and voices before higher-level behavior matters.
4. M4 through M7, because they shape the audible personality and safety envelope.
5. M8, once simulation pieces exist and can be serialized correctly.
6. M9 and M10 last, once behavior is concrete enough to expose and validate.

## Immediate Next Actions

If implementation work starts from the current repo state, the next concrete steps should be:

1. Implement the bounded mono delay buffer and interpolated reads for M2.
2. Add smoothed write gain handling for Freeze ducking.
3. Replace more of the remaining placeholder processor/editor skeleton with Hyphae-specific behavior and controls.
4. Update `README.md` once the first audible DSP milestone lands so the repo stops presenting itself as the generic template.
