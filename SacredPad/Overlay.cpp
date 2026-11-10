// Overlay.cpp - scritte della mod sopra il gioco, disegnate con il font di Sacred (fase 3)
// Linguaggio: C++17. Compilatore: MSVC, Win32 (x86).
//
// Come disegna il gioco (Sacred.exe 2.28, analisi con Ghidra del 23/10/2026):
//   0x60e640  cEngine::renderThread::run: a ogni fotogramma mondo, interfaccia, scritte
//             di debug (0x60f630), poi 0x647200 = dxDriver7::EndScene, poi il flip.
//   [0xcdb99c] dxDriver7; [+0xcc] IDirect3DDevice7 (EndScene = metodo 0x18 del vtable COM).
//   [0x182ddac] cFont usato dalle scritte di debug; metodo 0x1c = disegna testo
//             (thiscall: device, rettangolo {int x, int y, short w, short h}, testo,
//             colore ARGB, flag: 1 = centra nel rettangolo).
// La mod non modifica il codice di Sacred.exe: sostituisce il puntatore a EndScene nel
// vtable del device (memoria di dgVoodoo) e, prima dell'EndScene vero, scrive nel thread
// di rendering del gioco con il suo stesso font.
#include "SacredPad.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr DWORD kDriver   = 0x00cdb99c;   // dxDriver7
constexpr DWORD kDbgFont  = 0x0182ddac;   // cFont delle scritte di debug
constexpr DWORD kVtFsFont = 0x0089119c, kVtFontTTF = 0x0089121c, kVtFontTTF2 = 0x00891268;

using EndSceneFn = HRESULT(__stdcall*)(void* dev);
// thiscall del gioco chiamato come fastcall: ecx = this, edx inutilizzato, il resto sullo stack
using FontDrawFn = int(__fastcall*)(void* self, void* edx, void* dev, void* rect, const char* text,
                                    DWORD color, DWORD flags);

struct TextRect { int x, y; short w, h; };

EndSceneFn        g_origEndScene = nullptr;
void**            g_slot = nullptr;           // voce del vtable sostituita
std::atomic<bool> g_cursor{ false };          // modalita' cursore della mod, per la scritta
std::atomic<bool> g_cursorP2{ false };        // 0.50.0: il puntatore e' del giocatore 2
std::atomic<bool> g_markOn{ false };          // indicatore sopra il bersaglio a fuoco
std::atomic<LONG> g_markX{ 0 }, g_markY{ 0 };
std::atomic<int>  g_markIcon{ 0 };
std::atomic<bool> g_mark2On{ false };         // 0.55.2: cosa userebbe l'eroe 2 con A del pad 2 (icona con un "2" accanto)
std::atomic<bool> g_c2On{ false };            // 0.56.1: numero accanto al cursore del giocatore 2 nel mondo (lo disegna il gioco)
std::atomic<LONG> g_c2X{ 0 }, g_c2Y{ 0 };
std::atomic<DWORD> g_c2At{ 0 };
std::atomic<LONG> g_mark2X{ 0 }, g_mark2Y{ 0 };
std::atomic<int>  g_mark2Icon{ 0 };
std::atomic<bool> g_twoPlayers{ false };      // 0.55.4: c'e' l'eroe 2: anche l'icona del pad 1 ha il suo numero ("1")

// Simboli dei tasti dello Steam Controller sopra i cerchi degli slot delle arti (0.14.2).
std::atomic<int>  g_slotCount{ 0 };
std::atomic<LONG> g_slotX[5] = {}, g_slotY[5] = {};
// 0.55.27: gli stessi per il pad 2 sulla barra dell'eroe 2 (dal suo disegno, TaskbarDrawCopy), validi per 300 ms
std::atomic<int>  g_slot2Count{ 0 };
std::atomic<LONG> g_slot2X[5] = {}, g_slot2Y[5] = {};
std::atomic<DWORD> g_slot2At{ 0 };
// 0.56.23: centri dei cerchi delle pozioni con un tasto (cura, croce giu', croce su) delle due barre
std::atomic<int>  g_potCount[3] = {};
std::atomic<LONG> g_potX[3][3] = {}, g_potY[3][3] = {};
std::atomic<DWORD> g_potAt[3] = {};
// Per controller (0.15.0): 0 Steam Controller, 1 Xbox, 2 DualSense (Quadrato, Triangolo, R1, R2, L2;
// niente barra rovesciata, che per il font del gioco introduce un codice di colore).
const char* const kSlotLabels[3][5] = { { "X", "Y", "RB", "RT", "LT" }, { "X", "Y", "RB", "RT", "LT" },
                                        { "Qua", "Tri", "R1", "R2", "L2" } };
std::atomic<bool> g_helpOn{ false };
std::atomic<int>  g_helpOwner{ 1 };              // 0.51.3: giocatore che ha aperto l'aiuto (1 o 2)
std::atomic<int>  g_helpRow{ 0 };                // riga scelta nell'aiuto: 0 controller, 1 vibrazione, 2 controller del giocatore 2            // 0 mouse_action, 1 MOUSE_DIALOG04, 2 MOUSE_RIDE
std::atomic<bool> g_gridOn[4] = {};           // "+" al centro delle caselle delle griglie aperte (verifica)
std::atomic<LONG> g_gridX[4] = {}, g_gridY[4] = {}, g_gridC[4] = {}, g_gridR[4] = {};
std::atomic<LONG> g_draws{ 0 };               // fotogrammi disegnati (per il log)
std::atomic<LONG> g_faults{ 0 };              // eccezioni durante il disegno
DWORD             g_lastLog = 0;

template <class T>
bool Rd(DWORD addr, T& out)
{
    SIZE_T n = 0;
    return addr >= 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)),
                             &out, sizeof(T), &n) && n == sizeof(T);
}

// Font delle scritte di debug, se e' una delle classi di font conosciute.
void* GameFont()
{
    DWORD font = 0, vt = 0;
    if (!Rd(kDbgFont, font) || !font || !Rd(font, vt)) return nullptr;
    if (vt != kVtFsFont && vt != kVtFontTTF && vt != kVtFontTTF2) return nullptr;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(font));
}

void GameDrawText(void* font, void* dev, int x, int y, int w, const char* text, DWORD color, DWORD flags)
{
    auto draw = reinterpret_cast<FontDrawFn>((*reinterpret_cast<void***>(font))[0x1c / 4]);
    TextRect r{ x, y, static_cast<short>(w), 20 };
    draw(font, nullptr, dev, &r, text, color, flags);
}

// 0.55.4: numero del giocatore accanto all'icona del bersaglio, un po' piu' vicino all'icona che nella 0.55.2 (richiesta di
// il tester del 30/10): "1" oro per il pad 1, "2" azzurro per il pad 2.
// 0.56.20 (il tester, 04/11: "riesci anche a migliorare graficamente quella scritta 1 e 2?"): il numero era una cifra del font con
// l'ombra, piccola e poco leggibile sui fondi chiari. Ora e' un bollino tondo come quelli della schermata dei comandi: fondo
// scuro, bordo nel colore del giocatore, cifra al centro; stesso posto di prima (accanto all'icona o al cursore).
void Disc(float cx, float cy, float rx, float ry, DWORD cIn, DWORD cOut, int seg);   // piu' sotto
void Ring(float cx, float cy, float r0, float r1, DWORD c, int seg);
// 0.56.21 (foto della 0.56.20: "miglioralo ti prego, non deve dare fastidio"): il bollino con l'anello pieno sembrava un orologio
// e la cifra cadeva in basso a sinistra (nel font del gioco la cifra centrata sta 15 pixel sotto il bordo alto del rettangolo, non
// 9). Ora niente anello: un'ombra scura tonda che sfuma verso fuori e la cifra nel colore del giocatore, centrata.
void DrawMarkNumber(void* font, void* dev, int mx, int my, const char* num, DWORD color)
{
    const float cx = mx + 15.0f, cy = my + 3.0f;
    Disc(cx, cy, 11.0f, 11.0f, 0xA0000000u, 0x00000000u, 24);
    GameDrawText(font, dev, mx + 6, my - 11, 20, num, 0xC0000000u, 1);                // ombra della cifra
    GameDrawText(font, dev, mx + 5, my - 12, 20, num, color, 1);
}

// --- Icone del gioco (0.14.0, Ghidra) ----------------------------------------------------------
// Texture: gestore [0x13e67b4] (Pak/Texture.pak, 0x656eb0), numero dal nome con 0x657850
// (thiscall, 1 argomento); texture vera dalla cache [0x13e67b8] con 0x65f220(numero, 0) (2 argomenti),
// superficie Direct3D in +0x14. Disegno come il cursore e il font: cache degli stati [0xcd69fc]
// con 0x643790(stato, acceso), SetTexture del device (metodo 0x8c), rettangolo del driver
// [0xcdb99c] con 0x6468e0(x0, y0, x1, y1, colore, togli texture, u0, v0, u1, v1) (10 argomenti).
using TexHandleFn = DWORD(__fastcall*)(void* mgr, void* edx, const char* name);
using TexGetFn    = void*(__fastcall*)(void* cache, void* edx, DWORD handle, DWORD zero);
using StateFn     = DWORD(__fastcall*)(void* cache, void* edx, DWORD flag, DWORD on);
using QuadFn      = void(__fastcall*)(void* drv, void* edx, int x0, int y0, int x1, int y1, DWORD color, DWORD noTex,
                                      float u0, float v0, float u1, float v1);
using SetTexFn    = HRESULT(__stdcall*)(void* dev, DWORD stage, void* tex);

// 0.55.54: anche il cursore principale (MOUSE_MAIN.TGA), per il puntatore del giocatore che non ha il mouse del gioco.
const char* const kIconNames[4] = { "mouse_action.tga", "MOUSE_DIALOG04.TGA", "MOUSE_RIDE.TGA", "MOUSE_MAIN.TGA" };
DWORD g_iconHandle[4] = {};
bool  g_iconTried[4] = {};

// 0.55.51: ritratti delle classi per il menu Due giocatori. Il gioco li prende da FUN_00435d30 (classe -> numero dell'immagine):
// 1 Serafina 0x4e7, 2 Gladiatore 0x4e9, 3 Mago 0x4e8, 4 Elfo oscuro 0x4ec, 5 Elfa 0x4ed, 6 Vampira 0x4eb, 8 Nano 0x59e,
// 9 Diavola 0x59d; nella tabella delle immagini (0xa05a7c, voci da 0x54: nome, rettangolo in pixel, numero + 1) sono le celle
// 0, 2, 1, 5, 6, 4 di GUI_PORTRAIT00.TGA e 3, 2 di GUI_PORTRAIT15.TGA (256x256, celle 64x71, tre righe di quattro).
const char* const kPortraitTex[2] = { "GUI_PORTRAIT00.TGA", "GUI_PORTRAIT15.TGA" };
DWORD g_portraitHandle[2] = {};
bool  g_portraitTried[2] = {};
bool DrawPortrait(void* dev, char code, int x0, int y0, int x1, int y1, DWORD color)
{
    int sheet = 0, cell = -1;
    switch (code) {
    case '1': cell = 0; break; case '2': cell = 2; break; case '3': cell = 1; break; case '4': cell = 5; break;
    case '5': cell = 6; break; case '6': cell = 4; break; case '8': sheet = 1; cell = 3; break; case '9': sheet = 1; cell = 2; break;
    default: return false;
    }
    DWORD mgr = 0, cache = 0, states = 0, drv = 0;
    if (!Rd(0x013e67b4, mgr) || !mgr || !Rd(0x013e67b8, cache) || !cache || !Rd(kDriver, drv) || !drv) return false;
    if (!g_portraitTried[sheet]) {
        g_portraitTried[sheet] = true;
        g_portraitHandle[sheet] = reinterpret_cast<TexHandleFn>(0x00657850)(reinterpret_cast<void*>(mgr), nullptr, kPortraitTex[sheet]);
        Log("Overlay: texture %s -> numero %lu", kPortraitTex[sheet], g_portraitHandle[sheet]);
    }
    if (!g_portraitHandle[sheet]) return false;
    void* tex = reinterpret_cast<TexGetFn>(0x0065f220)(reinterpret_cast<void*>(cache), nullptr, g_portraitHandle[sheet], 0);
    if (!tex) return false;
    void* surface = *reinterpret_cast<void**>(static_cast<char*>(tex) + 0x14);
    if (!surface || !Rd(0x00cd69fc, states) || !states) return false;
    auto state = reinterpret_cast<StateFn>(0x00643790);
    void* sc = reinterpret_cast<void*>(states);
    state(sc, nullptr, 0x20, 1); state(sc, nullptr, 1, 0); state(sc, nullptr, 0x800, 0);
    state(sc, nullptr, 4, 1);    state(sc, nullptr, 8, 0); state(sc, nullptr, 0x10, 0);
    auto setTex = reinterpret_cast<SetTexFn>((*reinterpret_cast<void***>(dev))[0x8c / 4]);
    setTex(dev, 0, surface);
    const int col = cell % 4, row = cell / 4;
    const float u0 = (col * 64 + 0.5f) / 256.0f, v0 = (row * 71 + 0.5f) / 256.0f, u1 = (col * 64 + 63.5f) / 256.0f, v1 = (row * 71 + 69.5f) / 256.0f;
    reinterpret_cast<QuadFn>(0x006468e0)(reinterpret_cast<void*>(drv), nullptr, x0, y0, x1, y1, color, 0, u0, v0, u1, v1);
    return true;
}

void DrawIconRaw(void* dev, int icon, int x0, int y0, int x1, int y1, DWORD color = 0xFFFFFFFFu, float u1 = 1.0f, float v1 = 1.0f)
{
    DWORD mgr = 0, cache = 0, states = 0, drv = 0;
    if (icon < 0 || icon > 3) return;
    if (!Rd(0x013e67b4, mgr) || !mgr || !Rd(0x013e67b8, cache) || !cache || !Rd(kDriver, drv) || !drv) return;
    if (!g_iconTried[icon]) {
        g_iconTried[icon] = true;
        g_iconHandle[icon] = reinterpret_cast<TexHandleFn>(0x00657850)(reinterpret_cast<void*>(mgr), nullptr, kIconNames[icon]);
        Log("Overlay: texture %s -> numero %lu", kIconNames[icon], g_iconHandle[icon]);
    }
    if (!g_iconHandle[icon]) return;
    void* tex = reinterpret_cast<TexGetFn>(0x0065f220)(reinterpret_cast<void*>(cache), nullptr, g_iconHandle[icon], 0);
    if (!tex) return;
    void* surface = *reinterpret_cast<void**>(static_cast<char*>(tex) + 0x14);
    if (!Rd(0x00cd69fc, states) || !states) return;
    auto state = reinterpret_cast<StateFn>(0x00643790);
    void* sc = reinterpret_cast<void*>(states);
    state(sc, nullptr, 0x20, 1); state(sc, nullptr, 1, 0); state(sc, nullptr, 0x800, 0);
    state(sc, nullptr, 4, 1);    state(sc, nullptr, 8, 0); state(sc, nullptr, 0x10, 0);
    auto setTex = reinterpret_cast<SetTexFn>((*reinterpret_cast<void***>(dev))[0x8c / 4]);
    setTex(dev, 0, surface);
    reinterpret_cast<QuadFn>(0x006468e0)(reinterpret_cast<void*>(drv), nullptr, x0, y0, x1, y1, color, 0,
                                          0.0f, 0.0f, u1, v1);
}

// 0.55.54: due puntatori con i due zaini (Core.cpp, Pad2Pointer): quello di chi non ha il mouse del gioco si disegna qui, con il
// cursore del gioco colorato; accanto a tutti e due il numero del giocatore.
std::atomic<bool> g_ptrOn{ false }, g_ptr1Show{ false };
std::atomic<LONG> g_ptr1X{ 0 }, g_ptr1Y{ 0 }, g_ptr2X{ 0 }, g_ptr2Y{ 0 }, g_ptrOwner{ 1 };

// Rettangolo pieno (senza texture) con il driver del gioco, stessi stati di disegno del font.
void FillRect(int x0, int y0, int x1, int y1, DWORD color)
{
    DWORD states = 0, drv = 0;
    if (!Rd(0x00cd69fc, states) || !states || !Rd(kDriver, drv) || !drv) return;
    auto state = reinterpret_cast<StateFn>(0x00643790);
    void* sc = reinterpret_cast<void*>(states);
    state(sc, nullptr, 0x20, 1); state(sc, nullptr, 1, 0); state(sc, nullptr, 0x800, 0);
    state(sc, nullptr, 4, 1);    state(sc, nullptr, 8, 0); state(sc, nullptr, 0x10, 0);
    reinterpret_cast<QuadFn>(0x006468e0)(reinterpret_cast<void*>(drv), nullptr, x0, y0, x1, y1, color, 1,
                                          0.0f, 0.0f, 1.0f, 1.0f);
}

// --- Primitive (0.15.1): forme senza texture con DrawPrimitive del device (metodo 0x64), vertici
// gia' trasformati con il formato del gioco 0x1c4 (x, y, z, rhw, colore, speculare, u, v), come fanno
// il font e l'interfaccia di Sacred. Tipi: 4 lista di triangoli, 5 striscia, 6 ventaglio.
struct Vtx { float x, y, z, rhw; DWORD c, spec; float u, v; };
using DrawPrimFn = HRESULT(__stdcall*)(void* dev, DWORD type, DWORD fvf, void* verts, DWORD count, DWORD flags);
void* g_dev = nullptr;                                  // device del fotogramma in corso (thread di rendering)

inline Vtx V(float x, float y, DWORD c) { return Vtx{ x, y, 0.0f, 1.0f, c, 0, 0.0f, 0.0f }; }

void Prims(DWORD type, Vtx* v, int n)
{
    DWORD states = 0;
    if (!g_dev || n < 3 || !Rd(0x00cd69fc, states) || !states) return;
    auto state = reinterpret_cast<StateFn>(0x00643790);
    void* sc = reinterpret_cast<void*>(states);
    state(sc, nullptr, 0x20, 1); state(sc, nullptr, 1, 0); state(sc, nullptr, 0x800, 0);
    state(sc, nullptr, 4, 1);    state(sc, nullptr, 8, 0); state(sc, nullptr, 0x10, 0);
    void** vt = *reinterpret_cast<void***>(g_dev);
    reinterpret_cast<SetTexFn>(vt[0x8c / 4])(g_dev, 0, nullptr);
    reinterpret_cast<DrawPrimFn>(vt[0x64 / 4])(g_dev, type, 0x1c4, v, static_cast<DWORD>(n), 0);
}

// Disco pieno, con colore diverso al centro e al bordo (sfumatura).
void Disc(float cx, float cy, float rx, float ry, DWORD cIn, DWORD cOut, int seg = 28)
{
    Vtx v[64];
    v[0] = V(cx, cy, cIn);
    for (int i = 0; i <= seg; ++i) {
        const float a = i * 6.2831853f / seg;
        v[i + 1] = V(cx + rx * std::cos(a), cy + ry * std::sin(a), cOut);
    }
    Prims(6, v, seg + 2);
}

// Ellisse inclinata (impugnature).
void TiltDisc(float cx, float cy, float rx, float ry, float ang, DWORD cIn, DWORD cOut)
{
    Vtx v[40];
    const int seg = 32;
    const float ca = std::cos(ang), sa = std::sin(ang);
    v[0] = V(cx, cy, cIn);
    for (int i = 0; i <= seg; ++i) {
        const float a = i * 6.2831853f / seg, x = rx * std::cos(a), y = ry * std::sin(a);
        v[i + 1] = V(cx + x * ca - y * sa, cy + x * sa + y * ca, cOut);
    }
    Prims(6, v, seg + 2);
}

void Ring(float cx, float cy, float r0, float r1, DWORD c, int seg = 28)
{
    Vtx v[66];
    for (int i = 0; i <= seg; ++i) {
        const float a = i * 6.2831853f / seg, ca = std::cos(a), sa = std::sin(a);
        v[2 * i] = V(cx + r1 * ca, cy + r1 * sa, c);
        v[2 * i + 1] = V(cx + r0 * ca, cy + r0 * sa, c);
    }
    Prims(5, v, 2 * seg + 2);
}

void Line(float x0, float y0, float x1, float y1, float w, DWORD c)
{
    const float dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy);
    if (l < 0.5f) return;
    const float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    Vtx v[4] = { V(x0 + nx, y0 + ny, c), V(x0 - nx, y0 - ny, c), V(x1 + nx, y1 + ny, c), V(x1 - nx, y1 - ny, c) };
    Prims(5, v, 4);
}

// Rettangolo arrotondato pieno, sfumato dall'alto (cTop) al basso (cBot).
void RoundRect(float x0, float y0, float x1, float y1, float r, DWORD cTop, DWORD cBot)
{
    Vtx v[48];
    int n = 0;
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    v[n++] = V(cx, cy, cTop);
    const float corners[4][2] = { { x1 - r, y0 + r }, { x1 - r, y1 - r }, { x0 + r, y1 - r }, { x0 + r, y0 + r } };
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i <= 8; ++i) {
            const float a = (-90.0f + k * 90.0f + i * 11.25f) * 0.0174533f;
            const float px = corners[k][0] + r * std::cos(a), py = corners[k][1] + r * std::sin(a);
            v[n++] = V(px, py, py < cy ? cTop : cBot);
        }
    v[n] = v[1];
    ++n;
    Prims(6, v, n);
}

// Simboli PlayStation come contorni spessi (s = mezzo lato), in un colore solo (0.15.2: oro, come le
// lettere degli altri controller).
void PsSymbol(int sym, float cx, float cy, float s, float w, DWORD c = 0xFFFFE08Cu)
{
    if (sym == 0) {                                     // quadrato
        Line(cx - s, cy - s, cx + s, cy - s, w, c); Line(cx + s, cy - s, cx + s, cy + s, w, c);
        Line(cx + s, cy + s, cx - s, cy + s, w, c); Line(cx - s, cy + s, cx - s, cy - s, w, c);
    }
    else if (sym == 1) {                                // triangolo
        const float t = cy - s * 1.05f, bb = cy + s * 0.8f;
        Line(cx, t, cx + s, bb, w, c); Line(cx + s, bb, cx - s, bb, w, c); Line(cx - s, bb, cx, t, w, c);
    }
    else if (sym == 2) Ring(cx, cy, s - w * 0.5f, s + w * 0.5f, c);     // cerchio
    else {                                              // croce
        Line(cx - s, cy - s, cx + s, cy + s, w, c); Line(cx + s, cy - s, cx - s, cy + s, w, c);
    }
}

void DrawSlotLabels(void* dev, void* font, int count, const std::atomic<LONG>* xs, const std::atomic<LONG>* ys, int layout);   // piu' sotto

// 0.56.16: indicatori dei giocatori (il tester, 04/11: riconoscere chi e' chi a colpo d'occhio, da accendere e spegnere nella schermata
// Controller come la vibrazione). Anello ai piedi di ogni eroe nel colore del suo numero (oro il giocatore 1, azzurro il 2), con
// un bordo scuro; se un eroe e' fuori dallo schermo una freccia al bordo verso di lui, con il numero. Solo con due giocatori,
// nessuna finestra del gioco aperta e senza la schermata dei comandi.
void EllipseRing(float cx, float cy, float rx, float ry, float w, DWORD c)
{
    constexpr int kSeg = 32;
    float px = cx + rx, py = cy;
    for (int i = 1; i <= kSeg; ++i) {
        const float a = i * 6.2831853f / kSeg;
        const float nx = cx + rx * std::cos(a), ny = cy + ry * std::sin(a);
        Line(px, py, nx, ny, w, c);
        px = nx; py = ny;
    }
}
// 0.56.17 (il tester, foto del 04/11: "e' parecchio brutto quel cerchio"): l'anello era una linea netta di 2,5 pixel attorno ai piedi,
// disegnata sopra l'eroe come un cerchio appoggiato. Ora e' un alone a terra: fascia ellittica che sfuma verso il centro e verso
// l'esterno, piu' piccola (52x20), senza contorno, 3 pixel piu' su (la proiezione cade di poco davanti ai piedi).
void EllipseBand(float cx, float cy, float rx0, float ry0, float rx1, float ry1, DWORD c0, DWORD c1)
{
    constexpr int kSeg = 32;
    Vtx v[2 * kSeg + 2];
    for (int i = 0; i <= kSeg; ++i) {
        const float a = i * 6.2831853f / kSeg, ca = std::cos(a), sa = std::sin(a);
        v[2 * i] = V(cx + rx1 * ca, cy + ry1 * sa, c1);
        v[2 * i + 1] = V(cx + rx0 * ca, cy + ry0 * sa, c0);
    }
    Prims(5, v, 2 * kSeg + 2);
}
// 0.56.18: variante C scelta dal tester ("prova c, se va in direzione di dove guarda il personaggio"): con una direzione (dx, dy
// sullo schermo) l'alone resta solo dal lato in cui l'eroe guarda e sfuma verso i lati; senza (bersagli) e' intero. k: intensita'.
static DWORD GlowA(DWORD rgb, float a) { const int v = a < 0 ? 0 : a > 255 ? 255 : static_cast<int>(a); return rgb | (static_cast<DWORD>(v) << 24); }
void GlowBand(float cx, float cy, float rx0, float ry0, float rx1, float ry1, DWORD rgb, float a0, float a1, bool dir, float phi)
{
    constexpr int kSeg = 32;
    Vtx v[2 * kSeg + 2];
    for (int i = 0; i <= kSeg; ++i) {
        const float a = i * 6.2831853f / kSeg, ca = std::cos(a), sa = std::sin(a);
        float w = 1.0f;
        if (dir) { w = (std::cos(a - phi) + 0.1f) / 0.55f; w = w < 0 ? 0 : w > 1 ? 1 : w; }
        v[2 * i] = V(cx + rx1 * ca, cy + ry1 * sa, GlowA(rgb, a1 * w));
        v[2 * i + 1] = V(cx + rx0 * ca, cy + ry0 * sa, GlowA(rgb, a0 * w));
    }
    Prims(5, v, 2 * kSeg + 2);
}
void GroundGlow(float cx, float cy, DWORD rgb, float rx = 26.0f, float ry = 10.0f, bool dir = false, float ddx = 0, float ddy = 1, float k = 1.0f)
{
    const float phi = std::atan2(ddy / ry, ddx / rx);   // direzione nello spazio dell'ellisse
    GlowBand(cx, cy, 0, 0, rx * 0.55f, ry * 0.55f, rgb, 0x10 * k, 0x30 * k, dir, phi);
    GlowBand(cx, cy, rx * 0.55f, ry * 0.55f, rx, ry, rgb, 0x30 * k, 0x78 * k, dir, phi);
    GlowBand(cx, cy, rx, ry, rx * 1.45f, ry * 1.45f, rgb, 0x78 * k, 0, dir, phi);
}
// 0.56.18: alone a terra sotto il bersaglio di A dei due giocatori, che pulsa piano (il tester: "mostrarlo e migliorarlo
// graficamente per far capire meglio su cosa stai per cliccare"). Punto a terra da Core.cpp (pad 1) e da NativePad2Tick (pad 2).
std::atomic<bool> g_gndOn[3] = {};
std::atomic<LONG> g_gndX[3] = {}, g_gndY[3] = {};
void DrawTargetGlows()
{
    const float pulse = 0.75f + 0.25f * std::sin(GetTickCount() * 0.006f);
    for (int who = 1; who <= 2; ++who) {
        if (!g_gndOn[who].load()) continue;
        const DWORD rgb = who == 1 ? 0x00FFE08Cu : 0x008CD8FFu;
        // 0.56.21: piu' tenue ("farei anche piu' tenue il cerchio di quando stai per selezionare qualcosa"): intensita' da 1,25 a 0,7
        GroundGlow(static_cast<float>(g_gndX[who].load()), static_cast<float>(g_gndY[who].load()) - 2.0f, rgb, 28.0f, 11.0f, false, 0, 1, 0.7f * pulse);
    }
}
void DrawMarkNumber(void* font, void* dev, int mx, int my, const char* num, DWORD color);   // piu' sotto
void DrawPlayerMarks(void* font, void* dev)
{
    POINT p[2] = {}; bool on[2] = {}; int w = 0, h = 0; float dir[4] = { 0, 1, 0, 1 };
    if (!MemoryHeroesScreen(p, on, w, h, dir) || w <= 0 || h <= 0) return;
    static const DWORD col[2] = { 0xD8FFC850u, 0xD88CD8FFu };
    static const char* const num[2] = { "1", "2" };
    for (int i = 0; i < 2; ++i) {
        if (!on[i]) continue;
        const float x = static_cast<float>(p[i].x), y = static_cast<float>(p[i].y);
        constexpr float kM = 24.0f;                     // margine dal bordo
        if (x >= kM && x < w - kM && y >= kM && y < h - kM) {
            GroundGlow(x, y - 3.0f, col[i] & 0x00FFFFFFu, 26.0f, 10.0f, true, dir[i * 2], dir[i * 2 + 1], 1.2f);
            continue;
        }
        // fuori dallo schermo: freccia al bordo, verso l'eroe
        const float cx = w * 0.5f, cy = h * 0.5f, dx = x - cx, dy = y - cy;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) continue;
        const float ux = dx / len, uy = dy / len;
        float t = 1e9f;
        if (ux > 1e-4f) t = (std::min)(t, (w - kM - cx) / ux);
        if (ux < -1e-4f) t = (std::min)(t, (kM - cx) / ux);
        if (uy > 1e-4f) t = (std::min)(t, (h - kM - cy) / uy);
        if (uy < -1e-4f) t = (std::min)(t, (kM - cy) / uy);
        const float ex = cx + ux * t, ey = cy + uy * t;
        const float bx = ex - ux * 22.0f, by = ey - uy * 22.0f, nx = -uy * 11.0f, ny = ux * 11.0f;
        Line(ex, ey, bx + nx, by + ny, 5.0f, 0x90000000u); Line(ex, ey, bx - nx, by - ny, 5.0f, 0x90000000u);
        Line(ex, ey, bx + nx, by + ny, 3.0f, col[i]); Line(ex, ey, bx - nx, by - ny, 3.0f, col[i]);
        DrawMarkNumber(font, dev, static_cast<int>(bx - ux * 18.0f) - 16, static_cast<int>(by - uy * 18.0f) - 4, num[i], col[i] | 0xFF000000u);
    }
}

// Spezzata chiusa (contorni).
void Outline(const float (*p)[2], int n, float ox, float oy, float sc, float w, DWORD c)
{
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        Line(ox + p[i][0] * sc, oy + p[i][1] * sc, ox + p[j][0] * sc, oy + p[j][1] * sc, w, c);
        Disc(ox + p[i][0] * sc, oy + p[i][1] * sc, w * 0.5f, w * 0.5f, c, c, 8);   // giunti arrotondati
    }
}

// Contorno di un rettangolo arrotondato.
void RoundOutline(float x0, float y0, float x1, float y1, float r, float w, DWORD c)
{
    float p[40][2];
    int n = 0;
    const float corners[4][2] = { { x1 - r, y0 + r }, { x1 - r, y1 - r }, { x0 + r, y1 - r }, { x0 + r, y0 + r } };
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i <= 6; ++i) {
            const float a = (-90.0f + k * 90.0f + i * 15.0f) * 0.0174533f;
            p[n][0] = corners[k][0] + r * std::cos(a); p[n][1] = corners[k][1] + r * std::sin(a); ++n;
        }
    Outline(p, n, 0, 0, 1, w, c);
}

// --- Schermata di aiuto (0.15.0, controller ridisegnato nella 0.15.1) ------------------------------
enum HelpKey { kLT, kLB, kView, kLS, kDUp, kDLR, kDDown, kRT, kRB, kMenu, kY, kX, kB, kA, kRS, kKeys };
const char* const kKeyName[3][kKeys] = {
    { "LT", "LB", "View", "Stick sx", "Croce su", "Croce sx/dx", "Croce giu'", "RT", "RB", "Menu", "Y", "X", "B", "A", "Stick dx" },
    { "LT", "LB", "View", "Stick sx", "Croce su", "Croce sx/dx", "Croce giu'", "RT", "RB", "Menu", "Y", "X", "B", "A", "Stick dx" },
    { "L2", "L1", "Create", "Stick sx", "Croce su", "Croce sx/dx", "Croce giu'", "R2", "R1", "Options", "Triangolo", "Quadrato",
      "Cerchio", "X (croce)", "Stick dx" } };
// 0.51.4: testi accorciati (prova del 29/10 a 1920x1080: le righe piu' lunghe della colonna destra uscivano dal riquadro, che
// non si puo' allargare perche' le finestre dello schermo diviso sulla Steam Machine sono larghe 1280).
const char* const kKeyAction[kKeys] = {
    "Arte 5", "Pozione di cura", "Cursore (tenuto: inventario)", "Movimento (premuto: minimappa)",
    "Concentrazione (tenuto: zoom +)", "Arma precedente / successiva", "Antidoto (tenuto: zoom -)",
    "Arte 4", "Arte 3", "Menu (tenuto: aiuto)", "Arte 2", "Arte 1",
    "Raccogli (tenuto: cavallo)", "Attacca e interagisci", "Mira (premuto: oggetti)" };
// 0.51.4: le stesse righe per il pad del giocatore 2 (NativePad2Tick, Memory.cpp): stessi tasti, ma l'eroe 2 non ha ancora
// mira, cavallo e minimappa. 0.55.0: A interagisce (PNG, forzieri, porte, oggetti fissi) o, con nemici vicini, attacca il
// piu' vicino; la croce cambia arma dalla 0.52.3. 0.55.16: L3 minimappa e R3 Alt come il pad 1. 0.55.17: stick destro = mira.
const char* const kKeyAction2[kKeys] = {
    "Arte 5", "Pozione di cura", "Cursore (tenuto: inventario)", "Movimento (premuto: minimappa)",
    "Concentrazione (tenuto: zoom +)", "Arma precedente / successiva", "Antidoto (tenuto: zoom -)",
    "Arte 4", "Arte 3", "Menu (tenuto: aiuto)", "Arte 2", "Arte 1",
    "Raccogli (tenuto: cavallo)", "Interagisci o attacca", "Mira (premuto: oggetti)" };   // 0.55.19: B tenuto = cavallo
// 0.55.54: con i due zaini Back del pad 2 apre e chiude il suo zaino (premuto o tenuto); con mercante e altri pannelli resta il cursore
const char* const kKeyAction2View = "Il tuo zaino (premuto o tenuto)";

void HelpText(void* font, void* dev, int x, int y, int w, const char* t, DWORD color, DWORD flags = 0)
{
    GameDrawText(font, dev, x + 1, y + 1, w, t, 0xFF000000u, flags);
    GameDrawText(font, dev, x, y, w, t, color, flags);
}

// Punto (in coordinate dello schermo) di ciascun tasto sul controller disegnato, per i richiami.
struct PadGeom { POINT key[kKeys]; float faceR = 14.0f; };

// 0.55.68: nuova schermata dei comandi (il tester, 02/11: proposta con le anteprime in docs/proposta_aiuto, "si', falla cosi'",
// tenendo i tre controller disegnati). Tasti come bollini colorati (A verde, B rosso, X blu, Y giallo come sui controller;
// sul DualSense i suoi simboli), azione e "tieni" su due righe, scritte nello stesso ordine dei tasti dall'alto in basso (cosi'
// le linee non si incrociano), tre pagine sfogliate con LB e RB, tasto premuto acceso (bollino e tasto sul disegno).
enum BadgeKind { kBadgeN = 0, kBadgeA, kBadgeB, kBadgeX, kBadgeY };
const DWORD kBadgeFill[5] = { 0xFF464A52u, 0xFF50963Cu, 0xFFB43C37u, 0xFF376EB4u, 0xFFC8A028u };
const DWORD kBadgeInk[5]  = { 0xFFF0F0F0u, 0xFFF0FFEBu, 0xFFFFF0EEu, 0xFFEBF5FFu, 0xFF281C00u };
const int   kBadgePs[5]   = { -1, 3, 2, 0, 1 };      // simbolo PlayStation per PsSymbol: croce, cerchio, quadrato, triangolo
const WORD  kKeyBits[kKeys] = { PAD_LT, PAD_LB, PAD_BACK, PAD_LS, PAD_DPAD_UP, PAD_DPAD_LEFT | PAD_DPAD_RIGHT, PAD_DPAD_DOWN,
                                PAD_RT, PAD_RB, PAD_START, PAD_Y, PAD_X, PAD_B, PAD_A, PAD_RS };
std::atomic<int>  g_helpPage{ 0 };                   // 0 in gioco, 1 cursore e zaino, 2 due giocatori
std::atomic<LONG> g_helpBtns{ 0 };                   // tasti premuti adesso dal giocatore dell'aiuto (con gli stick inclinati)

// Larghezza di un testo con il font del gioco: metodo +0x30, lo stesso che il disegno del font usa per centrare (0x649190).
int GameTextW(void* font, const char* t)
{
    using WFn = short(__fastcall*)(void* self, void* edx, const char* text);
    if (!font || !t) return 0;
    __try { return reinterpret_cast<WFn>((*reinterpret_cast<void***>(font))[0x30 / 4])(font, nullptr, t); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return static_cast<int>(strlen(t)) * 8; }
}

// Bollino di un tasto in (x, y), alto 26; con alignRight x e' il bordo destro. Restituisce la larghezza.
int HelpBadge(void* font, void* dev, int x, int y, const char* txt, int kind, int lay, bool alignRight, bool lit)
{
    const bool ps = lay == 2 && kind != kBadgeN;
    int w = ps ? 34 : GameTextW(font, txt) + 18;
    if (w < 34) w = 34;
    const int x0 = alignRight ? x - w : x;
    if (lit) RoundRect((float)x0 - 3, (float)y - 3, (float)x0 + w + 3, (float)y + 29, 15, 0xFFFFE08Cu, 0xFFFFE08Cu);
    RoundRect((float)x0, (float)y, (float)x0 + w, (float)y + 26, 13, kBadgeFill[kind], kBadgeFill[kind]);
    if (ps) PsSymbol(kBadgePs[kind], x0 + w / 2.0f, y + 13.0f, 6.0f, 2.0f, kBadgeInk[kind]);
    else GameDrawText(font, dev, x0, y + 2, w, txt, kBadgeInk[kind], 1);
    return w;
}

// Simboli dei tasti sopra i cerchi delle arti (0.14.2; 0.55.27 in una funzione, per le due barre): layout 0 Steam, 1 Xbox,
// 2 DualSense. 0.56.23 (il tester, 04/11: "le icone colorate dei pad sulle arti, non e' una buona idea secondo me crea confusione,
// inoltre cercherei di inglobare i simboli dentro le arti in modo che non esca dallo slot, stessa cosa sulle pozioni"): niente
// bollini colorati sopra il cerchio; dentro il cerchio, in basso (17 pixel sotto il centro: il cerchio visibile ha raggio ~29),
// una pastiglia scura semitrasparente alta 14 con la scritta o il simbolo del DualSense in oro tenue. Le pozioni: vedi
// DrawPotionLabels. 0.56.15 (il tester, 04/11: "sono brutte le icone playstation sopra le arti, alcune piu' piccole e altre piu' grandi"):
// quadrato e triangolo erano contorni di 11 pixel e R1, R2, L2 scritte del font, quindi di misure diverse. Ora tutti e cinque sono
// bollini uguali come nella schermata dei comandi (HelpBadge): 32x22, il primo blu e il secondo giallo (X e Y; sul DualSense con il
// quadrato e il triangolo disegnati dentro), gli altri grigi con la scritta al centro; ombra scura sotto, centro a 40 pixel
// sopra il centro del cerchio dell'arte come prima.
// 0.56.24 (il tester, 04/11, con le foto: "altre vanno abbassate, in generale meglio che stiano alla base cosi' non danno fastidio al
// disegno"): misurato sulle foto, il cerchio visibile ha il centro 4 pixel sotto y e il bordo interno a raggio ~25; pastiglia
// alta 12 con il centro 21 sotto y (17 sotto il centro del cerchio): il fondo tocca il bordo interno, il disegno resta libero.
constexpr DWORD kSlotInk = 0xFFE6CE8Cu, kSlotPill = 0xA80E1014u;   // oro tenue, fondo scuro semitrasparente
void DrawSlotLabels(void* dev, void* font, int count, const std::atomic<LONG>* xs, const std::atomic<LONG>* ys, int layout)
{
    const int lay = layout < 0 || layout > 2 ? 0 : layout;
    constexpr int kH = 12;
    for (int k = 0; k < count && k < 5; ++k) {
        const int x = xs[k].load(), y = ys[k].load();
        const char* txt = kSlotLabels[lay][k];
        const bool ps = lay == 2 && k < 2;
        int w = ps ? 18 : GameTextW(font, txt) + 6;
        if (w < 18) w = 18;
        const float cy = static_cast<float>(y + 21), x0 = static_cast<float>(x - w / 2), y0 = cy - kH / 2.0f;
        RoundRect(x0, y0, x0 + w, y0 + kH, kH / 2.0f, kSlotPill, kSlotPill);
        // 0.56.26 (il tester: "il triangolo e' piu' piccolo del quadrato, sembrano sconnessi"): a parita' di s il triangolo ha
        // meta' dell'area del quadrato (lato 2s); con s 1.35 volte piu' grande (largo 9, alto 8) pesano uguale
        if (ps) PsSymbol(k == 0 ? 0 : 1, static_cast<float>(x), k == 0 ? cy : cy + 0.6f, k == 0 ? 3.3f : 4.4f, 1.5f, kSlotInk);
        // 0.56.26 (il tester, foto della 0.56.25: "le icone delle arti sono ancora piu' in alto"): misurate sulla foto, le scritte
        // del font stavano 6 pixel sopra cy (righe 1046-1053 con cy 1056, il quadrato del DualSense 1052-1060): la cifra ha il
        // centro 9 pixel sotto il punto passato, non 15
        else GameDrawText(font, dev, static_cast<int>(x0), static_cast<int>(cy) - 9, w, txt, kSlotInk, 1);
    }
}

// 0.56.23: tasti delle pozioni, dentro i loro cerchi (piccoli: ~24 pixel), in basso: LB (L1 sul DualSense) sulla cura, freccia
// giu' e freccia su sulle pozioni della croce (0.56.25: Mentore e concentrazione, g_cfg.dpadPotion). Lettere e frecce a tratti.
// 0.56.24: alla base del cerchio (raggio esterno ~15): pastiglia 15x9 con il centro 9 sotto il centro del cerchio, lettere alte 6.
void TinyGlyph(char g, float x, float y, DWORD c)   // x, y: angolo in alto a sinistra di una lettera 4x6
{
    const float w = 1.3f, k = 6.0f / 7.0f;   // il disegno e' su 7 righe, scalato a 6
    auto L = [&](float x1, float y1, float x2, float y2) { Line(x + x1, y + y1 * k, x + x2, y + y2 * k, w, c); };
    if (g == 'L') { L(0, 0, 0, 7); L(0, 7, 4, 7); }
    else if (g == 'B') {
        L(0, 0, 0, 7);
        L(0, 0, 3, 0); L(3, 0, 4, 1.2f); L(4, 1.2f, 4, 2.3f); L(4, 2.3f, 3, 3.5f);
        L(0, 3.5f, 3, 3.5f); L(3, 3.5f, 4, 4.7f); L(4, 4.7f, 4, 5.8f); L(4, 5.8f, 3, 7);
        L(0, 7, 3, 7);
    }
    else if (g == '1') { L(0.5f, 1.5f, 2.5f, 0); L(2.5f, 0, 2.5f, 7); }
}
void TinyArrow(float cx, float cy, bool up, DWORD c)
{
    const float h = 2.8f, s = 3.4f;
    Vtx v[3] = { V(cx, up ? cy - h : cy + h, c), V(cx + s, up ? cy + h : cy - h, c), V(cx - s, up ? cy + h : cy - h, c) };
    Prims(4, v, 3);
}
void DrawPotionLabels(int who, int layout)
{
    if (g_potCount[who].load() != 3) return;
    for (int i = 0; i < 3; ++i) {
        const float cx = static_cast<float>(g_potX[who][i].load()), cy = static_cast<float>(g_potY[who][i].load()) + 11.0f;   // 0.56.27: +11 (era +9; il tester: "di pochissimo verso il basso")
        RoundRect(cx - 7.5f, cy - 4.5f, cx + 7.5f, cy + 4.5f, 4.5f, kSlotPill, kSlotPill);
        if (i == 0) {
            const char second = layout == 2 ? '1' : 'B';
            TinyGlyph('L', cx - 4.75f, cy - 3, kSlotInk); TinyGlyph(second, cx + 0.75f, cy - 3, kSlotInk);
        }
        else TinyArrow(cx, cy, i == 2, kSlotInk);
    }
}

// Controller disegnato con il centro in (cx, cy), a contorni chiari come la schermata dei comandi di
// Steam (0.15.2). lay 0 Steam Controller (croce e tasti in alto, stick al centro, due trackpad sotto),
// 1 Xbox (stick sinistro in alto, croce e stick destro in basso), 2 DualSense (touchpad, stick in basso).
// 0.55.68: scala (scale), tasti frontali pieni nei loro colori, tasti premuti (lit) cerchiati in oro.
void DrawPad(void* font, void* dev, int lay, float cx, float cy, PadGeom& g, float scale, WORD lit)
{
    const DWORD ink = 0xFFE6E6E6u, gold = 0xFFFFE08Cu, fill = 0xB0141820u;
    const float w = 3.0f;
    // Sagoma: corpo con le impugnature (Steam con impugnature piu' lunghe).
    static const float body[][2] = {
        { -120, -112 }, { 120, -112 }, { 170, -106 }, { 200, -88 }, { 216, -58 }, { 226, -18 }, { 236, 40 }, { 246, 100 },
        { 250, 145 }, { 242, 175 }, { 222, 190 }, { 196, 188 }, { 176, 170 }, { 156, 128 }, { 130, 92 }, { 100, 78 },
        { -100, 78 }, { -130, 92 }, { -156, 128 }, { -176, 170 }, { -196, 188 }, { -222, 190 }, { -242, 175 }, { -250, 145 },
        { -246, 100 }, { -236, 40 }, { -226, -18 }, { -216, -58 }, { -200, -88 }, { -170, -106 } };
    const int nb = static_cast<int>(sizeof(body) / sizeof(body[0]));
    // Xbox e DualSense un po' piu' compatti; 0.51.7: tutto all'80% (prima le impugnature arrivavano sulla sezione
    // "Modalita' cursore", screenshot del tester del 29/10); 0.55.68: la scala la sceglie chi disegna
    const float k = (lay == 0 ? 1.0f : 0.92f) * scale;
    // Fondo leggermente piu' scuro del pannello (corpo e impugnature).
    RoundRect(cx - 225 * k, cy - 110 * k, cx + 225 * k, cy + 80 * k, 60 * k, fill, fill);
    TiltDisc(cx - 200 * k, cy + 110 * k, 48 * k, 85 * k, 0.32f, fill, fill);
    TiltDisc(cx + 200 * k, cy + 110 * k, 48 * k, 85 * k, -0.32f, fill, fill);
    Outline(body, nb, cx, cy, k, w, ink);
    // Dorsali e grilletti (sopra le spalle).
    RoundOutline(cx - 205 * k, cy - 134 * k, cx - 105 * k, cy - 116 * k, 8, 2.5f, ink);
    RoundOutline(cx + 105 * k, cy - 134 * k, cx + 205 * k, cy - 116 * k, 8, 2.5f, ink);
    RoundOutline(cx - 190 * k, cy - 160 * k, cx - 125 * k, cy - 140 * k, 8, 2.5f, ink);
    RoundOutline(cx + 125 * k, cy - 160 * k, cx + 190 * k, cy - 140 * k, 8, 2.5f, ink);
    g.key[kLT] = POINT{ (LONG)(cx - 158 * k), (LONG)(cy - 150 * k) }; g.key[kRT] = POINT{ (LONG)(cx + 158 * k), (LONG)(cy - 150 * k) };
    g.key[kLB] = POINT{ (LONG)(cx - 155 * k), (LONG)(cy - 125 * k) }; g.key[kRB] = POINT{ (LONG)(cx + 155 * k), (LONG)(cy - 125 * k) };
    // Posizioni dei comandi (unita' del disegno, centro del controller = 0,0).
    float dpx, dpy, fbx, fby, lsx, lsy, rsx, rsy, vwx, vwy, mnx, mny, hbx, hby;
    if (lay == 0) {        // Steam Controller
        dpx = -150; dpy = -52; fbx = 150; fby = -52; lsx = -78; lsy = -18; rsx = 78; rsy = -18;
        vwx = -92; vwy = -82; mnx = 92; mny = -82; hbx = 0; hby = -60;
    }
    else if (lay == 1) {   // Xbox
        lsx = -130; lsy = -42; dpx = -72; dpy = 22; rsx = 72; rsy = 22; fbx = 135; fby = -42;
        vwx = -42; vwy = -50; mnx = 42; mny = -50; hbx = 0; hby = -80;
    }
    else {                 // DualSense
        dpx = -140; dpy = -40; fbx = 140; fby = -40; lsx = -68; lsy = 28; rsx = 68; rsy = 28;
        vwx = -100; vwy = -92; mnx = 100; mny = -92; hbx = 0; hby = 38;
    }
    auto P = [&](float x, float y) { return POINT{ (LONG)(cx + x * k), (LONG)(cy + y * k) }; };
    // Croce direzionale (contorno a croce).
    {
        const float a = 11, l = 32;
        const float cross[][2] = { { -a, -l }, { a, -l }, { a, -a }, { l, -a }, { l, a }, { a, a }, { a, l }, { -a, l },
                                   { -a, a }, { -l, a }, { -l, -a }, { -a, -a } };
        Outline(cross, 12, cx + dpx * k, cy + dpy * k, k, 2.5f, ink);
    }
    g.key[kDUp] = P(dpx, dpy - 22); g.key[kDLR] = P(dpx - 22, dpy); g.key[kDDown] = P(dpx, dpy + 22);
    // Stick: anello esterno e cappuccio.
    for (int i = 0; i < 2; ++i) {
        const float sx = cx + (i ? rsx : lsx) * k, sy = cy + (i ? rsy : lsy) * k;
        Ring(sx, sy, 34 * k - 1.5f, 34 * k + 1.5f, ink);
        Ring(sx, sy, 24 * k - 1.2f, 24 * k + 1.2f, ink);
    }
    g.key[kLS] = P(lsx, lsy); g.key[kRS] = P(rsx, rsy);
    // Tasti frontali: Y in alto, X a sinistra, B a destra, A in basso, pieni nei loro colori; lettere o simboli sopra.
    const float fd = 27, fr = 14 * k;
    const POINT fb[4] = { P(fbx, fby - fd), P(fbx - fd, fby), P(fbx + fd, fby), P(fbx, fby + fd) };
    const char* letter[4] = { "Y", "X", "B", "A" };
    const int psSym[4] = { 1, 0, 2, 3 };
    const int kinds[4] = { kBadgeY, kBadgeX, kBadgeB, kBadgeA };
    for (int i = 0; i < 4; ++i) {
        Disc((float)fb[i].x, (float)fb[i].y, fr, fr, kBadgeFill[kinds[i]], kBadgeFill[kinds[i]]);
        Ring((float)fb[i].x, (float)fb[i].y, fr - 1.2f, fr + 1.2f, ink);
        if (lay == 2) PsSymbol(psSym[i], (float)fb[i].x, (float)fb[i].y, 6.0f, 2.0f, kBadgeInk[kinds[i]]);
        else GameDrawText(font, dev, fb[i].x - 10, fb[i].y - 9, 20, letter[i], kBadgeInk[kinds[i]], 1);
    }
    g.key[kY] = fb[0]; g.key[kX] = fb[1]; g.key[kB] = fb[2]; g.key[kA] = fb[3];
    g.faceR = fr;
    // View / Menu (Select / Start, Create / Options).
    RoundOutline(cx + vwx * k - 14, cy + vwy * k - 7, cx + vwx * k + 14, cy + vwy * k + 7, 6, 2.0f, ink);
    RoundOutline(cx + mnx * k - 14, cy + mny * k - 7, cx + mnx * k + 14, cy + mny * k + 7, 6, 2.0f, ink);
    g.key[kView] = P(vwx, vwy); g.key[kMenu] = P(mnx, mny);
    // Tasto centrale (Steam, Xbox, PS) e parti proprie.
    Ring(cx + hbx * k, cy + hby * k, 13 * k - 1.2f, 13 * k + 1.2f, ink);
    if (lay == 0) {
        // Trackpad quadrati e tasto "..." in basso al centro.
        // 0.51.7: piu' bassi e stretti, dentro il corpo (il bordo inferiore fra le impugnature e' a y 78..92; prima arrivavano a 98)
        RoundOutline(cx - 140 * k, cy + 20 * k, cx - 64 * k, cy + 70 * k, 12, 2.5f, ink);
        RoundOutline(cx + 64 * k, cy + 20 * k, cx + 140 * k, cy + 70 * k, 12, 2.5f, ink);
        RoundOutline(cx - 22 * k, cy + 52 * k, cx + 22 * k, cy + 66 * k, 7, 2.0f, ink);
        for (int i = -1; i <= 1; ++i) Disc(cx + i * 8 * k, cy + 59 * k, 1.8f, 1.8f, ink, ink, 8);
    }
    else if (lay == 2) {
        RoundOutline(cx - 75 * k, cy - 112 * k, cx + 75 * k, cy - 45 * k, 12, 2.5f, ink);   // touchpad
    }
    // Tasti premuti: cerchio d'oro intorno al punto del tasto.
    for (int i = 0; i < kKeys; ++i) {
        if (!(lit & kKeyBits[i])) continue;
        const float r = (i == kA || i == kB || i == kX || i == kY) ? fr + 4.0f : (i == kLS || i == kRS) ? 34 * k + 4.0f : 14.0f;
        Ring((float)g.key[i].x, (float)g.key[i].y, r - 1.5f, r + 1.5f, gold);
    }
}

// Riga di un comando: tasto (indice kKeys o -1), nome sul bollino, colore, azione e seconda riga (o nullptr).
struct HelpLine { int key; const char* name; int kind; const char* act; const char* sub; };

int BadgeKindOf(int key)
{
    return key == kA ? kBadgeA : key == kB ? kBadgeB : key == kX ? kBadgeX : key == kY ? kBadgeY : kBadgeN;
}

// Pagine 1 e 2: tre colonne con titolo e righe.
void HelpColumns(void* dev, void* font, int lay, int cx, int cy, const char* const heads[3], const HelpLine* const cols[3],
                 const int counts[3], WORD lit)
{
    const DWORD gold = 0xFFFFE08Cu, white = 0xFFE8E8E8u, dim = 0xFFB0A078u, guide = 0xFF5A5037u;
    const int colx[3] = { cx - 535, cx - 175, cx + 185 };
    for (int c = 0; c < 3; ++c) {
        HelpText(font, dev, colx[c], cy - 150, 330, Tr(heads[c]), gold);
        Line((float)colx[c], (float)cy - 126, (float)colx[c] + 330, (float)cy - 126, 1, guide);
        int y = cy - 110;
        for (int i = 0; i < counts[c]; ++i) {
            const HelpLine& h = cols[c][i];
            const bool on = h.key >= 0 && (lit & kKeyBits[h.key]);
            const int w = HelpBadge(font, dev, colx[c], y, h.name, h.kind, lay, false, on);
            const int tx = colx[c] + (w > 50 ? w : 50) + 12;
            HelpText(font, dev, tx, y + 5, 330 - (tx - colx[c]), Tr(h.act), white);
            if (h.sub) HelpText(font, dev, tx, y + 25, 330 - (tx - colx[c]), Tr(h.sub), dim);
            y += h.sub ? 52 : 40;
        }
    }
}

void DrawHelp(void* dev, void* font, void* big, int sw, int sh)
{
    // 0.51.3: l'aiuto si disegna con il controller di chi l'ha aperto (i tasti dei due pad sono gli stessi)
    // 0.53.3: sulle righe dei controller si disegna il controller della riga scelta (prova sulla Steam Machine del 29/10:
    // cambiando il controller del giocatore 2 dall'aiuto del giocatore 1 il disegno restava quello del giocatore 1). Nello
    // schermo diviso ogni finestra e' un gioco a se' con un solo controller: niente riga del giocatore 2.
    const bool split = g_cfg.coopRole != 0;
    const int row = g_helpRow.load();
    const int lay1 = g_cfg.padLayout < 0 || g_cfg.padLayout > 2 ? 0 : g_cfg.padLayout;
    const int lay2 = g_cfg.pad2Layout < 0 || g_cfg.pad2Layout > 2 ? 0 : g_cfg.pad2Layout;
    const int owner = split ? 1 : row == 0 ? 1 : row == 1 ? 2 : (g_helpOwner.load() == 2 ? 2 : 1);
    const int lay = owner == 2 ? lay2 : lay1;
    const int page = (g_helpPage.load() % 3 + 3) % 3;
    const WORD lit = static_cast<WORD>(g_helpBtns.load());
    const int cx = sw / 2, cy = sh / 2 - 40;
    const DWORD gold = 0xFFFFE08Cu, white = 0xFFE8E8E8u, line = 0xFFB89A5Au, dim = 0xFFB0A078u, guide = 0xFF5A5037u;
    RoundRect((float)cx - 560, (float)cy - 250, (float)cx + 560, (float)cy + 378, 18, 0xF4181C22u, 0xF40C0E12u);   // 0.51.6: piu' coprente
    Line((float)cx - 540, (float)cy - 205, (float)cx + 540, (float)cy - 205, 2, line);
    char title[128];
    if (owner == 2) _snprintf_s(title, sizeof(title), _TRUNCATE, Tr("SacredPad - comandi del controller del giocatore 2: %s"), PadLayoutName(lay));
    else _snprintf_s(title, sizeof(title), _TRUNCATE, Tr("SacredPad - comandi del controller: %s"), PadLayoutName(lay));
    HelpText(big, dev, cx - 560, cy - 238, 1120, title, gold, 1);
    // Linguette delle pagine, fra i bollini di LB e RB.
    {
        const char* tabs[3] = { Tr("In gioco"), Tr("Cursore e zaino"), Tr("Due giocatori") };
        int tw[3], total = 0;
        for (int i = 0; i < 3; ++i) { tw[i] = GameTextW(font, tabs[i]) + 36; total += tw[i]; }
        const int bl = (std::max)(34, GameTextW(font, kKeyName[lay][kLB]) + 18), br = (std::max)(34, GameTextW(font, kKeyName[lay][kRB]) + 18);
        int x = cx - (total + bl + br + 24) / 2;
        const int ty = cy - 194;
        x += HelpBadge(font, dev, x, ty, kKeyName[lay][kLB], kBadgeN, lay, false, (lit & PAD_LB) != 0) + 12;
        for (int i = 0; i < 3; ++i) {
            if (i == page) {
                RoundRect((float)x, (float)ty - 2, (float)x + tw[i], (float)ty + 28, 8, 0xE0806640u, 0xE0806640u);
                FillRect(x, ty - 2, x + 4, ty + 28, gold);
            }
            HelpText(font, dev, x + 18, ty + 5, tw[i], tabs[i], i == page ? gold : dim);
            x += tw[i];
        }
        HelpBadge(font, dev, x + 12, ty, kKeyName[lay][kRB], kBadgeN, lay, false, (lit & PAD_RB) != 0);
    }
    if (page == 0) {
        PadGeom g{};
        DrawPad(font, dev, lay, (float)cx, (float)cy + 35, g, 0.9f, lit);
        const bool two = owner == 2 && g_cfg.nativeTwoInventories;
        HelpLine L[kKeys] = {};
        L[kLT]    = { kLT, kKeyName[lay][kLT], kBadgeN, "Arte 5", nullptr };
        L[kLB]    = { kLB, kKeyName[lay][kLB], kBadgeN, "Pozione di cura", nullptr };
        // 0.56.2: con il secondo mouse View del pad 2 e' come quello del pad 1 (premuto = cursore, tenuto = zaino)
        const bool two2 = two && !MemoryMouse2Ready();
        L[kView]  = { kView, kKeyName[lay][kView], kBadgeN, two2 ? "Il tuo zaino" : "Cursore", two2 ? "premuto o tenuto" : "tieni: il tuo zaino" };
        L[kLS]    = { kLS, "L", kBadgeN, "Muovi", "premuto: minimappa" };
        L[kDUp]   = { kDUp, Tr("Su"), kBadgeN, PotionName(g_cfg.dpadPotion[0]), "tieni: zoom avanti" };   // 0.56.25
        L[kDLR]   = { kDLR, "<   >", kBadgeN, "Arma prec. / succ.", nullptr };
        L[kDDown] = { kDDown, Tr("Giu'"), kBadgeN, PotionName(g_cfg.dpadPotion[1]), "tieni: zoom indietro" };
        L[kRT]    = { kRT, kKeyName[lay][kRT], kBadgeN, "Arte 4", nullptr };
        L[kRB]    = { kRB, kKeyName[lay][kRB], kBadgeN, "Arte 3", nullptr };
        L[kMenu]  = { kMenu, kKeyName[lay][kMenu], kBadgeN, "Menu", "tieni: questo aiuto" };
        L[kY]     = { kY, "Y", kBadgeY, "Arte 2", nullptr };
        L[kX]     = { kX, "X", kBadgeX, "Arte 1", nullptr };
        L[kB]     = { kB, "B", kBadgeB, "Raccogli tutto", "tieni: cavallo" };
        L[kA]     = { kA, "A", kBadgeA, owner == 2 ? "Interagisci o attacca" : "Attacca e interagisci", owner == 2 ? nullptr : "tieni + stick: scegli" };
        L[kRS]    = { kRS, "R", kBadgeN, "Mira", "premuto: nomi oggetti" };
        // scritte nello stesso ordine dei tasti dall'alto in basso (a parita' prima il piu' esterno): le linee non si incrociano
        int left[] = { kLT, kLB, kView, kLS, kDUp, kDLR, kDDown };
        int right[] = { kRT, kRB, kMenu, kY, kX, kB, kA, kRS };
        std::sort(std::begin(left), std::end(left), [&](int a, int b) { return g.key[a].y != g.key[b].y ? g.key[a].y < g.key[b].y : g.key[a].x < g.key[b].x; });
        std::sort(std::begin(right), std::end(right), [&](int a, int b) { return g.key[a].y != g.key[b].y ? g.key[a].y < g.key[b].y : g.key[a].x > g.key[b].x; });
        for (int side = 0; side < 2; ++side) {
            const int* keys = side ? right : left;
            const int n = side ? 8 : 7;
            int y = cy - 150;
            for (int i = 0; i < n; ++i) {
                const int k = keys[i];
                const HelpLine& h = L[k];
                const bool on = (lit & kKeyBits[k]) != 0;
                const char* act = Tr(h.act);
                const char* sub = h.sub ? Tr(h.sub) : nullptr;
                const int aw = GameTextW(font, act);
                const float ly = (float)y + 13;
                float ax, tEnd;
                if (!side) {
                    const int w = HelpBadge(font, dev, cx - 545, y, h.name, h.kind, lay, false, on);
                    const int tx = cx - 545 + w + 10;
                    HelpText(font, dev, tx, y + 5, 300, act, white);
                    if (sub) HelpText(font, dev, tx, y + 25, 300, sub, dim);
                    ax = (float)cx - 252; tEnd = (float)tx + aw + 6;
                    if (tEnd < ax) Line(tEnd, ly, ax, ly, 1, guide);
                }
                else {
                    const int w = HelpBadge(font, dev, cx + 545, y, h.name, h.kind, lay, true, on);
                    const int tx = cx + 545 - w - 10 - aw;
                    HelpText(font, dev, tx, y + 5, aw + 8, act, white);
                    if (sub) { const int sw2 = GameTextW(font, sub); HelpText(font, dev, cx + 545 - w - 10 - sw2, y + 25, sw2 + 8, sub, dim); }
                    ax = (float)cx + 252; tEnd = (float)tx - 6;
                    if (tEnd > ax) Line(ax, ly, tEnd, ly, 1, guide);
                }
                float px = (float)g.key[k].x, py = (float)g.key[k].y;
                const bool face = (k == kA || k == kB || k == kX || k == kY);
                if (face) {                                   // la linea arriva al bordo del tasto colorato
                    const float dx = ax - px, dy = ly - py, d = std::sqrt(dx * dx + dy * dy);
                    if (d > 1.0f) { px += dx / d * (g.faceR + 2.0f); py += dy / d * (g.faceR + 2.0f); }
                }
                Line(ax, ly, px, py, 1.5f, line);
                if (!face) Disc(px, py, 3, 3, line, line, 10);
                y += sub ? 48 : 38;
            }
        }
    }
    else if (page == 1) {
        static const char* const heads[3] = { "Nel cursore", "Schede", "Mercante" };
        const HelpLine c0[] = {
            { kLS, "L", kBadgeN, "Muovi il cursore", nullptr },
            { kDUp, Tr("Croce"), kBadgeN, "Caselle e pulsanti", nullptr },
            { kA, "A", kBadgeA, "Clic", "prendi e posa" },
            { kB, "B", kBadgeB, "Clic destro", "usa, equipaggia" },
            { kY, "Y", kBadgeY, "Chiudi la finestra", nullptr } };
        const HelpLine c1[] = {
            { kLB, kKeyName[lay][kLB], kBadgeN, "Scheda precedente", "zaino, arti, combo" },
            { kRB, kKeyName[lay][kRB], kBadgeN, "Scheda successiva", "anche dal mercante" },
            { kView, kKeyName[lay][kView], kBadgeN, "Torna al gioco", nullptr } };
        const HelpLine c2[] = {
            { kX, "X", kBadgeX, "Vendi", "l'oggetto sotto il cursore" },
            { kY, "Y", kBadgeY, "Esci dal negozio", nullptr } };
        const HelpLine* const cols[3] = { c0, c1, c2 };
        const int counts[3] = { 5, 3, 2 };
        HelpColumns(dev, font, lay, cx, cy, heads, cols, counts, lit);
    }
    else {
        // 0.56.2: con il secondo mouse (un mouse del gioco per giocatore) la seconda e la terza colonna cambiano
        const bool m2 = MemoryMouse2Ready();
        static const char* const headsOld[3] = { "Il tuo zaino", "Il mouse del gioco", "Pad 1 con lo zaino del 2" };
        static const char* const headsM2[3] = { "Il tuo zaino", "Un mouse per giocatore", "Giocatore 2 col cursore" };
        const char* const* heads = m2 ? headsM2 : headsOld;
        const HelpLine c0[] = {
            { kView, kKeyName[lay1][kView], kBadgeN, "Giocatore 1", "tieni: zaino a sinistra" },
            { kView, kKeyName[lay2][kView], kBadgeN, "Giocatore 2", m2 ? "tieni: zaino a destra" : "premi: zaino a destra" },
            { kY, "Y", kBadgeY, "Chiudi il tuo lato", "l'altro resta aperto" } };
        const HelpLine c1old[] = {
            { kA, "A", kBadgeA, "Lo prende chi preme", "o chi muove lo stick" },
            { -1, "1  2", kBadgeN, "Puntatori numerati", "1 oro, 2 azzurro" },
            { -1, "!", kBadgeN, "Qualcosa in mano", "l'altro aspetta" } };
        const HelpLine c1m2[] = {
            { -1, "1  2", kBadgeN, "Due cursori del gioco", "il 2 ha il numero accanto" },
            { kA, "A", kBadgeA, "Ognuno ha la sua mano", "oggetti presi insieme" },
            { kView, kKeyName[lay2][kView], kBadgeN, "Cursore del giocatore 2", "View premuto, come il pad 1" } };
        const HelpLine c2old[] = {
            { kA, "A", kBadgeA, "Attacca, apre, parla", "forzieri e personaggi" },
            { kB, "B", kBadgeB, "Raccogli tutto", nullptr },
            { kX, "X", kBadgeX, "Arti", "anche Y, RB, RT, LT" },
            { kLB, kKeyName[lay][kLB], kBadgeN, "Pozione di cura", nullptr } };
        const HelpLine c2m2[] = {
            { kA, "A", kBadgeA, "Clic: attacca, usa, cammina", "tieni sul terreno: cammina" },
            { kB, "B", kBadgeB, "Arte della sua barra", "sul nemico indicato" },
            { kView, kKeyName[lay2][kView], kBadgeN, "Torna al gioco", "tieni: il suo zaino" } };
        const HelpLine* const cols[3] = { c0, m2 ? c1m2 : c1old, m2 ? c2m2 : c2old };
        const int counts[3] = { 3, 3, m2 ? 3 : 4 };
        HelpColumns(dev, font, lay, cx, cy, heads, cols, counts, lit);
    }
    // Impostazioni (0.51.5, richiesta del tester del 29/10: righe una sotto l'altra nell'ordine giocatore 1, giocatore 2,
    // vibrazione; la riga scelta su una fascia evidenziata con le frecce ai lati del valore; carattere semplice al posto
    // del Carolingia, piu' leggibile). Righe: 0 controller del giocatore 1, 1 controller del giocatore 2, 2 vibrazione.
    Line((float)cx - 540, (float)cy + 254, (float)cx + 540, (float)cy + 254, 1, line);
    // 0.56.16: quarta riga, indicatori dei giocatori (anello e freccia); righe a passo 24 da +262 perche' ci stiano tutte
    const char* labels[4] = { Tr(split ? "Controller" : "Controller giocatore 1"), Tr("Controller giocatore 2"), Tr("Vibrazione a vita bassa"),
                              Tr("Indicatori dei giocatori") };
    const char* values[4] = { PadLayoutName(lay1), PadLayoutName(lay2), Tr(g_cfg.vibration ? "attiva" : "disattivata"),
                              Tr(g_cfg.playerMarks ? "accesi" : "spenti") };
    // 0.51.6: valore e frecce vicini, fascia larga quanto il contenuto con un segno a sinistra; i testi centrati del font
    // escono 4 pixel piu' in basso di quelli allineati a sinistra (screenshot del 29/10): +2 e -2 li rimettono in riga.
    for (int i = 0, slot = 0; i < 4; ++i) {
        if (split && i == 1) continue;
        const int ry = cy + 262 + (slot++) * 24;   // 0.51.8: piu' spazio dalla linea (0.56.16: quattro righe)
        const bool sel = (i == row);
        if (sel) {
            RoundRect((float)cx - 300, (float)ry - 4, (float)cx + 250, (float)ry + 20, 6, 0x70806640u, 0x70584628u);
            FillRect(cx - 300, ry - 2, cx - 296, ry + 18, gold);
        }
        const DWORD c = sel ? gold : white;
        HelpText(font, dev, cx - 284, ry + 2, 300, labels[i], c);
        HelpText(font, dev, cx + 60, ry - 2, 20, "<", sel ? gold : line, 1);
        HelpText(font, dev, cx + 80, ry - 2, 140, values[i], c, 1);
        HelpText(font, dev, cx + 220, ry - 2, 20, ">", sel ? gold : line, 1);
    }
    char t[256];
    _snprintf_s(t, sizeof(t), _TRUNCATE, Tr("Premi un tasto: si accende   %s / %s: pagina   Croce su / giu': scegli   sinistra / destra: cambia   %s: chiudi"),
                Tr(kKeyName[lay][kLB]), Tr(kKeyName[lay][kRB]), Tr(kKeyName[lay][kB]));
    HelpText(font, dev, cx - 560, cy + 356, 1120, t, white, 1);
}

// Voce "Controller" sopra il menu Esc (0.15.1): disegnata nello stile del menu, apre l'aiuto con A.
std::atomic<bool> g_escOn{ false }, g_escHover{ false };
std::atomic<LONG> g_escX0{ 0 }, g_escY0{ 0 }, g_escX1{ 0 }, g_escY1{ 0 };
std::atomic<DWORD> g_escFont{ 0 }, g_escColor{ 0 }, g_escHoverColor{ 0 };

void* ManagerFont(DWORD index);

// 0.51.8: voce del menu con ombra e, se evidenziata, alone dorato come le voci vere del gioco (cUI_StaticText64FX,
// screenshot del tester del 29/10 su "Extra"): la scritta ripetuta attorno a se' in oro semitrasparente, poi quella vera.
void DrawMenuText(void* dev, void* f, int x0, int y0, int w, int h, const char* text, DWORD col, bool hover)
{
    auto draw = reinterpret_cast<FontDrawFn>((*reinterpret_cast<void***>(f))[0x1c / 4]);
    TextRect r{ x0 + 3, y0 + 3, static_cast<short>(w), static_cast<short>(h) };
    draw(f, nullptr, dev, &r, text, 0xA0000000u, 1);
    if (hover) {
        // 0.51.9: un po' piu' leggero (lo screenshot della 0.51.8 era piu' marcato di "Extra"): raggio 2, senza l'anello a 3
        static const int off[][2] = { { -2, 0 }, { 2, 0 }, { 0, -2 }, { 0, 2 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
        for (int i = 0; i < 8; ++i) {
            r = TextRect{ x0 + off[i][0], y0 + off[i][1], static_cast<short>(w), static_cast<short>(h) };
            draw(f, nullptr, dev, &r, text, 0x48FFC040u, 1);
        }
    }
    r = TextRect{ x0, y0, static_cast<short>(w), static_cast<short>(h) };
    draw(f, nullptr, dev, &r, text, col, 1);
}

// Riga generica nello stile delle voci (menu Esc e menu principale).
void DrawRow(void* dev, void* big, int x0, int y0, int w, int h, DWORD fontIdx, DWORD color, DWORD hoverColor, bool hover, const char* text)
{
    void* f = ManagerFont(fontIdx);
    if (!f) f = big;
    DWORD c = color;
    if (!c) c = 0xFFE9DFC6u;
    DWORD hc = hoverColor;
    if (!hc) hc = 0xFFFFFFFFu;
    DrawMenuText(dev, f, x0, y0, w, h, text, hover ? hc : c, hover);
}

std::atomic<bool> g_mmOn{ false }, g_mmHover{ false };
std::atomic<LONG> g_mmX0{ 0 }, g_mmY0{ 0 }, g_mmX1{ 0 }, g_mmY1{ 0 };
std::atomic<DWORD> g_mmFont{ 0 }, g_mmColor{ 0 }, g_mmHoverColor{ 0 };
std::atomic<const char*> g_mmLabel{ "Co-op locale" };

void DrawEscItem(void* dev, void* big)
{
    // 0.15.3: riga vera del menu, disegnata come le altre voci da 0x6bb4c0: stesso font (n. in +0x94 della
    // prima voce), ombra a +3 pixel in 0xA0000000, colore normale o della voce evidenziata.
    const int x0 = g_escX0.load(), y0 = g_escY0.load(), w = g_escX1.load() - x0, h = g_escY1.load() - y0;
    void* f = ManagerFont(g_escFont.load());
    if (!f) f = big;
    DWORD c = g_escColor.load();
    if (!c) c = 0xFFE9DFC6u;
    DWORD hc = g_escHoverColor.load();
    if (!hc) hc = 0xFFFFFFFFu;
    // 0.51.9: niente alone: nel menu Esc del gioco la voce evidenziata e' solo bianca (screenshot del tester del 29/10)
    DrawMenuText(dev, f, x0, y0, w, h, "Controller", g_escHover.load() ? hc : c, false);
}

// Font n. index del gestore dei font ([0xcdb9f0], vettore in [+0]..[+4]); 4 = Carolingia 20.
void* ManagerFont(DWORD index)
{
    DWORD mgr = 0, beg = 0, end = 0, font = 0, vt = 0;
    if (!Rd(0x00cdb9f0, mgr) || !mgr || !Rd(mgr, beg) || !Rd(mgr + 4, end) || index >= (end - beg) / 4) return nullptr;
    if (!Rd(beg + index * 4, font) || !font || !Rd(font, vt)) return nullptr;
    if (vt != kVtFsFont && vt != kVtFontTTF && vt != kVtFontTTF2) return nullptr;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(font));
}

// Separata da OverlayEndScene perche' __try non convive con oggetti da distruggere.
// --- Campagna cooperativa (fase 5, 0.36.0): elenco di scelta dell'eroe 2, scritta di stato e avviso ----------------
// 0.55.43: elenco piu' grande e curato (richiesta del tester del 01/11: "abbellire e ingrandire il menu che si apre con Due
// giocatori"): schede alte 58 pixel su due righe (nome grande, sotto classe, livello e file), distintivo colorato della classe,
// pannello di 940 pixel con bordo dorato; 8 righe visibili (Memory.cpp, kPage).
// 0.55.51 (il tester, 01/11: "Menu piu' bello"): righe da 76 con il ritratto della classe preso dal gioco, livello in un riquadro
// del colore della classe, data dell'ultima partita, selezione che scivola e bordo che pulsa, tasti del controller disegnati.
constexpr int kPickVisible = 8, kPickRowH = 76, kPickW = 1000, kPickHead = 112, kPickFoot = 80;
float g_pickAnimY = -1.0f;               // posizione disegnata della selezione (thread di rendering)
struct PickState {
    bool open = false;
    int sel = 0, top = 0, hover = -1;
    std::vector<std::string> lines;
    std::string status, toast;
    DWORD toastUntil = 0;
};
std::mutex g_pickMx;
PickState g_pick;

// Barra dell'eroe 2 (0.42.0, miglioria N15 della sezione 8c): nome, livello e vita in alto al centro; con l'eroe 2 a
// terra, la scritta "a terra" e l'avanzamento del rialzo vicino al compagno. Protetta da g_pickMx come l'elenco.
struct Hero2State { bool show = false; std::string name; int level = 0, hp = 0, maxHp = 0, revivePct = -1; };
Hero2State g_hero2;

// Geometria dell'elenco, uguale per il disegno e per il clic: pannello centrato, titolo, righe, nota in fondo.
// 0.55.52 (il tester, 01/11: "se puoi farlo responsive in base alla risoluzione"): a 1080 righe le misure piene (pannello alto 800);
// se lo schermo del gioco e' piu' basso (portatile 1920x1200 con il gioco a una risoluzione minore, allungata) intestazione e
// piede si stringono, le righe scendono fino a 58 pixel e, se ancora non basta, si vedono meno righe (Memory.cpp scorre con
// OverlayPickRows). Font del gioco a misura fissa: sotto i 58 pixel le due righe di testo si sovrapporrebbero.
struct PickGeom { int x0, y0, x1, y1, rows0, nvis, rowH, head, foot; bool compact; };
PickGeom PickLayout(int sw, int sh, int n)
{
    PickGeom g{};
    const int full = n < kPickVisible ? n : kPickVisible;
    const int avail = sh - 30;
    g.rowH = kPickRowH; g.head = kPickHead; g.foot = kPickFoot; g.compact = false;
    if (g.head + full * g.rowH + g.foot > avail) {
        g.compact = true;
        g.head = 96; g.foot = 60;
        const int r = (avail - g.head - g.foot) / (full > 0 ? full : 1);
        g.rowH = r > kPickRowH ? kPickRowH : r < 58 ? 58 : r;
    }
    const int fit = (avail - g.head - g.foot) / g.rowH;
    g.nvis = full < fit ? full : (fit > 1 ? fit : 1);
    const int w = kPickW < sw - 40 ? kPickW : sw - 40;   // finestre strette dello schermo diviso
    const int h = g.head + g.nvis * g.rowH + g.foot;
    g.x0 = (sw - w) / 2; g.x1 = g.x0 + w;
    g.y0 = (sh - h) / 2; g.y1 = g.y0 + h;
    g.rows0 = g.y0 + g.head;
    return g;
}

// 0.55.43: distintivo della riga dell'elenco. Codice dopo "\x01" in testa al testo (Memory.cpp): classe dell'eroe '1'..'9' come
// in HeroClassName, 'S' schermo diviso, 'N' partita normale. Colori della classe e lettera (iniziale del nome della classe).
struct PickBadge { DWORD top, bot; char letter[4]; };
PickBadge PickBadgeOf(char code)
{
    PickBadge b{ 0xFF9A9A9Au, 0xFF4A4A4Au, "?" };
    switch (code) {
    case '1': b.top = 0xFF8CC4F0u; b.bot = 0xFF2F628Fu; break;   // Serafina: azzurro
    case '2': b.top = 0xFFF0A65Au; b.bot = 0xFF94501Cu; break;   // Gladiatore: arancio
    case '3': b.top = 0xFFB896F0u; b.bot = 0xFF553A94u; break;   // Mago: viola chiaro
    case '4': b.top = 0xFF9278C4u; b.bot = 0xFF34234Fu; break;   // Elfo oscuro: viola scuro
    case '5': b.top = 0xFF96D876u; b.bot = 0xFF336B22u; break;   // Elfa dei boschi: verde
    case '6': b.top = 0xFFDA6676u; b.bot = 0xFF701A26u; break;   // Vampira: cremisi
    case '8': b.top = 0xFFE0BA7Au; b.bot = 0xFF7F5A28u; break;   // Nano: ocra
    case '9': b.top = 0xFFF07646u; b.bot = 0xFF82240Eu; break;   // Diavola: rosso fuoco
    case 'S': b.top = 0xFF78C0C0u; b.bot = 0xFF235858u; strcpy_s(b.letter, "2"); return b;
    case 'N': b.top = 0xFFA8A8A8u; b.bot = 0xFF464646u; strcpy_s(b.letter, "1"); return b;
    default: return b;
    }
    const char* name = HeroClassName(code - '0');
    b.letter[0] = (name && name[0] && name[0] != '?') ? name[0] : '?';
    b.letter[1] = 0;
    return b;
}

void DrawHero2Bar(void* font, void* dev, int sw, const Hero2State& h)
{
    const DWORD gold = 0xFFFFE08Cu, white = 0xFFE8E8E8u;
    const int w = 300, x0 = (sw - w) / 2, y0 = MemoryHero2TaskbarOnTop() ? 140 : 26;   // 0.52.7: sotto la seconda barra in alto
    FillRect(x0 - 8, y0 - 4, x0 + w + 8, y0 + 34, 0xB0101418u);
    char t[128];
    if (h.hp > 0) _snprintf_s(t, sizeof(t), _TRUNCATE, Tr("%s  -  liv. %d   %d / %d"), h.name.c_str(), h.level, h.hp, h.maxHp);
    else _snprintf_s(t, sizeof(t), _TRUNCATE, Tr("%s  -  liv. %d   A TERRA"), h.name.c_str(), h.level);
    HelpText(font, dev, x0, y0, w, t, h.hp > 0 ? white : gold, 1);
    const int by0 = y0 + 20, by1 = y0 + 28;
    FillRect(x0, by0, x0 + w, by1, 0xFF2A0A0Au);
    if (h.hp > 0 && h.maxHp > 0) {
        const int fill = static_cast<int>(static_cast<long long>(w) * (std::min)(h.hp, h.maxHp) / h.maxHp);
        FillRect(x0, by0, x0 + fill, by1, 0xFFC02020u);
    } else if (h.revivePct >= 0) {                       // rialzo vicino al compagno in corso
        FillRect(x0, by0, x0 + w * (std::min)(h.revivePct, 100) / 100, by1, gold);
    }
}

void DrawCoopUi(void* dev, void* font, void* big, int sw, int sh)
{
    PickState st;
    Hero2State h2;
    {   // 0.41.2: a elenco chiuso (quasi sempre) si copiano solo le scritte, non tutte le righe
        std::lock_guard<std::mutex> lk(g_pickMx);
        if (g_pick.open) st = g_pick;
        else { st.status = g_pick.status; st.toast = g_pick.toast; st.toastUntil = g_pick.toastUntil; }
        if (g_hero2.show) h2 = g_hero2;
    }
    const DWORD gold = 0xFFFFE08Cu, white = 0xFFE8E8E8u, line = 0xFFB89A5Au;
    const DWORD now = GetTickCount();
    if (h2.show && !st.open) DrawHero2Bar(font, dev, sw, h2);
    if (!st.open && !st.status.empty())
        HelpText(font, dev, 14, 28, 600, st.status.c_str(), gold);
    if (!st.toast.empty() && static_cast<int>(st.toastUntil - now) > 0) {
        FillRect(sw / 2 - 360, 60, sw / 2 + 360, 104, 0xC0101418u);
        HelpText(big, dev, sw / 2 - 360, 70, 720, st.toast.c_str(), gold, 1);
    }
    if (!st.open) { g_pickAnimY = -1.0f; return; }
    FillRect(0, 0, sw, sh, 0xC0000000u);
    const PickGeom g = PickLayout(sw, sh, static_cast<int>(st.lines.size()));
    const int w = g.x1 - g.x0;
    const float fx0 = static_cast<float>(g.x0), fy0 = static_cast<float>(g.y0), fx1 = static_cast<float>(g.x1), fy1 = static_cast<float>(g.y1);
    const float pulse = 0.5f + 0.5f * std::sin((now % 1800) * 6.2831853f / 1800.0f);   // 0..1, per bordo e alone
    // pannello: ombra larga, doppio bordo dorato, fondo scuro sfumato, fascia del titolo
    RoundRect(fx0 + 10, fy0 + 14, fx1 + 10, fy1 + 14, 24, 0x60000000u, 0x60000000u);
    RoundRect(fx0 - 5, fy0 - 5, fx1 + 5, fy1 + 5, 24, 0xFFE8C474u, 0xFF5E4620u);
    RoundRect(fx0 - 2, fy0 - 2, fx1 + 2, fy1 + 2, 21, 0xFF141210u, 0xFF141210u);
    RoundRect(fx0, fy0, fx1, fy1, 20, 0xFA20262Fu, 0xFA090B0Eu);
    RoundRect(fx0 + 4, fy0 + 4, fx1 - 4, fy0 + g.head - 18, 17, 0x60705A30u, 0x00000000u);
    // intestazione: titolo, sottotitolo, fregio con il rombo e due punti
    HelpText(big, dev, g.x0, g.y0 + 24, w, Tr("Due giocatori"), gold, 1);
    HelpText(font, dev, g.x0, g.y0 + 62, w, Tr("Schermo diviso, o stesso schermo con l'eroe del giocatore 2"), 0xFFD8D2C4u, 1);
    const float mid = (fx0 + fx1) / 2, ly = fy0 + g.head - 12;
    Line(fx0 + 60, ly, fx1 - 60, ly, 1.0f, 0x80B89A5Au);
    Line(mid - 160, ly, mid + 160, ly, 2.0f, gold);
    Disc(mid - 172, ly, 3, 3, gold, gold);
    Disc(mid + 172, ly, 3, 3, gold, gold);
    RoundRect(mid - 6, ly - 6, mid + 6, ly + 6, 2, gold, 0xFFB08A3Au);
    // selezione che scivola da una riga all'altra (posizione inseguita a ogni fotogramma)
    const int selRow = st.sel - st.top;
    if (selRow >= 0 && selRow < g.nvis) {
        const float target = static_cast<float>(g.rows0 + selRow * g.rowH);
        if (g_pickAnimY < 0.0f || std::fabs(target - g_pickAnimY) > g.rowH * kPickVisible) g_pickAnimY = target;
        else g_pickAnimY += (target - g_pickAnimY) * 0.32f;
        if (std::fabs(target - g_pickAnimY) < 0.5f) g_pickAnimY = target;
        const float cx0 = fx0 + 18, cx1 = fx1 - 18, cy0 = g_pickAnimY + 4, cy1 = g_pickAnimY + g.rowH - 4;
        const DWORD glowA = static_cast<DWORD>(0x30 + 0x50 * pulse) << 24;
        RoundRect(cx0 - 6, cy0 - 6, cx1 + 6, cy1 + 6, 16, glowA | 0xE8C474u, glowA | 0x8A6A2Cu);
        RoundRect(cx0 - 2, cy0 - 2, cx1 + 2, cy1 + 2, 13, gold, 0xFFA07C34u);
        RoundRect(cx0, cy0, cx1, cy1, 12, 0xFA5A4A28u, 0xFA221A0Eu);
        FillRect(static_cast<int>(cx0) + 5, static_cast<int>(cy0) + 10, static_cast<int>(cx0) + 9, static_cast<int>(cy1) - 10, gold);
    }
    // righe: "\x02" in testa = linea che separa un gruppo, "\x01" + codice = ritratto o distintivo, "\x05" livello "\x05",
    // poi titolo, tabulazione e dettaglio
    for (int i = 0; i < g.nvis; ++i) {
        const int idx = st.top + i;
        if (idx >= static_cast<int>(st.lines.size())) break;
        const int y = g.rows0 + i * g.rowH;
        const bool sel = idx == st.sel, hov = idx == st.hover && !sel;
        std::string text = st.lines[idx];
        bool groupLine = false;
        if (!text.empty() && text[0] == '\x02') { text.erase(0, 1); groupLine = i > 0; }
        char code = 0;
        if (text.size() >= 2 && text[0] == '\x01') { code = text[1]; text.erase(0, 2); }
        int level = 0;
        if (!text.empty() && text[0] == '\x05') {
            const size_t e = text.find('\x05', 1);
            if (e != std::string::npos) { level = atoi(text.substr(1, e - 1).c_str()); text.erase(0, e + 1); }
        }
        std::string detail;
        const size_t tab = text.find('\t');
        if (tab != std::string::npos) { detail = text.substr(tab + 1); text.erase(tab); }
        if (groupLine) Line(fx0 + 50, (float)y, fx1 - 50, (float)y, 1.0f, 0x80B89A5Au);
        const float cx0 = fx0 + 18, cx1 = fx1 - 18, cy0 = (float)y + 4, cy1 = (float)(y + g.rowH) - 4;
        if (hov) RoundRect(cx0, cy0, cx1, cy1, 12, 0x80584A2Eu, 0x80302816u);
        else if (!sel) RoundRect(cx0, cy0, cx1, cy1, 12, 0x48323946u, 0x48161A20u);
        const PickBadge bd = PickBadgeOf(code);
        int tx = static_cast<int>(cx0) + 20;
        if (code) {
            // ritratto della classe in una cornice ovale (o il distintivo per schermo diviso e partita normale)
            const float ph = cy1 - cy0 - 6, pw = ph * 64.0f / 70.0f, px = cx0 + 18, py = cy0 + 3;
            const float ocx = px + pw / 2, ocy = py + ph / 2;
            Disc(ocx, ocy, pw / 2 + 4, ph / 2 + 4, sel ? gold : 0xFF6E5A34u, sel ? 0xFFA07C34u : 0xFF3A3020u, 32);
            Disc(ocx, ocy, pw / 2 + 1, ph / 2 + 1, bd.bot, 0xFF0C0C0Cu, 32);
            if (!DrawPortrait(dev, code, static_cast<int>(px), static_cast<int>(py), static_cast<int>(px + pw), static_cast<int>(py + ph),
                              sel || hov ? 0xFFFFFFFFu : 0xFFD0D0D0u)) {
                RoundRect(ocx - 22, ocy - 22, ocx + 22, ocy + 22, 9, bd.top, bd.bot);
                HelpText(big, dev, static_cast<int>(ocx) - 22, static_cast<int>(ocy) - 14, 44, bd.letter, 0xFFFFFFFFu, 1);
            }
            tx = static_cast<int>(px + pw) + 22;
        }
        // livello a destra, in un riquadro del colore della classe
        int right = static_cast<int>(cx1) - 24;
        if (level > 0) {
            const bool small = cy1 - cy0 - 20 < 44;          // righe basse: "LIV. 29" su una riga sola
            const float bw = small ? 92.0f : 78.0f, bx1 = cx1 - 46, bx0 = bx1 - bw, by0 = cy0 + (small ? 8 : 10), by1 = cy1 - (small ? 8 : 10);
            RoundRect(bx0 - 1, by0 - 1, bx1 + 1, by1 + 1, 10, sel ? gold : 0xFF5A4A2Au, sel ? 0xFFA07C34u : 0xFF2A2214u);
            RoundRect(bx0, by0, bx1, by1, 9, bd.top & 0xE0FFFFFFu, bd.bot);
            char lv[24];
            if (small) {
                _snprintf_s(lv, sizeof(lv), _TRUNCATE, "%s %d", Tr("LIV."), level);
                HelpText(font, dev, static_cast<int>(bx0), static_cast<int>((by0 + by1) / 2) - 9, static_cast<int>(bw), lv, 0xFFFFFFFFu, 1);
            } else {
                _snprintf_s(lv, sizeof(lv), _TRUNCATE, "%d", level);
                HelpText(font, dev, static_cast<int>(bx0), static_cast<int>(by0) + 3, static_cast<int>(bw), Tr("LIV."), 0xFFF4E8C8u, 1);
                HelpText(big, dev, static_cast<int>(bx0), static_cast<int>(by0) + 19, static_cast<int>(bw), lv, 0xFFFFFFFFu, 1);
            }
            right = static_cast<int>(bx0) - 12;
        }
        const int tw = right - tx;
        const int mid1 = y + (g.rowH - 32) / 2, top2 = y + (g.rowH - 56) / 2 + 1;   // una riga al centro, o due (a 76: 22, 11 e 43)
        if (detail.empty()) HelpText(big, dev, tx, mid1, tw, text.c_str(), sel || hov ? gold : white);
        else {
            HelpText(big, dev, tx, top2, tw, text.c_str(), sel || hov ? gold : white);
            HelpText(font, dev, tx, top2 + 32, tw, detail.c_str(), sel ? 0xFFF0DFB0u : line);
        }
        if (sel) HelpText(big, dev, static_cast<int>(cx1) - 40, mid1, 30, ">", gold, 1);
    }
    if (st.top > 0) HelpText(big, dev, g.x1 - 52, g.rows0 - 30, 30, "^", gold, 1);
    if (st.top + g.nvis < static_cast<int>(st.lines.size())) HelpText(big, dev, g.x1 - 52, g.rows0 + g.nvis * g.rowH - 4, 30, "v", gold, 1);
    // piede: tasti del controller disegnati (croce, A verde, B rossa) e, sotto, tastiera e mouse
    Line(fx0 + 50, fy1 - g.foot + 10, fx1 - 50, fy1 - g.foot + 10, 1.0f, 0x90B89A5Au);
    const int fy = static_cast<int>(fy1) - g.foot + (g.compact ? 22 : 26);
    const int slotW = 210, gx0 = static_cast<int>(mid) - slotW * 3 / 2;
    for (int k = 0; k < 3; ++k) {
        const float gcx = static_cast<float>(gx0 + k * slotW + 22), gcy = static_cast<float>(fy + 12);
        if (k == 0) {                                     // croce
            FillRect(static_cast<int>(gcx) - 12, static_cast<int>(gcy) - 4, static_cast<int>(gcx) + 12, static_cast<int>(gcy) + 4, 0xFFE6E6E6u);
            FillRect(static_cast<int>(gcx) - 4, static_cast<int>(gcy) - 12, static_cast<int>(gcx) + 4, static_cast<int>(gcy) + 12, 0xFFE6E6E6u);
        }
        else {                                            // A verde, B rossa
            const DWORD c0 = k == 1 ? 0xFF6CD46Cu : 0xFFE86A5Au, c1 = k == 1 ? 0xFF22782Au : 0xFF8A1E16u;
            Disc(gcx, gcy, 13, 13, c0, c1);
            Ring(gcx, gcy, 12, 14, 0xFF101010u);
            HelpText(font, dev, static_cast<int>(gcx) - 13, static_cast<int>(gcy) - 9, 26, k == 1 ? "A" : "B", 0xFFFFFFFFu, 1);
        }
        static const char* const kWhat[3] = { "scegli", "conferma", "annulla" };
        HelpText(font, dev, static_cast<int>(gcx) + 22, static_cast<int>(gcy) - 9, slotW - 50, Tr(kWhat[k]), white);
    }
    if (!g.compact) HelpText(font, dev, g.x0, fy + 32, w, Tr("Tastiera: frecce, Invio, Esc   Mouse: clic"), 0xFF9A8E74u, 1);
}

// 0.56.40: scheda Compagni dello zaino (vedi Memory.cpp, "scheda Compagni nello zaino"): la scheda e, aperta, la pagina sopra la
// finestra dello zaino, un compagno per riga con il pulsante a destra. 0.56.41: una per colonna; il pulsante e la scheda sotto un
// cursore piu' chiari, e i cursori ridisegnati sopra (il gioco li disegna prima dell'overlay e la pagina li copriva).
static void FrameRect(float l, float t, float r, float b, DWORD c)
{
    Line(l, t, r, t, 1.5f, c); Line(r, t, r, b, 1.5f, c); Line(r, b, l, b, 1.5f, c); Line(l, b, l, t, 1.5f, c);
}
void DrawCompPanel(void* font, void* dev)
{
    CompPanelView v;
    if (!MemoryCompPanel(v)) return;
    const DWORD ink = 0xFFE6CE8Cu, gold = 0xFFFFE08Cu, edge = 0xFF8C7A50u, dim = 0xFFB4AA96u;
    auto inR = [](const RECT& r, const POINT& q) { return q.x >= r.left && q.x < r.right && q.y >= r.top && q.y < r.bottom; };
    auto hot = [&](const RECT& r) { for (int j = 0; j < 2; ++j) if (v.curOn[j] && inR(r, v.cur[j])) return true; return false; };
    for (int s = 0; s < 2; ++s) {
        const CompPanelCol& c = v.col[s];
        if (!c.tab) continue;
        if (c.open) {
            const RECT& p = c.panelR;
            RoundRect((float)p.left, (float)p.top, (float)p.right, (float)p.bottom, 6.0f, 0xF6141810u, 0xF60E100Cu);
            FrameRect((float)p.left + 2, (float)p.top + 2, (float)p.right - 2, (float)p.bottom - 2, edge);
            GameDrawText(font, dev, p.left + 16, p.top + 8, p.right - p.left - 32, Tr("Compagni conosciuti in questa avventura"), gold, 0);
            if (v.n == 0) GameDrawText(font, dev, p.left + 16, p.top + 44, p.right - p.left - 32, Tr("Nessuno per ora: chi entra nel tuo gruppo compare qui"), dim, 0);
            for (int i = 0; i < v.n && i < kCompPanelRows; ++i) {
                const CompPanelRow& r = v.rows[i];
                const RECT b = MemoryCompRowButton(p, i);
                const LONG top = b.top - 5;
                if (top + 36 > p.bottom) break;
                const RECT row = MemoryCompRow(p, i);
                const bool h = hot(row);                   // 0.56.43: tutta la riga
                if (h) RoundRect((float)row.left, (float)row.top, (float)row.right, (float)row.bottom, 5.0f, 0x40B4A070u, 0x30806A40u);
                Line((float)p.left + 12, (float)top + 37, (float)p.right - 12, (float)top + 37, 1.0f, 0x608C7A50u);
                GameDrawText(font, dev, p.left + 20, top + 1, b.left - p.left - 30, r.name, ink, 0);
                GameDrawText(font, dev, p.left + 20, top + 17, b.left - p.left - 30, r.status, dim, 0);
                RoundRect((float)b.left, (float)b.top, (float)b.right, (float)b.bottom, 7.0f, h ? 0xF8605434u : 0xF0342E20u, h ? 0xF8403824u : 0xF0221E14u);
                FrameRect((float)b.left + 2, (float)b.top + 1, (float)b.right - 2, (float)b.bottom - 1, h ? gold : edge);
                GameDrawText(font, dev, b.left, (b.top + b.bottom) / 2 - 9, b.right - b.left, r.button, r.action ? gold : dim, 1);
            }
        }
        const RECT& t = c.tabR;                       // dopo la pagina: resta sopra anche se la freccetta e' dentro la finestra
        const bool h = hot(t);
        if (t.right - t.left < 60) {                  // 0.56.48: scheda a icona dopo Combo; 0.56.49: pastiglia come le schede del gioco
            const bool lit = c.open || h;
            const float l = (float)t.left, tp = (float)t.top, rr = (float)t.right, bt = (float)t.bottom, hh = bt - tp, rad = hh * 0.5f;
            // 0.56.51 (il tester: "la cornice dorata non serve, i tasti a fianco e' piu' grigio argento"): bordo argento come Combo
            const DWORD rim = lit ? 0xFFC4CAC6u : 0xFF8C928Eu,   // 0.56.52: "contorno poco piu' scuro"
                        inner = lit ? 0xFF7C847Eu : 0xFF545C57u;
            const DWORD fTop = lit ? 0xFF505C55u : 0xFF434D47u, fBot = lit ? 0xFF3C4640u : 0xFF2F3833u;
            RoundRect(l, tp, rr, bt, rad, rim, rim);                                       // bordo dorato
            RoundRect(l + 1.5f, tp + 1.5f, rr - 1.5f, bt - 1.5f, rad - 1.5f, fTop, fBot);  // fondo verde scuro
            RoundRect(l + 3.0f, tp + 3.0f, rr - 3.0f, bt - 3.0f, rad - 3.0f, inner, inner); // filo interno
            RoundRect(l + 4.0f, tp + 4.0f, rr - 4.0f, bt - 4.0f, rad - 4.0f, fTop, fBot);
            // 0.56.50 (il tester, 06/11, foto della 0.56.49: "l'icona dei due omini esce fuori, poi mi ricorda molto msn"): le spalle delle
            // due figure uscivano sotto la pastiglia. Ora due spade incrociate, tutte dentro il filo interno: lama chiara, elsa e
            // pomolo dorati, guardia di traverso vicino all'impugnatura.
            const float cx = (l + rr) * 0.5f, y0 = tp + 5.0f, y1 = bt - 5.5f, half = (y1 - y0) * 0.62f;
            const DWORD blade = lit ? 0xFFFFF4D0u : 0xFFE8DDBEu, hilt = lit ? 0xFFFFD060u : 0xFFC9A048u, grip = lit ? 0xFF8A5A2Cu : 0xFF6E4824u;
            for (int k = 0; k < 2; ++k) {
                const float sx0 = k == 0 ? cx - half : cx + half, sx1 = k == 0 ? cx + half : cx - half;   // impugnatura in basso, punta in alto
                const float dx = sx1 - sx0, dy = y0 - y1, len = std::sqrt(dx * dx + dy * dy), ux = dx / len, uy = dy / len;
                const float gx = sx0 + ux * len * 0.28f, gy = y1 + uy * len * 0.28f;                   // guardia
                Line(gx, gy, sx1 - ux * 1.0f, y0 - uy * 1.0f, 2.2f, blade);                            // lama
                Line(sx0 + ux * 1.5f, y1 + uy * 1.5f, gx, gy, 2.0f, grip);                             // impugnatura
                Line(gx - uy * 3.6f, gy + ux * 3.6f, gx + uy * 3.6f, gy - ux * 3.6f, 2.0f, hilt);      // guardia di traverso
                Disc(sx0 + ux * 0.8f, y1 + uy * 0.8f, 1.6f, 1.6f, hilt, hilt);                         // pomolo
            }
            if (h && !c.open) {
                const LONG lw = 110, lx = (t.left + t.right) / 2 - lw / 2, ly = t.top - 30;
                RoundRect((float)lx, (float)ly, (float)(lx + lw), (float)(ly + 24), 6.0f, 0xF0262A20u, 0xF0181A14u);
                FrameRect((float)lx + 2, (float)ly + 1, (float)(lx + lw) - 2, (float)(ly + 23), edge);
                GameDrawText(font, dev, lx, ly + 3, lw, Tr("Compagni"), gold, 1);
            }
        }
        else {
            RoundRect((float)t.left, (float)t.top, (float)t.right, (float)t.bottom, 9.0f, c.open || h ? 0xF0384030u : 0xE0262A20u, c.open || h ? 0xF0262C20u : 0xE0181A14u);
            FrameRect((float)t.left + 3, (float)t.top + 1, (float)t.right - 3, (float)t.bottom - 1, c.open || h ? gold : edge);
            GameDrawText(font, dev, t.left, (t.top + t.bottom) / 2 - 9, t.right - t.left, Tr("Compagni"), c.open || h ? gold : ink, 1);
        }
    }
    for (int j = 0; j < 2; ++j) {                    // cursori sopra la scheda e la pagina (MOUSE_MAIN.TGA, come i puntatori)
        if (!v.curOn[j]) continue;
        bool over = false;
        for (int s = 0; s < 2; ++s) if (v.col[s].tab && (inR(v.col[s].tabR, v.cur[j]) || (v.col[s].open && inR(v.col[s].panelR, v.cur[j])))) over = true;
        if (!over) continue;
        const int px = v.cur[j].x, py = v.cur[j].y;
        DrawIconRaw(dev, 3, px, py, px + 40, py + 40, j == 0 ? 0xFFFFFFFFu : 0xFF8CD8FFu, 0.5f, 0.5f);
    }
}

void DrawOverlay(void* dev)
{
    void* font = GameFont();
    if (!font) return;
    g_dev = dev;
    if (g_helpOn.load()) {
        DWORD drv0 = 0; short sw = 0, shh = 0;
        if (Rd(kDriver, drv0) && Rd(drv0 + 0x20, sw) && Rd(drv0 + 0x1c, shh) && sw > 0 && shh > 0) {
            void* big = ManagerFont(4);
            __try {
                DrawHelp(dev, font, big ? big : font, sw, shh);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_faults.fetch_add(1);
                g_helpOn.store(false);
                Log("Overlay: eccezione nel disegno dell'aiuto, chiuso");
            }
        }
        return;                                          // con l'aiuto aperto nient'altro
    }
    if (g_escOn.load()) {
        void* big = ManagerFont(4);
        __try { DrawEscItem(dev, big ? big : font); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); g_escOn.store(false); }
    }
    if (g_mmOn.load() && MemoryMainMenuRowShownNow()) {
        void* big = ManagerFont(4);
        __try { DrawRow(dev, big ? big : font, g_mmX0.load(), g_mmY0.load(), g_mmX1.load() - g_mmX0.load(), g_mmY1.load() - g_mmY0.load(),
                        g_mmFont.load(), g_mmColor.load(), g_mmHoverColor.load(), g_mmHover.load(), Tr(g_mmLabel.load())); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); g_mmOn.store(false); }
    }
    for (int k = 0; k < 4; ++k) {
        if (!g_gridOn[k].load()) continue;
        const int gx = g_gridX[k].load(), gy = g_gridY[k].load(), gc = g_gridC[k].load(), gr = g_gridR[k].load();
        __try {
            for (int r = 0; r < gr && r < 40; ++r)
                for (int c = 0; c < gc && c < 40; ++c)
                    GameDrawText(font, dev, gx + c * 32, gy + r * 32 + 9, 32, "+", 0xFFFF4040u, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1);
            g_gridOn[k].store(false);
        }
    }
    // 0.56.16: indicatori dei giocatori, prima delle altre scritte (che restano sopra)
    if (g_twoPlayers.load() && g_cfg.playerMarks && !g_helpOn.load() && MemoryUIState() == 0) {
        __try { DrawPlayerMarks(font, dev); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); }
    }
    if (!g_helpOn.load()) {                           // 0.56.18: alone sotto il bersaglio di A
        __try { DrawTargetGlows(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); }
    }
    const int slots = g_slotCount.load();
    if (slots > 0) {
        __try { DrawSlotLabels(dev, font, slots, g_slotX, g_slotY, g_cfg.padLayout); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1);
            g_slotCount.store(0);
        }
    }
    __try {                                                              // 0.56.23: tasti delle pozioni
        DrawPotionLabels(1, g_cfg.padLayout);
        if (GetTickCount() - g_potAt[2].load() <= 300) DrawPotionLabels(2, g_cfg.pad2Layout);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); g_potCount[1].store(0); g_potCount[2].store(0); }
    __try { DrawCompPanel(font, dev); }                                  // 0.56.40: scheda Compagni
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); }
    __try {                                                              // 0.56.33: cornice delle mani del compagno
        RECT hr[4] = {};
        const int hn = MemoryCompHands(hr);
        for (int i = 0; i < hn && i < 4; ++i) {
            const float l = static_cast<float>(hr[i].left), t = static_cast<float>(hr[i].top), r = static_cast<float>(hr[i].right), b = static_cast<float>(hr[i].bottom);
            const DWORD c = 0xD0B49A64u;                                 // oro scuro, come le cornici delle caselle
            Line(l, t, r, t, 2.0f, c); Line(r, t, r, b, 2.0f, c); Line(r, b, l, b, 2.0f, c); Line(l, b, l, t, 2.0f, c);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); }
    const int slots2 = g_slot2Count.load();
    if (slots2 > 0 && GetTickCount() - g_slot2At.load() <= 300) {      // 0.55.27: barra dell'eroe 2, pad 2
        __try { DrawSlotLabels(dev, font, slots2, g_slot2X, g_slot2Y, g_cfg.pad2Layout); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1);
            g_slot2Count.store(0);
        }
    }
    if (g_markOn.load()) {
        // 0.14.0: il cursore di Sacred (azione, dialogo, cavallo) al posto di "( A )".
        const int mx = g_markX.load(), my = g_markY.load();
        __try {
            DrawIconRaw(dev, g_markIcon.load(), mx - 20, my - 8, mx + 20, my + 32);
            if (g_twoPlayers.load()) DrawMarkNumber(font, dev, mx, my, "1", 0xFFFFC850u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1);
            g_markOn.store(false);
            Log("Overlay: eccezione nel disegno dell'icona, indicatore spento");
        }
    }
    if (g_ptrOn.load()) {
        const int owner = g_ptrOwner.load();
        __try {
            for (int who = 1; who <= 2; ++who) {
                if (who == 1 && !g_ptr1Show.load()) continue;
                const int px = who == 1 ? g_ptr1X.load() : g_ptr2X.load(), py = who == 1 ? g_ptr1Y.load() : g_ptr2Y.load();
                const DWORD col = who == 1 ? 0xFFFFC850u : 0xFF8CD8FFu;
                // 0.55.57: il cursore occupa solo il quarto in alto a sinistra di MOUSE_MAIN.TGA (32x32 su 64x64): prima la texture
                // intera in 32 pixel lo rendeva minuscolo (prova delle 15:35: del puntatore del giocatore 2 si vedeva solo il "2").
                // Alone colorato sotto la punta, cosi' si distingue dal cursore del gioco.
                if (owner != 0 && who != owner) {   // 0.56.0: owner 0 = due mouse, i cursori li disegna il gioco
                    Disc(static_cast<float>(px) + 6, static_cast<float>(py) + 6, 14, 14, (col & 0x00FFFFFFu) | 0x90000000u, col & 0x00FFFFFFu);
                    DrawIconRaw(dev, 3, px, py, px + 40, py + 40, col, 0.5f, 0.5f);
                }
                DrawMarkNumber(font, dev, px + 14, py + 30, who == 1 ? "1" : "2", col);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1);
            g_ptrOn.store(false);
            Log("Overlay: eccezione nel disegno dei due puntatori, spenti");
        }
    }
    // 0.56.22 (il tester, 04/11, foto della 0.56.21): in modalita' gioco il cursore del mouse del giocatore 1 resta fermo dove era e il
    // suo "1" sembrava un secondo bersaglio accanto a quello vero: il numero solo in modalita' cursore, dove il cursore serve
    if (g_twoPlayers.load() && !g_ptrOn.load() && MemoryMouse2Ready() && g_cursor.load()) {   // 0.56.6: "1" accanto al cursore del giocatore 1
        DWORD m1 = 0; int x1 = 0, y1 = 0;
        if (Rd(0x00cdcadc, m1) && m1 && Rd(m1 + 4, x1) && Rd(m1 + 8, y1)) {
            __try { DrawMarkNumber(font, dev, x1 + 14, y1 + 30, "1", 0xFFFFC850u); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); }
        }
    }
    if (g_c2On.load() && !g_ptrOn.load() && GetTickCount() - g_c2At.load() < 200) {
        __try { DrawMarkNumber(font, dev, g_c2X.load() + 14, g_c2Y.load() + 30, "2", 0xFF8CD8FFu); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); g_c2On.store(false); }
    }
    if (g_mark2On.load()) {
        const int mx = g_mark2X.load(), my = g_mark2Y.load();
        __try {
            DrawIconRaw(dev, g_mark2Icon.load(), mx - 20, my - 8, mx + 20, my + 32);
            DrawMarkNumber(font, dev, mx, my, "2", 0xFF8CD8FFu);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1);
            g_mark2On.store(false);
            Log("Overlay: eccezione nel disegno dell'indicatore dell'eroe 2, spento");
        }
    }
    {
        DWORD drvc = 0; short swc = 0, shc = 0;
        if (Rd(kDriver, drvc) && Rd(drvc + 0x20, swc) && Rd(drvc + 0x1c, shc) && swc > 0 && shc > 0) {
            void* bigc = ManagerFont(4);
            __try { DrawCoopUi(dev, font, bigc ? bigc : font, swc, shc); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); }
        }
    }
    DWORD drv = 0;
    short width = 0;
    if (!Rd(kDriver, drv) || !Rd(drv + 0x20, width) || width <= 0) return;
    // 0.56.58: [Overlay] ShowVersion=Menu (ini della release): la scritta solo fuori dal mondo, per gli screen puliti
    if (g_cfg.showVersion == 0) return;
    if (g_cfg.showVersion == 1) { int hx = 0, hy = 0; if (MemoryHeroPos(hx, hy)) return; }
    char text[128], ver[16];
    // 0.55.10: il numero di versione fino al primo spazio (con %.6s la 0.55.10 si leggeva "0.55.1")
    _snprintf_s(ver, sizeof(ver), _TRUNCATE, "%s", SACREDPAD_VERSION);
    if (char* sp = strchr(ver, ' ')) *sp = 0;
    _snprintf_s(text, sizeof(text), _TRUNCATE, Tr("SacredPad %s - modalita' %s"),
                ver, Tr(g_cursorP2.load() ? "cursore del giocatore 2" : g_cursor.load() ? "cursore" : "gioco"));
    // 0.52.7: con la barra dell'eroe 2 in alto al centro la scritta va in alto a sinistra
    const bool left = MemoryHero2TaskbarOnTop();
    const int tx = left ? 10 : 0, tw = left ? 600 : width;
    const DWORD tf = left ? 0 : 1;
    __try {
        GameDrawText(font, dev, tx + 1, 7, tw, text, 0xFF000000u, tf);   // ombra
        GameDrawText(font, dev, tx, 6, tw, text, 0xFFFFE08Cu, tf);       // testo color oro chiaro
        g_draws.fetch_add(1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1);
    }
}

HRESULT __stdcall OverlayEndScene(void* dev)
{
    if (g_faults.load() < 5) DrawOverlay(dev);   // dopo 5 eccezioni la scritta si spegne
    const HRESULT r = g_origEndScene(dev);
    EngineThreadTick();                          // fase 5: lavori di Memory.cpp nel thread del motore
    return r;
}

} // namespace

void OverlaySetCursorMode(bool on) { g_cursor.store(on); }
void OverlaySetCursorPlayer2(bool on) { g_cursorP2.store(on); }
void OverlaySetHelp(bool open) { g_helpOn.store(open); }
void OverlaySetHelpRow(int row) { g_helpRow.store(row); }
void OverlaySetHelpPage(int page) { g_helpPage.store(page); }
void OverlaySetHelpButtons(WORD buttons) { g_helpBtns.store(buttons); }
void OverlaySetHelpOwner(int player) { g_helpOwner.store(player == 2 ? 2 : 1); }
void OverlaySetEscItem(bool on, const RECT& r, bool hover, DWORD font, DWORD color, DWORD hoverColor)
{
    g_escX0.store(r.left); g_escY0.store(r.top); g_escX1.store(r.right); g_escY1.store(r.bottom);
    g_escFont.store(font); g_escColor.store(color);
    if (hoverColor) g_escHoverColor.store(hoverColor);
    g_escHover.store(hover); g_escOn.store(on);
}
void OverlaySetMainRow(bool on, const RECT& r, bool hover, DWORD font, DWORD color, DWORD hoverColor)
{
    g_mmX0.store(r.left); g_mmY0.store(r.top); g_mmX1.store(r.right); g_mmY1.store(r.bottom);
    g_mmFont.store(font); g_mmColor.store(color);
    if (hoverColor) g_mmHoverColor.store(hoverColor);
    g_mmHover.store(hover); g_mmOn.store(on);
}
void OverlaySetMainLabel(const char* literal) { g_mmLabel.store(literal); }
void OverlayCoopPicker(bool open, const std::vector<std::string>& lines, int sel, int top, int hover)
{
    std::lock_guard<std::mutex> lk(g_pickMx);
    g_pick.open = open; g_pick.lines = lines; g_pick.sel = sel; g_pick.top = top; g_pick.hover = hover;
}
void OverlayCoopStatus(const char* text)
{
    std::lock_guard<std::mutex> lk(g_pickMx);
    g_pick.status = text ? text : "";
}
void OverlayCoopToast(const char* text, DWORD ms)
{
    std::lock_guard<std::mutex> lk(g_pickMx);
    g_pick.toast = text ? text : "";
    g_pick.toastUntil = GetTickCount() + ms;
}
void OverlayHero2(bool show, const char* name, int level, int hp, int maxHp, int revivePct)
{
    std::lock_guard<std::mutex> lk(g_pickMx);
    g_hero2.show = show;
    if (!show) return;
    g_hero2.name = name ? name : Tr("Eroe 2");
    g_hero2.level = level; g_hero2.hp = hp; g_hero2.maxHp = maxHp; g_hero2.revivePct = revivePct;
}
// Riga (indice nell'elenco) sotto il punto del client, -1 fuori dalle righe, -2 fuori dal pannello.
int OverlayCoopPickerHit(int x, int y, int cw, int ch, int n, int top)
{
    DWORD drv = 0; short sw = 0, sh = 0;
    if (!Rd(kDriver, drv) || !Rd(drv + 0x20, sw) || !Rd(drv + 0x1c, sh) || sw <= 0 || sh <= 0 || cw <= 0 || ch <= 0) return -2;
    const int gx = x * sw / cw, gy = y * sh / ch;
    const PickGeom g = PickLayout(sw, sh, n);
    if (gx < g.x0 || gx >= g.x1 || gy < g.y0 || gy >= g.y1) return -2;
    if (gx < g.x0 + 14 || gx >= g.x1 - 14 || gy < g.rows0 || gy >= g.rows0 + g.nvis * g.rowH) return -1;
    const int idx = top + (gy - g.rows0) / g.rowH;
    return idx < n ? idx : -1;
}
// Righe che l'elenco mostra a questa risoluzione (per lo scorrimento in Memory.cpp); fuori dallo spazio anonimo, e' pubblica).
int OverlayPickRows()
{
    DWORD drv = 0; short sw = 0, sh = 0;
    if (!Rd(kDriver, drv) || !drv || !Rd(drv + 0x20, sw) || !Rd(drv + 0x1c, sh) || sw <= 0 || sh <= 0) return kPickVisible;
    return PickLayout(sw, sh, 100).nvis;
}
const char* PadLayoutName(int layout) { return layout == 1 ? "Xbox" : layout == 2 ? "DualSense" : "Steam"; }
void OverlaySetSlotLabels(int count, const POINT* centers)
{
    for (int k = 0; k < count && k < 5; ++k) { g_slotX[k].store(centers[k].x); g_slotY[k].store(centers[k].y); }
    g_slotCount.store(count);
}
void OverlaySetPotionLabels(int who, int count, const POINT* centers)
{
    if (who < 1 || who > 2) return;
    for (int k = 0; k < count && k < 3; ++k) { g_potX[who][k].store(centers[k].x); g_potY[who][k].store(centers[k].y); }
    g_potCount[who].store(count);
    g_potAt[who].store(GetTickCount());
}
void OverlaySetSlotLabels2(int count, const POINT* centers)
{
    for (int k = 0; k < count && k < 5; ++k) { g_slot2X[k].store(centers[k].x); g_slot2Y[k].store(centers[k].y); }
    g_slot2Count.store(count);
    g_slot2At.store(GetTickCount());
}
void OverlaySetGrid(int k, bool show, LONG x, LONG y, int cols, int rows)
{
    if (k < 0 || k > 3) return;
    g_gridX[k].store(x); g_gridY[k].store(y); g_gridC[k].store(cols); g_gridR[k].store(rows); g_gridOn[k].store(show);
}
void OverlaySetMarker(bool show, LONG x, LONG y, int icon)
{
    g_markX.store(x); g_markY.store(y); g_markIcon.store(icon); g_markOn.store(show);
}
void OverlaySetMarkerGround(int who, bool show, LONG x, LONG y)
{
    if (who < 1 || who > 2) return;
    g_gndX[who].store(x); g_gndY[who].store(y); g_gndOn[who].store(show);
}
void OverlaySetMarker2(bool show, LONG x, LONG y, int icon)
{
    g_mark2X.store(x); g_mark2Y.store(y); g_mark2Icon.store(icon); g_mark2On.store(show);
}
void OverlaySetTwoPlayers(bool on) { g_twoPlayers.store(on); }
void OverlaySetCursor2Num(bool on, LONG x, LONG y)
{
    g_c2X.store(x); g_c2Y.store(y); g_c2At.store(GetTickCount()); g_c2On.store(on);
}
void OverlaySetPointers(bool on, bool p1Show, LONG x1, LONG y1, LONG x2, LONG y2, int owner)
{
    g_ptr1X.store(x1); g_ptr1Y.store(y1); g_ptr2X.store(x2); g_ptr2Y.store(y2); g_ptrOwner.store(owner);
    g_ptr1Show.store(p1Show); g_ptrOn.store(on);
}

// Dal ciclo principale: aggancia EndScene quando il device esiste e scrive nel log l'esito.
void OverlayTick(DWORD now)
{
    if (!g_cfg.overlayEnabled || !MemoryReady()) return;
    if (now - g_lastLog > 10000 && g_slot) {
        g_lastLog = now;
        LONG d = g_draws.exchange(0), e = g_faults.load();
        if (d || e) Log("Overlay: %ld fotogrammi con la scritta negli ultimi 10 s, eccezioni %ld", d, e);
    }
    DWORD drv = 0, dev = 0, vtbl = 0;
    if (!Rd(kDriver, drv) || !drv || !Rd(drv + 0xcc, dev) || !dev || !Rd(dev, vtbl) || !vtbl) return;
    void** slot = reinterpret_cast<void**>(static_cast<uintptr_t>(vtbl + 0x18));
    void* cur = nullptr;
    if (!Rd(vtbl + 0x18, cur) || cur == reinterpret_cast<void*>(&OverlayEndScene)) return;
    if (g_slot && g_slot != slot) Log("Overlay: il device ha un vtable nuovo (0x%08lX), aggancio di nuovo", vtbl);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        Log("Overlay: VirtualProtect sul vtable del device fallita (%lu)", GetLastError());
        g_cfg.overlayEnabled = false;
        return;
    }
    g_origEndScene = reinterpret_cast<EndSceneFn>(cur);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&OverlayEndScene));
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_slot = slot;
    Log("Overlay: EndScene del device 0x%08lX agganciato (vtable 0x%08lX, originale %p), font %p",
        dev, vtbl, cur, GameFont());
}
