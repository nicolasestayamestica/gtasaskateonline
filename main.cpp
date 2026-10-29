// gtskate_net - FASE 2A: espia + red
//
// Hace lo mismo que la fase 1 (engancha skate_ffi.dll y registra en
// gtskate_net.log) y ademas:
//   1. Envia por UDP, ~30 Hz, el estado de skate local a los otros jugadores
//      (pose de raiz, matrices de huesos, estado, skin de tabla/grip, pos del ped).
//   2. Recibe lo que mandan ellos y lo guarda (todavia NO dibuja nada).
//   3. Cada segundo escribe en el log: paquetes/s, perdida, y el ped del
//      pool del GTA que mejor coincide con la posicion de cada jugador remoto
//      (para validar el emparejamiento antes de la fase 2B/2C).
//
// NO cambia nada del juego: los detours llaman a la funcion original y
// devuelven su resultado tal cual.
//
// Config: gtskate_net.ini junto al .asi (si no existe, se crea una plantilla).

#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "MinHook.h"

#pragma comment(lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

// ---------------------------------------------------------------- log

static HMODULE          g_self = nullptr;
static FILE*            g_log  = nullptr;
static CRITICAL_SECTION g_cs;      // protege el log
static CRITICAL_SECTION g_dataCs;  // protege los datos compartidos (local y remotos)
static DWORD            g_t0   = 0;

static void Logf(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "[%8lu] %s\n", (unsigned long)(GetTickCount() - g_t0), buf);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static bool LooksPtr(uint32_t v) { return v >= 0x10000u && v < 0x7FFE0000u; }

// Copia n bytes de src a dst; devuelve false si src no es legible.
static bool SafeCopy(void* dst, const void* src, size_t n)
{
    __try
    {
        memcpy(dst, src, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Vuelca n floats de p. Protegido: si el puntero es invalido no revienta.
static void DumpFloats(const char* label, const void* p, int n)
{
    if (!p) { Logf("%s: (null)", label); return; }
    char line[1700];
    int len = 0;
    line[0] = 0;
    __try
    {
        const float* f = (const float*)p;
        for (int i = 0; i < n; i++)
        {
            len += snprintf(line + len, sizeof(line) - len, "%s%.3f", i ? " " : "", f[i]);
            if (len > (int)sizeof(line) - 24) break;
        }
        Logf("%s: %s", label, line);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Logf("%s: <ilegible %p>", label, p);
    }
}

static void DumpWords(const char* label, const void* p, int n)
{
    if (!p) { Logf("%s: (null)", label); return; }
    char line[1700];
    int len = 0;
    line[0] = 0;
    __try
    {
        const uint32_t* w = (const uint32_t*)p;
        for (int i = 0; i < n; i++)
        {
            len += snprintf(line + len, sizeof(line) - len, "%s%08X", i ? " " : "", (unsigned)w[i]);
            if (len > (int)sizeof(line) - 24) break;
        }
        Logf("%s: %s", label, line);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Logf("%s: <ilegible %p>", label, p);
    }
}

static void ArgsDump(const char* tag, const uint32_t* v, int n)
{
    char line[400];
    int len = 0;
    line[0] = 0;
    for (int i = 0; i < n; i++)
        len += snprintf(line + len, sizeof(line) - len, "%sa%d=%08X", i ? " " : "", i, (unsigned)v[i]);
    Logf("%s %s", tag, line);
    for (int i = 0; i < n; i++)
    {
        if (!LooksPtr(v[i])) continue;
        char lb[64];
        snprintf(lb, sizeof(lb), "    a%d->float", i);
        DumpFloats(lb, (const void*)(uintptr_t)v[i], 12);
    }
}

// ----------------------------------------------------- estado y muestreo

typedef const char* (__cdecl* FnState)(void*);
static FnState pState = nullptr;
static void*   g_session = nullptr;

// sk_state(session) devuelve un puntero a texto ("PhysicsGround", ...)
static void StateStr(void* s, char* out, size_t cap)
{
    snprintf(out, cap, "?");
    if (!pState || !s) return;
    __try
    {
        const char* p = pState(s);
        if (LooksPtr((uint32_t)(uintptr_t)p))
        {
            size_t i = 0;
            while (i + 1 < cap && p[i] >= 32 && p[i] < 127) { out[i] = p[i]; i++; }
            out[i] = 0;
            if (i == 0) snprintf(out, cap, "ptr=%p", (const void*)p);
        }
        else
        {
            snprintf(out, cap, "val=%08X", (unsigned)(uintptr_t)p);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        snprintf(out, cap, "<err>");
    }
}

struct Stat
{
    const char*   name;
    volatile LONG total;
    volatile LONG window;
    volatile LONG last;
};

static Stat S_bones    = { "sk_get_bones",  0, 0, 0 };
static Stat S_board    = { "sk_board_pose", 0, 0, 0 };
static Stat S_pose     = { "sk_get_pose",   0, 0, 0 };
static Stat S_rigpose  = { "sk_rig_pose",   0, 0, 0 };
static Stat S_rigsetup = { "sk_rig_setup",  0, 0, 0 };
static Stat S_mesh     = { "sk_board_mesh", 0, 0, 0 };

// true en las 4 primeras llamadas y luego como mucho 1 vez por segundo
static bool Sample(Stat& s)
{
    LONG t = InterlockedIncrement(&s.total);
    InterlockedIncrement(&s.window);
    if (t <= 4) return true;
    LONG now  = (LONG)GetTickCount();
    LONG last = s.last;
    if (now - last >= 1000 && InterlockedCompareExchange(&s.last, now, last) == last)
        return true;
    return false;
}

// ------------------------------------------------- datos locales capturados
// Lo que los detours ven en cada tick del motor. Protegido por g_dataCs.

static const int MAX_BONES = 40;           // el motor devuelve 32; margen por si acaso
static const int POSE_BYTES = 136;         // sk_get_pose: 4 de cabecera + 132 de pose

static uint8_t  g_pose[POSE_BYTES];
static DWORD    g_poseTick = 0;
static float    g_rig[MAX_BONES][16];      // matrices 4x4 (fila 3 = traslacion)
static int      g_rigN = 0;
static DWORD    g_rigTick = 0;
static char     g_stateStr[24] = "?";

// ----------------------------------------------------------- detours
// Todas las funciones de skate_ffi.dll son cdecl.

typedef int (__cdecl* T3)(void*, void*, uint32_t);
typedef int (__cdecl* T2)(void*, void*);
typedef int (__cdecl* T8)(uint32_t, uint32_t, uint32_t, uint32_t,
                          uint32_t, uint32_t, uint32_t, uint32_t);

static T3 o_bones = nullptr;
static T3 o_board = nullptr;
static T2 o_pose  = nullptr;
static T8 o_rigpose = nullptr, o_rigsetup = nullptr, o_mesh = nullptr;

static int __cdecl d_bones(void* s, void* out, uint32_t n)
{
    bool lg = Sample(S_bones);
    g_session = s;
    int r = o_bones(s, out, n);
    if (lg)
    {
        char st[48];
        StateStr(s, st, sizeof(st));
        Logf("sk_get_bones s=%p out=%p n=%u ret=%d state=%s", s, out, (unsigned)n, r, st);
        if (out)
            for (int b = 0; b < 4; b++)
            {
                char lb[32];
                snprintf(lb, sizeof(lb), "    bone%d", b);
                DumpFloats(lb, (const char*)out + b * 64, 16);
            }
    }
    return r;
}

static int __cdecl d_board(void* s, void* out, uint32_t len)
{
    bool lg = Sample(S_board);
    g_session = s;
    int r = o_board(s, out, len);
    if (lg)
    {
        char st[48];
        StateStr(s, st, sizeof(st));
        Logf("sk_board_pose s=%p out=%p len=%u ret=%d state=%s", s, out, (unsigned)len, r, st);
        DumpFloats("    out(48 floats)", out, 48);
    }
    return r;
}

// Captura la pose cruda (136 bytes) y el nombre del estado. Se llama desde el
// hilo del motor, asi sk_state no se invoca desde otro hilo.
static void CapturePose(void* s, void* out)
{
    char st[24];
    StateStr(s, st, sizeof(st));
    EnterCriticalSection(&g_dataCs);
    if (out && SafeCopy(g_pose, out, POSE_BYTES))
        g_poseTick = GetTickCount();
    memcpy(g_stateStr, st, sizeof(g_stateStr));
    g_stateStr[sizeof(g_stateStr) - 1] = 0;
    LeaveCriticalSection(&g_dataCs);
}

static int __cdecl d_pose(void* s, void* out)
{
    bool lg = Sample(S_pose);
    g_session = s;
    int r = o_pose(s, out);
    CapturePose(s, out);
    if (lg)
    {
        char st[48];
        StateStr(s, st, sizeof(st));
        Logf("sk_get_pose s=%p out=%p ret=%d state=%s", s, out, r, st);
        DumpWords("    out words(34)", out, 34);
        DumpFloats("    out floats(34)", out, 34);
    }
    return r;
}

// sk_rig_pose(session, out, nbones, ...): out recibe nbones matrices de 64 bytes
static void CaptureRig(uint32_t outPtr, int n)
{
    if (n <= 0 || n > MAX_BONES || !LooksPtr(outPtr)) return;
    EnterCriticalSection(&g_dataCs);
    if (SafeCopy(g_rig, (const void*)(uintptr_t)outPtr, (size_t)n * 64))
    {
        g_rigN = n;
        g_rigTick = GetTickCount();
    }
    LeaveCriticalSection(&g_dataCs);
}

static int __cdecl d_rigpose(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                             uint32_t e, uint32_t f, uint32_t g, uint32_t h)
{
    bool lg = Sample(S_rigpose);
    uint32_t v[6] = { a, b, c, d, e, f };
    if (lg) ArgsDump("sk_rig_pose PRE ", v, 4);
    int r = o_rigpose(a, b, c, d, e, f, g, h);
    CaptureRig(b, r);
    if (lg) { Logf("sk_rig_pose ret=%d", r); ArgsDump("sk_rig_pose POST", v, 4); }
    return r;
}

static int __cdecl d_rigsetup(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                              uint32_t e, uint32_t f, uint32_t g, uint32_t h)
{
    bool lg = Sample(S_rigsetup);
    uint32_t v[6] = { a, b, c, d, e, f };
    if (lg) ArgsDump("sk_rig_setup PRE ", v, 6);
    int r = o_rigsetup(a, b, c, d, e, f, g, h);
    if (lg) { Logf("sk_rig_setup ret=%d", r); ArgsDump("sk_rig_setup POST", v, 6); }
    return r;
}

static int __cdecl d_mesh(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                          uint32_t e, uint32_t f, uint32_t g, uint32_t h)
{
    bool lg = Sample(S_mesh);
    uint32_t v[6] = { a, b, c, d, e, f };
    if (lg) ArgsDump("sk_board_mesh PRE ", v, 4);
    int r = o_mesh(a, b, c, d, e, f, g, h);
    if (lg) { Logf("sk_board_mesh ret=%d", r); ArgsDump("sk_board_mesh POST", v, 4); }
    return r;
}

static bool Hook(HMODULE m, const char* name, void* detour, void** orig)
{
    void* tgt = (void*)GetProcAddress(m, name);
    if (!tgt) { Logf("hook %s: la DLL no la exporta", name); return false; }
    MH_STATUS st = MH_CreateHook(tgt, detour, orig);
    Logf("hook %s @%p -> %s", name, tgt, MH_StatusToString(st));
    return st == MH_OK;
}

// -------------------------------------------------- datos del juego (GTA SA 1.0 US)
// 0xB7CD98 = CWorld::Players[0].m_pPed ; 0xB6F5F0 = puntero rapido al ped local
// 0xB74490 = CPools::ms_pPedPool. sizeof(CPed) = 0x7A4.
// Todo protegido con SEH y sin objetos C++ en las funciones con __try.

static const uint32_t ADDR_PLAYER_PED = 0xB7CD98;
static const uint32_t ADDR_PED_FAST   = 0xB6F5F0;
static const uint32_t ADDR_PED_POOL   = 0xB74490;
static const uint32_t SIZEOF_CPED     = 0x7A4;

static uint32_t LocalPed()
{
    uint32_t ped = 0;
    __try
    {
        uint32_t pa = *(uint32_t*)ADDR_PLAYER_PED;
        uint32_t pb = *(uint32_t*)ADDR_PED_FAST;
        ped = pa ? pa : pb;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ped = 0;
    }
    return LooksPtr(ped) ? ped : 0;
}

// Posicion de un ped (via su matriz; si no hay matriz, via el placement).
static bool PedPos(uint32_t ped, float* out3)
{
    if (!LooksPtr(ped)) return false;
    __try
    {
        uint32_t mtx = *(uint32_t*)(ped + 0x14);
        const float* p = LooksPtr(mtx) ? (const float*)(mtx + 0x30) : (const float*)(ped + 4);
        out3[0] = p[0]; out3[1] = p[1]; out3[2] = p[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct PedInfo { uint32_t ped; float pos[3]; };

// Recorre el pool de peds del GTA y devuelve cuantos hay (y sus posiciones).
static int ScanPeds(PedInfo* out, int maxN)
{
    int n = 0;
    __try
    {
        uint32_t pool = *(uint32_t*)ADDR_PED_POOL;
        if (!LooksPtr(pool)) return 0;
        uint32_t objs = *(uint32_t*)(pool + 0);
        uint32_t map  = *(uint32_t*)(pool + 4);
        int      size = *(int*)(pool + 8);
        if (!LooksPtr(objs) || !LooksPtr(map) || size <= 0 || size > 1000) return 0;
        for (int i = 0; i < size && n < maxN; i++)
        {
            uint8_t flags = *(uint8_t*)(map + i);
            if (flags & 0x80) continue;                 // hueco libre
            uint32_t ped = objs + (uint32_t)i * SIZEOF_CPED;
            float p[3];
            if (!PedPos(ped, p)) continue;
            out[n].ped = ped;
            out[n].pos[0] = p[0]; out[n].pos[1] = p[1]; out[n].pos[2] = p[2];
            n++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return n;
    }
    return n;
}

// --------------------------------------------------------------- config

struct Peer
{
    char     nick[16];
    char     ipStr[32];
    uint32_t ip;           // en orden de red
    sockaddr_in addr;
    bool     used;
    // --- recibido ---
    DWORD    lastRecv;
    uint32_t pkts;         // paquetes en la ventana actual
    uint32_t metaTotal;
    uint16_t lastSeq;
    bool     haveSeq;
    uint32_t lost;
    // meta
    uint8_t  active, deck, grip, outfit, nbones;
    float    pedPos[3];
    char     state[24];
    uint8_t  pose[POSE_BYTES];
    // huesos (3x4 por hueso: columnas 0..2 de las 4 filas)
    float    rig[MAX_BONES][12];
    uint32_t rigMask;      // bit0 = bloque 0..15 recibido, bit1 = 16..31
    DWORD    rigTick;
};

static const int MAX_PEERS = 8;
static Peer   g_peers[MAX_PEERS];
static int    g_nPeers = 0;
static char   g_myNick[16] = "yo";
static int    g_port = 7778;
static int    g_hz = 30;
static SOCKET g_sock = INVALID_SOCKET;
static char   g_dir[MAX_PATH];        // carpeta del .asi (con \ final)
static char   g_iniPath[MAX_PATH];

static void WriteTemplateIni(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "; gtskate_net - configuracion de red (editar y reiniciar el juego)\n"
        "; 'nick' de [net] es TU nombre. Los [peerN] son los OTROS jugadores\n"
        "; (no te pongas a ti mismo). IPs de la VPN (Hamachi).\n"
        "[net]\n"
        "nick=yo\n"
        "port=7778\n"
        "hz=30\n"
        "\n"
        "[peer1]\n"
        "nick=mateas\n"
        "ip=25.37.90.98\n"
        "\n"
        "[peer2]\n"
        "nick=pipi\n"
        "ip=25.37.90.73\n");
    fclose(f);
}

static void LoadConfig()
{
    GetPrivateProfileStringA("net", "nick", "yo", g_myNick, sizeof(g_myNick), g_iniPath);
    g_port = GetPrivateProfileIntA("net", "port", 7778, g_iniPath);
    g_hz   = GetPrivateProfileIntA("net", "hz", 30, g_iniPath);
    if (g_hz < 5) g_hz = 5;
    if (g_hz > 60) g_hz = 60;

    g_nPeers = 0;
    for (int i = 1; i <= MAX_PEERS; i++)
    {
        char sec[16], nick[16], ip[32];
        snprintf(sec, sizeof(sec), "peer%d", i);
        GetPrivateProfileStringA(sec, "ip", "", ip, sizeof(ip), g_iniPath);
        if (!ip[0]) continue;
        GetPrivateProfileStringA(sec, "nick", sec, nick, sizeof(nick), g_iniPath);

        Peer& p = g_peers[g_nPeers];
        memset(&p, 0, sizeof(p));
        strncpy(p.nick, nick, sizeof(p.nick) - 1);
        strncpy(p.ipStr, ip, sizeof(p.ipStr) - 1);
        unsigned long a = inet_addr(ip);
        if (a == INADDR_NONE) { Logf("ini: IP invalida en [%s]: %s", sec, ip); continue; }
        p.ip = (uint32_t)a;
        p.addr.sin_family = AF_INET;
        p.addr.sin_port = htons((u_short)g_port);
        p.addr.sin_addr.s_addr = a;
        p.used = true;
        g_nPeers++;
        if (g_nPeers >= MAX_PEERS) break;
    }
    Logf("config: nick=%s puerto=%d hz=%d peers=%d", g_myNick, g_port, g_hz, g_nPeers);
    for (int i = 0; i < g_nPeers; i++)
        Logf("  peer %d: %s %s", i, g_peers[i].nick, g_peers[i].ipStr);
}

// --------------------------------------------------------------- paquetes

#pragma pack(push, 1)
struct PktHdr
{
    uint32_t magic;     // 'GSKN'
    uint8_t  ver;
    uint8_t  type;      // 0 = meta, 1 = huesos
    uint16_t seq;       // numero de tick del emisor (igual en los 3 paquetes de un tick)
    uint32_t tick;      // GetTickCount del emisor
    char     nick[16];
};

struct PktMeta
{
    PktHdr   h;
    uint8_t  active;    // 1 si el mod de skate esta corriendo ahora mismo
    uint8_t  deck;      // DeckSkin (255 = desconocido)
    uint8_t  grip;      // GripSkin (255 = desconocido)
    uint8_t  outfit;    // Outfit   (255 = desconocido)
    uint8_t  nbones;
    uint8_t  pad[3];
    float    pedPos[3];
    char     state[24];
    uint8_t  pose[POSE_BYTES];
};

struct PktRig
{
    PktHdr   h;
    uint8_t  first;     // indice del primer hueso de este paquete
    uint8_t  count;     // cuantos huesos trae (<= 16)
    uint16_t pad;
    float    m[16][12]; // por hueso: filas 0..3, columnas 0..2 (la columna 3 es 0,0,0,1)
};
#pragma pack(pop)

static const uint32_t MAGIC = 0x4E4B5347u;  // "GSKN"

static int g_deck = 255, g_grip = 255, g_outfit = 255;

static void ReadSkins()
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sgtskate.ini", g_dir);
    int d = GetPrivateProfileIntA("Skate", "DeckSkin", -1, path);
    int g = GetPrivateProfileIntA("Skate", "GripSkin", -1, path);
    int o = GetPrivateProfileIntA("Skate", "Outfit",   -1, path);
    g_deck   = (d >= 0 && d < 255) ? d : 255;
    g_grip   = (g >= 0 && g < 255) ? g : 255;
    g_outfit = (o >= 0 && o < 255) ? o : 255;
}

static void FillHdr(PktHdr& h, uint8_t type, uint16_t seq)
{
    h.magic = MAGIC;
    h.ver = 1;
    h.type = type;
    h.seq = seq;
    h.tick = GetTickCount();
    memset(h.nick, 0, sizeof(h.nick));
    strncpy(h.nick, g_myNick, sizeof(h.nick) - 1);
}

static uint32_t g_sentPkts = 0;      // en la ventana actual
static uint32_t g_sentBytes = 0;
static uint32_t g_sendErr = 0;

static void SendAll(const void* data, int len)
{
    for (int i = 0; i < g_nPeers; i++)
    {
        int r = sendto(g_sock, (const char*)data, len, 0,
                       (const sockaddr*)&g_peers[i].addr, sizeof(sockaddr_in));
        if (r == SOCKET_ERROR) g_sendErr++;
        else { g_sentPkts++; g_sentBytes += (uint32_t)r; }
    }
}

static DWORD WINAPI SendThread(LPVOID)
{
    uint16_t seq = 0;
    DWORD nextSkins = 0;
    ReadSkins();
    Logf("skins locales: deck=%d grip=%d outfit=%d (255 = no se pudo leer gtskate.ini)", g_deck, g_grip, g_outfit);

    for (;;)
    {
        DWORD now = GetTickCount();
        if (now >= nextSkins) { ReadSkins(); nextSkins = now + 2000; }

        PktMeta meta;
        memset(&meta, 0, sizeof(meta));
        FillHdr(meta.h, 0, seq);
        PktRig rigA, rigB;
        memset(&rigA, 0, sizeof(rigA));
        memset(&rigB, 0, sizeof(rigB));
        FillHdr(rigA.h, 1, seq);
        FillHdr(rigB.h, 1, seq);

        int nb = 0;
        EnterCriticalSection(&g_dataCs);
        bool active = (g_poseTick != 0) && (now - g_poseTick < 250);
        memcpy(meta.pose, g_pose, POSE_BYTES);
        memcpy(meta.state, g_stateStr, sizeof(meta.state));
        nb = g_rigN;
        if (nb > 32) nb = 32;
        for (int b = 0; b < nb; b++)
        {
            float (*dst)[12] = (b < 16) ? &rigA.m[b] : &rigB.m[b - 16];
            const float* s = g_rig[b];
            (*dst)[0]  = s[0];  (*dst)[1]  = s[1];  (*dst)[2]  = s[2];
            (*dst)[3]  = s[4];  (*dst)[4]  = s[5];  (*dst)[5]  = s[6];
            (*dst)[6]  = s[8];  (*dst)[7]  = s[9];  (*dst)[8]  = s[10];
            (*dst)[9]  = s[12]; (*dst)[10] = s[13]; (*dst)[11] = s[14];
        }
        LeaveCriticalSection(&g_dataCs);

        meta.active = active ? 1 : 0;
        meta.deck = (uint8_t)g_deck;
        meta.grip = (uint8_t)g_grip;
        meta.outfit = (uint8_t)g_outfit;
        meta.nbones = (uint8_t)nb;
        float pp[3] = { 0, 0, 0 };
        PedPos(LocalPed(), pp);
        meta.pedPos[0] = pp[0]; meta.pedPos[1] = pp[1]; meta.pedPos[2] = pp[2];

        SendAll(&meta, (int)sizeof(meta));
        if (active && nb > 0)
        {
            rigA.first = 0;
            rigA.count = (uint8_t)(nb < 16 ? nb : 16);
            SendAll(&rigA, (int)sizeof(rigA));
            if (nb > 16)
            {
                rigB.first = 16;
                rigB.count = (uint8_t)(nb - 16);
                SendAll(&rigB, (int)sizeof(rigB));
            }
        }
        seq++;
        Sleep(1000 / g_hz);
    }
    return 0;
}

static int FindPeerByIp(uint32_t ip)
{
    for (int i = 0; i < g_nPeers; i++)
        if (g_peers[i].used && g_peers[i].ip == ip) return i;
    return -1;
}

static DWORD WINAPI RecvThread(LPVOID)
{
    static char buf[2048];
    for (;;)
    {
        sockaddr_in from;
        int fromLen = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n == SOCKET_ERROR)
        {
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT) continue;
            Logf("recvfrom error %d", e);
            Sleep(200);
            continue;
        }
        if (n < (int)sizeof(PktHdr)) continue;
        const PktHdr* h = (const PktHdr*)buf;
        if (h->magic != MAGIC || h->ver != 1) continue;

        int pi = FindPeerByIp((uint32_t)from.sin_addr.s_addr);
        if (pi < 0)
        {
            static DWORD lastWarn = 0;
            DWORD now = GetTickCount();
            if (now - lastWarn > 5000)
            {
                lastWarn = now;
                Logf("paquete de IP desconocida %s (no esta en el ini)", inet_ntoa(from.sin_addr));
            }
            continue;
        }

        EnterCriticalSection(&g_dataCs);
        Peer& p = g_peers[pi];
        p.lastRecv = GetTickCount();
        p.pkts++;
        if (h->type == 0 && n == (int)sizeof(PktMeta))
        {
            const PktMeta* m = (const PktMeta*)buf;
            if (p.haveSeq)
            {
                uint16_t gap = (uint16_t)(m->h.seq - p.lastSeq);
                if (gap > 1 && gap < 1000) p.lost += gap - 1;
            }
            p.lastSeq = m->h.seq;
            p.haveSeq = true;
            p.metaTotal++;
            p.active = m->active;
            p.deck = m->deck; p.grip = m->grip; p.outfit = m->outfit;
            p.nbones = m->nbones;
            p.pedPos[0] = m->pedPos[0]; p.pedPos[1] = m->pedPos[1]; p.pedPos[2] = m->pedPos[2];
            memcpy(p.state, m->state, sizeof(p.state));
            p.state[sizeof(p.state) - 1] = 0;
            memcpy(p.pose, m->pose, POSE_BYTES);
        }
        else if (h->type == 1 && n == (int)sizeof(PktRig))
        {
            const PktRig* r = (const PktRig*)buf;
            if (r->first + r->count <= MAX_BONES && r->count <= 16)
            {
                for (int b = 0; b < r->count; b++)
                    memcpy(p.rig[r->first + b], r->m[b], sizeof(float) * 12);
                p.rigMask |= (r->first == 0) ? 1u : 2u;
                p.rigTick = GetTickCount();
            }
        }
        LeaveCriticalSection(&g_dataCs);
    }
    return 0;
}

static bool StartNet()
{
    WSADATA wsa;
    int e = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (e != 0) { Logf("WSAStartup fallo: %d", e); return false; }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { Logf("socket fallo: %d", WSAGetLastError()); return false; }

    // Evita que un ICMP "puerto inalcanzable" (amigo aun sin el juego abierto)
    // haga fallar recvfrom.
    BOOL nb = FALSE;
    DWORD ret = 0;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &nb, sizeof(nb), nullptr, 0, &ret, nullptr, nullptr);

    DWORD to = 500;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));

    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR)
    {
        Logf("bind al puerto UDP %d fallo: %d (puerto ocupado o firewall)", g_port, WSAGetLastError());
        return false;
    }
    Logf("red: escuchando UDP %d, enviando a %d peers (%d Hz)", g_port, g_nPeers, g_hz);

    HANDLE t1 = CreateThread(nullptr, 0, SendThread, nullptr, 0, nullptr);
    HANDLE t2 = CreateThread(nullptr, 0, RecvThread, nullptr, 0, nullptr);
    if (t1) CloseHandle(t1);
    if (t2) CloseHandle(t2);
    return true;
}

// ----------------------------------------------- informe por segundo de la red

static void NetReport()
{
    Logf("NET envio: %u pkts/s %u B/s errores=%u", (unsigned)g_sentPkts, (unsigned)g_sentBytes, (unsigned)g_sendErr);
    g_sentPkts = 0;
    g_sentBytes = 0;

    static PedInfo peds[256];
    int np = ScanPeds(peds, 256);
    uint32_t local = LocalPed();
    DWORD now = GetTickCount();

    for (int i = 0; i < g_nPeers; i++)
    {
        EnterCriticalSection(&g_dataCs);
        Peer& p = g_peers[i];
        DWORD age = p.metaTotal ? (now - p.lastRecv) : 0;
        uint32_t pk = p.pkts;
        p.pkts = 0;
        char nick[16], state[24];
        memcpy(nick, p.nick, sizeof(nick));
        memcpy(state, p.state, sizeof(state));
        nick[15] = 0;
        state[23] = 0;
        uint8_t act = p.active, deck = p.deck, grip = p.grip, outfit = p.outfit, nbones = p.nbones;
        float pos[3] = { p.pedPos[0], p.pedPos[1], p.pedPos[2] };
        uint32_t lost = p.lost, mask = p.rigMask, metaTotal = p.metaTotal;
        float root[3] = { p.rig[0][9], p.rig[0][10], p.rig[0][11] };
        LeaveCriticalSection(&g_dataCs);

        if (metaTotal == 0)
        {
            Logf("NET %s (%s): sin datos todavia", nick, g_peers[i].ipStr);
            continue;
        }

        // ped del GTA mas cercano a la posicion que ese jugador reporta
        uint32_t best = 0, second = 0;
        float bd = 1e9f, sd = 1e9f;
        for (int k = 0; k < np; k++)
        {
            if (peds[k].ped == local) continue;
            float dx = peds[k].pos[0] - pos[0];
            float dy = peds[k].pos[1] - pos[1];
            float dz = peds[k].pos[2] - pos[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { sd = bd; second = best; bd = d; best = peds[k].ped; }
            else if (d < sd) { sd = d; second = peds[k].ped; }
        }
        Logf("NET %s: %u pkts/s edad=%lums perdidos=%u act=%d state=%s deck=%d grip=%d outfit=%d huesos=%d mask=%u pos=%.1f %.1f %.1f raiz0=%.2f %.2f %.2f",
             nick, (unsigned)pk, (unsigned long)age, (unsigned)lost, (int)act, state, (int)deck, (int)grip, (int)outfit,
             (int)nbones, (unsigned)mask, pos[0], pos[1], pos[2], root[0], root[1], root[2]);
        if (best)
            Logf("     ped candidato=%08X dist=%.2f m (segundo=%08X a %.2f m) peds en pool=%d",
                 (unsigned)best, sqrtf(bd), (unsigned)second, second ? sqrtf(sd) : -1.0f, np);
    }
}

// ------------------------------------------------------------ arranque

static DWORD WINAPI InitThread(LPVOID)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);

    strcpy(g_dir, path);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;

    strcpy(g_iniPath, path);
    char* dot = strrchr(g_iniPath, '.');
    if (dot) strcpy(dot, ".ini");

    char* dot2 = strrchr(path, '.');
    if (dot2) strcpy(dot2, ".log");
    g_log = fopen(path, "w");

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    Logf("gtskate_net fase 2A (espia + red) cargado en %s", exe);

    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES)
    {
        WriteTemplateIni(g_iniPath);
        Logf("no existia %s: creada una plantilla. EDITALA (tu nick y las IPs de los otros) y reinicia el juego.", g_iniPath);
    }
    LoadConfig();

    HMODULE m = nullptr;
    for (int i = 0; i < 12000 && !m; i++)   // hasta ~10 min
    {
        m = GetModuleHandleA("skate_ffi.dll");
        if (!m) Sleep(50);
    }
    if (!m) { Logf("skate_ffi.dll no se cargo nunca; me rindo"); return 0; }

    char dll[MAX_PATH];
    GetModuleFileNameA(m, dll, MAX_PATH);
    Logf("skate_ffi.dll detectada: %s (base %p)", dll, (void*)m);

    pState = (FnState)GetProcAddress(m, "sk_state");
    Logf("sk_state @%p", (void*)pState);

    MH_STATUS st = MH_Initialize();
    Logf("MH_Initialize -> %s", MH_StatusToString(st));

    Hook(m, "sk_get_bones",  (void*)d_bones,    (void**)&o_bones);
    Hook(m, "sk_board_pose", (void*)d_board,    (void**)&o_board);
    Hook(m, "sk_get_pose",   (void*)d_pose,     (void**)&o_pose);
    Hook(m, "sk_rig_pose",   (void*)d_rigpose,  (void**)&o_rigpose);
    Hook(m, "sk_rig_setup",  (void*)d_rigsetup, (void**)&o_rigsetup);
    Hook(m, "sk_board_mesh", (void*)d_mesh,     (void**)&o_mesh);

    st = MH_EnableHook(MH_ALL_HOOKS);
    Logf("MH_EnableHook(todos) -> %s", MH_StatusToString(st));

    if (g_nPeers > 0) StartNet();
    else Logf("sin peers en el ini: la red no se inicia");

    Logf("Listo. Patina un rato con tus amigos conectados y manda este log.");

    Stat* all[] = { &S_bones, &S_board, &S_pose, &S_rigpose, &S_rigsetup, &S_mesh };
    for (;;)
    {
        Sleep(1000);
        char line[400];
        int len = 0;
        line[0] = 0;
        for (Stat* s : all)
            len += snprintf(line + len, sizeof(line) - len, "%s=%ld/s ", s->name, (long)InterlockedExchange(&s->window, 0));
        char stt[24];
        EnterCriticalSection(&g_dataCs);
        memcpy(stt, g_stateStr, sizeof(stt));
        LeaveCriticalSection(&g_dataCs);
        stt[23] = 0;
        float pp[3] = { 0, 0, 0 };
        bool okp = PedPos(LocalPed(), pp);
        Logf("HB %s| state=%s session=%p ped=%08X pos=%.2f %.2f %.2f%s", line, stt, g_session,
             (unsigned)LocalPed(), pp[0], pp[1], pp[2], okp ? "" : " (sin ped)");
        if (g_sock != INVALID_SOCKET) NetReport();
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_dataCs);
        g_t0 = GetTickCount();
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
