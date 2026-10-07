#define wmain legacy_engine_smoke_entry
#include "engine_smoke.cpp"
#undef wmain
#include "nvof/scene_cut.hpp"
#include <limits>

namespace {
std::vector<int64_t> four_times(int64_t start) { return {start+200000,start+400000,start+600000,start+800000}; }
void check_motion(const nvof::PhaseBatch<nvof::Frame>& batch, const nvof::Frame& a,
                  const nvof::Frame& b, int base_shift, bool print=false) {
    require(!batch.quality.scene_cut, "Translation misclassified as a cut");
    require(batch.quality.repetition_known, "Actual NVIDIA repetition metadata unavailable");
    require(batch.quality.repeated_mask == 0, "Known motion unexpectedly repeated");
    for (size_t i=0;i<batch.frames.size();++i) {
        const auto& output=batch.frames[i];
        const int step=int((output.pts-a.pts)/100000);
        auto truth=scene(base_shift+step,output.pts), blend=a;
        for(size_t p=0;p<blend.pixels.size();++p)
            blend.pixels[p]=uint8_t((int64_t(a.pixels[p])*(b.pts-output.pts)+int64_t(b.pixels[p])*(output.pts-a.pts)+(b.pts-a.pts)/2)/(b.pts-a.pts));
        const auto actual=mse(output,truth), mixed=mse(blend,truth);
        if(print) std::cout<<"PHASE "<<step*10<<" MSE "<<actual<<" BLEND_MSE "<<mixed<<'\n';
        require(output.width==W&&output.height==H&&output.pixels.size()==a.pixels.size(), "Output layout changed");
        if(output.pixels==a.pixels||output.pixels==b.pixels) std::cerr<<"REPEAT base="<<base_shift<<" phase="<<step<<" pts="<<output.pts<<" mask="<<batch.quality.repeated_mask<<" MSE="<<actual<<" BLEND="<<mixed<<" LEFT="<<mse(output,a)<<" RIGHT="<<mse(output,b)<<'\n';
        require(output.pixels!=a.pixels&&output.pixels!=b.pixels, "Motion phase repeated an original");
        require(actual<mixed, "Motion phase did not improve known translation over temporal blend");
        for(size_t j=0;j<i;++j) require(output.pixels!=batch.frames[j].pixels, "Motion phases were duplicates");
    }
}
nvof::Frame static_pattern(int pattern, int64_t pts) {
    auto f=scene(0,pts);
    for(int y=0;y<H;++y) for(int x=0;x<W;++x) {
        const size_t i=size_t(y)*W+x;
        f.pixels[i]=pattern==0?uint8_t(16+x*219/(W-1)):pattern==1?uint8_t(16+(x/80)*30):uint8_t((x*3+y*5)&255);
    }
    for(int y=0;y<H/2;++y) for(int x=0;x<W;x+=2) {
        const size_t i=size_t(W)*H+size_t(y)*W+x;
        f.pixels[i]=pattern==0?128:pattern==1?uint8_t(16+(x/80)*30):uint8_t((x*7+y*9)&255);
        f.pixels[i+1]=pattern==0?128:pattern==1?uint8_t(240-(x/80)*30):uint8_t((x*11+y*3)&255);
    }
    return f;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc<2) { std::cerr<<"phase_engine_smoke <runtime directory>\n"; return 2; }
        nvof::FrucEngine engine{std::filesystem::path(argv[1])};
        std::cout<<"DEVICE "<<engine.device_name()<<'\n';
        auto a=scene(0,0), b=scene(10,1000000);
        auto batch=engine.interpolate_pair(a,b,four_times(0));
        require(batch.frames.size()==4,"Missing genuine phase outputs");
        check_motion(batch,a,b,0,true);
        auto held=std::move(batch.frames[0]); const auto held_bytes=held.pixels;
        batch.frames.clear();

        // Fewer/zero outputs still advance every previously created history;
        // additional histories are primed with the current pair's original A.
        auto previous=b; int shift=10;
        for(const size_t count:{1u,0u,2u,6u,1u,4u}) {
            auto current=scene(shift+10,previous.pts+1000000);
            std::vector<int64_t> times;
            for(size_t p=1;p<=count;++p) times.push_back(previous.pts+int64_t(p)*100000);
            auto varying=engine.interpolate_pair(previous,current,times);
            require(varying.frames.size()==count,"Variable phase count was lost");
            if(count) check_motion(varying,previous,current,shift);
            else require(!varying.quality.repetition_known,"Empty batch invented output repetition metadata");
            previous=std::move(current); shift+=10;
        }
        // Same PTS, different pixels must not reuse the cached predecessor.
        auto replaced=scene(160,previous.pts), replacement_b=scene(170,previous.pts+1000000);
        auto replacement=engine.interpolate_pair(replaced,replacement_b,four_times(replaced.pts));
        check_motion(replacement,replaced,replacement_b,160);
        // PTS near INT64_MAX cannot lose phase precision through double conversion.
        const auto huge=(std::numeric_limits<int64_t>::max)()-2000000;
        auto huge_a=scene(0,huge),huge_b=scene(10,huge+1000000);
        auto huge_batch=engine.interpolate_pair(huge_a,huge_b,four_times(huge));
        check_motion(huge_batch,huge_a,huge_b,0);
        for(int i=0;i<100;++i) {
            engine.reset();
            const int offset=(i%20)*7; auto seek_a=scene(offset,0),seek_b=scene(offset+10,1000000);
            auto seek=engine.interpolate_pair(seek_a,seek_b,four_times(0));
            check_motion(seek,seek_a,seek_b,offset);
        }

        for(int pattern=0;pattern<3;++pattern) {
            engine.reset(); auto sa=static_pattern(pattern,0),sb=sa; sb.pts=1000000;
            auto exact=engine.interpolate_pair(sa,sb,four_times(0));
            require(exact.frames.size()==4&&!exact.quality.scene_cut&&exact.quality.repetition_known,"Static phase quality missing");
            for(size_t p=0;p<exact.frames.size();++p) {
                const auto& output=exact.frames[p];
                if(output.pixels!=sa.pixels) {
                    size_t changed=0; int largest=0;
                    for(size_t k=0;k<sa.pixels.size();++k) { const int delta=std::abs(int(output.pixels[k])-int(sa.pixels[k])); changed+=delta!=0; largest=(std::max)(largest,delta); }
                    std::cerr<<"STATIC pattern="<<pattern<<" phase="<<p<<" changed="<<changed<<" max_delta="<<largest<<" mask="<<exact.quality.repeated_mask<<'\n';
                }
                require(output.pixels==sa.pixels,"Static NV12 Y/UV was not byte-exact");
            }
        }

        engine.reset(); auto cut_a=a,cut_b=b;
        for(int y=0;y<H;++y) for(int x=0;x<W;++x) {
            cut_a.pixels[size_t(y)*W+x]=uint8_t(16+mix(uint32_t(x/32)*733U+uint32_t(y/32)*193U)%96);
            cut_b.pixels[size_t(y)*W+x]=uint8_t(176+mix(uint32_t(x/32)*313U+uint32_t(y/32)*977U+1U)%60);
        }
        auto cut=engine.interpolate_pair(cut_a,cut_b,four_times(0));
        require(cut.quality.scene_cut&&cut.frames.empty(),"Hard cut did not close history and bypass motion");
        auto after_cut=cut_b; after_cut.pts=2000000;
        auto recovered=engine.interpolate_pair(cut_b,after_cut,four_times(1000000));
        require(!recovered.quality.scene_cut&&recovered.frames.size()==4&&recovered.quality.repetition_known,"Cut recovery did not prime new histories");
        for(const auto& output:recovered.frames) require(output.pixels==cut_b.pixels,"Cut recovery retained old scene pixels");
        auto flash=a;
        for(size_t p=0;p<size_t(W)*H;++p) flash.pixels[p]=uint8_t((std::min)(255,int(flash.pixels[p])+25));
        flash.pts=1000000;
        require(!engine.interpolate_pair(a,flash,{500000}).quality.scene_cut,"Uniform flash misclassified as a hard cut");

        std::vector<int64_t> too_many; for(int i=1;i<=33;++i) too_many.push_back(int64_t(i)*10000);
        for(const auto& invalid:std::vector<std::vector<int64_t>>{{0},{1000000},{200000,200000},{600000,400000},too_many}) {
            bool rejected=false;
            try { engine.interpolate_pair(a,b,invalid); } catch(const std::invalid_argument&) { rejected=true; }
            require(rejected,"Invalid output phase timestamps were accepted");
        }
        auto resumed=engine.interpolate_pair(a,b,four_times(0)); check_motion(resumed,a,b,0);
        engine.reset(); engine.reset();
        require(held.pixels==held_bytes,"Later pairs or reset overwrote an owned output");
        nvof::Frame surviving;
        {
            nvof::FrucEngine reopened{std::filesystem::path(argv[1])};
            auto output=reopened.interpolate_pair(scene(0,0),scene(10,1000000),four_times(0));
            surviving=std::move(output.frames[0]);
        }
        require(surviving.pixels==held_bytes,"Engine destruction invalidated owned output");
        std::cout<<"PASS four genuine phases, actual repetition metadata, variable counts including zero, static NV12 exact, content identity, large PTS, cut/recovery, flash, 100 changing-scene seek/resets, validation and frame lifetime\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<'\n'; return 1; }
}
