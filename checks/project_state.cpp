// Stock IO boundary double; use the SDK's actual companion reader and writer.
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <project_companion.cpp>
#include "project_master.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../hocket/player.cpp"
#pragma GCC diagnostic pop
#include "hocket_settings.hpp"
using namespace opxy;
using platform::Address;
static hocket_settings::Projects setups;
static_assert(sizeof(hocket_data)==hocket_settings::bytes,"companion size disagrees with player");
alignas(4) static u8 buffers[2][0x54076c];
static void* active=buffers[0];
static u32 loading, deserializeResult=1, clears;
static u32 stock_save(void*,const char* path) {
    FILE* f=std::fopen(path,"wb");
    assert(f); std::fputs(path,f); return std::fclose(f)==0;
}
static u32 stock_load(void*,const char*) { return deserializeResult; }
static u32 stock_clear(u32,u32,u32) { ++clears; return 11; }
const platform::CompanionMachine sdk_companion_machines[]={{nullptr,nullptr,nullptr,false}};
extern "C" {
u32 sdk_project_kept(void*) { return 0; }
void sdk_project_loaded(const char*) {}
u32 sdk_project_locks_size(void*,u32) { return 0; }
void sdk_project_locks_write(void*) {}
void sdk_project_locks_begin() {}
void sdk_project_locks_byte(u32) {}
void sdk_project_locks_loaded(void*,u32) {}
bool sdk_hocket_save(const void* p,u8* dst) {
    if (p!=active) return false;
    setups.save(p,dst); return true;
}
void sdk_hocket_load(const void* p,const u8* src) { setups.load(active,p,src); }
void sdk_hocket_clear() { setups.clear(); }
}
Address platform::host_stock_address(Address address) {
    switch (address) {
    case 0x8fed0348: return reinterpret_cast<Address>(&std::fopen);
    case 0x8fed05bc: return reinterpret_cast<Address>(&std::fread);
    case 0x8fed0780: return reinterpret_cast<Address>(&std::fwrite);
    case 0x8fed00ae: return reinterpret_cast<Address>(&std::fclose);
    case 0x8d599074: return reinterpret_cast<Address>(&stock_save);
    case 0x8d599b10: return reinterpret_cast<Address>(&stock_load);
    case 0x840a0528: return reinterpret_cast<Address>(&loading);
    case 0x8d588114: case 0x8d588988: return reinterpret_cast<Address>(&stock_clear);
    }
    std::abort();
}

static void set_track(u32 t,u32 variant) {
    auto& s=setups.track(active,t);
    for (u32 k=0;k<4;++k) s.routeWords[k]=s.centre(k,(t+variant+k)%s.count(k));
    s.flags=(t^variant)&13; s.grid.tail=(t+variant)%6;
    s.grid.shuffle=(t+variant)&1; s.grid.transpose=static_cast<i32>(t+variant)-20;
    s.grid.pattern().changedMask=t*7+variant;
    for (u32 k=0;k<8;++k) s.grid.pattern().set_pitch(k,static_cast<i32>(t+variant+k)-18);
}
static void check(const u8* expected) {
    u8 live[hocket_settings::bytes]; setups.save(active,live);
    assert(!std::memcmp(live,expected,sizeof live));
}
static void load_project(const char* path,const u8* old,const u8* next) {
    void* inactive=active==buffers[0]?buffers[1]:buffers[0];
    assert(sdk_project_load(inactive,path)); check(old);
    active=inactive; check(next);
}
int main() {
    u8 a[hocket_settings::bytes],b[sizeof a],defaults[sizeof a];
    setups.save(active,defaults);
    for (u32 t=0;t<8;++t) set_track(t,1);
    auto& g=setups.track(active,0).grid;
    g.page=1; g.editEnabled=true; ++g.capture.session; g.capture.record(99);
    setups.save(active,a); assert(sdk_project_save(active,"songA.xy"));
    for (u32 t=0;t<8;++t) set_track(t,5);
    setups.save(active,b); assert(sdk_project_save(active,"songB.xy"));
    load_project("songA.xy",b,a);
    auto& restored=setups.track(active,0).grid;
    assert(!restored.page && !restored.editEnabled && !restored.capture.armed());
    load_project("songB.xy",a,b);
    setups.clear(); active=buffers[0]; load_project("songA.xy",defaults,a);
    // Failed stock load and an abandoned preload must preserve the active settings.
    deserializeResult=0; assert(!sdk_project_load(buffers[0],"songB.xy")); check(a);
    deserializeResult=1; assert(sdk_project_load(buffers[0],"songB.xy")); check(a);
    assert(sdk_project_save(active,"workspace.xy"));
    loading=1; assert(sdk_hocket_new(0,0,0)==11); check(a);
    loading=0; assert(sdk_hocket_random(0,0,0)==11); check(defaults); assert(clears==2);
    load_project("workspace.xy",defaults,a);
    FILE* f=std::fopen("songB.xy","ab"); assert(f); std::fputs("changed",f); std::fclose(f);
    load_project("songB.xy",a,defaults);
    load_project("songA.xy",defaults,a);
    assert(!std::remove("songB.oxy")); load_project("songB.xy",a,defaults);
    // An out-of-range field in the final record invalidates the entire input.
    b[sizeof b-1]=97; void* dest=active==buffers[0]?buffers[1]:buffers[0];
    setups.load(active,dest,b); active=dest; check(defaults);
    std::puts("PASS: eight tracks, two project files, workspace restart, failed and abandoned loads, missing and mismatched companions, new project, transient capture.");
}
