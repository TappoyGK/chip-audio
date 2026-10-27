#define main player_contract_main
#include "player-test.c"
#undef main
static void stems(ChipAudio *a, double **p, size_t n) {
    realtime=1;chip_audio_render_stems(a,p,n);realtime=0;
}
static ChipAudio *ensemble(void) {
    ChipAudioNote n={0,3000,60,15,0,0};
    ChipAudioTrack tracks[4];
    for(int i=0;i<4;i++)tracks[i]=(ChipAudioTrack){CHIP_PULSE,&n,1};
    ChipAudio *a=chip_audio_create(tracks,3000,NULL);assert(a);
    for(int i=0;i<5;i++)assert(chip_audio_set_voice_gain(a,i,2));
    ChipDPCMHit hit={0,3000,0,15};assert(chip_audio_set_dpcm_kicks(a,&hit,1));
    return a;
}
int main(void) {
    double planes[5][4096],split[5][4096];double *p[5];int16_t legacy[4096];
    for(int kind=0;kind<6;kind++) {
        ChipAudio *a=kind==5?ensemble():sound(kind,3000,69);
        ChipAudio *b=kind==5?ensemble():sound(kind,3000,69);
        ChipAudio *c=kind==5?ensemble():sound(kind,3000,69);
        for(int i=0;i<5;i++)p[i]=planes[i];
        stems(a,p,4096);chip_audio_render(b,legacy,4096,15);
        size_t sizes[]={1,127,128,129,735,3};size_t offset=0,block=0;
        while(offset<4096){size_t n=sizes[block++%6];if(n>4096-offset)n=4096-offset;
            for(int i=0;i<5;i++)p[i]=split[i]+offset;stems(c,p,n);offset+=n;}
        assert(memcmp(planes,split,sizeof planes)==0);
        int saturated=0,delayed=0;
        for(int i=0;i<4096;i++){
            double sum=0;for(int ch=0;ch<5;ch++)sum+=planes[ch][i];
            assert(pcm(sum)==legacy[i]);if(sum>32767||sum< -32768)saturated++;
            if(kind==3){for(int ch=0;ch<4;ch++)assert(planes[ch][i]==0);}
            if(kind==4&&i>=512&&planes[1][i]!=0)delayed++;
            if(i>=3000)assert(sum==0);
        }
        if(kind==5)assert(saturated>0);
        if(kind==4)assert(delayed>0);
        assert(chip_audio_position(a)==chip_audio_position(b));
        chip_audio_reset(a);chip_audio_reset(b);
        stems(a,NULL,735);chip_audio_render(b,legacy,735,15);
        for(int i=0;i<5;i++)p[i]=planes[i];stems(a,p,128);
        chip_audio_render(b,legacy,128,15);
        for(int i=0;i<128;i++){double sum=0;for(int ch=0;ch<5;ch++)sum+=planes[ch][i];assert(pcm(sum)==legacy[i]);}
        chip_audio_stop(a);stems(a,p,128);
        for(int ch=0;ch<5;ch++)for(int i=0;i<128;i++)assert(planes[ch][i]==0);
        chip_audio_destroy(a);chip_audio_destroy(b);chip_audio_destroy(c);
    }
    // Dropping one plane does not mute the echo source or history generation.
    ChipAudio *a=sound(4,3000,69),*b=sound(4,3000,69);
    double *partial[5]={NULL,planes[1],NULL,NULL,NULL};
    for(int i=0;i<5;i++)p[i]=split[i];stems(a,partial,1024);stems(b,p,1024);
    assert(memcmp(planes[1],split[1],1024*sizeof(double))==0);
    int64_t position=chip_audio_position(a);stems(a,partial,0);assert(chip_audio_position(a)==position);
    chip_audio_destroy(a);chip_audio_destroy(b);
    stems(NULL,partial,1024);for(int i=0;i<1024;i++)assert(planes[1][i]==0);
    ChipAudioGain gain, split_gain;
    assert(chip_audio_gain_init(&gain,1));
    assert(chip_audio_gain_set(&gain,0,4));
    double envelope[8], expected[]={1,.75,.5,.25,0,0,0,0};
    realtime=1;chip_audio_gain_render(&gain,envelope,8);realtime=0;
    assert(memcmp(envelope,expected,sizeof expected)==0);
    assert(chip_audio_gain_init(&gain,1));assert(chip_audio_gain_set(&gain,0,4));
    chip_audio_gain_render(&gain,NULL,2);assert(gain.current==.5);
    assert(chip_audio_gain_set(&gain,1,2));chip_audio_gain_render(&gain,envelope,3);
    assert(envelope[0]==.5&&envelope[1]==.75&&envelope[2]==1);
    ChipAudioGain saved=gain;
    assert(!chip_audio_gain_set(&gain,NAN,128));assert(!chip_audio_gain_set(&gain,1,44101));
    assert(memcmp(&gain,&saved,sizeof gain)==0);
    assert(chip_audio_gain_set(&gain,0,0));assert(gain.current==0);
    assert(chip_audio_gain_init(&gain,1));assert(chip_audio_gain_set(&gain,.123,44100));
    split_gain=gain;double whole[1000],part[1000];
    chip_audio_gain_render(&gain,whole,1000);
    for(int i=0;i<1000;i++)chip_audio_gain_render(&split_gain,part+i,1);
    assert(memcmp(whole,part,sizeof whole)==0);
    double sum[]={0,0,0},high[]={40000,-40000,1.9},low[]={-10000,10000,-.1};int16_t result[3];
    realtime=1;
    chip_audio_mix_add(sum,high,NULL,3);chip_audio_mix_add(sum,low,NULL,3);
    chip_audio_mix_pcm(sum,result,3);
    realtime=0;
    assert(result[0]==30000&&result[1]==-30000&&result[2]==1);
    chip_audio_mix_add(sum,high,NULL,3);chip_audio_mix_pcm(sum,result,3);
    assert(result[0]==32767&&result[1]==-32768);
    assert(allocations==frees);
    puts("gain/mix: ramp endpoints, retarget, invalid atomicity, split/discard, final-only clipping: PASS");
    puts("stems: five voices, echo destination/history, clipping parity, split/discard, stop/end, RT hooks: PASS");
    return 0;
}
