#pragma once
// Wire protocol between XnetikCan (64-bit) and J2534Bridge.exe (32-bit).
// Plain C++ only: this header is compiled by both MinGW/Qt and MSVC x86.
//
// Transport: the bridge's stdin/stdout (anonymous pipes owned by QProcess).
// Every message: [u8 type][u32 payloadLen, little-endian][payload]
#include <cstdint>

namespace xnbridge {

enum MsgType : uint8_t {
    // app -> bridge
    CmdOpen   = 'O',  // u32 bitrate, then UTF-8 DLL path (rest of payload)
    CmdWrite  = 'W',  // one WireFrame
    CmdClose  = 'C',  // no payload
    // bridge -> app
    RspResult = 'R',  // u32 code (0 = ok), then UTF-8 text
    RspFrames = 'F',  // N x WireFrame
    RspError  = 'E',  // UTF-8 text (async error, bus stays open)
    RspTxStats = 'T', // newer bridges: send totals (not used here, skipped)
    RspVbatt  = 'B',  // u32 millivolts at OBD pin 16, about once a second
};

enum FrameFlags : uint8_t {
    FlagExtended = 0x01,  // 29-bit identifier
};

#pragma pack(push, 1)
struct WireFrame {
    uint32_t timestampUs;  // J2534 hardware timestamp (wraps ~71 min)
    uint32_t id;
    uint8_t  flags;
    uint8_t  len;          // 0..8
    uint8_t  data[8];
};
#pragma pack(pop)
static_assert(sizeof(WireFrame) == 18, "WireFrame layout must match on both sides");

constexpr uint32_t kHeaderSize = 5;

} // namespace xnbridge
