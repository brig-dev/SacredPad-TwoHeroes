// Hooks.cpp - intercettazione delle funzioni di input nella tabella di import
// di Sacred.exe (strategia B2 del documento di progetto).
// Linguaggio: C++17, MSVC, Win32 (x86).
//
// Quando g_virt.active e' true il gioco "vede" il puntatore e i pulsanti
// virtuali guidati dal pad; altrimenti tutto passa alle funzioni originali e
// mouse e tastiera funzionano come sempre. Le voci si cambiano solo nella
// tabella di import di Sacred.exe: pHD.dll, dgVoodoo e Windows non sono toccati.

#include "SacredPad.h"
#include <cstring>
#include <tlhelp32.h>
#include <set>
#include <intrin.h>
#include <cstddef>
#include <mutex>
#include <vector>

VirtualInput g_virt;
std::atomic<DWORD> g_noCtrlTid{ 0 };

bool VirtualKeyDown(int vk)
{
    // Varianti sinistra/destra dei modificatori: valgono come il tasto generico.
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
    if (vk == VK_LMENU    || vk == VK_RMENU)    vk = VK_MENU;
    if (vk == VK_LSHIFT   || vk == VK_RSHIFT)   vk = VK_SHIFT;
    if (vk < 0 || vk > 255) return false;
    DWORD until = g_virt.keyUntil[vk].load();
    if (until == 0) return false;
    if (until == VirtualInput::kHeld) return true;
    return static_cast<LONG>(until - GetTickCount()) > 0;
}

namespace {

using GetCursorPos_t     = BOOL  (WINAPI*)(LPPOINT);
using SetCursorPos_t     = BOOL  (WINAPI*)(int, int);
using GetAsyncKeyState_t = SHORT (WINAPI*)(int);
using GetKeyboardState_t = BOOL  (WINAPI*)(PBYTE);

GetCursorPos_t     o_GetCursorPos     = nullptr;
SetCursorPos_t     o_SetCursorPos     = nullptr;
GetAsyncKeyState_t o_GetAsyncKeyState = nullptr;
GetKeyboardState_t o_GetKeyboardState = nullptr;

// Co-op (0.19.4): le chiamate del gioco che spostano la finestra passano da qui, cosi' nasce gia' dove serve.
using SetWindowPos_t = BOOL (WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using MoveWindow_t   = BOOL (WINAPI*)(HWND, int, int, int, int, BOOL);
SetWindowPos_t o_SetWindowPos = nullptr;
MoveWindow_t   o_MoveWindow   = nullptr;

// Co-op (0.20.12): chi esce (o avvia una partita) cerca con FindWindowA la finestra "SACREDGAMESERVERCLASS" e le
// manda il comando di spegnimento: l'ospite, che non ha un server suo, spegneva quello dell'host e lo scolliegava.
using FindWindowA_t = HWND (WINAPI*)(LPCSTR, LPCSTR);
FindWindowA_t o_FindWindowA = nullptr;
HWND WINAPI Hook_FindWindowA(LPCSTR cls, LPCSTR title)
{
    if (g_cfg.coopRole == 2 && cls && strcmp(cls, "SACREDGAMESERVERCLASS") == 0) return nullptr;
    return o_FindWindowA(cls, title);
}

BOOL WINAPI Hook_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    CoopWindowCall("SetWindowPos", h, x, y, cx, cy, flags);
    if (!(flags & SWP_NOMOVE)) CoopWindowOverride(h, x, y);
    return o_SetWindowPos(h, after, x, y, cx, cy, flags);
}

BOOL WINAPI Hook_MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL repaint)
{
    CoopWindowCall("MoveWindow", h, x, y, cx, cy, 0);
    CoopWindowOverride(h, x, y);
    return o_MoveWindow(h, x, y, cx, cy, repaint);
}

BOOL WINAPI Hook_GetCursorPos(LPPOINT p)
{
    if (p && g_virt.active.load()) {
        p->x = g_virt.x.load();
        p->y = g_virt.y.load();
        return TRUE;
    }
    return o_GetCursorPos(p);
}

BOOL WINAPI Hook_SetCursorPos(int x, int y)
{
    // Se il gioco riposiziona il puntatore mentre si usa il pad, sposta quello
    // virtuale e lascia fermo quello di Windows.
    if (g_virt.active.load()) {
        g_virt.x.store(x);
        g_virt.y.store(y);
        return TRUE;
    }
    return o_SetCursorPos(x, y);
}

SHORT WINAPI Hook_GetAsyncKeyState(int vKey)
{
    // 0.55.14: ordini della mod all'eroe 2 (NoCtrlScope in Memory.cpp): Ctrl su, vero o virtuale, per il thread che li manda
    if (vKey == VK_CONTROL || vKey == VK_LCONTROL || vKey == VK_RCONTROL) {
        const DWORD t = g_noCtrlTid.load();
        if (t != 0 && t == GetCurrentThreadId()) return 0;
        // 0.55.37: dentro il gestore del giocatore 2 (Pad2HandlerStub in Memory.cpp) vale solo il Ctrl del pad 2: quello del pad 1
        // (Ctrl+clic, arti) faceva colpire l'eroe 2 sul posto invece di camminare. Il segno vale al massimo 1 s (un'eccezione
        // dentro il gestore non lo lascia acceso per sempre).
        const DWORD ht = g_p2HandlerTid.load();
        if (ht != 0 && ht == GetCurrentThreadId() && GetTickCount() - g_p2HandlerAt.load() < 1000) {
            const DWORD cu = g_p2CtrlUntil.load();
            return (cu != 0 && static_cast<LONG>(GetTickCount() - cu) < 0) ? static_cast<SHORT>(0x8000) : static_cast<SHORT>(0);
        }
    }
    // 0.55.14: con PadSource=None la tastiera vera e' del giocatore 1: il gestore del giocatore 2 del gioco (FUN_006108a0,
    // frecce, Y, X, Ctrl+1/2/0) vede solo i tasti virtuali del pad 2, altrimenti le frecce e Y del giocatore 1 muoverebbero
    // l'eroe 2
    if (g_cfg.padSource == PadSource::None && g_cfg.nativeEnabled && g_cfg.nativePad2) {
        const DWORD ra = static_cast<DWORD>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
        if (ra >= 0x006108A0 && ra < 0x00611700) return VirtualKeyDown(vKey) ? static_cast<SHORT>(0x8000) : static_cast<SHORT>(0);
    }
    SHORT r = o_GetAsyncKeyState(vKey);
    bool down = VirtualKeyDown(vKey);
    if (g_virt.active.load())
        down = down || (vKey == VK_LBUTTON && g_virt.lmb.load()) || (vKey == VK_RBUTTON && g_virt.rmb.load());
    if (down) r = static_cast<SHORT>(r | 0x8000);   // bit alto = tasto premuto ora
    return r;
}

BOOL WINAPI Hook_GetKeyboardState(PBYTE keys)
{
    BOOL ok = o_GetKeyboardState(keys);
    if (ok && keys) {
        if (g_virt.active.load()) {
            if (g_virt.lmb.load()) keys[VK_LBUTTON] |= 0x80;
            if (g_virt.rmb.load()) keys[VK_RBUTTON] |= 0x80;
        }
        for (int vk = 1; vk < 256; ++vk)
            if (VirtualKeyDown(vk)) keys[vk] |= 0x80;
        if (keys[VK_CONTROL] & 0x80) keys[VK_LCONTROL] |= 0x80;
        if (keys[VK_MENU]    & 0x80) keys[VK_LMENU]    |= 0x80;
        if (keys[VK_SHIFT]   & 0x80) keys[VK_LSHIFT]   |= 0x80;   // B tenuto: smonta da cavallo (0.9.3)
    }
    return ok;
}

// Sostituisce una voce della tabella di import di un modulo (Sacred.exe o pHD.dll).
// Restituisce l'indirizzo che c'era prima (la funzione originale) o nullptr.
void* PatchImport(HMODULE module, const char* dllName, const char* funcName, void* hook)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;

    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dllName) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + imp->OriginalFirstThunk);
        auto* iat   = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) != 0) continue;

            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old)) {
                Log("ERRORE: VirtualProtect su %s fallita (%lu)", funcName, GetLastError());
                return nullptr;
            }
            void* prev = reinterpret_cast<void*>(InterlockedExchange(
                reinterpret_cast<volatile LONG*>(&iat->u1.Function),
                static_cast<LONG>(reinterpret_cast<ULONG_PTR>(hook))));
            VirtualProtect(&iat->u1.Function, sizeof(DWORD), old, &old);
            Log("  hook %-17s voce %p: %p -> %p", funcName, &iat->u1.Function, prev, hook);
            return prev;
        }
    }
    Log("ERRORE: %s!%s non trovata nella tabella di import", dllName, funcName);
    return nullptr;
}

// 0.55.45: musica spenta nelle copie del gioco che non devono suonarla. Prova del 01/11: nello schermo diviso "la musica si
// sentiva ancora doppia" anche con MUSICVOLUME 0 nell'ospite (0.55.37): la copia da cui parte la sessione resta aperta e nascosta
// sul menu principale e continua a suonare la sua musica. La musica di Sacred sono gli stream di Miles (mss32: _AIL_open_stream@12,
// _AIL_set_stream_volume_pan@12 con volume e bilanciamento float, _AIL_close_stream@4, importati da Sacred.exe): la mod tiene
// l'elenco degli stream aperti con il volume chiesto dal gioco e, con MusicMute, li porta a zero (e ci tiene quelli nuovi).
typedef void* (__stdcall* AilOpenStream_t)(void* dig, const char* name, int mem);
typedef void  (__stdcall* AilCloseStream_t)(void* stream);
typedef void  (__stdcall* AilStreamVolPan_t)(void* stream, float volume, float pan);
AilOpenStream_t   o_AilOpenStream = nullptr;
AilCloseStream_t  o_AilCloseStream = nullptr;
AilStreamVolPan_t o_AilStreamVolPan = nullptr;
struct MusicStream { void* s; float vol, pan; };
std::mutex g_musicMx;
std::vector<MusicStream> g_musicStreams;
std::atomic<bool> g_musicMuted{ false };

void* __stdcall Hook_AilOpenStream(void* dig, const char* name, int mem)
{
    void* s = o_AilOpenStream(dig, name, mem);
    if (s) {
        std::lock_guard<std::mutex> lk(g_musicMx);
        g_musicStreams.push_back(MusicStream{ s, 1.0f, 0.5f });
        if (g_musicMuted.load() && o_AilStreamVolPan) o_AilStreamVolPan(s, 0.0f, 0.5f);
    }
    return s;
}

void __stdcall Hook_AilCloseStream(void* s)
{
    {
        std::lock_guard<std::mutex> lk(g_musicMx);
        for (size_t i = 0; i < g_musicStreams.size(); ++i)
            if (g_musicStreams[i].s == s) { g_musicStreams.erase(g_musicStreams.begin() + static_cast<std::ptrdiff_t>(i)); break; }
    }
    o_AilCloseStream(s);
}

void __stdcall Hook_AilStreamVolPan(void* s, float vol, float pan)
{
    {
        std::lock_guard<std::mutex> lk(g_musicMx);
        for (MusicStream& m : g_musicStreams) if (m.s == s) { m.vol = vol; m.pan = pan; break; }
    }
    o_AilStreamVolPan(s, g_musicMuted.load() ? 0.0f : vol, pan);
}

void MusicMuteImpl(bool on)
{
    if (g_musicMuted.exchange(on) == on || !o_AilStreamVolPan) return;
    std::lock_guard<std::mutex> lk(g_musicMx);
    for (const MusicStream& m : g_musicStreams) o_AilStreamVolPan(m.s, on ? 0.0f : m.vol, m.pan);
    Log("Musica %s (%u stream aperti)", on ? "spenta in questa copia del gioco" : "di nuovo accesa", static_cast<unsigned>(g_musicStreams.size()));
}

void InstallMusicHooks()
{
    const HMODULE exe = GetModuleHandleA(nullptr);
    void* a = PatchImport(exe, "mss32.dll", "_AIL_open_stream@12", reinterpret_cast<void*>(&Hook_AilOpenStream));
    void* b = PatchImport(exe, "mss32.dll", "_AIL_close_stream@4", reinterpret_cast<void*>(&Hook_AilCloseStream));
    void* c = PatchImport(exe, "mss32.dll", "_AIL_set_stream_volume_pan@12", reinterpret_cast<void*>(&Hook_AilStreamVolPan));
    o_AilOpenStream = reinterpret_cast<AilOpenStream_t>(a);
    o_AilCloseStream = reinterpret_cast<AilCloseStream_t>(b);
    o_AilStreamVolPan = reinterpret_cast<AilStreamVolPan_t>(c);
    if (!a || !b || !c) {   // senza tutti e tre si rimette com'era: niente ganci a meta'
        if (a) PatchImport(exe, "mss32.dll", "_AIL_open_stream@12", a);
        if (b) PatchImport(exe, "mss32.dll", "_AIL_close_stream@4", b);
        if (c) PatchImport(exe, "mss32.dll", "_AIL_set_stream_volume_pan@12", c);
        o_AilOpenStream = nullptr; o_AilCloseStream = nullptr; o_AilStreamVolPan = nullptr;
        Log("Musica: ganci degli stream di Miles NON installati");
    }
}

template <typename T>
bool Install(const char* func, T hook, T& original)
{
    void* prev = PatchImport(GetModuleHandleA(nullptr), "USER32.dll", func, reinterpret_cast<void*>(hook));
    if (!prev) return false;
    original = reinterpret_cast<T>(prev);
    return true;
}

} // namespace

// Co-op: aggancia SetWindowPos e MoveWindow nei moduli del gioco (exe, pHD.dll, dgVoodoo...). Si richiama
// piu' volte perche' alcuni moduli si caricano dopo.
void HookWindowCalls()
{
    static std::set<HMODULE> done;
    if (!o_SetWindowPos) {
        HMODULE u = GetModuleHandleA("user32.dll");
        o_SetWindowPos = reinterpret_cast<SetWindowPos_t>(GetProcAddress(u, "SetWindowPos"));
        o_MoveWindow   = reinterpret_cast<MoveWindow_t>(GetProcAddress(u, "MoveWindow"));
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    wchar_t dirW[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, GameDir(), -1, dirW, MAX_PATH);
    const size_t dirLen = wcslen(dirW);
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        HMODULE m = me.hModule;
        if (m == SelfModule() || done.count(m)) continue;
        if (m != GetModuleHandleA(nullptr) && _wcsnicmp(me.szExePath, dirW, dirLen) != 0) continue;   // solo i moduli del gioco
        done.insert(m);
        bool a = PatchImport(m, "USER32.dll", "SetWindowPos", reinterpret_cast<void*>(&Hook_SetWindowPos)) != nullptr;
        bool b = PatchImport(m, "USER32.dll", "MoveWindow",   reinterpret_cast<void*>(&Hook_MoveWindow)) != nullptr;
        if (a || b) Log("Coop: SetWindowPos %s, MoveWindow %s agganciate in %ls", a ? "si'" : "no", b ? "si'" : "no", me.szModule);
    }
    CloseHandle(snap);
}

bool InstallHooks()
{
    // Le originali servono prima degli hook: le leggiamo da user32.
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) { Log("ERRORE: user32.dll non caricata"); return false; }

    Log("Installazione degli hook di input:");
    bool ok = true;
    ok &= Install("GetCursorPos",     &Hook_GetCursorPos,     o_GetCursorPos);
    ok &= Install("SetCursorPos",     &Hook_SetCursorPos,     o_SetCursorPos);
    ok &= Install("GetAsyncKeyState", &Hook_GetAsyncKeyState, o_GetAsyncKeyState);
    ok &= Install("GetKeyboardState", &Hook_GetKeyboardState, o_GetKeyboardState);

    // Se una voce mancasse, la versione originale va comunque presa da user32
    // per non lasciare puntatori nulli.
    if (!o_GetCursorPos)     o_GetCursorPos     = reinterpret_cast<GetCursorPos_t>(GetProcAddress(user32, "GetCursorPos"));
    if (!o_SetCursorPos)     o_SetCursorPos     = reinterpret_cast<SetCursorPos_t>(GetProcAddress(user32, "SetCursorPos"));
    if (!o_GetAsyncKeyState) o_GetAsyncKeyState = reinterpret_cast<GetAsyncKeyState_t>(GetProcAddress(user32, "GetAsyncKeyState"));
    if (!o_GetKeyboardState) o_GetKeyboardState = reinterpret_cast<GetKeyboardState_t>(GetProcAddress(user32, "GetKeyboardState"));

    Log(ok ? "Hook installati: 4 su 4" : "ATTENZIONE: non tutti gli hook sono stati installati");

    // pHD.dll (pureHD) legge lo stato dei tasti con la propria GetKeyboardState
    // (per esempio per la minimappa): anche quella deve vedere i tasti del pad.
    if (HMODULE phd = GetModuleHandleA("pHD.dll")) {
        Log("pHD.dll presente: hook di GetKeyboardState anche nella sua tabella di import");
        PatchImport(phd, "USER32.dll", "GetKeyboardState", reinterpret_cast<void*>(&Hook_GetKeyboardState));
    }
    if (g_cfg.coopRole == 2) {
        if (HMODULE u = GetModuleHandleA("user32.dll")) o_FindWindowA = reinterpret_cast<FindWindowA_t>(GetProcAddress(u, "FindWindowA"));
        if (o_FindWindowA) PatchImport(GetModuleHandleA(nullptr), "USER32.dll", "FindWindowA", reinterpret_cast<void*>(&Hook_FindWindowA));
    }
    if (g_cfg.coopRole && g_cfg.coopW > 0 && g_cfg.coopHookWindows) HookWindowCalls();
    InstallMusicHooks();   // 0.55.45
    return ok;
}

BOOL RealGetCursorPos(LPPOINT p)
{
    return o_GetCursorPos ? o_GetCursorPos(p) : GetCursorPos(p);
}

// Sposta il puntatore vero di Windows (0.9.13, schede del mercante): il gioco non lo vede
// passare da qui, perche' le sue chiamate a SetCursorPos vanno a Hook_SetCursorPos.
BOOL RealSetCursorPos(int x, int y)
{
    return o_SetCursorPos ? o_SetCursorPos(x, y) : SetCursorPos(x, y);
}

// 0.55.45: fuori dallo spazio anonimo (Coop.cpp la chiama).
void MusicMute(bool on) { MusicMuteImpl(on); }
