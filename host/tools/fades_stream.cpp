// Minimal single-thread ioctl example: packed 64-byte input records -> results.
// Results are completion ordered; use request IDs, not file position, to match.
#include "fades_stream.hpp"
#include <fstream>
#include <iostream>

int main(int argc,char** argv) try {
    fades::require(argc==3 || argc==4,
        "Usage: fades_stream INPUT_RECORDS OUTPUT_RECORDS [RECORDS_PER_BUFFER]");
    unsigned per=argc==4 ? std::stoul(argv[3]) : 10000;
    fades::require(per && per<=16384,"Invalid records per buffer");
    std::ifstream input(argv[1],std::ios::binary);
    std::ofstream output(argv[2],std::ios::binary);
    fades::require(bool(input) && bool(output),"Cannot open input/output");
    // Device only provides RAII for open/INFO/mmap/cleanup. All streaming
    // operations below are explicit ioctls and one poll, on this same fd.
    fades::Device dev;dev.open_device();
    auto qbuf=[&](unsigned queue,unsigned index,unsigned records) {
        fades_buffer b{queue,index,records,0};
        fades::control(dev.fd,FADES_QBUF,&b);
    };
    auto dqbuf=[&](unsigned queue,fades_buffer& b) {
        b={queue,0,0,0};
        if(ioctl(dev.fd,FADES_DQBUF,&b)==0)return true;
        if(errno==EAGAIN)return false;
        throw std::runtime_error("DQBUF: "+std::string(std::strerror(errno)));
    };
    uint64_t submitted=0,received=0;
    bool eof=false;
    auto submit_next=[&](unsigned index) {
        input.read(reinterpret_cast<char*>(dev.slot(FADES_QUEUE_TX,index)),size_t(per)*64);
        auto bytes=input.gcount();
        fades::require(!input.bad() && bytes%64==0,"Invalid/truncated input record file");
        if(bytes==0) {
            eof=true;fades::control(dev.fd,FADES_END_INPUT);return;
        }
        qbuf(FADES_QUEUE_TX,index,unsigned(bytes/64));submitted+=bytes/64;
    };
    for(unsigned i=0;i<dev.info.slots;++i)qbuf(FADES_QUEUE_RX,i,0);
    fades::control(dev.fd,FADES_STREAMON);
    for(unsigned i=0;i<dev.info.slots && !eof;++i)submit_next(i);
    while(!eof || received!=submitted) {
        pollfd pfd{dev.fd,short(POLLIN | (eof ? 0 : POLLOUT)),0};
        int ret=poll(&pfd,1,60000);
        if(ret<0 && errno==EINTR)continue;
        fades::require(ret>0 && !(pfd.revents&(POLLERR|POLLHUP|POLLNVAL)),"poll error/timeout");
        fades_buffer b{};
        while(dqbuf(FADES_QUEUE_RX,b)) {
            output.write(reinterpret_cast<char*>(dev.slot(FADES_QUEUE_RX,b.index)),size_t(b.records)*64);
            fades::require(bool(output),"Output write failed");received+=b.records;
            qbuf(FADES_QUEUE_RX,b.index,0);
        }
        if(!eof)while(dqbuf(FADES_QUEUE_TX,b)) {
            submit_next(b.index);if(eof)break;
        }
    }
    fades::control(dev.fd,FADES_STREAMOFF);
    output.close();fades::require(bool(output),"Output close failed");
    std::cout<<"Completed "<<received<<" records\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
