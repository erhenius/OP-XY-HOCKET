#include <initializer_list>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "player_object_hocket.cpp"
#include "player_plans.hpp"
using namespace opxy;
static u32 stamp;
extern "C" u32 sdk_voice_ticks(){return stamp;}
struct MockTrack {
    u32 object[2]; hocket::State state; player::Controls controls;
    platform::PlayerChord records[8]; volatile u32 held;
    unsigned attacks, releases, velocities[128], played[128]; bool sounding[128];
};
static MockTrack tracks[3]={};
static void release_stock(void* obj,u32 note) {
    auto& t=*static_cast<MockTrack*>(obj);
    for(u32 i=0;i<t.held;++i) if(t.records[i].root==note && t.records[i].count) {
        for(u32 j=0;j<t.records[i].count;++j) {
            const auto& tone=t.records[i].tones[j];
            if(tone.delay<0) { assert(t.sounding[tone.pitch]); t.sounding[tone.pitch]=false; ++t.releases; }
        }
        t.records[i].count=0;
    }
}
static void advance_stock(void* obj) {
    auto& t=*static_cast<MockTrack*>(obj);
    for(u32 i=0;i<t.held;++i) for(u32 j=0;j<t.records[i].count;++j) {
        auto& tone=t.records[i].tones[j];
        if(tone.delay--==0) { assert(!t.sounding[tone.pitch] && tone.velocity>0 && tone.velocity<=127); t.sounding[tone.pitch]=true; if(t.attacks<128) t.velocities[t.attacks]=tone.velocity; ++t.played[tone.pitch]; ++t.attacks; }
    }
}
static void compact(MockTrack& t) {
    unsigned n=0;
    for(unsigned i=0;i<t.held;++i) if(t.records[i].count) t.records[n++]=t.records[i];
    t.held=n;
}
static void service_stock(void* obj) {
    advance_stock(obj);
    compact(*static_cast<MockTrack*>(obj));
}
// This fixture models the scheduler. The voice probe has a separate ABI fixture.
void sdk_hprobe_step(void* obj) { service_stock(obj); }
static unsigned fadeStarts;
void sdk_hprobe_release(u32,u32,u32 owner,u32 fade,u32) { assert(owner && fade>=1 && fade<=6); ++fadeStarts; }
void sdk_hprobe_stamp(void*,u32,u32,u32,u32,u32,u32) {}
static u32 activeProject=1, edits;
u32 platform::word(u32 address) { assert(address==0x840a0524); return activeProject; }
static void dirty() { ++edits; }
static void noop(void*){}
const void* stock_address(u32 a) {
    using namespace platform;
    if(a==0x8d585ee8) return reinterpret_cast<const void*>(&dirty);
    if(a==kMaestroRelease) return reinterpret_cast<const void*>(&release_stock);
    if(a==kMaestroStep) return reinterpret_cast<const void*>(&service_stock);
    assert(a==kMaestroReset||a==kMaestroAllOff); return reinterpret_cast<const void*>(&noop);
}
extern "C" const char hocket_player_id[]="";
namespace opxy {namespace platform{
void player_clock_pending(void* obj,bool pending) {
    auto& t=*static_cast<MockTrack*>(obj);
    player_clock_record(t.records,t.held,pending);
}
void* player_state(void* obj) {return &static_cast<MockTrack*>(obj)->state;}
player::Controls player_controls(void* obj) {return static_cast<MockTrack*>(obj)->controls;}
void* player_object(u32 n,const void* const* vt) {
    assert(vt==hocket_player()->vtable);return n<3 ? &tracks[n] : nullptr;
}
void player_output(void* obj,player::Display& d) {
    auto& t=*static_cast<MockTrack*>(obj); player_activity(t.records,t.held,d.played);
}
void play_plan(void* obj,u32 root,const player::Plan& plan) {
    auto& t=*static_cast<MockTrack*>(obj);
    player_store(t.records,t.held,root,plan,false,false,[obj](u32 r){release_stock(obj,r);});
}
}}
static void check_editor_recall() {
    sdk_hocket_clear();
    activeProject=1;
    auto* first=current_project();
    auto& a=settings(0);
    const auto start=edits;
    assert(editor_turn(0,0,2,true) && editor_turn(0,1,14,true)); // T2, 15/16
    assert(editor_turn(0,2,8,true) && editor_turn(0,3,19,true)); // T8, 500ms
    assert(editor_click(0,0) && editor_click(0,2) && editor_click(0,3));
    assert(editor_click(0,1) && editor_click(0,0)); // Grid, Shuffle
    assert(editor_turn(0,3,-48,true));
    assert(editor_click(0,3) && editor_turn(0,3,1,true));
    // Opening the capture/editor alone never changes the saved pattern.
    assert(editor_click(0,2) && a.grid.capture.record(68));
    u8 disk[hocket_settings::bytes], after[sizeof disk];
    // OS 1.2.1 passes its serialization snapshot into the save hook.
    auto* snapshot=reinterpret_cast<const void*>(0x85b7e3d4u);
    assert(sdk_hocket_save(snapshot,disk) && edits>start);
    assert(disk[0]==2 && disk[1]==17 && disk[2]==8 && disk[3]==22 && disk[4]==29);
    assert(disk[7]==143 && disk[15]==0 && disk[16]==19 && disk[17]==0 && disk[18]==0 && disk[19]==3);
    editor_init(0);
    assert(!a.grid.page && !a.grid.capture.armed());
    assert(sdk_hocket_save(first,after) && !std::memcmp(after,disk,sizeof disk));
    auto* second=reinterpret_cast<const void*>(2);
    sdk_hocket_load(second,nullptr);
    assert(sdk_hocket_save(first,after) && !std::memcmp(after,disk,sizeof disk));
    activeProject=2;
    assert(!sdk_hocket_save(first,after));
    assert(sdk_hocket_save(snapshot,after) && after[0]==0 && after[1]==3);
    assert(editor_turn(1,0,4,true));
    sdk_hocket_load(first,disk);
    activeProject=1;
    assert(sdk_hocket_save(first,after) && !std::memcmp(after,disk,sizeof disk));
    activeProject=0;
    assert(!sdk_hocket_save(snapshot,after));
    activeProject=1;
    sdk_hocket_clear();
    std::puts("PASS: encoder edits, 15/16 routing, track isolation, reselect, discard live capture and restore.");
}
static void check_timing_encoders() {
    const auto turn=hocket_player()->enum_turn;
    assert(turn);
    for (u32 slot : {2u,3u}) {
        const u32 count=slot==2?9:20;
        for (u32 old=0;old<4;++old) {
            const i32 word=opxy::platform::player_enum_turn(0,4,old?old:-1);
            const u32 index=slot==2?old*2:old*5+4;
            const i32 next=turn(slot,word,count,1);
            const u32 decoded=slot==2?hocket::timing_index<9>(next):hocket::timing_index<20>(next);
            assert(decoded==(index+1<count?index+1:count-1));
            assert(turn(slot,word,count,0)==word);
        }
        i32 value=turn(slot,0,count,-2147483647-1);
        for (u32 i=0;i<count;++i) {
            assert((slot==2?hocket::timing_index<9>(value):hocket::timing_index<20>(value))==i);
            value=turn(slot,value,count,1);
        }
        for (u32 i=count;i;--i) {
            assert((slot==2?hocket::timing_index<9>(value):hocket::timing_index<20>(value))==i-1);
            value=turn(slot,value,count,-1);
        }
    }
}
int main(){
    check_timing_encoders();
    check_editor_recall();
    tracks[1].object[1]=1;
    auto& a=tracks[0];auto& b=tracks[1];
    const auto vt=hocket_player()->vtable;
    auto on=reinterpret_cast<u32(*)(void*,u32,u32,u32,u32)>(const_cast<void*>(vt[5]));
    auto off=reinterpret_cast<void(*)(void*,u32)>(const_cast<void*>(vt[6]));
    auto tick=reinterpret_cast<void(*)(void*)>(const_cast<void*>(vt[13]));
    // The player ABI receives note-on velocity. Keep the instrument response
    // flat in this fixture and inspect stock player records on both branches.
    tracks[2].object[1]=2;
    sdk_hocket_clear();
    assert(editor_turn(0,1,7,false) && hocket::fork_threshold(settings(0).grid.fork)==64);
    settings(0).routeWords[0]=hocket_settings::Track::centre(0,2);
    settings(0).routeWords[2]=hocket_settings::Track::centre(2,3);
    settings(0).routeWords[1]=settings(0).routeWords[3]=hocket_settings::Track::centre(1,0);
    for (unsigned v:{1u,63u,64u,127u}) {
        assert(on(&a,60,0,0,v)==1);
        for (auto& t:tracks) tick(&t);
        assert(a.sounding[60] && b.sounding[60]==(v>=64) && tracks[2].sounding[60]==(v<64));
        off(&a,60);
        for (auto& t:tracks) { tick(&t); assert(!t.sounding[60]); }
    }
    a.controls.value[0]=0x60000000; assert(editor_turn(0,1,200,false) && settings(0).grid.swing==19);
    assert(editor_turn(0,1,-2147483647-1,false) && settings(0).grid.swing==-19);
    assert(editor_turn(0,1,19,false) && !settings(0).grid.swing);
    assert(editor_turn(0,1,2147483647,false) && settings(0).grid.swing==19);
    a.controls.value[0]=0x40000000; assert(editor_turn(0,1,200,false) && settings(0).grid.nudge==20);
    assert(settings(0).grid.fork==7 && settings(0).grid.swing==19);
    a=MockTrack{}; b=MockTrack{}; b.object[1]=1;
    // Capture at the adapter entry point with both tracks routed. Incoming
    // handoffs can sound on the armed track but never enter its take.
    sdk_hocket_clear(); editor_init(0); editor_init(1);
    auto& grid=settings(0).grid;
    settings(0).routeWords[1]=settings(1).routeWords[1]=hocket_settings::Track::centre(1,0);
    settings(0).routeWords[0]=0x20000000; // T2
    settings(1).routeWords[0]=0x14000000; // T1
    a.controls.value[0]=0x60000000; // Arp
    assert(editor_click(0,1) && editor_click(0,2) && grid.capture.armed());
    for (u32 n : {60u, 62u, 64u, 65u, 67u, 69u, 71u, 72u, 74u}) {
        on(&a,n,0,0,96); tick(&a);
        assert(a.sounding[n] && !b.attacks && !a.state.eventCount);
        off(&a,n); tick(&a); assert(!a.sounding[n]);
    }
    hocket::Take take={};
    assert(grid.capture.read(take) && take.count==8 && take.notes[0]==62 && take.notes[7]==74);
    assert(grid.mask()==15 && grid.pitch(1)==7);
    on(&b,45,0,0,96); tick(&b); tick(&a);
    assert(a.sounding[45] && grid.capture.read(take) && take.notes[7]==74);
    off(&b,45); tick(&a); tick(&b);
    on(&a,76,0,0,96); tick(&a);
    assert(editor_click(0,2) && !grid.capture.armed() && grid.mask()==255);
    assert(grid.pitch(0)==0 && grid.pitch(1)==1 && grid.pitch(7)==12);
    off(&a,76); tick(&a); assert(!a.sounding[76]);
    // Empty save, explicit cancel, page exit, and a held repeated pitch.
    assert(editor_click(0,2) && editor_click(0,2) && grid.mask()==255);
    assert(editor_click(0,2)); on(&a,10,0,0,96); tick(&a);
    assert(editor_click(0,3)); off(&a,10); tick(&a);
    assert(!grid.capture.armed() && grid.pitch(7)==12);
    assert(editor_click(0,2)); on(&a,40,0,0,96); tick(&a);
    assert(editor_click(0,1) && !grid.capture.armed() && grid.mask()==255);
    off(&a,40); tick(&a);
    a=MockTrack{}; b=MockTrack{}; b.object[1]=1;
    sdk_hocket_clear(); editor_init(0); editor_init(1); assert(editor_click(0,1) && editor_click(0,2));
    for (u32 n : {70u, 0u, 127u, 70u, 70u}) {
        on(&a,n,0,0,96); tick(&a); off(&a,n); tick(&a);
    }
    assert(editor_click(0,2));
    assert(grid.mask()==31 && grid.pitch(0)==0 && grid.pitch(1)==-48 && grid.pitch(2)==48);
    assert(grid.pitch(3)==0 && grid.pitch(4)==0 && a.attacks==5 && a.releases==5);
    assert(settings(1).grid.mask()==15 && settings(1).grid.pitch(1)==7);
    a.controls.value[0]=0x60000000; settings(0).flags=1;
    settings(0).routeWords[1]=hocket_settings::Track::centre(1,0);
    settings(0).routeWords[0]=0x20000000;
    on(&a,52,0,0,96);
    for(unsigned i=0;i<170;++i) {tick(&a); tick(&b);}
    off(&a,52); tick(&a); tick(&b);
    assert(b.played[4]==1 && b.played[100]==1 && b.played[52]==3);
    assert(b.attacks==b.releases);
    std::puts("PASS: capture monitoring, rolling eight, transposition, repeats, routes, key-up after keep, cancel, empty keep and bank exit.");
    a=MockTrack{}; b=MockTrack{}; b.object[1]=1;
    sdk_hocket_clear(); editor_init(0); editor_init(1);
    a.controls.value[0]=0x40000000; // Spray, between the old Note and Arp centres.
    assert(editor_turn(0,1,200,false) && settings(0).grid.nudge==20);
    assert(editor_turn(0,1,1,true) && settings(0).grid.nudge==20);
    assert(editor_turn(0,1,-12,false) && settings(0).grid.nudge==8);
    assert(editor_turn(0,1,-20,false) && settings(0).grid.nudge==0);
    // Walk every decay preset, then set one second.
    for(unsigned i=1;i<=6;++i) assert(editor_click(0,2) && settings(0).grid.tail==i%6);
    assert(editor_click(0,1) && editor_click(0,2) && settings(0).grid.capture.armed());
    assert(editor_click(0,3) && editor_click(0,1));
    settings(0).grid.pattern().changedMask=15^2; // Leading rest, then slot 2 (+7).
    settings(0).flags=9;
    settings(0).routeWords[0]=0x20000000; settings(0).routeWords[1]=(0x7fffffffu/23*3+0x7fffffffu/23/2); // T2, 1/16
    stamp=0;
    on(&a,60,0,0,96);
    for(unsigned time=0;time<1000;++time) {
        stamp=time*7629u/1200u;
        if(time==360) off(&a,60);
        if(a.held) tick(&a);
        if(b.held) tick(&b);
    }
    assert(a.played[67]==14 && b.played[67]==14 && a.releases==14 && b.releases==14);
    for(unsigned i=0;i<14;++i) {
        const unsigned at=30+60*i;
        const unsigned elapsed=at<=360?0:at*7629/1200-360*7629/1200;
        assert(a.velocities[i]==96 && b.velocities[i]==96*(3052-elapsed)/3052);
    }
    assert(fadeStarts==1 && !a.held && !b.held && !a.state.eventCount && !a.state.leaseCount);
    std::puts("PASS: six decay settings, held velocity, release through a leading rest, delayed Steps and idle cleanup.");
    for (unsigned rateIndex=0;rateIndex<4;++rateIndex) for (unsigned gateIndex=0;gateIndex<4;++gateIndex)
    for (unsigned delayIndex : {0u,3u,4u,5u}) for (unsigned pattern : {0x12u,0x11u})
    for (bool reverse : {false,true}) for (bool shuffle : {false,true}) {
        a=MockTrack{}; b=MockTrack{}; b.object[1]=1; sdk_hocket_clear(); editor_init(0); editor_init(1); stamp=0;
        auto word=[](unsigned k,unsigned n){return (0x7fffffffu/n)*k+0x7fffffffu/n/2;};
        a.controls.value[0]=word(1,2); a.controls.value[2]=word(rateIndex,4);
        a.controls.value[3]=word(gateIndex,4); a.controls.bpm_x10=b.controls.bpm_x10=1200;
        settings(0).flags=13;
        settings(0).routeWords[0]=word(2,9); settings(0).routeWords[1]=word(delayIndex,23);
        assert(editor_click(0,1) && editor_click(0,3));
        for (unsigned bank=0;bank<2;++bank) {
            for (unsigned k=0;k<4;++k)
                assert(editor_turn(0,k,(pattern&(1u<<(4*bank+k)))?1:-1,bank!=0));
        }
        if(shuffle) assert(editor_click(0,0));
        assert(editor_click(0,3) && editor_turn(0,0,12,true) && editor_click(0,1));
        const unsigned rate=hocket::rate_steps[2*rateIndex],gate=rate*(gateIndex+1)/4;
        const unsigned delay=hocket::delay_steps(delayIndex,1200);
        const unsigned first=(pattern==0x12)?rate:0, pitch=(pattern==0x12)?67:60;
        auto runTrack=[&](MockTrack& t){ if(t.held) tick(&t); };
        on(&a,60,0,0,96);
        const unsigned releaseAt=10*rate;
        unsigned firstPitch[2]={pitch,pitch};
        for(unsigned time=0;time<releaseAt+delay+gate+5;++time) {
            stamp=time*7629u/1200u;
            if(time==releaseAt) off(&a,60);
            if(reverse){runTrack(b);runTrack(a);} else {runTrack(a);runTrack(b);}
            for(unsigned pass=0;pass<2;++pass) {
                const unsigned offset=pass*5*rate;
                if(time==offset+first+1) {
                    assert(a.sounding[pitch]!=a.sounding[72]);
                    firstPitch[pass]=a.sounding[pitch]?pitch:72;
                    if(!shuffle) assert(firstPitch[pass]==pitch);
                }
                const unsigned secondPitch=firstPitch[pass]==pitch?72:pitch;
                if(time==offset+4*rate+1) assert(a.sounding[secondPitch]);
                if(time==offset+first+delay+1) assert(b.sounding[firstPitch[pass]]);
                if(time==offset+4*rate+delay+1) assert(b.sounding[secondPitch]);
                if(time==offset+first+gate+1) assert(!a.sounding[firstPitch[pass]]);
                if(time==offset+first+delay+gate+1) assert(!b.sounding[firstPitch[pass]]);
            }
        }
        assert(a.attacks==4 && b.attacks==4 && a.attacks==a.releases && b.attacks==b.releases);
        assert(!a.state.eventCount && !a.state.leaseCount && !a.held && !b.held);
        for(unsigned i=0;i<4;++i) assert(a.velocities[i]==96 && b.velocities[i]==96);
    }
    // A rest reserves no pitch or polyphony. Eight real roots can reuse all
    // eight stock records, including MIDI 0, and receive exactly one note-off.
    a=MockTrack{}; sdk_hocket_clear(); editor_init(0); stamp=0;
    platform::player_clock_pending(&a,true);
    assert(a.held==1 && !a.records[0].count);
    for(u32 n=0;n<8;++n) on(&a,n,0,0,96);
    assert(a.held==8); tick(&a);
    assert(a.attacks==8);
    for(u32 n=0;n<8;++n) off(&a,n);
    tick(&a);
    assert(!a.held && a.releases==8);
    std::puts("PASS: 512 held-key routing cases across Forward and Shuffle and eight-note capacity with a pending silent clock record.");
}
