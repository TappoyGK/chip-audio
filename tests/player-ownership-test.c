#include "chip-audio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ChipAudio *sound(int pitch) {
    ChipAudioNote n = {0, 1000, pitch, 15, 0, 0};
    ChipAudioTrack tracks[4] = {{CHIP_PULSE, &n, 1}, {CHIP_PULSE, NULL, 0},
                               {CHIP_TRIANGLE, NULL, 0}, {CHIP_NOISE, NULL, 0}};
    ChipAudio *a = chip_audio_create(tracks, 1000, NULL);
    assert(a);
    return a;
}
int main(void) {
    // Current, fading, clear-retired, fade-retired, natural-end-retired.
    for (int state = 0; state < 5; state++) {
        ChipAudioPlayer *p = chip_audio_player_create(2, NULL);
        ChipAudioPlayer *q = chip_audio_player_create(2, NULL);
        ChipAudioPlayer *reference = chip_audio_player_create(2, NULL);
        ChipAudio *a = sound(69), *r = sound(69), *b = sound(76), *br = sound(76);
        assert(p && q && reference);
        assert(chip_audio_player_switch(p, a, 128) == CHIP_AUDIO_OK);
        assert(chip_audio_player_switch(reference, r, 128) == CHIP_AUDIO_OK);
        assert(chip_audio_player_switch(q, b, 128) == CHIP_AUDIO_OK);
        if (state == 1 || state == 3) {
            assert(chip_audio_player_stop(p, 128) == CHIP_AUDIO_OK);
            assert(chip_audio_player_stop(reference, 128) == CHIP_AUDIO_OK);
        }
        if (state == 2) {
            chip_audio_player_clear(p);
            chip_audio_player_clear(reference);
        }
        int16_t actual[1024], expected[1024];
        if (state >= 3) {
            size_t frames = state == 3 ? 128 : 1024;
            chip_audio_player_render(p, actual, frames, 15);
            chip_audio_player_render(reference, expected, frames, 15);
            // Isolate ownership rejection from position/finished validation.
            chip_audio_reset(a);
            chip_audio_reset(r);
        }
        assert(chip_audio_player_switch(q, a, 0) == CHIP_AUDIO_INVALID);
        assert(chip_audio_player_switch(p, a, 0) == CHIP_AUDIO_INVALID);
        assert(chip_audio_player_finished(p) == chip_audio_player_finished(reference));
        chip_audio_player_render(p, actual, 64, 15);
        chip_audio_player_render(reference, expected, 64, 15);
        assert(memcmp(actual, expected, 64 * sizeof(*actual)) == 0);
        chip_audio_player_render(q, actual, 64, 15);
        chip_audio_render(br, expected, 64, 15);
        assert(memcmp(actual, expected, 64 * sizeof(*actual)) == 0);
        // Exchanges and reset must not transfer/clear object ownership.
        ChipAudio *spare = sound(72);
        chip_audio_exchange_timbres(a, spare);
        chip_audio_exchange_score(a, spare);
        chip_audio_exchange_percussion(a, spare);
        chip_audio_reset(a);
        assert(chip_audio_player_switch(q, a, 0) == CHIP_AUDIO_INVALID);
        chip_audio_destroy(spare);
        chip_audio_player_clear(p);
        assert(chip_audio_player_switch(q, a, 0) == CHIP_AUDIO_INVALID);
        assert(chip_audio_player_collect(p) == a);
        chip_audio_reset(a);
        assert(chip_audio_player_switch(q, a, 128) == CHIP_AUDIO_OK);
        chip_audio_player_destroy(p);
        chip_audio_player_destroy(q);
        chip_audio_player_destroy(reference);
        chip_audio_destroy(br);
    }
    puts("cross-player ownership: current/fading/retired rejection, unchanged PCM, reset/exchanges, collect and reuse: PASS");
    return 0;
}
