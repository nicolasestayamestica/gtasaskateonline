// gtskate_net - FASE 1: "espia"
//
// Se carga como un .asi mas. Espera a que gtskate.asi cargue skate_ffi.dll,
// engancha (MinHook) algunas de sus funciones exportadas y escribe en
// gtskate_net.log lo que ve: estado del skater, pose de la tabla, huesos...
// NO cambia nada del juego ni del mod: llama a la funcion original y
// devuelve su resultado tal cual.
//
// Con ese log confirmamos los formatos reales de los datos antes de la
// fase 2 (enviar la tabla por red).

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "MinHook.h"

// ---------------------------------------------------------------- log

static HMODULE          g_self = nullptr;
static FILE*            g_log  = nullptr;
static CRITICAL_SECTION g_cs;
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

// Igual, pero en hexadecimal (para cabeceras / enteros).
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

// Para funciones cuyos argumentos aun no conocemos: muestra los valores y,
// si parecen punteros, lo que hay detras.
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

// ----------------------------------------------------------- detours
// Todas las funciones de skate_ffi.dll son cdecl (comprobado en el
// desensamblado: todas terminan en "ret" sin limpiar pila).

typedef int (__cdecl* T3)(void*, void*, uint32_t);
typedef int (__cdecl* T2)(void*, void*);
typedef int (__cdecl* T8)(uint32_t, uint32_t, uint32_t, uint32_t,
                          uint32_t, uint32_t, uint32_t, uint32_t);

static T3 o_bones = nullptr;
static T3 o_board = nullptr;
static T2 o_pose  = nullptr;
static T8 o_rigpose = nullptr, o_rigsetup = nullptr, o_mesh = nullptr;

// sk_get_bones(session, out, n): copia n huesos de 64 bytes (matriz 4x4)
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

// sk_board_pose(session, out, len): pose de las partes de la tabla
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

// sk_get_pose(session, out): 4 bytes de cabecera + 132 bytes de pose
static int __cdecl d_pose(void* s, void* out)
{
    bool lg = Sample(S_pose);
    g_session = s;
    int r = o_pose(s, out);
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

// Funciones con argumentos aun desconocidos: se reenvian 8 dwords tal cual.
static int __cdecl d_rigpose(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                             uint32_t e, uint32_t f, uint32_t g, uint32_t h)
{
    bool lg = Sample(S_rigpose);
    uint32_t v[6] = { a, b, c, d, e, f };
    if (lg) ArgsDump("sk_rig_pose PRE ", v, 4);
    int r = o_rigpose(a, b, c, d, e, f, g, h);
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

// ------------------------------------------------------- instalar hooks

static bool Hook(HMODULE m, const char* name, void* detour, void** orig)
{
    void* tgt = (void*)GetProcAddress(m, name);
    if (!tgt) { Logf("hook %s: la DLL no la exporta", name); return false; }
    MH_STATUS st = MH_CreateHook(tgt, detour, orig);
    Logf("hook %s @%p -> %s", name, tgt, MH_StatusToString(st));
    return st == MH_OK;
}

// Posicion del jugador local (solo para comparar con "skater=" del log del mod).
// 0xB7CD98 = CWorld::Players[0].m_pPed ; 0xB6F5F0 = puntero rapido al ped local
// (GTA SA 1.0 US). Todo protegido con SEH.
static void PedStr(char* out, size_t cap)
{
    snprintf(out, cap, "ped=?");
    __try
    {
        uint32_t pa = *(uint32_t*)0xB7CD98;
        uint32_t pb = *(uint32_t*)0xB6F5F0;
        uint32_t ped = pa ? pa : pb;
        if (LooksPtr(ped))
        {
            uint32_t mtx = *(uint32_t*)(ped + 0x14);
            const float* p = LooksPtr(mtx) ? (const float*)(mtx + 0x30) : (const float*)(ped + 4);
            snprintf(out, cap, "pedA=%08X pedB=%08X pos=%.2f %.2f %.2f", (unsigned)pa, (unsigned)pb, p[0], p[1], p[2]);
        }
        else
        {
            snprintf(out, cap, "pedA=%08X pedB=%08X (sin ped)", (unsigned)pa, (unsigned)pb);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        snprintf(out, cap, "ped=<err>");
    }
}

static DWORD WINAPI InitThread(LPVOID)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".log");
    g_log = fopen(path, "w");

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    Logf("gtskate_net fase 1 (espia) cargado en %s", exe);

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
    Logf("Listo. Patina un rato (suelo, aire, tricks, caidas) y manda este log.");

    // Latido: una vez por segundo, cuantas llamadas por segundo hace el mod
    // a cada funcion, el estado actual y la posicion del ped.
    Stat* all[] = { &S_bones, &S_board, &S_pose, &S_rigpose, &S_rigsetup, &S_mesh };
    for (;;)
    {
        Sleep(1000);
        char line[400];
        int len = 0;
        line[0] = 0;
        for (Stat* s : all)
            len += snprintf(line + len, sizeof(line) - len, "%s=%ld/s ", s->name, (long)InterlockedExchange(&s->window, 0));
        char stt[48], ped[160];
        StateStr(g_session, stt, sizeof(stt));
        PedStr(ped, sizeof(ped));
        Logf("HB %s| state=%s session=%p %s", line, stt, g_session, ped);
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
        g_t0 = GetTickCount();
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
