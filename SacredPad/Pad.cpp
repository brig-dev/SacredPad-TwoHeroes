// Pad.cpp - lettura del controller
// Linguaggio: C++17, MSVC, Win32 (x86).
//
// Due sorgenti che producono lo stesso PadState:
//  - XInput: pad reale (sotto Steam, Steam Controller e DualSense arrivano cosi');
//  - Tastierino: pad simulato con il tastierino numerico, per provare senza
//    controller. I tasti usati vengono "mangiati" e il gioco non li vede.
//    Tastierino: 8 4 6 2 (7 9 1 3 diagonali) = stick sinistro, 0 = A, punto = B,
//    5 = X, / = Y, * = RB, - = RT, + = LT, Invio = LB.
//    Frecce = stick destro. Canc/PagGiu = croce sinistra/destra, Home/Fine =
//    croce su/giu', Ins = View, PagSu = Menu, F11 = pressione stick destro,
//    F12 = pressione stick sinistro.

#include "SacredPad.h"
#include <cmath>

namespace {

// --- XInput, caricato dinamicamente (nessuna dipendenza in fase di link) -----
struct XGamepad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState   { DWORD dwPacketNumber; XGamepad Gamepad; };
using XInputGetState_t = DWORD (WINAPI*)(DWORD, XState*);
struct XVibration { WORD wLeftMotorSpeed, wRightMotorSpeed; };
using XInputSetState_t = DWORD (WINAPI*)(DWORD, XVibration*);

XInputGetState_t g_xGetState = nullptr;
XInputSetState_t g_xSetState = nullptr;
char             g_xDll[32]  = "nessuna";

void LoadXInput()
{
    const char* names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char* n : names) {
        if (HMODULE h = LoadLibraryA(n)) {
            g_xGetState = reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState"));
            g_xSetState = reinterpret_cast<XInputSetState_t>(GetProcAddress(h, "XInputSetState"));
            if (g_xGetState) {
                strcpy_s(g_xDll, n);
                Log("XInput caricato: %s", n);
                return;
            }
        }
    }
    Log("XInput non disponibile");
}

float Stick(SHORT v) { float f = v / 32767.0f; return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f); }

bool ReadXInput(PadState& out)
{
    if (!g_xGetState) return false;
    XState s{};
    if (g_xGetState(static_cast<DWORD>(g_cfg.controllerIndex), &s) != ERROR_SUCCESS) return false;
    out.connected = true;
    out.lx = Stick(s.Gamepad.sThumbLX);  out.ly = Stick(s.Gamepad.sThumbLY);
    out.rx = Stick(s.Gamepad.sThumbRX);  out.ry = Stick(s.Gamepad.sThumbRY);
    out.lt = s.Gamepad.bLeftTrigger / 255.0f;
    out.rt = s.Gamepad.bRightTrigger / 255.0f;
    out.buttons = s.Gamepad.wButtons;
    return true;
}

// --- Simulatore da tastierino numerico -----------------------------------------
// Si riconoscono i tasti dal codice di scansione (non esteso), quindi funziona
// con BlocNum acceso o spento e non confonde il tastierino con le frecce.
enum KeyBit {
    K7, K8, K9, K4, K6, K1, K2, K3, K0, KDot, KUp, KDown, KLeft, KRight,
    K5, KDiv, KMul, KSub, KAdd, KEnter, KDel, KPgDn, KHome, KEnd, KIns, KPgUp, KF11, KF12,
    KCount
};
std::atomic<unsigned> g_keys{ 0 };
// 0.53.11: con due tasti opposti premuti insieme vale l'ultimo premuto (prima si annullavano: passando dal 4 al 6 lo stick
// restava a zero finche' non si lasciava il 4, da 10 a 93 ms nella prova del 30/10, e l'eroe andava ancora dalla parte vecchia).
// Indici: 0 = x e 1 = y dello stick sinistro, 2 = x e 3 = y del destro; valore -1 o +1.
std::atomic<int> g_lastDir[4] = { 0, 0, 0, 0 };
bool g_kbEnabled = false;
bool g_laptop = false;      // PadSource=Laptop: lettere invece del tastierino

// Variante senza tastierino (portatili): stessi pulsanti, tasti alfabetici
// riconosciuti dal codice di scansione (posizione fisica, vale anche per la
// tastiera italiana). Le frecce restano lo stick destro.
int BitForLaptop(DWORD scan)
{
    switch (scan) {
    case 0x11: return K8;    case 0x1E: return K4;    case 0x1F: return K2;    case 0x20: return K6;   // W A S D
    case 0x24: return K0;    case 0x25: return KDot;                                                    // J = A, K = B
    case 0x16: return K5;    case 0x17: return KDiv;                                                    // U = X, I = Y
    case 0x18: return KMul;  case 0x19: return KSub;  case 0x15: return KAdd;                           // O = RB, P = RT, Y = LT
    case 0x23: return KEnter;                                                                           // H = LB
    case 0x2C: return KDel;  case 0x2D: return KPgDn; case 0x2E: return KHome; case 0x2F: return KEnd;  // Z X C V = croce sx dx su giu'
    case 0x10: return KIns;  case 0x12: return KPgUp;                                                   // Q = View, E = Menu
    case 0x13: return KF11;  case 0x21: return KF12;                                                    // R = R3, F = L3
    default:   return -1;
    }
}

int BitForScan(DWORD scan)
{
    switch (scan) {
    case 0x47: return K7;  case 0x48: return K8;  case 0x49: return K9;
    case 0x4B: return K4;  case 0x4D: return K6;
    case 0x4F: return K1;  case 0x50: return K2;  case 0x51: return K3;
    case 0x52: return K0;  case 0x53: return KDot;
    case 0x4C: return K5;  case 0x37: return KMul;
    case 0x4A: return KSub; case 0x4E: return KAdd;
    case 0x57: return KF11; case 0x58: return KF12;
    default:   return -1;
    }
}

// Frecce: stesso codice di scansione del tastierino, ma con il flag "esteso".
int BitForArrow(DWORD scan)
{
    switch (scan) {
    case 0x48: return KUp;    case 0x50: return KDown;
    case 0x4B: return KLeft;  case 0x4D: return KRight;
    case 0x35: return KDiv;   case 0x1C: return KEnter;      // / e Invio del tastierino
    case 0x53: return KDel;   case 0x51: return KPgDn;
    case 0x47: return KHome;  case 0x4F: return KEnd;
    case 0x52: return KIns;   case 0x49: return KPgUp;
    default:   return -1;
    }
}

// 0.55.1: pad del giocatore 2 da tastiera ([Native] Pad2Keyboard=true, solo con PadSource=Keyboard): le lettere della variante
// da portatile fanno il controller del giocatore 2 (W A S D stick sinistro, J = A, K = B, U = X, I = Y, O = RB, P = RT, Y = LT,
// H = LB, Z/X croce sx/dx, C/V croce su/giu', Q = View, E = Menu), per provare la co-op nativa sul PC senza controller. Con il
// simulatore le frecce sono lo stick destro del pad 1 e non arrivano al gioco, quindi l'eroe 2 non si muove con le frecce
// (prova del 30/10 alle 12:56).
std::atomic<unsigned> g_keys2{ 0 };
std::atomic<int> g_lastDir2[4] = { 0, 0, 0, 0 };   // come g_lastDir (0.55.17: anche 2 e 3, stick destro)
bool g_kb2Enabled = false;

// 0.55.17: stick destro del pad 2 da tastiera (mira): e' sopra, o' a' u' sotto, come le frecce, sulla tastiera italiana
// (codici di scansione 0x1A, 0x27, 0x28, 0x2B: posizione fisica, sulla tastiera americana [ ; ' \).
int BitForPad2Stick(DWORD scan)
{
    switch (scan) {
    case 0x1A: return KUp;    case 0x28: return KDown;
    case 0x27: return KLeft;  case 0x2B: return KRight;
    default:   return -1;
    }
}

// Ultimo lato premuto di ogni asse (dir: g_lastDir o g_lastDir2).
void NoteDir(std::atomic<int>* dir, int bit)
{
    if (bit == K4 || bit == K7 || bit == K1) dir[0].store(-1);
    if (bit == K6 || bit == K9 || bit == K3) dir[0].store(1);
    if (bit == K8 || bit == K7 || bit == K9) dir[1].store(1);
    if (bit == K2 || bit == K1 || bit == K3) dir[1].store(-1);
    if (bit == KLeft)  dir[2].store(-1);
    if (bit == KRight) dir[2].store(1);
    if (bit == KUp)    dir[3].store(1);
    if (bit == KDown)  dir[3].store(-1);
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        int bit = -1, bit2 = -1;
        if (!(k->flags & LLKHF_INJECTED)) {
            if (k->flags & LLKHF_EXTENDED) bit = BitForArrow(k->scanCode);
            else if (g_laptop)             bit = BitForLaptop(k->scanCode);
            else {
                bit = BitForScan(k->scanCode);
                if (bit < 0 && g_kb2Enabled) {                                  // 0.55.1: lettere = pad 2
                    bit2 = BitForLaptop(k->scanCode);
                    if (bit2 < 0) bit2 = BitForPad2Stick(k->scanCode);         // 0.55.17: e' o' a' u' = stick destro
                }
            }
        }
        HWND game = g_gameHwnd.load();
        HWND fg = GetForegroundWindow();
        static bool down[512];
        const unsigned idx = (k->scanCode & 0xff) | ((k->flags & LLKHF_EXTENDED) ? 0x100 : 0);
        const bool repeat = !(k->flags & LLKHF_UP) && down[idx];
        down[idx] = !(k->flags & LLKHF_UP);
        // 0.52.0: solo con il gioco in primo piano. Prima si registravano anche i tasti scritti in altre finestre (chat,
        // password...) mentre il gioco era aperto dietro: il log non deve contenerli (revisione del 29/10).
        if (g_cfg.keyLog && !(k->flags & LLKHF_UP) && !repeat && game && fg == game)
            Log("Tasto: scansione %02lX%s vk %02lX, bit %d%s%s", k->scanCode, (k->flags & LLKHF_EXTENDED) ? " esteso" : "",
                k->vkCode, bit >= 0 ? bit : bit2, bit2 >= 0 ? " (pad 2)" : "", (k->flags & LLKHF_INJECTED) ? ", iniettato" : "");
        const int use = bit >= 0 ? bit : bit2;
        if (use >= 0 && game && fg == game) {
            std::atomic<unsigned>& keys = bit >= 0 ? g_keys : g_keys2;
            if (k->flags & LLKHF_UP) keys.fetch_and(~(1u << use));
            else                     keys.fetch_or(1u << use);
            if (!(k->flags & LLKHF_UP) && !repeat) NoteDir(bit >= 0 ? g_lastDir : g_lastDir2, use);
            return 1;   // il gioco non riceve il tasto
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI KeyboardThread(LPVOID)
{
    // Un hook a basso livello richiede un ciclo di messaggi nel thread che lo installa.
    HHOOK hook = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardProc, SelfModule(), 0);
    if (!hook) { Log("ERRORE: hook tastiera del simulatore non installato (%lu)", GetLastError()); return 0; }
    if (g_laptop)
        Log("Simulatore pad da portatile attivo: W A S D = stick sinistro, frecce = stick destro, J = A, K = B, "
            "U = X, I = Y, O = RB, P = RT, Y = LT, H = LB, Z/X = croce sx/dx, C/V = croce su/giu, "
            "Q = View, E = Menu, R = R3, F = L3");
    else
        Log("Simulatore pad da tastierino attivo: tastierino 8/4/6/2 (+7/9/1/3) = stick sinistro, frecce = stick destro, "
            "0 = A, punto = B, 5 = X, / = Y, * = RB, - = RT, + = LT, Invio = LB, Canc/PagGiu = croce sx/dx, "
            "Home/Fine = croce su/giu, Ins = View, PagSu = Menu, F11 = R3, F12 = L3");
    if (g_kb2Enabled)
        Log("Pad del giocatore 2 da tastiera (Pad2Keyboard): W A S D = stick sinistro, J = A, K = B, U = X, I = Y, O = RB, P = RT, "
            "Y = LT, H = LB, Z/X = croce sx/dx, C/V = croce su/giu, Q = View, E = Menu, R = R3 (Alt), F = L3 (minimappa), "
            "e' o' a' u' = stick destro (mira)");
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    UnhookWindowsHookEx(hook);
    return 0;
}

// Stato del pad dai tasti premuti (bit di KeyBit) e dall'ultimo lato premuto di ogni asse.
void BuildPad(unsigned k, const std::atomic<int>* dir, PadState& out)
{
    auto on = [k](int b) { return (k >> b) & 1u; };
    // un asse: -1, +1, oppure l'ultimo premuto se ci sono tutti e due
    auto axis = [dir](bool neg, bool pos, int i) { return neg && pos ? static_cast<float>(dir[i].load()) : pos ? 1.0f : neg ? -1.0f : 0.0f; };
    float x = axis(on(K4) || on(K7) || on(K1), on(K6) || on(K9) || on(K3), 0);
    float y = axis(on(K2) || on(K1) || on(K3), on(K8) || on(K7) || on(K9), 1);
    float len = std::sqrt(x * x + y * y);
    if (len > 0) { x /= len; y /= len; }

    float rx = axis(on(KLeft), on(KRight), 2);
    float ry = axis(on(KDown), on(KUp), 3);
    float rlen = std::sqrt(rx * rx + ry * ry);
    if (rlen > 0) { rx /= rlen; ry /= rlen; }

    out.connected = true;
    out.lx = x; out.ly = y;
    out.rx = rx; out.ry = ry;
    if (on(K0))    out.buttons |= PAD_A;
    if (on(KDot))  out.buttons |= PAD_B;
    if (on(K5))    out.buttons |= PAD_X;
    if (on(KDiv))  out.buttons |= PAD_Y;
    if (on(KMul))  out.buttons |= PAD_RB;
    if (on(KEnter))out.buttons |= PAD_LB;
    if (on(KDel))  out.buttons |= PAD_DPAD_LEFT;
    if (on(KPgDn)) out.buttons |= PAD_DPAD_RIGHT;
    if (on(KHome)) out.buttons |= PAD_DPAD_UP;
    if (on(KEnd))  out.buttons |= PAD_DPAD_DOWN;
    if (on(KIns))  out.buttons |= PAD_BACK;
    if (on(KPgUp)) out.buttons |= PAD_START;
    if (on(KF11))  out.buttons |= PAD_RS;
    if (on(KF12))  out.buttons |= PAD_LS;
    out.rt = on(KSub) ? 1.0f : 0.0f;
    out.lt = on(KAdd) ? 1.0f : 0.0f;
}

bool ReadKeyboard(PadState& out)
{
    unsigned k = g_keys.load();
    HWND game = g_gameHwnd.load();
    if (!game || GetForegroundWindow() != game) { g_keys.store(0); g_keys2.store(0); k = 0; }   // niente tasti "incastrati"
    BuildPad(k, g_lastDir, out);
    return true;
}

// 0.55.1: pad del giocatore 2 dalle lettere (Pad2Keyboard).
bool ReadKeyboard2(PadState& out)
{
    unsigned k = g_keys2.load();
    HWND game = g_gameHwnd.load();
    if (!game || GetForegroundWindow() != game) { g_keys2.store(0); k = 0; }
    BuildPad(k, g_lastDir2, out);
    return true;
}

const char* g_sourceName = "nessuna";

} // namespace

void PadInit()
{
    const bool sim = (g_cfg.padSource == PadSource::Keyboard || g_cfg.padSource == PadSource::Laptop);
    g_laptop = (g_cfg.padSource == PadSource::Laptop);
    if (!sim) LoadXInput();
    if (g_cfg.padSource != PadSource::XInput) {
        g_kbEnabled = sim;
        g_kb2Enabled = sim && !g_laptop && g_cfg.nativePad2Keyboard;   // 0.55.1: le lettere fanno il pad 2
        if (g_kbEnabled) {
            if (HANDLE th = CreateThread(nullptr, 0, KeyboardThread, nullptr, 0, nullptr)) CloseHandle(th);
        }
    }
}

void PadRead(PadState& out)
{
    out = PadState{};
    const char* name = "nessuna";
    // 0.55.14: PadSource=None, il giocatore 1 usa mouse e tastiera: nessun pad 1 (come un controller scollegato), i controller
    // restano al giocatore 2 (PadRead2)
    if (g_cfg.padSource == PadSource::None) name = "nessuna (PadSource=None, mouse e tastiera)";
    else if (!g_kbEnabled && ReadXInput(out)) name = g_xDll;
    else if (g_kbEnabled && ReadKeyboard(out))                    name = "tastierino";

    if (name != g_sourceName) {
        Log("Sorgente del pad: %s", name);
        g_sourceName = name;
    }
}

const char* PadSourceName() { return g_sourceName; }

// Pad del giocatore 2 della co-op nativa (0.44.0): un altro controller XInput. [Native] Pad2Index = -1 sceglie il primo
// collegato diverso da quello del giocatore 1 (con il simulatore da tastiera per il giocatore 1, o con PadSource=None, anche
// il controller 0: cosi' sul PC si prova con un solo pad). Gli slot vuoti si ricontrollano al massimo ogni 2 s (XInputGetState su uno slot
// scollegato e' lento).
bool PadRead2(PadState& out, int& index)
{
    static int cur = -1;
    static DWORD nextScan = 0;
    static bool loaded = false;
    out = PadState{};
    if (g_kb2Enabled) { index = kPad2KeyboardIndex; return ReadKeyboard2(out); }   // 0.55.1: lettere (Pad2Keyboard)
    if (!g_xGetState && !loaded) { loaded = true; LoadXInput(); }
    if (!g_xGetState) return false;
    auto read = [&](int k) {
        XState s{};
        if (g_xGetState(static_cast<DWORD>(k), &s) != ERROR_SUCCESS) return false;
        out.connected = true;
        out.lx = Stick(s.Gamepad.sThumbLX);  out.ly = Stick(s.Gamepad.sThumbLY);
        out.rx = Stick(s.Gamepad.sThumbRX);  out.ry = Stick(s.Gamepad.sThumbRY);
        out.lt = s.Gamepad.bLeftTrigger / 255.0f;
        out.rt = s.Gamepad.bRightTrigger / 255.0f;
        out.buttons = s.Gamepad.wButtons;
        return true;
    };
    if (cur >= 0) {
        if (read(cur)) { index = cur; return true; }
        cur = -1; nextScan = GetTickCount() + 2000;
        return false;
    }
    if (static_cast<int>(GetTickCount() - nextScan) < 0) return false;
    nextScan = GetTickCount() + 2000;
    for (int k = 0; k < 4; ++k) {
        if (g_cfg.nativePad2Index >= 0 && k != g_cfg.nativePad2Index) continue;
        if (g_cfg.nativePad2Index < 0 && !g_kbEnabled && g_cfg.padSource != PadSource::None && k == g_cfg.controllerIndex) continue;   // e' il pad del giocatore 1
        if (read(k)) { cur = k; index = k; return true; }
    }
    return false;
}

// Vibrazione (0.17.0): XInputSetState sul controller in uso. Sotto Steam Input la richiesta arriva
// anche allo Steam Controller e al DualSense. Si chiama il driver solo quando i valori cambiano.
void PadVibrate(float left, float right)
{
    static WORD lastL = 0, lastR = 0;
    if (!g_xSetState || g_kbEnabled || g_cfg.padSource == PadSource::None) return;   // con None il controller e' del giocatore 2
    auto speed = [](float v) { v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); return static_cast<WORD>(v * 65535.0f); };
    XVibration vib{ speed(left), speed(right) };
    if (vib.wLeftMotorSpeed == lastL && vib.wRightMotorSpeed == lastR) return;
    lastL = vib.wLeftMotorSpeed; lastR = vib.wRightMotorSpeed;
    g_xSetState(static_cast<DWORD>(g_cfg.controllerIndex), &vib);
}

// 0.55.29: vibrazione del pad 2 (slot XInput di PadRead2), come quella del pad 1 ma sul suo controller; con il pad 2 da
// tastiera (Pad2Keyboard) niente. Si chiama il driver solo quando i valori o il controller cambiano.
void PadVibrate2(int index, float left, float right)
{
    static int lastIdx = -1;
    static WORD lastL = 0, lastR = 0;
    if (!g_xSetState || index < 0 || index > 3) return;
    auto speed = [](float v) { v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); return static_cast<WORD>(v * 65535.0f); };
    XVibration vib{ speed(left), speed(right) };
    if (index == lastIdx && vib.wLeftMotorSpeed == lastL && vib.wRightMotorSpeed == lastR) return;
    if (lastIdx >= 0 && lastIdx != index && (lastL || lastR)) { XVibration off{ 0, 0 }; g_xSetState(static_cast<DWORD>(lastIdx), &off); }
    lastIdx = index; lastL = vib.wLeftMotorSpeed; lastR = vib.wRightMotorSpeed;
    g_xSetState(static_cast<DWORD>(index), &vib);
}
