#include "chip-audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Compile the production implementation with allocator hooks. The RT flag also
// covers all underlying source renderers (DPCM and echo included).
static int realtime, fail_alloc;
static size_t allocations, frees, bytes;
static void *checked_malloc(size_t n) {
    assert(!realtime);if(fail_alloc){fail_alloc=0;return NULL;}
    void *p=malloc(n);if(p){allocations++;bytes+=n;}return p;
}
static void *checked_calloc(size_t n,size_t size) {
    assert(!realtime);if(fail_alloc){fail_alloc=0;return NULL;}
    void *p=calloc(n,size);if(p){allocations++;bytes+=n*size;}return p;
}
static void checked_free(void *p) {assert(!realtime);if(p)frees++;free(p);}
#define malloc checked_malloc
#define calloc checked_calloc
#define free checked_free
#include "../src/chip-audio.c"
#undef malloc
#undef calloc
#undef free

static ChipAudio *sound(int kind,int length,int pitch) {
    ChipAudioNote n={0,length,pitch,15,0,0};
    ChipAudioTrack tracks[4]={{CHIP_PULSE,NULL,0},{CHIP_PULSE,NULL,0},
                              {CHIP_TRIANGLE,NULL,0},{CHIP_NOISE,NULL,0}};
    int channel=kind==1?2:kind==2?3:0;
    if(kind!=3){tracks[channel].notes=&n;tracks[channel].count=1;}
    if(kind==4)n.duration=256;
    ChipAudio *a=chip_audio_create(tracks,length,NULL);assert(a);
    if(kind==3){ChipDPCMHit hit={0,length,0,15};assert(chip_audio_set_dpcm_kicks(a,&hit,1));}
    if(kind==4){ChipEchoTap tap={0,256,512,0,1};assert(chip_audio_set_echo_taps(a,&tap,1));}
    return a;
}
static int16_t pcm(double value) {return value>32767?32767:value<-32768?-32768:(int16_t)value;}
static void render(ChipAudioPlayer *p,int16_t *out,size_t n,unsigned mask,int chunk) {
    static const size_t irregular[]={1,127,129,735,3,256,17};size_t index=0;
    realtime=1;
    while(n){size_t take=chunk? (size_t)chunk:irregular[index++%7];if(take>n)take=n;
        chip_audio_player_render(p,out,take,mask);out+=take;n-=take;}
    realtime=0;
}
static void collect_all(ChipAudioPlayer *p) {
    for(;;){realtime=1;ChipAudio *a=chip_audio_player_collect(p);realtime=0;if(!a)break;chip_audio_destroy(a);}
}
static void reference_transitions(void) {
    int16_t actual[45000],old_pcm[45000],new_pcm[45000],warm[735];
    const int fades[]={0,1,128,44100},chunks[]={45000,1,127,128,129,735,0};
    for(int kind=0;kind<5;kind++)for(size_t f=0;f<4;f++)for(size_t c=0;c<7;c++){
        int n=fades[f],length=kind==4?4096:kind==3?5000:50000;
        ChipAudioPlayer *p=chip_audio_player_create(5,NULL);assert(p);
        ChipAudio *a=sound(kind,length,69),*ar=sound(kind,length,69);
        ChipAudio *b=sound((kind+1)%3,50000,75),*br=sound((kind+1)%3,50000,75);
        realtime=1;assert(chip_audio_player_switch(p,a,128)==CHIP_AUDIO_OK);realtime=0;
        render(p,warm,735,15,735);
        chip_audio_render(ar,warm,735,15);
        realtime=1;assert(chip_audio_player_switch(p,b,n)==CHIP_AUDIO_OK);realtime=0;
        render(p,actual,45000,15,chunks[c]);
        chip_audio_render(ar,old_pcm,45000,15);chip_audio_render(br,new_pcm,45000,15);
        for(int i=0;i<45000;i++)assert(actual[i]==pcm(new_pcm[i]+(i<n?old_pcm[i]*((double)(n-i)/n):0)));
        assert(!chip_audio_player_finished(p));collect_all(p);
        chip_audio_player_destroy(p);chip_audio_destroy(ar);chip_audio_destroy(br);
    }
    puts("player reference: pulse/triangle/noise/DPCM/echo, N=0/1/128/44100, natural end, immediate new onset, all chunk sizes: PASS");
}
static void repeated_transitions(void) {
    int16_t actual[300],a_pcm[300],b_pcm[300],c_pcm[300],discard;
    for(int gap=0;gap<2;gap++){
        ChipAudioPlayer *p=chip_audio_player_create(5,NULL);
        ChipAudio *a=sound(0,1000,60),*b=sound(1,1000,65),*c=sound(2,1000,70);
        ChipAudio *ar=sound(0,1000,60),*br=sound(1,1000,65),*cr=sound(2,1000,70);
        realtime=1;assert(!chip_audio_player_switch(p,a,128));assert(!chip_audio_player_switch(p,b,128));
        chip_audio_player_render(p,&discard,gap,15);assert(!chip_audio_player_switch(p,c,128));realtime=0;
        chip_audio_render(ar,&discard,gap,15);chip_audio_render(br,&discard,gap,15);
        render(p,actual,300,15,0);chip_audio_render(ar,a_pcm,300,15);chip_audio_render(br,b_pcm,300,15);chip_audio_render(cr,c_pcm,300,15);
        for(int i=0;i<300;i++)assert(actual[i]==pcm((i+gap<128?a_pcm[i]*((double)(128-i-gap)/128):0)+(i<128?b_pcm[i]*((double)(128-i)/128):0)+c_pcm[i]));
        realtime=1;assert(!chip_audio_player_stop(p,128));realtime=0;
        render(p,actual,1,15,1);
        realtime=1;assert(!chip_audio_player_stop(p,44100));realtime=0; // Must not extend existing fade.
        render(p,actual,127,15,0);assert(chip_audio_player_finished(p));
        render(p,actual,300,15,0);for(int i=0;i<300;i++)assert(!actual[i]);
        chip_audio_player_destroy(p);chip_audio_destroy(ar);chip_audio_destroy(br);chip_audio_destroy(cr);
    }
    puts("player repeated: same-sample / one-sample A->B->C, fade deadlines and repeated stop: PASS");
}
static void validation_and_busy(void) {
    ChipAudioError error=CHIP_AUDIO_OK;
    assert(!chip_audio_player_create(0,&error)&&error==CHIP_AUDIO_INVALID);
    assert(!chip_audio_player_create(1,NULL)&&!chip_audio_player_create(18,NULL));
    fail_alloc=1;assert(!chip_audio_player_create(5,&error)&&error==CHIP_AUDIO_MEMORY);
    ChipAudioPlayer *p=chip_audio_player_create(2,&error);assert(p&&error==CHIP_AUDIO_OK);
    ChipAudio *a=sound(0,2000,60),*b=sound(1,2000,65),*rejected=sound(2,2000,70);
    realtime=1;
    assert(chip_audio_player_switch(NULL,a,128)==CHIP_AUDIO_INVALID);
    assert(chip_audio_player_switch(p,NULL,128)==CHIP_AUDIO_INVALID);
    assert(chip_audio_player_switch(p,a,-1)==CHIP_AUDIO_INVALID);
    assert(chip_audio_player_switch(p,a,44101)==CHIP_AUDIO_INVALID);
    assert(chip_audio_player_stop(p,-1)==CHIP_AUDIO_INVALID);
    assert(chip_audio_player_stop(p,44101)==CHIP_AUDIO_INVALID);
    assert(chip_audio_player_finished(p));
    chip_audio_stop(a);assert(chip_audio_player_switch(p,a,128)==CHIP_AUDIO_INVALID);
    chip_audio_reset(a);realtime=0;
    chip_audio_seek(a,1);assert(chip_audio_player_switch(p,a,128)==CHIP_AUDIO_INVALID);
    chip_audio_seek(a,2000);assert(chip_audio_player_switch(p,a,128)==CHIP_AUDIO_INVALID);chip_audio_reset(a);
    realtime=1;
    assert(!chip_audio_player_switch(p,a,128));
    assert(chip_audio_player_switch(p,a,128)==CHIP_AUDIO_INVALID);
    assert(!chip_audio_player_switch(p,b,128));
    assert(chip_audio_player_switch(p,a,0)==CHIP_AUDIO_INVALID);
    assert(chip_audio_player_switch(p,rejected,0)==CHIP_AUDIO_BUSY);
    assert(chip_audio_position(rejected)==0);
    chip_audio_player_render(p,NULL,1000,15);
    int16_t sentinel=123;chip_audio_player_render(p,&sentinel,0,15);assert(sentinel==123);
    assert(!chip_audio_player_collect(p));realtime=0;
    int16_t actual[128],expected_a[128],expected_b[128];
    ChipAudio *ar=sound(0,2000,60),*br=sound(1,2000,65);
    chip_audio_render(ar,expected_a,128,15);chip_audio_render(br,expected_b,128,15);
    render(p,actual,128,15,0);
    for(int i=0;i<128;i++)assert(actual[i]==pcm(expected_b[i]+expected_a[i]*((double)(128-i)/128)));
    realtime=1;
    assert(chip_audio_player_switch(p,rejected,128)==CHIP_AUDIO_BUSY); // Retired slot still occupied.
    assert(chip_audio_player_collect(p)==a);
    assert(!chip_audio_player_switch(p,rejected,128));
    chip_audio_player_clear(p);assert(chip_audio_player_finished(p));
    assert(chip_audio_player_switch(p,b,128)==CHIP_AUDIO_INVALID); // Retired duplicate.
    chip_audio_player_clear(p);realtime=0;
    render(p,actual,128,15,0);for(int i=0;i<128;i++)assert(!actual[i]);
    collect_all(p);chip_audio_destroy(a);chip_audio_destroy(ar);chip_audio_destroy(br);
    chip_audio_player_destroy(p);chip_audio_player_destroy(NULL);
    chip_audio_player_render(NULL,actual,128,15);for(int i=0;i<128;i++)assert(!actual[i]);
    puts("player validation: limits, allocation failure, invalid inputs, duplicate/retired ownership, BUSY atomicity, zero-frame, clear/collect: PASS");
}
static void saturation_and_mute(void) {
    ChipAudioPlayer *p=chip_audio_player_create(17,NULL);assert(p);
    int16_t actual[1024],expected[1024],discard[256];
    ChipAudio *reference=sound(0,4000,69);assert(chip_audio_set_voice_gain(reference,0,2));chip_audio_render(reference,discard,256,15);
    for(int i=0;i<17;i++){
        ChipAudio *a=sound(0,4000,69);assert(chip_audio_set_voice_gain(a,0,2));
        // All at the same starting phase; exact expected sum of 16 fades + current.
        assert(!chip_audio_player_switch(p,a,44100));
    }
    render(p,discard,256,0,0);for(int i=0;i<256;i++)assert(!discard[i]);
    render(p,actual,1024,15,0);chip_audio_render(reference,expected,1024,15);
    int clipped=0;
    for(int i=0;i<1024;i++){
        double sum=0;for(int j=0;j<16;j++)sum+=expected[i]*((double)(44100-256-i)/44100);sum+=expected[i];
        assert(actual[i]==pcm(sum));if(actual[i]==32767||actual[i]==-32768)clipped++;
    }
    assert(clipped>0);chip_audio_player_destroy(p);chip_audio_destroy(reference);
    for(int i=0;i<100;i++){
        p=chip_audio_player_create(2,NULL);assert(!chip_audio_player_switch(p,sound(3,5000,60),0));
        render(p,discard,256,15,0);assert(!chip_audio_player_stop(p,0));assert(chip_audio_player_finished(p));
        collect_all(p);chip_audio_player_destroy(p);
    }
    puts("player mix: 17 voices, max gain, saturation, mute advances clocks/fades, repeated lifetime: PASS");
}
int main(void) {
    reference_transitions();repeated_transitions();validation_and_busy();saturation_and_mute();
    assert(allocations==frees);
    printf("player RT allocation/free hooks: PASS; lifetime allocations=%zu frees=%zu\n",allocations,frees);
    return 0;
}
