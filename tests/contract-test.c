#include "chip-audio.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ChipAudio *make_source(int channel, int64_t length) {
    ChipAudioNote note = {0, length, 69, 15, 0, 0};
    ChipAudioTrack tracks[4] = {
        {CHIP_PULSE, NULL, 0}, {CHIP_PULSE, NULL, 0}, {CHIP_PULSE, NULL, 0}, {CHIP_PULSE, NULL, 0}};
    tracks[channel].notes = &note;
    tracks[channel].count = 1;
    ChipAudio *a = chip_audio_create(tracks, length, NULL);
    assert(a);
    return a;
}
static void pulse_on_every_channel(void) {
    for (int p = 0; p < 4; p++) {
        ChipAudio *a = make_source(p, 6000);
        int16_t pcm[5000];
        chip_audio_render(a, pcm, 5000, 1u << p);
        assert(abs(pcm[4500]) == 6000);
        chip_audio_reset(a);
        chip_audio_render(a, pcm, 5000, 1u << p);
        assert(abs(pcm[4500]) == 6000);
        chip_audio_destroy(a);
    }
}
static void omitted_onsets_are_chunk_independent(void) {
    ChipAudio *a = make_source(0, 12000);
    ChipPulseChange change = {0, .25, 6, 24, 0, .1, .1, .2};
    assert(chip_audio_set_pulse_changes(a, 0, &change, 1, NULL, 0));
    int16_t whole[10000], split[10000];
    chip_audio_render(a, whole, 10000, 1);
    chip_audio_reset(a);
    for (int i = 0; i < 10000; i += 100)
        chip_audio_render(a, split + i, 100, 1);
    for (int i = 0; i < 10000; i++)
        if (whole[i] != split[i]) {
            fprintf(stderr, "first chunk difference at %d: %d / %d\n", i, whole[i], split[i]);
            break;
        }
    assert(!memcmp(whole, split, sizeof whole));
    assert(abs(whole[4500]) > 5000);
    chip_audio_seek(a, 4500);
    chip_audio_render(a, split, 100, 1);
    assert(abs(split[99]) > 4000);
    chip_audio_destroy(a);
}
static void saturating_gain(void) {
    ChipAudioNote note = {0, 6000, 69, 15, 0, 0};
    ChipAudioTrack tracks[4];
    for (int p = 0; p < 4; p++)
        tracks[p] = (ChipAudioTrack){CHIP_PULSE, &note, 1};
    ChipAudio *a = chip_audio_create(tracks, 6000, NULL);
    assert(a);
    for (int p = 0; p < 4; p++)
        assert(chip_audio_set_voice_gain(a, p, 2));
    int16_t pcm[5000];
    chip_audio_render(a, pcm, 5000, 15);
    int positive = 0, negative = 0;
    for (int i = 500; i < 5000; i++) {
        assert(pcm[i] == 32767 || pcm[i] == -32768);
        positive += pcm[i] == 32767;
        negative += pcm[i] == -32768;
    }
    assert(positive && negative);
    chip_audio_destroy(a);
    // Exercise the independent stem path as well as the direct mixer under UBSan.
    tracks[0].wave = CHIP_PULSE;
    tracks[1].count = 0;
    tracks[2].wave = CHIP_TRIANGLE;
    tracks[3].wave = CHIP_NOISE;
    a = chip_audio_create(tracks, 6000, NULL);
    assert(a);
    for (int p = 0; p < 4; p++)
        assert(chip_audio_set_voice_gain(a, p, 2));
    ChipEchoTap tap = {0, 4000, 500, 0, 1};
    assert(chip_audio_set_echo_taps(a, &tap, 1));
    chip_audio_render(a, pcm, 5000, 15);
    chip_audio_destroy(a);
}
static void kick_is_mixed_before_clipping(void) {
    ChipAudioNote note = {0, 12000, 69, 15, 0, 0};
    ChipAudioTrack tracks[4];
    for (int p = 0; p < 4; p++)
        tracks[p] = (ChipAudioTrack){CHIP_PULSE, &note, 1};
    ChipAudio *mix = chip_audio_create(tracks, 12000, NULL), *single = make_source(0, 12000);
    for (int p = 0; p < 4; p++)
        tracks[p].count = 0;
    ChipAudio *kick = chip_audio_create(tracks, 12000, NULL);
    assert(mix && kick);
    for (int p = 0; p < 5; p++)
        assert(chip_audio_set_voice_gain(mix, p, 2));
    assert(chip_audio_set_voice_gain(single, 0, 2));
    assert(chip_audio_set_voice_gain(kick, 4, 2));
    ChipDPCMHit hit = {100, 10000, 0, 15};
    assert(chip_audio_set_dpcm_kicks(mix, &hit, 1));
    assert(chip_audio_set_dpcm_kicks(kick, &hit, 1));
    int16_t actual[10000], tone[10000], drum[10000];
    chip_audio_render(mix, actual, 10000, 15);
    chip_audio_render(single, tone, 10000, 1);
    chip_audio_render(kick, drum, 10000, 8);
    for (int i = 0; i < 10000; i++) {
        int expected = 4 * tone[i] + drum[i];
        if (expected > 32767)
            expected = 32767;
        if (expected < -32768)
            expected = -32768;
        assert(abs(actual[i] - expected) <= 4);
    }
    chip_audio_destroy(mix);
    chip_audio_destroy(single);
    chip_audio_destroy(kick);
}
static ChipAudio *echo_source(void) {
    ChipAudio *a = make_source(0, 12000);
    ChipEchoTap tap = {0, 8000, 3000, 0, .5};
    assert(chip_audio_set_echo_taps(a, &tap, 1));
    return a;
}
static void prepared_seek_and_exchange(void) {
    ChipAudio *a = echo_source(), *b = echo_source(), *reference = echo_source();
    int64_t targets[] = {4000, 7000};
    int16_t expected[5000], actual[5000];
    assert(chip_audio_prepare_seeks(a, targets, 2));
    assert(chip_audio_position(a) == 0);
    for (int pass = 0; pass < 8; pass++) {
        int64_t target = targets[pass % 2];
        chip_audio_seek(reference, target);
        chip_audio_render(reference, expected, 5000, 15);
        chip_audio_seek(a, target);
        for (int i = 0; i < 5000; i += 100)
            chip_audio_render(a, actual + i, 100, 15);
        assert(!memcmp(expected, actual, sizeof expected));
    }
    assert(chip_audio_prepare_seeks(b, targets, 2));
    chip_audio_seek(a, 4000);
    chip_audio_seek(reference, 4000);
    chip_audio_render(a, actual, 300, 15);
    chip_audio_render(reference, expected, 300, 15);
    chip_audio_exchange_timbres(a, b);
    chip_audio_destroy(b); // Old seed remains alive while queued samples are used.
    chip_audio_render(a, actual, 5000, 15);
    chip_audio_render(reference, expected, 5000, 15);
    assert(!memcmp(actual, expected, sizeof actual));
    chip_audio_seek(a, 7000);
    chip_audio_seek(reference, 7000);
    chip_audio_render(a, actual, 5000, 15);
    chip_audio_render(reference, expected, 5000, 15);
    assert(!memcmp(actual, expected, sizeof actual));
    chip_audio_destroy(a);
    chip_audio_destroy(reference);
}
static ChipAudio *retained_channel_fixture(int varied, int delay, double gain, ChipWave wave) {
    ChipAudioNote note = {0, 10000, 69, 15, 0, 0}, sequence[20];
    for (int i = 0; i < 20; i++)
        sequence[i] = (ChipAudioNote){500 * i, 200, 60 + i % 12, 15, 0, 0};
    ChipAudioTrack tracks[4] = {{CHIP_PULSE, &note, 1},
                                {CHIP_PULSE, NULL, 0},
                                {wave, varied ? sequence : &note, varied ? 20 : 1},
                                {CHIP_NOISE, NULL, 0}};
    ChipAudio *a = chip_audio_create(tracks, 10000, NULL);
    assert(a);
    assert(chip_audio_set_voice_gain(a, 2, gain));
    if (delay) {
        ChipEchoTap tap = {0, 8000, delay, 0, .5};
        assert(chip_audio_set_echo_taps(a, &tap, 1));
    }
    return a;
}
// Echo stems share integer rounding with the other voices; enabling that path
// may change an isolated channel by one LSB, but not its waveform or envelope.
static void assert_retained_pcm(const int16_t *actual, const int16_t *expected) {
    for (int i = 0; i < 512; i++)
        assert(abs(actual[i] - expected[i]) <= 1);
}
static void exchanged_seeks_preserve_retained_channel(void) {
    const int delays[][2] = {{0, 0}, {1000, 1000}, {0, 1000}, {1000, 0}, {1000, 2000}};
    int64_t targets[] = {0, 199, 200, 500, 4000, 9999, 10000};
    for (int wave = CHIP_PULSE; wave <= CHIP_NOISE; wave++)
        for (int varied = 0; varied < 2; varied++)
            for (size_t scenario = 0; scenario < sizeof(delays) / sizeof(delays[0]); scenario++)
                for (int prepare_active = 0; prepare_active < 2; prepare_active++) {
                    ChipAudio *a =
                        retained_channel_fixture(varied, delays[scenario][0], 1, (ChipWave)wave);
                    ChipAudio *b =
                        retained_channel_fixture(!varied, delays[scenario][1], 2, CHIP_TRIANGLE);
                    ChipAudio *reference =
                        retained_channel_fixture(varied, delays[scenario][0], 1, (ChipWave)wave);
                    if (prepare_active)
                        assert(chip_audio_prepare_seeks(a, targets, 7));
                    assert(chip_audio_prepare_seeks(b, targets, 7));
                    int16_t actual[512], expected[512];
                    // The exchange itself still preserves the retained oscillator's ongoing phase.
                    chip_audio_render(a, actual, 512, 4);
                    chip_audio_render(reference, expected, 512, 4);
                    chip_audio_exchange_timbres(a, b);
                    chip_audio_destroy(b);
                    chip_audio_render(a, actual, 512, 4);
                    chip_audio_render(reference, expected, 512, 4);
                    assert_retained_pcm(actual, expected);
                    for (int pass = 0; pass < 2; pass++)
                        for (size_t j = 0; j < 7; j++) {
                            chip_audio_seek(a, targets[j]);
                            chip_audio_seek(reference, targets[j]);
                            chip_audio_render(reference, expected, 512, 4);
                            for (size_t offset = 0; offset < 512;) {
                                size_t count = 512 - offset < 37 ? 512 - offset : 37;
                                chip_audio_render(a, actual + offset, count, 4);
                                offset += count;
                            }
                            assert_retained_pcm(actual, expected);
                        }
                    // A second exchange must not accumulate imported retained-channel state.
                    b = retained_channel_fixture(!varied, 1000, 2, CHIP_TRIANGLE);
                    assert(chip_audio_prepare_seeks(b, targets, 7));
                    chip_audio_exchange_timbres(a, b);
                    chip_audio_destroy(b);
                    chip_audio_seek(a, 4000);
                    chip_audio_seek(reference, 4000);
                    chip_audio_render(a, actual, 512, 4);
                    chip_audio_render(reference, expected, 512, 4);
                    assert_retained_pcm(actual, expected);
                    chip_audio_destroy(a);
                    chip_audio_destroy(reference);
                }
    puts("retained channel: different gains/waves/timelines, echo enable/disable/delay, both cache "
         "states, boundaries and repeated exchanges: PASS");
}
static void live_score_exchange(void) {
    for (int p = 0; p < 4; p++) {
        ChipAudioNote old_note = {0, 12000, 69, 10, 0, 0};
        ChipAudioNote new_notes[] = {{0, 4096, 69, 10, 0, 0}, {4096, 4096, 72, 12, 0, 0}};
        ChipAudioTrack tracks[4] = {{CHIP_PULSE, NULL, 0}, {CHIP_PULSE, NULL, 0},
                                   {CHIP_TRIANGLE, NULL, 0}, {CHIP_NOISE, NULL, 0}};
        tracks[p].notes = &old_note; tracks[p].count = 1;
        ChipAudio *a = chip_audio_create(tracks, 12000, NULL);
        tracks[p].notes = new_notes; tracks[p].count = 2;
        ChipAudio *b = chip_audio_create(tracks, 16000, NULL);
        ChipAudio *reference = chip_audio_create(tracks, 16000, NULL);
        assert(a && b && reference);
        int64_t target = 4096;
        assert(chip_audio_prepare_seeks(b, &target, 1));
        assert(chip_audio_prepare_seeks(reference, &target, 1));
        chip_audio_seek(reference, 0);
        int16_t discarded[2048], actual[8192], expected[8192];
        chip_audio_render(a, discarded, 2048, 1u << p);
        chip_audio_render(reference, discarded, 2048, 1u << p);
        chip_audio_exchange_score(a, b);chip_audio_destroy(b);
        assert(chip_audio_position(a) == 2048);
        chip_audio_render(a, actual, 8192, 1u << p);
        chip_audio_render(reference, expected, 8192, 1u << p);
        assert(!memcmp(actual, expected, sizeof(actual)));
        chip_audio_seek(a, target);chip_audio_seek(reference, target);
        chip_audio_render(a, actual, 8192, 1u << p);
        chip_audio_render(reference, expected, 8192, 1u << p);
        assert(!memcmp(actual, expected, sizeof(actual)));
        // Shorten the score and remove all notes while beyond the new end.
        tracks[p].count = 0;b = chip_audio_create(tracks, 1000, NULL);assert(b);
        chip_audio_exchange_score(a, b);chip_audio_destroy(b);
        assert(chip_audio_position(a)==1000&&chip_audio_finished(a));
        chip_audio_seek(a,0);chip_audio_render(a,actual,512,15);
        for(int j=0;j<512;j++)assert(!actual[j]);
        chip_audio_destroy(a);chip_audio_destroy(reference);
    }
    puts("live score: four channels, changed notes/length, continuous PCM, prepared seeks, spare disposal, shrink/removal: PASS");
}
int main(void) {
    live_score_exchange();
    pulse_on_every_channel();
    omitted_onsets_are_chunk_independent();
    saturating_gain();
    kick_is_mixed_before_clipping();
    prepared_seek_and_exchange();
    exchanged_seeks_preserve_retained_channel();
    puts("contracts: pulse channels 0..3, omitted onsets, saturated mix, prepared "
         "seek/reset/exchange lifetime: PASS");
}
