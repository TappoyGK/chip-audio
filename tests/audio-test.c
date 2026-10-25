#include "chip-audio.h"
#include "../src/synth.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void parity(void) {
    ChipAudioNote notes[]={{31,700,60,10},{731,333,65,8},{1400,100,72,15}};
    ChipAudioTrack tracks[4];Voice original[4]={0};Sound sounds[4]={0};
    for(int p=0;p<4;p++){
        tracks[p]=(ChipAudioTrack){p<2?CHIP_PULSE:p==2?CHIP_TRIANGLE:CHIP_NOISE,notes,3};
        sounds[p].sustain_level=1;sounds[p].wave=tracks[p].wave;sounds[p].count=6;
        Note segments[]={{0,31,0},{60,700,10},{65,333,8},{0,336,0},{72,100,15},{0,200,0}};
        memcpy(sounds[p].notes,segments,sizeof(segments));synth_start(&original[p],&sounds[p],p);
    }
    ChipAudioError error;ChipAudio *a=chip_audio_create(tracks,1700,&error);assert(a&&error==CHIP_AUDIO_OK);
    int16_t expected[1700],actual[1700],loop[1700];synth_render(original,expected,1700);
    size_t offset=0;while(offset<1700){size_t n=(offset%79)+1;if(n>1700-offset)n=1700-offset;chip_audio_render(a,actual+offset,n,15);offset+=n;}
    assert(!memcmp(expected,actual,sizeof(actual)));assert(chip_audio_finished(a));
    chip_audio_reset(a);chip_audio_render(a,loop,1700,15);assert(!memcmp(actual,loop,sizeof(loop)));
    chip_audio_reset(a);chip_audio_render(a,loop,50,0);for(int i=0;i<50;i++)assert(loop[i]==0);
    chip_audio_render(a,loop+50,1650,15);assert(!memcmp(actual+50,loop+50,1650*sizeof(int16_t)));
    chip_audio_reset(a);chip_audio_stop(a);loop[0]=42;chip_audio_render(a,loop,1,15);assert(loop[0]==0&&chip_audio_position(a)==0);
    chip_audio_destroy(a);
}
static void large_and_invalid(void) {
    ChipAudioNote notes[2000];for(int i=0;i<2000;i++)notes[i]=(ChipAudioNote){i*10,5,60,8};
    ChipAudioTrack tracks[4]={{CHIP_PULSE,notes,2000},{CHIP_PULSE,NULL,0},{CHIP_TRIANGLE,NULL,0},{CHIP_NOISE,NULL,0}};
    ChipAudioError error;ChipAudio *a=chip_audio_create(tracks,20000,&error);assert(a);int16_t output[20010];chip_audio_render(a,output,20010,15);assert(chip_audio_position(a)==20000);for(int i=20000;i<20010;i++)assert(output[i]==0);chip_audio_destroy(a);
    notes[1].at=0;assert(!chip_audio_create(tracks,20000,&error)&&error==CHIP_AUDIO_INVALID);notes[1].at=10;
    notes[0].duration=INT64_MAX;assert(!chip_audio_create(tracks,20000,&error));
    tracks[0].count=0;a=chip_audio_create(tracks,(int64_t)INT_MAX+100,&error);assert(a);chip_audio_render(a,output,64,15);assert(chip_audio_position(a)==64);chip_audio_destroy(a);
    assert(!chip_audio_create(tracks,0,&error));
}
static void pulse_duty(void) {
    ChipAudioNote note={0,44100,69,15};ChipAudioTrack tracks[4]={{CHIP_PULSE,&note,1},{CHIP_PULSE,&note,1},{CHIP_TRIANGLE,NULL,0},{CHIP_NOISE,NULL,0}};
    ChipAudio *a=chip_audio_create(tracks,44100,NULL);assert(a);int16_t pcm[44100],other[44100],reference[44100];
    chip_audio_render(a,reference,44100,2);double duties[]={.125,.25,.5,.75};
    for(int d=0;d<4;d++){
        chip_audio_reset(a);assert(chip_audio_set_pulse_duty(a,0,duties[d]));chip_audio_render(a,pcm,44100,1);int positive=0;for(int i=1000;i<43000;i++)if(pcm[i]>0)positive++;assert(fabs(positive/42000.0-duties[d])<.002);
        chip_audio_reset(a);chip_audio_render(a,other,44100,1);assert(!memcmp(pcm,other,sizeof(pcm)));
        chip_audio_reset(a);chip_audio_render(a,other,44100,2);assert(!memcmp(reference,other,sizeof(other)));
    }
    assert(!chip_audio_set_pulse_duty(a,2,.5)&&!chip_audio_set_pulse_duty(a,-1,.5)&&!chip_audio_set_pulse_duty(a,0,.3)&&!chip_audio_set_pulse_duty(a,0,NAN));chip_audio_destroy(a);
    puts("pulse duty: four ratios measured from PCM, independent channels, reset deterministic, invalid inputs: PASS");
}
static void dpcm(void) {
    ChipAudioTrack tracks[4]={{CHIP_PULSE,NULL,0},{CHIP_PULSE,NULL,0},{CHIP_TRIANGLE,NULL,0},{CHIP_NOISE,NULL,0}};
    ChipAudio *a=chip_audio_create(tracks,12000,NULL);assert(a);
    ChipDPCMHit hit={50,10000,0,15};assert(chip_audio_set_dpcm_kicks(a,&hit,1));
    int16_t reference[12000],pcm[12000];chip_audio_render(a,reference,12000,8);
    for(int i=0;i<50;i++)assert(reference[i]==0);
    // DAC load creates the discontinuity immediately; no slow attack ramp.
    assert(reference[50]>=12000);
    for(int i=51;i<56;i++)assert(reference[i]==reference[50]);
    long long attackEnergy=0,tailEnergy=0;
    for(int i=50;i<491;i++)attackEnergy+=(long long)reference[i]*reference[i];
    for(int i=491;i<1500;i++)tailEnergy+=(long long)reference[i]*reference[i];
    assert(attackEnergy>tailEnergy*20);
    long long energy=0;for(int i=50;i<8000;i++)energy+=(long long)reference[i]*reference[i];assert(energy>100000000);
    // Quantization carrier must vanish after the short pop, not hiss in the tail.
    for(int i=950;i<12000;i++)assert(reference[i]==0);
    chip_audio_reset(a);for(int at=0;at<12000;){int n=at%127+1;if(n>12000-at)n=12000-at;chip_audio_render(a,pcm+at,n,8);at+=n;}assert(!memcmp(reference,pcm,sizeof(pcm)));
    chip_audio_reset(a);chip_audio_render(a,pcm,500,0);for(int i=0;i<500;i++)assert(!pcm[i]);chip_audio_render(a,pcm+500,11500,8);assert(!memcmp(reference+500,pcm+500,11500*sizeof(int16_t)));
    ChipDPCMHit bad=hit;bad.offset=INT64_MAX;assert(!chip_audio_set_dpcm_kicks(a,&bad,1));chip_audio_reset(a);chip_audio_render(a,pcm,12000,8);assert(!memcmp(reference,pcm,sizeof(pcm)));
    ChipDPCMHit clipped={0,9050,1000,15};assert(chip_audio_set_dpcm_kicks(a,&clipped,1));chip_audio_reset(a);chip_audio_render(a,pcm,12000,8);assert(!memcmp(reference+1050,pcm,7000*sizeof(int16_t)));
    ChipDPCMHit repeats[]={{0,8000,0,15},{1000,8000,0,15}};assert(chip_audio_set_dpcm_kicks(a,repeats,2));chip_audio_reset(a);chip_audio_render(a,pcm,12000,8);assert(!memcmp(pcm,pcm+1000,1000*sizeof(int16_t)));
    chip_audio_stop(a);chip_audio_render(a,pcm,20,8);for(int i=0;i<20;i++)assert(!pcm[i]);chip_audio_destroy(a);
    // Noise and DPCM run concurrently; adding kicks does not alter the noise PCM.
    ChipAudioNote noise={0,10000,100,4};tracks[3].notes=&noise;tracks[3].count=1;
    a=chip_audio_create(tracks,12000,NULL);chip_audio_render(a,pcm,12000,8);
    assert(chip_audio_set_dpcm_kicks(a,&hit,1));chip_audio_reset(a);int16_t mix[12000];chip_audio_render(a,mix,12000,8);
    for(int i=0;i<12000;i++)assert(mix[i]==pcm[i]+reference[i]);chip_audio_destroy(a);
    puts("DPCM: delta decode/rate, chunk parity, seek offset, mute, reset, retrigger, stop, independent noise mixing: PASS");
}
static void live_percussion(void){
    ChipAudioNote tone={0,12000,60,8},noise={0,12000,100,5};
    ChipAudioTrack tracks[4]={{CHIP_PULSE,&tone,1},{CHIP_PULSE,NULL,0},{CHIP_TRIANGLE,NULL,0},{CHIP_NOISE,&noise,1}};
    ChipAudio *a=chip_audio_create(tracks,14000,NULL),*reference=chip_audio_create(tracks,14000,NULL);int16_t scratch[2000],expected[2000],actual[2000];
    chip_audio_render(a,scratch,2000,15);chip_audio_render(reference,scratch,2000,15);
    tracks[3].count=0;ChipAudio *updated=chip_audio_create(tracks,14000,NULL);chip_audio_exchange_percussion(a,updated);chip_audio_destroy(updated);
    assert(chip_audio_position(a)==2000);chip_audio_render(a,actual,2000,15);chip_audio_render(reference,expected,2000,1);assert(!memcmp(actual,expected,sizeof(actual)));
    chip_audio_reset(a);chip_audio_render(a,actual,2000,8);for(int i=0;i<2000;i++)assert(!actual[i]);
    chip_audio_destroy(a);chip_audio_destroy(reference);puts("live percussion: changed voice only, tonal PCM/position preserved, loop keeps settings: PASS");
}

static void vibrato_release(void){
    ChipAudioNote note={0,88200,69,15,40000,48200};
    ChipAudioTrack tracks[4]={{CHIP_PULSE,&note,1},{CHIP_PULSE,NULL,0},{CHIP_TRIANGLE,NULL,0},{CHIP_NOISE,NULL,0}};
    ChipAudio *a=chip_audio_create(tracks,88200,NULL),*b=chip_audio_create(tracks,88200,NULL);
    ChipPulseChange change={.at=0,.duty=.25,.rate=7,.depth=25,.delay=.124};
    ChipPulseOnset onset={0,44100};
    assert(chip_audio_set_pulse_changes(a,0,&change,1,&onset,1));
    assert(chip_audio_set_pulse_changes(b,0,&change,1,&onset,1));
    int16_t *full=calloc(88200,2),*chunks=calloc(88200,2);
    chip_audio_render(a,full,88200,1);
    for(int at=0;at<88200;){int n=88200-at<317?88200-at:317;chip_audio_render(b,chunks+at,n,1);at+=n;}
    for(int i=0;i<88200;i++)assert(abs(full[i]-chunks[i])<=1);
    // Estimate pitch from full waveform periods, including the release tail.
    int prev=-1,cycles=0;double min=1000,max=-1000,sum=0;
    for(int i=6000;i<80000;i++)if(full[i-1]<=0&&full[i]>0){
        if(prev>=0){double cents=1200*log2(44100.0/(i-prev)/440.0);assert(fabs(cents)<43);if(cents<min)min=cents;if(cents>max)max=cents;sum+=cents;cycles++;}prev=i;
    }
    assert(cycles>600&&min<-10&&max>10&&fabs(sum/cycles)<4);
    free(full);free(chunks);chip_audio_destroy(a);chip_audio_destroy(b);
    puts("vibrato: 7Hz/25cent pitch bounds and centered tuning, release continuity/chunk parity: PASS");
}
// Pulse waveform edges are intentional; the magnitude must not jump when ADSR
// retriggers, switches from attack to zero decay, or is edited during playback.
static void adsr_clicks(void) {
    ChipAudioNote note={0,18000,69,15};
    ChipAudioTrack tracks[4]={{CHIP_PULSE,&note,1},{CHIP_PULSE,NULL,0},{CHIP_TRIANGLE,NULL,0},{CHIP_NOISE,NULL,0}};
    ChipPulseOnset onsets[]={{0,6000},{6000,18000}};
    ChipPulseChange changes[]={{.at=0,.duty=.25,.rate=6,.attack=.02,.sustain_drop=.75},
        {.at=9000,.duty=.25,.rate=6,.attack=.02,.sustain_drop=0}};
    ChipAudio *a=chip_audio_create(tracks,18000,NULL);
    assert(chip_audio_set_pulse_changes(a,0,changes,2,onsets,2));
    int16_t full[18000],chunked[18000];chip_audio_render(a,full,18000,1);
    for(int i=1;i<18000;i++)assert(abs(abs(full[i])-abs(full[i-1]))<=48);
    assert(abs(full[5000])==1500&&abs(full[11000])==6000);
    chip_audio_reset(a);for(int i=0;i<18000;i+=97)chip_audio_render(a,chunked+i,(18000-i<97?18000-i:97),1);
    for(int i=0;i<18000;i++)assert(abs(full[i]-chunked[i])<=1);
    chip_audio_reset(a);chip_audio_render(a,full,5000,1);
    ChipAudio *b=chip_audio_create(tracks,18000,NULL);changes[0].sustain_drop=0;
    assert(chip_audio_set_pulse_changes(b,0,changes,2,onsets,2));
    chip_audio_exchange_timbres(a,b);chip_audio_render(a,full+5000,1000,1);
    for(int i=5000;i<6000;i++)assert(abs(abs(full[i])-abs(full[i-1]))<=48);
    assert(chip_audio_position(a)==6000&&abs(full[5999])==6000);
    chip_audio_destroy(a);chip_audio_destroy(b);
    puts("ADSR anti-click: retrigger, zero decay, automation and live sustain edit; bounded gain steps, chunk parity: PASS");
}
static void boosted_voices(void) {
    ChipAudioNote n={0,4000,60,10};ChipAudioTrack tracks[4]={{CHIP_PULSE,NULL,0},{CHIP_PULSE,NULL,0},{CHIP_TRIANGLE,&n,1},{CHIP_NOISE,NULL,0}};
    ChipAudio *boost=chip_audio_create(tracks,5000,NULL);n.volume=15;ChipAudio *old=chip_audio_create(tracks,5000,NULL);
    ChipDPCMHit hit={0,4000,0,10};assert(chip_audio_set_dpcm_kicks(boost,&hit,1));hit.volume=15;assert(chip_audio_set_dpcm_kicks(old,&hit,1));
    assert(chip_audio_set_voice_gain(boost,2,1.5)&&chip_audio_set_voice_gain(boost,4,1.5));
    int16_t a[5000],b[5000],chunked[5000];
    for(unsigned mask=4;mask<=8;mask+=4){chip_audio_reset(boost);chip_audio_reset(old);chip_audio_render(boost,a,5000,mask);chip_audio_render(old,b,5000,mask);
        for(int i=128;i<3800;i++)assert(abs(a[i]-b[i])<=3); // new f = old fff, excluding the volume ramp
        chip_audio_reset(boost);for(int i=0;i<5000;i+=73)chip_audio_render(boost,chunked+i,5000-i<73?5000-i:73,mask);assert(!memcmp(a,chunked,sizeof(a)));
    }
    assert(!chip_audio_set_voice_gain(boost,5,1.5)&&!chip_audio_set_voice_gain(boost,2,NAN));
    chip_audio_destroy(boost);chip_audio_destroy(old);
    puts("triangle/kick gain: new f equals old fff, independent voices, reset and chunk parity: PASS");
}
int main(void) { boosted_voices();adsr_clicks();vibrato_release();parity();large_and_invalid();pulse_duty();dpcm();live_percussion();puts("audio: original synth PCM parity, reset, mute, stop, >513 segments, bounds: PASS"); }
