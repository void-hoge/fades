#include "Vfades.h"
#include "verilated.h"
#include "sudoku.hpp"
#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using Record = std::array<uint32_t,16>;
struct Problem { Record record{}; reference::Board board{}; unsigned status=0; };
static void check(bool b, const char* msg) { if (!b) throw std::runtime_error(msg); }
static uint64_t id(unsigned i) { return 0xbadc0ffe12340000ULL ^ (uint64_t(i)*7919); }
static void nibble(Record& r, unsigned bit, unsigned n) { r[bit/32] |= n << (bit%32); }
static unsigned nibble(const Record& r, unsigned bit) { return (r[bit/32] >> (bit%32)) & 15; }
struct Sim {
    Vfades d;
    uint64_t t=0;
    unsigned dma_half=3, core_half=5;
    uint32_t rng=0x1234abcd;
    unsigned random() { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
    void advance() {
        ++t;
        if (t%dma_half==0) d.dma_clk=!d.dma_clk;
        if (t%core_half==0) d.core_clk=!d.core_clk;
        d.eval();
    }
    bool dma_edge() const { return (t+1)%(2*dma_half)==dma_half; }
    void reset(unsigned total, unsigned packet) {
        d.reset=1; d.enable=0; d.s_axis_tvalid=0; d.m_axis_tready=0;
        d.total_records=total; d.packet_records=packet; d.eval();
        for (unsigned i=0;i<180;++i) advance();
        d.reset=0; d.eval();
    }
};
static void run(Sim& s, const std::vector<Problem>& ps, unsigned packet, bool stress) {
    auto& d=s.d;
    s.reset(ps.size(),packet);
    const auto start=s.t;
    unsigned sent=0, beat=0, got=0, obeat=0;
    Record answer{};
    std::vector<bool> seen(ps.size());
    bool held=false;
    std::array<uint32_t,4> held_data{};
    unsigned held_last=0;
    while (got<ps.size()) {
        check(s.t-start<50000000,"Simulation timeout");
        const auto age=s.t-start;
        d.enable=!stress || (age>20000 && s.random()%37!=0);
        if (s.dma_edge()) {
            if (!d.s_axis_tvalid && sent<ps.size() && (!stress || s.random()%4)) {
                for(unsigned w=0;w<4;++w) d.s_axis_tdata[w]=ps[sent].record[beat*4+w];
                d.s_axis_tkeep=0xffff;
                d.s_axis_tlast=beat==3 && (sent%11==10 || sent+1==ps.size());
                d.s_axis_tvalid=1;
            }
            d.m_axis_tready=(!stress || age>45000) && (!stress || s.random()%3);
            d.eval();
            if (held) {
                check(d.m_axis_tvalid,"Output valid lost under backpressure");
                for(unsigned w=0;w<4;++w) check(held_data[w]==d.m_axis_tdata[w],"Output changed under backpressure");
                check(held_last==d.m_axis_tlast,"TLAST changed under backpressure");
            }
            held=d.m_axis_tvalid && !d.m_axis_tready;
            if(held) { for(unsigned w=0;w<4;++w) held_data[w]=d.m_axis_tdata[w]; held_last=d.m_axis_tlast; }
            bool tx=d.s_axis_tvalid && d.s_axis_tready;
            bool rx=d.m_axis_tvalid && d.m_axis_tready;
            if(rx) {
                check(d.m_axis_tkeep==0xffff,"Bad output keep");
                for(unsigned w=0;w<4;++w) answer[obeat*4+w]=d.m_axis_tdata[w];
                check(bool(d.m_axis_tlast)==(obeat==3 && (got%packet==packet-1 || got+1==ps.size())),"Bad packet boundary");
                if(++obeat==4) {
                    obeat=0;
                    uint64_t rid=uint64_t(answer[0]) | uint64_t(answer[1])<<32;
                    unsigned index=ps.size();
                    // Decode multiplicative test ID without relying on output order.
                    uint64_t delta=rid ^ 0xbadc0ffe12340000ULL;
                    if(delta%7919==0) index=delta/7919;
                    check(index<ps.size() && !seen[index],"Unknown or duplicate ID");
                    seen[index]=true;
                    check((answer[2]&255)==ps[index].status,"Wrong solved/unsolved/invalid status");
                    if(ps[index].status==0) {
                        reference::Board board{};
                        for(unsigned c=0;c<81;++c) {
                            unsigned n=nibble(answer,72+4*c);
                            check(n>=1 && n<=9,"Invalid solved cell"); board[c]=1u<<(n-1);
                        }
                        check(reference::valid_solution(ps[index].board,board),"Invalid solution or changed hint");
                    } else for(unsigned c=0;c<81;++c) check(nibble(answer,72+4*c)==0,"Nonzero unsolved cells");
                    for(unsigned b=396;b<512;++b) check(((answer[b/32]>>(b%32))&1)==0,"Reserved bits nonzero");
                    ++got;
                }
            }
            s.advance();
            if(tx) { d.s_axis_tvalid=0; if(++beat==4) {beat=0; ++sent;} d.eval(); }
        } else s.advance();
        check(!d.framing_error,"Unexpected framing error");
    }
    check(sent==ps.size(),"Results without inputs");
    check(d.accepted_count==ps.size() && d.dispatch_count==ps.size() &&
          d.collected_count==ps.size() && d.output_count==ps.size(),"Counter mismatch");
    if(stress) check(d.input_stall_cycles>0,"Backpressure not exercised");
    check(d.queue_peak>0 && d.queue_peak<=64 && d.processing_cycles>0,"Invalid queue or work counters");
    uint64_t sum_sent=0,sum_got=0,sum_busy=0;
    for(unsigned g=0;g<FADES_CORES;++g) {
#if FADES_CORES == 1
        sum_sent += d.core_dispatch_counts;
        sum_got += d.core_result_counts;
        sum_busy += d.core_busy_cycles;
        check(d.core_max_inflight<=2,"Per-core limit exceeded");
#else
        sum_sent+=uint64_t(d.core_dispatch_counts[2*g]) | uint64_t(d.core_dispatch_counts[2*g+1])<<32;
        sum_got+=uint64_t(d.core_result_counts[2*g]) | uint64_t(d.core_result_counts[2*g+1])<<32;
        sum_busy+=uint64_t(d.core_busy_cycles[2*g]) | uint64_t(d.core_busy_cycles[2*g+1])<<32;
        check(d.core_max_inflight[2*g]<=2,"Per-core limit exceeded");
#endif
    }
    check(sum_sent==ps.size() && sum_got==ps.size(),"Per-core accounting mismatch");
    std::cout<<"PASS "<<ps.size()<<" records, stress="<<stress<<", time steps="<<s.t-start
             <<", processing_cycles="<<d.processing_cycles<<", core_busy_cycles="<<sum_busy<<'\n';
}
int main(int argc,char** argv) try {
    Verilated::commandArgs(argc,argv);
    std::ifstream f(argc>1?argv[1]:"sim/data/17puz49158.txt");
    check(bool(f),"Dataset unavailable");
    unsigned limit=argc>2?std::stoul(argv[2]):0;
    std::vector<Problem> ps;
    std::string line;
    while(std::getline(f,line) && (!limit || ps.size()<limit)) {
        if(line.find_first_not_of(" \r\t")==std::string::npos) continue;
        Problem p; p.board=reference::parse(line);
        for(unsigned c=0;c<81;++c) if(reference::single(p.board[c])) nibble(p.record,64+4*c,std::countr_zero(p.board[c])+1);
        ps.push_back(p);
    }
    check(!ps.empty(),"Empty dataset");
    auto stress=std::vector<Problem>(ps.begin(),ps.begin()+std::min<size_t>(256,ps.size()));
    for(unsigned i=0;i<64;++i) {
        Problem p; p.board.fill(reference::all);
        if(i%2==0) {nibble(p.record,64,1); nibble(p.record,68,1);p.status=1;}
        else {nibble(p.record,64+4*(i%81),10+(i/2)%6);p.status=2;}
        stress.push_back(p);
    }
    Problem empty;empty.board.fill(reference::all);stress.push_back(empty);
    Problem solved;solved.board=reference::solve(ps.front().board).first;
    for(unsigned c=0;c<81;++c)nibble(solved.record,64+4*c,std::countr_zero(solved.board[c])+1);
    stress.push_back(solved);
    auto set_ids=[](auto& v) {for(unsigned i=0;i<v.size();++i) {v[i].record[0]=id(i);v[i].record[1]=id(i)>>32;}};
    set_ids(ps);set_ids(stress);
    Sim s;
    run(s,stress,17,true);
    // Reset with an incomplete input and unconsumed output in flight.
    s.reset(1000,7); s.d.enable=1;s.d.s_axis_tvalid=1;s.d.s_axis_tkeep=0xffff;s.d.s_axis_tlast=0;
    for(unsigned w=0;w<4;++w)s.d.s_axis_tdata[w]=0;
    for(unsigned i=0;i<10000;++i) s.advance();
    check(s.d.m_axis_tvalid && s.d.output_count==0,"Reset case did not contain stalled results");
    run(s,ps,16384,false);
    // Framing errors are sticky and require a global reset.
    for(unsigned mode=0;mode<2;++mode) {
        s.reset(1,1); s.d.s_axis_tvalid=1;
        s.d.s_axis_tkeep=mode?0xfffe:0xffff;s.d.s_axis_tlast=!mode;
        for(unsigned i=0;i<300;++i) s.advance();
        check(s.d.framing_error && !s.d.s_axis_tready && s.d.accepted_count==0,"Framing fault handling failed");
    }
    run(s,std::vector<Problem>{ps.front()},1,false);
    Sim slow;slow.dma_half=7;
    run(slow,stress,23,true);
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
