// c31probe — prove the SH-C31G (gs_usb / candleLight over WinUSB) path before building the
// app backend: enumerate, open, read firmware/version, configure bit timing, start in
// LISTEN-ONLY (silent, no ACK) and print received frames incl. error frames.
//
// Build (MinGW): see build.ps1.  Run: c31probe.exe [--bitrate 500000] [--normal] [--seconds 20]
//   --normal     : start in NORMAL mode (ACKs the bus) instead of listen-only. Bench single-node only.
//   --seconds N  : capture window (default 20). 0 = until Ctrl+C.
//
// gs_usb protocol references: Linux drivers/net/can/usb/gs_usb.c and candleLight firmware.

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>

// candleLight / gs_usb WinUSB device-interface GUID (confirmed registered for this adapter).
static const GUID GUID_GS_USB =
    { 0xc15b4308, 0x04d3, 0x11e6, { 0xb3, 0xea, 0x60, 0x57, 0x18, 0x9e, 0x64, 0x43 } };

// gs_usb vendor control requests.
enum {
    GS_USB_BREQ_HOST_FORMAT = 0,
    GS_USB_BREQ_BITTIMING   = 1,
    GS_USB_BREQ_MODE        = 2,
    GS_USB_BREQ_BT_CONST    = 4,
    GS_USB_BREQ_DEVICE_CONFIG = 5,
};
// gs_device_mode.mode
enum { GS_CAN_MODE_RESET = 0, GS_CAN_MODE_START = 1 };
// gs_device_mode.flags
enum {
    GS_CAN_MODE_LISTEN_ONLY = (1u << 0),
    GS_CAN_MODE_LOOP_BACK   = (1u << 1),
    GS_CAN_MODE_ONE_SHOT    = (1u << 3),
    GS_CAN_MODE_HW_TIMESTAMP= (1u << 4),
};
// can_id flags (Linux SocketCAN convention, used by gs_usb)
static const uint32_t CAN_EFF_FLAG = 0x80000000u;
static const uint32_t CAN_RTR_FLAG = 0x40000000u;
static const uint32_t CAN_ERR_FLAG = 0x20000000u;
static const uint32_t CAN_EFF_MASK = 0x1FFFFFFFu;
static const uint32_t CAN_SFF_MASK = 0x000007FFu;

#pragma pack(push, 1)
struct gs_host_config   { uint32_t byte_order; };
struct gs_device_config { uint8_t reserved1, reserved2, reserved3, icount; uint32_t sw_version, hw_version; };
struct gs_device_mode   { uint32_t mode, flags; };
struct gs_device_bittiming { uint32_t prop_seg, phase_seg1, phase_seg2, sjw, brp; };
struct gs_device_bt_const {
    uint32_t feature, fclk_can;
    uint32_t tseg1_min, tseg1_max, tseg2_min, tseg2_max, sjw_max, brp_min, brp_max, brp_inc;
};
struct gs_host_frame {
    uint32_t echo_id;
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[8];
};
#pragma pack(pop)

static HANDLE   g_dev = INVALID_HANDLE_VALUE;
static WINUSB_INTERFACE_HANDLE g_usb = NULL;
static UCHAR    g_epIn = 0, g_epOut = 0;

static bool findDevicePath(char *out, size_t n) {
    HDEVINFO di = SetupDiGetClassDevsA(&GUID_GS_USB, NULL, NULL,
                                       DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
    bool ok = false;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &GUID_GS_USB, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
        auto *d = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(di, &ifd, d, need, NULL, NULL)) {
            strncpy(out, d->DevicePath, n - 1); out[n - 1] = 0; ok = true;
        }
        free(d);
        if (ok) break;
    }
    SetupDiDestroyDeviceInfoList(di);
    return ok;
}

static bool openDevice() {
    char path[512];
    if (!findDevicePath(path, sizeof(path))) {
        printf("  device not found (is the C31G plugged in and on WinUSB?)\n");
        return false;
    }
    printf("  path: %s\n", path);
    g_dev = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (g_dev == INVALID_HANDLE_VALUE) { printf("  CreateFile failed: %lu\n", GetLastError()); return false; }
    if (!WinUsb_Initialize(g_dev, &g_usb)) { printf("  WinUsb_Initialize failed: %lu\n", GetLastError()); return false; }

    USB_INTERFACE_DESCRIPTOR id;
    if (!WinUsb_QueryInterfaceSettings(g_usb, 0, &id)) { printf("  QueryInterfaceSettings failed\n"); return false; }
    for (UCHAR p = 0; p < id.bNumEndpoints; ++p) {
        WINUSB_PIPE_INFORMATION pi;
        if (!WinUsb_QueryPipe(g_usb, 0, p, &pi)) continue;
        if (pi.PipeType == UsbdPipeTypeBulk) {
            if (pi.PipeId & 0x80) g_epIn = pi.PipeId; else g_epOut = pi.PipeId;
        }
    }
    printf("  endpoints: IN 0x%02X  OUT 0x%02X\n", g_epIn, g_epOut);
    return g_epIn != 0;
}

static bool ctrlOut(uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len) {
    WINUSB_SETUP_PACKET sp;
    sp.RequestType = 0x41;           // host->device, vendor, interface
    sp.Request = req; sp.Value = value; sp.Index = index; sp.Length = len;
    ULONG sent = 0;
    return WinUsb_ControlTransfer(g_usb, sp, (PUCHAR)data, len, &sent, NULL) && sent == len;
}
static bool ctrlIn(uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len) {
    WINUSB_SETUP_PACKET sp;
    sp.RequestType = 0xC1;           // device->host, vendor, interface
    sp.Request = req; sp.Value = value; sp.Index = index; sp.Length = len;
    ULONG got = 0;
    return WinUsb_ControlTransfer(g_usb, sp, (PUCHAR)data, len, &got, NULL) && got == len;
}

// Pick bit timing for the target bitrate, sample point ~87.5%, from the device's own constants.
static bool computeTiming(const gs_device_bt_const &bt, uint32_t bitrate, gs_device_bittiming &out) {
    double best = 1e9;
    bool found = false;
    for (uint32_t brp = bt.brp_min; brp <= bt.brp_max; brp += bt.brp_inc) {
        double tq = (double)bt.fclk_can / brp;
        double total = tq / bitrate;                 // total time quanta per bit
        uint32_t tq_total = (uint32_t)llround(total);
        if (tq_total < 4 || fabs(total - tq_total) > 1e-6) continue;   // must divide evenly
        // total = 1(sync) + tseg1 + tseg2
        for (uint32_t tseg1 = bt.tseg1_min; tseg1 <= bt.tseg1_max; ++tseg1) {
            int tseg2 = (int)tq_total - 1 - (int)tseg1;
            if (tseg2 < (int)bt.tseg2_min || tseg2 > (int)bt.tseg2_max) continue;
            double sp = (double)(1 + tseg1) / tq_total;
            double err = fabs(sp - 0.875);
            if (err < best) {
                best = err; found = true;
                out.brp = brp;
                out.phase_seg2 = (uint32_t)tseg2;
                uint32_t prop = tseg1 / 2; if (prop < 1) prop = 1;
                out.prop_seg = prop;
                out.phase_seg1 = tseg1 - prop;
                out.sjw = (bt.sjw_max < out.phase_seg2) ? bt.sjw_max : out.phase_seg2;
                if (out.sjw < 1) out.sjw = 1;
            }
        }
    }
    return found;
}

int main(int argc, char **argv) {
    uint32_t bitrate = 500000, seconds = 20;
    bool listenOnly = true;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) bitrate = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--normal")) listenOnly = false;
    }
    printf("c31probe — SH-C31G gs_usb/WinUSB\n");
    printf("  mode: %s   bitrate: %u   window: %us\n",
           listenOnly ? "LISTEN-ONLY (silent)" : "NORMAL (ACKs bus!)", bitrate, seconds);

    if (!openDevice()) return 1;

    gs_device_config cfg{};
    if (ctrlIn(GS_USB_BREQ_DEVICE_CONFIG, 1, 0, &cfg, sizeof(cfg)))
        printf("  firmware sw v%u.%u  hw v%u.%u  channels %u\n",
               cfg.sw_version >> 16, cfg.sw_version & 0xFFFF,
               cfg.hw_version >> 16, cfg.hw_version & 0xFFFF, cfg.icount + 1);

    gs_host_config hc{0x0000beef};
    if (!ctrlOut(GS_USB_BREQ_HOST_FORMAT, 1, 0, &hc, sizeof(hc))) { printf("  HOST_FORMAT failed\n"); return 1; }

    gs_device_bt_const bt{};
    if (!ctrlIn(GS_USB_BREQ_BT_CONST, 0, 0, &bt, sizeof(bt))) { printf("  BT_CONST failed\n"); return 1; }
    printf("  CAN clock: %u Hz\n", bt.fclk_can);

    gs_device_bittiming t{};
    if (!computeTiming(bt, bitrate, t)) { printf("  no bit timing for %u at %u Hz clock\n", bitrate, bt.fclk_can); return 1; }
    double sp = (double)(1 + t.prop_seg + t.phase_seg1) / (1 + t.prop_seg + t.phase_seg1 + t.phase_seg2);
    printf("  timing: brp=%u prop=%u ps1=%u ps2=%u sjw=%u  (sample %.1f%%)\n",
           t.brp, t.prop_seg, t.phase_seg1, t.phase_seg2, t.sjw, sp * 100.0);
    if (!ctrlOut(GS_USB_BREQ_BITTIMING, 0, 0, &t, sizeof(t))) { printf("  BITTIMING failed\n"); return 1; }

    gs_device_mode md{GS_CAN_MODE_START, listenOnly ? GS_CAN_MODE_LISTEN_ONLY : 0u};
    if (!ctrlOut(GS_USB_BREQ_MODE, 0, 0, &md, sizeof(md))) { printf("  MODE(start) failed\n"); return 1; }
    printf("  started. reading...\n\n");

    // Read loop (synchronous overlapped reads with a timeout).
    ULONG readTimeout = 500;
    WinUsb_SetPipePolicy(g_usb, g_epIn, PIPE_TRANSFER_TIMEOUT, sizeof(ULONG), &readTimeout);
    uint64_t frames = 0, errs = 0;
    uint32_t firstSeen = 0, lastId = 0;
    DWORD t0 = GetTickCount();
    while (seconds == 0 || (GetTickCount() - t0) < seconds * 1000) {
        gs_host_frame f; ULONG got = 0;
        OVERLAPPED ov{}; ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        BOOL ok = WinUsb_ReadPipe(g_usb, g_epIn, (PUCHAR)&f, sizeof(f), &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(g_dev, &ov, &got, TRUE);
        CloseHandle(ov.hEvent);
        if (!ok || got < 20) continue;
        if (f.echo_id != 0xFFFFFFFF) continue;            // our own echoed TX, ignore
        ++frames;
        uint32_t id = f.can_id;
        bool eff = id & CAN_EFF_FLAG, rtr = id & CAN_RTR_FLAG, err = id & CAN_ERR_FLAG;
        uint32_t raw = eff ? (id & CAN_EFF_MASK) : (id & CAN_SFF_MASK);
        if (err) {
            ++errs;
            printf("  [ERR] frame  err_id=0x%08X  flags=0x%02X\n", id & CAN_EFF_MASK, f.flags);
            continue;
        }
        if (frames <= 30) {   // show the first frames, then just count
            printf("  %s 0x%0*X [%u]%s ", eff ? "EXT" : "STD", eff ? 8 : 3, raw, f.can_dlc, rtr ? " RTR" : "");
            for (int b = 0; b < f.can_dlc && b < 8; ++b) printf(" %02X", f.data[b]);
            printf("\n");
        }
        lastId = raw;
    }
    (void)firstSeen; (void)lastId;
    double secs = (GetTickCount() - t0) / 1000.0;
    printf("\n  done: %llu frames (%.0f/s), %llu error frames, over %.1fs\n",
           (unsigned long long)frames, secs > 0 ? frames / secs : 0.0,
           (unsigned long long)errs, secs);
    if (frames == 0) printf("  no frames — check bitrate, wiring (CANH/L, GND), and terminator switch.\n");

    md = {GS_CAN_MODE_RESET, 0};
    ctrlOut(GS_USB_BREQ_MODE, 0, 0, &md, sizeof(md));
    WinUsb_Free(g_usb);
    CloseHandle(g_dev);
    return 0;
}
