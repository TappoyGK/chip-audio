#include "chip-source.h"
#include "chip-audio.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#define BLOCK 256
#define NONE UINT32_MAX
struct CARetired { struct CARetired *next; void (*dispose)(struct CARetired *); };
struct CASource {
    CARetired retired; void *context; CASourceOps ops; const void *provider;
    CAMixer *owner;
};
struct CAOperation {
    CARetired retired; CAOperationOps ops; void *payload; CAMixer *owner;
};
typedef struct {
    CASource *source; uint64_t generation; CAState state; CARoute route;
    ChipAudioGain transition, gain[5], gate[5]; int gate_ramp[5];
} Slot;
typedef struct {
    CAGainBinding spec; uint64_t generation; int used, installed;
} Binding;
typedef enum { START, REPLACE, STOP, PAUSE, RESUME, GAIN, BIND, UNBIND, OP } Kind;
typedef struct {
    Kind kind; int used; int64_t at; uint64_t serial;
    CAHandle handle, other; int fade; CAGainTarget target; double value;
    uint32_t binding, result; CAOperation *operation;
} Command;
typedef struct {
    int used, ready; CACommandResult value; uint32_t lane;
} Result;
struct CAMixer {
    CALimits limits; int64_t clock; uint64_t serial; uint32_t slots_count;
    Slot *slots; Command *commands; Binding *bindings; Result *results;
    size_t reservations; CARetired *retired_head,*retired_tail;
    ChipAudioGain master,groups[3];
    double scratch[5][BLOCK],sum[BLOCK], master_values[BLOCK],group_values[3][BLOCK];
};
static void source_dispose(CARetired *r) { CASource *s=(CASource *)r;s->ops.dispose(s->context);free(s); }
static void operation_dispose(CARetired *r) { CAOperation *o=(CAOperation *)r;o->ops.dispose(o->payload);free(o); }
CASource *ca_source_prepare(void *context,const CASourceOps *ops,const CALimits *limits,CAResult *error) {
    if(error)*error=CA_INVALID;
    if(!context||!ops||!ops->settle||!ops->render||!ops->dispose||!limits)return NULL;
    if(limits->allocated_bytes<sizeof(CASource)){if(error)*error=CA_LIMIT;return NULL;}
    CASource *s=calloc(1,sizeof(*s));if(!s){if(error)*error=CA_MEMORY;return NULL;}
    s->retired.dispose=source_dispose;s->context=context;s->ops=*ops;
    if(error)*error=CA_OK;return s;
}
void ca_source_destroy(CASource *s){if(s&&!s->owner)source_dispose(&s->retired);}
CAResult ca_source_bind_provider(CASource *s,const void *provider){if(!s||s->owner||!provider)return CA_INVALID;s->provider=provider;return CA_OK;}
CAOperation *ca_operation_prepare(const CAOperationOps *ops,void *payload,CAResult *error){
    if(error)*error=CA_INVALID;
    if(!ops||!ops->provider||!ops->check||!ops->apply||!ops->dispose||!payload)return NULL;
    CAOperation *o=calloc(1,sizeof(*o));if(!o){if(error)*error=CA_MEMORY;return NULL;}
    o->retired.dispose=operation_dispose;o->ops=*ops;o->payload=payload;if(error)*error=CA_OK;return o;
}
void ca_operation_destroy(CAOperation *o){if(o&&!o->owner)operation_dispose(&o->retired);}
static int budget_add(size_t *bytes,size_t n,size_t size,size_t max){
    if(n>SIZE_MAX/size||n*size>max||*bytes>max-n*size)return 0;*bytes+=n*size;return 1;
}
CAMixer *ca_mixer_create(const CALimits *l,CAResult *error){
    if(error)*error=CA_INVALID;
    if(!l||l->se_instances>1024||l->commands>65536||l->gain_bindings>65536||l->command_results>65536)return NULL;
    size_t bytes=sizeof(CAMixer);
    if(bytes>l->allocated_bytes||!budget_add(&bytes,(size_t)l->se_instances+2,sizeof(Slot),l->allocated_bytes)||
       !budget_add(&bytes,l->commands?l->commands:1,sizeof(Command),l->allocated_bytes)||
       !budget_add(&bytes,l->gain_bindings?l->gain_bindings:1,sizeof(Binding),l->allocated_bytes)||
       !budget_add(&bytes,l->command_results?l->command_results:1,sizeof(Result),l->allocated_bytes)){if(error)*error=CA_LIMIT;return NULL;}
    CAMixer *m=calloc(1,sizeof(*m));if(!m){if(error)*error=CA_MEMORY;return NULL;}
    m->limits=*l;m->slots_count=l->se_instances+2;
    m->slots=calloc(m->slots_count,sizeof(Slot));m->commands=calloc(l->commands?l->commands:1,sizeof(Command));
    m->bindings=calloc(l->gain_bindings?l->gain_bindings:1,sizeof(Binding));m->results=calloc(l->command_results?l->command_results:1,sizeof(Result));
    if(!m->slots||!m->commands||!m->bindings||!m->results){ca_mixer_destroy(m);if(error)*error=CA_MEMORY;return NULL;}
    chip_audio_gain_init(&m->master,1);for(int i=0;i<3;i++)chip_audio_gain_init(&m->groups[i],1);
    if(error)*error=CA_OK;return m;
}
static void retire(CAMixer *m,CARetired *r){r->next=NULL;if(m->retired_tail)m->retired_tail->next=r;else m->retired_head=r;m->retired_tail=r;}
static int same(CAHandle a,CAHandle b){return a.slot==b.slot&&a.generation==b.generation;}
static Slot *slot(CAMixer *m,CAHandle h){
    return m&&h.slot<m->slots_count&&h.generation&&m->slots[h.slot].source&&m->slots[h.slot].generation==h.generation?&m->slots[h.slot]:NULL;
}
static void end_source(CAMixer *m,Slot *s){if(s->state!=CA_RETIRED){s->state=CA_RETIRED;retire(m,&s->source->retired);}}
CARetired *ca_collect(CAMixer *m){
    if(!m||!m->retired_head)return NULL;
    CARetired *r=m->retired_head;m->retired_head=r->next;if(!m->retired_head)m->retired_tail=NULL;r->next=NULL;
    for(uint32_t i=0;i<m->slots_count;i++)if(m->slots[i].source&& &m->slots[i].source->retired==r){m->slots[i].source->owner=NULL;m->slots[i].source=NULL;}
    if(r->dispose==operation_dispose)((CAOperation *)r)->owner=NULL;
    m->reservations--;return r;
}
void ca_dispose(CARetired *r){if(r)r->dispose(r);}
void ca_mixer_destroy(CAMixer *m){
    if(!m)return;
    if(m->commands)for(uint32_t i=0;i<m->limits.commands;i++)if(m->commands[i].used&&m->commands[i].operation){CAOperation *o=m->commands[i].operation;o->owner=NULL;ca_operation_destroy(o);}
    if(m->slots)for(uint32_t i=0;i<m->slots_count;i++)if(m->slots[i].source&&m->slots[i].state!=CA_RETIRED){m->slots[i].source->owner=NULL;ca_source_destroy(m->slots[i].source);}
    CARetired *r=m->retired_head;while(r){CARetired *next=r->next;ca_dispose(r);r=next;}
    free(m->slots);free(m->commands);free(m->bindings);free(m->results);free(m);
}
int64_t ca_mixer_position(const CAMixer *m){return m?m->clock:0;}
CAResult ca_state(const CAMixer *m,CAHandle h,CAState *out){Slot *s=slot((CAMixer *)m,h);if(!s)return CA_STALE_HANDLE;if(!out)return CA_INVALID;*out=s->state;return CA_OK;}
static CAResult room(CAMixer *m,int64_t at,uint32_t *index){
    if(!m||at<0)return CA_INVALID;if(at<m->clock)return CA_PAST;if(m->serial==UINT64_MAX)return CA_OVERFLOW;
    for(uint32_t i=0;i<m->limits.commands;i++)if(!m->commands[i].used){*index=i;return CA_OK;}return CA_BUSY;
}
static void enqueue(CAMixer *m,uint32_t i,Command c){c.used=1;c.serial=++m->serial;m->commands[i]=c;}
static int valid_fade(int64_t fade){return fade>=0&&fade<=44100;}
static CAResult attach(CAMixer *m,CASource *s,CARoute route,int64_t at,CAHandle old,int fade,int replace,CAHandle *out){
    if(!s||s->owner||!out||route.group<CA_BGM||route.group>CA_UI||!valid_fade(fade))return CA_INVALID;
    uint32_t ci;CAResult e=room(m,at,&ci);if(e)return e;
    Slot *prior=replace?slot(m,old):NULL;
    if(replace&&(!prior||prior->state==CA_RETIRED))return CA_STALE_HANDLE;
    if(m->reservations>=m->limits.retired_objects)return CA_BUSY;
    uint32_t begin=route.group==CA_BGM?0:2,end=route.group==CA_BGM?2:m->slots_count;
    uint32_t i;for(i=begin;i<end;i++)if(!m->slots[i].source&&m->slots[i].generation<UINT64_MAX)break;
    if(i==end)return CA_BUSY;
    Slot *v=&m->slots[i];uint64_t generation=v->generation+1;memset(v,0,sizeof(*v));v->generation=generation;
    v->source=s;v->state=CA_PENDING;v->route=route;s->owner=m;
    chip_audio_gain_init(&v->transition,1);
    for(int p=0;p<5;p++){chip_audio_gain_init(&v->gain[p],1);chip_audio_gain_init(&v->gate[p],1);v->gate_ramp[p]=128;}
    CAHandle h={i,generation};m->reservations++;
    enqueue(m,ci,(Command){.kind=replace?REPLACE:START,.at=at,.handle=h,.other=old,.fade=fade});*out=h;return CA_OK;
}
CAResult ca_source_attach(CAMixer *m,CASource *s,CARoute r,int64_t at,CAHandle *out){return attach(m,s,r,at,(CAHandle){0},0,0,out);}
CAResult ca_source_replace(CAMixer *m,CAHandle old,CASource *s,CARoute r,int64_t fade,int64_t at,CAHandle *out){if(!valid_fade(fade))return CA_INVALID;return attach(m,s,r,at,old,(int)fade,1,out);}
static CAResult control(CAMixer *m,CAHandle h,int64_t fade,int64_t at,Kind kind){
    if(!valid_fade(fade))return CA_INVALID;uint32_t i;CAResult e=room(m,at,&i);if(e)return e;
    Slot *s=slot(m,h);if(!s||s->state==CA_RETIRED)return CA_STALE_HANDLE;
    enqueue(m,i,(Command){.kind=kind,.handle=h,.at=at,.fade=(int)fade});return CA_OK;
}
CAResult ca_source_stop(CAMixer *m,CAHandle h,int64_t f,int64_t at){return control(m,h,f,at,STOP);}
CAResult ca_source_pause(CAMixer *m,CAHandle h,int64_t f,int64_t at){return control(m,h,f,at,PAUSE);}
CAResult ca_source_resume(CAMixer *m,CAHandle h,int64_t f,int64_t at){return control(m,h,f,at,RESUME);}
static ChipAudioGain *target_gain(CAMixer *m,CAGainTarget t){
    if(t.scope==CA_MASTER_GAIN)return &m->master;
    if(t.scope==CA_GROUP_GAIN&&t.group>=CA_BGM&&t.group<=CA_UI)return &m->groups[t.group];
    Slot *s=slot(m,t.instance);return t.scope==CA_TRACK_GAIN&&s&&s->state!=CA_RETIRED&&t.source<5?&s->gain[t.source]:NULL;
}
CAResult ca_gain(CAMixer *m,CAGainTarget t,double value,int64_t fade,int64_t at){
    if(!isfinite(value)||value<0||value>2||!valid_fade(fade))return CA_INVALID;
    uint32_t i;CAResult e=room(m,at,&i);if(e)return e;if(!target_gain(m,t))return CA_INVALID;
    enqueue(m,i,(Command){.kind=GAIN,.target=t,.value=value,.fade=(int)fade,.at=at});return CA_OK;
}
CAResult ca_gate_ramp(CAMixer *m,CAHandle h,uint32_t source,int64_t ramp){
    Slot *s=slot(m,h);if(!s||s->state==CA_RETIRED||source>=5||!valid_fade(ramp))return CA_INVALID;
    for(uint32_t i=0;i<m->limits.gain_bindings;i++)if(m->bindings[i].used&&same(m->bindings[i].spec.target,h)&&m->bindings[i].spec.target_source==source)return CA_BUSY;
    s->gate_ramp[source]=(int)ramp;return CA_OK;
}
CAResult ca_bind_gain(CAMixer *m,const CAGainBinding *spec,int64_t at,CAHandle *out){
    if(!spec||!out||!isfinite(spec->attenuation)||spec->attenuation<0||spec->attenuation>1||spec->target_source>=5||(spec->include_tail!=0&&spec->include_tail!=1))return CA_INVALID;
    uint32_t ci;CAResult e=room(m,at,&ci);if(e)return e;
    Slot *a=slot(m,spec->trigger),*b=slot(m,spec->target);if(!a||!b||a->state==CA_RETIRED||b->state==CA_RETIRED)return CA_STALE_HANDLE;
    uint32_t i;for(i=0;i<m->limits.gain_bindings;i++)if(!m->bindings[i].used&&m->bindings[i].generation<UINT64_MAX)break;if(i==m->limits.gain_bindings)return CA_BUSY;
    Binding *v=&m->bindings[i];v->spec=*spec;v->used=1;v->installed=0;v->generation++;
    CAHandle h={i,v->generation};enqueue(m,ci,(Command){.kind=BIND,.handle=h,.at=at});*out=h;return CA_OK;
}
CAResult ca_unbind_gain(CAMixer *m,CAHandle h,int64_t at){
    uint32_t ci;CAResult e=room(m,at,&ci);if(e)return e;
    if(h.slot>=m->limits.gain_bindings||!m->bindings[h.slot].used||m->bindings[h.slot].generation!=h.generation)return CA_STALE_HANDLE;
    enqueue(m,ci,(Command){.kind=UNBIND,.handle=h,.at=at});return CA_OK;
}
CAResult ca_source_operation(CAMixer *m,CAHandle h,CAOperation *o,int64_t at,CACommandID *out){
    if(!o||o->owner||!out)return CA_INVALID;uint32_t ci;CAResult e=room(m,at,&ci);if(e)return e;
    Slot *s=slot(m,h);if(!s||s->state==CA_RETIRED||s->state==CA_FADING)return CA_STALE_HANDLE;
    if(s->source->provider!=o->ops.provider)return CA_INVALID;
    e=o->ops.check(s->source->context,o->payload);if(e)return e;
    uint32_t ri=NONE;
    for(uint32_t i=0;i<m->limits.command_results;i++){
        Result *r=&m->results[i];if(!r->used&&ri==NONE)ri=i;
        if(r->used&&o->ops.exclusive_lane&&r->lane==o->ops.exclusive_lane&&same(r->value.target,h))return CA_BUSY;
    }
    if(ri==NONE||m->reservations>=m->limits.retired_objects)return CA_BUSY;
    o->owner=m;m->reservations++;
    enqueue(m,ci,(Command){.kind=OP,.handle=h,.at=at,.operation=o,.result=ri});
    *out=m->serial;m->results[ri]=(Result){.used=1,.lane=o->ops.exclusive_lane,.value={.command=*out,.target=h,.at=at}};return CA_OK;
}
CAResult ca_source_query(const CAMixer *m,CAHandle h,const void *provider,void (*query)(const void *,void *),void *out){
    Slot *s=slot((CAMixer *)m,h);if(!s||s->state==CA_RETIRED)return CA_STALE_HANDLE;
    if(!provider||s->source->provider!=provider||!query||!out)return CA_INVALID;query(s->source->context,out);return CA_OK;
}
int ca_poll_result(CAMixer *m,CACommandResult *out){
    if(!m||!out)return 0;Result *best=NULL;
    for(uint32_t i=0;i<m->limits.command_results;i++){Result *r=&m->results[i];if(r->used&&r->ready&&(!best||r->value.at<best->value.at||(r->value.at==best->value.at&&r->value.command<best->value.command)))best=r;}
    if(!best)return 0;*out=best->value;best->used=0;return 1;
}
static void stop_now(CAMixer *m,Slot *s,int fade){
    if(!s||s->state==CA_RETIRED||s->state==CA_FADING)return;
    if(s->state==CA_PENDING||s->state==CA_PAUSED||!fade){end_source(m,s);return;}
    s->state=CA_FADING;chip_audio_gain_set(&s->transition,0,fade);
}
static void execute(CAMixer *m,Command *c){
    Slot *s=slot(m,c->handle);
    if(c->kind==OP){
        CAOperation *o=c->operation;Result *r=&m->results[c->result];
        CAResult e=(!s||s->state==CA_RETIRED||s->state==CA_FADING)?CA_CANCELLED:o->ops.check(s->source->context,o->payload);
        if(e==CA_OK)o->ops.apply(s->source->context,o->payload);
        r->value.result=e;r->value.revision=s&&s->state!=CA_RETIRED&&o->ops.revision?o->ops.revision(s->source->context):0;r->ready=1;
        retire(m,&o->retired);return;
    }
    if(c->kind==BIND||c->kind==UNBIND){
        if(c->handle.slot<m->limits.gain_bindings){Binding *b=&m->bindings[c->handle.slot];if(b->used&&b->generation==c->handle.generation){if(c->kind==BIND)b->installed=1;else b->used=0;}}return;
    }
    if(c->kind==GAIN){ChipAudioGain *g=target_gain(m,c->target);if(g)chip_audio_gain_set(g,c->value,c->fade);return;}
    if(!s||s->state==CA_RETIRED)return;
    switch(c->kind){
    case START:case REPLACE:
        if(s->state==CA_PENDING){if(c->kind==REPLACE)stop_now(m,slot(m,c->other),c->fade);s->state=CA_PLAYING;}break;
    case STOP:stop_now(m,s,c->fade);break;
    case PAUSE:
        if(s->state==CA_PLAYING){chip_audio_gain_set(&s->transition,0,c->fade);s->state=c->fade?CA_PAUSING:CA_PAUSED;}break;
    case RESUME:
        if(s->state==CA_PAUSED||s->state==CA_PAUSING){chip_audio_gain_set(&s->transition,1,c->fade);s->state=CA_PLAYING;}break;
    default:break;
    }
}
static int audible_state(CAState state,int tails){return state==CA_PLAYING||state==CA_PAUSING||(tails&&state==CA_FADING);}
static void update_gates(CAMixer *m){
    for(uint32_t i=0;i<m->limits.gain_bindings;i++)if(m->bindings[i].used){Binding *b=&m->bindings[i];Slot *a=slot(m,b->spec.trigger),*t=slot(m,b->spec.target);if(!a||!t||a->state==CA_RETIRED||t->state==CA_RETIRED)b->used=0;}
    for(uint32_t i=0;i<m->slots_count;i++)if(m->slots[i].source&&m->slots[i].state!=CA_RETIRED){
        Slot *s=&m->slots[i];CAHandle h={i,s->generation};double targets[5]={1,1,1,1,1};
        for(uint32_t j=0;j<m->limits.gain_bindings;j++){
            Binding *b=&m->bindings[j];if(!b->used||!b->installed||!same(b->spec.target,h))continue;
            Slot *a=slot(m,b->spec.trigger);if(a&&audible_state(a->state,b->spec.include_tail)&&b->spec.attenuation<targets[b->spec.target_source])targets[b->spec.target_source]=b->spec.attenuation;
        }
        for(int p=0;p<5;p++)if(s->gate[p].target!=targets[p])chip_audio_gain_set(&s->gate[p],targets[p],s->gate_ramp[p]);
    }
}
CAResult ca_mixer_render(CAMixer *m,size_t frames,const CAOutput *out){
    if(!m)return CA_INVALID;if(frames>(uint64_t)(INT64_MAX-m->clock))return CA_OVERFLOW;
    if(out&&out->stem_count&&!out->stems)return CA_INVALID;
    if(out)for(size_t i=0;i<out->stem_count;i++)if(out->stems[i].source>=5)return CA_INVALID;
    size_t offset=0;
    while(offset<frames){
        for(;;){Command *best=NULL;for(uint32_t i=0;i<m->limits.commands;i++){Command *c=&m->commands[i];if(c->used&&c->at==m->clock&&(!best||c->serial<best->serial))best=c;}if(!best)break;execute(m,best);best->used=0;}
        size_t n=frames-offset;if(n>BLOCK)n=BLOCK;
        for(uint32_t i=0;i<m->limits.commands;i++){Command *c=&m->commands[i];if(c->used&&c->at>m->clock&&(uint64_t)(c->at-m->clock)<n)n=(size_t)(c->at-m->clock);}
        for(uint32_t i=0;i<m->slots_count;i++){
            Slot *s=&m->slots[i];if(!s->source||s->state==CA_PENDING||s->state==CA_RETIRED||s->state==CA_PAUSED)continue;
            if((s->state==CA_FADING||s->state==CA_PAUSING)&&s->transition.elapsed==s->transition.total){if(s->state==CA_FADING)end_source(m,s);else s->state=CA_PAUSED;continue;}
            int64_t available=s->source->ops.settle(s->source->context);
            if(available<=0){end_source(m,s);continue;}if((uint64_t)available<n)n=(size_t)available;
            if(s->state==CA_FADING||s->state==CA_PAUSING){size_t left=(size_t)(s->transition.total-s->transition.elapsed);if(left<n)n=left;}
        }
        update_gates(m);memset(m->sum,0,n*sizeof(double));
        chip_audio_gain_render(&m->master,m->master_values,n);for(int g=0;g<3;g++)chip_audio_gain_render(&m->groups[g],m->group_values[g],n);
        if(out)for(size_t i=0;i<out->stem_count;i++){CAStem *t=&out->stems[i];if(t->before_gain)memset(t->before_gain+offset,0,n*sizeof(double));if(t->after_gain)memset(t->after_gain+offset,0,n*sizeof(double));}
        for(uint32_t i=0;i<m->slots_count;i++){
            Slot *s=&m->slots[i];if(!s->source||s->state==CA_RETIRED)continue;
            if(!audible_state(s->state,1)){for(int p=0;p<5;p++){chip_audio_gain_render(&s->gain[p],NULL,n);chip_audio_gain_render(&s->gate[p],NULL,n);}continue;}
            double *planes[5]={m->scratch[0],m->scratch[1],m->scratch[2],m->scratch[3],m->scratch[4]};
            s->source->ops.render(s->source->context,planes,n);
            double transition[BLOCK];chip_audio_gain_render(&s->transition,transition,n);
            for(int p=0;p<5;p++){
                double gain[BLOCK],gate[BLOCK];chip_audio_gain_render(&s->gain[p],gain,n);chip_audio_gain_render(&s->gate[p],gate,n);
                for(size_t j=0;j<n;j++){
                    double raw=planes[p][j],value=raw*gain[j]*gate[j]*transition[j]*m->group_values[s->route.group][j]*m->master_values[j];m->sum[j]+=value;
                    if(out)for(size_t k=0;k<out->stem_count;k++){CAStem *t=&out->stems[k];if(t->instance.slot==i&&t->instance.generation==s->generation&&t->source==(uint32_t)p){if(t->before_gain)t->before_gain[offset+j]=raw;if(t->after_gain)t->after_gain[offset+j]=value;}}
                }
            }
        }
        if(out){if(out->mix)memcpy(out->mix+offset,m->sum,n*sizeof(double));if(out->pcm)chip_audio_mix_pcm(m->sum,out->pcm+offset,n);}
        m->clock+=(int64_t)n;offset+=n;
    }
    return CA_OK;
}
CAResult ca_mixer_advance(CAMixer *m,size_t frames){return ca_mixer_render(m,frames,NULL);}

struct CASeDefinition {ChipAudio *snapshot; CALimits limits;};
static int64_t se_settle(void *ctx){ChipAudio *a=ctx;return chip_audio_length(a)-chip_audio_position(a);}
static void se_render(void *ctx,double *planes[5],size_t n){chip_audio_render_stems(ctx,planes,n);}
static void se_dispose(void *ctx){chip_audio_destroy(ctx);}
CASeDefinition *ca_se_prepare(const ChipAudio *audio,const CALimits *l,CAResult *error){
    if(error)*error=CA_INVALID;if(!audio||!l)return NULL;
    size_t bytes=chip_audio_clone_size(audio);if(bytes==SIZE_MAX)return NULL;
    if(bytes>l->allocated_bytes||l->allocated_bytes-bytes<sizeof(CASeDefinition)){if(error)*error=CA_LIMIT;return NULL;}
    CASeDefinition *d=calloc(1,sizeof(*d));if(!d){if(error)*error=CA_MEMORY;return NULL;}
    d->snapshot=chip_audio_clone(audio,bytes,NULL);if(!d->snapshot){free(d);if(error)*error=CA_MEMORY;return NULL;}
    d->limits=*l;if(error)*error=CA_OK;return d;
}
void ca_se_definition_destroy(CASeDefinition *d){if(d){chip_audio_destroy(d->snapshot);free(d);}}
CASeInstance *ca_se_instantiate(const CASeDefinition *d,CAResult *error){
    if(error)*error=CA_INVALID;if(!d)return NULL;
    size_t bytes=chip_audio_clone_size(d->snapshot);
    if(bytes>d->limits.allocated_bytes||d->limits.allocated_bytes-bytes<sizeof(CASource)){if(error)*error=CA_LIMIT;return NULL;}
    ChipAudio *a=chip_audio_clone(d->snapshot,bytes,NULL);if(!a){if(error)*error=CA_MEMORY;return NULL;}
    CASourceOps ops={se_settle,se_render,se_dispose};CASource *s=ca_source_prepare(a,&ops,&d->limits,error);
    if(!s)chip_audio_destroy(a);return (CASeInstance *)s;
}
void ca_se_instance_destroy(CASeInstance *s){ca_source_destroy((CASource *)s);}
CAResult ca_se_trigger(CAMixer *m,CASeInstance *s,CARoute r,int64_t at,CAHandle *out){if(r.group==CA_BGM)return CA_INVALID;return ca_source_attach(m,(CASource *)s,r,at,out);}
CAResult ca_se_replace(CAMixer *m,CAHandle h,CASeInstance *s,CARoute r,int64_t fade,int64_t at,CAHandle *out){Slot *old=slot(m,h);if(!old)return CA_STALE_HANDLE;if(old->route.group==CA_BGM||r.group==CA_BGM)return CA_INVALID;return ca_source_replace(m,h,(CASource *)s,r,fade,at,out);}
CAResult ca_se_stop(CAMixer *m,CAHandle h,int64_t fade,int64_t at){Slot *old=slot(m,h);if(!old)return CA_STALE_HANDLE;if(old->route.group==CA_BGM)return CA_INVALID;return ca_source_stop(m,h,fade,at);}

CAResult ca_group_current(const CAMixer *m,CAGroup group,CAHandle *out){
    if(!m||!out||group<CA_BGM||group>CA_UI)return CA_INVALID;
    const Slot *found=NULL;uint32_t index=0;
    for(uint32_t i=0;i<m->slots_count;i++){const Slot *s=&m->slots[i];if(s->source&&s->route.group==group&&s->state!=CA_RETIRED&&s->state!=CA_FADING){if(found)return CA_BUSY;found=s;index=i;}}
    if(!found)return CA_STALE_HANDLE;*out=(CAHandle){index,found->generation};return CA_OK;
}
