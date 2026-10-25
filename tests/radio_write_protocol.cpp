#include "../src/cs7000usb.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace Cs7000Usb;
namespace {
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void put(Bytes &b, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b.at(at + i) = uint8_t(value >> (8 * i));
}
Bytes frame(uint8_t command, const Bytes &payload) {
    unsigned sum = 0; for (auto v : payload) sum += v;
    Bytes b{0xa1,0x85,command,uint8_t(payload.size() >> 8),uint8_t(payload.size())};
    b.insert(b.end(),payload.begin(),payload.end());
    b.insert(b.end(),{uint8_t(sum >> 8),uint8_t(sum),0xa4}); return b;
}
Bytes image(uint32_t count) {
    const uint32_t main = 0x1401, size = 0x400000;
    Bytes b(main + size, 0xa6);
    const std::string model("CS7000-M17 PLUS\0",16), version("V1.02.20.00\0",12);
    std::copy(model.begin(),model.end(),b.begin()+0x1200);
    std::copy(version.begin(),version.end(),b.begin()+0x1298);
    put(b,0x240,main); put(b,0x1264,main); put(b,main,count); put(b,main+4,size);
    for (uint32_t i=0; i<count; ++i) put(b,main+8+i*4,i+1);
    return b;
}
}
int main() {
    try {
        auto data=image(335); auto plan=planWrite(data);
        check(plan.size()==7,"Wrong sparse block count");
        check(plan[0].selector==0 && plan[2].selector==0xc002,"Wrong main selectors");
        check(plan[3].selector==0 && plan[4].selector==0xc7ef && plan.back().selector==0x47f1,"Wrong contact selectors");
        check(plan.back().extent==4165368,"Wrong contact transfer extent");
        check(std::equal(plan[2].request.begin()+13,plan[2].request.begin()+13+2048,data.begin()+4096),"Boundary block was padded");
        check(planWrite(image(0)).back().selector==0x4000,"Wrong empty-contact final selector");
        check(planWrite(image(600)).size()>plan.size(),"Larger contact directory was ignored");
        for (int failure=0; failure<8; ++failure) {
            Bytes request, previous; size_t sent=0, progressCalls=0; unsigned duplicate=0;
            bool failed=false;
            try {
                executeWrite(data,[&](uint8_t opcode,const Bytes &out,uint32_t length) {
                    if (opcode==0x2a) {
                        request=out;
                        if (out[2]==0x11 || out[2]==0x2d) {
                            check(sent<plan.size() && out==plan[sent].request,"Request differs from plan"); ++sent;
                        }
                        return Bytes{};
                    }
                    if (request[2]==6) {
                        if (request[1]!=5) return Bytes(length,0);
                        Bytes payload(257,0); payload[0]=1;
                        std::string model=failure==1 ? "WRONG RADIO" : "CS7000-M17 PLUS";
                        std::copy(model.begin(),model.end(),payload.begin()+1); return frame(6,payload);
                    }
                    if (request[2]==0x14) return frame(0x14,{uint8_t(failure==2 ? 0 : 1),0});
                    if (failure==4) return Bytes(length,0);
                    if (sent==2 && duplicate++==0) return previous;
                    Bytes ack=frame(failure==5 ? uint8_t(request[2]^1) : request[2],
                                    {uint8_t(failure==7 ? 0 : 1), uint8_t(request[5]&0x3f),
                                     uint8_t(failure==3 ? request[6]^1 : request[6])});
                    if (failure==6) ack[8]^=1;
                    previous=ack; return ack;
                },[&](const auto &,uint32_t done,uint32_t total) {
                    check(done>=1 && done<=total,"Wrong progress"); ++progressCalls;
                });
            } catch (const std::runtime_error &) { failed=true; }
            check(failed==(failure!=0),"Invalid response accepted or good write rejected");
            if (!failure) check(sent==plan.size() && progressCalls==plan.size(),"Write incomplete");
            if (failure==1 || failure==2) check(sent==0,"Data written after rejected identification/setup");
        }
        auto bad=data; put(bad,0x1264,0xffffffff); unsigned calls=0;
        bool rejected=false;
        try { executeWrite(bad,[&](auto,const auto &,auto) {++calls;return Bytes{};},{}); }
        catch (const std::runtime_error &) { rejected=true; }
        check(rejected && !calls,"Malformed image reached USB");
        std::cout << "Write planning, full boundary blocks, identification, acknowledgements, duplicate, timeout and preflight verified\n";
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
