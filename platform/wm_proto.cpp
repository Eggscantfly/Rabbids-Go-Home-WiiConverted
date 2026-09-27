// wm_proto.cpp - WPAD sample synthesis and the WPAD server that the KPAD code inside the PC executable connects to
// (TCP 127.0.0.1:4242, client thread at 0x007F3CD0).  Protocol: docs/platform.md section 2.
#include "wiimote.h"

#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// ---- KPAD model of the PC executable (docs/platform.md section 3) ----
const double KP_DIST_K    = 0.2 / 0.38386398553848267;   // channel +0x554: obj interval 009DA898 / 008EA0C8
const double KP_POS_SCALE = 1.25 / 0.55;                 // channel +0xC4: sqrt(1.5625) / min(1-|cx|, 0.75-|cy|)
const double KP_OBJ_CX    = 0.9990234375;                // 008EA0E0: kobj.x = raw.x / 512 - CX
const double KP_OBJ_CY    = 0.7490234375;                // 008EA0D8: kobj.y = raw.y / 512 - CY
const double KP_OBJ_PIX   = 512.0;                       // 1 / 008EA0E8
const float  ACC_COUNTS   = 100.0f;                      // (acc1g - accZero) * 4 with the calibration we send
const float  FS_COUNTS    = 200.0f;                      // Nunchuk: KPAD default scale 0.005 (008A9260)
const int    STICK_MIN    = 15;                          // 009DA8CC
const int    STICK_MAX    = 71;                          // 009DA8D0
const uint16_t OBJ_SIZE   = 3;

int16_t ToS16(double v, int lo, int hi) {
    long r = lround(v);
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    return (int16_t)r;
}

int8_t StickRaw(float v) {
    float a = (float)fabs(v);
    if (a < 0.001f) return 0;
    if (a > 1.0f) a = 1.0f;
    int r = (int)lround(STICK_MIN + a * (STICK_MAX - STICK_MIN));
    return (int8_t)(v < 0.0f ? -r : r);
}

bool SafeRead(uintptr_t addr, void* out, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

// ---- channel info block that precedes messages 2..7 (read by 0x007F2D50), 76 bytes ----
#pragma pack(push, 1)
struct InfoWire {
    int32_t chan;
    uint8_t addr[6];
    int32_t devType;            // ch+0x0C
    int32_t dataFormat;         // ch+0x10
    uint8_t dpdEnabled;         // ch+0x14 (first byte tested by the KPAD sampling callback)
    uint8_t pad[3];
    int32_t infoDpd, infoSpeaker, infoAttach, infoLowBat, infoNearEmpty;   // "== 1" -> BOOL, ch+0x18..
    uint8_t battery, led, protocol, firmware;                              // ch+0x2C..
    int16_t accZero[3];         // ch+0x30: WPADGetAccGravityUnit = (acc1g - accZero) * 4 (z uses y)
    int16_t acc1g[3];           // ch+0x36
    int16_t fsAcc[6];           // ch+0x3C: not read by this KPAD
    uint8_t fsStick[6];         // ch+0x48: not read by this KPAD
};
#pragma pack(pop)
C_ASSERT(sizeof(InfoWire) == 76);

SOCKET           s_listen = INVALID_SOCKET;
SOCKET           s_client = INVALID_SOCKET;
HANDLE           s_thread;
LONG             s_stop;
CRITICAL_SECTION s_stateCs;
WmState          s_latest;
bool             s_haveLatest;
uint32_t         s_seq;
InfoWire         s_info;
bool             s_engineConnected;          // message 2 sent and not followed by message 3
std::string      s_out;                      // bytes not yet accepted by send()
uint8_t          s_cmd[12];
int              s_cmdLen;
uint32_t         s_sent, s_dropped;

void InfoReset() {
    memset(&s_info, 0, sizeof(s_info));
    static const uint8_t addr[6] = { 0x00, 0x19, 0x1D, 0x57, 0x49, 0x49 };
    s_info.chan = g_cfg.channel;
    memcpy(s_info.addr, addr, sizeof(addr));
    s_info.devType = WPAD_DEV_NOT_FOUND;
    s_info.dataFormat = WPAD_FMT_CORE;
    s_info.battery = 4;
    s_info.led = (uint8_t)(1 << g_cfg.channel);
    for (int i = 0; i < 3; ++i) {
        s_info.accZero[i] = 0;
        s_info.acc1g[i] = 25;
        s_info.fsAcc[i] = 0;
        s_info.fsAcc[3 + i] = 50;
    }
}

void QueueBytes(const void* p, size_t n) {
    s_out.append((const char*)p, n);
}

void QueueInfo(uint8_t type) {
    char buf[1 + sizeof(InfoWire)];
    buf[0] = (char)type;
    memcpy(buf + 1, &s_info, sizeof(InfoWire));
    QueueBytes(buf, sizeof(buf));
}

void DropClient(const char* why, int err) {
    if (s_client == INVALID_SOCKET) return;
    closesocket(s_client);
    s_client = INVALID_SOCKET;
    s_out.clear();
    s_cmdLen = 0;
    s_engineConnected = false;
    Log("engine WPAD client dropped: %s (error %d)", why, err);
}

void Flush() {
    while (s_client != INVALID_SOCKET && !s_out.empty()) {
        int r = send(s_client, s_out.data(), (int)s_out.size(), 0);
        if (r > 0) {
            s_out.erase(0, (size_t)r);
            continue;
        }
        int e = WSAGetLastError();
        if (r < 0 && e == WSAEWOULDBLOCK) return;
        DropClient("send failed", e);
        return;
    }
}

void SyncConnection(const WmState& st) {
    if (s_client == INVALID_SOCKET) return;
    if (st.connected && !s_engineConnected) {
        s_info.devType = st.nunchuk ? WPAD_DEV_FREESTYLE : WPAD_DEV_CORE;
        s_info.dataFormat = WPAD_FMT_CORE;
        s_info.dpdEnabled = 0;
        s_info.infoDpd = 0;
        s_info.infoAttach = st.nunchuk ? 1 : 0;
        QueueInfo(2);
        s_engineConnected = true;
        Log("-> connect: channel %d dev_type %d (%s)", s_info.chan, s_info.devType,
            st.nunchuk ? "Nunchuk" : "no extension");
    } else if (!st.connected && s_engineConnected) {
        s_info.devType = WPAD_DEV_NOT_FOUND;
        s_info.dataFormat = WPAD_FMT_CORE;
        s_info.dpdEnabled = 0;
        s_info.infoDpd = 0;
        s_info.infoAttach = 0;
        QueueInfo(3);
        s_engineConnected = false;
        Log("-> disconnect: channel %d", s_info.chan);
    }
}

void SendSample(const WmState& st) {
    if (s_client == INVALID_SOCKET || !s_engineConnected) return;
    if (s_info.devType != WPAD_DEV_CORE && s_info.devType != WPAD_DEV_FREESTYLE) return;
    if (!s_out.empty()) {                    // never queue a sample behind a partially sent message
        ++s_dropped;
        return;
    }
    WpadRaw w;
    WpadFromState(st, KpadCenterY(), w);
    char b[64];
    int n = 0;
#define PUT(v) do { memcpy(b + n, &(v), sizeof(v)); n += (int)sizeof(v); } while (0)
    uint8_t type = 8;
    int32_t chan = s_info.chan;
    uint8_t dev = (uint8_t)s_info.devType;
    PUT(type);
    PUT(chan);
    PUT(w.button);
    PUT(w.accX);
    PUT(w.accY);
    PUT(w.accZ);
    for (int i = 0; i < 4; ++i) {
        int16_t ox = w.obj[i].x, oy = w.obj[i].y;
        uint16_t size = w.obj[i].size;
        uint8_t id = w.obj[i].traceId;
        PUT(ox);
        PUT(oy);
        PUT(size);
        PUT(id);                             // 7 bytes per object: the padding byte is not sent
    }
    PUT(dev);
    PUT(w.err);
    if (s_info.devType == WPAD_DEV_FREESTYLE) {
        PUT(w.fsAccX);
        PUT(w.fsAccY);
        PUT(w.fsAccZ);
        PUT(w.fsStickX);
        PUT(w.fsStickY);
    }
#undef PUT
    QueueBytes(b, (size_t)n);
    ++s_sent;
}

void HandleCommand(uint32_t cmd, uint32_t chan, uint32_t arg) {
    if ((int32_t)chan != s_info.chan || !s_engineConnected) {
        if (cmd != 5) Log("<- command %u for channel %u ignored (arg %u)", cmd, chan, arg);
        return;
    }
    switch (cmd) {
    case 3:                                  // WPADSetDataFormat (0x007F29E0)
        s_info.dataFormat = (int32_t)arg;
        QueueInfo(7);
        Log("<- SetDataFormat(%u, %u)", chan, arg);
        break;
    case 4:                                  // WPADControlDpd (0x007F2AE0), completion = message 6
        s_info.dpdEnabled = arg ? 1 : 0;
        s_info.infoDpd = arg ? 1 : 0;
        QueueInfo(6);
        Log("<- ControlDpd(%u, %u)", chan, arg);
        break;
    case 5:                                  // WPADControlMotor (0x007F2B40)
        PcSourceSetRumble(arg == 1);
        if (g_cfg.logVerbose) Log("<- ControlMotor(%u, %u)", chan, arg);
        break;
    default:
        Log("<- unknown command %u (channel %u, arg %u)", cmd, chan, arg);
        break;
    }
}

void ReadCommands() {
    while (s_client != INVALID_SOCKET) {
        int r = recv(s_client, (char*)s_cmd + s_cmdLen, 12 - s_cmdLen, 0);
        if (r == 0) {
            DropClient("engine closed the connection", 0);
            return;
        }
        if (r < 0) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) DropClient("recv failed", e);
            return;
        }
        s_cmdLen += r;
        if (s_cmdLen == 12) {
            uint32_t v[3];
            memcpy(v, s_cmd, sizeof(v));
            s_cmdLen = 0;
            HandleCommand(v[0], v[1], v[2]);
        }
    }
}

void AcceptClient() {
    if (s_listen == INVALID_SOCKET) return;
    SOCKET c = accept(s_listen, NULL, NULL);
    if (c == INVALID_SOCKET) return;
    if (s_client != INVALID_SOCKET) {
        Log("second WPAD client refused");
        closesocket(c);
        return;
    }
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    int sndbuf = 1 << 18;
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
    u_long nb = 1;
    ioctlsocket(c, FIONBIO, &nb);
    s_client = c;
    s_out.clear();
    s_cmdLen = 0;
    s_engineConnected = false;
    InfoReset();
    uint8_t hello[6] = { 1, (uint8_t)g_cfg.sensorBar, 1, 0, 0, 0 };   // sensor bar position, motor enabled = 1
    QueueBytes(hello, sizeof(hello));
    Log("engine WPAD client connected (hello: sensor bar %d, motor enabled)", g_cfg.sensorBar);
    WmState st;
    bool have = false;
    EnterCriticalSection(&s_stateCs);
    have = s_haveLatest;
    if (have) st = s_latest;
    LeaveCriticalSection(&s_stateCs);
    if (have) SyncConnection(st);
    Flush();
}

DWORD WINAPI IoThread(LPVOID) {
    timeBeginPeriod(1);
    const double period = 1.0 / (double)g_cfg.rateHz;
    double next = NowSeconds();
    double nextLog = next + 2.0;
    WmState st;
    memset(&st, 0, sizeof(st));
    while (!s_stop) {
        double now = NowSeconds();
        AcceptClient();
        ReadCommands();
        int n = 0;
        while (next <= now && n < 32) {
            PcSourcePoll(next, st);
            EnterCriticalSection(&s_stateCs);
            s_latest = st;
            s_haveLatest = true;
            ++s_seq;
            LeaveCriticalSection(&s_stateCs);
            SyncConnection(st);
            Flush();
            SendSample(st);
            Flush();
            next += period;
            ++n;
        }
        if (now - next > 0.25) next = now;   // resynchronise after a stall instead of bursting
        if (g_cfg.logVerbose && now >= nextLog) {
            nextLog = now + 2.0;
            Log("state: client=%d connected=%d buttons=%04X pointer=%d (%.2f, %.2f) roll=%.1f stick=(%.2f, %.2f) "
                "acc=(%.2f, %.2f, %.2f) fmt=%d dpd=%d sent=%u dropped=%u", s_client != INVALID_SOCKET,
                s_engineConnected, st.buttons, st.pointerValid, st.pointer.x, st.pointer.y, st.roll * 57.29578f,
                st.stick.x, st.stick.y, st.acc.x, st.acc.y, st.acc.z, s_info.dataFormat, s_info.dpdEnabled,
                s_sent, s_dropped);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void WpadFromState(const WmState& st, float centerY, WpadRaw& w) {
    memset(&w, 0, sizeof(w));
    w.button = st.buttons;
    // KPAD 0x007F0990: acc = (-accX, -accZ, +accY) / gravity unit
    w.accX = ToS16(-st.acc.x * ACC_COUNTS, -512, 511);
    w.accY = ToS16(st.acc.z * ACC_COUNTS, -512, 511);
    w.accZ = ToS16(-st.acc.y * ACC_COUNTS, -512, 511);
    if (st.pointerValid) {
        double c = cos(st.roll), s = sin(st.roll);
        double dist = st.dist > 0.6f ? st.dist : 0.6f;
        double d = KP_DIST_K / dist;                  // dot distance in KPAD object units
        double u = -st.pointer.x / KP_POS_SCALE;      // KPAD 0x007F1590 with centre (0, centerY)
        double v = centerY - st.pointer.y / KP_POS_SCALE;
        double mx = c * u + s * v;                    // undo the roll compensation
        double my = -s * u + c * v;
        double hx = c * d * 0.5, hy = -s * d * 0.5;   // (p2 - p1) / 2; KPAD horizon = (dir.x, -dir.y)
        double kx[2] = { mx - hx, mx + hx };
        double ky[2] = { my - hy, my + hy };
        for (int i = 0; i < 2; ++i) {
            double rx = (kx[i] + KP_OBJ_CX) * KP_OBJ_PIX;
            double ry = (ky[i] + KP_OBJ_CY) * KP_OBJ_PIX;
            if (rx < 0.0 || rx > 1023.0 || ry < 0.0 || ry > 767.0) continue;   // outside the camera image
            w.obj[i].x = ToS16(rx, 0, 1023);
            w.obj[i].y = ToS16(ry, 0, 767);
            w.obj[i].size = OBJ_SIZE;
            w.obj[i].traceId = (uint8_t)i;
        }
    }
    w.dev = st.nunchuk ? WPAD_DEV_FREESTYLE : WPAD_DEV_CORE;
    w.err = 0;
    if (st.nunchuk) {
        w.fsAccX = ToS16(-st.fsAcc.x * FS_COUNTS, -512, 511);
        w.fsAccY = ToS16(st.fsAcc.z * FS_COUNTS, -512, 511);
        w.fsAccZ = ToS16(-st.fsAcc.y * FS_COUNTS, -512, 511);
        w.fsStickX = StickRaw(st.stick.x);            // KPAD 0x007F1C90: |raw| 15..71 -> 0..1
        w.fsStickY = StickRaw(st.stick.y);
    }
}

float KpadCenterY() {
    if (!g_cfg.centerAuto) return g_cfg.centerY;
    // KPADInit stores +0.2 (bar below) or -0.2 (bar above) at channel +0xC0 from the WPAD sensor bar byte,
    // which is uninitialised heap memory at that moment (CRT operator new).  Read what KPAD really uses.
    static int    s_known = -1;
    static double s_readT = -100.0;
    static float  s_value = 0.2f;
    if (s_known < 0) {
        static const uint8_t kSig[] = { 0x51, 0xE8, 0xEA, 0x08, 0x00, 0x00, 0xE8, 0x55, 0xB3, 0xDA, 0xFF, 0x83, 0xF8, 0x03 };
        uint8_t buf[sizeof(kSig)];
        s_known = (GetModuleHandleA(NULL) == (HMODULE)0x00400000 && SafeRead(0x007F2050, buf, sizeof(buf)) &&
                   memcmp(buf, kSig, sizeof(kSig)) == 0) ? 1 : 0;
        Log("KPAD DPD centre: %s", s_known ? "read from the PC executable (KPADInit at 007F2050 recognised)"
                                           : "host is not the supported PC executable, using +0.2");
    }
    if (s_known != 1) return 0.2f;
    double now = NowSeconds();
    if (now - s_readT >= 0.5) {
        s_readT = now;
        uint32_t bits = 0;
        float v = 0.2f;
        if (SafeRead(0x00AAB2E0 + (uintptr_t)g_cfg.channel * 0x57C + 0xC0, &bits, sizeof(bits)) && bits == 0xBE4CCCCDu)
            v = -0.2f;
        if (v != s_value) Log("KPAD DPD centre y is now %.1f", v);
        s_value = v;
    }
    return s_value;
}

bool ServerStart() {
    InitializeCriticalSection(&s_stateCs);
    WSADATA wd;
    bool listening = false;
    if (WSAStartup(MAKEWORD(2, 2), &wd) == 0) {
        s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_listen != INVALID_SOCKET) {
            int one = 1;
            setsockopt(s_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
            sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port = htons((u_short)g_cfg.port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(s_listen, (sockaddr*)&a, sizeof(a)) == 0 && listen(s_listen, 4) == 0) {
                u_long nb = 1;
                ioctlsocket(s_listen, FIONBIO, &nb);
                listening = true;
                Log("WPAD server listening on 127.0.0.1:%d", g_cfg.port);
            } else {
                Log("WPAD server: bind/listen on 127.0.0.1:%d failed (error %d) - the engine will not see the remote",
                    g_cfg.port, WSAGetLastError());
                closesocket(s_listen);
                s_listen = INVALID_SOCKET;
            }
        }
    } else {
        Log("WSAStartup failed");
    }
    s_thread = CreateThread(NULL, 0, IoThread, NULL, 0, NULL);
    return listening;
}

void ServerStop() {
    InterlockedExchange(&s_stop, 1);
}

bool ServerLatest(WmState& out, uint32_t* seq) {
    if (!s_thread) return false;
    EnterCriticalSection(&s_stateCs);
    bool ok = s_haveLatest;
    if (ok) {
        out = s_latest;
        if (seq) *seq = s_seq;
    }
    LeaveCriticalSection(&s_stateCs);
    return ok;
}
