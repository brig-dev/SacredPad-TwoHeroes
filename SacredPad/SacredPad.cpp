// SacredPad.cpp - proxy di version.dll, log, DllMain
// Linguaggio: C++17. Compilatore: MSVC (Visual Studio Community 2026).
// Piattaforma: Win32 (x86), Release, runtime statico /MT.
//
// Questo file contiene solo la parte "proxy": si fa caricare da Sacred.exe al
// posto di version.dll, inoltra le 17 funzioni alla DLL vera di sistema,
// apre SacredPad.log e avvia il thread principale (Core.cpp).

#include "SacredPad.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {

HMODULE          g_self = nullptr;
HMODULE          g_real = nullptr;
HANDLE           g_log  = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock;
char             g_dir[MAX_PATH] = {};

// 0.49.1: archivio dei log. Prima di essere riscritto, ogni log (SacredPad.log, e in co-op a schermo diviso quelli
// dell'host e dell'ospite) si copia nella cartella SacredPad_log con la data e l'ora dell'ultima scrittura nel nome:
// chiudere e riaprire il gioco (anche piu' volte, anche sulla Steam Machine) non perde piu' nulla. Si tengono gli
// ultimi kLogKeep file, i piu' vecchi si cancellano.
// 0.56.58 (pubblicazione, il tester: "non si deve sovrascrivere ogni volta [...] magari dopo 50"): il numero da [Log] Keep
// dell'ini (di serie 50; 0 = tutti, non si cancella niente), letto qui perche' il log si apre prima della configurazione.
int g_logKeep = 50;

void ArchiveLog(const char* path, const char* tag)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a) || (a.nFileSizeHigh == 0 && a.nFileSizeLow == 0)) return;
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\SacredPad_log", g_dir);
    CreateDirectoryA(dir, nullptr);
    FILETIME lt{}; SYSTEMTIME st{};
    FileTimeToLocalFileTime(&a.ftLastWriteTime, &lt);
    FileTimeToSystemTime(&lt, &st);
    char dst[MAX_PATH];
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\%s_%04u%02u%02u_%02u%02u%02u.log", dir, tag, st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
    CopyFileA(path, dst, FALSE);
    // pulizia: i nomi hanno la data, quindi l'ordine alfabetico e' quello cronologico (per ciascun tag)
    char pat[MAX_PATH];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*.log", dir);
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    const int kLogKeep = g_logKeep;
    if (kLogKeep <= 0 || static_cast<int>(names.size()) <= kLogKeep) return;
    std::sort(names.begin(), names.end(), [](const std::string& x, const std::string& y) {
        return x.substr(x.size() >= 19 ? x.size() - 19 : 0) < y.substr(y.size() >= 19 ? y.size() - 19 : 0);   // per data
    });
    for (size_t i = 0; i + kLogKeep < names.size(); ++i) {
        char f[MAX_PATH];
        _snprintf_s(f, sizeof(f), _TRUNCATE, "%s\\%s", dir, names[i].c_str());
        DeleteFileA(f);
    }
}

void OpenLog()
{
    InitializeCriticalSection(&g_logLock);
    GetModuleFileNameA(g_self, g_dir, MAX_PATH);
    if (char* slash = strrchr(g_dir, '\\')) *slash = '\0';
    {
        char ini[MAX_PATH];
        _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\SacredPad.ini", g_dir);
        g_logKeep = static_cast<int>(GetPrivateProfileIntA("Log", "Keep", 50, ini));
    }

    char path[MAX_PATH];
    {   // GameServer.exe carica la stessa DLL: log separato per non rubare quello del gioco
        char me[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, me, MAX_PATH);
        if (strstr(me, "GameServer.exe") || strstr(me, "gameserver.exe")) {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s/SacredPad_server.log", g_dir);
            g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            return;
        }
    }
    // 0.20.9: in sessione co-op l'host gira mentre la prima istanza (nascosta) tiene aperto SacredPad.log: l'host e
    // l'ospite scrivono su file propri, altrimenti l'apertura falliva e non c'era nessun log.
    char role[16] = "";
    if (GetEnvironmentVariableA("SACREDPAD_COOP", role, sizeof(role)) > 0) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\SacredPad_%s.log", g_dir, _stricmp(role, "Host") == 0 ? "host" : "guest");
        ArchiveLog(path, _stricmp(role, "Host") == 0 ? "SacredPad_host" : "SacredPad_guest");
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return;
    }
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\SacredPad.log", g_dir);
    // 0.18.11: il log della sessione precedente si conserva come SacredPad_precedente.log (sulla Steam
    // Machine, se il gioco si chiude o si riavvia prima di copiare il log, non va perso).
    char prev[MAX_PATH];
    _snprintf_s(prev, sizeof(prev), _TRUNCATE, "%s\\SacredPad_precedente.log", g_dir);
    ArchiveLog(path, "SacredPad");
    MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

void Log(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;

    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u][T%05lu] ",
                        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    if (n < 0) n = 0;

    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(line + n, sizeof(line) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) m = static_cast<int>(strlen(line + n));
    // 0.56.58 (pubblicazione): la cartella dell'utente di Windows (USERPROFILE, con il nome dell'utente) non finisce mai nel
    // log, che i giocatori mandano nelle segnalazioni: diventa %USERPROFILE%
    {
        static char prof[MAX_PATH] = {};
        static const size_t plen = [] {
            const DWORD k = GetEnvironmentVariableA("USERPROFILE", prof, MAX_PATH);
            return (k > 3 && k < MAX_PATH) ? static_cast<size_t>(k) : size_t(0);
        }();
        if (plen) {
            static const char kRep[] = "%USERPROFILE%";
            const size_t rl = sizeof(kRep) - 1;
            for (char* q = line + n; *q; ++q) {
                if (_strnicmp(q, prof, plen) != 0) continue;
                const size_t tail = strlen(q + plen);
                if (static_cast<size_t>(q - line) + rl + tail + 3 > sizeof(line)) break;   // posto per \r\n
                memmove(q + rl, q + plen, tail + 1);
                memcpy(q, kRep, rl);
                q += rl - 1;
            }
            m = static_cast<int>(strlen(line + n));
        }
        // sotto Proton la cartella del gioco e' Z:\home\<utente di Linux>\...: il nome diventa %USER%
        static const char kHome[] = "\\home\\", kUser[] = "%USER%";
        for (char* q = line + n; *q; ++q) {
            if (_strnicmp(q, kHome, sizeof(kHome) - 1) != 0) continue;
            char* nm = q + sizeof(kHome) - 1;
            char* e = nm;
            while (*e && *e != '\\' && *e != '/' && *e != ' ' && *e != '"') ++e;
            const size_t nl = static_cast<size_t>(e - nm), ul = sizeof(kUser) - 1;
            if (nl == 0 || (nl == ul && memcmp(nm, kUser, ul) == 0)) continue;
            const size_t tail = strlen(e);
            if (static_cast<size_t>(nm - line) + ul + tail + 3 > sizeof(line)) break;
            memmove(nm + ul, e, tail + 1);
            memcpy(nm, kUser, ul);
            q = nm + ul - 1;
        }
        m = static_cast<int>(strlen(line + n));
    }
    n += m;
    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    WriteFile(g_log, line, static_cast<DWORD>(n), &written, nullptr);
    LeaveCriticalSection(&g_logLock);
}

const char* GameDir()    { return g_dir; }
HMODULE     SelfModule() { return g_self; }

// ---------------------------------------------------------------------------
// Proxy: le 17 funzioni di version.dll
// ---------------------------------------------------------------------------
namespace {

// Ripieghi per funzioni mancanti nella DLL di sistema (es. sotto Wine):
// ripuliscono lo stack come la funzione vera (__stdcall) e restituiscono 0.
BOOL WINAPI Missing8 (DWORD, DWORD)                                           { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
BOOL WINAPI Missing12(DWORD, DWORD, DWORD)                                    { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
BOOL WINAPI Missing16(DWORD, DWORD, DWORD, DWORD)                             { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
BOOL WINAPI Missing20(DWORD, DWORD, DWORD, DWORD, DWORD)                      { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
BOOL WINAPI Missing32(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }

struct ProxyEntry { const char* name; int argBytes; };

// L'ordine DEVE corrispondere agli indici di PROXY_STUB.
const ProxyEntry kEntries[] = {
    { "GetFileVersionInfoA",        16 },  //  0
    { "GetFileVersionInfoByHandle", 16 },  //  1 non documentata: 16 e' un'ipotesi
    { "GetFileVersionInfoExA",      20 },  //  2
    { "GetFileVersionInfoExW",      20 },  //  3
    { "GetFileVersionInfoSizeA",     8 },  //  4
    { "GetFileVersionInfoSizeExA",  12 },  //  5
    { "GetFileVersionInfoSizeExW",  12 },  //  6
    { "GetFileVersionInfoSizeW",     8 },  //  7
    { "GetFileVersionInfoW",        16 },  //  8
    { "VerFindFileA",               32 },  //  9
    { "VerFindFileW",               32 },  // 10
    { "VerInstallFileA",            32 },  // 11
    { "VerInstallFileW",            32 },  // 12
    { "VerLanguageNameA",           12 },  // 13
    { "VerLanguageNameW",           12 },  // 14
    { "VerQueryValueA",             16 },  // 15
    { "VerQueryValueW",             16 },  // 16
};
constexpr int kCount = sizeof(kEntries) / sizeof(kEntries[0]);

FARPROC MissingFor(int argBytes)
{
    switch (argBytes) {
    case 8:  return reinterpret_cast<FARPROC>(&Missing8);
    case 12: return reinterpret_cast<FARPROC>(&Missing12);
    case 20: return reinterpret_cast<FARPROC>(&Missing20);
    case 32: return reinterpret_cast<FARPROC>(&Missing32);
    default: return reinterpret_cast<FARPROC>(&Missing16);
    }
}

} // namespace

extern "C" FARPROC g_fn[kCount] = {};

// Ogni stub e' un salto all'indirizzo vero: parametri e risultato passano intatti.
#define PROXY_STUB(idx, name) \
    extern "C" __declspec(naked) void Proxy_##name() { __asm { jmp dword ptr [g_fn + (idx) * 4] } }

PROXY_STUB( 0, GetFileVersionInfoA)
PROXY_STUB( 1, GetFileVersionInfoByHandle)
PROXY_STUB( 2, GetFileVersionInfoExA)
PROXY_STUB( 3, GetFileVersionInfoExW)
PROXY_STUB( 4, GetFileVersionInfoSizeA)
PROXY_STUB( 5, GetFileVersionInfoSizeExA)
PROXY_STUB( 6, GetFileVersionInfoSizeExW)
PROXY_STUB( 7, GetFileVersionInfoSizeW)
PROXY_STUB( 8, GetFileVersionInfoW)
PROXY_STUB( 9, VerFindFileA)
PROXY_STUB(10, VerFindFileW)
PROXY_STUB(11, VerInstallFileA)
PROXY_STUB(12, VerInstallFileW)
PROXY_STUB(13, VerLanguageNameA)
PROXY_STUB(14, VerLanguageNameW)
PROXY_STUB(15, VerQueryValueA)
PROXY_STUB(16, VerQueryValueW)

namespace {

bool LoadRealVersionDll()
{
    char sysdir[MAX_PATH];
    UINT len = GetSystemDirectoryA(sysdir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        Log("ERRORE: GetSystemDirectoryA fallita (%lu)", GetLastError());
        return false;
    }
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\version.dll", sysdir);

    g_real = LoadLibraryA(path);
    if (!g_real || g_real == g_self) {
        Log("ERRORE: impossibile caricare la version.dll vera da %s (%lu)", path, GetLastError());
        return false;
    }
    char realPath[MAX_PATH] = {};
    GetModuleFileNameA(g_real, realPath, MAX_PATH);
    Log("version.dll vera caricata: %s (base %p)", realPath, g_real);

    for (int i = 0; i < kCount; ++i) {
        g_fn[i] = GetProcAddress(g_real, kEntries[i].name);
        if (!g_fn[i]) {
            Log("  avviso: %s non esportata dalla DLL di sistema, uso un ripiego", kEntries[i].name);
            g_fn[i] = MissingFor(kEntries[i].argBytes);
        }
    }
    return true;
}

} // namespace

// DllMain: solo il minimo indispensabile, siamo sotto il loader lock.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        OpenLog();

        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("SacredPad %s caricato. Processo: %s (PID %lu)", SACREDPAD_VERSION, exe, GetCurrentProcessId());
        Log("Cartella: %s", g_dir);

        if (!LoadRealVersionDll()) return FALSE;

        // Il thread parte davvero solo dopo che il loader ha finito.
        if (HANDLE th = CreateThread(nullptr, 0, CoreThread, nullptr, 0, nullptr))
            CloseHandle(th);
        else
            Log("ERRORE: CreateThread fallita (%lu)", GetLastError());
    }
    else if (reason == DLL_PROCESS_DETACH) {
        Log("SacredPad scaricato");
    }
    return TRUE;
}
