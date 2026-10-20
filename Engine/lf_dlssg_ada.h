// Ada MFG adaptation based on MfgUnlock.cpp in wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass,
// adapted there from y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG (7b7220bb).
// SPDX-License-Identifier: GPL-3.0-only
// Plans every change before writing. Unknown/ambiguous layouts remain unchanged.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lf_ada {
struct Patch { BYTE *at; std::vector<BYTE> before, after; DWORD protection = 0; };
struct Result { bool applied = false; unsigned containers = 0; };

inline bool Range(size_t offset, size_t length, size_t size) { return offset <= size && length <= size - offset; }
template<class T> inline T Read(const BYTE *p) { T v; memcpy(&v,p,sizeof(v)); return v; }
inline void Add(std::vector<Patch> &patches, BYTE *at, const void *data, size_t bytes) {
    const BYTE *p = static_cast<const BYTE *>(data);
    patches.push_back({at, std::vector<BYTE>(at,at+bytes), std::vector<BYTE>(p,p+bytes)});
}

inline BYTE *Unique(BYTE *base, const std::vector<IMAGE_SECTION_HEADER> &sections, const std::vector<int> &pattern) {
    BYTE *found = nullptr;
    for (const auto &s : sections) {
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) || s.Misc.VirtualSize < pattern.size()) continue;
        BYTE *begin = base + s.VirtualAddress;
        for (size_t i=0; i <= s.Misc.VirtualSize-pattern.size(); ++i) {
            bool match=true;
            for (size_t j=0;j<pattern.size();++j) if(pattern[j]>=0 && begin[i+j]!=pattern[j]) {match=false;break;}
            if(match) {if(found)return nullptr; found=begin+i;}
        }
    }
    return found;
}

inline Result Apply(BYTE *base, size_t size, unsigned computeCapability) {
    Result result;
    if(computeCapability!=89 || !base || size<sizeof(IMAGE_DOS_HEADER))return result;
    auto dos=Read<IMAGE_DOS_HEADER>(base);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || !Range(dos.e_lfanew,sizeof(IMAGE_NT_HEADERS64),size))return result;
    auto nt=Read<IMAGE_NT_HEADERS64>(base+dos.e_lfanew);
    if(nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
       nt.OptionalHeader.SizeOfImage>size || nt.FileHeader.NumberOfSections>96)return result;
    size_t table=dos.e_lfanew+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    if(!Range(table,nt.FileHeader.NumberOfSections*sizeof(IMAGE_SECTION_HEADER),size))return result;
    std::vector<IMAGE_SECTION_HEADER> sections;
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        auto s=Read<IMAGE_SECTION_HEADER>(base+table+i*sizeof(IMAGE_SECTION_HEADER));
        if(!Range(s.VirtualAddress,s.Misc.VirtualSize,size))return result;
        sections.push_back(s);
    }
    BYTE *advertise=Unique(base,sections,{0x81,0xfd,0xb0,1,0,0,0x0f,0x8c,-1,-1,-1,-1,0xbf,5,0,0,0});
    BYTE *validate=Unique(base,sections,{0x3d,0xb0,1,0,0,0x0f,0x93,0xc0});
    std::vector<Patch> patches;
    if(advertise && validate) {
        const BYTE nop[]={0x0f,0x1f,0x44,0,0,0x90}, yes[]={0xb0,1,0x90};
        Add(patches,advertise+6,nop,sizeof(nop)); Add(patches,validate+5,yes,sizeof(yes));
    } else {
        advertise=Unique(base,sections,{0xbb,1,0,0,0,0x41,0xb8,3,0,0,0,0x81,0xff,0xb0,1,0,0,0x44,0x0f,0x4c,0xc3});
        validate=Unique(base,sections,{0x3d,0xb0,1,0,0,0x7c,-1,0x83,0xfb,3,0x76});
        if(!advertise || !validate)return result;
        const BYTE five=5, nop4[]={0x0f,0x1f,0x40,0}, nop2[]={0x90,0x90};
        Add(patches,advertise+7,&five,1);Add(patches,advertise+17,nop4,4);
        Add(patches,validate+5,nop2,2);Add(patches,validate+9,&five,1);
    }
    const BYTE magic[]={0x50,0xed,0x55,0xba};
    for(const auto &s:sections) {
        if(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)continue;
        BYTE *begin=base+s.VirtualAddress, *end=begin+s.Misc.VirtualSize;
        for(BYTE *c=std::search(begin,end,magic,magic+4);c!=end;c=std::search(c+1,end,magic,magic+4)) {
            if(end-c<16 || Read<uint16_t>(c+6)!=16)continue;
            const uint64_t payload=Read<uint64_t>(c+8);
            if(!payload || payload>uint64_t(end-c-16))continue;
            BYTE *finish=c+16+payload,*blackwell=nullptr;size_t bh=0,bp=0;
            std::vector<BYTE *> ada; bool valid=true;
            for(BYTE *im=c+16;im<finish;) {
                if(finish-im<32) {valid=false;break;}
                const unsigned header=Read<uint32_t>(im+4),arch=Read<uint32_t>(im+28);
                const uint64_t bytes=Read<uint64_t>(im+8);
                if(header<32 || header>uint64_t(finish-im) || bytes>uint64_t(finish-im)-header) {valid=false;break;}
                if(Read<uint16_t>(im)==1 && arch==120) {
                    if(blackwell) {valid=false;break;}
                    blackwell=im;bh=header;bp=size_t(bytes);
                } else if(arch==89)ada.push_back(im);
                im+=header+bytes;
            }
            if(!valid || !blackwell || ada.empty())continue;
            const char from[]=".target sm_120",to[]=".target sm_89 ";
            static_assert(sizeof(from)==sizeof(to),"equal-length PTX edit");
            BYTE *body=blackwell+bh,*last=body+bp;
            BYTE *target=std::search(body,last,from,from+sizeof(from)-1);
            if(target==last || std::search(target+1,last,from,from+sizeof(from)-1)!=last)continue;
            const uint32_t arch=89,parked=122;
            Add(patches,target,to,sizeof(to)-1);Add(patches,blackwell+28,&arch,4);
            for(auto im:ada)Add(patches,im+28,&parked,4);
            ++result.containers;
        }
    }
    if(!result.containers)return result;
    // Grant access to every page before modifying anything. Restore in reverse
    // order because several edits can share a page and its prior protection.
    size_t unlocked=0;
    for(;unlocked<patches.size();++unlocked) {
        auto &p=patches[unlocked];
        if(memcmp(p.at,p.before.data(),p.before.size()) ||
           !VirtualProtect(p.at,p.after.size(),PAGE_EXECUTE_READWRITE,&p.protection))break;
    }
    if(unlocked==patches.size()) {
        for(auto &p:patches)memcpy(p.at,p.after.data(),p.after.size());
        FlushInstructionCache(GetCurrentProcess(),base,size);
        result.applied=true;
    }
    while(unlocked) {auto &p=patches[--unlocked];DWORD ignored;VirtualProtect(p.at,p.after.size(),p.protection,&ignored);}
    return result;
}
} // namespace lf_ada
