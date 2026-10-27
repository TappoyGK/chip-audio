#include "chip-audio.h"
#include "echo-plan.h"
#include "kick-data.h"
#include "synth.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
enum { RENDER_BLOCK = 256 };
#define KICK_BITS (sizeof(kick_data) * 8)
int64_t chip_audio_dpcm_kick_length(void) { return (KICK_BITS * 214 * 44100 + 1789772) / 1789773; }

typedef struct {
    int64_t end;
    int pitch, volume;
    int64_t release_at, release_samples;
} Segment;
typedef struct {
    Segment *segments;
    size_t count, index;
    Sound sound;
} Channel;
typedef struct {
    ChipPulseChange *changes;
    size_t count, index;
    ChipPulseOnset *onsets;
    size_t onset_count, onset_index;
} PulseAutomation;
typedef struct {
    int16_t pcm;
    uint64_t generation;
} EchoSample;
typedef struct {
    ChipEchoTap *taps;
    EchoArrival *arrivals;
    size_t arrival_count, next_arrival, destination[2];
    unsigned char *fades;
    size_t count, next, active[2], capacity[2];
    uint64_t generation;
    EchoSample *ring[2];
} EchoState;
// Only these channels are replaced by a timbre exchange. Channel 2 is retained
// by the active audio object and must never import a spare's configuration/index.
static const int exchanged_channels[] = {0, 1, 3};
typedef struct {
    Sound sound;
    Voice voice;
    size_t index;
} SeekChannelState;
typedef struct SeekState {
    atomic_size_t references;
    struct SeekState *retired_next;
    int64_t target;
    SeekChannelState channels[3];
    size_t change_index[2], onset_index[2], kick_index;
    size_t echo_next, echo_active[2], capacity[2], next_arrival, destination[2];
    uint64_t generation;
    EchoSample *ring[2];
} SeekState;
struct ChipAudio {
    // Object ownership survives reset and score/timbre exchanges.
    ChipAudioPlayer *owner;
    EchoState echo;
    SeekState **seeks, *seed, *retired_seeks;
    size_t seek_count;
    PulseAutomation pulse[2];
    Channel channels[4];
    Voice voices[4];
    int64_t position, length;
    int stopped;
    ChipDPCMHit *kicks;
    size_t kick_count, kick_index;
    int16_t kick_levels[KICK_BITS];
    int kick_attack, kick_decay;
    double kick_gain;
};
// A prepared seek is immutable. References can change on the owning audio thread;
// final disposal is deferred until a spare is destroyed off that thread.
static void release_seek(ChipAudio *a, SeekState *state) {
    if (state && atomic_fetch_sub_explicit(&state->references, 1, memory_order_acq_rel) == 1) {
        state->retired_next = a->retired_seeks;
        a->retired_seeks = state;
    }
}
static void clear_seek_seed(ChipAudio *a) {
    release_seek(a, a->seed);
    a->seed = NULL;
}
static void clear_prepared_seeks(ChipAudio *a) {
    clear_seek_seed(a);
    for (size_t i = 0; i < a->seek_count; i++)
        release_seek(a, a->seeks[i]);
    free(a->seeks);
    a->seeks = NULL;
    a->seek_count = 0;
}
static void dispose_retired_seeks(ChipAudio *a) {
    while (a->retired_seeks) {
        SeekState *s = a->retired_seeks;
        a->retired_seeks = s->retired_next;
        free(s->ring[0]);
        free(s->ring[1]);
        free(s);
    }
}
static int valid_duty(double duty) {
    return duty == .125 || duty == .25 || duty == .5 || duty == .75;
}
static int64_t kick_tail(const ChipAudio *a) {
    return a->kick_decay ? (int64_t)a->kick_decay * CHIP_AUDIO_SAMPLE_RATE / 1000
                         : chip_audio_dpcm_kick_length();
}
static void apply_pulse_change(Sound *sound, ChipPulseChange change) {
    sound->duty = change.duty;
    sound->vib_rate = change.rate;
    sound->vib_depth = 0;
    sound->vib_delay = change.delay;
    sound->decay_time = change.decay;
    sound->attack_time = change.attack;
    sound->sustain_level = 1 - change.sustain_drop;
}
int chip_audio_set_voice_gain(ChipAudio *a, int voice, double gain) {
    if (!a || voice < 0 || voice > 4 || !isfinite(gain) || gain <= 0 || gain > 2)
        return 0;
    if (voice == 4)
        a->kick_gain = gain;
    else
        a->channels[voice].sound.gain = gain;
    return 1;
}
int chip_audio_set_dpcm_shape(ChipAudio *a, int attack, int decay) {
    if (!a || attack < 25 || attack > 150 || decay < 5 || decay > 800)
        return 0;
    a->kick_attack = attack;
    a->kick_decay = decay;
    return 1;
}
static void exchange_percussion_settings(ChipAudio *a, ChipAudio *b, int preserve_phase) {
    if (!a || !b || a->length != b->length)
        return;
    Channel channel = a->channels[3];
    a->channels[3] = b->channels[3];
    b->channels[3] = channel;
    if (!preserve_phase) {
        memset(&a->voices[3], 0, sizeof(Voice));
        synth_start(&a->voices[3], &a->channels[3].sound, 3);
    } else {
        a->voices[3].sound = &a->channels[3].sound;
        a->voices[3].left = 0;
    }
    ChipDPCMHit *hits = a->kicks;
    a->kicks = b->kicks;
    b->kicks = hits;
    a->kick_count = b->kick_count;
    a->kick_index = 0;
    memcpy(a->kick_levels, b->kick_levels, sizeof(a->kick_levels));
    a->kick_attack = b->kick_attack;
    a->kick_decay = b->kick_decay;
}

void chip_audio_exchange_percussion(ChipAudio *a, ChipAudio *b) {
    exchange_percussion_settings(a, b, 0);
}

void chip_audio_exchange_timbres(ChipAudio *a, ChipAudio *b) {
    if (!a || !b || a->length != b->length)
        return;
    SeekState **seeks = a->seeks;
    a->seeks = b->seeks;
    b->seeks = seeks;
    size_t seek_count = a->seek_count;
    a->seek_count = b->seek_count;
    b->seek_count = seek_count;
    // Transfer deferred disposal to the spare; the owning thread never frees it.
    if (a->retired_seeks) {
        SeekState *tail = a->retired_seeks;
        while (tail->retired_next)
            tail = tail->retired_next;
        tail->retired_next = b->retired_seeks;
        b->retired_seeks = a->retired_seeks;
        a->retired_seeks = NULL;
    }
    for (int p = 0; p < 2; p++) {
        PulseAutomation automation = a->pulse[p];
        a->pulse[p] = b->pulse[p];
        b->pulse[p] = automation;
        Channel old = a->channels[p];
        a->channels[p] = b->channels[p];
        b->channels[p] = old;
        a->voices[p].sound = &a->channels[p].sound;
        a->voices[p].left = 0;
    }
    // Preserve queued echoes when a timbre edit only changes source envelopes.
    int same = a->echo.count == b->echo.count && a->echo.capacity[0] == b->echo.capacity[0] &&
               a->echo.capacity[1] == b->echo.capacity[1];
    for (size_t i = 0; same && i < a->echo.count; i++) {
        ChipEchoTap x = a->echo.taps[i], y = b->echo.taps[i];
        if (x.at != y.at || x.delay != y.delay || x.source != y.source || x.gain != y.gain)
            same = 0;
    }
    if (same) {
        ChipEchoTap *old = a->echo.taps;
        a->echo.taps = b->echo.taps;
        b->echo.taps = old;
        unsigned char *fades = a->echo.fades;
        a->echo.fades = b->echo.fades;
        b->echo.fades = fades;
    } else {
        clear_seek_seed(a);
        EchoState old = a->echo;
        a->echo = b->echo;
        b->echo = old;
    }
    exchange_percussion_settings(a, b, 1);
}
void chip_audio_exchange_score(ChipAudio *a, ChipAudio *b) {
    if (!a || !b || a == b)
        return;
    int64_t old_length = a->length;
    a->length = b->length;
    chip_audio_exchange_timbres(a, b);
    Channel old = a->channels[2];
    a->channels[2] = b->channels[2];
    b->channels[2] = old;
    a->voices[2].sound = &a->channels[2].sound;
    a->voices[2].left = 0;
    a->kick_gain = b->kick_gain;
    b->length = old_length;
    if (a->position > a->length)
        a->position = a->length;
}
int chip_audio_set_dpcm_kicks(ChipAudio *a, const ChipDPCMHit *hits, size_t count) {
    if (!a || (count && !hits) || count > SIZE_MAX / sizeof(*hits))
        return 0;
    const int64_t tail = kick_tail(a);
    for (size_t i = 0; i < count; i++)
        if (hits[i].at < 0 || hits[i].at >= a->length || hits[i].duration <= 0 ||
            hits[i].duration > a->length - hits[i].at || hits[i].offset < 0 ||
            hits[i].offset >= tail || hits[i].volume < 1 || hits[i].volume > 15 ||
            (i && hits[i].at <= hits[i - 1].at))
            return 0;
    ChipDPCMHit *copy = count ? malloc(count * sizeof(*hits)) : NULL;
    if (count && !copy)
        return 0;
    if (count)
        memcpy(copy, hits, count * sizeof(*hits));
    free(a->kicks);
    a->kicks = copy;
    a->kick_count = count;
    a->kick_index = 0;
    int level = KICK_INITIAL_DAC;
    for (size_t i = 0; i < KICK_BITS; i++) {
        int previous = level;
        if ((kick_data[i / 8] >> (i % 8)) & 1) {
            if (level <= 125)
                level += 2;
        } else if (level >= 2)
            level -= 2;
        // Cancel the alternating 64/66 idle carrier after delta decoding.
        // Keep the first step untouched: that discontinuity is the wanted pop.
        a->kick_levels[i] = i ? (level + previous - 130) * 110 : (level - 64) * 220;
    }
    return 1;
}

static void mix_kicks(ChipAudio *a, int32_t *output, int frames, unsigned mask) {
    if (!a->kick_count)
        return;
    for (int i = 0; i < frames; i++) {
        int64_t now = a->position + i;
        while (a->kick_index + 1 < a->kick_count && a->kicks[a->kick_index + 1].at <= now)
            a->kick_index++;
        ChipDPCMHit hit = a->kicks[a->kick_index];
        int64_t age = now - hit.at;
        if (age < 0 || age >= hit.duration || !(mask & 8u))
            continue;
        // Bound before multiplication, even for arbitrarily long event durations.
        int64_t tail = kick_tail(a);
        if (age >= tail - hit.offset)
            continue;
        size_t bit = a->kick_decay ? (size_t)((age + hit.offset) * KICK_BITS / tail)
                                   : (size_t)((age + hit.offset) * 1789773 / (214 * 44100));
        if (bit >= KICK_BITS)
            continue;
        int value =
            a->kick_levels[bit] * hit.volume / 15 * (a->kick_attack ? a->kick_attack : 100) / 100;
        // Brief gate release and sample-tail release suppress hard cut clicks.
        int64_t left = hit.duration - age;
        if (left < 64)
            value = (int)(value * left / 64);
        if (bit > KICK_BITS - 64)
            value = (int)(value * (KICK_BITS - bit) / 64);
        value = (int)llround(value * (a->kick_gain > 0 ? a->kick_gain : 1));
        output[i] += value;
    }
}

int chip_audio_set_pulse_duty(ChipAudio *a, int channel, double duty) {
    if (!a || channel < 0 || channel >= 4 || a->channels[channel].sound.wave != CHIP_PULSE)
        return 0;
    if (!valid_duty(duty))
        return 0;
    a->channels[channel].sound.duty = duty;
    return 1;
}
int chip_audio_set_pulse_changes(ChipAudio *a, int p, const ChipPulseChange *changes, size_t count,
                                 const ChipPulseOnset *onsets, size_t onset_count) {
    if (!a || p < 0 || p > 1 || !count || !changes || (onset_count && !onsets) ||
        count > SIZE_MAX / sizeof(*changes) || onset_count > SIZE_MAX / sizeof(*onsets))
        return 0;
    for (size_t i = 0; i < count; i++) {
        ChipPulseChange c = changes[i];
        if (c.at < 0 || c.at >= a->length || (i ? c.at <= changes[i - 1].at : c.at != 0) ||
            !valid_duty(c.duty) || !(c.rate >= 1 && c.rate <= 12) ||
            !(c.depth >= 0 && c.depth <= 100) || !(c.delay >= 0 && c.delay <= 1) ||
            !(c.decay >= 0 && c.decay <= 2) || !(c.attack >= 0 && c.attack <= .5) ||
            !(c.sustain_drop >= 0 && c.sustain_drop <= 1))
            return 0;
    }
    for (size_t i = 0; i < onset_count; i++)
        if (onsets[i].end <= onsets[i].at || (i && onsets[i].at < onsets[i - 1].end))
            return 0;
    ChipPulseChange *c = malloc(count * sizeof(*c));
    ChipPulseOnset *n = onset_count ? malloc(onset_count * sizeof(*n)) : NULL;
    if (!c || (onset_count && !n)) {
        free(c);
        free(n);
        return 0;
    }
    memcpy(c, changes, count * sizeof(*c));
    if (onset_count)
        memcpy(n, onsets, onset_count * sizeof(*n));
    free(a->pulse[p].changes);
    free(a->pulse[p].onsets);
    a->pulse[p] = (PulseAutomation){c, count, 0, n, onset_count, 0};
    return 1;
}
static void clear_echo(EchoState *e) {
    free(e->taps);
    free(e->arrivals);
    free(e->fades);
    free(e->ring[0]);
    free(e->ring[1]);
    memset(e, 0, sizeof(*e));
}
static void reset_echo(EchoState *e) {
    e->next = 0;
    e->next_arrival = 0;
    e->destination[0] = e->destination[1] = SIZE_MAX;
    e->active[0] = e->active[1] = SIZE_MAX;
    // Slots are invalidated by generation, not a delay-sized memset on the audio thread.
    e->generation += 2;
}
static EchoSpan *occupied_spans(const Channel *channel, const PulseAutomation *pulse,
                                size_t *count) {
    size_t onsets = pulse ? pulse->onset_count : 0;
    if (channel->count > SIZE_MAX / sizeof(EchoSpan) - onsets)
        return NULL;
    EchoSpan *spans = malloc((channel->count + onsets) * sizeof(*spans));
    if (!spans)
        return NULL;
    *count = 0;
    int64_t previous = 0;
    for (size_t i = 0; i < channel->count; i++) {
        Segment n = channel->segments[i];
        if (n.volume) {
            int64_t end = n.end;
            // A silent segment may still contain the oscillator's 128-sample level glide.
            if (!pulse) {
                int64_t length = channel->segments[channel->count - 1].end;
                end = length - end < SYNTH_RAMP ? length : end + SYNTH_RAMP;
            }
            spans[(*count)++] = (EchoSpan){previous, end};
        }
        previous = n.end;
    }
    for (size_t i = 0; i < onsets; i++)
        spans[(*count)++] = (EchoSpan){pulse->onsets[i].at, pulse->onsets[i].end};
    qsort(spans, *count, sizeof(*spans), echo_span_order);
    *count = echo_merge_spans(spans, *count);
    return spans;
}
int chip_audio_set_echo_taps(ChipAudio *a, const ChipEchoTap *taps, size_t count) {
    if (!a || (count && !taps) || count > SIZE_MAX / sizeof(*taps) ||
        count > SIZE_MAX / sizeof(EchoArrival))
        return 0;
    int64_t ends[2] = {0, 0};
    for (size_t i = 0; i < count; i++) {
        ChipEchoTap t = taps[i];
        if (t.source < 0 || t.source > 1 || a->channels[0].sound.wave != CHIP_PULSE ||
            a->channels[1].sound.wave != CHIP_PULSE || t.at < 0 || t.end <= t.at ||
            t.end > a->length || t.delay < 1 || t.delay > 30 * CHIP_AUDIO_SAMPLE_RATE ||
            !(t.gain > 0 && t.gain <= 1) || (i && t.at < taps[i - 1].at) || t.at < ends[t.source])
            return 0;
        ends[t.source] = t.end;
    }
    EchoState e = {0};
    EchoSpan *occupied[2] = {0}, *source[2] = {0};
    size_t occupied_count[2] = {0}, source_count[2] = {0};
    EchoArrival *arrivals = NULL;
    if (count) {
        e.count = count;
        e.taps = malloc(count * sizeof(*taps));
        e.fades = calloc(count, 1);
        arrivals = malloc(count * sizeof(*arrivals));
        if (!e.taps || !e.fades || !arrivals)
            goto failure;
        memcpy(e.taps, taps, count * sizeof(*taps));
        for (int p = 0; p < 2; p++) {
            occupied[p] = occupied_spans(&a->channels[p], &a->pulse[p], &occupied_count[p]);
            source[p] = occupied_spans(&a->channels[p], NULL, &source_count[p]);
            if (!occupied[p] || !source[p])
                goto failure;
        }
        size_t arrival_count = 0;
        for (size_t i = 0; i < count; i++) {
            ChipEchoTap *t = &e.taps[i];
            if (t->at >= a->length - t->delay) {
                t->end = t->at;
                continue;
            }
            arrivals[arrival_count++] = (EchoArrival){t->at + t->delay, i, t->source};
            if (t->end > a->length - t->delay)
                t->end = a->length - t->delay;
            int dest = 1 - t->source;
            t->end = echo_cut_at(occupied[dest], occupied_count[dest], t->at + t->delay,
                                 t->end + t->delay) -
                     t->delay;
        }
        qsort(arrivals, arrival_count, sizeof(*arrivals), echo_arrival_order);
        int64_t next[2] = {a->length, a->length};
        for (size_t i = arrival_count; i > 0; i--) {
            EchoArrival arrival = arrivals[i - 1];
            ChipEchoTap *t = &e.taps[arrival.id];
            int64_t cut = next[t->source] - t->delay;
            if (cut < t->end)
                t->end = cut;
            next[t->source] = arrival.at;
        }
        for (size_t i = 0; i < count; i++) {
            ChipEchoTap *t = &e.taps[i];
            size_t first = echo_first_overlap(source[t->source], source_count[t->source], t->at);
            if (first == source_count[t->source] || source[t->source][first].at >= t->end)
                t->end = t->at;
            e.fades[i] = t->end < taps[i].end;
            if (t->end > t->at) {
                int dest = 1 - t->source;
                size_t needed = (size_t)t->delay + 1;
                if (needed > e.capacity[dest])
                    e.capacity[dest] = needed;
            }
        }
        for (int p = 0; p < 2; p++)
            if (e.capacity[p]) {
                e.ring[p] = calloc(e.capacity[p], sizeof(EchoSample));
                if (!e.ring[p])
                    goto failure;
            }
        if (!e.capacity[0] && !e.capacity[1])
            clear_echo(&e);
        else {
            e.arrivals = arrivals;
            e.arrival_count = arrival_count;
            arrivals = NULL;
        }
    }
    for (int p = 0; p < 2; p++) {
        free(occupied[p]);
        free(source[p]);
    }
    free(arrivals);
    clear_echo(&a->echo);
    a->echo = e;
    reset_echo(&a->echo);
    return 1;
failure:
    for (int p = 0; p < 2; p++) {
        free(occupied[p]);
        free(source[p]);
    }
    free(arrivals);
    clear_echo(&e);
    return 0;
}
static void render_echo(ChipAudio *a, int32_t *output, int count, unsigned mask, double *planes[5]) {
    int16_t stems[4][RENDER_BLOCK];
    int16_t *channels[4] = {stems[0], stems[1], stems[2], stems[3]};
    synth_render_wide(a->voices, NULL, count, 15, channels, NULL);
    EchoState *e = &a->echo;
    for (int i = 0; i < count; i++) {
        int64_t now = a->position + i;
        while (e->next < e->count && e->taps[e->next].at <= now) {
            size_t id = e->next++;
            e->active[e->taps[id].source] = id;
        }
        while (e->next_arrival < e->arrival_count && e->arrivals[e->next_arrival].at <= now) {
            EchoArrival arrival = e->arrivals[e->next_arrival++];
            e->destination[1 - arrival.source] = arrival.id;
        }
        int values[4] = {stems[0][i], stems[1][i], stems[2][i], stems[3][i]};
        for (int p = 0; p < 2; p++) {
            if (!e->capacity[p])
                continue;
            size_t slot = (size_t)(now % (int64_t)e->capacity[p]);
            EchoSample sample = e->ring[p][slot];
            if (sample.generation != e->generation && sample.generation != e->generation + 1 &&
                a->seed && now - a->seed->target < (int64_t)e->capacity[p] && a->seed->ring[p]) {
                sample = a->seed->ring[p][slot];
                sample.generation =
                    sample.generation == a->seed->generation + 1 ? e->generation + 1 : 0;
            }
            e->ring[p][slot] = (EchoSample){0, e->generation};
            size_t id = e->destination[p];
            // Recheck the prepared cutoff against the current settings. A live edit may
            // shorten a destination reservation while old source PCM is still queued.
            if (sample.generation == e->generation + 1 && id != SIZE_MAX &&
                now < e->taps[id].end + e->taps[id].delay)
                values[p] = sample.pcm;
        }
        // Capture the original stems only, never the returned echo.
        for (int p = 0; p < 2; p++) {
            size_t id = e->active[p];
            if (id == SIZE_MAX)
                continue;
            ChipEchoTap t = e->taps[id];
            if (now >= t.end)
                continue;
            size_t future = (size_t)((now + t.delay) % (int64_t)e->capacity[1 - p]);
            double gain = t.gain;
            if (e->fades[id] && t.end - now < 128)
                gain *= (double)(t.end - now) / 128;
            e->ring[1 - p][future] =
                (EchoSample){(int16_t)llround(stems[p][i] * gain), e->generation + 1};
        }
        int mixed = 0;
        for (int p = 0; p < 4; p++)
            if (mask & (1u << p))
                mixed += values[p];
        output[i] = mixed;
        if (planes) for (int p = 0; p < 4; p++)
            if (planes[p]) planes[p][i] = values[p];
    }
}
void chip_audio_destroy(ChipAudio *a) {
    if (!a)
        return;
    clear_prepared_seeks(a);
    dispose_retired_seeks(a);
    clear_echo(&a->echo);
    for (int p = 0; p < 2; p++) {
        free(a->pulse[p].changes);
        free(a->pulse[p].onsets);
    }
    for (int p = 0; p < 4; p++)
        free(a->channels[p].segments);
    free(a->kicks);
    free(a);
}
void chip_audio_reset(ChipAudio *a) {
    if (!a)
        return;
    clear_seek_seed(a);
    reset_echo(&a->echo);
    a->position = 0;
    a->stopped = 0;
    a->kick_index = 0;
    for (int p = 0; p < 2; p++) {
        a->pulse[p].index = 0;
        a->pulse[p].onset_index = 0;
        a->channels[p].sound.vib_depth = 0;
        a->channels[p].sound.note_age_samples = 0;
        if (a->pulse[p].count) {
            ChipPulseChange c = a->pulse[p].changes[0];
            apply_pulse_change(&a->channels[p].sound, c);
        }
    }
    memset(a->voices, 0, sizeof(a->voices));
    for (int p = 0; p < 4; p++) {
        a->channels[p].index = 0;
        if (p >= 2 || !a->pulse[p].count)
            a->channels[p].sound.sustain_level = 1;
        synth_start(&a->voices[p], &a->channels[p].sound, p);
    }
}
void chip_audio_stop(ChipAudio *a) {
    if (a)
        a->stopped = 1;
}
// The retained channel does not feed echo. A seek resets its oscillator at the
// destination, independently of whether pulse history needed warming. Resolve
// its cursor against its own timeline in O(log N), without allocation/replay.
static void seek_retained_channel(ChipAudio *a, int64_t target) {
    Channel *channel = &a->channels[2];
    size_t lo = 0, hi = channel->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (channel->segments[mid].end <= target)
            lo = mid + 1;
        else
            hi = mid;
    }
    channel->index = lo < channel->count ? lo : channel->count - 1;
    memset(&a->voices[2], 0, sizeof(Voice));
    synth_start(&a->voices[2], &channel->sound, 2);
}
static void seek_unprepared(ChipAudio *a, int64_t sample) {
    if (!a)
        return;
    chip_audio_reset(a);
    int64_t target = sample < 0 ? 0 : sample > a->length ? a->length : sample;
    a->position = target;
    if (a->echo.count) {
        size_t history =
            a->echo.capacity[0] > a->echo.capacity[1] ? a->echo.capacity[0] : a->echo.capacity[1];
        a->position = target > (int64_t)history ? target - (int64_t)history : 0;
        int16_t discard[RENDER_BLOCK];
        while (a->position < target) {
            size_t count =
                target - a->position > RENDER_BLOCK ? RENDER_BLOCK : (size_t)(target - a->position);
            chip_audio_render(a, discard, count, 15);
        }
    }
    seek_retained_channel(a, target);
}
static void restore_seek(ChipAudio *a, SeekState *state) {
    clear_seek_seed(a);
    reset_echo(&a->echo);
    a->position = state->target;
    a->stopped = 0;
    a->kick_index = state->kick_index;
    for (size_t i = 0; i < sizeof(exchanged_channels) / sizeof(exchanged_channels[0]); i++) {
        int p = exchanged_channels[i];
        a->channels[p].sound = state->channels[i].sound;
        a->channels[p].index = state->channels[i].index;
        a->voices[p] = state->channels[i].voice;
        a->voices[p].sound = &a->channels[p].sound;
    }
    seek_retained_channel(a, state->target);
    for (int p = 0; p < 2; p++) {
        a->pulse[p].index = state->change_index[p];
        a->pulse[p].onset_index = state->onset_index[p];
        a->echo.active[p] = state->echo_active[p];
        a->echo.destination[p] = state->destination[p];
    }
    a->echo.next = state->echo_next;
    a->echo.next_arrival = state->next_arrival;
    a->seed = state;
    atomic_fetch_add_explicit(&state->references, 1, memory_order_relaxed);
}
void chip_audio_seek(ChipAudio *a, int64_t sample) {
    if (!a)
        return;
    int64_t target = sample < 0 ? 0 : sample > a->length ? a->length : sample;
    for (size_t i = 0; i < a->seek_count; i++)
        if (a->seeks[i]->target == target) {
            restore_seek(a, a->seeks[i]);
            return;
        }
    seek_unprepared(a, target);
}
int chip_audio_prepare_seeks(ChipAudio *a, const int64_t *samples, size_t count) {
    if (!a || (count && !samples) || count > SIZE_MAX / sizeof(SeekState *))
        return 0;
    for (size_t i = 0; i < count; i++)
        if (samples[i] < 0 || samples[i] > a->length)
            return 0;
    SeekState **prepared = count ? calloc(count, sizeof(*prepared)) : NULL;
    if (count && !prepared)
        return 0;
    size_t built = 0;
    for (size_t i = 0; i < count; i++) {
        SeekState *state = calloc(1, sizeof(*state));
        if (!state)
            goto failure;
        atomic_init(&state->references, 1);
        prepared[built++] = state;
        state->target = samples[i];
        seek_unprepared(a, state->target);
        state->kick_index = a->kick_index;
        state->echo_next = a->echo.next;
        state->next_arrival = a->echo.next_arrival;
        state->generation = a->echo.generation;
        for (size_t j = 0; j < sizeof(exchanged_channels) / sizeof(exchanged_channels[0]); j++) {
            int p = exchanged_channels[j];
            state->channels[j] =
                (SeekChannelState){a->channels[p].sound, a->voices[p], a->channels[p].index};
        }
        for (int p = 0; p < 2; p++) {
            state->change_index[p] = a->pulse[p].index;
            state->onset_index[p] = a->pulse[p].onset_index;
            state->echo_active[p] = a->echo.active[p];
            state->destination[p] = a->echo.destination[p];
            state->capacity[p] = a->echo.capacity[p];
            if (state->capacity[p]) {
                state->ring[p] = malloc(state->capacity[p] * sizeof(EchoSample));
                if (!state->ring[p])
                    goto failure;
                memcpy(state->ring[p], a->echo.ring[p], state->capacity[p] * sizeof(EchoSample));
            }
        }
    }
    clear_prepared_seeks(a);
    a->seeks = prepared;
    a->seek_count = count;
    chip_audio_reset(a);
    dispose_retired_seeks(a);
    return 1;
failure:
    for (size_t i = 0; i < built; i++)
        release_seek(a, prepared[i]);
    free(prepared);
    chip_audio_reset(a);
    dispose_retired_seeks(a);
    return 0;
}
int64_t chip_audio_position(const ChipAudio *a) { return a ? a->position : 0; }
int chip_audio_finished(const ChipAudio *a) { return !a || a->position >= a->length; }

ChipAudio *chip_audio_create(const ChipAudioTrack tracks[4], int64_t length,
                             ChipAudioError *error) {
    if (error)
        *error = CHIP_AUDIO_INVALID;
    if (!tracks || length <= 0)
        return NULL;
    // Validate everything before allocation. Bounds also prevent size_t overflow.
    for (int p = 0; p < 4; p++) {
        const ChipAudioTrack *t = &tracks[p];
        if (t->wave < CHIP_PULSE || t->wave > CHIP_NOISE || (t->count && !t->notes) ||
            t->count > (SIZE_MAX / sizeof(Segment) - 1) / 2)
            return NULL;
        int64_t end = 0;
        for (size_t i = 0; i < t->count; i++) {
            const ChipAudioNote *n = &t->notes[i];
            if (n->at < end || n->at >= length || n->duration <= 0 ||
                n->duration > length - n->at || n->pitch < 0 || n->pitch > 127 || n->volume < 1 ||
                n->volume > 15 || n->release_samples < 0)
                return NULL;
            end = n->at + n->duration;
        }
    }
    if (error)
        *error = CHIP_AUDIO_MEMORY;
    ChipAudio *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->length = length;
    for (int p = 0; p < 4; p++) {
        const ChipAudioTrack *t = &tracks[p];
        Channel *c = &a->channels[p];
        c->sound.wave = t->wave;
        c->segments = calloc(t->count * 2 + 1, sizeof(Segment));
        if (!c->segments) {
            chip_audio_destroy(a);
            return NULL;
        }
        int64_t end = 0;
        for (size_t i = 0; i < t->count; i++) {
            const ChipAudioNote *n = &t->notes[i];
            if (n->at > end)
                c->segments[c->count++] = (Segment){n->at, 0, 0};
            end = n->at + n->duration;
            c->segments[c->count++] =
                (Segment){end, n->pitch, n->volume, n->release_at, n->release_samples};
        }
        if (end < length)
            c->segments[c->count++] = (Segment){length, 0, 0};
    }
    chip_audio_reset(a);
    if (error)
        *error = CHIP_AUDIO_OK;
    return a;
}
static int apply_pulse_automation(ChipAudio *a, int p, int chunk) {
    PulseAutomation *automation = &a->pulse[p];
    if (!automation->count)
        return chunk;
    while (automation->index + 1 < automation->count &&
           automation->changes[automation->index + 1].at <= a->position)
        automation->index++;
    ChipPulseChange change = automation->changes[automation->index];
    Sound *sound = &a->channels[p].sound;
    apply_pulse_change(sound, change);
    if (automation->index + 1 < automation->count) {
        int64_t until = automation->changes[automation->index + 1].at - a->position;
        if (until < chunk)
            chunk = (int)until;
    }
    if (automation->onset_count) {
        while (automation->onset_index + 1 < automation->onset_count &&
               automation->onsets[automation->onset_index + 1].at <= a->position)
            automation->onset_index++;
        ChipPulseOnset onset = automation->onsets[automation->onset_index];
        sound->note_age_samples =
            (double)a->position - onset.at; /* Keep modulation through release; the next onset
                        resets age. Silence has no audible pitch. */
        if (a->position >= onset.at)
            sound->vib_depth = change.depth;
        if (automation->onset_index + 1 < automation->onset_count) {
            int64_t until = automation->onsets[automation->onset_index + 1].at - a->position;
            if (until < chunk)
                chunk = (int)until;
        }
    }

    return chunk;
}
static int prepare_channel_interval(ChipAudio *a, int p, int chunk) {
    Channel *c = &a->channels[p];
    Voice *v = &a->voices[p];
    while (c->index + 1 < c->count && c->segments[c->index].end <= a->position)
        c->index++;
    const Segment *s = &c->segments[c->index];
    if (p >= 2 || !a->pulse[p].onset_count) {
        int64_t start = c->index ? c->segments[c->index - 1].end : 0;
        c->sound.note_age_samples = (double)a->position - start;
        if (p < 2 && a->pulse[p].count)
            c->sound.vib_depth = a->pulse[p].changes[a->pulse[p].index].depth;
    }
    c->sound.release_time = (double)s->release_samples / 44100;
    c->sound.release_age_samples = (double)a->position - s->release_at;
    int64_t remaining = s->end - a->position;
    if (v->left == 0) {
        int take = remaining > INT_MAX ? INT_MAX : (int)remaining;
        synth_bind_interval(v, &c->sound, (Note){s->pitch, take, s->volume},
                            c->index + 1 == c->count && remaining <= INT_MAX);
    }
    if (v->left < chunk)
        chunk = v->left;

    return chunk;
}
// One state advance serves both the legacy PCM boundary and the five-plane API.
static void render_outputs(ChipAudio *a, int16_t *output, double *planes[5],
                           size_t frames, unsigned mask) {
    size_t offset = 0;
    while (frames) {
        int chunk = frames > RENDER_BLOCK ? RENDER_BLOCK : (int)frames;
        double *current[5] = {0};
        if (planes) for (int p = 0; p < 5; p++)
            if (planes[p]) current[p] = planes[p] + offset;
        if (!a || a->stopped || a->position >= a->length) {
            if (output) memset(output + offset, 0, (size_t)chunk * sizeof(*output));
            for (int p = 0; p < 5; p++)
                if (current[p]) memset(current[p], 0, (size_t)chunk * sizeof(double));
        } else {
            int32_t mixed[RENDER_BLOCK], kicks[RENDER_BLOCK] = {0};
            for (int p = 0; p < 2; p++) chunk = apply_pulse_automation(a, p, chunk);
            for (int p = 0; p < 4; p++) chunk = prepare_channel_interval(a, p, chunk);
            if (a->echo.count)
                render_echo(a, mixed, chunk, mask & 15u, planes ? current : NULL);
            else if (planes) {
                int16_t stems[4][RENDER_BLOCK];
                int16_t *channels[4] = {stems[0], stems[1], stems[2], stems[3]};
                synth_render_wide(a->voices, NULL, chunk, 15, channels, mixed);
                for (int p = 0; p < 4; p++) if (current[p])
                    for (int i = 0; i < chunk; i++) current[p][i] = stems[p][i];
            } else
                synth_render_wide(a->voices, NULL, chunk, mask & 15u, NULL, mixed);
            mix_kicks(a, kicks, chunk, mask);
            for (int i = 0; i < chunk; i++) {
                if (current[4]) current[4][i] = kicks[i];
                if (output) output[offset + (size_t)i] = synth_pcm(mixed[i] + kicks[i]);
            }
            a->position += chunk;
        }
        offset += (size_t)chunk;
        frames -= (size_t)chunk;
    }
}
void chip_audio_render(ChipAudio *a, int16_t *output, size_t frames, unsigned mask) {
    if (output) render_outputs(a, output, NULL, frames, mask);
}
void chip_audio_render_stems(ChipAudio *a, double *planes[5], size_t frames) {
    // Non-NULL array selects the stems path even if every plane is discarded.
    double *discard[5] = {0};
    render_outputs(a, NULL, planes ? planes : discard, frames, 15);
}

// Each owned audio is in exactly one slot; retired slots still consume capacity.
typedef enum { PLAYER_EMPTY, PLAYER_CURRENT, PLAYER_FADING, PLAYER_RETIRED } PlayerSlotState;
typedef struct {
    ChipAudio *audio;
    PlayerSlotState state;
    int total, remaining;
} PlayerSlot;
struct ChipAudioPlayer {
    size_t capacity;
    int current;
    int16_t scratch[RENDER_BLOCK];
    double mixed[RENDER_BLOCK];
    PlayerSlot slots[];
};
ChipAudioPlayer *chip_audio_player_create(size_t slots, ChipAudioError *error) {
    if (error) *error = CHIP_AUDIO_INVALID;
    if (slots < CHIP_AUDIO_PLAYER_MIN_SLOTS || slots > CHIP_AUDIO_PLAYER_MAX_SLOTS)
        return NULL;
    ChipAudioPlayer *p = calloc(1, sizeof(*p) + slots * sizeof(PlayerSlot));
    if (!p) { if (error) *error = CHIP_AUDIO_MEMORY; return NULL; }
    p->capacity = slots;
    p->current = -1;
    if (error) *error = CHIP_AUDIO_OK;
    return p;
}
void chip_audio_player_destroy(ChipAudioPlayer *p) {
    if (!p) return;
    for (size_t i = 0; i < p->capacity; i++) chip_audio_destroy(p->slots[i].audio);
    free(p);
}
ChipAudioError chip_audio_player_stop(ChipAudioPlayer *p, int fade_samples) {
    if (!p || fade_samples < 0 || fade_samples > CHIP_AUDIO_PLAYER_MAX_FADE)
        return CHIP_AUDIO_INVALID;
    if (p->current >= 0) {
        PlayerSlot *slot = &p->slots[p->current];
        slot->total = slot->remaining = fade_samples;
        slot->state = fade_samples ? PLAYER_FADING : PLAYER_RETIRED;
        p->current = -1;
    }
    return CHIP_AUDIO_OK;
}
ChipAudioError chip_audio_player_switch(ChipAudioPlayer *p, ChipAudio *next, int fade_samples) {
    if (!p || !next || fade_samples < 0 || fade_samples > CHIP_AUDIO_PLAYER_MAX_FADE ||
        next->owner || next->position != 0 || next->stopped || chip_audio_finished(next))
        return CHIP_AUDIO_INVALID;
    int empty = -1;
    for (size_t i = 0; i < p->capacity; i++) {
        if (p->slots[i].audio == next) return CHIP_AUDIO_INVALID;
        if (empty < 0 && p->slots[i].state == PLAYER_EMPTY) empty = (int)i;
    }
    if (empty < 0) return CHIP_AUDIO_BUSY;
    chip_audio_player_stop(p, fade_samples);
    next->owner = p;
    p->slots[empty] = (PlayerSlot){next, PLAYER_CURRENT, 0, 0};
    p->current = empty;
    return CHIP_AUDIO_OK;
}
int chip_audio_player_finished(const ChipAudioPlayer *p) {
    if (!p) return 1;
    for (size_t i = 0; i < p->capacity; i++)
        if (p->slots[i].state == PLAYER_CURRENT || p->slots[i].state == PLAYER_FADING)
            return 0;
    return 1;
}
ChipAudio *chip_audio_player_collect(ChipAudioPlayer *p) {
    if (!p) return NULL;
    for (size_t i = 0; i < p->capacity; i++) if (p->slots[i].state == PLAYER_RETIRED) {
        ChipAudio *audio = p->slots[i].audio;
        audio->owner = NULL;
        p->slots[i] = (PlayerSlot){0};
        return audio;
    }
    return NULL;
}
void chip_audio_player_clear(ChipAudioPlayer *p) {
    if (!p) return;
    for (size_t i = 0; i < p->capacity; i++)
        if (p->slots[i].state != PLAYER_EMPTY) p->slots[i].state = PLAYER_RETIRED;
    p->current = -1;
}
void chip_audio_player_render(ChipAudioPlayer *p, int16_t *output, size_t frames, unsigned mask) {
    if (!output) return;
    while (frames) {
        size_t chunk = frames < RENDER_BLOCK ? frames : RENDER_BLOCK;
        if (!p) memset(output, 0, chunk * sizeof(*output));
        else {
            memset(p->mixed, 0, chunk * sizeof(*p->mixed));
            for (size_t i = 0; i < p->capacity; i++) {
                PlayerSlot *slot = &p->slots[i];
                if (slot->state != PLAYER_CURRENT && slot->state != PLAYER_FADING) continue;
                int fading = slot->state == PLAYER_FADING;
                size_t take = fading && (size_t)slot->remaining < chunk ? (size_t)slot->remaining : chunk;
                chip_audio_render(slot->audio, p->scratch, take, mask);
                for (size_t k = 0; k < take; k++) {
                    double gain = fading ? (double)(slot->remaining - (int)k) / slot->total : 1.0;
                    p->mixed[k] += p->scratch[k] * gain;
                }
                if (fading) slot->remaining -= (int)take;
                if ((fading && !slot->remaining) || chip_audio_finished(slot->audio)) {
                    slot->state = PLAYER_RETIRED;
                    if (p->current == (int)i) p->current = -1;
                }
            }
            for (size_t k = 0; k < chunk; k++) {
                double value = p->mixed[k];
                output[k] = value > 32767 ? 32767 : value < -32768 ? -32768 : (int16_t)value;
            }
        }
        output += chunk;
        frames -= chunk;
    }
}

int chip_audio_gain_init(ChipAudioGain *g, double value) {
    if (!g || !isfinite(value) || value < 0 || value > 2) return 0;
    *g = (ChipAudioGain){value, value, value, 0, 0};
    return 1;
}
int chip_audio_gain_set(ChipAudioGain *g, double target, int samples) {
    if (!g || !isfinite(target) || target < 0 || target > 2 || samples < 0 ||
        samples > CHIP_AUDIO_SAMPLE_RATE) return 0;
    g->start = g->current;
    g->target = target;
    g->total = samples;
    g->elapsed = 0;
    if (!samples) g->current = target;
    return 1;
}
void chip_audio_gain_render(ChipAudioGain *g, double *output, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        if (output) output[i] = g->current;
        if (g->elapsed < g->total) {
            g->elapsed++;
            g->current = g->elapsed == g->total ? g->target :
                g->start + (g->target - g->start) * ((double)g->elapsed / g->total);
        }
    }
}
void chip_audio_mix_add(double *mix, const double *input, const double *gain, size_t frames) {
    for (size_t i = 0; i < frames; i++) mix[i] += input[i] * (gain ? gain[i] : 1.0);
}
void chip_audio_mix_pcm(const double *mix, int16_t *output, size_t frames) {
    for (size_t i = 0; i < frames; i++) output[i] = synth_pcm(mix[i]);
}
