#include "nvof/seek_preview.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}
}
int main() {
    try {
        nvof::SeekPreviewWindow preview;
        require(!preview.active(1,0),"Startup must not add a preview interval");
        // Decode one second of video in a small burst. The old wall-only policy
        // bypassed every frame, independent of the amount of video queued.
        for (unsigned duration : {100u,200u}) for (int rate : {24,25,30,60}) {
            preview.begin(1000,duration);
            int bypassed=0;
            for (int frame=0; frame<rate; ++frame) {
                const int64_t pts=10000000LL*frame/rate;
                if (preview.active(1000+frame,pts)) ++bypassed;
            }
            require(bypassed==int((rate*duration+999)/1000),"Burst decoding extended source-rate video beyond the media bound");
            require(!preview.active(1100,0),"Expired preview restarted on an older timestamp");
        }
        preview.begin(2000,200);
        require(preview.active(2000,900000000),"First visible original was withheld");
        require(preview.active(2199,900400000),"Low-rate input lost the short settling interval");
        require(!preview.active(2200,900800000),"Wall-clock deadline failed");
        preview.begin(2300,100);
        require(preview.active(2399,400000),"Short preview ended before its deadline");
        require(!preview.active(2400,800000),"Short preview kept the old 200ms deadline");
        // Rapid forward/backward and segment-relative seeks each get fresh
        // state. A 120ms drag interval remains protected for its first frames.
        for (unsigned duration : {100u,200u}) for (int64_t origin : {900000000LL,0LL,400000000LL,1000000LL}) {
            preview.begin(3000,duration);
            require(!preview.active(3000,-400000),"Preroll was shown");
            for (int i=0;i<3;++i)
                require(preview.active(3000+i*40,origin+i*417083),"Repeated seek lost its preview");
            require(!preview.active(3000+duration,origin+3*417083),"Repeated seek did not expire");
        }
        preview.begin(4000,200);
        require(!preview.active(9000,0),"A pause extended the preview past its deadline");
        preview.begin(9000,200);
        require(preview.active(9000,1000000),"Seek after a pause did not rearm");
        preview.clear();
        require(!preview.active(9001,1400000),"Restart retained preview state");
        const auto maximum=(std::numeric_limits<int64_t>::max)();
        preview.begin(10000,200);
        require(preview.active(10000,maximum-2000000),"Large PTS rejected");
        require(!preview.active(10001,maximum),"Large PTS overflowed the media bound");
        preview.begin(11000,0);
        require(!preview.active(11000,0),"Zero duration enabled a preview");
        std::cout << "PASS seek preview: burst media bound, wall deadline, immediate original, preroll, rapid/reverse seeks, pause, restart, overflow\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
