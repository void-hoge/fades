// Duration-driven single-threaded streaming benchmark. Initial preparation,
// CPU reference and warmup/final full-content checks are outside timing.
#include "fades_stream.hpp"
#include "sudoku.hpp"
#include <array>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unordered_set>
#include <vector>

using Record=std::array<uint8_t,64>;
using fades::require;
struct Puzzle {reference::Board board{};bool solvable=false;};
static double parse_duration(const char* text) {
    char* end=nullptr;errno=0;double d=std::strtod(text,&end);
    require(end!=text && *end=='\0' && errno!=ERANGE && std::isfinite(d) && d>=0.001 && d<=86400,
            "Duration must be a finite number from 0.001 to 86400 seconds");
    return d;
}
static uint64_t record_id(const Record& r) {
    uint64_t id=0;for(unsigned b=0;b<8;++b)id|=uint64_t(r[b])<<(8*b);return id;
}
int main(int argc,char** argv) try {
    require(argc==3,"Usage: fades_benchmark PUZZLE_FILE SECONDS");
    const double duration=parse_duration(argv[2]);
    std::ifstream file(argv[1]);require(bool(file),"Cannot open puzzle file");
    std::vector<Puzzle> puzzles;
    std::vector<Record> corpus;
    std::string line;
    while(std::getline(file,line)) {
        if(line.find_first_not_of(" \r\t")==std::string::npos)continue;
        Puzzle p;p.board=reference::parse(line);Record r{};
        for(unsigned c=0;c<81;++c)if(reference::single(p.board[c]))
            r[8+c/2]|=uint8_t((std::countr_zero(p.board[c])+1)<<(4*(c%2)));
        puzzles.push_back(p);corpus.push_back(r);
    }
    require(!file.bad(),"Puzzle file read failed");require(!puzzles.empty(),"Puzzle file is empty");
    std::cout<<"Loaded "<<puzzles.size()<<" puzzles; preparing independent CPU references..."<<std::endl;
    for(auto& p:puzzles)p.solvable=reference::solve(p.board).solutions!=0;
    fades::Device dev;dev.open_device();
    constexpr unsigned per=10000;
    const unsigned slots=dev.info.slots;
    uint64_t tag=0;
    auto fill=[&](uint8_t* target,unsigned n,uint64_t first) {
        unsigned copied=0;
        while(copied<n) {
            size_t offset=(first+copied)%corpus.size();
            unsigned chunk=unsigned(std::min<size_t>(n-copied,corpus.size()-offset));
            fades::copy_records_with_ids(target+size_t(copied)*64,corpus[offset].data(),
                                         chunk,tag+first+copied);
            copied+=chunk;
        }
    };
    auto verify_record=[&](const Record& r,uint64_t total,std::unordered_set<uint64_t>& seen) {
        uint64_t id=record_id(r);
        require(id>=tag && id-tag<total,"Unknown or stale result ID");
        require(seen.insert(id).second,"Duplicate result ID");
        const auto& p=puzzles[(id-tag)%puzzles.size()];
        require(r[8]==(p.solvable ? 0 : 1),"Unexpected result status");
        if(p.solvable) {
            reference::Board answer{};
            for(unsigned c=0;c<81;++c) {
                unsigned d=(r[9+c/2]>>(4*(c%2)))&15;
                require(d>=1 && d<=9,"Invalid solved digit");answer[c]=reference::Mask(1u<<(d-1));
            }
            require(reference::valid_solution(p.board,answer),"Invalid Sudoku solution or changed hint");
        } else for(unsigned b=9;b<50;++b)require(r[b]==0,"Nonzero unsolved payload");
        require((r[49]&0xf0)==0,"Nonzero reserved nibble");
        for(unsigned b=50;b<64;++b)require(r[b]==0,"Nonzero reserved bytes");
    };
    tag=0x1234567800000000ULL;
    std::vector<Record> warmup;
    auto warm=fades::pump(dev,per,slots,uint64_t(per)*slots,0,fill,[&](unsigned index,unsigned n) {
        auto old=warmup.size();warmup.resize(old+n);
        std::memcpy(warmup.data()+old,dev.slot(FADES_QUEUE_RX,index),size_t(n)*64);
    });
    std::unordered_set<uint64_t> seen;
    for(const auto& r:warmup)verify_record(r,warm.received,seen);
    std::cout<<"Warmup verification PASS: "<<warm.received<<" IDs\n"
             <<"Streaming: "<<slots<<" slots x "<<per<<" records; requested="<<duration<<" s\n"
             <<"Refill/copy and unique ID generation are included; initial DDR fill is excluded."<<std::endl;
    tag=0xcafe987600000000ULL;
    std::array<unsigned,FADES_DMA_SLOTS> last_lengths{};
    auto run=fades::pump(dev,per,slots,std::numeric_limits<uint64_t>::max(),duration,fill,
        [&](unsigned index,unsigned n) {last_lengths[index]=n;});
    seen.clear();
    for(unsigned s=0;s<slots;++s)for(unsigned j=0;j<last_lengths[s];++j) {
        Record r;std::memcpy(r.data(),dev.slot(FADES_QUEUE_RX,s)+size_t(j)*64,64);
        verify_record(r,run.submitted,seen);
    }
    std::cout<<"Final resident-buffer verification PASS: "<<seen.size()<<" IDs\n"
             <<std::fixed<<std::setprecision(6)
             <<"Requested seconds: "<<duration<<'\n'
             <<"Actual timed seconds (including final drain): "<<run.elapsed<<'\n'
             <<"Completed requests: "<<run.received<<'\n'
             <<"Throughput: "<<run.received/run.elapsed<<" problem/s ("<<run.received/run.elapsed/1e6<<" Mproblem/s)\n"
             <<"Packet completions observed within requested window: "<<run.within<<'\n'
             <<"Observed-window throughput: "<<run.within/duration/1e6<<" Mproblem/s\n"
             <<"Final drain seconds: "<<run.drain<<'\n'
             <<"Deadline overrun seconds: "<<std::max(0.0,run.elapsed-duration)<<'\n'
             <<"Completed packets: "<<run.packets<<'\n'
             <<"Timed input refill seconds: "<<run.refill<<'\n'
             <<"FIFO write-domain peak: "<<run.stats.counters[30]<<'\n'
             <<"DMA input stall cycles: "<<run.stats.counters[2]<<'\n'
             <<"Input FIFO empty cycles: "<<run.stats.counters[5]<<'\n';
    for(unsigned g=0;g<6;++g)
        std::cout<<"Core "<<g<<": dispatched="<<run.stats.counters[6+g]<<", collected="<<run.stats.counters[12+g]
                 <<", input_wait="<<run.stats.counters[32+g]<<", output_wait="<<run.stats.counters[24+g]<<'\n';
    std::cout<<"Errors: 0\nCounts include repeated puzzles. Full content checks cover warmup and final resident buffers;\n"
             <<"the timed stream checks DMA lengths and final cumulative PL counts.\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
