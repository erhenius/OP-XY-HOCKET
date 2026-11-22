// The initiating player clocks its own phrase and all of its handoffs.
// The SDK adapter supplies only same-player destinations and local note output.
#include "arp_grid.hpp"
#define OPXY_HOCKET_PLAYER 1
namespace hocket {
using namespace opxy;
using player::Controls;
const char* const rates[] = {"1/32", "1/16", "1/8", "1/4"};
const u8 rate_steps[] = {30, 60, 120, 240};
const char* const delays[] = {"0", "1/64", "1/32", "1/16", "2/16", "3/16", "4/16", "5/16", "6/16", "7/16", "8/16", "9/16", "10/16", "11/16", "12/16", "13/16", "14/16", "15/16", "20ms", "50ms", "100ms", "250ms", "500ms"};
const u16 delay_values[] = {0,15,30,60,120,180,240,300,360,420,480,540,600,660,720,780,840,900,20,50,100,250,500};
const char* const tracks[] = {"Off","T1","T2","T3","T4","T5","T6","T7","T8"};
const char* const gates[] = {"25%", "50%", "75%", "100%"};
const u32 delayChoices=sizeof(delay_values)/sizeof(delay_values[0]);
const u32 eventCapacity=64;
enum class Probe : u8 { idle, clockOnly, noPitch, noVelocity, waiting, stepped, consumed, cleared };
enum class Route : u8 { off, pending, delivered, missing, received, full };
enum class Playhead : u8 { note, spray, arp };
static inline Playhead playhead(const Controls& c) {
    return static_cast<Playhead>(player::choice(c.value[0],3));
}
const u16 spray_ms[]={1000,2000,3000,5000,250,500};
const u32 spray_choices=sizeof(spray_ms)/sizeof(spray_ms[0]);
static inline u32 tail_stamps(u32 setting) {
    // Milliseconds to SDK timer units (400 MHz / 2^17).
    return (spray_ms[setting]*3125u+512u)/1024u;
}
enum class Kind : u8 { launch, arpStep, sprayStep };
struct Event {
    u32 due, id;
    union { u32 duration; u32 fadeStamp; }; // Incoming gate, then wall time of release for Spray.
    u32 start;
    union { u32 order; u32 sprayOwner; }; // Arp slot order or the ID owning this Spray's local gates.
    u8 rate, gate; // current rate table tops out at 240 player ticks
    u8 target, note, velocity, visited, step;
    Kind kind;
    u8 released:1, repeat:1, sendSteps:1, rests:1, fading:1, tail:3;
    u8 mask, length;
    signed char transpose;
};
struct Input { u32 id, start; u8 note; };
struct Lease {
    u32 id, ticket;
    union { u32 start; u32 receiverDue; }; // start while held, receiver deadline after key-up
    u32 until; // normal deadline on the originating clock
    u8 target, pitch;
    bool held;
};
struct Voice {
    u32 ticket, time, sourceTime, checkDue;
    u16 references;
    u8 pitch, origin, senders;
    bool checkRelease;
};
// One overflow queue per initiating instrument track; the stock player slot keeps the first 32.
static Event extraEvents[8][eventCapacity-32];
struct State {
    u32 clock, lastStamp, clockAtStamp;
    u16 tempo;
    bool stampValid, peerClock;
    u8 wakeSources;
    Input keys[8];
    Event events[32];
    u8 track;
    Event& event(u32 index) { return index<32 ? events[index] : extraEvents[track][index-32]; }
    Lease leases[32];
    Voice voices[8];
    u8 keyCount, eventCount, leaseCount, sounding, cursor;
    Probe probe;
    u8 probePitch, probeSteps;
    Route route;
    u8 routeTrack;
};
static_assert(sizeof(State)<=player::state_bytes,"track state budget");
static inline bool ready(u32 now,u32 due) { return static_cast<i32>(now-due)>=0; }
static inline u32 choice(i32 word,u32 count) { return player::choice(word,count); }
static inline u32 delay_steps(u32 index,u32 tempo) {
    return index<18 ? delay_values[index] : (delay_values[index]*(tempo?tempo:1200)+1250)/2500;
}
// Stock player callbacks are serialized. The output ticket survives a target
// being replaced, so a late release can never claim a newer instance's note.
static u32 generation;
static inline u32 ticket() { if (!++generation) ++generation; return generation; }

static bool schedule(State& s,const Event& e) {
    if (e.kind==Kind::launch) {
        for (u32 i=0;i<s.eventCount;++i) {
            Event& old=s.event(i);
            if (old.kind==e.kind && old.id==e.id && old.target==e.target && old.due==e.due) {
                old.visited|=e.visited;
                return true;
            }
        }
    }
    if (s.eventCount==eventCapacity) { s.route=Route::full; return false; }
    s.event(s.eventCount++)=e;
    return true;
}

template<class Port>
static void stop_voice(State& receiver,const Lease& lease,Port& output) {
    for (u32 i=0;i<receiver.sounding;++i) {
        Voice& v=receiver.voices[i];
        if (v.ticket!=lease.ticket) continue;
        if (--v.references==0) {
            output.off(v.pitch);
            receiver.voices[i]=receiver.voices[--receiver.sounding];
            if (!receiver.sounding) receiver.cursor=0;
        }
        return;
    }
}

// Keep the normal sender deadline. If only the receiver keeps stepping,
// its clock may also retire this specific lease two ticks after the gate.
static void arm_receiver(State& receiver,Voice& v,Lease& l,u32 remaining,u32 sender) {
    l.receiverDue=receiver.clock+remaining+2;
    v.senders|=1u<<sender;
    if (!v.checkRelease || ready(v.checkDue,l.receiverDue)) v.checkDue=l.receiverDue;
    v.checkRelease=true;
}

template<class Port>
static void check_releases(State& receiver,Port& output) {
    for (u32 i=0;i<receiver.sounding;) {
        Voice& v=receiver.voices[i];
        if (!v.checkRelease || !ready(receiver.clock,v.checkDue)) { ++i; continue; }
        const u8 senders=v.senders;
        v.checkRelease=false;
        v.senders=0;
        for (u32 k=0;k<8;++k) {
            if (!(senders & (1u<<k))) continue;
            Port sender=output.at(k);
            if (!sender.valid()) continue;
            State& owner=sender.memory();
            for (u32 j=0;j<owner.leaseCount;) {
                Lease& l=owner.leases[j];
                if (l.ticket!=v.ticket || l.held) { ++j; continue; }
                if (ready(receiver.clock,l.receiverDue)) {
                    --v.references;
                    l=owner.leases[--owner.leaseCount];
                    continue;
                }
                v.senders|=1u<<k;
                if (!v.checkRelease || ready(v.checkDue,l.receiverDue)) v.checkDue=l.receiverDue;
                v.checkRelease=true;
                ++j;
            }
        }
        if (v.references) { ++i; continue; }
        output.off(v.pitch);
        v=receiver.voices[--receiver.sounding];
        if (!receiver.sounding) receiver.cursor=0;
    }
}

template<class Port>
static void expire(State& s,Port& source,bool all=false) {
    for (u32 i=0;i<s.leaseCount;) {
        const Lease lease=s.leases[i];
        if (!all && (lease.held || !ready(s.clock,lease.until))) { ++i; continue; }
        Port output=source.at(lease.target);
        if (output.valid()) stop_voice(output.memory(),lease,output);
        s.leases[i]=s.leases[--s.leaseCount];
    }
}

// Derive a pass from its scheduled start. A long clock gap can jump
// straight to the current pass without replaying or drawing missed notes.
static u32 deal(const Event& e,u32 passStart) {
    const u32 seed=e.id*0x9e3779b9u ^ passStart*0x85ebca6bu ^
                   (e.target+1u)*0xc2b2ae35u ^ e.note;
    return shuffled_order(e.mask,seed);
}

// The step is the current subdivision in a single pass, even after long holds.
static void advance_spray(Event& e,u32 subdivisions) {
    e.due+=subdivisions*e.rate;
    e.step=(e.step+subdivisions%e.length)%e.length;
    while (e.rests && !(e.mask&(1u<<e.step))) {
        e.due+=e.rate;
        if (++e.step==e.length) e.step=0;
    }
}

template<class Port>
static void begin_spray_release(Port& source,Event& e,u32 clock,u32 stamp,u32 tempo) {
    if (e.fading || !e.released || !ready(clock,e.start+e.duration)) return;
    const u32 late=clock-(e.start+e.duration);
    // A suspended player can wake after the entire decay has passed.
    const u32 span=tail_stamps(e.tail);
    const u32 elapsed=late>=(span*tempo+7628)/7629?span:(late*7629u)/tempo;
    e.fadeStamp=stamp-elapsed;
    e.fading=true;
    source.release_spray(e.target,e.sprayOwner,e.tail+1,e.fadeStamp);
}

static u32 spray_slot(const Event& e) {
    const u32 count=phrase_length(e.mask,false);
    if (e.repeat) { // In Spray, this flag selects a freshly shuffled order per pass.
        const u32 order=deal(e,e.due-e.step*e.rate);
        const u32 slot=phrase_slot(e.mask,e.step,e.rests);
        return (order>>(4*slot))&7u;
    }
    // Mix the scheduled attack into the seed. Resuming a suspended phrase
    // produces the same pitches as uninterrupted playback from this point.
    u32 bits=e.id*0x9e3779b9u ^ (e.target+1u)*0xc2b2ae35u ^ e.note ^ e.due*0x85ebca6bu;
    bits^=bits>>16;
    bits*=0x7feb352du;
    bits^=bits>>15;
    return phrase_slot(e.mask,((bits>>16)*count)>>16,false);
}

// Missed launches cannot be played on time. An arp already in progress
// can still resume at its next original subdivision, including across a wrap.
static void skip_missed_attacks(State& s,u32 grace) {
    for (u32 i=0;i<s.eventCount;) {
        Event& e=s.event(i);
        if (!ready(s.clock,e.due+grace+1)) { ++i; continue; }
        if (e.kind==Kind::sprayStep) {
            const u32 missed=s.clock-e.due;
            advance_spray(e,missed/e.rate+(missed%e.rate!=0));
            ++i;
            continue;
        }
        if (e.kind==Kind::arpStep) {
            const u32 skip=(s.clock-e.due+e.rate-1)/e.rate;
            const u32 step=e.step+skip;
            e.due+=skip*e.rate;
            if ((e.repeat || step<e.length) && !(e.repeat && e.released && ready(e.due,e.start+e.duration))) {
                e.step=static_cast<u8>(step%e.length);
                if (e.order && step>=e.length) e.order=deal(e,e.due-e.step*e.rate);
                ++i;
                continue;
            }
        }
        s.event(i)=s.event(--s.eventCount);
    }
}

// The player step remains the musical clock. Compare it with the SDK's
// cycle clock only to detect a gap in service; never replay missed callbacks.
enum class ClockCall { input, step, peer };

static bool observe_clock(State& s,u32 stamp,ClockCall call) {
    const bool peer=call==ClockCall::peer;
    bool jumped=false;
    if (s.stampValid) {
        const u32 elapsed=stamp-s.lastStamp;
        if (elapsed>=16 || (peer && s.peerClock)) {
            // 240 steps/quarter; 400 MHz / 2^17 stamps/s. Splitting
            // the product keeps even a long idle interval in 32 bits.
            const u32 bpm=s.tempo?s.tempo:1200;
            const u32 steps=(elapsed/7629)*bpm+((elapsed%7629)*bpm+3814)/7629;
            const u32 expected=s.clockAtStamp+steps;
            const u32 gap=peer && s.peerClock?1:8;
            if (ready(expected,s.clock+gap)) {
                const u32 missed=expected-s.clock;
                s.clock=expected;
                // A running peer takes over after eight missing ticks.
                // A peer waking after a long silence skips the backlog.
                skip_missed_attacks(s,peer && missed<=8?missed:0);
                jumped=true;
            }
        }
    }
    if (peer) {
        if (jumped) s.peerClock=true;
    } else if (!s.stampValid || call==ClockCall::step || jumped) {
        s.stampValid=true;
        s.peerClock=false;
        s.lastStamp=stamp;
        s.clockAtStamp=s.clock;
    }
    return jumped;
}

template<class Port>
static void resume_input(State& s,Port& port) {
    if (observe_clock(s,port.time_stamp(),ClockCall::input)) {
        expire(s,port);
        check_releases(s,port);
    }
}

template<class Port>
static void sound(State& s,Port& source,Port& output,const Event& e,u32 pitch,bool hold,u32 gate,u32 slot) {
    if (pitch>127) return;
    State& r=output.memory();
    u32 i=0;
    while (i<r.sounding && r.voices[i].pitch!=pitch) ++i;
    if (i==8 || s.leaseCount==32) { s.route=Route::full; return; }
    const bool existing=i<r.sounding;
    Voice& v=r.voices[i];
    // A clock may be idle on the destination. Successive notes from a
    // running sender must still retrigger there.
    const bool together=existing && v.time==r.clock &&
        (v.origin!=source.track() || v.sourceTime==s.clock);
    if (!together) {
        if (existing) output.off(pitch);
        else {
            ++r.sounding;
            v.checkRelease=false;
            v.senders=0;
            v.ticket=ticket();
            v.references=0;
        }
        const bool spray=e.kind==Kind::sprayStep;
        const bool fading=spray && e.fading;
        output.on(pitch,e.velocity,source.track(),spray ? e.id : 0,
                  fading ? e.tail+1 : 0,fading ? e.fadeStamp : 0);
        v.pitch=static_cast<u8>(pitch);
        v.time=r.clock;
        v.sourceTime=s.clock;
        v.origin=static_cast<u8>(source.track());
    }
    ++v.references;
    Lease& lease=s.leases[s.leaseCount++];
    lease={e.id,v.ticket,{s.clock},s.clock+gate,e.target,static_cast<u8>(pitch),hold};
    if (!hold && e.target!=source.track()) arm_receiver(r,v,lease,gate,source.track());
    r.cursor=slot+1;
    r.probe=Probe::waiting;
    r.probePitch=pitch;
    r.probeSteps=0;
}

static u32 next_id(u32& sequence) {
    if (!++sequence) ++sequence;
    return sequence;
}

template<class Port>
static void watch_sender(Port& source,u32 target) {
    Port destination=source.at(target);
    if (destination.valid()) destination.memory().wakeSources|=1u<<source.track();
}

template<class Port>
static void handoff(State& s,Port& source,const Controls& c,Event next) {
    const u8 visited=next.visited;
    next.kind=Kind::launch;
    next.step=0;
    for (u32 k=0;k<4;k+=2) {
        const u32 dest=choice(c.shift_value[k],9);
        if (!dest || (visited & (1u<<(dest-1)))) continue;
        next.target=dest-1;
        next.due=s.clock+delay_steps(choice(c.shift_value[k+1],delayChoices),c.bpm_x10);
        if (!schedule(s,next)) continue;
        watch_sender(source,next.target);
        if (next.due!=s.clock) { s.route=Route::pending; s.routeTrack=next.target; }
    }
}

template<class Port>
static void send_step(State& s,Port& source,const Controls& c,Event step,u32 pitch,u32 gate) {
    step.id=next_id(source.sequence());
    step.note=static_cast<u8>(pitch);
    step.fading=false;
    step.duration=gate;
    step.released=true;
    handoff(s,source,c,step);
}

template<class Port>
static void finish_spray(State& s,Port& source,const Event& e) {
    // Each spray owns its local gates independently, even on converging routes.
    Port output=source.at(e.target);
    for (u32 i=0;i<s.leaseCount;) {
        const Lease& lease=s.leases[i];
        if (lease.id!=e.sprayOwner) { ++i; continue; }
        if (output.valid()) stop_voice(output.memory(),lease,output);
        s.leases[i]=s.leases[--s.leaseCount];
    }
}

template<class Port>
static void spray_step(State& s,Port& source,Port& output,const Controls& c,Event e,u32 stamp) {
    u32 gate=e.gate;
    u8 velocity=e.velocity;
    if (e.fading) {
        const u32 span=tail_stamps(e.tail);
        const u32 elapsed=stamp-e.fadeStamp;
        const u32 left=elapsed<span?span-elapsed:0;
        velocity=e.velocity*left/span;
        if (left<=1) { finish_spray(s,source,e); return; }
        const u32 tempo=(c.bpm_x10>=200 && c.bpm_x10<=3000)?c.bpm_x10:(s.tempo?s.tempo:1200u);
        const u32 remainingSteps=(left*tempo+7628)/7629;
        if (remainingSteps<gate) gate=remainingSteps;
    }
    Event continuation=e;
    advance_spray(continuation,1);
    schedule(s,continuation); // Reserve the existing tail before offering new handoffs.
    const u32 slot=spray_slot(e);
    const i32 pitch=e.note+e.transpose+output.grid().pitch(slot);
    if (pitch>=0 && pitch<=127) {
        Event hit=e;
        // Local Spray keeps the input dynamics. Steps keeps its outgoing velocity contour.
        hit.id=e.sprayOwner;
        sound(s,source,output,hit,pitch,false,gate,slot);
        if (e.sendSteps && velocity) {
            hit.velocity=velocity;
            send_step(s,source,c,hit,pitch,gate);
        }
    }
}

template<class Port>
static void perform(State& s,Port& source,Event e,u32 stamp) {
    Port output=source.at(e.target);
    if (!output.valid()) { s.route=Route::missing; s.routeTrack=e.target; return; }
    if (e.target!=source.track()) resume_input(output.memory(),output);
    const bool launch=e.kind==Kind::launch;
    Controls c;
    if (launch || e.sendSteps || (e.kind==Kind::sprayStep && e.fading)) c=output.controls();
    if (e.kind==Kind::sprayStep) {
        spray_step(s,source,output,c,e,stamp);
        return;
    }
    bool arp=true;
    if (launch) {
        if (source.track()!=e.target) {
            s.route=Route::delivered; s.routeTrack=e.target;
            output.memory().route=Route::received; output.memory().routeTrack=source.track();
        }
        if (c.bpm_x10>=200 && c.bpm_x10<=3000) {
            s.tempo=static_cast<u16>(c.bpm_x10);
            output.memory().tempo=s.tempo;
        }
        const Playhead mode=playhead(c);
        arp=mode!=Playhead::note;
        e.order=0;
        e.start=s.clock;
        e.visited|=1u<<e.target;
        e.sendSteps=arp && (c.clicks[0]&1);
        e.repeat=mode==Playhead::arp && (c.clicks[2]&1);
        if (arp) {
            e.mask=output.grid().mask();
            e.rests=(c.clicks[3]&1)!=0;
            e.length=phrase_length(e.mask,e.rests);
            if (!e.length) return;
            if (mode==Playhead::arp && output.grid().shuffle) e.order=deal(e,s.clock);
            e.rate=rate_steps[choice(c.value[2],4)];
            e.gate=e.rate*(1+choice(c.value[3],4))/4;
        }
        if (!e.sendSteps) handoff(s,source,c,e);
        else if (e.rests && !(e.mask&1)) {
            // A leading rest has no note to dispatch. A running destination
            // can still service the originating phrase during this interval.
            for (u32 k=0;k<4;k+=2) {
                const u32 dest=choice(c.shift_value[k],9);
                if (dest && !(e.visited&(1u<<(dest-1)))) watch_sender(source,dest-1);
            }
        }
        if (mode==Playhead::spray) {
            e.kind=Kind::sprayStep;
            e.tail=output.grid().tail;
            e.sprayOwner=next_id(source.sequence());
            e.fading=false;
            e.transpose=output.grid().transpose;
            e.repeat=output.grid().shuffle;
            e.due=s.clock;
            advance_spray(e,0);
            if (e.due==s.clock) spray_step(s,source,output,c,e,stamp);
            else schedule(s,e);
            return;
        }
    }
    if (e.repeat && e.released && ready(s.clock,e.start+e.duration)) return;
    const u32 slot=arp?phrase_slot(e.mask,e.step,e.rests):0;
    const u32 pitchSlot=e.order?(e.order>>(4*slot))&7u:slot;
    const i32 pitch=arp?e.note+output.grid().pitch(pitchSlot):e.note;
    const u32 gate=arp ? e.gate : e.duration;
    if ((!arp || (e.mask&(1u<<slot))) && pitch>=0 && pitch<=127) {
        sound(s,source,output,e,pitch,!arp && !e.released,gate,pitchSlot);
        if (e.sendSteps) send_step(s,source,c,e,pitch,gate);
    }
    if (arp && (++e.step<e.length || e.repeat)) {
        e.kind=Kind::arpStep;
        e.due=s.clock+e.rate;
        if (e.step==e.length) {
            e.step=0;
            if (e.order) e.order=deal(e,e.due);
        }
        if (!e.repeat || !e.released || !ready(e.due,e.start+e.duration)) schedule(s,e);
    }
}

template<class Port>
static void drain(State& s,Port& source,u32 stamp) {
    // Iterative zero-delay forks. A converging launch is delivered once for
    // this input and target on this tick. No recursive calls between players.
    u32 ids[eventCapacity]; u8 targets[eventCapacity]; u32 n=0;
    for (u32 i=0;i<s.eventCount;) {
        Event& pending=s.event(i);
        if (pending.kind==Kind::sprayStep && pending.released) {
            begin_spray_release(source,pending,s.clock,stamp,s.tempo?s.tempo:1200);
            if (pending.fading && stamp-pending.fadeStamp>=tail_stamps(pending.tail)) {
                finish_spray(s,source,pending);
                pending=s.event(--s.eventCount);
                continue;
            }
        }
        if (!ready(s.clock,pending.due)) { ++i; continue; }
        Event e=s.event(i);
        s.event(i)=s.event(--s.eventCount);
        if (e.kind==Kind::launch) {
            u32 j=0;
            while (j<n && (ids[j]!=e.id || targets[j]!=e.target)) ++j;
            if (j<n) continue;
            if (n==eventCapacity) { s.route=Route::full; continue; }
            ids[n]=e.id; targets[n++]=e.target;
        }
        perform(s,source,e,stamp);
    }
}

template<class Port>
static void release_key(State& s,Port& source,u32 note) {
    resume_input(s,source);
    for (u32 k=0;k<s.keyCount;++k) {
        const Input key=s.keys[k];
        if (key.note!=note) continue;
        // Stock consumes new records on its next player step. A press
        // and release between callbacks must span one playback opportunity.
        const u32 elapsed=s.clock-key.start;
        const u32 duration=elapsed?elapsed:1;
        for (u32 i=0;i<s.eventCount;) {
            Event& e=s.event(i);
            if (e.id==key.id && !e.released) {
                e.released=true;
                e.duration=duration;
                if (e.kind==Kind::sprayStep) begin_spray_release(source,e,s.clock,source.time_stamp(),s.tempo?s.tempo:1200);
                if (e.kind==Kind::arpStep && e.repeat && ready(e.due,e.start+duration)) {
                    s.event(i)=s.event(--s.eventCount);
                    continue;
                }
            }
            ++i;
        }
        for (u32 i=0;i<s.leaseCount;++i) {
            Lease& l=s.leases[i];
            if (l.id!=key.id || !l.held) continue;
            l.held=false;
            l.until=l.start+duration;
            if (l.target==source.track() || ready(s.clock,l.until)) continue;
            Port output=source.at(l.target);
            if (!output.valid()) continue;
            State& r=output.memory();
            observe_clock(r,output.time_stamp(),ClockCall::input);
            for (u32 j=0;j<r.sounding;++j) {
                if (r.voices[j].ticket!=l.ticket) continue;
                arm_receiver(r,r.voices[j],l,l.until-s.clock,source.track());
                break;
            }
        }
        s.keys[k]=s.keys[--s.keyCount];
        expire(s,source);
        return;
    }
}

template<class Port>
static void press_key(State& s,Port& source,u8 note,u8 velocity) {
    s.track=static_cast<u8>(source.track());
    resume_input(s,source);
    for (u32 i=0;i<s.keyCount;++i) {
        if (s.keys[i].note!=note) continue;
        if (s.keys[i].start==s.clock) return;
        release_key(s,source,note);
        break;
    }
    if (s.keyCount==8 || s.eventCount==eventCapacity) { s.route=Route::full; return; }
    const u32 id=next_id(source.sequence());
    s.keys[s.keyCount++]={id,s.clock,note};
    s.route=Route::off;
    Event e={};
    e.id=id; e.due=s.clock; e.note=note; e.velocity=velocity; e.target=source.track();
    if (source.grid().capture.record(note)) {
        // Monitor the actual key. The ordinary input and lease keep its
        // release valid even if the user saves the take while holding it.
        sound(s,source,source,e,note,true,0,0);
        return;
    }
    schedule(s,e);
    drain(s,source,source.time_stamp());
}

template<class Port>
static void tick(State& s,Port& source) {
    if (s.probe==Probe::idle) s.probe=Probe::clockOnly;
    const bool busy=s.keyCount || s.eventCount || s.leaseCount || s.sounding;
    const u32 stamp=busy || s.wakeSources?source.time_stamp():0;
    if (busy) observe_clock(s,stamp,ClockCall::step);
    else s.stampValid=false;
    if (s.leaseCount) expire(s,source);
    if (s.sounding) check_releases(s,source);
    if (s.eventCount) drain(s,source,stamp);
    // Same sequencer role as the sender. Run due work once, without
    // synthesizing player callbacks or stepping another track's stock player.
    for (u32 pending=s.wakeSources,k=0;pending;++k,pending>>=1) {
        if (!(pending&1)) continue;
        Port origin=source.at(k);
        if (!origin.valid() || !origin.memory().eventCount) {
            s.wakeSources&=~(1u<<k);
            continue;
        }
        State& owner=origin.memory();
        if (observe_clock(owner,stamp,ClockCall::peer)) {
            expire(owner,origin);
            drain(owner,origin,stamp);
        }
    }
    ++s.clock;
}
static inline void reset(State& s) { s=State{}; s.probe=Probe::cleared; }

static inline const char* probe_text(Probe probe) {
    switch (probe) {
        case Probe::idle: return "waiting for key and clock";
        case Probe::clockOnly: return "clock seen / waiting for key";
        case Probe::noPitch: return "invalid pitch";
        case Probe::noVelocity: return "invalid velocity";
        case Probe::waiting: return "tone queued / waiting for clock";
        case Probe::stepped: return "clock seen / tone unconfirmed";
        case Probe::consumed: return "SDK tone seen";
        case Probe::cleared: return "player cleared";
    }
    return "";
}
}
