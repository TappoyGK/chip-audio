#define _POSIX_C_SOURCE 200809L
#define main player_contract_main
#include "player-test.c"
#undef main
#ifdef _WIN32
#include <windows.h>
static double now(void) {
    LARGE_INTEGER ticks, frequency;
    assert(QueryPerformanceCounter(&ticks));
    assert(QueryPerformanceFrequency(&frequency));
    return (double)ticks.QuadPart / frequency.QuadPart;
}
#else
#include <time.h>
static double now(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}
#endif
int main(void){
    const int voices[]={1,2,17};
    for(int scenario=0;scenario<3;scenario++){
        double total=0,worst=0;size_t memory=0;
        for(int trial=0;trial<500;trial++){
            size_t before=bytes;ChipAudioPlayer *p=chip_audio_player_create(voices[scenario]==1?2:voices[scenario],NULL);assert(p);
            for(int i=0;i<voices[scenario];i++)assert(!chip_audio_player_switch(p,sound(i%5,100000,60+i),44100));
            memory=bytes-before;int16_t pcm[735];render(p,pcm,256,15,256);
            double begin=now();render(p,pcm,735,15,735);double elapsed=now()-begin;
            total+=elapsed;if(elapsed>worst)worst=elapsed;
            chip_audio_player_destroy(p);
        }
        printf("sources=%d frames=735 trials=500 mean_ms=%.4f max_ms=%.4f allocated_bytes=%zu player_bytes=%zu budget_ms=16.6667\n",
               voices[scenario],total*1000/500,worst*1000,memory,sizeof(ChipAudioPlayer)+(voices[scenario]==1?2:voices[scenario])*sizeof(PlayerSlot));
    }
    assert(allocations==frees);return 0;
}
