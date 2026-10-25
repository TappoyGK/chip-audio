// Optional standalone benchmark. Timings are observations, not pass/fail limits.
#include "chip-audio.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
static double seconds(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
int main(void) {
    ChipAudioNote n = {0, 44100 * 70, 69, 15, 0, 0};
    ChipAudioTrack tracks[4] = {{CHIP_PULSE, &n, 1},
                                {CHIP_PULSE, NULL, 0},
                                {CHIP_TRIANGLE, NULL, 0},
                                {CHIP_NOISE, NULL, 0}};
    ChipAudio *a = chip_audio_create(tracks, 44100 * 90, NULL);
    assert(a);
    ChipEchoTap tap = {0, 44100 * 60, 44100 * 30, 0, .5};
    assert(chip_audio_set_echo_taps(a, &tap, 1));
    int64_t target = 44100 * 60;
    double start = seconds();
    chip_audio_seek(a, target);
    printf("active echo, uncached seek: %.3f ms\n", (seconds() - start) * 1000);
    start = seconds();
    assert(chip_audio_prepare_seeks(a, &target, 1));
    printf("prepare one seek off audio thread: %.3f ms\n", (seconds() - start) * 1000);
    int16_t pcm[256];
    double maximum = 0;
    start = seconds();
    for (int i = 0; i < 1000; i++) {
        double before = seconds();
        chip_audio_seek(a, target);
        chip_audio_render(a, pcm, 256, 15);
        double elapsed = seconds() - before;
        if (elapsed > maximum)
            maximum = elapsed;
    }
    printf("prepared seek + 256 samples: mean %.3f ms, maximum %.3f ms (1000 iterations)\n",
           (seconds() - start), maximum * 1000);
    chip_audio_destroy(a);
}
