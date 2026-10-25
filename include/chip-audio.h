#ifndef CHIP_AUDIO_H
#define CHIP_AUDIO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CHIP_AUDIO_SAMPLE_RATE 44100
#define CHIP_AUDIO_CHANNELS 4

typedef enum { CHIP_PULSE = 0, CHIP_TRIANGLE = 1, CHIP_NOISE = 2 } ChipWave;
// Absolute sample positions, not musical ticks. Notes must be sorted and nonoverlapping.
// Optional release envelope: absolute gate-off sample (may precede a seek), decay length; zero
// disables.
typedef struct {
    int64_t at, duration;
    int pitch, volume;
    int64_t release_at, release_samples;
} ChipAudioNote;
typedef struct {
    ChipWave wave;
    const ChipAudioNote *notes;
    size_t count;
} ChipAudioTrack;
typedef enum { CHIP_AUDIO_OK = 0, CHIP_AUDIO_INVALID, CHIP_AUDIO_MEMORY } ChipAudioError;
typedef struct ChipAudio ChipAudio;
// Copies input. No public note-count limit. Creation and setup functions may allocate.
// Every channel accepts any ChipWave. Pitch 0..127, volume 1..15, length > 0.
ChipAudio *chip_audio_create(const ChipAudioTrack tracks[4], int64_t length, ChipAudioError *error);
// Separate DPCM voice, mixed with the percussion part (mask bit 3).
// Sorted onsets; a later kick retriggers the voice. offset resumes a clipped hit.
typedef struct {
    int64_t at, duration, offset;
    int volume;
} ChipDPCMHit;
int64_t chip_audio_dpcm_kick_length(void);
// Attack 25..150 percent; decay 5..800 ms. Invalid input leaves settings unchanged.
int chip_audio_set_dpcm_shape(ChipAudio *audio, int attack_percent, int decay_ms);
// Audio-thread operations; preserve transport. Timbres also exchanges pulse
// timelines/automation without resetting oscillator phase. Destroy spare later.
// Gain for the five independent voices (0..3 synth, 4 DPCM). Finite (0,2], default 1.
int chip_audio_set_voice_gain(ChipAudio *audio, int voice, double gain);
void chip_audio_exchange_timbres(ChipAudio *a, ChipAudio *b);
// Replace all note timelines/settings, including channel 2 and total length.
// Preserve position (clamped to the new length), oscillator phases and queued
// compatible echoes. Adopt spare's prepared seeks. No allocation or destruction.
// Caller must update its loop boundaries. Consume/destroy spare off audio thread.
void chip_audio_exchange_score(ChipAudio *audio, ChipAudio *spare);
void chip_audio_exchange_percussion(ChipAudio *audio, ChipAudio *spare);
// Copies events before playback. Empty input clears kicks; invalid input is atomic.
int chip_audio_set_dpcm_kicks(ChipAudio *audio, const ChipDPCMHit *hits, size_t count);
// Duty ratio: 0.125, 0.25 (default), 0.5, 0.75. Owning thread only.
int chip_audio_set_pulse_duty(ChipAudio *audio, int channel, double duty);
// at: samples; first must be 0, strictly increasing, below length.
// duty: .125/.25/.5/.75; rate: 1..12 Hz; depth: 0..100 cents;
// delay: 0..1 s; decay: 0..2 s; attack: 0..0.5 s; sustain_drop: 0..1.
typedef struct {
    int64_t at;
    double duty, rate, depth, delay, decay, attack, sustain_drop;
} ChipPulseChange;
typedef struct {
    int64_t at, end;
} ChipPulseOnset;
// Setup: part 0 or 1, nonempty changes. Copies arrays; rejects invalid input atomically.
// Onsets: sorted, nonoverlapping [at,end), may start before zero. Optional: when
// omitted, envelope/vibrato age follows the current note segment. Setup echoes afterward.
int chip_audio_set_pulse_changes(ChipAudio *audio, int part, const ChipPulseChange *changes,
                                 size_t count, const ChipPulseOnset *onsets, size_t onset_count);
// One delayed copy of a source pulse interval, routed to the other pulse track.
// Times/delay are samples; gain is (0,1]. Intervals must be sorted by at and
// nonoverlapping per source. Delay is 1 sample..30 seconds at the fixed sample rate.
// Setup only: copies input and allocates a bounded delay buffer. Empty clears.
// Native destination notes win; an interrupted tap never resumes. No feedback.
// Output is clipped at the existing audio length. Reset clears history; seek rebuilds the required
// delay history.
typedef struct {
    int64_t at, end, delay;
    int source;
    double gain;
} ChipEchoTap;
int chip_audio_set_echo_taps(ChipAudio *audio, const ChipEchoTap *taps, size_t count);
void chip_audio_destroy(ChipAudio *audio);
// Resets phase and noise state as well as the cursor. Ready for deterministic looping.
void chip_audio_reset(ChipAudio *audio);
// Reset oscillators and jump to an absolute sample (clamped to [0,length]).
// Unprepared echo targets rebuild history: use off the real-time thread.
// Prepare all callback jump destinations after final configuration.
// Channel 2 is retained during timbre exchange: each seek resolves its own
// timeline and resets its oscillator at the destination, independent of echo
// history. No channel-2 settings or cursors are imported from a spare.
void chip_audio_seek(ChipAudio *audio, int64_t sample);
// Setup only, after all settings: precompute jump states. Resets playback to zero.
// Prepared targets seek without allocation, history rendering or delay-sized copies.
// Reprepare after configuration edits; live timbre exchange adopts spare checkpoints.
// Failure retains old checkpoints but resets playback. Input samples must be in [0,length].
int chip_audio_prepare_seeks(ChipAudio *audio, const int64_t *samples, size_t count);
// Stopped render calls output silence and do not advance. Reset resumes playback.
void chip_audio_stop(ChipAudio *audio);
// Mono signed 16-bit PCM. mask bit 0..3 controls audibility, not clock advancement.
// Zero-fills after the end. No allocation, locks or OS/device calls during rendering.
// One owning thread: do not call reset/stop/destroy concurrently with render.
void chip_audio_render(ChipAudio *audio, int16_t *output, size_t frames, unsigned mask);
int64_t chip_audio_position(const ChipAudio *audio);
int chip_audio_finished(const ChipAudio *audio);
#ifdef __cplusplus
}
#endif
#endif
