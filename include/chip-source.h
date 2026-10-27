#ifndef CHIP_SOURCE_H
#define CHIP_SOURCE_H
#include "chip-mixer.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Library-to-library interface owned by chip-audio. No sequencer types/symbols.
   Every call except dispose runs on the mixer's single owning thread.
   A source is prepared before attachment. Registration transfers on success only.
   No render callback may allocate/free, lock, do I/O, reenter the mixer or fail. */
typedef struct {
    /* Settle all source-local zero-time transitions at current position, including
       loops/visit changes. Bounded by the prepared event budget; cannot spin.
       Returns 0 when naturally ended, otherwise positive frames until the next
       source-local boundary. Pause is controlled by mixer, not this callback. */
    int64_t (*settle)(void *context);
    /* n <= last settle result and mixer block capacity. Writes n values to each
       of five planes, zeros inactive planes. Advances source time EXACTLY n.
       Outputs wide PCM after intrinsic source gain/effects, before mixer gains.
       No command/loop boundary is crossed; mixer calls settle again afterward. */
    void (*render)(void *context, double *planes[5], size_t n);
    /* Called only from ca_dispose or non-RT mixer destruction. */
    void (*dispose)(void *context);
} CASourceOps;
typedef struct CASource CASource;
/* Copies ops, adopts context only on success; allocates outside callback.
   Context must have a single wrapper. Immutable tables may be shared separately. */
CASource *ca_source_prepare(void *context, const CASourceOps *, const CALimits *, CAResult *);
void ca_source_destroy(CASource *); /* unowned wrapper only, non-RT */
CAResult ca_source_attach(CAMixer *, CASource *, CARoute, int64_t at, CAHandle *);
CAResult ca_source_replace(CAMixer *, CAHandle, CASource *, CARoute,
                            int64_t fade, int64_t at, CAHandle *);
CAResult ca_source_stop(CAMixer *, CAHandle, int64_t fade, int64_t at);
CAResult ca_source_pause(CAMixer *, CAHandle, int64_t fade, int64_t at);
CAResult ca_source_resume(CAMixer *, CAHandle, int64_t fade, int64_t at);
/* Prepared source-specific operation: used for seek/edit/position query.
   Identity key is an address of immutable static storage owned by the provider.
   This prevents dispatching a sequencer operation into an SE context. */
typedef struct CAOperation CAOperation;
typedef struct {
    const void *provider;
    /* 0: no exclusion. Nonzero: reserve this per-source lane until result poll.
       Sequencer edits use lane 1; another edit on that lane returns BUSY. */
    uint32_t exclusive_lane;
    /* Read current revision for result delivery; NULL means revision=0. */
    uint64_t (*revision)(const void *context);
    /* Pure check, no mutation; runs at acceptance AND execution. */
    CAResult (*check)(const void *context, const void *payload);
    /* Infallible after check at this boundary; no allocation/free/history replay. */
    void (*apply)(void *context, void *payload);
    void (*dispose)(void *payload); /* deferred, non-RT */
} CAOperationOps;
CAOperation *ca_operation_prepare(const CAOperationOps *, void *payload, CAResult *);
void ca_operation_destroy(CAOperation *);
/* Queue/result/retirement capacity reserved atomically. Result for every accepted
   operation, including cancellation. Takes operation only on success.
   Source registration carries a provider key, supplied via bind_provider below. */
CAResult ca_source_operation(CAMixer *, CAHandle, CAOperation *, int64_t at, CACommandID *);
CAResult ca_source_bind_provider(CASource *, const void *provider); /* before attach */
/* Read-only, owning-thread query. No context pointer escapes. */
CAResult ca_source_query(const CAMixer *, CAHandle, const void *provider,
                         void (*query)(const void *context, void *out), void *out);
#ifdef __cplusplus
}
#endif
#endif
