// Build against the patched SDK platform directory. Substitute only the stock
// callees. The real bridge encodes, dispatches, binds, promotes and observes.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include "stock.hpp"
namespace opxy { namespace platform {
void* probe_lookup(u32);
template<class F> F* probe_stock(u32 a) { return reinterpret_cast<F*>(probe_lookup(a)); }
}}
#define stock probe_stock
#include "hocket_probe.cpp"
#undef stock

alignas(4) static u8 playerBytes[0x380], voice[0x140];
static u8 message[24], releaseMessage[17];
static u32 audioTime;
extern "C" u32 sdk_voice_ticks(){return audioTime;}
static u32 nextMode, expectedSource, expectedOwner;
static bool injectBoundsCheck;
static u32 onGui;
static u32 role() { return onGui; }
static int outgoing=-1;
struct Frame { u8 bytes[20]; };
static u32 enqueue(void* queue,u32 type,const u8* input,u32 size,u32 flags,u32 src,u32 dst) {
    if (type==reportType) {
        assert(queue==platform::at<void>(0x841460b4) && (size==requestSize || size==replySize || size==13));
        assert(!flags && src==255 && dst==255);
        if (size==13) {
            releaseMessage[2]=type; releaseMessage[3]=size;
            std::memcpy(releaseMessage+4,input,size);
            return 97;
        }
        assert(outgoing>=0);
        Frame f={}; f.bytes[2]=type; f.bytes[3]=size;
        std::memcpy(f.bytes+4,input,size);
        assert(write(outgoing,&f,sizeof f)==sizeof f);
        return 97;
    }
    assert(type==0x98 && (size==8 || size==16 || size==20) && !flags && src==255 && dst==255);
    message[2]=static_cast<u8>(type); message[3]=static_cast<u8>(size);
    std::memcpy(message+4,input,size);
    return 97;
}
static u32 note(u32 ctx,u32 track,u32 pitch,u32 a,u32 b,u32 vel,
                u32 d,u32 e,u32 f,u32 g,u32 h,u32 j) {
    assert(ctx==9 && track==1 && pitch==60 && a==2 && b==3 && vel==78);
    assert(d==4 && e==5 && f==6 && g==7 && h==8 && j==9);
    const u8 p[]={1,60,78,51,99,26,1,0};
    return sdk_hprobe_queue(nullptr,0x98,p,8,0,255,255);
}
static void step(void* obj) {
    auto& t=records(obj)[0].tones[0];
    // Stock emitter's P5 is the address of the pitch. Include a marker
    // from outside this object's chord records to test capture isolation.
    u8 fake[]={60,78,t.unused[0],t.unused[1]};
    const u8* ptr=injectBoundsCheck ? fake : &t.pitch;
    assert(sdk_hprobe_emit(9,1,60,2,3,78,4,5,6,7,8,9,ptr)==97);
}
static void* copy13(void* d,const void* s,u32 n) { return std::memcpy(d,s,n); }
static u32 dispatch(void*,const u8* m) {
    assert(m[4]==1 && m[5]==60 && m[6]==78 && m[8]==99);
    assert(receiving.owner==expectedOwner);
    if (expectedOwner) assert(receiving.source==expectedSource && receiving.dest==1);
    voice[0x55]=1;
    if (nextMode==0) sdk_hprobe_fresh(voice+0x64,voice+0x54,13);
    else sdk_hprobe_pending(voice,1);
    return 21;
}
static u32 finishRender;
static u32 render(void*,i32* out) {
    for (u32 i=0;i<128;i+=2) { out[i]=0x1234567; out[i+1]=-0x1234567; }
    return finishRender;
}
void* opxy::platform::probe_lookup(u32 a) {
    switch (a) {
        case 0x8d5c3eac: return reinterpret_cast<void*>(&role);
        case platform::kMaestroStep: return reinterpret_cast<void*>(&step);
        case 0x8d595738: return reinterpret_cast<void*>(&note);
        case 0x11a00ec0: return reinterpret_cast<void*>(&enqueue);
        case 0x8d5b1260: return reinterpret_cast<void*>(&dispatch);
        case 0x040230c4: return reinterpret_cast<void*>(&copy13);
        case 0x8d51bbaa: return reinterpret_cast<void*>(&render);
        default: assert(false); return nullptr;
    }
}
static void attack(u32 source,u32 owner,u32 fade=0,u32 start=0) {
    sdk_hprobe_stamp(playerBytes,1,60,source,owner,fade,start);
    sdk_hprobe_step(playerBytes);
    expectedOwner=injectBoundsCheck ? 0 : owner;
    expectedSource=source;
    assert(message[3]==(expectedOwner?(fade?20:16):8));
    assert(sdk_hprobe_dispatch(nullptr,message)==21);
    assert(!receiving.owner && !sending.owner && !stepping);
}
static void simulateAudio() {
    reinterpret_cast<u32*>(playerBytes)[1]=1;
    *reinterpret_cast<u32*>(playerBytes+0x34c)=1;
    auto& r=records(playerBytes)[0]; r.root=60; r.count=1; r.tones[0]={0,60,78,{0,0}};
    attack(0,0xface1234);
    assert(watches.size==1 && watches.entries[0].active.owner==0xface1234);
    nextMode=1;
    attack(2,0xface1234); // Source-local IDs may coincide on separate tracks.
    assert(watches.entries[0].active.source==0 && watches.entries[0].pending.source==2);
    i32 out[128]={};
    assert(sdk_hprobe_render(voice,out)==0 && out[0]==0x1234567);
    assert(lastOwner[1]==0x11234 && rendered[1]==1 && watches.entries[0].pending.source==2);
    sdk_hprobe_promote(voice+0x54,voice+0x64,13);
    assert(watches.entries[0].active.source==2);
    sdk_hprobe_render(voice,out); sdk_hprobe_render(voice,out);
    assert(lastOwner[1]==0x31234 && watches.size==1 && received[1]==2 && rendered[1]==2 && sent[1]==2);
    // Fresh -> promotion before first render; ordinary reuse replaces an
    // unrendered tagged pending note, including when track and pitch match.
    nextMode=0; attack(3,78);
    sdk_hprobe_promote(voice+0x54,voice+0x64,13);
    sdk_hprobe_render(voice,out); assert(rendered[1]==3);
    nextMode=1; attack(2,79);
    injectBoundsCheck=true; attack(5,999);
    sdk_hprobe_promote(voice+0x54,voice+0x64,13);
    sdk_hprobe_render(voice,out); assert(rendered[1]==3 && watches.size==0);
    // A stock note has no trailer. A wrong marker, invalid source, and
    // unsupported payload length must also leave stock notes untagged.
    u8 malformed[20]={}; malformed[2]=0x98; malformed[3]=16;
    assert(!hprobe::decode(malformed).owner);
    hprobe::encode(malformed+4,{99,8,0}); assert(!hprobe::decode(malformed).owner);
    malformed[15]=0; malformed[3]=8; assert(!hprobe::decode(malformed).owner);
    assert(sdk_hprobe_status(0)==0);
}
static void checkGain() {
    alignas(4) static u8 newer[0x140];
    injectBoundsCheck=false; nextMode=0;
    i32 out[128];
    for (u32 fade=1;fade<=6;++fade) {
        const u32 span=(hprobe::frames[fade]*1024u+7399)/14798u;
        const u32 owner=100+fade;
        audioTime=0xfffffff0;
        attack(0,owner);
        sdk_hprobe_render(voice,out); assert(out[0]==0x1234567 && out[127]==-0x1234567);
        sdk_hprobe_release(0,1,owner,fade,audioTime);
        // The two boards' clocks have unrelated origins.
        audioTime=8000;
        assert(sdk_hprobe_dispatch(nullptr,releaseMessage)==1);
        audioTime+=span/2;
        watches.bind(newer,{owner+1,0,1,0,0},true);
        sdk_hprobe_render(newer,out); assert(out[0]==0x1234567);
        sdk_hprobe_render(voice,out);
        assert(out[0]>0x1234567*49/100 && out[0]<0x1234567*51/100);
        assert(out[126]<out[0] && out[127]<0 && out[127]+out[126]>=-1);
        // A newly emitted attack contains enough information to start mid-tail.
        attack(0,owner,fade,8000);
        sdk_hprobe_render(voice,out);
        assert(out[0]>0x1234567*49/100 && out[0]<0x1234567*51/100);
        audioTime+=span+2;
        finishRender=47;
        assert(sdk_hprobe_render(voice,out)==47);
        for (i32 sample:out) assert(sample==0);
        assert(watches.find(voice)==watches.size);
        assert(sdk_hprobe_render(newer,out)==47 && out[0]==0x1234567);
        assert(!watches.size);
        finishRender=0;
    }
    // Gate-off can precede stock emitting an already staged note.
    sdk_hprobe_stamp(playerBytes,1,60,0,55,0,0);
    sdk_hprobe_release(0,1,55,4,audioTime);
    sdk_hprobe_step(playerBytes);
    assert(message[3]==20 && hprobe::decode(message,audioTime).fade==4);

}
static Frame take(int fd) {
    Frame f;
    assert(read(fd,&f,sizeof f)==sizeof f);
    return f;
}
int main() {
    int requests[2],replies[2];
    assert(pipe(requests)==0 && pipe(replies)==0);
    const pid_t child=fork();
    assert(child>=0);
    if (!child) {
        close(requests[1]); close(replies[0]);
        outgoing=replies[1];
        for (int i=0;i<4;++i) {
            const Frame request=take(requests[0]);
            if (i==1) simulateAudio();
            assert(sdk_hprobe_dispatch(nullptr,request.bytes)==1);
        }
        _exit(0);
    }
    close(requests[0]); close(replies[1]);
    outgoing=requests[1]; onGui=1;
    auto apply=[](Frame f){assert(sdk_hprobe_dispatch(nullptr,f.bytes)==1);};
    sdk_hprobe_poll(1,1);
    assert(sdk_hprobe_status(1)==0 && !sdk_hprobe_last(1));
    Frame empty=take(replies[0]); apply(empty);
    assert(sdk_hprobe_status(1)==(1u<<25) && !sdk_hprobe_last(1));
    sdk_hprobe_poll(1,2); // throttled
    sdk_hprobe_poll(1,1+pollInterval);
    Frame played=take(replies[0]);
    // Audio has run, but the GUI still sees its previous acknowledged snapshot.
    assert(sdk_hprobe_status(1)==(1u<<25) && !sdk_hprobe_last(1));
    apply(played);
    assert((sdk_hprobe_status(1)&0xffffff)==0x030400 && sdk_hprobe_last(1)==0x4004e);
    sdk_hprobe_poll(2,2+pollInterval);
    apply(played); // stale destination
    assert(!sdk_hprobe_status(2));
    Frame other=take(replies[0]); apply(other);
    assert(sdk_hprobe_status(2)==(1u<<25));
    sdk_hprobe_poll(2,2+6*pollInterval); // link timeout clears both readouts
    apply(other); // stale token, same destination
    assert(!sdk_hprobe_status(2) && !sdk_hprobe_last(2));
    Frame retry=take(replies[0]); apply(retry);
    assert(sdk_hprobe_status(2)==(1u<<25));
    // Unknown stock status subcommands and lengths must pass to stock.
    Frame ordinary={}; ordinary.bytes[2]=reportType; ordinary.bytes[3]=requestSize;
    assert(!report(ordinary.bytes));
    retry.bytes[3]=7; assert(!report(retry.bytes));
    onGui=0; simulateAudio(); checkGain();
    int status=0; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    std::puts("PASS: two-process telemetry, held and released stereo renders, six durations, same-pitch overlap, mid-tail attacks, slot reuse and silence on the final block.");
}
