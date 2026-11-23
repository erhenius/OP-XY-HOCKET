// Host check of the screen's fixed-point geometry for every enabled-slot mask.
#include "hocket_pitch_axis.hpp"
#include <cassert>
using namespace hocket_axis;
static void check(const i32 (&notes)[8]) {
    for (unsigned mask=0;mask<256;++mask) {
        Axis axis;
        axis.build(notes,mask);
        unsigned expected=0;
        for (int pitch=-48;pitch<=48;++pitch) {
            bool enabled=false;
            for (unsigned i=0;i<8;++i) if (notes[i]==pitch && (mask&(1u<<i))) enabled=true;
            bool found=false;
            for (unsigned i=0;i<axis.count;++i) if (axis.pitches[i]==pitch) found=true;
            assert(enabled==found);
            expected+=enabled;
        }
        assert(axis.count==expected);
        for (unsigned i=0;i<axis.count;++i) {
            assert(axis.labels[i]>=95*unit && axis.labels[i]<=177*unit);
            if (i) {
                assert(axis.labels[i]-axis.labels[i-1]>=8*unit);
                assert(axis.mark(axis.pitches[i])>axis.mark(axis.pitches[i-1]));
            }
        }
        if (axis.count==1) assert(axis.mark(axis.pitches[0])==136*unit);
        if (axis.count>1) assert(axis.mark(axis.pitches[0])==99*unit &&
                                 axis.mark(axis.pitches[axis.count-1])==173*unit);
    }
}
int main() {
    check({-48,0,47,48,0,12,-24,0});
    check({-48,42,43,44,45,46,47,48});
    check({48,-42,-43,-44,-45,-46,-47,-48});
    check({7,7,7,7,7,7,7,7});
}
