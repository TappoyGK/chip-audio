# chip-audio

Portable C11 chip synthesizer. No OS, window system, audio device, or application framework is required. Input is sample-timed events; output is 44,100 Hz signed 16-bit mono PCM.

## Build and test

```sh
make test
```

Or use CMake (including Windows toolchains):

```sh
cmake -S . -B build -DCHIP_AUDIO_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /your/install/prefix
```

The library has no build-time dependency on Python or application source code. `scripts/make-kick.py` regenerates the committed synthetic DPCM sample; it is not a build dependency. Windows execution has not yet been verified.

## Consume

Public interfaces: `include/chip-audio.h`, `include/chip-drums.h`.
Link `libchip-audio.a` (or the platform's static library equivalent) and the C math library where required. The headers support C++ linkage.

For CMake, use `add_subdirectory(path/to/chip-audio)` and link `chip-audio::chip-audio`, or use `find_package(chip-audio CONFIG REQUIRED)` after installation.

```c
#include "chip-audio.h"

ChipAudioNote note = {.at=0, .duration=22050, .pitch=69, .volume=8};
ChipAudioTrack tracks[4] = {
    {CHIP_PULSE, &note, 1}, {CHIP_PULSE, NULL, 0},
    {CHIP_TRIANGLE, NULL, 0}, {CHIP_NOISE, NULL, 0}
};
ChipAudio *audio = chip_audio_create(tracks, 44100, NULL);
if (audio) {
    int16_t pcm[512];
    chip_audio_render(audio, pcm, 512, 15);
    /* Send pcm to the application's audio output. */
    chip_audio_destroy(audio);
}
```

## Contract

- Four synth tracks plus an independent DPCM kick. The kick shares mute bit 3 with noise; voice gain index 4 controls the kick separately.
- Sorted, nonoverlapping note events; times are absolute samples, not ticks or frames. Input arrays are copied by setup functions.
- Pulse duty, vibrato and envelope automation; DPCM and noise drum helpers. No score-file parser or device output.
- Audio rendering performs no allocation, locks, or OS calls. Use a single owning audio thread. Creation and allocating setters belong outside the audio callback; do not destroy/reset/seek concurrently with rendering.
- `exchange_timbres`/`exchange_percussion` exchange prepared states on the audio thread; dispose of the spare outside that callback.
- `reset` restores deterministic phase/noise state; `seek` resets and positions the timeline. The caller controls looping.
- Sample rate is fixed at 44,100 Hz. The host performs any required device-format conversion.

## Pulse echo

`chip_audio_set_echo_taps` schedules a single delayed copy of each supplied pulse
interval on the opposite pulse track. The caller supplies sample positions,
delay (1 sample through 30 seconds), source track (0 or 1), and gain (0..1,
exclusive of zero). This is a voice replacement, not an additional mixed voice.
The delayed waveform retains the source pitch, duty, envelope, modulation and
articulation. Echo output is never fed back into another tap.

Configure native pulse automation/onsets first, then echo taps, before playback.
Native destination events have priority, including their declared onset/end
interval and release. A tap blocked at arrival is discarded; one interrupted
later is cut permanently, with a short fade before the scheduled interruption
to suppress clicks. Later arriving taps also cut earlier echo tails.
An empty schedule disables echo. The caller represents ON/OFF by scheduling
only intervals whose original note began while enabled; already scheduled tails
can finish. Output never extends the existing audio length.

Rendering uses a preallocated delay buffer and no extra oscillator. Mask bits
control the output tracks (the destination bit controls the echo), while original
source waveforms are captured regardless of mute. Reset clears history; seek
reconstructs the required bounded history without allocating. Setters that replace
native event schedules require rebuilding the echo schedule before playback.

## Dependency boundary

The library depends only on the C standard library and math functions.
Applications consume the public headers and compiled library. Application-specific
adapters, file formats, playback policies and integration instructions belong to
the consuming application, not this repository. The synthesis core in `src/`
is private and is not installed as a public interface.

## 0.3: PCM safety, echo planning and prepared seeks

All five voices are summed in a wide accumulator and saturated once to signed
16-bit PCM, including DPCM and delayed stems. Pulse oscillators work on every
channel; pulse automation remains available on channels 0 and 1. Omitting the
onset array derives envelope/modulation age from the current note segment.
Sample age is computed before converting to seconds so render block size does
not change the envelope's rounding.

Echo setup resolves native-note reservations and competing arrivals before
allocating delay memory. Interval merging and binary searches replace full
scans for every tap; arrival ordering is `O(T log T)`. At equal arrival times,
the later input tap wins. A cut tap never resumes. Silent sources (including
their short level glide), blocked destinations and arrivals after the end do
not require a delay buffer. Each active direction allocates only its own maximum
delay. Reset invalidates slots with a generation counter rather than clearing
the entire buffer.

### Preparing real-time jumps

`chip_audio_seek` without a prepared target may rebuild up to 30 seconds of
history and must not run in a real-time callback when echoes are active.
After configuring notes, gains, drums, pulse automation and echoes, call:

```c
int64_t destinations[] = {0, loop_start, alternate_ending};
if (!chip_audio_prepare_seeks(audio, destinations, 3)) {
    /* Handle setup allocation failure before starting the audio device. */
}
```

This setup call renders each jump history and saves its playback state. It
resets playback to zero. Subsequent `chip_audio_seek` calls to those exact
targets restore oscillator/cursor state and read immutable saved echo slots;
they do not allocate, render history, or copy delay-sized buffers. Lookup is
linear in the number of prepared destinations, so prepare only actual jump
points. Each saved state uses approximately one additional copy of the active
delay buffers; this is an explicit memory/time tradeoff for real-time jumps.
Empty input clears prepared targets. Invalid input leaves state untouched;
an allocation failure during preparation keeps old targets but resets playback.

Reprepare after changing configuration. When publishing a live timbre update,
prepare the same jump targets on the spare first. Timbre exchange adopts its
prepared states while preserving the current oscillator phase and, for compatible
echo layouts, queued PCM. Saved histories stay alive until their final reader
releases them; disposal occurs when a spare or the audio object is destroyed
outside the callback. The exchanged spare is consumed and must only be destroyed,
not rendered or reused. The two objects must have equal lengths.

### API contracts

All times in note, onset, echo and DPCM events are absolute **samples**, except
for the following pulse change fields:

| Field | Unit / accepted range |
| --- | --- |
| `at` | Samples; first change must be 0, later changes strictly increasing and below audio length |
| `duty` | Ratio: 0.125, 0.25, 0.5 or 0.75 |
| `rate` | Hz, 1–12 |
| `depth` | Cents, 0–100 |
| `delay` | Seconds, 0–1 |
| `attack` | Seconds, 0–0.5 |
| `decay` | Seconds, 0–2 |
| `sustain_drop` | Ratio, 0–1; sustain amplitude is `1 - sustain_drop` |

Pulse changes require a nonempty array and part 0 or 1. Onsets are optional,
sorted, nonoverlapping `[at,end)` intervals; `at` may be negative for an onset
preceding a clipped playback origin. They also reserve the entire written
note against echo, including silent gates. Configuration must precede echo
planning. Arrays are copied and caller storage may then be released.

Voice gain accepts finite values in **(0, 2]**, default 1; use the render mask to
mute. DPCM shape accepts attack 25–150 percent and decay 5–800 milliseconds.
Empty kick/echo arrays clear those events; empty pulse-change arrays are rejected.
Setters return 1 on success and 0 for invalid arguments or allocation failure;
existing settings are retained on failure. The current API intentionally does
not distinguish allocation failure from validation failure for setters.
Creation additionally returns `ChipAudioError`.

| Live timbre exchange | Behavior |
| --- | --- |
| Pulse 0/1 note timelines, automation and gains | Replaced by spare configuration |
| Noise timeline and DPCM events/shape | Replaced |
| Triangle configuration and DPCM gain | Retained on the active object |
| Transport position, stopped state, oscillator phase/noise/level | Retained |
| Pulse/noise interval cursors | Rebound to new timelines on next render |
| Echo history | Retained only for matching resolved tap identity and buffer layout; otherwise reset |
| Prepared jump states | Replaced by spare states |

Standalone percussion exchange replaces noise and DPCM configuration, resets
the noise oscillator, and preserves transport, melodic voices and DPCM gain.
As with all configuration changes, rebuild prepared targets if using this API.

The implementation separates echo interval queries (`src/echo-plan.h`), pulse
configuration application, interval binding, and prepared playback snapshots.
The private mixer now accepts an explicit single-interval/final-fade binding;
it no longer pretends a second note exists to suppress a fade. Public API
contract tests are separate from the private waveform reference and interval
rule tests. `tests/seek-benchmark.c` is an optional timing probe, not a pass/fail
performance promise. Build with sanitizers to check clipping and snapshot lifetime.


### 0.3.1: retained channel and prepared jumps

A timbre exchange retains channel 2's configuration and timeline. Its seek
state is therefore not part of the snapshots exchanged with a spare. On every
seek (prepared or unprepared), channel 2 resolves its own interval by binary
search and resets its oscillator at the destination. Pulse echo warm-up no
longer advances this independent oscillator. This makes its seek behavior
independent of echo enablement or delay and prevents importing a gain, note
index or oscillator state belonging to another timeline. Normal playback and
the timbre exchange itself still preserve its current phase and level.

This also applies when channel 2 uses a pulse or noise wave. No replay,
allocation, or delay-sized copy is needed for this retained voice: locating
its interval costs O(log N). Prepared snapshots contain only the replaced
channels (0, 1 and 3), their automation and echo history. Tests cover different
retained gains, waveforms and timeline lengths; active objects with and without
prepared targets; echo enable/disable/delay changes; segment/end boundaries;
and successive exchanges followed by destruction of each spare.

### Full score exchange (0.4.0)

`chip_audio_exchange_score(active, spare)` adopts all four synth timelines,
DPCM settings/gain, echo configuration and prepared seeks. Unlike timbre-only
exchange it also replaces channel 2 and allows a different total length.
It retains oscillator phases and current sample position, clamping position to
new length when shortened. Compatible queued echoes are retained using the same
rules as timbre exchange. The caller updates transport boundaries; this API does
not map time across tempo changes. Prepare the spare off the audio thread,
exchange on the owning audio thread, then destroy the consumed spare off that
thread. No allocation, destruction or historical seek replay occurs in exchange.
