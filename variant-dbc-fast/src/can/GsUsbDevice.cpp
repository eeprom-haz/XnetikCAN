#include "GsUsbDevice.h"
#include <QElapsedTimer>
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <cmath>

// candleLight / gs_usb WinUSB device-interface GUID (registered for the SH-C31G).
static const GUID GUID_GS_USB =
    { 0xc15b4308, 0x04d3, 0x11e6, { 0xb3, 0xea, 0x60, 0x57, 0x18, 0x9e, 0x64, 0x43 } };

namespace {
enum { GS_USB_BREQ_HOST_FORMAT = 0, GS_USB_BREQ_BITTIMING = 1, GS_USB_BREQ_MODE = 2,
       GS_USB_BREQ_BT_CONST = 4, GS_USB_BREQ_DEVICE_CONFIG = 5,
       GS_USB_BREQ_DATA_BITTIMING = 10, GS_USB_BREQ_BT_CONST_EXT = 11 };
enum { GS_CAN_MODE_RESET = 0, GS_CAN_MODE_START = 1 };
// gs_device_mode.flags
const quint32 GS_CAN_MODE_LISTEN_ONLY = (1u << 0);
const quint32 GS_CAN_MODE_FD          = (1u << 8);
// gs_device_bt_const.feature
const quint32 GS_CAN_FEATURE_FD          = (1u << 8);
const quint32 GS_CAN_FEATURE_BT_CONST_EXT = (1u << 10);
// per-frame flags
const quint8 GS_CAN_FLAG_FD  = (1u << 1);
const quint8 GS_CAN_FLAG_BRS = (1u << 2);
const quint8 GS_CAN_FLAG_ESI = (1u << 3);
// can_id flags (SocketCAN convention)
const quint32 CAN_EFF_FLAG = 0x80000000u, CAN_RTR_FLAG = 0x40000000u, CAN_ERR_FLAG = 0x20000000u;
const quint32 CAN_EFF_MASK = 0x1FFFFFFFu, CAN_SFF_MASK = 0x000007FFu;

// CAN FD data-length-code <-> byte-length.
const int kDlc2Len[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
int len2dlc(int n) { for (int i = 0; i < 16; ++i) if (kDlc2Len[i] >= n) return i; return 15; }

#pragma pack(push, 1)
struct gs_host_config   { quint32 byte_order; };
struct gs_device_config { quint8 r1, r2, r3, icount; quint32 sw_version, hw_version; };
struct gs_device_mode   { quint32 mode, flags; };
struct gs_device_bittiming { quint32 prop_seg, phase_seg1, phase_seg2, sjw, brp; };
struct gs_device_bt_const {
    quint32 feature, fclk_can, tseg1_min, tseg1_max, tseg2_min, tseg2_max, sjw_max, brp_min, brp_max, brp_inc;
};
struct gs_device_bt_const_ext {   // nominal timing, then data (FD) timing
    quint32 feature, fclk_can, tseg1_min, tseg1_max, tseg2_min, tseg2_max, sjw_max, brp_min, brp_max, brp_inc,
            dtseg1_min, dtseg1_max, dtseg2_min, dtseg2_max, dsjw_max, dbrp_min, dbrp_max, dbrp_inc;
};
// Header is 12 bytes; data is 8 (classic) or 64 (FD). No HW timestamp requested.
struct gs_host_frame { quint32 echo_id, can_id; quint8 can_dlc, channel, flags, reserved; quint8 data[64]; };
#pragma pack(pop)
const int kHdrLen = 12;

// Timing ranges for one phase (nominal or data), reused for classic and FD.
struct Ranges { quint32 tseg1_min, tseg1_max, tseg2_min, tseg2_max, sjw_max, brp_min, brp_max, brp_inc; };

bool solveTiming(quint32 fclk, const Ranges &r, quint32 bitrate, gs_device_bittiming &out) {
    double best = 1e9; bool found = false;
    for (quint32 brp = r.brp_min; brp <= r.brp_max; brp += (r.brp_inc ? r.brp_inc : 1)) {
        double total = (double)fclk / brp / bitrate;
        quint32 tq = (quint32)llround(total);
        if (tq < 4 || fabs(total - tq) > 1e-6) continue;
        for (quint32 tseg1 = r.tseg1_min; tseg1 <= r.tseg1_max; ++tseg1) {
            int tseg2 = (int)tq - 1 - (int)tseg1;
            if (tseg2 < (int)r.tseg2_min || tseg2 > (int)r.tseg2_max) continue;
            double err = fabs((double)(1 + tseg1) / tq - 0.80);   // ~80% sample point (FD-friendly)
            if (err < best) {
                best = err; found = true;
                out.brp = brp; out.phase_seg2 = (quint32)tseg2;
                quint32 prop = tseg1 / 2; if (prop < 1) prop = 1;
                out.prop_seg = prop; out.phase_seg1 = tseg1 - prop;
                out.sjw = (r.sjw_max < out.phase_seg2) ? r.sjw_max : out.phase_seg2;
                if (out.sjw < 1) out.sjw = 1;
            }
        }
    }
    return found;
}

bool findDevicePath(char *out, size_t n) {
    HDEVINFO di = SetupDiGetClassDevsA(&GUID_GS_USB, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
    bool ok = false;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &GUID_GS_USB, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &ifd, nullptr, 0, &need, nullptr);
        auto *d = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(di, &ifd, d, need, nullptr, nullptr)) {
            strncpy(out, d->DevicePath, n - 1); out[n - 1] = 0; ok = true;
        }
        free(d);
        if (ok) break;
    }
    SetupDiDestroyDeviceInfoList(di);
    return ok;
}

bool ctrlOut(WINUSB_INTERFACE_HANDLE h, quint8 req, quint16 val, quint16 idx, void *d, quint16 len) {
    WINUSB_SETUP_PACKET sp{}; sp.RequestType = 0x41; sp.Request = req; sp.Value = val; sp.Index = idx; sp.Length = len;
    ULONG sent = 0;
    return WinUsb_ControlTransfer(h, sp, (PUCHAR)d, len, &sent, nullptr) && sent == len;
}
bool ctrlIn(WINUSB_INTERFACE_HANDLE h, quint8 req, quint16 val, quint16 idx, void *d, quint16 len) {
    WINUSB_SETUP_PACKET sp{}; sp.RequestType = 0xC1; sp.Request = req; sp.Value = val; sp.Index = idx; sp.Length = len;
    ULONG got = 0;
    return WinUsb_ControlTransfer(h, sp, (PUCHAR)d, len, &got, nullptr) && got == len;
}
}  // namespace

GsUsbDevice::GsUsbDevice(QObject *parent) : QObject(parent) {
    qRegisterMetaType<QCanBusFrame>("QCanBusFrame");
    qRegisterMetaType<QList<QCanBusFrame>>("QList<QCanBusFrame>");
}

GsUsbDevice::~GsUsbDevice() { close(); }

bool GsUsbDevice::present() {
    char path[512];
    return findDevicePath(path, sizeof(path));
}

bool GsUsbDevice::open(quint32 nominalBitrate, quint32 dataBitrate, bool listenOnly, QString *err) {
    close();
    const bool fd = dataBitrate > 0;
    auto fail = [&](const QString &m) { if (err) *err = m; close(); return false; };

    char path[512];
    if (!findDevicePath(path, sizeof(path)))
        return fail("SH-C31G not found. Plug it in (it appears under Universal Serial Bus devices).");
    HANDLE dev = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (dev == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return fail(e == 5 ? "Adapter is busy (another program has it, or a previous run didn't release it). "
                             "Unplug and replug the SH-C31G and try again."
                           : QString("Could not open the adapter (error %1).").arg(e));
    }
    m_handle = dev;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(dev, &usb)) return fail("WinUsb_Initialize failed.");
    m_winusb = usb;

    USB_INTERFACE_DESCRIPTOR id;
    if (!WinUsb_QueryInterfaceSettings(usb, 0, &id)) return fail("Could not read the adapter's USB interface.");
    for (UCHAR p = 0; p < id.bNumEndpoints; ++p) {
        WINUSB_PIPE_INFORMATION pi;
        if (!WinUsb_QueryPipe(usb, 0, p, &pi)) continue;
        if (pi.PipeType == UsbdPipeTypeBulk) { if (pi.PipeId & 0x80) m_epIn = pi.PipeId; else m_epOut = pi.PipeId; }
    }
    if (!m_epIn) return fail("No bulk IN endpoint on the adapter.");
    if (fd && !m_epOut) { /* OK: listening to FD needs no OUT */ }

    gs_device_config cfg{};
    if (ctrlIn(usb, GS_USB_BREQ_DEVICE_CONFIG, 1, 0, &cfg, sizeof(cfg)))
        m_firmware = QString("sw %1.%2 / hw %3.%4").arg(cfg.sw_version >> 16).arg(cfg.sw_version & 0xFFFF)
                         .arg(cfg.hw_version >> 16).arg(cfg.hw_version & 0xFFFF);

    gs_host_config hc{0x0000beef};
    if (!ctrlOut(usb, GS_USB_BREQ_HOST_FORMAT, 1, 0, &hc, sizeof(hc))) return fail("gs_usb HOST_FORMAT failed.");

    // Timing: read the adapter's constants (extended form carries the data-phase ranges for FD).
    Ranges nom{}, dat{}; quint32 feature = 0, fclk = 0; bool haveExt = false;
    gs_device_bt_const_ext ext{};
    if (ctrlIn(usb, GS_USB_BREQ_BT_CONST_EXT, 0, 0, &ext, sizeof(ext))) {
        haveExt = true; feature = ext.feature; fclk = ext.fclk_can;
        nom = {ext.tseg1_min, ext.tseg1_max, ext.tseg2_min, ext.tseg2_max, ext.sjw_max, ext.brp_min, ext.brp_max, ext.brp_inc};
        dat = {ext.dtseg1_min, ext.dtseg1_max, ext.dtseg2_min, ext.dtseg2_max, ext.dsjw_max, ext.dbrp_min, ext.dbrp_max, ext.dbrp_inc};
    }
    gs_device_bt_const bt{};
    if (!haveExt) {
        if (!ctrlIn(usb, GS_USB_BREQ_BT_CONST, 0, 0, &bt, sizeof(bt))) return fail("gs_usb BT_CONST failed.");
        feature = bt.feature; fclk = bt.fclk_can;
        nom = {bt.tseg1_min, bt.tseg1_max, bt.tseg2_min, bt.tseg2_max, bt.sjw_max, bt.brp_min, bt.brp_max, bt.brp_inc};
        dat = nom;
    }
    if (fd && !(feature & GS_CAN_FEATURE_FD))
        return fail("This adapter's firmware reports no CAN FD support. Use a classic speed, or update the firmware.");

    gs_device_bittiming t{};
    if (!solveTiming(fclk, nom, nominalBitrate, t))
        return fail(QString("No bit timing for %1 bps on this adapter.").arg(nominalBitrate));
    if (!ctrlOut(usb, GS_USB_BREQ_BITTIMING, 0, 0, &t, sizeof(t))) return fail("gs_usb BITTIMING failed.");
    if (fd) {
        gs_device_bittiming dt{};
        if (!solveTiming(fclk, dat, dataBitrate, dt))
            return fail(QString("No CAN FD data timing for %1 bps.").arg(dataBitrate));
        if (!ctrlOut(usb, GS_USB_BREQ_DATA_BITTIMING, 0, 0, &dt, sizeof(dt))) return fail("gs_usb DATA_BITTIMING failed.");
    }

    quint32 flags = (listenOnly ? GS_CAN_MODE_LISTEN_ONLY : 0u) | (fd ? GS_CAN_MODE_FD : 0u);
    gs_device_mode md{GS_CAN_MODE_START, flags};
    if (!ctrlOut(usb, GS_USB_BREQ_MODE, 0, 0, &md, sizeof(md))) return fail("gs_usb MODE(start) failed.");

    ULONG to = 400;
    WinUsb_SetPipePolicy(usb, m_epIn, PIPE_TRANSFER_TIMEOUT, sizeof(ULONG), &to);
    m_listenOnly = listenOnly;
    m_fd = fd;
    m_frameLen = kHdrLen + (fd ? 64 : 8);
    m_open = true;
    m_run = true;
    m_thread = std::thread(&GsUsbDevice::readLoop, this);
    return true;
}

void GsUsbDevice::readLoop() {
    auto *usb = (WINUSB_INTERFACE_HANDLE)m_winusb;
    QElapsedTimer clock; clock.start();
    const int stride = m_frameLen;
    const int kBatch = 64;
    unsigned char buf[kBatch * sizeof(gs_host_frame)];
    while (m_run.load()) {
        ULONG got = 0;
        OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = WinUsb_ReadPipe(usb, m_epIn, buf, kBatch * stride, &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult((HANDLE)m_handle, &ov, &got, TRUE);
        CloseHandle(ov.hEvent);
        if (!ok || (int)got < stride) continue;

        QList<QCanBusFrame> out;
        const qint64 us = clock.nsecsElapsed() / 1000;
        for (ULONG off = 0; off + stride <= got; off += stride) {
            gs_host_frame f{}; memcpy(&f, buf + off, stride);
            if (f.echo_id != 0xFFFFFFFFu) continue;   // echoed TX, not bus traffic
            QCanBusFrame frame;
            if (f.can_id & CAN_ERR_FLAG) {
                frame.setFrameType(QCanBusFrame::ErrorFrame);
                frame.setFrameId(f.can_id & CAN_EFF_MASK);
            } else {
                bool eff = f.can_id & CAN_EFF_FLAG;
                frame.setFrameId(eff ? (f.can_id & CAN_EFF_MASK) : (f.can_id & CAN_SFF_MASK));
                frame.setExtendedFrameFormat(eff);
                if (f.can_id & CAN_RTR_FLAG) {
                    frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
                } else {
                    const bool isFd = f.flags & GS_CAN_FLAG_FD;
                    int n = isFd ? kDlc2Len[f.can_dlc & 0x0F] : qMin<int>(f.can_dlc, 8);
                    if (n > 64) n = 64;
                    frame.setPayload(QByteArray(reinterpret_cast<const char *>(f.data), n));
                    if (isFd) {
                        frame.setFlexibleDataRateFormat(true);
                        if (f.flags & GS_CAN_FLAG_BRS) frame.setBitrateSwitch(true);
                        if (f.flags & GS_CAN_FLAG_ESI) frame.setErrorStateIndicator(true);
                    }
                }
            }
            frame.setTimeStamp(QCanBusFrame::TimeStamp::fromMicroSeconds(us));
            out.append(frame);
        }
        if (!out.isEmpty()) emit framesReceived(out);
    }
}

bool GsUsbDevice::writeFrame(const QCanBusFrame &frame) {
    if (!m_open || m_listenOnly) return false;   // transmit needs Active mode
    auto *usb = (WINUSB_INTERFACE_HANDLE)m_winusb;
    if (!usb || !m_epOut) return false;
    gs_host_frame f{}; f.echo_id = 0; f.channel = 0;
    quint32 id = frame.frameId();
    if (frame.hasExtendedFrameFormat()) id |= CAN_EFF_FLAG;
    if (frame.frameType() == QCanBusFrame::RemoteRequestFrame) id |= CAN_RTR_FLAG;
    f.can_id = id;
    QByteArray p = frame.payload();
    const bool fd = m_fd && frame.hasFlexibleDataRateFormat();
    int n = qMin(p.size(), fd ? 64 : 8);
    f.can_dlc = fd ? (quint8)len2dlc(n) : (quint8)n;
    if (fd) { f.flags |= GS_CAN_FLAG_FD; if (frame.hasBitrateSwitch()) f.flags |= GS_CAN_FLAG_BRS; }
    memcpy(f.data, p.constData(), n);
    ULONG sent = 0; OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = WinUsb_WritePipe(usb, m_epOut, (PUCHAR)&f, m_frameLen, &sent, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult((HANDLE)m_handle, &ov, &sent, TRUE);
    CloseHandle(ov.hEvent);
    return ok && (int)sent == m_frameLen;
}

void GsUsbDevice::stopThread() {
    m_run = false;
    if (m_thread.joinable()) m_thread.join();
}

void GsUsbDevice::close() {
    stopThread();
    if (m_winusb) {
        gs_device_mode md{GS_CAN_MODE_RESET, 0};
        ctrlOut((WINUSB_INTERFACE_HANDLE)m_winusb, GS_USB_BREQ_MODE, 0, 0, &md, sizeof(md));
        WinUsb_Free((WINUSB_INTERFACE_HANDLE)m_winusb);
        m_winusb = nullptr;
    }
    if (m_handle) { CloseHandle((HANDLE)m_handle); m_handle = nullptr; }
    m_epIn = m_epOut = 0;
    if (m_open) { m_open = false; emit closed(); }
}
