// dlss5_link.h - the link between LIVE FLOW (DLSS 5 MANAGER) and MFG SUPER
// MOTION, as both sides see it. The same file is in DLSS 5 MANAGER
// (ScreenNative\lf_msm_link.h): change both together.
//
// LIVE FLOW takes the game, puts DLSS 5 on each of its frames and hands every
// finished frame to MFG SUPER MOTION, which makes the frames in between and
// shows them all. DLSS 5 works once a frame of the game - not once a frame
// shown, which at x6 would be six times the work for frames that are made from
// those it already did.
//
// The hand-over stays on the graphics card:
//   - a piece of shared memory, kLinkMemory, with what follows (LinkHeader);
//   - kLinkSlots textures, "<kLinkName>_g<generation>_s<slot>": LIVE FLOW
//     copies each finished frame into the next one;
//   - the fence "<kLinkName>_g<generation>_ready": LIVE FLOW signals the
//     frame's sequence number on it once the copy is done on the card;
//   - the fence "<kLinkName>_g<generation>_taken": MFG SUPER MOTION signals
//     the sequence it copied out once its copy is done - a slot is written
//     again only after that;
//   - the event kLinkEvent, set for every frame handed over.
// The generation changes whenever the textures are made again (another size
// or format); the names carry it so an old texture is never opened by mistake.
//
// The time stamp is the game's moment of the frame (when it reached the
// desktop, QueryPerformanceCounter ticks) - what DLSS MFG paces by.
#pragma once
#include <cstdint>

static const wchar_t kLinkMemory[] = L"Local\\DLSS5_LiveFlow_MFG_Link";
static const wchar_t kLinkName[] = L"Local\\DLSS5_LiveFlow_MFG_Link";
static const wchar_t kLinkEvent[] = L"Local\\DLSS5_LiveFlow_MFG_Link_frame";
static constexpr uint32_t kLinkMagic = 0x4B4C464Cu;   // "LFLK"
static constexpr uint32_t kLinkVersion = 1;
static constexpr uint32_t kLinkSlots = 3;

#pragma pack(push, 8)
struct LinkFrame
{
    int64_t sequence;        // 0: none yet
    int64_t stamp;           // the game's moment, QPC ticks
};

struct LinkHeader
{
    uint32_t magic;          // kLinkMagic while LIVE FLOW offers the link
    uint32_t version;        // kLinkVersion
    uint32_t producer_pid;   // LIVE FLOW's worker
    uint32_t generation;     // of the textures and fences
    uint64_t game_hwnd;      // the game's window, as LIVE FLOW took it
    uint32_t width, height;  // of the frames handed over
    uint32_t format;         // their DXGI_FORMAT
    uint32_t fill;           // 1: the window keeps its own size - its picture belongs scaled onto its screen
    int64_t producer_beat;   // QPC ticks, written at least once a second
    volatile int64_t latest; // the newest frame's sequence, 0 none
    LinkFrame frame[kLinkSlots];   // by slot: sequence % kLinkSlots
    uint32_t consumer_pid;   // MFG SUPER MOTION's engine while it takes the frames, 0 none
    uint32_t consumer_flags; // reserved
    int64_t consumer_beat;   // QPC ticks, written with every frame taken and at least once a second
    volatile int64_t consumer_taken;   // the last sequence whose copy MFG SUPER MOTION sent to its card
};
#pragma pack(pop)
static_assert(sizeof(LinkHeader) == 128, "the link's layout changed - change MFG SUPER MOTION's copy too");
