// Coop.cpp - co-op locale (fase 4): seconda istanza di Sacred e finestre affiancate
// Linguaggio: C++17, MSVC, Win32 (x86).
// Il gioco parte normalmente (Role=Off). Dal menu principale, con il tasto LaunchKey (0.20.0), la mod
// avvia una sessione co-op: salva Settings.cfg, imposta la risoluzione di WindowRect, rilancia Sacred.exe
// con SACREDPAD_COOP=Host e nasconde questa istanza (che resta come "supervisore"); alla fine della
// sessione ripristina Settings.cfg e rimostra la finestra, cioe' si torna al gioco normale. L'host, appena
// pronto, lancia da solo la seconda copia del gioco (GuestDir) con SACREDPAD_COOP=Guest. Le finestre nascono
// dove dice WindowRect (SetWindowPos del gioco intercettata in Hooks.cpp).

#include "SacredPad.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <set>
#include <tlhelp32.h>

namespace {

HANDLE g_guestProc  = nullptr;
bool   g_placed     = false;
bool   g_keyWasDown = false;
DWORD  g_lastCheck  = 0;
int    g_moves      = 0;
bool   g_listed     = false;
DWORD  g_placedAt   = 0;

std::string ReadWhole(const std::string& path)
{
    std::string s;
    if (FILE* f = nullptr; fopen_s(&f, path.c_str(), "rb") == 0 && f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        fclose(f);
    }
    return s;
}

bool WriteWhole(const std::string& path, const std::string& data)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

// "CHIAVE : valore" nei file Settings.cfg: sostituisce il valore della riga, se esiste.
bool SetSettingsValue(const std::string& path, const char* key, int value)
{
    std::string s = ReadWhole(path);
    if (s.empty()) return false;
    const std::string k = std::string(key) + " : ";
    size_t p = 0;
    for (;;) {
        p = s.find(k, p);
        if (p == std::string::npos) return false;
        if (p == 0 || s[p - 1] == '\n') break;
        p += k.size();
    }
    size_t v = p + k.size();
    size_t e = s.find_first_of("\r\n", v);
    if (e == std::string::npos) e = s.size();
    s.replace(v, e - v, std::to_string(value));
    return WriteWhole(path, s);
}

// Valore numerico "CHIAVE : n" di Settings.cfg (-1 se manca).
int GetSettingsValue(const std::string& path, const char* key)
{
    const std::string s = ReadWhole(path);
    const std::string k = std::string(key) + " : ";
    size_t p = 0;
    for (;;) {
        p = s.find(k, p);
        if (p == std::string::npos) return -1;
        if (p == 0 || s[p - 1] == '\n') break;
        p += k.size();
    }
    return atoi(s.c_str() + p + k.size());
}

// 0.21.2: Settings.cfg del tester era rimasto a 1280x720 in finestra dopo una sessione co-op (il gioco non partiva piu'
// a schermo intero, 25/10): la copia di sicurezza era stata fatta da un file gia' modificato. I valori "normali" di
// risoluzione e schermo intero si ricordano quindi nell'ini ([Coop] Normal*) ogni volta che il file non ha quelli
// della co-op, e si rimettono se si trova il file con quelli della co-op fuori sessione.
void RememberOrRepairNormalSettings(bool sessionOver)
{
    if (g_cfg.coopW <= 0 || g_cfg.coopH <= 0) return;
    char ini[MAX_PATH];
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\SacredPad.ini", GameDir());
    const char* files[] = { "\\Settings.cfg", "\\STEAM_Settings.cfg" };
    const char* tags[]  = { "Settings", "SteamSettings" };
    for (int i = 0; i < 2; ++i) {
        const std::string p = std::string(GameDir()) + files[i];
        const int w = GetSettingsValue(p, "PHD_RES_SCX"), h = GetSettingsValue(p, "PHD_RES_SCY"), fs = GetSettingsValue(p, "FULLSCREEN");
        if (w <= 0 || h <= 0 || fs < 0) continue;
        char kw[48], kh[48], kf[48], buf[16];
        _snprintf_s(kw, sizeof(kw), _TRUNCATE, "Normal%sW", tags[i]);
        _snprintf_s(kh, sizeof(kh), _TRUNCATE, "Normal%sH", tags[i]);
        _snprintf_s(kf, sizeof(kf), _TRUNCATE, "Normal%sFullscreen", tags[i]);
        const bool coopLike = (w == g_cfg.coopW && h == g_cfg.coopH && fs == 0);
        if (!coopLike) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", w); WritePrivateProfileStringA("Coop", kw, buf, ini);
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", h); WritePrivateProfileStringA("Coop", kh, buf, ini);
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", fs); WritePrivateProfileStringA("Coop", kf, buf, ini);
        }
        else {
            const int nw = GetPrivateProfileIntA("Coop", kw, 0, ini), nh = GetPrivateProfileIntA("Coop", kh, 0, ini), nf = GetPrivateProfileIntA("Coop", kf, -1, ini);
            if (nw > 0 && nh > 0 && nf >= 0) {
                SetSettingsValue(p, "PHD_RES_SCX", nw);
                SetSettingsValue(p, "PHD_RES_SCY", nh);
                SetSettingsValue(p, "FULLSCREEN", nf);
                Log("Coop: %s aveva le impostazioni della co-op fuori sessione (%dx%d in finestra): rimesse %dx%d, schermo intero %d", files[i] + 1, w, h, nw, nh, nf);
            }
            else if (sessionOver) Log("Coop: %s ha le impostazioni della co-op ma non ci sono valori normali da rimettere", files[i] + 1);
        }
    }
}

// --- Ambiente Wine/Proton (0.21.0) ---------------------------------------------------------------
// Sotto Proton gamescope mostra una sola finestra alla volta. Per avere due istanze affiancate, host e ospite si
// avviano dentro lo stesso desktop virtuale di Wine ("explorer /desktop=nome,LxA"): il desktop e' una finestra sola
// (nera) e le due finestre di Sacred ci stanno dentro, posizionate da [Coop] come su Windows.
bool RunningOnWine()
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != nullptr;
}

bool UseWineDesktop()
{
    if (g_cfg.coopWineDesktop == 0) return false;      // Off
    return g_cfg.coopWineDesktop == 1 || RunningOnWine(); // On / Auto
}

// PID dei processi Sacred.exe in esecuzione (sotto Wine il processo avviato e' explorer.exe, non il gioco: il gioco
// si riconosce come il Sacred.exe nuovo comparso dopo l'avvio).
std::set<DWORD> SacredPids()
{
    std::set<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"Sacred.exe") == 0) out.insert(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

std::set<DWORD> g_sacredBefore;   // Sacred.exe gia' presenti prima di avviare l'host

// 0.53.3: sfondo nero del desktop virtuale. Prova sulla Steam Machine del 29/10: intorno alle due finestre c'era
// l'azzurro predefinito di Wine. Il desktop virtuale dipinge il colore di sistema COLOR_DESKTOP, che explorer.exe legge
// da HKCU\Control Panel\Colors\Background all'avvio: lo si scrive nero prima di avviarlo (solo sotto Wine, nel
// prefisso di Proton del gioco, dove serve solo a questo) e lo si cambia anche per la sessione in corso.
void WineDesktopBlack()
{
    static bool done = false;
    if (done || !RunningOnWine()) return;
    done = true;
    HKEY k = nullptr;
    LONG r = RegCreateKeyExA(HKEY_CURRENT_USER, "Control Panel\\Colors", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (r == ERROR_SUCCESS) {
        static const char kBlack[] = "0 0 0";
        r = RegSetValueExA(k, "Background", 0, REG_SZ, reinterpret_cast<const BYTE*>(kBlack), sizeof(kBlack));
        RegCloseKey(k);
    }
    const INT idx = COLOR_DESKTOP;
    const COLORREF black = RGB(0, 0, 0);
    const BOOL sys = SetSysColors(1, &idx, &black);
    Log("Coop: sfondo del desktop virtuale nero (registro %s, colore di sistema %s)", r == ERROR_SUCCESS ? "si'" : "no", sys ? "si'" : "no");
}

// Avvia Sacred.exe di una cartella, direttamente o dentro il desktop virtuale di Wine.
bool SpawnGame(const std::string& exe, const std::string& dir, PROCESS_INFORMATION& pi)
{
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (UseWineDesktop()) {
        WineDesktopBlack();
        char sys[MAX_PATH] = {}, win[MAX_PATH] = {}, expl[MAX_PATH] = {};
        GetSystemDirectoryA(sys, MAX_PATH);
        GetWindowsDirectoryA(win, MAX_PATH);
        _snprintf_s(expl, sizeof(expl), _TRUNCATE, "%s\\explorer.exe", sys);
        if (GetFileAttributesA(expl) == INVALID_FILE_ATTRIBUTES) _snprintf_s(expl, sizeof(expl), _TRUNCATE, "%s\\explorer.exe", win);
        if (GetFileAttributesA(expl) == INVALID_FILE_ATTRIBUTES) strcpy_s(expl, "explorer.exe");   // lo trova Wine da solo
        char cmd[2 * MAX_PATH + 96];
        // 0.49.3: il desktop virtuale prende la misura dello schermo letta all'avvio. Prova sulla Steam Machine del 29/10:
        // schermo 2560x1440 all'avvio, ma al momento della sessione GetSystemMetrics dava 1920x1080 e le due finestre da
        // 1280x720 non ci stavano affiancate (quella dell'ospite copriva quella dell'host).
        const int dw = g_cfg.coopScreenW > 0 ? g_cfg.coopScreenW : GetSystemMetrics(SM_CXSCREEN);
        const int dh = g_cfg.coopScreenH > 0 ? g_cfg.coopScreenH : GetSystemMetrics(SM_CYSCREEN);
        char scr[32];
        _snprintf_s(scr, sizeof(scr), _TRUNCATE, "%dx%d", dw, dh);
        SetEnvironmentVariableA("SACREDPAD_SCREEN", scr);   // host e ospite lo ereditano e ci calcolano la disposizione
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\" /desktop=SacredPadCoop,%dx%d \"%s\"", expl, dw, dh, exe.c_str());
        Log("Coop: Wine, avvio nel desktop virtuale: %s", cmd);
        return CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi) != 0;
    }
    return CreateProcessA(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi) != 0;
}

void LaunchGuest()
{
    if (g_guestProc && WaitForSingleObject(g_guestProc, 0) == WAIT_TIMEOUT) {
        Log("Coop: la seconda istanza e' gia' avviata");
        return;
    }
    if (g_guestProc) { CloseHandle(g_guestProc); g_guestProc = nullptr; }

    const std::string dir = g_cfg.coopGuestDir;
    const std::string exe = dir + "\\Sacred.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("Coop: Sacred.exe non trovato in %s (GuestDir)", dir.c_str());
        return;
    }
    // L'ospite riceve ruolo, pad, posizione e risoluzione dalla mod, cosi' le due cartelle restano allineate.
    const std::string ini = dir + "\\SacredPad.ini";
    char rect[64];
    _snprintf_s(rect, sizeof(rect), _TRUNCATE, "%d,%d,%d,%d", g_cfg.coopGuestX, g_cfg.coopGuestY, g_cfg.coopGuestW, g_cfg.coopGuestH);
    WritePrivateProfileStringA("Coop", "WindowRect", rect, ini.c_str());
    WritePrivateProfileStringA("Coop", "IgnoreMouse", "true", ini.c_str());
    char idx[16];
    _snprintf_s(idx, sizeof(idx), _TRUNCATE, "%d", g_cfg.coopGuestPad);
    WritePrivateProfileStringA("General", "ControllerIndex", idx, ini.c_str());
    if (g_cfg.coopGuestW > 0 && g_cfg.coopGuestH > 0) {
        for (const char* f : { "\\Settings.cfg", "\\STEAM_Settings.cfg" }) {
            SetSettingsValue(dir + f, "PHD_RES_SCX", g_cfg.coopGuestW);
            SetSettingsValue(dir + f, "PHD_RES_SCY", g_cfg.coopGuestH);
        }
    }
    // 0.55.37: musica dell'ospite spenta. Prova del 31/10 alle 23:21-23:26 sulla Steam Machine: "tracce dell'audio sovrapposte nello
    // split screen" (due istanze, due brani insieme). I suoni e le voci restano: quelli vicini al suo eroe li sente solo l'ospite.
    // [Coop] GuestMusicVolume (0 predefinito, -1 = lascia il valore del suo Settings.cfg).
    if (g_cfg.coopGuestMusic >= 0)
        for (const char* f : { "\\Settings.cfg", "\\STEAM_Settings.cfg" })
            SetSettingsValue(dir + f, "MUSICVOLUME", g_cfg.coopGuestMusic);

    PROCESS_INFORMATION pi{};
    SetEnvironmentVariableA("SACREDPAD_COOP", "Guest");   // il figlio eredita l'ambiente
    char hostPid[16];
    _snprintf_s(hostPid, sizeof(hostPid), _TRUNCATE, "%lu", GetCurrentProcessId());
    SetEnvironmentVariableA("SACREDPAD_COOP_HOST", hostPid);
    const BOOL created = SpawnGame(exe, dir, pi);
    SetEnvironmentVariableA("SACREDPAD_COOP", nullptr);
    SetEnvironmentVariableA("SACREDPAD_COOP_HOST", nullptr);
    if (!created) {
        Log("Coop: CreateProcess fallita (%lu) per %s", GetLastError(), exe.c_str());
        return;
    }
    CloseHandle(pi.hThread);
    g_guestProc = pi.hProcess;
    Log("Coop: seconda istanza avviata (PID %lu) in %s, finestra %s, pad %d", pi.dwProcessId, dir.c_str(), rect, g_cfg.coopGuestPad);
}

// --- Sessione co-op attivabile (0.20.0) ---------------------------------------------------------

HANDLE g_hostProc = nullptr;   // nel supervisore: il processo host avviato
HWND   g_superHwnd = nullptr;

const char* const kSettingsFiles[] = { "\\Settings.cfg", "\\STEAM_Settings.cfg" };

void RestoreSettings();

void BackupSettings()
{
    RememberOrRepairNormalSettings(false);   // se il file e' gia' "da co-op" lo riporta com'era prima di copiarlo
    for (const char* f : kSettingsFiles) {
        const std::string p = std::string(GameDir()) + f;
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA((p + ".coop_orig").c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileA(p.c_str(), (p + ".coop_orig").c_str(), TRUE);
    }
}

void RestoreSettings()
{
    for (const char* f : kSettingsFiles) {
        const std::string p = std::string(GameDir()) + f;
        const std::string b = p + ".coop_orig";
        if (GetFileAttributesA(b.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileA(b.c_str(), p.c_str(), FALSE)) { DeleteFileA(b.c_str()); Log("Coop: %s ripristinato", f + 1); }
        }
    }
    RememberOrRepairNormalSettings(true);
}

// Salvataggi della partita (0.20.14): in multiplayer salva e carica solo chi ospita la partita, e i salvataggi (GAMEnn.PAK)
// stanno nella cartella save dell'istanza che ospitava. Perche' li trovi anche l'altra, si copiano nei due sensi quelli
// mancanti o piu' recenti; gli eroi (heroNN.pax) restano separati, ogni istanza ha i suoi.
void CopyNewerSaves(const std::string& from, const std::string& to)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((from + "\\GAME*.PAK").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string src = from + "\\" + fd.cFileName, dst = to + "\\" + fd.cFileName;
        WIN32_FILE_ATTRIBUTE_DATA d{};
        const bool exists = GetFileAttributesExA(dst.c_str(), GetFileExInfoStandard, &d) != 0;
        if (!exists || CompareFileTime(&fd.ftLastWriteTime, &d.ftLastWriteTime) > 0) {
            if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) Log("Coop: salvataggio %s copiato in %s", fd.cFileName, to.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void SyncGameSaves()
{
    const std::string a = std::string(GameDir()) + "\\save", b = std::string(g_cfg.coopGuestDir) + "\\save";
    if (GetFileAttributesA(a.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesA(b.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    CopyNewerSaves(a, b);
    CopyNewerSaves(b, a);
}

DWORD WINAPI SupervisorThread(LPVOID)
{
    // Sotto Wine g_hostProc e' explorer.exe (resta vivo finche' c'e' il desktop virtuale): si aspetta il Sacred.exe vero.
    if (UseWineDesktop()) {
        HANDLE real = nullptr;
        for (int i = 0; i < 150 && !real; ++i) {                 // fino a 30 s
            Sleep(200);
            for (DWORD pid : SacredPids())
                if (!g_sacredBefore.count(pid))
                    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) { real = h; Log("Coop: host reale = Sacred.exe PID %lu", pid); break; }
        }
        if (real) { CloseHandle(g_hostProc); g_hostProc = real; }
        else Log("Coop: Sacred.exe dell'host non trovato, si aspetta explorer.exe");
    }
    WaitForSingleObject(g_hostProc, INFINITE);
    CloseHandle(g_hostProc);
    g_hostProc = nullptr;
    SyncGameSaves();
    RestoreSettings();
    if (g_cfg.coopReturnNormal && g_superHwnd) {
        ShowWindow(g_superHwnd, SW_SHOW);
        SetForegroundWindow(g_superHwnd);
        Log("Coop: sessione terminata, gioco normale ripristinato");
        return 0;
    }
    // Per sessioni normali non c'e' nulla da salvare (il supervisore era al menu principale): chiude il gioco.
    Log("Coop: sessione terminata, il gioco si chiude (AfterSession=Quit)");
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

void StartSession(HWND hwnd)
{
    if (g_hostProc) return;
    if (g_cfg.coopW <= 0 || g_cfg.coopH <= 0) { Log("Coop: WindowRect non impostato, sessione non avviata"); return; }
    bool mainMenu = false;
    std::vector<MenuItem> items;
    bool charSelect = false;
    if (MemoryMenuItems(items, &charSelect))
        for (const MenuItem& it : items) if (strcmp(it.name, "UI_LABEL_MULTIPL") == 0) mainMenu = true;
    if (!mainMenu) { Log("Coop: la co-op locale si avvia dal menu principale"); return; }

    // L'ospite deve esistere, altrimenti la sessione partirebbe zoppa (0.21.0).
    if (GetFileAttributesA((std::string(g_cfg.coopGuestDir) + "\\Sacred.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("Coop: cartella dell'ospite non trovata (%s): sessione non avviata. Vedi [Coop] GuestDir", g_cfg.coopGuestDir);
        return;
    }
    g_sacredBefore = SacredPids();
    SyncGameSaves();
    BackupSettings();
    for (const char* f : kSettingsFiles) {
        const std::string p = std::string(GameDir()) + f;
        SetSettingsValue(p, "PHD_RES_SCX", g_cfg.coopW);
        SetSettingsValue(p, "PHD_RES_SCY", g_cfg.coopH);
        SetSettingsValue(p, "FULLSCREEN", 0);
    }
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    PROCESS_INFORMATION pi{};
    SetEnvironmentVariableA("SACREDPAD_COOP", "Host");
    const BOOL created = SpawnGame(exe, GameDir(), pi);
    SetEnvironmentVariableA("SACREDPAD_COOP", nullptr);
    if (!created) {
        Log("Coop: CreateProcess fallita (%lu) per %s", GetLastError(), exe);
        RestoreSettings();
        return;
    }
    CloseHandle(pi.hThread);
    g_hostProc = pi.hProcess;
    g_superHwnd = hwnd;
    ShowWindow(hwnd, SW_HIDE);
    Log("Coop: sessione avviata, host PID %lu; questa istanza resta nascosta fino alla fine", pi.dwProcessId);
    if (HANDLE th = CreateThread(nullptr, 0, SupervisorThread, nullptr, 0, nullptr)) CloseHandle(th);
}

// --- Chiusura dell'ospite con l'host e sfondo nero (0.20.5) ---------------------------------------

DWORD WINAPI HostWatchThread(LPVOID p)
{
    HANDLE host = reinterpret_cast<HANDLE>(p);
    WaitForSingleObject(host, INFINITE);
    CloseHandle(host);
    Log("Coop: l'host e' terminato, chiudo la finestra dell'ospite");
    // Prima la chiusura normale; se il gioco non risponde in 20 s il processo termina.
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        char cls[16] = ""; GetClassNameA(h, cls, sizeof(cls));
        if (pid == GetCurrentProcessId() && strcmp(cls, "Sacred") == 0) PostMessageA(h, WM_CLOSE, 0, 0);
        return TRUE;
    }, 0);
    Sleep(20000);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

HWND g_backdrop = nullptr;

LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcA(h, m, w, l);
}

DWORD WINAPI BackdropThread(LPVOID)
{
    WNDCLASSA wc{};
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "SacredPadBackdrop";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    g_backdrop = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "", WS_POPUP, 0, 0,
                                 GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_backdrop) { Log("Coop: sfondo nero non creato (%lu)", GetLastError()); return 0; }
    ShowWindow(g_backdrop, SW_SHOWNOACTIVATE);
    Log("Coop: sfondo nero a tutto schermo creato");
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

void PlaceWindow(HWND hwnd)
{
    if (g_cfg.coopW <= 0 || g_cfg.coopH <= 0) { g_placed = true; return; }
    UINT flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;   // solo spostamento: la dimensione la decide il gioco
    if (g_cfg.coopBorderless) {
        const LONG_PTR old = GetWindowLongA(hwnd, GWL_STYLE);
        LONG_PTR st = old & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
        st |= WS_POPUP;
        if (st != old) { SetWindowLongA(hwnd, GWL_STYLE, static_cast<LONG>(st)); flags |= SWP_FRAMECHANGED; }
    }
    SetWindowPos(hwnd, nullptr, g_cfg.coopX, g_cfg.coopY, 0, 0, flags);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    Log("Coop: finestra portata in (%d,%d); area client attuale %ldx%ld", g_cfg.coopX, g_cfg.coopY, rc.right - rc.left, rc.bottom - rc.top);
    g_placed = true;
}

BOOL CALLBACK ListProc(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof(cls));
    RECT r{};
    GetWindowRect(h, &r);
    Log("Coop: finestra %p classe \"%s\" visibile %d rect (%ld,%ld)-(%ld,%ld) stile %08lX", h, cls, IsWindowVisible(h) ? 1 : 0,
        r.left, r.top, r.right, r.bottom, static_cast<DWORD>(GetWindowLongA(h, GWL_STYLE)));
    return TRUE;
}

} // namespace

void CoopWindowCall(const char* fn, HWND h, int x, int y, int cx, int cy, UINT flags)
{
    static int n = 0;
    if (n >= 40) return;
    char cls[32] = "";
    GetClassNameA(h, cls, sizeof(cls));
    ++n;
    Log("Coop: %s(%p classe \"%s\", x %d, y %d, w %d, h %d, flag %04X)", fn, h, cls, x, y, cx, cy, flags);
}

// Le finestre del gioco (classe "Sacred") nascono e restano dove dice [Coop] WindowRect.
void CoopWindowOverride(HWND h, int& x, int& y)
{
    if (!g_cfg.coopRole || g_cfg.coopW <= 0 || g_cfg.coopH <= 0) return;
    char cls[32] = "";
    GetClassNameA(h, cls, sizeof(cls));
    if (strcmp(cls, "Sacred") != 0) return;
    static int n = 0;
    if (n < 10 && (x != g_cfg.coopX || y != g_cfg.coopY)) { ++n; Log("Coop: posizione richiesta dal gioco (%d,%d) sostituita con (%d,%d)", x, y, g_cfg.coopX, g_cfg.coopY); }
    x = g_cfg.coopX;
    y = g_cfg.coopY;
}

void CoopStartFromMenu(HWND hwnd) { StartSession(hwnd); }
bool CoopSessionActive() { return g_hostProc != nullptr; }

void CoopRecoverSettings()
{
    // Avvio normale: se una sessione precedente e' finita male, Settings.cfg torna com'era.
    if (g_cfg.coopRole == 0) RestoreSettings();
}

void CoopTick(HWND hwnd)
{
    if (!hwnd) return;
    const DWORD now = GetTickCount();
    // 0.55.45: musica spenta nella copia nascosta che ha avviato la sessione e, con GuestMusicVolume 0, nell'ospite
    MusicMute(CoopSessionActive() || (g_cfg.coopRole == 2 && g_cfg.coopGuestMusic == 0));
    {   // 0.55.44: l'istanza in primo piano permette all'altra di prenderselo (minimappa con il Tab fisico, MinimapTakeFocus)
        static DWORD allowAt = 0;
        if (g_cfg.coopRole && (!allowAt || static_cast<LONG>(now - allowAt) >= 0) && GetForegroundWindow() == hwnd) {   // 0.55.46: anche dopo 24,8 giorni
            allowAt = now + 1000;
            if (!allowAt) allowAt = 1;
            AllowSetForegroundWindow(ASFW_ANY);
        }
    }
    // Tasto di avvio: nel gioco normale parte la sessione (solo dal menu principale), nell'host lancia l'ospite.
    if (g_cfg.coopLaunchVk && (g_cfg.coopRole == 1 || (g_cfg.coopRole == 0 && g_cfg.coopEnabled))) {
        const bool down = (GetAsyncKeyState(g_cfg.coopLaunchVk) & 0x8000) != 0 && GetForegroundWindow() == hwnd;
        if (down && !g_keyWasDown) {
            if (g_cfg.coopRole == 1) LaunchGuest();
            else StartSession(hwnd);
        }
        g_keyWasDown = down;
    }
    if (g_cfg.coopRole == 0) return;
    static bool watching = false;
    if (g_cfg.coopRole == 2 && !watching) {          // l'ospite segue l'host: se l'host termina, si chiude
        watching = true;
        char b[16] = "";
        if (GetEnvironmentVariableA("SACREDPAD_COOP_HOST", b, sizeof(b)) > 0)
            if (HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(atoi(b))))
                if (HANDLE th = CreateThread(nullptr, 0, HostWatchThread, hp, 0, nullptr)) CloseHandle(th);
    }
    if (g_cfg.coopRole == 1 && g_cfg.coopBackdrop && g_placed && !UseWineDesktop()) {   // nel desktop virtuale di Wine lo sfondo e' gia' nero   // sfondo nero dietro alla finestra dell'host
        static bool started = false;
        static DWORD zAt = 0;
        if (!started) { started = true; if (HANDLE th = CreateThread(nullptr, 0, BackdropThread, nullptr, 0, nullptr)) CloseHandle(th); }
        if (g_backdrop && now - zAt >= 1000) {
            // 0.20.13: sotto la piu' bassa delle due finestre di Sacred (host e ospite), ma sopra a tutto il resto: in
            // fondo a tutto (0.20.10) spariva dietro a Steam e al resto del desktop.
            zAt = now;
            HWND lowest = hwnd;
            for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
                char cls[16] = "";
                GetClassNameA(w, cls, sizeof(cls));
                if (w == hwnd || (strcmp(cls, "Sacred") == 0 && IsWindowVisible(w))) lowest = w;
            }
            SetWindowPos(g_backdrop, lowest, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    static DWORD lastHook = 0;
    if (g_cfg.coopW > 0 && g_cfg.coopHookWindows && now - lastHook >= 1000) { lastHook = now; HookWindowCalls(); }   // moduli caricati piu' tardi

    // Nei menu pHD disegna su una superficie grande quanto lo schermo (sfondo con l'intro bloccata) e
    // solo in partita la finestra prende la risoluzione di PHD_RES: la si mette a posto solo allora,
    // cioe' quando l'area client ha la dimensione voluta (0.19.2).
    if (g_cfg.coopW <= 0 || g_cfg.coopH <= 0) { g_placed = true; }
    else if (now - g_lastCheck >= 250) {
        g_lastCheck = now;
        RECT cr{}, wr{};
        GetClientRect(hwnd, &cr);
        const bool inGame = (cr.right <= g_cfg.coopW + 64 && cr.bottom <= g_cfg.coopH + 64);   // 0.20.9: il gioco puo' arrotondare la risoluzione
        if (!inGame) {
            if (g_placed) { g_placed = false; g_moves = 0; Log("Coop: area client %ldx%ld (menu): finestra lasciata al gioco", cr.right, cr.bottom); }
        }
        else if (!g_placed) {
            PlaceWindow(hwnd);
            g_placedAt = now;
        }
        else if (GetWindowRect(hwnd, &wr) && (wr.left != g_cfg.coopX || wr.top != g_cfg.coopY)) {
            if (g_moves < 8) Log("Coop: finestra in (%ld,%ld), la riporto in (%d,%d)", wr.left, wr.top, g_cfg.coopX, g_cfg.coopY);
            ++g_moves;
            SetWindowPos(hwnd, nullptr, g_cfg.coopX, g_cfg.coopY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
    }
    if (g_placed && !g_listed && now - g_placedAt >= 6000) {
        g_listed = true;
        EnumWindows(ListProc, 0);
    }

    // L'host lancia da solo l'ospite quando la sua finestra e' a posto (0.20.0).
    static bool autoDone = false;
    static DWORD hostStart = now;
    if (g_cfg.coopRole == 1 && !autoDone && ((g_placed && now - g_placedAt >= 4000) || now - hostStart >= 25000)) {
        autoDone = true;
        if (!g_placed) Log("Coop: finestra dell'host non riconosciuta dopo 25 s, avvio comunque l'ospite");
        LaunchGuest();
    }
}
