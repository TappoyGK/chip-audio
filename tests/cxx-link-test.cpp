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
    ChipAudioNote hits[CHIP_DRUM_MAX_SEGMENTS];
    assert(chip_drum_expand(CHIP_DRUM_SNARE,0,4096,10,hits,CHIP_DRUM_MAX_SEGMENTS)>0);
}
