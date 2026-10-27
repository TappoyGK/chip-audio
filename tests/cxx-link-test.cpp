#include "chip-audio.h"
#include "chip-drums.h"
#include <cassert>
int main() {
    ChipAudioNote note{};
    note.duration=4096; note.pitch=60; note.volume=10;
    ChipAudioTrack tracks[4]={{CHIP_PULSE,&note,1},{CHIP_PULSE,nullptr,0},{CHIP_TRIANGLE,nullptr,0},{CHIP_NOISE,nullptr,0}};
    ChipAudio *audio=chip_audio_create(tracks,4096,nullptr);
    assert(audio);
    int16_t pcm[512]; chip_audio_render(audio,pcm,512,15);
    assert(chip_audio_position(audio)==512);
    bool audible=false; for(auto sample:pcm) audible|=sample!=0; assert(audible);
    chip_audio_destroy(audio);
    auto player=chip_audio_player_create(CHIP_AUDIO_PLAYER_DEFAULT_SLOTS,nullptr);
    assert(player && chip_audio_player_finished(player));
    audio=chip_audio_create(tracks,4096,nullptr);
    assert(chip_audio_player_switch(player,audio,CHIP_AUDIO_PLAYER_DEFAULT_FADE)==CHIP_AUDIO_OK);
    chip_audio_player_render(player,pcm,512,15);
    assert(chip_audio_player_stop(player,128)==CHIP_AUDIO_OK);
    chip_audio_player_render(player,pcm,128,15);
    assert(chip_audio_player_finished(player));
    assert(chip_audio_player_collect(player)==audio);
    chip_audio_destroy(audio);chip_audio_player_destroy(player);
    ChipAudioGain gain{};double envelope[4],mix[4]={},input[4]={40000,40000,40000,40000};
    assert(chip_audio_gain_init(&gain,1));assert(chip_audio_gain_set(&gain,0,4));
    chip_audio_gain_render(&gain,envelope,4);chip_audio_mix_add(mix,input,envelope,4);
    chip_audio_mix_pcm(mix,pcm,4);assert(pcm[0]==32767&&pcm[3]==10000);
    audio=chip_audio_create(tracks,4096,nullptr);assert(audio);
    double voice[4];double *planes[5]={voice,nullptr,nullptr,nullptr,nullptr};
    chip_audio_render_stems(audio,planes,4);assert(chip_audio_position(audio)==4);
    chip_audio_destroy(audio);
    ChipAudioNote hits[CHIP_DRUM_MAX_SEGMENTS];
    assert(chip_drum_expand(CHIP_DRUM_SNARE,0,4096,10,hits,CHIP_DRUM_MAX_SEGMENTS)>0);
}
