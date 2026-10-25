#include "chip-drums.h"
typedef struct {
    int end_ms, pitch, volume;
} Stage;
size_t chip_drum_expand(ChipDrumPreset preset, int64_t at, int64_t duration, int volume,
                        ChipAudioNote *out, size_t capacity) {
    static const Stage bass[] = {{3, 90, 15}, {10, 70, 14}, {25, 55, 12}, {45, 43, 9},
                                 {70, 36, 6}, {100, 30, 4}, {140, 26, 1}};
    static const Stage snare[] = {{2, 127, 15}, {6, 120, 14}, {14, 112, 11}, {26, 115, 8},
                                  {42, 108, 5}, {60, 111, 2}, {80, 106, 1}};
    static const Stage open[] = {{8, 127, 12},  {25, 126, 11}, {55, 127, 10}, {100, 125, 8},
                                 {170, 127, 6}, {250, 126, 4}, {340, 127, 2}, {440, 125, 1}};
    static const Stage close[] = {{3, 127, 13}, {8, 127, 11}, {16, 126, 8},
                                  {26, 127, 5}, {38, 126, 2}, {50, 127, 1}};
    if (!out || capacity < CHIP_DRUM_MAX_SEGMENTS || at < 0 || duration <= 0 ||
        duration > INT64_MAX - at || volume < 1 || volume > 15)
        return 0;
    const Stage *stages = NULL;
    size_t count = 0;
    switch (preset) {
    case CHIP_DRUM_BASS:
        stages = bass;
        count = sizeof(bass) / sizeof(*bass);
        break;
    case CHIP_DRUM_SNARE:
        stages = snare;
        count = sizeof(snare) / sizeof(*snare);
        break;
    case CHIP_DRUM_OPEN:
        stages = open;
        count = sizeof(open) / sizeof(*open);
        break;
    case CHIP_DRUM_CLOSE:
        stages = close;
        count = sizeof(close) / sizeof(*close);
        break;
    default:
        return 0;
    }
    size_t used = 0;
    int64_t cursor = 0;
    for (size_t i = 0; i < count && cursor < duration; i++) {
        int64_t end = ((int64_t)stages[i].end_ms * CHIP_AUDIO_SAMPLE_RATE + 500) / 1000;
        if (end > duration)
            end = duration;
        int level = (stages[i].volume * volume + 7) / 15;
        if (level < 1)
            level = 1;
        out[used++] = (ChipAudioNote){at + cursor, end - cursor, stages[i].pitch, level};
        cursor = end;
    }
    return used;
}
