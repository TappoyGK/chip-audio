// Private synthesis implementation. Not part of the public API.
#ifndef CHIP_SYNTH_H
#define CHIP_SYNTH_H
#include <math.h>
#include <stdint.h>
#include <string.h>
// SYNTH_RAMP: samples a voice's level takes to move across the full volume range. Levels
// glide toward each note's volume instead of jumping, and a sound's last note glides to
// silence before it ends, so notes start and stop without a click.
enum { SYNTH_RATE = 44100, SYNTH_RAMP = 128 };
typedef struct {
    int pitch, samples, volume;
} Note;
typedef struct {
    int wave, count;
    Note notes[513];
    double duty, vib_rate, vib_depth, vib_delay, note_age_samples, release_age_samples,
        release_time, decay_time, attack_time, sustain_level, gain;
} Sound;
typedef struct {
    const Sound *sound;
    int note, left;
    double phase;
    uint32_t noise;
    double level;
    int single_interval, fade_at_end;
} Voice;
// A new sound on a voice glides from where the previous one's level was.

static double synth_adsr(const Sound *sound, int sample) {
    if (sound->wave != 0)
        return 1.0;
    double age = fmax(0.0, (sound->note_age_samples + sample) / SYNTH_RATE);
    double release = (sound->release_age_samples + sample) / SYNTH_RATE;
    // Release starts from the actual gate-off level, including mid-attack/decay.
    if (sound->release_time > 0 && release > 0)
        age = fmax(0.0, age - release);
    double level;
    if (sound->attack_time > 0 && age < sound->attack_time)
        level = age / sound->attack_time;
    else if (sound->decay_time > 0)
        level = sound->sustain_level +
                (1 - sound->sustain_level) *
                    fmax(0.0, 1 - (age - sound->attack_time) / sound->decay_time);
    else
        level = sound->sustain_level;
    if (sound->release_time > 0)
        level *= fmax(0.0, fmin(1.0, 1 - release / sound->release_time));
    return level;
}
static void synth_start(Voice *voice, const Sound *sound, int channel) {
    double level = voice->sound ? voice->level : 0;
    *voice = (Voice){sound, 0, 0, 0, (uint32_t)(channel + 1) * 7919, level, 0, 0};
}
// One sample's step of a voice's level toward its note's volume (0 near the sound's end).
static void synth_glide(Voice *v, const Note *note, double envelope) {
    double target = note->volume / 15.0;
    target *= envelope;
    if ((v->single_interval ? v->fade_at_end : v->note == v->sound->count - 1) &&
        v->left <= SYNTH_RAMP)
        target = 0;
    double step = 1.0 / SYNTH_RAMP;
    v->level = v->level < target ? fmin(target, v->level + step) : fmax(target, v->level - step);
}
// Saturate only at the final PCM boundary; intermediate sums remain wide.
static int16_t synth_pcm(double value) {
    return value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
}
// Bind a single prepared interval without inventing a second array element.
// Caller renders at most voice->left samples before binding the next interval.
static void synth_bind_interval(Voice *voice, Sound *sound, Note note, int final) {
    sound->notes[0] = note;
    sound->count = 1;
    voice->sound = sound;
    voice->note = 0;
    voice->left = note.samples;
    voice->single_interval = 1;
    voice->fade_at_end = final;
}
static void synth_render_wide(Voice voices[4], int16_t *output, int count, unsigned mask,
                              int16_t *channels[4], int32_t *wide) {
    for (int i = 0; i < count; i++) {
        double mixed = 0;
        for (int ch = 0; ch < 4; ch++) {
            if (channels)
                channels[ch][i] = 0;
            Voice *v = &voices[ch];
            if (!v->sound)
                continue;
            const Sound *sound = v->sound;
            if (v->left == 0) {
                if (v->note >= sound->count) {
                    v->sound = 0;
                    v->level = 0;
                    continue;
                }
                v->left = sound->notes[v->note].samples;
            }
            Note note = sound->notes[v->note];
            double frequency = 440.0 * pow(2.0, (note.pitch - 69) / 12.0);
            if (sound->wave == 0 && sound->vib_depth > 0) {
                double age = (sound->note_age_samples + i) / SYNTH_RATE - sound->vib_delay;
                if (age >= 0)
                    frequency *=
                        pow(2.0, sound->vib_depth * sin(6.283185307179586 * sound->vib_rate * age) /
                                     1200.0);
            }
            double duty = sound->duty > 0 ? sound->duty : 0.25;
            double previous = v->phase;
            v->phase += frequency / SYNTH_RATE;
            v->phase -= floor(v->phase);
            if (v->phase < previous) {
                v->noise ^= v->noise << 13;
                v->noise ^= v->noise >> 17;
                v->noise ^= v->noise << 5;
            }
            double signal = sound->wave == 0   ? (v->phase < duty ? 1 : -1)
                            : sound->wave == 1 ? (1 - 4 * fabs(v->phase - 0.5))
                                               : ((v->noise & 1) ? 1 : -1);
            synth_glide(v, &note, synth_adsr(sound, i));
            int32_t before = (int32_t)mixed;
            if (mask & (1u << ch))
                mixed += signal * v->level * 6000 * (sound->gain > 0 ? sound->gain : 1);
            // Allocate integer rounding in channel order: stems telescope to the
            // original cast of the sum, without changing any audible sample.
            if (channels)
                channels[ch][i] = (int16_t)((int32_t)mixed - before);
            if (--v->left == 0)
                v->note++;
        }
        if (output)
            output[i] = synth_pcm(mixed);
        if (wide)
            wide[i] = (int32_t)mixed;
    }
}
static void synth_render_channels(Voice voices[4], int16_t *output, int count, unsigned mask,
                                  int16_t *channels[4]) {
    synth_render_wide(voices, output, count, mask, channels, NULL);
}
static void synth_render_mask(Voice voices[4], int16_t *output, int count, unsigned mask) {
    synth_render_channels(voices, output, count, mask, NULL);
}
static void synth_render(Voice voices[4], int16_t *output, int count) {
    synth_render_mask(voices, output, count, 15);
}
#endif
