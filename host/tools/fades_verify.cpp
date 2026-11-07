// Single-threaded QBUF/DQBUF verifier. Every streamed result is checked.
#include "fades_stream.hpp"
#include "sudoku.hpp"
#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <vector>

using Record = std::array<uint8_t,64>;
using fades::require;
static uint64_t get_id(const Record& r) {
    uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(r[i])<<(8*i);return v;
}
static void set_id(Record& r,uint64_t id) {for(unsigned i=0;i<8;++i)r[i]=id>>(8*i);}
static void set_cell(Record& r,unsigned offset,unsigned c,unsigned v) {r[offset+c/2]|=v<<(4*(c%2));}
static unsigned cell(const Record& r,unsigned offset,unsigned c) {return (r[offset+c/2]>>(4*(c%2)))&15;}
struct Puzzle {Record record{};reference::Board input{},answer{};};
int main(int argc,char** argv) try {
    require(argc>=5,"Usage: fades_verify dataset records_per_packet slots rounds [mixed]");
    const unsigned per=std::stoul(argv[2]),slots=std::stoul(argv[3]),rounds=std::stoul(argv[4]);
    const bool mixed=argc>5 && std::string(argv[5])=="mixed";
    require(per && per<=16384 && slots && slots<=8 && rounds && rounds<=100000,"Invalid transfer dimensions");
    const uint64_t count=uint64_t(per)*slots;
    require(!mixed || count>=64,"Mixed test requires at least 64 records");
    const auto begin=fades::Clock::now();
    std::ifstream dataset(argv[1]);require(bool(dataset),"Cannot open dataset");
    std::vector<Puzzle> puzzles;
    std::string line;
    while(std::getline(dataset,line)) {
        if(line.find_first_not_of(" \r\t")==std::string::npos)continue;
        Puzzle p;p.input=reference::parse(line);
        auto ref=reference::solve(p.input);require(ref.solutions==1,"CPU reference found no solution");p.answer=ref.first;
        for(unsigned c=0;c<81;++c)if(reference::single(p.input[c]))set_cell(p.record,8,c,std::countr_zero(p.input[c])+1);
        puzzles.push_back(p);
    }
    require(!dataset.bad(),"Dataset read failed");
    require(!puzzles.empty(),"Empty dataset");
    const auto oracle_done=fades::Clock::now();
    fades::Device dev;dev.open_device();
    std::vector<Record> staging(per);
    auto execute=[&](uint64_t total,uint64_t salt) {
        std::vector<bool> seen(total);
        uint64_t unsolved=0,invalid=0;
        auto status=[&](uint64_t i) -> unsigned {
            return mixed && i%count>=count-64 ? (i%2 ? 2 : 1) : 0;
        };
        auto fill=[&](uint8_t* target,unsigned n,uint64_t first) {
            for(unsigned j=0;j<n;++j) {
                uint64_t i=first+j;auto& r=staging[j];
                r=puzzles[i%puzzles.size()].record;
                if(status(i)) {
                    r={};
                    if(status(i)==2)set_cell(r,8,i%81,10+(i/2)%6);
                    else {set_cell(r,8,0,1);set_cell(r,8,1,1);}
                }
                set_id(r,salt+i);
            }
            std::memcpy(target,staging.data(),size_t(n)*64);
        };
        auto consume=[&](unsigned slot,unsigned n) {
            for(unsigned j=0;j<n;++j) {
                Record r;std::memcpy(r.data(),dev.slot(FADES_QUEUE_RX,slot)+size_t(j)*64,64);
                uint64_t rid=get_id(r);require(rid>=salt && rid-salt<total,"Unknown ID");
                uint64_t i=rid-salt;require(!seen[i],"Duplicate ID");seen[i]=true;
                require(r[8]==status(i),"Unexpected result status");
                if(!status(i)) {
                    reference::Board answer{};
                    for(unsigned c=0;c<81;++c) {unsigned d=cell(r,9,c);require(d>=1 && d<=9,"Bad digit");answer[c]=1u<<(d-1);}
                    const auto& p=puzzles[i%puzzles.size()];
                    require(reference::valid_solution(p.input,answer) && answer==p.answer,"Solution mismatch");
                } else {
                    if(status(i)==1)++unsolved;else ++invalid;
                    for(unsigned c=0;c<81;++c)require(cell(r,9,c)==0,"Nonzero error cells");
                }
                require((r[49]&0xf0)==0,"Reserved bits changed");
                for(unsigned b=50;b<64;++b)require(r[b]==0,"Reserved bytes changed");
            }
        };
        auto run=fades::pump(dev,per,slots,total,0,fill,consume);
        std::cout<<"Verified "<<total<<" unique IDs: solved="<<total-unsolved-invalid
                 <<", unsolved="<<unsolved<<", invalid="<<invalid<<'\n';
        return run;
    };
    execute(count,0x1234567800000000ULL);
    auto run=execute(count*rounds,0xcafe987600000000ULL);
    std::cout<<"Continuous stream: "<<run.received/run.elapsed/1e6<<" Mproblem/s, "<<run.elapsed
             <<" s, "<<run.packets<<" packets; final drain "<<run.drain<<" s\n"
             <<"Oracle preparation (excluded): "<<fades::seconds(begin,oracle_done)<<" s\n"
             <<"All streamed results validated inside elapsed time; initial DDR fill excluded.\n"
             <<"Session PL counters:";
    for(auto v:run.stats.counters)std::cout<<' '<<v;
    std::cout<<"\nPASS\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
