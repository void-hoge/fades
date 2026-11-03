// DMA buffers are coherent mappings; access only outside RUN ioctl.
#include "fades_ioctl.h"
#include "sudoku.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using Record=std::array<uint8_t,64>;
using Clock=std::chrono::steady_clock;
static void check(bool b,const char* msg) {if(!b) throw std::runtime_error(msg);}
static double seconds(Clock::time_point a,Clock::time_point b) {return std::chrono::duration<double>(b-a).count();}
static uint64_t get_id(const Record& r) {uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(r[i])<<(8*i);return v;}
static void set_id(Record& r,uint64_t id) {for(unsigned i=0;i<8;++i)r[i]=id>>(8*i);}
static void set_cell(Record& r,unsigned offset,unsigned c,unsigned v) {r[offset+c/2]|=v<<(4*(c%2));}
static unsigned cell(const Record& r,unsigned offset,unsigned c) {return (r[offset+c/2]>>(4*(c%2)))&15;}
struct Puzzle {Record record{};reference::Board input{},answer{};};
int main(int argc,char** argv) try {
    check(argc>=5,"Usage: fades_verify dataset records_per_packet slots rounds [mixed]");
    const unsigned per=std::stoul(argv[2]),slots=std::stoul(argv[3]),rounds=std::stoul(argv[4]);
    const bool mixed=argc>5 && std::string(argv[5])=="mixed";
    check(per && per<=16384 && slots && slots<=8 && rounds && rounds<=100000,"Invalid transfer dimensions");
    const unsigned count=per*slots;
    check(!mixed || count>=64,"Mixed test requires at least 64 records");
    const auto begin=Clock::now();
    std::ifstream dataset(argv[1]);check(bool(dataset),"Cannot open dataset");
    std::vector<Puzzle> puzzles;
    std::string line;
    while(std::getline(dataset,line)) {
        if(line.find_first_not_of(" \r\t")==std::string::npos)continue;
        Puzzle p;p.input=reference::parse(line);
        auto ref=reference::solve(p.input);check(ref.solutions==1,"CPU reference found no solution");p.answer=ref.first;
        for(unsigned c=0;c<81;++c)if(reference::single(p.input[c]))set_cell(p.record,8,c,std::countr_zero(p.input[c])+1);
        puzzles.push_back(p);
    }
    check(!puzzles.empty(),"Empty dataset");
    auto oracle_done=Clock::now();
    int fd=open("/dev/fades",O_RDWR|O_CLOEXEC);check(fd>=0,"Cannot open /dev/fades");
    fades_dma_info info{};check(ioctl(fd,FADES_DMA_INFO,&info)==0,"INFO failed");
    check(info.abi_version==FADES_DMA_ABI_VERSION && info.slot_bytes>=per*64 && info.slots>=slots,"ABI mismatch");
    auto* tx=static_cast<uint8_t*>(mmap(nullptr,info.map_bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));
    auto* rx=static_cast<uint8_t*>(mmap(nullptr,info.map_bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,info.map_bytes));
    check(tx!=MAP_FAILED && rx!=MAP_FAILED,"mmap failed");
    std::vector<Record> inputs(count);
    std::vector<unsigned> statuses(count);
    auto prepare=[&](uint64_t salt) {
        for(unsigned i=0;i<count;++i) {
            inputs[i]=puzzles[i%puzzles.size()].record;statuses[i]=0;
            if(mixed && i>=count-64) {
                inputs[i]={};
                if(i%2) {set_cell(inputs[i],8,i%81,10+(i/2)%6);statuses[i]=2;}
                else {set_cell(inputs[i],8,0,1);set_cell(inputs[i],8,1,1);statuses[i]=1;}
            }
            set_id(inputs[i],salt+i);
            std::memcpy(tx+(i/per)*info.slot_bytes+(i%per)*64,inputs[i].data(),64);
        }
        for(unsigned s=0;s<slots;++s) std::memset(rx+s*info.slot_bytes,0xa5,per*64);
    };
    auto verify=[&](uint64_t salt) {
        std::vector<bool> seen(count);
        unsigned unsolved=0,invalid=0;
        for(unsigned i=0;i<count;++i) {
            Record r;std::memcpy(r.data(),rx+(i/per)*info.slot_bytes+(i%per)*64,64);
            const uint64_t rid=get_id(r);check(rid>=salt && rid-salt<count,"Unknown ID");
            unsigned j=rid-salt;check(!seen[j],"Duplicate ID");seen[j]=true;
            check(r[8]==statuses[j],"Unexpected result status");
            if(r[8]==0) {
                reference::Board answer{};
                for(unsigned c=0;c<81;++c) {unsigned n=cell(r,9,c);check(n>=1 && n<=9,"Bad digit");answer[c]=1u<<(n-1);}
                const auto& p=puzzles[j%puzzles.size()];
                check(reference::valid_solution(p.input,answer) && answer==p.answer,"Solution mismatch");
            } else {
                if(r[8]==1)++unsolved;else ++invalid;
                for(unsigned c=0;c<81;++c)check(cell(r,9,c)==0,"Nonzero error cells");
            }
            check((r[49]&0xf0)==0,"Reserved bits changed");
            for(unsigned b=50;b<64;++b)check(r[b]==0,"Reserved bytes changed");
        }
        std::cout<<"Verified "<<count<<" unique IDs: solved="<<count-unsolved-invalid<<", unsolved="<<unsolved<<", invalid="<<invalid<<'\n';
    };
    fades_dma_run run{};run.bytes=per*64;run.slots=slots;run.rounds=1;run.timeout_ms=60000;
    prepare(0x1234567800000000ULL);
    check(ioctl(fd,FADES_DMA_RUN,&run)==0,"Warmup RUN failed");verify(0x1234567800000000ULL);
    const auto measured_start=Clock::now();
    prepare(0xcafe987600000000ULL);
    const auto prepared=Clock::now();
    run.rounds=rounds;
    check(ioctl(fd,FADES_DMA_RUN,&run)==0,"Measured RUN failed");
    const auto transferred=Clock::now();
    fades_solver_stats stats{};check(ioctl(fd,FADES_SOLVER_STATS,&stats)==0,"STATS failed");
    const auto snapshotted=Clock::now();
    verify(0xcafe987600000000ULL);
    const auto verified=Clock::now();
    check(stats.counters[0]==count && stats.counters[1]==count && stats.counters[3]==count && stats.counters[4]==count,"PL counter mismatch");
    uint64_t dispatched=0,collected=0;
    for(unsigned g=0;g<6;++g) {
        dispatched+=stats.counters[6+g];collected+=stats.counters[12+g];
        check(stats.counters[38+g]<=2,"PL inflight limit exceeded");
    }
    check(dispatched==count && collected==count && stats.counters[31]>0,"PL per-core/work counter mismatch");
    if(count>=1024 && stats.counters[2]>0)
        check(stats.counters[30]>=64 && stats.counters[30]<=66,"PL FIFO occupancy measurement failed");
    std::cout<<"DMA+solver+reset+completion: "<<double(count)*rounds*1e3/run.elapsed_ns<<" Mproblem/s, "<<run.elapsed_ns<<" ns, "<<rounds<<" rounds\n";
    std::cout<<"Oracle preparation: "<<seconds(begin,oracle_done)<<" s; measured input prepare/copy: "<<seconds(measured_start,prepared)<<" s; transfer: "<<seconds(prepared,transferred)<<" s; snapshot: "<<seconds(transferred,snapshotted)<<" s; final validation: "<<seconds(snapshotted,verified)<<" s\n";
    if(rounds==1)std::cout<<"End-to-end (prepare+transfer+validate, excludes oracle): "<<count/seconds(measured_start,verified)/1e6<<" Mproblem/s\n";
    std::cout<<"Last-round PL counters:";
    for(auto v:stats.counters)std::cout<<' '<<v;
    std::cout<<"\nFirst dispatch to last collection: "<<stats.counters[31]<<" core cycles, "<<double(count)*100/stats.counters[31]<<" Mproblem/s at 100 MHz";
    std::cout<<"\nValidation covers warmup and final round; timed intermediate rounds check DMA completion.\n";
    munmap(rx,info.map_bytes);munmap(tx,info.map_bytes);close(fd);
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<" (errno="<<errno<<")\n";return 1;}
