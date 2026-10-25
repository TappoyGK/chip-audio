#ifndef CHIP_DRUMS_H
#define CHIP_DRUMS_H
#include "chip-audio.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    CHIP_DRUM_NONE = 0,
    CHIP_DRUM_BASS,
    CHIP_DRUM_SNARE,
    CHIP_DRUM_OPEN,
    CHIP_DRUM_CLOSE
} ChipDrumPreset;
#define CHIP_DRUM_MAX_SEGMENTS 8
// Expand one drum into an envelope on a single CHIP_NOISE channel. Times are
// absolute samples. Duration is a hard upper bound; long slots do not stretch tails.
// Returns 0 for invalid inputs or insufficient capacity. No allocation.
size_t chip_drum_expand(ChipDrumPreset preset, int64_t at, int64_t duration, int volume,
                        ChipAudioNote *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
