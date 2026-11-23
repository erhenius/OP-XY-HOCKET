#include "../player.cpp"
#include <cassert>
#include <vector>
using namespace hocket;
struct Hit { unsigned now,track,pitch,vel; bool on; };
struct Rig {
    State states[8]={};
    u32 sequences[8]={};
    Grid grids[8]={};
    Controls knobs[8]={};
    bool installed[8]={};
    unsigned time=0, stampOffset=0, tempo=1200;
    std::vector<Hit> hits;
    struct Port {
        Rig& rig; unsigned id;
        u32 track() const { return id; }
        u32 time_stamp() const { return rig.stampOffset+static_cast<u32>(static_cast<unsigned long long>(rig.time)*7629/rig.tempo); }
        bool valid() const { return rig.installed[id]; }
        State& memory() const { return rig.states[id]; }
        Grid& grid() const {return rig.grids[id];}
        Controls controls() const { return rig.knobs[id]; }
        Port at(u32 t) const { return {rig,t}; }
        u32& sequence() const { return rig.sequences[id]; }
        void release_spray(u32,u32,u32,u32) {}
        void on(u32 n,u8 v,u32,u32,u32,u32) { rig.hits.push_back({rig.time,id,n,v,true}); }
        void off(u32 n) { rig.hits.push_back({rig.time,id,n,0,false}); }
    };
    static i32 word(u32 i,u32 n) { return (0x7fffffffu/n)*i+(0x7fffffffu/n)/2; }
    Rig() {
        for (u32 i=0;i<8;++i) { installed[i]=true; knobs[i].bpm_x10=1200;
            knobs[i].value[2]=word(1,4); knobs[i].value[3]=word(1,4); }
    }
    void arp(u32 t){knobs[t].value[0]=word(1,2);}
    void route(u32 t,u32 branch,u32 dest,u32 delay) {
        knobs[t].shift_value[2*branch]=word(dest+1,9);
        knobs[t].shift_value[2*branch+1]=word(delay,delayChoices);
    }
    void press(u32 track=0,u8 n=60,u8 velocity=96) {Port p={*this,track}; press_key(states[track],p,n,velocity);}
    void lift(u32 track=0,u8 n=60) {Port p={*this,track}; release_key(states[track],p,n);}
    void run(u32 ticks,u32 enabled=255) {
        while(ticks--) {for(u32 i=0;i<8;++i) if(installed[i] && (enabled&(1u<<i))) {
            Port p={*this,i}; tick(states[i],p);
        } ++time;}
    }
    u32 count(u32 t,u32 pitch,u32 at,bool on=true) const {
        u32 n=0;
        for(const Hit& h:hits) if(h.track==t && h.pitch==pitch && h.now==at && h.on==on) ++n;
        return n;
    }
};
static void check_shuffle_masks() {
    bool heardNewOrder=false;
    for (u32 mask=0;mask<256;++mask) for (u32 rests=0;rests<2;++rests) {
        Rig r; r.arp(0); r.knobs[0].clicks[0]=1; r.knobs[0].clicks[2]=1;
        r.knobs[0].clicks[3]=rests; r.knobs[0].value[3]=Rig::word(0,4);
        r.route(0,0,1,3); r.route(1,0,2,0);
        Grid& grid=r.grids[0];
        grid.pattern().changedMask=15^mask;
        grid.click(1); grid.click(0); grid.click(1);
        assert(grid.shuffle && !grid.page);
        std::vector<u32> positions;
        for(u32 slot=0;slot<8;++slot) {
            grid.pattern().offsets[slot]=static_cast<i32>(slot)-intervals[slot];
            if(mask&(1u<<slot)) positions.push_back(slot);
        }
        const u32 count=positions.size();
        const u32 length=count && rests ? positions.back()+1 : count;
        r.press(); r.run(2*length*60); r.lift(); r.run(180);
        u32 firstPass=0;
        for(u32 pass=0;pass<2;++pass) {
            u32 seen=0,order=0;
            for(u32 j=0;j<count;++j) {
                const u32 when=60*(pass*length+(rests?positions[j]:j));
                u32 attacks=0;
                for(u32 pitchSlot:positions) {
                    const u32 pitch=60+pitchSlot;
                    if(!r.count(0,pitch,when)) continue;
                    assert(!(seen&(1u<<pitchSlot)));
                    seen|=1u<<pitchSlot; order=(order<<4)|pitchSlot; ++attacks;
                    assert(r.count(0,pitch,when+15,false)==1);
                    for(u32 dest : {1u,2u}) {
                        assert(r.count(dest,pitch,when+60)==1);
                        assert(r.count(dest,pitch,when+75,false)==1);
                    }
                }
                assert(attacks==1);
            }
            assert(seen==mask);
            if(!pass) firstPass=order;
            else if(firstPass!=order) heardNewOrder=true;
        }
        assert(r.hits.size()==12*count);
    }
    assert(heardNewOrder);
}

static void check_receiving_shuffle() {
    Rig r; r.arp(0); r.arp(1);
    r.route(0,0,1,3); r.route(1,0,2,0);
    r.grids[0].shuffle=true; r.grids[1].shuffle=true;
    r.knobs[1].clicks[0]=r.knobs[1].clicks[3]=1;
    r.grids[1].pattern().changedMask=15^0x52; // Slots 2, 5, 7; leading and internal rests.
    for(u32 i=0;i<8;++i) r.grids[1].pattern().offsets[i]=static_cast<i32>(i)-intervals[i];
    r.press(0,40); r.press(0,80); r.run(2); r.lift(0,40); r.lift(0,80); r.run(600);
    for(u32 root:{40u,80u}) {
        u32 heard=0;
        for(u32 step:{1u,4u,6u}) for(u32 slot:{1u,4u,6u}) {
            const u32 when=60+step*60;
            if(!r.count(1,root+slot,when)) continue;
            assert(!(heard&(1u<<slot)));
            heard|=1u<<slot;
            assert(r.count(2,root+slot,when)==1);
        }
        assert(heard==0x52);
    }
    assert(r.hits.size()==40);
}

static void check_shuffle_resume() {
    Rig continuous,paused;
    for(Rig* rig:{&continuous,&paused}) {
        rig->arp(0); rig->knobs[0].clicks[2]=rig->knobs[0].clicks[3]=1;
        rig->grids[0].shuffle=true; rig->grids[0].pattern().changedMask=15^0x53;
        rig->press(); rig->run(10);
    }
    continuous.run(1100); paused.run(1100,0);
    continuous.run(900); paused.run(900);
    for(const auto& h:continuous.hits) if(h.on && h.now>=1110) assert(paused.count(0,h.pitch,h.now)==1);
    for(const auto& h:paused.hits) if(h.on && h.now>=1110) assert(continuous.count(0,h.pitch,h.now)==1);
}

static void check_spray() {
    bool heardImmediateRepeat=false;
    for (unsigned mask=0;mask<256;++mask) for (bool rest:{false,true})
    for (bool shuffle:{false,true}) {
        Rig r;
        r.knobs[0].value[0]=Rig::word(1,3);
        r.knobs[0].value[2]=Rig::word(0,4);
        r.knobs[0].clicks[0]=1;
        r.knobs[0].clicks[3]=rest;
        r.route(0,0,1,1); r.route(1,0,2,0);
        Grid& grid=r.grids[0];
        grid.pattern().changedMask=mask^15;
        grid.shuffle=shuffle;
        grid.tail=4; // 250 ms, or 120 player ticks at 120 BPM.
        const u32 releaseAt=540;
        std::vector<unsigned> allowed;
        for(unsigned i=0;i<8;++i) {
            grid.pattern().set_pitch(i,i);
            if(mask&(1u<<i)) allowed.push_back(i);
        }
        r.press(); r.run(releaseAt); r.lift();
        grid.tail=3;
        r.run(220);
        unsigned n=0,seen=0,last=0;
        for(const Hit& h:r.hits) {
            if(!h.on || h.track) continue;
            assert(!allowed.empty() && h.pitch>=60 && h.pitch<68);
            const unsigned bit=1u<<(h.pitch-60);
            assert(mask&bit);
            const unsigned pos=n%allowed.size();
            const unsigned tick=rest?(n/allowed.size())*(allowed.back()+1)+allowed[pos]:n;
            const unsigned elapsed=h.now>releaseAt?(h.now*7629/1200-releaseAt*7629/1200):0;
            assert(elapsed<763 && h.now==30*tick && h.vel==96);
            if(shuffle) {
                if(!pos) seen=0;
                assert(!(seen&bit)); seen|=bit;
            } else if(n && last==h.pitch && allowed.size()>1) heardImmediateRepeat=true;
            for(unsigned dest:{1u,2u}) {
                unsigned matches=0;
                for(const Hit& echo:r.hits)
                    if(echo.track==dest && echo.on && echo.now==h.now+15 &&
                       echo.pitch==h.pitch && echo.vel==96*(763-elapsed)/763) ++matches;
                assert(matches==1 && r.count(dest,h.pitch,h.now+30,false)==1);
            }
            last=h.pitch; ++n;
        }
        unsigned expected=0;
        for(unsigned t=0;mask && t<releaseAt+120;t+=30)
            if(!rest || (mask&(1u<<((t/30)%(allowed.back()+1))))) ++expected;
        assert(n==expected && r.hits.size()==n*6);
        assert(!r.states[0].eventCount && !r.states[0].leaseCount);
    }
    assert(heardImmediateRepeat);
    for(bool shuffle:{false,true}) {
        Rig continuous,paused;
        for(Rig* r:{&continuous,&paused}) {
            r->knobs[0].value[0]=Rig::word(1,3); r->grids[0].shuffle=shuffle;
            r->grids[0].pattern().changedMask=0x52^15; r->knobs[0].clicks[3]=1;
            r->press(); r->run(10);
        }
        continuous.run(390); paused.run(390,0);
        continuous.run(1200); paused.run(1200);
        unsigned hits=0;
        for(const Hit& h:continuous.hits) if(h.on && h.now>=400) {
            assert(paused.count(0,h.pitch,h.now)==1); ++hits;
        }
        for(const Hit& h:paused.hits) if(h.on && h.now>=400) {
            assert(continuous.count(0,h.pitch,h.now)==1); --hits;
        }
        assert(!hits && paused.states[0].eventCount);
        paused.lift(); paused.run(700); assert(!paused.states[0].eventCount);
    }
    Rig root;
    root.knobs[1].value[0]=Rig::word(1,3); root.grids[1].pattern().set_pitch(0,12);
    root.grids[1].pattern().changedMask=15^1;
    root.route(0,0,1,3); root.route(1,0,2,0);
    root.press(); root.run(8); root.lift(); root.run(600);
    assert(root.count(2,60,60) && root.count(2,60,68,false));
    for(unsigned i=0;i<9;++i) assert(root.count(1,72,60+60*i));
    assert(root.hits.size()==22);
    Rig soft;
    soft.knobs[0].value[0]=Rig::word(1,3);
    Rig::Port port={soft,0}; press_key(soft.states[0],port,60,1);
    soft.run(1); soft.lift(); soft.run(2000);
    assert(soft.hits.size()>2 && soft.hits[0].vel==1 && !soft.states[0].eventCount);
}
static void check_spray_overlap() {
    // Two direct keypresses enter one downstream Spray through a delayed Root.
    // The second gate opens during the first release; both run at their own velocity.
    for (bool retrigger:{false,true}) {
        Rig r;
        r.route(0,0,1,3); r.route(1,0,2,1);
        r.knobs[1].value[0]=Rig::word(1,3); r.knobs[1].clicks[0]=1;
        r.grids[1].pattern().changedMask=15^1;
        r.grids[1].tail=3; // Five seconds, 2400 ticks.
        const u32 second=retrigger?60:72;
        r.press(0,60,101); r.run(3000); r.lift();
        r.run(17); r.press(0,second,47);
        r.run(623); r.lift(0,second); r.run(2800);
        unsigned firstCount=0,secondCount=0;
        for (const Hit& h:r.hits) if(h.on && h.track==1) {
            const bool first=(h.now-60)%60==0;
            const u32 launch=first?60:3077, release=first?3060:3700, velocity=first?101:47;
            assert(h.now>=launch && h.pitch==(first?60:second));
            const u32 age=h.now<=release?0:(h.now*7629/1200-release*7629/1200);
            assert(age<15259 && h.vel==velocity);
            bool echoed=false;
            for(const Hit& echo:r.hits)
                if(echo.on && echo.track==2 && echo.now==h.now+15 && echo.pitch==h.pitch && echo.vel==velocity*(15259-age)/15259) echoed=true;
            assert(echoed==(velocity*(15259-age)/15259!=0));
            if(first) ++firstCount; else ++secondCount;
        }
        assert(firstCount==90 && secondCount==51);
        assert(!r.states[0].eventCount && !r.states[0].leaseCount && !r.states[1].sounding && !r.states[2].sounding);
    }
    // Arp Steps delivers a timed gate even when the initiating key stays held.
    Rig steps; steps.arp(0); steps.route(0,0,1,3);
    steps.knobs[0].clicks[0]=1; steps.grids[0].pattern().changedMask=15^1;
    steps.knobs[1].value[0]=Rig::word(1,3); steps.grids[1].pattern().changedMask=15^1;
    steps.grids[1].tail=4;
    steps.press(0,60,80); steps.run(1300);
    unsigned received=0;
    for(const Hit& h:steps.hits) if(h.on && h.track==1) {
        assert(h.now==60+60*received);
        assert(h.vel==80); ++received;
    }
    assert(received==3 && !steps.states[0].eventCount);
    // A long gate can cross the release deadline before its next subdivision.
    Rig slow; slow.tempo=300; slow.knobs[0].bpm_x10=300;
    slow.knobs[0].value[0]=Rig::word(1,3); slow.knobs[0].value[2]=slow.knobs[0].value[3]=Rig::word(3,4);
    slow.grids[0].tail=4; slow.grids[0].pattern().changedMask=15^1;
    slow.press(); slow.run(6); slow.lift(); slow.run(40);
    assert(slow.hits.size()==2 && slow.hits[1].now>=35 && slow.hits[1].now<=37);
    // The release stays on wall time across a tempo change. Shift the stamp
    // origin to keep the simulated real clock continuous.
    Rig tempo; tempo.grids[0].pattern().changedMask=15^1;
    tempo.knobs[0].value[0]=Rig::word(1,3);
    tempo.press(); tempo.run(10); tempo.lift(); tempo.run(120);
    const u32 oldStamp=Rig::Port{tempo,0}.time_stamp();
    tempo.tempo=600; tempo.knobs[0].bpm_x10=600;
    tempo.stampOffset=oldStamp-tempo.time*7629/tempo.tempo;
    tempo.run(155);
    assert(tempo.states[0].eventCount);
    tempo.run(32);
    assert(!tempo.states[0].eventCount && !tempo.states[0].sounding);
}
static void check_delays() {
    for (u32 bpm:{300u,1200u,3000u}) for (u32 branch=0;branch<2;++branch)
    for (u32 n=1;n<=15;++n) {
        Rig r; r.tempo=bpm; r.knobs[0].bpm_x10=bpm;
        const u32 dest=branch+1;
        r.route(0,branch,dest,n+2);
        r.press(); r.run(10); r.lift(); r.run(n*60+20);
        assert(r.count(dest,60,n*60)==1 && r.count(dest,60,n*60+10,false)==1);
    }
    Rig dense;
    dense.arp(0); dense.knobs[0].clicks[0]=dense.knobs[0].clicks[2]=1;
    dense.knobs[0].value[2]=Rig::word(0,4);
    dense.grids[0].pattern().changedMask=15^1;
    dense.route(0,0,1,17); dense.route(0,1,2,16);
    dense.press();
    for (u32 i=0;i<1800;++i) {dense.run(1); assert(dense.states[0].route!=Route::full);}
    dense.lift(); dense.run(1000);
    for (u32 i=0;i<60;++i) {
        assert(dense.count(1,60,900+i*30));
        assert(dense.count(2,60,840+i*30));
    }
    assert(!dense.states[0].eventCount && !dense.states[0].leaseCount);
}
static void check_expanded_timing() {
    const unsigned periods[]={30,45,60,90,120,180,240,360,480};
    for (u32 ri=0;ri<9;++ri) for (u32 gi=0;gi<20;++gi) {
        Rig r; r.arp(0); r.knobs[0].clicks[0]=1; r.route(0,0,1,3);
        r.knobs[0].value[2]=timing_word<rateChoices>(ri);
        r.knobs[0].value[3]=timing_word<gateChoices>(gi);
        const u32 beat=periods[ri], duration=beat*(gi+1)/20;
        r.press(); r.run(beat*4+61);
        for (u32 j=0;j<4;++j) {
            const unsigned note=60+intervals[j], at=beat*j;
            assert(r.count(0,note,at)==1 && r.count(0,note,at+duration,false)==1);
            assert(r.count(1,note,at+60)==1 && r.count(1,note,at+60+duration,false)==1);
        }
        r.lift(); r.run(2); assert(!r.states[0].eventCount && !r.states[0].leaseCount);
        // An incoming sustained note starts Spray at the same interval and gate.
        Rig spray; spray.knobs[0].value[0]=Rig::word(1,3);
        spray.knobs[0].value[2]=r.knobs[0].value[2]; spray.knobs[0].value[3]=r.knobs[0].value[3];
        spray.grids[0].pattern().changedMask=1^15;
        spray.press(); spray.run(beat*2+1);
        assert(spray.count(0,60,0) && spray.count(0,60,beat) && spray.count(0,60,duration,false));
    }
}
static void check_velocity_fork() {
    for (u32 split=0;split<=forkMax;++split) for (u32 velocity=1;velocity<=127;++velocity) {
        Rig r;
        // Test a receiving Note track with its own fork, plus independent delays.
        r.route(0,0,1,1); r.route(1,0,2,3); r.route(1,1,3,1);
        r.grids[1].fork=split;
        r.press(0,60,velocity); r.run(2); r.lift(); r.run(80);
        const bool upper=!split || velocity>=fork_threshold(split);
        const bool lower=!split || velocity<fork_threshold(split);
        assert(r.count(1,60,15)==1 && r.count(1,60,17,false)==1);
        assert(r.count(2,60,75)==upper && r.count(2,60,77,false)==upper);
        assert(r.count(3,60,30)==lower && r.count(3,60,32,false)==lower);
        assert(r.hits.size()==4+2*(unsigned(upper)+unsigned(lower)));
        for (const Hit& hit:r.hits) if (hit.on) assert(hit.vel==velocity);
    }
    Rig missing;
    missing.route(0,0,1,0); missing.grids[0].fork=7;
    missing.press(0,60,63); missing.run(1); missing.lift();
    assert(missing.hits.size()==2 && !missing.states[0].eventCount);
}

static void check_swing_and_nudge() {
    for (u32 ri=0;ri<rateChoices;++ri) for (i32 depth=-swingMax;depth<=swingMax;++depth) {
        Rig r; r.arp(0); r.route(0,0,1,1); r.grids[0].swing=depth;
        r.knobs[0].clicks[0]=r.knobs[0].clicks[2]=r.knobs[0].clicks[3]=1;
        r.knobs[0].value[2]=timing_word<rateChoices>(ri);
        r.grids[0].pattern().changedMask=15^5; // on, rest, on; odd-length repeat
        const u32 rate=rate_steps[ri]; r.press(); r.run(8*rate+1); r.lift(); r.run(rate+20);
        for (u32 i=0;i<9;++i) {
            if (i%3==1) continue;
            const i32 displacement=(rate*swing_magnitude(depth)+50)/100;
            const u32 when=i*rate+((i%2)?(depth<0?-displacement:displacement):0);
            const u32 pitch=i%3?72:60;
            assert(r.count(0,pitch,when)==1 && r.count(1,pitch,when+15)==1);
        }
        assert(r.hits.size()==24);
    }
    for (u32 ri=0;ri<rateChoices;++ri) for (u32 depth=0;depth<=nudgeMax;++depth) {
        Rig r; r.knobs[0].value[0]=Rig::word(1,3); r.route(0,0,1,1);
        r.knobs[0].value[2]=timing_word<rateChoices>(ri); r.knobs[0].clicks[0]=1;
        r.grids[0].nudge=depth;
        const u32 rate=rate_steps[ri], radius=rate*depth/40;
        r.press(); r.run(18*rate+rate/2+1);
        unsigned hits=0; bool early=false,late=false;
        for (const Hit& hit:r.hits) if (hit.on && hit.track==0) {
            const int offset=int(hit.now)-int(hits*rate);
            assert(offset>=-int(radius) && offset<=int(radius));
            if (!hits) assert(!offset);
            if (hits<18) assert(r.count(1,hit.pitch,hit.now+15)==1);
            early|=offset<0; late|=offset>0; ++hits;
        }
        assert(hits==19);
        if (depth==nudgeMax) assert(early && late);
    }
    // Cover both extremes and every shared boundary for each rate. Compare
    // successive deadlines directly, including across the 32-bit clock wrap.
    for (u32 ri=0;ri<rateChoices;++ri) {
        Event e={}; e.kind=Kind::sprayStep; e.rateIndex=ri; e.depth=nudgeMax;
        e.id=42; e.start=e.due=0xffffff00u;
        const i32 rate=e.interval(); u32 previous=e.start;
        bool earliest=false,latest=false;
        for (unsigned i=1;i<=8192;++i) {
            e.due+=rate; place_attack(e);
            assert(static_cast<i32>(e.attack()-previous)>0);
            assert(e.offset>=-(rate/2) && e.offset<=rate/2);
            earliest|=e.offset==-(rate/2); latest|=e.offset==rate/2;
            previous=e.attack();
        }
        assert(earliest && latest);
    }
    // A held repeat responds without a retrigger. Its pending step at 60 is fixed.
    Rig live; live.arp(0); live.knobs[0].clicks[2]=1; live.press(); live.run(30);
    live.grids[0].swing=swingMax; live.run(210);
    assert(live.count(0,67,60) && live.count(0,72,120) && live.count(0,79,237));
    Rig cloud; cloud.knobs[0].value[0]=Rig::word(1,3); cloud.press(); cloud.run(30);
    cloud.grids[0].nudge=nudgeMax; cloud.run(570);
    bool displaced=false;
    for (const Hit& h:cloud.hits) if (h.on && h.now>60 && h.now%60) displaced=true;
    assert(displaced);
    for (int timing:{-swingMax,int(swingMax),0}) {
        const bool spray=timing==0;
        Rig a,b;
        for (Rig* r:{&a,&b}) {
            r->knobs[0].value[0]=Rig::word(spray?1:2,3);
            r->knobs[0].clicks[2]=r->knobs[0].clicks[3]=1;
            r->grids[0].shuffle=true; r->grids[0].swing=timing; r->grids[0].nudge=nudgeMax;
            r->grids[0].pattern().changedMask=15^0x52; r->press(); r->run(10);
        }
        a.run(767); b.run(767,0); a.run(660); b.run(660);
        for (const Hit& h:a.hits) if (h.on && h.now>=777) assert(b.count(0,h.pitch,h.now));
        for (const Hit& h:b.hits) if (h.on && h.now>=777) assert(a.count(0,h.pitch,h.now));
    }
}
int main(){
    check_velocity_fork();
    check_swing_and_nudge();
    check_expanded_timing();
    check_delays();
    check_spray();
    check_spray_overlap();
    check_shuffle_masks();
    check_receiving_shuffle();
    check_shuffle_resume();
    for(u32 rate=0;rate<4;++rate) for(u32 gate=0;gate<4;++gate) {
        Rig r; r.knobs[0].value[2]=Rig::word(rate,4); r.knobs[0].value[3]=Rig::word(gate,4);
        r.press(); r.press(0,64); r.run(1000);
        assert(r.hits.size()==2);
        r.lift(); assert(r.count(0,60,1000,false)==1 && r.states[0].sounding==1);
        r.lift(0,64); assert(r.states[0].sounding==0);
    }
    // Delayed note-on and key-up follow the same delay. No Hocket tick on T2.
    Rig late;late.route(0,0,1,3);late.press();late.run(10,1);late.lift();late.run(61,1);
    assert(late.count(1,60,60)==1 && late.count(1,60,70,false)==1);
    assert(late.hits.size()==4 && late.states[1].route==Route::received);
    Rig fork; fork.arp(0); fork.route(0,0,1,3);fork.route(0,1,2,0);
    fork.route(1,0,0,0);fork.press();fork.run(110,1);fork.lift();fork.run(160,1);
    for(u32 i=0;i<4;++i) {
        assert(fork.count(0,60+intervals[i],60*i)==1);
        assert(fork.count(0,60+intervals[i],60*i+30,false)==1);
    }
    assert(fork.count(2,60,0)==1 && fork.count(1,60,60)==1);
    assert(fork.count(2,60,110,false)==1 && fork.count(1,60,170,false)==1);
    assert(fork.hits.size()==12);
    Rig same; same.route(0,0,1,0);same.route(0,1,2,0);same.route(1,0,3,0);same.route(2,0,3,0);
    same.press();same.press();same.press(0,64);same.run(1);same.lift();same.lift(0,64);
    assert(same.hits.size()==16 && same.count(3,60,0)==1 && same.count(3,64,0)==1);
    Rig independent; independent.route(0,0,2,0);independent.route(1,0,2,0);
    independent.press(0);independent.press(1);independent.run(10);independent.lift(0);
    assert(independent.states[2].sounding==1 && independent.count(2,60,0)==1);
    independent.lift(1); assert(independent.states[2].sounding==0);
    Rig retrig;retrig.route(0,0,2,0);retrig.route(1,0,2,0);retrig.press();retrig.run(8);
    retrig.press(1);retrig.run(2);retrig.lift(0);assert(retrig.states[2].sounding==1);
    retrig.lift(1);assert(retrig.states[2].sounding==0);
    Rig edit;edit.route(0,0,1,3);edit.press();edit.run(5);edit.route(0,0,2,0);edit.run(60);
    assert(edit.count(1,60,60)==1 && edit.states[2].sounding==0);
    Rig missing;missing.installed[1]=false;missing.route(0,0,1,0);missing.press();
    assert(missing.states[0].route==Route::missing && missing.hits.size()==1);
    Rig remoteArp;remoteArp.arp(1);remoteArp.route(0,0,1,0);remoteArp.press();remoteArp.run(240,1);
    for(u32 i=0;i<4;++i)assert(remoteArp.count(1,60+intervals[i],i*60)==1);
    Rig stop;stop.route(0,0,1,0);stop.press();Rig::Port port={stop,0};
    expire(stop.states[0],port,true);reset(stop.states[0]);assert(!stop.states[1].sounding);
    Rig wrap;wrap.states[0].clock=0xfffffff0u;wrap.arp(0);wrap.press();wrap.run(241);assert(wrap.hits.size()==8);
    Rig high;high.arp(0);high.press(0,121);high.run(300);assert(high.hits.size()==2);
    Rig cap;for(u8 i=0;i<9;++i)cap.press(0,50+i);assert(cap.hits.size()==8 && cap.states[0].route==Route::full);
    for(u8 i=0;i<9;++i)cap.lift(0,50+i);
    cap.run(2); assert(!cap.states[0].sounding);
    // Root is the default on a repeating sender. The receiver gets one held C.
    Rig root; root.arp(0); root.knobs[0].clicks[2]=1; root.route(0,0,1,3);
    root.press(); root.run(501); root.lift(); root.run(160);
    assert(root.count(0,60,240)==1 && root.count(0,60,480)==1);
    assert(root.count(1,60,60)==1 && root.count(1,60,561,false)==1);
    unsigned remoteHits=0; for(const Hit& h:root.hits) if(h.track==1) ++remoteHits;
    assert(remoteHits==2 && root.count(0,67,540)==0);

    // Each arp gate travels independently, even after the originating key lifts.
    for (u32 repeat=0;repeat<2;++repeat) {
        Rig steps; steps.arp(0); steps.knobs[0].clicks[0]=1; steps.knobs[0].clicks[2]=repeat;
        steps.route(0,0,1,3); steps.route(0,1,2,0); steps.route(2,0,0,0);
        steps.press(); steps.run(repeat?245:5); steps.lift(); steps.run(400);
        const u32 notes=repeat?5:4;
        for (u32 j=0;j<notes;++j) {
            u32 pitch=60+intervals[j%4], when=60*j;
            assert(steps.count(1,pitch,when+60)==1 && steps.count(1,pitch,when+90,false)==1);
            assert(steps.count(2,pitch,when)==1 && steps.count(2,pitch,when+30,false)==1);
        }
        assert(steps.hits.size()==notes*6);
        assert(!steps.states[0].eventCount && !steps.states[0].leaseCount);
    }
    // A brief tap delivered to a repeating arp gets its delayed release.
    Rig brief; brief.route(0,0,1,3); brief.arp(1); brief.knobs[1].clicks[2]=1;
    brief.press(); brief.run(4); brief.lift(); brief.run(400);
    assert(brief.hits.size()==4 && brief.count(1,60,60)==1 && brief.count(1,60,90,false)==1);
    // Retrigger the same destination pitch with a second, shorter owner.
    Rig cross; cross.route(0,0,2,0); cross.route(1,0,2,0);
    cross.press(0,60); cross.press(0,64); cross.run(5);
    cross.press(1,60); cross.run(2); cross.lift(1,60);
    assert(cross.states[2].sounding==2 && cross.count(2,60,7,false)==0);
    cross.lift(0,64); cross.lift(0,60);
    assert(!cross.states[2].sounding && cross.count(2,60,7,false)==1);
    // Two inputs before a stock step, including a release in that same interval.
    Rig tap; tap.route(0,0,1,0); tap.press(0,64); tap.press(); tap.lift();
    assert(tap.count(1,60,0,false)==0);
    tap.run(2); assert(tap.count(1,60,1,false)==1 && tap.states[1].sounding==1);
    tap.lift(0,64); assert(!tap.states[1].sounding);
    // Rapid repeating taps must free canceled steps immediately.
    Rig rapid; rapid.arp(0); rapid.knobs[0].clicks[2]=1; rapid.knobs[0].clicks[0]=1;
    rapid.route(0,0,1,0);
    for(u32 j=0;j<80;++j) {
        rapid.press(0,60+j%4); rapid.lift(0,60+j%4); rapid.run(4);
        assert(!rapid.states[0].eventCount && rapid.states[0].route!=Route::full);
    }
    rapid.run(240); assert(!rapid.states[0].sounding && !rapid.states[1].sounding && !rapid.states[0].leaseCount);
    // T1 releases after T2's delayed attack, then only T2's player gets serviced.
    Rig paused; paused.route(0,0,1,3); paused.press(); paused.run(80); paused.lift();
    paused.run(100,2);
    assert(paused.count(1,60,60)==1 && paused.count(1,60,142,false)==1);
    assert(!paused.states[1].sounding && !paused.states[0].leaseCount);
    paused.press(1,60); paused.run(100); assert(paused.states[1].sounding==1);
    paused.lift(1,60); assert(!paused.states[1].sounding);

    // The earlier gate can end during the other source's held note.
    Rig shared; shared.route(0,0,2,3); shared.route(1,0,2,0);
    shared.press(0,60); shared.run(65); shared.press(1,60); shared.run(15); shared.lift(0,60);
    shared.run(120,6);
    assert(!shared.states[0].leaseCount && shared.states[2].sounding==1);
    assert(shared.count(2,60,142,false)==0);
    shared.lift(1,60); assert(shared.count(2,60,200,false)==1);

    // Different senders have different deadlines on the same destination voice.
    Rig overlap; overlap.route(0,0,2,3); overlap.route(1,0,2,3);
    overlap.press(0,60); overlap.run(5); overlap.press(1,60); overlap.run(75);
    overlap.lift(0,60); overlap.run(10); overlap.lift(1,60);
    overlap.run(60,4); assert(overlap.states[2].sounding==1 && !overlap.states[0].leaseCount);
    overlap.run(5,4); assert(overlap.count(2,60,152,false)==1 && !overlap.states[1].leaseCount);

    // A finite Steps handoff must end even if its sender clock goes idle.
    Rig stepsIdle; stepsIdle.arp(0); stepsIdle.knobs[0].clicks[0]=1; stepsIdle.route(0,0,1,0);
    stepsIdle.states[1].clock=0xfffffff0u;
    stepsIdle.press(); stepsIdle.run(2); stepsIdle.lift(); stepsIdle.run(300,2);
    assert(stepsIdle.count(1,60,30,false)==1 && !stepsIdle.states[1].sounding);
    // A long key hold is intentional, including when the source clock is idle.
    Rig held; held.route(0,0,1,0); held.press(); held.run(1000,2);
    assert(held.states[1].sounding==1); held.lift(); held.run(5,2); assert(!held.states[1].sounding);
    // Three short taps, a silent bar, then a different sustained pitch.
    // If T2 is running it serves the taps. If both are idle they expire.
    for (u32 clocks : {0u,1u,2u,3u}) {
        Rig r; r.route(0,0,1,3);
        for (u8 n : {60,62,64}) { r.press(0,n); r.run(2); r.lift(0,n); r.run(2); }
        r.run(960,clocks);
        const u32 newPress=r.time;
        r.press(0,72); r.run(120); r.lift(0,72); r.run(180);
        for (const Hit& h:r.hits)
            if (h.track==1 && h.on && h.pitch!=72) assert(h.now<newPress);
        for (u32 j=0;j<3;++j) assert(r.count(1,60+2*j,60+4*j)==(clocks!=0));
        assert(r.count(1,72,newPress+60)==1 && r.count(1,72,newPress+180,false)==1);
        assert(!r.states[0].eventCount && !r.states[0].leaseCount && !r.states[1].sounding);
    }
    // Resume before an existing deadline. The due time stays tied to the tap.
    Rig early; early.route(0,0,1,3); early.press(); early.run(2); early.lift();
    early.run(30,0); early.press(0,72); early.run(70);
    assert(early.count(1,60,60)==1 && early.count(1,72,92)==1);
    // An arp resumes at the next subdivision; no old steps burst on the key.
    Rig phase; phase.arp(0); phase.press(); phase.run(2); phase.lift();
    phase.run(100,0); phase.press(0,48); phase.run(180);
    assert(!phase.count(0,67,102) && phase.count(0,72,120)==1 && phase.count(0,79,180)==1);
    // A rest precedes the first generated handoff. T2 alone keeps ticking.
    Rig restPeer; restPeer.arp(0); restPeer.knobs[0].clicks[0]=1; restPeer.knobs[0].clicks[3]=1;
    restPeer.grids[0].pattern().changedMask=15^18; restPeer.knobs[0].value[3]=Rig::word(0,4);
    restPeer.route(0,0,1,3); restPeer.press(); restPeer.run(360,2);
    assert(restPeer.count(1,67,120)==1 && restPeer.count(1,60,300)==1);
    assert(restPeer.count(1,67,135,false)==1 && restPeer.count(1,60,317,false)==1);
    restPeer.lift(); restPeer.run(80); assert(!restPeer.states[1].sounding);

    // Two live receivers cannot count a source tick twice. With the sender
    // asleep, a chord of generated Steps and its fork keep their own timing.
    Rig peers; peers.arp(0); peers.knobs[0].clicks[0]=1;
    peers.route(0,0,1,3); peers.route(0,1,2,3);
    peers.press(); peers.press(0,48); peers.run(3); peers.lift(); peers.lift(0,48);
    peers.run(250,6); peers.run(100);
    for(u32 t : {1u,2u}) for(u32 j=0;j<4;++j) for(u32 root : {48u,60u}) {
        assert(peers.count(t,root+intervals[j],(j+1)*60)==1);
    }
    assert(!peers.states[0].leaseCount && !peers.states[1].sounding && !peers.states[2].sounding);
    for(u32 bpm : {200u,1200u,3000u}) {
        Rig r; r.tempo=bpm; r.knobs[0].bpm_x10=bpm; r.knobs[1].bpm_x10=bpm;
        r.stampOffset=0xfffffff0u; r.states[0].clock=0xfffffff0u;
        r.route(0,0,1,3); r.press(); r.run(3); r.lift();
        r.run(2000,0); r.run(80,2); r.press(0,75); r.run(80);
        assert(r.count(1,75,2143)==1);
        for(const Hit& h:r.hits) assert(!(h.track==1 && h.pitch==60 && h.on));
    }
    // Check the actual routing stream against an independent list of slots.
    for (u32 mask=0;mask<256;++mask) for(u32 rests=0;rests<2;++rests) {
        Rig r; r.arp(0); r.route(0,0,1,0);
        r.knobs[0].clicks[0]=1; r.knobs[0].clicks[3]=rests;
        Grid& grid=r.grids[0];
        assert(grid.click(1));
        for(u32 bank=0;bank<2;++bank) {
            for(u32 k=0;k<4;++k) {
                const u32 slot=4*bank+k;
                grid.turn(k,static_cast<i32>(slot)-grid.pitch(slot)-4,bank!=0);
                grid.click(3); grid.turn(k,(mask&(1u<<slot))?1:-1,bank!=0); grid.click(3);
            }
        }
        assert(grid.click(1) && !grid.page);
        r.press(); r.run(540); r.lift();
        u32 count=0;
        for(u32 slot=0;slot<8;++slot) {
            if (!(mask&(1u<<slot))) continue;
            const u32 due=60*(rests?slot:count++);
            const u32 pitch=56+slot;
            for(u32 track=0;track<2;++track) {
                assert(r.count(track,pitch,due)==1);
                assert(r.count(track,pitch,due+30,false)==1);
            }
        }
        u32 enabled=0; for(u32 slot=0;slot<8;++slot) enabled+=(mask>>slot)&1;
        assert(r.hits.size()==4*enabled && !r.states[0].eventCount && !r.states[0].leaseCount);
    }
    // Only slots 2 and 5: five ticks in Rest, two ticks in Skip.
    for (u32 rests=0;rests<2;++rests) {
        Rig r; r.arp(0); r.route(0,0,1,3); r.knobs[0].clicks[0]=1;
        r.knobs[0].clicks[2]=1; r.knobs[0].clicks[3]=rests; r.grids[0].pattern().changedMask=15^18;
        r.press(); const u32 length=rests?5:2; r.run(2*length*60); r.lift(); r.run(200);
        for(u32 pass=0;pass<2;++pass) {
            assert(r.count(1,67,60*(pass*length+(rests?1:0))+60)==1);
            assert(r.count(1,60,60*(pass*length+(rests?4:1))+60)==1);
        }
        assert(r.hits.size()==16);
    }
    Grid bounds={}; bounds.click(1); bounds.turn(1,2147483647,false); assert(bounds.pitch(1)==48);
    bounds.turn(1,(-2147483647-1),false); assert(bounds.pitch(1)==-48);
    bounds.click(3); bounds.turn(1,-1,false); bounds.turn(1,1,false); assert(bounds.pitch(1)==-48);
    Rig silent; silent.arp(0); silent.grids[0].pattern().changedMask=15; silent.route(0,0,1,0);
    silent.knobs[0].clicks[2]=1; silent.press(); silent.run(400); silent.lift();
    assert(silent.hits.empty() && !silent.states[0].eventCount);
    Rig limits; limits.arp(0); limits.grids[0].pattern().changedMask=15^3; limits.grids[0].pattern().offsets[0]=-48;
    limits.grids[0].pattern().offsets[1]=41; limits.press(0,1); limits.run(150);
    assert(limits.hits.size()==2 && limits.count(0,49,60)==1);
    Rig edited; edited.arp(0); edited.grids[0].pattern().changedMask=15^3; edited.press(); edited.run(4);
    edited.grids[0].pattern().changedMask=15; edited.grids[0].pattern().offsets[1]=-5; edited.run(130);
    assert(edited.hits.size()==4 && edited.count(0,62,60)==1);
    edited.press(0,72); assert(edited.hits.size()==4);
    assert(delay_steps(20,1200)==48 && delay_steps(20,600)==24);
}
