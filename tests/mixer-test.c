#include "chip-source.h"
#include "chip-audio.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct { int64_t position,length,period; double value; uint64_t revision; } Counter;
static int disposed;
static int64_t settle(void *p){Counter *c=p;return c->position==c->length?0:c->period?c->period-c->position%c->period:c->length-c->position;}
static void render(void *p,double *planes[5],size_t n){Counter *c=p;assert(c->position+(int64_t)n<=c->length);for(int j=0;j<5;j++)for(size_t i=0;i<n;i++)planes[j][i]=j?0:c->value;c->position+=(int64_t)n;}
static void dispose(void *p){disposed++;free(p);}
static const char provider;
static CALimits limits(void){CALimits l={0};l.allocated_bytes=1024*1024;l.se_instances=17;l.commands=64;l.retired_objects=64;l.command_results=16;l.gain_bindings=32;return l;}
static CASource *source(int64_t length,int64_t period,double value){Counter *c=calloc(1,sizeof(*c));assert(c);*c=(Counter){0,length,period,value,1};CASourceOps ops={settle,render,dispose};CALimits l=limits();CASource *s=ca_source_prepare(c,&ops,&l,NULL);assert(s);assert(ca_source_bind_provider(s,&provider)==CA_OK);return s;}
static void query(const void *p,void *out){*(Counter *)out=*(const Counter *)p;}
static void run(CAMixer *m,double *out,size_t n,size_t chunk){for(size_t at=0;at<n;){size_t take=chunk?chunk:(at%137+1);if(take>n-at)take=n-at;CAOutput output={.mix=out+at};assert(ca_mixer_render(m,take,&output)==CA_OK);at+=take;}}
static void gates(size_t chunk,double output[900]){
    CALimits l=limits();CAMixer *m=ca_mixer_create(&l,NULL);assert(m);CAHandle bgm,se,binding;
    assert(ca_source_attach(m,source(2000,137,1000),(CARoute){CA_BGM,0},0,&bgm)==CA_OK);
    assert(ca_source_attach(m,source(137,0,0),(CARoute){CA_SE,0},0,&se)==CA_OK);
    CAGainBinding b={se,bgm,0,0,1};assert(ca_bind_gain(m,&b,0,&binding)==CA_OK);
    run(m,output,900,chunk);assert(output[137]==0&&output[138]==1000./128&&output[265]==1000);
    Counter pos;assert(ca_source_query(m,bgm,&provider,query,&pos)==CA_OK&&pos.position==900);
    CARetired *r;while((r=ca_collect(m)))ca_dispose(r);ca_mixer_destroy(m);
}
static CAResult check(const void *ctx,const void *data){return ((const Counter *)ctx)->revision==*(const uint64_t *)data?CA_OK:CA_STALE_REVISION;}
static void apply(void *ctx,void *data){((Counter *)ctx)->revision=*(uint64_t *)data+1;}
static uint64_t revision(const void *ctx){return ((const Counter *)ctx)->revision;}
static CAOperation *operation(void){uint64_t *v=malloc(sizeof(*v));assert(v);*v=1;CAOperationOps o={&provider,1,revision,check,apply,free};CAOperation *p=ca_operation_prepare(&o,v,NULL);assert(p);return p;}
int main(void){
    double expected[900],actual[900];gates(1,expected);size_t chunks[]={128,735,0};for(int i=0;i<3;i++){gates(chunks[i],actual);assert(memcmp(actual,expected,sizeof actual)==0);}
    CALimits l=limits();CAMixer *m=ca_mixer_create(&l,NULL);assert(m);CAHandle h;
    CASource *s=source(3000,137,1);assert(ca_source_attach(m,s,(CARoute){CA_BGM,0},0,&h)==CA_OK);
    CAMixer *other=ca_mixer_create(&l,NULL);CAHandle ignored;assert(ca_source_attach(other,s,(CARoute){CA_BGM,0},0,&ignored)==CA_INVALID);ca_mixer_destroy(other);
    assert(ca_source_pause(m,h,4,100)==CA_OK);assert(ca_source_resume(m,h,4,200)==CA_OK);
    assert(ca_mixer_advance(m,150)==CA_OK);Counter pos;assert(ca_source_query(m,h,&provider,query,&pos)==CA_OK&&pos.position==104);
    assert(ca_mixer_advance(m,100)==CA_OK);assert(ca_source_query(m,h,&provider,query,&pos)==CA_OK&&pos.position==154);
    CAOperation *a=operation(),*b=operation();CACommandID id;
    assert(ca_source_operation(m,h,a,300,&id)==CA_OK);assert(ca_source_operation(m,h,b,400,&id)==CA_BUSY);
    ca_operation_destroy(b);assert(ca_mixer_advance(m,100)==CA_OK);CACommandResult result;assert(ca_poll_result(m,&result)&&result.result==CA_OK&&result.revision==2);
    b=operation();assert(ca_source_operation(m,h,b,400,&id)==CA_STALE_REVISION);ca_operation_destroy(b);
    assert(ca_source_stop(m,h,4,400)==CA_OK);assert(ca_mixer_advance(m,100)==CA_OK);CAState state;assert(ca_state(m,h,&state)==CA_OK&&state==CA_RETIRED);
    CARetired *r;int count=0;while((r=ca_collect(m))){ca_dispose(r);count++;}assert(count==2);assert(ca_state(m,h,&state)==CA_STALE_HANDLE);
    assert(ca_mixer_render(m,0,NULL)==CA_OK);assert(ca_mixer_position(m)==450);ca_mixer_destroy(m);
    // Reserved sources consume slots before start and cancelled starts never sound.
    l.se_instances=1;m=ca_mixer_create(&l,NULL);s=source(1000,0,1);assert(ca_source_attach(m,s,(CARoute){CA_SE,0},200,&h)==CA_OK);
    s=source(1000,0,1);assert(ca_source_attach(m,s,(CARoute){CA_SE,0},200,&ignored)==CA_BUSY);ca_source_destroy(s);
    assert(ca_source_stop(m,h,128,100)==CA_OK);run(m,actual,900,735);for(int i=0;i<900;i++)assert(actual[i]==0);ca_mixer_destroy(m);
    // Real SE definitions create independent mutable instances and survive
    // destruction of the caller's source and definition before playback.
    ChipAudioNote note={0,1000,69,15,0,0};
    ChipAudioTrack tracks[4]={{CHIP_PULSE,&note,1},{CHIP_PULSE,NULL,0},{CHIP_TRIANGLE,NULL,0},{CHIP_NOISE,NULL,0}};
    ChipAudio *audio=chip_audio_create(tracks,1000,NULL);assert(audio);
    CASeDefinition *def=ca_se_prepare(audio,&l,NULL);assert(def);
    CASeInstance *instance=ca_se_instantiate(def,NULL);assert(instance);
    ca_se_definition_destroy(def);chip_audio_destroy(audio);
    m=ca_mixer_create(&l,NULL);assert(ca_se_trigger(m,instance,(CARoute){CA_SE,0},137,&h)==CA_OK);
    run(m,actual,900,735);for(int i=0;i<137;i++)assert(actual[i]==0);
    int nonzero=0;for(int i=137;i<900;i++)if(actual[i]!=0)nonzero++;assert(nonzero);
    ca_mixer_destroy(m);
    l=limits();
    // Replacement keeps the old tail and exposes exactly the post-gain sum.
    m=ca_mixer_create(&l,NULL);CAHandle old,next;
    assert(ca_source_attach(m,source(1000,0,1000),(CARoute){CA_BGM,0},0,&old)==CA_OK);
    assert(ca_source_replace(m,old,source(1000,0,2000),(CARoute){CA_BGM,0},4,100,&next)==CA_OK);
    double old_after[110],next_after[110];CAStem stems[2]={{.after_gain=old_after,.instance=old,.source=0},{.after_gain=next_after,.instance=next,.source=0}};
    CAOutput mix={.mix=actual,.stems=stems,.stem_count=2};assert(ca_mixer_render(m,110,&mix)==CA_OK);
    for(int i=0;i<110;i++)assert(actual[i]==old_after[i]+next_after[i]);
    assert(actual[99]==1000&&actual[100]==3000&&actual[101]==2750&&actual[104]==2000);
    s=source(1000,0,1);assert(ca_source_replace(m,next,s,(CARoute){CA_BGM,0},4,110,&h)==CA_BUSY);ca_source_destroy(s);
    CARetired *tail=ca_collect(m);assert(tail);ca_dispose(tail);assert(ca_state(m,old,&state)==CA_STALE_HANDLE);
    assert(ca_source_replace(m,next,source(1000,0,1),(CARoute){CA_BGM,0},4,110,&h)==CA_OK);ca_mixer_destroy(m);
    // Overlapping gates recover only after the final request ends. Ending the
    // weaker request must not restart the stronger request's envelope.
    m=ca_mixer_create(&l,NULL);CAHandle se1,se2,binding;
    assert(ca_source_attach(m,source(1000,0,1000),(CARoute){CA_BGM,0},0,&h)==CA_OK);
    assert(ca_source_attach(m,source(137,0,0),(CARoute){CA_SE,0},0,&se1)==CA_OK);
    assert(ca_source_attach(m,source(200,0,0),(CARoute){CA_SE,0},0,&se2)==CA_OK);
    CAGainBinding one={se1,h,0,.5,1},two={se2,h,0,.25,1};assert(ca_bind_gain(m,&one,0,&binding)==CA_OK);assert(ca_bind_gain(m,&two,0,&binding)==CA_OK);
    run(m,actual,400,127);assert(actual[137]==250&&actual[200]==250&&actual[201]==250+750./128&&actual[328]==1000);ca_mixer_destroy(m);
    // Gain automation follows output time while a paused source clock is frozen.
    m=ca_mixer_create(&l,NULL);assert(ca_source_attach(m,source(1000,0,1000),(CARoute){CA_BGM,0},0,&h)==CA_OK);
    assert(ca_source_pause(m,h,0,10)==CA_OK);assert(ca_gain(m,(CAGainTarget){.scope=CA_TRACK_GAIN,.instance=h,.source=0},0,4,10)==CA_OK);
    assert(ca_source_resume(m,h,0,30)==CA_OK);run(m,actual,40,1);assert(actual[9]==1000&&actual[30]==0);ca_mixer_destroy(m);
    printf("mixer: scheduled attachment, 137 boundary gates/chunks, loop source, pause/resume, ownership, edit lane/results, cancellation: PASS (%d contexts disposed)\n",disposed);
    return 0;
}
