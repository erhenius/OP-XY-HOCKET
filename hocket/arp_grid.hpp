#pragma once
#include <opxy/player.hpp>
namespace hocket {
using namespace opxy;
const i32 intervals[8]={0,7,12,19,0,0,0,0};

struct Pattern {
    volatile u8 changedMask;
    volatile signed char offsets[8];
    u8 mask() const { return changedMask^15u; }
    i32 pitch(u32 slot) const { return intervals[slot]+offsets[slot]; }
    void set_pitch(u32 slot,i32 pitch) {
        offsets[slot]=static_cast<signed char>(pitch-intervals[slot]);
    }
};

struct Take {
    u8 count, notes[8];
    u8 mask() const { return static_cast<u8>((1u<<count)-1); }
    i32 pitch(u32 slot) const {
        if (slot>=count) return 0;
        const i32 delta=static_cast<i32>(notes[slot])-notes[0];
        return delta>48?48:(delta< -48?-48:delta);
    }
};

// The GUI owns the session, the player owns the two published snapshots.
// A frame interrupted by note input can still read the preceding complete take.
struct Capture {
    volatile u32 session, published;
    struct Snapshot { volatile u32 session; volatile u8 count, notes[8]; } takes[2];
    bool armed() const { return (session&1u)!=0; }

    bool record(u8 note) {
        const u32 current=session;
        if (!(current&1u)) return false;
        const u32 next=published+1;
        const Snapshot& old=takes[(next-1)&1u];
        Snapshot& out=takes[next&1u];
        const u32 count=old.session==current?old.count:0;
        const u32 skip=count==8?1:0;
        for (u32 i=skip;i<count;++i) out.notes[i-skip]=old.notes[i];
        out.notes[count-skip]=note;
        out.count=count-skip+1;
        out.session=current;
        published=next;
        return true;
    }
    bool read(Take& take) const {
        const u32 current=session;
        for (u32 attempt=0;attempt<3;++attempt) {
            const u32 version=published;
            const Snapshot& in=takes[version&1u];
            take.count=in.session==current?in.count:0;
            for (u32 i=0;i<take.count;++i) take.notes[i]=in.notes[i];
            if (version==published) return true;
        }
        take.count=0;
        return false;
    }
};

struct Grid {
    volatile u8 page, active;
    volatile bool shuffle,editEnabled;
    volatile u8 tail; // One second of release at zero initialization.
    volatile signed char transpose;
    void turn_transpose(i32 delta) {
        const i32 value=transpose;
        transpose=delta>24-value?24:(delta< -24-value?-24:value+delta);
    }
    Pattern patterns[2];
    Capture capture;
    Pattern& pattern() { return patterns[active]; }
    u8 mask() const { return patterns[active].mask(); }
    i32 pitch(u32 slot) const { return patterns[active].pitch(slot); }
    void finish_capture() {
        Take take;
        if (!capture.read(take)) return; // Keep armed if three snapshots raced.
        if (take.count) {
            const u8 next=active^1u;
            Pattern& result=patterns[next];
            for (u32 i=0;i<8;++i) result.set_pitch(i,take.pitch(i));
            result.changedMask=take.mask()^15u;
            active=next; // Publish the complete pattern in one store.
        }
        ++capture.session;
    }
    bool click(u32 k) {
        if (page && k==2) {
            if (capture.armed()) finish_capture();
            else ++capture.session;
            return true;
        }
        if (capture.armed() && k==3) {
            ++capture.session;
            return true;
        }
        if (page && k==0) {
            if (!capture.armed()) shuffle=!shuffle;
            return true;
        }
        if (page && k==3) { editEnabled=!editEnabled; return true; }
        if (k!=1) return page!=0;
        page=!page;
        editEnabled=false;
        if (!page && capture.armed()) ++capture.session;
        return true;
    }
    bool turn(u32 k,i32 delta,bool shift) {
        if (!page || k>=4) return false;
        if (!delta || capture.armed()) return true;
        const u32 slot=(shift?4u:0u)+k;
        if (editEnabled) {
            u8 enabled=mask();
            if (delta>0) enabled|=1u<<slot;
            else enabled&=~(1u<<slot);
            pattern().changedMask=enabled^15u;
        } else {
            const i32 value=pitch(slot);
            const i32 next=delta>48-value?48:(delta< -48-value?-48:value+delta);
            pattern().set_pitch(slot,next);
        }
        return true;
    }
};

// Fisher-Yates over enabled slots only. One nibble per grid position
// lets each in-flight phrase carry its whole pass in a single word.
static inline u32 shuffled_order(u8 mask,u32 seed) {
    u8 slots[8];
    u32 count=0;
    for (u32 slot=0;slot<8;++slot)
        if (mask&(1u<<slot)) slots[count++]=static_cast<u8>(slot);

    u32 random=seed?seed:0x9e3779b9u;
    for (u32 n=count;n>1;--n) {
        random^=random<<13;
        random^=random>>17;
        random^=random<<5;
        const u32 pick=((random>>16)*n)>>16;
        const u8 last=slots[n-1];
        slots[n-1]=slots[pick];
        slots[pick]=last;
    }
    u32 order=0, next=0;
    for (u32 slot=0;slot<8;++slot) {
        const u32 pitchSlot=(mask&(1u<<slot))?slots[next++]:slot;
        order|=pitchSlot<<(4*slot);
    }
    return order;
}

static inline u8 phrase_length(u8 mask,bool rests) {
    u8 length=0;
    for (;mask;mask>>=1) length+=rests?1:(mask&1);
    return length;
}
static inline u8 phrase_slot(u8 mask,u8 step,bool rests) {
    if (rests) return step;
    for (u8 slot=0;slot<8;++slot) {
        if (!(mask&(1u<<slot))) continue;
        if (!step) return slot;
        --step;
    }
    return 8;
}
}
