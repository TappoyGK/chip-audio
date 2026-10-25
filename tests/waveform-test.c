
#include <assert.h>
#include "../src/synth.h"
enum { TEST_BLOCK = 735 };
// Original mixer, kept independent of the channel decomposition under test.
static void reference(Voice voices[4],int16_t *output,int count,unsigned mask) {
 for(int i=0;i<count;i++) {
  double mixed=0;
  for(int ch=0;ch<4;ch++) {
   Voice *v=&voices[ch];if(!v->sound)continue;
   const Sound *sound=v->sound;
   if(v->left==0) {if(v->note>=sound->count){v->sound=0;v->level=0;continue;}v->left=sound->notes[v->note].samples;}
   Note note=sound->notes[v->note];
   double frequency=440.0*pow(2.0,(note.pitch-69)/12.0),previous=v->phase;
   v->phase+=frequency/SYNTH_RATE;v->phase-=floor(v->phase);
   if(v->phase<previous){v->noise^=v->noise<<13;v->noise^=v->noise>>17;v->noise^=v->noise<<5;}
   double signal=sound->wave==0?(v->phase<0.25?1:-1):sound->wave==1?(1-4*fabs(v->phase-0.5)):((v->noise&1)?1:-1);
   double target=note.volume/15.0;if(v->note==sound->count-1&&v->left<=SYNTH_RAMP)target=0;
   v->level=v->level<target?fmin(target,v->level+1.0/SYNTH_RAMP):fmax(target,v->level-1.0/SYNTH_RAMP);
   if(mask&(1u<<ch))mixed+=signal*v->level*6000;
   if(--v->left==0)v->note++;
  }
  output[i]=(int16_t)mixed;
 }
}
int main(void) {
 Sound sounds[4]={0};
 for(int ch=0;ch<4;ch++) {
  sounds[ch].sustain_level=1;sounds[ch].wave=ch%3;sounds[ch].count=5;
  for(int n=0;n<5;n++)sounds[ch].notes[n]=(Note){40+ch*11+n,1433+n*271,(ch*5+n*7)%16};
 }
 for(unsigned mask=0;mask<16;mask++) {
  Voice old[4],split[4];memset(old,0,sizeof old);
  for(int ch=0;ch<4;ch++){synth_start(&old[ch],&sounds[ch],ch);split[ch]=old[ch];}
  for(int frame=0;frame<20;frame++) {
   int16_t a[TEST_BLOCK],b[TEST_BLOCK],parts[4][TEST_BLOCK];int16_t *p[4];
   for(int ch=0;ch<4;ch++)p[ch]=parts[ch];
   reference(old,a,TEST_BLOCK,mask);synth_render_channels(split,b,TEST_BLOCK,mask,p);
   for(int i=0;i<TEST_BLOCK;i++) {
    assert(a[i]==b[i]);int sum=0;for(int ch=0;ch<4;ch++)sum+=parts[ch][i];assert(sum==a[i]);
   }
  }
 }
 return 0;
}
