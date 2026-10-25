#include "chip-audio.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ChipAudio *make_audio(int source, int occupied) {
    ChipAudioNote note = {100, 1200, 69, 12, 0, 0};
    ChipAudioNote native = {occupied == 1 ? 2300 : 1850, 200, 60, 10, 0, 0};
    ChipAudioTrack tracks[4] = {{CHIP_PULSE, NULL, 0},
                                {CHIP_PULSE, NULL, 0},
                                {CHIP_TRIANGLE, NULL, 0},
                                {CHIP_NOISE, NULL, 0}};
    tracks[source].notes = &note;
    tracks[source].count = 1;
    if (occupied) {
        tracks[1 - source].notes = &native;
        tracks[1 - source].count = 1;
    }
    ChipAudio *a = chip_audio_create(tracks, 6000, NULL);
    assert(a);
    ChipPulseChange changes[] = {{0, .125, 7, 24, .005, .08, .01, .3}};
    ChipPulseOnset onset = {100, 1500};
    assert(chip_audio_set_pulse_changes(a, source, changes, 1, &onset, 1));
    return a;
}
static void delayed_copy_and_native_priority(void) {
    int16_t raw[6000], heard[6000], chunked[6000], native[6000];
    for (int source = 0; source < 2; source++) {
        ChipAudio *a = make_audio(source, 0);
        chip_audio_render(a, raw, 6000, 1u << source);
        chip_audio_reset(a);
        ChipEchoTap tap = {100, 1500, 1800, source, .4};
        assert(chip_audio_set_echo_taps(a, &tap, 1));
        chip_audio_render(a, heard, 6000, 1u << (1 - source));
        long energy = 0;
        for (int i = 0; i < 6000; i++) {
            int expected = i >= 1900 && i < 3300 ? (int)llround(raw[i - 1800] * .4) : 0;
            assert(abs(heard[i] - expected) <= 1);
            energy += abs(heard[i]);
        }
        assert(energy > 10000);
        chip_audio_reset(a);
        for (int i = 0; i < 6000; i += 37)
            chip_audio_render(a, chunked + i, 6000 - i < 37 ? 6000 - i : 37, 1u << (1 - source));
        assert(!memcmp(heard, chunked, sizeof heard));
        chip_audio_reset(a);
        chip_audio_render(a, chunked, 6000, 1u << source);
        assert(!memcmp(raw, chunked, sizeof raw));
        ChipEchoTap invalid = tap;
        invalid.delay = 0;
        assert(!chip_audio_set_echo_taps(a, &invalid, 1));
        chip_audio_reset(a);
        chip_audio_render(a, chunked, 6000, 1u << (1 - source));
        assert(!memcmp(heard, chunked, sizeof heard));
        invalid = tap;
        invalid.source = 2;
        assert(!chip_audio_set_echo_taps(a, &invalid, 1));
        invalid = tap;
        invalid.gain = NAN;
        assert(!chip_audio_set_echo_taps(a, &invalid, 1));
        chip_audio_seek(a, 2000);
        chip_audio_render(a, chunked, 100, 1u << (1 - source));
        energy = 0;
        for (int i = 0; i < 100; i++)
            energy += abs(chunked[i]);
        assert(energy > 0);
        chip_audio_reset(a);
        chip_audio_stop(a);
        chip_audio_render(a, chunked, 100, 15);
        for (int i = 0; i < 100; i++)
            assert(!chunked[i]);
        assert(chip_audio_set_echo_taps(a, NULL, 0));
        chip_audio_reset(a);
        chip_audio_render(a, chunked, 6000, 15);
        assert(!memcmp(raw, chunked, sizeof raw));
        chip_audio_destroy(a);
        for (int occupied = 1; occupied <= 2; occupied++) {
            a = make_audio(source, occupied);
            chip_audio_render(a, native, 6000, 1u << (1 - source));
            chip_audio_reset(a);
            assert(chip_audio_set_echo_taps(a, &tap, 1));
            chip_audio_render(a, heard, 6000, 1u << (1 - source));
            int cutoff = occupied == 1 ? 2300 : 0;
            if (cutoff)
                assert(abs(heard[cutoff] - heard[cutoff - 1]) < 200);
            assert(!memcmp(heard + cutoff, native + cutoff, (6000 - cutoff) * sizeof(int16_t)));
            chip_audio_seek(a, 2900);
            chip_audio_render(a, chunked, 500, 1u << (1 - source));
            for (int i = 0; i < 500; i++)
                assert(!chunked[i]);
            chip_audio_destroy(a);
        }
    }
}
static void no_feedback(void) {
    int16_t heard[6000];
    // A delayed copy is never an input for another tap.
    ChipAudio *a = make_audio(0, 0);
    ChipEchoTap chain[] = {{100, 1500, 1800, 0, .4}, {1900, 3300, 1800, 1, .4}};
    assert(chip_audio_set_echo_taps(a, chain, 2));
    chip_audio_render(a, heard, 6000, 1);
    for (int i = 3700; i < 6000; i++)
        assert(!heard[i]);
    chip_audio_destroy(a);
}
static void written_note_reservation(void) {
    int16_t heard[6000];
    ChipAudio *a;
    ChipEchoTap chain[] = {{100, 1500, 1800, 0, .4}};
    // Priority covers the entire native written note, including its silent gate.
    a = make_audio(0, 0);
    ChipPulseChange settings = {0, .25, 6, 0, 0, 0, 0, 0};
    ChipPulseOnset reserved = {1800, 2100};
    assert(chip_audio_set_pulse_changes(a, 1, &settings, 1, &reserved, 1));
    assert(chip_audio_set_echo_taps(a, chain, 1));
    chip_audio_render(a, heard, 6000, 2);
    for (int i = 0; i < 6000; i++)
        assert(!heard[i]);
    chip_audio_destroy(a);
}
static void live_timbre_preserves_pending_echo(void) {
    int16_t heard[6000], raw[6000];
    ChipAudio *a;
    ChipEchoTap chain[] = {{100, 1500, 1800, 0, .4}};
    // Editing a timbre keeps already queued source samples and the playhead.
    a = make_audio(0, 0);
    ChipAudio *replacement = make_audio(0, 0), *reference = make_audio(0, 0);
    assert(chip_audio_set_echo_taps(a, chain, 1) &&
           chip_audio_set_echo_taps(replacement, chain, 1) &&
           chip_audio_set_echo_taps(reference, chain, 1));
    chip_audio_render(a, heard, 600, 2);
    chip_audio_render(reference, raw, 600, 2);
    chip_audio_exchange_timbres(a, replacement);
    assert(chip_audio_position(a) == 600);
    chip_audio_render(a, heard, 4000, 2);
    chip_audio_render(reference, raw, 4000, 2);
    assert(!memcmp(heard, raw, 4000 * sizeof(int16_t)));
    chip_audio_destroy(a);
    chip_audio_destroy(replacement);
    chip_audio_destroy(reference);
}
static void simultaneous_arrival_priority(void) {
    ChipAudio *a = make_audio(0, 0);
    int16_t source[6000], heard[6000];
    chip_audio_render(a, source, 6000, 1);
    chip_audio_reset(a);
    ChipEchoTap taps[] = {{100, 200, 400, 0, .4}, {200, 300, 300, 0, .4}};
    assert(chip_audio_set_echo_taps(a, taps, 2));
    chip_audio_render(a, heard, 6000, 2);
    for (int i = 0; i < 6000; i++) {
        int expected = i >= 500 && i < 600 ? (int)llround(source[i - 300] * .4) : 0;
        assert(abs(heard[i] - expected) <= 1);
    }
    chip_audio_destroy(a);
}
static void live_edit_cuts_already_queued_echo(void) {
    ChipAudio *a = make_audio(0, 0), *replacement = make_audio(0, 1);
    ChipEchoTap tap = {100, 1500, 1800, 0, .4};
    assert(chip_audio_set_echo_taps(a, &tap, 1));
    assert(chip_audio_set_echo_taps(replacement, &tap, 1));
    int16_t pcm[6000];
    chip_audio_render(a, pcm, 1200, 2);
    chip_audio_exchange_timbres(a, replacement);
    chip_audio_destroy(replacement);
    chip_audio_render(a, pcm, 4800, 2);
    for (int i = 1500; i < 4800; i++)
        assert(pcm[i] == 0);
    chip_audio_destroy(a);
}
int main(void) {
    delayed_copy_and_native_priority();
    no_feedback();
    written_note_reservation();
    live_timbre_preserves_pending_echo();
    simultaneous_arrival_priority();
    live_edit_cuts_already_queued_echo();
    puts("echo: both directions, delayed source timbre/gain, no feedback, native-note priority/no "
         "resume, chunk/reset/seek/stop/mute, validation: PASS");
}
