Hyphae v0.1 Design Doc (JUCE Stereo Plugin) 0. Summary

Hyphae is a stereo audio effect that turns incoming audio into a drifting field of micro-grains (“spores”) with organismic stereo motion. It is delay-framed spore synthesis: grains are sampled from a bounded delay buffer of recent audio. A “mycelium” control model shapes clustering, branching/merging, and motion. A conduction bed provides an audible delay texture (no feedback in v0.1).

Locked v0.1 decisions (from you)

Plugin type: Effect only
Buffer: mono sum write, tap from input buffer
Grains: subtle rate drift, Hann window, lock grain size range for v0, drop spawns when full
Scheduling: Poisson / weather now; tempo sync later
Spawn cap: 1–2 grains per block
Density can “storm softly” (audible lift, still safe)
Stereo: cluster-based panning, plus mid/side shaping
Conduction bed: audible delay texture, NO feedback in v0.1
Freeze: duck writes (write gain ramps down/up, not abrupt stop)
Spore Burst: reseeds without gust
State: resume exact sim timeline/state (not just restart from seed)
Include Reset/Clear button
Transport behavior: free-run always (no auto-reset on play/stop)
Safety: max delay = 8 seconds (future versions may increase)
Target headroom: ~ −12 dBFS operating level
Wet normalization scaling: yes (e.g., 1/sqrt(activeVoices))
UI: minimal in v0.1, “cool UI” in v0.2+

Identity snapshot (M0)

Project / product name: Hyphae
Company name: Lucian Par
Manufacturer code: LUCP
Plugin code: Hyph
Bundle ID: com.lucianpar.hyphae
Formats: AU, VST3, Standalone
Plugin role: audio effect
Initial bus support confirmation: matched mono-in/mono-out and stereo-in/stereo-out, with stereo effect behavior remaining the primary design target

1. Repo starting point

Repo: https://github.com/lucianpar/hyphae

Current baseline is a JUCE kickstart template, but M0 has renamed the plugin identity:

CMakeLists.txt now builds the product as Hyphae and targets AU/VST3/Standalone.
PluginProcessor contains placeholder DSP and placeholder state saving.

Constraint: do not modify JUCE submodule code.

2. Creative contract
   2.1 Aesthetic invariants
   Spore Weather first: sporadic, airy micro-events; not a rhythmic stutter machine by default.
   Organismic motion: clustered drift + branch/merge; avoid generic random ping-pong.
   No destructive transients: every event is windowed; every audible control is smoothed.
   Audible conduction bed (v0.1): a real delay texture supporting the spores, but not runaway.
   Clarity bias: max voices = 12 in v0.1, so texture stays readable.
   2.2 Gesture vocabulary (performable “moves”)
   Branch: new cluster center appears; spores start orbiting it.
   Anastomosis: two clusters merge; stereo centers converge; bed becomes more coherent briefly.
   Spore Burst (button): reseed topology without density gust (reconfiguration, not a hit).
   Freeze Garden: duck input writes so spores “scan” a held past without hard discontinuity.
   2.3 Default “first 10 seconds”

On insert: audible delicate spores + audible delay bed, stable level, no pop.
Turning Growth increases “life” (movement + branching), not loudness spikes.
Pressing Spore Burst reconfigures smoothly (no click, no silence drop, no gust).

2.4 Preset taxonomy (creative regression tests)
Air: low density, wide, delicate, brighter spores.
Canopy: moderate density, warmer bed.
Storm (soft): higher density but controlled, still not harsh.
Freeze Garden: freeze engaged; slow drift.
Root Hum: darker damping, more mid, slower movement. 3. System overview
3.1 High-level signal flow
Input (stereo)
Write mono sum (L+R)\*0.5 into a bounded circular buffer (“nutrient field”)
Wet synthesis
Spore Engine: 12 grain voices sampling from buffer (micro-grains)
Conduction Bed: multi-tap delay texture (no feedback)
Stereo shaping
cluster-based pan distribution (spores)
mild mid/side shaping to strengthen stereo readability
Safety + output
DC/rumble protection
soft clip/limiter
Dry/Wet mix + output trim
3.2 Why mono buffer (v0.1)

Mono buffer keeps the “source memory” coherent and reduces complexity. Stereo identity is created by where spores are panned + mid/side shaping, not by sampling stereo content.

4. DSP design
   4.1 Delay buffer

Max delay length: 8.0 seconds (hard cap for v0.1)
Write source: mono sum of input
Tap source: the input buffer (no feedback loop)

Implementation requirements

Allocated only in prepareToPlay()
No resizing / allocations in processBlock()
Linear interpolation for fractional delays
Strict bounds checking for reads
4.2 Spore grain engine

Voices: fixed pool of 12 concurrent grains.

Each grain voice stores:

active
delayStartSamples (float)
durationSamples (int)
phase (0..1)
rate (subtle drift around 1.0; locked range v0.1)
gain
pan (−1..+1)
optional: per-grain filter tilt (very light) if needed

Windowing

Hann window for every grain
Always fades to 0 at start/end

Locked “no-click” rule

Delay offset is fixed for the life of a grain (no mid-grain pointer moves)

Spawn policy

Weather-like (Poisson-ish) spawn schedule
Hard cap 2 spawns per block (and optionally 1 at very small blocks)
When pool is full: drop spawns (do not steal) in v0.1
4.3 Conduction bed (v0.1: NO feedback)

A multi-tap delay texture mixed into wet output. No feedback loop in v0.1.

Tap topology

3–6 taps, times in ~30–350 ms range
Damping filter (one-pole LP) for tone control
Tap gains scaled to keep headroom

Creativity note

“Delay texture” means it is audible when Conduction is raised, not just a hidden glue.
4.4 Stereo shaping (cluster + M/S)

Cluster panning is the primary spatial cue:

grains sample pan from one of K cluster centers (e.g., 2–4), with cluster spread

Mid/side shaping is the secondary cue:

spores bias slightly toward Side at higher Spread
conduction bed remains more Mid-anchored for stability
keep M/S shaping subtle, level-safe, and smoothed
4.5 Wet normalization scaling

Because grains sum, we normalize wet gain by active voices:

Compute normTarget = 1/sqrt(max(1, activeVoices))
Smooth normTarget per block to avoid pumping
Apply to wet spore sum (and optionally to bed) so “soft storm” increases activity without blasting level 5. Mycelium control model
5.1 Representation (v0.1)

Use a small number of cluster centers instead of simulating a large graph.
This hits the “fungal” feel with minimal CPU and direct creative control.

State includes:

K clusters (recommend 2–4 for v0.1)
panCenter
panSpread
energy
age
Event system:
branch: create a new cluster near an existing one
merge: blend two clusters into one

Outputs (control-rate, 30–60 Hz):

target density (grains/sec)
target grain size (ms)
delay center and delay spread (how far back spores reach)
cluster centers/spreads
“coherence” for conduction bed (e.g., whether taps bunch closer)
5.2 Parameter-to-behavior mapping
Growth: cluster drift speed + branch/merge rate
Nutrients: persistence + average energy + density ceiling
Spread: cluster spreads + side bias
Scatter: per-grain jitter (pan + delay variance) inside cluster rules
Conduction: conduction bed mix and tap coherence (not feedback)
Damping: bed damping and any subtle grain tilt
5.3 Spore Burst semantics (no gust)

Pressing Spore Burst:

triggers a topology reseed (new cluster centers / ages / energies)
does not spike density
reconfiguration must be click-free:
either smooth cluster center transitions over ~50–200 ms
and/or apply a short density multiplier ramp down/up without a “gust” peak
6. Parameters
6.1 v0.1 parameter set

M1 implementation snapshot

APVTS root type: `HyphaeState`
State version property: `stateVersion = 1`

Core mix:

`dryWet`
Display name: Dry/Wet
Range/default: `0.0..1.0`, default `0.5`

`outputTrimDb`
Display name: Output Trim
Range/default: `-18.0..6.0 dB`, default `0.0 dB`

Spore:

`density`
Display name: Density
Range/default: `0.1..24.0 grains/s`, default `4.0`
Note: this is the control range only; later DSP stages still enforce spawn-budget and voice-cap limits

`sizeMs`
Display name: Size
Range/default: `20.0..180.0 ms`, default `80.0 ms`
Note: this is the provisional v0.1 locked range from M1 and may be tightened once the grain engine is tuned

`scatter`
Display name: Scatter
Range/default: `0.0..1.0`, default `0.35`

`spread`
Display name: Spread
Range/default: `0.0..1.0`, default `0.6`

`growth`
Display name: Growth
Range/default: `0.0..1.0`, default `0.5`

`nutrients`
Display name: Nutrients
Range/default: `0.0..1.0`, default `0.5`

`seed`
Display name: Seed
Range/default: integer `0..65535`, default `12345`

Conduction:

`conduction`
Display name: Conduction
Range/default: `0.0..1.0`, default `0.4`

`damping`
Display name: Damping
Range/default: `0.0..1.0`, default `0.45`

Actions:

`sporeBurst`
Display name: Spore Burst
Current M1 implementation: boolean action flag, default `false`
Intended later behavior: momentary trigger

`freeze`
Display name: Freeze
Current M1 implementation: boolean toggle, default `false`

`resetClear`
Display name: Reset/Clear
Current M1 implementation: boolean action flag, default `false`
Intended later behavior: momentary trigger that clears sim/buffer state
6.2 Locked v0.1 voice cap
MaxVoices = 12 fixed internally
Design doc note: v0.2+ increases this; must preserve level behavior via normalization. 7. Safety and robustness
7.1 Memory safety

Hard caps:

maxDelaySeconds = 8.0 in v0.1 (compile-time constant)
maxDelaySamples = clamp(sr \* maxDelaySeconds, 1, HARD_CAP_SAMPLES)
HARD_CAP_SAMPLES protects against absurd host sample rates.

Allocation policy:

allocate delay buffer, voice pool, and all scratch buffers only in prepareToPlay()
no allocations or resizes in processBlock()
7.2 Realtime thread safety

Audio thread must have:

no locks, no mutexes, no blocking calls
no file I/O or logging
UI actions communicated via atomics / lock-free flags and applied at block boundaries
7.3 Click/pop prevention

Guaranteed by design:

every grain windowed (Hann)
delay offset fixed per grain
all audible params smoothed (Dry/Wet, Output, Density multipliers, M/S bias, etc.)
Freeze ducks writes with a ramp (no abrupt discontinuity in buffer write)
7.4 Output protection
design wet operating level around −12 dBFS
apply soft clipper or limiter on wet (or final output)
add DC blocker / HPF to prevent DC/rumble buildup
7.5 CPU bounding
hard spawn budget <= 2 grains per block
fixed 12-voice pool
when full: drop spawns
no per-sample expensive operations beyond simple interpolation/windowing
7.6 Numerical stability
denormal protection
clamp all derived values (delay bounds, filter cutoffs)
if non-finite sample detected: zero and reset the responsible voice/state
7.7 State saving + exact resume policy

Goal: resume exact sim timeline/state (not only Seed).

Current M1 implementation stores:

the APVTS parameter tree under root type `HyphaeState`
the root property `stateVersion = 1`

This means the plugin now has stable versioned parameter save/restore, but not yet the full sim-state resume required by later milestones.

State must include:

all parameter values (APVTS)
mycelium model state (clusters, ages, energies, drift phase)
RNG state
scheduler accumulator state (so density timing continues)
optional: Freeze write gain smoothing state

Non-goal (v0.1): persisting the full delay buffer audio contents across DAW reload (too large).
Expectation: after reopening a project, the sim resumes exactly, but the buffer “fills” with fresh input audio.

Reset/Clear button:

clears buffer
kills all grains
resets sim/scheduler to a defined initial state (or seed-defined initial state)

Transport:

free-run always (no auto reset on play/stop) 8. UI (v0.1 and v0.2)
v0.1 UI

Minimal, utilitarian:

knobs/sliders for core params
buttons for Spore Burst, Freeze, Reset/Clear
no visualization required
v0.2 UI (note only)

Cool branded UI, optional micro-visualization:

cluster dots across stereo field
subtle “hyphae” motif
high-quality layout + animations, but no audio-thread coupling 9. Acceptance criteria
Drop on an Ableton track: audible spores + delay texture, no pop
Density/Size/Scatter/Spread behave musically without blowing level
Freeze ducks writes smoothly
Spore Burst reconfigures without gust spike
Reset/Clear performs a clean reset without clicks
Save/reload project: sim state resumes exactly (buffer refills from live audio) 10. Roadmap note (voices expansion)

v0.2+ increases max voices. Must preserve:

no-click rules
CPU and memory caps
wet normalization scaling (so more voices doesn’t become louder by default)
