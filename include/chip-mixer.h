#ifndef CHIP_MIXER_H
#define CHIP_MIXER_H
/* Bounded single-owner mixer, C11/C++17. All setup/destruction is non-RT. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    CA_OK, CA_INVALID, CA_MEMORY, CA_LIMIT, CA_BUSY, CA_PAST,
    CA_STALE_HANDLE, CA_NOT_PREPARED, CA_UNSUPPORTED, CA_OVERFLOW,
    CA_STALE_REVISION, CA_CANCELLED
} CAResult;
typedef struct { uint32_t slot; uint64_t generation; } CAHandle;
typedef struct {
    size_t input_bytes, allocated_bytes, events, visits, checkpoints;
    uint32_t repeat_depth, se_instances, commands, retired_objects;
    uint32_t gain_bindings, command_results;
    int64_t score_samples, performance_samples;
} CALimits;
#define CA_DEFAULT_RAMP 128
typedef struct CAMixer CAMixer;
typedef struct CASeDefinition CASeDefinition;
typedef struct CASeInstance CASeInstance;
typedef struct CASeek CASeek;
typedef enum { CA_BGM, CA_SE, CA_UI } CAGroup;
typedef struct { CAGroup group; uint32_t bus; } CARoute;
typedef enum { CA_MASTER_GAIN, CA_GROUP_GAIN, CA_TRACK_GAIN } CAGainScope;
typedef struct {
    CAGainScope scope; CAGroup group; CAHandle instance; uint32_t source;
} CAGainTarget;
typedef enum { CA_PENDING, CA_PLAYING, CA_PAUSING, CA_PAUSED, CA_FADING, CA_RETIRED } CAState;
typedef struct { double *before_gain; double *after_gain; CAHandle instance; uint32_t source; } CAStem;
/* Wide PCM units: +/-32768 correspond to int16 full scale; no early clipping.
   after_gain includes track, transition, group and master gains.
   Pointers are borrowed for this call; arrays have frames elements, no aliases.
   NULL outputs discard data; zero frames executes no commands and advances nothing. */
typedef struct { int16_t *pcm; double *mix; CAStem *stems; size_t stem_count; } CAOutput;
CAMixer *ca_mixer_create(const CALimits *, CAResult *);
void ca_mixer_destroy(CAMixer *); /* non-RT, drains all owned states */
int64_t ca_mixer_position(const CAMixer *);
CAResult ca_mixer_render(CAMixer *, size_t frames, const CAOutput *);
CAResult ca_mixer_advance(CAMixer *, size_t frames);
CAResult ca_gain(CAMixer *, CAGainTarget, double target, int64_t ramp, int64_t at);
CAResult ca_group_current(const CAMixer *, CAGroup, CAHandle *);
CAResult ca_state(const CAMixer *, CAHandle, CAState *);
/* Generic pre-registered gain gates; policy selection remains in the host.
   Target is one exact instance generation/source; never follows a replacement.
   Effective attenuation = min(1, all active bindings); multiplied by base gain.
   Include-tail gates remain active through FADING; pending/paused/retired do not.
   Binding is reserved at acceptance and installed at 'at', before that sample. */
typedef struct {
    CAHandle trigger, target;
    uint32_t target_source;
    double attenuation; /* [0,1] */
    int include_tail;
} CAGainBinding;
/* One recovery/attack ramp per target, configured before any bindings. */
CAResult ca_gate_ramp(CAMixer *, CAHandle target, uint32_t source, int64_t ramp);
CAResult ca_bind_gain(CAMixer *, const CAGainBinding *, int64_t at, CAHandle *binding);
CAResult ca_unbind_gain(CAMixer *, CAHandle binding, int64_t at);
/* Edit acceptance and application are distinct. Results consume reserved slots
   until polled; full result storage causes BUSY at acceptance, never silent loss. */
typedef uint64_t CACommandID;
typedef struct {
    CACommandID command; CAHandle target; CAResult result;
    int64_t at; uint64_t revision;
} CACommandResult;
int ca_poll_result(CAMixer *, CACommandResult *); /* owning thread, RT-safe */
/* Opaque retirement token; collect is RT-safe, dispose is non-RT. */
typedef struct CARetired CARetired;
CARetired *ca_collect(CAMixer *);
void ca_dispose(CARetired *);
/* SE uses a prepared source, not a sequence. Definition construction copies
   a configured, unowned ChipAudio through the snapshot interface. */
struct ChipAudio;
CASeDefinition *ca_se_prepare(const struct ChipAudio *, const CALimits *, CAResult *);
void ca_se_definition_destroy(CASeDefinition *);
CASeInstance *ca_se_instantiate(const CASeDefinition *, CAResult *); /* non-RT */
void ca_se_instance_destroy(CASeInstance *); /* caller-owned instances only */
CAResult ca_se_trigger(CAMixer *, CASeInstance *, CARoute, int64_t at, CAHandle *);
CAResult ca_se_replace(CAMixer *, CAHandle, CASeInstance *, CARoute,
                       int64_t fade, int64_t at, CAHandle *);
CAResult ca_se_stop(CAMixer *, CAHandle, int64_t fade, int64_t at);

#ifdef __cplusplus
}
#endif
#endif
