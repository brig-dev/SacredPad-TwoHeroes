// Memory.cpp - lettura della memoria di Sacred.exe (fase 2, diagnostica).
// Linguaggio: C++17, MSVC, Win32 (x86).
//
// Solo lettura, sempre tramite ReadProcessMemory sul proprio processo: se un
// puntatore non e' valido la lettura fallisce invece di mandare in crash il
// gioco. Gli indirizzi vengono dall'analisi statica di Sacred.exe 2.28 (hash
// in SacredPad_progetto.md, sezione 3.4); all'avvio si controlla che i vtable
// attesi siano al loro posto, altrimenti la lettura resta spenta.
//
//   0xad4bc0  gestore degli oggetti: [+4] inizio, [+8] fine del vettore di
//             puntatori agli oggetti, indicizzato per id ([obj+0xc] = id)
//   0xad5e78  cEngine: [+0x48] (word) indice della vista, [+8+i*4] viste
//   cWorldView0: [+0x14],[+0x18] camera (int), [+0xb60] scala (float)
//   0xcdb99c  finestra: [+0x1c] altezza, [+0x20] larghezza (word)
//   0xad6708, 0xad6710, 0xad6714, 0xad6720, 0xad6724  costanti della
//             proiezione isometrica (float)
//   oggetti:  [+0] vtable, [+0xc] id, [+0x1c],[+0x20] posizione (int),
//             [+0x24] byte (livello?), [+0x150] parte bassa 6 = morto (0.9.6; [+0x120]
//             vale 1 per i cadaveri ma anche per creature vive),
//             [+0x1f0] tipo: 1 eroe, 2 bestia ostile (lupi), 3 NPC (anche il
//             compagno e i mercanti), 6 animale innocuo, 7 soldato/guardia,
//             12 umanoide ostile (elfi, briganti, boss), 14 elfi ostili fuori
//             dalla caverna. Verificato con F7 il 22/10/2026 nel tutorial
//             dell'Elfo scuro; per questo gli ostili sono "tutti tranne i neutri".
//   0x182db68 dati del giocatore: [+0x14] id dell'oggetto eroe (funzione 0x604380)
//   vtable:   cCreature 0x890184, cCreatureHero 0x890250, cWorldView0 0x8910f8,
//             cEngine 0x890f34. L'eroe nel vettore e' un cCreature; cCreatureHero
//             e' un oggetto separato con i dati del personaggio (0.6.0: eroe assente)
//
// Da schermo a mondo (cWorldView0, metodo 0x62a3b0):
//   u = (sx - W/2) * k + camX,  v = (sy - H/2) * k + camY
//   X = (u*c24 - v*c20) * c08,  Y = (v*c10 - u*c14) * c08
// Qui serve l'inverso, da mondo a schermo.

#include "SacredPad.h"
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>
#include <cstdio>
#include <cstring>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdlib>
#include <functional>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <atomic>

namespace {

constexpr DWORD kObjMgr     = 0x00ad4bc0;
constexpr DWORD kPlayer     = 0x0182db68;
constexpr DWORD kEngine     = 0x00ad5e78;
constexpr DWORD kWindow     = 0x00cdb99c;
constexpr DWORD kC08 = 0x00ad6708, kC10 = 0x00ad6710, kC14 = 0x00ad6714, kC20 = 0x00ad6720, kC24 = 0x00ad6724;
constexpr DWORD kVtCreature = 0x00890184;
constexpr DWORD kVtHero     = 0x00890250;
constexpr DWORD kVtView0    = 0x008910f8;
constexpr DWORD kVtEngine   = 0x00890f34;
constexpr DWORD kVtChest    = 0x00890bb0;   // forzieri e bauli
constexpr DWORD kVtObject3D = 0x0088fcfc;   // oggetti fissi generici, porte comprese
constexpr DWORD kDoorClosed = 0xAB;         // +0x64 di una porta chiusa (0xAA aperta), 0.10.5
// La posizione di una porta e' alla base del muro, circa 130 pixel sotto la porta disegnata (porta
// della casa del 23/10 alle 10:18): per direzione e clic si usa un punto piu' in alto (0.10.6).
constexpr float kDoorLift    = 0.09f;       // frazione dell'altezza della finestra
constexpr float kDoorBetween = 0.06f;       // porta "in mezzo": entro questa distanza dalla linea eroe-bersaglio
constexpr DWORD kUIManager  = 0x017edb3c;   // puntatore al cUI_Manager (funzione 0x756c30)
// Oggetti a terra che si raccolgono con un clic (classi da RTTI, verificate con F7 il 22/10/2026:
// cArmor3D e cItemUpgrade; le altre per analogia).
constexpr DWORD kVtItems[] = {
    0x00890a64,   // cArmor3D
    0x00890b60,   // cBsItem3D
    0x00890c00,   // cPotion3D
    0x00890c50,   // cRing3D
    0x00890ca0,   // cScroll3D
    0x00890cf0,   // cShield3D
    0x00890d40,   // cSpecial3D
    0x00890d90,   // cItemUpgrade (arti di combattimento)
    0x00890de0,   // cWeapon3D
};
bool IsItemVt(DWORD vt) { for (DWORD v : kVtItems) if (v == vt) return true; return false; }

bool g_ok = false;          // vtable verificati: la lettura e' attiva
bool g_checked = false;

template <class T>
bool Rd(DWORD addr, T& out)
{
    SIZE_T n = 0;
    return addr >= 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)),
                             &out, sizeof(T), &n) && n == sizeof(T);
}

bool RdBuf(DWORD addr, void* out, size_t size)
{
    SIZE_T n = 0;
    return addr >= 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)),
                             out, size, &n) && n == size;
}

// Eroe locale visto dalla mod (0.40.0): con lo scambio attivo [player+0x14] vale 2, ma per tutto il resto del codice
// "l'eroe" resta l'eroe 1 (id originale).
std::atomic<DWORD> g_swapOrig{ 0 };
std::atomic<DWORD> g_saveOrig{ 0 };
std::atomic<DWORD> g_xpPending{ 0 };    // 0.45.3: esperienza dell'eroe 2 in attesa mentre e' l'eroe locale
// 0.45.4: l'eroe 2 e' l'eroe "locale" (scambio con Home appena cominciato): da controllare anche nel thread del motore, al
// momento della scrittura (prova del 28/10, 14:22:46: Home premuto 26 ms dopo la decisione di salire di livello)
bool Hero2IsLocal() { DWORD p = 0, id = 0; return Rd(0x0182db68, p) && p && Rd(p + 0x14, id) && id == 2; }     // 0.45.2: eroe locale vero mentre il salvataggio dell'eroe 2 tiene [player+0x14] = 2
bool RdHero(DWORD player, DWORD& id)
{
    const DWORD so = g_saveOrig.load();
    if (so) { id = so; return true; }
    const DWORD o = g_swapOrig.load();
    if (o) { id = o; return true; }
    return Rd(player + 0x14, id);
}

// Il codice di Sacred.exe deve contenere i vtable nei punti attesi (versione 2.28).
bool CheckExe()
{
    DWORD v = 0;
    // Il primo elemento di ciascun vtable e' un puntatore a codice in .text.
    for (DWORD vt : { kVtCreature, kVtHero, kVtView0, kVtEngine }) {
        if (!Rd(vt, v) || v < 0x401000 || v >= 0x88f000) return false;
    }
    // Controllo piu' stretto: primi metodi noti dall'analisi statica.
    DWORD a = 0, b = 0, c = 0;
    return Rd(kVtCreature, a) && a == 0x553480 &&
           Rd(kVtView0, b) && b == 0x6330e0 &&
           Rd(kVtEngine, c) && c == 0x618410;
}

struct View {
    float k = 0, c08 = 0, c10 = 0, c14 = 0, c20 = 0, c24 = 0;   // 0.53.6: inizializzati (analisi statica di MSVC)
    int   camX = 0, camY = 0;
    int   w = 0, h = 0;
    DWORD view = 0;      // cWorldView0 attivo
};

bool ReadView(View& v)
{
    DWORD engine = 0, eng_vt = 0, view = 0, view_vt = 0, win = 0;
    WORD  idx = 0, ww = 0, wh = 0;
    if (!Rd(kEngine, engine) || !Rd(engine, eng_vt) || eng_vt != kVtEngine) return false;
    if (!Rd(engine + 0x48, idx) || idx > 15) return false;
    if (!Rd(engine + 8 + idx * 4u, view) || !Rd(view, view_vt) || view_vt != kVtView0) return false;
    if (!Rd(view + 0x14, v.camX) || !Rd(view + 0x18, v.camY) || !Rd(view + 0xb60, v.k)) return false;
    if (!Rd(kWindow, win) || !Rd(win + 0x1c, wh) || !Rd(win + 0x20, ww)) return false;
    if (!Rd(kC08, v.c08) || !Rd(kC10, v.c10) || !Rd(kC14, v.c14) || !Rd(kC20, v.c20) || !Rd(kC24, v.c24)) return false;
    v.w = ww; v.h = wh;
    v.view = view;
    return v.k != 0.0f && v.c08 != 0.0f && v.w > 0 && v.h > 0;
}

// Da mondo a schermo (coordinate client), inverso della funzione del gioco.
bool WorldToScreen(const View& v, int X, int Y, float& sx, float& sy)
{
    float xp = X / v.c08, yp = Y / v.c08;
    float det = v.c24 * v.c10 - v.c20 * v.c14;
    if (std::fabs(det) < 1e-9f) return false;
    float u = (v.c10 * xp + v.c20 * yp) / det;
    float w = (v.c14 * xp + v.c24 * yp) / det;
    sx = (u - v.camX) / v.k + (v.w >> 1);
    sy = (w - v.camY) / v.k + (v.h >> 1);
    return true;
}

struct Obj {
    DWORD addr, vt, id;
    int   x, y;
    BYTE  z;
};

// Compagno al seguito dell'eroe (l'elfo silvano del tutorial): [+0x258] bit 0x100
// e [+0x164] = 3 solo per lui nei dump del 21-22/10/2026. Non e' un NPC con cui parlare.
bool IsFollower(DWORD addr)
{
    DWORD a = 0, b = 0;
    return (Rd(addr + 0x258, a) && (a & 0x100)) || (Rd(addr + 0x164, b) && b == 3);
}

// Tipo e stato (letti a parte, solo per le creature che interessano).
// Morte (0.9.6): parte bassa di +0x150 = 6. Su 182 dump di creature (log dal 22 al 23/10/2026)
// vale 6 per tutti i cadaveri e i nemici che stanno morendo e per nessuna creatura viva.
// Fino alla 0.9.5 si usava +0x120, che vale 1 anche per creature vive (eroe, compagna ferma,
// il nemico 699 in combattimento) e passa a 1 qualche istante dopo la morte.
bool ReadKind(DWORD addr, int& cls, bool& dead)
{
    DWORD c = 0, st = 0;
    if (!Rd(addr + 0x1f0, c) || !Rd(addr + 0x150, st)) return false;
    cls = static_cast<int>(c);
    dead = ((st & 0xffff) == 6);
    return true;
}

// Legge tutte le creature (vtable cCreature o cCreatureHero) dal vettore degli oggetti.
bool ReadCreatures(std::vector<Obj>& out, Obj& hero, bool& haveHero)
{
    out.clear();
    haveHero = false;
    DWORD beg = 0, end = 0;
    if (!Rd(kObjMgr, beg)) return false;           // beg = gestore
    DWORD mgr = beg;
    if (!Rd(mgr + 4, beg) || !Rd(mgr + 8, end) || end < beg) return false;
    DWORD count = (end - beg) / 4;
    if (count == 0 || count > 200000) return false;
    std::vector<DWORD> ptrs(count);
    if (!RdBuf(beg, ptrs.data(), count * 4)) return false;
    DWORD player = 0, heroId = 0xffffffffu;
    if (Rd(kPlayer, player) && player) RdHero(player, heroId);
    for (DWORD p : ptrs) {
        if (!p) continue;
        DWORD hdr[10];                              // 0x00..0x27
        if (!RdBuf(p, hdr, sizeof(hdr))) continue;
        if (hdr[0] != kVtCreature && hdr[0] != kVtHero) continue;
        Obj o{ p, hdr[0], hdr[3], static_cast<int>(hdr[7]), static_cast<int>(hdr[8]),
               static_cast<BYTE>(hdr[9] & 0xff) };
        if (hdr[3] == heroId && !haveHero) { hero = o; haveHero = true; }
        else out.push_back(o);
    }
    return true;
}

// Tutti gli oggetti del vettore (qualsiasi classe), con posizione a +0x1c/+0x20.
bool ReadAllObjects(std::vector<Obj>& out)
{
    out.clear();
    DWORD mgr = 0, beg = 0, end = 0;
    if (!Rd(kObjMgr, mgr) || !Rd(mgr + 4, beg) || !Rd(mgr + 8, end) || end < beg) return false;
    DWORD count = (end - beg) / 4;
    if (count == 0 || count > 200000) return false;
    std::vector<DWORD> ptrs(count);
    if (!RdBuf(beg, ptrs.data(), count * 4)) return false;
    for (DWORD p : ptrs) {
        if (!p) continue;
        DWORD hdr[10];
        if (!RdBuf(p, hdr, sizeof(hdr))) continue;
        if (hdr[0] < 0x88f000 || hdr[0] >= 0x8e6000) continue;      // vtable in .rdata di Sacred.exe
        out.push_back(Obj{ p, hdr[0], hdr[3], static_cast<int>(hdr[7]), static_cast<int>(hdr[8]),
                           static_cast<BYTE>(hdr[9] & 0xff) });
    }
    return true;
}

// Nome della classe dal RTTI di MSVC: [vtable-4] -> COL, [COL+12] -> TypeDescriptor, nome a +8.
void ClassName(DWORD vt, char* out, size_t size)
{
    DWORD col = 0, td = 0;
    char raw[64] = {};
    if (Rd(vt - 4, col) && Rd(col + 12, td) && RdBuf(td + 8, raw, sizeof(raw) - 1)) {
        raw[sizeof(raw) - 1] = 0;
        const char* s = raw;
        if (strncmp(s, ".?AV", 4) == 0) s += 4;
        size_t n = 0;
        while (s[n] && s[n] != '@' && n + 1 < size) { out[n] = s[n]; ++n; }
        out[n] = 0;
        if (n) return;
    }
    _snprintf_s(out, size, _TRUNCATE, "?");
}

DWORD g_lastDump = 0;
std::vector<DWORD> g_dumped;    // id delle creature gia' scaricate nel log

void DumpObject(const Obj& o, int size = 0x200)
{
    BYTE buf[0x400];
    if (size > 0x400) size = 0x400;
    if (!RdBuf(o.addr, buf, size)) return;
    char name[64];
    ClassName(o.vt, name, sizeof(name));
    Log("  dump id %lu (%s) @%08lX:", o.id, name, o.addr);
    char line[200];
    for (int row = 0; row < size; row += 32) {
        int n = wsprintfA(line, "    +%03X", row);
        for (int i = 0; i < 32; i += 4)
            n += wsprintfA(line + n, " %08lX", *reinterpret_cast<DWORD*>(buf + row + i));
        Log("%s", line);
    }
}

} // namespace

// 0.56.13: prove del 03-04/11 sul portatile (disegno software: D3DImm + D3D10Warp, le superfici stanno nella memoria del processo).
// Lo heap 1 si rovinava subito dopo l'apertura del menu Esc (0.56.5-0.56.12, anche con Mouse2=false) o della schermata Controller
// (0.55.68 il 04/11 alle 10:16, dopo 15 menu Esc senza danni), e il gioco cadeva poi all'uscita (ntdll+4E728). HeapFindBad (04/11
// alle 10:05): il blocco prima del danno e' di 131072 byte, una pagina 256x256 a 16 bit con pixel 0FFF (bianco trasparente) e
// Fxxx, e dopo la sua fine ancora pixel: una pagina dei caratteri dei font TrueType del gioco. cFontTTF (0x64b9c0) disegna ogni
// lettera con GDI, prende un posto nella pagina (0x662500) e la copia con 0x64c080 (stdcall: descrizione della superficie
// bloccata, sorgente, posto {x, y}, rettangolo {x, y, word larghezza a +8, word altezza a +0xc}; ret 0x10) senza controllare i
// bordi della pagina. Qui la copia passa da FontGlyphCopyHook, che taglia la lettera alla pagina (dwHeight +8, dwWidth +0xc
// della DDSURFACEDESC2) e lo scrive nel log: se il danno era questo, la riga nel log lo conferma.
bool PatchCode(DWORD addr, const BYTE* bytes, size_t n);   // piu' sotto, con il gancio del salvataggio
// Il secondo tipo di font (vtable 0x891268) ha una copia uguale, 0x64ee40 (chiamata a 0x64eb78 e 0x6525d8): stesso controllo.
typedef void(__stdcall* GlyphCopyFn)(BYTE* dst, BYTE* src, int* pos, BYTE* rect);
static std::atomic<int> g_glyphLogged{ 0 };
static void GlyphCopyChecked(GlyphCopyFn orig, BYTE* dst, BYTE* src, int* pos, BYTE* rect)
{
    std::atomic<int>& logged = g_glyphLogged;
    DWORD H = 0, W = 0; int x = 0, y = 0; WORD w = 0, h = 0;
    if (!dst || !pos || !rect || !Rd(reinterpret_cast<DWORD>(dst) + 8, H) || !Rd(reinterpret_cast<DWORD>(dst) + 0xc, W) ||
        !Rd(reinterpret_cast<DWORD>(pos), x) || !Rd(reinterpret_cast<DWORD>(pos) + 4, y) ||
        !Rd(reinterpret_cast<DWORD>(rect) + 8, w) || !Rd(reinterpret_cast<DWORD>(rect) + 0xc, h)) { orig(dst, src, pos, rect); return; }
    if (x >= 0 && y >= 0 && x + w <= static_cast<int>(W) && y + h <= static_cast<int>(H)) { orig(dst, src, pos, rect); return; }
    if (logged.fetch_add(1) < 20)
        Log("FONT: ATTENZIONE: lettera fuori dalla pagina dei caratteri: posto (%d,%d) %ux%u, pagina %lux%lu: tagliata al bordo", x, y, w, h, W, H);
    if (x < 0 || y < 0 || x >= static_cast<int>(W) || y >= static_cast<int>(H)) return;
    const WORD cw = static_cast<WORD>((std::min)(static_cast<int>(w), static_cast<int>(W) - x));
    const WORD ch = static_cast<WORD>((std::min)(static_cast<int>(h), static_cast<int>(H) - y));
    WORD* pw = reinterpret_cast<WORD*>(rect + 8); WORD* ph = reinterpret_cast<WORD*>(rect + 0xc);
    *pw = cw; *ph = ch;
    orig(dst, src, pos, rect);
    *pw = w; *ph = h;
}
static void __stdcall FontGlyphCopyHook(BYTE* dst, BYTE* src, int* pos, BYTE* rect) { GlyphCopyChecked(reinterpret_cast<GlyphCopyFn>(0x0064c080), dst, src, pos, rect); }
static void __stdcall FontGlyphCopyHook2(BYTE* dst, BYTE* src, int* pos, BYTE* rect) { GlyphCopyChecked(reinterpret_cast<GlyphCopyFn>(0x0064ee40), dst, src, pos, rect); }
static void InstallGlyphCopyHook()
{
    struct Site { DWORD call, copy; void* hook; };
    const Site sites[3] = { { 0x0064be3a, 0x0064c080, reinterpret_cast<void*>(&FontGlyphCopyHook) },
                            { 0x0064eb78, 0x0064ee40, reinterpret_cast<void*>(&FontGlyphCopyHook2) },
                            { 0x006525d8, 0x0064ee40, reinterpret_cast<void*>(&FontGlyphCopyHook2) } };
    int n = 0;
    for (const Site& t : sites) {
        BYTE op = 0; LONG rel = 0;
        if (!Rd(t.call, op) || !Rd(t.call + 1, rel) || op != 0xE8 || t.call + 5 + static_cast<DWORD>(rel) != t.copy) {
            Log("FONT: chiamata della copia delle lettere inattesa a %08lX (%02X): li' niente controllo dei bordi", t.call, op);
            continue;
        }
        const LONG mine = static_cast<LONG>(reinterpret_cast<uintptr_t>(t.hook)) - static_cast<LONG>(t.call + 5);
        if (PatchCode(t.call + 1, reinterpret_cast<const BYTE*>(&mine), 4)) ++n;
    }
    Log("FONT: copia delle lettere dei font con il controllo dei bordi della pagina (%d punti su 3)", n);
}

void MemoryInit()
{
    if (!g_cfg.memoryEnabled) { Log("Lettura della memoria disattivata (MemoryEnabled=false)"); return; }
    g_ok = CheckExe();
    g_checked = true;
    Log("Lettura della memoria: %s", g_ok ? "Sacred.exe riconosciuto, attiva"
                                          : "Sacred.exe non riconosciuto (vtable diversi), disattivata");
    if (g_ok) InstallGlyphCopyHook();       // 0.56.13
}

bool MemoryReady() { return g_ok; }

static bool HasQuestIcon(DWORD addr);
static void DiagNearRects(const View& v, DWORD heroId, float hx, float hy);

// Diagnostica periodica: eroe, camera, proiezione e creature vicine.
void MemoryDiagTick(DWORD now)
{
    if (!g_ok || !g_cfg.memoryDiag) return;
    if (now - g_lastDump < static_cast<DWORD>(g_cfg.memoryDiagMs)) return;
    g_lastDump = now;

    View v{};
    if (!ReadView(v)) { Log("MEM: vista non leggibile (menu o caricamento?)"); return; }
    std::vector<Obj> cr;
    Obj hero{};
    bool haveHero = false;
    if (!ReadCreatures(cr, hero, haveHero)) { Log("MEM: vettore degli oggetti non leggibile"); return; }
    Log("MEM: finestra %dx%d, camera (%d,%d), scala %.4f, creature %u, eroe %s",
        v.w, v.h, v.camX, v.camY, v.k, static_cast<unsigned>(cr.size()), haveHero ? "trovato" : "assente");
    if (!haveHero) return;
    float hx = 0, hy = 0;
    WorldToScreen(v, hero.x, hero.y, hx, hy);
    Log("MEM: eroe id %lu pos (%d,%d,%u) -> schermo (%.0f,%.0f)", hero.id, hero.x, hero.y, hero.z, hx, hy);

    std::sort(cr.begin(), cr.end(), [&](const Obj& a, const Obj& b) {
        double da = double(a.x - hero.x) * (a.x - hero.x) + double(a.y - hero.y) * (a.y - hero.y);
        double db = double(b.x - hero.x) * (b.x - hero.x) + double(b.y - hero.y) * (b.y - hero.y);
        return da < db;
    });
    int shown = 0;
    for (const Obj& o : cr) {
        float sx = 0, sy = 0;
        WorldToScreen(v, o.x, o.y, sx, sy);
        if (sx < -200 || sy < -200 || sx > v.w + 200 || sy > v.h + 200) continue;   // fuori schermo
        int cls = 0; bool dead = false;
        ReadKind(o.addr, cls, dead);
        static unsigned seen = 0;
        if (cls >= 0 && cls < 32 && !((seen >> cls) & 1u)) {
            seen |= 1u << cls;
            Log("MEM: primo incontro con il tipo di creatura %d (id %lu, %s)", cls, o.id,
                (g_cfg.hostileMask >> cls) & 1u ? "considerato ostile" : "non ostile");
        }
        Log("MEM:   id %lu tipo %d%s%s dist %.0f -> schermo (%.0f,%.0f)", o.id, cls, dead ? " morto" : "",
            HasQuestIcon(o.addr) ? " con icona" : "",
            std::sqrt(double(o.x - hero.x) * (o.x - hero.x) + double(o.y - hero.y) * (o.y - hero.y)), sx, sy);
        if (++shown >= 6) break;
    }
    DiagNearRects(v, hero.id, hx, hy);
}

// Alla pressione di A: creatura piu' vicina al punto del clic (coordinate client).
void MemoryDiagClick(LONG cx, LONG cy)
{
    if (!g_ok || !g_cfg.memoryDiag) return;
    View v{};
    std::vector<Obj> cr;
    Obj hero{};
    bool haveHero = false;
    if (!ReadView(v) || !ReadCreatures(cr, hero, haveHero)) return;
    const Obj* best = nullptr;
    float bestD = 1e9f, bx = 0, by = 0;
    for (const Obj& o : cr) {
        float sx = 0, sy = 0;
        if (!WorldToScreen(v, o.x, o.y, sx, sy)) continue;
        float d = std::sqrt((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy));
        if (d < bestD) { bestD = d; best = &o; bx = sx; by = sy; }
    }
    if (best)
        Log("MEM: clic in (%ld,%ld): creatura piu' vicina id %lu a (%.0f,%.0f), %.0f px", cx, cy, best->id, bx, by, bestD);
}

// F7 (tasto fisico): creatura piu' vicina al mouse vero, con il contenuto
// completo, per associare a mano ogni dump a un tipo (nemico, NPC, cadavere).
void MemoryDiagProbe(HWND hwnd)
{
    static bool prev = false;
    if (!g_ok || !g_cfg.memoryDiag) return;
    bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    static int n = 0;
    ++n;
    POINT p;
    if (!RealGetCursorPos(&p)) return;
    ScreenToClient(hwnd, &p);
    View v{};
    std::vector<Obj> cr;
    Obj hero{};
    bool haveHero = false;
    if (!ReadView(v) || !ReadCreatures(cr, hero, haveHero)) { Log("MEM: F7 n.%d: memoria non leggibile", n); return; }
    // Tutti gli oggetti del mondo, non solo le creature: i tre piu' vicini al mouse
    // con il nome della classe (RTTI), e il contenuto del piu' vicino.
    std::vector<Obj> all;
    ReadAllObjects(all);
    struct Near { const Obj* o; float d, sx, sy; };
    std::vector<Near> nears;
    for (const Obj& o : all) {
        float sx = 0, sy = 0;
        if (!WorldToScreen(v, o.x, o.y, sx, sy)) continue;
        if (sx < -100 || sy < -100 || sx > v.w + 100 || sy > v.h + 100) continue;
        float d = std::sqrt((sx - p.x) * (sx - p.x) + (sy - p.y) * (sy - p.y));
        nears.push_back({ &o, d, sx, sy });
    }
    if (nears.empty()) { Log("MEM: F7 n.%d: nessun oggetto", n); return; }
    std::sort(nears.begin(), nears.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
    Log("MEM: F7 n.%d: mouse (%ld,%ld)", n, p.x, p.y);
    for (size_t i = 0; i < nears.size() && i < 3; ++i) {
        const Obj& o = *nears[i].o;
        char name[64];
        ClassName(o.vt, name, sizeof(name));
        int cls = -1; bool dead = false;
        if (o.vt == kVtCreature) ReadKind(o.addr, cls, dead);
        Log("MEM:   %u) %s vt %08lX id %lu a (%.0f,%.0f), %.0f px%s", static_cast<unsigned>(i + 1), name, o.vt, o.id,
            nears[i].sx, nears[i].sy, nears[i].d, o.vt == kVtCreature ? (dead ? " (creatura morta)" : " (creatura)") : "");
    }
    // Dal 0.9.3 tutti e tre: il piu' vicino al mouse spesso non e' quello cercato
    // (il 22/10 un oggetto a terra accanto al baule giallo).
    for (size_t i = 0; i < nears.size() && i < 3; ++i) DumpObject(*nears[i].o, i == 0 ? 0x400 : 0x200);
}

// --- Aggancio del bersaglio (fase 2) -------------------------------------------
namespace {
bool InMask(unsigned mask, int cls) { return cls >= 0 && cls < 32 && (mask >> cls) & 1u; }

// Forzieri gia' raggiunti dall'eroe dopo essere stati scelti con A (0.9.3): il campo +0x64
// non dice in modo affidabile se un forziere e' chiuso (il baule giallo del 22/10 veniva
// scartato da chiuso), quindi la mod ricorda quelli aperti. Id e posizione, perche' gli id
// degli oggetti potrebbero essere riusati.
struct Used { DWORD id; int x, y; };
std::vector<Used> g_used;
bool IsUsed(const Obj& o)
{
    for (const Used& u : g_used) if (u.id == o.id && u.x == o.x && u.y == o.y) return true;
    return false;
}
}

// 0.55.38: oggetti fissi della mappa (trigger degli script: nascondigli magici, portali) appena scelti con A dal pad 1: per 6 s
// non sono piu' candidati, cosi' il colpo successivo di A va all'oggetto che il nascondiglio lascia a terra.
struct FixedUsed { DWORD id, until; };
std::vector<FixedUsed> g_fixedUsed;
bool FixedRecentlyUsed(DWORD id)
{
    const DWORD now = GetTickCount();
    for (const FixedUsed& u : g_fixedUsed) if (u.id == id && static_cast<LONG>(now - u.until) < 0) return true;
    return false;
}
void MemoryMarkFixedUsed(DWORD id)
{
    const DWORD now = GetTickCount();
    g_fixedUsed.erase(std::remove_if(g_fixedUsed.begin(), g_fixedUsed.end(),
                                     [&](const FixedUsed& u) { return u.id == id || static_cast<LONG>(now - u.until) >= 0; }), g_fixedUsed.end());
    g_fixedUsed.push_back(FixedUsed{ id, now + 6000 });
}

void MemoryUsedReset() { g_used.clear(); g_fixedUsed.clear(); }

bool MemoryIsChest(const Target& t) { return t.vt == kVtChest; }

void MemoryMarkUsed(const Target& t)
{
    DWORD hdr[9];
    if (!t.addr || !RdBuf(t.addr, hdr, sizeof(hdr)) || hdr[0] != t.vt || hdr[3] != t.id) return;
    Obj o{ t.addr, hdr[0], hdr[3], static_cast<int>(hdr[7]), static_cast<int>(hdr[8]), 0 };
    if (IsUsed(o)) return;
    if (g_used.size() >= 512) g_used.erase(g_used.begin());
    g_used.push_back(Used{ o.id, o.x, o.y });
}

// Sceglie la creatura viva del tipo richiesto piu' adatta alla direzione
// (dx,dy) a partire da (ox,oy), coordinate client. Nel cono di LockAngle gradi
// entro range pixel vince la piu' vicina, con una penalita' per l'angolo; fuori
// dal cono vale solo se entro nearRadius pixel.
// follower (solo talk): 0 il compagno al seguito e' escluso, 1 vale solo nel cono
// della direzione, 2 vale ovunque; in ogni caso viene dopo tutti gli altri candidati
// (0.9.2: l'elfa con una missione per l'eroe era irraggiungibile con A). Dal 0.9.6 solo
// se sta accanto all'eroe (al seguito: 70-100 unita'): fermo e lontano, come l'elfa col
// "!" del saluto finale, e' un NPC come gli altri.
constexpr int kFollowerNear = 110;
// 0.56.8: bit di +0x200 come li legge il cursore del gioco (0x612160): 0x2000 mercante (MOUSE_TRADER), 0x4000 Maestro delle combo
// (MOUSE_COMBO), 0x1000 fabbro (MOUSE_BLACKSMITH), 0x04000000 stalliere (MOUSE_HORSETRADER). Fino alla 0.56.7 mancava 0x4000: il
// Maestro delle combo non era un servizio (per l'eroe 2 niente scambio dell'interfaccia: la sua finestra si apriva per l'eroe 1).
constexpr DWORD kServiceMask = 0x04007000u;   // +0x200: stalliere, mercante, Maestro delle combo, fabbro
constexpr float kGenericPenalty = 10000.0f;   // NPC generici dopo gli altri, compagno (100000) per ultimo
constexpr float kQuestBonus = 5000.0f;        // NPC con l'icona di missione prima di tutti gli altri candidati (fino alla 0.56.17)
// 0.56.18 (il tester, 04/11): "da solo non vede gli npc che non hanno nulla da dire... l'aggancio deve funzionare per mercante, fabbro,
// maestro e missioni (?! o !), gli npc normali non servono... se sei piu' vicino al mercante di barile o baule, apri il mercante...
// se esco dal mercante e mi sposto verso il barile apro il barile non il mercante". I PNG generici e quelli con il solo dialogo
// speciale senza icona non si agganciano piu'; quelli con l'icona (missioni e servizi) e quelli con un servizio (+0x200) valgono
// come gli altri candidati, con una lieve preferenza (kUsefulFactor) al posto del bonus di 5000 pixel che li faceva vincere sempre
// entro il raggio; il PNG appena usato (finestra o dialogo chiusi, MemoryNoteUsedNpc) conta tre volte la sua distanza per 2 s (0.56.19, il tester: "accorcialo a 2 secondi"; erano 6).
constexpr float kUsefulFactor = 0.9f;
std::atomic<DWORD> g_usedNpc{ 0 }, g_usedNpcUntil{ 0 };
constexpr DWORD kQuestTable = 0x00aaa564;     // tabella delle missioni per NPC, voci da 0x80 byte

// Icona di missione sopra la creatura (Ghidra, 23/10/2026): il metodo di disegno di cCreature
// (0x599ba0) disegna l'icona (0x409210) se +0x14 ha il bit 0x80000 oppure se la tabella delle
// missioni ha il bit 19 nella voce della creatura (0x428460: [[0xaaa564] + id*0x80], id = +0x10,
// valido da 1 a 0x7e5f).
// Porte (0.11.2, F7 del 23/10/2026 alle 13:31-13:32 e tabella dei tipi trovata con Ghidra):
// +0x10 di un oggetto e' il suo tipo; la tabella dei tipi di Sacred.exe sta a 0x8eb330, 5624
// voci da 0x44 byte {numero, nome "TYPE_..."} (0x1617 TYPE_OBJECT_DOOR_HOF03_B, 0x1618
// DOOR_HOF04_A, 0x1647 DOOR_DUNGEON02_B; 119 tipi di porta). +0x64 di una porta: 0 se non e'
// mai stata toccata dopo il caricamento, 0xAA aperta, 0xAB richiusa; la regola della 0.10.5
// (solo 0xAB) perdeva le porte chiuse mai usate, come la 1328 del mercante. Chiusa e mai
// toccata: +0x64 = 0. Nella 0.11.3 serviva anche il bit 0x20000000 di +0x14, ma e' il segno
// "evidenziato sotto il cursore" (0x612160, 0.11.6): c'era solo perche' durante l'F7 il mouse
// stava sulla porta. Limite: una porta aperta in partenza e mai toccata conta come chiusa.
constexpr DWORD kTypeTable = 0x008eb330, kTypeCount = 5624, kTypeStride = 0x44;
static bool IsDoorType(DWORD type)
{
    static std::vector<DWORD> doors;
    static bool built = false;
    if (!built) {
        built = true;
        char name[0x40];
        for (DWORD i = 0; i < kTypeCount; ++i) {
            DWORD id = 0;
            if (!Rd(kTypeTable + i * kTypeStride, id) || !RdBuf(kTypeTable + i * kTypeStride + 4, name, sizeof(name))) break;
            name[sizeof(name) - 1] = 0;
            if (strncmp(name, "TYPE_OBJECT_", 12) == 0 && strstr(name, "DOOR")) doors.push_back(id);
        }
        Log("Porte: %u tipi trovati nella tabella dei tipi", static_cast<unsigned>(doors.size()));
    }
    return std::find(doors.begin(), doors.end(), type) != doors.end();
}

// Nome del tipo dalla tabella dei tipi, senza "TYPE_" (per il log). 0.55.38: separato da MemoryTypeName per gli oggetti delle
// missioni, che negli inventari sono solo il numero del tipo.
bool TypeNameById(DWORD type, char* out, size_t size)
{
    if (!g_ok) return false;
    char name[0x40];
    for (DWORD i = 0; i < kTypeCount; ++i) {
        DWORD id = 0;
        if (!Rd(kTypeTable + i * kTypeStride, id)) return false;
        if (id != type) continue;
        if (!RdBuf(kTypeTable + i * kTypeStride + 4, name, sizeof(name))) return false;
        name[sizeof(name) - 1] = 0;
        _snprintf_s(out, size, _TRUNCATE, "%s", strncmp(name, "TYPE_", 5) == 0 ? name + 5 : name);
        return true;
    }
    return false;
}

// Nome del tipo dell'oggetto (+0x10).
bool MemoryTypeName(DWORD addr, char* out, size_t size)
{
    DWORD type = 0;
    return g_ok && Rd(addr + 0x10, type) && TypeNameById(type, out, size);
}

static bool IsClosedDoor(DWORD addr, DWORD& state)
{
    DWORD type = 0, flags = 0;
    if (!Rd(addr + 0x10, type) || !IsDoorType(type) || !Rd(addr + 0x64, state)) return false;
    (void)flags;
    return state == kDoorClosed || state == 0;
}

// --- Scelta del gioco dell'oggetto sotto il clic (0.11.6, Ghidra) ---------------------------
// A ogni fotogramma 0x612160 chiama 0x627050 (this = cWorldView0 attivo) con il punto del
// cursore e salva l'id scelto in [0xcdcadc]+0x6c. 0x627050 scorre la lista dei rettangoli a
// schermo degli oggetti disegnati, [view+0x96b24]..[view+0x96b28], voci da 0x1c byte:
// +0 id (bit 31 = oggetto fisso della mappa), +4 x, +8 y (int, client), +0xc larghezza,
// +0xe altezza (short). Fra i rettangoli che contengono il punto prende la categoria migliore
// (metodo 0x24(1) dell'oggetto: 0 nemici, 2 oggetti a terra, forzieri e oggetti fissi, 3 NPC,
// porte e cObject3D; ordine 0,1,2,3, con Alt 2,1,0,3) e in quella il rettangolo con il
// centro piu' vicino. Per questo il clic sulla porta del mercante andava al forziere vicino.
struct PickRect { DWORD id; int x, y; short w, h; DWORD pad[3]; };
static_assert(sizeof(PickRect) == 0x1c, "voce della lista dei rettangoli");

static bool ReadPickRects(const View& v, std::vector<PickRect>& out)
{
    DWORD beg = 0, end = 0;
    if (!v.view || !Rd(v.view + 0x96b24, beg) || !Rd(v.view + 0x96b28, end) || end < beg) return false;
    const DWORD n = (end - beg) / sizeof(PickRect);
    if (n == 0 || n > 1000) return false;
    out.resize(n);
    return RdBuf(beg, out.data(), n * sizeof(PickRect));
}

static DWORD ObjectById(DWORD id);
static bool PickAt(LONG px, LONG py, DWORD& pick);

// 0.55.38: cio' che il gioco sceglierebbe sotto il punto (client), per il log dei clic in modalita' cursore: serve a riconoscere
// le cose che A non trova ancora (prova del 31/10 sera: nascondigli magici aperti solo in modalita' cursore).
bool MemoryPickDescribe(LONG x, LONG y, char* out, size_t size)
{
    out[0] = 0;
    DWORD id = 0;
    if (!g_ok || !PickAt(x, y, id) || !id) return false;
    if (id & 0x80000000u) {
        View v{};
        std::vector<PickRect> rs;
        int w = 0, h = 0;
        if (ReadView(v) && ReadPickRects(v, rs)) for (const PickRect& r : rs) if (r.id == id) { w = r.w; h = r.h; break; }
        _snprintf_s(out, size, _TRUNCATE, "oggetto fisso della mappa %dx%d id %lu (indice %lu)", w, h, id, id & 0xffffu);
        return true;
    }
    const DWORD obj = ObjectById(id);
    DWORD vt = 0;
    char cn[40] = "?", type[64] = "";
    if (obj && Rd(obj, vt)) ClassName(vt, cn, sizeof(cn));
    if (obj) MemoryTypeName(obj, type, sizeof(type));
    _snprintf_s(out, size, _TRUNCATE, "id %lu %s %s", id, cn, type);
    return true;
}

static DWORD ObjectById(DWORD id)
{
    DWORD mgr = 0, beg = 0, end = 0, obj = 0;
    if (!Rd(kObjMgr, mgr) || !mgr || !Rd(mgr + 4, beg) || !Rd(mgr + 8, end)) return 0;
    if (id >= (end - beg) / 4 || !Rd(beg + id * 4, obj)) return 0;
    return obj;
}

// 0.56.45: con MemoryDiag, i rettangoli del gioco vicini ai piedi dell'eroe 1 che non sono creature (oggetti fissi della mappa,
// forzieri, oggetti a terra, porte, altro), una riga quando l'elenco cambia. Prova del 05/11: alle 12:04 un portale aperto (foto)
// non era fra i candidati ne' di A ne' del portale automatico, e un nascondiglio magico nel bosco non veniva indicato: serve sapere
// che rettangolo hanno, se ne hanno uno.
static void DiagNearRects(const View& v, DWORD heroId, float hx, float hy)
{
    std::vector<PickRect> rs;
    if (!ReadPickRects(v, rs)) return;
    float fx = hx, fy = hy;
    for (const PickRect& r : rs)
        if (r.id == heroId && r.w > 0 && r.h > 0) { fx = r.x + r.w * 0.5f; fy = r.y + r.h * 0.95f; break; }
    struct NearR { const PickRect* r; float d; };
    std::vector<NearR> cand;
    for (const PickRect& r : rs) {
        if (r.w <= 0 || r.h <= 0 || r.id == heroId) continue;
        if (!(r.id & 0x80000000u)) {
            const DWORD obj = ObjectById(r.id);
            DWORD vt = 0;
            if (!obj || !Rd(obj, vt) || vt == kVtCreature || vt == kVtHero) continue;
        }
        const float bx = (std::max)(static_cast<float>(r.x), (std::min)(fx, static_cast<float>(r.x + r.w)));
        const float by = (std::max)(static_cast<float>(r.y), (std::min)(fy, static_cast<float>(r.y + r.h)));
        const float d = std::sqrt((bx - fx) * (bx - fx) + (by - fy) * (by - fy));   // dal bordo del rettangolo
        if (d <= 0.3f * v.h) cand.push_back(NearR{ &r, d });
    }
    std::sort(cand.begin(), cand.end(), [](const NearR& a, const NearR& b) { return a.d < b.d; });
    if (cand.size() > 8) cand.resize(8);
    DWORD key = static_cast<DWORD>(cand.size());
    for (const NearR& n : cand) key = key * 131u + n.r->id;
    static DWORD lastKey = 0;
    if (cand.empty() || key == lastKey) return;
    lastKey = key;
    char buf[1200];
    int len = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "MEM: vicino ai piedi (%.0f,%.0f), non creature:", fx, fy);
    for (const NearR& n : cand) {
        if (len < 0 || len >= static_cast<int>(sizeof(buf)) - 8) break;
        const PickRect& r = *n.r;
        char what[72] = "fisso";
        if (!(r.id & 0x80000000u)) {
            const DWORD obj = ObjectById(r.id);
            DWORD vt = 0;
            char cn[32] = "?", tn[40] = "";
            if (obj && Rd(obj, vt)) ClassName(vt, cn, sizeof(cn));
            if (obj) MemoryTypeName(obj, tn, sizeof(tn));
            _snprintf_s(what, sizeof(what), _TRUNCATE, "%s %s", cn, tn);
        }
        const int k = _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, " [%lu %s %dx%d in (%d,%d) a %.0f]", r.id, what, r.w, r.h, r.x, r.y, n.d);
        if (k < 0) break;
        len += k;
    }
    Log("%s", buf);
}

// 0.56.46: con MemoryDiag, l'oggetto fisso della mappa che il gioco sceglie sotto il cursore ([0xcdcadc]+0x6c, bit 31), una riga
// per oggetto ogni 10 s, con il suo rettangolo e i piedi dell'eroe 1. Prova del 05/11 (foto: "Nascondiglio magico" sotto il mouse
// nel bosco): quel nascondiglio non compariva mai fra i rettangoli vicini all'eroe ne' fra i candidati di A.
void DiagHoverFixed(DWORD now)
{
    DWORD in = 0, id = 0;
    if (!g_ok || !Rd(0x00cdcadc, in) || !in || !Rd(in + 0x6c, id) || !(id & 0x80000000u) || id == 0xffffffffu) return;
    static DWORD lastId = 0, lastAt = 0;
    if (id == lastId && static_cast<LONG>(now - lastAt) < 10000) return;
    lastId = id; lastAt = now;
    View v{};
    std::vector<PickRect> rs;
    if (!ReadView(v) || !ReadPickRects(v, rs)) return;
    DWORD pl = 0, loc = 0;
    float fx = -1, fy = -1;
    Rd(kPlayer, pl);
    if (pl) RdHero(pl, loc);
    const PickRect* hit = nullptr;
    for (const PickRect& r : rs) {
        if (r.id == id) hit = &r;
        if (loc && r.id == loc && r.w > 0 && r.h > 0) { fx = r.x + r.w * 0.5f; fy = r.y + r.h * 0.95f; }
    }
    LONG cx = 0, cy = 0;
    Rd(in + 4, cx); Rd(in + 8, cy);
    if (hit) Log("MEM: sotto il cursore l'oggetto fisso %lu (indice %lu) %dx%d in (%d,%d); piedi dell'eroe 1 in (%.0f,%.0f), %u rettangoli", id, id & 0xffffu,
                 hit->w, hit->h, hit->x, hit->y, fx, fy, static_cast<unsigned>(rs.size()));
    else Log("MEM: sotto il cursore l'oggetto fisso %lu (indice %lu), NON nella lista dei rettangoli (%u); piedi dell'eroe 1 in (%.0f,%.0f)", id, id & 0xffffu,
             static_cast<unsigned>(rs.size()), fx, fy);
}

// Categoria di 0x627050 (approssimata per le creature: ostili vive 0, le altre 3).
static int PickCategory(DWORD id)
{
    if (id & 0x80000000u) return 2;
    DWORD obj = ObjectById(id), vt = 0;
    if (!obj || !Rd(obj, vt)) return 3;
    if (vt == kVtChest || IsItemVt(vt)) return 2;
    if (vt == kVtCreature) {
        int cls = 0; bool dead = false;
        if (ReadKind(obj, cls, dead) && !dead && InMask(g_cfg.hostileMask, cls)) return 0;
    }
    return 3;
}

// Punto (client) in cui il gioco sceglierebbe l'oggetto id: il piu' vicino al centro del suo
// rettangolo fra quelli in cui la scelta cade su di lui.
bool MemoryPickPoint(DWORD id, LONG& x, LONG& y)
{
    if (!g_ok) return false;
    View v{};
    std::vector<PickRect> rs;
    if (!ReadView(v)) return false;
    if (!ReadPickRects(v, rs)) {
        static DWORD warned = 0;
        if (GetTickCount() - warned > 10000) { warned = GetTickCount(); Log("Clic: lista dei rettangoli del gioco non leggibile"); }
        return false;
    }
    const PickRect* me = nullptr;
    for (const PickRect& r : rs) if (r.id == id && r.w > 0 && r.h > 0) { me = &r; break; }
    if (!me) return false;
    // Solo i rettangoli che toccano il bersaglio contano; categoria letta una volta sola.
    struct Cand { const PickRect* r; int cat; };
    std::vector<Cand> cands;
    for (const PickRect& r : rs) {
        if (r.w <= 0 || r.h <= 0) continue;
        if (r.x > me->x + me->w || r.x + r.w < me->x || r.y > me->y + me->h || r.y + r.h < me->y) continue;
        cands.push_back(Cand{ &r, PickCategory(r.id) });
    }
    const int cx = me->x + me->w / 2, cy = me->y + me->h / 2;
    const int side = me->w < me->h ? me->w : me->h;
    const int step = side / 10 > 3 ? side / 10 : 3;
    long bestD = -1;
    for (int py = me->y; py <= me->y + me->h; py += step) {
        for (int px = me->x; px <= me->x + me->w; px += step) {
            int bestCat = 4; long bestR = 0; DWORD pick = 0;
            for (const Cand& n : cands) {
                const PickRect& r = *n.r;
                if (px < r.x || px > r.x + r.w || py < r.y || py > r.y + r.h) continue;
                const long dx = px - r.w / 2 - r.x, dy = py - r.h / 2 - r.y, d = dx * dx + dy * dy;
                if (n.cat < bestCat || (n.cat == bestCat && d < bestR)) { bestCat = n.cat; bestR = d; pick = r.id; }
            }
            if (pick != id) continue;
            const long d = long(px - cx) * (px - cx) + long(py - cy) * (py - cy);
            if (bestD < 0 || d < bestD) { bestD = d; x = px; y = py; }
        }
    }
    static DWORD lastId = 0xffffffffu;
    if (id != lastId) {
        lastId = id;
        if (bestD >= 0) Log("Clic: id %lu, rettangolo (%d,%d) %dx%d, punto scelto (%ld,%ld)", id, me->x, me->y, me->w, me->h, x, y);
        else            Log("Clic: id %lu, rettangolo (%d,%d) %dx%d coperto da altri oggetti in ogni punto", id, me->x, me->y, me->w, me->h);
    }
    return bestD >= 0;
}

// Rettangolo (client) dell'oggetto id nella lista del gioco del fotogramma corrente.
bool MemoryTargetRect(DWORD id, RECT& r)
{
    if (!g_ok) return false;
    View v{};
    std::vector<PickRect> rs;
    if (!ReadView(v) || !ReadPickRects(v, rs)) return false;
    for (const PickRect& p : rs) {
        if (p.id != id || p.w <= 0 || p.h <= 0) continue;
        r = RECT{ p.x, p.y, p.x + p.w, p.y + p.h };
        return true;
    }
    return false;
}

static bool DrawnIds(std::vector<DWORD>& ids)
{
    View v{};
    std::vector<PickRect> rs;
    if (!ReadView(v) || !ReadPickRects(v, rs)) return false;
    ids.clear();
    for (const PickRect& r : rs) ids.push_back(r.id);
    std::sort(ids.begin(), ids.end());
    return true;
}

// Oggetto che il gioco sceglierebbe nel punto (client), come 0x627050; 0 = nessuno.
static bool PickAt(LONG px, LONG py, DWORD& pick)
{
    View v{};
    std::vector<PickRect> rs;
    if (!ReadView(v) || !ReadPickRects(v, rs)) return false;
    int bestCat = 4; long bestR = 0;
    pick = 0;
    for (const PickRect& r : rs) {
        if (r.w <= 0 || r.h <= 0 || px < r.x || px > r.x + r.w || py < r.y || py > r.y + r.h) continue;
        const int cat = PickCategory(r.id);
        const long dx = px - r.w / 2 - r.x, dy = py - r.h / 2 - r.y, d = dx * dx + dy * dy;
        if (cat < bestCat || (cat == bestCat && d < bestR)) { bestCat = cat; bestR = d; pick = r.id; }
    }
    return true;
}

static bool HasQuestIcon(DWORD addr)
{
    DWORD flags = 0, id = 0, table = 0, entry = 0;
    if (Rd(addr + 0x14, flags) && (flags & 0x80000)) return true;
    if (!Rd(addr + 0x10, id) || id < 1 || id > 0x7e5f) return false;
    return Rd(kQuestTable, table) && table && Rd(table + id * 0x80, entry) && ((entry >> 19) & 1);
}
// Id degli oggetti disegnati nel fotogramma corrente (lista dei rettangoli del gioco), ordinati.
static bool DrawnIds(std::vector<DWORD>& ids);

void MemoryNoteUsedNpc(DWORD id)
{
    if (!id) return;
    g_usedNpc.store(id); g_usedNpcUntil.store(GetTickCount() + 2000);   // 0.56.19: 2 s (erano 6)
}

// 0.56.53: trigger della mappa vicini all'eroe 1, dalla tabella [0xab3538] (vedi TrigNearRefresh piu' sotto)
struct TrigNear { DWORD id; int wx, wy; BYTE z; DWORD textId, kind; };
static bool TrigNearCopy(std::vector<TrigNear>& out);
static bool TrigWorldPos(DWORD id, int& wx, int& wy);
static bool FixedLabel(DWORD id, std::string& out, DWORD* textId = nullptr);
bool MemoryFindTarget(bool talk, float ox, float oy, float dx, float dy, float range, float nearRadius, Target& out,
                      const DWORD* skip, int nSkip, int follower, bool aim, bool forHero2, bool allNpc)
{
    // aim (0.11.10, bersaglio a fuoco con lo stick): vince cio' che sta piu' dritto nella
    // direzione, a parita' il piu' vicino; niente bonus per l'icona ne' penalita' per gli NPC
    // generici e il compagno (con l'icona e la distanza vincevano NPC lontani, prova delle 15:28).
    if (!g_ok || !g_cfg.lockEnabled) return false;
    View v{};
    if (!ReadView(v)) return false;
    std::vector<DWORD> drawn;
    const bool drawnOk = talk && DrawnIds(drawn);
    // Nemici: solo creature. Interazione (talk): NPC, forzieri chiusi e oggetti a terra.
    std::vector<Obj> objs;
    Obj hero{};
    bool haveHero = false;
    if (talk) { if (!ReadAllObjects(objs)) return false; }
    else if (!ReadCreatures(objs, hero, haveHero)) return false;
    DWORD player = 0, heroId = 0xffffffffu;
    if (Rd(kPlayer, player) && player) RdHero(player, heroId);
    // 0.55.20: per l'eroe 2 i cavalli si scelgono (per comprarne uno dallo stalliere e salire sul suo), ma non quello dell'eroe 1
    DWORD h1Horse = 0, h2Horse = 0;
    if (forHero2 && heroId != 0xffffffffu) { const DWORD ha = ObjectById(heroId); if (ha) Rd(ha + 0x570, h1Horse); }
    // 0.55.23: e per il pad 1 niente cavallo dell'eroe 2 (prova delle 17:26: bersaglio a fuoco sul cavallo 61 della diavola)
    if (!forHero2 && heroId != 2) { const DWORD hb = ObjectById(2); if (hb) Rd(hb + 0x570, h2Horse); }
    int heroX = 0, heroY = 0;
    const bool heroPos = MemoryHeroPos(heroX, heroY);
    const unsigned mask = talk ? g_cfg.talkMask : g_cfg.hostileMask;
    const float dl = std::sqrt(dx * dx + dy * dy);
    const bool hasDir = dl > 0.1f;
    if (hasDir) { dx /= dl; dy /= dl; }
    const float cosLim = std::cos(g_cfg.lockAngle * 3.14159265f / 180.0f);
    float bestScore = 1e9f;
    bool found = false;
    struct DoorSeen { const Obj* o; float sx, sy, d; };
    DoorSeen doors[16];
    int nDoors = 0;
    for (const Obj& o : objs) {
        const bool creature = (o.vt == kVtCreature);
        // Porte chiuse (0.10.5): cObject3D con +0x64 = 0xAB. Verificato su due porte il 22-23/10
        // (1655 e 1144: 0xAB chiusa, 0xAA aperta); nessun altro oggetto visto aveva 0xAB.
        DWORD door64 = 0;
        const bool door = talk && o.vt == kVtObject3D && IsClosedDoor(o.addr, door64);
        // 0.55.22: anche per l'eroe 2 gli oggetti a terra (prima erano solo di B): A sceglie cio' che vuole raccogliere, come il pad 1
        if (!creature && !door && !(talk && (o.vt == kVtChest || IsItemVt(o.vt)))) continue;
        if (o.id == heroId || (forHero2 && o.id == 2)) continue;
        bool skipped = false;
        for (int k = 0; k < nSkip; ++k) if (skip[k] == o.id) { skipped = true; break; }
        if (skipped) continue;
        float sx = 0, sy = 0;
        if (!WorldToScreen(v, o.x, o.y, sx, sy)) continue;
        if (door) sy -= kDoorLift * v.h;
        if (sx < 0 || sy < 0 || sx > v.w || sy > v.h) continue;
        float vx = sx - ox, vy = sy - oy;
        float d = std::sqrt(vx * vx + vy * vy);
        if (d > range) continue;
        if (!creature && d < 12.0f) continue;               // sotto i piedi: probabilmente non a terra
        // 0.14.1: oggetti, forzieri e porte solo se il gioco li disegna in questo fotogramma (le monete
        // 1332-1337 del 24/10 alle 9:43 erano nel vettore ma non si vedevano: mano su posti vuoti).
        if (!creature && drawnOk && !std::binary_search(drawn.begin(), drawn.end(), o.id)) continue;
        if (door && nDoors < 16) doors[nDoors++] = DoorSeen{ &o, sx, sy, d };
        float score;
        bool inCone = true;
        if (!hasDir || d < 1.0f) score = d;
        else {
            float c = (vx * dx + vy * dy) / d;          // coseno dell'angolo con la direzione
            if (c >= cosLim) score = aim ? (1.0f - c) * 2000.0f + d : d * (1.0f + (1.0f - c) * 2.0f);
            else if (d <= nearRadius) { score = aim ? 2000.0f + d : d * 4.0f; inCone = false; }
            else continue;
        }
        if (score >= bestScore) continue;
        int cls = -1; bool dead = false;
        const char* kind = "";
        if (creature) {
            if (!ReadKind(o.addr, cls, dead) || dead || !InMask(mask, cls)) continue;
            // Il cavallo dell'eroe in sella sta nella sua stessa posizione.
            if (cls == 4 && (d < 20.0f || (forHero2 && h1Horse && o.id == h1Horse) || (h2Horse && o.id == h2Horse))) continue;
            kind = talk ? (cls == 4 ? "cavallo" : "NPC") : "nemico";
            // Priorita' degli NPC utili (0.10.3, F7 del 23/10/2026): +0x200 = servizio (0x2000
            // mercanti e maestri, 0x1000 fabbro, 0x04000000 stalliere), +0x244 diverso da zero =
            // dialogo di missione o speciale (Anurf, Romata, Melissa, Azortt, il medico...). Soldati
            // e popolani senza ne' l'uno ne' l'altro vengono dopo tutti gli altri candidati (NPC
            // utili, forzieri, oggetti), ma prima del compagno al seguito.
            // Dal 0.11.1 chi ha un'icona sopra la testa viene prima di tutti (anche il compagno al
            // seguito, come l'elfa col "!" del 22/10). L'icona e' quella delle missioni ("!", "?!")
            // o quella del servizio (fabbro, mercante, maestro, stalliere: lo stalliere 60 e il
            // mercante 1331 risultano "con icona" nel log del 23/10).
            const bool quest = talk && cls != 4 && HasQuestIcon(o.addr);
            if (quest) {
                if (!aim) score *= kUsefulFactor;   // 0.56.18: era score -= kQuestBonus
                kind = "NPC (icona)";
            }
            else if (talk && cls != 4) {
                DWORD svc = 0;
                Rd(o.addr + 0x200, svc);
                if (svc & kServiceMask) { if (!aim) score *= kUsefulFactor; kind = "NPC (servizio)"; }
                else if (!allNpc) continue;          // 0.56.18: generici e dialogo senza icona, niente aggancio da solo
                else kind = "NPC generico";
            }
            if (talk && cls != 4 && o.id == g_usedNpc.load() && static_cast<LONG>(GetTickCount() - g_usedNpcUntil.load()) < 0) score *= 3.0f;
            if (score >= bestScore) continue;
            const double hx = double(o.x - heroX), hy = double(o.y - heroY);
            const bool nearHero = heroPos && hx * hx + hy * hy < double(kFollowerNear) * kFollowerNear;
            // 0.17.6 (richiesta del tester, prova delle 18:30): con follower 0 il compagno e' escluso a
            // qualunque distanza. Oltre kFollowerNear prima contava come "NPC generico" (l'elfa 693 alle
            // 18:29:35) e finiva nel bersaglio a fuoco e nell'anteprima. L'id visto una volta resta escluso.
            static DWORD followerId = 0xffffffffu;
            const bool isFollower = IsFollower(o.addr) || o.id == followerId;
            if (isFollower && nearHero) followerId = o.id;
            if (talk && !quest && isFollower && follower == 0) continue;
            if (talk && !quest && nearHero && isFollower) {
                if (follower == 1 && !inCone) continue;
                if (!aim) score += 100000.0f;           // ultimo fra i candidati
                if (score >= bestScore) continue;
                kind = "NPC (compagno al seguito)";
            }
        }
        DWORD st = 0;
        // Forzieri e oggetti gia' raggiunti dopo essere stati scelti con A: esclusi (dal 0.9.6
        // anche gli oggetti, i cSpecial3D del 22/10 scelti venti volte senza sparire mai).
        if (!creature && !door && IsUsed(o)) continue;     // le porte dicono da sole se sono aperte
        if (door) kind = "porta";
        else if (o.vt == kVtChest) {
            // Fino alla 0.9.2 solo +0x64 = 0 ("chiuso", verificato su un baule): scartava il baule
            // giallo del 22/10. Ora valgono tutti tranne quelli gia' aperti con A; +0x64 va nel log.
            Rd(o.addr + 0x64, st);
            // +0x64 = 0xAA: aperto (0.10.3; il forziere 1342 e' passato da 0 a 0xAA appena aperto,
            // e le porte aperte hanno lo stesso valore), anche se aperto col mouse o prima.
            if (st == 0xAA) continue;
            kind = "forziere";
        }
        else if (!creature) kind = "oggetto a terra";
        bestScore = score;
        out.addr = o.addr; out.id = o.id; out.vt = o.vt; out.cls = cls; out.sx = sx; out.sy = sy; out.kind = kind;
        out.name[0] = 0;
        if (!creature) {
            char cn[32];
            ClassName(o.vt, cn, sizeof(cn));
            if (o.vt == kVtChest) _snprintf_s(out.name, sizeof(out.name), _TRUNCATE, "%s +64=%lX", cn, st);
            else _snprintf_s(out.name, sizeof(out.name), _TRUNCATE, "%s", cn);
        }
        found = true;
    }
    // 0.55.38: oggetti fissi della mappa, cioe' i rettangoli del gioco con l'id col bit alto (trigger degli script: nascondigli
    // magici, portali). Prova del 31/10 sera sulla Steam Machine: il pad 1 apriva i nascondigli magici solo passando alla modalita'
    // cursore e cliccandoci sopra ("veramente scomodo"); negli script sono "Versteck" in punti presi a caso da un elenco
    // (PoolPosition, testo VERSTECKMOUSEOVER), e il gioco li tratta come i portali: ordine "vai e usa" su un id col bit alto, indice
    // del trigger nella parola bassa (0x53a5a2, tabella [0xab3538], voci da 0x58 byte). Per l'eroe 2 li trova gia' Hero2FindInteract.
    // Punto e distanza dal piede del disegno; dopo l'uso 6 s di pausa (MemoryMarkFixedUsed).
    if (talk && !forHero2) {
        std::vector<PickRect> rs;
        if (ReadPickRects(v, rs)) {
            for (const PickRect& r : rs) {
                if (!(r.id & 0x80000000u) || r.w <= 0 || r.h <= 0 || FixedRecentlyUsed(r.id)) continue;
                if (r.w > 0.4f * v.h || r.h > 0.4f * v.h) continue;   // niente sagome grandi (edifici): solo cose da usare
                bool skipped = false;
                for (int k = 0; k < nSkip; ++k) if (skip[k] == r.id) { skipped = true; break; }
                if (skipped) continue;
                const float sx = r.x + r.w * 0.5f, sy = r.y + r.h * 0.9f;
                if (sx < 0 || sy < 0 || sx > v.w || sy > v.h) continue;
                const float vx = sx - ox, vy = sy - oy, d = std::sqrt(vx * vx + vy * vy);
                if (d > range || d < 12.0f) continue;          // sotto i piedi: l'eroe ci sta gia' sopra
                float score;
                if (!hasDir || d < 1.0f) score = d;
                else {
                    const float c = (vx * dx + vy * dy) / d;
                    if (c >= cosLim) score = aim ? (1.0f - c) * 2000.0f + d : d * (1.0f + (1.0f - c) * 2.0f);
                    else if (d <= nearRadius) score = aim ? 2000.0f + d : d * 4.0f;
                    else continue;
                }
                if (score >= bestScore) continue;
                bestScore = score;
                out.addr = 0; out.id = r.id; out.vt = 0; out.cls = -1; out.sx = sx; out.sy = sy; out.kind = "oggetto fisso della mappa";
                _snprintf_s(out.name, sizeof(out.name), _TRUNCATE, "%dx%d", r.w, r.h);
                found = true;
            }
        }
        // 0.56.53 (prova sulla Steam Machine del 06/11 alle 00:33:37, foto: "Nascondiglio magico" con la mano sotto il mouse, ma
        // nessun rettangolo; il clic del cursore in quel punto l'ha aperto): il gioco trova i trigger dalla posizione nel mondo
        // (0x612c30), non dalla lista dei rettangoli. Si guardano anche i trigger vicini all'eroe 1 (TrigNearRefresh) che hanno un
        // nome, cioe' quelli che il gioco scrive col mouse sopra (nascondigli, portali), sullo stesso piano; punto = loro posizione
        // nel mondo proiettata sullo schermo. Quelli gia' fra i rettangoli contano una volta sola.
        std::vector<TrigNear> tn;
        BYTE fz = 0;
        const DWORD fh = heroId != 0xffffffffu ? ObjectById(heroId) : 0;
        if (fh && Rd(fh + 0x24, fz) && TrigNearCopy(tn)) {
            for (const TrigNear& t : tn) {
                if (t.z != fz || FixedRecentlyUsed(t.id)) continue;
                bool dup = false;
                for (const PickRect& r : rs) if (r.id == t.id) { dup = true; break; }
                for (int k = 0; k < nSkip && !dup; ++k) if (skip[k] == t.id) dup = true;
                if (dup) continue;
                std::string label;
                if (!FixedLabel(t.id, label) || label.empty()) continue;
                float sx = 0, sy = 0;
                if (!WorldToScreen(v, t.wx, t.wy, sx, sy) || sx < 0 || sy < 0 || sx > v.w || sy > v.h) continue;
                const float vx = sx - ox, vy = sy - oy, d = std::sqrt(vx * vx + vy * vy);
                if (d > range || d < 12.0f) continue;
                float score;
                if (!hasDir || d < 1.0f) score = d;
                else {
                    const float c = (vx * dx + vy * dy) / d;
                    if (c >= cosLim) score = aim ? (1.0f - c) * 2000.0f + d : d * (1.0f + (1.0f - c) * 2.0f);
                    else if (d <= nearRadius) score = aim ? 2000.0f + d : d * 4.0f;
                    else continue;
                }
                if (score >= bestScore) continue;
                bestScore = score;
                out.addr = 0; out.id = t.id; out.vt = 0; out.cls = -1; out.sx = sx; out.sy = sy; out.kind = "oggetto fisso della mappa";
                _snprintf_s(out.name, sizeof(out.name), _TRUNCATE, "trigger %s", label.c_str());
                found = true;
            }
        }
    }
    // Una porta chiusa fra l'eroe e il bersaglio scelto viene prima di lui (0.10.6, richiesta di
    // il tester del 23/10: il forziere dietro la porta chiusa non si raggiunge senza aprirla).
    if (talk && found && out.vt != kVtObject3D && nDoors) {
        const float tx = out.sx - ox, ty = out.sy - oy, tl = std::sqrt(tx * tx + ty * ty);
        const DoorSeen* pick = nullptr;
        for (int k = 0; k < nDoors && tl > 1.0f; ++k) {
            const DoorSeen& dd = doors[k];
            if (dd.d >= tl) continue;                           // la porta deve essere piu' vicina
            const float px = dd.sx - ox, py = dd.sy - oy;
            const float along = (px * tx + py * ty) / tl;       // proiezione sulla linea eroe-bersaglio
            const float across = std::fabs(px * ty - py * tx) / tl;
            if (along <= 0.0f || across > kDoorBetween * v.h) continue;
            if (!pick || dd.d < pick->d) pick = &dd;
        }
        if (pick) {
            out.addr = pick->o->addr; out.id = pick->o->id; out.vt = pick->o->vt; out.cls = -1;
            out.sx = pick->sx; out.sy = pick->sy; out.kind = "porta (davanti)";
            _snprintf_s(out.name, sizeof(out.name), _TRUNCATE, "cObject3D");
        }
    }
    return found;
}

// Aggiorna la posizione sullo schermo del bersaglio; false se morto, sparito o fuori schermo.
// Creature (cls >= 0) e, dal 0.9.3, anche forzieri e oggetti (stesso vtable e stesso id).
bool MemoryTrackTarget(Target& t)
{
    if (g_ok && !t.addr && (t.id & 0x80000000u)) {   // 0.55.38: oggetto fisso della mappa, dal suo rettangolo
        View v{};
        std::vector<PickRect> rs;
        if (!ReadView(v) || !ReadPickRects(v, rs)) return false;
        for (const PickRect& r : rs) {
            if (r.id != t.id || r.w <= 0 || r.h <= 0) continue;
            const float sx = r.x + r.w * 0.5f, sy = r.y + r.h * 0.9f;
            if (sx < 0 || sy < 0 || sx > v.w || sy > v.h) return false;
            t.sx = sx; t.sy = sy;
            return true;
        }
        int wx = 0, wy = 0;                            // 0.56.53: trigger senza rettangolo (nascondigli nel bosco)
        float sx = 0, sy = 0;
        if (TrigWorldPos(t.id, wx, wy) && WorldToScreen(v, wx, wy, sx, sy) && sx >= 0 && sy >= 0 && sx <= v.w && sy <= v.h) {
            t.sx = sx; t.sy = sy;
            return true;
        }
        return false;
    }
    if (!g_ok || !t.addr) return false;
    DWORD hdr[9];
    const DWORD vt = t.cls >= 0 ? kVtCreature : t.vt;
    if (!RdBuf(t.addr, hdr, sizeof(hdr)) || hdr[0] != vt || hdr[3] != t.id) return false;
    int cls = 0; bool dead = false;
    if (t.cls >= 0 && (!ReadKind(t.addr, cls, dead) || dead)) return false;
    View v{};
    if (!ReadView(v)) return false;
    float sx = 0, sy = 0;
    if (!WorldToScreen(v, static_cast<int>(hdr[7]), static_cast<int>(hdr[8]), sx, sy)) return false;
    if (t.vt == kVtObject3D) sy -= kDoorLift * v.h;           // porte: punto sulla porta disegnata
    if (sx < 0 || sy < 0 || sx > v.w || sy > v.h) return false;
    t.sx = sx; t.sy = sy;
    return true;
}

// Il bersaglio a fuoco (0.11.8) e' ancora valido? Visibile, vivo, e per forzieri, porte e
// oggetti ancora da usare (forziere non aperto, porta chiusa, oggetto non gia' raggiunto).
static bool IsClosedDoor(DWORD addr, DWORD& state);
bool MemoryTargetValid(Target& t)
{
    if (!MemoryTrackTarget(t)) return false;
    if (t.cls >= 0) return true;
    if (!t.addr) return !FixedRecentlyUsed(t.id);   // 0.55.38: oggetto fisso della mappa
    std::vector<DWORD> drawn;
    if (DrawnIds(drawn) && !std::binary_search(drawn.begin(), drawn.end(), t.id)) return false;   // non disegnato
    if (t.vt == kVtObject3D) { DWORD st = 0; return IsClosedDoor(t.addr, st); }
    Obj o{};
    o.id = t.id;
    if (!Rd(t.addr + 0x1c, o.x) || !Rd(t.addr + 0x20, o.y) || IsUsed(o)) return false;
    DWORD st = 0;
    return !(t.vt == kVtChest && Rd(t.addr + 0x64, st) && st == 0xAA);
}

// Posizione dell'eroe nel mondo: id dai dati del giocatore, oggetto dal vettore (indice = id).
// Vita dell'eroe (0.17.0): frazione vita attuale (+0x4d8) / massima (+0x4d4) come nel ritratto
// (0x6d74b0), e il tempo del gioco usato dal lampeggio ([0xcd69d8], double; il ritratto lampeggia
// sotto 0.25 con intensita' (sin(t * 4) + 1) / 2).
bool MemoryHeroHealth(float& frac, double& gameTime)
{
    if (!g_ok) return false;
    DWORD player = 0, heroId = 0;
    if (!Rd(kPlayer, player) || !player || !RdHero(player, heroId)) return false;
    const DWORD obj = ObjectById(heroId);
    DWORD vt = 0; int hp = 0, maxHp = 0;
    if (!obj || !Rd(obj, vt) || vt != kVtCreature || !Rd(obj + 0x4d8, hp) || !Rd(obj + 0x4d4, maxHp) || maxHp <= 0) return false;
    if (!Rd(0x00cd69d8, gameTime)) return false;
    frac = static_cast<float>(hp) / static_cast<float>(maxHp);
    return true;
}

// 0.55.29: l'eroe 1 (locale) sta parlando con un PNG (modo 10: il gioco ha il dialogo aperto).
bool MemoryHeroTalking()
{
    if (!g_ok) return false;
    DWORD player = 0, heroId = 0;
    if (!Rd(kPlayer, player) || !player || !RdHero(player, heroId)) return false;
    const DWORD obj = ObjectById(heroId);
    WORD mode = 0;
    return obj && Rd(obj + 0xfc, mode) && mode == 10;
}

// 0.55.31: zoom di un passo, come la rotellina del mouse. La finestra del gioco (gestore dei messaggi, 0x812a75 -> 0x812d4f)
// trasforma WM_MOUSEWHEEL in un evento d'ingresso di tipo 3 con lo scatto della rotellina (parola alta di wParam) e la posizione
// del cursore che ha gia' ([0x182dd90]/[0x182dd94]), poi lo mette in coda (0x808610, 0x808ab0): basta mandarle il messaggio,
// come Esc per il menu. Livelli dello zoom visti nella prova del 31/10 (rotellina vera): 0.50, 0.75, 1.00.
void GameZoomStep(int dir, const char* who)
{
    const HWND hwnd = g_gameHwnd.load();
    if (!hwnd || !dir) return;
    if (g_cfg.zoomInvert) dir = -dir;
    RECT cr{};
    GetClientRect(hwnd, &cr);
    POINT c{ cr.right / 2, cr.bottom / 2 };
    ClientToScreen(hwnd, &c);
    const short delta = static_cast<short>(dir > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
    PostMessageA(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), MAKELPARAM(c.x, c.y));
    DWORD sm = 0; float zoom = 0;
    if (Rd(0x017e8f24, sm) && sm) Rd(sm + 0x84, zoom);   // lo zoom come lo vede il gestore dell'audio (prima del passo)
    Log("%s: zoom, rotellina %s (zoom attuale %.2f)", who, delta > 0 ? "in avanti" : "indietro", zoom);
}

bool MemoryHeroPos(int& x, int& y)
{
    if (!g_ok) return false;
    DWORD player = 0, heroId = 0, mgr = 0, beg = 0, end = 0, obj = 0;
    if (!Rd(kPlayer, player) || !player || !RdHero(player, heroId)) return false;
    if (!Rd(kObjMgr, mgr) || !Rd(mgr + 4, beg) || !Rd(mgr + 8, end) || end <= beg) return false;
    if (heroId >= (end - beg) / 4 || !Rd(beg + heroId * 4, obj) || !obj) return false;
    DWORD hdr[9];
    if (!RdBuf(obj, hdr, sizeof(hdr)) || hdr[0] != kVtCreature || hdr[3] != heroId) return false;
    x = static_cast<int>(hdr[7]);
    y = static_cast<int>(hdr[8]);
    return true;
}

// Distanza nel mondo fra l'eroe e il bersaglio (creatura, forziere o oggetto ancora nel vettore).
bool MemoryHeroDistance(const Target& t, float& d)
{
    int hx = 0, hy = 0;
    DWORD hdr[9];
    if (!t.addr || !MemoryHeroPos(hx, hy)) return false;
    const DWORD vt = t.cls >= 0 ? kVtCreature : t.vt;
    if (!RdBuf(t.addr, hdr, sizeof(hdr)) || hdr[0] != vt || hdr[3] != t.id) return false;
    double ddx = double(static_cast<int>(hdr[7]) - hx), ddy = double(static_cast<int>(hdr[8]) - hy);
    d = static_cast<float>(std::sqrt(ddx * ddx + ddy * ddy));
    return true;
}

// Livello dell'eroe: parola a +0x3FE della creatura (trovato il 22/10/2026 confrontando
// i dati dell'Elfa silvana prima e dopo il passaggio dal livello 1 al 2).
bool MemoryHeroLevel(int& level)
{
    if (!g_ok) return false;
    DWORD player = 0, heroId = 0, mgr = 0, beg = 0, end = 0, obj = 0;
    if (!Rd(kPlayer, player) || !player || !RdHero(player, heroId)) return false;
    if (!Rd(kObjMgr, mgr) || !Rd(mgr + 4, beg) || !Rd(mgr + 8, end) || end <= beg) return false;
    if (heroId >= (end - beg) / 4 || !Rd(beg + heroId * 4, obj) || !obj) return false;
    DWORD hdr[4];
    WORD lv = 0;
    if (!RdBuf(obj, hdr, sizeof(hdr)) || hdr[0] != kVtCreature || hdr[3] != heroId) return false;
    if (!Rd(obj + 0x3FE, lv) || lv < 1 || lv > 216) return false;
    level = lv;
    return true;
}

// Porta che il clic sceglierebbe nel punto (client), con il suo id (0.16.0: regola dei 2 secondi
// dello stick sulle porte).
bool MemoryDoorAt(LONG x, LONG y, DWORD& id)
{
    DWORD pick = 0, type = 0;
    if (!g_ok || !PickAt(x, y, pick) || !pick || (pick & 0x80000000u)) return false;
    const DWORD obj = ObjectById(pick);
    if (!obj || !Rd(obj + 0x10, type) || !IsDoorType(type)) return false;
    id = pick;
    return true;
}

// Stato di una porta (+0x64: 0 mai toccata, 0xAA aperta, 0xAB richiusa), 0.16.2.
bool MemoryDoorState(DWORD id, DWORD& state)
{
    DWORD type = 0;
    const DWORD obj = (g_ok && !(id & 0x80000000u)) ? ObjectById(id) : 0;
    return obj && Rd(obj + 0x10, type) && IsDoorType(type) && Rd(obj + 0x64, state);
}

// Oggetto (eroe escluso) che il clic sceglierebbe nel punto, senza scrivere nel log (0.17.5: controllo a
// ogni fotogramma del punto di movimento col tasto tenuto).
bool MemoryPickObject(LONG x, LONG y, DWORD& id)
{
    DWORD pick = 0, player = 0, heroId = 0xffffffffu;
    if (!g_ok || !PickAt(x, y, pick) || !pick) return false;
    if (Rd(kPlayer, player) && player) RdHero(player, heroId);
    if (pick == heroId) return false;
    id = pick;
    return true;
}

// Oggetto che il gioco ha scelto sotto il puntatore nell'ultimo fotogramma (0x612160 lo salva in
// [0xcdcadc]+0x6c, 0 se nessuno), eroe escluso. Un clic adesso andrebbe a lui (0.16.1).
bool MemoryHoverObject(DWORD& id)
{
    DWORD in = 0, player = 0, heroId = 0xffffffffu;
    id = 0;
    if (!g_ok || !Rd(0x00cdcadc, in) || !in || !Rd(in + 0x6c, id) || !id || id == 0xffffffffu) return false;
    if (Rd(kPlayer, player) && player) RdHero(player, heroId);
    return id != heroId;
}

// Il punto (client) cade sul corpo di una creatura (eroe escluso)? Serve al
// movimento: premere sopra una persona apre il dialogo, sopra un nemico lo attacca.
static bool PickAt(LONG px, LONG py, DWORD& pick);

bool MemoryPointOnCreature(float px, float py, float hf)
{
    if (!g_ok) return false;
    // 0.11.7: con la lista dei rettangoli del gioco il punto e' "occupato" se il clic vi
    // sceglierebbe un oggetto diverso dall'eroe (NPC, nemico, forziere, porta, oggetto a terra).
    DWORD pick = 0, player = 0, heroId = 0xffffffffu;
    if (PickAt(static_cast<LONG>(px), static_cast<LONG>(py), pick)) {
        if (Rd(kPlayer, player) && player) RdHero(player, heroId);
        if (pick == 0 || pick == heroId) return false;
        Log("Movimento: punto (%.0f,%.0f) sull'oggetto id %lu", px, py, pick);
        return true;
    }
    View v{};
    std::vector<Obj> cr;
    Obj hero{};
    bool haveHero = false;
    if (!ReadView(v) || !ReadCreatures(cr, hero, haveHero)) return false;
    for (const Obj& o : cr) {
        float sx = 0, sy = 0;
        if (!WorldToScreen(v, o.x, o.y, sx, sy)) continue;
        // Sagoma approssimata: rettangolo dai piedi fino a circa 100 pixel sopra (a 1080p),
        // piu' largo per i cavalli (tipo 4).
        if (py > sy + 0.015f * hf || py < sy - 0.10f * hf) continue;
        if (std::fabs(px - sx) > 0.07f * hf) continue;
        int cls = 0; bool dead = false;
        if (!ReadKind(o.addr, cls, dead) || dead) continue;
        // 0.11.2: persone 0.055 (prima 0.04): il 23/10 alle 13:27:09 un clic di movimento a 49 px
        // dai piedi del mercante ha aperto il suo negozio.
        if (std::fabs(px - sx) <= (cls == 4 ? 0.07f : 0.055f) * hf) {
            Log("Movimento: punto (%.0f,%.0f) sulla creatura id %lu tipo %d in (%.0f,%.0f)", px, py, o.id, cls, sx, sy);
            return true;
        }
    }
    return false;
}

// Diagnostica dell'interfaccia. Il cUI_Manager ([0x17edb3c]) contiene a +0x80..+0xD8
// 23 puntatori alle finestre (inventario, dialoghi, menu...): per ciascuna si
// registra la classe (RTTI) e ogni parola dei primi 0x80 byte che cambia, per
// trovare il segno "finestra visibile". Le parole che cambiano troppo spesso
// vengono ignorate dopo 15 variazioni.
void MemoryDiagUI(DWORD now)
{
    if (!g_ok || !g_cfg.memoryDiag) return;
    static DWORD last = 0;
    if (now - last < 50) return;
    last = now;
    constexpr int kWin = 23, kWords = 0x80 / 4;
    static DWORD ptr[kWin];
    static DWORD prev[kWin][kWords];
    static unsigned char changes[kWin][kWords];
    DWORD mgr = 0, cur[kWin];
    if (!Rd(kUIManager, mgr) || !mgr || !RdBuf(mgr + 0x80, cur, sizeof(cur))) return;
    for (int w = 0; w < kWin; ++w) {
        if (cur[w] != ptr[w]) {
            ptr[w] = cur[w];
            memset(changes[w], 0, sizeof(changes[w]));
            DWORD vt = 0;
            char name[64] = "-";
            if (cur[w] && Rd(cur[w], vt)) ClassName(vt, name, sizeof(name));
            Log("UI[%d] = %s @%08lX", w, name, cur[w]);
            if (!cur[w] || !RdBuf(cur[w], prev[w], sizeof(prev[w]))) memset(prev[w], 0, sizeof(prev[w]));
            continue;
        }
        if (!cur[w]) continue;
        DWORD now_[kWords];
        if (!RdBuf(cur[w], now_, sizeof(now_))) continue;
        for (int i = 1; i < kWords; ++i) {
            if (now_[i] == prev[w][i] || changes[w][i] > 15) continue;
            if (++changes[w][i] > 15) { Log("UI[%d] +%02X cambia troppo spesso, ignorata", w, i * 4); continue; }
            Log("UI[%d] +%02X %08lX -> %08lX", w, i * 4, prev[w][i], now_[i]);
        }
        memcpy(prev[w], now_, sizeof(now_));
    }
}

// F6 (tasto fisico): cerca nella memoria del processo tutti gli oggetti
// dell'interfaccia (vtable delle classi cUI_ di Sacred.exe, 0x895288..0x8967d0),
// per trovare la finestra dei dialoghi con gli NPC, che non e' tra le 23 del
// cUI_Manager. Registra le classi trovate, gli oggetti con +0x10 dispari
// (visibili, come le finestre del cUI_Manager) e chi li punta in .data.
namespace {
bool IsUiVt(DWORD v)
{
    if (v < 0x895288 || v > 0x8967d0 || (v & 3)) return false;
    static const DWORD vts[] = {
        0x895288,0x8952d8,0x895330,0x89537c,0x8953e4,0x895434,0x895490,0x8954dc,0x895528,0x895578,
        0x8955c4,0x895610,0x895668,0x8956cc,0x895714,0x895798,0x8957e4,0x895834,0x895880,0x8958cc,
        0x895918,0x895968,0x8959b8,0x895a08,0x895a58,0x895ac0,0x895b3c,0x895b88,0x895bd4,0x895c2c,
        0x895c8c,0x895d78,0x895dc4,0x895e2c,0x895e78,0x895ec4,0x895f10,0x895f5c,0x895fa8,0x896004,
        0x89605c,0x8960e4,0x896134,0x896180,0x8961d8,0x896220,0x896258,0x896308,0x89633c,0x896374,
        0x8963c0,0x896488,0x8964d8,0x896534,0x8965ac,0x8965e8,0x896620,0x896664,0x89669c,0x896700,
        0x896758,0x8967d0 };
    for (DWORD x : vts) if (x == v) return true;
    return false;
}
}

// Diagnostica (0.9.9): elementi dell'interfaccia contenuti nell'oggetto di una delle 23
// finestre (per il mercante: le schede Armi/Armatura/Varie). Come per lo stalliere ogni
// elemento ha +0x10 stato (bit 0 visibile, bit 2 attivo), +0x24/+0x28 posizione relativa al
// genitore in +0x50, +0x2C/+0x2E larghezza e altezza, nome a +0x30. Si cerca nei primi
// 0x3000 byte della finestra ogni parola che sia un vtable cUI_ e si registra cosa c'e'.
void MemoryWindowDump(int index, const char* why)
{
    if (!g_ok || index < 0 || index >= 23) return;
    DWORD mgr = 0, win = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + index * 4, win) || !win) return;
    static DWORD buf[0x3000 / 4];
    int words = static_cast<int>(sizeof(buf) / 4);
    if (!RdBuf(win, buf, sizeof(buf))) {                    // oltre la fine della memoria: meno byte
        words = 0x1000 / 4;
        if (!RdBuf(win, buf, 0x1000)) { Log("FINESTRA n.%d (%s): non leggibile", index, why); return; }
    }
    Log("FINESTRA n.%d (%s) @%08lX:", index, why, win);
    int shown = 0;
    for (int i = 1; i < words && shown < 40; ++i) {
        if (!IsUiVt(buf[i])) continue;
        const DWORD e = win + i * 4;
        DWORD f = 0, p = 0; int x = 0, y = 0; short wh[2] = {}; char nm[17] = {};
        if (!Rd(e + 0x10, f) || !Rd(e + 0x24, x) || !Rd(e + 0x28, y) || !RdBuf(e + 0x2C, wh, sizeof(wh))) continue;
        RdBuf(e + 0x30, nm, 16);
        for (char& c : nm) if (c && (c < 32 || c > 126)) c = '.';
        Rd(e + 0x50, p);
        int ax = x, ay = y;
        for (int depth = 0; p && depth < 6; ++depth) {     // posizione assoluta: somma dei genitori
            int px = 0, py = 0;
            if (!Rd(p + 0x24, px) || !Rd(p + 0x28, py)) break;
            ax += px; ay += py;
            if (!Rd(p + 0x50, p)) break;
        }
        char cn[40];
        ClassName(buf[i], cn, sizeof(cn));
        Log("  +%04X %-16s %-16s stato %08lX rel (%d,%d) ass (%d,%d) %dx%d", i * 4, cn, nm, f, x, y, ax, ay, wh[0], wh[1]);
        ++shown;
    }
    if (!shown) Log("  nessun elemento cUI_ nei primi 0x3000 byte");
    // 0.9.10: nel mercante c'e' un solo elemento dentro la finestra (il ritratto), quindi le
    // schede sono oggetti a parte: si seguono i puntatori dei primi 0x400 byte della finestra
    // e, un livello sotto, quelli dei primi 0x200 byte di ciascun oggetto cUI_ trovato.
    std::vector<DWORD> seen;
    int lines = 0;
    auto follow = [&](DWORD from, int bytes, int depth, auto& self) -> void {
        DWORD w[0x400 / 4];
        if (bytes > static_cast<int>(sizeof(w)) || !RdBuf(from, w, bytes)) return;
        for (int i = 1; i < bytes / 4 && lines < 80; ++i) {
            const DWORD p = w[i];
            if (p < 0x01000000u || p >= 0x7FFF0000u || (p & 3)) continue;
            DWORD vt = 0;
            if (!Rd(p, vt) || !IsUiVt(vt)) continue;
            if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
            seen.push_back(p);
            DWORD f = 0; int x = 0, y = 0; short wh[2] = {}; char nm[17] = {};
            Rd(p + 0x10, f); Rd(p + 0x24, x); Rd(p + 0x28, y); RdBuf(p + 0x2C, wh, sizeof(wh)); RdBuf(p + 0x30, nm, 16);
            for (char& c : nm) if (c && (c < 32 || c > 126)) c = '.';
            char cn[40];
            ClassName(vt, cn, sizeof(cn));
            Log("  %s[+%03X] -> %08lX %-16s %-16s stato %08lX rel (%d,%d) %dx%d", depth ? "    " : "", i * 4, p, cn, nm, f,
                x, y, wh[0], wh[1]);
            ++lines;
            if (depth == 0) self(p, 0x200, 1, self);
        }
    };
    seen.push_back(win);
    follow(win, 0x400, 0, follow);
}

void MemoryDiagScanUI()
{
    static bool prev = false;
    if (!g_ok || !g_cfg.memoryDiag) return;
    bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    static int n = 0;
    ++n;
    DWORD t0 = GetTickCount();
    struct Hit { DWORD addr, vt, f10; };
    std::vector<Hit> hits;
    std::vector<DWORD> buf;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0xFFFE0000u; ) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) break;
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD)) {
            for (uintptr_t off = 0; off < size; off += 0x100000) {
                size_t len = (size - off) < 0x100000 ? (size - off) : 0x100000;
                buf.resize(len / 4);
                if (!RdBuf(static_cast<DWORD>(base + off), buf.data(), len)) continue;
                for (size_t i = 0; i + 5 < buf.size(); ++i) {
                    if (!IsUiVt(buf[i])) continue;
                    hits.push_back({ static_cast<DWORD>(base + off + i * 4), buf[i], buf[i + 4] });
                }
            }
        }
        a = base + size;
        if (size == 0) break;
    }
    Log("UISCAN n.%d: %u oggetti dell'interfaccia in %lu ms", n, static_cast<unsigned>(hits.size()), GetTickCount() - t0);
    // Conteggio per classe
    std::vector<std::pair<DWORD, int>> cnt;
    for (const Hit& h : hits) {
        auto it = std::find_if(cnt.begin(), cnt.end(), [&](const std::pair<DWORD, int>& p) { return p.first == h.vt; });
        if (it == cnt.end()) cnt.push_back({ h.vt, 1 }); else ++it->second;
    }
    for (auto& c : cnt) {
        char name[64]; ClassName(c.first, name, sizeof(name));
        int vis = 0;
        for (const Hit& h : hits) if (h.vt == c.first && (h.f10 & 1)) ++vis;
        Log("UISCAN   %s: %d (con +0x10 dispari: %d)", name, c.second, vis);
    }
    // 0.49.4: finestre grandi visibili (qualunque classe tranne pulsanti e testi) e pulsanti visibili
    // della fascia centrale dello schermo, con nome, posizione assoluta e genitore: serve a
    // riconoscere la schermata "Il sogno" (inizio dell'esercitazione), che non e' un UI_WND_TOOLTIP.
    {
        auto absPos = [](DWORD e, int& ax, int& ay) {
            int x = 0, y = 0; DWORD p = 0;
            Rd(e + 0x24, x); Rd(e + 0x28, y); Rd(e + 0x50, p);
            ax = x; ay = y;
            for (int d = 0; p && d < 6; ++d) {
                int px = 0, py = 0;
                if (!Rd(p + 0x24, px) || !Rd(p + 0x28, py)) break;
                ax += px; ay += py;
                if (!Rd(p + 0x50, p)) break;
            }
        };
        int big = 0, btn = 0;
        for (const Hit& h : hits) {
            if (!(h.f10 & 1)) continue;
            const bool isBtn = h.vt == 0x896308;
            short wh[2] = {};
            if (!RdBuf(h.addr + 0x2C, wh, sizeof(wh))) continue;
            char nm[17] = {}; RdBuf(h.addr + 0x30, nm, 16);
            for (char& c : nm) if (c && (c < 32 || c > 126)) c = '.';
            int ax = 0, ay = 0; DWORD par = 0;
            absPos(h.addr, ax, ay); Rd(h.addr + 0x50, par);
            char cn[64]; ClassName(h.vt, cn, sizeof(cn));
            if (!isBtn && wh[0] >= 300 && wh[1] >= 100 && big < 40) {
                ++big;
                Log("UISCAN grande %s @%08lX %-16s stato %08lX ass (%d,%d) %dx%d genitore @%08lX", cn, h.addr, nm, h.f10, ax, ay, wh[0], wh[1], par);
            }
            else if (isBtn && wh[0] >= 20 && wh[1] >= 15 && wh[0] <= 400 && wh[1] <= 100 && ay >= 650 && ay <= 850 && ax >= 500 && ax <= 1400 && btn < 40) {
                ++btn;
                Log("UISCAN pulsante @%08lX %-16s stato %08lX ass (%d,%d) %dx%d genitore @%08lX", h.addr, nm, h.f10, ax, ay, wh[0], wh[1], par);
            }
        }
    }
    // Finestre e cornici visibili (escluse le 23 note): dettaglio e riferimenti in .data
    int shown = 0;
    for (const Hit& h : hits) {
        if (!(h.f10 & 1) || shown >= 25) continue;
        char name[64]; ClassName(h.vt, name, sizeof(name));
        if (h.vt == 0x895ac0) continue;                          // barra in basso, sempre visibile
        DWORD w[24];
        if (!RdBuf(h.addr, w, sizeof(w))) continue;
        ++shown;
        Log("UISCAN   %s @%08lX +10=%08lX: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
            name, h.addr, h.f10, w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[19]);
    }
    // Riferimenti dalla sezione dati di Sacred.exe ai cUI_Book / cUI_Window2 / cUI_Frame visibili
    const DWORD dataBeg = 0x8e6000, dataEnd = 0x1d41000;
    std::vector<DWORD> data((dataEnd - dataBeg) / 4);
    if (RdBuf(dataBeg, data.data(), data.size() * 4)) {
        int refs = 0;
        for (const Hit& h : hits) {
            if (!(h.f10 & 1)) continue;
            if (h.vt != 0x895434 && h.vt != 0x8961d8 && h.vt != 0x8965ac && h.vt != 0x896180) continue;
            for (size_t i = 0; i < data.size() && refs < 30; ++i)
                if (data[i] == h.addr) {
                    char name[64]; ClassName(h.vt, name, sizeof(name));
                    Log("UISCAN   %s @%08lX puntato da [%08lX]", name, h.addr, static_cast<DWORD>(dataBeg + i * 4));
                    ++refs;
                }
        }
    }
}

// Stato dell'interfaccia dalle 23 finestre del cUI_Manager ([+0x10] bit 0 = visibile,
// verificato il 22/10/2026 con inventario, menu Esc e stalliere):
//   -1 nessuna finestra (menu principale o caricamento), 0 in gioco senza finestre
//   aperte, 1 almeno una finestra aperta. Escluse la barra in basso (0), la
//   minimappa (6), la console (9) e le finestre di rete (18, 19).
// Finestra dello stalliere (cUI_Horse, finestra n.17, vtable 0x895528). Il gestore dei clic
// 0x6be460 prova 3 pulsanti a +0x170 (passo 0x9c), ciascuno con il proprio rettangolo
// (+0x24 x, +0x28 y relativi al genitore in +0x50, +0x2C larghezza e +0x2E altezza a 16 bit,
// come in 0x732050) e reagisce solo se +0x10 ha il bit 2. Restituisce il centro, in coordinate
// client, dell'ultimo pulsante visibile e attivo ("Continua" quando non si compra).
bool MemoryHorseButton(LONG& x, LONG& y)
{
    if (!g_ok) return false;
    DWORD mgr = 0, win = 0, vt = 0, wf = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + 17 * 4, win) || !win) return false;
    if (!Rd(win, vt) || vt != 0x895528 || !Rd(win + 0x10, wf) || !(wf & 1)) return false;
    bool found = false;
    char info[160] = "";
    for (int i = 0; i < 3; ++i) {
        const DWORD b = win + 0x170 + i * 0x9c;
        DWORD f = 0; int bx = 0, by = 0; short wh[2] = {};
        if (!Rd(b + 0x10, f) || !Rd(b + 0x24, bx) || !Rd(b + 0x28, by) || !RdBuf(b + 0x2C, wh, sizeof(wh))) continue;
        // Somma delle posizioni dei genitori (finestra compresa)
        DWORD p = 0;
        Rd(b + 0x50, p);
        for (int depth = 0; p && depth < 6; ++depth) {
            int px = 0, py = 0;
            if (!Rd(p + 0x24, px) || !Rd(p + 0x28, py)) break;
            bx += px; by += py;
            if (!Rd(p + 0x50, p)) break;
        }
        size_t len = strlen(info);
        _snprintf_s(info + len, sizeof(info) - len, _TRUNCATE, " [%d: flag %lX, %d,%d %dx%d]", i, f, bx, by, wh[0], wh[1]);
        if ((f & 5) == 5 && wh[0] > 0 && wh[1] > 0) {
            x = bx + wh[0] / 2; y = by + wh[1] / 2;
            found = true;
        }
    }
    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 500) { Log("Stalliere: pulsanti%s", info); lastLog = GetTickCount(); }
    return found;
}

// Una delle 23 finestre del cUI_Manager e' visibile? (4 = mercante, cUI_Merchant). Se aperta e
// rect non e' nullo, ne riporta posizione (+0x24, +0x28) e dimensioni (+0x2C, +0x2E), per il log.
bool MemoryWindowOpen(int index, RECT* rect)
{
    if (!g_ok || index < 0 || index >= 23) return false;
    DWORD mgr = 0, win = 0, f = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + index * 4, win) || !win) return false;
    if (!Rd(win + 0x10, f) || !(f & 1)) return false;
    if (rect) {
        int x = 0, y = 0; short wh[2] = {};
        Rd(win + 0x24, x); Rd(win + 0x28, y); RdBuf(win + 0x2C, wh, sizeof(wh));
        *rect = RECT{ x, y, x + wh[0], y + wh[1] };
    }
    return true;
}

// 0.55.27: menu del gioco aperto (la partita e' in pausa): menu Esc (cUI_EscMenu), opzioni (cUI_Options) o salvataggi
// (cUI_Savegame), riconosciuti dalla classe fra le 23 finestre del cUI_Manager e non dal numero.
bool MemoryGameMenuOpen()
{
    if (!g_ok) return false;
    DWORD mgr = 0;
    if (!Rd(kUIManager, mgr) || !mgr) return false;
    for (int i = 0; i < 23; ++i) {
        DWORD win = 0, vt = 0, f = 0;
        if (!Rd(mgr + 0x80 + i * 4, win) || !win || !Rd(win, vt)) continue;
        if (vt != 0x008954dc && vt != 0x0089605c && vt != 0x008960e4) continue;
        if (Rd(win + 0x10, f) && (f & 1)) return true;
    }
    return false;
}

// Schede del mercante (0.9.11, analisi statica del 23/10/2026): la finestra cUI_Merchant (n.4)
// ha in +0x154 un cUI_TabSheet ("UI_TABSHEET_MERC..."), che in +0x78 ha il cUI_TabControl delle
// schede. Nel suo gestore dei clic (0x727e40) la parola in +0x84 e' l'indice della scheda attiva,
// confrontato con il numero di schede = ([+0x7C] - [+0x78]) / 24; ogni voce del vettore da 24 byte
// ha in +0x10 il pulsante della scheda (stesso schema di posizione degli altri elementi) e in
// +0x14 la pagina. Restituisce indice attivo, numero di schede e rettangoli assoluti dei pulsanti.
bool MemoryMerchantTabs(int& active, int& count, RECT* rects, int maxRects)
{
    if (!g_ok) return false;
    DWORD mgr = 0, win = 0, sheet = 0, ctl = 0, vt = 0, beg = 0, end = 0;
    WORD idx = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + 4 * 4, win) || !win) return false;
    if (!Rd(win + 0x154, sheet) || !sheet || !Rd(sheet, vt) || vt != 0x896258) return false;   // cUI_TabSheet
    if (!Rd(sheet + 0x78, ctl) || !ctl || !Rd(ctl, vt) || vt != 0x896220) return false;        // cUI_TabControl
    if (!Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7C, end) || !Rd(ctl + 0x84, idx) || end < beg) return false;
    count = static_cast<int>((end - beg) / 24);
    if (count < 1 || count > 8 || idx >= count) return false;
    active = idx;
    for (int i = 0; i < count && i < maxRects; ++i) {
        DWORD b = 0, p = 0; int x = 0, y = 0; short wh[2] = {};
        if (!Rd(beg + i * 24 + 0x10, b) || !b || !Rd(b + 0x24, x) || !Rd(b + 0x28, y) || !RdBuf(b + 0x2C, wh, sizeof(wh)))
            return false;
        Rd(b + 0x50, p);
        for (int depth = 0; p && depth < 6; ++depth) {
            int px = 0, py = 0;
            if (!Rd(p + 0x24, px) || !Rd(p + 0x28, py)) break;
            x += px; y += py;
            if (!Rd(p + 0x50, p)) break;
        }
        x += MemoryMerchantRealDx();                // 0.56.9: mercante condiviso, il lato dell'eroe 1 e' spostato
        rects[i] = RECT{ x, y, x + wh[0], y + wh[1] };
    }
    return true;
}

// --- Zaino dell'eroe (0.12.0, Ghidra del 23/10/2026) -------------------------------------
// Inventari: [0xad33c8] -> 32 cInventoryEntry da 0x1180 byte (0x5df160(n) = voce n). Nella voce:
// +0x115c colonne, +0x115e righe, +0x1164/+0x1166 origine dell'ultimo disegno in coordinate
// client (lo zaino 20x8 dell'eroe a (896,392) a 1920x1080), caselle da +0x18 a 12 byte: +0 riferimento dell'oggetto (solo nella
// casella d'origine), +4 flag (bit 0 occupata), +5/+6 larghezza e altezza dell'oggetto nella
// casella d'origine, colonna e riga d'origine nelle altre. Caselle di 32x32 pixel.
// La pagina dello zaino e' un cUI_InventoryTab (vtable 0x8955c4) dentro un cUI_TabSheet della
// finestra cUI_Inventory3 (n.1); [pagina+0x158] = numero dell'inventario (0x6c15a0).
constexpr DWORD kInvTable = 0x00ad33c8, kInvSize = 0x1180;
constexpr DWORD kVtInventory3 = 0x00895668, kVtInventoryTab = 0x008955c4;
constexpr DWORD kVtTabSheet = 0x00896258, kVtTabControl = 0x00896220;

static void AbsPos(DWORD e, int& ax, int& ay)
{
    ax = 0; ay = 0;
    for (int depth = 0; e && depth < 8; ++depth) {
        int x = 0, y = 0;
        if (!Rd(e + 0x24, x) || !Rd(e + 0x28, y)) break;
        ax += x; ay += y;
        if (!Rd(e + 0x50, e)) break;
    }
}

// Icona del cavallo accanto al ritratto (0.17.5, Ghidra 0x6d6d50 e 0x6d74b0): elemento incorporato a +0x1d4
// del cUI_Mercenary (finestra n.7 del gestore); il gioco lo rende visibile (+0x10 bit 0) solo se l'eroe ha
// un cavallo. Centro in coordinate client.
bool MemoryHorseIcon(LONG& x, LONG& y)
{
    if (!g_ok) return false;
    DWORD mgr = 0, w = 0, f = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + 7 * 4, w) || !w) return false;
    const DWORD e = w + 0x1d4;
    short wh[2] = {};
    if (!Rd(e + 0x10, f) || !(f & 1) || !RdBuf(e + 0x2C, wh, sizeof(wh)) || wh[0] < 4 || wh[1] < 4) return false;
    int ax = 0, ay = 0;
    AbsPos(e, ax, ay);
    x = ax + wh[0] / 2; y = ay + wh[1] / 2;
    return true;
}

// Frecce della lista dei salvataggi (0.17.1, Ghidra 0x7526d0): cUI_Scrollbar (vtable 0x896664)
// incorporata in cUI_Savegame a +0x478, freccia su incorporata a +0x94 e freccia giu' a +0x110 della
// barra; un clic su una freccia sposta la lista di una riga (posizione float in +0x8c, righe in +0x88).
bool MemorySaveScroll(RECT& up, RECT& down)
{
    if (!g_ok) return false;
    DWORD mgr = 0;
    if (!Rd(kUIManager, mgr) || !mgr) return false;
    for (DWORD off = 0x80; off < 0x200; off += 4) {
        DWORD w = 0, vt = 0, f = 0, svt = 0;
        if (!Rd(mgr + off, w) || !w || (w & 3) || !Rd(w, vt) || vt != 0x008960e4 || !Rd(w + 0x10, f) || !(f & 1)) continue;
        const DWORD sb = w + 0x478;
        if (!Rd(sb, svt) || svt != 0x00896664) return false;
        RECT* out[2] = { &up, &down };
        const DWORD arrows[2] = { sb + 0x94, sb + 0x110 };
        for (int k = 0; k < 2; ++k) {
            int x = 0, y = 0; short wh[2] = {};
            AbsPos(arrows[k], x, y);
            if (!RdBuf(arrows[k] + 0x2C, wh, sizeof(wh)) || wh[0] < 4 || wh[1] < 4) return false;
            *out[k] = RECT{ x, y, x + wh[0], y + wh[1] };
        }
        static DWORD logged = 0;
        if (logged != w) {
            logged = w;
            Log("Salvataggi: freccia su (%ld,%ld)-(%ld,%ld), freccia giu' (%ld,%ld)-(%ld,%ld)", up.left, up.top, up.right,
                up.bottom, down.left, down.top, down.right, down.bottom);
        }
        return true;
    }
    return false;
}

// Pagina attiva di un cUI_TabSheet fra i puntatori dei primi 0x400 byte della finestra, se ha il
// vtable richiesto.
static DWORD ActiveTabPage(DWORD win, DWORD pageVt)
{
    DWORD w[0x400 / 4];
    if (!RdBuf(win, w, sizeof(w))) return 0;
    for (int i = 1; i < 0x100; ++i) {
        const DWORD sheet = w[i];
        if (sheet < 0x01000000u || sheet >= 0x7FFF0000u || (sheet & 3)) continue;
        DWORD svt = 0, ctl = 0, cvt = 0, beg = 0, end = 0, page = 0, pvt = 0;
        WORD active = 0;
        if (!Rd(sheet, svt) || svt != kVtTabSheet) continue;
        if (!Rd(sheet + 0x78, ctl) || !ctl || !Rd(ctl, cvt) || cvt != kVtTabControl) continue;
        if (!Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7C, end) || !Rd(ctl + 0x84, active) || end < beg) continue;
        const DWORD n = (end - beg) / 24;
        if (active >= n || n > 8) continue;
        if (!Rd(beg + active * 24 + 0x14, page) || !page || !Rd(page, pvt) || pvt != pageVt) continue;
        return page;
    }
    return 0;
}

static bool ReadGrid(DWORD e, GridInfo& g)
{
    WORD cols = 0, rows = 0, ox = 0, oy = 0;
    if (!e || !Rd(e + 0x115c, cols) || !Rd(e + 0x115e, rows) || !Rd(e + 0x1164, ox) || !Rd(e + 0x1166, oy)) return false;
    if (cols < 1 || cols > 40 || rows < 1 || rows > 40) return false;
    g.entry = e; g.cols = cols; g.rows = rows; g.ox = ox; g.oy = oy;
    // L'origine e' gia' in coordinate client (prova del 23/10 alle 17:10: origine (896,392),
    // griglia a schermo da (896,393); sommando la pagina (896,388) si finiva in (1792,780)).
    g.x = ox;
    g.y = oy;
    return true;
}

static DWORD VisibleWindow(int index, DWORD vt)
{
    DWORD mgr = 0, win = 0, wvt = 0, f = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + index * 4, win) || !win) return 0;
    if (!Rd(win, wvt) || wvt != vt || !Rd(win + 0x10, f) || !(f & 1)) return 0;
    return win;
}

// Schede di una finestra (0.12.6): il primo cUI_TabSheet fra i puntatori della finestra visibile
// n. index; indice attivo, numero e rettangoli (client) dei pulsanti delle schede (voce da 24 byte
// del cUI_TabControl: +0x10 pulsante), come per il mercante.
// 0.55.50: con i due zaini la finestra vera dello zaino si disegna spostata (eroe 1 a sinistra): le schede per LB/RB e il
// cursore si calcolano relative alla finestra e messe nella posizione disegnata (DualDraw, thread del motore, le aggiorna).
std::atomic<LONG> g_dwInvX{ 0 }, g_dwInvY{ 0 }, g_dwInvRealDx{ 0 };
bool MemoryDualActive();
bool MemoryWindowTabs(int index, int& active, int& count, RECT* rects, int maxRects)
{
    if (!g_ok || index < 0 || index >= 23) return false;
    DWORD mgr = 0, win = 0, f = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + index * 4, win) || !win || !Rd(win + 0x10, f) || !(f & 1)) return false;
    DWORD w[0x400 / 4];
    if (!RdBuf(win, w, sizeof(w))) return false;
    for (int i = 1; i < 0x100; ++i) {
        const DWORD sheet = w[i];
        if (sheet < 0x01000000u || sheet >= 0x7FFF0000u || (sheet & 3)) continue;
        DWORD svt = 0, ctl = 0, cvt = 0, beg = 0, end = 0;
        WORD idx = 0;
        if (!Rd(sheet, svt) || svt != kVtTabSheet) continue;
        if (!Rd(sheet + 0x78, ctl) || !ctl || !Rd(ctl, cvt) || cvt != kVtTabControl) continue;
        if (!Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7C, end) || !Rd(ctl + 0x84, idx) || end < beg) continue;
        count = static_cast<int>((end - beg) / 24);
        if (count < 1 || count > 8 || idx >= count) continue;
        active = idx;
        for (int k = 0; k < count && k < maxRects; ++k) {
            DWORD b = 0; short wh[2] = {};
            int x = 0, y = 0;
            if (!Rd(beg + k * 24 + 0x10, b) || !b || !RdBuf(b + 0x2C, wh, sizeof(wh))) return false;
            AbsPos(b, x, y);
            if (index == 1 && MemoryDualActive()) {
                int wx = 0, wy = 0;
                AbsPos(win, wx, wy);
                x += g_dwInvX.load() + g_dwInvRealDx.load() - wx;
                y += g_dwInvY.load() - wy;
            }
            rects[k] = RECT{ x, y, x + wh[0], y + wh[1] };
        }
        return true;
    }
    return false;
}

// Barra in basso (0.14.2, scarico del 24/10 alle 9:38): la finestra cUI_Taskbar2 (n.0) contiene, dentro
// il suo oggetto, due serie di 5 cUI_Button2 32x32 nelle stesse posizioni (i cerchi degli slot sopra
// l'occhio centrale): +0x258.. e +0x604.., passo 0xBC. Centri in coordinate client.
// 0.14.3: quelli sono i cerchi delle pozioni (in mezzo). Le arti sono i cerchi a destra: la barra punta
// in [+0x178]..[+0x188] ai 5 cUI_Static2 "UI_TB_SPELL1" 64x64 (le armi, a sinistra, in [+0x164]..[+0x174],
// "UI_TB_SKILL1"); quelli non sbloccati sono nascosti (+0x10 bit 0 spento) e spostati fuori schermo.
// Restituisce il numero di cerchi visibili delle arti, con i centri.
// 0.52.8: con due giocatori la barra dell'eroe 1 si disegna spostata a sinistra di g_tbDxL pixel solo durante il suo
// disegno e i suoi clic (TbShift, piu' avanti): fuori da li' i pezzi sono al centro e qui si aggiunge lo spostamento.
// Mentre e' in corso uno spostamento (g_tbShifting, nel thread del motore) si restituisce l'ultimo risultato.
std::atomic<int> g_tbDxL{ 0 };
std::atomic<LONG> g_tbShifting{ 0 };
int MemoryTaskbarArts(POINT* centers, int n)
{
    if (!g_ok || n > 5) return 0;
    static POINT last[5] = {};
    static int lastN = 0;
    if (g_tbShifting.load()) {
        const int m = (std::min)(n, lastN);
        for (int k = 0; k < m; ++k) centers[k] = last[k];
        return m;
    }
    const DWORD win = VisibleWindow(0, 0x00895ac0);
    if (!win) return 0;
    const int dx = g_tbDxL.load();
    int found = 0;
    for (int k = 0; k < n; ++k) {
        DWORD e = 0, vt = 0, f = 0; short wh[2] = {};
        int x = 0, y = 0;
        if (!Rd(win + 0x178 + k * 4, e) || !e || !Rd(e, vt) || vt != 0x89669c || !Rd(e + 0x10, f) || !(f & 1)) break;
        if (!RdBuf(e + 0x2C, wh, sizeof(wh))) break;
        AbsPos(e, x, y);
        centers[found++] = POINT{ x + dx + wh[0] / 2, y + wh[1] / 2 };
    }
    if (g_tbShifting.load()) {                          // spostamento cominciato durante la lettura
        const int m = (std::min)(n, lastN);
        for (int k = 0; k < m; ++k) centers[k] = last[k];
        return m;
    }
    for (int k = 0; k < found; ++k) last[k] = centers[k];
    lastN = found;
    return found;
}

// 0.56.23: cerchi delle pozioni della barra (0x6e3d50: cinque cUI_Button2 32x32 da +0x258, passo 0xBC, il k-esimo pieno se
// 0x5dad90 trova una pozione del tipo k+1, altrimenti quello vuoto a +0x604): tipo 1 cura (LB, Spazio), poi i tipi della croce
// giu' e su (g_cfg.dpadPotion; 0.56.25: 3 Mentore W e 5 concentrazione R). Centri di cura, croce giu', croce su, come MemoryTaskbarArts.
int MemoryTaskbarPotions(POINT* centers)
{
    if (!g_ok || g_tbShifting.load()) return 0;
    const DWORD win = VisibleWindow(0, 0x00895ac0);
    if (!win) return 0;
    const int dx = g_tbDxL.load();
    const int idx[3] = { 0, g_cfg.dpadPotion[1] - 1, g_cfg.dpadPotion[0] - 1 };
    for (int i = 0; i < 3; ++i) {
        const DWORD e = win + 0x258 + idx[i] * 0xBC;
        DWORD vt = 0; short wh[2] = {}; int x = 0, y = 0;
        if (!Rd(e, vt) || vt != 0x00896308 || !RdBuf(e + 0x2C, wh, sizeof(wh)) || wh[0] <= 0 || wh[1] <= 0) return 0;
        AbsPos(e, x, y);
        centers[i] = POINT{ x + dx + wh[0] / 2, y + wh[1] / 2 };
    }
    return g_tbShifting.load() ? 0 : 3;
}

// --- Riga "Controller" nel menu Esc (0.15.3) ---------------------------------------------------
// Il menu Esc (cUI_EscMenu, finestra n.12, vtable 0x8954dc; disegno 0x6bb4c0) disegna lo sfondo grande
// quanto la finestra, le voci del vettore [+0x158]..[+0x15c] (cUI_StaticText64, posizione relativa alla
// finestra in +0x24/+0x28; font n. in +0x94, colore in +0x7c) con un'ombra a +3 pixel, poi la cornice
// [+0x168]. Con il menu aperto la mod alza la finestra di una riga e la allunga (la cornice pure),
// spostando le voci in giu' di una riga rispetto alla finestra: le voci restano dove sono e in cima
// resta una riga vuota per "Controller". Alla chiusura i valori originali tornano al loro posto.
// Prima e unica scrittura nella memoria del gioco: solo posizioni e dimensioni di questo menu.
template <class T>
static bool Wr(DWORD addr, const T& v)
{
    SIZE_T n = 0;
    return addr >= 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(addr)),
                                                 &v, sizeof(T), &n) && n == sizeof(T);
}

namespace {
struct EscPatch {
    DWORD win = 0;
    int winY = 0; short winH = 0;
    DWORD items[8] = {}; int itemY[8] = {}; int n = 0;
    // Pezzi della cornice (UI_STATIC_FRAME, figli in [+0x78..+0x7c], creati da 0x725360 una volta sola).
    // 0.15.8: a ogni apertura il gioco AGGIUNGE 32 pezzi nuovi in coda senza togliere i vecchi (prova delle
    // 17:04: 32, 64, 96, 128 pezzi); nella 0.15.6 si modificavano solo i primi 32 e dalla seconda apertura
    // quelli nuovi, disegnati sopra, restavano corti. Nella 0.15.7 il vettore oltre 64 pezzi era scartato
    // e la modifica si ripeteva a ogni fotogramma.
    DWORD parts[512] = {}; int partY[512] = {}; short partH[512] = {}; int np = 0;
    DWORD partsBeg = 0, partsEnd = 0;
    int step = 0;
    bool itemsRel = false;   // posizioni relative alla finestra (genitore +0x50)
} g_esc;
}

// 0.53.3: puntatori contenuti in un vettore del gioco [beg, end) (voci +0x158, pezzi della cornice +0x78), per non
// scrivere in oggetti che il gioco ha nel frattempo liberato (0x6bc870 cancella e ricrea le voci quando cambia pagina).
static std::vector<DWORD> EscVec(DWORD win, DWORD off)
{
    std::vector<DWORD> v;
    DWORD b = 0, e = 0;
    if (!Rd(win + off, b) || !Rd(win + off + 4, e) || e <= b || e - b > 4 * 100000) return v;
    v.resize((e - b) / 4);
    if (!RdBuf(b, v.data(), v.size() * 4)) v.clear();
    std::sort(v.begin(), v.end());
    return v;
}

// 0.53.3: y delle voci senza lo spostamento dell'ombra. Mentre il gioco disegna una voce le scrive x + 3, y + 3 (vedi
// sotto); le voci del menu hanno tutte la stessa x, quindi una voce con x = minima + 3 e' a meta' disegno e la sua y vera
// e' 3 piu' su. Restituisce false se una voce non e' leggibile.
static bool EscItemsY(const DWORD* items, int n, int* ys, bool* drawing)
{
    int xs[8] = {}, minX = INT_MAX;
    for (int i = 0; i < n; ++i) {
        if (!Rd(items[i] + 0x24, xs[i]) || !Rd(items[i] + 0x28, ys[i])) return false;
        if (xs[i] < minX) minX = xs[i];
    }
    for (int i = 0; i < n; ++i) {
        const bool d = xs[i] == minX + 3;
        if (d) ys[i] -= 3;
        if (drawing) drawing[i] = d;
    }
    return true;
}

static void EscRestore()
{
    if (!g_esc.win) return;
    DWORD vt = 0;
    if (Rd(g_esc.win, vt) && vt == 0x008954dc) {
        Wr(g_esc.win + 0x28, g_esc.winY);
        Wr(g_esc.win + 0x2E, g_esc.winH);
        const std::vector<DWORD> items = EscVec(g_esc.win, 0x158);
        int skipped = 0;
        if (g_esc.itemsRel)
            for (int i = 0; i < g_esc.n; ++i) {
                if (std::binary_search(items.begin(), items.end(), g_esc.items[i])) Wr(g_esc.items[i] + 0x28, g_esc.itemY[i]);
                else ++skipped;
            }
        // 0.53.4: i pezzi della cornice a menu chiuso non sono nel vettore +0x78 (prova sulla Steam Machine del 30/10: "17
        // oggetti lasciati stare" a ogni chiusura), ma ci tornano all'apertura dopo (64 pezzi) e la mod li spostava una
        // seconda volta. Si rimettono se sono ancora oggetti dell'interfaccia con il menu come genitore.
        for (int i = 0; i < g_esc.np; ++i) {
            DWORD pvt = 0, par = 0;
            if (!Rd(g_esc.parts[i], pvt) || !IsUiVt(pvt) || !Rd(g_esc.parts[i] + 0x50, par) || par != g_esc.win) { ++skipped; continue; }
            Wr(g_esc.parts[i] + 0x28, g_esc.partY[i]); Wr(g_esc.parts[i] + 0x2E, g_esc.partH[i]);
        }
        if (skipped) Log("Menu Esc: valori originali rimessi (%d oggetti non piu' del menu lasciati stare)", skipped);
        else Log("Menu Esc: valori originali rimessi");
    }
    g_esc = EscPatch{};
}

// Riga libera del menu principale (0.20.1): le voci stanno a passo fisso (32 a 1080p) e fra "Extra" ed "Esci"
// ce n'e' una vuota. Font e colore si leggono dalla voce "Extra" (cUI_StaticText64FX: +0x94, +0x7C come le
// voci del menu Esc).
// 0.49.6: dopo un clic su un'altra voce la riga "Due giocatori" restava sulla nuova schermata per circa un secondo
// (29/10), per le memorie della ricerca completa. Ora la ricerca completa trova le tre voci "Carica", "Extra" ed
// "Esci"; poi a ogni chiamata (e a ogni fotogramma del disegno, 0.49.7) basta leggere le loro bandiere di
// visibilita' e quelle dei genitori, le stesse che il gioco usa per disegnarle.
static bool MainMenuRowHeavy(EscRow& row, DWORD lab[3])
{
    if (!g_ok) return false;
    std::vector<MenuItem> items;
    if (!MemoryMenuItems(items)) return false;
    const MenuItem* extras = nullptr; const MenuItem* quit = nullptr; const MenuItem* load = nullptr;
    for (const MenuItem& it : items) {
        if (strcmp(it.name, "UI_LABEL_EXTRAS") == 0) extras = &it;
        else if (strcmp(it.name, "UI_LABEL_QUIT") == 0) quit = &it;
        else if (strcmp(it.name, "UI_LABEL_LOADGAM") == 0) load = &it;
    }
    if (!extras || !quit || !load) return false;
    lab[0] = load->addr; lab[1] = extras->addr; lab[2] = quit->addr;
    const int step = extras->y - load->y;
    if (step < 16 || step > 80 || quit->y - extras->y < 2 * step) return false;
    row.r.left = extras->x; row.r.top = extras->y + step;
    row.r.right = extras->x + extras->w; row.r.bottom = row.r.top + extras->h;
    // 0.20.2: il colore di "Extra" non e' un ARGB visibile (la riga spariva fuori dall'evidenziazione, 25/10):
    // si usa l'oro delle voci del menu principale e il bianco da evidenziata.
    // 0.51.9: font fisso n. 4 (Carolingia 20, quello delle voci del menu principale). Il "font" letto a +0x94 di Extra
    // non lo e': nei log del 29/10 vale a caso 65055, 28735, 2794, 59701, 58360... Di solito non e' un font valido e il
    // disegno ripiegava gia' sul n. 4; quando capitava un numero piccolo valido la voce usciva minuscola (avvio con il
    // filmato saltato, 0.49.2 e prova del tester del 29/10).
    row.font = 4; row.color = 0xFFE8C257u; row.hoverColor = 0xFFFFFFFFu;
    return true;
}

// Voce visibile per il gioco: bandiera accesa su di lei e su tutti i genitori.
static bool MenuLabelShown(DWORD p)
{
    DWORD vt = 0, f = 0;
    if (!p || !Rd(p, vt) || vt != 0x896758 /* cUI_StaticText64FX */ || !Rd(p + 0x10, f) || !(f & 1)) return false;
    DWORD q = p;
    for (int k = 0; k < 10; ++k) {
        DWORD parent = 0, pf = 0;
        if (!Rd(q + 0x50, parent)) return false;
        if (!parent) return true;
        if (!Rd(parent + 0x10, pf) || !(pf & 1)) return false;
        q = parent;
    }
    return true;
}

static DWORD g_mmLab[3] = {};
static bool g_mmHave = false;

// 0.49.8: in partita (barra in basso presente) il menu principale non c'e', anche se le sue voci vecchie restassero
// in memoria con le bandiere accese: senza questo controllo la riga poteva comparire fino a 1,5 s in partita e, con
// Menu=false, un clic li' avrebbe avviato lo schermo diviso.
static bool InGameUi()
{
    DWORD mgr = 0, w0 = 0;
    return Rd(kUIManager, mgr) && mgr && Rd(mgr + 0x80, w0) && w0;
}

// 0.49.7: chiamata dal disegno (EndScene) a ogni fotogramma, cosi' la riga sparisce nello stesso fotogramma in cui
// il gioco smette di disegnare le sue voci, senza aspettare il giro del thread della mod.
// 0.55.59: prova del 01/11 (foto delle 16:06): passando a un'altra schermata del menu (Extra, Multigiocatore) la voce "Due
// giocatori" restava per circa un secondo sotto le voci nuove, perche' durante il passaggio il gioco lascia accese le bandiere
// delle voci vecchie. Dopo un clic (o Invio, o A) su qualcosa che non e' la voce, la voce si nasconde per 1,5 s; poi torna solo
// se il menu principale c'e' ancora.
std::atomic<DWORD> g_mmHideUntil{ 0 };
void MemoryMainMenuRowHide(DWORD ms) { g_mmHideUntil.store(GetTickCount() + ms); }
bool MemoryMainMenuRowShownNow()
{
    if (static_cast<LONG>(GetTickCount() - g_mmHideUntil.load()) < 0) return false;
    return g_mmHave && !InGameUi() && MenuLabelShown(g_mmLab[0]) && MenuLabelShown(g_mmLab[1]) && MenuLabelShown(g_mmLab[2]);
}

bool MemoryMainMenuRow(EscRow& row)
{
    if (!g_ok) return false;
    DWORD (&lab)[3] = g_mmLab;
    bool& have = g_mmHave;
    static EscRow cached;
    static DWORD heavyAt = 0, verifyAt = 0;
    const DWORD now = GetTickCount();
    if (InGameUi()) return false;
    if (have) {
        if (MenuLabelShown(lab[0]) && MenuLabelShown(lab[1]) && MenuLabelShown(lab[2])) {
            // ogni 1,5 s si controlla con la ricerca completa che siano ancora le voci vere (copie vecchie in memoria)
            if (now - verifyAt < 1500) { row = cached; return true; }
            verifyAt = now;
            EscRow r; DWORD l[3] = {};
            if (MainMenuRowHeavy(r, l) && l[0] == lab[0] && l[1] == lab[1] && l[2] == lab[2]) { cached = r; row = r; return true; }
            have = false;
            return false;
        }
    }
    if (now - heavyAt < 250) return false;
    heavyAt = now;
    EscRow r; DWORD l[3] = {};
    if (!MainMenuRowHeavy(r, l)) return false;     // la cache resta: se le voci tornano visibili la riga torna subito
    memcpy(lab, l, sizeof(lab)); cached = r; have = true; verifyAt = now;
    row = r;
    return true;
}

bool MemoryEscMenuRow(EscRow& row)
{
    if (!g_ok) return false;
    const DWORD win = VisibleWindow(12, 0x008954dc);
    if (!win) { EscRestore(); return false; }
    if (g_esc.win && g_esc.win != win) EscRestore();
    DWORD beg = 0, end = 0;
    if (!Rd(win + 0x158, beg) || !Rd(win + 0x15C, end) || end <= beg) return false;
    const int n = static_cast<int>((end - beg) / 4);
    if (n < 2 || n > 8) return false;
    DWORD it0 = 0, it1 = 0;
    int y0 = 0, y1 = 0;
    if (!Rd(beg, it0) || !Rd(beg + 4, it1) || !Rd(it0 + 0x28, y0) || !Rd(it1 + 0x28, y1)) return false;
    // 0.15.7: riaprendo il menu il gioco (0x6bb7f0) rimette posizione e altezza della finestra e
    // ricrea i pezzi della cornice anche dopo la modifica (prova delle 11:29:47: cornice di nuovo corta,
    // voci gia' spostate). Se succede si rifa' la modifica partendo dai valori originali salvati, senza
    // toccare i vecchi pezzi (liberati).
    EscPatch old;
    if (g_esc.win == win) {
        int wy = 0; DWORD cb = 0, ce = 0;
        Rd(win + 0x28, wy); Rd(win + 0x78, cb); Rd(win + 0x7C, ce);
        if (wy != g_esc.winY - g_esc.step || cb != g_esc.partsBeg || ce != g_esc.partsEnd) {
            static DWORD lastLog = 0;
            if (GetTickCount() - lastLog > 1000) {
                lastLog = GetTickCount();
                Log("Menu Esc: finestra o cornice rifatte dal gioco (y %d, pezzi %08lX..%08lX), modifica ripetuta", wy, cb, ce);
            }
            old = g_esc;
            g_esc = EscPatch{};
        }
    }
    if (g_esc.win == win && g_esc.itemsRel) {
        // 0.53.3: voci ricreate dal gioco (0x6bc870, chiamata da 0x6bbbb0 quando il menu cambia pagina): le nuove sono
        // nella posizione originale e le vecchie sono liberate. Si prendono le nuove come originali e si spostano.
        bool changed = n != g_esc.n;
        for (int i = 0; i < n && !changed; ++i) { DWORD it = 0; changed = !Rd(beg + i * 4, it) || it != g_esc.items[i]; }
        if (changed) {
            bool ok = true;
            for (int i = 0; i < n && ok; ++i) ok = Rd(beg + i * 4, g_esc.items[i]);
            if (ok) ok = EscItemsY(g_esc.items, n, g_esc.itemY, nullptr);
            if (ok) {
                g_esc.n = n;
                for (int i = 0; i < n; ++i) Wr(g_esc.items[i] + 0x28, g_esc.itemY[i] + g_esc.step);
                Log("Menu Esc: voci ricreate dal gioco, spostate di nuovo (%d)", n);
            }
            else { g_esc.n = 0; Log("Menu Esc: voci ricreate dal gioco, non leggibili"); }
        }
        // 0.53.3: foto della prova sulla Steam Machine del 29/10: "Opzioni" era rimasta al suo posto, sotto la riga
        // "Controller", e al suo posto nuovo c'era un buco. Il disegno del menu (0x6bb4c0) per l'ombra di ogni voce scrive
        // x + 3, y + 3, la disegna e poi riscrive x, y letti prima: se lo spostamento della mod (thread della mod) cade fra
        // la lettura e la riscrittura, il gioco rimette la y vecchia. A ogni giro ogni voce che ha di nuovo la y
        // originale si risposta (se e' a +3 il gioco la sta disegnando: si lascia stare).
        // 0.53.6: si rimette solo una voce tornata esattamente alla y originale (la scrittura persa); una y diversa da tutte
        // e due vuol dire voci ricreate dal gioco allo stesso indirizzo (pagina nuova): si prendono come originali.
        int healed = 0, ys[8] = {};
        bool drawing[8] = {}, rebuilt = false;
        if (g_esc.n > 0 && EscItemsY(g_esc.items, g_esc.n, ys, drawing)) {
            // 0.55.49: prova del 01/11 (12:21:43): 115 ms dopo che la mod aveva messo il cursore su "Esporta" una voce era a
            // una y diversa da tutte e due (probabilmente l'evidenziazione sotto il cursore, disegnata senza l'ombra a +3) e la
            // mod ha preso le voci gia' spostate come originali: menu giu' di una riga. Ora serve che almeno due voci, ciascuna
            // a piu' di 4 pixel da tutte e due le posizioni, siano cambiate; una voce sola fuori posto si lascia stare.
            int moved = 0;
            for (int i = 0; i < g_esc.n; ++i)
                if (!drawing[i] && std::abs(ys[i] - g_esc.itemY[i]) > 4 && std::abs(ys[i] - g_esc.itemY[i] - g_esc.step) > 4) ++moved;
            rebuilt = moved >= 2 || (moved == 1 && g_esc.n == 1);
            if (rebuilt) {
                bool ok = true;
                for (int i = 0; i < g_esc.n && ok; ++i) ok = !drawing[i];          // si aspetta un giro senza disegno a meta'
                if (ok) {
                    for (int i = 0; i < g_esc.n; ++i) { g_esc.itemY[i] = ys[i]; Wr(g_esc.items[i] + 0x28, ys[i] + g_esc.step); }
                    Log("Menu Esc: voci rimesse dal gioco in posizioni nuove, spostate di nuovo (%d)", g_esc.n);
                }
            }
            else
                for (int i = 0; i < g_esc.n; ++i)
                    if (!drawing[i] && ys[i] == g_esc.itemY[i]) { Wr(g_esc.items[i] + 0x28, g_esc.itemY[i] + g_esc.step); ++healed; }
        }
        static DWORD healLog = 0;
        if (healed && GetTickCount() - healLog > 2000) { healLog = GetTickCount(); Log("Menu Esc: %d voci rimesse al posto nuovo (spostamento perso)", healed); }
    }
    if (!g_esc.win) {
        EscPatch p;
        p.win = win;
        p.step = y1 - y0;                                  // passo fra le voci (36 a 1080p)
        if (p.step < 16 || p.step > 80) return false;
        if (!Rd(win + 0x28, p.winY) || !Rd(win + 0x2E, p.winH)) return false;
        p.n = n;
        for (int i = 0; i < n; ++i) if (!Rd(beg + i * 4, p.items[i])) return false;
        // 0.53.3: y senza l'ombra del disegno in corso; anche il passo (nel log della Steam Machine del 29/10 "passo 39"
        // con voci a 448, 487, 520, 556, 592: la seconda era letta a meta' disegno, 3 pixel piu' giu')
        if (!EscItemsY(p.items, n, p.itemY, nullptr)) return false;
        p.step = p.itemY[1] - p.itemY[0];
        if (p.step < 16 || p.step > 80) return false;
        if (old.win == win) {                             // valori originali della prima modifica
            p.winY = old.winY; p.winH = old.winH;
            for (int i = 0; i < n && i < old.n; ++i) if (old.items[i] == p.items[i]) p.itemY[i] = old.itemY[i];
        }
        DWORD par = 0, cb0 = 0, ce0 = 0;
        p.itemsRel = Rd(p.items[0] + 0x50, par) && par == win;
        Log("Menu Esc: finestra y %d alta %d, %d voci a passo %d (%s), %lu pezzi di cornice", p.winY, p.winH, n, p.step,
            p.itemsRel ? "relative" : "assolute", (Rd(win + 0x78, cb0) && Rd(win + 0x7C, ce0) && ce0 > cb0) ? (ce0 - cb0) / 4 : 0ul);
        // 0.15.6: la cornice non e' l'oggetto a +0x168 (e' un pulsante invisibile) ma una serie di pezzi
        // UI_STATIC_FRAME figli della finestra, posizionati alla creazione rispetto all'altezza di allora.
        // Pezzi nella meta' inferiore: giu' di una riga; pezzi che attraversano la meta' (lati): piu' lunghi.
        DWORD cb = 0, ce = 0;
        if (Rd(win + 0x78, cb) && Rd(win + 0x7C, ce) && ce > cb && ce - cb <= 4 * 100000) {
            p.partsBeg = cb; p.partsEnd = ce;
            // Dal fondo: gli ultimi pezzi sono quelli dell'apertura in corso, disegnati sopra agli altri
            // (dopo 16 aperture i pezzi sono piu' di 512 e i piu' vecchi restano com'erano).
            for (DWORD a = ce - 4; a >= cb && p.np < 512; a -= 4) {
                DWORD c = 0, cpar = 0; int cy = 0, cx = 0; short ch = 0, cw = 0;
                if (!Rd(a, c) || !c || !Rd(c + 0x28, cy) || !Rd(c + 0x2E, ch)) continue;
                Rd(c + 0x24, cx); Rd(c + 0x2C, cw); Rd(c + 0x50, cpar);
                const int ry = cpar == win ? cy : cy - p.winY, mid = p.winH / 2;
                const bool low = ry >= mid, side = !low && ry + ch > mid;
                if (a >= ce - 4 * 32)                        // solo l'ultimo giro: gli altri sono uguali
                    Log("Menu Esc:   pezzo @%08lX (%d,%d) %dx%d %s%s", c, cx, cy, cw, ch, cpar == win ? "rel " : "ass ",
                        low ? "-> giu'" : side ? "-> piu' lungo" : "");
                if (!low && !side) continue;
                p.parts[p.np] = c; p.partY[p.np] = cy; p.partH[p.np] = ch; ++p.np;
                if (low) Wr(c + 0x28, cy + p.step);
                else Wr(c + 0x2E, static_cast<short>(ch + p.step));
            }
        }
        // Finestra piu' alta di una riga (verso l'alto), voci giu' di una riga dentro la finestra.
        Wr(win + 0x28, p.winY - p.step);
        Wr(win + 0x2E, static_cast<short>(p.winH + p.step));
        if (p.itemsRel) for (int i = 0; i < n; ++i) Wr(p.items[i] + 0x28, p.itemY[i] + p.step);
        g_esc = p;
    }
    // Riga libera: dove sarebbe la voce prima della prima, in coordinate client. 0.18.9: la voce evidenziata
    // ([+0x154]) il gioco la sposta di (3,3), quindi si parte da una voce non evidenziata (i) e si sale di
    // i + 1 righe.
    WORD hov = 0xFFFF;
    Rd(win + 0x154, hov);
    // 0.18.10: [+0x154] non dice quale voce il gioco sposta (con la 0.18.9 la riga si spostava ancora): la
    // riga si calcola dalla finestra, che non si muove, e dalla y originale salvata della prima voce
    // (relativa alla finestra); x = la minima fra le voci (quella evidenziata ha +3).
    int ax = 0, ay = 0;
    short wh[2] = {};
    RdBuf(it0 + 0x2C, wh, sizeof(wh));
    if (g_esc.itemsRel) {
        int wx = 0, wy = 0, minX = INT_MAX;
        AbsPos(win, wx, wy);
        for (int i = 0; i < n && i < g_esc.n; ++i) { int x = 0; if (Rd(g_esc.items[i] + 0x24, x) && x < minX) minX = x; }
        ax = wx + (minX == INT_MAX ? 0 : minX);
        ay = wy + g_esc.itemY[0];
    }
    else { AbsPos(it0, ax, ay); ay -= g_esc.step; }
    row.r = RECT{ ax, ay, ax + wh[0], ay + wh[1] };
    WORD font = 0;
    Rd(it0 + 0x94, font);
    row.font = font;
    // Colore normale e colore della voce evidenziata (quella di [+0x154], se c'e').
    row.color = 0; row.hoverColor = 0;
    for (int i = 0; i < n; ++i) {
        DWORD c = 0;
        if (!Rd(g_esc.items[i] + 0x7C, c)) continue;
        if (i == hov) row.hoverColor = c; else if (!row.color) row.color = c;
    }
    return true;
}

// Equipaggiamento (0.13.0, Ghidra 0x6b7830): finestra cUI_Equipment (n.2, vtable 0x895490) con in
// +0x15c un vettore di 32 caselle (elementi dell'interfaccia: posizione relativa +0x24/+0x28 con
// genitore +0x50, dimensioni +0x2C/+0x2E); la disposizione dipende dalla classe dell'eroe (switch
// su eroe+0x10). Il gioco trova la casella sotto il mouse con il metodo 0x1c di ciascuna.
bool DwItemsFor(int hero, int k, std::vector<MenuItem>& out);   // 0.55.63: due zaini, caselle e pulsanti dove sono disegnati
bool MemoryEquipSlots(std::vector<MenuItem>& out)
{
    out.clear();
    if (!g_ok) return false;
    if (DwItemsFor(1, 1, out)) return !out.empty();
    const DWORD win = VisibleWindow(2, 0x00895490);
    if (!win) return false;
    DWORD slots[32];
    if (!RdBuf(win + 0x15c, slots, sizeof(slots))) return false;
    for (int k = 0; k < 32; ++k) {
        const DWORD e = slots[k];
        short wh[2] = {};
        if (!e || (e & 3) || !RdBuf(e + 0x2C, wh, sizeof(wh)) || wh[0] < 8 || wh[1] < 8) continue;
        MenuItem it;
        it.addr = e;
        Rd(e, it.vt);
        Rd(e + 0x10, it.flags);
        AbsPos(e, it.x, it.y);
        it.w = wh[0]; it.h = wh[1];
        _snprintf_s(it.name, sizeof(it.name), _TRUNCATE, "casella %d", k);
        out.push_back(it);
    }
    return !out.empty();
}

// Scheda del personaggio (0.17.5, Ghidra 0x6a6270): cUI_StatisticsChar (finestra n.8, vtable 0x8952d8) ha
// incorporati da +0x2CC otto pulsanti "+" da 0xBC byte (alzare abilita'/attributi, scegliere dalla lista
// "Nuova abilita'"), visibili col bit 0 di +0x10. Si prendono tutti i cUI_Button2 visibili incorporati
// nell'oggetto (anche le frecce dell'intestazione, se lo sono) e si aggiungono a out.
bool MemoryStatButtons(std::vector<MenuItem>& out)
{
    if (!g_ok) return false;
    { const size_t b0 = out.size(); if (DwItemsFor(1, 2, out)) return out.size() > b0; }
    const DWORD win = VisibleWindow(8, 0x008952d8);
    if (!win) return false;
    std::vector<DWORD> buf(0xF80 / 4);
    if (!RdBuf(win, buf.data(), 0xF80)) return false;
    size_t before = out.size();
    std::string sig;
    for (size_t i = 1; i < buf.size(); ++i) {
        if (buf[i] != 0x00896308) continue;                       // cUI_Button2
        const DWORD b = win + static_cast<DWORD>(i * 4);
        DWORD f = 0; short wh[2] = {};
        if (!Rd(b + 0x10, f) || !(f & 1) || !RdBuf(b + 0x2C, wh, sizeof(wh)) || wh[0] < 6 || wh[1] < 6) continue;
        MenuItem it;
        it.addr = b; it.vt = 0x00896308; it.flags = f;
        AbsPos(b, it.x, it.y);
        it.w = wh[0]; it.h = wh[1];
        _snprintf_s(it.name, sizeof(it.name), _TRUNCATE, "scheda +%03X", static_cast<unsigned>(i * 4));
        out.push_back(it);
        char t[48];
        _snprintf_s(t, sizeof(t), _TRUNCATE, "%X@%d,%d;", static_cast<unsigned>(i * 4), it.x, it.y);
        sig += t;
    }
    static std::string lastSig;
    if (sig != lastSig) {
        lastSig = sig;
        Log("Scheda del personaggio: %u pulsanti visibili %s", static_cast<unsigned>(out.size() - before), sig.c_str());
    }
    return out.size() > before;
}

bool MemoryHeroGrid(GridInfo& g)
{
    if (!g_ok) return false;
    const DWORD win = VisibleWindow(1, kVtInventory3);
    const DWORD page = win ? ActiveTabPage(win, kVtInventoryTab) : 0;
    WORD inv = 0;
    DWORD table = 0;
    if (!page || !Rd(page + 0x158, inv) || inv >= 0x20 || !Rd(kInvTable, table) || !table) return false;
    if (!ReadGrid(table + inv * kInvSize, g)) return false;
    g.inv = inv;
    g.kind = "zaino";
    AbsPos(page, g.pageX, g.pageY);
    return true;
}

// Merce del mercante (0.12.2): finestra cUI_Merchant (n.4), pagina attiva cUI_Merchandise
// (vtable 0x895968, schede Armi/Armatura/Varie) con in [+0x154] la sua cInventoryEntry
// (0x6dab50 la disegna con 0x5dc900, this = [pagina+0x154]).
bool MemoryMerchantGrid(GridInfo& g)
{
    if (!g_ok) return false;
    const DWORD win = VisibleWindow(4, 0x008959b8);
    const DWORD page = win ? ActiveTabPage(win, 0x00895968) : 0;
    DWORD e = 0;
    if (!page || !Rd(page + 0x154, e) || !ReadGrid(e, g)) return false;
    MemoryMerchantFixGrid(g);                       // 0.56.9: con il mercante condiviso l'origine del lato dell'eroe 1
    g.inv = -1;
    g.kind = "merce";
    AbsPos(page, g.pageX, g.pageY);
    return true;
}

// Griglie delle altre finestre (0.18.0, Ghidra): cUI_Blacksmith (vtable 0x895288, disegno 0x6a3510)
// con la cInventoryEntry in [+0x154]; cUI_Chest, il deposito (0x895330, 0x6acd60), in [+0x158];
// cUI_Cube (0x895b88, 0x6e8bb0) disegna la voce 0x19 della tabella degli inventari; cUI_Trade
// (0x895bd4, preparazione 0x6e90a0) ha le due griglie dello scambio in [+0xbbc] (a sinistra) e
// [+0xbc0] (a destra). Le finestre si cercano fra quelle del gestore, visibili. Come per lo zaino
// l'origine nella voce e' in coordinate client (0x6a3510 la confronta con il mouse).
extern LONG g_dwNest;                    // piu' sotto (due zaini)
int MemoryWindowGrids(GridInfo* out, int max)
{
    if (!g_ok || max < 1) return 0;
    DWORD mgr = 0, table = 0;
    if (!Rd(kUIManager, mgr) || !mgr) return 0;
    Rd(kInvTable, table);
    int n = 0;
    auto add = [&](DWORD e, const char* kind, DWORD win) {
        if (n >= max || !e || !ReadGrid(e, out[n])) return;
        out[n].inv = -1; out[n].kind = kind;
        AbsPos(win, out[n].pageX, out[n].pageY);
        ++n;
    };
    for (DWORD off = 0x80; off < 0x200; off += 4) {
        DWORD w = 0, vt = 0, f = 0, e = 0, e2 = 0;
        if (!Rd(mgr + off, w) || !w || (w & 3) || !Rd(w, vt) || !Rd(w + 0x10, f) || !(f & 1)) continue;
        switch (vt) {
        case 0x00895288:
            if (Rd(w + 0x154, e)) {
                const int before = n;
                add(e, "fabbro", w);
                if (n > before) { MemoryMerchantFixGrid(out[before]); out[before].pageX += MemorySharedRealDx(); }   // 0.56.14
            }
            break;
        case 0x00895330:
            if (Rd(w + 0x158, e)) {
                // 0.56.45 (prova del 05/11 alle 11:56, foto: griglia del deposito a x 256 con la finestra a 896, "sfasato"): il
                // costruttore del deposito (0x6acd0e) scrive l'origine della griglia della voce 0x11 una volta sola, dalla x che la
                // finestra ha in quel momento ([+0x24]; y fissa 20); pureHD poi sposta la finestra e l'origine resta indietro (gia'
                // nella 0.55.29, origine (256,20) con la pagina in (896,0); nella 0.18.0 e nella 0.55.1 era (896,20)). Disegno e
                // clic usano tutti e due l'origine: la si rimette alla x della finestra, come la vuole il costruttore.
                int wx = 0, wy = 0;
                WORD ox = 0;
                AbsPos(w, wx, wy);
                // 0.56.54: non durante un disegno della mod (finestra e origine si spostano in due tempi con le due colonne)
                if (g_dwNest == 0 && Rd(e + 0x1164, ox) && wx >= 0 && wx < 4096 && static_cast<int>(static_cast<short>(ox)) != wx) {
                    const WORD nx = static_cast<WORD>(wx);
                    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(e + 0x1164)), &nx, 2, nullptr);
                    Log("DEPOSITO: origine della griglia da x %d a x %d, come la finestra", static_cast<int>(static_cast<short>(ox)), wx);
                }
                const int before = n;
                add(e, "deposito", w);
                if (n > before) { MemoryMerchantFixGrid(out[before]); out[before].pageX += MemorySharedRealDx(); }   // 0.56.54: deposito condiviso
            }
            break;
        case 0x00895b88: if (table) add(table + 0x19 * kInvSize, "cubo", w); break;
        case 0x00895bd4:
            if (Rd(w + 0xbbc, e)) add(e, "scambio sx", w);
            if (Rd(w + 0xbc0, e2)) add(e2, "scambio dx", w);
            break;
        }
    }
    return n;
}

// Comandi delle finestre senza griglia (0.18.2): fabbro (n.3: incastonature, martello, pulsanti accanto al
// ritratto), maestro (n.14, pagine cUI_MasterCombo/Lotto/Portal: rune, casella della combo, pulsante) e
// schede Arti/Combo dell'inventario (cUI_SpellTab, cUI_ComboTab). Ghidra (0x6d2ed0): le pagine tengono i
// loro elementi come puntatori (o incorporati) e li disegnano col metodo 5 (+0x14). Si raccolgono gli
// elementi visibili di classe cUI_Button2, RadioButton2, Combobox, Static2 o Control2, da 12 a 160 pixel
// per lato, cercati nei primi 0x1000 byte della finestra e delle sue pagine visibili (due livelli).
static int g_ctlW = 1920, g_ctlH = 1080;
static bool g_ctlLog = false;

static void CollectControls(DWORD obj, int depth, std::vector<MenuItem>& out, std::vector<DWORD>& seen)
{
    std::vector<DWORD> buf(0x1000 / 4);
    if (!RdBuf(obj, buf.data(), 0x1000)) return;
    for (size_t i = 1; i < buf.size(); ++i) {
        const DWORD v = buf[i];
        DWORD e = 0, vt = 0;
        if (v >= 0x00890000u && v < 0x008a0000u) { e = obj + static_cast<DWORD>(i * 4); vt = v; }   // incorporato
        else if (v >= 0x01000000u && v < 0x7FFF0000u && !(v & 3) && Rd(v, vt) && vt >= 0x00895000u && vt < 0x00896800u) e = v;
        else continue;
        if (e == obj || std::find(seen.begin(), seen.end(), e) != seen.end()) continue;
        DWORD f = 0;
        if (!Rd(e + 0x10, f) || !(f & 1)) continue;
        const bool page = vt == 0x008957e4 || vt == 0x00895834 || vt == 0x00895880 || vt == 0x00895578 ||
                          vt == 0x00895610 || vt == 0x00896258 || vt == 0x008961d8;   // pagine, TabSheet, Window2
        const bool ctl = vt == 0x00896308 || vt == 0x0089633c || vt == 0x008963c0 || vt == 0x0089669c || vt == 0x00896488;
        if (!page && !ctl) continue;
        seen.push_back(e);
        if (page) { if (depth < 2) CollectControls(e, depth + 1, out, seen); continue; }
        short wh[2] = {};
        if (!RdBuf(e + 0x2C, wh, sizeof(wh)) || wh[0] < 12 || wh[1] < 12 || wh[0] > 160 || wh[1] > 160) continue;
        MenuItem it;
        it.addr = e; it.vt = vt; it.flags = f;
        AbsPos(e, it.x, it.y);
        it.w = wh[0]; it.h = wh[1];
        // 0.18.3: fuori dallo schermo (pagine nascoste del maestro) o pezzi di cornice: scartati.
        if (it.x < 0 || it.y < 0 || it.x + it.w > g_ctlW || it.y + it.h > g_ctlH) continue;
        char nm[17] = {};
        RdBuf(e + 0x30, nm, 16);
        if (!strncmp(nm, "UI_STATIC_FRAME", 15) || !strncmp(nm, "UI_FRAME", 8)) continue;
        if (it.w < 24 || it.h < 24) continue;      // frecce, decorazioni piccole, etichette 64x21 della scheda Arti
        _snprintf_s(it.name, sizeof(it.name), _TRUNCATE, "%s+%03X", vt == 0x00896308 ? "btn" : vt == 0x0089669c ? "static" :
                    vt == 0x00896488 ? "ctl" : "altro", static_cast<unsigned>(i * 4));
        out.push_back(it);
        if (g_ctlLog) {
            for (char& c : nm) if (c && (c < 32 || c > 126)) c = '.';
            Log("  comando %s \"%s\" (%d,%d) %dx%d", it.name, nm, it.x, it.y, it.w, it.h);
        }
    }
}

// Maestro (0.18.4, Ghidra 0x6d2ed0): la pagina attiva (cUI_MasterCombo 0x8957e4, cUI_MasterLotto 0x895834,
// cUI_MasterPortal 0x895880) ha i suoi elementi incorporati (per le combo: 4 blocchi da 0x188 byte da +0x154,
// ciascuno con tre elementi a +0, +0x7c, +0xf8, piu' +0x78c e +0x808), di classi senza RTTI. Si prendono gli
// oggetti incorporati o puntati, visibili, il cui genitore (+0x50) e' la pagina, da 20 a 160 pixel per lato.
// Nella 0.18.3 la ricerca generica trovava solo elementi di altre finestre.
static DWORD MasterPage(DWORD master)
{
    // 0.18.6: [maestro+0x154] e' un cUI_TabSheet (log della 0.18.5); la pagina attiva e' quella del suo
    // cUI_TabControl in +0x78 (voci da 24 byte, pagina in +0x14, indice attivo in +0x84), come per il mercante.
    DWORD sheet = 0, svt = 0, ctl = 0, cvt = 0, beg = 0, end = 0, page = 0, f = 0;
    WORD active = 0;
    if (Rd(master + 0x154, sheet) && sheet && !(sheet & 3) && Rd(sheet, svt) && svt == kVtTabSheet &&
        Rd(sheet + 0x78, ctl) && ctl && Rd(ctl, cvt) && cvt == kVtTabControl &&
        Rd(ctl + 0x78, beg) && Rd(ctl + 0x7C, end) && Rd(ctl + 0x84, active) && end > beg &&
        active < (end - beg) / 24 && Rd(beg + active * 24 + 0x14, page) && page && Rd(page + 0x10, f) && (f & 1))
        return page;
    std::vector<DWORD> buf(0x1000 / 4);
    if (!RdBuf(master, buf.data(), 0x1000)) return 0;
    for (size_t i = 1; i < buf.size(); ++i) {
        const DWORD v = buf[i];
        DWORD e = 0, vt = 0, f = 0;
        if (v == 0x008957e4 || v == 0x00895834 || v == 0x00895880) { e = master + static_cast<DWORD>(i * 4); vt = v; }
        else if (v >= 0x01000000u && v < 0x7FFF0000u && !(v & 3) && Rd(v, vt) &&
                 (vt == 0x008957e4 || vt == 0x00895834 || vt == 0x00895880)) e = v;
        else continue;
        if (Rd(e + 0x10, f) && (f & 1)) return e;
    }
    return 0;
}

static void CollectChildren(DWORD page, std::vector<MenuItem>& out)
{
    std::vector<DWORD> buf(0x1000 / 4);
    if (!RdBuf(page, buf.data(), 0x1000)) return;
    std::vector<DWORD> seen;
    for (size_t i = 1; i < buf.size(); ++i) {
        const DWORD v = buf[i];
        DWORD e = 0, parent = 0, f = 0;
        if (v >= 0x00880000u && v < 0x00900000u) e = page + static_cast<DWORD>(i * 4);              // incorporato
        else if (v >= 0x01000000u && v < 0x7FFF0000u && !(v & 3)) e = v;                             // puntato
        else continue;
        DWORD grand = 0;
        if (!Rd(e + 0x50, parent) || !parent || !Rd(e + 0x10, f) || !(f & 1)) continue;
        if (parent != page && !(Rd(parent + 0x50, grand) && grand == page)) continue;    // figli o nipoti
        if (std::find(seen.begin(), seen.end(), e) != seen.end()) continue;
        seen.push_back(e);
        short wh[2] = {};
        if (!RdBuf(e + 0x2C, wh, sizeof(wh)) || wh[0] < 20 || wh[1] < 20 || wh[0] > 160 || wh[1] > 160) continue;
        MenuItem it;
        it.addr = e; Rd(e, it.vt); it.flags = f;
        AbsPos(e, it.x, it.y);
        it.w = wh[0]; it.h = wh[1];
        if (it.x < 0 || it.y < 0 || it.x + it.w > g_ctlW || it.y + it.h > g_ctlH) continue;
        _snprintf_s(it.name, sizeof(it.name), _TRUNCATE, "maestro+%03X", static_cast<unsigned>(i * 4));
        out.push_back(it);
        if (g_ctlLog) {
            char nm[17] = {};
            RdBuf(e + 0x30, nm, 16);
            for (char& c : nm) if (c && (c < 32 || c > 126)) c = '.';
            Log("  comando %s vt %08lX \"%s\" (%d,%d) %dx%d stato %08lX", it.name, it.vt, nm, it.x, it.y, it.w, it.h, f);
        }
    }
}

bool MemoryWindowControls(std::vector<MenuItem>& out, int clientW, int clientH)
{
    if (!g_ok) return false;
    const size_t before = out.size();
    std::vector<DWORD> seen;
    const DWORD smith = VisibleWindow(3, 0x00895288), master = VisibleWindow(14, 0x00895798);
    if (clientW > 0 && clientH > 0) { g_ctlW = clientW; g_ctlH = clientH; }
    // Elenco nel log (una riga per comando) solo quando cambiano le finestre o la pagina.
    DWORD invPage = 0;
    if (const DWORD inv0 = VisibleWindow(1, kVtInventory3)) {
        invPage = ActiveTabPage(inv0, 0x00895578);
        if (!invPage) invPage = ActiveTabPage(inv0, 0x00895610);
    }
    static DWORD lastKey = 0xFFFFFFFF;
    const DWORD key = smith ^ (master << 1) ^ (invPage << 2) ^ (master ? MasterPage(master) << 3 : 0);
    g_ctlLog = key != lastKey;
    lastKey = key;
    if (g_ctlLog) Log("Comandi delle finestre (fabbro %08lX, maestro %08lX, scheda %08lX):", smith, master, invPage);
    const size_t sharedFrom = out.size();
    if (smith) { seen.push_back(smith); CollectControls(smith, 0, out, seen); }
    if (master) {
        const DWORD page = MasterPage(master);
        if (g_ctlLog) {
            // 0.18.5: la pagina attiva e' in [maestro+0x154] (Ghidra 0x6d1260, 0x6d13e0); nella 0.18.4 la
            // ricerca per vtable non la trovava. Diagnostica: gli elementi alle posizioni viste in 0x6d2ed0.
            DWORD pvt = 0, pf = 0; Rd(page, pvt); Rd(page + 0x10, pf);
            Log("  maestro: pagina %08lX vt %08lX stato %08lX", page, pvt, pf);
            static const DWORD offs[] = { 0x154, 0x1d0, 0x24c, 0x2dc, 0x464, 0x5ec, 0x78c, 0x808 };
            for (DWORD o : offs) {
                DWORD vt = 0, f = 0, par = 0; int ax = 0, ay = 0; short wh[2] = {};
                Rd(page + o, vt); Rd(page + o + 0x10, f); Rd(page + o + 0x50, par); RdBuf(page + o + 0x2C, wh, sizeof(wh));
                AbsPos(page + o, ax, ay);
                Log("    +%03lX: vt %08lX stato %08lX genitore %08lX (%d,%d) %dx%d", o, vt, f, par, ax, ay, wh[0], wh[1]);
            }
        }
        if (page) CollectChildren(page, out);
    }
    // 0.56.14: fabbro e Maestro condivisi: il lato dell'eroe 1 e' disegnato spostato
    if (const LONG sdx = MemorySharedRealDx()) for (size_t i = sharedFrom; i < out.size(); ++i) out[i].x += sdx;
    if (invPage) {
        const size_t b0 = out.size();
        seen.push_back(invPage); CollectControls(invPage, 0, out, seen);   // schede Combo, Arti
        // 0.55.63: con i due zaini lo zaino dell'eroe 1 e' disegnato spostato: i comandi della sua pagina dove si vedono
        if (MemoryDualActive()) { const LONG dx = g_dwInvRealDx.load(); for (size_t i = b0; i < out.size(); ++i) out[i].x += dx; }
    }
    if (g_ctlLog) Log("Comandi delle finestre: %u", static_cast<unsigned>(out.size() - before));
    g_ctlLog = false;
    return out.size() > before;
}

// Casella idx: true se occupata; anchor = indice della casella d'origine dell'oggetto, w/h sue dimensioni.
bool MemoryGridCell(const GridInfo& g, int idx, int& anchor, int& w, int& h)
{
    BYTE c[12];
    if (idx < 0 || idx >= g.cols * g.rows || !RdBuf(g.entry + 0x18 + idx * 12, c, sizeof(c))) return false;
    if (!(c[4] & 1)) return false;
    DWORD ref = 0;
    memcpy(&ref, c, 4);
    if (ref) { anchor = idx; w = c[5]; h = c[6]; }
    else {
        anchor = c[6] * g.cols + c[5];                   // colonna e riga d'origine
        BYTE a[12];
        if (anchor < 0 || anchor >= g.cols * g.rows || !RdBuf(g.entry + 0x18 + anchor * 12, a, sizeof(a))) return false;
        w = a[5]; h = a[6];
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    return true;
}

// --- Pulsanti dei menu (0.12.3) -------------------------------------------------------------
// Finestre: le 23 del cUI_Manager (+0x80..+0xD8) e quelle del menu principale create da
// 0x75a170 (+0x130 cUI_MainMenu, +0x134, +0x138 opzioni, +0x13c salvataggi, +0x140 ...).
// Dentro ogni finestra visibile si seguono i puntatori (finestra 0x400 byte, poi 0x200 byte di
// ogni oggetto cUI_ trovato, fino a 3 livelli) e si tengono pulsanti, pulsanti di scelta, caselle
// a discesa e testi con effetto (voci del menu principale?), visibili, con dimensioni e con la
// catena dei genitori (+0x50) che arriva alla finestra (in memoria ci sono anche copie vecchie).
static bool IsMenuItemVt(DWORD vt)
{
    return vt == 0x896308 /* cUI_Button2 */ || vt == 0x89633c /* cUI_RadioButton2 */ ||
           vt == 0x8963c0 /* cUI_Combobox */ || vt == 0x896758 /* cUI_StaticText64FX */ ||
           vt == 0x896700 /* cUI_StaticText64 */;
}

// 0.12.5: ricerca in tutta la memoria (come F6, circa 200 ms) degli oggetti pulsante e testo,
// ripetuta solo quando cambia l'insieme delle finestre aperte; a ogni pressione valgono quelli
// visibili con tutti i genitori visibili e la radice fra le finestre aperte del cUI_Manager
// (+0x80..+0x1FC: le 23 finestre, quelle del menu principale e le altre). Cosi' spariscono le
// voci dei sottomenu nascosti (Multigiocatore, Extra) e le copie vecchie, e si trovano i
// pulsanti a qualsiasi profondita' (opzioni, salvataggi, scelta del personaggio).
static std::vector<DWORD> g_menuCache;
static std::vector<DWORD> g_mainMenus;      // oggetti cUI_MainMenu trovati in memoria (0.14.3)
static std::vector<DWORD> g_menuWins;
static DWORD g_menuScanAt = 0;

// 0.18.7: lettura diretta della memoria del processo (protetta da __try: una regione liberata nel
// frattempo da' un'eccezione e si salta) invece di copiarla a blocchi con ReadProcessMemory, e un solo
// confronto (pagina 0x896000 dei vtable) prima dei cinque; la ricerca durava 400-500 ms in partita.
static DWORD g_scanHits[0x4000];
static int ScanChunk(const DWORD* p, size_t n, int count)
{
    __try {
        for (size_t i = 0; i < n; ++i) {
            const DWORD v = p[i];
            if ((v & 0xFFFFF000u) != 0x00896000u) continue;
            if ((IsMenuItemVt(v) || v == 0x896004) && count < static_cast<int>(_countof(g_scanHits)))
                g_scanHits[count++] = static_cast<DWORD>(reinterpret_cast<uintptr_t>(p + i));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

static void ScanMenuObjects()
{
    g_menuCache.clear();
    g_mainMenus.clear();
    int count = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0xFFFE0000u; ) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD))
            count = ScanChunk(reinterpret_cast<const DWORD*>(base), size / 4, count);
        a = base + size;
        if (size == 0) break;
    }
    for (int i = 0; i < count; ++i) {
        DWORD v = 0;
        if (!Rd(g_scanHits[i], v)) continue;
        if (v == 0x896004) g_mainMenus.push_back(g_scanHits[i]);
        else if (IsMenuItemVt(v)) g_menuCache.push_back(g_scanHits[i]);
    }
}

// Pulsanti della scelta del personaggio (Inizia partita, Indietro, Importa, Hardcore): li punta
// anche il menu principale e il loro bit visibile resta acceso, quindi valgono solo con la
// finestra cUI_Character del menu principale aperta (0.12.9; nella 0.12.8 il menu principale
// "diventava" la scelta del personaggio, prova delle 20:01).
static bool IsCharSelectButton(const char* nm)
{
    return strncmp(nm, "UI_BTN_ACCEPT", 13) == 0 || strncmp(nm, "UI_BTN_BACK", 11) == 0 ||
           strncmp(nm, "UI_BTN_TOGGLE", 13) == 0 || strncmp(nm, "UI_BTN_HARDCORE", 15) == 0;
}

// Byte dell'oggetto finestra in cui cercare i pulsanti che possiede. 0x9000 copre cUI_Savegame (0x826c);
// il menu Esc e' piccolo e oltre ci sono altri oggetti: nella prova delle 11:29:48 c'erano i pulsanti
// nascosti delle opzioni e la croce ci saltava sopra (0.15.7).
// 0.18.7: 0x9000 solo per le finestre del menu che ne hanno bisogno (salvataggi, opzioni, scelta del
// personaggio); il menu principale e il menu Esc 0x300; le finestre di gioco 0x1000. Col maestro aperto e
// col menu principale dopo una partita (prove della 0.18.4-0.18.5) la lettura di 0x9000 byte arrivava ai
// pulsanti nascosti delle opzioni (UI_BTN_SNDQ_..., UI_BTN_DETAILSET...) e la croce ci saltava sopra.
static DWORD WinExtent(DWORD w)
{
    DWORD vt = 0;
    if (!Rd(w, vt)) return 0x1000;
    if (vt == 0x008954dc || vt == 0x00896004) return 0x300;                         // menu Esc, menu principale
    if (vt == 0x008960e4 || vt == 0x0089605c || vt == 0x00895fa8) return 0x9000;    // salvataggi, opzioni, personaggio
    return 0x1000;
}

// Menu principale (0.18.7, Ghidra 0x712540): [+0x154] = schermata (0 principale, 1 Multigiocatore, 2 Extra,
// 3 titoli di coda) e il gioco disegna solo le voci del vettore di quella schermata: [+0x158..+0x15c],
// [+0x164..+0x168], [+0x170..+0x174]. Le voci delle altre schermate hanno il bit visibile acceso lo stesso:
// si raccolgono per scartarle.
static void HiddenMainMenuItems(const std::vector<DWORD>& wins, std::vector<DWORD>& hidden)
{
    hidden.clear();
    for (DWORD w : wins) {
        DWORD vt = 0, state = 0;
        if (!Rd(w, vt) || vt != 0x00896004 || !Rd(w + 0x154, state)) continue;
        for (DWORD k = 0; k < 3; ++k) {
            if (k == state) continue;
            DWORD beg = 0, end = 0;
            if (!Rd(w + 0x158 + k * 12, beg) || !Rd(w + 0x15c + k * 12, end) || end < beg || end - beg > 0x100) continue;
            std::vector<DWORD> v((end - beg) / 4);
            if (!v.empty() && RdBuf(beg, v.data(), v.size() * 4)) hidden.insert(hidden.end(), v.begin(), v.end());
        }
    }
    std::sort(hidden.begin(), hidden.end());
}

// F9 (0.20.3): elenco di tutti i pulsanti e testi visibili con nome, classe, posizione e radice, anche se la
// finestra non e' fra quelle note (schermate di rete: cUI_Network, lobby). Serve a ricavare i nomi per
// l'ingresso automatico in partita.
void MemoryNetDump()
{
    if (!g_ok) return;
    static int n = 0;
    ++n;
    ScanMenuObjects();
    Log("NETDUMP n.%d: %u oggetti candidati", n, static_cast<unsigned>(g_menuCache.size()));
    int printed = 0;
    for (DWORD p : g_menuCache) {
        DWORD vt = 0, f = 0; short wh[2] = {};
        if (!Rd(p, vt) || !IsMenuItemVt(vt) || !Rd(p + 0x10, f) || !(f & 1)) continue;
        if (!RdBuf(p + 0x2C, wh, sizeof(wh)) || wh[0] < 4 || wh[1] < 4) continue;
        DWORD q = p, root = 0;
        bool ok = true;
        for (int k = 0; k < 10; ++k) {
            DWORD parent = 0, pf = 0;
            if (!Rd(q + 0x50, parent)) { ok = false; break; }
            if (!parent) { root = q; break; }
            if (!Rd(parent + 0x10, pf) || !(pf & 1)) { ok = false; break; }
            q = parent;
        }
        if (!ok) continue;
        char nm[17] = {}, cn[40] = "", rc[40] = "";
        RdBuf(p + 0x30, nm, 16);
        for (char& c : nm) if (c && (c < 32 || c > 126)) c = '.';
        ClassName(vt, cn, sizeof(cn));
        DWORD rv = 0;
        if (root && Rd(root, rv)) ClassName(rv, rc, sizeof(rc));
        int ax = 0, ay = 0;
        AbsPos(p, ax, ay);
        if (printed++ < 200)
            Log("NETDUMP @%08lX %-18s \"%s\" (%d,%d) %dx%d stato %08lX radice @%08lX %s", p, cn, nm, ax, ay, wh[0], wh[1], f, root, rc);
    }
}

// Finestre vive del gestore (0.20.4): quelle con la bandiera di visibilita' nelle posizioni +0x80..+0x200.
static void LiveWindows(std::vector<DWORD>& wins)
{
    wins.clear();
    DWORD mgr = 0;
    if (!Rd(kUIManager, mgr) || !mgr) return;
    for (DWORD off = 0x80; off < 0x200; off += 4) {
        DWORD w = 0, f = 0, vt = 0;
        if (Rd(mgr + off, w) && w && !(w & 3) && Rd(w, vt) && IsUiVt(vt) && Rd(w + 0x10, f) && (f & 1))
            if (std::find(wins.begin(), wins.end(), w) == wins.end()) wins.push_back(w);
    }
}

// C'e' una finestra viva (visibile) di questa classe, per esempio "cUI_Network", "cUI_NetLobby"?
bool MemoryLiveWindowClass(const char* cls)
{
    if (!g_ok) return false;
    std::vector<DWORD> wins;
    LiveWindows(wins);
    for (DWORD w : wins) {
        DWORD vt = 0; char cn[40] = "";
        if (Rd(w, vt)) ClassName(vt, cn, sizeof(cn));
        if (strcmp(cn, cls) == 0) return true;
    }
    return false;
}

// Pulsante o testo di nome (anche troncato a 16 caratteri) contenuto in una finestra viva: gli oggetti di
// schermate chiuse restano in memoria con le bandiere di prima, ma non stanno dentro una finestra viva.
bool MemoryNetButton(const char* name, RECT& r)
{
    if (!g_ok) return false;
    static DWORD scanAt = 0;
    const DWORD now = GetTickCount();
    if (g_menuCache.empty() || now - scanAt > 400) { ScanMenuObjects(); scanAt = GetTickCount(); }
    std::vector<DWORD> wins;
    LiveWindows(wins);
    const size_t len = strlen(name) < 16 ? strlen(name) : 16;
    for (DWORD p : g_menuCache) {
        DWORD vt = 0, f = 0; short wh[2] = {};
        if (!Rd(p, vt) || !IsMenuItemVt(vt) || !Rd(p + 0x10, f) || !(f & 1)) continue;
        if (!RdBuf(p + 0x2C, wh, sizeof(wh)) || wh[0] < 4 || wh[1] < 4) continue;
        char nm[17] = {};
        RdBuf(p + 0x30, nm, 16);
        if (strncmp(nm, name, len) != 0) continue;
        bool inside = false;
        for (DWORD w : wins) if (p > w && p < w + WinExtent(w)) inside = true;
        if (!inside) continue;
        int ax = 0, ay = 0;
        AbsPos(p, ax, ay);
        r.left = ax; r.top = ay; r.right = ax + wh[0]; r.bottom = ay + wh[1];
        return true;
    }
    return false;
}

bool MemoryMenuItems(std::vector<MenuItem>& out, bool* charSelect)
{
    out.clear();
    if (charSelect) *charSelect = false;
    if (!g_ok) return false;
    DWORD mgr = 0;
    if (!Rd(kUIManager, mgr) || !mgr) return false;
    std::vector<DWORD> wins;
    for (DWORD off = 0x80; off < 0x200; off += 4) {
        if (off == 0x80 || off == 0x80 + 6 * 4 || off == 0x80 + 7 * 4 || off == 0x80 + 9 * 4 ||
            off == 0x80 + 18 * 4 || off == 0x80 + 19 * 4) continue;               // sempre visibili
        DWORD w = 0, f = 0, vt = 0;
        if (Rd(mgr + off, w) && w && !(w & 3) && Rd(w, vt) && IsUiVt(vt) && Rd(w + 0x10, f) && (f & 1))
            if (std::find(wins.begin(), wins.end(), w) == wins.end()) wins.push_back(w);
    }
    // 0.14.3: tornando al menu principale dopo una partita il gestore non ha finestre aperte (prova del
    // 24/10 alle 9:53:47): si usa il cUI_MainMenu visibile trovato in memoria.
    if (wins.empty()) {
        // 0.18.4: prima il menu principale gia' conosciuto; nuova ricerca solo se non ce n'e' uno visibile
        // (nella 0.18.3, alle 2:38:20, l'ultima ricerca era del menu Esc di un momento prima e la croce
        // nel menu principale non trovava nulla).
        auto visibleMains = [&]() {
            for (DWORD m : g_mainMenus) {
                DWORD vt = 0, f = 0;
                if (Rd(m, vt) && vt == 0x896004 && Rd(m + 0x10, f) && (f & 1)) wins.push_back(m);
            }
        };
        visibleMains();
        if (wins.empty() && GetTickCount() - g_menuScanAt > 300) {
            ScanMenuObjects(); g_menuScanAt = GetTickCount(); g_menuWins.clear();
            visibleMains();
        }
        if (wins.empty()) return false;
        static DWORD loggedMain = 0;
        if (loggedMain != wins[0]) { loggedMain = wins[0]; Log("Menu: menu principale fuori dal gestore @%08lX", wins[0]); }
    }
    bool charSel = false;
    for (DWORD off = 0x130; off < 0x200; off += 4) {      // cUI_Character fra le finestre del menu principale
        DWORD w = 0, vt = 0, f = 0;
        if (Rd(mgr + off, w) && w && !(w & 3) && Rd(w, vt) && vt == 0x895fa8 && Rd(w + 0x10, f) && (f & 1)) charSel = true;
    }
    if (charSelect) *charSelect = charSel;
    std::sort(wins.begin(), wins.end());
    const DWORD now = GetTickCount();
    // 0.20.12: nell'host co-op la lista restava quella della lobby (la voce "Esci" del menu Esc non si navigava con la croce,
    // 25/10): oltre che al cambio delle finestre si ripete la ricerca ogni 1,5 s (circa 60 ms).
    if (wins != g_menuWins || now - g_menuScanAt > 1500) {
        const DWORD t0 = now;
        const bool changed = (wins != g_menuWins);
        ScanMenuObjects();
        g_menuWins = wins;
        g_menuScanAt = GetTickCount();
        if (changed)
            Log("Menu: ricerca dei pulsanti in memoria, %u oggetti in %lu ms (%u finestre aperte)",
                static_cast<unsigned>(g_menuCache.size()), g_menuScanAt - t0, static_cast<unsigned>(wins.size()));
    }
    // 0.12.7: pulsanti senza genitore (+0x50 = 0, come quelli dei salvataggi e della scelta del
    // personaggio, prova delle 19:13): valgono se una finestra aperta li punta, direttamente nel
    // suo oggetto (letto fino a 0x9000 byte: cUI_Savegame e' grande 0x826c) o un livello sotto
    // (vettori e oggetti a cui punta, 0x200 byte ciascuno). Il loro bit "visibile" resta acceso
    // anche a finestra chiusa, quindi senza questo legame non valgono.
    static std::vector<DWORD> owned, ownedWins;
    static DWORD ownedAt = 0;
    if (wins != ownedWins || GetTickCount() - ownedAt > 2000) {      // ricalcolo al cambio di finestre
        ownedWins = wins;
        ownedAt = GetTickCount();
        owned.clear();
        std::vector<DWORD> sorted(g_menuCache);
        std::sort(sorted.begin(), sorted.end());
        auto isItem = [&](DWORD q) { return std::binary_search(sorted.begin(), sorted.end(), q); };
        for (DWORD w : wins) {
            std::vector<DWORD> mem;
            const DWORD ext = WinExtent(w);
            for (DWORD off = 0; off < ext; off += 0x1000) {
                DWORD chunk[0x1000 / 4];
                if (!RdBuf(w + off, chunk, sizeof(chunk))) break;
                mem.insert(mem.end(), chunk, chunk + (ext - off < 0x1000 ? (ext - off) / 4 : 0x400));
            }
            for (DWORD q : mem) {
                if (q < 0x01000000u || q >= 0x7FFF0000u || (q & 3)) continue;
                if (isItem(q)) { owned.push_back(q); continue; }
                DWORD sub[0x200 / 4];
                if (!RdBuf(q, sub, sizeof(sub))) continue;
                for (DWORD r : sub) if (r >= 0x01000000u && r < 0x7FFF0000u && !(r & 3) && isItem(r)) owned.push_back(r);
            }
        }
        std::sort(owned.begin(), owned.end());
        owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
    }
    std::vector<DWORD> hiddenMain;
    HiddenMainMenuItems(wins, hiddenMain);
    int logged = 0;
    static std::vector<DWORD> lastWins;
    const bool explain = (wins != lastWins);          // una volta per insieme di finestre
    for (DWORD p : g_menuCache) {
        DWORD vt = 0, f = 0; short wh[2] = {};
        if (!Rd(p, vt) || !IsMenuItemVt(vt) || !Rd(p + 0x10, f) || !(f & 1)) continue;
        if (!RdBuf(p + 0x2C, wh, sizeof(wh)) || wh[0] < 4 || wh[1] < 4) continue;
        if (std::binary_search(hiddenMain.begin(), hiddenMain.end(), p)) continue;   // altra schermata del menu principale
        // Testi: solo le voci cliccabili (0x80605 nel menu principale e nel menu Esc; le cornici
        // delle opzioni hanno 0x80005).
        char nm0[10] = {};
        RdBuf(p + 0x30, nm0, 9);
        const bool slider = memcmp(nm0, "UI_SLIDER", 9) == 0;   // barre del volume (0x100005)
        if ((vt == 0x896700 || vt == 0x896758) && !(f & 0x600) && !slider) continue;
        // Genitori tutti visibili, radice fra le finestre aperte.
        DWORD q = p, root = 0;
        bool ok = true;
        for (int k = 0; k < 10; ++k) {
            DWORD parent = 0, pf = 0;
            if (!Rd(q + 0x50, parent)) { ok = false; break; }
            if (!parent) { root = q; break; }
            if (!Rd(parent + 0x10, pf) || !(pf & 1)) { ok = false; break; }
            q = parent;
        }
        char nmc[17] = {};
        RdBuf(p + 0x30, nmc, 16);
        if (IsCharSelectButton(nmc)) {
            if (!charSel) continue;
            bool dup = false;                             // copie con lo stesso nome: una sola
            for (const MenuItem& o : out) if (strcmp(o.name, nmc) == 0) dup = true;
            if (dup) continue;
        }
        const bool linked = ok && std::binary_search(wins.begin(), wins.end(), root);
        // 0.13.1: anche i pulsanti contenuti nell'oggetto della finestra (Carica/Salva/Cancella/Indietro
        // dei salvataggi stanno a +0x764.. dentro cUI_Savegame, prova delle 20:10).
        bool inside = false;
        for (DWORD w : wins) if (p > w && p < w + WinExtent(w)) inside = true;
        const bool ownedByWin = (root == p) && (inside || std::binary_search(owned.begin(), owned.end(), p));
        if (!linked && !ownedByWin && !(charSel && IsCharSelectButton(nmc))) {
            // Diagnostica (0.12.6): il primo motivo di scarto dei pulsanti visibili.
            if (explain && logged < 12) {
                ++logged;
                char nm[17] = {}, cn[40] = "";
                RdBuf(p + 0x30, nm, 16);
                for (char& c : nm) if (c && (c < 32 || c > 126)) c = '.';
                DWORD rv = 0;
                if (root && Rd(root, rv)) ClassName(rv, cn, sizeof(cn));
                int ax = 0, ay = 0;
                AbsPos(p, ax, ay);
                Log("Menu: scartato @%08lX \"%s\" (%d,%d) stato %08lX: %s (genitore @%08lX, radice @%08lX %s)", p, nm, ax, ay, f,
                    ok ? "radice non fra le finestre aperte" : "un genitore non e' visibile", q, root, cn);
            }
            continue;
        }
        MenuItem it;
        it.addr = p; it.vt = vt; it.flags = f; it.slider = slider;
        AbsPos(p, it.x, it.y);
        it.w = wh[0]; it.h = wh[1];
        RdBuf(p + 0x30, it.name, 16);
        it.name[16] = 0;
        for (char& c : it.name) if (c && (c < 32 || c > 126)) c = '.';
        out.push_back(it);
    }
    // Salvataggi (0.16.0, Ghidra 0x71d3b0): le righe della lista non sono pulsanti; cUI_Savegame
    // (vtable 0x8960e4) ha a +0x72c quattro rettangoli da 12 byte {x, y int; larghezza, altezza short}
    // (salvataggio rapido e le tre righe visibili) e il clic dentro il rettangolo i chiama 0x71be70(i),
    // che seleziona la riga. Diventano destinazioni della croce.
    for (DWORD w : wins) {
        DWORD vt = 0;
        if (!Rd(w, vt) || vt != 0x008960e4) continue;
        static DWORD loggedSave = 0;
        for (int i = 0; i < 4; ++i) {
            int xy[2] = {}; short wh[2] = {};
            if (!RdBuf(w + 0x72c + i * 12, xy, sizeof(xy)) || !RdBuf(w + 0x734 + i * 12, wh, sizeof(wh))) continue;
            if (loggedSave != w) Log("Salvataggi: riga %d (%d,%d) %dx%d", i, xy[0], xy[1], wh[0], wh[1]);
            if (wh[0] < 8 || wh[1] < 8 || xy[0] < 0 || xy[1] < 0) continue;
            MenuItem it;
            it.addr = w + 0x72c + i * 12;
            it.x = xy[0]; it.y = xy[1]; it.w = wh[0]; it.h = wh[1];
            _snprintf_s(it.name, sizeof(it.name), _TRUNCATE, "SAVE_ROW_%d", i);
            out.push_back(it);
        }
        loggedSave = w;
    }
    if (explain) {
        lastWins = wins;
        if (out.empty()) {
            for (DWORD w : wins) {
                DWORD vt = 0; char cn[40] = "";
                if (Rd(w, vt)) ClassName(vt, cn, sizeof(cn));
                Log("Menu: finestra aperta @%08lX %s", w, cn);
            }
        }
    }
    return !out.empty();
}

int MemoryUIState()
{
    if (!g_ok) return -1;
    DWORD mgr = 0, win[23];
    if (!Rd(kUIManager, mgr) || !mgr || !RdBuf(mgr + 0x80, win, sizeof(win))) return -1;
    if (!win[0]) return -1;
    for (int i = 1; i < 23; ++i) {
        // 7 = ritratti dei mercenari/compagni a destra, sempre visibili durante il gioco
        if (i == 6 || i == 7 || i == 9 || i == 18 || i == 19 || !win[i]) continue;
        DWORD f = 0;
        if (Rd(win[i] + 0x10, f) && (f & 1)) {
            static int lastOpen = -1;
            if (i != lastOpen) {
                char name[64] = "?"; DWORD vt = 0;
                if (Rd(win[i], vt)) ClassName(vt, name, sizeof(name));
                Log("UI: finestra aperta n.%d (%s)", i, name);
                lastOpen = i;
            }
            return 1;
        }
    }
    return 0;
}

// Finestra dei dialoghi con gli NPC: un cUI_Tooltip di nome "UI_WND_TOOLTIP"
// (non e' tra le 23 del cUI_Manager). Con F6 il 22/10/2026: +0x10 dispari da
// visibile (0x805), +0x24 x, +0x28 y, +0x2C larghezza (16 bit bassi) e altezza
// (16 alti); 752 pixel di larghezza sia per il dialogo con OK sia per quello
// con Accetta/Rifiuta. Si trova con una ricerca nella memoria (circa 150 ms),
// fatta una volta e ripetuta solo se l'oggetto non e' piu' valido.
namespace {
// Nella memoria ci sono anche copie vecchie dell'oggetto (stessa classe e nome),
// quindi si tengono tutti i candidati e vale solo quello che e' stato visto
// almeno una volta nascosto e poi visibile (la 0.7.6 prendeva una copia).
// 0.55.27: actedAt/sig/stale/loggedNew per le finestre mai viste nascoste accettate dopo il caricamento (MemoryDialogTrustNew)
struct DlgCand { DWORD addr; bool seenHidden; DWORD actedAt = 0, sig = 0; bool stale = false, loggedNew = false; };
std::vector<DlgCand> g_dlgs;
bool g_dlgSearched = false;
DWORD g_dlgSearch = 0;
DWORD g_dlgTrustUntil = 0;
// Impronta dell'oggetto della finestra fino ai pulsanti (+0x2180, 4 voci da 0x14): una copia vecchia non cambia mai.
DWORD DialogSig(DWORD a)
{
    static BYTE buf[0x21D0];
    if (!RdBuf(a, buf, sizeof(buf))) return 0;
    DWORD h = 2166136261u;
    for (BYTE c : buf) { h ^= c; h *= 16777619u; }
    return h;
}
bool IsDialogWindow(DWORD a)
{
    DWORD vt = 0; char nm[16] = {};
    return Rd(a, vt) && vt == 0x895b3c && RdBuf(a + 0x30, nm, 14) && memcmp(nm, "UI_WND_TOOLTIP", 14) == 0;
}
// 0.55.32 (controllo del 31/10): l'elenco ha un tetto di 64 e si azzerava solo caricando una partita, ma il gioco crea una
// finestra nuova a ogni conversazione (prova delle 13:10: 22 candidati al caricamento, poi uno per dialogo). Dopo una
// quarantina di conversazioni senza caricare, le finestre nuove non entravano piu' e A e B nei dialoghi smettevano di andare.
// Con l'elenco pieno si tolgono prima le finestre distrutte (vtable o nome non piu' validi), poi le nascoste (fino a 48).
void PruneDialogCands()
{
    const size_t before = g_dlgs.size();
    g_dlgs.erase(std::remove_if(g_dlgs.begin(), g_dlgs.end(), [](const DlgCand& c) { return !IsDialogWindow(c.addr); }), g_dlgs.end());
    for (size_t i = 0; g_dlgs.size() > 48 && i < g_dlgs.size(); ) {
        DWORD f = 0;
        if (Rd(g_dlgs[i].addr + 0x10, f) && !(f & 1)) g_dlgs.erase(g_dlgs.begin() + static_cast<ptrdiff_t>(i));
        else ++i;
    }
    Log("Dialoghi: elenco pieno, tolti %u candidati (distrutti o nascosti), ne restano %u", static_cast<unsigned>(before - g_dlgs.size()),
        static_cast<unsigned>(g_dlgs.size()));
}
// Finestre dei dialoghi in [base, base + size), memoria privata leggibile che finisce in regionEnd, a pezzi da 1 MB.
// 0.55.28: ogni pezzo si legge con 64 byte in piu' (se la regione li ha), cosi' un oggetto a cavallo di due pezzi si trova
// (prima la vtable in fondo a un pezzo e il nome nel successivo non si vedevano mai, sempre per lo stesso indirizzo).
void ScanDialogRange(uintptr_t base, uintptr_t size, uintptr_t regionEnd, bool add)
{
    static std::vector<DWORD> buf;
    for (uintptr_t off = 0; off < size; off += 0x100000) {
        const size_t len = (size - off) < 0x100000 ? static_cast<size_t>(size - off) : 0x100000;
        size_t rd = len + 0x40;
        if (base + off + rd > regionEnd) rd = static_cast<size_t>(regionEnd - (base + off));
        rd &= ~static_cast<size_t>(3);
        if (rd < 4) continue;
        buf.resize(rd / 4);
        if (!RdBuf(static_cast<DWORD>(base + off), buf.data(), rd)) continue;
        for (size_t i = 0; i < len / 4 && i + 16 < buf.size(); ++i) {
            if (buf[i] != 0x895b3c || buf[i + 12] != 0x575f4955) continue;   // "UI_W" a +0x30
            DWORD cand = static_cast<DWORD>(base + off + i * 4);
            if (cand >= reinterpret_cast<uintptr_t>(buf.data()) &&
                cand < reinterpret_cast<uintptr_t>(buf.data() + buf.size())) continue;   // la nostra copia
            if (!IsDialogWindow(cand)) continue;
            bool known = false;
            for (const DlgCand& c : g_dlgs) if (c.addr == cand) known = true;
            if (!known && g_dlgs.size() >= 64) PruneDialogCands();
            if (!known && g_dlgs.size() < 64) {
                g_dlgs.push_back({ cand, add });
                if (add) {
                    DWORD f = 0, pos[3] = {};
                    Rd(cand + 0x10, f); RdBuf(cand + 0x24, pos, sizeof(pos));
                    Log("Dialoghi: nuovo candidato @%08lX +10=%08lX pos (%lu,%lu) %lux%lu", cand, f,
                        pos[0], pos[1], pos[2] & 0xffff, pos[2] >> 16);
                }
            }
        }
    }
}

// add = true: si aggiungono solo i candidati nuovi, considerati affidabili (la
// ricerca si ripete subito dopo aver parlato con un NPC, a dialogo aperto: la
// finestra vera viene creata dal gioco solo al primo dialogo).
// nearOnly = true: solo le regioni di memoria che contengono gia' un candidato (le
// finestre dei dialoghi nascono accanto alle altre: pochi millisecondi). 0.55.28: non e' vero sulla Steam Machine (prova del
// 31/10 alle 11:20: il dialogo dell'eroe 2 @155E81F0 lontano da tutti i candidati, trovato solo dopo 16 s): durante l'attesa
// di un dialogo si usa MemoryDialogScanStep, la ricerca completa a fette.
void FindDialogWindows(bool add = false, bool nearOnly = false)
{
    if (!add) g_dlgs.clear();
    MEMORY_BASIC_INFORMATION mbi;
    // Ricerca vicina: da 2 MB prima del primo candidato a 2 MB dopo l'ultimo
    // (i dialoghi nascono a 0x40000-0x60000 byte dalle finestre gia' note).
    uintptr_t lo = 0x10000, hi = 0xFFFE0000u;
    if (nearOnly) {
        if (g_dlgs.empty()) return;
        DWORD mn = 0xFFFFFFFFu, mx = 0;
        for (const DlgCand& c : g_dlgs) { mn = c.addr < mn ? c.addr : mn; mx = c.addr > mx ? c.addr : mx; }
        lo = mn > 0x210000 ? mn - 0x200000 : 0x10000;
        hi = static_cast<uintptr_t>(mx) + 0x200000;
    }
    for (uintptr_t a = lo; a < hi; ) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) break;
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), size = mbi.RegionSize;
        if (size == 0) break;
        const uintptr_t regionEnd = base + size;
        if (nearOnly) {          // solo la parte della regione dentro la finestra [lo, hi)
            uintptr_t b2 = base < lo ? lo : base, e2 = (base + size) > hi ? hi : (base + size);
            if (e2 <= b2) { a = base + size; continue; }
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
                base = b2 & ~static_cast<uintptr_t>(3); size = e2 - base;
            }
            else { a = base + size; continue; }
        }
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD))
            ScanDialogRange(base, size, regionEnd, add);
        a = base + size;
    }
}
}

// Pulsanti del dialogo: il gestore dei clic del cUI_Tooltip (0x6e7490) scorre 4
// voci da 0x14 byte a partire da +0x2180: [+0] valore della risposta, [+4] x e
// [+8] y relativi alla finestra, [+0xC] larghezza e [+0xE] altezza (16 bit).
int ReadDialogButtons(DWORD a, int wx, int wy, RECT* out)
{
    BYTE raw[4 * 0x14];
    if (!RdBuf(a + 0x2180, raw, sizeof(raw))) return 0;
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        const BYTE* e = raw + i * 0x14;
        int bx = *reinterpret_cast<const int*>(e + 4), by = *reinterpret_cast<const int*>(e + 8);
        int bw = *reinterpret_cast<const short*>(e + 12), bh = *reinterpret_cast<const short*>(e + 14);
        if (bw <= 0 || bh <= 0 || bw > 1000 || bh > 300 || bx < 0 || by < 0 || bx > 2000 || by > 2000) continue;
        out[n].left = wx + bx; out[n].top = wy + by; out[n].right = wx + bx + bw; out[n].bottom = wy + by + bh;
        ++n;
    }
    return n;
}

bool MemoryDialog(DialogInfo& d)
{
    if (!g_ok) return false;
    DWORD now = GetTickCount();
    if (!g_dlgSearched || (g_dlgs.empty() && now - g_dlgSearch >= 10000)) {
        g_dlgSearched = true;
        g_dlgSearch = now;
        FindDialogWindows();
        Log("Dialoghi: %u candidati per la finestra (%lu ms)", static_cast<unsigned>(g_dlgs.size()), GetTickCount() - now);
    }
    // 0.49.8: tolta l'eccezione della 0.49.4 per le finestre mai viste nascoste: avrebbe accettato anche le copie vecchie
    // rimaste in memoria dopo un caricamento (il difetto della 0.7.6: modalita' cursore bloccata e A = Invio).
    // 0.55.27: prova del 31/10 sulla Steam Machine: la schermata "Il sogno" (nuova campagna, "Salta esercitazione") era gia'
    // aperta alla ricerca completa dopo il caricamento, quindi mai vista nascosta: B andava a "raccogli tutto". Nei 20 s dopo
    // il caricamento (MemoryDialogTrustNew) un secondo giro accetta anche quelle, se grandi da dialogo e con pulsanti, e una
    // volta accettata la finestra resta valida anche dopo i 20 s (si puo' leggere con calma); se 1,5 s dopo A o B
    // (MemoryDialogActed) e' ancora aperta e identica, e' una copia vecchia e si scarta; se e' cambiata (pagina nuova) e' viva
    // e vale come vista nascosta.
    const bool trustNew = static_cast<int>(g_dlgTrustUntil - now) > 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (DlgCand& c : g_dlgs) {
            DWORD f = 0, pos[3];
            if (pass == 1 && (c.seenHidden || c.stale || (!trustNew && !c.loggedNew))) continue;
            if (!IsDialogWindow(c.addr) || !Rd(c.addr + 0x10, f)) continue;
            if (!(f & 1)) { c.seenHidden = true; continue; }
            if (pass == 0 && !c.seenHidden) continue;
            if (pass == 1 && c.actedAt && now - c.actedAt >= 1500) {
                const DWORD sig = DialogSig(c.addr);
                c.actedAt = 0;
                if (sig == c.sig) {
                    c.stale = true;
                    Log("Dialoghi: @%08lX mai vista nascosta, ancora aperta e uguale 1,5 s dopo A/B: copia vecchia, scartata", c.addr);
                    continue;
                }
                c.seenHidden = true;
                Log("Dialoghi: @%08lX cambiata dopo A/B: finestra viva", c.addr);
            }
            if (!RdBuf(c.addr + 0x24, pos, sizeof(pos))) continue;
            DialogInfo t;
            t.x = static_cast<int>(pos[0]); t.y = static_cast<int>(pos[1]);
            t.w = static_cast<int>(pos[2] & 0xffff); t.h = static_cast<int>(pos[2] >> 16);
            t.buttons = ReadDialogButtons(c.addr, t.x, t.y, t.btn);
            // Un dialogo ha almeno un pulsante; i suggerimenti sugli oggetti nessuno.
            if (t.buttons == 0 && t.w < 600) continue;
            if (pass == 1 && (t.buttons == 0 || t.w < 600 || t.h < 200)) continue;   // non verificata: solo da dialogo
            t.addr = c.addr;
            t.unverified = !c.seenHidden;
            if (t.unverified && !c.loggedNew) {
                c.loggedNew = true;
                Log("Dialoghi: @%08lX (%d,%d) %dx%d con %d pulsanti, mai vista nascosta: accettata (dopo il caricamento)", c.addr,
                    t.x, t.y, t.w, t.h, t.buttons);
            }
            d = t;
            return true;
        }
    }
    return false;
}

void MemoryDialogTrustNew(DWORD until) { g_dlgTrustUntil = until; }

void MemoryDialogActed(DWORD addr)
{
    for (DlgCand& c : g_dlgs) {
        if (c.addr != addr || c.seenHidden || c.actedAt) continue;
        c.actedAt = GetTickCount();
        if (!c.actedAt) c.actedAt = 1;
        c.sig = DialogSig(addr);
    }
}

// Per la diagnostica: stato di tutti i candidati diversi da "nascosto, 1x1".
void MemoryDialogDump()
{
    int shown = 0;
    for (const DlgCand& c : g_dlgs) {
        DWORD f = 0, pos[3] = {};
        if (!Rd(c.addr + 0x10, f) || !RdBuf(c.addr + 0x24, pos, sizeof(pos))) continue;
        if (!(f & 1) && (pos[2] & 0xffff) <= 1) continue;
        RECT b[4];
        int n = ReadDialogButtons(c.addr, static_cast<int>(pos[0]), static_cast<int>(pos[1]), b);
        Log("Dialoghi:   @%08lX +10=%08lX pos (%lu,%lu) %lux%lu, pulsanti %d%s", c.addr, f, pos[0], pos[1],
            pos[2] & 0xffff, pos[2] >> 16, n, c.seenHidden ? "" : " (mai visto nascosto)");
        if (++shown >= 10) break;
    }
    if (!shown) Log("Dialoghi:   nessun candidato visibile o con dimensioni");
}

void MemoryDialogReset() { g_dlgSearched = false; g_dlgs.clear(); }

// --- Finestra di scelta della destinazione dei portali (0.10.5) ---------------------------
// Con F6 il 23/10/2026 (09:21, schermata dei portali aperta) e' visibile un cUI_BusyDlg di nome
// "DEFAULT_BUSYDLG" (vtable 0x896134) che senza portali non lo e', con dentro, a +0x17C, il
// pulsante "Annulla" (cUI_Button2 "DEFAULTBUTTON", vtable 0x896308, (922,736) 76x37 a 1080p).
// Come per i dialoghi in memoria ci sono copie vecchie: vale solo quella vista prima nascosta
// e poi visibile. La ricerca in tutta la memoria (circa 200 ms) si fa una volta per partita.
namespace {
constexpr DWORD kVtBusyDlg = 0x896134, kVtButton2 = 0x896308;

}   // namespace (sospeso: MemoryConfirmDlg e' pubblica)

// Conferma con due pulsanti (0.17.4, "Caricare la partita!" nei salvataggi): e' un cUI_BusyDlg fra le
// finestre del gestore, con i pulsanti cUI_Button2 incorporati (Annulla a +0x17C, OK a +0x238 nella prova
// delle 18:12). Si cercano i cUI_Button2 visibili incorporati nei primi 0x400 byte, ordinati da sinistra.
// Con un solo pulsante (finestra dei portali) non vale: li' A serve alla lista.
int MemoryConfirmDlg(RECT* btns, int maxBtns)
{
    if (!g_ok) return 0;
    DWORD mgr = 0;
    if (!Rd(kUIManager, mgr) || !mgr) return 0;
    for (DWORD off = 0x80; off < 0x200; off += 4) {
        DWORD w = 0, vt = 0, f = 0;
        if (!Rd(mgr + off, w) || !w || (w & 3) || !Rd(w, vt) || vt != kVtBusyDlg || !Rd(w + 0x10, f) || !(f & 1)) continue;
        DWORD buf[0x400 / 4];
        if (!RdBuf(w, buf, sizeof(buf))) continue;
        int n = 0;
        for (int i = 1; i < 0x400 / 4 && n < maxBtns; ++i) {
            if (buf[i] != kVtButton2) continue;
            const DWORD b = w + i * 4;
            DWORD bf = 0; short wh[2] = {};
            if (!Rd(b + 0x10, bf) || !(bf & 1) || !RdBuf(b + 0x2C, wh, sizeof(wh)) || wh[0] < 8 || wh[1] < 8) continue;
            int x = 0, y = 0;
            AbsPos(b, x, y);
            btns[n++] = RECT{ x, y, x + wh[0], y + wh[1] };
        }
        std::sort(btns, btns + n, [](const RECT& p, const RECT& q) { return p.left < q.left; });
        return n;
    }
    return 0;
}

namespace {
struct BusyCand { DWORD addr; bool seenHidden; };
std::vector<BusyCand> g_busy;
bool g_busySearched = false;
bool IsBusyDlg(DWORD a)
{
    DWORD vt = 0; char nm[16] = {};
    return Rd(a, vt) && vt == kVtBusyDlg && RdBuf(a + 0x30, nm, 15) && memcmp(nm, "DEFAULT_BUSYDLG", 15) == 0;
}
void FindBusyDlgs()
{
    g_busy.clear();
    std::vector<DWORD> buf;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0xFFFE0000u; ) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), size = mbi.RegionSize;
        if (size == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
            for (uintptr_t off = 0; off < size; off += 0x100000) {
                // 0.55.29: 64 byte in piu' come per i dialoghi (ScanDialogRange): un oggetto con la vtable in fondo a un pezzo da
                // 1 MB e il nome nel successivo non si trovava mai
                const size_t len = (size - off) < 0x100000 ? (size - off) : 0x100000;
                size_t rd = len + 0x40;
                if (off + rd > size) rd = static_cast<size_t>(size - off);
                buf.resize(rd / 4);
                if (!RdBuf(static_cast<DWORD>(base + off), buf.data(), rd & ~static_cast<size_t>(3))) continue;
                for (size_t i = 0; i < len / 4 && i + 16 < buf.size(); ++i) {
                    if (buf[i] != kVtBusyDlg || buf[i + 12] != 0x41464544) continue;   // "DEFA" a +0x30
                    const DWORD cand = static_cast<DWORD>(base + off + i * 4);
                    if (cand >= reinterpret_cast<uintptr_t>(buf.data()) &&
                        cand < reinterpret_cast<uintptr_t>(buf.data() + buf.size())) continue;   // la nostra copia
                    if (IsBusyDlg(cand) && g_busy.size() < 64) g_busy.push_back({ cand, false });
                }
            }
        }
        a = base + size;
    }
}
}

bool MemoryBusyDlg(RECT* cancel)
{
    if (!g_ok) return false;
    if (!g_busySearched) {
        g_busySearched = true;
        const DWORD t0 = GetTickCount();
        FindBusyDlgs();
        Log("Finestra dei portali: %u candidati (%lu ms)", static_cast<unsigned>(g_busy.size()), GetTickCount() - t0);
    }
    for (BusyCand& c : g_busy) {
        DWORD f = 0;
        if (!IsBusyDlg(c.addr) || !Rd(c.addr + 0x10, f)) continue;
        if (!(f & 1)) { c.seenHidden = true; continue; }
        if (!c.seenHidden) continue;
        if (cancel) {
            *cancel = RECT{};
            const DWORD b = c.addr + 0x17C;
            DWORD vt = 0, p = 0; int x = 0, y = 0; short wh[2] = {};
            if (Rd(b, vt) && vt == kVtButton2 && Rd(b + 0x24, x) && Rd(b + 0x28, y) && RdBuf(b + 0x2C, wh, sizeof(wh))) {
                Rd(b + 0x50, p);
                for (int depth = 0; p && depth < 6; ++depth) {
                    int px = 0, py = 0;
                    if (!Rd(p + 0x24, px) || !Rd(p + 0x28, py)) break;
                    x += px; y += py;
                    if (!Rd(p + 0x50, p)) break;
                }
                *cancel = RECT{ x, y, x + wh[0], y + wh[1] };
            }
        }
        return true;
    }
    return false;
}

void MemoryBusyReset() { g_busySearched = false; g_busy.clear(); }
void MemoryDialogRescan(bool quick)
{
    if (!g_ok) return;
    DWORD t0 = GetTickCount();
    size_t before = g_dlgs.size();
    FindDialogWindows(true, quick);
    g_dlgSearched = true;
    if (!quick || g_dlgs.size() != before)
        Log("Dialoghi: nuova ricerca%s, %u candidati nuovi (%lu ms)", quick ? " vicina" : " completa",
            static_cast<unsigned>(g_dlgs.size() - before), GetTickCount() - t0);
}

// 0.55.28: ricerca completa a fette, mentre si aspetta un dialogo (dopo il caricamento, dopo un'interazione, eroe 2 che parla).
// Prova del 31/10 sulla Steam Machine (0.55.27): la ricerca vicina guarda solo 2 MB attorno ai candidati gia' noti, ma li' il
// gioco crea le finestre dove capita: il dialogo dell'eroe 2 (11:20:27) e' stato trovato solo 16 s dopo, dalla ricerca completa
// partita con A del pad 1, e la domanda sull'esercitazione (11:19:41) mai (B finiva a "raccogli tutto"). Qui ogni chiamata legge
// al massimo 24 MB di memoria privata scrivibile, ripartendo da dove si era fermata la precedente: con una chiamata ogni 50 ms
// tutta la memoria si rilegge in circa un secondo, senza il blocco di 50-85 ms della ricerca completa in un colpo solo.
// I candidati trovati qui sono nuovi e valgono come affidabili, come quelli della ricerca vicina.
void MemoryDialogScanStep()
{
    if (!g_ok || !g_dlgSearched) return;                 // la prima ricerca completa la fa MemoryDialog
    static uintptr_t at = 0x10000;
    static DWORD roundStart = 0;
    static int roundsLogged = 0;
    constexpr uintptr_t kBudget = 24u << 20;
    const DWORD t0 = GetTickCount();
    if (at == 0x10000) roundStart = t0;
    const size_t before = g_dlgs.size();
    uintptr_t done = 0;
    int regions = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (done < kBudget && ++regions <= 4096) {
        if (at >= 0xFFFE0000u || !VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof(mbi)) || mbi.RegionSize == 0) {
            if (roundsLogged < 3) { ++roundsLogged; Log("Dialoghi: giro completo della ricerca a fette in %lu ms", GetTickCount() - roundStart); }
            at = 0x10000;
            break;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (end <= at) { at = 0x10000; break; }          // fine dello spazio degli indirizzi
        const uintptr_t from = at > base ? at : base;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
            uintptr_t lim = from + (kBudget - done);
            if (lim > end || lim < from) lim = end;
            ScanDialogRange(from, lim - from, end, true);
            done += lim - from;
            at = lim;
        }
        else at = end;
    }
    if (g_dlgs.size() != before)
        Log("Dialoghi: ricerca a fette, %u candidati nuovi (%lu ms)", static_cast<unsigned>(g_dlgs.size() - before), GetTickCount() - t0);
}

// Da chiamare spesso: tiene aggiornato "visto nascosto" per ogni candidato.
void MemoryDialogWatch()
{
    if (!g_ok || !g_dlgSearched) return;
    for (DlgCand& c : g_dlgs) {
        DWORD f = 0;
        if (!c.seenHidden && Rd(c.addr + 0x10, f) && !(f & 1)) c.seenHidden = true;
    }
}

// F5 (diagnostica): salva su file i dati del giocatore e della creatura dell'eroe, piu'
// i blocchi puntati da questi (due livelli), per confrontare due momenti (es. livello 1 e 2).
// File SacredPad_dump_N.bin nella cartella del gioco: record {indirizzo, lunghezza, dati}.
// La lettura procede a pezzi di 0x100 byte: un blocco a cavallo di una pagina non mappata
// viene salvato fino a dove si legge.
void MemoryDiagDump()
{
    static bool prev = false;
    if (!g_ok || !g_cfg.memoryDiag) return;
    bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    static int n = 0;
    ++n;
    std::vector<Obj> objs;
    Obj hero{};
    bool haveHero = false;
    DWORD player = 0;
    Rd(kPlayer, player);
    ReadCreatures(objs, hero, haveHero);
    struct Block { DWORD addr, len; std::vector<BYTE> data; int depth; };
    std::vector<Block> all;
    auto readBlock = [](Block& b) {
        b.data.clear();
        BYTE chunk[0x100];
        for (DWORD off = 0; off < b.len; off += sizeof(chunk)) {
            if (!RdBuf(b.addr + off, chunk, sizeof(chunk))) break;
            b.data.insert(b.data.end(), chunk, chunk + sizeof(chunk));
        }
        return !b.data.empty();
    };
    auto known = [&](DWORD p) {
        for (const Block& b : all) if (p >= b.addr && p < b.addr + b.len) return true;
        return false;
    };
    if (player) { Block b{ player, 0x200, {}, 1 }; if (readBlock(b)) all.push_back(b); }
    if (haveHero) { Block b{ hero.addr, 0x1000, {}, 0 }; if (readBlock(b)) all.push_back(b); }
    for (size_t i = 0; i < all.size() && all.size() < 1500; ++i) {
        if (all[i].depth >= 2) continue;
        const std::vector<BYTE>& d = all[i].data;
        const int depth = all[i].depth;
        for (size_t o = 0; o + 4 <= d.size(); o += 4) {
            DWORD p = *reinterpret_cast<const DWORD*>(&d[o]);
            if (p < 0x00100000u || p >= 0x7FFF0000u || (p & 3)) continue;
            if (p >= 0x00400000u && p < 0x01900000u) continue;   // immagine di Sacred.exe (codice, vtable)
            if (known(p) || all.size() >= 1500) continue;
            Block b{ p, 0x400, {}, depth + 1 };
            if (readBlock(b)) all.push_back(std::move(b));
        }
    }
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\SacredPad_dump_%d.bin", GameDir(), n);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("DUMP n.%d: impossibile creare %s", n, path); return; }
    for (const Block& b : all) {
        DWORD hdr[2] = { b.addr, static_cast<DWORD>(b.data.size()) }, wr = 0;
        WriteFile(h, hdr, sizeof(hdr), &wr, nullptr);
        WriteFile(h, b.data.data(), static_cast<DWORD>(b.data.size()), &wr, nullptr);
    }
    CloseHandle(h);
    Log("DUMP n.%d: giocatore %08lX, eroe %08lX, %u blocchi salvati in %s", n, player,
        haveHero ? hero.addr : 0, static_cast<unsigned>(all.size()), path);
}

// ---------------------------------------------------------------------------------------------
// Fase 5 (ramo native-coop), prove sulla co-op nativa. Solo con MemoryDiag. F11:
//   F11            registro: giocatore, id dell'eroe, [engine+0x13c] (creatura che riceve gli ordini),
//                 eroi/compagni/cavalli con proprietario (+0x1ec), tipo (+0x1f0), flag e posizione.
//   Maiusc+F11     ordine di movimento (tipo 4) all'eroe locale, 600 unita' a est: convalida il
//                 formato dell'ordine ricavato da Ghidra (0x617640: vtable 0x88f95c, +4 tipo,
//                 +0x14 modo 2, +0x18/+0x1c destinazione, +0x24 livello; consegna col metodo 0x18).
//   Ctrl+F11       lo stesso ordine al compagno al seguito (creatura che non e' l'eroe).
//   Ctrl+Maiusc+F11  [engine+0x13c] = compagno: i clic normali dovrebbero comandare lui.
// ---------------------------------------------------------------------------------------------
bool ReadHeroName(const char* path, std::string& name);   // definita piu' sotto (campagna cooperativa)
namespace {
// Dati dell'ultima eccezione dentro le chiamate al motore (0.31.3), per sapere dove falliscono.
struct ExcInfo { DWORD code, eip, addr, eax, ebx, ecx, edx, esi, edi, ebp, esp; DWORD ret[12]; int nret; };
ExcInfo g_exc{};
int CaptureException(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    g_exc = { r->ExceptionCode, static_cast<DWORD>(reinterpret_cast<uintptr_t>(r->ExceptionAddress)),
              r->NumberParameters >= 2 ? static_cast<DWORD>(r->ExceptionInformation[1]) : 0,
              c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, {}, 0 };
    // indirizzi di ritorno plausibili sullo stack: valori nel codice di Sacred.exe preceduti da una CALL
    BYTE stack[0x600] = {};
    SIZE_T got = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(c->Esp)), stack, sizeof(stack), &got)) {
        for (size_t o = 0; o + 4 <= got && g_exc.nret < 12; o += 4) {
            DWORD v = *reinterpret_cast<DWORD*>(stack + o);
            BYTE b5 = 0;
            if (v > 0x401005 && v < 0x88f000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(v - 5)), &b5, 1, nullptr) && b5 == 0xE8)
                g_exc.ret[g_exc.nret++] = v;
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
void LogException(const char* what)
{
    Log("NATIVO: ECCEZIONE in %s: codice %08lX a EIP %08lX, indirizzo %08lX; eax %08lX ebx %08lX ecx %08lX edx %08lX "
        "esi %08lX edi %08lX ebp %08lX esp %08lX", what, g_exc.code, g_exc.eip, g_exc.addr, g_exc.eax, g_exc.ebx,
        g_exc.ecx, g_exc.edx, g_exc.esi, g_exc.edi, g_exc.ebp, g_exc.esp);
    char chain[200] = {};
    for (int i = 0; i < g_exc.nret; ++i) {
        char t[16]; _snprintf_s(t, sizeof(t), _TRUNCATE, " %08lX", g_exc.ret[i]);
        strncat_s(chain, t, _TRUNCATE);
    }
    Log("NATIVO:   chiamate sullo stack:%s", chain);
}

// Chiamate al motore con controllo dello stack (0.32.3). Ogni funzione di Sacred.exe chiamata dalla DLL
// passa da uno di questi tre aiutanti: gli argomenti vengono messi in registri prima dei push, lo stack
// viene confrontato prima e dopo la chiamata e, se la funzione ne ha tolti piu' o meno di quelli messi
// (convenzione o numero di argomenti sbagliati, come per 0x5fd200 nella 0.31.4-0.32.1), ESP viene
// rimesso a posto e l'errore finisce nel log invece di rovinare i registri salvati del chiamante.
// Convenzioni verificate sul disassemblato (ret n finale): 0x616220 thiscall ret 4, 0x5fce00 thiscall
// ret 4, 0x5fd200 thiscall ret 8, 0x6173b0 stdcall ret 8, 0x5fc300 thiscall ret 0x10, 0x5fadb0 thiscall ret 4 (0.54.0).
std::atomic<DWORD> g_engineTid{ 0 };

struct CallResult { bool ok = false; bool balanced = true; DWORD ret = 0; };

CallResult CallThis1(DWORD fn, DWORD thisp, DWORD a1)
{
    CallResult cr;
    DWORD before = 0, after = 0, r = 0;
    __try {
        __asm {
            mov  eax, fn
            mov  ecx, thisp
            mov  edx, a1
            mov  before, esp
            push edx
            call eax
            mov  after, esp
            mov  esp, before
            mov  r, eax
        }
        cr.ok = true; cr.ret = r; cr.balanced = (after == before);   // il chiamato toglie il suo argomento: dopo la call ESP == before
    } __except (CaptureException(GetExceptionInformation())) {
        cr.ok = false;
    }
    return cr;
}

CallResult CallThis0(DWORD fn, DWORD thisp)
{
    CallResult cr;
    DWORD before = 0, after = 0, r = 0;
    __try {
        __asm {
            mov  eax, fn
            mov  ecx, thisp
            mov  before, esp
            call eax
            mov  after, esp
            mov  esp, before
            mov  r, eax
        }
        cr.ok = true; cr.ret = r; cr.balanced = (after == before);
    } __except (CaptureException(GetExceptionInformation())) {
        cr.ok = false;
    }
    return cr;
}

CallResult CallThis2(DWORD fn, DWORD thisp, DWORD a1, DWORD a2)
{
    CallResult cr;
    DWORD before = 0, after = 0, r = 0;
    __try {
        __asm {
            mov  eax, fn
            mov  ecx, thisp
            mov  edx, a1
            mov  esi, a2
            mov  before, esp
            push esi
            push edx
            call eax
            mov  after, esp
            mov  esp, before
            mov  r, eax
        }
        cr.ok = true; cr.ret = r; cr.balanced = (after == before);
    } __except (CaptureException(GetExceptionInformation())) {
        cr.ok = false;
    }
    return cr;
}

CallResult CallThis3(DWORD fn, DWORD thisp, DWORD a1, DWORD a2, DWORD a3)
{
    CallResult cr;
    DWORD before = 0, after = 0, r = 0;
    __try {
        __asm {
            mov  eax, fn
            mov  ecx, thisp
            mov  edx, a1
            mov  esi, a2
            mov  edi, a3
            mov  before, esp
            push edi
            push esi
            push edx
            call eax
            mov  after, esp
            mov  esp, before
            mov  r, eax
        }
        cr.ok = true; cr.ret = r; cr.balanced = (after == before);
    } __except (CaptureException(GetExceptionInformation())) {
        cr.ok = false;
    }
    return cr;
}

CallResult CallThis4(DWORD fn, DWORD thisp, DWORD a1, DWORD a2, DWORD a3, DWORD a4)
{
    CallResult cr;
    DWORD before = 0, after = 0, r = 0;
    __try {
        __asm {
            mov  eax, fn
            mov  ecx, thisp
            mov  edx, a1
            mov  esi, a2
            mov  edi, a3
            mov  before, esp
            push a4
            push edi
            push esi
            push edx
            call eax
            mov  after, esp
            mov  esp, before
            mov  r, eax
        }
        cr.ok = true; cr.ret = r; cr.balanced = (after == before);
    } __except (CaptureException(GetExceptionInformation())) {
        cr.ok = false;
    }
    return cr;
}

CallResult CallStd2(DWORD fn, DWORD a1, DWORD a2)
{
    CallResult cr;
    DWORD before = 0, after = 0, r = 0;
    __try {
        __asm {
            mov  eax, fn
            mov  edx, a1
            mov  esi, a2
            mov  before, esp
            push esi
            push edx
            call eax
            mov  after, esp
            mov  esp, before
            mov  r, eax
        }
        cr.ok = true; cr.ret = r; cr.balanced = (after == before);
    } __except (CaptureException(GetExceptionInformation())) {
        cr.ok = false;
    }
    return cr;
}

// Gestore di eccezioni a vettore (0.31.6): registra, in qualunque thread, le violazioni di accesso
// all'indirizzo 0x84F20E (routine di formattazione del gioco) con la catena delle chiamate, anche
// quando cadono fuori dai nostri __try (crash del 26/10 alle 17:50 e 17:52).
// 0.53.5: in piu' le prime 8 eccezioni gravi di qualunque punto (accesso non valido, istruzione illegale, divisione per
// zero, stack esaurito, passo singolo non riconosciuto da WatchVeh), con il modulo dell'istruzione: il crash sulla Steam
// Machine del 30/10 alle 01:03:24 non ha lasciato nulla nel log. Sono eccezioni "alla prima occasione": alcune possono
// essere gestite piu' avanti (anche dai __try della mod), quindi la riga non vuol dire per forza crash.
// 0.53.5: eroe 2 "assestato": lo stesso oggetto visto da almeno ms millisecondi. Crash sulla Steam Machine del 30/10 alle
// 01:03:24: la camera (registri di debug impostati sospendendo il thread del motore) e la seconda barra partivano mentre
// SpawnHero2 stava ancora creando e posizionando l'eroe 2, e il gioco si e' chiuso un fotogramma dopo. Ora aspettano.
// Chiamata a ogni giro dal thread della mod (con 0 se l'eroe 2 non c'e', cosi' si azzera) e dal disegno della barra.
std::atomic<DWORD> g_h2SeenAddr{ 0 }, g_h2SeenAt{ 0 };
bool Hero2Settled(DWORD h2, DWORD ms)
{
    if (!h2) { g_h2SeenAddr.store(0); return false; }
    const DWORD now = GetTickCount();
    if (g_h2SeenAddr.load() != h2) { g_h2SeenAddr.store(h2); g_h2SeenAt.store(now); return false; }
    return now - g_h2SeenAt.load() >= ms;
}

std::atomic<int> g_vehAny{ 0 };       // eccezioni gravi registrate (si riparte da 0 a ogni comparsa dell'eroe 2)
LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep)
{
    static std::atomic<int> n{ 0 };
    std::atomic<int>& nAny = g_vehAny;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const uintptr_t at = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    if (code == 0xC0000005 && at == 0x0084F20E) {
        if (n.fetch_add(1) < 3) { CaptureException(ep); LogException("thread del gioco (VEH)"); }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const bool serious = code == 0xC0000005 || code == 0xC000001D || code == 0xC0000094 || code == 0xC0000096 ||
                         code == 0xC00000FD || code == 0xC0000409 || code == EXCEPTION_SINGLE_STEP;
    // 0.53.7: le eccezioni nel codice della mod si saltano: dove possono capitare sono protette da __try (prova sul PC del
    // 30/10 alle 10:39:36: ScanChunk, la ricerca dei pulsanti dei menu, su una regione liberata nel frattempo).
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(at), &hm);
    if (hm && hm == SelfModule() && code == 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    if (serious && nAny.fetch_add(1) < 8) {
        const ExcInfo saved = g_exc;                    // g_exc serve anche ai __try delle chiamate al motore
        CaptureException(ep);
        char mod[MAX_PATH] = "?";
        if (hm) {
            char full[MAX_PATH] = {};
            if (GetModuleFileNameA(hm, full, MAX_PATH)) { const char* b = strrchr(full, '\\'); strcpy_s(mod, b ? b + 1 : full); }
        }
        Log("NATIVO: ATTENZIONE: eccezione %08lX nel thread %lu, istruzione %08lX (%s+%lX)%s", code, GetCurrentThreadId(),
            static_cast<DWORD>(at), mod, static_cast<DWORD>(at - reinterpret_cast<uintptr_t>(hm)),
            code == EXCEPTION_SINGLE_STEP ? " (passo singolo non della mod)" : "");
        LogException("VEH, prima occasione");
        g_exc = saved;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep);

void InstallCrashVeh()
{
    static bool done = false;
    if (done) return;
    done = true;
    AddVectoredExceptionHandler(1, CrashVeh);
    AddVectoredExceptionHandler(1, WatchVeh);
    Log("NATIVO: gestore di eccezioni a vettore installato");
}

// Punti di controllo hardware (0.32.7): chi scrive i campi dello stato dell'oggetto comandato? Quattro
// registri di debug (DR0-DR3) sui campi +0x150 (stato), +0xfc (modo), +0x1c (x) e +0x158 (destinazione
// x) dell'oggetto, sul thread del motore e su quello della finestra, per 25 s dopo un cambio di comando:
// a ogni scrittura il gestore registra l'indirizzo dell'istruzione successiva, il valore scritto e la catena
// delle chiamate. Serve a trovare la funzione che rimette a riposo l'eroe 2 dopo un clic sul terreno.
struct WatchSlot { DWORD addr; int len; const char* name; bool exec; };
WatchSlot g_watch[4] = {};
std::atomic<int> g_watchHits{ 0 };

// Camera sul punto medio dei due eroi (0.33.0). Il motore sposta la camera da FUN_00610670, che a ogni
// fotogramma legge la creatura in [engine+0x13c] e chiama FUN_006108a0(x, y, 0x10) con il centro voluto
// della vista (coordinate proiettate, le stesse di cWorldView0+0x14/+0x18). Un punto di arresto
// sull'esecuzione di 0x6108a0 (registro DR3 del thread del motore) somma agli argomenti sullo stack lo
// spostamento dal centro sulla creatura comandata al centro fra le due: e' lineare, quindi basta la
// proiezione di meta' della differenza fra le posizioni (formula di WorldToScreen).
constexpr DWORD kCamFn = 0x006108A0;
std::atomic<bool> g_camOn{ false };

bool CameraShift(int& du, int& dw)
{
    DWORD mgr = 0, engine = 0, ctl = 0, player = 0, heroId = 0;
    if (!Rd(kObjMgr, mgr) || !Rd(kEngine, engine) || !Rd(engine + 0x13c, ctl) || !Rd(kPlayer, player) || !RdHero(player, heroId)) return false;
    const DWORD other = (ctl == 2) ? heroId : 2;
    if (!mgr || ctl == 0 || other == 0 || other == ctl) return false;
    DWORD a = 0, b = 0, vt = 0, fa = 0, fb = 0;
    if (!Rd(mgr + 4, a)) return false;                  // vettore degli oggetti
    DWORD pa = 0, pb = 0, beg = a, end = 0;
    if (!Rd(mgr + 8, end) || ctl >= (end - beg) / 4 || other >= (end - beg) / 4) return false;
    if (!Rd(beg + ctl * 4, pa) || !Rd(beg + other * 4, pb) || !pa || !pb) return false;
    if (!Rd(pa, vt) || (vt != kVtCreature && vt != kVtHero)) return false;
    if (!Rd(pb, vt) || (vt != kVtCreature && vt != kVtHero)) return false;
    if (!Rd(pa + 0x14, fa) || !Rd(pb + 0x14, fb) || (fa & 2) || (fb & 2)) return false;   // fuori dal mondo
    int ax = 0, ay = 0, bx = 0, by = 0; BYTE za = 0, zb = 0;
    if (!Rd(pa + 0x1c, ax) || !Rd(pa + 0x20, ay) || !Rd(pb + 0x1c, bx) || !Rd(pb + 0x20, by) || !Rd(pa + 0x24, za) || !Rd(pb + 0x24, zb)) return false;
    if (za != zb) return false;                         // livelli diversi (caverne): nessuna media
    float c08 = 0, c10 = 0, c14 = 0, c20 = 0, c24 = 0;
    if (!Rd(kC08, c08) || !Rd(kC10, c10) || !Rd(kC14, c14) || !Rd(kC20, c20) || !Rd(kC24, c24) || c08 == 0.0f) return false;
    const float det = c24 * c10 - c20 * c14;
    if (std::fabs(det) < 1e-9f) return false;
    const float xp = (bx - ax) * 0.5f / c08, yp = (by - ay) * 0.5f / c08;
    du = static_cast<int>((c10 * xp + c20 * yp) / det);
    dw = static_cast<int>((c14 * xp + c24 * yp) / det);
    return true;
}

// Posizione dell'eroe comandato sullo schermo con la camera sul punto medio (0.49.2). Prova sulla Steam Machine del
// 29/10: con l'eroe 2 rimasto indietro la camera sta fra i due e l'eroe 1 non e' piu' al centro dello schermo, ma lo
// stick del pad 1 calcolava il punto di movimento dal centro: il puntatore restava "verso l'eroe 2" e andare nella
// direzione opposta riusciva male. Restituisce lo scarto dell'eroe locale dal centro, in frazioni della finestra.
bool CameraHeroOffsetImpl(float& fx, float& fy)   // nello spazio anonimo: la funzione pubblica e' dopo la sua chiusura
{
    fx = fy = 0;
    if (!g_ok || !g_camOn.load()) return false;
    View v{};
    int hx = 0, hy = 0;
    if (!ReadView(v) || !MemoryHeroPos(hx, hy)) return false;
    float sx = 0, sy = 0;
    if (!WorldToScreen(v, hx, hy, sx, sy) || v.w <= 0 || v.h <= 0) return false;
    fx = (sx - v.w * 0.5f) / v.w;
    fy = (sy - v.h * 0.5f) / v.h;
    if (std::fabs(fx) > 0.45f || std::fabs(fy) > 0.45f) { fx = fy = 0; return false; }   // valori assurdi: centro
    return true;
}

// 0.55.13: meta' della distanza fra i due eroi in coordinate della camera (lo spostamento di CameraShift), aggiornata a ogni
// fotogramma da WatchVeh con la camera sul punto medio; la usa l'audio (SndHalf). g_sndAt = 0: camera spenta o un solo eroe.
std::atomic<int> g_sndHu{ 0 }, g_sndHw{ 0 };
std::atomic<DWORD> g_sndAt{ 0 };

LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    // 0.53.5: il punto di arresto della camera si riconosce dall'istruzione, anche se DR6 non dice quale registro e'
    // scattato (sotto Wine non e' garantito): senza, l'eccezione restava a nessuno e il gioco cadeva. Crash sulla
    // Steam Machine del 30/10 alle 01:03:24, un fotogramma dopo l'accensione della camera (causa non certa).
    if (!(c->Dr6 & 0xF) && c->Eip != kCamFn) return EXCEPTION_CONTINUE_SEARCH;
    if (c->Eip == kCamFn) {                             // camera: DR3
        // 0.41.2: si entra qui anche a camera appena spenta (g_camOn falso, DR3 non ancora tolto): senza RF il punto di
        // arresto sull'esecuzione riscatterebbe sulla stessa istruzione all'infinito.
        // Due punti di chiamata in FUN_00610670: 0x610783 (ritorno 0x610788) passa il centro GIA' avanzato di
        // meta' verso il bersaglio (cur - (cur - T)/2), 0x6107a8 (ritorno 0x6107ad) passa il bersaglio T
        // intero. Sommare lo spostamento S sempre per intero (0.33.0) raddoppiava S nel primo caso: la camera
        // finiva sull'eroe 2 e oscillava fra i due rami quando gli eroi erano lontani. Corretto: S/2 nel primo
        // caso e S nel secondo, cosi' il punto fisso e' T + S in tutti e due.
        DWORD ret = 0;
        int du = 0, dw = 0;
        if (g_camOn.load() && CameraShift(du, dw)) {
            ret = *reinterpret_cast<DWORD*>(static_cast<uintptr_t>(c->Esp));
            int* args = reinterpret_cast<int*>(static_cast<uintptr_t>(c->Esp) + 4);
            if (ret == 0x00610788) { args[0] += du / 2; args[1] += dw / 2; }
            else if (ret == 0x006107AD) { args[0] += du; args[1] += dw; }
            g_sndHu.store(du); g_sndHw.store(dw); g_sndAt.store(GetTickCount());
        } else g_sndAt.store(0);
        c->EFlags |= 0x10000;
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    int slot = 0;
    while (slot < 3 && !(c->Dr6 & (1u << slot))) ++slot;
    if (g_watch[slot].exec) {              // punto di controllo sull'esecuzione: registra chi chiama e con quali argomenti
        const DWORD target = g_watch[0].addr - 0x150;   // oggetto controllato
        if (c->Ecx == target && g_watchHits.fetch_add(1) < 500) {
            DWORD ret = 0, a1 = 0, a2 = 0, ev4 = 0, ev14 = 0, ev18 = 0, ev1c = 0;
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(c->Esp)), &ret, 4, nullptr);
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(c->Esp + 4)), &a1, 4, nullptr);
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(c->Esp + 8)), &a2, 4, nullptr);
            if (g_watch[slot].addr == 0x0052E9A0 && a1 > 0x10000) {   // vf18: a1 = evento (ordine)
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(a1 + 4)), &ev4, 4, nullptr);
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(a1 + 0x14)), &ev14, 4, nullptr);
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(a1 + 0x18)), &ev18, 4, nullptr);
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(a1 + 0x1c)), &ev1c, 4, nullptr);
            }
            Log("NATIVO: ESEGUITO %s, chiamante %08lX, arg1 %08lX arg2 %08lX, evento tipo %lX modo %lX x %lX y %lX, thread %lu",
                g_watch[slot].name, ret, a1, a2, ev4, ev14, ev18, ev1c, GetCurrentThreadId());
        }
        c->EFlags |= 0x10000;              // RF: l'istruzione al punto di arresto puo' proseguire
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_watchHits.fetch_add(1) < 500 && g_watch[slot].addr) {
        DWORD v = 0;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(g_watch[slot].addr)), &v, g_watch[slot].len, nullptr);
        char chain[160] = {};
        BYTE stack[0x200] = {};
        SIZE_T got = 0;
        int n = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(c->Esp)), stack, sizeof(stack), &got)) {
            for (size_t o = 0; o + 4 <= got && n < 5; o += 4) {
                DWORD r = *reinterpret_cast<DWORD*>(stack + o);
                BYTE b5 = 0;
                if (r > 0x401005 && r < 0x88f000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(r - 5)), &b5, 1, nullptr) && b5 == 0xE8) {
                    char t[16]; _snprintf_s(t, sizeof(t), _TRUNCATE, " %08lX", r);
                    strncat_s(chain, t, _TRUNCATE);
                    ++n;
                }
            }
        }
        Log("NATIVO: SCRITTURA %s (%08lX) = %lX, dopo l'istruzione a %08lX, thread %lu, chiamate:%s", g_watch[slot].name,
            g_watch[slot].addr, v, c->Eip, GetCurrentThreadId(), chain);
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Imposta (o toglie, con addr = 0) i punti di controllo su un thread.
bool SetThreadWatch(DWORD tid, const WatchSlot* slotsIn)
{
    WatchSlot slots[4] = { slotsIn[0], slotsIn[1], slotsIn[2], slotsIn[3] };
    slots[3] = {};
    if (g_camOn.load() && tid == g_engineTid.load()) slots[3] = { kCamFn, 0, "camera", true };
    // 0.41.2: il gestore WatchVeh deve esistere prima di armare un registro, anche con MemoryDiag=false (la camera e'
    // accesa in modo predefinito e l'eroe 2 puo' arrivare da un salvataggio): senza, l'eccezione fa cadere il gioco.
    if (slots[0].addr || slots[1].addr || slots[2].addr || slots[3].addr) InstallCrashVeh();
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) return false;
    bool ok = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        CONTEXT cx = {};
        cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &cx)) {
            DWORD* dr[4] = { &cx.Dr0, &cx.Dr1, &cx.Dr2, &cx.Dr3 };
            DWORD dr7 = 0;
            for (int i = 0; i < 4; ++i) {
                *dr[i] = slots[i].addr;
                if (!slots[i].addr) continue;
                if (slots[i].exec) { dr7 |= (1u << (2 * i)); continue; }   // esecuzione: RW = 00, LEN = 00
                const DWORD lenCode = slots[i].len == 4 ? 3u : slots[i].len == 2 ? 1u : 0u;
                dr7 |= (1u << (2 * i)) | (1u << (16 + 4 * i)) | (lenCode << (18 + 4 * i));   // scrittura, locale
            }
            cx.Dr7 = dr7;
            cx.Dr6 = 0;
            ok = SetThreadContext(h, &cx) != 0;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return ok;
}

// Tutti i thread del processo (tranne quello della DLL che chiama): l'IA dei nemici gira in un thread
// proprio, non in quello del motore ne' in quello della finestra (0.34.3).
template <class F>
int ForEachThread(F fn)
{
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = {}; te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) { fn(te.th32ThreadID); ++n; }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

void ArmWatch(DWORD obj, DWORD id)
{
    g_watch[0] = { obj + 0x150, 2, "stato +0x150", false };
    g_watch[1] = { obj + 0x0fc, 2, "modo +0xfc", false };
    g_watch[2] = { 0x0052E9A0, 0, "ordine vf18 0x52e9a0", true };
    g_watch[3] = {};                                    // DR3: riservato alla camera
    g_watchHits.store(0);
    int ok = 0;
    const int n = ForEachThread([&](DWORD tid) { if (SetThreadWatch(tid, g_watch)) ++ok; });
    Log("NATIVO: punti di controllo sull'oggetto %lu (%08lX): %d thread su %d", id, obj, ok, n);
}

void DisarmWatch()
{
    WatchSlot none[4] = {};
    ForEachThread([&](DWORD tid) { SetThreadWatch(tid, none); });
    Log("NATIVO: punti di controllo tolti (%d scritture registrate)", g_watchHits.load());
}

// 0.55.36: orderMode 0 = fermata, come quella del gestore del giocatore 2 (tipo 4, modo 0, posizione attuale): 0x6173b0 con il
// modo 0 salta l'aggancio alla griglia (0x6173d1 -> 0x6175d0, metodo 0x18 diretto) e il punto non si sposta. Prima la mod
// fermava l'eroe 1 con un "vai al punto dove sei" (modo 2), che al piano di sopra della Vampira il gioco spostava alle spalle
// (prova del 31/10 alle 19:44:45-46: dopo "eroe fermo" l'eroe e' andato verso (187910,133278)).
bool SendMoveOrder(DWORD creature, int x, int y, BYTE z, bool forceCtrl = false, DWORD orderMode = 2)
{
    DWORD vt = 0, fn = 0;
    if (!Rd(creature, vt) || !Rd(vt + 0x18, fn) || fn < 0x401000 || fn >= 0x88f000) return false;   // il metodo 0x18 deve esistere
    DWORD order[0x11] = {};                     // 0x44 byte, come l'oggetto sullo stack del gioco
    order[0]    = 0x0088f95c;
    order[1]    = 4;
    order[5]    = orderMode;
    order[6]    = static_cast<DWORD>(x);
    order[7]    = static_cast<DWORD>(y);
    order[9]    = z;
    // 0.31.2: come per i clic sul terreno di 0x617640, la destinazione passa da 0x6173b0 (stdcall,
    // aggancia il punto alla griglia di navigazione e riempie livello e campi mancanti) che poi
    // consegna l'ordine col metodo 0x18. L'ordine "nudo" della 0.31.1 muoveva l'eroe 1 ma non l'eroe 2.
    // forceCtrl (0.32.6): il gestore degli ordini di tipo 4 (0x52f270) legge GetAsyncKeyState(VK_CONTROL):
    // con Ctrl entra nel ramo "muoviti senza attaccare" (modo 4, quello che usa anche la mod per i clic sul
    // terreno), senza Ctrl nel ramo "vai a" (modo 2), che per l'eroe 2 si interrompe subito. Il tasto viene
    // tenuto virtualmente premuto solo per la durata della chiamata (stesso thread, lettura sincrona).
    // 0.50.2: 0x6173b0 usa anche ECX, il motore (FUN_00617640 e FUN_0060f420 la chiamano con ECX = cEngine e lei lo passa a
    // FUN_00616e50 come "this", che legge [this+4]). Con CallStd2 ECX era quello che capitava: l'ordine riusciva per caso o
    // cadeva in 0x616fb3 (prova del 29/10 alle 14:18, lettura di [0+8]).
    DWORD engine = 0;
    if (!Rd(kEngine, engine) || !engine) return false;
    const DWORD prevCtrl = g_virt.keyUntil[VK_CONTROL].load();
    if (forceCtrl) g_virt.keyUntil[VK_CONTROL].store(VirtualInput::kHeld);
    const CallResult cr = CallThis2(0x006173b0, engine, creature, reinterpret_cast<DWORD>(order));
    if (forceCtrl) {   // 0.41.2: si rimette il valore di prima solo se nessun altro ha cambiato Ctrl durante la chiamata
        DWORD held = VirtualInput::kHeld;
        g_virt.keyUntil[VK_CONTROL].compare_exchange_strong(held, prevCtrl);
    }
    if (!cr.ok) { LogException("ordine di movimento"); return false; }
    if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x6173b0 ha lasciato lo stack sbilanciato (corretto)");
    return true;
}
// Esecuzione nel thread del motore (0.31.8). Sacred ha tre thread: quello della finestra (input),
// quello del motore (cEngine::renderThread::run, 0x60e640: a ogni fotogramma aggiorna il mondo e
// Granny, disegna e chiama EndScene) e i thread del sonoro e della rete. Chiamare 0x616220 dal
// thread della DLL (0.31.0) o da quello della finestra (0.31.1-0.31.7) lo faceva girare in parallelo
// al motore, che nello stesso momento svuota la coda delle texture nuove di Granny (0x401be0): nomi di
// texture non validi, eccezione in 0x84F20E e crash. Il lavoro in coda gira ora dentro l'EndScene
// agganciato (Overlay.cpp), cioe' nel thread del motore e fra un fotogramma e l'altro.
std::mutex g_jobMutex;
std::deque<std::function<void()>> g_jobs;
std::atomic<int> g_jobCount{ 0 };
std::atomic<bool> g_spawned{ false };   // l'eroe 2 e' stato creato: si attiva la traccia dell'oggetto comandato
std::atomic<bool> g_ctl2{ false };      // i comandi sono dati all'eroe 2: controllo di sicurezza a ogni fotogramma

bool RunOnGameThread(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lk(g_jobMutex);
    g_jobs.push_back(std::move(fn));
    g_jobCount.fetch_add(1);
    return true;
}

// cEngine::console(char*) (0x616220, thiscall, ret 4): la riga di comando della console del gioco.
// "sys loadplayer N" (0x614520) crea l'oggetto 2 dal file Save\HeroNN.pax vicino all'eroe locale,
// con +0x39 = 1 (proprietario locale): il gioco ha gia' un secondo eroe locale per il collaudo.
bool EngineConsole(DWORD engine, const char* line)
{
    char buf[128];
    strncpy_s(buf, line, _TRUNCATE);
    const CallResult cr = CallThis1(0x00616220, engine, reinterpret_cast<DWORD>(buf));
    if (cr.ok && !cr.balanced) Log("NATIVO: ATTENZIONE: 0x616220 ha lasciato lo stack sbilanciato (corretto)");
    return cr.ok;
}

// 0.54.0: l'eroe 2 della mod. Il gioco salva l'oggetto 2 con la partita (GAMExx.PAK) e lo ricrea al caricamento: una campagna
// giocata una volta in due restava in due per sempre, e la mod lo prendeva per suo (camera, barre, aggro, salvataggio nel
// HeroNN.pax scelto per ultimo, anche se era di un altro). Ora vale come eroe 2 solo l'oggetto creato dalla mod in questo mondo
// (SpawnHero2, azzerato a ogni nuova partita) o adottato con [Native] CleanSavedHero2=false; quello arrivato con il salvataggio
// ObjectAddr lo nasconde a tutta la mod finche' MemoryNativeProbe non lo toglie (RemoveSavedHero2).
std::atomic<DWORD> g_h2MineAddr{ 0 };   // oggetto dell'eroe 2 della mod (0 = nessuno)
// 0.55.37: -1 = sconosciuto. Fino alla 0.55.36 lo era 0, che pero' e' anche Hero00.pax: l'eroe 2 nato da Hero00.pax non si salvava
// mai (prova del 31/10 sera, vedi AdoptSavedHero2)
std::atomic<int> g_h2MineFile{ -1 };    // HeroNN.pax da cui e' nato (-1 = sconosciuto: l'eroe 2 non si salva in nessun file)
std::atomic<bool> g_h2Spawning{ false }; // SpawnHero2 in corso: l'oggetto 2 nuovo non e' ancora registrato
std::atomic<DWORD> g_h2SpawnSeq{ 0 };   // 0.55.44: conta le comparse dell'eroe 2 della mod (oro condiviso: si riparte solo da li')
constexpr DWORD kNetGame = 0x0182db6c;  // byte: partita in rete (0 in giocatore singolo; FUN_006108a0 muove l'oggetto 2 solo con 0)

// Oggetto 2 cosi' com'e' nel vettore del gestore: eroe o creatura non distrutta (+0x14 bit 0x10, cObjectManager::destroy).
DWORD RawHero2(DWORD mgr)
{
    DWORD beg = 0, end = 0, p = 0, vt = 0, fl = 0;
    if (!mgr || !Rd(mgr + 4, beg) || !Rd(mgr + 8, end) || 2 >= (end - beg) / 4 || !Rd(beg + 2 * 4, p) || !p) return 0;
    if (!Rd(p, vt) || (vt != kVtCreature && vt != kVtHero) || !Rd(p + 0x14, fl) || (fl & 0x10)) return 0;
    return p;
}

DWORD ObjectAddr(DWORD mgr, DWORD id)
{
    DWORD beg = 0, end = 0, p = 0, vt = 0, fl = 0;
    if (!Rd(mgr + 4, beg) || !Rd(mgr + 8, end) || id >= (end - beg) / 4 || !Rd(beg + id * 4, p) || !p) return 0;
    if (!Rd(p, vt) || (vt != kVtCreature && vt != kVtHero)) return 0;
    if (Rd(p + 0x14, fl) && (fl & 0x10)) return 0;      // 0.54.0: distrutto, sta per essere cancellato
    if (id == 2 && p != g_h2MineAddr.load()) {
        // l'oggetto 2 resta visibile se e' l'eroe locale (giocatore 2 di una partita in rete; RdHero vede l'eroe 1 durante
        // il salvataggio dell'eroe 2 e lo scambio): con la co-op nativa spenta nessun filtro
        DWORD player = 0, local = 0;
        if (g_cfg.nativeEnabled && !(Rd(kPlayer, player) && player && RdHero(player, local) && local == 2)) return 0;
    }
    return p;
}

// Oggetti nel vettore del gestore (diagnostica della rimozione dell'eroe 2).
int CountObjects(DWORD mgr)
{
    DWORD beg = 0, end = 0;
    if (!mgr || !Rd(mgr + 4, beg) || !Rd(mgr + 8, end) || end <= beg || (end - beg) / 4 > 200000) return -1;
    std::vector<DWORD> ptrs((end - beg) / 4);
    if (!RdBuf(beg, ptrs.data(), static_cast<DWORD>(ptrs.size() * 4))) return -1;
    int n = 0;
    for (DWORD p : ptrs) if (p) ++n;
    return n;
}

// L'ordine parte al fotogramma dopo: l'indirizzo letto nel thread della DLL puo' non valere piu'
// (creatura morta e tolta, cambio di area). Nel thread del motore l'oggetto si ricerca per id e
// l'ordine parte solo se l'indirizzo coincide ancora.
bool SendMoveOrderMT(DWORD creature, DWORD id, int x, int y, BYTE z, bool forceCtrl = false)
{
    return RunOnGameThread([=] {
        DWORD mgr = 0;
        Rd(kObjMgr, mgr);
        if (!mgr || ObjectAddr(mgr, id) != creature) {
            Log("NATIVO: ordine alla creatura %lu annullato: l'oggetto non e' piu' li'", id);
            return;
        }
        bool r = SendMoveOrder(creature, x, y, z, forceCtrl);
        Log("NATIVO: ordine alla creatura %lu (thread del motore%s): %s", id, forceCtrl ? ", Ctrl forzato" : "", r ? "consegnato" : "FALLITO");
    });
}

bool WriteByte(DWORD addr, BYTE v)
{
    __try { *reinterpret_cast<BYTE*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteDword(DWORD addr, DWORD v)
{
    __try { *reinterpret_cast<DWORD*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Gestore degli oggetti: 0x5fce00(id) toglie l'oggetto dal mondo ("push"), 0x5fd200(id, &pos) lo rimette
// alla posizione data ("pop", funziona solo se +0x14 ha il bit 2 acceso, cioe' dopo il push). Servono a
// riposizionare gli eroi: loadHero rimette l'eroe 2 nella posizione salvata nel suo file e il
// riposizionamento finale di "loadplayer" non sempre riesce (prove del 26/10, 0.31.1-0.31.3).
bool TeleportObject(DWORD mgr, DWORD id, const BYTE* pos16)
{
    BYTE copy[16];
    memcpy(copy, pos16, sizeof(copy));
    // 0x5fce00(id): thiscall, ret 4. 0x5fd200(id, &pos): thiscall, ret 8 (due argomenti: nella 0.31.4-0.32.1
    // passavo solo la posizione, il "ret 8" sbilanciava lo stack e il gioco cadeva a 0xa15).
    const CallResult a = CallThis1(0x005fce00, mgr, id);
    if (!a.ok) return false;
    if (!a.balanced) Log("NATIVO: ATTENZIONE: 0x5fce00 ha lasciato lo stack sbilanciato (corretto)");
    const CallResult b = CallThis2(0x005fd200, mgr, id, reinterpret_cast<DWORD>(copy));
    if (!b.ok) return false;
    if (!b.balanced) Log("NATIVO: ATTENZIONE: 0x5fd200 ha lasciato lo stack sbilanciato (corretto)");
    return true;
}

}   // namespace (sospeso: il secondo mouse ha funzioni pubbliche)
bool PatchCode(DWORD addr, const BYTE* bytes, size_t n);   // piu' sotto, con il gancio del salvataggio
// --- Secondo mouse del giocatore 2 (0.56.0) ------------------------------------------------------------------------------
// Studio del 03/11 (docs, "Studio del secondo mouse"). Il mouse del gioco e' un oggetto da 0x70 byte in [0xcdcadc]: +0 bandiere
// (0x100 sinistro premuto, 0x200 destro), +4/+8 posizione, +0xc/+0x10 posizione precedente (0x655a60 copia +4/+8 in +0xc/+0x10 e
// poi scrive la nuova; lo zaino prende il clic da +0xc/+0x10 meno il punto caldo, 0x655d90), +0x24 sezione critica, +0x3c.. pila
// dei cursori con quello in cima in +0x64, +0x68 oggetto in mano (e' il cursore stesso: 0x655b00 lo mette, 0x655c70 lo toglie),
// +0x6c oggetto del mondo sotto il cursore (0x612160 a ogni fotogramma). Tutto il gioco lo prende dal getter 0x6554e0 (259
// chiamate; nel codice nessun'altra lettura di [0xcdcadc]). Il gancio del getter restituisce un secondo oggetto, creato con le
// funzioni del gioco, solo nel thread indicato da g_m2Tid e solo per la durata di una chiamata della mod: i clic del giocatore 2
// (eventi del mouse passati al gestore dell'interfaccia, mai al mondo), il disegno della sua copia delle finestre e una terza
// chiamata del cursore del mondo (0x612160 con la bandiera 1, subito dopo quella del gioco a 0x60ecf1). Gli eventi del mouse il
// gioco li elabora nel thread del motore (log della Steam Machine del 02/11, 19:28:19: clic nello stesso thread dei lavori in
// coda), la procedura della finestra (thread principale) vede sempre il mouse del gioco: per il giocatore 1 non cambia nulla.
// [Native] Mouse2=false: come la 0.55.68 (un solo mouse, che passa da un giocatore all'altro).
constexpr DWORD kMouseGet = 0x006554e0, kWorldCursor = 0x00612160, kWorldCursorCall = 0x0060ecf1;
volatile DWORD g_m2 = 0;                    // oggetto mouse del giocatore 2
volatile DWORD g_m2Tid = 0;                 // thread in cui il getter restituisce g_m2 (0 = nessuno)
volatile DWORD g_mouseGetCont = 0x006554e7; // seguito del getter dopo i 7 byte copiati nello stub
std::atomic<bool> g_m2Hook{ false }, g_m2Draw{ false }, g_m2Off{ false };
std::atomic<int> g_m2Faults{ 0 };
std::atomic<LONG> g_m2UiX{ 0 }, g_m2UiY{ 0 }, g_m2WX{ 0 }, g_m2WY{ 0 };
std::atomic<DWORD> g_m2UiAt{ 0 }, g_m2WAt{ 0 }, g_m2Hover{ 0 };
std::atomic<bool> g_m2UiOn{ false }, g_m2WOn{ false }, g_m2OverUi{ false };
std::atomic<DWORD> g_m2GameAt{ 0 };            // 0.56.1: ultimo fotogramma di gioco (cursore del mondo chiamato)

__declspec(naked) void MouseGetStub()       // al posto dei primi 7 byte di 0x6554e0 (push -1 / push 0x870012): cambia solo eax
{
    __asm {
        mov  eax, dword ptr fs:[0x24]       // id del thread (TEB a 32 bit)
        cmp  eax, g_m2Tid
        jne  orig
        mov  eax, g_m2
        test eax, eax
        jz   orig
        ret
    orig:
        push -1
        push 0x00870012
        jmp  dword ptr [g_mouseGetCont]
    }
}

typedef DWORD(__cdecl* GameNewFn)(DWORD size);
typedef DWORD(__cdecl* GetterFn)();
typedef void(__fastcall* MouseSetPosFn)(DWORD m, DWORD edx, LONG x, LONG y);
typedef DWORD(__fastcall* WorldCursorFn)(DWORD engine, DWORD edx, DWORD dev, DWORD draw);
typedef char(__fastcall* UiHitFn)(DWORD mgr, DWORD edx, LONG x, LONG y);
typedef char(__fastcall* UiEventFn)(DWORD mgr, DWORD edx, DWORD ev);

bool Mouse2On() { return g_m2 && g_m2Hook.load() && !g_m2Off.load(); }

// Mouse che il gioco vede in questo thread: il secondo durante le chiamate della mod per il giocatore 2, altrimenti il suo.
DWORD Mouse2Cur()
{
    const DWORD m = g_m2;
    if (m && g_m2Tid == GetCurrentThreadId()) return m;
    DWORD g = 0;
    Rd(0x00cdcadc, g);
    return g;
}

static void Mouse2SetPos(LONG x, LONG y)    // due volte: anche la posizione precedente (+0xc/+0x10, quella dei clic) e' questa
{
    const DWORD m = g_m2;
    if (!m) return;
    __try {
        reinterpret_cast<MouseSetPosFn>(0x00655a60)(m, 0, x, y);
        reinterpret_cast<MouseSetPosFn>(0x00655a60)(m, 0, x, y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Per la durata di un blocco (solo nel thread del motore): il getter restituisce il secondo mouse, messo in (x, y).
struct Mouse2Scope {
    bool on = false; DWORD prev = 0;
    Mouse2Scope(bool want, LONG x, LONG y)
    {
        if (!want || !Mouse2On()) return;
        prev = g_m2Tid;
        g_m2Tid = GetCurrentThreadId();
        on = true;
        Mouse2SetPos(x, y);
    }
    ~Mouse2Scope() { if (on) g_m2Tid = prev; }
};

// Secondo oggetto mouse, come lo crea il gioco (operator new 0x848782 e costruttore 0x655200(0)), con il cursore normale spinto
// come all'avvio (0x816641: texture 0x657850("MOUSE_MAIN.TGA") del gestore 0x656eb0, poi 0x6555e0(tex, 0, 0, 0)).
static DWORD Mouse2Alloc()
{
    DWORD m = 0;
    __try { m = reinterpret_cast<GameNewFn>(0x00848782)(0x70); }
    __except (EXCEPTION_EXECUTE_HANDLER) { m = 0; }
    return m;
}
static DWORD GameTexMgr()
{
    DWORD t = 0;
    __try { t = reinterpret_cast<GetterFn>(0x00656eb0)(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { t = 0; }
    return t;
}
static DWORD Mouse2Create()
{
    static const char kMain[] = "MOUSE_MAIN.TGA";
    const DWORD m = Mouse2Alloc();
    if (!m) { Log("MOUSE2: allocazione del secondo mouse non riuscita"); return 0; }
    const CallResult c = CallThis1(0x00655200, m, 0);
    if (!c.ok || !c.balanced) { LogException("costruttore del secondo mouse (0x655200)"); return 0; }
    const DWORD tm = GameTexMgr();
    const CallResult t = tm ? CallThis1(0x00657850, tm, reinterpret_cast<DWORD>(kMain)) : CallResult{};
    if (!t.ok || !t.ret) { Log("MOUSE2: texture MOUSE_MAIN.TGA non trovata (gestore %08lX)", tm); return 0; }
    const CallResult p = CallThis4(0x006555e0, m, t.ret, 0, 0, 0);
    if (!p.ok || !p.balanced) { LogException("cursore del secondo mouse (0x6555e0)"); return 0; }
    DWORD top = 0; Rd(m + 0x64, top);
    Log("MOUSE2: secondo mouse creato in %08lX (texture %08lX, cursore %08lX)", m, t.ret, top);
    return m;
}

DWORD __fastcall WorldCursorHook(DWORD engine, DWORD edx, DWORD dev, DWORD draw);
void __fastcall UiDrawHook(DWORD mgr, DWORD edx, DWORD dev);
constexpr DWORD kUiDrawCall = 0x0060ecc4, kUiDraw = 0x00758bc0;
std::atomic<bool> g_m2UiDrawHook{ false };
std::atomic<DWORD> g_swapM2At{ 0 };          // 0.56.7: ultimo giro di Core con lo scambio sul secondo mouse
static bool SwapM2Now() { const DWORD at = g_swapM2At.load(); return at && GetTickCount() - at < 300; }

// Nel thread del motore, una volta: secondo mouse, gancio del getter e terza chiamata del cursore del mondo.
static void Mouse2InstallNow()
{
    static const BYTE want[7] = { 0x6A, 0xFF, 0x68, 0x12, 0x00, 0x87, 0x00 };
    BYTE cur[8] = {};
    if (!RdBuf(kMouseGet, cur, sizeof(cur)) || memcmp(cur, want, sizeof(want)) != 0) {
        Log("MOUSE2: NON attivato: inizio di 0x6554e0 inatteso (%02X %02X %02X %02X %02X %02X %02X): resta un solo mouse",
            cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6]);
        return;
    }
    BYTE op = 0; LONG rel = 0;
    if (!Rd(kWorldCursorCall, op) || !Rd(kWorldCursorCall + 1, rel) || op != 0xE8 || kWorldCursorCall + 5 + static_cast<DWORD>(rel) != kWorldCursor) {
        Log("MOUSE2: NON attivato: a %08lX non c'e' la chiamata a %08lX (byte %02X): resta un solo mouse", kWorldCursorCall, kWorldCursor, op);
        return;
    }
    const DWORD m = Mouse2Create();
    if (!m) return;
    g_m2 = m;
    // getter: 8 byte in una scrittura sola (jmp rel32, due nop e l'ottavo byte com'era), perche' anche la procedura della finestra
    // lo chiama mentre la mod lo cambia
    BYTE nb[8];
    nb[0] = 0xE9;
    const LONG j = static_cast<LONG>(reinterpret_cast<DWORD>(&MouseGetStub) - (kMouseGet + 5));
    memcpy(nb + 1, &j, 4);
    nb[5] = 0x90; nb[6] = 0x90; nb[7] = cur[7];
    LONGLONG o = 0, n = 0;
    memcpy(&o, cur, 8); memcpy(&n, nb, 8);
    void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(kMouseGet));
    DWORD old = 0;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) { Log("MOUSE2: NON attivato: VirtualProtect fallita (%lu)", GetLastError()); g_m2 = 0; return; }
    const LONGLONG was = InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(p), n, o);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    if (was != o) { Log("MOUSE2: NON attivato: 0x6554e0 cambiato durante la scrittura"); g_m2 = 0; return; }
    g_m2Hook.store(true);
    const LONG mine = static_cast<LONG>(reinterpret_cast<DWORD>(&WorldCursorHook) - (kWorldCursorCall + 5));
    if (PatchCode(kWorldCursorCall + 1, reinterpret_cast<const BYTE*>(&mine), 4)) g_m2Draw.store(true);
    // 0.56.7: disegno dell'interfaccia (cUI_Manager 0x758bc0, chiamata a 0x60ecc4 fra le due del cursore del mondo): durante lo
    // scambio sul secondo mouse lo si fa con il secondo mouse (suggerimenti e passaggio del puntatore del giocatore 2)
    BYTE uop = 0; LONG urel = 0;
    if (Rd(kUiDrawCall, uop) && Rd(kUiDrawCall + 1, urel) && uop == 0xE8 && kUiDrawCall + 5 + static_cast<DWORD>(urel) == kUiDraw) {
        const LONG ui = static_cast<LONG>(reinterpret_cast<DWORD>(&UiDrawHook) - (kUiDrawCall + 5));
        if (PatchCode(kUiDrawCall + 1, reinterpret_cast<const BYTE*>(&ui), 4)) g_m2UiDrawHook.store(true);
    }
    if (!g_m2UiDrawHook.load()) Log("MOUSE2: disegno dell'interfaccia non deviato (0x60ecc4 inatteso): con lo scambio i suggerimenti seguono il giocatore 1");
    Log("MOUSE2: attivo: getter del mouse 0x6554e0 deviato, cursore del giocatore 2 %s", g_m2Draw.load() ? "disegnato dal gioco (0x60ecf1)" : "NON disegnato (scrittura fallita)");
}

static bool Mouse2Want(LONG& x, LONG& y)
{
    if (!Mouse2On() || !g_m2Draw.load()) return false;
    const DWORD now = GetTickCount();
    if (g_m2UiOn.load() && now - g_m2UiAt.load() < 300) { x = g_m2UiX.load(); y = g_m2UiY.load(); return true; }
    if (g_m2WOn.load() && now - g_m2WAt.load() < 150) { x = g_m2WX.load(); y = g_m2WY.load(); return true; }
    return false;
}

// Terza chiamata del cursore del mondo, con il secondo mouse: forma (spada, dialogo, mercante, cavallo...), contorno
// dell'oggetto indicato, oggetto in mano, come per il giocatore 1. La scritta del nemico indicato (13 dword del gestore
// dell'interfaccia in +0xdc, 0x75c450) resta quella del giocatore 1.
static int Mouse2PassCall(DWORD engine, DWORD dev, DWORD mgr, LONG x, LONG y)   // senza oggetti C++, per il __try
{
    int r = 0;
    g_m2Tid = GetCurrentThreadId();
    __try {
        reinterpret_cast<MouseSetPosFn>(0x00655a60)(g_m2, 0, x, y);
        reinterpret_cast<MouseSetPosFn>(0x00655a60)(g_m2, 0, x, y);
        reinterpret_cast<WorldCursorFn>(kWorldCursor)(engine, 0, dev, 1);
        r = (mgr && reinterpret_cast<UiHitFn>(0x0075a780)(mgr, 0, x, y)) ? 2 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    g_m2Tid = 0;
    return r;
}
static void Mouse2Pass(DWORD engine, DWORD dev)
{
    LONG x = 0, y = 0;
    if (!Mouse2Want(x, y)) return;
    DWORD mgr = 0, save[13] = {};
    Rd(kUIManager, mgr);
    const bool saved = mgr && RdBuf(mgr + 0xdc, save, sizeof(save));
    const int r = Mouse2PassCall(engine, dev, mgr, x, y);
    if (saved) for (int i = 0; i < 13; ++i) WriteDword(mgr + 0xdc + i * 4, save[i]);
    if (r < 0) {
        const int n = g_m2Faults.fetch_add(1) + 1;
        Log("MOUSE2: ATTENZIONE: eccezione nel cursore del giocatore 2 in (%ld,%ld), %d di 3", x, y, n);
        if (n >= 3) { g_m2Off.store(true); Log("MOUSE2: spento dopo 3 errori: torna un solo mouse"); }
        return;
    }
    g_m2OverUi.store(r == 2);
    DWORD h = 0;
    Rd(g_m2 + 0x6c, h);
    g_m2Hover.store(h);
}

typedef void(__fastcall* UiDrawFn)(DWORD mgr, DWORD edx, DWORD dev);
void __fastcall UiDrawHook(DWORD mgr, DWORD edx, DWORD dev)
{
    const bool m2 = SwapM2Now() && g_m2UiOn.load();
    Mouse2Scope ms(m2, g_m2UiX.load(), g_m2UiY.load());
    reinterpret_cast<UiDrawFn>(kUiDraw)(mgr, edx, dev);
}

// 0.56.7: durante lo scambio sul secondo mouse un oggetto che il gioco mette nella mano del giocatore 1 (il giocatore 1 non usa le
// finestre: i messaggi "metti in mano" del server, 0x7dbb20 caso 0x4b, scrivono nella mano del mouse attivo, che fuori dalle
// chiamate della mod e' il suo) va nella mano del giocatore 2, se e' vuota. Come lo zaino: 0x655700 toglie il cursore
// dell'oggetto dal primo mouse, 0x655b00 lo mette nel secondo. Nel thread del motore, a ogni fotogramma.
typedef void(__fastcall* SetHeldFn)(DWORD m, DWORD edx, DWORD held, DWORD tex, DWORD a, DWORD b, DWORD sx, DWORD sy, DWORD hx, DWORD hy);
void Mouse2SwapCatch()
{
    static DWORD keep = 0;
    static bool was = false;
    DWORD m1 = 0, h1 = 0, h2 = 0, tex = 0;
    if (!g_m2 || !Rd(0x00cdcadc, m1) || !m1) return;
    Rd(m1 + 0x68, h1);
    const bool on = SwapM2Now();
    if (!on) { was = false; return; }
    if (!was) { was = true; keep = h1; }           // cio' che il giocatore 1 aveva gia' in mano resta suo
    if (!h1) { keep = 0; return; }
    if (h1 == keep || !Rd(g_m2 + 0x68, h2) || h2 || !Rd(h1 + 0x10, tex)) return;
    bool ok = false;
    __try {
        WriteDword(m1 + 0x68, 0);
        CallThis0(0x00655700, m1);
        reinterpret_cast<SetHeldFn>(0x00655b00)(g_m2, 0, h1, tex, 0, 0, 0x3f800000, 0x3f800000, 0xffffffe0, 0xffffffe0);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    DWORD id = 0; Rd(h1 + 4, id);
    Log("MOUSE2: scambio: il gioco ha messo l'oggetto %lu nella mano del giocatore 1: %s", id, ok ? "passato al giocatore 2" : "ERRORE");
}

DWORD __fastcall WorldCursorHook(DWORD engine, DWORD edx, DWORD dev, DWORD draw)
{
    const DWORD r = reinterpret_cast<WorldCursorFn>(kWorldCursor)(engine, edx, dev, draw);
    g_m2GameAt.store(GetTickCount() | 1);
    Mouse2Pass(engine, dev);
    return r;
}

// 0.56.1: fuori dalla partita (menu principale: nessun fotogramma di gioco da 10 s e nessun eroe 2) la mano del secondo mouse
// si svuota: l'id dell'oggetto non vale piu' e in una partita nuova potrebbe indicare un altro oggetto. Con "salva ed esci"
// l'oggetto e' gia' tornato nello zaino (SaveGameHook); uscendo senza salvare si perde come quello in mano al giocatore 1.
void Mouse2IdleTick()
{
    const DWORD m = g_m2, at = g_m2GameAt.load();
    DWORD h = 0, om = 0;
    if (!m || !at || GetTickCount() - at < 10000 || !Rd(m + 0x68, h) || !h) return;
    if (Rd(kObjMgr, om) && om && ObjectAddr(om, 2)) return;
    CallThis0(0x00655c70, m);
    g_m2GameAt.store(0);
    Log("MOUSE2: fuori dalla partita: mano del giocatore 2 svuotata");
}
// 0.56.3 (controllo): partita caricata dal menu senza passare dal menu principale (l'oggetto dell'eroe 1 cambia, MemoryNativeProbe):
// l'oggetto in mano al giocatore 2 era della partita di prima e il suo id ora puo' essere di un altro oggetto. Nel thread del motore.
void Mouse2Forget(const char* why)
{
    const DWORD m = g_m2;
    DWORD h = 0, id = 0;
    if (!m || !Rd(m + 0x68, h) || !h) return;
    Rd(h + 4, id);
    CallThis0(0x00655c70, m);
    Log("MOUSE2: %s: mano del giocatore 2 svuotata (oggetto %lu della partita di prima)", why, id);
}

// Un pulsante del giocatore 2 (nel thread del motore): bit del pulsante e posizione nel secondo mouse, evento del gioco
// (cEvent_mouseButton_down 0x8960a8 / _up 0x898248, 0x14 byte: pulsante 0 sinistro 2 destro, x, y) passato al gestore
// dell'interfaccia (metodo 0 di cUI_Manager, come fa il motore in 0x618410), mai al mondo (0x617640 comanderebbe l'eroe 1).
static int Mouse2ClickCall(DWORD mgr, DWORD fn, DWORD ev, LONG x, LONG y)
{
    int r = -1;
    g_m2Tid = GetCurrentThreadId();
    __try {
        reinterpret_cast<MouseSetPosFn>(0x00655a60)(g_m2, 0, x, y);
        reinterpret_cast<MouseSetPosFn>(0x00655a60)(g_m2, 0, x, y);
        r = reinterpret_cast<UiEventFn>(fn)(mgr, 0, ev) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    g_m2Tid = 0;
    return r;
}
static void Mouse2ClickNow(int btn, bool down, LONG x, LONG y, bool shift = false)
{
    if (!Mouse2On()) return;
    // 0.56.9: con Shift (vendita, come X del pad 1 dal mercante) il tasto virtuale resta giu' per la durata dell'evento: il gioco
    // lo legge con GetAsyncKeyState nello stesso thread
    const DWORD prevShift = g_virt.keyUntil[VK_SHIFT].load();
    if (shift) g_virt.keyUntil[VK_SHIFT].store(VirtualInput::kHeld);
    DWORD mgr = 0, vt = 0, fn = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(mgr, vt) || !Rd(vt, fn) || fn < 0x401000 || fn >= 0x88f000) return;
    DWORD fl = 0;
    Rd(g_m2, fl);
    const DWORD bit = btn == 2 ? 0x200u : 0x100u;
    WriteDword(g_m2, down ? (fl | bit) : (fl & ~bit));
    DWORD ev[5] = { down ? 0x008960a8u : 0x00898248u, static_cast<DWORD>(btn), static_cast<DWORD>(x), static_cast<DWORD>(y), 0 };
    const int r = Mouse2ClickCall(mgr, fn, reinterpret_cast<DWORD>(ev), x, y);
    if (shift) { DWORD held = VirtualInput::kHeld; g_virt.keyUntil[VK_SHIFT].compare_exchange_strong(held, prevShift); }
    DWORD h = 0; WORD ht = 0;
    if (Rd(g_m2 + 0x68, h) && h) Rd(h, ht);
    Log("MOUSE2: %s %s in (%ld,%ld): %s, in mano %s", btn == 2 ? "destro" : "sinistro", down ? "giu'" : "su", x, y,
        r < 0 ? "ERRORE" : r ? "preso da una finestra" : "nessuna finestra", !h ? "niente" : ht == 1 ? "un oggetto" : "un'arte o una combo");
    if (r < 0) {
        const int n = g_m2Faults.fetch_add(1) + 1;
        if (n >= 3) { g_m2Off.store(true); Log("MOUSE2: spento dopo 3 errori: torna un solo mouse"); }
    }
}

// Oggetto in mano al giocatore 2 rimesso a posto (nel thread del motore): il salvataggio scrive nella partita solo l'oggetto in
// mano del mouse del gioco (0x619f70, blocco "Mouse", record 0x98) e quello del secondo mouse, staccato dallo zaino quando e'
// stato preso (0x5da8a0), andrebbe perso. Come la posa dello zaino (0x6c0870-0x6c091a): oggetto 0x5fe550, cObject3D vf28(0),
// misure 0x428d60 della tabella dei tipi [0xaaa564], posto libero 0x5db070(w, h, &casella, 0) e posa 0x5da3d0(casella, id, w, h,
// 1) nella voce dell'inventario dell'eroe 2 (poi dell'eroe 1), mano svuotata con 0x655c70. Senza posto, se la mano del giocatore
// 1 e' vuota, l'oggetto passa a lui (0x655b00, come lo mette lo zaino): si salva con la partita e torna in mano al caricamento.
typedef DWORD(__cdecl* DynCastFn)(DWORD obj, DWORD off, DWORD from, DWORD to, DWORD isRef);
typedef void(__fastcall* VfArg1Fn)(DWORD self, DWORD edx, DWORD a);
typedef char(__fastcall* FreeCellFn)(DWORD entry, DWORD edx, DWORD w, DWORD h, DWORD* cell, DWORD z);
typedef void(__fastcall* PlaceFn)(DWORD entry, DWORD edx, DWORD cell, DWORD id, DWORD w, DWORD h, DWORD one);
typedef void(__fastcall* SetHeldFn)(DWORD m, DWORD edx, DWORD held, DWORD tex, DWORD a, DWORD b, DWORD sx, DWORD sy, DWORD hx, DWORD hy);
static void Hide3D(DWORD obj)
{
    __try {
        const DWORD o3 = reinterpret_cast<DynCastFn>(0x008498a1)(obj, 0, 0x008ea650, 0x0094bce0, 0);
        DWORD vt = 0, fn = 0;
        if (o3 && Rd(o3, vt) && Rd(vt + 0x28, fn) && fn >= 0x401000 && fn < 0x88f000) reinterpret_cast<VfArg1Fn>(fn)(o3, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static int StowInEntry(DWORD entry, DWORD id, DWORD w, DWORD h)   // 1 posato, 0 niente posto, -1 errore
{
    int r = 0;
    __try {
        DWORD cell = 0;
        if (reinterpret_cast<FreeCellFn>(0x005db070)(entry, 0, w, h, &cell, 0)) {
            reinterpret_cast<PlaceFn>(0x005da3d0)(entry, 0, cell, id, w, h, 1);
            r = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}
static bool GiveHeldToMouse1(DWORD held)
{
    DWORD m1 = 0, h1 = 0, tex = 0;
    if (!Rd(0x00cdcadc, m1) || !m1 || !Rd(m1 + 0x68, h1) || h1 || !Rd(held + 0x10, tex)) return false;
    bool ok = false;
    __try {
        WriteDword(g_m2 + 0x68, 0);                                  // la struttura passa al primo mouse: niente free qui
        CallThis0(0x00655700, g_m2);                                 // toglie il cursore dell'oggetto dal secondo
        reinterpret_cast<SetHeldFn>(0x00655b00)(m1, 0, held, tex, 0, 0, 0x3f800000, 0x3f800000, 0xffffffe0, 0xffffffe0);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}
bool Mouse2StowNow(const char* why)
{
    if (!g_m2) return true;
    DWORD held = 0;
    if (!Rd(g_m2 + 0x68, held) || !held) return true;
    WORD type = 0; DWORD id = 0;
    Rd(held, type); Rd(held + 4, id);
    if (type != 1) {                                                // arte o combo: solo il riferimento sul cursore
        CallThis0(0x00655c70, g_m2);
        Log("MOUSE2: %s: arte o combo in mano al giocatore 2 lasciata (tipo %u)", why, type);
        return true;
    }
    DWORD om = 0, inv = 0, db = 0;
    Rd(kObjMgr, om); Rd(kInvTable, inv); Rd(0x00aaa564, db);
    const CallResult co = om ? CallThis1(0x005fe550, om, id) : CallResult{};
    const DWORD obj = co.ok ? co.ret : 0;
    DWORD otype = 0, w = 0, h = 0;
    if (!obj || !inv || !db || !Rd(obj + 0x10, otype) || !CallThis3(0x00428d60, db, otype, reinterpret_cast<DWORD>(&w), reinterpret_cast<DWORD>(&h)).ok || !w || !h) {
        Log("MOUSE2: ATTENZIONE: %s: oggetto %lu in mano al giocatore 2 non leggibile (oggetto %08lX, misure %lux%lu)", why, id, obj, w, h);
        return false;
    }
    Hide3D(obj);
    for (DWORD hid = 2; hid >= 1; --hid) {
        const int r = StowInEntry(inv + hid * kInvSize, id, w, h);
        if (r > 0) {
            CallThis0(0x00655c70, g_m2);
            Log("MOUSE2: %s: oggetto %lu in mano al giocatore 2 rimesso nello zaino dell'eroe %lu", why, id, hid);
            if (hid == 1) OverlayCoopToast(Tr("Zaino dell'eroe 2 pieno: l'oggetto in mano e' andato all'eroe 1"), 4000);
            return true;
        }
        if (r < 0) { Log("MOUSE2: ATTENZIONE: %s: eccezione posando l'oggetto %lu nello zaino dell'eroe %lu", why, id, hid); break; }
    }
    if (GiveHeldToMouse1(held)) {
        Log("MOUSE2: %s: zaini pieni, oggetto %lu passato in mano al giocatore 1", why, id);
        OverlayCoopToast(Tr("Zaini pieni: l'oggetto del giocatore 2 e' in mano al giocatore 1"), 4000);
        return true;
    }
    Log("MOUSE2: ATTENZIONE: %s: oggetto %lu in mano al giocatore 2 NON salvato (zaini pieni e mano del giocatore 1 occupata)", why, id);
    OverlayCoopToast(Tr("Giocatore 2: posa l'oggetto che hai in mano"), 4000);
    return false;
}

bool MemoryMouse2Ready() { return g_ok && g_cfg.nativeMouse2 && Mouse2On() && g_m2Draw.load(); }
// Il pad 1 e' in modalita' gioco (Core.cpp, a ogni giro). Con il secondo mouse il giocatore 1 gioca con i clic veri anche mentre
// il giocatore 2 ha lo zaino aperto: un suo clic nel mondo (attacco, bersaglio di A) che cade sulla copia dell'eroe 2 deve andare
// al mondo, non allo zaino dell'altro (prenderebbe i suoi oggetti).
std::atomic<bool> g_p1Game{ false };
void MemoryPad1GameMode(bool on) { g_p1Game.store(on); }
// 0.56.2: modalita' cursore del giocatore 2 (NativePad2Tick: View premuto e lasciato) e i suoi clic fuori dalle sue finestre
// (Core.cpp, Pad2PointerM2), che NativePad2Tick traduce in azioni dell'eroe 2.
std::atomic<bool> g_p2Cur{ false }, g_p2cAHeld{ false };
std::atomic<DWORD> g_p2MerchAt{ 0 };          // 0.56.9: A del pad 2 su un mercante (il negozio che si apre e' dell'eroe 2)
bool MemoryPad2MerchantRecent() { const DWORD t = g_p2MerchAt.load(); return t && GetTickCount() - t < 15000; }
void MemoryPad2MerchantClear() { g_p2MerchAt.store(0); }
constexpr bool kShareSmith = false;          // 0.56.14: fabbro condiviso no (vedi kDualWin)
std::atomic<int> g_p2ShopWin{ -1 };           // 0.56.14: finestra del servizio a cui l'eroe 2 va a parlare
int MemoryPad2ShopWin() { return MemoryPad2MerchantRecent() ? g_p2ShopWin.load() : -1; }
int MemorySharedServiceWin(DWORD id)
{
    const DWORD o = id ? ObjectById(id) : 0;
    DWORD svc = 0;
    if (!o || !Rd(o + 0x200, svc)) return -1;
    return (svc & 0x2000u) ? 4 : ((svc & 0x1000u) && kShareSmith) ? 3 : (svc & 0x4000u) ? 14 : -1;
}
bool MemoryIsMerchant(DWORD id)
{
    const DWORD o = id ? ObjectById(id) : 0;
    DWORD svc = 0;
    return o && Rd(o + 0x200, svc) && (svc & 0x2000u) != 0;
}
std::atomic<int> g_p2cAPress{ 0 }, g_p2cBPress{ 0 };
bool MemoryPad2CursorMode() { return g_p2Cur.load() && MemoryMouse2Ready(); }
void MemoryPad2WorldButton(int btn, bool down, LONG x, LONG y)
{
    (void)x; (void)y;                                // il punto e' quello del cursore (g_m2UiX/Y), letto a ogni giro
    if (btn == 0) { if (down) g_p2cAPress.fetch_add(1); g_p2cAHeld.store(down); }
    else if (btn == 2 && down) g_p2cBPress.fetch_add(1);
}
static bool P1GameMouse() { return g_p1Game.load() && MemoryMouse2Ready() && Mouse2Cur() != g_m2; }
void MemoryMouse2Ui(bool on, LONG x, LONG y)
{
    g_m2UiX.store(x); g_m2UiY.store(y); g_m2UiAt.store(GetTickCount()); g_m2UiOn.store(on);
}
void Mouse2World(bool on, LONG x, LONG y)
{
    g_m2WX.store(x); g_m2WY.store(y); g_m2WAt.store(GetTickCount()); g_m2WOn.store(on);
}
void MemoryMouse2Button(int btn, bool down, LONG x, LONG y, bool shift)
{
    if (!MemoryMouse2Ready()) return;
    RunOnGameThread([=] { Mouse2ClickNow(btn, down, x, y, shift); });
}
bool MemoryMouse2Held()
{
    DWORD h = 0;
    return g_m2 && Rd(g_m2 + 0x68, h) && h;
}
bool MemoryMouse2OverUi() { return g_m2OverUi.load(); }
// 0.56.6: l'oggetto in mano al giocatore 2 si getta a terra come fa il clic del giocatore 1 sul terreno (0x617640 -> 0x612110:
// thiscall sul motore (posizione di chi getta, posizione del punto), ret 8: con un oggetto in mano al mouse attivo chiama
// cEngine::dropItem 0x611910(id, da, verso) e, riuscito, svuota la mano con 0x655c70). Qui con il secondo mouse attivo, la
// posizione dell'eroe 2 (+0x18, 16 byte {ushort, x, y, livello}) e il punto del mondo sotto il cursore.
static bool ScreenToWorldNear(const View& v, int ox, int oy, float sx, float sy, int& wx, int& wy);   // piu' sotto
typedef char(__fastcall* DropFn)(DWORD engine, DWORD edx, DWORD from, DWORD to);
static int Mouse2DropCall(DWORD engine, DWORD from, DWORD to)
{
    int r = -1;
    g_m2Tid = GetCurrentThreadId();
    __try { r = reinterpret_cast<DropFn>(0x00612110)(engine, 0, from, to) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    g_m2Tid = 0;
    return r;
}
void MemorySwapMouse2(bool on) { g_swapM2At.store(on ? (GetTickCount() | 1) : 0); }
void MemoryMouse2Drop(LONG x, LONG y)
{
    if (!MemoryMouse2Ready() || !MemoryMouse2Held()) return;
    RunOnGameThread([x, y] {
        DWORD mgr = 0, engine = 0, h = 0, id = 0; WORD t = 0;
        Rd(kObjMgr, mgr); Rd(kEngine, engine);
        const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0;
        if (!h2 || !engine || !g_m2 || !Rd(g_m2 + 0x68, h) || !h || !Rd(h, t) || t != 1) return;
        Rd(h + 4, id);
        BYTE from[16] = {}, to[16] = {};
        View v;
        int hx = 0, hy = 0, wx = 0, wy = 0;
        if (!RdBuf(h2 + 0x18, from, sizeof(from)) || !ReadView(v)) return;
        memcpy(&hx, from + 4, 4); memcpy(&hy, from + 8, 4);
        memcpy(to, from, sizeof(to));
        if (ScreenToWorldNear(v, hx, hy, static_cast<float>(x), static_cast<float>(y), wx, wy)) { memcpy(to + 4, &wx, 4); memcpy(to + 8, &wy, 4); }
        const int r = Mouse2DropCall(engine, reinterpret_cast<DWORD>(from), reinterpret_cast<DWORD>(to));
        Log("MOUSE2: oggetto %lu del giocatore 2 gettato a terra verso (%d,%d): %s", id, wx, wy, r < 0 ? "ERRORE" : r ? "fatto" : "non riuscito");
    });
}

void MemoryMouse2Stow(const char* why)
{
    if (!MemoryMouse2Held()) return;
    RunOnGameThread([why] { Mouse2StowNow(why); });
}

// 0.56.4: prova del 03/11 alle 12:26 (log e dump in diag/pc_20261103_crash): il giocatore 2 ha preso un oggetto (12:25:38), il gioco
// ha chiuso lo zaino per il diario (tasto L, 12:25:40), lo zaino si e' riaperto con l'oggetto ancora nella mano del secondo mouse
// (12:25:52): da li' i fotogrammi sono scesi da 28 a 11 al secondo e alle 12:26:04 una richiesta di memoria della mod
// (ReadCreatures, std::bad_alloc) e' fallita: la memoria del processo era finita in 12 s (Sacred.exe ha il flag LARGE_ADDRESS_AWARE:
// 4 GB di indirizzi sia su Windows a 64 bit sia sulla Steam Machine, quindi circa 300 MB al secondo), con gli heap ancora validi
// (controllo ogni 3 s). Funzione del gioco che alloca non trovata. Qui, nel thread della mod, ogni secondo: memoria privata del
// processo; con qualcosa nella mano del secondo mouse una riga ogni 2 s, e se la memoria cresce di oltre 200 MB da quando l'oggetto
// e' in mano (o supera 3000 MB) l'oggetto torna subito nel suo zaino (riga ATTENZIONE). Ogni 30 s la memoria anche senza.
static DWORD ProcessMb()
{
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof(pm);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm))) return 0;
    return static_cast<DWORD>(pm.PrivateUsage / (1024 * 1024));
}
void Mouse2MemoryWatch()
{
    if (!g_m2) return;
    static DWORD next = 0, heldSince = 0, heldBase = 0, logAt = 0, quietAt = 0;
    static bool rescued = false;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - next) < 0) return;
    next = now + 1000;
    const DWORD mb = ProcessMb();
    DWORD h = 0; WORD ht = 0;
    const bool held = Rd(g_m2 + 0x68, h) && h;
    if (held) Rd(h, ht);
    if (!held) {
        if (heldSince) Log("MOUSE2: mano del giocatore 2 vuota dopo %lu ms, memoria %lu MB (era %lu MB)", now - heldSince, mb, heldBase);
        heldSince = 0; rescued = false;
        if (static_cast<LONG>(now - quietAt) >= 30000) { quietAt = now; Log("MEMORIA: %lu MB", mb); }
        return;
    }
    if (!heldSince) { heldSince = now ? now : 1; heldBase = mb; logAt = 0; Log("MOUSE2: il giocatore 2 ha in mano %s, memoria %lu MB", ht == 1 ? "un oggetto" : "un'arte o una combo", mb); }
    if (static_cast<LONG>(now - logAt) >= 2000) {
        logAt = now;
        Log("MOUSE2: in mano da %lu ms, memoria %lu MB (%+ld), zaino del giocatore 2 %s", now - heldSince, mb,
            static_cast<long>(mb) - static_cast<long>(heldBase), (MemoryDualSide(2) && MemoryDualActive()) ? "aperto" : "chiuso");
    }
    if (!rescued && (mb > heldBase + 200 || mb > 3000)) {
        rescued = true;
        Log("MOUSE2: ATTENZIONE: memoria da %lu a %lu MB in %lu ms con un oggetto in mano al giocatore 2: rimesso nel suo zaino", heldBase, mb, now - heldSince);
        OverlayCoopToast(Tr("Giocatore 2: l'oggetto in mano e' tornato nello zaino"), 3000);
        RunOnGameThread([] { Mouse2StowNow("memoria in crescita"); });
    }
}
bool MemoryMouse2WorldPos(LONG& x, LONG& y)
{
    if (!g_m2WOn.load() || GetTickCount() - g_m2WAt.load() >= 300) return false;
    x = g_m2WX.load(); y = g_m2WY.load();
    return true;
}
DWORD MemoryMouse2Hover() { const DWORD h = g_m2Hover.load(); return h == 0xffffffffu ? 0 : h; }

namespace {


// Posizione di un oggetto: struttura di 16 byte a +0x18 ({ushort, x, y, livello}), come la passa il gioco.
bool InWorld(DWORD obj)
{
    DWORD fl = 0;
    return Rd(obj + 0x14, fl) && (fl & 2) == 0;     // +0x14 bit 1: oggetto tolto dal mondo ("push")
}

bool Near(DWORD obj, int x, int y, int tol)
{
    int ox = 0, oy = 0;
    return Rd(obj + 0x1c, ox) && Rd(obj + 0x20, oy) && std::abs(ox - x) <= tol && std::abs(oy - y) <= tol;
}

// Rimette un eroe in una posizione con push e pop del gestore degli oggetti (le stesse funzioni che
// usano loadHero e "loadplayer") e controlla il risultato: l'oggetto deve essere tornato nel mondo e
// vicino alla posizione. Se il pop non riesce prova le posizioni di ripiego, una dopo l'altra.
bool PlaceHero(DWORD mgr, DWORD id, DWORD addr, const BYTE* const* positions, int count, const char* name)
{
    for (int i = 0; i < count; ++i) {
        int px = 0, py = 0;
        memcpy(&px, positions[i] + 4, 4); memcpy(&py, positions[i] + 8, 4);
        const bool t = TeleportObject(mgr, id, positions[i]);
        if (!t) { LogException(name); return false; }
        const bool ok = InWorld(addr) && Near(addr, px, py, 150);
        Log("NATIVO: SpawnHero2: %s rimesso a (%d,%d) (posizione %d di %d): %s", name, px, py, i + 1, count, ok ? "ok" : "non riuscito");
        if (ok) return true;
    }
    return false;
}


// 0.55.39: mappa scoperta tenuta quando compare l'eroe 2. Prova del 31/10 sera: "minimappa di nuovo nera dove gia' siamo passati".
// Studio: ogni settore della mappa ha 1024 bit di zona scoperta (32x32, +0x180; 0x622f20 li legge, 0x622f40 li sovrascrive e
// accende il flag 0x1000 di +0x30, 0x622fb0 li somma con OR, mai chiamata dal gioco; 0x639360 ne fa la tessera della nebbia,
// FOGOFWAR01.TGA). Stanno nel file dell'eroe: caricandolo (0x606fbd: settore 0x634c70(indice), 0x606fd0: call 0x622f40) i bit dei
// settori salvati sostituiscono quelli del mondo. La mod crea l'eroe 2 caricando il suo HeroNN.pax ("sys loadplayer"), quindi
// la zona scoperta dall'eroe 1 tornava come la conosceva l'eroe 2. Durante SpawnHero2 la chiamata in 0x606fd0 va a 0x622fb0
// (stesso tipo: thiscall, un argomento, ret 4): le due mappe si sommano. Fuori dalla creazione dell'eroe 2 resta 0x622f40.
volatile DWORD g_exploredSet = 0x00622f40, g_exploredOr = 0x00622fb0;
volatile BYTE g_exploredOrMode = 0;
constexpr DWORD kExploredLoadCall = 0x00606fd0;

__declspec(naked) void ExploredLoadStub()   // al posto di "call 0x622f40" in 0x606fd0: ECX = settore, argomento = bit
{
    __asm {
        cmp  byte ptr [g_exploredOrMode], 0
        jne  orbits
        jmp  dword ptr [g_exploredSet]
    orbits:
        jmp  dword ptr [g_exploredOr]
    }
}

// Crea l'eroe 2 accanto all'eroe 1, nel thread del motore: "sys loadplayer N" carica Save/HeroNN.pax come
// oggetto 2 e, per come e' fatto loadHero, rimette nel mondo l'oggetto con l'id scritto nel file (l'eroe 1)
// nella posizione salvata nel file: l'eroe 1 viene quindi spostato lontano e va rimesso dov'era.
void SpawnHero2(DWORD engine, int fileNo)
{
    static DWORD lastSpawnTick = 0;
    DWORD mgr = 0, player = 0, heroId = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player);
    if (player) RdHero(player, heroId);
    View vw;
    if (!ReadView(vw)) { Log("NATIVO: SpawnHero2: mondo non attivo (menu o caricamento), annullato"); return; }
    DWORD hero = mgr ? ObjectAddr(mgr, heroId) : 0;
    if (!hero || !InWorld(hero)) { Log("NATIVO: SpawnHero2: eroe 1 non trovato o fuori dal mondo, annullato"); return; }
    // 0.54.0: con l'oggetto 2 di qualcun altro (salvataggio non ancora tolto, partita in rete) "loadplayer" lo distruggerebbe
    // senza copia: si rinuncia, come prima con l'eroe 2 gia' presente
    if (RawHero2(mgr)) { Log("NATIVO: SpawnHero2: l'oggetto 2 esiste gia' (%s), annullato", ObjectAddr(mgr, 2) ? "eroe 2 della mod" : "dal salvataggio o dalla rete"); return; }
    if (lastSpawnTick && GetTickCount() - lastSpawnTick < 5000) { Log("NATIVO: SpawnHero2: troppo presto dopo il precedente, annullato"); return; }
    lastSpawnTick = GetTickCount();
    BYTE pos1[16] = {};
    RdBuf(hero + 0x18, pos1, sizeof(pos1));
    int hx = 0, hy = 0; Rd(hero + 0x1c, hx); Rd(hero + 0x20, hy);
    {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\Save\\Hero%02d.pax", GameDir(), fileNo);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { Log("NATIVO: SpawnHero2: %s non esiste, annullato", path); return; }
    }
    DWORD heroType = 0; Rd(hero + 0x10, heroType);
    Log("NATIVO: SpawnHero2: eroe 1 a (%d,%d) tipo oggetto %lu, carico hero%02d.pax", hx, hy, heroType, fileNo);
    InstallCrashVeh();
    g_vehAny.store(0);                                  // 0.53.5: le eccezioni dei momenti dopo la comparsa si registrano
    // Censimento degli oggetti prima e dopo (0.36.1): nella prova del 27/10 i soldati della missione sparivano alla
    // comparsa dell'eroe 2. Per ogni id: puntatore e flag +0x14.
    struct Snap { DWORD p, fl; };
    auto census = [&](std::map<DWORD, Snap>& m) {
        DWORD beg = 0, end = 0;
        if (!Rd(mgr + 4, beg) || !Rd(mgr + 8, end) || end <= beg) return;
        for (DWORD id = 0; id < (end - beg) / 4 && id < 20000; ++id) {
            DWORD p = 0, fl = 0;
            if (!Rd(beg + id * 4, p) || !p) continue;
            Rd(p + 0x14, fl);
            m[id] = Snap{ p, fl };
        }
    };
    std::map<DWORD, Snap> before, after;
    if (g_cfg.nativeVerbose) census(before);
    char cmd[64];
    // la sintassi del gioco vuole i due punti: "loadplayer : N" (token 0x3a = 0x1a, poi atoi); senza, il numero era
    // ignorato e si caricava sempre Hero06 (il nano), come nelle prove precedenti (27/10, 0.36.0)
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "sys loadplayer : %d", fileNo);
    g_h2Spawning.store(true);
    g_exploredOrMode = g_cfg.nativeKeepMap ? 1 : 0;   // 0.55.39: la mappa scoperta del file dell'eroe 2 si somma a quella del mondo
    const bool ok = EngineConsole(engine, cmd);
    g_exploredOrMode = 0;
    if (const DWORD made = ok ? RawHero2(mgr) : 0) {    // 0.54.0: l'oggetto nuovo e' l'eroe 2 della mod, nato da HeroNN.pax
        g_h2MineFile.store(fileNo);
        g_h2MineAddr.store(made);
        g_h2SpawnSeq.fetch_add(1);
    }
    g_h2Spawning.store(false);
    if (g_cfg.nativeVerbose) {
        census(after);
        int gone = 0, pushed = 0, added = 0;
        for (const auto& kv : before) {
            auto it = after.find(kv.first);
            if (it == after.end() || it->second.p != kv.second.p) {
                if (gone++ < 15) { DWORD ty = 0; int x = 0, y = 0; Rd(kv.second.p + 0x10, ty); Rd(kv.second.p + 0x1c, x); Rd(kv.second.p + 0x20, y); Log("NATIVO: censimento: id %lu sparito/sostituito (tipo %lu pos %d,%d)", kv.first, ty, x, y); }
            } else if (((it->second.fl ^ kv.second.fl) & 2) && pushed++ < 15) {
                DWORD ty = 0; int x = 0, y = 0; Rd(it->second.p + 0x10, ty); Rd(it->second.p + 0x1c, x); Rd(it->second.p + 0x20, y);
                Log("NATIVO: censimento: id %lu cambia fuori-dal-mondo %08lX -> %08lX (tipo %lu pos %d,%d)", kv.first, kv.second.fl, it->second.fl, ty, x, y);
            }
        }
        for (const auto& kv : after) if (!before.count(kv.first)) ++added;
        Log("NATIVO: censimento oggetti: prima %d, dopo %d, spariti %d, nuovi %d, cambio flag %d", static_cast<int>(before.size()), static_cast<int>(after.size()), gone, added, pushed);
    }
    DWORD h2 = ObjectAddr(mgr, 2);
    hero = ObjectAddr(mgr, heroId);                 // riletto: l'oggetto potrebbe essere stato ricreato
    Log("NATIVO: SpawnHero2: console %s, eroe 2 %s, eroe 1 %s", ok ? "eseguita" : "FALLITA", h2 ? "presente" : "assente", hero ? "presente" : "ASSENTE");
    if (!ok) { LogException("sys loadplayer"); Log("NATIVO: SpawnHero2: eccezione nella console, niente riposizionamento (stato da non usare)"); return; }
    if (!h2 || !hero) { Log("NATIVO: SpawnHero2: eroe mancante dopo il comando, niente riposizionamento"); return; }
    int x2 = 0, y2 = 0, x1 = 0, y1 = 0;
    Rd(h2 + 0x1c, x2); Rd(h2 + 0x20, y2); Rd(hero + 0x1c, x1); Rd(hero + 0x20, y1);
    DWORD type2 = 0; Rd(h2 + 0x10, type2);
    Log("NATIVO: SpawnHero2: dopo il comando eroe 1 (%d,%d), eroe 2 (%d,%d) tipo oggetto %lu", x1, y1, x2, y2, type2);
    BYTE pos2[16] = {};
    RdBuf(h2 + 0x18, pos2, sizeof(pos2));
    // eroe 1: prima nella posizione di partenza; se il pop non riesce, dove sta l'eroe 2 (posizione valida
    // perche' appena occupata dal gioco)
    if (!(Near(hero, hx, hy, 150) && InWorld(hero))) {
        const BYTE* const tries[] = { pos1, pos2 };
        if (!PlaceHero(mgr, heroId, hero, tries, 2, "eroe 1")) Log("NATIVO: SpawnHero2: ERRORE: eroe 1 non rimesso, controlla il gioco");
    }
    RdBuf(hero + 0x18, pos1, sizeof(pos1));          // posizione attuale dell'eroe 1 (dopo il ripristino)
    {
        int cx = 0, cy = 0; Rd(hero + 0x1c, cx); Rd(hero + 0x20, cy);
        if (!(Near(h2, cx, cy, 150) && InWorld(h2))) {
            const BYTE* const tries[] = { pos1 };
            if (!PlaceHero(mgr, 2, h2, tries, 1, "eroe 2")) Log("NATIVO: SpawnHero2: ERRORE: eroe 2 non rimesso accanto all'eroe 1");
        }
    }
    // Posizione del modello (0.32.5): push/pop cambiano solo la posizione intera (+0x1c/+0x20); le posizioni
    // in virgola mobile (+0x7c..+0xa0, +0xd8..) e quella di Granny restavano dove il gioco aveva messo
    // l'oggetto durante loadplayer (nel punto salvato nel file), e un ordine "vai a questo punto" non faceva
    // partire l'eroe 2 (traccia del 27/10, confronto automatico). 0x44d8d0(1, 0) le riallinea alla posizione
    // intera, come fa lo stesso loadplayer dopo il suo pop. thiscall, due argomenti, ret 8.
    for (const DWORD obj : { h2, hero }) {
        const CallResult cr = CallThis2(0x0044d8d0, obj, 1, 0);
        if (!cr.ok) { LogException("0x44d8d0"); }
        else if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x44d8d0 ha lasciato lo stack sbilanciato (corretto)");
        else Log("NATIVO: SpawnHero2: posizione del modello riallineata per l'oggetto %08lX", obj);
    }
    Rd(h2 + 0x1c, x2); Rd(h2 + 0x20, y2); Rd(hero + 0x1c, x1); Rd(hero + 0x20, y1);
    Log("NATIVO: SpawnHero2: finale eroe 1 (%d,%d) %s, eroe 2 (%d,%d) %s", x1, y1, InWorld(hero) ? "nel mondo" : "FUORI DAL MONDO", x2, y2,
        InWorld(h2) ? "nel mondo" : "FUORI DAL MONDO");
    g_spawned.store(true);
    if (g_cfg.nativeVerbose) {   // confronto automatico eroe 1 / eroe 2 (tutti e due locali): cosa manca all'eroe 2 per camminare
        BYTE a[0x600], b[0x600];
        if (RdBuf(hero, a, sizeof(a)) && RdBuf(h2, b, sizeof(b))) {
            Log("NATIVO: confronto automatico eroe 1 (%08lX) / eroe 2 (%08lX): dword diversi", hero, h2);
            int n = 0;
            for (DWORD off = 0; off < sizeof(a) && n < 220; off += 4) {
                DWORD x = *reinterpret_cast<DWORD*>(a + off), y = *reinterpret_cast<DWORD*>(b + off);
                if (x != y) { Log("NATIVO:   +0x%03lX  eroe1 %08lX  eroe2 %08lX", off, x, y); ++n; }
            }
        }
    }
}
}   // namespace

// 0.49.3: pubblica (dichiarata in SacredPad.h); nella 0.49.2 stava nello spazio anonimo e il linker non la trovava.
bool MemoryCameraHeroOffset(float& fx, float& fy) { return CameraHeroOffsetImpl(fx, fy); }

// 0.53.8: direzione sullo schermo della destinazione che il gioco ha dato all'eroe comandato (+0x158/+0x15c, destinazione x
// e y come nei punti di controllo della 0.32.7), dalla differenza delle proiezioni, che non dipende dal centro della vista.
// Prova sul PC del 30/10 (10:55:53): tastierino 4 tenuto, punto della mod a sinistra dell'eroe, eroe andato a destra; serve
// a capire se il gioco riceve un punto sbagliato o gira intorno a un ostacolo. Falso se l'eroe non ha una destinazione.
bool MemoryHeroDestDir(float& dx, float& dy, int& hx, int& hy, int& tx, int& ty)
{
    dx = dy = 0; hx = hy = tx = ty = 0;
    if (!g_ok) return false;
    View v{};
    DWORD player = 0, id = 0, mgr = 0;
    if (!ReadView(v) || !Rd(kPlayer, player) || !player || !RdHero(player, id) || !id || !Rd(kObjMgr, mgr) || !mgr) return false;
    const DWORD h = ObjectAddr(mgr, id);
    if (!h || !Rd(h + 0x1c, hx) || !Rd(h + 0x20, hy) || !Rd(h + 0x158, tx) || !Rd(h + 0x15c, ty)) return false;
    if (!tx || !ty || (std::abs(tx - hx) + std::abs(ty - hy) < 20) || std::abs(tx - hx) > 20000 || std::abs(ty - hy) > 20000) return false;
    float ax = 0, ay = 0, bx = 0, by = 0;
    if (!WorldToScreen(v, hx, hy, ax, ay) || !WorldToScreen(v, tx, ty, bx, by)) return false;
    const float ex = bx - ax, ey = by - ay, n = std::sqrt(ex * ex + ey * ey);
    if (n < 1.0f) return false;
    dx = ex / n; dy = ey / n;
    return true;
}

// 0.53.4: punto dei piedi dell'eroe comandato sullo schermo (coordinate client) con la camera sul punto medio. Prova sulla
// Steam Machine del 30/10 (00:47-00:49): subito dopo la comparsa dell'eroe 2 il centro dello stick era giusto, poi andava
// sempre piu' a sinistra (00:48:22: stick a destra, punto di arrivo a x 360 con il centro a 960) e l'eroe 1 lo seguiva:
// la proiezione di CameraHeroOffset (centro della vista in cWorldView0+0x14/+0x18) non da' la posizione disegnata quando
// la camera e' spostata. Fa fede la lista dei rettangoli che il gioco disegna (la stessa dei clic sui bersagli, che vanno a
// segno): il rettangolo dell'eroe, o quello dell'eroe 2 piu' la differenza delle proiezioni dei due eroi, che non dipende
// dal centro della vista. Ultima risorsa la proiezione, come prima. Piedi a 0.92 dell'altezza del rettangolo.
bool MemoryCameraHeroScreen(float& sx, float& sy)
{
    if (!g_ok || !g_camOn.load()) return false;
    View v{};
    DWORD player = 0, id = 0, mgr = 0;
    if (!ReadView(v) || !Rd(kPlayer, player) || !player || !RdHero(player, id) || !id || !Rd(kObjMgr, mgr) || !mgr) return false;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    const DWORD h1 = ObjectAddr(mgr, id), h2 = id != 2 ? ObjectAddr(mgr, 2) : 0;
    if (!h1 || !Rd(h1 + 0x1c, x1) || !Rd(h1 + 0x20, y1)) return false;
    float p1x = 0, p1y = 0, p2x = 0, p2y = 0;
    const bool w1 = WorldToScreen(v, x1, y1, p1x, p1y);
    const bool w2 = h2 && Rd(h2 + 0x1c, x2) && Rd(h2 + 0x20, y2) && WorldToScreen(v, x2, y2, p2x, p2y);
    std::vector<PickRect> rs;
    const PickRect* r1 = nullptr; const PickRect* r2 = nullptr;
    if (ReadPickRects(v, rs))
        for (const PickRect& r : rs) {
            if (r.w <= 0 || r.h <= 0) continue;
            if (r.id == id && !r1) r1 = &r;
            else if (r.id == 2 && id != 2 && !r2) r2 = &r;
        }
    int src = 0;                                         // 1 rettangolo dell'eroe, 2 rettangolo dell'eroe 2, 3 proiezione
    if (r1) { sx = r1->x + r1->w * 0.5f; sy = r1->y + r1->h * 0.92f; src = 1; }
    else if (r2 && w1 && w2) { sx = r2->x + r2->w * 0.5f + (p1x - p2x); sy = r2->y + r2->h * 0.92f + (p1y - p2y); src = 2; }
    else if (w1) { sx = p1x; sy = p1y; src = 3; }
    else return false;
    if (v.w > 0 && v.h > 0 && (sx < v.w * 0.03f || sx > v.w * 0.97f || sy < v.h * 0.03f || sy > v.h * 0.97f)) return false;   // fuori: centro
    static DWORD logAt = 0;
    static int lastSrc = -1;
    const DWORD now = GetTickCount();
    if (src != lastSrc || now - logAt > 30000) {           // 0.53.6: ogni 30 s (ogni 3 s riempiva il log)
        lastSrc = src; logAt = now;
        Log("Camera sul punto medio: eroe %lu sullo schermo (%.0f,%.0f) da %s; proiezione (%.0f,%.0f)%s", id, sx, sy,
            src == 1 ? "il suo rettangolo" : src == 2 ? "il rettangolo dell'eroe 2" : "la proiezione", p1x, p1y,
            r2 ? "" : ", eroe 2 senza rettangolo");
    }
    return true;
}

// Cerca GameServer.exe e controlla se la nostra DLL e' caricata nel server; se manca la carica con una
// LoadLibrary remota (0.34.1: nella prova del 27/10 il server non ha prodotto SacredPad_server.log).
void FindAndInjectServer(bool inject)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { Log("NATIVO: server: elenco dei processi non disponibile"); return; }
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"GameServer.exe") != 0) continue;
            found = true;
            bool hasUs = false;
            HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
            if (ms != INVALID_HANDLE_VALUE) {
                MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
                if (Module32FirstW(ms, &me)) {
                    do {
                        wchar_t self[MAX_PATH] = {};
                        GetModuleFileNameW(SelfModule(), self, MAX_PATH);
                        if (_wcsicmp(me.szExePath, self) == 0) hasUs = true;
                    } while (Module32NextW(ms, &me));
                }
                CloseHandle(ms);
            }
            Log("NATIVO: server: GameServer.exe PID %lu, la nostra DLL %s", pe.th32ProcessID, hasUs ? "e' gia' caricata" : "NON e' caricata");
            if (!hasUs && inject) {
                char self[MAX_PATH] = {};
                GetModuleFileNameA(SelfModule(), self, MAX_PATH);
                HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                if (!hp) { Log("NATIVO: server: OpenProcess fallita (%lu)", GetLastError()); continue; }
                const size_t n = strlen(self) + 1;
                LPVOID remote = VirtualAllocEx(hp, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                SIZE_T wr = 0;
                bool ok = remote && WriteProcessMemory(hp, remote, self, n, &wr);
                HANDLE th = ok ? CreateRemoteThread(hp, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA")), remote, 0, nullptr) : nullptr;
                if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); Log("NATIVO: server: DLL caricata nel server con una LoadLibrary remota"); }
                else Log("NATIVO: server: iniezione fallita (%lu)", GetLastError());
                CloseHandle(hp);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!found) Log("NATIVO: server: GameServer.exe non e' in esecuzione (il server di questa partita e' dentro Sacred.exe?)");
}

// Elenco dei giocatori della sessione (0.34.2). Il server di una partita singola e' dentro Sacred.exe (nella
// prova del 27/10 non c'e' nessun GameServer.exe): le creature ostili scelgono il bersaglio fra i giocatori
// dell'elenco {contatore a 0xad2b48, voci da 8 byte a 0xad2c60: id dell'eroe e un secondo dword} (ciclo
// "while (i < DAT_00ad2b48) FUN_005374c0(*voce)" in FUN_004ef560 e altre; il getter 0x5b7cc0 restituisce 1 se
// l'indice e' fuori elenco). Con un solo giocatore l'elenco e' {1}: l'eroe 2 non e' mai un bersaglio. Qui
// l'eroe 2 viene aggiunto come secondo giocatore finche' esiste, e tolto quando sparisce.
constexpr DWORD kPlCount = 0x00AD2B48, kPlList = 0x00AD2C60;

void PlayerListSync(bool want, DWORD heroId)
{
    static int lastState = -1;       // 0 = elenco normale, 1 = con l'eroe 2
    DWORD cnt = 0, e0 = 0, e0b = 0, e1 = 0, e1b = 0;
    if (!Rd(kPlCount, cnt) || !Rd(kPlList, e0) || !Rd(kPlList + 4, e0b) || !Rd(kPlList + 8, e1) || !Rd(kPlList + 12, e1b)) return;
    static bool dumped = false;
    if (want && !dumped) {
        dumped = true;
        DWORD v[20] = {};
        RdBuf(kPlCount - 0x10, v, sizeof(v));
        Log("NATIVO: elenco giocatori: contatore %lu, voci {%lX,%lX} {%lX,%lX}; dump 0xad2b38: %lX %lX %lX %lX | %lX %lX %lX %lX", cnt, e0, e0b, e1, e1b,
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
    if (want) {
        if (cnt == 1 && e0 == heroId) {
            WriteDword(kPlList + 8, 2);
            WriteDword(kPlList + 12, e0b);
            WriteDword(kPlCount, 2);                      // per ultimo: l'IA legge prima il contatore
            if (lastState != 1) Log("NATIVO: elenco giocatori: aggiunto l'eroe 2 (contatore 2)");
            lastState = 1;
        } else if (cnt >= 2 && e1 == 2) lastState = 1;
    } else if (cnt == 2 && e1 == 2) {
        WriteDword(kPlCount, 1);
        WriteDword(kPlList + 8, 0);
        if (lastState != 0) Log("NATIVO: elenco giocatori: tolto l'eroe 2 (contatore 1)");
        lastState = 0;
    }
}

// --- Campagna cooperativa (0.36.0): voce del menu principale e scelta dell'eroe 2 ---------------------------------
// La voce sta nella riga libera fra "Extra" e "Esci". Il clic apre un elenco (Overlay.cpp) con gli eroi di Save\HeroNN.pax;
// la scelta vale per la sessione ("armata"): ogni partita avviata o caricata crea l'eroe 2 con il file scelto.
// Con l'elenco aperto un gancio WH_GETMESSAGE butta via mouse e tastiera destinati al gioco.
std::atomic<bool> g_coopArmed{ false };
std::atomic<bool> g_pickBlock{ false };
// 0.47.3: controller. Il clic del pad (A in modalita' cursore) arriva come WM_LBUTTONDOWN e g_virt.lmb, non come tasto
// vero, e il puntatore del pad e' g_virt.x/y, non il mouse: senza questo A sulla voce "Co-op" non apriva l'elenco.
// Con l'elenco aperto Core.cpp manda qui la croce (su/giu'), A (conferma) e B (annulla).
std::atomic<bool> g_pickOpen{ false };
std::atomic<int> g_pickPadMove{ 0 }, g_pickPadOk{ 0 }, g_pickPadCancel{ 0 };
bool NativeCoopPickerOpen() { return g_pickOpen.load(); }
void NativeCoopPickerPad(int move, bool ok, bool cancel)
{
    if (move) g_pickPadMove += move;
    if (ok) ++g_pickPadOk;
    if (cancel) ++g_pickPadCancel;
}
HHOOK g_pickHook = nullptr;

LRESULT CALLBACK PickMsgProc(int code, WPARAM w, LPARAM l)
{
    if (code >= 0 && w == PM_REMOVE && g_pickBlock.load()) {
        MSG* m = reinterpret_cast<MSG*>(l);
        if ((m->message >= WM_MOUSEFIRST && m->message <= WM_MOUSELAST) || (m->message >= WM_KEYFIRST && m->message <= WM_KEYLAST) ||
            m->message == WM_INPUT)
            m->message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

struct HeroEntry { int fileNo; std::string name; int level = 0, cls = 0; std::string when; };   // when: ultima partita (0.55.51)

// 0.51.7: livello e classe dell'eroe dal file: nel chunk 0xc3, dword a +0 = livello e +4 = classe (verificato il 29/10 sui
// sei HeroNN.pax del tester: Serafina livello 2 classe 1 come nel gioco, Dwarf e Daemon livello 29 classi 8 e 9; classi come
// in RuneClass: 1 Serafina, 2 Gladiatore, 3 Mago, 4 Elfo oscuro, 5 Elfa, 6 Vampira, 8 Nano, 9 Demone).
bool ReadHeroInfo(const char* path, int& level, int& cls)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> d(1 << 20);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, d.data(), static_cast<DWORD>(d.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 0x100 + 12 || memcmp(d.data(), "AMH\x1b", 4) != 0) return false;
    for (int i = 0; i < 48 && 0x100 + 12 * (i + 1) <= static_cast<int>(got); ++i) {
        DWORD e[3];
        memcpy(e, &d[0x100 + 12 * i], 12);
        if (e[0] == 0) break;
        if (e[0] != 0xc3 || e[1] + 8 > got) continue;
        DWORD v[2];
        memcpy(v, &d[e[1]], 8);
        if (v[0] < 1 || v[0] > 300 || v[1] > 20) return false;
        level = static_cast<int>(v[0]); cls = static_cast<int>(v[1]);
        return true;
    }
    return false;
}

const char* HeroClassName(int cls)
{
    // nomi della versione italiana del gioco (il Daemon e' "la Diavola", il tester 29/10)
    static const char* const k[] = { "?", "Serafina", "Gladiatore", "Mago", "Elfo oscuro", "Elfa dei boschi", "Vampira", "?", "Nano", "Diavola" };
    return (cls >= 0 && cls < 10) ? Tr(k[cls]) : "?";
}

// Nome dell'eroe: chunk 0xc3 del .pax, testo UTF-16 a +8 (tabella a 0x100, voci da 12 byte: tipo, offset, dimensione).
bool ReadHeroName(const char* path, std::string& name)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> d(1 << 20);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, d.data(), static_cast<DWORD>(d.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 0x100 + 12 || memcmp(d.data(), "AMH\x1b", 4) != 0) return false;
    for (int i = 0; i < 48 && 0x100 + 12 * (i + 1) <= static_cast<int>(got); ++i) {
        DWORD e[3];
        memcpy(e, &d[0x100 + 12 * i], 12);
        if (e[0] == 0) break;
        if (e[0] != 0xc3 || e[1] >= got || e[2] <= 8) continue;
        const DWORD avail = (std::min)(e[2], got - e[1]);
        if (avail <= 8) continue;
        const DWORD nch = (std::min)(static_cast<DWORD>(40), (avail - 8) / 2);
        wchar_t w[48] = {};
        memcpy(w, &d[e[1] + 8], nch * 2);
        char a[96] = {};
        WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof(a) - 1, nullptr, nullptr);
        name = a;
        return !name.empty();
    }
    return false;
}

std::vector<HeroEntry> ListHeroes()
{
    std::vector<HeroEntry> v;
    for (int n = 0; n < 100; ++n) {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\Save\\Hero%02d.pax", GameDir(), n);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        HeroEntry e{ n, "" };
        if (!ReadHeroName(path, e.name)) e.name = "(eroe illeggibile)";
        ReadHeroInfo(path, e.level, e.cls);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        FILETIME lt{}; SYSTEMTIME t{};
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt) && FileTimeToSystemTime(&lt, &t)) {
            char w[32];
            _snprintf_s(w, sizeof(w), _TRUNCATE, "%02u/%02u %02u:%02u", t.wDay, t.wMonth, t.wHour, t.wMinute);
            e.when = w;
        }
        v.push_back(e);
    }
    return v;
}

void NativeCoopMenu(HWND hwnd, bool inWorld)
{
    if (!g_cfg.nativeMenu || !hwnd) return;
    const int kPage = OverlayPickRows();   // 0.55.52: righe visibili dell'elenco a questa risoluzione (8 a 1080 righe)
    static bool init = false, open = false, rowOn = false, lmbWas = false;
    static bool kUp = false, kDn = false, kEnt = false, kEsc = false, kPu = false, kPd = false;
    static DWORD unblockAt = 0, openedAt = 0;
    static EscRow row;
    static std::vector<HeroEntry> heroes;
    static std::vector<std::string> lines;
    static int sel = 0, top = 0;
    static int splitRow = -1, heroBase = 1;   // 0.47.2: riga "schermo diviso" (-1 se non disponibile) e prima riga degli eroi
    static std::string armedName, lastStatus;
    const DWORD now = GetTickCount();
    if (!init) {
        init = true;
        OverlaySetMainLabel("Due giocatori");   // 0.47.4: era "Co-op" (scelta del tester del 28/10)
        if (!g_pickHook) g_pickHook = SetWindowsHookExA(WH_GETMESSAGE, PickMsgProc, SelfModule(), GetWindowThreadProcessId(hwnd, nullptr));
        Log("CAMPAGNA: menu attivo, gancio messaggi %s", g_pickHook ? "installato" : "NON installato");
    }
    if (unblockAt && static_cast<int>(now - unblockAt) >= 0) { unblockAt = 0; g_pickBlock.store(false); }
    {   // scritta di stato in alto a sinistra finche' non si e' in partita
        std::string st;
        if (!inWorld && g_coopArmed.load()) { char b[160]; _snprintf_s(b, sizeof(b), _TRUNCATE, Tr("Due giocatori: eroe 2 = %s"), armedName.c_str()); st = b; }
        if (st != lastStatus) { lastStatus = st; OverlayCoopStatus(st.c_str()); }
    }
    RECT cr{};
    GetClientRect(hwnd, &cr);
    POINT mp{};
    bool havePos;
    if (g_virt.active.load()) { mp.x = g_virt.x.load(); mp.y = g_virt.y.load(); havePos = ScreenToClient(hwnd, &mp) != 0; }   // puntatore del pad
    else havePos = RealGetCursorPos(&mp) && ScreenToClient(hwnd, &mp);
    const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || g_virt.lmb.load();
    const bool lmbEdge = lmb && !lmbWas;
    lmbWas = lmb;
    auto close = [&](bool unblockLater) {
        open = false;
        g_pickOpen.store(false);
        OverlayCoopPicker(false, lines, sel, top, -1);
        if (unblockLater) unblockAt = now + 250; else { unblockAt = 0; g_pickBlock.store(false); }
    };
    if (open) {
        if (inWorld) { close(false); return; }
        OverlaySetMainRow(false, row.r, false, row.font, row.color, row.hoverColor);
        auto edge = [](int vk, bool& was) { const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0; const bool e = d && !was; was = d; return e; };
        const int n = static_cast<int>(lines.size());
        bool confirm = false, cancel = false;
        if (edge(VK_UP, kUp) && sel > 0) --sel;
        if (edge(VK_DOWN, kDn) && sel < n - 1) ++sel;
        if (edge(VK_PRIOR, kPu)) sel = (std::max)(0, sel - kPage);
        if (edge(VK_NEXT, kPd)) sel = (std::min)(n - 1, sel + kPage);
        if (edge(VK_RETURN, kEnt) && static_cast<int>(now - openedAt) > 300) confirm = true;
        if (edge(VK_ESCAPE, kEsc) && static_cast<int>(now - openedAt) > 300) cancel = true;
        {   // 0.47.3: controller (croce, A, B) da Core.cpp
            const int mv = g_pickPadMove.exchange(0);
            sel = (std::max)(0, (std::min)(n - 1, sel + mv));
            if (g_pickPadOk.exchange(0) && static_cast<int>(now - openedAt) > 300) confirm = true;
            if (g_pickPadCancel.exchange(0) && static_cast<int>(now - openedAt) > 300) cancel = true;
        }
        int hover = -1;
        if (havePos) {
            const int hit = OverlayCoopPickerHit(mp.x, mp.y, cr.right, cr.bottom, n, top);
            if (hit >= 0) hover = hit;
            if (lmbEdge && static_cast<int>(now - openedAt) > 150) {
                if (hit >= 0) { sel = hit; confirm = true; }
                else if (hit == -2) cancel = true;
            }
        }
        if (sel < top) top = sel;
        if (sel >= top + kPage) top = sel - kPage + 1;
        if (confirm) {
            if (sel == splitRow) {
                // 0.47.2: la co-op a schermo diviso del ramo main (due finestre, fase 4) dalla stessa voce del menu
                g_coopArmed.store(false);
                close(true);
                OverlayCoopToast(Tr("Schermo diviso: avvio delle due finestre"), 4000);
                Log("CAMPAGNA: scelta la co-op a schermo diviso");
                CoopStartFromMenu(hwnd);
                return;
            }
            if (sel == heroBase - 1) {
                g_coopArmed.store(false);
                OverlayCoopToast(Tr("Partita normale: nessun eroe 2"), 4000);
                Log("CAMPAGNA: scelta annullata, partita normale");
            } else {
                const HeroEntry& he = heroes[sel - heroBase];
                g_cfg.nativeHero2 = he.fileNo;
                char v[16];
                _snprintf_s(v, sizeof(v), _TRUNCATE, "%d", he.fileNo);
                SaveConfigValue("Native", "Hero2File", v);
                armedName = he.name;
                g_coopArmed.store(true);
                char t[160];
                _snprintf_s(t, sizeof(t), _TRUNCATE, Tr("Eroe 2: %s. Ora avvia o carica una partita."), he.name.c_str());
                OverlayCoopToast(t, 6000);
                Log("CAMPAGNA: eroe 2 = Hero%02d.pax (%s)", he.fileNo, he.name.c_str());
            }
            close(true);
            return;
        }
        if (cancel) { close(true); return; }
        OverlayCoopPicker(true, lines, sel, top, hover);
        return;
    }
    // elenco chiuso: la riga del menu
    if (inWorld) {
        if (rowOn) { rowOn = false; OverlaySetMainRow(false, row.r, false, row.font, row.color, row.hoverColor); }
        return;
    }
    rowOn = MemoryMainMenuRow(row);    // 0.49.6: a ogni giro (la ricerca lenta la limita lei)
    const bool hov = rowOn && havePos && mp.x >= row.r.left && mp.x < row.r.right && mp.y >= row.r.top && mp.y < row.r.bottom;
    {   // 0.55.59: clic, Invio o A fuori dalla voce: si cambia schermata, la voce si nasconde per 1,5 s
        static bool entWas = true;
        const bool ent = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        if ((lmbEdge && !hov) || (ent && !entWas)) MemoryMainMenuRowHide(1500);
        entWas = ent;
    }
    if (static_cast<LONG>(now - g_mmHideUntil.load()) < 0) rowOn = false;
    OverlaySetMainRow(rowOn, row.r, hov, row.font, row.color, row.hoverColor);
    if (rowOn && hov && lmbEdge) {
        heroes = ListHeroes();
        lines.clear();
        // 0.47.2: una sola voce "Co-op" per le due co-op. In cima la co-op a schermo diviso (se il gioco normale la puo'
        // avviare: [Coop] Enabled, nessuna sessione in corso), poi la campagna cooperativa sullo stesso schermo.
        splitRow = -1;
        if (g_cfg.coopRole == 0 && g_cfg.coopEnabled && !CoopSessionActive()) {
            splitRow = 0;
            // 0.56.58 (il tester, 10/11: "se no uno avvia e ha uno schermo piu' grosso dell'altro"): le due finestre (WindowSize, di
            // serie 1280x720) stanno affiancate solo se lo schermo e' largo almeno il doppio; altrimenti la mod le mette ai bordi e
            // l'ospite copre una parte dell'host. La riga lo dice, e dice anche se manca la cartella dell'ospite (GuestDir).
            char split[200];
            const std::string gexe = std::string(g_cfg.coopGuestDir) + "\\Sacred.exe";
            if (GetFileAttributesA(gexe.c_str()) == INVALID_FILE_ATTRIBUTES)
                _snprintf_s(split, sizeof(split), _TRUNCATE, "%s", Tr("Schermo diviso\tmanca la cartella del gioco copiata con \" P2\" nel nome"));
            else if (g_cfg.coopW > 0 && g_cfg.coopScreenW > 0 && 2 * g_cfg.coopW > g_cfg.coopScreenW)
                _snprintf_s(split, sizeof(split), _TRUNCATE, Tr("Schermo diviso\tfinestre sovrapposte: serve uno schermo largo %d (questo %d)"),
                            2 * g_cfg.coopW, g_cfg.coopScreenW);
            else _snprintf_s(split, sizeof(split), _TRUNCATE, "%s", Tr("Schermo diviso\tdue finestre, un pad ciascuno"));
            lines.push_back(std::string("\x01S") + split);   // 0.55.43: distintivo
        }
        // 0.51.6: righe a due colonne (descrizione, tabulazione, dettaglio) e "\x02" = linea di separazione dal gruppo sopra
        lines.push_back(std::string("\x02\x01N") + Tr("Stesso schermo, solo eroe 1\tpartita normale"));
        heroBase = static_cast<int>(lines.size());
        sel = heroBase - 1;
        for (const HeroEntry& he : heroes) {
            char t[200], d[120], lv[16] = "";
            // 0.55.51: livello a parte ("\x05" livello "\x05", riquadro a destra), sotto il nome classe, file e ultima partita
            if (he.level > 0) _snprintf_s(lv, sizeof(lv), _TRUNCATE, "\x05%d\x05", he.level);
            const char* cls = he.level > 0 ? HeroClassName(he.cls) : "";
            if (!he.when.empty()) _snprintf_s(d, sizeof(d), _TRUNCATE, Tr("%s   Hero%02d   giocato il %s"), cls, he.fileNo, he.when.c_str());
            else _snprintf_s(d, sizeof(d), _TRUNCATE, "%s   Hero%02d", cls, he.fileNo);
            _snprintf_s(t, sizeof(t), _TRUNCATE, Tr("Stesso schermo con %s"), he.name.c_str());
            const char code[3] = { '\x01', static_cast<char>(he.cls >= 1 && he.cls <= 9 ? '0' + he.cls : '0'), 0 };   // 0.55.43: distintivo
            lines.push_back(std::string(code) + lv + t + "\t" + d);
            if (g_coopArmed.load() && he.fileNo == g_cfg.nativeHero2) sel = static_cast<int>(lines.size()) - 1;
        }
        top = sel >= kPage ? sel - kPage + 1 : 0;
        open = true;
        g_pickOpen.store(true);
        g_pickPadMove.store(0); g_pickPadOk.store(0); g_pickPadCancel.store(0);
        openedAt = now;
        kUp = kDn = kEnt = kEsc = kPu = kPd = true;     // tasti gia' premuti all'apertura non contano
        g_pickBlock.store(true);
        OverlaySetMainRow(false, row.r, false, row.font, row.color, row.hoverColor);
        OverlayCoopPicker(true, lines, sel, top, -1);
        Log("CAMPAGNA: elenco aperto, %d eroi in Save", static_cast<int>(heroes.size()));
    }
}

// Esperienza all'eroe 2 con la funzione del gioco (0.45.6). addExperience 0x57e410 (thiscall, this = oggetto statistiche
// della creatura a +0x3a8, il cui +4 punta alla creatura; argomento: esperienza da aggiungere; docs/ghidra_morte.txt) somma
// l'esperienza (+0xc = creatura +0x3b4), calcola il livello (0x57fb80) e per ogni livello nuovo da' i punti attributo
// (0x581140), i punti abilita' (+0x42 = creatura +0x3ea) e gli slot delle abilita' nuove ai livelli della tabella 0xad3718
// (+0x44 = creatura +0x3ec), poi applica il livello (0x565010). Il controllo e l'aggiornamento della copia anti-trucchi li fa
// solo per l'eroe locale. Prima la mod scriveva +0x3b4 e chiamava solo 0x565010: livelli senza punti (prova del 28/10).
// Solo nel thread del motore.
// 0.55.61: suono della salita di livello per l'eroe 2 (il tester, 02/11: "quando sale di livello non ha il suono, anche se ha
// almeno l'effetto visivo"). addExperience lo suona solo se la creatura e' l'eroe locale ([0x7d7f30()+0x14], 0x57f10c):
// 0x677750() (senza argomenti, ret) da' il gestore dei suoni [0x17e8f24], poi 0x67ae80 (thiscall, ret 0x28) con
// (9, 0x10, 0, 0, 0, 0, 0, 0, 0, 0), come a 0x57f121..0x57f134. Solo nel thread del motore.
CallResult CallLevelUpSound()
{
    CallResult cr;
    DWORD before = 0, after = 0;
    __try {
        __asm {
            mov  before, esp
            mov  eax, 0x00677750
            call eax
            test eax, eax
            jz   nosnd
            mov  ecx, eax
            xor  edx, edx
            push edx
            push edx
            push edx
            push edx
            push edx
            push edx
            push edx
            push edx
            push 0x10
            push 9
            mov  eax, 0x0067ae80
            call eax
        nosnd:
            mov  after, esp
            mov  esp, before
        }
        cr.ok = true; cr.balanced = (after == before);
    } __except (CaptureException(GetExceptionInformation())) {
        cr.ok = false;
    }
    return cr;
}

bool AddExperienceHero2(DWORD add)
{
    DWORD m = 0, back = 0;
    Rd(kObjMgr, m);
    const DWORD e2 = m ? ObjectAddr(m, 2) : 0;
    if (!e2 || !add || !Rd(e2 + 0x3ac, back) || back != e2) return false;
    if (Hero2IsLocal()) { g_xpPending += add; Log("EROI: esperienza +%lu per l'eroe 2 tenuta da parte (interfaccia sull'eroe 2)", add); return false; }
    DWORD x0 = 0; WORD l0 = 0, sp0 = 0, sl0 = 0;
    Rd(e2 + 0x3b4, x0); Rd(e2 + 0x3fe, l0); Rd(e2 + 0x3ea, sp0); Rd(e2 + 0x3ec, sl0);
    const CallResult cr = CallThis1(0x0057e410, e2 + 0x3a8, add);
    if (!cr.ok) { LogException("addExperience dell'eroe 2"); return false; }
    if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x57e410 ha lasciato lo stack sbilanciato (corretto)");
    DWORD x1 = 0; WORD l1 = 0, sp1 = 0, sl1 = 0;
    Rd(e2 + 0x3b4, x1); Rd(e2 + 0x3fe, l1); Rd(e2 + 0x3ea, sp1); Rd(e2 + 0x3ec, sl1);
    if (l1 != l0) Log("EROI: l'eroe 2 sale di livello con addExperience: %u -> %u, punti abilita' %u -> %u, abilita' nuove %u -> %u", l0, l1, sp0, sp1, sl0, sl1);
    if (l1 > l0) {
        const CallResult cs = CallLevelUpSound();
        if (!cs.ok) LogException("suono della salita di livello dell'eroe 2");
        else if (!cs.balanced) Log("NATIVO: ATTENZIONE: il suono della salita di livello ha lasciato lo stack sbilanciato (corretto)");
    }
    Log("EROI: esperienza all'eroe 2 (addExperience): +%lu, %lu -> %lu (livello %u)", add, x0, x1, l1);
    return true;
}

// 0.55.25: niente piu' raccolta fatta dalla mod (FindNearestItem, PickupHero2, raccolta automatica della 0.39-0.55.23, rune
// alla classe della 0.47.1, ripiego di A della 0.55.22). Regola del tester (30/10): "l'eroe 2 non deve poter raccogliere cose
// senza premere il tasto"; raccoglie come l'eroe 1, con A sull'oggetto (ordine di tipo 7) e B (raccogli tutto, 0x113). In piu'
// pickupItem (0x558340) chiamato dalla mod fuori dal suo percorso e' il sospetto principale del crash all'uscita al menu
// principale del 30/10 alle 17:40 (4 raccolte cosi' in quella partita, nessuna nelle uscite senza crash).

// Classe di una runa (0.47.1, N11). Prova della 0.47.0 (docs/log_0470.txt): nella voce del tipo della tabella dinamica
// ([0xaaa564] + tipo*0x80) le rune hanno il modello "upgrade_<classe>.grn" (sera, gladi, mage, delf, welf, vamp, demon,
// dwarf; anche il byte +0x70 cambia con la classe); "Runenstein01.grn" e' una runa senza classe. Restituisce il tipo
// dell'eroe di quella classe (1 Serafina, 2 Gladiatore, 3 Mago, 4 Elfo oscuro, 5 Elfa, 6 Vampira, 8 Nano, 9 Demone) o 0.
DWORD RuneClass(DWORD type)
{
    if (type < 5375 || type > 5782) return 0;                 // TYPE_SPELL_UPGRADE_* e TYPE_SMOVE_UPGRADE_*
    static std::map<DWORD, DWORD> cache;
    const auto it = cache.find(type);
    if (it != cache.end()) return it->second;
    DWORD tab = 0, cls = 0;
    char e[0x31] = {};
    if (Rd(kQuestTable, tab) && tab && RdBuf(tab + type * 0x80 + 0x30, e, 0x30)) {
        for (int i = 0; i + 8 < 0x30; ++i) {
            if (_strnicmp(e + i, "upgrade_", 8) != 0) continue;
            static const struct { const char* k; DWORD c; } m[] = { { "sera", 1 }, { "gladi", 2 }, { "mage", 3 }, { "delf", 4 }, { "welf", 5 },
                                                                    { "vamp", 6 }, { "dwarf", 8 }, { "demon", 9 } };
            for (const auto& x : m) if (_strnicmp(e + i + 8, x.k, strlen(x.k)) == 0) cls = x.c;
            break;
        }
    }
    cache[type] = cls;
    return cls;
}
std::atomic<int> g_pad2SwapReq{ 0 };   // 0.44.0: Back del pad 2 (come Home)
// 0.50.0 (N4, fase A): lo scambio partito dal Back del pad 2 da' al pad 2 anche il puntatore (Core.cpp usa il suo stato al
// posto di quello del pad 1). Eventi per Core.cpp: 1 = puntatore al pad 2, 2 = restituito con Back del pad 2, 3 = restituito
// per un altro motivo (Home, Esc, morte dell'eroe 2, uscita dalla partita).
std::atomic<bool> g_pad2Ui{ false };
std::atomic<int> g_pad2UiEvent{ 0 };
std::atomic<bool> g_pad2ReturnReq{ false };   // 0.55.49: finestre chiuse dal giocatore 2, il puntatore torna al pad 1
std::atomic<bool> g_pad2InvToggleReq{ false }; // 0.55.50: Back del pad 2 tenuto con il puntatore suo: zaino (tasto I, Core.cpp)
std::atomic<bool> g_pad2SideReq{ false };      // 0.55.54: Back del pad 2 con i due zaini possibili: apre o chiude il suo lato (Core.cpp)
bool NativePad2TakeSideReq() { return g_pad2SideReq.exchange(false); }
bool MemoryDualSide(int who);
bool MemoryDualPossible();
bool NativePad2TakeInvToggle() { return g_pad2InvToggleReq.exchange(false); }
bool MemoryDualActive();
void NativePad2ReturnPointer() { g_pad2ReturnReq.store(true); }
bool NativePad2HasPointer() { return g_pad2Ui.load() && g_swapOrig.load() != 0; }
int NativePad2UiEvent() { return g_pad2UiEvent.exchange(0); }
void NativePad2UiEventPutBack(int ev) { int z = 0; g_pad2UiEvent.compare_exchange_strong(z, ev); }   // 0.55.18: se nel frattempo non ne e' arrivato un altro

// Oggetto tenuto sul puntatore (0.50.0). Il gioco lo legge con 0x6554e0 (oggetto unico in [0xcdcadc]) e 0x655ad0
// ([+0x68]): NULL se non c'e' nulla, altrimenti una struttura con il tipo a +0 (word, 1 = oggetto, come controlla 0x6d6840
// quando si clicca un ritratto con un oggetto in mano) e l'id a +4. Verificato nel gioco il 29/10 (id 981). Dalla 0.50.1 serve
// solo al log e all'avviso del passaggio di oggetti fra i due eroi (l'oggetto in mano e' gia' staccato dallo zaino).
bool MemoryHeldItem(DWORD* type, DWORD* id)
{
    DWORD s = 0, p = 0;
    if (!Rd(0x00cdcadc, s) || !s || !Rd(s + 0x68, p) || !p) return false;
    WORD t = 0; DWORD i = 0;
    if (!Rd(p, t) || t != 1) return false;     // solo gli oggetti (tipo 1), come 0x6d6840
    Rd(p + 4, i);
    if (type) *type = t;
    if (id) *id = i;
    return true;
}

// --- 0.51.0 (N4 fase B, strada C): scambio della sola interfaccia ---------------------------------------------------
// Studio del 29/10 (sezione 8b): il gioco elabora solo le creature il cui proprietario (+0x39) e' il giocatore locale
// [player+0x14] (81 controlli), quindi lo scambio che scrive [player+0x14] = 2 mette in pausa il mondo. Qui [player+0x14]
// resta 1 e si devia verso l'eroe 2 solo cio' che legge l'eroe locale nel codice delle finestre (0x6a0000-0x6f0000):
// (1) 107 chiamate a GetHeroId 0x604380 (creatura di [player+0x14]) puntano a UiGetHeroStub, che con lo scambio attivo
// restituisce la creatura dell'eroe 2; (2) 46 chiamate a 0x7d7f30 (il giocatore [0x182db68]) seguite subito da una
// lettura di [eax+0x14] puntano a UiPlayerStub, che con lo scambio attivo restituisce un giocatore finto con +0x14 = 2.
// Le 46 sono state scelte con uno script (capstone): dopo la lettura eax non viene piu' usato (sovrascritto o perso alla
// chiamata successiva), quindi il giocatore finto serve solo per quel campo. Esclusi di proposito i punti della minimappa e
// della mappa (cUI_OverviewMap 0x6c70d9, cUI_Megamap 0x6c7f13 e le loro funzioni 0x6c9600, 0x6cb490, 0x6cccb0): le mappe
// restano sull'eroe 1, come la camera. Senza scambio i due stub saltano alle funzioni
// originali, quindi le modifiche restano installate per tutta la partita (si installano una volta, nel thread del motore,
// che e' quello che esegue le finestre: nessuna chiamata a meta' modifica).
static const DWORD kUiHeroCalls[] = {
    0x006a2883, 0x006a2c56, 0x006a323a, 0x006a3f96, 0x006a4076, 0x006a4e8b, 0x006a4f20, 0x006a60b7,
    0x006a6303, 0x006a66d2, 0x006a8520, 0x006aa10f, 0x006aa21f, 0x006aa70b, 0x006aa74d, 0x006ab410,
    0x006ad3a2, 0x006af3bc, 0x006b0633, 0x006b0f62, 0x006b3f77, 0x006b6966, 0x006b6ce4, 0x006b7161,
    0x006b7391, 0x006b787a, 0x006b79aa, 0x006bd599, 0x006bd870, 0x006bd896, 0x006bd8b8, 0x006bd8d8,
    0x006bd8f8, 0x006bdb04, 0x006bdbab, 0x006bdc5f, 0x006be52e, 0x006be80f, 0x006becc9, 0x006bf1b9,
    0x006bf297, 0x006bf5e0, 0x006bf84d, 0x006c044f, 0x006c059d, 0x006c06e8, 0x006c0ec5, 0x006c0fab,
    0x006c0fba, 0x006c101f, 0x006c17a8, 0x006c1e37, 0x006c1f0c, 0x006c1fa5, 0x006c2032, 0x006c2290,
    0x006c2381, 0x006c2545, 0x006c262b, 0x006c2705, 0x006c637b,
    0x006d032a, 0x006d2556, 0x006d2d60, 0x006d34fa, 0x006d3750,
    0x006d4049, 0x006d40fe, 0x006d418c, 0x006d463e, 0x006d48b0, 0x006d4c83, 0x006d4d7f, 0x006d543c,
    0x006d56d8, 0x006d5b79, 0x006d5bd5, 0x006d5f6e, 0x006d61e5, 0x006d6701, 0x006d71f6, 0x006d8937,
    0x006da015, 0x006da024, 0x006da2a6, 0x006da4d9, 0x006da51e, 0x006da52d, 0x006daa67, 0x006daa76,
    0x006dada8, 0x006db396, 0x006df081, 0x006dfee9, 0x006dff40, 0x006dffc8, 0x006e02c9, 0x006e16b7,
    0x006e17d4, 0x006e1b4b, 0x006e2697, 0x006e3d6c, 0x006e46c3, 0x006e6560, 0x006e8711, 0x006e8724,
    0x006ea4dd,
    // 0.55.39: disegno delle griglie (zaino, deposito, fabbro, mercante, cubo, scambio; 0x5dc900, chiamato solo da quelle finestre):
    // GetHeroId e poi, per ogni oggetto, 0x54afe0 (l'eroe puo' usarlo?); se no la casella diventa rossa (0x40ff0000). Prova del
    // 31/10 sera: armi, armature e rune dell'eroe 2 rosse nel suo zaino ("vede la classe dell'eroe 1").
    0x005dc9bf,
};
static const DWORD kUiPlayerCalls[] = {
    0x006a4478, 0x006a44b7, 0x006a6407, 0x006a7536, 0x006a7e50, 0x006a84c9, 0x006aa1c2, 0x006ab3e9,
    0x006ad3cd, 0x006b6b3f, 0x006b6c86, 0x006c0155, 0x006c0317, 0x006c05f8,
    0x006d035d, 0x006d0369, 0x006d1499, 0x006d14dc, 0x006d2822, 0x006d2d79,
    0x006d5abe, 0x006d5ac6, 0x006d7205, 0x006d8b1f, 0x006daf29, 0x006db044, 0x006db21b, 0x006db352,
    0x006db680, 0x006dbc9b, 0x006dbdc2, 0x006de090, 0x006de409, 0x006de8de, 0x006de931, 0x006dea37,
    0x006dea52, 0x006deeb7, 0x006defb9, 0x006df7d2, 0x006e1f35, 0x006e5cf4, 0x006e975c, 0x006e98a9,
    0x006ea508, 0x006ebcf7,
};
// 0.51.1: la cintura delle pozioni. Il clic su uno slot della barra (cUI_Taskbar2 vf10) non beve da solo: manda al motore
// l'evento 0x17, che cEngine vf00 passa a FUN_00613290 (usa lo slot), fuori dal codice delle finestre; li' GetHeroId sceglie
// chi beve (0x61329d: eroe e sua voce dell'inventario; 0x61337a: ramo del tasto destro) e FUN_005dad90 (chiamata solo da
// 0x613290 e dalla barra 0x6e3d50) sceglie la pozione in base alla vita dell'eroe locale (tre chiamate). Prova del 29/10:
// con lo scambio la pozione l'aveva bevuta l'eroe 1. Durante lo scambio anche i tasti della cintura valgono per l'eroe 2.
static const DWORD kUiBeltCalls[] = { 0x0061329d, 0x0061337a, 0x005dadb3, 0x005dadcc, 0x005dade9 };
constexpr DWORD kGetHeroId = 0x00604380, kGetPlayer = 0x007d7f30;
volatile DWORD g_uiHero = 0;            // creatura dell'eroe 2 mostrata dalle finestre (0 = nessuna deviazione)
volatile DWORD g_uiOn = 0;              // 1 = le letture di [giocatore+0x14] delle finestre vedono 2
volatile DWORD g_beltHero = 0;          // 0.51.2: eroe che beve per la cintura, solo per la durata di UseBeltHero2 (thread del motore)
DWORD g_uiFakePlayer[0x40] = {};        // giocatore finto: le 46 letture deviate usano solo +0x14
std::atomic<bool> g_uiPatched{ false }; // tutte le 158 chiamate deviate (altrimenti si usa lo scambio della 0.50)
std::atomic<bool> g_swapUi{ false };    // lo scambio della sola interfaccia chiesto da HeroSwapTick (thread della DLL)
// 0.53.2: g_uiHero, g_uiOn e g_uiFakePlayer li scrive solo il thread del motore. Prima li scriveva anche HeroSwapTick, dal
// thread della DLL, mentre le finestre giravano: una funzione delle finestre che chiama GetHeroId e poi legge [player+0x14]
// (comprare, vendere, maestro) poteva vedere l'eroe 2 nella prima lettura e l'id 1 nella seconda, o il contrario, se lo
// scambio cadeva fra le due. Ora HeroSwapTick cambia solo g_swapUi e UiSwapLatch, in EngineThreadTick (EndScene, fra un
// fotogramma e l'altro), lo riporta sulle deviazioni: dentro un fotogramma le finestre vedono sempre lo stesso eroe.
std::atomic<bool> g_swapUiLive{ false }; // lo scambio come lo vedono le finestre in questo fotogramma (thread del motore)
std::atomic<DWORD> g_engineTickAt{ 0 }; // ultimo EngineThreadTick: se il motore non disegna, il ritorno non puo' aspettarlo

__declspec(naked) void UiGetHeroStub()   // thiscall come 0x604380 (ECX = gestore degli oggetti), nessun argomento
{
    __asm {
        mov  eax, g_uiHero
        test eax, eax
        jz   orig
        ret
    orig:
        mov  eax, 0x00604380            // kGetHeroId
        jmp  eax
    }
}

__declspec(naked) void UiBeltStub()      // 0.51.2: come UiGetHeroStub, ma prima l'eroe della pozione del pad 2 (g_beltHero)
{
    __asm {
        mov  eax, g_beltHero
        test eax, eax
        jnz  done
        mov  eax, g_uiHero
        test eax, eax
        jz   orig
    done:
        ret
    orig:
        mov  eax, 0x00604380            // kGetHeroId
        jmp  eax
    }
}

__declspec(naked) void UiPlayerStub()    // come 0x7d7f30: nessun argomento, restituisce il giocatore in EAX
{
    __asm {
        cmp  g_uiOn, 0
        je   orig
        lea  eax, g_uiFakePlayer
        ret
    orig:
        mov  eax, 0x007d7f30            // kGetPlayer
        jmp  eax
    }
}
// Le schede dell'inventario (cUI_Inventory*) mostrano la voce dell'inventario del gioco [DAT_00ad33c8, 32 voci da 0x1180]
// con indice ushort a +0x158 dell'oggetto (nel codice di 0x6c0270: FUN_005df160(*(ushort*)(this+0x158))): 1 per l'eroe
// 1. Con lo scambio si porta a 2 (e si ripristina). Si cercano i puntatori agli oggetti dentro le finestre dell'interfaccia
// (cUI_Manager + 0x80 + n*4). Prova del 27/10 con la 0.40.0: dopo Home ritratto e attributi erano dell'eroe 2, lo zaino e
// le armi dell'eroe 1. log: stampa il risultato (solo allo scambio).
std::atomic<bool> g_invMoved{ false };   // lo zaino era aperto e la voce e' stata spostata: va richiuso e riaperto (0.40.4)

// 0.40.2: nella prova della 0.40.1 la ricerca dentro le finestre non ha trovato nulla (0 oggetti). Gli oggetti
// dell'interfaccia si cercano allora in tutta la memoria per vtable (RTTI: cUI_Equipment 0x895490, cUI_InventoryTab
// 0x8955c4, cUI_Inventory3 0x895668, cUI_Character 0x895fa8); una sola volta per scambio, poi l'elenco si riusa.
WORD SlotCountOf(DWORD hero);   // piu' avanti, con la seconda barra
void RetargetInventoryUi(DWORD from, DWORD to, bool log)
{
    struct Cls { DWORD vt; const char* name; bool modify; };
    static const Cls cls[] = { { 0x895490, "cUI_Equipment", false }, { 0x8955c4, "cUI_InventoryTab", true }, { 0x895668, "cUI_Inventory3", false }, { 0x895fa8, "cUI_Character", false } };
    static std::vector<std::pair<DWORD, int>> objs;       // indirizzo, indice della classe
    static DWORD scannedAt = 0;
    // 0.41.2: si cerca a ogni Home (log) e, se l'ultima ricerca non ha trovato nulla, al massimo ogni 20 s (prima con
    // l'elenco vuoto la ricerca in tutta la memoria si ripeteva a ogni chiamata, cioe' ogni 500 ms).
    {
        if (log || (objs.empty() && static_cast<int>(GetTickCount() - scannedAt) > 20000)) {
            objs.clear();
            std::vector<BYTE> buf(1 << 20);
            MEMORY_BASIC_INFORMATION mbi{};
            for (DWORD addr = 0x10000; addr < 0x7FFE0000 && VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi)); addr = static_cast<DWORD>(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)) {
                if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) continue;
                const DWORD base = static_cast<DWORD>(reinterpret_cast<uintptr_t>(mbi.BaseAddress));
                for (size_t off = 0; off < mbi.RegionSize; off += buf.size()) {
                    SIZE_T got = 0;
                    const size_t want = (std::min)(buf.size(), static_cast<size_t>(mbi.RegionSize) - off);
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(static_cast<uintptr_t>(base + off)), buf.data(), want, &got)) continue;
                    for (size_t k = 0; k + 4 <= got; k += 4) {
                        const DWORD v = *reinterpret_cast<DWORD*>(&buf[k]);
                        if (v < 0x895490 || v > 0x895fa8) continue;
                        for (int c = 0; c < 4; ++c) if (v == cls[c].vt) objs.push_back({ static_cast<DWORD>(base + off + k), c });
                    }
                }
            }
            scannedAt = GetTickCount();
        }
    }
    int changed = 0;
    for (const auto& o : objs) {
        DWORD vt = 0;
        if (!Rd(o.first, vt) || vt != cls[o.second].vt) continue;        // oggetto distrutto o memoria riusata
        WORD idx = 0; Rd(o.first + 0x158, idx);
        if (log) {
            char line[400] = {}; int n = 0;
            for (DWORD off = 0x100; off < 0x1a0 && n < 12; off += 2) { WORD w = 0; if (Rd(o.first + off, w) && (w == from || w == to)) { char t[24]; _snprintf_s(t, sizeof(t), _TRUNCATE, " +%03lX=%u", off, w); strncat_s(line, t, _TRUNCATE); ++n; } }
            Log("SCAMBIO: %s a %08lX, +0x158 = %u, campi uguali a %lu/%lu:%s", cls[o.second].name, o.first, idx, from, to, line);
        }
        if (cls[o.second].modify && idx == from) {
            const WORD nv = static_cast<WORD>(to);
            WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(o.first + 0x158)), &nv, 2, nullptr);
            ++changed;
        }
    }
    if (log) Log("SCAMBIO: oggetti dell'interfaccia trovati: %d, indice cambiato in %d (da %lu a %lu)", static_cast<int>(objs.size()), changed, from, to);
    if (log) {   // barra in basso (cUI_Taskbar2, finestra 0, vtable 0x895ac0): +0x154 = id dell'eroe, +0x158 = voce dell'inventario
        // (ushort), +0x15c = numero di slot. Gli slot (armi, arti, pozioni) si ricostruiscono da quella voce con 0x6e0310(numero).
        // Nulla di cio' che e' nelle statistiche o nelle voci cambia: solo i due campi dell'interfaccia.
        const DWORD tb = VisibleWindow(0, 0x00895ac0);
        if (tb) {
            DWORD id = 0; WORD ei = 0, cnt = 0;
            Rd(tb + 0x154, id); Rd(tb + 0x158, ei); Rd(tb + 0x15c, cnt);
            Log("SCAMBIO: barra %08lX: +0x154 = %lu, +0x158 = %u, slot %u", tb, id, ei, cnt);
            if (id == from && ei == from) {
                const WORD nv = static_cast<WORD>(to);
                WriteDword(tb + 0x154, to);
                WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(tb + 0x158)), &nv, 2, nullptr);
                RunOnGameThread([=] {
                    DWORD vt = 0, mgr = 0;
                    if (!Rd(tb, vt) || vt != 0x00895ac0) return;
                    // 0.53.7: con gli slot dell'eroe che la barra mostra ora (prima quelli dell'eroe di prima: con eroi di
                    // livello diverso la barra vera dell'eroe 2 aveva il numero di slot dell'eroe 1)
                    WORD n = 0;
                    if (Rd(kObjMgr, mgr) && mgr) n = SlotCountOf(ObjectAddr(mgr, to));
                    if (n < 1 || n > 5) n = cnt;
                    const CallResult cr = CallThis1(0x006e0310, tb, n);
                    if (!cr.ok) { LogException("0x6e0310 barra"); return; }
                    if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x6e0310 ha lasciato lo stack sbilanciato (corretto)");
                    Log("SCAMBIO: barra ricostruita (slot %u) per l'eroe %lu", n, to);
                });
            }
        } else Log("SCAMBIO: barra non trovata (finestra 0 non visibile)");
    }
    if (log) {   // lo zaino e' disegnato dalla voce dell'inventario [DAT_00ad33c8 + idx*0x1180] con il bit 8 di +0x1162 (aperta):
        // se era aperta la voce dell'eroe di partenza, la si chiude (come fa 0x5d94b0(0)) e si apre quella dell'altro
        DWORD inv = 0;
        if (Rd(0x00ad33c8, inv) && inv && from < 0x20 && to < 0x20) {
            const DWORD ef = inv + from * 0x1180, et = inv + to * 0x1180;
            WORD a = 0, b2 = 0;
            Rd(ef + 0x1162, a); Rd(et + 0x1162, b2);
            Log("SCAMBIO: voci dell'inventario %lu (flag %04X) e %lu (flag %04X)", from, a, to, b2);
            if (a & 8) {
                DWORD v = 0;
                Rd(ef + 0x116c, v);
                a = static_cast<WORD>(a & 0xfff7); b2 = static_cast<WORD>(b2 | 8);
                WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(ef + 0x1162)), &a, 2, nullptr);
                WriteDword(ef + 0x1170, v);
                WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(et + 0x1162)), &b2, 2, nullptr);
                g_invMoved.store(true);
                Log("SCAMBIO: zaino aperto passato dalla voce %lu alla voce %lu", from, to);
            }
        }
    }
}

// Copia protetta dell'anti-trucchi (0.45.5). Il gioco tiene una copia nascosta dei valori dell'eroe "locale" ([player+0x14])
// in due pagine protette, puntate da DAT_0182dddc e DAT_0182dde0 (PAGE_NOACCESS fuori dai controlli), con la chiave k = byte
// +0x38 dell'eroe ripetuto 4 volte: pagina 1 [0] esperienza +0x3b4 ^ k, [1] +0x3ee ^ k, [2] livello +0x3fe ^ k, byte 0x20..0x27 =
// +0x3c4..+0x3cb, byte 0x30..0x37 = +0x3d4..+0x3db (abilita'), word a 0x4e..0x58 = +0x3f2..+0x3fc (attributi); pagina 2 [0..2]
// come [0..2] con k << 1. I controlli (0x5909e0, il salvataggio 0x619f70 e altri punti, per esempio quando si guadagna
// esperienza) confrontano l'eroe locale con la copia e, se non coincide, rimettono livello ed esperienza a 1, uccidono l'eroe
// facendolo esplodere (0x4e91b0(1, 2)) e accendono DAT_0182ddd0, il segno "hardcore", per cui l'eroe NON rinasce. Prove del
// 28/10 alle 13:57, 14:30 e 14:31: dopo uno scambio con Home (eroe locale = eroe 2) la copia non era piu' quella dell'eroe 1 e
// alla prima uccisione l'eroe 1 e' esploso senza rinascere. Rimedio: a ogni cambio dell'eroe locale fatto dalla mod, e ogni 2 s
// mentre c'e' l'eroe 2, la copia si riscrive con i valori veri dell'eroe locale. Solo nel thread del motore.
void SyncAntiCheat(DWORD hero, const char* why)
{
    DWORD p1 = 0, p2 = 0;
    if (!hero || !Rd(0x0182dddc, p1) || !Rd(0x0182dde0, p2) || !p1 || !p2) return;
    BYTE kb = 0; DWORD xp = 0, f3ee = 0; WORD lvl = 0;
    BYTE bonus[8] = {}, skill[8] = {}; WORD attr[6] = {};
    if (!Rd(hero + 0x38, kb) || !Rd(hero + 0x3b4, xp) || !Rd(hero + 0x3ee, f3ee) || !Rd(hero + 0x3fe, lvl) ||
        !RdBuf(hero + 0x3c4, bonus, 8) || !RdBuf(hero + 0x3d4, skill, 8) || !RdBuf(hero + 0x3f2, attr, 12)) return;
    const DWORD k = 0x01010101u * kb, k2 = k << 1;
    DWORD o1 = 0, o2 = 0;
    BYTE* a = reinterpret_cast<BYTE*>(static_cast<uintptr_t>(p1));
    BYTE* b = reinterpret_cast<BYTE*>(static_cast<uintptr_t>(p2));
    if (!VirtualProtect(a, 0x1000, PAGE_READWRITE, &o1)) return;
    const bool same = p2 >= p1 && p2 < p1 + 0x1000;
    if (!same && !VirtualProtect(b, 0x1000, PAGE_READWRITE, &o2)) { VirtualProtect(a, 0x1000, o1, &o1); return; }
    __try {
        reinterpret_cast<DWORD*>(a)[0] = xp ^ k;
        reinterpret_cast<DWORD*>(a)[1] = f3ee ^ k;
        reinterpret_cast<DWORD*>(a)[2] = static_cast<DWORD>(lvl) ^ k;
        memcpy(a + 0x20, bonus, 8);
        memcpy(a + 0x30, skill, 8);
        memcpy(a + 0x4e, attr, 12);
        reinterpret_cast<DWORD*>(b)[0] = xp ^ k2;
        reinterpret_cast<DWORD*>(b)[1] = f3ee ^ k2;
        reinterpret_cast<DWORD*>(b)[2] = static_cast<DWORD>(lvl) ^ k2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!same) VirtualProtect(b, 0x1000, o2, &o2);
    VirtualProtect(a, 0x1000, o1, &o1);
    if (why) Log("NATIVO: copia dell'anti-trucchi riallineata all'eroe %08lX (%s): livello %u, esperienza %lu", hero, why, lvl, xp);
}

void SetLocalHero(DWORD player, DWORD id, const char* why)
{
    RunOnGameThread([=] {
        DWORD mgr = 0; Rd(kObjMgr, mgr);
        const DWORD h = mgr ? ObjectAddr(mgr, id) : 0;
        if (!h) return;
        WriteDword(player + 0x14, id);
        SyncAntiCheat(h, why);
    });
}

// 0.53.0: richiesta di scambio da un pulsante della barra dell'altro eroe (TaskbarInputHook): HeroSwapTick cambia eroe
// dell'interfaccia (come Home) e poi ripete il clic nello stesso punto, dove ora c'e' la barra vera di quell'eroe.
std::atomic<LONG> g_tbSwapReq{ 0 };           // MAKELPARAM(x, y) + 1 del clic da ripetere, 0 = nessuna
std::atomic<bool> g_tbSwapPending{ false };   // richiesta fatta, clic non ancora ripetuto: i clic sulla copia aspettano
std::atomic<LONG> g_tbCtxOn{ 0 };             // copia della barra in corso (TbOtherCtx): HeroSwapTick non tocca le deviazioni

// Scambio dell'eroe locale (0.40.0, prova): [player+0x14] e' l'eroe "locale" per l'interfaccia (inventario, statistiche,
// barra della vita, abilita'); [engine+0x13c] e' la creatura comandata da tastiera. Con Home si portano tutti e due
// sull'eroe 2 e si torna indietro. Il salvataggio del gioco controlla (anti-cheat) l'eroe locale contro una copia di
// controllo che vale solo per l'eroe 1: con lo scambio attivo andrebbe a vuoto e azzererebbe l'eroe 2. Per questo si
// torna all'eroe 1 premendo Esc, uscendo dalla partita o se l'eroe 2 muore o sparisce.
void HeroSwapTick(DWORD mgr, DWORD player, DWORD engine, bool inWorld, DWORD now)
{
    static bool homeWas = false, escWas = false;
    static int tapStep = 0; static DWORD tapAt = 0;
    if (tapStep && static_cast<int>(now - tapAt) >= 0 && g_gameHwnd.load()) {     // I, I: chiude e riapre lo zaino, cosi' la scheda si ricostruisce dalla voce nuova
        g_virt.keyUntil['I'].store(GetTickCount() + 80);
        const LPARAM lp = 1 | (0x17 << 16);
        PostMessageA(g_gameHwnd.load(), WM_KEYDOWN, 'I', lp);
        PostMessageA(g_gameHwnd.load(), WM_KEYUP, 'I', lp | (1 << 30) | (1u << 31));
        Log("SCAMBIO: tasto I %d di 2 (zaino richiuso e riaperto)", tapStep);
        tapStep = tapStep >= 2 ? 0 : tapStep + 1;
        tapAt = now + 400;
    }
    const bool home = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0, esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool padSwap = g_pad2SwapReq.exchange(0) > 0;   // 0.50.0: Back del pad 2, distinto da Home (il pad 2 prende il puntatore)
    const bool retReq = g_pad2ReturnReq.exchange(false);  // 0.55.49: vale solo durante uno scambio del pad 2 (mai per cominciarne uno)
    // 0.53.0: clic su un pulsante (zaino, personaggio...) della barra dell'altro eroe: scambio come con Home, poi lo stesso
    // clic ripetuto 250 ms dopo, quando nello stesso punto c'e' la barra vera di quell'eroe. Senza scambio fatto la
    // richiesta decade (dopo 1 s i clic sulla copia tornano a valere).
    static DWORD replayAt = 0, pendSince = 0; static LPARAM replayLp = 0;
    if (replayAt && static_cast<int>(now - replayAt) >= 0) {
        if (const HWND w = g_gameHwnd.load()) {
            PostMessageA(w, WM_LBUTTONDOWN, MK_LBUTTON, replayLp);
            PostMessageA(w, WM_LBUTTONUP, 0, replayLp);
            Log("SCAMBIO: clic ripetuto sulla barra in (%d,%d)", static_cast<SHORT>(LOWORD(replayLp)), static_cast<SHORT>(HIWORD(replayLp)));
        }
        replayAt = 0;
        g_tbSwapPending.store(false);
    }
    const LONG tbReq = g_tbSwapReq.exchange(0);
    if (tbReq) { replayLp = static_cast<LPARAM>(tbReq - 1); pendSince = now; }
    if (g_tbSwapPending.load() && !replayAt && !tbReq && static_cast<int>(now - pendSince) > 1000) g_tbSwapPending.store(false);
    const bool homeEdge = (home && !homeWas) || padSwap || tbReq != 0, escEdge = esc && !escWas;
    homeWas = home; escWas = esc;
    DWORD heldType = 0, heldId = 0;
    const bool held = inWorld && MemoryHeldItem(&heldType, &heldId);
    {   // 0.50.0: traccia dell'oggetto in mano, per verificare alla prova la lettura di [[0xcdcadc]+0x68]
        static bool heldWas = false;
        if (held != heldWas) {
            heldWas = held;
            if (held) Log("SCAMBIO: oggetto in mano (tipo %lu, id %lu)", heldType, heldId);
            else Log("SCAMBIO: nessun oggetto in mano");
        }
    }
    const DWORD orig = g_swapOrig.load();
    auto revert = [&](const char* why, int ev) {
        if (!orig || !player) return;
        if (held) Log("SCAMBIO: si torna all'eroe %lu con un oggetto in mano (tipo %lu, id %lu, %s)", orig, heldType, heldId, why);
        if (g_swapUi.exchange(false)) {          // 0.51.0: l'eroe locale non era cambiato
            // 0.53.2: le deviazioni le spegne UiSwapLatch al prossimo EndScene; se il motore non disegna da un po' (caricamento,
            // finestra ridotta) si spengono subito, perche' l'eroe 2 potrebbe sparire prima (spegnerle e' sempre sicuro)
            if (static_cast<int>(GetTickCount() - g_engineTickAt.load()) > 250) { g_uiOn = 0; g_uiHero = 0; g_swapUiLive.store(false); }
        }
        else {
            WriteDword(player + 0x14, orig);
            SetLocalHero(player, orig, "fine dello scambio");     // 0.45.5: anche la copia dell'anti-trucchi torna all'eroe 1
        }
        RetargetInventoryUi(2, orig, true);
        g_swapOrig.store(0);
        if (g_pad2Ui.exchange(false)) g_pad2UiEvent.store(ev);
        Log("SCAMBIO: tornati all'eroe %lu (%s)", orig, why);
    };
    // 0.50.1: passaggio di oggetti fra i due eroi. Nella 0.50.0 Home e Back con un oggetto in mano erano rifiutati; la prova
    // del tester del 29/10 (14:06, oggetto id 981 preso dallo zaino dell'eroe 2, scambio forzato, posato nello zaino dell'eroe 1)
    // ha mostrato che l'oggetto in mano e' gia' staccato dallo zaino: e' finito nell'eroe 1, nessun doppione, zaino
    // dell'eroe 2 a posto. Quindi si cambia eroe anche con l'oggetto in mano, ed e' cosi' che si passano gli oggetti.
    auto handOver = [&]() {
        if (!held) return;
        Log("SCAMBIO: passaggio con l'oggetto in mano (tipo %lu, id %lu)", heldType, heldId);
        OverlayCoopToast(Tr("Oggetto in mano: posalo nello zaino dell'altro eroe"), 3000);
    };
    if (!g_cfg.nativeSwap) { if (orig) revert("scambio disattivato", 3); return; }
    // 0.50.2 (studio della fase B): durante lo scambio il mondo si ferma. Bandiere del motore ([+0x54]: 0x800 salta la camera
    // in 0x610670, 0x4000 salta l'input in 0x617640; [0xab635c] bit 2, idem) e stato dei due eroi, prima e durante lo scambio.
    auto swapDiag = [&](const char* tag, DWORD h1id) {
        if (!g_cfg.memoryDiag) return;
        DWORD e54 = 0, b635c = 0, f1 = 0, s1 = 0, f2 = 0, s2 = 0, ctl = 0; WORD m1 = 0, m2 = 0;
        const DWORD a1 = ObjectAddr(mgr, h1id), a2 = ObjectAddr(mgr, 2);
        if (engine) { Rd(engine + 0x54, e54); Rd(engine + 0x13c, ctl); }
        Rd(0x00ab635c, b635c);
        if (a1) { Rd(a1 + 0x14, f1); Rd(a1 + 0x150, s1); Rd(a1 + 0xfc, m1); }
        if (a2) { Rd(a2 + 0x14, f2); Rd(a2 + 0x150, s2); Rd(a2 + 0xfc, m2); }
        Log("SCAMBIO DIAG (%s): engine+0x54 %08lX [+0x13c] %lu, [0xab635c] %08lX; eroe %lu +0x14 %08lX stato %08lX modo %04X; eroe 2 +0x14 %08lX stato %08lX modo %04X",
            tag, e54, ctl, b635c, h1id, f1, s1, m1, f2, s2, m2);
    };
    if (orig) {
        static DWORD diagAt = 0;
        if (static_cast<int>(now - diagAt) >= 1000) { diagAt = now; swapDiag("durante", orig); }
        static DWORD lastUi = 0;
        if (static_cast<int>(now - lastUi) > 500) { lastUi = now; RetargetInventoryUi(orig, 2, false); }
        DWORD hp = 0; const DWORD h2 = ObjectAddr(mgr, 2);
        // 0.53.2: l'indirizzo dell'eroe 2 per UiGetHeroStub lo aggiorna UiSwapLatch a ogni fotogramma, nel thread del motore
        if (!inWorld) revert("fuori dalla partita", 3);
        else if (escEdge) revert("tasto Esc (menu e salvataggio)", 3);
        else if (!h2 || !Rd(h2 + 0x4d8, hp) || hp == 0) revert("l'eroe 2 non e' vivo", 3);
        else if (g_swapUi.load() && (!ObjectAddr(mgr, orig) || !Rd(ObjectAddr(mgr, orig) + 0x4d8, hp) || hp == 0))
            revert("l'eroe 1 non e' vivo", 3);       // 0.51.0: col mondo che gira l'eroe 1 puo' cadere durante lo scambio
        else if (retReq && g_pad2Ui.load() && !held) revert("finestre chiuse dal giocatore 2", 2);
        else if (homeEdge) {
            handOver();
            revert(padSwap ? "Back del pad 2" : tbReq ? "pulsante della barra dell'eroe 1" : "tasto Home", padSwap ? 2 : 3);
            // con lo zaino aperto lo scambio lo passa gia' all'altro eroe: ripetere il clic sul pulsante lo chiuderebbe
            const bool moved = g_invMoved.exchange(false) && !MemoryDualActive();   // 0.55.50: con i due zaini niente I, I
            if (moved) { tapStep = 1; tapAt = now + 200; }
            if (tbReq && !padSwap && !moved) replayAt = now + 250;
        }
        return;
    }
    if (!homeEdge || !inWorld || !player || !engine) return;
    const DWORD h2 = ObjectAddr(mgr, 2);
    DWORD hp = 0, cur = 0;
    if (!h2 || !Rd(h2 + 0x4d8, hp) || hp == 0 || !Rd(player + 0x14, cur) || cur == 2 || cur == 0) { Log("SCAMBIO: non possibile (eroe 2 assente o morto)"); return; }
    handOver();
    swapDiag("prima", cur);
    // 0.51.0: con SwapUi (e le 158 chiamate deviate) l'eroe locale resta cur e solo le finestre vedono l'eroe 2
    const bool ui = g_cfg.nativeSwapUi && g_uiPatched.load();
    g_swapOrig.store(cur);
    if (ui) g_swapUi.store(true);       // 0.53.2: le deviazioni le accende UiSwapLatch al prossimo EndScene
    else SetLocalHero(player, 2, "inizio dello scambio");      // 0.45.5: [player+0x14] = 2 e copia dell'anti-trucchi dell'eroe 2
    RetargetInventoryUi(cur, 2, true);
    if (ui) Log("SCAMBIO: interfaccia sull'eroe 2, l'eroe locale resta %lu (il mondo continua)%s", cur, tbReq ? " (pulsante della barra dell'eroe 2)" : "");
    else Log("SCAMBIO: eroe locale %lu -> 2 (interfaccia e comandi dell'eroe 2)", cur);
    const bool moved = g_invMoved.exchange(false) && !MemoryDualActive();   // 0.55.50: con i due zaini niente I, I
    if (moved) { tapStep = 1; tapAt = now + 200; }
    if (tbReq && ui && !padSwap && !moved) replayAt = now + 250;
    if (padSwap) {   // 0.50.0 (N4): scambio chiesto dal pad 2, il puntatore passa a lui
        g_pad2Ui.store(true);
        g_pad2UiEvent.store(1);
        Log("SCAMBIO: puntatore al pad 2 (%s)", ui ? "lo stick sinistro del pad 1 muove l'eroe 1" : "il pad 1 resta fermo fino al ritorno");
    }
}

// 0.51.0 (N4 fase B): con lo scambio della sola interfaccia e il puntatore al pad 2, il pad 1 non ha puntatore ma il mondo
// gira, quindi il suo stick sinistro muove l'eroe 1 con ordini di movimento diretti (come Maiusc+F11, provato il 29/10).
// sdx, sdy = direzione sullo schermo (y verso il basso). Il punto di arrivo e' a 300 unita' nel mondo: rinnovato ogni
// ~150 ms mentre lo stick e' inclinato, l'eroe si ferma poco dopo il rilascio.
// 0.53.9: anche con la camera sul punto medio (Core, movimento del pad 1 nella co-op sullo stesso schermo): la direzione nel
// mondo si ricava dalle immagini di due passi del mondo sullo schermo, che non dipendono dal centro della vista.
bool MemoryNativeCameraOn() { return g_ok && g_camOn.load(); }

// 0.53.11: misura delle inversioni del movimento diretto (con MemoryDiag). Dall'ordine nella direzione opposta alla
// precedente, a ogni fotogramma si registra di quanto l'eroe 1 si e' spostato lungo la direzione nuova: separa il ritardo
// della mod (attesa in coda fino al thread del motore) da quello del gioco (l'eroe che rallenta e si gira).
namespace {
double QpcMs()
{
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c = {};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}
struct TurnTrack {      // solo nel thread del motore
    bool on = false;
    DWORD h1 = 0;
    double t0 = 0, queuedMs = 0, minAt = 0, backAt = -1;
    int x0 = 0, y0 = 0;
    float dx = 0, dy = 0, minP = 0;
    int n = 0;
    char buf[720] = {};
    size_t len = 0;
} g_turn;
float g_lastOrdDx = 0, g_lastOrdDy = 0;     // direzione dell'ultimo ordine di movimento diretto (thread del motore)

void TurnTrackOrder(DWORD h1, float dx, float dy, double queuedAt)
{
    const bool reverse = dx * g_lastOrdDx + dy * g_lastOrdDy < -0.5f;
    g_lastOrdDx = dx; g_lastOrdDy = dy;
    if (!reverse || !g_cfg.memoryDiag) return;
    int x = 0, y = 0;
    if (!Rd(h1 + 0x1c, x) || !Rd(h1 + 0x20, y)) return;
    g_turn = TurnTrack{};
    g_turn.on = true; g_turn.h1 = h1;
    g_turn.t0 = QpcMs(); g_turn.queuedMs = g_turn.t0 - queuedAt;
    g_turn.x0 = x; g_turn.y0 = y; g_turn.dx = dx; g_turn.dy = dy;
}

void TurnTrackTick()
{
    if (!g_turn.on) return;
    int x = 0, y = 0;
    if (!Rd(g_turn.h1 + 0x1c, x) || !Rd(g_turn.h1 + 0x20, y)) { g_turn.on = false; return; }
    const double t = QpcMs() - g_turn.t0;
    const float p = (x - g_turn.x0) * g_turn.dx + (y - g_turn.y0) * g_turn.dy;   // unita' lungo la direzione nuova
    if (p < g_turn.minP) { g_turn.minP = p; g_turn.minAt = t; }
    if (g_turn.backAt < 0 && g_turn.minP < 0 && p >= 0) g_turn.backAt = t;
    if (g_turn.len + 16 < sizeof(g_turn.buf)) {
        const int w = _snprintf_s(g_turn.buf + g_turn.len, sizeof(g_turn.buf) - g_turn.len, _TRUNCATE, " %.0f:%.0f", t, p);
        if (w > 0) g_turn.len += w;
    }
    ++g_turn.n;
    if (p < 40 && t < 1000 && g_turn.n < 80) return;
    g_turn.on = false;
    if (g_turn.minP >= 0) g_turn.backAt = 0;   // non e' mai andato indietro
    Log("Inversione: in coda %.0f ms, l'eroe va ancora indietro di %.0f unita' (fino a +%.0f ms), torna al punto di partenza "
        "a +%.0f ms, %s a +%.0f ms; campioni ms:unita'%s", g_turn.queuedMs, -g_turn.minP, g_turn.minAt, g_turn.backAt,
        p >= 40 ? "+40 unita'" : "misura interrotta", t, g_turn.buf);
}
} // namespace

// 0.55.36: il gioco ha rifiutato il punto davanti sul piano dell'eroe 1 (destinazione spostata alle spalle): per 2,5 s Core.cpp
// muove l'eroe 1 con i clic sul terreno, come il mouse, invece che con gli ordini diretti.
std::atomic<DWORD> g_dmBlockedAt{ 0 };
bool MemoryDirectMoveBlocked()
{
    const DWORD at = g_dmBlockedAt.load();
    return at && static_cast<LONG>(GetTickCount() - at) < 2500;
}

static bool NearestHostile(int x, int y, BYTE z, int r, DWORD& id, int& ex, int& ey);
std::atomic<int> g_moveTaken{ 0 };   // 0.56.47: ultimo ordine di movimento diretto: 1 preso dal gioco, 2 no
bool MemoryMoveHero1(float sdx, float sdy)
{
    if (!g_ok || !(g_swapUi.load() || g_camOn.load())) return false;
    DWORD mgr = 0, player = 0, id = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player);
    if (!mgr || !player || !RdHero(player, id) || !id) return false;
    const DWORD h1 = ObjectAddr(mgr, id);
    if (!h1) return false;
    int x = 0, y = 0; BYTE z = 0;
    if (!Rd(h1 + 0x1c, x) || !Rd(h1 + 0x20, y) || !Rd(h1 + 0x24, z)) return false;
    View v;
    float ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
    if (!ReadView(v) || !WorldToScreen(v, x, y, ax, ay) || !WorldToScreen(v, x + 256, y, bx, by) || !WorldToScreen(v, x, y + 256, cx, cy)) return false;
    // a * e1 + b * e2 = direzione sullo schermo, con e1 ed e2 le immagini di +256 in x e in y del mondo
    const float e1x = bx - ax, e1y = by - ay, e2x = cx - ax, e2y = cy - ay;
    const float det = e1x * e2y - e2x * e1y;
    if (std::fabs(det) < 1e-3f) return false;
    const float a = (sdx * e2y - e2x * sdy) / det, b = (e1x * sdy - sdx * e1y) / det;
    const float n = std::sqrt(a * a + b * b);
    if (n < 1e-6f) return false;
    const float wdx = a / n, wdy = b / n;
    // 0.55.34: controllo sempre acceso dell'eroe 1 che va al contrario dello stick. Prova sulla Steam Machine del 31/10 alle
    // 19:25 (Vampira come eroe 1, zona di partenza in alto: proiezione piatta 280 pixel sotto la posizione disegnata): "tornava
    // indietro anche se gli dicevo di andare avanti, rendendomi impossibile proseguire". Il punto dell'ordine e' a 300 unita' sullo
    // stesso piano dell'eroe e 0x6173b0 lo aggancia alla griglia di navigazione: se davanti c'e' un ostacolo o un altro piano, il
    // percorso del gioco puo' passare all'indietro (ipotesi, non verificata: il log non aveva tracce). Fra due chiamate di fila nella
    // stessa direzione (entro 400 ms) si misura lo spostamento vero: se per due volte va contro la direzione dell'ordine, riga
    // ATTENZIONE con posizione, piano, punto dell'ordine e destinazione del gioco, e per 1,5 s passi corti (100 unita').
    // Una riga anche quando l'eroe cambia piano camminando (al massimo ogni 2 s).
    static DWORD wAt = 0, shortUntil = 0, warnAt = 0, planeLogAt = 0;
    static int wX = 0, wY = 0, wBack = 0, wTx = 0, wTy = 0;
    static float wDx = 0, wDy = 0;
    static BYTE wZ = 0;
    const DWORD nowT = GetTickCount();
    const bool chained = wAt && nowT - wAt <= 400 && wdx * wDx + wdy * wDy > 0.85f;
    if (chained) {
        const float mx = static_cast<float>(x - wX), my = static_cast<float>(y - wY), ml = std::sqrt(mx * mx + my * my);
        if (ml >= 12.0f) { if ((mx * wdx + my * wdy) / ml < -0.3f) ++wBack; else wBack = 0; }
        if (z != wZ && static_cast<LONG>(nowT - planeLogAt) >= 2000) {
            planeLogAt = nowT;
            Log("Movimento diretto: l'eroe 1 passa dal piano %u al piano %u in (%d,%d)", wZ, z, x, y);
        }
    }
    else wBack = 0;
    {   // 0.56.47 (il tester, 06/11: "a volte se il giocatore 2 e' dietro con dei nemici, il giocatore 1 non puo' avanzare che si blocca"):
        // stick tenuto nella stessa direzione e l'eroe 1 fermo (meno di 15 unita' in 1,2 s): una riga ogni 4 s con modo, stato,
        // destinazione, se il gioco ha preso l'ultimo ordine, distanza dall'eroe 2 e nemici vicini ai due eroi.
        static DWORD stillAt = 0, stillLog = 0; static int sX = 0, sY = 0;
        if (!chained || std::abs(x - sX) + std::abs(y - sY) >= 15) { stillAt = nowT; sX = x; sY = y; }
        else if (static_cast<LONG>(nowT - stillAt) >= 1200 && (!stillLog || static_cast<LONG>(nowT - stillLog) >= 4000)) {
            stillLog = nowT;
            WORD md = 0; DWORD st = 0; int d104 = 0, d108 = 0, h2x = 0, h2y = 0;
            Rd(h1 + 0xfc, md); Rd(h1 + 0x150, st); Rd(h1 + 0x104, d104); Rd(h1 + 0x108, d108);
            const DWORD h2 = ObjectAddr(mgr, 2);
            BYTE z2 = 0;
            double d2 = -1;
            if (h2 && Rd(h2 + 0x1c, h2x) && Rd(h2 + 0x20, h2y)) { Rd(h2 + 0x24, z2); d2 = std::sqrt(double(h2x - x) * (h2x - x) + double(h2y - y) * (h2y - y)); }
            DWORD e1 = 0, e2 = 0; int ex = 0, ey = 0, fx = 0, fy = 0;
            const bool n1 = NearestHostile(x, y, z, 300, e1, ex, ey);
            const bool n2 = h2 && NearestHostile(h2x, h2y, z2, 300, e2, fx, fy);
            Log("Movimento diretto: eroe 1 fermo da %lu ms con lo stick (%d,%d): modo %u, stato %08lX, destinazione (%d,%d), ultimo ordine %s; "
                "eroe 2 a %.0f; nemico vicino all'eroe 1 %s%lu, all'eroe 2 %s%lu", static_cast<unsigned long>(nowT - stillAt), x, y, md, st, d104, d108,
                g_moveTaken.load() == 1 ? "preso" : g_moveTaken.load() == 2 ? "NON preso" : "?", d2, n1 ? "" : "nessuno ", n1 ? e1 : 0, n2 ? "" : "nessuno ", n2 ? e2 : 0);
        }
    }
    if (wBack >= 2) {
        wBack = 0;
        if (!warnAt || static_cast<LONG>(nowT - warnAt) >= 3000) {
            warnAt = nowT;
            WORD md = 0; DWORD st = 0; int d104 = 0, d108 = 0, d158 = 0, d15c = 0;
            Rd(h1 + 0xfc, md); Rd(h1 + 0x150, st); Rd(h1 + 0x104, d104); Rd(h1 + 0x108, d108); Rd(h1 + 0x158, d158); Rd(h1 + 0x15c, d15c);
            Log("ATTENZIONE: movimento diretto: l'eroe 1 va al contrario dello stick (da (%d,%d) a (%d,%d) mentre l'ordine era verso "
                "(%d,%d)); piano %u, modo %u, stato %08lX, +0x104/+0x108 (%d,%d), +0x158/+0x15c (%d,%d): per 1,5 s passi corti",
                wX, wY, x, y, wTx, wTy, z, md, st, d104, d108, d158, d15c);
        }
        shortUntil = nowT + 1500;
    }
    // 0.56.43 (prova sul PC del 05/11, 10:10:59-10:11:02: "entra ma poi esce subito, forse perche' e' una casa molto piccola e
    // stretta"): dentro la casa (piano 1) il punto a 300 unita' cade fuori dal muro, il gioco lo accetta sul piano di fuori e la strada
    // passa dalla porta (riga "va al contrario", poi piano 0). Su un piano diverso da 0 passi da 100 unita', come dopo l'avviso.
    const float stepLen = ((shortUntil && static_cast<LONG>(nowT - shortUntil) < 0) || z != 0) ? 100.0f : 300.0f;
    int tx = x + static_cast<int>(wdx * stepLen), ty = y + static_cast<int>(wdy * stepLen);
    // 0.56.42 (il tester, 05/11, prova sulla Steam Machine: "si fa anche fatica ad entrare in qualche casa"; log delle 23:31:35-45: porta
    // 1539 aperta, poi 8 s di "va al contrario dello stick" prima di passare al piano 1): il punto dritto davanti cade dentro la casa,
    // dietro il muro, e la strada del gioco gira intorno. Con una porta aperta fra gli oggetti a schermo, fra 70 e 400 unita' e nella
    // direzione dello stick (entro 35 gradi), l'ordine va alla porta; a meno di 70 unita' si torna al punto davanti (la soglia).
    {
        std::vector<PickRect> rs;
        DWORD doorId = 0; int doorX = 0, doorY = 0; float best = -1.0f;
        if (ReadPickRects(v, rs))
            for (const PickRect& r : rs) {
                if (r.id & 0x80000000u) continue;
                const DWORD o = ObjectById(r.id);
                DWORD t = 0, st = 0; int ox = 0, oy = 0;
                if (!o || !Rd(o + 0x10, t) || !IsDoorType(t) || !Rd(o + 0x64, st) || st != 0xAA || !Rd(o + 0x1c, ox) || !Rd(o + 0x20, oy)) continue;
                const float ddx = static_cast<float>(ox - x), ddy = static_cast<float>(oy - y), dd = std::sqrt(ddx * ddx + ddy * ddy);
                if (dd < 70.0f || dd > 400.0f) continue;
                const float c = (ddx * wdx + ddy * wdy) / dd;
                if (c < 0.82f) continue;
                const float score = c - dd / 2000.0f;
                if (score > best) { best = score; doorId = r.id; doorX = ox; doorY = oy; }
            }
        static DWORD doorLogId = 0, doorLogAt = 0;
        if (doorId) {
            tx = doorX; ty = doorY;
            if (doorId != doorLogId || static_cast<LONG>(nowT - doorLogAt) >= 3000) {
                doorLogId = doorId; doorLogAt = nowT;
                Log("Movimento diretto: porta aperta %lu davanti all'eroe 1, l'ordine va alla porta (%d,%d) da (%d,%d)", doorId, doorX, doorY, x, y);
            }
        }
    }
    wAt = nowT; wX = x; wY = y; wDx = wdx; wDy = wdy; wZ = z; wTx = tx; wTy = ty;
    const double queuedAt = QpcMs();
    // 0.55.35: la destinazione scelta dal gioco si controlla subito. Prova della 0.55.34 sulla Steam Machine (31/10, 19:36-19:37,
    // Vampira sul piano 2): 10 righe ATTENZIONE, tutte con l'ordine verso il punto avanti e la destinazione del gioco (+0x104/
    // +0x108) alle spalle dell'eroe, sempre fra pochi punti fissi ((187749,133171) quattro volte): 0x6173b0 (0x616e50) sostituisce
    // un punto non valido sul piano dell'eroe con un nodo vicino della sua griglia, che li' sta dietro; con il punto a 100 unita'
    // il gioco lo accettava cosi' com'era. Ora dopo l'ordine si legge la destinazione: se sta dietro rispetto allo stick si riprova
    // subito a meta' e a un quarto della distanza; se anche cosi' va dietro, l'eroe si ferma (davanti non si passa, come contro
    // un muro) invece di camminare all'indietro.
    return RunOnGameThread([=] {
        DWORD m = 0;
        Rd(kObjMgr, m);
        if (!m || ObjectAddr(m, id) != h1) return;
        static DWORD fixLogAt = 0;
        static const float kFrac[3] = { 1.0f, 0.5f, 0.25f };
        int cx = x, cy = y;
        Rd(h1 + 0x1c, cx); Rd(h1 + 0x20, cy);
        for (int k = 0; k < 3; ++k) {
            const int ox = k == 0 ? tx : cx + static_cast<int>(wdx * stepLen * kFrac[k]);
            const int oy = k == 0 ? ty : cy + static_cast<int>(wdy * stepLen * kFrac[k]);
            // segnaposto in +0x104: se dopo l'ordine e' ancora li', il gioco non l'ha preso (arte in corso, eroe a cavallo...) e
            // in +0x104 c'era altro (per esempio l'id del nemico attaccato): si rimette il valore di prima e non si giudica
            DWORD before104 = 0;
            Rd(h1 + 0x104, before104);
            WriteDword(h1 + 0x104, 0x7FFFFFFFu);
            const bool sent = SendMoveOrder(h1, ox, oy, z);
            DWORD after104 = 0;
            Rd(h1 + 0x104, after104);
            const bool taken = after104 != 0x7FFFFFFFu;
            g_moveTaken.store(taken ? 1 : 2);
            if (!taken) WriteDword(h1 + 0x104, before104);
            if (!sent) {
                static DWORD failLog = 0;
                if (static_cast<int>(GetTickCount() - failLog) > 5000) { failLog = GetTickCount(); Log("N4: ordine di movimento all'eroe 1 fallito"); }
                return;
            }
            if (!taken) break;
            int gx = 0, gy = 0;
            if (!Rd(h1 + 0x104, gx) || !Rd(h1 + 0x108, gy)) break;
            if (std::abs(gx - cx) > 4000 || std::abs(gy - cy) > 4000) break;   // non e' un punto di cammino
            const float vx = static_cast<float>(gx - cx), vy = static_cast<float>(gy - cy), vl = std::sqrt(vx * vx + vy * vy);
            if (vl < 10.0f || (vx * wdx + vy * wdy) / vl > -0.2f) {   // avanti o di lato (lungo un muro) va bene; solo dietro no
                if (k > 0 && static_cast<LONG>(GetTickCount() - fixLogAt) >= 3000) {
                    fixLogAt = GetTickCount();
                    Log("Movimento diretto: punto avanti spostato dal gioco alle spalle dell'eroe 1, accettato a %.0f unita' (piano %u, eroe (%d,%d))",
                        stepLen * kFrac[k], z, cx, cy);
                }
                break;
            }
            if (k == 0) {                                // 0.55.36: punto davanti rifiutato: per 2,5 s clic sul terreno (Core.cpp)
                const DWORD t = GetTickCount();
                if (!MemoryDirectMoveBlocked())
                    Log("Movimento diretto: il gioco sposta alle spalle il punto davanti (piano %u, eroe (%d,%d), destinazione (%d,%d)): "
                        "per 2,5 s clic sul terreno, come il mouse", z, cx, cy, gx, gy);
                g_dmBlockedAt.store(t ? t : 1);
            }
            if (k == 2) {                                // anche a un quarto il gioco lo manda indietro: si ferma
                SendMoveOrder(h1, cx, cy, z, false, 0);  // 0.55.36: fermata vera (modo 0), che il gioco non sposta
                if (static_cast<LONG>(GetTickCount() - fixLogAt) >= 3000) {
                    fixLogAt = GetTickCount();
                    Log("Movimento diretto: davanti nessun punto valido per il gioco (destinazione (%d,%d) alle spalle, piano %u, eroe (%d,%d)): eroe fermo",
                        gx, gy, z, cx, cy);
                }
            }
        }
        TurnTrackOrder(h1, wdx, wdy, queuedAt);
    });
}

// 0.53.9: fine del movimento diretto: ordine verso il punto dove l'eroe e' adesso (si ferma invece di finire i 300 passi).
bool MemoryStopHero1()
{
    if (!g_ok) return false;
    DWORD mgr = 0, player = 0, id = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player);
    if (!mgr || !player || !RdHero(player, id) || !id) return false;
    const DWORD h1 = ObjectAddr(mgr, id);
    if (!h1) return false;
    return RunOnGameThread([=] {
        DWORD m = 0;
        Rd(kObjMgr, m);
        int x = 0, y = 0; BYTE z = 0;
        if (!m || ObjectAddr(m, id) != h1 || !Rd(h1 + 0x1c, x) || !Rd(h1 + 0x20, y) || !Rd(h1 + 0x24, z)) return;
        SendMoveOrder(h1, x, y, z, false, 0);   // 0.55.36: fermata vera (modo 0); il "vai qui" del modo 2 il gioco poteva spostarlo
        g_lastOrdDx = g_lastOrdDy = 0;      // da fermo la ripartenza non e' un'inversione
        if (g_cfg.memoryDiag) Log("Movimento diretto: eroe fermato");
    });
}

// 0.51.2: pozione dell'eroe 2 dal pad 2, anche senza scambio. E' la stessa funzione del motore dei tasti della cintura,
// FUN_00613290(tipo, destro) (thiscall, ret 8; tipi dai tasti del gioco: 6 = cura, con il miglior tipo scelto dal gioco,
// 5 = R concentrazione, 4 = E antidoto, 2 = Q, 3 = W), con le sue chiamate a GetHeroId deviate sull'eroe 2 (UiBeltStub)
// solo per la durata della chiamata, nel thread del motore. Serve lo scambio dell'interfaccia installato (SwapUi).
// 0.53.0: per un eroe qualsiasi (la copia della barra durante lo scambio mostra l'eroe 1).
void UseBeltHeroId(int kind, DWORD id)
{
    if (!g_uiPatched.load()) {
        static bool told = false;
        if (!told) { told = true; Log("PAD2: pozioni dell'eroe 2 non disponibili (serve SwapUi=true con le deviazioni installate)"); }
        return;
    }
    RunOnGameThread([=] {
        DWORD mgr = 0, engine = 0;
        Rd(kObjMgr, mgr); Rd(kEngine, engine);
        const DWORD h = mgr ? ObjectAddr(mgr, id) : 0;
        DWORD hp = 0;
        if (!h || !engine || !Rd(h + 0x4d8, hp) || hp == 0) return;
        g_beltHero = h;
        const CallResult cr = CallThis2(0x00613290, engine, static_cast<DWORD>(kind), 0);
        g_beltHero = 0;
        if (!cr.ok) { LogException("pozione dalla cintura"); return; }
        if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x613290 ha lasciato lo stack sbilanciato (corretto)");
        Log("PAD2: pozione (tipo %d) per l'eroe %lu: %s", kind, id, (cr.ret & 0xff) ? "bevuta" : "nessuna adatta nella cintura");
    });
}
void UseBeltHero2(int kind) { UseBeltHeroId(kind, 2); }

// L'eroe 2 morto si rialza (0.37.0): vita piena, stato e modo come all'inizio, accanto all'eroe 1 se il cadavere e' lontano.
// Nella prova del 27/10 (0.36.6) l'eroe 2 restava morto anche dopo il ritorno dell'eroe 1 al punto di salvataggio, e il
// gioco gli teletrasportava dietro il cadavere. Morte osservata: stato +0x150 = 0x20006, modo +0xfc = 0x10009, vita 0;
// vivo: stato 0x00010000, modo 0. Vita massima a +0x4d4.
void ReviveHero2(const char* why)
{
    RunOnGameThread([=] {
        DWORD mgr = 0, player = 0, heroId = 0, engine = 0;
        Rd(kObjMgr, mgr); Rd(kPlayer, player); Rd(kEngine, engine);
        if (player) RdHero(player, heroId);
        const DWORD h1 = mgr ? ObjectAddr(mgr, heroId) : 0, h2 = mgr ? ObjectAddr(mgr, 2) : 0;
        if (!h1 || !h2) return;
        DWORD hp = 0;
        Rd(h2 + 0x4d8, hp);
        if (hp != 0) return;
        int maxHp = 0;
        Rd(h2 + 0x4d4, maxHp);
        if (maxHp <= 0) maxHp = 100;
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        Rd(h1 + 0x1c, x1); Rd(h1 + 0x20, y1); Rd(h2 + 0x1c, x2); Rd(h2 + 0x20, y2);
        const double d = sqrt(static_cast<double>(x1 - x2) * (x1 - x2) + static_cast<double>(y1 - y2) * (y1 - y2));
        if (d > 400) {
            BYTE pos[16];
            if (RdBuf(h1 + 0x18, pos, sizeof(pos))) {
                int nx = x1 + 80;
                memcpy(pos + 4, &nx, 4);
                if (!TeleportObject(mgr, 2, pos)) LogException("revive teleport");
            }
        }
        const DWORD h2n = ObjectAddr(mgr, 2);
        if (!h2n) return;
        WriteDword(h2n + 0x4d8, static_cast<DWORD>(maxHp));
        WriteDword(h2n + 0x0fc, 0);
        WriteDword(h2n + 0x150, 0x00010000);
        const CallResult cr = CallThis2(0x0044d8d0, h2n, 1, 0);
        if (!cr.ok) LogException("0x44d8d0 revive");
        Log("EROI: l'eroe 2 si rialza (%s): vita %d, distanza dall'eroe 1 %.0f", why, maxHp, d);
    });
}

// 0.56.16: anche l'eroe 1 si rialza con il compagno vicino (il tester, 04/11: "basta stargli vicino per rianimarlo, devi pero'
// applicarlo anche al giocatore 1"). Morte dell'eroe 1 osservata il 28/10: vita 0, stato +0x150 0x10003 -> 0x20003; il gioco lo fa
// rinascere da solo al punto di rinascita dopo circa 6 s (teletrasportando anche l'eroe 2): con ReviveNearSeconds (3) il compagno
// vicino arriva prima. Come ReviveHero2: vita massima, modo +0xfc 0, stato con la parte alta 1 (la bassa resta), 0x44d8d0(1, 0).
void ReviveHero1(const char* why)
{
    RunOnGameThread([=] {
        DWORD mgr = 0, player = 0, heroId = 0;
        Rd(kObjMgr, mgr); Rd(kPlayer, player);
        if (player) RdHero(player, heroId);
        const DWORD h1 = mgr && heroId ? ObjectAddr(mgr, heroId) : 0;
        DWORD hp = 0, st = 0;
        if (!h1 || !Rd(h1 + 0x4d8, hp) || hp != 0 || !InWorld(h1)) return;
        int maxHp = 0;
        Rd(h1 + 0x4d4, maxHp);
        if (maxHp <= 0) maxHp = 100;
        Rd(h1 + 0x150, st);
        WriteDword(h1 + 0x4d8, static_cast<DWORD>(maxHp));
        WriteDword(h1 + 0x0fc, 0);
        WriteDword(h1 + 0x150, 0x00010000u | (st & 0xFFFFu));
        const CallResult cr = CallThis2(0x0044d8d0, h1, 1, 0);
        if (!cr.ok) LogException("0x44d8d0 rialzo dell'eroe 1");
        Log("EROI: l'eroe 1 si rialza (%s): vita %d, stato %08lX -> %08lX", why, maxHp, st, 0x00010000u | (st & 0xFFFFu));
    });
}

// 0.51.3: arte dell'eroe 2 per slot, come le arti 1-5 del pad 1. Il gestore 0x6108a0 usa l'arte di indice +0x261 (elenco
// delle arti dell'eroe a +0x4aa..+0x4ae, voci da 0x16 byte) con Ctrl+0, una volta per pressione grazie al blocco +0x2b6, che
// si rilascia solo con Ctrl premuto e nessuna cifra: qui si azzera a mano. Nel thread del motore: indice, blocco e tasti
// virtuali (Ctrl e 0 per 150 ms) nello stesso fotogramma. Approssimazione: lo slot k e' la k-esima arte dell'elenco, non
// lo slot k della barra dell'eroe 2. 0.55.14: resta solo per uno slot della barra vuoto (come prima).
static void UseArtListNow(DWORD h2, int slot)
{
    DWORD b = 0, e = 0;
    if (!Rd(h2 + 0x4aa, b) || !Rd(h2 + 0x4ae, e)) return;
    const int n = e > b ? static_cast<int>((e - b) / 0x16) : 0;
    if (slot >= n) { Log("PAD2: arte %d non disponibile (l'eroe 2 ha %d arti)", slot + 1, n); return; }
    const WORD idx = static_cast<WORD>(slot);
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(h2 + 0x261)), &idx, 2, nullptr);
    WriteByte(h2 + 0x2b6, 0);
    const DWORD until = GetTickCount() + 150;
    for (int vk : { VK_CONTROL, static_cast<int>('0') })
        if (g_virt.keyUntil[vk].load() != VirtualInput::kHeld) g_virt.keyUntil[vk].store(until);
    g_p2CtrlUntil.store(until);                 // 0.55.37: il Ctrl del pad 2, l'unico che il gestore vede (Pad2HandlerStub)
    Log("PAD2: arte %d dell'elenco dell'eroe 2 (di %d), con Ctrl+0 del gestore", slot + 1, n);
}

// Ctrl spento per gli ordini della mod all'eroe 2 (0.55.14): il metodo 0x18 delle creature legge Ctrl con GetAsyncKeyState
// (tipo 7 in 0x52ffb0, tipo 4 in 0x52f270) e con Ctrl l'ordine cambia ("fermati e attacca"). Hook_GetAsyncKeyState risponde
// "Ctrl su" al thread in g_noCtrlTid, vero o virtuale che sia, solo per la durata della chiamata. Prima si toglieva per un
// istante il Ctrl virtuale del pad 1, e con il Ctrl vero premuto l'ordine non partiva: con PadSource=None il giocatore 1 usa
// la tastiera vera.
struct NoCtrlScope {
    NoCtrlScope() { g_noCtrlTid.store(GetCurrentThreadId()); }
    ~NoCtrlScope() { g_noCtrlTid.store(0); }
};

// 0.55.14: arte appena lanciata dall'eroe 2: per un po' Hero2AttackTick non rimanda l'ordine di attacco, che la interromperebbe
// (vedi Hero2AttackTick).
std::atomic<DWORD> g_h2ArtAt{ 0 }, g_h2ArtUntil{ 0 }, g_h2ArtMode{ 0 }, g_h2ArtLast{ 0 };

// 0.55.14: arti dell'eroe 2 come quelle dell'eroe 1. Studio del 30/10: la barra ha 5 slot d'arte (pulsanti +0x178..), ognuno con
// un record da 0x14 byte nella voce dell'inventario dell'eroe ([0xad33c8] + id * 0x1180): tipo (word, +0xc90 + (b + 2k) * 0x14)
// e valore (dword, 4 byte dopo), con b = bit 1 di +0x1162 (0x5d98f0 e 0x5d9930); tipo 2, 3 o 4 (probabilmente arte, magia e
// combo: da verificare nel log), 0 = vuoto. Scegliendo lo slot (0x6e1b40 -> 0x5d9d30) la voce si segna lo slot in +0xd5a e manda al motore il messaggio del tipo
// (cEngine 0x618410, casi 2/3/4): +0x58 = tipo - 2, +0x5a = valore. Il clic destro dell'eroe 1 (FUN_00617640, 0x617f47) se
// +0x5a < 0x4b0 manda con 0x6173b0 (ECX = motore; creatura, ordine) l'ordine {0x88f95c, 0x105, 0, 0, nemico, word +0x18 del
// nemico, x, y, 0, 0, 100, +0x58, 0x552770(valore, 0) sulla creatura (arte del giorno o della notte per la Vampira), livello z}
// (lo stesso del tasto X del gestore del giocatore 2, che pero' lo usa solo sui nemici dell'eroe 1). L'arte non pronta (voce
// dell'elenco +0x4aa con la rigenerazione +0x12 oltre 0,01, come 0x5810a0) non parte. Senza nemico: il punto (gx, gy). Nel
// thread del motore, con Ctrl spento; lo slot si segna anche in +0xd5a della voce dell'eroe 2, cosi' la sua barra lo evidenzia.
// 0.55.55: per un eroe qualunque (hid), cosi' anche l'eroe 1 lancia le arti senza il mouse quando il mouse del gioco e' del
// giocatore 2 (due zaini, MemoryHero1Art); lo stato delle arti per Hero2AttackTick e l'elenco con Ctrl+0 restano dell'eroe 2.
static void UseArtHeroId(DWORD hid, int slot, DWORD target, int gx, int gy)
{
    RunOnGameThread([=] {
        const bool is2 = hid == 2;
        const char* tag = is2 ? "PAD2" : "PAD1";
        DWORD mgr = 0, inv = 0, engine = 0, hp = 0;
        Rd(kObjMgr, mgr); Rd(kInvTable, inv); Rd(kEngine, engine);
        const DWORD h2 = mgr && hid && hid < 0x20 ? ObjectAddr(mgr, hid) : 0;
        if (!h2 || !inv || !engine || !Rd(h2 + 0x4d8, hp) || hp == 0) return;
        const DWORD e2 = inv + hid * kInvSize;
        BYTE fl = 0; Rd(e2 + 0x1162, fl);
        const DWORD rec = e2 + 0xc90 + (((fl >> 1) & 1u) + 2u * static_cast<DWORD>(slot)) * 0x14;
        WORD kind = 0; DWORD val = 0;
        Rd(rec, kind); Rd(rec + 4, val);
        if (kind < 2 || kind > 4 || val == 0) {
            Log("%s: slot %d della barra dell'eroe %lu vuoto (tipo %u, valore %lu)", tag, slot + 1, hid, kind, val);
            if (is2) UseArtListNow(h2, slot);
            return;
        }
        if (val >= 0x4b0) {                     // il clic destro dell'eroe 1 non lo lancia nemmeno lui
            Log("%s: slot %d della barra dell'eroe %lu: tipo %u, valore %lu oltre 0x4b0, non lanciato", tag, slot + 1, hid, kind, val);
            return;
        }
        const CallResult ca = CallThis2(0x00552770, h2, val, 0);
        if (!ca.ok) { LogException("arte di un eroe (0x552770)"); return; }
        const DWORD art = ca.ret & 0xffff;
        DWORD lb = 0, le = 0;
        if (Rd(h2 + 0x4aa, lb) && Rd(h2 + 0x4ae, le) && le > lb && le - lb < 0x2000) {
            for (DWORD q = lb; q + 0x16 <= le; q += 0x16) {
                WORD id = 0; float regen = 0;
                if (!Rd(q + 4, id) || id != art) continue;
                if (Rd(q + 0x12, regen) && regen > 0.01f) { Log("%s: arte %lu (slot %d) dell'eroe %lu non pronta: rigenerazione %.1f", tag, art, slot + 1, hid, regen); return; }
                break;
            }
        }
        if (is2) {   // 0.55.15: la stessa arte ripremuta mentre l'eroe 2 la sta ancora lanciando (stesso modo dell'ordine, entro 3 s) non
            // riparte: nella prova delle 16:17:21 U premuto 5 volte in 0,6 s aveva mandato 5 volte l'ordine durante lo stesso
            // lancio (modo 11). La rigenerazione comincia solo a lancio finito, quindi il controllo qui sopra non lo fermava.
            WORD curMode = 0; Rd(h2 + 0xfc, curMode);
            const DWORD artMode = g_h2ArtMode.load();
            if (art == g_h2ArtLast.load() && artMode != 0 && curMode == artMode &&
                static_cast<LONG>(GetTickCount() - g_h2ArtAt.load()) < 3000) {
                Log("PAD2: arte %lu (slot %d) dell'eroe 2 gia' in corso (modo %u): ignorata", art, slot + 1, curMode);
                return;
            }
        }
        int tx = gx, ty = gy;
        BYTE tz = 0; WORD t18 = 0;
        const DWORD te = target ? ObjectById(target) : 0;
        if (te && Rd(te + 0x1c, tx) && Rd(te + 0x20, ty)) { Rd(te + 0x24, tz); Rd(te + 0x18, t18); }
        else { tx = gx; ty = gy; Rd(h2 + 0x24, tz); }
        DWORD order[0x11] = {};                 // 0x44 byte, come l'ordine sullo stack di FUN_00617640
        order[0] = 0x0088f95c;
        order[1] = 0x105;
        order[4] = te ? target : 0;
        order[5] = t18;
        order[6] = static_cast<DWORD>(tx);
        order[7] = static_cast<DWORD>(ty);
        order[10] = 100;
        order[11] = kind - 2u;
        order[12] = art;
        order[13] = tz;
        const WORD sel = static_cast<WORD>(slot);
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(e2 + 0xd5a)), &sel, 2, nullptr);
        CallResult cr{};
        {
            NoCtrlScope nc;
            cr = CallThis2(0x006173b0, engine, h2, reinterpret_cast<DWORD>(order));
        }
        if (!cr.ok) { LogException("ordine dell'arte di un eroe"); return; }
        WORD mode = 0; DWORD tgt = 0, t100 = 0;
        Rd(h2 + 0xfc, mode); Rd(h2 + 0x104, tgt); Rd(h2 + 0x100, t100);
        const DWORD now = GetTickCount();
        if (is2) {
            g_h2ArtMode.store(mode);
            g_h2ArtLast.store(art);
            g_h2ArtAt.store(now);
            g_h2ArtUntil.store(now + 3000);
        }
        Log("%s: arte %lu (slot %d, tipo %u) dell'eroe %lu su %s %lu (%d,%d): modo %u, +0x104 %lu, +0x100 %lu", tag, art, slot + 1, kind, hid,
            te ? "nemico" : "punto", te ? target : 0, tx, ty, mode, tgt, t100);
    });
}
void UseArtHero2(int slot, DWORD target, int gx, int gy) { UseArtHeroId(2, slot, target, gx, gy); }

// 0.55.55: eroe 1 senza mouse (il tester, 01/11: "pero' prima se il giocatore 1 apriva lo zaino, il giocatore 2 poteva attaccare i
// nemici"): con il lato dell'eroe 2 aperto il mouse del gioco e' del giocatore 2, quindi il pad 1 attacca e lancia le arti con gli
// stessi ordini che la mod manda all'eroe 2. Arte: UseArtHeroId sull'eroe locale (senza nemico, sull'eroe stesso). Attacco: ordine
// di tipo 7 come il clic su un nemico (vedi Hero2AttackNow), senza Ctrl ("vai e attacca"); niente ordine se l'eroe 1 sta gia'
// attaccando o usando quel nemico (+0xfc 1, 4 o 8 e +0x104 = nemico), che ricomincerebbe il colpo.
static DWORD LocalHeroId()
{
    DWORD player = 0, id = 0;
    return (Rd(kPlayer, player) && player && RdHero(player, id) && id && id < 0x20) ? id : 0;
}
void MemoryHero1Art(int slot, DWORD target)
{
    const DWORD hid = LocalHeroId();
    if (!hid || hid == 2 || slot < 0 || slot > 4) return;
    DWORD mgr = 0; int x = 0, y = 0;
    Rd(kObjMgr, mgr);
    const DWORD h = mgr ? ObjectAddr(mgr, hid) : 0;
    if (h) { Rd(h + 0x1c, x); Rd(h + 0x20, y); }
    UseArtHeroId(hid, slot, target, x, y);
}
void MemoryHero1Attack(DWORD enemyId)
{
    if (!enemyId) return;
    RunOnGameThread([=] {
        const DWORD hid = LocalHeroId();
        DWORD mgr = 0, hp = 0, vt = 0, fn = 0, tgt = 0;
        WORD mode = 0;
        Rd(kObjMgr, mgr);
        const DWORD h = mgr && hid ? ObjectAddr(mgr, hid) : 0, e = ObjectById(enemyId);
        int x = 0, y = 0;
        if (!h || !e || !Rd(e + 0x1c, x) || !Rd(e + 0x20, y)) return;
        if (!Rd(h + 0x4d8, hp) || hp == 0 || !Rd(h, vt) || !Rd(vt + 0x18, fn) || fn < 0x401000 || fn >= 0x88f000) return;
        Rd(h + 0xfc, mode); Rd(h + 0x104, tgt);
        if ((mode == 1 || mode == 4 || mode == 8 || mode == 11) && tgt == enemyId) return;
        DWORD order[0x11] = {};
        order[0] = 0x0088f95c;
        order[1] = 7;
        order[6] = enemyId;
        order[7] = static_cast<DWORD>(x == 1 ? 2 : x);
        order[8] = static_cast<DWORD>(y == 0xb ? 0xc : y);
        CallResult cr{};
        {
            NoCtrlScope nc;
            cr = CallThis1(fn, h, reinterpret_cast<DWORD>(order));
        }
        if (!cr.ok) LogException("ordine di attacco dell'eroe 1");
    });
}

// 0.55.22: "raccogli tutto" per l'eroe 2, come B del pad 1 (tasto A del gioco). Studio del 30/10: il tasto A (cEngine 0x618410,
// caso 0x41) manda {0x88f95c, 0x113, 0, id dell'eroe locale, 0...}; il metodo 0x18 della creatura (0x52e9a0, caso 0x113), fuori
// dal multigiocatore e se l'eroe non parla (modo 10), non attacca (4, 8), non lancia (0xb, 6) e lo stato non e' 7, avvisa la
// barra (UI_TB_COLLECT) e mette il modo 12 (+0xfc = 0xc, +0xfe = 0, +0x104 = 0): l'eroe va da solo a prendere gli oggetti vicini,
// con le scelte "Raccogli" delle opzioni del gioco. Qui lo stesso ordine al metodo 0x18 dell'eroe 2, nel thread del motore e con
// Ctrl spento. Il modo 12 il gestore del giocatore 2 lo fermerebbe a ogni passo: e' escluso dalla fermata in Pad2StopStub.
void CollectAllHero2()
{
    RunOnGameThread([] {
        DWORD mgr = 0, hp = 0, vt = 0, fn = 0, id2 = 0;
        Rd(kObjMgr, mgr);
        const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0;
        if (!h2 || !Rd(h2 + 0x4d8, hp) || hp == 0 || !Rd(h2 + 0xc, id2) || !Rd(h2, vt) || !Rd(vt + 0x18, fn) || fn < 0x401000 || fn >= 0x88f000) return;
        DWORD order[0x11] = {};
        order[0] = 0x0088f95c;
        order[1] = 0x113;
        order[3] = id2;
        CallResult cr{};
        {
            NoCtrlScope nc;
            cr = CallThis1(fn, h2, reinterpret_cast<DWORD>(order));
        }
        if (!cr.ok) { LogException("raccogli tutto dell'eroe 2"); return; }
        WORD mode = 0; Rd(h2 + 0xfc, mode);
        Log("PAD2: raccogli tutto (0x113): eroe 2 modo %u%s", mode, mode == 0xc ? "" : " (non raccoglie: parla, attacca o e' occupato)");
    });
}

// 0.55.19: cavallo dell'eroe 2 (B tenuto del pad 2, come l'icona del cavallo accanto al ritratto che il pad 1 clicca con B
// tenuto). Studio del 30/10: il clic sull'icona (cUI_Mercenary, metodo 0x10 a 0x6d6d50, 0x6d6e72-0x6d6f9b) prende il cavallo
// dell'eroe da +0x570 (0x55c8f0) e la sua distanza (0x5374c0): fino a 250 unita' manda all'eroe {0x88f95c, 7, id dell'eroe,
// cavallo, 0, 0, cavallo, 1, 0...} (tipo 7 con x = 1, il valore speciale del ramo del tipo 7: sali, e in sella scendi), oltre
// {0x88f95c, 0x116, 0, 0, cavallo, 0...} (richiama il cavallo); senza l'oggetto del cavallo non fa nulla. Qui lo stesso con
// l'eroe 2 e il suo cavallo, con il metodo 0x18 dell'eroe 2 e Ctrl spento, nel thread del motore.
void HorseHero2()
{
    RunOnGameThread([] {
        DWORD mgr = 0, hp = 0, vt = 0, fn = 0, horse = 0, id2 = 0;
        Rd(kObjMgr, mgr);
        const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0;
        if (!h2 || !Rd(h2 + 0x4d8, hp) || hp == 0) return;
        if (!Rd(h2 + 0x570, horse) || !horse) {
            Log("PAD2: l'eroe 2 non ha un cavallo (+0x570 = 0)");
            OverlayCoopToast(Tr("Giocatore 2: il tuo eroe non ha un cavallo"), 2500);
            return;
        }
        const DWORD ho = ObjectById(horse);
        int hx = 0, hy = 0, cx = 0, cy = 0;
        if (!ho || !Rd(ho + 0x1c, cx) || !Rd(ho + 0x20, cy) || !Rd(h2 + 0x1c, hx) || !Rd(h2 + 0x20, hy)) {
            Log("PAD2: cavallo %lu dell'eroe 2 non trovato fra gli oggetti", horse);
            return;
        }
        if (!Rd(h2 + 0xc, id2) || !Rd(h2, vt) || !Rd(vt + 0x18, fn) || fn < 0x401000 || fn >= 0x88f000) return;
        const double d = std::sqrt(double(cx - hx) * (cx - hx) + double(cy - hy) * (cy - hy));
        DWORD order[0x11] = {};                 // 0x44 byte, come l'ordine sullo stack di 0x6d6d50
        order[0] = 0x0088f95c;
        const bool close = d <= 250.0;
        if (close) { order[1] = 7; order[2] = id2; order[3] = horse; order[6] = horse; order[7] = 1; }
        else { order[1] = 0x116; order[4] = horse; }
        CallResult cr{};
        {
            NoCtrlScope nc;
            cr = CallThis1(fn, h2, reinterpret_cast<DWORD>(order));
        }
        if (!cr.ok) { LogException("ordine del cavallo dell'eroe 2"); return; }
        WORD mode = 0; DWORD tgt = 0;
        Rd(h2 + 0xfc, mode); Rd(h2 + 0x104, tgt);
        Log("PAD2: cavallo %lu dell'eroe 2 a %.0f unita': %s (modo %u, +0x104 %lu)", horse, d,
            close ? "sali o scendi (tipo 7, x = 1)" : "richiamato (0x116)", mode, tgt);
    });
}

void SwitchWeaponHero2(int step);   // 0.52.3, definita con la seconda barra

// --- 0.55.0: interazioni dell'eroe 2 con A del pad 2 (PNG, forzieri, porte, oggetti fissi della mappa) ---------------------
// Il clic del gioco su un oggetto (FUN_00617640, ramo del tasto sinistro con un id scelto da 0x627050) manda alla creatura
// comandata ([engine+0x13c]) l'ordine di tipo 7 {vtable 0x88f95c, 7, 0, 0, 0, modo 0, id, x, y} direttamente con il suo metodo
// 0x18, anche per gli oggetti fissi (id con il bit 31). Il metodo 0x18 di cCreature (0x52e9a0, ramo del tipo 7 a 0x52ff98)
// mette il modo +0xfc = 1 ("vai e usa"), il bersaglio in +0x104 e id, x, y in +0x10c..+0x114; con Ctrl premuto invece diventa
// un attacco (mai su un PNG: l'ordine parte solo senza Ctrl, vero o virtuale), con il modo dell'ordine diverso da 0 non fa
// nulla. Con il comando passato all'eroe 2 (Ctrl+Insert, prova del 26-27/10) lo stesso clic lo faceva camminare fino al PNG,
// parlare e accettare la missione; il gestore del giocatore 2 (0x6108a0) non annulla il modo 1 con il suo ordine di fermata.
// Qui l'ordine lo manda la mod all'eroe 2 senza toccare [engine+0x13c].
struct H2Target {
    DWORD id = 0; int wx = 0, wy = 0; float d = 0; const char* kind = ""; char name[40] = "";
    LONG mx = 0, my = 0; int icon = 0;   // 0.55.2: indicatore sullo schermo (icona del gioco: 0 azione, 1 dialogo)
    LONG gx = 0, gy = 0;                 // 0.56.18: punto a terra (alone sotto il bersaglio)
    bool service = false;                // 0.55.2: PNG con un servizio (mercante, fabbro, maestro)
    bool door = false;                   // 0.55.5: porta (si apre con il messaggio 0x21, vedi Hero2OpenDoor)
    bool merchant = false;               // 0.56.9: mercante (bit 0x2000 di +0x200): con il secondo mouse il negozio e' condiviso
    int sharedWin = -1;                  // 0.56.14: finestra condivisa che apre (4 mercante, 14 Maestro; 3 fabbro con kShareSmith)
    bool smith = false;                  // 0.56.14: fabbro (bit 0x1000)
};

// Punto del mondo che si proietta in (sx, sy): WorldToScreen e' affine, la si inverte dalle immagini di due passi del mondo
// intorno a (ox, oy), come per la direzione del movimento diretto (MemoryMoveHero1).
static bool ScreenToWorldNear(const View& v, int ox, int oy, float sx, float sy, int& wx, int& wy)
{
    float ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
    if (!WorldToScreen(v, ox, oy, ax, ay) || !WorldToScreen(v, ox + 256, oy, bx, by) || !WorldToScreen(v, ox, oy + 256, cx, cy)) return false;
    const float e1x = bx - ax, e1y = by - ay, e2x = cx - ax, e2y = cy - ay, det = e1x * e2y - e2x * e1y;
    if (std::fabs(det) < 1e-3f) return false;
    const float px = sx - ax, py = sy - ay;
    const float a = (px * e2y - e2x * py) / det, b = (e1x * py - px * e1y) / det;
    wx = ox + static_cast<int>(a * 256.0f);
    wy = oy + static_cast<int>(b * 256.0f);
    return true;
}

// Il nemico vivo piu' vicino entro r unita' dal punto, sullo stesso piano (0.55.5: anche id e posizione, per l'attacco).
static bool NearestHostile(int x, int y, BYTE z, int r, DWORD& id, int& ex, int& ey)
{
    std::vector<Obj> cr; Obj hero{}; bool haveHero = false;
    if (!ReadCreatures(cr, hero, haveHero)) return false;
    long long best = static_cast<long long>(r) * r + 1;
    for (const Obj& o : cr) {
        if (o.id <= 16 || o.z != z) continue;
        int cls = 0; bool dead = false;
        if (!ReadKind(o.addr, cls, dead) || dead || !InMask(g_cfg.hostileMask, cls)) continue;
        const long long dx = o.x - x, dy = o.y - y, d = dx * dx + dy * dy;
        if (d < best) { best = d; id = o.id; ex = o.x; ey = o.y; }
    }
    return best <= static_cast<long long>(r) * r;
}

// 0.56.54: il nemico vivo successivo nella direzione (dx, dy) dal punto, entro r e LockAngle, o entro nearR in ogni direzione;
// vince il piu' dritto, a parita' il piu' vicino (come MemoryFindTarget per l'aggancio del pad 1).
static bool NextHostileInDir(int x, int y, BYTE z, int dx, int dy, int r, int nearR, DWORD& id, int& ex, int& ey)
{
    std::vector<Obj> cr; Obj hero{}; bool haveHero = false;
    if (!ReadCreatures(cr, hero, haveHero)) return false;
    const double dl = std::sqrt(double(dx) * dx + double(dy) * dy);
    const double cosLim = std::cos(g_cfg.lockAngle * 3.14159265 / 180.0);
    double best = 1e18;
    bool found = false;
    for (const Obj& o : cr) {
        if (o.id <= 16 || o.z != z) continue;
        int cls = 0; bool dead = false;
        if (!ReadKind(o.addr, cls, dead) || dead || !InMask(g_cfg.hostileMask, cls)) continue;
        const double vx = o.x - x, vy = o.y - y, d = std::sqrt(vx * vx + vy * vy);
        if (d > r) continue;
        double score;
        if (d <= nearR) score = d;
        else {
            if (dl < 1.0 || d < 1.0) continue;
            const double c = (vx * dx + vy * dy) / (d * dl);
            if (c < cosLim) continue;
            score = d * (1.0 + (1.0 - c) * 2.0);
        }
        if (score < best) { best = score; id = o.id; ex = o.x; ey = o.y; found = true; }
    }
    return found;
}

// Un nemico vivo entro r unita' dal punto, sullo stesso piano: A attacca invece di interagire.
static bool HostileNear(int x, int y, BYTE z, int r)
{
    DWORD id = 0; int ex = 0, ey = 0;
    return NearestHostile(x, y, z, r, id, ex, ey);
}

// Bersaglio di A per l'eroe 2, dal suo punto sullo schermo: PNG, forzieri e porte con la stessa scelta del pad 1
// (MemoryFindTarget, raggio TalkRange; 0.55.2: con lo stick inclinato vince cio' che sta piu' dritto nella direzione, senza la
// precedenza dei PNG con l'icona, come il bersaglio a fuoco del pad 1: nella prova del 30/10 A sceglieva sempre il PNG 685),
// le porte chiuse anche dai loro rettangoli disegnati (0.55.2: come la porta a spinta del pad 1; la posizione nel mondo di
// una porta puo' stare lontana da dove e' disegnata e alle 13:03 nessuna porta veniva scelta) e gli oggetti fissi della mappa
// (rettangoli con l'id a bit 31), che vincono solo se chiaramente piu' vicini.
// 0.56.2: org = punto dello schermo da cui cercare (il cursore del giocatore 2), con un raggio piccolo e senza direzione.
// 0.56.6: oggetto del mondo sotto il cursore del giocatore 2 che non e' una creatura, una porta o un oggetto fisso della mappa
// (per questi c'e' Hero2FindInteract): un oggetto a terra, come lo indica il gioco (+0x6c del secondo mouse).
static DWORD Pad2ItemUnderCursor()
{
    const DWORD hov = MemoryMouse2Hover();
    if (!hov || (hov & 0x80000000u)) return 0;
    const DWORD a = ObjectById(hov);
    int cls = 0; bool dead = false; DWORD vt = 0;
    if (!a || ReadKind(a, cls, dead) || !Rd(a, vt) || vt == kVtObject3D) return 0;
    return hov;
}

static bool Hero2FindInteract(DWORD h2, float dx, float dy, bool hasDir, H2Target& out, const float* org = nullptr)
{
    View v{};
    int hx = 0, hy = 0;
    float ax = 0, ay = 0;
    if (!ReadView(v) || !Rd(h2 + 0x1c, hx) || !Rd(h2 + 0x20, hy) || !WorldToScreen(v, hx, hy, ax, ay)) return false;
    if (org) { ax = org[0]; ay = org[1]; hasDir = false; }
    const float nearR = (org ? 0.06f : g_cfg.talkRange) * static_cast<float>(v.h);
    const float cosLim = std::cos(g_cfg.lockAngle * 3.14159265f / 180.0f);
    bool found = false;
    Target t{};
    const DWORD skip[1] = { 2 };
    // 0.56.18: con il punto del cursore (org) la scelta e' voluta: anche i PNG generici
    if (MemoryFindTarget(true, ax, ay, hasDir ? dx : 0.0f, hasDir ? dy : 0.0f, nearR, nearR, t, skip, 1, 0, hasDir, true, org != nullptr)) {
        int tx = 0, ty = 0;
        if (Rd(t.addr + 0x1c, tx) && Rd(t.addr + 0x20, ty)) {
            out = H2Target{};
            out.id = t.id; out.wx = tx; out.wy = ty; out.kind = t.kind;
            out.d = std::sqrt((t.sx - ax) * (t.sx - ax) + (t.sy - ay) * (t.sy - ay));
            _snprintf_s(out.name, sizeof(out.name), _TRUNCATE, "%s", t.name);
            const bool creature = t.cls >= 0;
            out.icon = t.cls == 4 ? 2 : creature ? 1 : 0;   // 0.55.20: cavallo con l'icona della sella, come per il pad 1
            out.mx = static_cast<LONG>(t.sx);
            out.my = static_cast<LONG>(t.sy - (creature ? 0.19f : 0.08f) * v.h);
            out.gx = static_cast<LONG>(t.sx); out.gy = static_cast<LONG>(t.sy);
            out.door = t.vt == kVtObject3D;
            DWORD svc = 0, horse2 = 0;
            // 0.55.3: mercante, maestro, fabbro (0x3000). 0.55.21: anche lo stalliere (0x04000000) e i cavalli che non sono dell'eroe
            // 2: tutti e due aprono la finestra del cavallo (cUI_Horse, n.17; prova delle 17:19), che legge l'eroe con GetHeroId in
            // 0x6be52e, uno dei punti deviati dallo scambio della sola interfaccia: con lo scambio paga l'oro dell'eroe 2 (0x580030
            // su +0x3a8) e il cavallo va all'eroe 2 (0x55c900); senza, pagava l'eroe 1 ("non hai abbastanza oro" con i 100 pezzi
            // dell'elfo oscuro, la diavola ne ha 90.000) e i pulsanti della finestra erano del mouse (J rifaceva parlare lo stalliere).
            Rd(h2 + 0x570, horse2);
            // 0.56.8: anche il Maestro delle combo (0x4000, vedi kServiceMask): combo, scambio delle rune e portali dell'eroe 2
            out.service = creature && ((Rd(t.addr + 0x200, svc) && (svc & kServiceMask) != 0) || (t.cls == 4 && t.id != horse2));
            out.merchant = creature && (svc & 0x2000u) != 0;
            out.sharedWin = !creature ? -1 : (svc & 0x2000u) ? 4 : ((svc & 0x1000u) && kShareSmith) ? 3 : (svc & 0x4000u) ? 14 : -1;
            out.smith = creature && (svc & 0x1000u) != 0;
            // 0.55.30: anche il deposito della citta' (OBJECT_CHEST_STATIC): apre deposito e zaino dell'eroe locale (cUI_Inventory3,
            // "Griglia deposito" nel log), quindi come il mercante l'interfaccia passa prima all'eroe 2 con il puntatore al pad 2.
            // Prova del 31/10 alle 13:19 sulla Steam Machine: aperto dalla diavola, mostrava lo zaino dell'elfo oscuro, il pad 1 in
            // modalita' cursore e il pad 2 ancora in gioco (la sua croce beveva le pozioni).
            // 0.56.45 (prova del 05/11 alle 11:58:24: "se apro il baule condiviso con l'eroe 2, apre l'inventario dell'eroe 1"):
            // t.name e' la classe ("cChest3D +64=AB"), non il tipo, quindi la condizione non era mai vera. Il tipo e' OBJECT_CHEST_STATIC.
            if (!creature) {
                char tn[64] = "";
                // 0.56.54: e il deposito e' una finestra condivisa (n.16): gia' aperto dal giocatore 1, A apre il lato del giocatore 2
                if (MemoryTypeName(t.addr, tn, sizeof(tn)) && strstr(tn, "CHEST_STATIC")) { out.service = true; out.sharedWin = 16; }
            }
            found = true;
        }
    }
    std::vector<PickRect> rs;
    if (!ReadPickRects(v, rs)) return found;
    // punto del rettangolo piu' vicino all'eroe 2, nella fascia bassa (dove la cosa disegnata tocca terra)
    auto nearPoint = [&](const PickRect& r, float& px, float& py) {
        const float top = r.y + r.h * 0.6f, bot = static_cast<float>(r.y + r.h);
        px = ax < r.x ? static_cast<float>(r.x) : ax > r.x + r.w ? static_cast<float>(r.x + r.w) : ax;
        py = ay < top ? top : ay > bot ? bot : ay;
    };
    auto inCone = [&](float vx, float vy, float d) {
        return !hasDir || d <= 0.4f * nearR || (vx * dx + vy * dy) / (d > 1.0f ? d : 1.0f) >= cosLim;
    };
    const PickRect* door = nullptr; float doorD = 0;
    const PickRect* fixed = nullptr; float fixedD = 0, fixedX = 0, fixedY = 0;
    for (const PickRect& r : rs) {
        if (r.w <= 0 || r.h <= 0) continue;
        float px = 0, py = 0;
        nearPoint(r, px, py);
        const float vx = px - ax, vy = py - ay, d = std::sqrt(vx * vx + vy * vy);
        if (d > nearR || !inCone(vx, vy, d)) continue;
        if (r.id & 0x80000000u) {
            // 0.55.4: per l'ordine il piede del disegno (centro in basso), non il punto piu' vicino all'eroe 2 (vedi le porte)
            if (!fixed || d < fixedD) { fixed = &r; fixedD = d; fixedX = r.x + r.w * 0.5f; fixedY = r.y + r.h * 0.95f; }
            continue;
        }
        DWORD st = 0;
        if (!MemoryDoorState(r.id, st) || st == 0xAA) continue;          // solo porte chiuse
        if (!door || d < doorD) { door = &r; doorD = d; }
    }
    int wx = 0, wy = 0;
    // 0.55.4: nell'ordine la posizione della porta nel mondo (+0x1c/+0x20), come per i PNG. Nella 0.55.3 c'era il punto del
    // rettangolo piu' vicino all'eroe 2 riportato nel mondo, cioe' quasi dove l'eroe 2 stava gia': il gioco lo dava per
    // arrivato, la porta vera era lontana e il modo tornava a 0 senza un passo (prova del 30/10 alle 13:45, porte 1622 e 1646).
    const DWORD doorObj = door ? ObjectById(door->id) : 0;
    if (door && doorObj && (!found || doorD < out.d) && Rd(doorObj + 0x1c, wx) && Rd(doorObj + 0x20, wy)) {
        out = H2Target{};
        out.id = door->id; out.wx = wx; out.wy = wy; out.d = doorD; out.kind = "porta"; out.door = true;
        _snprintf_s(out.name, sizeof(out.name), _TRUNCATE, "rettangolo %dx%d", door->w, door->h);
        out.mx = door->x + door->w / 2; out.my = static_cast<LONG>(door->y + door->h * 0.35f);
        out.gx = door->x + door->w / 2; out.gy = static_cast<LONG>(door->y + door->h * 0.95f);
        found = true;
    }
    if (fixed && (!found || fixedD + 0.05f * v.h < out.d) && ScreenToWorldNear(v, hx, hy, fixedX, fixedY, wx, wy)) {
        out = H2Target{};
        out.id = fixed->id; out.wx = wx; out.wy = wy; out.d = fixedD; out.kind = "oggetto fisso della mappa";
        _snprintf_s(out.name, sizeof(out.name), _TRUNCATE, "%dx%d", fixed->w, fixed->h);
        out.mx = fixed->x + fixed->w / 2; out.my = fixed->y - static_cast<LONG>(0.02f * v.h);
        out.gx = static_cast<LONG>(fixedX); out.gy = static_cast<LONG>(fixedY);
        found = true;
    }
    return found;
}

std::atomic<int> g_pad2InteractReq{ 0 };   // 0.55.0: l'eroe 2 ha ricevuto un ordine di interazione (per Core.cpp: dialoghi)
bool NativePad2TakeInteract() { return g_pad2InteractReq.exchange(0) > 0; }
std::atomic<bool> g_pad2AutoSwap{ false }; // 0.55.2: scambio partito da solo per un PNG con un servizio (Core.cpp non apre lo zaino)
bool NativePad2AutoSwap() { return g_pad2AutoSwap.load(); }
std::atomic<WORD> g_pad2DlgBtn{ 0 };       // 0.55.0: A o B del pad 2 con un dialogo aperto, da usare come quelli del pad 1
std::atomic<DWORD> g_pad2DlgAt{ 0 };
WORD NativePad2TakeDialogBtn()
{
    const WORD btn = g_pad2DlgBtn.exchange(0);
    return (btn && GetTickCount() - g_pad2DlgAt.load() < 500) ? btn : 0;   // una richiesta vecchia (Core.cpp occupato) non vale
}
// 0.55.28: eroe 2 nel modo 10 (parla con un PNG: il gioco ha il dialogo aperto), letto da NativePad2Tick a ogni giro.
std::atomic<DWORD> g_h2TalkAt{ 0 };
bool NativePad2Talking()
{
    const DWORD at = g_h2TalkAt.load();
    return at && static_cast<LONG>(GetTickCount() - at) <= 300;
}
// 0.55.16: pulsanti del pad 2 per Core.cpp (R3 = Alt e L3 = minimappa, come il pad 1): l'ultimo stato letto da NativePad2Tick
// in modalita' gioco, valido 200 ms. Se NativePad2Tick non lo aggiorna piu' (puntatore o aiuto al pad 2, eroe 2 assente, gioco
// non in primo piano) i pulsanti risultano lasciati e Core.cpp li rilascia.
std::atomic<DWORD> g_pad2Btn{ 0 }, g_pad2BtnAt{ 0 };
WORD NativePad2Buttons()
{
    const DWORD at = g_pad2BtnAt.load();
    if (!at || static_cast<LONG>(GetTickCount() - at) > 200) return 0;
    return static_cast<WORD>(g_pad2Btn.load());
}
bool NativePad2InGame()
{
    const DWORD at = g_pad2BtnAt.load();
    return at && static_cast<LONG>(GetTickCount() - at) <= 300;
}
struct H2Act { DWORD id = 0, until = 0, next = 0; int logs = 0; };
H2Act g_h2Act;                             // traccia dell'interazione nel log (thread della DLL)

// Ordine di tipo 7 all'eroe 2, nel thread del motore.
static void Hero2Interact(const H2Target& t)
{
    const H2Target tg = t;
    RunOnGameThread([tg] {
        DWORD mgr = 0, hp = 0, vt = 0, fn = 0;
        Rd(kObjMgr, mgr);
        const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0;
        if (!h2 || !Rd(h2 + 0x4d8, hp) || hp == 0) { Log("PAD2: interazione annullata: eroe 2 assente o a terra"); return; }
        if (!Rd(h2, vt) || !Rd(vt + 0x18, fn) || fn < 0x401000 || fn >= 0x88f000) { Log("PAD2: interazione annullata: metodo 0x18 dell'eroe 2 non valido"); return; }
        DWORD order[0x11] = {};                 // 0x44 byte, come l'ordine sullo stack di FUN_00617640
        order[0] = 0x0088f95c;
        order[1] = 7;
        order[6] = tg.id;
        order[7] = static_cast<DWORD>(tg.wx);
        order[8] = static_cast<DWORD>(tg.wy);
        if (order[7] == 1) order[7] = 2;        // nel ramo del tipo 7, x = 1 e y = 0xb hanno un significato loro
        if (order[8] == 0xb) order[8] = 0xc;
        CallResult cr{};
        {
            NoCtrlScope nc;                     // 0.55.14: con Ctrl l'ordine diventerebbe un attacco
            cr = CallThis1(fn, h2, reinterpret_cast<DWORD>(order));
        }
        if (!cr.ok) { LogException("ordine di interazione dell'eroe 2"); return; }
        if (!cr.balanced) Log("NATIVO: ATTENZIONE: il metodo 0x18 dell'eroe 2 ha lasciato lo stack sbilanciato (corretto)");
        WORD mode = 0; DWORD tgt = 0;
        Rd(h2 + 0xfc, mode); Rd(h2 + 0x104, tgt);
        Log("PAD2: l'eroe 2 va verso %s %s id %lu (%d,%d): modo %u, bersaglio %lu", tg.kind, tg.name, tg.id, tg.wx, tg.wy, mode, tgt);
        g_pad2InteractReq.fetch_add(1);
    });
}

// 0.56.47: nome di un oggetto fisso della mappa (trigger degli script). Il gioco, col mouse sopra, scrive il testo della voce della
// tabella dei trigger ([0xab3538], voci da 0x58 byte, +0x10 numero del testo: 0x612c7b) con il gestore dei testi 0x672c50 e 0x673300
// ("Nascondiglio magico"). L'indice del trigger e' la parola bassa dell'id del rettangolo. I testi si leggono nel thread del motore e
// restano in memoria per numero di testo (i numeri dei trigger cambiano con la partita, i testi no).
std::mutex g_flMx;
std::map<DWORD, std::string> g_flText;   // numero del testo -> nome (vuoto: senza nome o non leggibile)
std::set<DWORD> g_flPending;
static DWORD FixedTextId(DWORD id)
{
    DWORD b = 0, e = 0, t = 0;
    const DWORD idx = id & 0xffffu;
    if (!Rd(0x00ab3538, b) || !Rd(0x00ab353c, e) || !b || e <= b || idx >= (e - b) / 0x58) return 0;
    return Rd(b + idx * 0x58 + 0x10, t) ? t : 0;
}
static std::string ReadGameText(DWORD tid)   // nel thread del motore
{
    DWORD tm = 0, s[2] = {};
    if (!tid || !Rd(0x017e6ee8, tm) || !tm) return std::string();
    const CallResult cr = CallThis1(0x00673300, tm, tid);
    if (!cr.ok || !cr.ret || cr.ret == tm + 0xc || !RdBuf(cr.ret, s, sizeof(s)) || !s[0] || s[1] <= s[0] || (s[1] - s[0]) / 2 >= 64) return std::string();
    wchar_t w[64] = {};
    RdBuf(s[0], w, s[1] - s[0]);
    char a[128] = {};
    WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof(a) - 1, nullptr, nullptr);
    return a;
}
// true = nome noto (out, anche vuoto); false = chiesto adesso o ancora in arrivo
static bool FixedLabel(DWORD id, std::string& out, DWORD* textId)
{
    const DWORD tid = FixedTextId(id);
    if (textId) *textId = tid;
    if (!tid) { out.clear(); return true; }
    {
        std::lock_guard<std::mutex> lk(g_flMx);
        const auto it = g_flText.find(tid);
        if (it != g_flText.end()) { out = it->second; return true; }
        if (g_flPending.count(tid)) return false;
        g_flPending.insert(tid);
    }
    const DWORD idx = id & 0xffffu;
    if (!RunOnGameThread([tid, idx] {
            const std::string s = ReadGameText(tid);
            {
                std::lock_guard<std::mutex> lk(g_flMx);
                g_flText[tid] = s;
                g_flPending.erase(tid);
            }
            Log("OGGETTI FISSI: trigger %lu, testo %08lX = \"%s\"", idx, tid, s.c_str());
        })) {
        std::lock_guard<std::mutex> lk(g_flMx);
        g_flPending.erase(tid);
    }
    return false;
}
static bool IsPortalLabel(const std::string& s)
{
    std::string l(s);
    for (char& c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return l.find("portal") != std::string::npos || l.find("porta") != std::string::npos || l.find("telepor") != std::string::npos ||
           l.find("teletras") != std::string::npos;
}

// 0.56.53: tabella dei trigger della mappa (studio del 07/11): [0xab3538]..[0xab353c], voci da 0x58 byte costruite da 0x4a6d30:
// +0 x, +2 y, +4 piano (short), +6 0x3e80 o altro, +8..+0xe quattro short, +0x10 numero del testo del nome, +0x14 tipo (1, o 2 per i
// nascondigli: "versteck %d, %d, %d"), +0x18 nome dello script (40 caratteri). x e y sono la posizione nel mondo divisa per
// 53,66563 (costante a 0x890150: 0x53bb84 la confronta con +0x1c/+0x20 dell'eroe), e l'eroe che usa il trigger cammina fino a
// quel punto (+0x158/+0x15c) e lo usa entro 2 unita' (circa 107 nel mondo). Il gioco col mouse sopra trova il trigger dalla
// posizione nel mondo (0x612c30, 0x4a6b50 sul gestore 0xaabf00), percio' i nascondigli del bosco non hanno un rettangolo.
// Qui, ogni 700 ms dal thread della mod, i trigger entro 1500 unita' dall'eroe 1 vanno in g_trigNear.
constexpr float kTrigUnit = 53.66563f;
// 0.56.54 (prova sulla Steam Machine del 07/11, foto 160830 e 160842): il gioco trova il trigger solo se il punto sotto il mouse cade
// nella sua cella (0x4a6940 confronta x, y e piano interi, cioe' la posizione nel mondo divisa per 53,66 e troncata), mentre la mod
// mirava all'angolo della cella: il clic di A cadeva circa 35 pixel sopra il punto dove compare "Nascondiglio magico" (a 1920x1080),
// sulla stessa verticale, cioe' mezza cella in x e in y. Ora si usa il centro della cella.
static int TrigCellCenter(int c) { return static_cast<int>((c + 0.5f) * kTrigUnit); }
std::mutex g_tnMx;
std::vector<TrigNear> g_trigNear;
DWORD g_tnBase = 0;
static void TrigNearRefresh(DWORD now)
{
    static DWORD nextAt = 0;
    if (nextAt && static_cast<LONG>(now - nextAt) < 0) return;
    nextAt = now + 700;
    std::vector<TrigNear> out;
    DWORD mgr = 0, pl = 0, loc = 0, b = 0, e = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, pl);
    const DWORD h1 = (mgr && pl && RdHero(pl, loc) && loc) ? ObjectAddr(mgr, loc) : 0;
    int hx = 0, hy = 0;
    if (h1 && Rd(h1 + 0x1c, hx) && Rd(h1 + 0x20, hy) && Rd(0x00ab3538, b) && Rd(0x00ab353c, e) && b && e > b) {
        const DWORD n = (e - b) / 0x58;
        if (n > 0 && n <= 60000) {
            std::vector<BYTE> all(n * 0x58);
            if (RdBuf(b, all.data(), n * 0x58)) {
                const int cx = static_cast<int>(hx / kTrigUnit), cy = static_cast<int>(hy / kTrigUnit);
                for (DWORD i = 0; i < n; ++i) {
                    const BYTE* p = &all[i * 0x58];
                    WORD x = 0, y = 0; short z = 0; DWORD tid = 0, kind = 0;
                    memcpy(&x, p, 2); memcpy(&y, p + 2, 2); memcpy(&z, p + 4, 2); memcpy(&tid, p + 0x10, 4); memcpy(&kind, p + 0x14, 4);
                    if (std::abs(static_cast<int>(x) - cx) > 28 || std::abs(static_cast<int>(y) - cy) > 28) continue;   // ~1500 unita'
                    TrigNear t{ 0x80000000u | i, TrigCellCenter(x), TrigCellCenter(y), static_cast<BYTE>(z), tid, kind };
                    out.push_back(t);
                }
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_tnMx);
    g_trigNear.swap(out);
    g_tnBase = b;
}
static bool TrigNearCopy(std::vector<TrigNear>& out)
{
    std::lock_guard<std::mutex> lk(g_tnMx);
    out = g_trigNear;
    return !out.empty();
}
static bool TrigWorldPos(DWORD id, int& wx, int& wy)
{
    if (!(id & 0x80000000u)) return false;
    DWORD b = 0, e = 0;
    const DWORD idx = id & 0xffffu;
    if (!Rd(0x00ab3538, b) || !Rd(0x00ab353c, e) || !b || e <= b || idx >= (e - b) / 0x58) return false;
    WORD xy[2] = {};
    if (!RdBuf(b + idx * 0x58, xy, sizeof(xy))) return false;
    wx = TrigCellCenter(xy[0]); wy = TrigCellCenter(xy[1]);
    return true;
}

// 0.56.47-0.56.53: con MemoryDiag, i trigger vicini all'eroe 1 (TrigNearRefresh), una riga per trigger e partita: nome, tipo,
// piano, posizione nel mondo, distanza, e i short +6..+0xe.
void DiagTriggers(DWORD now)
{
    static DWORD nextAt = 0;
    if (nextAt && static_cast<LONG>(now - nextAt) < 0) return;
    nextAt = now + 3000;
    std::vector<TrigNear> tn;
    DWORD mgr = 0, pl = 0, loc = 0, base = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, pl);
    const DWORD h1 = (mgr && pl && RdHero(pl, loc) && loc) ? ObjectAddr(mgr, loc) : 0;
    int hx = 0, hy = 0;
    if (!h1 || !Rd(h1 + 0x1c, hx) || !Rd(h1 + 0x20, hy) || !TrigNearCopy(tn)) return;
    { std::lock_guard<std::mutex> lk(g_tnMx); base = g_tnBase; }
    static std::set<unsigned long long> seen;
    static DWORD lastBase = 0;
    if (base != lastBase) { lastBase = base; seen.clear(); }
    int logged = 0;
    for (const TrigNear& t : tn) {
        const double d = std::sqrt(double(t.wx - hx) * (t.wx - hx) + double(t.wy - hy) * (t.wy - hy));
        if (d > 900 || logged >= 10) continue;
        const unsigned long long key = (static_cast<unsigned long long>(base) << 32) | t.id;
        if (seen.count(key)) continue;
        std::string name;
        if (!FixedLabel(t.id, name)) continue;   // nome in arrivo: al prossimo giro
        seen.insert(key);
        ++logged;
        short s[4] = {}; char scr[0x29] = {};
        RdBuf(base + (t.id & 0xffffu) * 0x58 + 6, s, 2);
        RdBuf(base + (t.id & 0xffffu) * 0x58 + 8, s, sizeof(s));
        RdBuf(base + (t.id & 0xffffu) * 0x58 + 0x18, scr, 0x28);
        Log("TRIGGER %lu \"%s\" (script \"%s\"): tipo %lu, piano %u, nel mondo (%d,%d), a %.0f dall'eroe 1; +8..+0xe %d %d %d %d", t.id & 0xffffu,
            name.c_str(), scr, t.kind, t.z, t.wx, t.wy, d, s[0], s[1], s[2], s[3]);
    }
}

// 0.56.53: ordine "vai e usa" all'eroe 1 su un id (oggetto o trigger col bit alto) verso il punto (wx, wy), come il clic
static void SendUseOrderHero1(DWORD id, int wx, int wy, const char* why)
{
    RunOnGameThread([id, wx, wy, why] {
        DWORD m = 0, p = 0, l = 0, vt = 0, fn = 0;
        Rd(kObjMgr, m); Rd(kPlayer, p);
        const DWORD h1 = m && p && RdHero(p, l) && l ? ObjectAddr(m, l) : 0;
        if (!h1 || !Rd(h1, vt) || !Rd(vt + 0x18, fn) || fn < 0x401000 || fn >= 0x88f000) return;
        DWORD order[0x11] = {};
        order[0] = 0x0088f95c;
        order[1] = 7;
        order[6] = id;
        order[7] = static_cast<DWORD>(wx == 1 ? 2 : wx);
        order[8] = static_cast<DWORD>(wy == 0xb ? 0xc : wy);
        CallResult cr{};
        {
            NoCtrlScope nc;
            cr = CallThis1(fn, h1, reinterpret_cast<DWORD>(order));
        }
        if (!cr.ok) { LogException(why); return; }
        WORD mode = 0; Rd(h1 + 0xfc, mode);
        Log("PORTALE: ordine mandato (%s), modo dell'eroe 1 %u", why, mode);
    });
}

// 0.56.38 (il tester, 04/11: "il portale con A si passa, pero' forse meglio fare direttamente che ti avvicini e quando lo tocchi lo
// attraversi"): con la camera sul punto medio lo stick muove l'eroe 1 con ordini di movimento (MemoryMoveHero1), e un portale
// (oggetto fisso della mappa, id col bit alto, ordine "vai e usa" dal clic: vedi Hero2Interact) si attraversa solo con un clic o
// con A. Qui, mentre si cammina con lo stick, se il punto dei piedi dell'eroe 1 cade nella parte bassa del rettangolo di un
// oggetto fisso piu' alto che largo (il portale della prova delle 16:50 era 85x110; i nascondigli magici sono piu' larghi, 105x97,
// e restano ad A) la mod manda all'eroe 1 lo stesso ordine di tipo 7 del clic; poi 6 s di pausa per quell'oggetto
// (MemoryMarkFixedUsed) e Core ferma per un attimo gli ordini di movimento. [Keys] AutoPortal.
// 0.56.42 (prova sulla Steam Machine del 04/11, 23:31-23:34: "il portale facevo fatica ad attraversarlo comunque", nessuna riga
// PORTALE; foto 233247 e 233252): il rettangolo del portale per il gioco e' 85x110, molto piu' piccolo dell'anello, e l'eroe si
// ferma contro il bordo dell'anello senza mettere i piedi dentro. Ora basta essere vicini alla base (ellisse di 1,6 larghezze per
// 0,8 altezze attorno al punto a 0,85 dell'altezza) con lo stick che spinge verso di lei (entro 50 gradi, direzione sdx, sdy dello
// schermo); dentro il rettangolo come prima. Vicino al doppio di quella distanza una riga "PORTALE: vicino" al secondo per oggetto,
// con le misure, per le prove.
bool MemoryAutoPortal(float sdx, float sdy)
{
    if (!g_ok || !g_cfg.autoPortal) return false;
    View v{};
    DWORD mgr = 0, pl = 0, loc = 0;
    if (!ReadView(v) || !Rd(kObjMgr, mgr) || !mgr || !Rd(kPlayer, pl) || !pl || !RdHero(pl, loc) || !loc) return false;
    const DWORD h = ObjectAddr(mgr, loc);
    int hx = 0, hy = 0;
    float sx = 0, sy = 0;
    if (!h || !Rd(h + 0x1c, hx) || !Rd(h + 0x20, hy) || !WorldToScreen(v, hx, hy, sx, sy)) return false;
    std::vector<PickRect> rs;
    if (!ReadPickRects(v, rs)) return false;
    // 0.56.43 (prova sul PC del 05/11 alle 10:10:53-54: piedi fermi in (959,608) e (958,619), rettangolo del portale da (869,671) a
    // (673,574) in poco piu' di un secondo): con la camera sul punto medio i rettangoli del gioco sono fatti con una vista diversa
    // da quella del disegno (vedi 0.53.9), e WorldToScreen da' i piedi nella vista del disegno. I piedi si prendono dal rettangolo
    // dell'eroe 1 nella stessa lista, al centro in basso; WorldToScreen solo se l'eroe non c'e'.
    bool fromRect = false;
    for (const PickRect& r : rs)
        if (r.id == loc && r.w > 0 && r.h > 0) { sx = r.x + r.w * 0.5f; sy = r.y + r.h * 0.95f; fromRect = true; break; }
    const PickRect* hit = nullptr;
    static DWORD nearAt = 0, nearId = 0;
    const DWORD nowT = GetTickCount();
    const float ml = std::sqrt(sdx * sdx + sdy * sdy);
    for (const PickRect& r : rs) {
        if (!(r.id & 0x80000000u) || r.w <= 0 || r.h <= 0 || FixedRecentlyUsed(r.id)) continue;
        // 0.56.47 (prova sulla Steam Machine del 06/11): alle 00:36:02 l'oggetto fisso 98x202 (piu' alto che largo come il portale)
        // ha avuto l'ordine "vai e usa" ed era un cartello (si e' aperto un dialogo); i portali 85x110 invece l'eroe non li avvicina
        // mai sotto 1,5 della vecchia distanza (00:39:06-00:40:11: 1,98, 1,86, 1,81, con lo stick verso la base 0,97): l'anello lo
        // ferma prima. Ora si guarda il nome del trigger (FixedLabel): con un nome da portale basta 2,3 della distanza con lo stick
        // verso la base (coseno >= 0,5), e non conta la forma; con un altro nome niente portale automatico; senza nome come prima.
        std::string label;
        const bool known = FixedLabel(r.id, label);
        if (!known) continue;                                                // nome in arrivo: al prossimo giro
        const bool portal = !label.empty() && IsPortalLabel(label);
        const bool inside = sx >= r.x && sx < r.x + r.w && sy >= r.y + r.h * 0.35f && sy <= r.y + r.h + 6;
        const float bx = r.x + r.w * 0.5f, by = r.y + r.h * 0.85f;          // base del portale
        const float ex = bx - sx, ey = by - sy, ed = std::sqrt(ex * ex + ey * ey);
        const float nx = ex / (r.w * 1.6f), ny = ey / (r.h * 0.8f), e2 = nx * nx + ny * ny;
        const float toward = (ed > 1.0f && ml > 0.1f) ? (ex * sdx + ey * sdy) / (ed * ml) : 1.0f;
        // 0.56.57 (il tester, 10/11, foto del portale ad anello vicino a Bellevue: "non ci riesco ad entrare, mentre col mouse subito"):
        // in nessun log dalla 0.56.47 e' mai arrivato un nome da portale fra gli oggetti fissi, quindi i portali sono senza nome, e per
        // loro restava la soglia vecchia (1,0 con lo stick verso la base a 0,64), che l'anello non lascia mai raggiungere (06/11:
        // 1,81-1,98 con 0,97). Ora senza nome e con la forma da portale bastano 2,0 con lo stick verso la base (>= 0,7); col nome da
        // portale 2,3 e 0,5 come prima. Gli oggetti scartati vicini restano nel log, con il motivo, per le prove.
        const bool shape = r.h >= r.w && r.w <= 0.4f * v.h && r.h <= 0.4f * v.h;
        const char* why = nullptr;
        if (!label.empty() && !portal) why = "nome non da portale";
        else if (!portal && !shape) why = "forma non da portale";
        const bool ok = !why && (inside || (portal ? (e2 <= 2.3f * 2.3f && toward >= 0.5f) : (e2 <= 2.0f * 2.0f && toward >= 0.7f)));
        if (e2 <= 6.25f && (r.id != nearId || static_cast<LONG>(nowT - nearAt) >= 1000)) {
            nearId = r.id; nearAt = nowT;
            Log("PORTALE: vicino all'oggetto fisso %lu \"%s\" (%dx%d in (%d,%d)), piedi in (%.0f,%.0f)%s, distanza %.2f, verso la base %.2f%s%s",
                r.id, label.c_str(), r.w, r.h, r.x, r.y, sx, sy, fromRect ? "" : " (dalla vista del disegno)", std::sqrt(e2), toward,
                ok ? ": dentro" : "", why ? (std::string(": scartato, ") + why).c_str() : "");
        }
        if (why) continue;
        if (ok) { hit = &r; break; }
    }
    if (!hit) {
        // 0.56.53: portale senza rettangolo (foto della Steam Machine del 06/11 e del PC del 05/11 alle 12:04: nessuna riga PORTALE):
        // dai trigger vicini col nome da portale, sullo stesso piano, a meno di 300 unita' nel mondo e con lo stick verso di lui
        // (coseno >= 0,5 sullo schermo); l'ordine porta l'eroe al punto del trigger, dove il gioco lo usa.
        std::vector<TrigNear> tn;
        BYTE hz = 0;
        float hsx = 0, hsy = 0;
        if (!Rd(h + 0x24, hz) || !WorldToScreen(v, hx, hy, hsx, hsy) || !TrigNearCopy(tn)) return false;
        static DWORD tLogAt = 0, tLogId = 0;
        for (const TrigNear& t : tn) {
            if (t.z != hz || FixedRecentlyUsed(t.id)) continue;
            std::string label;
            if (!FixedLabel(t.id, label) || label.empty() || !IsPortalLabel(label)) continue;
            const double d = std::sqrt(double(t.wx - hx) * (t.wx - hx) + double(t.wy - hy) * (t.wy - hy));
            if (d > 600) continue;
            float px = 0, py = 0;
            if (!WorldToScreen(v, t.wx, t.wy, px, py)) continue;
            const float ex = px - hsx, ey = py - hsy, ed = std::sqrt(ex * ex + ey * ey);
            const float toward = (ed > 1.0f && ml > 0.1f) ? (ex * sdx + ey * sdy) / (ed * ml) : 1.0f;
            const bool ok = d <= 300 && toward >= 0.5f;
            if (t.id != tLogId || static_cast<LONG>(nowT - tLogAt) >= 1000) {
                tLogId = t.id; tLogAt = nowT;
                Log("PORTALE: trigger %lu \"%s\" a %.0f unita' (nel mondo (%d,%d)), verso di lui %.2f%s", t.id & 0xffffu, label.c_str(), d, t.wx, t.wy, toward,
                    ok ? ": vai e usa" : "");
            }
            if (!ok) continue;
            MemoryMarkFixedUsed(t.id);
            SendUseOrderHero1(t.id, t.wx, t.wy, "portale dal trigger");
            return true;
        }
        return false;
    }
    const DWORD id = hit->id;
    int wx = hx, wy = hy;
    ScreenToWorldNear(v, hx, hy, hit->x + hit->w * 0.5f, hit->y + hit->h * 0.9f, wx, wy);
    MemoryMarkFixedUsed(id);
    Log("PORTALE: l'eroe 1 tocca l'oggetto fisso %lu (%dx%d in (%d,%d), piedi in (%.0f,%.0f)): vai e usa", id, hit->w, hit->h, hit->x, hit->y, sx, sy);
    SendUseOrderHero1(id, wx, wy, "ordine del portale");
    return true;
}

// 0.55.5: porte dell'eroe 2. Con l'ordine di tipo 7 su una porta l'eroe 2 non si muove e il modo torna subito a 0 (prove del
// 30/10 alle 13:45 e 13:54, porte 1622 e 1646, anche con la posizione vera della porta): la parte del gioco che usa il bersaglio
// all'arrivo (FUN_0053a570, entro 300 unita') per l'eroe 2 non arriva mai al messaggio. Quel messaggio la mod lo manda da se':
// {vtable 0x88f8ac (vf4 = 6, vf8 = 0x22, i controlli di cObject3D::vf18 0x44c080), 0x21, chi la usa, id della porta}, comando
// 0x21 = apri o chiudi; la porta decide da sola per le serrature (+0x34). Nel thread del motore, con l'eroe 2 entro 450 unita'.
static void Hero2OpenDoor(DWORD doorId)
{
    RunOnGameThread([doorId] {
        DWORD mgr = 0, hp = 0, st = 0, vt = 0, fn = 0;
        Rd(kObjMgr, mgr);
        const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0, door = ObjectById(doorId);
        if (!h2 || !Rd(h2 + 0x4d8, hp) || hp == 0 || !door) { Log("PAD2: porta %lu: eroe 2 o porta non piu' presenti", doorId); return; }
        if (!MemoryDoorState(doorId, st) || st == 0xAA) { Log("PAD2: porta %lu gia' aperta o non e' una porta (stato %lX)", doorId, st); return; }
        int hx = 0, hy = 0, dx = 0, dy = 0;
        Rd(h2 + 0x1c, hx); Rd(h2 + 0x20, hy); Rd(door + 0x1c, dx); Rd(door + 0x20, dy);
        const double d = std::sqrt(double(dx - hx) * (dx - hx) + double(dy - hy) * (dy - hy));
        if (d > 450) {
            Log("PAD2: porta %lu a %.0f unita' dall'eroe 2: troppo lontana", doorId, d);
            OverlayCoopToast(Tr("Giocatore 2: avvicinati alla porta"), 2000);
            return;
        }
        if (!Rd(door, vt) || !Rd(vt + 0x18, fn) || fn < 0x401000 || fn >= 0x88f000) { Log("PAD2: porta %lu: metodo 0x18 non valido", doorId); return; }
        DWORD msg[0x11] = {};                   // 0x44 byte, come il messaggio sullo stack di FUN_0053a570
        msg[0] = 0x0088f8ac;
        msg[1] = 0x21;
        msg[2] = 2;                             // chi la usa: l'eroe 2
        msg[3] = doorId;
        const CallResult cr = CallThis1(fn, door, reinterpret_cast<DWORD>(msg));
        if (!cr.ok) { LogException("apertura della porta dall'eroe 2"); return; }
        if (!cr.balanced) Log("NATIVO: ATTENZIONE: il metodo 0x18 della porta ha lasciato lo stack sbilanciato (corretto)");
        DWORD st2 = 0, lock = 0;
        Rd(door + 0x64, st2); Rd(door + 0x34, lock);
        Log("PAD2: porta %lu a %.0f unita': stato %lX -> %lX%s", doorId, d, st, st2, lock ? " (con serratura)" : "");
    });
}

// 0.55.5: attacco dell'eroe 2 su un nemico scelto (A tenuto): l'ordine di tipo 7 del clic del gioco su un nemico, cioe' quello
// che il gestore del giocatore 2 manda con Y (0x610cf7: {0x88f95c, 7, 0, 0, 0, 0, id, x, y}).
// 0.55.11: senza Ctrl. Nella 0.55.9-0.55.10 l'ordine partiva con Ctrl (modo 4 o 8 diretto, +0x100 = nemico, +0x104/+0x108 =
// punto): nel gioco e' "fermati e attacca", l'eroe colpisce da dove sta verso il punto senza avvicinarsi, e con un'arma corpo a
// corpo a 45-100 unita' colpiva l'aria (prova delle 14:44, nemici 1457 e 1463: modo 4, punto sul nemico, vita ferma per 7 s).
// Senza Ctrl il metodo 0x18 mette il modo 1 con +0x104 = nemico ("vai e usa") e l'uso di un nemico e' l'attacco vero; con il modo
// 4 o 8 gia' attivo cambia solo +0x104. Il modo 1 pero' il gestore del giocatore 2 lo ferma a ogni passo (FUN_00547460, +0x150 =
// 1): per questo NativePad2Tick avvicina l'eroe 2 con le frecce finche' il nemico e' oltre 220 unita' (le frecce il gestore non le
// ferma) e solo da vicino pubblica il bersaglio per Hero2AttackTick, che manda l'ordine nel thread del motore se l'eroe 2 non sta
// gia' usando o attaccando quel nemico (+0x104), per non ricominciare il colpo; senza conferma per 300 ms si ferma da solo.
std::atomic<DWORD> g_h2AtkId{ 0 }, g_h2AtkUntil{ 0 };

static void Hero2AttackNow(DWORD id)        // nel thread del motore
{
    DWORD mgr = 0, hp = 0, vt = 0, fn = 0;
    Rd(kObjMgr, mgr);
    const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0, e = ObjectById(id);
    int x = 0, y = 0;
    if (!h2 || !e || !Rd(e + 0x1c, x) || !Rd(e + 0x20, y)) return;          // posizione del nemico di questo fotogramma
    if (!Rd(h2 + 0x4d8, hp) || hp == 0 || !Rd(h2, vt) || !Rd(vt + 0x18, fn) || fn < 0x401000 || fn >= 0x88f000) return;
    DWORD order[0x11] = {};
    order[0] = 0x0088f95c;
    order[1] = 7;
    order[6] = id;
    order[7] = static_cast<DWORD>(x == 1 ? 2 : x);
    order[8] = static_cast<DWORD>(y == 0xb ? 0xc : y);
    // Ctrl (vero, o virtuale del pad 1 con la mira a distanza) spento solo per la chiamata (0.55.14, NoCtrlScope): cosi' in co-op
    // l'eroe 2 attacca anche mentre l'eroe 1 attacca con Ctrl
    CallResult cr{};
    {
        NoCtrlScope nc;
        cr = CallThis1(fn, h2, reinterpret_cast<DWORD>(order));
    }
    if (!cr.ok) { g_h2AtkId.store(0); LogException("ordine di attacco dell'eroe 2"); }
}

// Da EngineThreadTick, a ogni fotogramma.
static void Hero2AttackTick()
{
    static DWORD lastSent = 0;
    const DWORD id = g_h2AtkId.load();
    if (!id) return;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - g_h2AtkUntil.load()) >= 0) { g_h2AtkId.store(0); return; }   // conferma scaduta
    DWORD mgr = 0, tgt = 0;
    WORD mode = 0;
    Rd(kObjMgr, mgr);
    const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0;
    if (!h2) return;
    Rd(h2 + 0xfc, mode); Rd(h2 + 0x104, tgt);
    // 0.55.14: arte appena lanciata (UseArtHero2): almeno 300 ms, e finche' l'eroe 2 resta nel modo in cui l'ha messo l'ordine
    // dell'arte (al massimo 3 s), niente ordine di attacco, che la interromperebbe
    const DWORD artUntil = g_h2ArtUntil.load();
    if (artUntil && static_cast<LONG>(now - artUntil) < 0) {
        const DWORD artMode = g_h2ArtMode.load();
        if (static_cast<LONG>(now - g_h2ArtAt.load()) < 300 || (artMode != 0 && mode == artMode)) return;
        g_h2ArtUntil.store(0);
    }
    if ((mode == 1 || mode == 4 || mode == 8) && tgt == id) return;   // gia' su quel nemico: un ordine ricomincerebbe l'uso
    if (static_cast<LONG>(now - lastSent) < 30) return;
    {   // 0.56.47: traccia degli ordini di attacco (al massimo 10 ogni 2 s)
        static DWORD winAt = 0; static int nWin = 0;
        if (!winAt || static_cast<LONG>(now - winAt) >= 2000) { winAt = now; nWin = 0; }
        if (nWin++ < 10) Log("PAD2: ordine di attacco su %lu (prima modo %u, +0x104 %lu)", id, mode, tgt);
    }
    lastSent = now;
    Hero2AttackNow(id);
}

// 0.55.12: il gestore del giocatore 2 (FUN_006108a0), dopo frecce, Y e X, a ogni fotogramma manda all'eroe 2 che cammina
// (FUN_00547460: +0x150 == 1) l'ordine di fermata (tipo 4, modo 0), salvo nei modi 4 e 8 con +0x104 diverso da 0 (o stati 3 e 5,
// modi 0xb e 6, con +0x158). Serve a fermarlo quando si lasciano le frecce (modo 2), ma ferma anche il modo 1 ("vai e usa":
// l'ordine del clic e del tasto Y su un nemico o un PNG), che cosi' va a scatti e, se deve fare un passo, non arriva mai al colpo
// (prova delle 14:57: nemico 1410 a 61 unita', modi 4 e 1 alternati, vita ferma per 5 s; avvicinamento ai PNG con modi 1 e 0
// alternati). In 0x610fb7 ("cmp ax, 4 / je 0x611423", AX = +0xfc) si salta a Pad2StopStub, che manda anche il modo 1 a 0x611423
// (fermata saltata se +0x104 != 0); con le frecce lasciate (modo 2) l'eroe 2 si ferma come prima. [Native] Pad2WalkToUse.
bool PatchCode(DWORD addr, const BYTE* bytes, size_t n);   // piu' sotto, con il gancio del salvataggio
constexpr DWORD kP2StopAt = 0x00610fb7;
volatile DWORD g_p2StopSkip = 0x00611423, g_p2StopCont = 0x00610fcb, g_p2NoStop = 0x006114d9;
std::atomic<bool> g_p2UseHookOn{ false };

__declspec(naked) void Pad2StopStub()   // al posto di "cmp ax, 4 / je" in 0x610fb7: AX e registri intatti, i flag dopo non servono
{
    __asm {
        cmp  ax, 4
        je   skip
        cmp  ax, 8
        je   skip
        cmp  ax, 1
        je   skip
        cmp  ax, 0xc
        je   nostop                           // 0.55.22: modo 12, "raccogli tutto" (+0x104 resta 0)
        jmp  dword ptr [g_p2StopCont]         // 0x610fcb: stati 3 e 5, modi 0xb e 6, poi la fermata
    skip:
        jmp  dword ptr [g_p2StopSkip]         // 0x611423: con +0x104 diverso da 0 niente fermata
    nostop:
        jmp  dword ptr [g_p2NoStop]           // 0x6114d9: dopo la fermata (guinzaglio e resto del gestore)
    }
}

// Nel thread del motore, fra un fotogramma e l'altro; i 10 byte originali si controllano prima (Sacred.exe 2.28).
void InstallPad2StopHookNow()
{
    static const BYTE kOrig[10] = { 0x66, 0x3D, 0x04, 0x00, 0x0F, 0x84, 0x62, 0x04, 0x00, 0x00 };
    BYTE cur[10] = {};
    for (int i = 0; i < 10; ++i) {
        if (!Rd(kP2StopAt + i, cur[i])) { Log("PAD2: gancio della fermata NON installato: %08lX non leggibile", kP2StopAt); return; }
    }
    if (memcmp(cur, kOrig, sizeof(kOrig)) != 0) {
        Log("PAD2: gancio della fermata NON installato: in %08lX byte diversi (%02X %02X %02X %02X %02X), l'eroe 2 va al nemico con le frecce",
            kP2StopAt, cur[0], cur[1], cur[2], cur[3], cur[4]);
        return;
    }
    BYTE jmp[5] = { 0xE9, 0, 0, 0, 0 };
    const LONG rel = static_cast<LONG>(reinterpret_cast<DWORD>(&Pad2StopStub) - (kP2StopAt + 5));
    memcpy(jmp + 1, &rel, 4);
    if (!PatchCode(kP2StopAt, jmp, sizeof(jmp))) { Log("PAD2: gancio della fermata NON installato: scrittura non riuscita"); return; }
    g_p2UseHookOn.store(true);
    Log("PAD2: gancio della fermata installato in %08lX: il modo 1 dell'eroe 2 (vai e usa) non si ferma piu' a ogni passo", kP2StopAt);
}

// 0.55.37: Ctrl del pad 1 tenuto fuori dal gestore del giocatore 2. Prova del 31/10 sera sulla Steam Machine: 61 righe "eroe 2
// bloccato ... modo 4, stato 00010003" (l'eroe 2 colpisce sul posto e lo stick non lo sposta: "ogni tanto l'eroe 2 attaccava da
// solo", 23:48), ognuna mezzo secondo dopo un "A premuto: Ctrl+clic" del pad 1, che tiene Ctrl finche' A e' premuto (23:47:57.068
// -> 23:47:57.5, 00:05:04.313 -> 00:05:04.955, 00:14:01.374 -> 00:14:02.176). Il metodo 0x18 delle creature legge Ctrl con
// GetAsyncKeyState (tipo 7 in 0x52ffb0, tipo 4 in 0x52f270) e con Ctrl l'ordine delle frecce e di Y del gestore diventa "fermati
// e attacca" (modo 4 senza bersaglio). Le due chiamate del gestore (0x610783 e 0x6107a8: thiscall, 3 argomenti, ret 0xc) passano
// da Pad2HandlerStub, che segna il thread per la durata della chiamata: Hook_GetAsyncKeyState gli risponde con il solo Ctrl del
// pad 2 (Ctrl+0 di UseArtListNow, g_p2CtrlUntil). Lo stub non e' rientrante: il gestore non si richiama da se'.
std::atomic<DWORD> g_p2HandlerTid{ 0 }, g_p2HandlerAt{ 0 }, g_p2CtrlUntil{ 0 };
constexpr DWORD kP2HandlerCalls[2] = { 0x00610783, 0x006107a8 };
volatile DWORD g_p2HandlerFn = 0x006108a0, g_p2HandlerRet = 0;

void Pad2HandlerEnter() { g_p2HandlerAt.store(GetTickCount()); g_p2HandlerTid.store(GetCurrentThreadId()); }
void Pad2HandlerLeave() { g_p2HandlerTid.store(0); }

__declspec(naked) void Pad2HandlerStub()   // al posto di "call 0x6108a0": ECX e argomenti come li ha lasciati il gioco
{
    __asm {
        pop  dword ptr [g_p2HandlerRet]       // ritorno del chiamante: gli argomenti restano dove il gestore li cerca
        pushad
        pushfd
        call Pad2HandlerEnter
        popfd
        popad
        call dword ptr [g_p2HandlerFn]        // 0x6108a0 (ret 0xc toglie i 3 argomenti)
        pushad
        pushfd
        call Pad2HandlerLeave
        popfd
        popad
        push dword ptr [g_p2HandlerRet]
        ret
    }
}

// Nel thread del motore, fra un fotogramma e l'altro (non sta eseguendo le due chiamate); i byte si controllano prima.
void InstallPad2HandlerHookNow()
{
    for (DWORD at : kP2HandlerCalls) {
        BYTE op = 0; LONG rel = 0;
        if (!Rd(at, op) || !Rd(at + 1, rel) || op != 0xE8 || at + 5 + static_cast<DWORD>(rel) != g_p2HandlerFn) {
            Log("PAD2: gancio del Ctrl NON installato: %08lX non e' una chiamata a %08lX (byte %02X)", at, g_p2HandlerFn, op);
            return;
        }
    }
    for (DWORD at : kP2HandlerCalls) {
        const LONG mine = static_cast<LONG>(reinterpret_cast<DWORD>(&Pad2HandlerStub) - (at + 5));
        if (!PatchCode(at + 1, reinterpret_cast<const BYTE*>(&mine), 4)) { Log("PAD2: gancio del Ctrl NON installato in %08lX: scrittura non riuscita", at); return; }
    }
    Log("PAD2: gancio del Ctrl installato (%08lX, %08lX): nel gestore del giocatore 2 vale solo il Ctrl del pad 2", kP2HandlerCalls[0], kP2HandlerCalls[1]);
}

// 0.55.38: oggetti delle missioni raccolti dall'eroe 2 passati all'eroe 1. Prova del 31/10 sera: la spada del fabbro Noj raccolta
// dall'eroe 2 nel sotterraneo (00:20-00:24) non ha fatto completare la missione all'eroe 1 (00:28-00:33); il tester: "gli oggetti delle
// missioni hanno la scritta rosa quando li raccogli, e non li vedi nell'inventario". Studio: ogni voce d'inventario ([0xad33c8] + id
// dell'eroe * 0x1180) comincia con un vettore di coppie {tipo, valore} (+0 inizio, +4 fine, +8 capienza; NumQuestItem nel
// salvataggio 0x5de18f); la raccolta (0x558527, 0x5594fc) ci aggiunge l'oggetto con cInventoryEntry::AddQuestItem 0x5d8750 (thiscall,
// tipo, valore, rete; ret 0xc; con un tipo gia' presente non aggiunge nulla), nella voce dell'eroe che raccoglie; le missioni
// guardano quella dell'eroe locale. Qui, nel thread del motore, ogni coppia della voce dell'eroe 2 passa con la stessa funzione alla
// voce dell'eroe 1 (rete 0) e il vettore dell'eroe 2 si svuota (coppie di due DWORD: basta riportare la fine all'inizio).
// [Native] Hero2QuestItems.
constexpr DWORD kAddQuestItem = 0x005d8750;
void QuestItemsToHero1Now()
{
    DWORD inv = 0, player = 0, heroId = 0, mgr = 0;
    Rd(kInvTable, inv); Rd(kPlayer, player); Rd(kObjMgr, mgr);
    if (!inv || !player || !RdHero(player, heroId) || heroId == 0 || heroId == 2 || heroId >= 0x20 || !mgr || !ObjectAddr(mgr, 2)) return;
    const DWORD e1 = inv + heroId * kInvSize, e2 = inv + 2 * kInvSize;
    DWORD b = 0, e = 0;
    if (!Rd(e2, b) || !Rd(e2 + 4, e) || !b || e <= b || (e - b) % 8 != 0 || e - b > 8 * 64) return;
    std::vector<DWORD> items((e - b) / 4);
    if (!RdBuf(b, items.data(), e - b)) return;
    for (size_t k = 0; k + 1 < items.size(); k += 2) {
        const CallResult cr = CallThis3(kAddQuestItem, e1, items[k], items[k + 1], 0);
        if (!cr.ok) { LogException("passaggio di un oggetto delle missioni all'eroe 1"); return; }
        if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x5d8750 ha lasciato lo stack sbilanciato (corretto)");
        char name[64] = "?";
        TypeNameById(items[k], name, sizeof(name));
        Log("EROI: oggetto delle missioni %s (tipo %lu, valore %lu) raccolto dall'eroe 2: passato all'eroe 1", name, items[k], items[k + 1]);
    }
    WriteDword(e2 + 4, b);
    OverlayCoopToast(Tr("Oggetto delle missioni dato all'eroe 1"), 3000);
}

// 0.55.39: bottino diviso fra le classi dei due eroi (richiesta del tester del 31/10 sera: "drop armi e in generale: 50% dei due
// eroi"). TypeManager::getRandomItem (0x5c8410, stringa "TypeManager::getRandomItem(Fam=%d Lvl=%d, ...)", 23 chiamanti: bottino e
// merce) controlla in 0x5c8cd8 che ci sia l'eroe locale (GetHeroId), poi fino a tre volte crea un oggetto a caso (0x5fba80) e lo
// tiene solo se l'eroe locale lo puo' usare (0x5c8d35: GetHeroId, 0x5c8d3c: 0x54afe0), altrimenti lo distrugge (0x5fc300): per
// questo il bottino era sempre per la classe dell'eroe 1. Con l'eroe 2 della mod nel mondo, in 0x5c8cd8 si sceglie a caso, una
// volta per oggetto, l'eroe 1 o l'eroe 2, e in 0x5c8d35 si risponde con quello. [Native] SharedLoot.
volatile DWORD g_getHeroIdFn = 0x00604380;
constexpr DWORD kLootPickCall = 0x005c8cd8, kLootUseCall = 0x005c8d35;
std::atomic<DWORD> g_lootHero{ 0 };      // creatura per l'oggetto in creazione (0 = eroe locale)

void LootPickHero(DWORD mgr)
{
    g_lootHero.store(0);
    if (!g_cfg.nativeSharedLoot || !mgr) return;
    const DWORD h2 = ObjectAddr(mgr, 2);
    if (!h2 || h2 != g_h2MineAddr.load()) return;
    static DWORD seed = GetTickCount();
    seed = seed * 1103515245u + 12345u;
    if ((seed >> 16) & 1u) g_lootHero.store(h2);
}
DWORD LootUseHero() { return g_lootHero.load(); }

__declspec(naked) void LootPickStub()   // al posto di "call GetHeroId" in 0x5c8cd8 (ECX = gestore degli oggetti)
{
    __asm {
        push ecx
        push ecx
        call LootPickHero
        add  esp, 4
        pop  ecx
        jmp  dword ptr [g_getHeroIdFn]        // GetHeroId com'era: qui serve solo a sapere se c'e' un eroe
    }
}

__declspec(naked) void LootUseStub()    // al posto di "call GetHeroId" in 0x5c8d35 (argomenti di 0x54afe0 gia' sullo stack)
{
    __asm {
        push ecx
        call LootUseHero
        pop  ecx
        test eax, eax
        jz   orig
        ret
    orig:
        jmp  dword ptr [g_getHeroIdFn]
    }
}

// 0.56.16: cure di un eroe sull'altro (il tester, 04/11: "tutte le abilita' di cura se usate curano entrambi gli eroi in automatico,
// cosi' l'eroe che ha l'abilita' di cura non deve mirare l'altro eroe, che e' veramente difficile"). Studio: l'effetto delle arti
// 0x5b2890, nel caso della cura ("ObjectFX: %s do healing to %d", 0x958b48), prende la creatura bersaglio da [effetto+0x60] e le
// manda un evento sullo stack (vtable 0x88f95c: +4 = 6, +0x10 = chi cura [effetto+0x54], +0x14 = 3, +0x18 = quanto) con il suo
// metodo +0x18 (cCreature 0x52e9a0, thiscall, ret 4): 0x5b57f0 mov [esp+0x1960], 0x28 / 0x5b57fb call [edx+0x18]. Qui quei 14 byte
// diventano una chiamata a HealStub, che fa le stesse due cose e poi, se il bersaglio e' uno dei due eroi, manda lo stesso evento
// all'altro eroe (vivo, sullo stesso piano, entro ShareXpRange). [Native] ShareHeal.
constexpr DWORD kHealSite = 0x005b57f0, kHealCont = 0x005b57fe;
volatile DWORD g_healEv = 0, g_healTgt = 0, g_healCont = kHealCont;
static void __cdecl HealShareNow()
{
    static bool inShare = false;
    const DWORD ev = g_healEv, tgt = g_healTgt;
    if (inShare || !g_cfg.nativeShareHeal || !ev || !tgt) return;
    DWORD type = 0, sub = 0, src = 0; int amount = 0;
    if (!Rd(ev + 4, type) || type != 6 || !Rd(ev + 0x14, sub) || sub != 3 || !Rd(ev + 0x18, amount) || amount <= 0) return;
    Rd(ev + 0x10, src);
    DWORD mgr = 0, player = 0, heroId = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player);
    if (player) RdHero(player, heroId);
    const DWORD h1 = mgr && heroId ? ObjectAddr(mgr, heroId) : 0, h2 = mgr ? ObjectAddr(mgr, 2) : 0;
    if (!h1 || !h2 || h2 != g_h2MineAddr.load()) return;
    const DWORD other = tgt == h1 ? h2 : tgt == h2 ? h1 : 0;
    if (!other) return;
    DWORD hp = 0, vt = 0, fn = 0; int ax = 0, ay = 0, bx = 0, by = 0; BYTE az = 0, bz = 0;
    if (!Rd(other + 0x4d8, hp) || hp == 0 || !InWorld(other)) return;
    Rd(tgt + 0x1c, ax); Rd(tgt + 0x20, ay); Rd(tgt + 0x24, az); Rd(other + 0x1c, bx); Rd(other + 0x20, by); Rd(other + 0x24, bz);
    const double dx = ax - bx, dy = ay - by;
    if (az != bz || dx * dx + dy * dy > static_cast<double>(g_cfg.nativeXpRange) * g_cfg.nativeXpRange) return;
    if (!Rd(other, vt) || !Rd(vt + 0x18, fn) || fn != 0x0052e9a0) return;
    inShare = true;
    const CallResult cr = CallThis1(fn, other, ev);
    inShare = false;
    if (!cr.ok || !cr.balanced) { LogException("cura condivisa"); return; }
    static DWORD logAt = 0;
    if (GetTickCount() - logAt > 1000) {
        logAt = GetTickCount();
        DWORD sid = 0; Rd(other + 0xc, sid);
        Log("EROI: cura di %d (da %lu) anche all'eroe %lu", amount, src, sid);
    }
}
__declspec(naked) void HealStub()   // al posto di 0x5b57f0..0x5b57fd (ECX = bersaglio, EDX = suo vtable, evento gia' spinto)
{
    __asm {
        add  esp, 4                             // indirizzo di ritorno: si torna a kHealCont
        mov  eax, [esp]
        mov  g_healEv, eax
        mov  g_healTgt, ecx
        mov  dword ptr [esp + 0x1960], 0x28     // come 0x5b57f0
        call dword ptr [edx + 0x18]             // come 0x5b57fb (toglie l'evento dallo stack)
        pushad
        pushfd
        call HealShareNow
        popfd
        popad
        jmp  dword ptr [g_healCont]
    }
}
void InstallHealHookNow()
{
    static const BYTE want[14] = { 0xC7, 0x84, 0x24, 0x60, 0x19, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0xFF, 0x52, 0x18 };
    BYTE cur[14] = {};
    if (!RdBuf(kHealSite, cur, sizeof(cur)) || memcmp(cur, want, sizeof(want)) != 0) {
        Log("EROI: cure condivise NON installate: codice inatteso a %08lX (%02X %02X %02X)", kHealSite, cur[0], cur[1], cur[2]);
        return;
    }
    BYTE patch[14];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE8;
    const LONG rel = static_cast<LONG>(reinterpret_cast<DWORD>(&HealStub) - (kHealSite + 5));
    memcpy(patch + 1, &rel, 4);
    if (PatchCode(kHealSite, patch, sizeof(patch))) Log("EROI: cure condivise installate (%08lX): una cura su un eroe vale anche per l'altro, vicino", kHealSite);
    else Log("EROI: cure condivise NON installate: scrittura non riuscita");
}

// 0.56.16: "raggiungi il compagno" (doppio L3 di uno dei due pad): l'eroe 2 accanto all'eroe 1, con il teletrasporto della mod
// (0x5fce00 / 0x5fd200, come nel rialzo). Solo con tutti e due vivi e sullo stesso piano (+0x24).
void MemoryJoinCompanion(int who)
{
    RunOnGameThread([who] {
        DWORD mgr = 0, player = 0, heroId = 0;
        Rd(kObjMgr, mgr); Rd(kPlayer, player);
        if (player) RdHero(player, heroId);
        const DWORD h1 = mgr && heroId ? ObjectAddr(mgr, heroId) : 0, h2 = mgr ? ObjectAddr(mgr, 2) : 0;
        if (!h1 || !h2 || h2 != g_h2MineAddr.load()) return;
        DWORD hp1 = 0, hp2 = 0; BYTE z1 = 0, z2 = 0; int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        Rd(h1 + 0x4d8, hp1); Rd(h2 + 0x4d8, hp2); Rd(h1 + 0x24, z1); Rd(h2 + 0x24, z2);
        Rd(h1 + 0x1c, x1); Rd(h1 + 0x20, y1); Rd(h2 + 0x1c, x2); Rd(h2 + 0x20, y2);
        if (!hp1 || !hp2 || !InWorld(h1) || !InWorld(h2)) { OverlayCoopToast(Tr("Raggiungi il compagno: servono tutti e due vivi"), 2000); return; }
        if (z1 != z2) { OverlayCoopToast(Tr("Raggiungi il compagno: siete su piani diversi"), 2000); return; }
        BYTE pos[16];
        if (!RdBuf(h1 + 0x18, pos, sizeof(pos))) return;
        const int nx = x1 + 80;
        memcpy(pos + 4, &nx, 4);
        const double d = sqrt(static_cast<double>(x1 - x2) * (x1 - x2) + static_cast<double>(y1 - y2) * (y1 - y2));
        if (!TeleportObject(mgr, 2, pos)) { LogException("raggiungi il compagno"); return; }
        OverlayCoopToast(Tr("L'eroe 2 ha raggiunto l'eroe 1"), 2000);
        Log("EROI: raggiungi il compagno (doppio L3 del pad %d): eroe 2 accanto all'eroe 1, era a %.0f unita'", who, d);
    });
}

// 0.56.16: posizione dei due eroi sullo schermo per gli indicatori (Overlay.cpp, nel thread del motore). Falso senza eroe 2 della mod.
bool MemoryHeroesScreen(POINT* p, bool* on, int& w, int& h, float* dir)
{
    if (!g_ok) return false;
    View v;
    if (!ReadView(v)) return false;
    DWORD mgr = 0, player = 0, heroId = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player);
    if (player) RdHero(player, heroId);
    const DWORD hs[2] = { mgr && heroId ? ObjectAddr(mgr, heroId) : 0, mgr ? ObjectAddr(mgr, 2) : 0 };
    if (!hs[0] || !hs[1] || hs[1] != g_h2MineAddr.load()) return false;
    for (int i = 0; i < 2; ++i) {
        int x = 0, y = 0; float sx = 0, sy = 0;
        on[i] = InWorld(hs[i]) && Rd(hs[i] + 0x1c, x) && Rd(hs[i] + 0x20, y) && WorldToScreen(v, x, y, sx, sy);
        p[i].x = static_cast<LONG>(sx); p[i].y = static_cast<LONG>(sy);
        // 0.56.18: dove guarda l'eroe: vettore nel mondo in +0x70 (x, y, z float; 0x450a30 ne fa l'angolo sullo schermo con la
        // matrice della vista, 0x6229c0), portato sullo schermo proiettando il punto 100 unita' piu' in la'
        if (dir) {
            float f[2] = {}; float ex = 0, ey = 0;
            dir[i * 2] = 0.0f; dir[i * 2 + 1] = 1.0f;                // verso lo spettatore se non si legge
            if (on[i] && RdBuf(hs[i] + 0x70, f, sizeof(f)) && f[0] * f[0] + f[1] * f[1] > 0.01f) {
                const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1]);
                if (WorldToScreen(v, x + static_cast<int>(f[0] / fl * 100.0f), y + static_cast<int>(f[1] / fl * 100.0f), ex, ey)) {
                    const float qx = ex - sx, qy = ey - sy, ql = std::sqrt(qx * qx + qy * qy);
                    if (ql > 0.5f) { dir[i * 2] = qx / ql; dir[i * 2 + 1] = qy / ql; }
                }
            }
            static DWORD logAt[2] = {};
            if (GetTickCount() - logAt[i] > 5000) {         // per la prova: il campo +0x70 e' davvero dove guarda l'eroe?
                logAt[i] = GetTickCount();
                Log("INDICATORI: eroe %d, +0x70 (%.2f, %.2f) -> sullo schermo (%.2f, %.2f)", i + 1, f[0], f[1], dir[i * 2], dir[i * 2 + 1]);
            }
        }
    }
    w = v.w; h = v.h;
    return on[0] || on[1];
}

void InstallLootHookNow()
{
    const DWORD sites[2] = { kLootPickCall, kLootUseCall };
    const DWORD stubs[2] = { reinterpret_cast<DWORD>(&LootPickStub), reinterpret_cast<DWORD>(&LootUseStub) };
    for (DWORD at : sites) {
        BYTE op = 0; LONG rel = 0;
        if (!Rd(at, op) || !Rd(at + 1, rel) || op != 0xE8 || at + 5 + static_cast<DWORD>(rel) != g_getHeroIdFn) {
            Log("EROI: bottino diviso NON installato: %08lX non e' una chiamata a GetHeroId (byte %02X)", at, op);
            return;
        }
    }
    for (int k = 0; k < 2; ++k) {
        const LONG mine = static_cast<LONG>(stubs[k] - (sites[k] + 5));
        if (!PatchCode(sites[k] + 1, reinterpret_cast<const BYTE*>(&mine), 4)) { Log("EROI: bottino diviso NON installato in %08lX", sites[k]); return; }
    }
    Log("EROI: bottino diviso installato (%08lX, %08lX): con l'eroe 2 un oggetto su due e' per la sua classe", kLootPickCall, kLootUseCall);
}

// 0.55.39: installazione del gancio della mappa scoperta (ExploredLoadStub, vicino a SpawnHero2), dopo la dichiarazione di PatchCode.
void InstallExploredHookNow()
{
    BYTE op = 0; LONG rel = 0;
    if (!Rd(kExploredLoadCall, op) || !Rd(kExploredLoadCall + 1, rel) || op != 0xE8 || kExploredLoadCall + 5 + static_cast<DWORD>(rel) != g_exploredSet) {
        Log("EROE2: gancio della mappa scoperta NON installato: %08lX non e' una chiamata a %08lX (byte %02X)", kExploredLoadCall, g_exploredSet, op);
        return;
    }
    const LONG mine = static_cast<LONG>(reinterpret_cast<DWORD>(&ExploredLoadStub) - (kExploredLoadCall + 5));
    if (!PatchCode(kExploredLoadCall + 1, reinterpret_cast<const BYTE*>(&mine), 4)) { Log("EROE2: gancio della mappa scoperta NON installato: scrittura non riuscita"); return; }
    Log("EROE2: gancio della mappa scoperta installato in %08lX: alla comparsa dell'eroe 2 le due mappe si sommano", kExploredLoadCall);
}

// 0.55.40: nebbia della mappa tolta anche intorno all'eroe 2. Prova del 31/10 sera: "il pg 2 non toglie la nebbia nella minimappa,
// non la esplora". Studio: 0x599a50 (creatura, thiscall: cella x, cella y, modo; ret 0xc; 10 chiamanti nei movimenti) scopre le
// celle intorno (maschera 29x29 in 0x8905cc, o 15x15 da 0x890328 per i modi 1..) con 0x639230 (mondo [0xad24e0]: settore dalla
// griglia +0x284, poi 0x622eb0 accende il bit), ma solo se l'id della creatura (+0xc; cavalli ed evocati risalgono al padrone
// con +0x1ec) e' quello dell'eroe locale: 0x599aaa call 0x7d7f30, poi [giocatore+0x14] contro [esi+0xc]. Qui la chiamata passa
// da RevealPlayerStub: se ESI (la creatura gia' risalita) e' l'eroe 2 della mod (g_revealHero2, aggiornato da MemoryNativeProbe,
// solo senza rete) risponde con un giocatore finto che ha +0x14 = 2, altrimenti va al gioco. [Native] RevealHero2.
constexpr DWORD kRevealPlayerCall = 0x00599aaa;
volatile DWORD g_revealHero2 = 0;
DWORD g_revealFakePlayer[0x10] = { 0, 0, 0, 0, 0, 2 };   // +0x14 = 2

__declspec(naked) void RevealPlayerStub()   // come 0x7d7f30: nessun argomento, restituisce il giocatore in EAX
{
    __asm {
        mov  eax, g_revealHero2
        test eax, eax
        jz   orig
        cmp  esi, eax
        jne  orig
        lea  eax, g_revealFakePlayer
        ret
    orig:
        mov  eax, 0x007d7f30            // kGetPlayer
        jmp  eax
    }
}

// 0.55.44: 0x599a50 la chiama solo l'aggiornamento dell'eroe locale (0x60c9e8, dopo la copia dell'anti-trucchi; prova del 01/11:
// "non funziona"): la si chiama qui per l'eroe 2, nel thread del motore, quando cambia cella. Cella come 0x622880: posizione
// (+0x1c, +0x20) diviso il float [0x891044], troncata. Il controllo dell'eroe locale lo passa RevealPlayerStub.
void RevealHero2Tick()
{
    static DWORD nextAt = 0;
    static int lastCx = -1, lastCy = -1;
    const DWORD now = GetTickCount();
    if (nextAt && static_cast<LONG>(now - nextAt) < 0) return;   // 0.55.46: il primo giro anche dopo 24,8 giorni di accensione
    nextAt = now + 200;
    if (!nextAt) nextAt = 1;
    const DWORD h2 = g_revealHero2;
    if (!h2) { lastCx = lastCy = -1; return; }
    int x = 0, y = 0;
    float div = 0;
    DWORD fl = 0;
    if (!Rd(h2 + 0x14, fl) || (fl & 2) || !Rd(h2 + 0x1c, x) || !Rd(h2 + 0x20, y) || !Rd(0x00891044, div) || div <= 0.0f) return;
    const int cx = static_cast<int>(x / div), cy = static_cast<int>(y / div);
    if (cx == lastCx && cy == lastCy) return;
    lastCx = cx; lastCy = cy;
    const CallResult cr = CallThis3(0x00599a50, h2, static_cast<DWORD>(cx), static_cast<DWORD>(cy), 0);
    if (!cr.ok) { g_revealHero2 = 0; LogException("mappa scoperta dall'eroe 2"); return; }
    if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x599a50 ha lasciato lo stack sbilanciato (corretto)");
    static bool logged = false;
    if (!logged) { logged = true; Log("EROE2: la mappa si scopre anche intorno all'eroe 2 (cella %d,%d)", cx, cy); }
}

// 0.55.44: missioni degli oggetti raccolti dall'eroe 2. Prova del 01/11: la lettera OBJECT_QITEM_NQ01_2_LETTER01 passata all'eroe 1
// (0.55.38) non ha chiuso la missione dal fabbro. Studio: alla raccolta (0x559445, 0x5584f7) il gioco chiede al gestore dei
// trigger lo script dell'oggetto (0x4a75d0) e, se comincia con "Take:", lo esegue con 0x46c320(nome, creatura che raccoglie, 0)
// (cdecl; cerca la funzione nella tabella [0xaaa688] e la lancia con 0x46be70): con l'eroe 2 lo script girava per lui. Le due
// chiamate (0x55945b, 0x55850d) passano da TakeScriptStub, che al posto dell'eroe 2 della mod mette l'eroe 1.
volatile DWORD g_takeHero1 = 0, g_takeHero2 = 0, g_takeFn = 0x0046c320;
constexpr DWORD kTakeCalls[2] = { 0x0055945b, 0x0055850d };

__declspec(naked) void TakeScriptStub()   // al posto di "call 0x46c320": [esp+4] nome, [esp+8] creatura, [esp+0xc] flag
{
    __asm {
        mov  eax, dword ptr [esp + 8]
        test eax, eax
        jz   orig
        cmp  eax, g_takeHero2
        jne  orig
        mov  eax, g_takeHero1
        test eax, eax
        jz   orig
        mov  dword ptr [esp + 8], eax
    orig:
        jmp  dword ptr [g_takeFn]
    }
}

// 0.56.53: punti delle missioni anche all'eroe 2 (studio del 07/11). Negli script, gli argomenti "HERO", "CPOS:hero" e "QUESTFELLOW"
// li risolve 0x472fa0 con GetHeroId: ogni comando degli script vale per l'eroe locale. Due comandi danno punti: il caso 0x8a
// dell'interprete 0x475a60 (0x4b0b70: N volte 0x581140(7) sulle statistiche +0x3a8, un punto attributo) e il caso 0x8b (0x4b0d50:
// +0x3ea += N, punti abilita'); nel FunkCode.bin dell'elfo oscuro ce n'e' uno per tipo (0x4c9ee e 0x4c9f7, valore 2). Le chiamate
// a GetHeroId dei due comandi (0x4b0be2 con N in ebx, 0x4b0dc2 con N in ebp) passano da RewardAttrStub e RewardSkillStub, che
// sommano N in un contatore; RewardPointsTick (thread della mod) li da' all'eroe 2 della mod nel thread del motore. [Native]
// RewardPoints.
volatile LONG g_rwAttr = 0, g_rwSkill = 0;
volatile DWORD g_getHeroFn = 0x00604380;
constexpr DWORD kRewardAttrCall = 0x004b0be2, kRewardSkillCall = 0x004b0dc2;
__declspec(naked) void RewardAttrStub()    // al posto di "call 0x604380" in 0x4b0be2: ebx = punti attributo
{
    __asm {
        lock add dword ptr [g_rwAttr], ebx
        jmp  dword ptr [g_getHeroFn]
    }
}
__declspec(naked) void RewardSkillStub()   // al posto di "call 0x604380" in 0x4b0dc2: ebp = punti abilita'
{
    __asm {
        lock add dword ptr [g_rwSkill], ebp
        jmp  dword ptr [g_getHeroFn]
    }
}
void InstallRewardPointsHookNow()
{
    const DWORD at[2] = { kRewardAttrCall, kRewardSkillCall };
    const DWORD stub[2] = { reinterpret_cast<DWORD>(&RewardAttrStub), reinterpret_cast<DWORD>(&RewardSkillStub) };
    for (DWORD a : at) {
        BYTE op = 0; LONG rel = 0;
        if (!Rd(a, op) || !Rd(a + 1, rel) || op != 0xE8 || a + 5 + static_cast<DWORD>(rel) != g_getHeroFn) {
            Log("EROI: gancio dei punti delle missioni NON installato: %08lX non e' una chiamata a GetHeroId (byte %02X)", a, op);
            return;
        }
    }
    for (int k = 0; k < 2; ++k) {
        const LONG mine = static_cast<LONG>(stub[k] - (at[k] + 5));
        if (!PatchCode(at[k] + 1, reinterpret_cast<const BYTE*>(&mine), 4)) { Log("EROI: gancio dei punti delle missioni NON installato in %08lX", at[k]); return; }
    }
    Log("EROI: gancio dei punti delle missioni installato (%08lX, %08lX): punti attributo e abilita' anche all'eroe 2", at[0], at[1]);
}
void RewardPointsTick()
{
    const LONG attr = InterlockedExchange(&g_rwAttr, 0), skill = InterlockedExchange(&g_rwSkill, 0);
    if (attr <= 0 && skill <= 0) return;
    DWORD mgr = 0; BYTE net = 0;
    Rd(kObjMgr, mgr); Rd(kNetGame, net);
    const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0;
    if (!h2 || h2 != g_h2MineAddr.load() || net) { Log("EROI: missione: %ld punti attributo e %ld abilita' all'eroe 1, nessun eroe 2 della mod", attr, skill); return; }
    Log("EROI: missione: %ld punti attributo e %ld punti abilita' all'eroe 1: anche all'eroe 2", attr, skill);
    RunOnGameThread([attr, skill] {
        DWORD m = 0;
        Rd(kObjMgr, m);
        const DWORD e2 = m ? ObjectAddr(m, 2) : 0;
        if (!e2) return;
        if (skill > 0) {
            WORD p = 0;
            Rd(e2 + 0x3ea, p);
            const WORD np = static_cast<WORD>(p + skill);
            WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(e2 + 0x3ea)), &np, 2, nullptr);
        }
        for (LONG i = 0; i < attr && i < 50; ++i) {
            const CallResult cr = CallThis1(0x00581140, e2 + 0x3a8, 7);
            if (!cr.ok) { LogException("punto attributo all'eroe 2"); break; }
        }
        Log("EROI: missione: all'eroe 2 %ld punti attributo e %ld punti abilita'", attr, skill);
        OverlayCoopToast(Tr("Missione: punti anche per l'eroe 2"), 4000);
    });
}

void InstallTakeHookNow()
{
    for (DWORD at : kTakeCalls) {
        BYTE op = 0; LONG rel = 0;
        if (!Rd(at, op) || !Rd(at + 1, rel) || op != 0xE8 || at + 5 + static_cast<DWORD>(rel) != g_takeFn) {
            Log("EROI: gancio degli script delle missioni NON installato: %08lX non e' una chiamata a %08lX (byte %02X)", at, g_takeFn, op);
            return;
        }
    }
    for (DWORD at : kTakeCalls) {
        const LONG mine = static_cast<LONG>(reinterpret_cast<DWORD>(&TakeScriptStub) - (at + 5));
        if (!PatchCode(at + 1, reinterpret_cast<const BYTE*>(&mine), 4)) { Log("EROI: gancio degli script delle missioni NON installato in %08lX", at); return; }
    }
    Log("EROI: gancio degli script delle missioni installato (%08lX, %08lX): gli oggetti raccolti dall'eroe 2 contano per l'eroe 1", kTakeCalls[0], kTakeCalls[1]);
}

void InstallRevealHookNow()
{
    BYTE op = 0; LONG rel = 0;
    if (!Rd(kRevealPlayerCall, op) || !Rd(kRevealPlayerCall + 1, rel) || op != 0xE8 || kRevealPlayerCall + 5 + static_cast<DWORD>(rel) != 0x007d7f30) {
        Log("EROE2: gancio della nebbia NON installato: %08lX non e' una chiamata a 0x7d7f30 (byte %02X)", kRevealPlayerCall, op);
        return;
    }
    const LONG mine = static_cast<LONG>(reinterpret_cast<DWORD>(&RevealPlayerStub) - (kRevealPlayerCall + 5));
    if (!PatchCode(kRevealPlayerCall + 1, reinterpret_cast<const BYTE*>(&mine), 4)) { Log("EROE2: gancio della nebbia NON installato: scrittura non riuscita"); return; }
    Log("EROE2: gancio della nebbia installato in %08lX: anche l'eroe 2 scopre la mappa", kRevealPlayerCall);
}

// 0.55.47: rune delle ricompense anche per l'eroe 2 (richiesta del tester del 01/11: "quando si compie una missione la ricompensa e'
// per entrambi gli eroi: se l'npc da' una cosa esclusivamente per l'eroe 1, deve darla anche compatibile per l'eroe 2"; studio
// nella sezione 8c del documento). Le ricompense le danno gli script delle missioni; qui, senza leggerli: ogni mezzo secondo si
// guarda lo zaino dell'eroe 1 (voce dell'inventario, caselle da 12 byte da +0x18, +0 id dell'oggetto, +4 bit 0 occupata); una
// runa nuova della classe dell'eroe 1 (RuneClass) conta come ricompensa se non e' stata vista a terra negli ultimi 30 s (cioe'
// non raccolta), se l'eroe 1 parlava con un PNG negli ultimi 8 s e se non sono aperte finestre del gioco (mercante, deposito)
// ne' lo scambio dell'interfaccia. Allora nel thread del motore si crea una runa a caso della classe dell'eroe 2 (0x5fba80: tipo,
// 0, 0, 0 come in getRandomItem) e la si posa a terra accanto all'eroe 2 (push e pop del gestore degli oggetti). [Native] RewardRunes.
void GiveRuneToHero2Now(DWORD type)
{
    DWORD mgr = 0;
    Rd(kObjMgr, mgr);
    const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0;
    if (!h2) return;
    const CallResult cr = CallThis4(0x005fba80, mgr, type, 0, 0, 0);
    if (!cr.ok) { LogException("creazione della runa per l'eroe 2"); return; }
    if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x5fba80 ha lasciato lo stack sbilanciato (corretto)");
    // 0.56.47 (prova sulla Steam Machine del 06/11 alle 00:32:04 e 00:41:49: "runa gemella non creata"): 0x5fba80 restituisce il
    // numero dell'oggetto, non il suo indirizzo (getRandomItem a 0x5c8d26 lo passa cosi' a 0x54afe0 e a 0x5fc300)
    DWORD obj = cr.ret;
    if (obj && obj < 0x00100000u) obj = ObjectById(obj);
    DWORD id = 0, fl = 0;
    if (!obj || !Rd(obj + 0xc, id) || !Rd(obj + 0x14, fl)) { Log("EROI: runa per l'eroe 2 non creata (tipo %lu)", type); return; }
    BYTE pos[16] = {};
    RdBuf(h2 + 0x18, pos, sizeof(pos));
    int x = 0; memcpy(&x, pos + 4, 4); x += 60; memcpy(pos + 4, &x, 4);
    bool ok;
    if (fl & 2) {   // fuori dal mondo: basta il pop
        const CallResult p = CallThis2(0x005fd200, mgr, id, reinterpret_cast<DWORD>(pos));
        ok = p.ok;
    } else ok = TeleportObject(mgr, id, pos);
    char name[64] = "?";
    TypeNameById(type, name, sizeof(name));
    Log("EROI: ricompensa: runa %s (tipo %lu, oggetto %lu, flag %08lX) per l'eroe 2, posata accanto a lui: %s", name, type, id, fl, ok ? "ok" : "NON riuscito");
    if (ok) OverlayCoopToast(Tr("Ricompensa: una runa per l'eroe 2, a terra accanto a lui"), 5000);
}

// 0.56.45: rune gemelle dei forzieri (il tester, 05/11: "anche nel baule appena aperto, sarebbe interessante che se viene data
// un'abilita' compatibile con la classe dell'eroe 1, venga data un'abilita' compatibile con la classe dell'eroe 2, per farli
// procedere in progressione alla pari"). Il bottino diviso (0.55.39) fa per meta' oggetti della classe dell'eroe 2, ma a caso: un
// forziere puo' dare una runa a uno solo. Qui RewardRuneTick guarda anche i forzieri (+0x64 da 0 ad aperto) e le rune che compaiono
// a terra entro 5 s a meno di 350 unita' da uno appena aperto: per una runa della classe di un eroe ne crea una a caso della classe
// dell'altro (0x5fba80, come GiveRuneToHero2Now), a terra accanto. Le gemelle non fanno altre gemelle (numeri e posti ricordati).
// [Native] ChestRunes.
std::atomic<DWORD> g_twinIds[8];      // memoria statica: tutti a 0
std::atomic<unsigned> g_twinN{ 0 };
static bool IsTwinRune(DWORD id)
{
    for (const auto& t : g_twinIds) if (t.load() == id) return true;
    return false;
}
struct RunePos { BYTE b[16]; };
// kind (0.56.54): 0 gemella di un forziere, 1 runa per l'eroe 2 da una ricompensa a terra, 2 oggetto dello stesso tipo da una ricompensa
void GiveTwinRuneNow(DWORD type, RunePos pos, DWORD forId, int kind = 0)
{
    DWORD mgr = 0;
    Rd(kObjMgr, mgr);
    if (!mgr) return;
    const CallResult cr = CallThis4(0x005fba80, mgr, type, 0, 0, 0);
    if (!cr.ok) { LogException("creazione della runa gemella"); return; }
    if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x5fba80 ha lasciato lo stack sbilanciato (corretto)");
    // 0.56.47 (prova sulla Steam Machine del 06/11 alle 00:32:04 e 00:41:49: "runa gemella non creata"): 0x5fba80 restituisce il
    // numero dell'oggetto, non il suo indirizzo (getRandomItem a 0x5c8d26 lo passa cosi' a 0x54afe0 e a 0x5fc300)
    DWORD obj = cr.ret;
    if (obj && obj < 0x00100000u) obj = ObjectById(obj);
    DWORD id = 0, fl = 0;
    if (!obj || !Rd(obj + 0xc, id) || !Rd(obj + 0x14, fl)) { Log("EROI: forziere: runa gemella non creata (tipo %lu)", type); return; }
    g_twinIds[g_twinN.fetch_add(1) % 8].store(id);
    int x = 0; memcpy(&x, pos.b + 4, 4); x += 45; memcpy(pos.b + 4, &x, 4);
    bool ok;
    if (fl & 2) ok = CallThis2(0x005fd200, mgr, id, reinterpret_cast<DWORD>(pos.b)).ok;
    else ok = TeleportObject(mgr, id, pos.b);
    char name[64] = "?";
    TypeNameById(type, name, sizeof(name));
    Log("EROI: %s %s (tipo %lu, oggetto %lu) accanto all'oggetto %lu: %s", kind == 0 ? "forziere: runa gemella" : kind == 1 ? "ricompensa: runa per l'eroe 2" : "ricompensa: oggetto per l'eroe 2",
        name, type, id, forId, ok ? "ok" : "NON riuscito");
    if (ok) OverlayCoopToast(Tr(kind == 0 ? "Forziere: una runa anche per l'altro eroe, a terra" : "Ricompensa anche per l'eroe 2, a terra"), 4000);
}

void RewardRuneTick(DWORD now)
{
    static DWORD nextAt = 0, talkAt = 0, lastSeq = 0xffffffffu;
    static std::set<DWORD> known;
    static std::map<DWORD, DWORD> ground;   // runa a terra -> ultima volta vista
    static std::map<DWORD, DWORD> groundAll;   // 0.56.54: oggetto a terra -> ultima volta visto
    static bool init = false;
    struct ChestOpen { int x, y; BYTE z; DWORD at; };
    struct Pending { int x, y; DWORD at; };
    static std::map<DWORD, DWORD> chestSt;  // 0.56.45: forziere -> +0x64 dell'ultimo giro
    static std::vector<ChestOpen> opened;   // forzieri aperti negli ultimi 5 s
    static std::vector<Pending> pending;    // gemelle chieste negli ultimi 3 s
    static bool groundReady = false;
    if (nextAt && static_cast<LONG>(now - nextAt) < 0) return;
    nextAt = now + 500;
    if (!nextAt) nextAt = 1;
    DWORD mgr = 0, player = 0, heroId = 0, inv = 0;
    BYTE net = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player); Rd(kInvTable, inv); Rd(kNetGame, net);
    const DWORD h2 = mgr ? ObjectAddr(mgr, 2) : 0;
    const DWORD h1 = (h2 && player && RdHero(player, heroId) && heroId && heroId != 2 && heroId < 0x20) ? ObjectAddr(mgr, heroId) : 0;
    if (!h1 || !h2 || h2 != g_h2MineAddr.load() || net || !inv) { init = false; return; }
    const DWORD seq = g_h2SpawnSeq.load();
    if (seq != lastSeq) { lastSeq = seq; init = false; ground.clear(); groundAll.clear(); chestSt.clear(); opened.clear(); pending.clear(); groundReady = false; }
    DialogInfo dd;
    if (MemoryHeroTalking() || MemoryDialog(dd)) talkAt = now ? now : 1;
    DWORD c1 = 0, c2 = 0;
    Rd(h1 + 0x10, c1); Rd(h2 + 0x10, c2);
    std::vector<Obj> objs;   // rune a terra (per riconoscere quelle raccolte)
    std::vector<Obj> fresh;  // 0.56.45: rune comparse a terra in questo giro
    std::vector<Obj> freshAll;   // 0.56.54: oggetti comparsi a terra in questo giro (ricompense delle missioni)
    if (ReadAllObjects(objs)) {
        if (chestSt.size() > 5000) chestSt.clear();
        for (const Obj& o : objs) {
            DWORD t = 0, fl = 0;
            const bool item = IsItemVt(o.vt) && Rd(o.addr + 0x10, t) && Rd(o.addr + 0x14, fl) && !(fl & 2);
            if (item) {
                if (groundReady && !groundAll.count(o.id)) freshAll.push_back(o);
                groundAll[o.id] = now;
            }
            if (item && t >= 5375 && t <= 5782) {
                if (groundReady && !ground.count(o.id)) fresh.push_back(o);
                ground[o.id] = now;
            }
            if (g_cfg.nativeChestRunes && o.vt == kVtChest) {
                DWORD st = 0;
                if (Rd(o.addr + 0x64, st)) {
                    const auto it = chestSt.find(o.id);
                    if (it != chestSt.end() && it->second == 0 && st != 0) opened.push_back(ChestOpen{ o.x, o.y, o.z, now });
                    chestSt[o.id] = st;
                }
            }
        }
        groundReady = true;
    }
    for (auto it = ground.begin(); it != ground.end();) it = static_cast<LONG>(now - it->second) > 30000 ? ground.erase(it) : std::next(it);
    for (auto it = groundAll.begin(); it != groundAll.end();) it = static_cast<LONG>(now - it->second) > 30000 ? groundAll.erase(it) : std::next(it);
    opened.erase(std::remove_if(opened.begin(), opened.end(), [&](const ChestOpen& c) { return static_cast<LONG>(now - c.at) > 5000; }), opened.end());
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Pending& p) { return static_cast<LONG>(now - p.at) > 3000; }), pending.end());
    if (g_cfg.nativeChestRunes && c1 != c2 && !opened.empty()) {
        for (const Obj& o : fresh) {
            if (IsTwinRune(o.id)) continue;
            bool twin = false;
            for (const Pending& p : pending) if (std::abs(p.x - o.x) <= 80 && std::abs(p.y - o.y) <= 80) twin = true;
            if (twin) continue;
            const ChestOpen* from = nullptr;
            for (const ChestOpen& c : opened)
                if (c.z == o.z && double(c.x - o.x) * (c.x - o.x) + double(c.y - o.y) * (c.y - o.y) <= 350.0 * 350.0) { from = &c; break; }
            if (!from) continue;
            DWORD t = 0;
            if (!Rd(o.addr + 0x10, t)) continue;
            const DWORD rc = RuneClass(t);
            const DWORD other = rc == c1 ? c2 : rc == c2 ? c1 : 0;
            char name[64] = "?";
            TypeNameById(t, name, sizeof(name));
            if (!other) { Log("EROI: forziere: runa %s (classe %lu) di nessuno dei due eroi: niente gemella", name, rc); continue; }
            std::vector<DWORD> pool;
            for (DWORD r = 5375; r <= 5782; ++r) if (RuneClass(r) == other) pool.push_back(r);
            if (pool.empty()) { Log("EROI: forziere: nessuna runa della classe %lu", other); continue; }
            static DWORD seed2 = GetTickCount() ^ 0x5a5a;
            seed2 = seed2 * 1103515245u + 12345u;
            const DWORD pick = pool[(seed2 >> 16) % pool.size()];
            RunePos pos{};
            if (!RdBuf(o.addr + 0x18, pos.b, sizeof(pos.b))) continue;
            pending.push_back(Pending{ o.x + 45, o.y, now });
            const DWORD forId = o.id;
            Log("EROI: forziere aperto: runa %s (classe %lu) a terra; all'altro eroe una runa della classe %lu", name, rc, other);
            RunOnGameThread([=] { GiveTwinRuneNow(pick, pos, forId); });
        }
    }
    // 0.56.54 (prova sulla Steam Machine del 07/11, missione "Occhi di lupo": alle 16:10:02 il contadino 664 chiude il dialogo e le
    // due rune della ricompensa, SMOVE_UPGRADE_MULTIHIT_DELF e SMOVE_UPGRADE_DE_C3, compaiono a terra, non nello zaino; raccolte
    // con B alle 16:10:17 la mod le prendeva per bottino, "niente copia per l'eroe 2"; il tester: "credo che in sacred tutte le
    // ricompense vanno a terra"). Un oggetto che compare a terra entro 3 s da un dialogo, a meno di 400 unita' dall'eroe 1 e senza
    // un forziere aperto vicino, e' una ricompensa: per una runa l'eroe 2 riceve una runa a caso della sua classe, per un altro
    // oggetto uno dello stesso tipo (0x5fba80, come le rune), accanto. Niente per l'oro, gli oggetti delle missioni e le copie.
    // Non con una finestra aperta o con lo scambio: un oggetto gettato a terra dallo zaino (anche dal mercante, dove il gioco tiene
    // il dialogo) non e' una ricompensa e non va duplicato.
    if (g_cfg.nativeRewardRunes && !freshAll.empty() && talkAt && static_cast<LONG>(now - talkAt) <= 3000 && MemoryUIState() != 1 &&
        !g_swapOrig.load() && !MemoryMouse2Held()) {
        int hx = 0, hy = 0; BYTE hz = 0;
        Rd(h1 + 0x1c, hx); Rd(h1 + 0x20, hy); Rd(h1 + 0x24, hz);
        for (const Obj& o : freshAll) {
            if (IsTwinRune(o.id) || o.z != hz) continue;
            bool mine = false;
            for (const Pending& p : pending) if (p.at != now && std::abs(p.x - o.x) <= 80 && std::abs(p.y - o.y) <= 80) mine = true;
            if (mine) continue;   // (quelle chieste in questo giro no: due ricompense una accanto all'altra valgono tutte e due)
            if (double(o.x - hx) * (o.x - hx) + double(o.y - hy) * (o.y - hy) > 400.0 * 400.0) continue;
            bool chest = false;
            for (const ChestOpen& c : opened)
                if (c.z == o.z && double(c.x - o.x) * (c.x - o.x) + double(c.y - o.y) * (c.y - o.y) <= 350.0 * 350.0) chest = true;
            if (chest) continue;
            DWORD t = 0;
            if (!Rd(o.addr + 0x10, t) || !t) continue;
            char name[64] = "?";
            TypeNameById(t, name, sizeof(name));
            if (strstr(name, "GOLD") || HasQuestIcon(o.addr)) { Log("EROI: ricompensa a terra %s: oro o oggetto di missione, niente per l'eroe 2", name); continue; }
            DWORD pick = t;
            int kind = 2;
            if (t >= 5375 && t <= 5782) {
                std::vector<DWORD> pool;
                for (DWORD r = 5375; r <= 5782; ++r) if (RuneClass(r) == c2) pool.push_back(r);
                if (pool.empty()) { Log("EROI: ricompensa a terra %s: nessuna runa della classe %lu", name, c2); continue; }
                static DWORD seed3 = GetTickCount() ^ 0x3c3c;
                seed3 = seed3 * 1103515245u + 12345u;
                pick = pool[(seed3 >> 16) % pool.size()];
                kind = 1;
            }
            RunePos pos{};
            if (!RdBuf(o.addr + 0x18, pos.b, sizeof(pos.b))) continue;
            pending.push_back(Pending{ o.x + 45, o.y, now });   // la copia compare li' accanto: non e' un'altra ricompensa
            Log("EROI: ricompensa a terra dopo un dialogo: %s (tipo %lu, oggetto %lu); all'eroe 2 %s", name, t, o.id,
                kind == 1 ? "una runa della sua classe" : "un oggetto dello stesso tipo");
            const DWORD forId = o.id;
            RunOnGameThread([=] { GiveTwinRuneNow(pick, pos, forId, kind); });
        }
    }
    if (!g_cfg.nativeRewardRunes) return;
    const DWORD e1 = inv + heroId * kInvSize;
    WORD gw = 0, gh = 0;
    if (!Rd(e1 + 0x115c, gw) || !Rd(e1 + 0x115e, gh) || !gw || !gh || gw * gh > 400) return;
    std::vector<BYTE> cells(gw * gh * 12u);
    if (!RdBuf(e1 + 0x18, cells.data(), static_cast<DWORD>(cells.size()))) return;
    std::set<DWORD> cur;
    for (size_t k = 0; k < static_cast<size_t>(gw) * gh; ++k) {
        DWORD ref = 0; memcpy(&ref, &cells[k * 12], 4);
        if ((cells[k * 12 + 4] & 1) && ref) cur.insert(ref);
    }
    if (!init) { known = cur; init = true; return; }
    for (DWORD id : cur) {
        if (known.count(id)) continue;
        const DWORD o = ObjectById(id);
        DWORD t = 0;
        if (!o || !Rd(o + 0x10, t) || t < 5375 || t > 5782) continue;
        const DWORD rc = RuneClass(t);
        if (!rc || rc != c1 || c1 == c2) continue;
        char name[64] = "?";
        TypeNameById(t, name, sizeof(name));
        if (ground.count(id)) { Log("EROI: runa %s nello zaino dell'eroe 1: raccolta da terra, niente copia per l'eroe 2", name); continue; }
        if (!talkAt || static_cast<LONG>(now - talkAt) > 8000 || MemoryUIState() == 1 || g_swapOrig.load()) {
            Log("EROI: runa %s nello zaino dell'eroe 1 fuori da un dialogo (o con una finestra aperta): non e' una ricompensa", name);
            continue;
        }
        std::vector<DWORD> mine;
        for (DWORD r = 5375; r <= 5782; ++r) if (RuneClass(r) == c2) mine.push_back(r);
        if (mine.empty()) { Log("EROI: nessuna runa della classe %lu per l'eroe 2", c2); continue; }
        static DWORD seed = GetTickCount();
        seed = seed * 1103515245u + 12345u;
        const DWORD pick = mine[(seed >> 16) % mine.size()];
        Log("EROI: ricompensa per l'eroe 1: runa %s (classe %lu); all'eroe 2 una runa della classe %lu", name, c1, c2);
        RunOnGameThread([=] { GiveRuneToHero2Now(pick); });
    }
    known = cur;
}

// 0.55.13: audio con due eroi. Il gestore dell'audio del gioco (Miles Sound System, oggetto [0x17e8f24]) misura ogni suono dal
// punto di ascolto, il centro della camera che FUN_006108a0 manda col comando 4 (+0xaf2c/+0xaf30, coordinate della camera: pixel
// con la scala a 1). Con il raggio +0xb790 = 1200 e lo zoom +0x84 = 1 il volume e' pieno da vicino, circa meta' a 300 e zero a
// 600 (FUN_006788d0 in 2D, FUN_00678c10 in 3D); i suoni delle creature partono solo entro 600 (FUN_0069b8e0, sulla distanza +0x14
// delle voci registrate, calcolata con FUN_00691990) e le voci oltre 1200 si tolgono. Era pensato per l'eroe sempre al centro:
// con la camera sul punto medio (0.33.0) tutti e due gli eroi stanno a meta' della loro distanza dal punto di ascolto, e
// allontanandosi un po' non si sente piu' niente (prova del 30/10 alle 15:17). Ora la distanza di un suono e' quella dal segmento
// fra i due eroi (punto di ascolto +- h, h = meta' distanza da CameraShift): un suono vicino a uno dei due si sente come vicino
// all'eroe al centro. FUN_00691990 e' sostituita da SndDistHook; in 0x67891e e 0x678c78, dove le due funzioni del volume hanno la
// posizione del suono in ECX/EAX (dopo l'eventuale ricerca della voce), la posizione si avvicina al punto di ascolto nella stessa
// direzione (la panoramica resta). Con un solo eroe o la camera spenta h non c'e' e tutto resta come nel gioco. Tutto nel thread
// del motore (FUN_0069b8e0 la chiama FUN_0060f420 da renderThread::run). [Native] AudioBothHeroes.
std::atomic<bool> g_sndOn{ false };
volatile DWORD g_snd2DCont = 0x00678924, g_snd3DCont = 0x00678c7e;

static bool SndHalf(float& hx, float& hy)
{
    const DWORD at = g_sndAt.load();
    if (!g_sndOn.load() || !at || static_cast<LONG>(GetTickCount() - at) > 500) return false;
    hx = static_cast<float>(g_sndHu.load());
    hy = static_cast<float>(g_sndHw.load());
    return hx != 0.0f || hy != 0.0f;
}

// Distanza dal segmento [-h, +h] di un vettore v dal punto di ascolto (il verso di v non conta: il segmento e' simmetrico).
static float SndSegDist(float vx, float vy, float hx, float hy)
{
    float t = (vx * hx + vy * hy) / (hx * hx + hy * hy);
    if (t > 1.0f) t = 1.0f;
    else if (t < -1.0f) t = -1.0f;
    const float ex = vx - t * hx, ey = vy - t * hy;
    return std::sqrt(ex * ex + ey * ey);
}

// Al posto di FUN_00691990(dx, dy) (cdecl, chiamata solo dal gestore dell'audio con la differenza fra punto di ascolto e voce):
// senza h la stessa approssimazione della lunghezza del gioco.
unsigned __cdecl SndDistHook(int dx, int dy)
{
    float hx = 0, hy = 0;
    if (SndHalf(hx, hy)) return static_cast<unsigned>(SndSegDist(static_cast<float>(dx), static_cast<float>(dy), hx, hy) + 0.5f);
    unsigned a = static_cast<unsigned>(dx < 0 ? -dx : dx), b = static_cast<unsigned>(dy < 0 ? -dy : dy);
    if (static_cast<int>(a) < static_cast<int>(b)) std::swap(a, b);
    unsigned r = a * 0x3ef + b * 0x1b9;
    if (a < (b << 4)) r -= a * 0x28;
    return (r + 0x200) >> 10;
}

// Dagli stub di 0x67891e e 0x678c78: xy[0], xy[1] = posizione del suono (coordinate della camera), sm = gestore dell'audio.
void __cdecl SndAdjustPos(DWORD sm, int* xy)
{
    float hx = 0, hy = 0;
    if (!SndHalf(hx, hy)) return;
    // lettura diretta: il gioco sta usando proprio questo oggetto (Rd costerebbe due ReadProcessMemory per ogni suono)
    const int lx = *reinterpret_cast<const int*>(static_cast<uintptr_t>(sm + 0xaf2c));
    const int ly = *reinterpret_cast<const int*>(static_cast<uintptr_t>(sm + 0xaf30));
    const float vx = static_cast<float>(xy[0] - lx), vy = static_cast<float>(xy[1] - ly), d = std::sqrt(vx * vx + vy * vy);
    if (d < 1.0f) return;
    const float k = SndSegDist(vx, vy, hx, hy) / d;
    xy[0] = lx + static_cast<int>(std::lround(vx * k));
    xy[1] = ly + static_cast<int>(std::lround(vy * k));
}

__declspec(naked) void Snd2DStub()   // al posto di "fld qword ptr [esi+0xb790]" in 0x67891e: ESI = gestore, ECX = x, EAX = y
{
    __asm {
        push eax
        push ecx
        mov  eax, esp
        pushad
        pushfd
        push eax
        push esi
        call SndAdjustPos
        add  esp, 8
        popfd
        popad
        pop  ecx
        pop  eax
        fld  qword ptr [esi + 0xb790]
        jmp  dword ptr [g_snd2DCont]
    }
}

__declspec(naked) void Snd3DStub()   // al posto di "fld qword ptr [edi+0xb790]" in 0x678c78: EDI = gestore, ECX = x, EAX = y
{
    __asm {
        push eax
        push ecx
        mov  eax, esp
        pushad
        pushfd
        push eax
        push edi
        call SndAdjustPos
        add  esp, 8
        popfd
        popad
        pop  ecx
        pop  eax
        fld  qword ptr [edi + 0xb790]
        jmp  dword ptr [g_snd3DCont]
    }
}

// Nel thread del motore; i 6 byte di ogni punto si controllano tutti prima di cambiarne uno (Sacred.exe 2.28).
void InstallAudioHooksNow()
{
    struct Site { DWORD at; BYTE orig[6]; DWORD to; };
    const Site sites[3] = {
        { 0x00691990, { 0x8B, 0x54, 0x24, 0x04, 0x85, 0xD2 }, reinterpret_cast<DWORD>(&SndDistHook) },
        { 0x0067891E, { 0xDD, 0x86, 0x90, 0xB7, 0x00, 0x00 }, reinterpret_cast<DWORD>(&Snd2DStub) },
        { 0x00678C78, { 0xDD, 0x87, 0x90, 0xB7, 0x00, 0x00 }, reinterpret_cast<DWORD>(&Snd3DStub) },
    };
    for (const Site& st : sites) {
        BYTE cur[6] = {};
        for (int i = 0; i < 6; ++i) {
            if (!Rd(st.at + i, cur[i])) { Log("AUDIO: ganci NON installati: %08lX non leggibile", st.at); return; }
        }
        if (memcmp(cur, st.orig, sizeof(cur)) != 0) {
            Log("AUDIO: ganci NON installati: in %08lX byte diversi (%02X %02X %02X %02X %02X %02X), audio come nel gioco",
                st.at, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]);
            return;
        }
    }
    for (const Site& st : sites) {
        BYTE jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
        const LONG rel = static_cast<LONG>(st.to - (st.at + 5));
        memcpy(jmp + 1, &rel, 4);
        if (!PatchCode(st.at, jmp, sizeof(jmp))) { Log("AUDIO: scrittura in %08lX non riuscita", st.at); return; }
    }
    g_sndOn.store(true);
    Log("AUDIO: ganci installati (0x691990, 0x67891e, 0x678c78): con due eroi i suoni si misurano dal segmento fra gli eroi");
}

// Traccia dell'audio (dal thread del motore): lo stato che blocca i suoni appena cambia ([0x17e8f20] spento, [0xa1bd18]
// driver, +0x11, +0x89, +0x64 bit 1, +0x69c, +0x80 3D: le uscite anticipate di FUN_00693680) e ogni 15 s con due eroi il
// punto di ascolto, h e le voci registrate. Per "a volte ritornandogli vicino non sentivo comunque l'audio" (30/10, 15:17).
void AudioTraceTick()
{
    if (!g_sndOn.load()) return;
    static DWORD next = 0, lastKey = 0xFFFFFFFF, checkAt = 0;
    if (static_cast<LONG>(GetTickCount() - checkAt) < 250) return;   // 0.55.18: 4 volte al secondo bastano (8 letture ogni volta)
    checkAt = GetTickCount();
    DWORD sm = 0;
    if (!Rd(0x017e8f24, sm) || !sm) return;
    BYTE off = 0, drv = 0, b11 = 0, b89 = 0, b64 = 0, b80 = 0;
    WORD w69c = 0;
    Rd(0x017e8f20, off); Rd(0x00a1bd18, drv); Rd(sm + 0x11, b11); Rd(sm + 0x89, b89); Rd(sm + 0x64, b64); Rd(sm + 0x69c, w69c); Rd(sm + 0x80, b80);
    const DWORD key = (off ? 1u : 0u) | (drv ? 2u : 0u) | (b11 ? 4u : 0u) | (b89 ? 8u : 0u) | ((b64 & 2) ? 16u : 0u) |
                      (w69c == 1 ? 32u : 0u) | (b80 ? 64u : 0u);
    if (key != lastKey) {
        lastKey = key;
        Log("AUDIO: stato: spento %u, driver %u, +0x11 %u, +0x89 %u, +0x64 bit 1 %u, +0x69c %u, 3D %u", off, drv, b11, b89, (b64 >> 1) & 1, w69c, b80);
    }
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - next) < 0) return;
    next = now + 15000;
    float hx = 0, hy = 0;
    if (!SndHalf(hx, hy)) return;
    int lx = 0, ly = 0, n1 = 0, n2 = 0, n3 = 0;
    float zoom = 0;
    Rd(sm + 0xaf2c, lx); Rd(sm + 0xaf30, ly); Rd(sm + 0xaf34, n1); Rd(sm + 0xaf38, n2); Rd(sm + 0xaf3c, n3); Rd(sm + 0x84, zoom);
    Log("AUDIO: ascolto (%d,%d), meta' distanza fra gli eroi %.0f, zoom %.2f, voci %d/%d/%d", lx, ly, std::sqrt(hx * hx + hy * hy), zoom, n1, n2, n3);
}

// 0.51.3: richieste del pad 2 a Core.cpp: aiuto (Start tenuto) e, mentre l'aiuto e' suo, Core.cpp usa lo stato del pad 2.
std::atomic<int> g_pad2HelpReq{ 0 };
std::atomic<bool> g_pad2InHelp{ false };
bool NativePad2TakeHelpReq() { return g_pad2HelpReq.exchange(0) > 0; }
void NativePad2SetInHelp(bool on) { g_pad2InHelp.store(on); }

// Pad del giocatore 2 (0.44.0, N3 della sezione 8c). L'eroe 2 (oggetto 2) lo comanda il gestore del gioco FUN_006108a0
// con le frecce (ordini di tipo 4 modo 2 a +-200 unita' in x e/o y; senza frecce, ordine di fermata a ogni fotogramma),
// Y (attacca il nemico piu' vicino), X (arte scelta) e Ctrl+1 / Ctrl+2 (scelta dell'arte), letti con GetAsyncKeyState,
// che la DLL aggancia: il pad 2 li "preme" con g_virt.keyUntil. I tasti hanno una scadenza di 150 ms rinnovata a ogni
// giro, cosi' non restano mai premuti se la mod smette di chiamare questa funzione (finestra non in primo piano, uscita).
// Direzione: lo stick e' in coordinate dello schermo, le frecce in coordinate del mondo. Per ognuna delle 8 combinazioni
// si usa la direzione sullo schermo IMPARATA guardando come si sposta davvero l'eroe 2 quando e' premuta; finche' non
// e' imparata si usa quella prevista (spostamenti nel mondo di FUN_006108a0, proiettati con WorldToScreen): se la
// previsione e' sbagliata l'eroe va per un attimo nella direzione sbagliata e la mod si corregge da sola.
void NativePad2Tick(DWORD mgr, bool inWorld, DWORD now)
{
    struct Combo { unsigned keys; int wx, wy; };
    // spostamenti nel mondo letti in FUN_006108a0 (docs/ghidra_morte.txt): le frecce sono gia' "da schermo" nella vista
    // isometrica (giu' = +x +y, su = -x -y, destra = +x -y, sinistra = -x +y; le diagonali muovono un asse solo)
    static const Combo kCombo[8] = { { 1, -1, 1 }, { 4, 1, -1 }, { 2, -1, -1 }, { 8, 1, 1 }, { 3, -1, 0 }, { 9, 0, 1 }, { 6, 0, -1 }, { 12, 1, 0 } };
    static const int kArrowVk[4] = { VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN };
    static float learnX[8] = {}, learnY[8] = {};
    static bool learned[8] = {};
    static int curCombo = -1, lastIdx = -2, comboX = 0, comboY = 0;
    static DWORD comboAt = 0;
    static WORD prevBtn = 0, prevRel = 0;
    static DWORD bSince = 0, startSince = 0;       // 0.51.3: B e Start tenuti
    static DWORD zSince[2] = {}, zNext[2] = {};    // 0.55.31: croce su (0) e giu' (1) tenute: zoom
    static bool zDone[2] = {};
    static bool bLong = false, startLong = false;
    auto press = [&](int vk, DWORD ms) {      // non accorcia mai un tasto premuto piu' a lungo dal pad 1 (tenuto o a scadenza)
        const DWORD cur = g_virt.keyUntil[vk].load(), until = now + ms;
        if (cur == VirtualInput::kHeld) return;
        if (cur != 0 && static_cast<LONG>(cur - until) >= 0) return;
        g_virt.keyUntil[vk].store(until);
    };
    constexpr DWORD kHoldMs = 600;                  // come lo Start e il B tenuti del pad 1
    // Start: breve = menu del gioco (Esc), al rilascio; tenuto = aiuto con il controller del giocatore 2. 0.55.27: anche con il
    // menu aperto (lo chiude), quindi in una lambda; edge e b sono quelli del giro, dichiarati piu' avanti.
    WORD edge = 0, b = 0;
    // 0.56.2: View come quello del pad 1 (il tester, 03/11: "esattamente uguale al pad 1, quindi se cursore tenuto e' lo zaino"):
    // premuto e lasciato = modalita' cursore on/off, tenuto ViewHoldMs = il suo zaino (o lo scambio dell'interfaccia con mercante e
    // altri pannelli). Senza il secondo mouse come prima: alla pressione lo zaino (o lo scambio).
    static DWORD viewSince = 0;
    static bool viewLong = false;
    auto viewButton = [&](WORD released) {
        if (!MemoryMouse2Ready()) {
            viewSince = 0;
            if (edge & PAD_BACK) {
                if (MemoryDualPossible()) g_pad2SideReq.store(true);
                else g_pad2SwapReq.fetch_add(1);
            }
            return;
        }
        if (edge & PAD_BACK) { viewSince = now ? now : 1; viewLong = false; }
        const DWORD hold = static_cast<DWORD>(g_cfg.viewHoldMs > 150 ? g_cfg.viewHoldMs : 150);
        if ((b & PAD_BACK) && viewSince && !viewLong && now - viewSince >= hold) {
            viewLong = true;
            g_p2Cur.store(false);
            const bool dual = MemoryDualPossible();
            if (dual) g_pad2SideReq.store(true);
            else g_pad2SwapReq.fetch_add(1);
            Log("PAD2: View tenuto: %s", dual ? "il suo zaino" : "interfaccia dell'eroe 2");
        }
        if ((released & PAD_BACK) && viewSince) {
            if (!viewLong) {
                const bool on = !g_p2Cur.load();
                g_p2Cur.store(on);
                g_p2cAPress.store(0); g_p2cBPress.store(0); g_p2cAHeld.store(false);
                Log("PAD2: modalita' %s (View)", on ? "cursore" : "gioco");
                OverlayCoopToast(Tr(on ? "Giocatore 2: modalita' cursore" : "Giocatore 2: modalita' gioco"), 1500);
            }
            viewSince = 0;
        }
    };
    auto startButton = [&](WORD released) {
        if (edge & PAD_START) { startSince = now; startLong = false; }
        if ((b & PAD_START) && startSince && !startLong && now - startSince >= kHoldMs) { startLong = true; g_pad2HelpReq.fetch_add(1); }
        if ((released & PAD_START) && startSince) {
            if (!startLong && g_gameHwnd.load()) {
                press(VK_ESCAPE, 100);                  // anche per GetAsyncKeyState (lo scambio si chiude con Esc)
                const LPARAM lp = 1 | (0x01 << 16);
                PostMessageA(g_gameHwnd.load(), WM_KEYDOWN, VK_ESCAPE, lp);
                PostMessageA(g_gameHwnd.load(), WM_KEYUP, VK_ESCAPE, lp | (1 << 30) | (1u << 31));
                Log("PAD2: menu (Esc)");
            }
            startSince = 0;
        }
    };
    const DWORD h2 = (inWorld && g_cfg.nativePad2 && mgr) ? ObjectAddr(mgr, 2) : 0;
    {   // 0.55.4: con l'eroe 2 in partita anche l'icona del bersaglio del pad 1 ha il suo numero
        static bool two = false;
        const bool nowTwo = inWorld && mgr && ObjectAddr(mgr, 2) != 0;
        if (nowTwo != two) { two = nowTwo; OverlaySetTwoPlayers(two); }
    }
    PadState p;
    int idx = -1;
    // 0.55.2: scambio dell'interfaccia partito da solo per un PNG con un servizio (vedi sotto): torna all'eroe 1 quando la sua
    // finestra si chiude, o dopo 8 s se non si e' mai aperta; se il puntatore non arriva al pad 2 (scambio rifiutato) si lascia.
    static int autoSwap = 0;                        // 0 nessuno, 1 chiesto, 2 attivo, 3 finestra vista
    static DWORD autoAt = 0;
    if (autoSwap) {
        if (NativePad2HasPointer()) {
            const int win = MemoryUIState();
            if (autoSwap == 1) autoSwap = 2;
            if (win == 1) autoSwap = 3;
            else if (autoSwap == 3 || static_cast<int>(now - autoAt) > 8000) {
                g_pad2SwapReq.fetch_add(1);
                Log("PAD2: %s: interfaccia di nuovo all'eroe 1", autoSwap == 3 ? "finestra del servizio chiusa" : "nessuna finestra in 8 s");
                autoSwap = 0;
                g_pad2AutoSwap.store(false);
            }
        } else if (autoSwap >= 2 || static_cast<int>(now - autoAt) > 3000) {   // tornato con Back, o mai partito
            autoSwap = 0;
            g_pad2AutoSwap.store(false);
        }
    }
    // 0.56.46 (prova del 05/11 alle 12:35:30, deposito 210 con l'eroe 2 gia' accanto): l'ordine "vai e usa" partiva insieme alla
    // richiesta dello scambio; il deposito si apriva (12:35:30.635) mentre lo scambio richiudeva e riapriva lo zaino col tasto I, e
    // la chiusura sembrava quella del servizio: interfaccia subito di nuovo all'eroe 1 (12:35:30.733). Ora con lo scambio automatico
    // l'ordine parte solo quando il puntatore e' passato al pad 2 (o dopo 800 ms).
    static H2Target pendT{};
    static DWORD pendAt = 0;
    static bool pendOn = false;
    if (pendOn && (NativePad2HasPointer() || static_cast<int>(now - pendAt) > 800 || !autoSwap)) {
        pendOn = false;
        Log("PAD2: %s %lu: ordine dopo lo scambio dell'interfaccia (%lu ms)", pendT.kind, pendT.id, static_cast<unsigned long>(now - pendAt));
        Hero2Interact(pendT);
        g_h2Act = H2Act{ pendT.id, now + 6000, now + 500, 0 };
    }
    static bool markOn = false;                     // 0.55.2: indicatore del bersaglio di A sull'eroe 2
    auto hideMark = [&]() { if (markOn) { markOn = false; OverlaySetMarker2(false, 0, 0); } OverlaySetMarkerGround(2, false, 0, 0); };
    auto stopAtk = []() { g_h2AtkId.store(0); };    // 0.55.8: niente piu' attacco a ogni fotogramma
    if (!h2) {                                      // i tasti scadono da soli
        curCombo = -1; prevBtn = 0; hideMark(); stopAtk();
        if (lastIdx >= 0 && lastIdx <= 3) PadVibrate2(lastIdx, 0.0f, 0.0f);
        return;
    }
    if (!PadRead2(p, idx)) {
        curCombo = -1; prevBtn = 0; hideMark(); stopAtk();
        if (lastIdx >= 0) {                         // 0.55.29: avviso, come per il collegamento (prima nessuna traccia)
            if (lastIdx <= 3) PadVibrate2(lastIdx, 0.0f, 0.0f);
            Log("PAD2: controller del giocatore 2 non piu' letto (era %s %d): l'eroe 2 resta fermo finche' non torna", lastIdx == kPad2KeyboardIndex ? "la tastiera" : "XInput", lastIdx);
            OverlayCoopToast(Tr("Giocatore 2: controller scollegato"), 3000);
            lastIdx = -2;
        }
        return;
    }
    if (idx != lastIdx) {
        lastIdx = idx;
        if (idx == kPad2KeyboardIndex) Log("PAD2: controller del giocatore 2 = tastiera (lettere, Pad2Keyboard)");
        else Log("PAD2: controller del giocatore 2 = XInput %d", idx);
        OverlayCoopToast(Tr("Giocatore 2: controller collegato"), 2500);
    }
    {   // 0.55.29: vibrazione del pad 2 con la vita dell'eroe 2 sotto il 25%, come il pad 1 con l'eroe 1 (VibrationTick in Core.cpp):
        // un impulso a ogni lampeggio del gioco ((sin(t * 4) + 1) / 2 con t = [0xcd69d8]), ferma se il tempo del gioco non avanza
        // (pausa, menu) o con la vibrazione spenta nell'aiuto. Prima il pad 2 non vibrava mai.
        static bool low = false;
        static double lastT = 0.0;
        static DWORD tMovedAt = 0;
        int hp = 0, maxHp = 0;
        double t = 0.0;
        const bool ok = Rd(h2 + 0x4d8, hp) && Rd(h2 + 0x4d4, maxHp) && maxHp > 0 && Rd(0x00cd69d8, t);
        if (ok && t != lastT) { lastT = t; tMovedAt = now; }
        const float frac = ok ? static_cast<float>(hp) / static_cast<float>(maxHp) : 1.0f;
        const bool nowLow = ok && frac > 0.0f && frac < 0.25f;
        if (nowLow != low) {
            low = nowLow;
            Log("PAD2: vita dell'eroe 2 %s il 25%% (%.0f%%): vibrazione del pad 2 %s", low ? "sotto" : "sopra", frac * 100.0f,
                !low ? "ferma" : g_cfg.vibration ? "a ogni lampeggio" : "disattivata nell'aiuto");
        }
        const float power = g_cfg.vibrationPower / 100.0f;
        const bool pulse = g_cfg.vibration && low && now - tMovedAt < 300 && (std::sin(t * 4.0) + 1.0) * 0.5 > 0.85;
        if (idx >= 0 && idx <= 3) PadVibrate2(idx, pulse ? power : 0.0f, pulse ? power * 0.5f : 0.0f);
    }
    WORD bt = p.buttons;                            // 0.51.2: grilletti come pulsanti (pozioni)
    if (p.lt > 0.5f) bt |= PAD_LT;
    if (p.rt > 0.5f) bt |= PAD_RT;
    b = bt;
    edge = static_cast<WORD>(b & ~prevBtn);
    prevBtn = b;
    // 0.50.0 (N4): con il puntatore al pad 2 stick, croce e pulsanti li usa Core.cpp per l'interfaccia; qui resta solo Back,
    // che chiude e torna all'eroe 1. Niente frecce ne' azioni, altrimenti l'eroe 2 camminerebbe mentre si usa lo zaino.
    // 0.55.53 (controllo): lo stato del Back si azzera quando il puntatore non e' del pad 2. Prima, perso il puntatore con Back
    // ancora premuto (Esc, eroe 2 a terra), al primo giro con il puntatore di nuovo suo il rilascio vecchio lo restituiva subito.
    static DWORD backAt = 0; static bool backLong = false;
    if (!NativePad2HasPointer()) { backAt = 0; backLong = false; }
    // 0.55.54: lato dell'eroe 2 aperto con i due zaini: stick, croce e pulsanti sono del suo puntatore (Core.cpp, Pad2Pointer);
    // l'eroe 2 sta fermo. Back (premuto o tenuto) chiude il suo lato.
    if (MemoryDualSide(2) && MemoryDualActive() && !NativePad2HasPointer()) {
        curCombo = -1;
        hideMark();
        stopAtk();
        zSince[0] = zSince[1] = 0;
        g_p2Cur.store(false);                       // 0.56.2: chiuso lo zaino si torna in modalita' gioco, come il pad 1
        viewSince = 0;
        if (edge & PAD_BACK) g_pad2SideReq.store(true);
        return;
    }
    if (NativePad2HasPointer()) {
        curCombo = -1;
        hideMark();
        stopAtk();
        zSince[0] = zSince[1] = 0;                  // 0.55.31: croce premuta qui: niente pozione al rilascio in gioco
        // 0.55.50: come View del pad 1 (il tester, 01/11: "il pg 1 entra ed esce dall'inventario tenendo premuto il tasto, il pg 2
        // prima lo deve tenere premuto e poi per chiuderlo basta che lo preme una volta"): Back tenuto apre e chiude lo zaino
        // (il puntatore resta; a finestre chiuse torna da solo al pad 1, 0.55.49), Back premuto e lasciato restituisce il
        // puntatore. Prima Back alla pressione tornava subito all'eroe 1 e chiudeva lo zaino.
        if (edge & PAD_BACK) { backAt = now ? now : 1; backLong = false; }
        if (backAt && !backLong && (b & PAD_BACK) && now - backAt >= static_cast<DWORD>(g_cfg.viewHoldMs > 150 ? g_cfg.viewHoldMs : 150)) {
            backLong = true;
            g_pad2InvToggleReq.store(true);
        }
        if (backAt && !(b & PAD_BACK)) {
            if (!backLong) g_pad2SwapReq.fetch_add(1);
            backAt = 0;
        }
        return;
    }
    if (g_pad2InHelp.load()) { curCombo = -1; hideMark(); stopAtk(); zSince[0] = zSince[1] = 0; return; }   // 0.51.3: l'aiuto del pad 2 lo guida Core.cpp
    // 0.55.27: menu del gioco aperto (Esc, opzioni, salvataggi; partita in pausa): niente azioni dell'eroe 2. Prova del 31/10
    // sulla Steam Machine: Start del pad 2 apriva il menu e la sua croce, usata per scorrere le voci, beveva le pozioni dell'eroe
    // 2 e cambiava arma. Croce, A, B, Y e stick li passa al menu Core.cpp insieme a quelli del pad 1; qui resta Start, che lo
    // chiude come l'ha aperto. B tenuto prima del menu non raccoglie piu' al rilascio.
    if (MemoryGameMenuOpen()) {
        curCombo = -1; hideMark(); stopAtk();
        bSince = 0;
        zSince[0] = zSince[1] = 0;                  // 0.55.31: niente pozione al rilascio dopo il menu
        const WORD rel = static_cast<WORD>(prevRel & ~b);
        prevRel = b;
        startButton(rel);
        return;
    }
    g_pad2Btn.store(b);                             // 0.55.16: R3 e L3 li usa Core.cpp (Alt e minimappa)
    g_pad2BtnAt.store(now);
    // movimento: stick sinistro (dalla 0.51.3 la croce fa le stesse cose del pad 1: pozioni e armi)
    float sx = p.lx, sy = p.ly;
    const float mag = std::sqrt(sx * sx + sy * sy);
    int x2 = 0, y2 = 0; Rd(h2 + 0x1c, x2); Rd(h2 + 0x20, y2);
    int want = -1;
    View vw;
    // combinazione di frecce piu' vicina a una direzione sullo schermo (imparata, o prevista con WorldToScreen)
    auto bestCombo = [&](const View& v, float dx, float dy) {
        int c0 = -1;
        float best = -2.0f;
        for (int c = 0; c < 8; ++c) {
            float ex = learnX[c], ey = learnY[c];
            if (!learned[c]) {
                float ax = 0, ay = 0, bx = 0, by = 0;
                if (!WorldToScreen(v, x2, y2, ax, ay) || !WorldToScreen(v, x2 + kCombo[c].wx * 200, y2 + kCombo[c].wy * 200, bx, by)) continue;
                ex = bx - ax; ey = by - ay;
                const float n = std::sqrt(ex * ex + ey * ey);
                if (n < 1e-3f) continue;
                ex /= n; ey /= n;
            }
            const float dot = ex * dx + ey * dy;
            if (dot > best) { best = dot; c0 = c; }
        }
        return c0;
    };
    // 0.56.2: modalita' cursore del giocatore 2, come quella del pad 1: lo stick muove il suo cursore (Core.cpp, Pad2PointerM2) e
    // l'eroe 2 sta fermo; A e B sono i clic sinistro e destro del gioco, tradotti in azioni dell'eroe 2 (i clic veri nel mondo
    // comanderebbero l'eroe 1): A su un nemico (quello sotto il cursore, o il piu' vicino al cursore) lo attacca finche' e' tenuto,
    // A su un personaggio, un forziere, una porta o un oggetto fisso lo usa (come A in modalita' gioco), A sul terreno fa camminare
    // l'eroe 2 verso il cursore finche' e' tenuto (con le frecce, come lo stick: un ordine "vai al punto" il gestore del
    // giocatore 2 lo ferma subito); B lancia l'arte scelta nella sua barra sul nemico sotto il cursore (o sul punto). Con un
    // dialogo aperto A e B vanno al dialogo. View premuto torna in modalita' gioco, tenuto apre il suo zaino; Start = menu.
    if (g_p2Cur.load()) {
        if (!MemoryMouse2Ready()) { g_p2Cur.store(false); Log("PAD2: modalita' cursore chiusa: secondo mouse non attivo"); }
        else {
            curCombo = -1; hideMark();
            zSince[0] = zSince[1] = 0; bSince = 0;
            const WORD rel = static_cast<WORD>(prevRel & ~b);
            prevRel = b;
            static DWORD cAtk = 0;
            static bool cWalk = false;
            BYTE cz = 0; Rd(h2 + 0x24, cz);
            View cv{};
            const bool haveV = ReadView(cv) && cv.h > 0;
            const float vh = haveV ? static_cast<float>(cv.h) : 1080.0f;
            const LONG px = g_m2UiX.load(), py = g_m2UiY.load();
            int gx = x2, gy = y2;
            const bool haveG = haveV && ScreenToWorldNear(cv, x2, y2, static_cast<float>(px), static_cast<float>(py), gx, gy);
            auto hostileAt = [&](DWORD& id) {
                const DWORD hov = MemoryMouse2Hover();
                const DWORD a = hov ? ObjectById(hov) : 0;
                int cls = 0; bool dead = false; BYTE ez = 0;
                if (a && ReadKind(a, cls, dead) && !dead && InMask(g_cfg.hostileMask, cls) && Rd(a + 0x24, ez) && ez == cz) { id = hov; return true; }
                Target tg;
                if (haveV && MemoryFindTarget(false, static_cast<float>(px), static_cast<float>(py), 0, 0, 0.05f * vh, 0.05f * vh, tg,
                                              nullptr, 0, 0, false, true)) { id = tg.id; return true; }
                return false;
            };
            WORD cMode = 0; Rd(h2 + 0xfc, cMode);
            DialogInfo cd;
            const bool talk = cMode == 10 || MemoryDialog(cd);
            if (talk) g_h2TalkAt.store(now ? now : 1);
            const int aPress = g_p2cAPress.exchange(0), bPress = g_p2cBPress.exchange(0);
            if ((aPress || bPress) && talk) {                 // dialogo aperto: come A e B del pad 2 in modalita' gioco
                g_pad2DlgAt.store(now);
                g_pad2DlgBtn.store(aPress ? PAD_A : PAD_B);
                cAtk = 0; cWalk = false;
            }
            else if ((aPress || bPress) && MemoryMouse2OverUi()) {   // sopra una finestra del gioco (per esempio dell'eroe 1)
                cAtk = 0; cWalk = false;
                Log("PAD2: cursore: clic in (%ld,%ld) sopra una finestra del gioco, ignorato", px, py);
            }
            else if (aPress) {
                cAtk = 0; cWalk = false;
                DWORD id = 0;
                const float org[2] = { static_cast<float>(px), static_cast<float>(py) };
                H2Target t;
                if (MemoryMouse2Held()) {                     // 0.56.6: come il pad 1, l'oggetto in mano si getta a terra
                    MemoryMouse2Drop(px, py);
                    Log("PAD2: cursore: A nel mondo con un oggetto in mano in (%ld,%ld): lo getta", px, py);
                }
                else if (hostileAt(id)) { cAtk = id; Log("PAD2: cursore: A sul nemico %lu in (%ld,%ld): attacco", id, px, py); }
                else if (Hero2FindInteract(h2, 0.0f, 0.0f, false, t, org)) {
                    Log("PAD2: cursore: A su %s %s id %lu in (%ld,%ld)", t.kind, t.name, t.id, px, py);
                    // 0.56.9 mercante, 0.56.14 anche fabbro e Maestro
                    const bool shopC = t.sharedWin >= 0 && MemoryMouse2Ready() && g_cfg.nativeTwoInventories;
                    if (shopC && MemoryWindowOpen(t.sharedWin)) {
                        g_pad2SideReq.store(true);
                        Log("PAD2: cursore: A su %s %lu gia' aperto: il lato del giocatore 2", t.kind, t.id);
                    }
                    else if (t.smith && MemoryWindowOpen(3)) {      // 0.56.14: fabbro uno alla volta
                        OverlayCoopToast(Tr("Il fabbro lo sta usando l'altro giocatore: uno alla volta"), 2500);
                        Log("PAD2: cursore: A sul fabbro %lu gia' aperto: uno alla volta", t.id);
                    }
                    else if (t.door) Hero2OpenDoor(t.id);
                    else {
                        if (shopC) { g_p2ShopWin.store(t.sharedWin); g_p2MerchAt.store(now | 1); }
                        if (t.service && !shopC && !autoSwap && !g_swapOrig.load() && g_cfg.nativeSwap && g_cfg.nativeSwapUi && g_uiPatched.load()) {
                            g_pad2AutoSwap.store(true);
                            g_pad2SwapReq.fetch_add(1);
                            autoSwap = 1; autoAt = now;
                            pendT = t; pendAt = now; pendOn = true;   // 0.56.46: l'ordine dopo lo scambio
                            Log("PAD2: %s con un servizio (id %lu): interfaccia all'eroe 2 prima dell'arrivo", t.kind, t.id);
                        }
                        else Hero2Interact(t);
                        g_h2Act = H2Act{ t.id, now + 6000, now + 500, 0 };
                    }
                }
                else if (const DWORD it = Pad2ItemUnderCursor()) {   // 0.56.6: oggetto a terra: l'eroe 2 va e lo raccoglie (tipo 7)
                    const DWORD o = ObjectById(it);
                    H2Target ti{};
                    ti.id = it; ti.kind = "oggetto a terra";
                    if (o) { Rd(o + 0x1c, ti.wx); Rd(o + 0x20, ti.wy); }
                    Hero2Interact(ti);
                    g_h2Act = H2Act{ it, now + 6000, now + 500, 0 };
                    Log("PAD2: cursore: A sull'oggetto a terra %lu in (%ld,%ld): l'eroe 2 lo raccoglie", it, px, py);
                }
                else if (haveG) { cWalk = true; Log("PAD2: cursore: A sul terreno in (%ld,%ld): l'eroe 2 va verso (%d,%d)", px, py, gx, gy); }
            }
            if (!g_p2cAHeld.load()) { cAtk = 0; cWalk = false; }
            if (cAtk) {
                const DWORD e = ObjectById(cAtk);
                int cls = 0, ex = 0, ey = 0; bool dead = false;
                if (!e || !ReadKind(e, cls, dead) || dead || !Rd(e + 0x1c, ex) || !Rd(e + 0x20, ey)) { cAtk = 0; stopAtk(); }
                else {
                    const double d = std::sqrt(double(ex - x2) * (ex - x2) + double(ey - y2) * (ey - y2));
                    if (!g_p2UseHookOn.load() && d > 220.0 && haveV) {   // senza il gancio della fermata: avvicinamento con le frecce
                        stopAtk();
                        float ax = 0, ay = 0, bx = 0, by = 0;
                        if (WorldToScreen(cv, x2, y2, ax, ay) && WorldToScreen(cv, ex, ey, bx, by)) {
                            float tdx = bx - ax, tdy = by - ay;
                            const float dn = std::sqrt(tdx * tdx + tdy * tdy);
                            if (dn > 1e-3f) { tdx /= dn; tdy /= dn; }
                            const int bc = bestCombo(cv, tdx, tdy);
                            if (bc >= 0) for (int k = 0; k < 4; ++k) if (kCombo[bc].keys & (1u << k)) press(kArrowVk[k], 150);
                        }
                    }
                    else { g_h2AtkUntil.store(now + 300); g_h2AtkId.store(cAtk); }
                }
            }
            else stopAtk();
            if (cWalk && haveV) {                             // verso il cursore, che si puo' spostare mentre A e' tenuto
                float ax = 0, ay = 0;
                if (WorldToScreen(cv, x2, y2, ax, ay)) {
                    float tdx = static_cast<float>(px) - ax, tdy = static_cast<float>(py) - (ay - 0.04f * vh);
                    const float dn = std::sqrt(tdx * tdx + tdy * tdy);
                    if (dn > 0.03f * vh) {
                        tdx /= dn; tdy /= dn;
                        const int bc = bestCombo(cv, tdx, tdy);
                        if (bc >= 0) for (int k = 0; k < 4; ++k) if (kCombo[bc].keys & (1u << k)) press(kArrowVk[k], 150);
                    }
                }
            }
            if (bPress && !talk && !MemoryMouse2OverUi()) {   // clic destro: l'arte scelta nella sua barra
                DWORD id = 0, inv = 0;
                hostileAt(id);
                WORD sel = 0;
                if (Rd(kInvTable, inv) && inv) Rd(inv + 2 * kInvSize + 0xd5a, sel);
                const int slot = sel <= 4 ? sel : 0;
                UseArtHero2(slot, id, gx, gy);
                Log("PAD2: cursore: B in (%ld,%ld): arte dello slot %d %s", px, py, slot + 1, id ? "sul nemico" : "sul punto");
            }
            viewButton(rel);
            startButton(rel);
            return;
        }
    }
    // 0.55.12: scarto laterale quando l'eroe 2 e' bloccato. Con le frecce il gestore del giocatore 2 lo manda verso un punto a
    // 200 unita' in linea retta, senza aggirare niente: contro casse, barili, cadaveri o l'eroe 1 resta fermo (prova delle 14:58:
    // "si muove e si blocca in modo strano quando ci sono cose attorno"; attacco al nemico 1292 fermo a 152 unita'). Come il
    // movimento bloccato del pad 1: se in 500 ms l'eroe 2 si e' spostato meno di 20 unita' pur dovendo muoversi, per 400 ms va
    // a 45 gradi da un lato, poi dall'altro, poi a 90 gradi. Lo usano lo stick e l'attacco (avvicinamento e inseguimento).
    // Il passo fatto durante uno scarto non conta come "si muove da solo": i tentativi vanno avanti (45 gradi da un lato, poi
    // dall'altro, poi 90) finche' l'eroe 2 non avanza nella sua direzione. Con [Sticks] Unstick=false niente scarti, come il pad 1.
    static DWORD stCheckAt = 0, stSideUntil = 0;
    static int stX = 0, stY = 0, stTry = 0;
    static float stAng = 0;
    static bool stSideWin = false;
    static DWORD stLastCall = 0;
    bool stUsed = false;
    auto stuckSide = [&](bool intent, float& dx, float& dy) {
        stUsed = true;
        if (static_cast<int>(now - stLastCall) > 250) { stCheckAt = 0; stSideUntil = 0; stTry = 0; stSideWin = false; }   // dopo una pausa (finestra, aiuto) si riparte
        stLastCall = now;
        DWORD hpS = 0;
        if (!intent || !g_cfg.unstick || !Rd(h2 + 0x4d8, hpS) || hpS == 0) { stCheckAt = 0; stSideUntil = 0; stTry = 0; stSideWin = false; return false; }
        if (!stCheckAt) { stCheckAt = now + 500; stX = x2; stY = y2; }
        else if (static_cast<int>(now - stCheckAt) >= 0) {
            const int moved = std::abs(x2 - stX) + std::abs(y2 - stY);
            const bool sideNow = stSideUntil && static_cast<int>(now - stSideUntil) < 0;
            if (moved < 20 && !sideNow) {
                stTry = stTry >= 4 ? 1 : stTry + 1;
                stAng = ((stTry & 1) ? 1.0f : -1.0f) * (stTry <= 2 ? 0.785f : 1.571f);
                stSideUntil = now + 400;
                stSideWin = true;
                WORD md = 0; DWORD st = 0;
                Rd(h2 + 0xfc, md); Rd(h2 + 0x150, st);
                Log("PAD2: eroe 2 bloccato (spostamento %d in 500 ms, modo %u, stato %08lX): scarto di %d gradi", moved, md, st, static_cast<int>(stAng * 57.3f));
            } else {
                if (moved >= 20 && !stSideWin) stTry = 0;   // avanza da solo: al prossimo blocco si riparte dal primo scarto
                stSideWin = sideNow;
            }
            stCheckAt = now + 500; stX = x2; stY = y2;
        }
        if (stSideUntil && static_cast<int>(now - stSideUntil) < 0) {
            const float c = std::cos(stAng), sn = std::sin(stAng), rx = dx * c - dy * sn, ry = dx * sn + dy * c;
            dx = rx; dy = ry;
            return true;
        }
        stSideUntil = 0;
        return false;
    };
    // 0.55.30: limite dello stick del pad 2 (bordi dello schermo e guinzaglio). Il gestore del giocatore 2 del gioco controlla la
    // distanza dall'eroe 1 (1499 unita', poi teletrasporto) solo quando nessuna freccia e' premuta: con lo stick tenuto l'eroe 2
    // poteva allontanarsi molto, anche fuori dallo schermo, e tornava solo lasciando lo stick (il tester, 31/10 sulla Steam Machine:
    // "se l'eroe 2 si allontana dall'eroe 1, il 2 si puo' allontanare molto anche dopo che e' scomparso dalla visuale"). Come nei
    // giochi in co-op sullo stesso schermo, con l'eroe 2 al bordo dello schermo (piedi oltre il 5% ai lati, il 20% in alto per il
    // corpo, il 17% in basso per le barre) o oltre 1400 unita' dall'eroe 1 la parte dello stick verso l'esterno non conta: verso
    // l'interno e di lato si muove come sempre. [Native] Pad2EdgeLimit=false lo spegne.
    float mdx = mag > 1e-3f ? sx / mag : 0.0f, mdy = mag > 1e-3f ? -sy / mag : 0.0f;   // direzione sullo schermo (y in basso)
    const bool haveVw = ReadView(vw);
    if (mag >= g_cfg.nativePad2Dead && haveVw && g_cfg.nativePad2EdgeLimit) {
        static bool limLogged = false;
        float hsx = 0, hsy = 0;
        bool limited = false;
        if (WorldToScreen(vw, x2, y2, hsx, hsy)) {
            const float W = static_cast<float>(vw.w), H = static_cast<float>(vw.h);
            if ((hsx > W * 0.95f && mdx > 0.0f) || (hsx < W * 0.05f && mdx < 0.0f)) { mdx = 0.0f; limited = true; }
            if ((hsy > H * 0.83f && mdy > 0.0f) || (hsy < H * 0.20f && mdy < 0.0f)) { mdy = 0.0f; limited = true; }
        }
        int x1 = 0, y1 = 0;
        float ax = 0, ay = 0, bx = 0, by = 0;
        if (MemoryHeroPos(x1, y1) && double(x2 - x1) * (x2 - x1) + double(y2 - y1) * (y2 - y1) > 1400.0 * 1400.0 &&
            WorldToScreen(vw, x1, y1, ax, ay) && WorldToScreen(vw, x2, y2, bx, by)) {
            float ox = bx - ax, oy = by - ay;                    // dall'eroe 1 all'eroe 2, sullo schermo
            const float on = std::sqrt(ox * ox + oy * oy);
            if (on > 1e-3f) {
                ox /= on; oy /= on;
                const float out = mdx * ox + mdy * oy;
                if (out > 0.0f) { mdx -= out * ox; mdy -= out * oy; limited = true; }
            }
        }
        const float ml = std::sqrt(mdx * mdx + mdy * mdy);
        if (limited && !limLogged) { limLogged = true; Log("PAD2: eroe 2 al bordo dello schermo o del guinzaglio: lo stick verso l'esterno non lo sposta"); }
        else if (!limited) limLogged = false;
        if (ml >= 0.35f) want = bestCombo(vw, mdx / ml, mdy / ml);
    }
    else if (mag >= g_cfg.nativePad2Dead && haveVw) want = bestCombo(vw, mdx, mdy);   // sullo schermo y cresce in basso
    if (stSideUntil) { curCombo = want; comboAt = now; comboX = x2; comboY = y2; }   // durante lo scarto non si impara
    else if (want != curCombo) { curCombo = want; comboAt = now; comboX = x2; comboY = y2; }
    else if (want >= 0 && static_cast<int>(now - comboAt) >= 300) {      // impara dove va davvero l'eroe 2
        const int mx = x2 - comboX, my = y2 - comboY;
        if (std::abs(mx) + std::abs(my) > 400) { comboAt = now; comboX = x2; comboY = y2; }   // salto (guinzaglio, teletrasporto): non e' un passo
        else if (std::abs(mx) + std::abs(my) > 40) {
            float ax = 0, ay = 0, bx = 0, by = 0;
            if (WorldToScreen(vw, comboX, comboY, ax, ay) && WorldToScreen(vw, x2, y2, bx, by)) {
                float ex = bx - ax, ey = by - ay;
                const float n = std::sqrt(ex * ex + ey * ey);
                if (n > 1e-3f) {
                    ex /= n; ey /= n;
                    if (!learned[want]) Log("PAD2: frecce %u: l'eroe 2 va verso (%.2f, %.2f) sullo schermo (spostamento nel mondo %d, %d)", kCombo[want].keys, ex, ey, mx, my);
                    learnX[want] = learned[want] ? 0.7f * learnX[want] + 0.3f * ex : ex;
                    learnY[want] = learned[want] ? 0.7f * learnY[want] + 0.3f * ey : ey;
                    const float n2 = std::sqrt(learnX[want] * learnX[want] + learnY[want] * learnY[want]);
                    if (n2 > 1e-3f) { learnX[want] /= n2; learnY[want] /= n2; }
                    learned[want] = true;
                }
            }
            comboAt = now; comboX = x2; comboY = y2;
        }
    }
    // 0.55.0: dopo un'interazione con A le frecce restano ferme (il gestore del giocatore 2 trasformerebbe ogni freccia in un
    // ordine di movimento e l'eroe 2 lascerebbe perdere il bersaglio) finche' lo stick non torna al centro o non cambia
    // direzione di piu' di 60 gradi, al massimo per il tempo della traccia.
    static bool holdArrows = false;
    static float holdDx = 0, holdDy = 0;
    if (holdArrows) {
        const bool center = mag < g_cfg.nativePad2Dead;
        const bool turned = !center && (sx * holdDx - sy * holdDy) / mag < 0.5f;
        if (center || turned || static_cast<int>(now - g_h2Act.until) >= 0) holdArrows = false;
    }
    int pressC = want;
    const bool stickMove = want >= 0 && !holdArrows;
    if (stickMove) {
        float ddx = mdx, ddy = mdy;                     // 0.55.30: la direzione gia' limitata ai bordi (prima quella dello stick)
        const float dl = std::sqrt(ddx * ddx + ddy * ddy);
        if (dl > 1e-3f) { ddx /= dl; ddy /= dl; }
        if (stuckSide(true, ddx, ddy)) pressC = bestCombo(vw, ddx, ddy);
    }
    if (pressC >= 0 && !holdArrows) for (int k = 0; k < 4; ++k) if (kCombo[pressC].keys & (1u << k)) press(kArrowVk[k], 150);
    // 0.55.10: porta a spinta anche per l'eroe 2, come la porta a spinta del pad 1 (0.17.1, "porta chiusa davanti all'eroe"):
    // con lo stick inclinato, ogni 100 ms, la porta chiusa che il gioco sceglierebbe a 0,2, 0,35 o 0,5 di MoveRadius davanti
    // all'eroe 2 sullo schermo (dal suo corpo, un po' sopra i piedi) si apre con Hero2OpenDoor. Nella prova delle 14:35 il tester
    // spingeva l'eroe 2 contro la porta come fa con l'eroe 1 e non succedeva nulla.
    // 0.55.12: doorAhead vale anche per l'attacco, nella direzione del nemico.
    static DWORD doorNext = 0, doorLast = 0, doorLastAt = 0;
    auto doorAhead = [&](const View& v, float dx, float dy) {
        if (static_cast<int>(now - doorNext) < 0) return;
        doorNext = now + 100;
        float hsx = 0, hsy = 0;
        if (!v.view || !WorldToScreen(v, x2, y2, hsx, hsy)) return;
        const float r = g_cfg.moveRadius * static_cast<float>(v.h);
        hsy -= 0.05f * static_cast<float>(v.h);
        for (float k : { 0.2f, 0.35f, 0.5f }) {
            DWORD did = 0, st = 0;
            const LONG px = static_cast<LONG>(hsx + dx * r * k), py = static_cast<LONG>(hsy + dy * r * k);
            if (!MemoryDoorAt(px, py, did) || !MemoryDoorState(did, st) || st == 0xAA) continue;   // solo chiuse
            if (did == doorLast && static_cast<int>(now - doorLastAt) < 1500) break;              // appena provata
            doorLast = did; doorLastAt = now;
            Log("PAD2: porta %lu chiusa davanti all'eroe 2: la si apre", did);
            Hero2OpenDoor(did);
            break;
        }
    };
    if (want >= 0 && mag >= g_cfg.nativePad2Dead) doorAhead(vw, sx / mag, -sy / mag);
    // azioni (0.51.3): stessi tasti del pad 1 (schema in cima a Core.cpp), sulle azioni che l'eroe 2 ha
    const WORD released = static_cast<WORD>(prevRel & ~b);
    prevRel = b;
    // 0.55.0: con un dialogo di un PNG aperto A e B del pad 2 vanno al dialogo come quelli del pad 1 (Core.cpp: A = Invio, cioe'
    // OK o Accetta; B = ultimo pulsante, Rifiuta) e non fanno altro (niente attacco, interazione o raccolta).
    static bool aInteract = false;
    WORD dlgUsed = 0;
    // 0.55.28: con l'eroe 2 nel modo 10 (sta parlando: il dialogo del gioco e' aperto) A e B vanno al dialogo anche se la mod non
    // ha ancora trovato la sua finestra in memoria: Core.cpp la cerca e, se non c'e', A diventa Invio. Prova del 31/10 alle 11:20:
    // per 16 s ogni A rimandava l'eroe 2 verso il PNG con cui stava gia' parlando e la conversazione non andava avanti.
    WORD h2Mode = 0;
    Rd(h2 + 0xfc, h2Mode);
    const bool h2Talking = h2Mode == 10;
    if (h2Talking) g_h2TalkAt.store(now ? now : 1);
    if (edge & (PAD_A | PAD_B)) {
        DialogInfo dd;
        if (MemoryDialog(dd) || h2Talking) {
            dlgUsed = static_cast<WORD>(edge & (PAD_A | PAD_B));
            g_pad2DlgAt.store(now);
            g_pad2DlgBtn.store((edge & PAD_A) ? PAD_A : PAD_B);
            if (dlgUsed & PAD_A) aInteract = true;      // A tenuto dopo la risposta: niente attacco
            if (dlgUsed & PAD_B) bSince = 0;            // B: niente raccolta al rilascio
        }
    }
    const WORD act = static_cast<WORD>(edge & ~dlgUsed);
    // 0.55.17: mira con lo stick destro, come il bersaglio a fuoco del pad 1 (MemoryFindTarget con aim: vince il nemico piu'
    // dritto nella direzione, a parita' il piu' vicino; entro LockRange, fuori dal cono solo entro LockNear), misurata dai piedi
    // dell'eroe 2 sullo schermo, ogni 80 ms. Il nemico mirato ha l'indicatore "2" e lo usano A (attacco anche se e' oltre i 350
    // dell'attacco automatico), l'attacco con A tenuto (cambia bersaglio quando lo stick indica un altro nemico) e le arti.
    // Senza stick destro tutto come prima (il nemico piu' vicino). Da tastiera (Pad2Keyboard): e', o', a', u'.
    const float rmag = std::sqrt(p.rx * p.rx + p.ry * p.ry);
    const bool aiming = rmag >= g_cfg.nativePad2Dead;
    static DWORD aimId = 0, aimNext = 0, aimLogged = 0;
    static LONG aimMx = 0, aimMy = 0;
    if (!aiming) { aimId = 0; aimLogged = 0; }
    else if (static_cast<int>(now - aimNext) >= 0) {
        aimNext = now + 80;
        aimId = 0;
        View av{};
        float hsx = 0, hsy = 0;
        BYTE za = 0; Rd(h2 + 0x24, za);
        if (ReadView(av) && WorldToScreen(av, x2, y2, hsx, hsy)) {
            const float vh = static_cast<float>(av.h);
            Target tg;
            BYTE tz = 0xff;
            if (MemoryFindTarget(false, hsx, hsy, p.rx / rmag, -p.ry / rmag, g_cfg.lockRange * vh, g_cfg.lockNear * vh, tg,
                                 nullptr, 0, 0, true, true) && Rd(tg.addr + 0x24, tz) && tz == za) {
                aimId = tg.id;
                RECT r{};
                aimMx = static_cast<LONG>(tg.sx);
                aimMy = static_cast<LONG>(MemoryTargetRect(tg.id, r) ? static_cast<float>(r.top) - 0.02f * vh : tg.sy - 0.19f * vh);
                if (aimId != aimLogged) { aimLogged = aimId; Log("PAD2: mira: nemico %lu %s", aimId, tg.name); }
            }
        }
    }
    // 0.55.2: anteprima del bersaglio di A ogni 120 ms, con l'indicatore sopra (prova del 30/10: "difficilissimo capire chi
    // seleziona"). Niente indicatore con nemici vicini (A attacca) o con un dialogo aperto.
    static H2Target preview;
    static bool previewOk = false, hostile = false;
    static DWORD previewAt = 0;
    if (static_cast<int>(now - previewAt) >= 120 || (act & PAD_A)) {
        previewAt = now;
        BYTE z2 = 0; Rd(h2 + 0x24, z2);
        DWORD hp2 = 0; Rd(h2 + 0x4d8, hp2);
        DialogInfo dd;
        hostile = HostileNear(x2, y2, z2, 350);
        previewOk = hp2 != 0 && !hostile && !MemoryDialog(dd) &&
                    Hero2FindInteract(h2, mag > 1e-3f ? sx / mag : 0.0f, mag > 1e-3f ? -sy / mag : 0.0f, mag >= g_cfg.nativePad2Dead, preview);
        // l'indicatore no sopra una finestra del gioco (menu Esc, zaino...), A invece vale lo stesso
        if (previewOk && MemoryUIState() != 1 && !MemoryMouse2Ready()) { markOn = true; OverlaySetMarker2(true, preview.mx, preview.my, preview.icon); }
        else hideMark();
        // 0.56.18: alone a terra sotto cio' che A userebbe, anche con il secondo mouse (il cursore del gioco sta sopra al bersaglio)
        if (previewOk && MemoryUIState() != 1 && !preview.door) OverlaySetMarkerGround(2, true, preview.gx, preview.gy);
        else OverlaySetMarkerGround(2, false, 0, 0);
    }
    if (aiming && aimId && MemoryUIState() != 1 && !MemoryMouse2Ready()) { markOn = true; OverlaySetMarker2(true, aimMx, aimMy, 0); }   // 0.55.17: mira
    // A (0.55.0): alla pressione, senza nemici vicini all'eroe 2, interazione con il PNG, il forziere, la porta o l'oggetto fisso
    // indicato (nella direzione dello stick se inclinato); altrimenti, finche' e' tenuto, attacca come prima (Y del gestore
    // del giocatore 2: il nemico piu' vicino). Gli oggetti a terra restano a B.
    // 0.55.3-0.55.13: con Ctrl premuto si aspettava che tornasse libero; dalla 0.55.14 l'ordine parte con Ctrl spento (NoCtrlScope).
    bool fire = false;
    H2Target t = preview;
    // 0.55.5: attacco con A tenuto. Il gestore del giocatore 2 del gioco, con Y premuto, a ogni fotogramma cerca il nemico piu'
    // vicino nell'elenco dei nemici a contatto con l'eroe 1 (0xad2b48) e rimanda l'ordine: se il piu' vicino cambia l'attacco
    // riparte da capo (prova del 30/10 alle 13:57: "la mossa dell'attacco si spezzava a meta'"). Ora si sceglie il nemico piu'
    // vicino all'eroe 2 e lo si tiene finche' e' vivo e vicino, con l'ordine di tipo 7 del clic del gioco su un nemico, rimandato
    // solo se l'eroe 2 smette di attaccarlo. Se dopo il primo ordine l'eroe 2 non attacca mai (modo 1, 4, 8), per quella
    // pressione si torna a Y come prima.
    // 0.55.6: ogni ordine e' un colpo solo: finito il colpo l'eroe 2 torna fermo (modo 0) e aspetta (prova delle 14:07). Nella
    // 0.55.5 l'ordine si rimandava al controllo ogni 250 ms, quindi fra un colpo e l'altro fino a un quarto di secondo di pausa
    // ("l'attacco mi sembra un po' lento"), e un controllo caduto fra due colpi riportava a Y. Ora si rimanda appena l'eroe 2 e'
    // fermo (a ogni giro, al massimo ogni 60 ms) e si torna a Y solo se in 800 ms non ha mai attaccato.
    static DWORD atkId = 0, atkNext = 0, atkCheckAt = 0;
    static bool atkUseY = false, atkSeen = false, atkChecked = false;
    static bool atkHad = false, atkOver = false;   // 0.56.54: in questa pressione c'e' gia' stato un bersaglio / e' finito
    if (act & PAD_A) {
        aInteract = false;
        atkId = 0; atkNext = 0; atkCheckAt = 0; atkUseY = false; atkSeen = false; atkChecked = false; atkHad = false; atkOver = false;
        stopAtk();
        t = preview;
        if (aiming && aimId) { atkId = aimId; atkHad = true; Log("PAD2: A con la mira: attacco su %lu", aimId); }   // 0.55.17
        else if (hostile) Log("PAD2: A con nemici vicini: attacco");
        else if (!previewOk) Log("PAD2: A senza niente da usare vicino all'eroe 2: attacco (se ci sono nemici)");
        else if (t.door) { Hero2OpenDoor(t.id); aInteract = true; }
        else fire = true;
    }
    // 0.56.9: mercante con il secondo mouse: niente scambio dell'interfaccia. Se il negozio e' gia' aperto (dal giocatore 1) A apre
    // il lato del giocatore 2 senza parlare al PNG (la sessione e' di un interlocutore solo, 0x52af80: gliela toglierebbe);
    // altrimenti l'eroe 2 ci va a parlare e il negozio che si apre e' suo (DualSync apre il suo lato).
    // 0.56.14: fabbro gia' aperto (dal giocatore 1): uno alla volta, A non gli parla (la sessione del PNG e' di un interlocutore)
    if (fire && t.smith && MemoryWindowOpen(3)) {
        fire = false;
        aInteract = true;
        OverlayCoopToast(Tr("Il fabbro lo sta usando l'altro giocatore: uno alla volta"), 2500);
        Log("PAD2: A sul fabbro %lu gia' aperto: uno alla volta", t.id);
    }
    // 0.56.14: anche il Maestro delle combo (finestra n.14), come il mercante
    // 0.56.54: il deposito come finestra condivisa solo se e' gia' aperto (dal giocatore 1); aperto dall'eroe 2 resta lo scambio di prima
    const bool shop = fire && t.sharedWin >= 0 && MemoryMouse2Ready() && g_cfg.nativeTwoInventories && (t.sharedWin != 16 || MemoryWindowOpen(16));
    if (shop && MemoryWindowOpen(t.sharedWin)) {
        fire = false;
        aInteract = true;
        g_pad2SideReq.store(true);
        Log("PAD2: A su %s %lu gia' aperto: il lato del giocatore 2 (finestra condivisa n.%d)", t.kind, t.id, t.sharedWin);
    }
    if (shop && fire) {
        g_p2ShopWin.store(t.sharedWin); g_p2MerchAt.store(now | 1);
        Log("PAD2: l'eroe 2 va da %s %lu: la finestra n.%d sara' sua (finestra condivisa)", t.kind, t.id, t.sharedWin);
    }
    if (fire) {
        // 0.55.2: mercante, fabbro, maestro o stalliere: l'interfaccia passa subito all'eroe 2 con il puntatore al pad 2
        // (come Back), cosi' la finestra che si apre all'arrivo e' gia' la sua (nella prova delle 13:02 il fabbro aperto
        // dall'eroe 2 mostrava lo zaino dell'eroe 1); torna all'eroe 1 da sola quando la finestra si chiude.
        if (t.service && !shop && !autoSwap && !g_swapOrig.load() && g_cfg.nativeSwap && g_cfg.nativeSwapUi && g_uiPatched.load()) {
            g_pad2AutoSwap.store(true);
            g_pad2SwapReq.fetch_add(1);
            autoSwap = 1; autoAt = now;
            pendT = t; pendAt = now; pendOn = true;   // 0.56.46: l'ordine dopo lo scambio
            Log("PAD2: %s con un servizio (id %lu): interfaccia all'eroe 2 prima dell'arrivo", t.kind, t.id);
        }
        else Hero2Interact(t);
        aInteract = true;
        g_h2Act = H2Act{ t.id, now + 6000, now + 500, 0 };
        for (int vk : kArrowVk) {                 // le frecce premute in questo giro non devono annullare l'ordine
            DWORD cur = g_virt.keyUntil[vk].load();
            if (cur != VirtualInput::kHeld) g_virt.keyUntil[vk].compare_exchange_strong(cur, 0);
        }
        holdArrows = mag >= g_cfg.nativePad2Dead;
        holdDx = holdArrows ? sx / mag : 0.0f;
        holdDy = holdArrows ? -sy / mag : 0.0f;
    }
    if (!(b & PAD_A)) { aInteract = false; atkId = 0; }
    DWORD hpNow = 0; Rd(h2 + 0x4d8, hpNow);
    const bool attacking = (b & PAD_A) && !aInteract && hpNow != 0;   // A tenuto senza interazione (0.55.7: non a terra)
    {   // 0.56.47 (il tester, 06/11: "l'attacco del pg 2 continua ad essere stroncato a meta' se entrambi i giocatori tengono premuto il
        // tasto A"): con A tenuto, ogni cambio di modo, bersaglio (+0x104) o stato (+0x150) dell'eroe 2 nel log, con i millisecondi
        // dalla pressione e se il pad 1 tiene A (al massimo 80 righe per pressione); i clic del pad 1 hanno le loro righe.
        static DWORD trAt = 0, trTgt = 0, trSt = 0; static WORD trMode = 0xffff; static int trN = 0;
        if (!attacking) { trAt = 0; trMode = 0xffff; }
        else {
            if (!trAt) { trAt = now ? now : 1; trN = 0; }
            WORD md = 0; DWORD tg = 0, st = 0;
            Rd(h2 + 0xfc, md); Rd(h2 + 0x104, tg); Rd(h2 + 0x150, st);
            if ((md != trMode || tg != trTgt || st != trSt) && trN < 80) {
                ++trN;
                Log("PAD2: traccia A +%lu ms: modo %u, +0x104 %lu, stato %08lX%s", static_cast<unsigned long>(now - trAt), md, tg, st,
                    MemoryPad1AHeld() ? " (pad 1 con A)" : "");
            }
            trMode = md; trTgt = tg; trSt = st;
        }
    }
    if (!attacking || atkUseY) stopAtk();
    if (attacking) {
        if (atkUseY) { if (!stickMove) press('Y', 150); }   // 0.55.12: con Y premuto il gestore ignora le frecce
        else {
            BYTE z2 = 0; Rd(h2 + 0x24, z2);
            WORD mode = 0; DWORD tgt = 0, tgt100 = 0;
            Rd(h2 + 0xfc, mode); Rd(h2 + 0x104, tgt); Rd(h2 + 0x100, tgt100);   // 0.55.11: il nemico dell'uso e dell'attacco e' +0x104
            if (atkId && (mode == 1 || mode == 4 || mode == 8) && tgt == atkId) atkSeen = true;
            if (atkId && mode == 11 && tgt100 == atkId) atkSeen = true;   // 0.55.37: arte sullo stesso nemico (+0x100)
            if (static_cast<int>(now - atkNext) >= 0) {   // ogni 100 ms: il bersaglio tenuto e' ancora valido?
                atkNext = now + 100;
                if (aiming && aimId && aimId != atkId) {      // 0.55.17: lo stick destro indica un altro nemico
                    Log("PAD2: attacco: bersaglio %lu dalla mira (prima %lu)", aimId, atkId);
                    atkId = aimId;
                }
                // vale finche' e' vivo, sullo stesso piano ed entro 900 unita'
                bool keep = false;
                int ex = 0, ey = 0;
                if (atkId) {
                    const DWORD e = ObjectById(atkId);
                    int cls = 0; bool dead = false; BYTE ez = 0;
                    keep = e && ReadKind(e, cls, dead) && !dead && Rd(e + 0x1c, ex) && Rd(e + 0x20, ey) && Rd(e + 0x24, ez) && ez == z2 &&
                           double(ex - x2) * (ex - x2) + double(ey - y2) * (ey - y2) <= 900.0 * 900.0;
                }
                // 0.56.54 (il tester, 07/11: "se il giocatore 2 tiene premuto a, va all'attacco di qualsiasi nemico in automatico, mentre
                // il giocatore 1 no", scelto il comportamento del pad 1): come l'aggancio del pad 1 (UpdateHeldClick), alla pressione il
                // nemico piu' vicino entro 350 unita'; morto o lontano, il successivo nella stessa direzione (dall'eroe 2 verso il
                // nemico di prima, entro LockAngle e 700 unita', o vicinissimo, entro 200); altrimenti l'attacco finisce. Prima
                // si prendeva il piu' vicino entro 700 in ogni direzione, e l'eroe 2 correva da un nemico all'altro.
                static int lastEx = 0, lastEy = 0;
                if (keep) { lastEx = ex; lastEy = ey; }
                else {
                    DWORD id = 0;
                    bool found = false;
                    if (atkOver) found = false;
                    else if (!atkId && !atkHad) found = NearestHostile(x2, y2, z2, 350, id, ex, ey);
                    else if (atkHad) found = NextHostileInDir(x2, y2, z2, lastEx - x2, lastEy - y2, 700, 200, id, ex, ey);
                    if (found) {
                        if (id != atkId) Log("PAD2: attacco: bersaglio %lu (%d,%d)%s", id, ex, ey, atkHad ? " (il precedente e' morto o lontano: il successivo nella stessa direzione)" : "");
                        atkId = id; atkHad = true; lastEx = ex; lastEy = ey;
                    } else {
                        if (atkHad && atkId) { Log("PAD2: attacco: bersaglio perso, nessun altro nemico nella stessa direzione"); atkOver = true; }
                        atkId = 0;
                    }
                }
            }
            // 0.55.11: lontano (oltre 220 unita') l'eroe 2 si avvicina con le frecce verso il nemico, come con lo stick; da vicino
            // l'ordine di attacco lo manda EngineThreadTick (Hero2AttackTick)
            // 0.55.12: con il gancio della fermata (Pad2WalkToUse) il modo 1 non si ferma piu': l'ordine parte da qualunque distanza
            // e l'eroe 2 va al nemico con il percorso del gioco, che aggira gli ostacoli (le frecce vanno dritte e si incastrano);
            // le frecce dell'avvicinamento restano solo senza il gancio. Con lo stick inclinato l'eroe 2 lo guida il giocatore:
            // niente ordini ne' frecce dell'attacco (prova delle 14:58, J tenuto con W A S D: le frecce dello stick e quelle
            // dell'avvicinamento si sommavano, e l'ordine di attacco annullava il passo delle frecce).
            static bool atkWalking = false;
            double atkD = 0;
            int aex = 0, aey = 0;
            DWORD hpTgt = 0;
            if (atkId) {
                const DWORD e = ObjectById(atkId);
                if (e && Rd(e + 0x1c, aex) && Rd(e + 0x20, aey)) atkD = std::sqrt(double(aex - x2) * (aex - x2) + double(aey - y2) * (aey - y2));
                if (e) Rd(e + 0x4d8, hpTgt);
            }
            static DWORD hpId = 0, hpLast = 0, hpDropAt = 0;   // ultimo colpo andato a segno (la vita del nemico scende)
            if (atkId != hpId) { hpId = atkId; hpLast = hpTgt; hpDropAt = now; }
            else { if (hpTgt < hpLast) hpDropAt = now; hpLast = hpTgt; }
            // ...salvo che stia gia' attaccando quel nemico (modo 4 o 8): allora lo insegue da solo e la fermata non scatta.
            View av{};
            const bool haveAv = ReadView(av);
            const bool chasing = (mode == 4 || mode == 8) && tgt == atkId;
            const bool farTarget = atkId && !stickMove && !g_p2UseHookOn.load() && atkD > 220.0 && !chasing && haveAv;
            float tdx = 0, tdy = 0;                   // direzione del nemico sullo schermo
            bool haveDir = false;
            if (atkId && haveAv) {
                float ax = 0, ay = 0, bx = 0, by = 0;
                if (WorldToScreen(av, x2, y2, ax, ay) && WorldToScreen(av, aex, aey, bx, by)) {
                    tdx = bx - ax; tdy = by - ay;
                    const float dn = std::sqrt(tdx * tdx + tdy * tdy);
                    if (dn > 1e-3f) { tdx /= dn; tdy /= dn; haveDir = true; }
                }
            }
            // 0.55.12: eroe 2 fermo mentre deve andare al nemico (frecce dell'avvicinamento, o modo 1 o 4 su di lui oltre 100 unita'
            // senza colpirlo da 1,5 s; nella prova i colpi andati a segno arrivano a 146 unita', quindi la sola distanza non basta):
            // si apre una porta chiusa verso il nemico (alle 14:58 il nemico 1292 era dietro la porta 1747, chiusa a chiave, e l'eroe
            // 2 restava in modo 4 a 152 unita'), e se in 500 ms non si e' spostato scarto laterale con le frecce, poi di nuovo l'ordine
            const bool chaseStuck = atkId && !stickMove && (mode == 1 || mode == 4) && tgt == atkId && atkD > 100.0 &&
                                    static_cast<int>(now - hpDropAt) >= 1500;
            bool side = false;
            if (!stickMove && haveDir) {
                if (farTarget || chaseStuck) doorAhead(av, tdx, tdy);
                side = stuckSide(farTarget || chaseStuck || stSideUntil != 0, tdx, tdy);   // durante lo scarto il modo e' 2
            }
            if (stickMove) {
                stopAtk();
                atkWalking = false;
                atkCheckAt = 0;                       // il controllo del ritorno a Y riparte dal prossimo ordine
            } else if (farTarget || side) {
                stopAtk();
                if (haveDir) {
                    const int bc = bestCombo(av, tdx, tdy);
                    if (bc >= 0) for (int k = 0; k < 4; ++k) if (kCombo[bc].keys & (1u << k)) press(kArrowVk[k], 150);
                }
                if (!atkWalking) {
                    atkWalking = true;
                    if (farTarget) Log("PAD2: attacco: nemico %lu a %.0f unita', l'eroe 2 si avvicina con le frecce", atkId, atkD);
                }
            } else if (atkId) {
                if (atkWalking) {                     // arrivato o scarto finito: le frecce si lasciano subito, poi l'ordine
                    atkWalking = false;
                    for (int vk : kArrowVk) { DWORD cur = g_virt.keyUntil[vk].load(); if (cur != VirtualInput::kHeld) g_virt.keyUntil[vk].compare_exchange_strong(cur, 0); }
                }
                g_h2AtkUntil.store(now + 300);
                g_h2AtkId.store(atkId);
                if (!atkChecked && !atkCheckAt) atkCheckAt = now + 800;   // il controllo del ritorno a Y parte da vicino
            }
            // 0.56.57 (il tester, 10/11: "j tenendolo premuto il giocatore 2 fa piccoli passi perche' viene bloccato dalla mod"): senza un
            // nemico della mod si premeva Y, e il gestore del giocatore 2 con Y manda l'eroe 2 sul nemico piu' vicino fra quelli
            // dell'eroe 1, a qualunque distanza (prove delle 16:10 e 16:39: 696, 1556, 1533, 1558 fino a 1344 unita'); la fermata
            // della 0.56.56 lo fermava e Y lo rimandava, a passetti. Era la mod, non il gioco: come il pad 1, senza nemico vicino
            // (350 alla pressione, poi il successivo nella stessa direzione) A tenuto non fa niente.
            if (!atkId) stopAtk();
            {   // 0.55.10: traccia dell'attacco, una riga al secondo (modo, nemico, punto dell'ordine, distanze)
                static DWORD atkLogAt = 0;
                if (atkId && static_cast<int>(now - atkLogAt) >= 1000) {
                    atkLogAt = now;
                    const DWORD e = ObjectById(atkId);
                    int ex = 0, ey = 0, px = 0, py = 0, hpE = 0;
                    if (e) { Rd(e + 0x1c, ex); Rd(e + 0x20, ey); Rd(e + 0x4d8, hpE); }
                    DWORD st2 = 0;
                    Rd(h2 + 0x110, px); Rd(h2 + 0x114, py); Rd(h2 + 0x150, st2);
                    Log("PAD2: attacco su %lu: eroe 2 modo %u stato %08lX, +0x104 %lu, +0x100 %lu, nemico a %.0f (vita %d), punto dell'uso a %.0f dal nemico%s",
                        atkId, mode, st2, tgt, tgt100, std::sqrt(double(ex - x2) * (ex - x2) + double(ey - y2) * (ey - y2)), hpE,
                        std::sqrt(double(ex - px) * (ex - px) + double(ey - py) * (ey - py)), stickMove ? " (stick)" : "");
                }
            }
            // 0.55.37: con un'arte in corso (modo 11) il controllo si rimanda. Prova del 31/10 sera: tutte le 44 righe "non attacca
            // con l'ordine diretto: si torna a Y" avevano il modo 11; con Y il gestore del gioco sceglie a ogni fotogramma il nemico piu'
            // vicino fra quelli a contatto con l'eroe 1, e l'attacco dell'eroe 2 ripartiva quando l'eroe 1 ingaggiava un altro nemico
            // ("l'attacco del giocatore 1 a corpo a corpo interrompe il mio attacco", 00:55).
            if (atkCheckAt && static_cast<int>(now - atkCheckAt) >= 0 && atkId && !atkSeen && mode == 11) atkCheckAt = now + 300;
            if (atkCheckAt && static_cast<int>(now - atkCheckAt) >= 0) {
                atkCheckAt = 0;
                atkChecked = true;
                if (atkId && !atkSeen) {
                    atkUseY = true;
                    stopAtk();
                    Log("PAD2: l'eroe 2 non attacca con l'ordine diretto (modo %u, bersaglio %lu): si torna a Y", mode, tgt);
                } else if (atkId) Log("PAD2: attacco diretto: eroe 2 modo %u, bersaglio %lu", mode, tgt);
            }
        }
    }
    if (!stUsed) { float zx = 0, zy = 0; stuckSide(false, zx, zy); }   // 0.55.12: niente da muovere: rilevamento del blocco azzerato
    if (g_h2Act.id && static_cast<int>(now - g_h2Act.until) < 0 && static_cast<int>(now - g_h2Act.next) >= 0 && g_h2Act.logs < 8) {
        g_h2Act.next = now + 500;                     // traccia dell'interazione: modo, bersaglio e distanza dell'eroe 2
        ++g_h2Act.logs;
        WORD mode = 0; DWORD tgt = 0, st = 0;
        Rd(h2 + 0xfc, mode); Rd(h2 + 0x104, tgt); Rd(h2 + 0x150, st);
        const DWORD to = (g_h2Act.id & 0x80000000u) ? 0 : ObjectById(g_h2Act.id);
        int tx = 0, ty = 0;
        const bool tp = to && Rd(to + 0x1c, tx) && Rd(to + 0x20, ty);
        const double d = tp ? std::sqrt(double(tx - x2) * (tx - x2) + double(ty - y2) * (ty - y2)) : -1.0;
        DWORD horse2 = 0, owner = 0;                  // 0.55.20: per il cavallo comprato dall'eroe 2 (di chi diventa)
        Rd(h2 + 0x570, horse2);
        if (to) Rd(to + 0x1ec, owner);
        Log("PAD2: interazione con id %lu: eroe 2 modo %u, bersaglio %lu, stato %08lX, distanza %.0f, cavallo dell'eroe 2 %lu, +0x1ec del bersaglio %lu",
            g_h2Act.id, mode, tgt, st, d, horse2, owner);
        // 0.55.25: niente piu' ripiego con pickupItem per gli oggetti scelti con A (li raccoglie il gioco all'arrivo, 0x53a570)
    } else if (g_h2Act.id && static_cast<int>(now - g_h2Act.until) >= 0) g_h2Act.id = 0;
    static const WORD kArtBtn[5] = { PAD_X, PAD_Y, PAD_RB, PAD_RT, PAD_LT };   // arti 1-5 come il pad 1
    // 0.55.14: bersaglio dell'arte: il nemico mirato con lo stick destro (0.55.17), poi quello dell'attacco con A tenuto, poi il
    // nemico piu' vicino all'eroe 2 (entro 700),
    // altrimenti un punto a 150 unita' nella direzione dello stick (o l'eroe 2 stesso, per le arti su di se')
    for (int k = 0; k < 5; ++k) {
        if (!(edge & kArtBtn[k]) || h2Talking) continue;   // 0.55.29: non durante un dialogo dell'eroe 2, come il pad 1
        BYTE za = 0; Rd(h2 + 0x24, za);
        DWORD tid = (aiming && aimId) ? aimId : atkId;   // 0.55.17: prima la mira
        int ex = 0, ey = 0;
        if (!tid && !NearestHostile(x2, y2, za, 700, tid, ex, ey)) tid = 0;
        int gx = x2, gy = y2;
        if (want >= 0 && mag >= g_cfg.nativePad2Dead) {
            const float wl = std::sqrt(static_cast<float>(kCombo[want].wx * kCombo[want].wx + kCombo[want].wy * kCombo[want].wy));
            if (wl > 0.0f) { gx += static_cast<int>(150.0f * kCombo[want].wx / wl); gy += static_cast<int>(150.0f * kCombo[want].wy / wl); }
        }
        UseArtHero2(k, tid, gx, gy);
    }
    // 0.55.28: non mentre l'eroe 2 parla (come il pad 1, che con un dialogo aperto e' in modalita' cursore): alle 11:20:29 la
    // croce, usata per provare a far andare avanti il dialogo, gli ha fatto bere un antidoto e cambiare arma
    if (!h2Talking) {
        if (edge & PAD_LB) UseBeltHero2(6);             // LB: pozione di cura (Spazio)
        // croce su / giu': g_cfg.dpadPotion (0.56.25: concentrazione R / Mentore W). 0.55.31: tenute oltre ZoomHoldMs = zoom (rotellina avanti /
        // indietro, ripetuto ogni 500 ms), come il pad 1; la pozione allora si beve al rilascio, solo se non c'e' stato zoom.
        static const WORD kZBtn[2] = { PAD_DPAD_UP, PAD_DPAD_DOWN };
        for (int k = 0; k < 2; ++k) {
            const int kind = g_cfg.dpadPotion[k];
            if (g_cfg.zoomHoldMs <= 0) { if (edge & kZBtn[k]) UseBeltHero2(kind); continue; }
            if (edge & kZBtn[k]) { zSince[k] = now ? now : 1; zNext[k] = 0; zDone[k] = false; }
            if ((b & kZBtn[k]) && zSince[k] && now - zSince[k] >= static_cast<DWORD>(g_cfg.zoomHoldMs) && (!zNext[k] || static_cast<LONG>(now - zNext[k]) >= 0)) {
                GameZoomStep(k == 0 ? 1 : -1, k == 0 ? "PAD2: croce su tenuta" : "PAD2: croce giu' tenuta");
                zDone[k] = true;
                zNext[k] = now + 500;
            }
            if ((released & kZBtn[k]) && zSince[k]) {
                if (!zDone[k]) UseBeltHero2(kind);
                zSince[k] = 0;
            }
        }
        if (edge & PAD_DPAD_LEFT) SwitchWeaponHero2(-1);   // 0.52.3: croce sx/dx: arma precedente / successiva, come il pad 1
        if (edge & PAD_DPAD_RIGHT) SwitchWeaponHero2(1);
    }
    else zSince[0] = zSince[1] = 0;                     // durante un dialogo niente pozioni, nemmeno al rilascio
    // 0.56.1: cursore del giocatore 2 nel gioco normale (il tester, 03/11: "deve comparire anche in game normale il cursore del
    // giocatore 2, esattamente come appare quello del giocatore 1"). Con il secondo mouse lo disegna il gioco, con le stesse forme
    // del cursore del giocatore 1 (spada sui nemici, dialogo sui personaggi, mano su forzieri e oggetti, cavallo...) e il contorno
    // dell'oggetto indicato; la mod lo mette come quello del pad 1: sul nemico mirato con lo stick destro o attaccato con A, sul
    // bersaglio di A da fermo (cio' che A userebbe), sul nemico che A attaccherebbe, altrimenti davanti all'eroe 2 nella direzione
    // dello stick (o dell'ultimo passo). Al posto dell'indicatore "2" sopra il bersaglio, con il numero accanto al cursore.
    if (MemoryMouse2Ready()) {
        static float curDx = 0.0f, curDy = 1.0f;
        if (mag >= g_cfg.nativePad2Dead) { curDx = sx / mag; curDy = -sy / mag; }
        View cv{};
        float hsx = 0, hsy = 0;
        if (ReadView(cv) && cv.w > 0 && cv.h > 0 && WorldToScreen(cv, x2, y2, hsx, hsy)) {
            const float vh = static_cast<float>(cv.h);
            auto objPoint = [&](DWORD id, LONG& px, LONG& py) {
                if (!id) return false;
                if (MemoryPickPoint(id, px, py)) return true;
                const DWORD o = ObjectById(id);
                int ox = 0, oy = 0; float fx = 0, fy = 0;
                if (!o || !Rd(o + 0x1c, ox) || !Rd(o + 0x20, oy) || !WorldToScreen(cv, ox, oy, fx, fy)) return false;
                px = static_cast<LONG>(fx); py = static_cast<LONG>(fy - g_cfg.lockOffsetY * vh);
                return true;
            };
            LONG cx2 = 0, cy2 = 0;
            DWORD nearId = 0; int nex = 0, ney = 0; BYTE nz = 0;
            if (aiming && aimId && objPoint(aimId, cx2, cy2)) {}
            else if (attacking && atkId && objPoint(atkId, cx2, cy2)) {}
            else if (previewOk && objPoint(preview.id, cx2, cy2)) {}
            else if (previewOk) { cx2 = preview.mx; cy2 = preview.my + static_cast<LONG>(0.10f * vh); }
            else if (hostile && Rd(h2 + 0x24, nz) && NearestHostile(x2, y2, nz, 350, nearId, nex, ney) && objPoint(nearId, cx2, cy2)) {}
            else {
                const float r = g_cfg.moveRadius * vh * (mag >= g_cfg.nativePad2Dead ? 1.0f : 0.6f);
                cx2 = static_cast<LONG>(hsx + curDx * r);
                cy2 = static_cast<LONG>(hsy - 0.04f * vh + curDy * r);
            }
            cx2 = cx2 < 1 ? 1 : (cx2 > cv.w - 2 ? cv.w - 2 : cx2);
            cy2 = cy2 < 1 ? 1 : (cy2 > cv.h - 2 ? cv.h - 2 : cy2);
            Mouse2World(true, cx2, cy2);
            OverlaySetCursor2Num(true, cx2, cy2);
        }
    }
    // B: breve = raccogli tutto del gioco, come il pad 1 (0.55.22; prima l'oggetto piu' vicino); tenuto = cavallo, come il pad 1
    // (0.55.19; prima la raccolta automatica on/off, tolta nella 0.55.25)
    if (act & PAD_B) { bSince = now; bLong = false; }
    if ((b & PAD_B) && bSince && !bLong && now - bSince >= kHoldMs) { bLong = true; Log("PAD2: B tenuto: cavallo"); HorseHero2(); }
    if ((released & PAD_B) && bSince) {
        if (!bLong) CollectAllHero2();                  // 0.55.22: "raccogli tutto" del gioco, come B del pad 1 (prima l'oggetto piu' vicino)
        bSince = 0;
    }
    // View: interfaccia dell'eroe 2 (Home). 0.55.54: con i due zaini possibili apre il suo lato dello zaino (Core.cpp), come View
    // tenuto del pad 1; con mercante, deposito e gli altri pannelli resta lo scambio dell'interfaccia di prima. 0.56.2: premuto e
    // lasciato = modalita' cursore (viewButton).
    viewButton(released);
    startButton(released);
}

// 0.55.29: controllo automatico (revisione del 31/10 dopo le prove sulla Steam Machine): un eroe che parla (modo 10, il gioco ha
// il dialogo aperto) da 2 s senza che la mod abbia trovato la finestra del dialogo e' il difetto delle 11:20 (A che non manda
// avanti la conversazione). Una riga ATTENZIONE nel log per ogni conversazione, con l'elenco dei candidati, cosi' il problema
// si vede nel log anche se in partita passa inosservato. Ogni 250 ms, solo lettura.
void DialogWatchdog(DWORD mgr, DWORD heroId, bool inWorld, DWORD now)
{
    static DWORD checkAt = 0, talkFrom[2] = {}, missFrom[2] = {};
    static bool warned[2] = {};
    if (static_cast<LONG>(now - checkAt) < 250) return;
    checkAt = now;
    for (int k = 0; k < 2; ++k) {
        // con PadSource=None l'eroe 1 lo comanda il mouse e la mod non cerca le sue finestre: niente avvisi per lui
        const bool skip = k == 0 && g_cfg.padSource == PadSource::None;
        const DWORD h = (inWorld && mgr && !skip && (k == 1 || heroId)) ? ObjectAddr(mgr, k == 0 ? heroId : 2) : 0;
        WORD mode = 0;
        const bool talking = h && Rd(h + 0xfc, mode) && mode == 10;
        if (!talking) {
            if (warned[k]) Log("Dialoghi: l'eroe %d ha smesso di parlare dopo %lu ms", k + 1, now - talkFrom[k]);
            talkFrom[k] = 0; missFrom[k] = 0; warned[k] = false;
            continue;
        }
        if (!talkFrom[k]) talkFrom[k] = now ? now : 1;
        DialogInfo d;
        if (MemoryDialog(d)) { missFrom[k] = 0; continue; }
        if (!missFrom[k]) { missFrom[k] = now ? now : 1; continue; }
        if (!warned[k] && now - missFrom[k] >= 2000) {
            warned[k] = true;
            Log("ATTENZIONE: l'eroe %d parla (modo 10) da %lu ms e la finestra del dialogo non e' stata trovata in memoria", k + 1, now - missFrom[k]);
            MemoryDialogDump();
        }
    }
}

// Salvataggio dell'eroe 2 nel suo file (0.45.0, N7 della sezione 8c). Il salvataggio del file eroe del gioco e'
// 0x61d080(percorso): controllo anti-trucchi 0x5909e0 sull'eroe locale ([player+0x14]), poi saveHero 0x604be0 sull'eroe
// locale e scrittura su disco. saveHero prende quasi tutto dall'eroe che salva (0xc0 oggetto: attributi, abilita',
// esperienza, livello; 0xc1 voce dell'inventario; 0xc2 equipaggiamento; 0xc3 informazioni e nome), tranne 0xc4 (0x40 byte)
// e 0xce (100 byte), che vengono dal cStatsManager dell'eroe locale e che loadHero 0x605df0 rilegge solo per l'eroe locale;
// il nome in 0xc3 viene dalla stringa globale del giocatore (quello dell'eroe 1). Procedura, nel thread del motore: [player+0x14]
// = 2 per il tempo della chiamata, la chiamata a 0x5909e0 dentro 0x61d080 sostituita da "mov al, 1" (il controllo vale solo
// per l'eroe 1 e sull'eroe 2 lo ucciderebbe come "Cheater"), salvataggio in un file temporaneo, ripristino; poi nel file
// nuovo si rimettono 0xc4, 0xce e il nome presi dal file originale dell'eroe 2, si controlla il file e si sostituisce
// l'originale dopo averne fatto una copia in Save\SacredPad_backup.
constexpr DWORD kSaveHeroFile = 0x0061d080, kAntiCheat = 0x005909e0;

bool PaxLoad(const char* path, std::vector<BYTE>& d)
{
    d.clear();
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(h, nullptr);
    bool ok = size != INVALID_FILE_SIZE && size >= 0x100 + 12 && size < (16u << 20);
    if (ok) { d.resize(size); DWORD got = 0; ok = ReadFile(h, d.data(), size, &got, nullptr) && got == size; }
    CloseHandle(h);
    return ok && memcmp(d.data(), "AMH\x1b", 4) == 0;
}

// Voce della tabella dei blocchi (a 0x100, voci da 12 byte: tipo, posizione, versione); la lunghezza e' la distanza
// dal blocco successivo (o dalla fine del file).
bool PaxChunk(const std::vector<BYTE>& d, DWORD type, DWORD& off, DWORD& len)
{
    std::vector<DWORD> offs;
    off = 0;
    for (size_t i = 0; i < 48 && 0x100 + 12 * (i + 1) <= d.size(); ++i) {
        DWORD e[3]; memcpy(e, &d[0x100 + 12 * i], 12);
        if (e[0] == 0) break;
        if (e[1] < d.size()) offs.push_back(e[1]);
        if (e[0] == type) off = e[1];
    }
    if (!off || off >= d.size()) return false;
    len = static_cast<DWORD>(d.size()) - off;
    for (DWORD o : offs) if (o > off && o - off < len) len = o - off;
    return true;
}

// Blocchi compressi (loadHero 0x605df0: 0xc7, 0xca, 0xcb, 0xcd passano da FUN_00607b90 e poi valgono come 0xc0, 0xc1,
// 0xc6, 0xcc): nella prova della 0.45.0 il file nuovo era stato scartato perche' l'oggetto e' salvato come 0xc7.
bool PaxHas(const std::vector<BYTE>& d, DWORD type, DWORD packed)
{
    DWORD o = 0, l = 0;
    return PaxChunk(d, type, o, l) || (packed && PaxChunk(d, packed, o, l));
}

std::string PaxTable(const std::vector<BYTE>& d)
{
    std::string s;
    for (size_t i = 0; i < 48 && 0x100 + 12 * (i + 1) <= d.size(); ++i) {
        DWORD e[3]; memcpy(e, &d[0x100 + 12 * i], 12);
        if (e[0] == 0) break;
        char t[48]; _snprintf_s(t, sizeof(t), _TRUNCATE, " %lX@%lX", e[0], e[1]);
        s += t;
    }
    return s;
}

bool PaxCopyBlock(const std::vector<BYTE>& from, std::vector<BYTE>& to, DWORD type, DWORD size)
{
    DWORD fo = 0, fl = 0, to_ = 0, tl = 0;
    if (!PaxChunk(from, type, fo, fl) || !PaxChunk(to, type, to_, tl) || fl < size || tl < size) return false;
    memcpy(&to[to_], &from[fo], size);
    return true;
}

bool PaxCopyName(const std::vector<BYTE>& from, std::vector<BYTE>& to)
{
    DWORD fo = 0, fl = 0, to_ = 0, tl = 0;
    if (!PaxChunk(from, 0xc3, fo, fl) || !PaxChunk(to, 0xc3, to_, tl) || fl < 0x22c || tl < 0x22c) return false;
    auto wlen = [](const BYTE* p, size_t maxCh) { size_t n = 0; while (n < maxCh && (p[2 * n] || p[2 * n + 1])) ++n; return n; };
    const size_t maxCh = 60;
    const size_t a = wlen(&from[fo + 8], maxCh), b = wlen(&to[to_ + 8], maxCh);
    if (a == 0 || a >= maxCh || b >= maxCh) return false;
    const size_t span = ((a > b ? a : b) + 1) * 2;
    memset(&to[to_ + 8], 0, span);
    memcpy(&to[to_ + 8], &from[fo + 8], a * 2);
    return true;
}

// Indirizzo della chiamata a 0x5909e0 dentro 0x61d080 (E8 rel32), cercata nei primi 0x200 byte.
DWORD FindAntiCheatCall()
{
    static DWORD found = 0, tried = 0;
    if (tried) return found;
    tried = 1;
    BYTE code[0x200];
    if (!RdBuf(kSaveHeroFile, code, sizeof(code))) return 0;
    for (DWORD i = 0; i + 5 <= sizeof(code); ++i) {
        if (code[i] != 0xE8) continue;
        LONG rel = 0; memcpy(&rel, &code[i + 1], 4);
        if (kSaveHeroFile + i + 5 + static_cast<DWORD>(rel) == kAntiCheat) { found = kSaveHeroFile + i; break; }
    }
    return found;
}

bool PatchCode(DWORD addr, const BYTE* bytes, size_t n)
{
    DWORD old = 0;
    void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

std::atomic<bool> g_hero2Saving{ false };

// L'oggetto 2 scritto in un file .pax con la funzione del gioco che salva l'eroe locale (0x61d080), nel thread del motore:
// [player+0x14] = 2 per la durata della chiamata e la chiamata al controllo anti-trucchi 0x5909e0 sostituita da "mov al,1"
// (0.45.0). rel e' relativo alla cartella del gioco. Il blocco 0xc4 e il nome nel file sono quelli dell'eroe 1 (singleton
// cStatsManager): SaveHero2File li rimette dal file originale. 0.54.0: separata da SaveHero2File per la copia di sicurezza
// dell'eroe 2 arrivato con un salvataggio della partita.
bool WriteHero2Pax(const char* rel, const char* why)
{
    DWORD player = 0, cur = 0;
    Rd(kPlayer, player);
    if (!player || !Rd(player + 0x14, cur) || !cur || cur == 2) { Log("EROE2: %s annullato: eroe locale non valido (%lu)", why, cur); return false; }
    const DWORD call = FindAntiCheatCall();
    BYTE saved[5] = {};
    static const BYTE kMovAl1[5] = { 0xB0, 0x01, 0x90, 0x90, 0x90 };
    if (!call || !RdBuf(call, saved, 5) || !PatchCode(call, kMovAl1, 5)) { Log("EROE2: %s annullato: chiamata del controllo non trovata o non modificabile", why); return false; }
    g_saveOrig.store(cur);              // il thread della DLL continua a vedere l'eroe 1 (prima: falsa "nuova partita")
    WriteDword(player + 0x14, 2);
    const CallResult cr = CallThis1(kSaveHeroFile, 0, reinterpret_cast<DWORD>(rel));
    WriteDword(player + 0x14, cur);
    g_saveOrig.store(0);
    PatchCode(call, saved, 5);
    if (!cr.ok) { LogException(why); return false; }
    if ((cr.ret & 0xff) == 0) { Log("EROE2: %s fallito: il gioco non ha scritto il file (ritorno %lu)", why, cr.ret & 0xff); return false; }
    return true;
}

// Salvataggio dell'eroe 2 nel suo HeroNN.pax, nel thread del motore (0.54.1: separato da SaveHero2File per il gancio del
// salvataggio della partita). Chi chiama ha gia' messo g_hero2Saving a true; fileNo e' g_h2MineFile al momento della richiesta.
void SaveHero2Now(const char* why, int fileNo)
{
    struct Done { ~Done() { g_hero2Saving.store(false); } } done;
    char orig[MAX_PATH], tmp[MAX_PATH], tmpRel[64], bdir[MAX_PATH], bak[MAX_PATH];
    _snprintf_s(orig, sizeof(orig), _TRUNCATE, "%s\\Save\\Hero%02d.pax", GameDir(), fileNo);
    _snprintf_s(tmpRel, sizeof(tmpRel), _TRUNCATE, "Save\\SacredPad_hero2.tmp");
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s\\%s", GameDir(), tmpRel);
    std::vector<BYTE> src, out;
    if (!PaxLoad(orig, src)) { Log("EROE2: salvataggio annullato (%s): Hero%02d.pax non leggibile", why, fileNo); return; }
    DWORD mgr = 0;
    Rd(kObjMgr, mgr);
    if (!mgr || !ObjectAddr(mgr, 2) || g_h2MineFile.load() != fileNo) { Log("EROE2: salvataggio annullato (%s): eroe 2 assente o cambiato", why); return; }
    DeleteFileA(tmp);
    if (!WriteHero2Pax(tmpRel, "salvataggio dell'eroe 2")) return;
    if (!PaxLoad(tmp, out)) { Log("EROE2: salvataggio fallito (%s): file scritto dal gioco non leggibile", why); return; }
    const bool okC0 = PaxHas(out, 0xc0, 0xc7), okC3 = PaxHas(out, 0xc3, 0);
    const bool okC4 = PaxCopyBlock(src, out, 0xc4, 0x40), okCe = PaxCopyBlock(src, out, 0xce, 100), okNm = PaxCopyName(src, out);
    if (!okC0 || !okC3 || out.size() < 4096) {
        char bad[MAX_PATH]; _snprintf_s(bad, sizeof(bad), _TRUNCATE, "%s\\Save\\SacredPad_hero2_scartato.pax", GameDir());
        MoveFileExA(tmp, bad, MOVEFILE_REPLACE_EXISTING);
        Log("EROE2: file nuovo non valido (%u byte, blocchi:%s; originale:%s), originale lasciato com'era, file scartato in %s",
            static_cast<unsigned>(out.size()), PaxTable(out).c_str(), PaxTable(src).c_str(), bad);
        return;
    }
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    const bool written = h != INVALID_HANDLE_VALUE && WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &wr, nullptr) && wr == out.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!written) { Log("EROE2: scrittura del file corretto fallita (%lu)", GetLastError()); return; }
    SYSTEMTIME t; GetLocalTime(&t);
    _snprintf_s(bdir, sizeof(bdir), _TRUNCATE, "%s\\Save\\SacredPad_backup", GameDir());
    CreateDirectoryA(bdir, nullptr);
    _snprintf_s(bak, sizeof(bak), _TRUNCATE, "%s\\Hero%02d_%04u%02u%02u_%02u%02u%02u.pax", bdir, fileNo, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (!CopyFileA(orig, bak, FALSE)) { Log("EROE2: copia di sicurezza fallita (%lu), originale lasciato com'era", GetLastError()); return; }
    if (!MoveFileExA(tmp, orig, MOVEFILE_REPLACE_EXISTING)) { Log("EROE2: sostituzione di Hero%02d.pax fallita (%lu)", fileNo, GetLastError()); return; }
    DWORD hp = 0; WORD lvl = 0; DWORD xp = 0;
    const DWORD h2 = ObjectAddr(mgr, 2);
    if (h2) { Rd(h2 + 0x3FE, lvl); Rd(h2 + 0x3b4, xp); Rd(h2 + 0x4d8, hp); }
    Log("EROE2: salvato in Hero%02d.pax (%s): %u byte, livello %u, esperienza %lu; blocchi dall'originale 0xc4 %s, 0xce %s, nome %s; copia in %s",
        fileNo, why, static_cast<unsigned>(out.size()), lvl, xp, okC4 ? "si" : "no", okCe ? "si" : "no", okNm ? "si" : "no", bak);
    char msg[96]; _snprintf_s(msg, sizeof(msg), _TRUNCATE, Tr("Eroe 2 salvato (livello %u)"), lvl);
    OverlayCoopToast(msg, 3000);
}

void SaveHero2File(const char* why)
{
    // 0.54.0: nel file da cui la mod ha creato l'eroe 2 in questa partita, non in Hero2File dell'ini (l'ultimo scelto nel menu,
    // che poteva essere il personaggio di un altro)
    const int fileNo = g_h2MineFile.load();
    if (fileNo < 0) { Log("EROE2: salvataggio dell'eroe 2 saltato (%s): non si sa da quale HeroNN.pax viene", why); return; }
    if (g_hero2Saving.exchange(true)) return;
    const bool queued = RunOnGameThread([=] { SaveHero2Now(why, fileNo); });
    if (!queued) { g_hero2Saving.store(false); Log("EROE2: salvataggio non accodato (thread del motore non pronto)"); }
}

// 0.55.37: progressi dell'eroe 2 recuperati dal salvataggio della partita. Fino alla 0.55.36 l'eroe 2 nato da Hero00.pax non si
// salvava mai (g_h2MineFile usava 0 per "sconosciuto"): prova del 31/10 sera, Elfo oscuro dell'amico del tester in Hero00.pax, righe
// "la partita si salva, l'eroe 2 no" alle 23:40, 00:33, 00:51 e 01:14; al ricarico delle 00:33 l'eroe 2 del salvataggio (livello 6,
// esperienza 14571) e' finito solo nella copia di sicurezza e la partita e' ripresa con l'eroe 2 di livello 2 ("ho perso i
// progressi"). Ora, se l'eroe 2 arrivato con il salvataggio e' della stessa classe dell'eroe 2 scelto (Hero2File) e di livello piu'
// alto del suo file, prima di toglierlo lo si scrive in quel HeroNN.pax, con i blocchi 0xc4 e 0xce e il nome dal file com'era (come
// SaveHero2Now); il file di prima resta in Save\SacredPad_backup. Solo con la co-op attiva (eroe 2 scelto nel menu o creazione
// automatica), che poi crea l'eroe 2 da quel file.
void AdoptSavedHero2(const std::vector<BYTE>& copy, int cls, int level)
{
    if (!g_coopArmed.load() && !g_cfg.nativeAuto) return;
    const int fileNo = g_cfg.nativeHero2;
    char orig[MAX_PATH], tmp[MAX_PATH], bdir[MAX_PATH], bak[MAX_PATH];
    _snprintf_s(orig, sizeof(orig), _TRUNCATE, "%s\\Save\\Hero%02d.pax", GameDir(), fileNo);
    int fLevel = 0, fCls = 0;
    if (!ReadHeroInfo(orig, fLevel, fCls)) { Log("EROE2: recupero dal salvataggio saltato: Hero%02d.pax non leggibile", fileNo); return; }
    if (fCls != cls || level <= fLevel) {
        Log("EROE2: recupero dal salvataggio non fatto: eroe 2 del salvataggio classe %d livello %d, Hero%02d.pax classe %d livello %d",
            cls, level, fileNo, fCls, fLevel);
        return;
    }
    std::vector<BYTE> src, out = copy;
    if (!PaxLoad(orig, src)) { Log("EROE2: recupero dal salvataggio annullato: Hero%02d.pax non leggibile", fileNo); return; }
    const bool okC4 = PaxCopyBlock(src, out, 0xc4, 0x40), okCe = PaxCopyBlock(src, out, 0xce, 100), okNm = PaxCopyName(src, out);
    if (!PaxHas(out, 0xc0, 0xc7) || !PaxHas(out, 0xc3, 0) || out.size() < 4096) {
        Log("EROE2: recupero dal salvataggio annullato: copia non valida (%u byte, blocchi:%s)", static_cast<unsigned>(out.size()), PaxTable(out).c_str());
        return;
    }
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s\\Save\\SacredPad_hero2.tmp", GameDir());
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    const bool written = h != INVALID_HANDLE_VALUE && WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &wr, nullptr) && wr == out.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!written) { Log("EROE2: recupero dal salvataggio: scrittura fallita (%lu)", GetLastError()); return; }
    SYSTEMTIME t; GetLocalTime(&t);
    _snprintf_s(bdir, sizeof(bdir), _TRUNCATE, "%s\\Save\\SacredPad_backup", GameDir());
    CreateDirectoryA(bdir, nullptr);
    _snprintf_s(bak, sizeof(bak), _TRUNCATE, "%s\\Hero%02d_%04u%02u%02u_%02u%02u%02u_prima_del_recupero.pax", bdir, fileNo, t.wYear, t.wMonth, t.wDay,
                t.wHour, t.wMinute, t.wSecond);
    if (!CopyFileA(orig, bak, FALSE)) { Log("EROE2: recupero dal salvataggio: copia di sicurezza fallita (%lu), Hero%02d.pax lasciato com'era", GetLastError(), fileNo); return; }
    if (!MoveFileExA(tmp, orig, MOVEFILE_REPLACE_EXISTING)) { Log("EROE2: recupero dal salvataggio: sostituzione di Hero%02d.pax fallita (%lu)", fileNo, GetLastError()); return; }
    Log("EROE2: progressi recuperati dal salvataggio della partita in Hero%02d.pax: livello %d -> %d (blocchi dall'originale 0xc4 %s, 0xce %s, "
        "nome %s); file di prima in %s", fileNo, fLevel, level, okC4 ? "si" : "no", okCe ? "si" : "no", okNm ? "si" : "no", bak);
    char msg[96]; _snprintf_s(msg, sizeof(msg), _TRUNCATE, Tr("Eroe 2: progressi recuperati dal salvataggio (livello %d)"), level);
    OverlayCoopToast(msg, 6000);
}

// 0.54.0: toglie dal mondo l'eroe 2 arrivato con il salvataggio della partita (vedi ObjectAddr), nel thread del motore.
// Stessa sequenza del comando "loadplayer" del gioco quando l'oggetto 2 esiste gia' (FUN_00614520, 0x614e64: push 0x5fce00(2) e
// cObjectManager::destroy 0x5fc300(2, 1, 0, 0)), piu' la cancellazione con 0x5fadb0(2) che in loadplayer fa subito dopo
// l'allocazione del nuovo oggetto 2 (FUN_005fb530: un oggetto distrutto nello slot viene cancellato). In giocatore singolo destroy
// segna l'oggetto (+0x14 bit 0x10) ma lo libera solo con il bit 0x10000, che gli eroi non hanno (log: 04008000, 04408200), e
// cObjectManager::save (0x602850) salva ogni oggetto del vettore: senza la cancellazione l'eroe 2 tornerebbe nel salvataggio.
// Prima si scrive una copia dell'eroe in Save\SacredPad_backup (fuori dall'elenco dei personaggi): senza copia non si toglie.
void RemoveSavedHero2(DWORD expect)
{
    DWORD mgr = 0, player = 0, local = 0, engine = 0, heroId = 0;
    BYTE net = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player); Rd(kEngine, engine); Rd(kNetGame, net);
    if (player) { Rd(player + 0x14, local); RdHero(player, heroId); }
    const DWORD h2 = RawHero2(mgr);
    if (!h2 || h2 != expect || h2 == g_h2MineAddr.load() || g_h2Spawning.load()) { Log("EROE2: rimozione dell'eroe 2 del salvataggio annullata: l'oggetto 2 e' cambiato"); return; }
    if (net || !local || local == 2 || local != heroId || g_hero2Saving.load()) {
        Log("EROE2: rimozione dell'eroe 2 del salvataggio annullata (rete %u, eroe locale %lu)", net, local);
        return;
    }
    DWORD ty = 0, fl = 0, xp = 0; WORD lvl = 0; int x = 0, y = 0;
    Rd(h2 + 0x10, ty); Rd(h2 + 0x14, fl); Rd(h2 + 0x3b4, xp); Rd(h2 + 0x3FE, lvl); Rd(h2 + 0x1c, x); Rd(h2 + 0x20, y);
    Log("EROE2: eroe 2 arrivato con il salvataggio della partita (%08lX, tipo %lu, livello %u, esperienza %lu, flag %08lX, pos %d,%d): copia e rimozione",
        h2, ty, lvl, xp, fl, x, y);
    // 1. copia di sicurezza (oggetto, inventario ed equipaggiamento dell'eroe 2; statistiche e nome restano quelli dell'eroe 1)
    char bdir[MAX_PATH], rel[MAX_PATH], full[MAX_PATH];
    SYSTEMTIME t; GetLocalTime(&t);
    _snprintf_s(bdir, sizeof(bdir), _TRUNCATE, "%s\\Save\\SacredPad_backup", GameDir());
    CreateDirectoryA(bdir, nullptr);
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "Save\\SacredPad_backup\\Eroe2_partita_%04u%02u%02u_%02u%02u%02u.pax", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", GameDir(), rel);
    std::vector<BYTE> copy;
    if (!WriteHero2Pax(rel, "copia dell'eroe 2 del salvataggio") || !PaxLoad(full, copy) || !PaxHas(copy, 0xc0, 0xc7) || copy.size() < 4096) {
        Log("EROE2: copia di sicurezza non riuscita (%u byte): l'eroe 2 del salvataggio resta nella partita, nascosto alla mod", static_cast<unsigned>(copy.size()));
        OverlayCoopToast(Tr("Eroe 2 del salvataggio non tolto: copia di sicurezza non riuscita"), 6000);
        return;
    }
    if (RawHero2(mgr) != h2) { Log("EROE2: rimozione annullata: l'oggetto 2 e' cambiato durante la copia"); return; }
    AdoptSavedHero2(copy, static_cast<int>(ty), lvl);   // 0.55.37
    // 2. elenco dei giocatori della sessione (si salva con la partita) e comandi del motore
    DWORD cnt = 0, e1 = 0;
    if (Rd(kPlCount, cnt) && Rd(kPlList + 8, e1) && cnt == 2 && e1 == 2) {
        WriteDword(kPlCount, 1);
        WriteDword(kPlList + 8, 0);
        Log("EROE2: elenco giocatori: tolto l'eroe 2 (contatore 1)");
    }
    DWORD ctl = 0;
    if (engine && Rd(engine + 0x13c, ctl) && ctl == 2) WriteDword(engine + 0x13c, heroId);
    // 3. push, destroy, cancellazione
    const int before = CountObjects(mgr);
    if ((fl & 2) == 0) {
        const CallResult p = CallThis1(0x005fce00, mgr, 2);
        if (!p.ok) { LogException("push dell'eroe 2"); return; }
        if (!p.balanced) Log("NATIVO: ATTENZIONE: 0x5fce00 ha lasciato lo stack sbilanciato (corretto)");
    }
    DWORD fd = 0;
    for (DWORD a2 : { 1u, 0u }) {   // come loadplayer (1); con 0 destroy non guarda il proprietario +0x39
        const CallResult d = CallThis4(0x005fc300, mgr, 2, a2, 0, 0);
        if (!d.ok) { LogException("destroy dell'eroe 2"); return; }
        if (!d.balanced) Log("NATIVO: ATTENZIONE: 0x5fc300 ha lasciato lo stack sbilanciato (corretto)");
        DWORD beg = 0, p = 0;
        if (!Rd(mgr + 4, beg) || !Rd(beg + 2 * 4, p) || p != h2) break;     // lo slot non e' piu' l'eroe 2: gia' tolto
        if (Rd(h2 + 0x14, fd) && (fd & 0x10)) break;
        Log("EROE2: destroy(2, %lu, 0, 0) non ha segnato l'oggetto (flag %08lX)", a2, fd);
    }
    DWORD beg = 0, slot = 0;
    Rd(mgr + 4, beg); Rd(beg + 2 * 4, slot);
    if (slot == h2) {
        if (!(fd & 0x10)) { Log("EROE2: ERRORE: l'eroe 2 del salvataggio non e' stato distrutto (flag %08lX): resta fuori dal mondo", fd); return; }
        const CallResult f = CallThis1(0x005fadb0, mgr, 2);
        if (!f.ok) { LogException("cancellazione dell'eroe 2"); return; }
        if (!f.balanced) Log("NATIVO: ATTENZIONE: 0x5fadb0 ha lasciato lo stack sbilanciato (corretto)");
        slot = 0;
        Rd(beg + 2 * 4, slot);
    }
    Log("EROE2: eroe 2 del salvataggio tolto (oggetti %d -> %d, slot 2 %s); copia in %s", before, CountObjects(mgr), slot ? "ANCORA OCCUPATO" : "vuoto", full);
    if (!g_coopArmed.load() && !g_cfg.nativeAuto) OverlayCoopToast(Tr("Partita per un giocatore: eroe 2 del salvataggio tolto"), 6000);
}

// 0.54.1: l'eroe 2 si salva nello stesso momento della partita. cEngine::renderThread::run (0x60e640, thread del motore) salva
// la partita quando [engine+0x54] ha i bit 0xc0: percorso SAVE/GAME%02d.PAK e chiamata a FUN_00619f70 in 0x60e872, l'unica
// chiamata (thiscall, ECX = motore, percorso sullo stack; la funzione scrive cObjectManager::save e SAVEGAME.BMP). La chiamata
// passa da SaveGameStub, che salva prima l'eroe 2 e poi salta alla funzione del gioco con registri e stack intatti. Prima
// (Hero2SaveWatch, rimasto solo come riserva) si guardavano i GAME*.PAK ogni 2 s e si salvava 1,5 s dopo: con "salva ed esci"
// la partita era gia' chiusa (prova del 30/10: GAME10 scritto alle 11:14:41, menu principale alle 11:14:44, eroe 2 mai salvato),
// e dalla 0.54.0 quei progressi non restano neanche nella campagna.
constexpr DWORD kSaveGameCall = 0x0060e872, kSaveGame = 0x00619f70;
volatile DWORD g_saveGameCont = kSaveGame;
std::atomic<bool> g_saveHookOn{ false };

bool Mouse2StowNow(const char* why);
static void DwInstCloseAll(const char* why);   // piu' sotto (0.56.14)
void SaveGameHook()   // dallo stub, nel thread del motore, prima del salvataggio della partita
{
    Mouse2StowNow("salvataggio della partita");   // 0.56.0: prima che la partita (e l'eroe 2) si salvino
    DwInstCloseAll("salvataggio della partita");  // 0.56.14: oggetto e gemme sul piano del fabbro dell'eroe 2 tornano nel suo zaino
    if (!g_cfg.nativeSaveHero2) return;
    DWORD mgr = 0;
    Rd(kObjMgr, mgr);
    if (!mgr || !ObjectAddr(mgr, 2)) return;
    const int fileNo = g_h2MineFile.load();
    if (fileNo < 0) { Log("EROE2: la partita si salva, l'eroe 2 no: non si sa da quale HeroNN.pax viene"); return; }
    if (g_hero2Saving.exchange(true)) { Log("EROE2: la partita si salva mentre l'eroe 2 si sta gia' salvando"); return; }
    Log("EROE2: la partita si sta salvando: prima si salva l'eroe 2 in Hero%02d.pax", fileNo);
    SaveHero2Now("salvataggio della partita", fileNo);
}

__declspec(naked) void SaveGameStub()   // al posto di "call 0x619f70" in 0x60e872: registri, flag e stack come li ha lasciati il gioco
{
    __asm {
        pushad
        pushfd
        call SaveGameHook
        popfd
        popad
        jmp  dword ptr [g_saveGameCont]
    }
}

// Nel thread del motore (lavoro in coda, fra un fotogramma e l'altro): non sta eseguendo 0x60e872 mentre si cambiano i 4 byte.
void InstallSaveHookNow()
{
    BYTE op = 0; LONG rel = 0;
    if (!Rd(kSaveGameCall, op) || !Rd(kSaveGameCall + 1, rel) || op != 0xE8 || kSaveGameCall + 5 + static_cast<DWORD>(rel) != kSaveGame) {
        Log("EROE2: gancio del salvataggio NON installato: %08lX non e' una chiamata a %08lX (byte %02X), resta il controllo dei file", kSaveGameCall, kSaveGame, op);
        return;
    }
    const LONG mine = static_cast<LONG>(reinterpret_cast<DWORD>(&SaveGameStub) - (kSaveGameCall + 5));
    if (!PatchCode(kSaveGameCall + 1, reinterpret_cast<const BYTE*>(&mine), 4)) { Log("EROE2: gancio del salvataggio NON installato: scrittura non riuscita"); return; }
    g_saveHookOn.store(true);
    Log("EROE2: gancio del salvataggio installato in %08lX: l'eroe 2 si salva insieme alla partita", kSaveGameCall);
}

// Riserva: si salva l'eroe 2 quando il gioco ha salvato la partita (file Save\GAMExx.PAK nuovo o modificato), se il gancio non c'e'.
void Hero2SaveWatch(bool inWorld, bool have2, DWORD now)
{
    static DWORD nextScan = 0, pendingAt = 0;
    static FILETIME last{};
    static bool init = false;
    if (!g_cfg.nativeSaveHero2 || g_saveHookOn.load()) return;
    if (pendingAt && static_cast<int>(now - pendingAt) >= 0) {
        pendingAt = 0;
        if (inWorld && have2) SaveHero2File("salvataggio della partita");
    }
    if (static_cast<int>(now - nextScan) < 0) return;
    nextScan = now + 2000;
    char pat[MAX_PATH];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\Save\\GAME*.PAK", GameDir());
    WIN32_FIND_DATAA fd; FILETIME newest{};
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) newest = fd.ftLastWriteTime; } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!init) { init = true; last = newest; return; }
    if (CompareFileTime(&newest, &last) > 0) {
        last = newest;
        if (inWorld && have2) { pendingAt = now + 1500; Log("EROE2: la partita e' stata salvata, tra poco si salva anche l'eroe 2"); }
    }
}

bool WriteWord(DWORD addr, WORD v);   // definita piu' sotto

// Ritratto dell'eroe 2 nel riquadro dei compagni (0.43.0, N15 della sezione 8c). La finestra dei ritratti (cUI_Mercenary,
// n.7 del gestore, vtable 0x8958cc) disegna con il suo metodo +0x14 (0x6d71d0, thiscall ret 4): slot 0 = [player+0x14],
// slot 1..4 = compagni presi dall'elenco dell'eroe (+0x39c..+0x3a0, voci da 9 byte), id a +0x154 + k*0x194; per ogni
// slot pieno chiama 0x6d74b0 (thiscall ret 8: contesto di disegno, slot), che disegna immagine della classe, anello della
// vita e lampeggio. Mettere l'eroe 2 nell'elenco dei compagni lo farebbe trattare dall'IA come un seguace (e 0x54d780 lo
// congederebbe), quindi si sostituisce il metodo nel vtable: dopo l'originale, se c'e' uno slot libero, vi si scrive per un
// attimo l'id dell'eroe 2 (dell'eroe 1 durante lo scambio con Home, quando lo slot 0 e' l'eroe 2), si chiama 0x6d74b0 e
// lo si rimette a zero: un clic su quel ritratto non fa nulla. Disassemblato in docs/dis_ritratti.txt.
typedef void(__fastcall* MercDrawFn)(DWORD self, DWORD edx, DWORD ctx);
MercDrawFn g_mercDrawOrig = nullptr;
std::atomic<DWORD> g_portraitAt{ 0 };       // ultimo fotogramma con il ritratto dell'eroe 2 disegnato
std::atomic<int> g_portraitSlot{ 0 };        // 0.56.29: ritratto (1..4) in cui si disegna l'eroe 2
std::atomic<LONG> g_portraitFaults{ 0 };
constexpr DWORD kMercVtSlot = 0x008958cc + 0x14, kMercDraw = 0x006d71d0, kMercSlotDraw = 0x006d74b0;

// 0.55.61: barretta dell'esperienza (i pallini verdi sotto il ritratto) anche per l'eroe 2 (il tester, 02/11). La costruzione
// dei ritratti (0x6d8330) crea solo per lo slot 0 la traccia (+0x258, a x 8 y +0x6a dal ritratto) e il riempimento (+0x25c,
// y +0x6c); 0x6d74b0 per uno slot che li ha disegna la traccia e poi il riempimento largo (esperienza - inizio del livello) /
// (fine - inizio) * larghezza della traccia (0x57fa60 e 0x57f990 sulle statistiche della creatura dello slot), coordinate
// della texture u 0..73*frazione, v 119..125. Per il disegno del ritratto dell'eroe 2 lo slot aggiunto riceve per un attimo
// le due barrette dello slot 0, spostate della distanza fra i due ritratti; poi tutto torna com'era.
struct MercXpLend {
    DWORD s = 0, trk = 0, fill = 0; bool on = false;
    BYTE save[2][0x40] = {};
    MercXpLend(DWORD self, DWORD slot) : s(slot) {
        DWORD f0 = 0, fk = 0, cur = 0; LONG y0 = 0, yk = 0;
        if (!Rd(self + 0x258, trk) || !Rd(self + 0x25c, fill) || !trk || !fill) return;
        if (!Rd(slot + 0x258, cur) || cur || !Rd(slot + 0x25c, cur) || cur) return;
        if (!Rd(self + 0x250, f0) || !Rd(slot + 0x250, fk) || !f0 || !fk) return;
        if (!Rd(f0 + 0x28, y0) || !Rd(fk + 0x28, yk)) return;
        const LONG dy = yk - y0;
        if (dy <= 0 || dy > 2000) return;
        if (!RdBuf(trk + 0x24, save[0], 0x40) || !RdBuf(fill + 0x24, save[1], 0x40)) return;
        LONG t = 0;
        Rd(trk + 0x28, t); WriteDword(trk + 0x28, static_cast<DWORD>(t + dy));
        Rd(fill + 0x28, t); WriteDword(fill + 0x28, static_cast<DWORD>(t + dy));
        WriteDword(slot + 0x258, trk); WriteDword(slot + 0x25c, fill);
        on = true;
    }
    ~MercXpLend() {
        if (!on) return;
        WriteDword(s + 0x258, 0); WriteDword(s + 0x25c, 0);
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(trk + 0x24)), save[0], 0x40, nullptr);
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(fill + 0x24)), save[1], 0x40, nullptr);
    }
};

void MercDrawExtra(DWORD self, DWORD ctx)
{
    DWORD f = 0, mgr = 0, slot0 = 0;
    if (!Rd(self + 0x10, f) || !(f & 1) || !Rd(kObjMgr, mgr) || !mgr || !Rd(self + 0x154, slot0) || !slot0) return;
    DWORD extra = 0;
    if (slot0 == 2) extra = g_swapOrig.load();          // scambio con Home: sotto si mostra l'eroe 1
    else if (ObjectAddr(mgr, 2)) extra = 2;
    if (!extra || extra == slot0 || !ObjectAddr(mgr, extra)) return;
    for (DWORD k = 1; k <= 4; ++k) {
        const DWORD s = self + k * 0x194;
        DWORD id = 0, e0 = 0, e1 = 0;
        if (!Rd(s + 0x154, id) || id == extra) return;     // gia' disegnato (o compagno vero con lo stesso id)
        if (id) continue;
        if (!Rd(s + 0x250, e0) || !Rd(s + 0x254, e1) || !e0 || !e1) return;
        WriteDword(s + 0x154, extra);
        CallResult cr;
        {
            MercXpLend xp(self, s);
            cr = CallThis2(kMercSlotDraw, self, ctx, k);
        }
        WriteDword(s + 0x154, 0);
        if (!cr.ok) { g_portraitFaults.fetch_add(1); LogException("ritratto dell'eroe 2"); return; }
        if (!cr.balanced) { g_portraitFaults.fetch_add(1); Log("NATIVO: ATTENZIONE: 0x6d74b0 ha lasciato lo stack sbilanciato (corretto)"); }
        g_portraitAt.store(GetTickCount());
        g_portraitSlot.store(static_cast<int>(k));
        return;
    }
}

bool MercHidden(DWORD self);   // 0.55.57: ritratti nascosti con lo zaino dell'eroe 2 aperto (due zaini, piu' avanti)
void __fastcall MercDrawHook(DWORD self, DWORD edx, DWORD ctx)
{
    if (MercHidden(self)) return;
    g_mercDrawOrig(self, edx, ctx);
    if (g_portraitFaults.load() < 3) MercDrawExtra(self, ctx);    // dopo 3 errori il ritratto aggiunto si spegne
}

void InstallPortraitHook()
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    DWORD cur = 0;
    if (!Rd(kMercVtSlot, cur) || cur != kMercDraw) { Log("NATIVO: ritratto dell'eroe 2 non attivato: vtable di cUI_Mercenary inatteso (%08lX)", cur); return; }
    DWORD old = 0;
    void* slot = reinterpret_cast<void*>(static_cast<uintptr_t>(kMercVtSlot));
    if (!VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) { Log("NATIVO: ritratto dell'eroe 2: VirtualProtect fallita (%lu)", GetLastError()); return; }
    g_mercDrawOrig = reinterpret_cast<MercDrawFn>(static_cast<uintptr_t>(cur));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(&MercDrawHook)));
    VirtualProtect(slot, 4, old, &old);
    Log("NATIVO: ritratto dell'eroe 2 nel riquadro dei compagni attivato (vtable cUI_Mercenary +0x14)");
}

// --- 0.52.0 (N4 fase C): barra in basso dell'eroe 2 sempre visibile -------------------------------------------------
// Studio del 29/10: la barra (cUI_Taskbar2, finestra n.0, vtable 0x895ac0, disegno = metodo +0x14 0x6e3e30, thiscall ret 4)
// a ogni fotogramma legge armi, arti e cintura dalla voce dell'inventario in +0x158 (ushort) e l'eroe da +0x154 (6e3550,
// 6e2b00, 6dfd10) e i suoi pezzi prendono la posizione assoluta sommando quella dei genitori (0x731f80): spostando +0x24
// della barra si sposta tutto. Quindi, come per il ritratto, dopo il disegno normale la barra si ridisegna una seconda
// volta di lato con +0x154 = 2, +0x158 = 2 (la voce dell'eroe 2, come nello scambio), le deviazioni dell'interfaccia
// accese (le parti che chiedono l'eroe locale vedono l'eroe 2: vita, mana) e poi tutto si rimette com'era. FUN_006dfd10(1)
// (thiscall ret 4) ricalcola lo slot evidenziato: prima con l'eroe 2, dopo di nuovo con l'eroe 1. Solo disegno: i clic
// sulla barra dell'eroe 2 vanno al mondo. Gli slot visibili sono quanti ne ha l'eroe 1 (+0x15c, si cambiano solo con la
// ricostruzione 0x6e0310). Niente seconda barra durante lo scambio (la barra mostra gia' l'eroe 2).
typedef void(__fastcall* TaskbarDrawFn)(DWORD self, DWORD edx, DWORD ctx);
TaskbarDrawFn g_tbDrawOrig = nullptr;
std::atomic<LONG> g_tbFaults{ 0 };
constexpr DWORD kTbVt = 0x00895ac0, kTbVtSlot = kTbVt + 0x14, kTbDraw = 0x006e3e30, kTbSel = 0x006dfd10;

// 0.52.2: per la seconda barra non si chiama tutto il disegno 0x6e3e30 ma la stessa sequenza senza tre parti che non
// sono dell'eroe e si disegnano in un punto fisso dello schermo (costanti, non la posizione della barra): 0x6e3f20 (il
// cielo dell'occhio, che gira con l'ora del giorno), 0x6e4090 (la "bussola" delle missioni nell'occhio: disegnata due
// volte, le sue onde andavano al doppio della velocita') e il pulsante +0x18c (il "+" in alto a destra), piu' il lampeggio
// finale. Prova della 0.52.1: l'occhio della seconda barra era vuoto proprio per questo. Ogni pezzo e' thiscall ret 4.
// 0.53.7: eye = true anche cielo (0x6e3f20, prima di tutto come nell'originale) e bussola (0x6e4090, dopo il disegno di
// base): la copia dell'eroe 1 durante lo scambio tiene l'occhio, cosi' l'occhio resta sempre sulla barra di sinistra.
bool CallTaskbarDraw(DWORD self, DWORD ctx, bool eye = false)
{
    bool ok = true;
    if (eye) ok = CallThis1(0x006e3f20, self, ctx).ok;
    if (ok) ok = CallThis1(0x006e3550, self, ctx).ok;     // slot, armi e arti
    DWORD b = 0, e = 0;
    if (ok && Rd(self + 0x248, b) && Rd(self + 0x24c, e) && e >= b && e - b < 0x400)
        for (DWORD p = b; p < e && ok; p += 4) {            // figli in [+0x248..+0x24c] (metodo +0x14)
            DWORD c = 0, vt = 0, fn = 0;
            if (Rd(p, c) && c && Rd(c, vt) && Rd(vt + 0x14, fn) && fn) ok = CallThis1(fn, c, ctx).ok;
        }
    if (ok) ok = CallThis1(0x006e2b00, self, ctx).ok;
    DWORD c160 = 0, vt = 0, fn = 0;
    if (ok && Rd(self + 0x160, c160) && c160 && Rd(c160, vt) && Rd(vt + 0x14, fn) && fn) ok = CallThis1(fn, c160, ctx).ok;
    if (ok) ok = CallThis1(0x007272a0, self, ctx).ok;      // disegno di base dei pezzi
    if (ok && eye) ok = CallThis1(0x006e4090, self, ctx).ok;
    if (ok) ok = CallThis1(0x006e3d50, self, ctx).ok;      // cintura
    if (ok) ok = CallThis1(0x006e4680, self, ctx).ok;
    return ok;
}

// 0.52.1: nella prova della 0.52.0 la seconda barra aveva i due cerchi grandi (armi e arti, cUI_Static2 puntati da
// +0x164/+0x178, figli della barra) ma non l'occhio centrale con i cerchi delle pozioni: sono pezzi incorporati
// nell'oggetto della barra (cUI_Button2 a +0x258.. e +0x604.., cUI_Static2 a +0x9B0..) con posizioni assolute, cioe' senza
// la barra come genitore, quindi non seguivano lo spostamento (e si ridisegnavano sopra quelli dell'eroe 1). Si
// spostano anche loro: elenco, una volta per barra, dei pezzi cUI_ nei primi 0x3000 byte e di quelli puntati da
// +0x160..+0x18c il cui genitore (+0x50) non e' la barra; registrato nel log.
struct TbPiece { DWORD addr; int x, y; };
void TaskbarLoosePieces(DWORD self, std::vector<TbPiece>& out)
{
    static DWORD cachedFor = 0;
    static std::vector<DWORD> cached;
    if (cachedFor != self) {
        cachedFor = self;
        cached.clear();
        // 0.52.5: solo i pezzi noti della barra (i 10 cerchi delle pozioni +0x258 + k*0xbc e i due statici +0x9B0, +0xA2C).
        // La 0.52.1 cercava nei primi 0x3000 byte e in una partita nuova (29/10, 17:23) aveva preso anche oggetti che stanno
        // subito dopo la barra in memoria (la finestra dei ritratti cUI_Mercenary a +0xDA8 e quattro statici a (0,0)): i
        // bordi della barra diventavano grandi quanto lo schermo e la seconda barra finiva fuori, sopra.
        static const DWORD kOffs[] = { 0x258, 0x314, 0x3d0, 0x48c, 0x548, 0x604, 0x6c0, 0x77c, 0x838, 0x8f4, 0x9b0, 0xa2c };
        auto consider = [&](DWORD e, const char* where, DWORD off) {
            DWORD vt = 0, par = 0; int x = 0, y = 0;
            if (!e || e == self || !Rd(e, vt) || !IsUiVt(vt) || !Rd(e + 0x50, par) || par == self) return;
            if (std::find(cached.begin(), cached.end(), e) != cached.end()) return;
            Rd(e + 0x24, x); Rd(e + 0x28, y);
            if (par != 0) return;                            // figlio di qualcos'altro: non e' della barra
            cached.push_back(e);
            char cn[40]; ClassName(vt, cn, sizeof(cn));
            Log("NATIVO: barra dell'eroe 2: pezzo senza genitore %s+%03lX %s @%08lX (%d,%d)", where, off, cn, e, x, y);
        };
        for (DWORD off : kOffs) consider(self + off, "", off);
        Log("NATIVO: barra dell'eroe 2: %u pezzi da spostare a parte", static_cast<unsigned>(cached.size()));
    }
    out.clear();
    for (DWORD e : cached) { int x = 0, y = 0; if (Rd(e + 0x24, x) && Rd(e + 0x28, y)) out.push_back({ e, x, y }); }
}

// 0.52.7: ultima volta che la seconda barra e' stata messa in alto (l'overlay sposta la sua scritta a sinistra).
std::atomic<DWORD> g_tbTopAt{ 0 };
bool MemoryHero2TaskbarOnTop() { const DWORD t = g_tbTopAt.load(); return t && GetTickCount() - t < 500; }

// Spostamento della seconda barra: in pixel dall'ini, o il 22% della larghezza dello schermo.
int TaskbarHero2Dx()
{
    if (g_cfg.nativeHero2TaskbarDx) return g_cfg.nativeHero2TaskbarDx;
    DWORD drv = 0; short sw = 0;
    return (Rd(0x00cdb99c, drv) && drv && Rd(drv + 0x20, sw) && sw > 0) ? sw * 22 / 100 : 420;
}

// 0.53.0: le due barre anche durante lo scambio con Home (il tester, 29/10: "mi passa all'eroe 2 facendomi vedere solo la sua
// barra"). La barra vera mostra l'eroe di [+0x154] (l'eroe 1, o l'eroe 2 durante lo scambio della sola interfaccia), la
// copia l'altro: l'eroe 2 fuori dallo scambio, l'eroe di partenza durante. L'eroe 1 sta sempre a sinistra, l'eroe 2 a destra.
struct TbSide { DWORD otherId = 0, otherAddr = 0; bool otherIs2 = true; };
bool TaskbarSides(DWORD self, TbSide& s)
{
    if (!g_cfg.nativeHero2Taskbar || !g_uiPatched.load() || g_saveOrig.load() || g_tbFaults.load() >= 3) return false;
    DWORD vt = 0, f = 0, mgr = 0, id = 0, hp = 0; WORD idx = 0;
    if (!Rd(self, vt) || vt != kTbVt || !Rd(self + 0x10, f) || !(f & 1) || !Rd(self + 0x154, id) || !Rd(self + 0x158, idx)) return false;
    if (!Rd(kObjMgr, mgr) || !mgr) return false;
    // 0.53.7: i lati seguono l'eroe che la barra mostra (+0x154/+0x158), senza aspettare g_swapUiLive. Prima, fra la
    // scrittura dei campi della barra (RetargetInventoryUi, thread della mod) e il prossimo EndScene (UiSwapLatch), per un
    // fotogramma la barra vera tornava al centro da sola: il "cambio totale" visto dal tester il 30/10 a ogni scambio.
    // Con lo scambio della 0.50 (SwapUi=false, eroe locale 2, mondo fermo) niente seconda barra, come prima.
    if (id == 2 && idx == 2) {
        if (!g_cfg.nativeSwapUi) return false;
        DWORD loc = g_swapOrig.load();
        if (!loc) { DWORD player = 0; if (Rd(kPlayer, player) && player) RdHero(player, loc); }   // ritorno in corso
        if (!loc || loc == 2) return false;
        s.otherId = loc; s.otherIs2 = false;
    }
    else if (id != 2 && idx != 2) { s.otherId = 2; s.otherIs2 = true; }
    else return false;                                  // campi a meta' scrittura
    s.otherAddr = ObjectAddr(mgr, s.otherId);
    if (!s.otherAddr || !Rd(s.otherAddr + 0x4d8, hp) || hp == 0) return false;
    // 0.53.5: niente seconda barra (e niente costruzione del suo fondo) finche' l'eroe 2 non c'e' da 2 secondi: crash del
    // 30/10 alle 01:03:24 subito dopo la comparsa. Durante SpawnHero2 il gioco ridisegna la barra dentro il comando della
    // console (log: righe della barra fra "carico hero07.pax" e "console eseguita").
    const DWORD h2 = s.otherIs2 ? s.otherAddr : ObjectAddr(mgr, 2);
    return h2 && g_h2SeenAddr.load() == h2 && GetTickCount() - g_h2SeenAt.load() >= 2000;
}

// Slot sbloccati di un eroe dal livello (+0x3fe; 1, 2, 8, 16, 30, come 0x57fb40).
WORD SlotCountOf(DWORD hero)
{
    WORD lvl = 0;
    if (!hero || !Rd(hero + 0x3fe, lvl)) return 0;
    static const int kAt[5] = { 1, 2, 8, 16, 30 };
    WORD n = 0;
    for (int at : kAt) if (lvl >= at) ++n;
    return n;
}

// La barra con l'altro eroe al posto del suo (campi +0x154/+0x158 e deviazioni dell'interfaccia) per la durata di una
// chiamata, nel thread del motore. Per l'eroe 2 le deviazioni puntano a lui; per l'eroe di partenza (durante lo scambio)
// si spengono, cosi' le letture tornano all'eroe locale vero. 0.53.0: anche il numero di slot e' quello dell'altro eroe
// (prova della 0.52.9: la barra di destra aveva uno slot come l'eroe 1 anche se l'eroe 2 ne aveva due), ricostruito con
// 0x6e0310 solo se diverso, e rimesso da Restore().
// 0.53.1: nella prova della 0.53.0 la barra di destra sfarfallava: 0x6e0310 a ogni fotogramma (due volte) distrugge e
// ricrea i pezzi del fondo (vettore +0x248..+0x250) e mostra/nasconde gli slot con le notifiche del metodo +0x24. Ora gli
// slot si impostano come li imposta 0x6e0310 (numero in +0x15c; statici delle armi +0x164.. visibili, bit 0 e 2 di +0x10,
// per k < n, a x = 0x18a + 66 * (k - n), y = 15; statici delle arti +0x178.. visibili per k < n) scrivendo i campi, e li
// si rimette dopo; il fondo della copia e' un secondo vettore di pezzi tenuto da parte (CopyTiles, piu' avanti).
// emulate = false: nessun cambio di slot (per costruire quel fondo).
WORD SetWord(DWORD addr, WORD v) { WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(addr)), &v, 2, nullptr); return v; }
struct TbOtherCtx {
    DWORD self, id, oldHero, oldOn, oldFake; WORD idx, count, applied; bool wasSwap;
    struct St { DWORD p, f; int x, y; } st[10] = {};
    TbOtherCtx(DWORD s, const TbSide& sd, bool emulate = true) : self(s), id(0), oldHero(g_uiHero), oldOn(g_uiOn), oldFake(g_uiFakePlayer[0x14 / 4]),
        idx(0), count(0), applied(0), wasSwap(g_swapUiLive.load()) {
        g_tbCtxOn.fetch_add(1);
        g_tbShifting.fetch_add(1);                      // MemoryTaskbarArts e ganci annidati: barra non al suo stato
        Rd(self + 0x154, id); Rd(self + 0x158, idx); Rd(self + 0x15c, count);
        applied = count;
        if (sd.otherIs2) { g_uiFakePlayer[0x14 / 4] = 2; g_uiHero = sd.otherAddr; g_uiOn = 1; }
        else { g_uiHero = 0; g_uiOn = 0; }
        WriteDword(self + 0x154, sd.otherId);
        SetWord(self + 0x158, static_cast<WORD>(sd.otherId));
        const WORD n = SlotCountOf(sd.otherAddr);
        if (emulate && n >= 1 && n <= 5 && n != count) ApplySlots(n);
    }
    void ApplySlots(WORD n) {
        for (int k = 0; k < 10; ++k) {
            St& s = st[k];
            if (!Rd(self + 0x164 + k * 4, s.p) || !s.p || !Rd(s.p + 0x10, s.f) || !Rd(s.p + 0x24, s.x) || !Rd(s.p + 0x28, s.y)) { s.p = 0; continue; }
            const bool on = (k < 5 ? k : k - 5) < n;
            WriteDword(s.p + 0x10, on ? (s.f | 5) : (s.f & ~5u));
            if (k < 5 && on) {
                WriteDword(s.p + 0x24, static_cast<DWORD>(static_cast<int>(static_cast<short>(0x18a + 66 * (k - n)))));
                WriteDword(s.p + 0x28, 0xf);
            }
        }
        SetWord(self + 0x15c, n);
        applied = n;
    }
    void UndoSlots() {
        for (const St& s : st) {
            if (!s.p) continue;
            DWORD f = 0;
            if (Rd(s.p + 0x10, f)) WriteDword(s.p + 0x10, (f & ~5u) | (s.f & 5u));
            WriteDword(s.p + 0x24, static_cast<DWORD>(s.x)); WriteDword(s.p + 0x28, static_cast<DWORD>(s.y));
        }
        SetWord(self + 0x15c, count);
    }
    void Restore() {
        WORD now = 0;
        Rd(self + 0x15c, now);
        // se lo scambio e' cominciato o finito nel frattempo, la barra deve restare sull'eroe della copia (l'altro rispetto a
        // prima): i campi restano come li ha messi il costruttore, e se gli slot erano solo impostati qui la barra si
        // ricostruisce davvero per quell'eroe. 0.53.2: g_swapUiLive cambia solo in EndScene, fuori dal disegno della barra,
        // quindi questo ramo non dovrebbe piu' servire; resta per sicurezza.
        if (wasSwap != g_swapUiLive.load()) {
            if (!g_swapUiLive.load()) { g_uiHero = 0; g_uiOn = 0; }
            if (applied != count && now == applied) { UndoSlots(); CallThis1(0x006e0310, self, applied); }
            CallThis1(kTbSel, self, 1);
            g_tbCtxOn.fetch_sub(1);
            g_tbShifting.fetch_sub(1);
            return;
        }
        SetWord(self + 0x158, idx);
        WriteDword(self + 0x154, id);
        g_uiHero = oldHero; g_uiOn = oldOn; g_uiFakePlayer[0x14 / 4] = oldFake;
        if (applied != count && now == applied) UndoSlots();              // slot impostati qui
        else if (now != count && count >= 1 && count <= 5) CallThis1(0x006e0310, self, count);   // cambiati dal gioco
        CallThis1(kTbSel, self, 1);
        g_tbCtxOn.fetch_sub(1);
        g_tbShifting.fetch_sub(1);
    }
};

// Fondo della copia (0.53.1): costruito una volta per barra e numero di slot dell'altro eroe con 0x6e0310 (l'altro eroe al
// suo posto), poi il vettore +0x248 si prende cosi' com'e' e si lascia vuoto (stato del vettore vuoto: tre puntatori a 0),
// e la barra si ricostruisce per il suo eroe. I pezzi hanno la barra come genitore (+0x50) e non sono registrati altrove
// (costruttore 0x7533a0/0x731730, disegnati solo da quel vettore). Quelli vecchi (altro numero di slot, barra di una
// partita precedente) si lasciano in memoria: pochi oggetti, nessun rischio di liberarli mentre il gioco li usa.
struct CopyTiles { DWORD bar = 0; WORD n = 0; DWORD v[3] = {}; };
CopyTiles g_ct[6];                                  // per numero di slot: si riusano a ogni scambio
bool BuildCopyTiles(DWORD self, const TbSide& sd, WORD n)
{
    WORD count = 0;
    Rd(self + 0x15c, count);
    bool ok = false;
    DWORD v[3] = {};
    if (n >= count && count >= 1) {
        // 0.53.5: con piu' slot dell'eroe vero si costruisce con l'eroe vero al suo posto (lo slot scelto, < count, non
        // supera n: 0x6e0310 non cambia arma ne' arte), senza leggere la voce dell'inventario dell'altro eroe, che subito
        // dopo la comparsa potrebbe non essere pronta; poi la barra si ricostruisce con il suo numero.
        const CallResult a = CallThis1(0x006e0310, self, n);
        ok = a.ok && RdBuf(self + 0x248, v, sizeof(v)) && v[1] >= v[0] && v[1] - v[0] < 0x400 && v[2] >= v[1];
        if (ok) {
            g_ct[n].bar = self; g_ct[n].n = n; memcpy(g_ct[n].v, v, sizeof(v));
            WriteDword(self + 0x248, 0); WriteDword(self + 0x24c, 0); WriteDword(self + 0x250, 0);
        }
        CallThis1(0x006e0310, self, count);
        CallThis1(kTbSel, self, 1);
        Log("NATIVO: fondo della barra dell'eroe %lu con %u slot %s (%lu pezzi)", sd.otherId, n, ok ? "preparato" : "NON preparato",
            ok ? (v[1] - v[0]) / 4 : 0);
        return ok;
    }
    TbOtherCtx c(self, sd, false);
    const CallResult a = CallThis1(0x006e0310, self, n);
    ok = a.ok && RdBuf(self + 0x248, v, sizeof(v)) && v[1] >= v[0] && v[1] - v[0] < 0x400 && v[2] >= v[1];
    if (ok) {
        g_ct[n].bar = self; g_ct[n].n = n; memcpy(g_ct[n].v, v, sizeof(v));
        WriteDword(self + 0x248, 0); WriteDword(self + 0x24c, 0); WriteDword(self + 0x250, 0);
    }
    c.Restore();                                        // +0x15c = n diverso: ricostruisce la barra per il suo eroe
    Log("NATIVO: fondo della barra dell'eroe %lu con %u slot %s (%lu pezzi)", sd.otherId, n, ok ? "preparato" : "NON preparato",
        ok ? (v[1] - v[0]) / 4 : 0);
    return ok;
}

// Bordi della barra dell'eroe 1 (pezzi visibili: i 10 cerchi grandi puntati da +0x164..+0x188 e i pezzi incorporati
// senza genitore tranne il "+" in alto a destra, +0x18c), per sapere se un clic cade sulla seconda barra.
bool TaskbarBounds(DWORD self, RECT& r)
{
    std::vector<DWORD> pcs;
    for (DWORD off = 0x164; off <= 0x188; off += 4) { DWORD p = 0; if (Rd(self + off, p) && p) pcs.push_back(p); }
    std::vector<TbPiece> loose;
    TaskbarLoosePieces(self, loose);
    DWORD plus = 0; Rd(self + 0x18c, plus);
    for (const TbPiece& pc : loose) if (pc.addr != plus && pc.addr != self + 0x18c) pcs.push_back(pc.addr);
    bool any = false;
    for (DWORD p : pcs) {
        DWORD f = 0; short wh[2] = {}; int x = 0, y = 0;
        if (!Rd(p + 0x10, f) || !(f & 1) || !RdBuf(p + 0x2c, wh, sizeof(wh)) || wh[0] <= 0 || wh[1] <= 0) continue;
        AbsPos(p, x, y);
        if (!any) { r = RECT{ x, y, x + wh[0], y + wh[1] }; any = true; }
        else { r.left = (std::min)(r.left, (LONG)x); r.top = (std::min)(r.top, (LONG)y); r.right = (std::max)(r.right, (LONG)(x + wh[0])); r.bottom = (std::max)(r.bottom, (LONG)(y + wh[1])); }
    }
    if (any) { r.left -= 12; r.top -= 12; r.right += 12; r.bottom += 12; }
    return any;
}

// 0.52.8: cielo e bussola dell'occhio. Si disegnano in 0x6e3f20 (cielo, x = [0x895b08] in due punti) e 0x6e4090 (bussola
// delle missioni, x = [0x88f6fc] in cinque punti, piu' l'immediato 512.0 passato all'effetto 0x75e320) a una x fissa, non
// dalla posizione della barra: con la barra spostata l'occhio resterebbe al centro. Le sette istruzioni fadd/fsubr si fanno
// leggere da float della mod (0x88f6fc e' condivisa con il resto del gioco, quindi non la si tocca), di valore uguale a
// quello originale letto in memoria; durante il disegno della barra dell'eroe 1 spostata vi si aggiunge lo spostamento.
// La scala fra pixel e unita' dell'occhio si ricava dal centro del cerchio della pozione centrale (+0x3d0), che sta sopra
// il centro dell'occhio: sul disco il cielo e' a 513, a 1920 il cerchio e' centrato a 961.
float g_eyeVal[2] = {}, g_eyeBase[2] = {};   // 0 cielo, 1 bussola
float g_eyeImmBase = 0.0f;
bool g_eyeOk = false;
constexpr DWORD kEyeImm = 0x006e4629;        // immediato di push 0x44000000 a 0x6e4628
void PatchTaskbarEye()
{
    struct Site { DWORD at; BYTE op; DWORD want; int slot; };
    static const Site kSites[] = {
        { 0x006e3fc9, 0x05, 0x00895b08, 0 }, { 0x006e4045, 0x05, 0x00895b08, 0 },
        { 0x006e42c1, 0x05, 0x0088f6fc, 1 }, { 0x006e43af, 0x05, 0x0088f6fc, 1 }, { 0x006e442f, 0x05, 0x0088f6fc, 1 },
        { 0x006e44d6, 0x05, 0x0088f6fc, 1 }, { 0x006e457d, 0x05, 0x0088f6fc, 1 } };
    for (const Site& s : kSites) {
        BYTE b[2] = {}; DWORD op = 0;
        if (!RdBuf(s.at, b, 2) || b[0] != 0xd8 || b[1] != s.op || !Rd(s.at + 2, op) || op != s.want) {
            Log("NATIVO: occhio della barra non spostabile: istruzione inattesa a %08lX (%02X %02X %08lX)", s.at, b[0], b[1], op);
            return;
        }
    }
    // 0.52.9: l'immediato non e' piu' 512.0 quando la mod si installa: pureHD lo riscrive in memoria con la meta' della
    // larghezza dello schermo (prova della 0.52.8: 0x44700000 = 960.0 a 1920), quindi si accetta qualunque x plausibile.
    BYTE push = 0; DWORD imm = 0; float immF = 0.0f;
    if (Rd(kEyeImm, imm)) memcpy(&immF, &imm, 4);
    if (!Rd(kEyeImm - 1, push) || push != 0x68 || !(immF >= 100.0f && immF <= 8000.0f)) {
        Log("NATIVO: occhio della barra non spostabile: push inatteso a 0x6e4628 (%02X %08lX)", push, imm);
        return;
    }
    if (!Rd(0x00895b08, g_eyeBase[0]) || !Rd(0x0088f6fc, g_eyeBase[1])) return;
    g_eyeImmBase = immF;
    g_eyeVal[0] = g_eyeBase[0]; g_eyeVal[1] = g_eyeBase[1];
    // pagina lasciata scrivibile: l'immediato si riscrive a ogni disegno spostato
    DWORD old = 0;
    void* page = reinterpret_cast<void*>(static_cast<uintptr_t>(0x006e3fc9));
    if (!VirtualProtect(page, kEyeImm + 4 - 0x006e3fc9, PAGE_EXECUTE_READWRITE, &old)) {
        Log("NATIVO: occhio della barra: VirtualProtect fallita (%lu)", GetLastError());
        return;
    }
    for (const Site& s : kSites)
        *reinterpret_cast<DWORD*>(static_cast<uintptr_t>(s.at + 2)) = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&g_eyeVal[s.slot]));
    FlushInstructionCache(GetCurrentProcess(), page, kEyeImm + 4 - 0x006e3fc9);
    g_eyeOk = true;
    Log("NATIVO: occhio della barra spostabile (cielo %.1f, bussola %.1f, effetto %.1f)", g_eyeBase[0], g_eyeBase[1], g_eyeImmBase);
}

// 0.52.9: le basi si rileggono a ogni chiamata dalle costanti originali (pureHD potrebbe riscriverle, per esempio a un
// cambio di risoluzione); per l'immediato, un valore diverso dall'ultimo scritto dalla mod e' una nuova base.
void SetEyeShift(float d)
{
    if (!g_eyeOk) return;
    static float lastImm = 0.0f;
    float* imm = reinterpret_cast<float*>(static_cast<uintptr_t>(kEyeImm));
    float s = 0.0f, c = 0.0f;
    if (Rd(0x00895b08, s) && s >= 100.0f && s <= 8000.0f) g_eyeBase[0] = s;
    if (Rd(0x0088f6fc, c) && c >= 100.0f && c <= 8000.0f) g_eyeBase[1] = c;
    if (*imm != lastImm && *imm >= 100.0f && *imm <= 8000.0f) g_eyeImmBase = *imm;
    g_eyeVal[0] = g_eyeBase[0] + d;
    g_eyeVal[1] = g_eyeBase[1] + d;
    lastImm = g_eyeImmBase + d;
    *imm = lastImm;
}

// Unita' dell'occhio per pixel (vedi sopra); 0 se non si puo' sapere.
float EyeUnitsPerPixel(DWORD self)
{
    const DWORD mid = self + 0x3d0;
    short wh[2] = {}; int x = 0, y = 0;
    if (!RdBuf(mid + 0x2c, wh, sizeof(wh)) || wh[0] <= 0) return 0.0f;
    AbsPos(mid, x, y);
    const float c = x + wh[0] / 2.0f;
    if (c < 100.0f) return 0.0f;
    const float k = g_eyeBase[0] / c;
    return (k > 0.2f && k < 3.0f) ? k : 0.0f;
}

// 0.52.8: disposizione delle due barre (il tester, 29/10, dopo accanto, sopra, angolo e in alto: "la barra del giocatore 1 a
// sinistra, si deve comportare esattamente come farebbe al centro, e la barra del giocatore 2 a destra"). dxL sposta la
// barra dell'eroe 1 (0 = al centro come nel gioco), dx2/dy2 la copia dell'eroe 2 rispetto alla barra al centro. Con
// Hero2TaskbarOffset diverso da 0 resta lo spostamento orizzontale a mano della 0.52.0; se le due barre non ci stanno
// affiancate (schermo stretto) o l'occhio non si puo' spostare, la seconda va in alto come nella 0.52.7.
// Da chiamare con la barra al suo posto (non durante uno spostamento). 0.53.0: la barra vera va a sinistra se mostra l'eroe
// 1 e a destra se mostra l'eroe 2 (scambio); la x della copia ai lati si calcola nel suo disegno, dai suoi bordi (con i suoi
// slot). 0.52.9, prova: le righe "misure mancanti" e la scritta della mod che saltava aprendo lo zaino venivano da chiamate
// annidate durante uno spostamento (il gestore della barra che richiama se stesso): ora i ganci le passano dritte.
struct TbLayout { int dxReal = 0; bool sides = false; int copyDx = 0, copyDy = 0; };
short ScreenW()
{
    DWORD drv = 0; short sw = 0;
    return (Rd(0x00cdb99c, drv) && drv && Rd(drv + 0x20, sw) && sw > 0) ? sw : 0;
}
TbLayout TaskbarLayout(DWORD self, const TbSide& sd)
{
    TbLayout L;
    if (g_cfg.nativeHero2TaskbarDx) { L.copyDx = g_cfg.nativeHero2TaskbarDx; return L; }
    RECT b{};
    if (!TaskbarBounds(self, b)) { L.copyDx = TaskbarHero2Dx(); return L; }
    const short sw = ScreenW();
    const int w = b.right - b.left;                      // con i 12 pixel di margine per lato
    static int logged = 0x7fffffff;
    const float k = EyeUnitsPerPixel(self);
    if (g_cfg.nativeHero1TaskbarLeft && g_eyeOk && sw > 0 && 2 * w <= sw && k > 0.0f) {
        L.sides = true;
        L.dxReal = sd.otherIs2 ? -b.left : sw - b.right; // pezzi a 12 pixel dal bordo
        if (logged != L.dxReal) {
            logged = L.dxReal;
            int x = 0, y = 0; short wh[2] = {};
            Rd(self + 0x24, x); Rd(self + 0x28, y); RdBuf(self + 0x2c, wh, sizeof(wh));
            Log("NATIVO: barre ai lati: barra vera (eroe %s) %+d (pezzi %ld..%ld, schermo %d, finestra %d,%d %dx%d, %.4f unita' dell'occhio per pixel)",
                sd.otherIs2 ? "1" : "2", L.dxReal, b.left + 12, b.right - 12, sw, x, y, wh[0], wh[1], k);
        }
        return L;
    }
    if (logged != 0) {
        logged = 0;
        Log("NATIVO: barre ai lati non possibili (%s): seconda barra in alto", !g_cfg.nativeHero1TaskbarLeft ? "Hero1TaskbarLeft=false" :
            !g_eyeOk ? "occhio non spostabile" : (sw <= 0 || k <= 0.0f) ? "misure mancanti" : "schermo troppo stretto");
    }
    // 0.52.7: in alto al centro, bordo superiore dei pezzi a 4 pixel dal bordo dello schermo
    L.copyDy = 4 - b.top;
    g_tbTopAt.store(GetTickCount());
    return L;
}

// Spostamento temporaneo della barra dell'eroe 1 (finestra e pezzi senza genitore; i figli la seguono). Al ritorno si
// rimette ogni valore solo se e' ancora quello scritto: se il gioco nel frattempo ha riposizionato un pezzo, vale il suo.
// 0.53.0: anche in verticale (copia in alto) e con il contatore g_tbShifting acceso anche a spostamento nullo: i ganci
// chiamati dentro (il gestore della barra che richiama se stesso) vanno dritti all'originale.
struct TbShift {
    DWORD self; int dx, dy, x0, y0;
    std::vector<TbPiece> loose;
    bool moved = false;
    TbShift(DWORD s, int ddx, int ddy = 0) : self(s), dx(ddx), dy(ddy), x0(0), y0(0) {
        g_tbShifting.fetch_add(1);
        if ((!dx && !dy) || !Rd(self + 0x24, x0) || !Rd(self + 0x28, y0)) return;
        TaskbarLoosePieces(self, loose);
        moved = true;
        WriteDword(self + 0x24, static_cast<DWORD>(x0 + dx)); WriteDword(self + 0x28, static_cast<DWORD>(y0 + dy));
        for (const TbPiece& pc : loose) { WriteDword(pc.addr + 0x24, static_cast<DWORD>(pc.x + dx)); WriteDword(pc.addr + 0x28, static_cast<DWORD>(pc.y + dy)); }
    }
    ~TbShift() {
        if (moved) {
            int v = 0;
            for (const TbPiece& pc : loose) {
                if (Rd(pc.addr + 0x24, v) && v == pc.x + dx) WriteDword(pc.addr + 0x24, static_cast<DWORD>(pc.x));
                if (Rd(pc.addr + 0x28, v) && v == pc.y + dy) WriteDword(pc.addr + 0x28, static_cast<DWORD>(pc.y));
            }
            if (Rd(self + 0x24, v) && v == x0 + dx) WriteDword(self + 0x24, static_cast<DWORD>(x0));
            if (Rd(self + 0x28, v) && v == y0 + dy) WriteDword(self + 0x28, static_cast<DWORD>(y0));
        }
        g_tbShifting.fetch_sub(1);
    }
};

// Rettangolo della copia sullo schermo e suo spostamento, dall'ultimo disegno (per i clic, nello stesso thread).
RECT g_tbCopyRect{};
int g_tbCopyDx = 0, g_tbCopyDy = 0;
bool g_tbCopyOk = false;
// 0.55.67 (controllo): il punto cade sulla barra dell'eroe 2 (la copia disegnata, a destra), per il puntatore del giocatore 2
// che deve posare li' un'arte o una pozione presa dal suo zaino (i clic sulla copia li traduce TaskbarInputHook per l'eroe 2).
bool MemoryHero2BarPoint(LONG x, LONG y)
{
    if (!g_tbCopyOk || g_swapUiLive.load()) return false;
    const RECT r = g_tbCopyRect;
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Copia dell'altro eroe (0.52.0, 0.53.0 per tutti e due i lati): l'altro eroe al posto del suo con i suoi slot, bordi
// calcolati cosi', spostamento al lato suo (o quello fisso di L), disegno senza occhio (CallTaskbarDraw), tutto rimesso.
void TaskbarDrawCopy(DWORD self, DWORD ctx, const TbSide& sd, const TbLayout& L, bool eye = false)
{
    WORD count = 0;
    Rd(self + 0x15c, count);
    const WORD n2 = SlotCountOf(sd.otherAddr);
    bool tiles = false;                                 // fondo da scambiare con quello tenuto da parte
    if (n2 >= 1 && n2 <= 5 && n2 != count) {
        static DWORD failedFor = 0; static WORD failedN = 0;
        if (g_ct[n2].bar != self && !(failedFor == self && failedN == n2) && !BuildCopyTiles(self, sd, n2)) { failedFor = self; failedN = n2; }
        tiles = g_ct[n2].bar == self;
    }
    TbOtherCtx c(self, sd);
    DWORD own[3] = {};
    if (tiles && RdBuf(self + 0x248, own, sizeof(own))) {
        WriteDword(self + 0x248, g_ct[n2].v[0]); WriteDword(self + 0x24c, g_ct[n2].v[1]); WriteDword(self + 0x250, g_ct[n2].v[2]);
    }
    else tiles = false;
    const CallResult a = CallThis1(kTbSel, self, 1);
    int dx = L.copyDx, dy = L.copyDy;
    RECT bo{};
    const bool haveB = TaskbarBounds(self, bo);
    if (L.sides && haveB) {
        const short sw = ScreenW();
        dx = sd.otherIs2 ? sw - bo.right : -bo.left;
    }
    bool drawn = false;
    const float ek = (eye && dy == 0) ? EyeUnitsPerPixel(self) : 0.0f;   // con la barra ancora al centro
    if (a.ok && haveB) {
        TbShift sh(self, dx, dy);
        if (ek > 0.0f) SetEyeShift(dx * ek);
        drawn = CallTaskbarDraw(self, ctx, ek > 0.0f);
        if (ek > 0.0f) SetEyeShift(0.0f);
        g_tbCopyRect = RECT{ bo.left + dx, bo.top + dy, bo.right + dx, bo.bottom + dy };
        g_tbCopyDx = dx; g_tbCopyDy = dy; g_tbCopyOk = true;
    }
    {   // 0.55.27: simboli dei tasti del pad 2 sopra le arti della barra dell'eroe 2 (prova del 31/10 sulla Steam Machine: "nella
        // barra dell'eroe 2 non si vedono le icone del pad"), come MemoryTaskbarArts per la barra vera: cerchi visibili delle arti
        // (+0x178..) con gli slot dell'eroe 2 ancora impostati (c), barra al centro, piu' lo spostamento della copia. Solo con il
        // pad 2 in modalita' gioco (non con il puntatore, l'aiuto o il menu), come quelli del pad 1 solo fuori dalla modalita' cursore.
        POINT lc[5] = {};
        int ln = 0;
        if (sd.otherIs2 && drawn && NativePad2InGame()) {
            for (int k = 0; k < 5; ++k) {
                DWORD e = 0, vt = 0, f = 0; short wh[2] = {};
                int x = 0, y = 0;
                if (!Rd(self + 0x178 + k * 4, e) || !e || !Rd(e, vt) || vt != 0x89669c || !Rd(e + 0x10, f) || !(f & 1)) break;
                if (!RdBuf(e + 0x2C, wh, sizeof(wh))) break;
                AbsPos(e, x, y);
                lc[ln++] = POINT{ x + dx + wh[0] / 2, y + dy + wh[1] / 2 };
            }
        }
        OverlaySetSlotLabels2(ln, lc);
        // 0.56.23: e i cerchi delle pozioni della barra dell'eroe 2 (cura, croce giu', croce su), come MemoryTaskbarPotions
        POINT pc[3] = {};
        int pn = 0;
        if (sd.otherIs2 && drawn && NativePad2InGame()) {
            const int idx[3] = { 0, g_cfg.dpadPotion[1] - 1, g_cfg.dpadPotion[0] - 1 };
            for (int i = 0; i < 3; ++i) {
                const DWORD e = self + 0x258 + idx[i] * 0xBC;
                DWORD vt = 0; short wh[2] = {}; int x = 0, y = 0;
                if (!Rd(e, vt) || vt != 0x00896308 || !RdBuf(e + 0x2C, wh, sizeof(wh)) || wh[0] <= 0) break;
                AbsPos(e, x, y);
                pc[pn++] = POINT{ x + dx + wh[0] / 2, y + dy + wh[1] / 2 };
            }
            if (pn != 3) pn = 0;
        }
        OverlaySetPotionLabels(2, pn, pc);
    }
    if (tiles) {                                        // il disegno puo' aver spostato i pezzi nel vettore: si riprende
        RdBuf(self + 0x248, g_ct[n2].v, sizeof(g_ct[n2].v));
        WriteDword(self + 0x248, own[0]); WriteDword(self + 0x24c, own[1]); WriteDword(self + 0x250, own[2]);
    }
    c.Restore();
    if (!a.ok || (haveB && !drawn)) {
        g_tbFaults.fetch_add(1);
        Log("NATIVO: ATTENZIONE: barra dell'altro eroe: errore nel disegno (%s), %ld di 3 prima di spegnerla",
            !a.ok ? "0x6dfd10" : "disegno", g_tbFaults.load());
    }
}

// 0.52.8: con la seconda barra attiva, quella vera si disegna spostata (TbShift) con l'occhio spostato insieme
// (SetEyeShift), poi la copia dell'altro eroe con la barra di nuovo al centro.
void __fastcall TaskbarDrawHook(DWORD self, DWORD edx, DWORD ctx)
{
    if (g_tbShifting.load()) { g_tbDrawOrig(self, edx, ctx); return; }   // chiamata annidata
    TbSide sd;
    if (!TaskbarSides(self, sd)) {
        g_tbDxL.store(0); g_tbCopyOk = false;
        g_tbDrawOrig(self, edx, ctx);
        return;
    }
    const TbLayout L = TaskbarLayout(self, sd);
    g_tbDxL.store(L.dxReal);
    // 0.53.7: durante lo scambio (barra vera dell'eroe 2, a destra) cielo e bussola si disegnano nella copia dell'eroe 1, a
    // sinistra, e quelli della barra vera fuori dallo schermo: l'occhio non salta piu' da una barra all'altra (il tester, 30/10:
    // aprendo lo zaino dell'eroe 2 "la barra sotto fa tipo un cambio totale").
    const bool eyeOnCopy = L.sides && !sd.otherIs2 && g_eyeOk;
    {
        const float k = L.dxReal ? EyeUnitsPerPixel(self) : 0.0f;   // con la barra ancora al centro
        TbShift sh(self, L.dxReal);
        if (eyeOnCopy) SetEyeShift(30000.0f);           // fuori dallo schermo
        else if (L.dxReal) SetEyeShift(L.dxReal * k);
        g_tbDrawOrig(self, edx, ctx);
        if (L.dxReal || eyeOnCopy) SetEyeShift(0.0f);
    }
    TaskbarDrawCopy(self, ctx, sd, L, eyeOnCopy);
}

// 0.52.3: clic sulla seconda barra (il tester, 29/10: "non essendo cliccabile puo' dar fastidio"). Il gestore dei clic della
// barra (metodo +0x10, 0x6e1ce0, thiscall ret 4, restituisce un char "gestito") riceve un evento con il tipo dal suo metodo
// +4 (2 = clic del mouse), il tasto in [1] (0 sinistro, 2 destro) e la posizione in [2], [3]. Un clic che cade nei bordi
// della barra dell'eroe 1 spostati come la seconda barra si passa all'originale riportato nella barra vera (x - dx) e con
// l'eroe 2 al posto dell'eroe 1: scelta di armi e arti, oggetto posato su uno slot. Le pozioni no: il clic su una pozione
// manda al motore l'evento 0x17, eseguito dopo, quando l'eroe 2 non e' piu' al suo posto: per i cerchi delle pozioni
// (+0x258 + k * 0xbc, come in 0x6e1ce0) si chiama direttamente UseBeltHero2(k + 1) (tipo = numero del cerchio).
typedef char(__fastcall* TaskbarInputFn)(DWORD self, DWORD edx, DWORD ev);
typedef int(__fastcall* EvTypeFn)(DWORD ev, DWORD edx);
TaskbarInputFn g_tbInputOrig = nullptr;
constexpr DWORD kTbInput = 0x006e1ce0;
void UseBeltHeroId(int kind, DWORD id);

// 0.52.8: eventi per la barra dell'eroe 1 spostata a sinistra: il gestore originale li riceve con la barra spostata come
// nel disegno, cosi' i suoi controlli (clic, passaggio del mouse, oggetti posati) valgono dove la barra si vede.
char CallTaskbarInputReal(DWORD self, DWORD edx, DWORD ev, int dx)
{
    TbShift sh(self, dx);
    return g_tbInputOrig(self, edx, ev);
}

// 0.53.0: clic sulla copia, con l'altro eroe al suo posto e i suoi slot. Se il punto (riportato nella barra al centro) cade
// su uno slot visibile di armi o arti (+0x164..+0x188) lo gestisce l'originale, come nella 0.52.3; altrimenti restituisce
// -1: e' un pulsante delle finestre (zaino, personaggio...) o il fondo, che il gioco aprirebbe dopo per l'eroe che le
// finestre vedono in quel momento (prova della 0.52.9: lo zaino dalla barra di destra era quello dell'eroe 1).
int CallTaskbarInputCopy(DWORD self, DWORD ev, const TbSide& sd, int dx, int dy)   // senza oggetti da distruggere, per il __try
{
    int x = 0, y = 0;
    if (!Rd(ev + 8, x) || !Rd(ev + 12, y)) return 0;
    g_tbShifting.fetch_add(1);
    TbOtherCtx c(self, sd);
    const int lx = x - dx, ly = y - dy;
    bool slot = false;
    for (DWORD off = 0x164; off <= 0x188 && !slot; off += 4) {
        DWORD p = 0, f = 0; short wh[2] = {}; int px = 0, py = 0;
        if (!Rd(self + off, p) || !p || !Rd(p + 0x10, f) || !(f & 1) || !RdBuf(p + 0x2c, wh, sizeof(wh))) continue;
        AbsPos(p, px, py);
        slot = lx >= px && lx < px + wh[0] && ly >= py && ly < py + wh[1];
    }
    int r = -1;
    if (slot) {
        WriteDword(ev + 8, static_cast<DWORD>(lx)); WriteDword(ev + 12, static_cast<DWORD>(ly));
        __try { r = g_tbInputOrig(self, 0, ev); }
        __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; g_tbFaults.fetch_add(1); }
        WriteDword(ev + 8, static_cast<DWORD>(x)); WriteDword(ev + 12, static_cast<DWORD>(y));
    }
    c.Restore();
    g_tbShifting.fetch_sub(1);
    return r;
}

char __fastcall TaskbarInputHook(DWORD self, DWORD edx, DWORD ev)
{
    if (!ev || g_tbShifting.load()) return g_tbInputOrig(self, edx, ev);   // chiamata annidata
    TbSide sd;
    if (!TaskbarSides(self, sd)) return g_tbInputOrig(self, edx, ev);
    const TbLayout L = TaskbarLayout(self, sd);
    DWORD vt = 0, fn = 0;
    if (!Rd(ev, vt) || !Rd(vt + 4, fn) || !fn) return CallTaskbarInputReal(self, edx, ev, L.dxReal);
    int type = -1;
    __try { type = reinterpret_cast<EvTypeFn>(static_cast<uintptr_t>(fn))(ev, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { type = -1; }
    int btn = 0, x = 0, y = 0;
    if (type != 2 || !g_tbCopyOk || !Rd(ev + 4, btn) || !Rd(ev + 8, x) || !Rd(ev + 12, y)) return CallTaskbarInputReal(self, edx, ev, L.dxReal);
    const RECT& cr = g_tbCopyRect;
    if (x < cr.left || x >= cr.right || y < cr.top || y >= cr.bottom) return CallTaskbarInputReal(self, edx, ev, L.dxReal);
    if (g_tbSwapPending.load()) return 1;               // scambio in arrivo: il clic si ripete dopo
    const int dx = g_tbCopyDx, dy = g_tbCopyDy;
    for (int k = 0; k < 5; ++k) {                       // cerchi delle pozioni (non dipendono dagli slot)
        const DWORD p = self + 0x258 + k * 0xbc;
        DWORD f = 0; short wh[2] = {}; int px = 0, py = 0;
        if (!Rd(p + 0x10, f) || !(f & 1) || !RdBuf(p + 0x2c, wh, sizeof(wh))) continue;
        AbsPos(p, px, py);
        if (x >= px + dx && x < px + dx + wh[0] && y >= py + dy && y < py + dy + wh[1]) {
            if (btn == 0) { UseBeltHeroId(k + 1, sd.otherId); Log("NATIVO: clic sulla pozione %d della barra dell'eroe %lu", k + 1, sd.otherId); }
            return 1;
        }
    }
    const int r = CallTaskbarInputCopy(self, ev, sd, dx, dy);
    if (r < 0 && g_m2 && Mouse2Cur() == g_m2) {         // 0.56.3: clic del giocatore 2 con il secondo mouse: niente scambio
        Log("NATIVO: clic del giocatore 2 su un pulsante della barra dell'eroe %lu (%d,%d): ignorato (solo armi, arti e pozioni)", sd.otherId, x, y);
        return 1;
    }
    if (r < 0) {
        // 0.56.35 (prova del 04/11 alle 16:32:45, il tester: "se apro l'inventario col mouse del giocatore 2 mi sparisce il compagno",
        // "poi una volta chiuso mi riapre entrambi gli inventari"): il clic veniva dal mouse del giocatore 1 (il mouse vero) sul
        // pulsante dello zaino della barra dell'eroe 2, che dalla 0.52 passa tutta l'interfaccia all'eroe 2 (ritratti del suo
        // gruppo, senza compagni; poi I e I per rifare lo zaino). Con i due zaini possibili ora apre o chiude la colonna dell'eroe 2.
        if (btn == 0 && g_cfg.nativeTwoInventories && MemoryMouse2Ready() && MemoryDualPossible()) {
            g_pad2SideReq.store(true);
            Log("NATIVO: clic su un pulsante della barra dell'eroe %lu (%d,%d): con i due zaini apre o chiude la sua colonna", sd.otherId, x, y);
            return 1;
        }
        if (btn == 0 && g_cfg.nativeSwap) {
            g_tbSwapPending.store(true);
            g_tbSwapReq.store(static_cast<LONG>(MAKELPARAM(static_cast<SHORT>(x), static_cast<SHORT>(y))) + 1);
            Log("NATIVO: clic su un pulsante della barra dell'eroe %lu (%d,%d): interfaccia a lui, poi clic ripetuto", sd.otherId, x, y);
        }
        return 1;
    }
    return r ? static_cast<char>(r) : 1;                // un clic sulla copia non arriva al mondo
}

void InstallTaskbarHook()
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    DWORD cur = 0;
    if (!Rd(kTbVtSlot, cur) || cur != kTbDraw) { Log("NATIVO: barra dell'eroe 2 non attivata: vtable di cUI_Taskbar2 inatteso (%08lX)", cur); return; }
    DWORD old = 0;
    void* slot = reinterpret_cast<void*>(static_cast<uintptr_t>(kTbVtSlot));
    if (!VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) { Log("NATIVO: barra dell'eroe 2: VirtualProtect fallita (%lu)", GetLastError()); return; }
    g_tbDrawOrig = reinterpret_cast<TaskbarDrawFn>(static_cast<uintptr_t>(cur));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(&TaskbarDrawHook)));
    VirtualProtect(slot, 4, old, &old);
    Log("NATIVO: barra dell'eroe 2 attivata (vtable cUI_Taskbar2 +0x14)");
    if (g_cfg.nativeHero1TaskbarLeft) PatchTaskbarEye();   // 0.52.8: occhio che segue la barra dell'eroe 1 spostata
    // 0.52.3: anche i clic (metodo +0x10, 0x6e1ce0)
    DWORD curIn = 0;
    void* slotIn = reinterpret_cast<void*>(static_cast<uintptr_t>(kTbVt + 0x10));
    if (!Rd(kTbVt + 0x10, curIn) || curIn != kTbInput) { Log("NATIVO: clic sulla barra dell'eroe 2 non attivati: vtable inatteso (%08lX)", curIn); return; }
    if (!VirtualProtect(slotIn, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    g_tbInputOrig = reinterpret_cast<TaskbarInputFn>(static_cast<uintptr_t>(curIn));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slotIn), static_cast<LONG>(reinterpret_cast<uintptr_t>(&TaskbarInputHook)));
    VirtualProtect(slotIn, 4, old, &old);
    Log("NATIVO: clic sulla barra dell'eroe 2 attivati (vtable cUI_Taskbar2 +0x10)");
}

// 0.55.48: due zaini aperti insieme, fase 1 (il tester, 01/11: "inizia a studiare tutto per poter aprire due inventari
// contemporaneamente", poi "Si', fase 1"; studio nella sezione 8c). Come la seconda barra: zaino (finestra n.1,
// cUI_Inventory3, vtable 0x895668: eventi +0x10 0x6c62a0, disegno +0x14 0x6c64a0), equipaggiamento (n.2, cUI_Equipment,
// 0x895490: 0x6b67c0, 0x6b7830) e, dalla 0.55.50, statistiche e abilita' (n.8, cUI_StatisticsChar, 0x8952d8: 0x6a6270,
// 0x6aa080) si disegnano una seconda volta, spostati, con l'altro eroe al posto del loro (deviazioni dell'interfaccia e voce
// dell'inventario della pagina, come TbOtherCtx). Clic: il gestore dell'interfaccia (0x758600) chiama il metodo +0x10 delle
// finestre in ordine e si ferma alla prima che risponde vero; un clic (evento 2) dentro il rettangolo della copia (per lo
// zaino comprese le sue schede) va all'originale con la copia al posto della finestra e non arriva al mondo. Gli altri
// eventi (tasti, messaggi del gioco sugli oggetti) vanno sempre alla finestra vera. L'oggetto in mano e' uno solo
// ([0xcdcadc]+0x68) e gia' staccato dallo zaino: preso in una colonna si posa nell'altra.
// 0.55.50 (prova della 0.55.49, 01/11 12:29, foto): (1) il tester: "eroe 1 se ha la barra a sinistra deve avere l'inventario a
// sinistra, eroe 2 se ha la barra a destra deve avere l'inventario a destra" e "la prima colonna dell'inventario dell'eroe 1
// era tagliata": le cornici delle finestre escono di qualche pixel e le colonne erano attaccate. Ora i due gruppi
// (equipaggiamento e statistiche all'esterno, zaini all'interno) stanno in una fila centrata con spazi fra le finestre.
// (2) la griglia dell'eroe 2 era vuota: la pagina dello zaino (0x6c15a0) scrive l'origine della griglia nella voce
// (+0x1164/+0x1166) in coordinate fisse dello schermo (sul disco 0 e 0x188; a partita (896,392): pureHD), non dalla
// posizione della finestra, quindi la copia disegnava la griglia dell'eroe 2 sopra quella dell'eroe 1 e i clic sulla copia
// non trovavano caselle. La chiamata del disegno della griglia (0x6c1615, call 0x5dc900) passa da GridOriginStub, che somma
// lo spostamento del disegno in corso (g_dwOrgDx/Dy, 0 fuori dai due zaini); i clic usano l'origine salvata. (3) "le schede
// dell'inventario dell'eroe 2 non funzionavano, se ci cliccavo sopra mi faceva l'azione nel mondo": le schede stanno sopra
// il rettangolo della finestra; ora sono nel rettangolo dei clic, e ogni lato ha la sua scheda attiva (stato del
// cUI_TabControl: +0x84 scheda, +0x90/+0x94, +0x10 dei pulsanti e delle pagine, scambiato durante le chiamate della copia).
// Equipaggiamento: il disegno ricostruisce i riquadri (32 oggetti in +0x15c..+0x1d8, schede da 0x14 byte in +0x314..+0x594,
// +0x158 numero) quando la classe dell'eroe (+0x10) e' diversa da +0x154; per non ricostruirli due volte a fotogramma la
// copia ha una sua serie, scambiata con quella della finestra solo durante le sue chiamate (come il fondo della barra).
// Niente copie con altri pannelli aperti (mercante, deposito, cubo, personaggio...: resta lo scambio di oggi). [Native]
// TwoInventories.
typedef void(__fastcall* WinDrawFn)(DWORD self, DWORD edx, DWORD ctx);
typedef char(__fastcall* WinInputFn)(DWORD self, DWORD edx, DWORD ev);
typedef void(__fastcall* WinDraw3Fn)(DWORD self, DWORD edx, DWORD ctx, DWORD a, DWORD b);
struct DualWinDef { DWORD vt, draw, input; int index; const char* name; DWORD slot; };
// 0.56.14: anche fabbro (n.3, cUI_Blacksmith 0x895288: disegno +0x44 0x6a3510 (dispositivo, 0, 0) ret 0xc, chiamata a 0x758fc2;
// eventi 0x6a44f0) e Maestro delle combo (n.14, cUI_Master 0x895798: disegno +0x14 0x6d1260, eventi 0x6d13e0). Vedi "Fabbro e
// Maestro condivisi" piu' sotto.
// 0.56.9: anche il mercante (n.4, cUI_Merchant, vtable 0x8959b8, eventi +0x10 0x6dc030). Vedi "Mercante condiviso" piu' sotto.
// 0.56.10: prova del 03/11 alle 15:57 (foto: un solo mercante, al posto del gioco, anche con tutti e due i lati aperti): il
// gestore non disegna il mercante con +0x14 (0x7272a0, disegno generico) ma con +0x44 (0x6dbba0, thiscall (dispositivo, 0, 0),
// ret 0xc: 0x758fd0), che chiama 0x7272a0 direttamente e poi le schede ([+0x154]) e il nome. Il gancio della 0.56.9 su +0x14
// non veniva mai chiamato: il mercante non si spostava e i clic sulla sua zona (x 912-1552, quasi tutto il mercante visibile)
// erano presi come clic sulla copia dell'eroe 2 ("il giocatore 1 col . ha comprato e messo nell'inventario del giocatore 2").
// 0.56.54 (il tester, prova del 07/11 alle 16:04-16:05: "se il giocatore 1 apre il baule condiviso, il giocatore 2 non puo' aprirlo
// [...] devi fare esattamente come il mercante"): anche il deposito (n.16, cUI_Chest, vtable 0x895330: eventi +0x10 0x6ad120,
// disegno +0x14 0x6acd60, chiamato dal gestore a 0x7590e5; [gestore+0xc0], creato a 0x759d5d). Una finestra sola come il mercante:
// il contenuto (voce 0x11 della tabella degli inventari, [+0x158]) e' di tutti e due, ogni colonna ha la sua copia spostata con
// le letture dell'eroe deviate all'eroe della colonna, e l'origine della griglia si sposta con la colonna (ShiftMerch).
constexpr int kDw = 7;
constexpr int kDwChest = 6;
static bool DwHasInst(int k) { return k == 4 || k == 5; }   // 0.56.54: fabbro e Maestro hanno un'istanza per l'eroe 2, il deposito no
static const DualWinDef kDualWin[kDw] = {
    { 0x00895668, 0x006c64a0, 0x006c62a0, 1, "zaino", 0x14 },
    { 0x00895490, 0x006b7830, 0x006b67c0, 2, "equipaggiamento", 0x14 },
    { 0x008952d8, 0x006aa080, 0x006a6270, 8, "statistiche", 0x14 },
    { 0x008959b8, 0x006dbba0, 0x006dc030, 4, "mercante", 0x44 },
    { 0x00895288, 0x006a3510, 0x006a44f0, 3, "fabbro", 0x44 },
    { 0x00895798, 0x006d1260, 0x006d13e0, 14, "maestro", 0x14 },
    { 0x00895330, 0x006acd60, 0x006ad120, 16, "deposito", 0x14 } };
// 0.56.14 (studio del 04/11): il fabbro NON si condivide. Il suo piano di lavoro e' sempre la voce 0x12 della tabella degli
// inventari (0x6a21f1 push 0x12 per 0x5df160, 32 voci da 0x1180), e quel numero e' scritto nel codice: alla chiusura (0x6a31f0,
// push 0x12 a 0x6a32ac), negli eventi (0x6a464f cmp eax, 0x12; 0x6a44f0 "voce 0x11 o 0x12") e fuori dal fabbro, nel codice
// generico degli oggetti (0x5df470 "else if (voce == 0x12)", 0x604be0 "voce != 0x12"). Un secondo fabbro con un'altra voce non
// avrebbe quelle regole (e chiudendolo renderebbe gli oggetti della voce 0x12, cioe' del giocatore 1): rischio di perdere o
// duplicare oggetti. Il fabbro resta di un giocatore alla volta, come nella 0.56.7; la parte per l'istanza resta pronta (kShareSmith,
// definita con MemorySharedServiceWin).
WinDrawFn g_dwDraw[kDw] = {};
WinDraw3Fn g_dwDraw3[kDw] = {};          // 0.56.10: disegni a tre argomenti (+0x44: mercante, fabbro)
DWORD g_inst[kDw] = {};                  // 0.56.14: fabbro e Maestro dell'eroe 2 (istanze della mod, fuori dal gestore)
bool g_instOpen[kDw] = {};
DWORD g_instDrawAt[kDw] = {};
WinInputFn g_dwInput[kDw] = {};
std::atomic<LONG> g_dwFaults{ 0 };
LONG g_dwNest = 0;                       // solo thread del motore: chiamate annidate e copie in corso
volatile LONG g_dwOrgDx = 0, g_dwOrgDy = 0;   // spostamento dell'origine della griglia per il disegno in corso
bool g_dwGridOk = false;                 // GridOriginStub installato
std::atomic<DWORD> g_dwActiveAt{ 0 };
std::atomic<DWORD> g_compSide[3];        // 0.56.28: compagno mostrato da equipaggiamento e statistiche di ogni colonna (1, 2; CompDraw)
// 0.55.57: confronto con segno. Prova della 0.55.56 (01/11, 15:34): il puntatore del giocatore 2 si spegneva e riaccendeva
// decine di volte al secondo ("spento (lato chiuso)" e subito "acceso"), tornando ogni volta alla prima casella e restituendo il
// mouse al giocatore 1: DualDraw scrive GetTickCount() | 1, che con il contatore pari e' 1 ms "nel futuro", e la differenza
// senza segno diventava enorme. Lo stesso per i rettangoli disegnati (MemoryDualPointInUi).
bool MemoryDualActive() { const DWORD t = g_dwActiveAt.load(); return t && static_cast<LONG>(GetTickCount() - t) < 400; }
// 0.55.54: fase 2 dei due zaini (il tester, 01/11: "se entrambi gli zaini sono aperti, il back tenuto premuto del giocatore 2
// non dovrebbe chiudere solo il suo zaino?", poi "butta su tutto"). Ogni giocatore ha il suo lato: g_dualSide1 (eroe 1, a
// sinistra) e g_dualSide2 (eroe 2, a destra). La finestra vera del gioco e' una sola (zaino n.1, equipaggiamento n.2,
// statistiche n.8): resta aperta finche' almeno un lato lo e'; un lato chiuso non si disegna e i clic sulla sua zona vanno al
// mondo. Core.cpp apre e chiude i lati (View tenuto del pad 1, Back del pad 2, Y), manda il tasto I solo quando si apre il
// primo lato o si chiude l'ultimo, e con il lato dell'eroe 2 aperto da' al pad 2 il suo puntatore (Core, Pad2Pointer).
std::atomic<bool> g_dualSide1{ false }, g_dualSide2{ false };
// 0.55.56 (controllo): rettangoli delle finestre disegnate in questo momento (vera e copia di zaino, equipaggiamento,
// statistiche), scritti da DualDraw nel thread del motore. Il puntatore del giocatore 2 clicca solo li' dentro: fuori un clic
// andrebbe al mondo, che obbedisce solo all'eroe locale (l'eroe 1 camminerebbe o lancerebbe un'arte al clic del giocatore 2).
struct DwRectA { std::atomic<LONG> l{ 0 }, t{ 0 }, r{ 0 }, b{ 0 }; std::atomic<DWORD> at{ 0 }; };   // at: ultimo disegno
DwRectA g_dwShown[2][kDw];
static void DwStoreRect(DwRectA& a, const RECT& r, bool on)
{
    a.l.store(on ? r.left : 0); a.t.store(on ? r.top : 0); a.r.store(on ? r.right : 0); a.b.store(on ? r.bottom : 0);
    a.at.store(GetTickCount());
}
DwRectA g_handShown[3][2];               // 0.56.33: caselle delle mani dei compagni disegnate (colonna 1 e 2), fuori dalle finestre
// 0.56.42 (prova sulla Steam Machine del 04/11 alle 23:34:41-52: "il pad 2 non apriva la scheda compagni dall'inventario 2"; dieci
// righe "clic in (1021,667) fuori dagli zaini, ignorato"): la scheda Compagni sta sotto il bordo della finestra dello zaino e il
// pad 2 manda i clic solo dentro le finestre dei due zaini; ora anche dentro le schede disegnate (CompTabNote)
DwRectA g_compTabShown[2];
bool MemoryDualPointInUi(LONG x, LONG y)
{
    if (!MemoryDualActive()) return false;
    for (auto& row : g_dwShown)
        for (DwRectA& a : row)
            if (static_cast<LONG>(GetTickCount() - a.at.load()) < 200 && x >= a.l.load() && x < a.r.load() && y >= a.t.load() && y < a.b.load()) return true;
    for (int s = 1; s <= 2; ++s)
        for (DwRectA& a : g_handShown[s])
            if (static_cast<LONG>(GetTickCount() - a.at.load()) < 200 && x >= a.l.load() && x < a.r.load() && y >= a.t.load() && y < a.b.load()) return true;
    for (DwRectA& a : g_compTabShown)
        if (static_cast<LONG>(GetTickCount() - a.at.load()) < 200 && x >= a.l.load() && x < a.r.load() && y >= a.t.load() && y < a.b.load()) return true;
    return false;
}
int MemoryCompHands(RECT* r)
{
    int n = 0;
    for (int s = 1; s <= 2; ++s)
        for (DwRectA& a : g_handShown[s])
            if (static_cast<LONG>(GetTickCount() - a.at.load()) < 150 && a.r.load() > a.l.load()) r[n++] = RECT{ a.l.load(), a.t.load(), a.r.load(), a.b.load() };
    return n;
}
std::atomic<LONG> g_dwInvCopyDx{ 0 };
bool MemoryDualSide(int who) { return who == 1 ? g_dualSide1.load() : who == 2 ? g_dualSide2.load() : false; }
void MemoryDualSetSide(int who, bool on) { if (who == 1) g_dualSide1.store(on); else if (who == 2) g_dualSide2.store(on); }


__declspec(naked) void GridOriginStub()  // al posto di call 0x5dc900 in 0x6c1615: ECX = voce dell'inventario
{
    __asm {
        mov  eax, g_dwOrgDx
        add  word ptr [ecx + 0x1164], ax
        mov  eax, g_dwOrgDy
        add  word ptr [ecx + 0x1166], ax
        mov  eax, 0x005dc900
        jmp  eax
    }
}

// Stato delle schede dello zaino (cUI_TabControl, voci da 24 byte da [+0x78] con +0x10 pulsante e +0x14 pagina).
struct TabState { bool have = false; WORD active = 0; DWORD f90 = 0, f94 = 0; int n = 0; DWORD pf[8] = {}, bf[8] = {}; };
static DWORD TabCtlOf(DWORD win)
{
    DWORD c = 0, vt = 0;
    if (Rd(win + 0x78, c) && c && Rd(c, vt) && vt == kVtTabControl) return c;
    DWORD w[0x400 / 4];
    if (!RdBuf(win, w, sizeof(w))) return 0;
    for (int i = 1; i < 0x100; ++i) {
        const DWORD sheet = w[i];
        DWORD svt = 0, ctl = 0, cvt = 0;
        if (sheet < 0x01000000u || sheet >= 0x7FFF0000u || (sheet & 3) || !Rd(sheet, svt) || svt != kVtTabSheet) continue;
        if (Rd(sheet + 0x78, ctl) && ctl && Rd(ctl, cvt) && cvt == kVtTabControl) return ctl;
    }
    return 0;
}
static bool TabRead(DWORD ctl, TabState& t)
{
    DWORD beg = 0, end = 0;
    if (!ctl || !Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7c, end) || end < beg || (end - beg) % 24 || (end - beg) / 24 > 8) return false;
    t.n = static_cast<int>((end - beg) / 24);
    if (!Rd(ctl + 0x84, t.active) || !Rd(ctl + 0x90, t.f90) || !Rd(ctl + 0x94, t.f94)) return false;
    for (int k = 0; k < t.n; ++k) {
        DWORD b = 0, p = 0;
        Rd(beg + k * 24 + 0x10, b); Rd(beg + k * 24 + 0x14, p);
        t.bf[k] = 0; t.pf[k] = 0;
        if (b) Rd(b + 0x10, t.bf[k]);
        if (p) Rd(p + 0x10, t.pf[k]);
    }
    t.have = true;
    return true;
}
static void TabWrite(DWORD ctl, const TabState& t)
{
    DWORD beg = 0, end = 0;
    if (!t.have || !ctl || !Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7c, end) || static_cast<int>((end - beg) / 24) != t.n) return;
    SetWord(ctl + 0x84, t.active);
    WriteDword(ctl + 0x90, t.f90); WriteDword(ctl + 0x94, t.f94);
    for (int k = 0; k < t.n; ++k) {
        DWORD b = 0, p = 0;
        Rd(beg + k * 24 + 0x10, b); Rd(beg + k * 24 + 0x14, p);
        if (b) WriteDword(b + 0x10, t.bf[k]);
        if (p) WriteDword(p + 0x10, t.pf[k]);
    }
}
// indice della pagina dello zaino fra le schede e la pagina stessa
static int TabInvIndex(DWORD ctl, DWORD* page)
{
    DWORD beg = 0, end = 0;
    if (!ctl || !Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7c, end) || end < beg || (end - beg) / 24 > 8) return -1;
    for (int k = 0; k < static_cast<int>((end - beg) / 24); ++k) {
        DWORD p = 0, vt = 0;
        if (Rd(beg + k * 24 + 0x14, p) && p && Rd(p, vt) && vt == kVtInventoryTab) { if (page) *page = p; return k; }
    }
    return -1;
}
static bool TabsRect(DWORD ctl, RECT& r)
{
    DWORD beg = 0, end = 0;
    if (!ctl || !Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7c, end) || end < beg || (end - beg) / 24 > 8) return false;
    bool any = false;
    for (int k = 0; k < static_cast<int>((end - beg) / 24); ++k) {
        DWORD b = 0, f = 0; short wh[2] = {}; int x = 0, y = 0;
        if (!Rd(beg + k * 24 + 0x10, b) || !b || !Rd(b + 0x10, f) || !(f & 1) || !RdBuf(b + 0x2c, wh, sizeof(wh)) || wh[0] <= 0 || wh[1] <= 0) continue;
        AbsPos(b, x, y);
        const RECT q{ x, y, x + wh[0], y + wh[1] };
        if (!any) { r = q; any = true; }
        else { r.left = (std::min)(r.left, q.left); r.top = (std::min)(r.top, q.top); r.right = (std::max)(r.right, q.right); r.bottom = (std::max)(r.bottom, q.bottom); }
    }
    return any;
}
TabState g_tabCopy, g_tabInv;            // scheda della copia; ultimo stato visto con la scheda dello zaino attiva

struct DualLayout {
    DWORD win[kDw] = {};                 // finestre n.1, n.2, n.8 (anche nascoste: servono le misure)
    bool vis[kDw] = {};
    RECT r[kDw] = {};                    // dove il gioco le mette
    RECT hit[kDw] = {};                  // rettangolo dei clic (zaino: con le schede)
    POINT off1[kDw] = {}, off2[kDw] = {};   // spostamento per il posto dell'eroe 1 (sinistra) e dell'eroe 2 (destra)
    bool realIs2 = false;                // scambio: le finestre vere mostrano l'eroe 2
    TbSide sd;                           // l'altro eroe (quello della copia)
    DWORD ctl = 0, page = 0;             // schede dello zaino e sua pagina (cUI_InventoryTab)
    int invTab = -1;
    DWORD mctl = 0;                      // 0.56.9: schede del mercante (cUI_TabControl del suo cUI_TabSheet)
    int up = -1;                         // 0.56.14: finestra condivisa sopra lo zaino (3 mercante, 4 fabbro, 5 Maestro)
    POINT RealOff(int k) const { return realIs2 ? off2[k] : off1[k]; }
    POINT CopyOff(int k) const { return realIs2 ? off1[k] : off2[k]; }
    RECT CopyHit(int k) const { const POINT o = CopyOff(k); return RECT{ hit[k].left + o.x, hit[k].top + o.y, hit[k].right + o.x, hit[k].bottom + o.y }; }
    RECT RealHit(int k) const { const POINT o = RealOff(k); return RECT{ hit[k].left + o.x, hit[k].top + o.y, hit[k].right + o.x, hit[k].bottom + o.y }; }
};

static bool DualLayoutNow(DualLayout& L)
{
    if (!g_cfg.nativeTwoInventories || !g_uiPatched.load() || g_saveOrig.load() || g_dwFaults.load() >= 3) return false;
    DWORD mgr = 0, om = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(kObjMgr, om) || !om) return false;
    for (int i = 0; i < 23; ++i) {
        DWORD w = 0, f = 0;
        if (!Rd(mgr + 0x80 + i * 4, w) || !w) continue;
        const bool v = Rd(w + 0x10, f) && (f & 1);
        int k = -1;
        for (int j = 0; j < kDw; ++j) if (kDualWin[j].index == i) k = j;
        // 0.56.9: il mercante solo con il secondo mouse (senza, resta lo scambio dell'interfaccia di prima)
        if (k >= 3 && v && !MemoryMouse2Ready()) return false;   // 0.56.14: anche il Maestro
        if (k == 4 && v && !kShareSmith) return false;           // il fabbro no (vedi kShareSmith): un lato solo, come prima
        if (k >= 0) { L.win[k] = w; L.vis[k] = v; continue; }
        // 0.55.49: la n.8 (statistiche) il gioco la apre insieme allo zaino; nella prova della 0.55.48 bloccava tutto
        // 0.56.11: con il mercante condiviso anche la conferma dell'acquisto (n.13, [gestore+0xb4]): prova del 03/11 alle 16:31, con
        // la conferma aperta le due colonne sparivano e la conferma restava al posto del gioco, sopra la colonna dell'eroe 2
        if (v && i == 13 && L.vis[3]) continue;
        if (v && i != 0 && i != 6 && i != 7 && i != 9 && i != 18 && i != 19) return false;   // altri pannelli aperti
    }
    if (!L.vis[0] && !L.vis[1]) return false;
    for (int k = 0; k < kDw; ++k) {
        DWORD vt = 0;
        if (!L.win[k] || !Rd(L.win[k], vt) || vt != kDualWin[k].vt) { L.win[k] = 0; L.vis[k] = false; continue; }
        int x = 0, y = 0; short wh[2] = {};
        AbsPos(L.win[k], x, y);
        if (!RdBuf(L.win[k] + 0x2c, wh, sizeof(wh)) || wh[0] <= 0 || wh[1] <= 0) { L.win[k] = 0; L.vis[k] = false; continue; }
        L.r[k] = L.hit[k] = RECT{ x, y, x + wh[0], y + wh[1] };
    }
    if (!L.win[0] || !L.win[1] || (!L.vis[0] && !L.vis[1])) return false;
    L.ctl = TabCtlOf(L.win[0]);
    L.invTab = TabInvIndex(L.ctl, &L.page);
    RECT tr{};
    if (TabsRect(L.ctl, tr)) {
        RECT& h = L.hit[0];
        h.left = (std::min)(h.left, tr.left); h.top = (std::min)(h.top, tr.top); h.right = (std::max)(h.right, tr.right); h.bottom = (std::max)(h.bottom, tr.bottom);
    }
    L.up = L.vis[3] ? 3 : L.vis[4] ? 4 : L.vis[5] ? 5 : L.vis[kDwChest] ? kDwChest : -1;
    if (L.up == 3 || L.up == 5) {                       // 0.56.9: mercante (0.56.14: e Maestro), con le schede nei clic
        const DWORD c = TabCtlOf(L.win[L.up]);
        if (L.up == 3) L.mctl = c;
        RECT mr{};
        if (TabsRect(c, mr)) {
            RECT& h = L.hit[L.up];
            h.left = (std::min)(h.left, mr.left); h.top = (std::min)(h.top, mr.top); h.right = (std::max)(h.right, mr.right); h.bottom = (std::max)(h.bottom, mr.bottom);
        }
    }
    // chi mostrano le finestre vere: lo scambio della sola interfaccia come lo vede questo fotogramma, piu' la pagina dello
    // zaino (RetargetInventoryUi la cambia dal thread della mod): se non concordano lo scambio e' a meta', niente copia
    const bool live = g_swapUiLive.load();
    WORD idx = 0;
    if (L.page && Rd(L.page + 0x158, idx)) {
        // la copia rimette la voce della pagina alla fine delle sue chiamate: se RetargetInventoryUi (thread della mod) l'ha
        // riportata all'eroe 1 proprio in quel momento, la pagina resterebbe sull'eroe 2 a scambio finito. Si corregge qui.
        DWORD player = 0, loc = 0;
        if (!live && idx == 2 && !g_swapOrig.load() && !g_swapUi.load() && Rd(kPlayer, player) && player && RdHero(player, loc) && loc && loc != 2) {
            SetWord(L.page + 0x158, static_cast<WORD>(loc));
            Log("ZAINI: pagina dello zaino rimessa sull'eroe %lu a scambio finito", loc);
            idx = static_cast<WORD>(loc);
        }
        if ((idx == 2) != live) return false;
    }
    if (live) {
        DWORD loc = g_swapOrig.load();
        if (!loc || loc == 2) return false;
        L.realIs2 = true; L.sd.otherId = loc; L.sd.otherIs2 = false;
    }
    else if (g_swapOrig.load()) return false;       // scambio della 0.50 o in arrivo
    else { L.sd.otherId = 2; L.sd.otherIs2 = true; }
    if (L.realIs2 && (L.vis[4] || L.vis[5])) return false;   // 0.56.14: fabbro e Maestro dell'eroe 2 solo con la loro istanza
    if (L.realIs2 && L.vis[kDwChest]) return false;          // 0.56.54: deposito aperto dall'eroe 2 con lo scambio: come prima
    DWORD hp = 0;
    L.sd.otherAddr = ObjectAddr(om, L.sd.otherId);
    if (!L.sd.otherAddr || !Rd(L.sd.otherAddr + 0x4d8, hp) || hp == 0) return false;
    const DWORD h2 = L.sd.otherIs2 ? L.sd.otherAddr : ObjectAddr(om, 2);
    if (!h2 || g_h2SeenAddr.load() != h2 || GetTickCount() - g_h2SeenAt.load() < 2000) return false;
    // 0.55.62: prova della 0.55.61 (02/11, 13:08-13:10, foto e log): (a) con un lato solo l'eroe 1 stava al posto del gioco e
    // con tutti e due si spostava a sinistra ("se lo apre da solo lo apre a destra e se il pg 2 apre l'inventario si sposta a
    // sinistra, creando confusione"); (b) a 1920 equipaggiamento e statistiche dell'eroe 1 partivano da x -11 ("tutto a sinistra
    // la schermata del pg 1 era tagliata"). La cornice sinistra dello zaino non e' fissa nel gioco: la disegna pHD.dll (pureHD)
    // dopo i figli della finestra, alla x della sua variabile [pHD+0xa91dc] (larghezza dello schermo - 1024 = 896) - 16 e ai 7
    // pezzi della tabella [pHD+0xa8cf4]; DwShift ora sposta anche quelli (PhdBorder). Quindi ogni eroe ha sempre lo stesso posto,
    // con un lato o con due: [equipaggiamento 1][zaino 1|cornice][zaino 2|cornice][equipaggiamento 2], la cornice destra dello
    // zaino 1 sopra la sinistra dello zaino 2. L'equipaggiamento dell'eroe 1 sta attaccato al suo zaino e copre la sua cornice
    // sinistra (il gestore disegna lo zaino 0x84, poi barra 0x80, statistiche 0xa0 ed equipaggiamento 0x88: 0x759034..). Il
    // tutto si sposta a destra quanto basta perche' l'eroe 1 parta da x 0 (16 a 1920), non di piu': piu' a destra le statistiche
    // dell'eroe 2 finirebbero sotto i ritratti (da x 1824 a 1920, foto delle 13:10:27). Le due cornici sono larghe 16 (misurate
    // nella foto delle 13:09:01: pHD da 880 a 896, quella destra della finestra da 1536 a 1552). A 1920: equipaggiamento 1 da x 0,
    // zaino 1 da 256, zaino 2 da 912, equipaggiamento e statistiche 2 fino a 1824.
    const short sw = ScreenW();
    const RECT& ri = L.r[0];
    const RECT& re = L.r[1];
    const LONG iw = ri.right - ri.left, ew = re.right - re.left;
    constexpr LONG kPhdBorder = 16;                           // cornice sinistra di pureHD (e destra della finestra)
    const LONG left0 = ri.left - kPhdBorder - iw - ew;        // bordo sinistro dell'eroe 1 con l'eroe 2 al posto del gioco (-16)
    const LONG sh = left0 < 0 ? -left0 : 0;                   // spostamento dell'eroe 2
    // 0.56.9: il mercante sta sopra lo zaino (a 1920: (896,0)-(1536,336) e lo zaino (896,388)-(1536,644)): segue la colonna del suo
    // zaino, a sinistra per l'eroe 1 e a destra per l'eroe 2
    const int up = L.up;
    const RECT& rm = L.r[up >= 0 ? up : 3];
    const bool fitsM = up < 0 || (rm.left - ri.left <= 0 && rm.right - rm.left <= iw + kPhdBorder && ri.left + sh + (rm.right - rm.left) <= sw);
    const bool fits = sw > 0 && re.left >= ri.right - 16 && re.right + sh <= sw && fitsM;
    if (fits) {
        const LONG inv1 = ri.left + sh - kPhdBorder - iw, eq1 = inv1 - ew;
        for (int k = 0; k < kDw; ++k) L.off2[k] = POINT{ sh, 0 };
        L.off1[0] = POINT{ inv1 - ri.left, 0 };
        L.off1[1] = POINT{ eq1 - re.left, 0 };
        L.off1[2] = L.off1[1];                                // le statistiche seguono l'equipaggiamento (stessa colonna nel gioco)
        if (up >= 0) { L.off1[up] = POINT{ inv1 - rm.left, 0 }; L.off2[up] = POINT{ ri.left + sh - rm.left, 0 }; }
    }
    static LONG logged = -1;
    const LONG key = (ri.left << 20) ^ (ri.top << 10) ^ (re.left << 4) ^ sw ^ (fits ? 1 : 0) ^ (L.vis[2] ? (L.r[2].left << 2) ^ L.r[2].top : 0) ^ (L.hit[0].top << 12) ^
                     (up >= 0 ? (rm.left << 8) ^ (rm.bottom << 16) ^ (0x40000000 + (up << 26)) : 0);
    static LONG loggedM = -1;
    if (up >= 0 && key != loggedM) {
        loggedM = key;
        Log("CONDIVISO: %s (%ld,%ld)-(%ld,%ld) con le schede fino a y %ld; %s; eroe 1 a %+ld, eroe 2 a %+ld", kDualWin[up].name, rm.left, rm.top, rm.right,
            rm.bottom, L.hit[up].bottom, fits ? "le due colonne ci stanno" : "NON ci stanno le due colonne", L.off1[up].x, L.off2[up].x);
    }
    if (key != logged) {
        logged = key;
        Log("ZAINI: zaino (%ld,%ld)-(%ld,%ld) con le schede da y %ld, equipaggiamento (%ld,%ld)-(%ld,%ld), statistiche (%ld,%ld)-(%ld,%ld)%s, schermo %d: %s; eroe 1 a %+ld/%+ld, eroe 2 a %+ld/%+ld",
            ri.left, ri.top, ri.right, ri.bottom, L.hit[0].top, re.left, re.top, re.right, re.bottom, L.r[2].left, L.r[2].top, L.r[2].right, L.r[2].bottom,
            L.vis[2] ? "" : " (chiuse)", sw, !fits ? "NON ci stanno i due gruppi, niente copia" : "eroe 1 a sinistra, eroe 2 a destra",
            L.off1[0].x, L.off1[1].x, L.off2[0].x, L.off2[1].x);
    }
    return fits;
}

// Spostamento temporaneo di una finestra e dei suoi pezzi senza genitore (+0x50 = 0, quindi in coordinate assolute; per
// l'equipaggiamento i 32 riquadri e +0x1dc, per zaino e statistiche +0x154 e +0x158), rimessi solo se il gioco non li ha
// cambiati. Per lo zaino anche l'origine della griglia (GridOriginStub).
// 0.55.62: cornice sinistra dello zaino di pHD.dll (vedi DualLayoutNow): la funzione di pHD agganciata alla chiamata dei figli
// dello zaino (0x6c64c9) disegna i pezzi a x [pHD+0xa91dc] - 16 e alle x della tabella [pHD+0xa8cf4] (7 voci da 8 byte, x e
// sprite), calcolate da [pHD+0xa91dc]. Si controllano le due istruzioni che le leggono (pHD+0x4c3a mov edx,[+0xa91dc] e
// pHD+0x4c89 mov edx,[ecx*8+0xa8cf4]); con un altro pHD.dll la cornice resta dov'e'.
struct PhdBorder { DWORD x = 0, tab = 0; bool ok = false; };
static const PhdBorder& PhdBorderInfo()
{
    static PhdBorder p;
    static bool tried = false;
    if (tried) return p;
    tried = true;
    const DWORD base = static_cast<DWORD>(reinterpret_cast<uintptr_t>(GetModuleHandleA("pHD.dll")));
    BYTE a[6] = {}, b[7] = {};
    if (!base || !RdBuf(base + 0x4c3a, a, sizeof(a)) || !RdBuf(base + 0x4c89, b, sizeof(b))) { Log("ZAINI: pHD.dll non trovato: la cornice sinistra dello zaino resta fissa"); return p; }
    DWORD ra = 0, rb = 0;
    memcpy(&ra, a + 2, 4); memcpy(&rb, b + 3, 4);
    if (a[0] != 0x8B || a[1] != 0x15 || ra != base + 0xa91dc || b[0] != 0x8B || b[1] != 0x14 || b[2] != 0xCD || rb != base + 0xa8cf4) {
        Log("ZAINI: pHD.dll di un'altra versione (%02X %02X %08lX, %02X %02X %02X %08lX): la cornice sinistra dello zaino resta fissa", a[0], a[1], ra, b[0], b[1], b[2], rb);
        return p;
    }
    p.x = base + 0xa91dc; p.tab = base + 0xa8cf4; p.ok = true;
    LONG v = 0; Rd(p.x, v);
    Log("ZAINI: cornice dello zaino di pHD.dll (base %08lX, x %ld): si sposta con lo zaino", base, v);
    return p;
}
// 0.55.62: icone della pagina "Arti di combattimento" (cUI_SpellTab, vtable 0x895610). Prova delle 13:09: "le arti di
// combattimento del mago del pg 1 si vedevano sulla griglia del pg 2". La pagina tiene le icone in un vettore (+0x2cc..+0x2d0,
// voci da 16 byte: x e y assolute come word a +4 e +6) che 0x6c2f80 riempie dalla posizione della pagina solo quando cambia la
// classe dell'eroe (0x6c2620, chiamata all'inizio del disegno); spostando la finestra le icone restavano al posto del gioco.
// Con la pagina visibile: prima 0x6c2620 (thiscall, nessun argomento), cosi' un eventuale rifacimento avviene ora con la pagina
// ancora al suo posto, poi le icone si spostano con la finestra e alla fine si rimettono.
constexpr DWORD kVtSpellTab = 0x00895610;
static DWORD SpellTabOf(DWORD win)
{
    const DWORD ctl = TabCtlOf(win);
    DWORD beg = 0, end = 0;
    if (!ctl || !Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7c, end) || end < beg || (end - beg) / 24 > 8) return 0;
    for (DWORD e = beg; e < end; e += 24) {
        DWORD p = 0, vt = 0;
        if (Rd(e + 0x14, p) && p && Rd(p, vt) && vt == kVtSpellTab) return p;
    }
    return 0;
}

static DWORD MerchActiveEntry(DWORD win);    // piu' sotto (mercante condiviso)
struct DwShift {
    struct P { DWORD a; int x, y; };
    P p[40] = {};
    int n = 0; int dx = 0, dy = 0; bool grid = false;
    DWORD mEnt = 0; WORD mX = 0, mY = 0; bool mOn = false;
    // 0.56.10: l'origine della griglia della merce (+0x1164/+0x1166 della voce della pagina attiva) la scrive 0x6db930 una volta
    // sola, alla creazione della pagina, dalla sua x (896 a 1920: pureHD la somma in 0x6d96cd) e y 0x34: e' in coordinate dello
    // schermo e non segue la finestra. Si sposta con il lato per il disegno e per gli eventi, e si rimette se non e' cambiata.
    // 0.56.14: anche la griglia di lavoro del fabbro ([+0x154], 3x5, origine scritta da 0x6a2140 alla costruzione: 0x10 + 896, 0x40)
    void ShiftMerch(DWORD entry) {
        mEnt = entry;
        if (!mEnt || !Rd(mEnt + 0x1164, mX) || !Rd(mEnt + 0x1166, mY)) { mEnt = 0; return; }
        SetWord(mEnt + 0x1164, static_cast<WORD>(mX + dx)); SetWord(mEnt + 0x1166, static_cast<WORD>(mY + dy));
        mOn = true;
    }
    void UnshiftMerch() {
        WORD cx = 0, cy = 0;
        if (!mOn || !Rd(mEnt + 0x1164, cx) || !Rd(mEnt + 0x1166, cy)) return;
        if (cx == static_cast<WORD>(mX + dx)) SetWord(mEnt + 0x1164, mX);
        if (cy == static_cast<WORD>(mY + dy)) SetWord(mEnt + 0x1166, mY);
    }
    // 0.56.14: costanti del disegno che pHD ha scritto alla sua partenza (x = costante + 896) e che non seguono la finestra: titolo
    // del fabbro (0x6a35a3 mov ecx, imm), testi delle pagine Combo (0x6d30dc mov ecx, imm) e Lotto (0x6d4acc mov edi, imm) del
    // Maestro. Durante il disegno o l'evento del lato valgono costante + spostamento, poi si rimettono.
    struct Imm { DWORD a, v; };
    Imm imm[3] = {}; int nImm = 0;
    void ShiftImm(DWORD a) {
        DWORD v = 0;
        if (nImm >= 3 || !Rd(a, v)) return;
        const DWORD nv = v + static_cast<DWORD>(dx);
        if (PatchCode(a, reinterpret_cast<const BYTE*>(&nv), 4)) imm[nImm++] = Imm{ a, v };
    }
    void UnshiftImm() {
        for (int i = nImm - 1; i >= 0; --i) {
            DWORD cur = 0;
            if (Rd(imm[i].a, cur) && cur == imm[i].v + static_cast<DWORD>(dx)) PatchCode(imm[i].a, reinterpret_cast<const BYTE*>(&imm[i].v), 4);
        }
    }
    // pulsanti del fabbro senza genitore (+0x1dc, +0x258, +0x2d4: x assolute che pHD ha scritto in 0x6a5652, 0x6a56c4, 0x6a572f)
    void AddAbs(DWORD a) { DWORD vt = 0, par = 1; if (Rd(a, vt) && IsUiVt(vt) && Rd(a + 0x50, par) && par == 0) Add(a); }
    LONG phdX = 0, phdTab[7] = {}; bool phd = false;
    DWORD spell = 0, spellBeg = 0; int spellN = 0; WORD spellXY[64][2] = {};
    void ShiftPhd() {
        const PhdBorder& b = PhdBorderInfo();
        if (!b.ok || !Rd(b.x, phdX)) return;
        for (int i = 0; i < 7; ++i) Rd(b.tab + i * 8, phdTab[i]);
        WriteDword(b.x, static_cast<DWORD>(phdX + dx));
        for (int i = 0; i < 7; ++i) WriteDword(b.tab + i * 8, static_cast<DWORD>(phdTab[i] + dx));
        phd = true;
    }
    void UnshiftPhd() {
        if (!phd) return;
        const PhdBorder& b = PhdBorderInfo();
        LONG v = 0;
        if (Rd(b.x, v) && v == phdX + dx) WriteDword(b.x, static_cast<DWORD>(phdX));
        for (int i = 0; i < 7; ++i) if (Rd(b.tab + i * 8, v) && v == phdTab[i] + dx) WriteDword(b.tab + i * 8, static_cast<DWORD>(phdTab[i]));
    }
    void SyncSpell(DWORD win) {                       // prima di spostare la finestra
        DWORD f = 0;
        spell = SpellTabOf(win);
        if (!spell || !Rd(spell + 0x10, f) || !(f & 1)) { spell = 0; return; }
        const CallResult cr = CallThis0(0x006c2620, spell);
        if (!cr.ok || !cr.balanced) { spell = 0; LogException("aggiornamento della pagina delle arti"); }
    }
    void ShiftSpell() {
        DWORD end = 0;
        if (!spell || !Rd(spell + 0x2cc, spellBeg) || !Rd(spell + 0x2d0, end) || end < spellBeg || (end - spellBeg) % 16 || (end - spellBeg) / 16 > 64) { spellN = 0; return; }
        spellN = static_cast<int>((end - spellBeg) / 16);
        for (int i = 0; i < spellN; ++i) {
            const DWORD e = spellBeg + i * 16;
            if (!RdBuf(e + 4, spellXY[i], 4)) { spellN = i; break; }
            SetWord(e + 4, static_cast<WORD>(spellXY[i][0] + dx)); SetWord(e + 6, static_cast<WORD>(spellXY[i][1] + dy));
        }
    }
    void UnshiftSpell() {
        DWORD beg = 0, end = 0;
        if (!spellN || !Rd(spell + 0x2cc, beg) || !Rd(spell + 0x2d0, end) || beg != spellBeg || static_cast<int>((end - beg) / 16) != spellN) return;
        for (int i = 0; i < spellN; ++i) {
            const DWORD e = spellBeg + i * 16;
            WORD v[2] = {};
            if (!RdBuf(e + 4, v, 4)) continue;
            if (v[0] == static_cast<WORD>(spellXY[i][0] + dx)) SetWord(e + 4, spellXY[i][0]);
            if (v[1] == static_cast<WORD>(spellXY[i][1] + dy)) SetWord(e + 6, spellXY[i][1]);
        }
    }
    void Add(DWORD a) {
        int x = 0, y = 0;
        if (n < 40 && a && Rd(a + 0x24, x) && Rd(a + 0x28, y)) { p[n++] = P{ a, x, y }; WriteDword(a + 0x24, static_cast<DWORD>(x + dx)); WriteDword(a + 0x28, static_cast<DWORD>(y + dy)); }
    }
    DwShift(int k, DWORD win, POINT o) : dx(o.x), dy(o.y) {
        if (!dx && !dy) return;
        if (k == 0) { g_dwOrgDx = dx; g_dwOrgDy = dy; grid = true; SyncSpell(win); }
        Add(win);
        auto loose = [&](DWORD ptrAt) {
            DWORD e = 0, vt = 0, par = 1;
            if (Rd(ptrAt, e) && e && e != win && Rd(e, vt) && IsUiVt(vt) && Rd(e + 0x50, par) && par == 0) Add(e);
        };
        if (k == 1) { for (DWORD off = 0x15c; off <= 0x1dc; off += 4) loose(win + off); }
        else { loose(win + 0x154); loose(win + 0x158); }
        if (k == 3) ShiftMerch(MerchActiveEntry(win));
        if (k == 4) {                               // 0.56.14: fabbro
            DWORD e = 0;
            if (Rd(win + 0x154, e)) ShiftMerch(e);
            AddAbs(win + 0x1dc); AddAbs(win + 0x258); AddAbs(win + 0x2d4);
            ShiftImm(0x006a35a4);
        }
        // 0.56.14: Maestro. Il suo bordo sinistro lo disegna pHD (0x6d1278: la chiamata di 0x7272a0 va a pHD+0x4ff7, pezzi a x
        // [pHD+0xa91dc] - 6), come la cornice dello zaino: ShiftPhd sposta quella variabile per la durata del disegno.
        if (k == 5) { ShiftPhd(); ShiftImm(0x006d30dd); ShiftImm(0x006d4acd); }
        if (k == kDwChest) { DWORD e = 0; if (Rd(win + 0x158, e)) ShiftMerch(e); }   // 0.56.54: griglia del deposito
        if (k == 0) {
            ShiftPhd(); ShiftSpell();
            // 0.55.64: prova delle 18:42 (foto: "non c'e' la griglia eroe 1", quella dell'eroe 2 spostata di 16): l'origine x della
            // griglia la scrive pHD (aggancio a 0x6c15ea, pHD+0x50c0: [+0x1164] = [pHD+0xa91dc]) dalla variabile che ShiftPhd ha gia'
            // spostato; GridOriginStub la spostava una seconda volta (eroe 1 a -1280: fuori dallo schermo). Con pHD spostato, solo dy.
            if (phd) g_dwOrgDx = 0;
        }
        static int logged[kDw] = { -1, -1, -1, -1, -1, -1, -1 };
        if (logged[k] != n) { logged[k] = n; Log("ZAINI: %s spostato di %+d,%+d con %d pezzi senza genitore", kDualWin[k].name, dx, dy, n - 1); }
    }
    ~DwShift() {
        UnshiftImm();
        UnshiftMerch();
        UnshiftSpell();
        UnshiftPhd();
        int v = 0;
        for (int i = n - 1; i >= 0; --i) {
            if (Rd(p[i].a + 0x24, v) && v == p[i].x + dx) WriteDword(p[i].a + 0x24, static_cast<DWORD>(p[i].x));
            if (Rd(p[i].a + 0x28, v) && v == p[i].y + dy) WriteDword(p[i].a + 0x28, static_cast<DWORD>(p[i].y));
        }
        if (grid) { g_dwOrgDx = 0; g_dwOrgDy = 0; }
    }
};

// Serie dei riquadri dell'equipaggiamento della copia (vedi sopra): +0x154..+0x1dc e +0x314..+0x594.
struct EqSet { BYTE a[0x88]; BYTE b[0x280]; };
DWORD g_eqAltWin = 0;
EqSet g_eqAlt{};
static bool EqRead(DWORD w, EqSet& s) { return RdBuf(w + 0x154, s.a, sizeof(s.a)) && RdBuf(w + 0x314, s.b, sizeof(s.b)); }
static void EqWrite(DWORD w, const EqSet& s)
{
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(w + 0x154)), s.a, sizeof(s.a), nullptr);
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(w + 0x314)), s.b, sizeof(s.b), nullptr);
}

static bool DwCallDraw(int k, DWORD self, DWORD ctx, DWORD a, DWORD b)
{
    __try {
        if (g_dwDraw3[k]) g_dwDraw3[k](self, 0, ctx, a, b);
        else g_dwDraw[k](self, 0, ctx);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void DwOrigDraw(int k, DWORD self, DWORD edx, DWORD ctx, DWORD a, DWORD b)
{
    if (g_dwDraw3[k]) g_dwDraw3[k](self, edx, ctx, a, b);
    else g_dwDraw[k](self, edx, ctx);
}
static int DwCallInput(int k, DWORD self, DWORD ev)
{
    __try { return g_dwInput[k](self, 0, ev) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Contesto della copia: l'altro eroe per le finestre (come TbOtherCtx); per lo zaino la sua voce nella pagina e la scheda
// della copia; per l'equipaggiamento la serie della copia.
TabState g_tabCopyM;                     // 0.56.9: scheda della copia del mercante (Armi, Armatura, Varie)
struct DwCopyCtx {
    int k; DWORD win, page, ctl, oldHero, oldOn, oldFake; WORD idx = 0, wrote = 0; EqSet own{}; bool eq = false; TabState tabs;
    DWORD mctl = 0; TabState mtabs;
    DWORD instSlot[2] = {}, instOld[2] = {};
    DwCopyCtx(int kk, const DualLayout& L) : k(kk), win(L.win[kk]), page(L.page), ctl(L.ctl), oldHero(g_uiHero), oldOn(g_uiOn), oldFake(g_uiFakePlayer[0x14 / 4]) {
        // 0.56.14: per le copie di zaino, equipaggiamento e statistiche il fabbro e il Maestro del gestore sono quelli dell'eroe 2
        // (un clic sull'oggetto dello zaino che li cerca nel gestore deve andare al suo piano di lavoro)
        DWORD um = 0;
        if (k <= 2 && Rd(kUIManager, um) && um)
            for (int j = 0; j < 2; ++j) {
                const int kw = 4 + j;
                const DWORD slot = um + 0x80 + kDualWin[kw].index * 4;
                if (g_instOpen[kw] && g_inst[kw] && Rd(slot, instOld[j]) && instOld[j]) { instSlot[j] = slot; WriteDword(slot, g_inst[kw]); }
            }
        if (k == 3) {
            mctl = L.mctl;
            if (!g_tabCopyM.have && mctl && TabRead(mctl, g_tabCopyM)) Log("MERCANTE: scheda della copia: quella attiva (%u)", g_tabCopyM.active);
            if (g_tabCopyM.have && TabRead(mctl, mtabs)) TabWrite(mctl, g_tabCopyM);
        }
        ++g_dwNest;
        if (L.sd.otherIs2) { g_uiFakePlayer[0x14 / 4] = 2; g_uiHero = L.sd.otherAddr; g_uiOn = 1; }
        else { g_uiHero = 0; g_uiOn = 0; }
        if (k == 0) {
            if (page && Rd(page + 0x158, idx)) wrote = SetWord(page + 0x158, static_cast<WORD>(L.sd.otherId));
            if (g_tabCopy.have && TabRead(ctl, tabs)) TabWrite(ctl, g_tabCopy);
        }
        if (k == 1 && EqRead(win, own)) {
            eq = true;
            if (g_eqAltWin != win) {                    // prima volta per questa finestra: serie vuota, la ricostruisce il disegno
                g_eqAltWin = win;
                memcpy(&g_eqAlt, &own, sizeof(own));
                memset(g_eqAlt.a + 4, 0, 2);            // +0x158 = 0: ricostruzione
                memset(g_eqAlt.a + 8, 0, 0x80);         // +0x15c..+0x1d8: nessun riquadro da distruggere
                memset(g_eqAlt.b, 0, sizeof(g_eqAlt.b));
                Log("ZAINI: serie dei riquadri dell'equipaggiamento per l'eroe %lu da costruire (finestra %08lX)", L.sd.otherId, win);
            }
            EqWrite(win, g_eqAlt);
        }
    }
    void Restore() {
        if (k == 3 && mtabs.have) { TabRead(mctl, g_tabCopyM); TabWrite(mctl, mtabs); }
        if (eq) { EqRead(win, g_eqAlt); EqWrite(win, own); }
        if (k == 0) {
            if (tabs.have) { TabRead(ctl, g_tabCopy); TabWrite(ctl, tabs); }
            WORD now = 0;                               // solo se e' ancora quella scritta qui (vedi DualLayoutNow)
            if (page && Rd(page + 0x158, now) && now == wrote) SetWord(page + 0x158, idx);
        }
        g_uiHero = oldHero; g_uiOn = oldOn; g_uiFakePlayer[0x14 / 4] = oldFake;
        for (int j = 1; j >= 0; --j) { DWORD cur = 0; if (instSlot[j] && Rd(instSlot[j], cur) && cur == g_inst[4 + j]) WriteDword(instSlot[j], instOld[j]); }
        --g_dwNest;
    }
};

static bool MouseUi(int& x, int& y)          // 0.56.0: il mouse che il gioco vede in questo thread (il secondo per il giocatore 2)
{
    const DWORD m = Mouse2Cur();
    return m && Rd(m + 4, x) && Rd(m + 8, y);
}
static bool InRect(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// Suggerimento della finestra dello zaino (+0x168 bit 0, testo da 0x7323e0, disegnato accanto alla finestra): solo nel
// disegno su cui sta il puntatore, altrimenti comparirebbe due volte.
struct DwTipOff {
    DWORD a = 0, v = 0; bool on = false;
    DwTipOff(int k, DWORD win, bool off) { if (k == 0 && off && Rd(win + 0x168, v) && (v & 1)) { a = win + 0x168; on = true; WriteDword(a, v & ~1u); } }
    ~DwTipOff() { DWORD now = 0; if (on && Rd(a, now) && now == (v & ~1u)) WriteDword(a, v); }
};

// Scheda della copia (0.55.50): la prima volta quella dello zaino (ultimo stato visto con lo zaino attivo, o quello di ora se
// e' lo zaino); al cambio dello scambio le schede dei due lati si scambiano, cosi' ognuna resta al suo eroe.
static bool DwTabsReady(const DualLayout& L)
{
    static int lastReal = -1;
    TabState live;
    if (!L.ctl || !TabRead(L.ctl, live)) return false;
    if (L.invTab >= 0 && live.active == L.invTab) g_tabInv = live;
    if (lastReal >= 0 && lastReal != (L.realIs2 ? 1 : 0) && g_tabCopy.have && g_tabCopy.n == live.n) {
        TabWrite(L.ctl, g_tabCopy);
        g_tabCopy = live;
        Log("ZAINI: scambio: schede dei due lati scambiate");
    }
    lastReal = L.realIs2 ? 1 : 0;
    if (!g_tabCopy.have && g_tabInv.have && g_tabInv.n == live.n) { g_tabCopy = g_tabInv; Log("ZAINI: scheda della copia: zaino"); }
    return g_tabCopy.have;
}

// 0.55.58: le statistiche mostrano il dettaglio (Arma con i danni, Attributi) quando il puntatore passa sopra certe righe: il
// disegno (0x6aa080) confronta la posizione del mouse ([0xcdcadc]+4/+8) con rettangoli globali (0x17e8f84..0x17e8fa2), comuni
// alla finestra vera e alla copia, e nella prova delle 15:35 le due statistiche cambiavano pagina insieme. Nel disegno della
// finestra su cui il puntatore non sta, il mouse si porta per un attimo fuori dallo schermo e poi si rimette.
// 0.55.59: e ogni lato ha i suoi rettangoli delle righe (0x17e8f84..0x17e8fb3, scritti e letti dal disegno: prima li scriveva
// per ultima la copia e il dettaglio dell'eroe 1 non compariva mai) e il suo interruttore della pagina Abilita'/Attributi
// (bit 0x10 di +0x164, il +0x10 del figlio +0x154 che il clic sulla barra verde accende e spegne: "se col pad 2 apro gli
// attributi del pg 2, apre gli attributi anche del pg 1").
// 0.55.64: prova delle 18:42 ("se vado col cursore due sulle abilita' dell'eroe 2, vedo nella pagina dell'eroe 1 le cose"; "se
// clicco su nuova abilita' col cursore 1 sull'eroe 1 non fa nulla"). Il blocco 0x17e8f50..0x17e8fbf sono le righe della scheda in
// coordinate assolute dello schermo (voci da 12 byte x, y, larghezza, altezza: x = finestra + 2, y = finestra + 0, +0x20, +0xde,
// +0x108, +0x132, +0x146, +0x15b; poi il dettaglio a +0x30 e +0xde), scritto una volta sola dal primo disegno (0x6aa080, che usa
// la posizione di 0x731f80) e usato sia dal disegno (righe e dettaglio) sia dai clic. Con le finestre spostate restava la
// posizione del primo disegno: il dettaglio dell'eroe 2 compariva nel pannello dell'eroe 1 e i clic dell'eroe 1 (spostati
// all'indietro al posto del gioco) non trovavano piu' le righe. Ora, per la durata di ogni chiamata, x e y del blocco seguono la
// finestra come e' in quel momento (spostata nel disegno, al posto del gioco negli eventi); fuori torna com'era. Solo a righe
// pronte (bandiere 1 e 2 di 0x17e8fc1, 0.55.63). Restano per lato l'interruttore della pagina Abilita'/Attributi (bit 0x10 di
// +0x164) e (0.55.64) la scelta dell'abilita' nuova (byte +0xf84: il clic su "Nuova abilita'" lo accende, 0x6a6270, e il
// disegno lo spegne se l'eroe non ha abilita' nuove da scegliere: il disegno della copia dell'eroe 2 annullava quella dell'eroe 1).
constexpr DWORD kStatBlock = 0x017e8f50, kStatBlockSize = 0x70;
static const DWORD kStatX[] = { 0x00, 0x0c, 0x18, 0x24, 0x30, 0x3c, 0x48, 0x58, 0x64 };   // x delle voci (y a +4)
struct StatSide { bool have = false; DWORD page = 0; BYTE choose = 0; };
StatSide g_statSide[2];                 // [0] finestra vera, [1] copia
struct DwStatScope {
    DWORD win; int side; BYTE cur[kStatBlockSize] = {}; DWORD curPage = 0; BYTE curChoose = 0; bool on = false, moved = false;
    DwStatScope(int k, DWORD w, int sd) : win(w), side(sd) {
        if (k != 2) return;
        BYTE init = 0;
        if (!Rd(0x017e8fc1, init) || (init & 3) != 3) return;
        DWORD f = 0;
        if (!RdBuf(kStatBlock, cur, kStatBlockSize) || !Rd(win + 0x164, f) || !Rd(win + 0xf84, curChoose)) return;
        curPage = f & 0x10; on = true;
        int wx = 0, wy = 0;
        AbsPos(win, wx, wy);
        LONG x0 = 0, y0 = 0;
        memcpy(&x0, cur, 4); memcpy(&y0, cur + 4, 4);
        const LONG dx = (wx + 2) - x0, dy = wy - y0;
        if (dx || dy) {
            BYTE nb[kStatBlockSize];
            memcpy(nb, cur, sizeof(nb));
            for (DWORD o : kStatX) {
                LONG x = 0, y = 0;
                memcpy(&x, nb + o, 4); memcpy(&y, nb + o + 4, 4);
                x += dx; y += dy;
                memcpy(nb + o, &x, 4); memcpy(nb + o + 4, &y, 4);
            }
            WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(kStatBlock)), nb, sizeof(nb), nullptr);
            moved = true;
        }
        StatSide& s = g_statSide[side];
        if (!s.have) { s.page = curPage; s.choose = curChoose; s.have = true; }   // la prima volta come la finestra adesso
        WriteDword(win + 0x164, (f & ~0x10u) | s.page);
        WriteByte(win + 0xf84, s.choose);
    }
    ~DwStatScope() {
        if (!on) return;
        DWORD f = 0;
        if (Rd(win + 0x164, f)) g_statSide[side].page = f & 0x10;
        Rd(win + 0xf84, g_statSide[side].choose);
        WriteByte(win + 0xf84, curChoose);
        if (moved) WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(kStatBlock)), cur, kStatBlockSize, nullptr);
        if (Rd(win + 0x164, f)) WriteDword(win + 0x164, (f & ~0x10u) | curPage);
    }
};

struct DwMouseHide {
    DWORD m = 0; int x = 0, y = 0; bool on = false;
    explicit DwMouseHide(bool hide) {
        if (!hide || (m = Mouse2Cur()) == 0 || !Rd(m + 4, x) || !Rd(m + 8, y)) return;
        on = true;
        WriteDword(m + 4, static_cast<DWORD>(-20000)); WriteDword(m + 8, static_cast<DWORD>(-20000));
    }
    ~DwMouseHide() {
        int cx = 0, cy = 0;
        if (!on || !Rd(m + 4, cx) || !Rd(m + 8, cy)) return;
        if (cx == -20000) WriteDword(m + 4, static_cast<DWORD>(x));
        if (cy == -20000) WriteDword(m + 8, static_cast<DWORD>(y));
    }
};

static void DwSnap(int side, int k, DWORD win, LONG dx, int hero);   // piu' sotto (0.55.63)

static bool PatchVtSlot(DWORD slotAddr, DWORD want, void* hook, DWORD& orig);   // piu' sotto
// --- Mercante condiviso (0.56.9) ---------------------------------------------------------------------------------------------
// Studio del 03/11 (docs, "Studio del mercante condiviso"). Con il secondo mouse il mercante e' una quarta finestra dei due lati:
// sopra lo zaino di ogni eroe la sua copia, con la sua scheda attiva (g_tabCopyM) e le letture dell'eroe deviate all'eroe della
// copia, quindi ognuno compra e vende con il suo oro e il suo zaino dalla stessa merce. La merce di una scheda e' una sola voce
// dell'inventario (pagina cUI_Merchandise +0x154) con la sua origine sullo schermo (+0x1164/+0x1166), fissa: la sposta DwShift
// con il lato (0.56.10; nella 0.56.9 si pensava la scrivesse il disegno) e dopo ogni disegno se ne tiene una fotografia per lato
// per la croce dei pad (MerchSnap). La conferma dell'acquisto (cUI_Purchase, vtable 0x895a08, una sola) e' di chi l'ha
// aperta: l'altro aspetta, e quella del giocatore 2 si conferma con le letture dell'eroe deviate all'eroe 2 (l'OK manda alla merce
// l'evento 0x2e che compra con l'id del giocatore). La sessione con il PNG resta di chi ha parlato (0x52af80: un interlocutore).
constexpr DWORD kVtMerchandise = 0x00895968, kVtPurchase = 0x00895a08;
struct MerchOrg { DWORD entry = 0; WORD x = 0, y = 0; DWORD at = 0; };
MerchOrg g_mOrg[2];                      // [0] lato della finestra vera, [1] copia
std::atomic<LONG> g_dwMerX{ 0 }, g_dwMerY{ 0 }, g_dwMerRealDx{ 0 }, g_dwMerCopyDx{ 0 };
std::atomic<DWORD> g_dwMerAt{ 0 };       // ultimo disegno del mercante condiviso
std::atomic<DWORD> g_dwShAt{ 0 };        // 0.56.14: ultimo disegno di una finestra condivisa (mercante, fabbro, Maestro)
std::atomic<int> g_dwShK{ -1 };          // e quale (indice di kDualWin)
std::atomic<int> g_purchOwner{ 0 }, g_merchClicker{ 0 };
std::atomic<DWORD> g_merchClickAt{ 0 };   // ultimo clic sulla merce (per il proprietario della conferma che si apre)
static DWORD MerchActiveEntry(DWORD win)
{
    const DWORD ctl = TabCtlOf(win);
    DWORD beg = 0, end = 0, page = 0, vt = 0, e = 0; WORD idx = 0;
    if (!ctl || !Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7c, end) || !Rd(ctl + 0x84, idx) || end <= beg || idx >= (end - beg) / 24) return 0;
    if (!Rd(beg + idx * 24 + 0x14, page) || !page || !Rd(page, vt) || vt != kVtMerchandise || !Rd(page + 0x154, e)) return 0;
    return e;
}
static void MerchSnap(int side, DWORD win, int k)
{
    DWORD e = 0;
    if (k == 4) Rd(win + 0x154, e);                 // 0.56.14: griglia di lavoro del fabbro
    else if (k == kDwChest) Rd(win + 0x158, e);     // 0.56.54: deposito
    else e = MerchActiveEntry(win);
    MerchOrg o;
    if (e && Rd(e + 0x1164, o.x) && Rd(e + 0x1166, o.y)) { o.entry = e; o.at = GetTickCount(); g_mOrg[side] = o; }
}
std::atomic<DWORD> g_purchSeenAt{ 0 };    // ultimo disegno della conferma (gancio sul suo vtable +0x14)
std::atomic<DWORD> g_purchSelf{ 0 };      // 0.56.54: la conferma disegnata per ultima
// 0.56.54 (prova sulla Steam Machine del 07/11 alle 16:11:58, foto 161207: "se premo a su un oggetto del mercante, quando mi chiede
// se acquistarlo, a deve acquistarlo mentre b deve cliccare su no"). Il gestore dei clic della conferma (0x6dda90) confronta il
// mouse con tre rettangoli dell'oggetto, {x int, y int, larghezza short, altezza short}: +0x160 compra (0x6dd5c0), +0x16c e
// +0x178 chiudono senza comprare (0x6dd820). Coordinate dello schermo, scritte dal disegno (anche con la conferma spostata
// nella colonna di chi compra). owner: giocatore che l'ha aperta (0 se non si sa).
bool MemoryPurchaseButtons(int& owner, RECT& yes, RECT& no)
{
    const DWORD seen = g_purchSeenAt.load(), w = g_purchSelf.load();
    if (!w || !seen || GetTickCount() - seen >= 250) return false;
    LONG x[2] = {}, y[2] = {};
    short wh[2][2] = {};
    if (!Rd(w + 0x160, x[0]) || !Rd(w + 0x164, y[0]) || !RdBuf(w + 0x168, wh[0], 4) ||
        !Rd(w + 0x16c, x[1]) || !Rd(w + 0x170, y[1]) || !RdBuf(w + 0x174, wh[1], 4)) return false;
    if (wh[0][0] <= 0 || wh[0][1] <= 0 || wh[1][0] <= 0 || wh[1][1] <= 0) return false;
    yes = RECT{ x[0], y[0], x[0] + wh[0][0], y[0] + wh[0][1] };
    no = RECT{ x[1], y[1], x[1] + wh[1][0], y[1] + wh[1][1] };
    owner = g_purchOwner.load();
    return true;
}
static DWORD PurchaseWin()
{
    const DWORD seen = g_purchSeenAt.load();
    if (seen && GetTickCount() - seen < 250) return 1;
    DWORD mgr = 0;
    if (!Rd(kUIManager, mgr) || !mgr) return 0;
    for (DWORD off = 0x80; off < 0x200; off += 4) {
        DWORD w = 0, vt = 0, f = 0;
        if (Rd(mgr + off, w) && w && !(w & 3) && Rd(w, vt) && vt == kVtPurchase && Rd(w + 0x10, f) && (f & 1)) return w;
    }
    return 0;
}
bool MemoryMerchantShared() { const DWORD t = g_dwMerAt.load(); return t && static_cast<LONG>(GetTickCount() - t) < 400; }
int MemorySharedWin()
{
    const DWORD t = g_dwShAt.load();
    const int k = g_dwShK.load();
    return (t && static_cast<LONG>(GetTickCount() - t) < 400 && k >= 3 && k < kDw) ? kDualWin[k].index : -1;
}
LONG MemorySharedRealDx() { return MemorySharedWin() >= 0 ? g_dwMerRealDx.load() : 0; }
// Per le letture del pad 1 (thread della mod, finestre al posto del gioco): dove le disegna il suo lato.
LONG MemoryMerchantRealDx() { return MemoryMerchantShared() ? g_dwMerRealDx.load() : 0; }
void MemoryMerchantFixGrid(GridInfo& g)
{
    const MerchOrg o = g_mOrg[0];
    if (MemorySharedWin() < 0 || o.entry != g.entry || GetTickCount() - o.at > 1000) return;
    g.x = g.ox = o.x; g.y = g.oy = o.y;
}
// Schede della copia del mercante (per LB / RB del pad 2): i pulsanti come li disegna la copia.
bool MemoryMerchantCopyTabs(int& active, int& count, RECT* rects, int maxRects)
{
    // 0.56.14: dal Maestro le schede sono quelle della sua istanza dell'eroe 2 (al posto del gioco: si sposta come la copia)
    const int sw = MemorySharedWin();
    if (!g_ok || (sw != 4 && sw != 14)) return false;
    const DWORD win = sw == 4 ? VisibleWindow(4, 0x008959b8) : (g_instOpen[5] ? g_inst[5] : 0);
    const DWORD ctl = win ? TabCtlOf(win) : 0;
    DWORD beg = 0, end = 0;
    if (!ctl || !Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7c, end) || end < beg || (end - beg) / 24 > 8) return false;
    count = static_cast<int>((end - beg) / 24);
    const TabState t = g_tabCopyM;
    WORD act = 0;
    if (sw == 14) { Rd(ctl + 0x84, act); active = act; }
    else active = t.have ? t.active : 0;
    int wx = 0, wy = 0;
    AbsPos(win, wx, wy);
    for (int k = 0; k < count && k < maxRects; ++k) {
        DWORD b = 0; short wh[2] = {}; int x = 0, y = 0;
        if (!Rd(beg + k * 24 + 0x10, b) || !b || !RdBuf(b + 0x2c, wh, sizeof(wh))) return false;
        AbsPos(b, x, y);
        if (sw == 14) x += g_dwMerCopyDx.load();    // istanza al posto del gioco fuori dal disegno
        else { x += g_dwMerX.load() + g_dwMerCopyDx.load() - wx; y += g_dwMerY.load() - wy; }
        rects[k] = RECT{ x, y, x + wh[0], y + wh[1] };
    }
    return count > 0;
}
// Griglia della merce come la disegna la copia (per la croce del pad 2).
bool MemoryMerchantCopyGrid(GridInfo& g)
{
    const int sw = MemorySharedWin();
    if (!g_ok || (sw != 4 && sw != 3 && sw != 16)) return false;
    const MerchOrg o = g_mOrg[1];
    if (!o.entry || GetTickCount() - o.at > 1000 || !ReadGrid(o.entry, g)) return false;
    g.x = g.ox = o.x; g.y = g.oy = o.y; g.inv = -1; g.kind = sw == 3 ? "fabbro (eroe 2)" : sw == 16 ? "deposito (eroe 2)" : "merce (eroe 2)";
    return true;
}
// 0.56.11: la conferma sta nella colonna di chi l'ha aperta. 0.56.12: prova delle 17:59 (foto: la conferma del giocatore 1 a destra;
// "per premere si' ho premuto invio perche' lo 0 non me lo comprava"): il disegno (0x6dcff0) rimette lui la posizione della finestra
// a ogni fotogramma, +0x24 = costante - larghezza / 2 (0x6dd21d mov ecx, 0x140, costante che pHD ha gia' portato a 0x140 + 896) e
// +0x28 = 0x72, quindi lo spostamento della 0.56.11 era cancellato durante il disegno e negli eventi spostava la finestra a sinistra
// di dove si vedeva (il clic dello 0 mancava il pulsante). Ora si cambia quella costante prima del disegno: la finestra e' dove si
// vede anche per eventi e punto dentro, senza altro.
static LONG PurchaseDx()
{
    if (!MemoryMouse2Ready()) return 0;
    DualLayout L;
    if (!DualLayoutNow(L) || !L.vis[3]) return 0;
    const int o = g_purchOwner.load() ? g_purchOwner.load() : g_merchClicker.load();
    return o == 2 ? L.off2[3].x : L.off1[3].x;
}
static void PurchaseSetX(LONG dx)
{
    static LONG base = 0, now = 0;
    static int state = 0;                        // 0 da vedere, 1 pronto, -1 istruzione inattesa
    if (state == 0) {
        BYTE op = 0;
        state = (Rd(0x006dd21d, op) && op == 0xB9 && Rd(0x006dd21e, base)) ? 1 : -1;
        now = base;
        Log(state > 0 ? "MERCANTE: conferma dell'acquisto: x del centro %ld (0x6dd21e), spostata con la colonna di chi compra"
                      : "MERCANTE: conferma dell'acquisto: istruzione inattesa a 0x6dd21d, resta al posto del gioco", base);
    }
    if (state < 0 || base + dx == now) return;
    const LONG v = base + dx;
    if (PatchCode(0x006dd21e, reinterpret_cast<const BYTE*>(&v), sizeof(v))) now = v;
}
// Conferma dell'acquisto: eventi del suo mouse, con l'eroe 2 per le letture deviate se e' il giocatore 2.
WinInputFn g_purchInput = nullptr;
static int DwEvType(DWORD ev);                   // piu' sotto
char __fastcall PurchaseInputHook(DWORD self, DWORD edx, DWORD ev)
{
    if (!ev || !MemoryMouse2Ready()) return g_purchInput(self, edx, ev);
    const int type = DwEvType(ev);
    if (type != 2 && type != 3) return g_purchInput(self, edx, ev);
    const int who = (g_m2 && Mouse2Cur() == g_m2) ? 2 : 1;
    const int owner = g_purchOwner.load();
    if (owner && who != owner) {
        static DWORD toastAt = 0;
        if (type == 2 && GetTickCount() - toastAt > 2000) {
            toastAt = GetTickCount();
            OverlayCoopToast(Tr(owner == 1 ? "Aspetta: il giocatore 1 sta confermando un acquisto" : "Aspetta: il giocatore 2 sta confermando un acquisto"), 2000);
        }
        return 1;
    }
    if (who != 2) return g_purchInput(self, edx, ev);
    DWORD om = 0;
    Rd(kObjMgr, om);
    const DWORD h2 = om ? ObjectAddr(om, 2) : 0;
    const DWORD oldHero = g_uiHero, oldOn = g_uiOn, oldFake = g_uiFakePlayer[0x14 / 4];
    if (h2) { g_uiFakePlayer[0x14 / 4] = 2; g_uiHero = h2; g_uiOn = 1; }
    // l'OK manda l'evento 0x2e alla pagina attiva del mercante, che compra l'oggetto che lei ha in +0x3dc: deve essere la pagina
    // della copia (quella su cui il giocatore 2 ha scelto), non quella del lato del giocatore 1
    const DWORD mwin = VisibleWindow(4, 0x008959b8);
    const DWORD mctl = mwin ? TabCtlOf(mwin) : 0;
    TabState mine;
    const bool swapTabs = mctl && g_tabCopyM.have && TabRead(mctl, mine);
    if (swapTabs) TabWrite(mctl, g_tabCopyM);
    const char r = g_purchInput(self, edx, ev);
    if (swapTabs) { TabRead(mctl, g_tabCopyM); TabWrite(mctl, mine); }
    g_uiHero = oldHero; g_uiOn = oldOn; g_uiFakePlayer[0x14 / 4] = oldFake;
    if (type == 2) Log("MERCANTE: conferma dell'acquisto del giocatore 2 (eroe 2)");
    return r;
}
WinDraw3Fn g_purchDraw = nullptr;
void __fastcall PurchaseDrawHook(DWORD self, DWORD edx, DWORD ctx, DWORD a, DWORD b)
{
    // 0.56.11: il gestore chiama il disegno a ogni fotogramma anche con la conferma chiusa (la funzione guarda +0x10 da se'): nella
    // prova delle 16:30 la conferma risultava sempre aperta e LB/RB del pad 2 davano "il giocatore 1 sta confermando un acquisto"
    DWORD f = 0;
    if (!Rd(self + 0x10, f) || !(f & 1)) { g_purchDraw(self, edx, ctx, a, b); return; }
    g_purchSeenAt.store(GetTickCount() | 1);
    g_purchSelf.store(self);
    PurchaseSetX(PurchaseDx());
    g_purchDraw(self, edx, ctx, a, b);
}
static void InstallPurchaseHook()
{
    DWORD cur = 0, o = 0, d = 0;
    // 0.56.10: il gestore la disegna con +0x44 (0x6dcff0, (dispositivo, 0, 0), ret 0xc; e' in [gestore+0xb4], 0x759009), come il
    // mercante; la 0.56.9 agganciava +0x14 e la trovava solo con la ricerca fra le finestre del gestore (PurchaseWin)
    if (Rd(kVtPurchase + 0x44, d) && d == 0x006dcff0) {
        g_purchDraw = reinterpret_cast<WinDraw3Fn>(static_cast<uintptr_t>(d));
        PatchVtSlot(kVtPurchase + 0x44, d, reinterpret_cast<void*>(&PurchaseDrawHook), o);
    }
    if (!Rd(kVtPurchase + 0x10, cur) || cur != 0x006dda90) { Log("MERCANTE: conferma dell'acquisto non agganciata (vtable %08lX)", cur); return; }
    g_purchInput = reinterpret_cast<WinInputFn>(static_cast<uintptr_t>(cur));
    if (PatchVtSlot(kVtPurchase + 0x10, cur, reinterpret_cast<void*>(&PurchaseInputHook), o)) Log("MERCANTE: conferma dell'acquisto agganciata (una per volta, di chi l'ha aperta)");
}

// --- Fabbro e Maestro condivisi (0.56.14) ---------------------------------------------------------------------------------
// Studio del 04/11. Il mercante si condivide con una finestra sola perche' la merce e' di tutti e due; il fabbro e il Maestro
// invece hanno un lavoro per giocatore: il fabbro il piano 3x5 con l'oggetto da incastonare ([+0x154], una voce dell'inventario
// presa da 0x5df160 nel costruttore 0x6a2140) e le quattro caselle delle gemme (blocchi da 0x10c da +0x3cc, oggetti in
// +0x4c8/+0x4cc), il Maestro le sue pagine Combo, Lotto e Portali (create da 0x6d1540, ognuna con il suo stato). Per l'eroe 2 la
// mod crea un secondo fabbro e un secondo Maestro con i costruttori del gioco (operator new 0x848782(0x7fc) + 0x6a2140,
// 0x848782(0x1dc) + 0x6d1150), fuori dal gestore: il lato dell'eroe 2 disegna e clicca quelli (spostati come la copia), con le
// letture dell'eroe deviate all'eroe 2 (come la copia) e il posto del gestore (n.3 o n.14) che indica l'istanza per la durata
// della chiamata, cosi' il codice che cerca "il fabbro" nel gestore trova il suo. Apertura: le creature del PNG passano
// dalla finestra vera (+0x158/+0x15c del fabbro, +0x1d4/+0x1d8 del Maestro) e si chiama il suo onShow(1) (vf24: suono,
// partner del giocatore 2 per l'inventario). Chiusura: onShow(0) con le letture dell'eroe 2: il fabbro rimette nel suo zaino
// l'oggetto e le gemme rimasti (0x6a31f0, GetHeroId deviato a 0x6a323a) o li posa a terra accanto al PNG, il Maestro chiude le
// pagine. Si chiude quando il lato dell'eroe 2 non e' disegnato da 600 ms (lato chiuso, finestra chiusa, menu) e prima di ogni
// salvataggio della partita.
constexpr DWORD kInstSize[kDw] = { 0, 0, 0, 0, 0x7fc, 0x1dc };
constexpr DWORD kInstCtor[kDw] = { 0, 0, 0, 0, 0x006a2140, 0x006d1150 };
constexpr DWORD kInstShow[kDw] = { 0, 0, 0, 0, 0x006a4360, 0x006d1440 };
struct MgrSlotScope {                    // il posto n del gestore indica w per la durata del blocco
    DWORD slot = 0, old = 0, w = 0;
    MgrSlotScope(int index, DWORD win) : w(win) {
        DWORD mgr = 0;
        if (index < 0 || !win || !Rd(kUIManager, mgr) || !mgr) return;
        slot = mgr + 0x80 + index * 4;
        if (!Rd(slot, old) || !old || old == win) { slot = 0; return; }
        WriteDword(slot, win);
    }
    ~MgrSlotScope() { DWORD cur = 0; if (slot && Rd(slot, cur) && cur == w) WriteDword(slot, old); }
};
struct Hero2Scope {                      // letture dell'eroe deviate all'eroe 2, come nella copia (DwCopyCtx)
    DWORD oldHero, oldOn, oldFake; bool on = false;
    Hero2Scope() : oldHero(g_uiHero), oldOn(g_uiOn), oldFake(g_uiFakePlayer[0x14 / 4]) {
        DWORD om = 0;
        const DWORD h2 = Rd(kObjMgr, om) && om ? ObjectAddr(om, 2) : 0;
        ++g_dwNest;
        if (h2) { g_uiFakePlayer[0x14 / 4] = 2; g_uiHero = h2; g_uiOn = 1; on = true; }
    }
    ~Hero2Scope() { g_uiHero = oldHero; g_uiOn = oldOn; g_uiFakePlayer[0x14 / 4] = oldFake; --g_dwNest; }
};
static DWORD InstFor(int k)
{
    if (k < 4 || k >= kDw) return 0;
    if (g_inst[k]) return g_inst[k];
    static bool failed[kDw] = {};
    if (failed[k]) return 0;
    DWORD m = 0;
    __try { m = reinterpret_cast<GameNewFn>(0x00848782)(kInstSize[k]); }
    __except (EXCEPTION_EXECUTE_HANDLER) { m = 0; }
    const CallResult c = m ? CallThis0(kInstCtor[k], m) : CallResult{};
    DWORD vt = 0;
    if (!m || !c.ok || !c.balanced || !Rd(m, vt) || vt != kDualWin[k].vt) {
        failed[k] = true;
        LogException("costruzione dell'istanza della mod");
        Log("CONDIVISO: %s dell'eroe 2 NON creato (memoria %08lX, vtable %08lX): resta un lato solo", kDualWin[k].name, m, vt);
        return 0;
    }
    g_inst[k] = m;
    DWORD e = 0; if (k == 4) Rd(m + 0x154, e);
    Log("CONDIVISO: %s dell'eroe 2 creato in %08lX%s", kDualWin[k].name, m, k == 4 ? " (piano di lavoro tutto suo)" : " (pagine tutte sue)");
    return m;
}
static void InstShowCall(int k, DWORD inst, bool show)
{
    LONG mx = 0, my = 0;
    if (g_m2) { Rd(g_m2 + 4, mx); Rd(g_m2 + 8, my); }
    Hero2Scope hs;
    Mouse2Scope ms(true, mx, my);
    MgrSlotScope sl(kDualWin[k].index, inst);
    const CallResult c = CallThis1(kInstShow[k], inst, show ? 1 : 0);
    if (!c.ok || !c.balanced) { g_dwFaults.fetch_add(1); LogException(show ? "apertura dell'istanza della mod" : "chiusura dell'istanza della mod"); }
}
static void InstOpenNow(int k, DWORD real, DWORD zainoPage)
{
    const DWORD inst = InstFor(k);
    if (!inst || g_instOpen[k]) return;
    if (k == 4) { DWORD npc = 0; WORD w = 0; Rd(real + 0x158, npc); Rd(real + 0x15c, w); WriteDword(inst + 0x158, npc); SetWord(inst + 0x15c, w); }
    else { DWORD npc = 0; WORD w = 0; Rd(real + 0x1d8, npc); Rd(real + 0x1d4, w); WriteDword(inst + 0x1d8, npc); SetWord(inst + 0x1d4, w); }
    // l'apertura del fabbro manda al gestore "scheda dello zaino" (0x6a4360): la pagina dello zaino resta all'eroe che aveva
    // il gestore, quando apre una finestra, accende i bit 0 e 2 di +0x10 e poi chiama il suo onShow (0x726980, la base, non li
    // tocca): per l'istanza li accende la mod come sono nella finestra vera
    DWORD rf = 0, inf = 0;
    if (Rd(real + 0x10, rf) && Rd(inst + 0x10, inf)) WriteDword(inst + 0x10, inf | (rf & 5u));
    WORD idx = 0;
    const bool pg = zainoPage && Rd(zainoPage + 0x158, idx);
    InstShowCall(k, inst, true);
    WORD now = 0;
    if (pg && Rd(zainoPage + 0x158, now) && now != idx) SetWord(zainoPage + 0x158, idx);
    g_instOpen[k] = true;
    g_instDrawAt[k] = GetTickCount();
    DWORD npc = 0; Rd(inst + (k == 4 ? 0x158 : 0x1d8), npc);
    Log("CONDIVISO: %s dell'eroe 2 aperto (PNG %lu)", kDualWin[k].name, npc);
}
static void InstCloseNow(int k, const char* why)
{
    if (k < 4 || k >= kDw || !g_instOpen[k] || !g_inst[k]) return;
    g_instOpen[k] = false;
    // controllo del 04/11: l'onShow di fabbro e Maestro scrive il PNG con cui si tratta in un posto solo della tabella degli
    // inventari (0x5df2e0: [tabella+0x23000] = PNG, [+0x23004] = word; il giocatore che gli si passa non lo usa), e alla chiusura
    // lo azzera: chiudendo l'istanza dell'eroe 2 si toglierebbe il PNG al giocatore 1 che e' ancora li'. Si rimette se la
    // finestra vera e' ancora aperta.
    DWORD table = 0, pNpc = 0; WORD pW = 0;
    const bool partner = Rd(kInvTable, table) && table && Rd(table + 0x23000, pNpc) && Rd(table + 0x23004, pW);
    InstShowCall(k, g_inst[k], false);
    if (partner && VisibleWindow(kDualWin[k].index, kDualWin[k].vt)) { WriteDword(table + 0x23000, pNpc); SetWord(table + 0x23004, pW); }
    DWORD inf = 0;
    if (Rd(g_inst[k] + 0x10, inf)) WriteDword(g_inst[k] + 0x10, inf & ~5u);
    Log("CONDIVISO: %s dell'eroe 2 chiuso (%s)%s", kDualWin[k].name, why, k == 4 ? ": cio' che era sul piano torna nel suo zaino" : "");
}
static void DwInstCloseAll(const char* why) { for (int k = 4; k < kDw; ++k) if (DwHasInst(k)) InstCloseNow(k, why); }
static void DwInstForget() { for (int k = 4; k < kDw; ++k) g_instOpen[k] = false; }
static void DwInstTick()                  // thread del motore, a ogni fotogramma
{
    const DWORD now = GetTickCount();
    for (int k = 4; k < kDw; ++k)
        if (DwHasInst(k) && g_instOpen[k] && static_cast<LONG>(now - g_instDrawAt[k]) > 600) InstCloseNow(k, "lato dell'eroe 2 non piu' disegnato");
}

// --- 0.56.28: vista del compagno (prova) ---------------------------------------------------------------------------------
// il tester, 04/11: "poter aprire l'inventario e le statistiche di un alleato che ci segue, per potergli equipaggiare oggetti etc.
// gia' adesso se prendi un arma e la premi sul suo ritratto, lui la equipaggia, pero' finisce li'". Studio del 04/11: per il
// gioco eroi e compagni sono cCreature, con gli stessi 19 slot di equipaggiamento a +0x1a4 (0x557210 legge, 0x5560c0 mette,
// 0x556cf0 toglie: l'oggetto di base del compagno non si toglie e si ricrea, 0x426580 dal tipo) e le stesse statistiche. La
// finestra dell'equipaggiamento (n.2, cUI_Equipment: eventi 0x6b67c0, disegno 0x6b7830) prende la creatura con GetHeroId
// (chiamate gia' deviate per l'eroe 2, g_uiHero) e usa l'indice della casella come numero dello slot; il clic sinistro con un
// oggetto in mano controlla vita (0x549480), uso (0x54afe0: classe e requisiti della creatura) e casella (0x43efc0, solo dal
// tipo dell'oggetto) e mette l'oggetto; a mani vuote lo toglie e lo da' in mano. Il ritratto del compagno (cUI_Mercenary n.7,
// 0x6d6d50 -> 0x6d6840) accetta solo armi e scudi (mani 0xc e 0xd), e prima 0x6d6c70 controlla l'oggetto in mano: armi del tipo
// che il compagno sa usare (tabella per tipo di creatura 0x4116c0/0x4118a0) o scudi.
// Qui: clic destro a mani vuote sul ritratto di un compagno (giocatore 1) = vista del compagno. Le finestre di equipaggiamento
// e statistiche si disegnano con GetHeroId sul compagno (g_uiHero, ma [giocatore+0x14] resta l'eroe 1: g_uiOn = 0); lo zaino
// resta dell'eroe 1, l'oggetto in mano e' uno solo e passa dallo zaino al compagno e indietro. Problemi gestiti:
// (1) la disposizione delle caselle si sceglie dalla classe dell'eroe (+0x10, 1..9; il compagno ha un altro tipo e la finestra
// resterebbe vuota): durante le chiamate per il compagno +0x154 della finestra vale il tipo del compagno, cosi' resta la
// disposizione gia' costruita per l'eroe 1, e poi torna com'era; (2) le due chiamate a 0x43efc0 degli eventi (0x6b6d4d clic,
// 0x6b740d messaggio 0x2c) passano da SlotFitStub, che con la vista accesa per le mani chiede anche 0x6d6c70 al ritratto del
// compagno (le regole del ritratto). 0.56.30 (prova della 0.56.29, 04/11 15:03, foto: "non mi fa equipaggiare una armatura a
// lei, come faccio ad equipaggiargli le armi se non ha slot visibili?"): la 0.56.29 rifiutava tutto tranne le mani, ma la
// finestra dell'equipaggiamento di Sacred non ha le mani (le armi passano dalla barra), e la corazza aveva gia' passato i
// controlli del gioco (0x54afe0 col tipo del compagno: casella verde nella foto). Ora le altre caselle le decide il gioco, come
// per l'eroe; le armi si danno sul ritratto (0x6d6840, il gioco); (3) il clic destro sull'equipaggiamento (manda l'oggetto
// allo zaino, che per il compagno non esiste) e il messaggio 0x2c dallo zaino non passano: si trascina col sinistro; 0.56.30:
// anche il clic destro sugli oggetti dello zaino (equipaggiava l'eroe 1: "se premo il . lo equipaggia a me");
// (4) le statistiche si guardano: i clic non arrivano; (5) un errore spegne la vista. Si chiude con un altro clic destro sul
// ritratto, chiudendo lo zaino, o se il compagno sparisce. [Native] CompanionView.
// 0.56.31 (il tester, 04/11: "un modo piu' comodo per aprire l'inventario del compagno? e che possa convivere anche con la modalita'
// a due inventari? e che si veda anche gli slot del compagno per le armi? e che si veda se livelli e che gli possa mettere le
// statistiche"; studio del 04/11): (a) RT in modalita' cursore (pad 1, e pad 2 con il suo zaino aperto) gira la colonna del
// giocatore: il suo eroe, il compagno del primo ritratto, il secondo... e di nuovo l'eroe (MemoryCompanionCycle); il clic destro
// sul ritratto resta, anche per il secondo mouse; (b) ogni colonna ha il suo compagno (g_compSide[1] per il giocatore 1, [2] per
// il giocatore 2): con i due zaini la finestra vera (colonna dell'eroe 1) e la copia (colonna dell'eroe 2, DwCopyCtx) si
// disegnano e ricevono i clic con il compagno della loro colonna, dentro gli spostamenti e le serie dei riquadri di sempre;
// (c) le mani: nessuna delle sei disposizioni delle classi (0x6b9900, 0x6b83c0, 0x6b8a10, 0x6b9180, 0x6b9ed0, 0x6ba640) crea le
// caselle 0xc e 0xd, ma tutte lasciano libero l'angolo in alto a sinistra (prima colonna a x 16 da y 76-80, elmo a x 96 y 6). Le
// caselle sono cUI_Static2 (operator new 0x7c, costruttore 0x7533a0 (nome, rettangolo x/y/w/h da 12 byte, genitore, trama,
// coordinate della trama {u0, v0, u1, v1} -> +0x54 u0, +0x58 u1, +0x5c v0, +0x60 v1, 0x4005), ret 0x20; la base 0x731730 non le
// iscrive fra i figli della finestra; disegno 0x753430: quadrato con la trama +0x78 tinta con +0x14, solo con +0x10 bit 0).
// 0.56.33 (prova della 0.56.32, 04/11 16:01-16:03, due foto; il tester: "lo slot armi e' strano", "ho premuto sull'arco ed e' sparito,
// non ho capito dove", "armatura, scudo e torcia le ha messe correttamente"): la 0.56.31 copiava le coordinate della trama
// dalla casella dei guanti nell'ordine di +0x54..+0x60 (u0, u1, v0, v1) invece di {u0, v0, u1, v1}, e le caselle 36x64 in alto
// a sinistra erano troppo piccole: l'icona si disegna alla sua misura, centrata (0x6b7830: celle * 32), e l'arco (curva
// arancione nella foto) e lo scudo uscivano dalla casella. Ora due caselle 64x128 (ci sta qualunque arma o scudo, 2x4 celle)
// sotto la finestra dell'equipaggiamento, a x 16 e 96, con un pezzo dello sfondo della finestra (+0x1dc, cUI_Static2) come
// trama e una cornice disegnata dall'overlay (MemoryCompHands); fuori dalla finestra la sua prova del punto (+0x1c) le esclude,
// quindi i clic sulle mani li fa tutti la mod: con un oggetto in mano 0x6d6c70 poi 0x6d6840, a mani vuote 0x6d6840 (il gioco
// rida' prima la seconda mano, poi la prima). Le caselle si mettono in
// +0x18c/+0x190 della finestra solo durante le chiamate per il compagno (CompHands), con le loro schede dell'immagine
// (+0x314 + slot*0x14, +0x12 = tipo della casella: 5 arma, 0xd scudo, per il verde e il rosso) tenute da parte per ogni colonna:
// il disegno generico le tratta come le altre (0x6b76d0 carica l'immagine, poi icona, colore e descrizione). Un'arma o uno
// scudo in mano posati su una mano passano dalla regola del ritratto (0x6d6c70, poi 0x6d6840: sceglie la mano, scambia, rida' in
// mano quella di prima, due mani comprese); a mani vuote il clic sulla mano la toglie (0x556cf0 dell'equipaggiamento);
// (d) le statistiche: i clic passano (0x6a6270 lavora tutto sulla creatura di GetHeroId: "+" degli attributi 0x581140 con i
// punti +0x3cb, abilita' 0x582750 con i punti +0x3ea, abilita' nuova con +0x3ec e +0x3e1, pagina con il pulsante del titolo);
// (e) l'esperienza: il gioco non ne da' ai compagni (Laurelinad con 0 dopo i combattimenti); con [Native] CompanionXp ogni
// aumento dell'esperienza dell'eroe 1 (come per l'eroe 2, per XpFactor, entro ShareXpRange) va anche ai compagni dei ritratti,
// con addExperience 0x57e410 sul loro oggetto delle statistiche (livelli, punti attributo e abilita' del gioco); a ogni livello
// nuovo un avviso e il suono.
static int DwEvType(DWORD ev);
bool MemoryHeldAny();
volatile LONG g_compLimit = 0;           // SlotFitStub attivo (solo durante gli eventi dell'equipaggiamento per il compagno)
volatile LONG g_compRefused = 0;
volatile DWORD g_compCurId = 0;          // compagno della chiamata in corso (CompScope), per SlotFitStub
// 0.56.34 (il tester, 04/11: "puo' equipaggiare l'equipaggiamento che posso io ma non quello dell'elfo silvano, io farei che i
// compagni possono equipaggiare qualsiasi equipaggiamento o arma", "ci sono anche compagni umani normali che non hanno classi
// di appartenenza"): il controllo di classe e' il metodo +0x38 dell'oggetto (0x5bfc80 per cItemBase, cWeapon3D, cShield3D,
// cRing3D, cBsItem3D, cPotion3D, cScroll3D, cSpecial3D; 0x5ba550 per cArmor3D), chiamato da 0x54afe0 con il tipo della creatura:
// confronta i bit di classe dell'oggetto (+0x11c) con il tipo, e alcuni tipi di compagni (0x69, 0x84-0x86, 0x13f-0x141,
// 0x146-0x147, 0x2b5) li tratta come una classe d'eroe (Maegalcarwen, tipo 0x86 = 134). Con [Native] CompanionAnyClass e la
// vista del compagno in corso (g_compAnyClass, solo durante le chiamate per il compagno) il metodo risponde si' per il tipo del
// compagno; il resto di 0x54afe0 (requisiti dell'oggetto: forza, destrezza, livello...) resta. Le armi passano anche da
// 0x6d6c70 (le mosse del corpo del compagno per quel tipo d'arma, tabella 0x4116c0/0x4118a0): quello resta, un'arma senza mosse
// il gioco non saprebbe animarla.
volatile LONG g_compAnyClass = 0;
volatile DWORD g_compCurType = 0;
__declspec(naked) void ClassFitStubItem()  // al posto di 0x5bfc80 nei vtable (thiscall, ECX = oggetto; tipo della creatura; ret 4)
{
    __asm {
        cmp  g_compAnyClass, 0
        je   orig
        mov  eax, [esp + 4]
        cmp  eax, g_compCurType
        jne  orig
        mov  eax, 1
        ret  4
    orig:
        mov  eax, 0x005bfc80
        jmp  eax
    }
}
__declspec(naked) void ClassFitStubArmor() // al posto di 0x5ba550 (cArmor3D)
{
    __asm {
        cmp  g_compAnyClass, 0
        je   orig
        mov  eax, [esp + 4]
        cmp  eax, g_compCurType
        jne  orig
        mov  eax, 1
        ret  4
    orig:
        mov  eax, 0x005ba550
        jmp  eax
    }
}
struct CompClassScope {                  // il controllo di classe lascia passare il tipo del compagno
    LONG old; DWORD oldType;
    explicit CompClassScope(DWORD addr) : old(g_compAnyClass), oldType(g_compCurType) {
        DWORD t = 0;
        if (g_cfg.nativeCompanionAnyClass && addr && Rd(addr + 0x10, t) && t) { g_compCurType = t; g_compAnyClass = 1; }
    }
    ~CompClassScope() { g_compAnyClass = old; g_compCurType = oldType; }
};
bool g_compFitOk = false;                // SlotFitStub installato
bool g_compBad[3] = {};                  // per colonna: errore, vista spenta fino alla prossima apertura
DWORD g_compOnAt[3] = {}, g_compSeenAt[3] = {};   // thread del motore
std::atomic<bool> g_compCycleReq[3];     // 0.56.31: RT in modalita' cursore (Core.cpp, thread della mod)
static DWORD CompanionAddr(DWORD id)
{
    DWORD om = 0, vt = 0, hp = 0;
    int cls = 0; bool dead = false;
    const DWORD a = id && Rd(kObjMgr, om) && om ? ObjectAddr(om, id) : 0;
    if (!a || !Rd(a, vt) || vt != kVtCreature || !Rd(a + 0x4d8, hp) || !hp || !ReadKind(a, cls, dead) || dead) return 0;
    return a;
}
static DWORD MercWin()
{
    DWORD mgr = 0, w = 0, vt = 0;
    return Rd(kUIManager, mgr) && mgr && Rd(mgr + 0x80 + 7 * 4, w) && w && Rd(w, vt) && vt == 0x008958cc ? w : 0;
}
// 0.56.36 (il tester, 04/11: "io aggiungerei anche i pallini di esperienza al compagno a questo punto"): la barretta dell'esperienza
// sotto il ritratto (vedi MercXpLend, 0.55.61) c'e' solo nello slot 0. Il disegno dei ritratti (0x6d71d0) rifa' a ogni
// fotogramma gli slot 1..4 dall'elenco dei compagni dell'eroe (+0x39c) e li disegna da un solo punto, call 0x6d74b0 a
// 0x6d73e4 (ECX = finestra, (dispositivo, slot), ret 8): passa da MercSlotStub, che per un compagno (cCreature viva) presta
// allo slot la barretta dello slot 0 come per l'eroe 2; il riempimento lo calcola il gioco dalle statistiche della creatura.
void __stdcall MercSlotDraw(DWORD self, DWORD ctx, DWORD k)
{
    bool lend = false;
    if (g_cfg.nativeCompanionXp && k >= 1 && k <= 4) { DWORD id = 0; lend = Rd(self + 0x154 + k * 0x194, id) && CompanionAddr(id); }
    CallResult cr;
    if (lend) { MercXpLend xp(self, self + k * 0x194); cr = CallThis2(kMercSlotDraw, self, ctx, k); }
    else cr = CallThis2(kMercSlotDraw, self, ctx, k);
    static bool told = false;
    if ((!cr.ok || !cr.balanced) && !told) { told = true; LogException("ritratto di un compagno"); Log("COMPAGNO: ATTENZIONE: errore nel disegno del ritratto %lu", k); }
}
__declspec(naked) void MercSlotStub()     // al posto di call 0x6d74b0 a 0x6d73e4
{
    __asm {
        push dword ptr [esp + 8]        // slot
        push dword ptr [esp + 8]        // dispositivo
        push ecx                        // finestra
        call MercSlotDraw
        ret  8
    }
}
static int MercSlotOf(DWORD merc, DWORD id)  // ritratto 1..4 del compagno
{
    for (int k = 1; k <= 4; ++k) { DWORD v = 0; if (merc && Rd(merc + 0x154 + k * 0x194, v) && v == id) return k; }
    return -1;
}
static DWORD CompAddrSide(int side) { return side >= 1 && side <= 2 && !g_compBad[side] ? CompanionAddr(g_compSide[side].load()) : 0; }
// controllo del 04/11: la scelta di un'abilita' nuova (+0xf84 della finestra delle statistiche, 0x6a6270) e' della finestra, non
// della creatura (con i due zaini DwStatScope la tiene per colonna, g_statSide[0] vera, [1] copia): cambiando la creatura della
// colonna a meta' scelta, il clic seguente avrebbe insegnato l'abilita' all'altra. Si azzera a ogni cambio.
static void CompResetChoose(int side)
{
    if (side >= 1 && side <= 2) g_statSide[side - 1].choose = 0;
    DWORD mgr = 0, w = 0, vt = 0;
    if (Rd(kUIManager, mgr) && mgr && Rd(mgr + 0x80 + 8 * 4, w) && w && Rd(w, vt) && vt == 0x008952d8) WriteByte(w + 0xf84, 0);
}
static void CompanionOff(int side, const char* why)
{
    const DWORD id = g_compSide[side].exchange(0);
    if (!id) return;
    CompResetChoose(side);
    g_compSeenAt[side] = 0;
    Log("COMPAGNO: colonna del giocatore %d: vista del compagno %lu chiusa (%s)", side, id, why);
    OverlayCoopToast(Tr("Vista del compagno chiusa"), 2000);
}
static bool CompSideOpen(int side)          // la colonna del giocatore e' aperta (come realOn/copyOn di DualDraw)
{
    if (MemoryDualActive()) return side == 1 ? (g_dualSide1.load() || !g_dualSide2.load()) : g_dualSide2.load();
    return side == 1 && VisibleWindow(2, 0x00895490) != 0;
}
static bool CompanionOn(int side, DWORD id, const char* how)
{
    if (g_swapUiLive.load() || g_swapOrig.load() || g_saveOrig.load()) { OverlayCoopToast(Tr("Prima torna al tuo eroe"), 2500); return false; }
    const DWORD a = CompanionAddr(id);
    if (!a) return false;
    g_compBad[side] = false;
    g_compOnAt[side] = GetTickCount(); g_compSeenAt[side] = 0;
    CompResetChoose(side);
    g_compSide[side].store(id);
    DWORD type = 0; WORD lv = 0; Rd(a + 0x10, type); Rd(a + 0x3fe, lv);
    Log("COMPAGNO: colonna del giocatore %d: vista del compagno %lu (tipo %lu, livello %u, creatura %08lX) aperta (%s)", side, id, type, lv, a, how);
    OverlayCoopToast(Tr("Compagno: le sue armi nelle due caselle sotto l'equipaggiamento"), 4000);
    return true;
}
static void CompanionCycle(int side)       // RT: eroe -> compagno del primo ritratto -> ... -> eroe
{
    if (!g_cfg.nativeCompanionView || !g_compFitOk || !g_uiPatched.load()) return;
    if (!CompSideOpen(side)) { Log("COMPAGNO: RT del giocatore %d con la sua colonna chiusa: niente", side); return; }
    // 0.56.33: prova delle 16:03:52: il clic del giocatore 1 sulla barra dell'eroe 2 passa tutta l'interfaccia all'eroe 2 (i ritratti
    // sono del suo gruppo, senza compagni) e RT diceva "Nessun compagno al seguito"
    if (g_swapUiLive.load() || g_swapOrig.load() || g_saveOrig.load()) { OverlayCoopToast(Tr("Prima torna al tuo eroe"), 2500); return; }
    const DWORD merc = MercWin();
    DWORD ids[4] = {}; int n = 0;
    for (int k = 1; k <= 4 && merc; ++k) { DWORD id = 0; if (Rd(merc + 0x154 + k * 0x194, id) && id && CompanionAddr(id)) ids[n++] = id; }
    const DWORD cur = g_compSide[side].load();
    if (!n) {
        if (cur) CompanionOff(side, "nessun compagno");
        else OverlayCoopToast(Tr("Nessun compagno al seguito"), 2500);
        return;
    }
    int at = -1;
    for (int i = 0; i < n; ++i) if (ids[i] == cur) at = i;
    if (cur && at + 1 >= n) { CompanionOff(side, "RT: di nuovo il suo eroe"); return; }
    CompanionOn(side, ids[at + 1], "RT");
}
bool MemoryCompanionCycle(int who)         // 0.56.31: Core.cpp, thread della mod
{
    if (who < 1 || who > 2 || !g_compFitOk) return false;
    g_compCycleReq[who].store(true);
    return true;
}
static void CompanionTick()                // thread del motore, a ogni fotogramma
{
    for (int side = 1; side <= 2; ++side) {
        if (g_compCycleReq[side].exchange(false)) CompanionCycle(side);
        const DWORD id = g_compSide[side].load();
        if (!id) continue;
        const DWORD now = GetTickCount();
        if (!CompanionAddr(id)) CompanionOff(side, "il compagno non c'e' piu'");
        else if (MercSlotOf(MercWin(), id) < 0) CompanionOff(side, "non e' piu' fra i ritratti");
        else if (g_swapUiLive.load() || g_swapOrig.load() || g_saveOrig.load()) CompanionOff(side, "scambio dell'interfaccia o salvataggio");
        else if (g_compSeenAt[side] ? static_cast<LONG>(now - g_compSeenAt[side]) > 600 : static_cast<LONG>(now - g_compOnAt[side]) > 2500) CompanionOff(side, "colonna chiusa");
    }
}
char __stdcall CompSlotFit(DWORD slot)      // da SlotFitStub: 1 = la casella puo' prendere l'oggetto (poi decide 0x43efc0)
{
    const WORD s = static_cast<WORD>(slot);
    if (s != 0xc && s != 0xd) return 1;          // 0.56.30: armature e gioielli come per l'eroe (decidono 0x54afe0 e 0x43efc0)
    const DWORD merc = MercWin();
    const int k = MercSlotOf(merc, g_compCurId);
    const CallResult c = k > 0 ? CallThis1(0x006d6c70, merc, static_cast<DWORD>(k)) : CallResult{};
    if (!c.ok || !c.balanced || !(c.ret & 0xff)) { ++g_compRefused; return 0; }
    return 1;
}
__declspec(naked) void SlotFitStub()     // al posto di call 0x43efc0 (thiscall, ECX = [0xaaa564]; slot, tipo; ret 8)
{
    __asm {
        cmp  g_compLimit, 0
        je   orig
        push ecx
        push dword ptr [esp + 8]        // slot
        call CompSlotFit
        pop  ecx
        test al, al
        jnz  orig
        xor  eax, eax
        ret  8
    orig:
        mov  eax, 0x0043efc0
        jmp  eax
    }
}
static void InstallCompanionFitNow()       // thread del motore
{
    static const DWORD kAt[2] = { 0x006b6d4d, 0x006b740d };
    for (DWORD at : kAt) {
        BYTE op = 0; LONG rel = 0;
        if (!Rd(at, op) || op != 0xE8 || !Rd(at + 1, rel) || static_cast<DWORD>(at + 5 + rel) != 0x0043efc0) {
            Log("COMPAGNO: vista del compagno non attivata: chiamata inattesa a %08lX (%02X, verso %08lX)", at, op, static_cast<DWORD>(at + 5 + rel));
            return;
        }
    }
    for (DWORD at : kAt) {
        DWORD old = 0;
        void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(at + 1));
        if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return;
        *reinterpret_cast<LONG*>(p) = static_cast<LONG>(reinterpret_cast<uintptr_t>(&SlotFitStub)) - static_cast<LONG>(at + 5);
        VirtualProtect(p, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 4);
    }
    g_compFitOk = true;
    Log("COMPAGNO: vista del compagno pronta (0x6b6d4d e 0x6b740d -> SlotFitStub)");
    // 0.56.34: controllo di classe degli oggetti (metodo +0x38) per i compagni
    static const DWORD kItemVt[] = { 0x00890b0c, 0x00890b60, 0x00890c00, 0x00890c50, 0x00890ca0, 0x00890cf0, 0x00890d40, 0x00890de0 };
    int n = 0;
    for (DWORD vt : kItemVt) {
        DWORD cur = 0, old = 0;
        if (!Rd(vt + 0x38, cur) || cur != 0x005bfc80) { Log("COMPAGNO: classe degli oggetti: vtable %08lX +0x38 inatteso (%08lX)", vt, cur); continue; }
        void* slot = reinterpret_cast<void*>(static_cast<uintptr_t>(vt + 0x38));
        if (!VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(&ClassFitStubItem)));
        VirtualProtect(slot, 4, old, &old);
        ++n;
    }
    {
        DWORD cur = 0, old = 0;
        void* slot = reinterpret_cast<void*>(static_cast<uintptr_t>(0x00890a64 + 0x38));
        if (Rd(0x00890a64 + 0x38, cur) && cur == 0x005ba550 && VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(&ClassFitStubArmor)));
            VirtualProtect(slot, 4, old, &old);
            ++n;
        }
        else Log("COMPAGNO: classe degli oggetti: vtable di cArmor3D +0x38 inatteso (%08lX)", cur);
    }
    Log("COMPAGNO: controllo di classe degli oggetti per i compagni: %d vtable su 9", n);
    {   // 0.56.36: pallini dell'esperienza sotto i ritratti dei compagni
        constexpr DWORD at = 0x006d73e4;
        BYTE op = 0; LONG rel = 0; DWORD old = 0;
        void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(at + 1));
        if (!Rd(at, op) || op != 0xE8 || !Rd(at + 1, rel) || static_cast<DWORD>(at + 5 + rel) != kMercSlotDraw)
            Log("COMPAGNO: pallini dell'esperienza dei compagni non attivati: chiamata inattesa a %08lX (%02X)", at, op);
        else if (VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<LONG*>(p) = static_cast<LONG>(reinterpret_cast<uintptr_t>(&MercSlotStub)) - static_cast<LONG>(at + 5);
            VirtualProtect(p, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 4);
            Log("COMPAGNO: pallini dell'esperienza sotto i ritratti dei compagni (0x6d73e4 -> MercSlotStub)");
        }
    }
}
struct CompScope {                       // letture dell'eroe delle finestre deviate sul compagno; il giocatore resta l'eroe 1
    DWORD oldHero, oldOn, oldCur; bool lim; CompClassScope cls;
    CompScope(DWORD addr, DWORD id, bool limit) : oldHero(g_uiHero), oldOn(g_uiOn), oldCur(g_compCurId), lim(limit), cls(addr) {
        ++g_dwNest; g_uiHero = addr; g_uiOn = 0; g_compCurId = id;
        if (lim) g_compLimit = 1;
    }
    ~CompScope() { if (lim) g_compLimit = 0; g_compCurId = oldCur; g_uiHero = oldHero; g_uiOn = oldOn; --g_dwNest; }
};
struct CompLayout {                      // (1): la disposizione dell'eroe resta durante le chiamate per il compagno
    DWORD win = 0, old = 0, type = 0; bool on = false;
    CompLayout(int k, DWORD w, DWORD addr) : win(w) {
        WORD built = 0;
        if (k == 1 && Rd(w + 0x158, built) && built && Rd(w + 0x154, old) && Rd(addr + 0x10, type)) on = WriteDword(w + 0x154, type);
    }
    bool Ready(int k) const { return k != 1 || on; }
    ~CompLayout() { DWORD cur = 0; if (on && Rd(win + 0x154, cur) && cur == type) WriteDword(win + 0x154, old); }
};
// (c) le due mani
static void PutBytes(DWORD a, const void* p, SIZE_T n)
{
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(static_cast<uintptr_t>(a)), p, n, nullptr);
}
struct HandEls { DWORD win = 0; DWORD el[2] = {}; bool failed = false; };
HandEls g_hands;
BYTE g_handRec[3][2][0x14] = {};         // per colonna: schede dell'immagine delle due mani (+0x12: tipo della casella)
static DWORD NewHandEl(DWORD win, const char* name, int x, int y, short w, short h, DWORD tex, const DWORD* uv)
{
    DWORD m = 0;
    __try { m = reinterpret_cast<GameNewFn>(0x00848782)(0x7c); }
    __except (EXCEPTION_EXECUTE_HANDLER) { m = 0; }
    if (!m) return 0;
    const DWORD rx = static_cast<DWORD>(x), ry = static_cast<DWORD>(y);
    const DWORD rwh = static_cast<DWORD>(static_cast<WORD>(w)) | (static_cast<DWORD>(static_cast<WORD>(h)) << 16);
    const DWORD uvp = static_cast<DWORD>(reinterpret_cast<uintptr_t>(uv)), np = static_cast<DWORD>(reinterpret_cast<uintptr_t>(name));
    DWORD before = 0, after = 0, rv = 0;
    bool ok = false;
    __try {
        __asm {
            mov  before, esp
            push 0x4005
            push uvp
            push tex
            push win
            sub  esp, 12
            mov  eax, rx
            mov  [esp], eax
            mov  eax, ry
            mov  [esp + 4], eax
            mov  eax, rwh
            mov  [esp + 8], eax
            push np
            mov  ecx, m
            mov  eax, 0x007533a0
            call eax
            mov  after, esp
            mov  esp, before
            mov  rv, eax
        }
        ok = true;
    } __except (CaptureException(GetExceptionInformation())) { ok = false; }
    DWORD vt = 0, par = 0;
    if (!ok || after != before || rv != m || !Rd(m, vt) || vt != 0x0089669c || !Rd(m + 0x50, par) || par != win) {
        if (!ok) LogException("casella della mano del compagno");
        Log("COMPAGNO: casella %s NON creata (esito %d, pila %s, vtable %08lX, genitore %08lX)", name, ok ? 1 : 0, after == before ? "giusta" : "SBILANCIATA", vt, par);
        return 0;
    }
    return m;
}
static bool HandsReady(DWORD win)
{
    if (g_hands.win == win && g_hands.el[0] && g_hands.el[1]) return true;
    if (g_hands.win == win && g_hands.failed) return false;
    g_hands = HandEls{};
    g_hands.win = win;
    g_hands.failed = true;                          // fino alla fine: un tentativo per finestra
    short wwh[2] = {};
    if (!RdBuf(win + 0x2c, wwh, sizeof(wwh)) || wwh[0] < 160 || wwh[1] < 100) { Log("COMPAGNO: caselle delle mani non create: misure della finestra %dx%d", wwh[0], wwh[1]); return false; }
    constexpr int kW = 64, kH = 128;
    const int x0 = 16, x1 = 16 + kW + 16, y = wwh[1] + 8;
    DWORD vt = 0, tex = 0;
    float uv[4] = {};                               // {u0, v0, u1, v1}, come le legge il costruttore
    bool bgOk = false;
    {   // trama: un pezzo dello sfondo della finestra (l'angolo in alto a sinistra, 64x64 da (16,6)), allungato
        DWORD bg = 0; float f[4] = {}; int bx = 0, by = 0; short bwh[2] = {};
        if (Rd(win + 0x1dc, bg) && bg && Rd(bg, vt) && vt == 0x0089669c && Rd(bg + 0x78, tex) && tex && RdBuf(bg + 0x54, f, sizeof(f)) &&
            Rd(bg + 0x24, bx) && Rd(bg + 0x28, by) && RdBuf(bg + 0x2c, bwh, sizeof(bwh)) && bwh[0] > 64 && bwh[1] > 64) {
            const float u0 = f[0], u1 = f[1], v0 = f[2], v1 = f[3];    // +0x54 u0, +0x58 u1, +0x5c v0, +0x60 v1
            const float rx0 = (16.0f - bx) / bwh[0], rx1 = (80.0f - bx) / bwh[0], ry0 = (6.0f - by) / bwh[1], ry1 = (70.0f - by) / bwh[1];
            const bool sane = u0 == u0 && u1 == u1 && v0 == v0 && v1 == v1 && rx0 >= 0.0f && rx1 <= 1.0f && ry0 >= 0.0f && ry1 <= 1.0f &&
                              u0 > -2.0f && u0 < 2.0f && u1 > -2.0f && u1 < 2.0f && v0 > -2.0f && v0 < 2.0f && v1 > -2.0f && v1 < 2.0f;
            if (sane) {
                uv[0] = u0 + (u1 - u0) * rx0; uv[2] = u0 + (u1 - u0) * rx1;
                uv[1] = v0 + (v1 - v0) * ry0; uv[3] = v0 + (v1 - v0) * ry1;
                bgOk = true;
            }
        }
    }
    if (!bgOk) {   // altrimenti la trama di una casella (guanti, braccia, elmo), con le coordinate nell'ordine giusto
        DWORD tmpl = 0; float f[4] = {};
        for (int s : { 4, 5, 0 }) { DWORD e = 0; if (Rd(win + 0x15c + s * 4, e) && e && Rd(e, vt) && vt == 0x0089669c) { tmpl = e; break; } }
        if (!tmpl || !Rd(tmpl + 0x78, tex) || !RdBuf(tmpl + 0x54, f, sizeof(f))) { Log("COMPAGNO: caselle delle mani non create: nessuna trama"); return false; }
        uv[0] = f[0]; uv[1] = f[2]; uv[2] = f[1]; uv[3] = f[3];
    }
    DWORD uvd[4];
    memcpy(uvd, uv, sizeof(uvd));
    static const char kN0[] = "UI_EQ_HAND_1", kN1[] = "UI_EQ_HAND_2";
    const short w = kW, h = kH;
    g_hands.el[0] = NewHandEl(win, kN0, x0, y, w, h, tex, uvd);
    g_hands.el[1] = g_hands.el[0] ? NewHandEl(win, kN1, x1, y, w, h, tex, uvd) : 0;
    if (!g_hands.el[0] || !g_hands.el[1]) return false;
    for (int s = 1; s <= 2; ++s) { g_handRec[s][0][0x12] = 5; g_handRec[s][1][0x12] = 0xd; }   // arma, scudo
    g_hands.failed = false;
    Log("COMPAGNO: caselle delle mani create per la finestra %08lX: (%d,%d) e (%d,%d), %dx%d, trama %s", win, x0, y, x1, y, w, h, bgOk ? "dello sfondo della finestra" : "di una casella");
    return true;
}
struct CompHands {                       // le due mani nella serie dei riquadri, solo durante le chiamate per il compagno
    DWORD win = 0; int side = 0; bool on = false; DWORD oldEl[2] = {}; BYTE oldRec[2][0x14] = {};
    CompHands(int k, DWORD w, int sd) : win(w), side(sd) {
        if (k != 1 || !HandsReady(w)) return;
        if (!Rd(w + 0x15c + 0xc * 4, oldEl[0]) || !Rd(w + 0x15c + 0xd * 4, oldEl[1]) || oldEl[0] || oldEl[1]) return;   // occupate: non si tocca
        if (!RdBuf(w + 0x314 + 0xc * 0x14, oldRec, sizeof(oldRec))) return;
        PutBytes(w + 0x314 + 0xc * 0x14, g_handRec[side], sizeof(oldRec));
        WriteDword(w + 0x15c + 0xc * 4, g_hands.el[0]);
        WriteDword(w + 0x15c + 0xd * 4, g_hands.el[1]);
        on = true;
    }
    ~CompHands() {
        if (!on) return;
        RdBuf(win + 0x314 + 0xc * 0x14, g_handRec[side], sizeof(oldRec));
        WriteDword(win + 0x15c + 0xc * 4, oldEl[0]);
        WriteDword(win + 0x15c + 0xd * 4, oldEl[1]);
        PutBytes(win + 0x314 + 0xc * 0x14, oldRec, sizeof(oldRec));
    }
};
static void CompFault(int side, int k, const char* what)
{
    g_compBad[side] = true;
    LogException(what);
    Log("COMPAGNO: ATTENZIONE: errore nel %s di %s per il compagno %lu della colonna %d: vista spenta", what, kDualWin[k].name, g_compSide[side].load(), side);
    OverlayCoopToast(Tr("Vista del compagno: errore, spenta"), 3000);
    g_compSide[side].store(0);
    CompResetChoose(side);
}
static bool InWin(DWORD w, int x, int y)
{
    int wx = 0, wy = 0; short wh[2] = {};
    if (!RdBuf(w + 0x2c, wh, sizeof(wh))) return false;
    AbsPos(w, wx, wy);
    return x >= wx && x < wx + wh[0] && y >= wy && y < wy + wh[1];
}
static DWORD HeldOf(int side)            // oggetto in mano al giocatore della colonna (secondo mouse per il giocatore 2)
{
    DWORD m = 0, p = 0;
    if (side == 2 && g_m2) m = g_m2; else Rd(0x00cdcadc, m);
    return m && Rd(m + 0x68, p) ? p : 0;
}
// 0.56.33: "ho premuto sull'arco ed e' sparito": mani del compagno (slot +0x1a4 grezzi, anche gli oggetti di base) e oggetto in
// mano al giocatore, con il modello del tipo ([0xaaa564] + tipo*0x80 + 0x30), prima e dopo ogni clic sulle mani
static DWORD ItemAddr(DWORD id)            // 0.56.34: un oggetto qualunque dal suo numero (come 0x5fe550; ObjectAddr vale per le creature)
{
    DWORD om = 0, beg = 0, end = 0, p = 0, pid = 0;
    if (!id || !Rd(kObjMgr, om) || !om || !Rd(om + 4, beg) || !Rd(om + 8, end) || end < beg || id >= (end - beg) / 4 || !Rd(beg + id * 4, p) || !p) return 0;
    return Rd(p + 0xc, pid) && pid == id ? p : 0;
}
static bool ItemBound(DWORD id)            // 0.56.34: oggetto di serie del compagno (+0xb0 bit 8): 0x556cf0 lo stacca senza darlo, e si perde
{
    const DWORD o = ItemAddr(id);
    BYTE f = 0;
    return o && Rd(o + 0xb0, f) && (f & 8);
}
// oggetto di serie in una casella del compagno: togliere no; sostituire (il gioco lo perde) solo con un secondo clic entro 4 s
static bool CompBoundOk(bool give, DWORD id, const char* where)
{
    static DWORD confirmAt = 0;
    if (!give) {
        OverlayCoopToast(Tr("E' un oggetto di serie del compagno: non si toglie, si puo' solo sostituire"), 3000);
        Log("COMPAGNO: %s del compagno %lu: oggetto di serie, non si toglie", where, id);
        return false;
    }
    if (GetTickCount() - confirmAt > 4000) {
        confirmAt = GetTickCount();
        OverlayCoopToast(Tr("Clicca di nuovo per sostituire l'oggetto di serie: andra' perso"), 4000);
        Log("COMPAGNO: %s del compagno %lu: sostituzione di un oggetto di serie, si aspetta la conferma", where, id);
        return false;
    }
    confirmAt = 0;
    Log("COMPAGNO: %s del compagno %lu: oggetto di serie sostituito (confermato)", where, id);
    return true;
}
static void HandsLog(const char* when, int side, DWORD c)
{
    auto desc = [&](DWORD id, char* out, size_t n) {
        DWORD o = ItemAddr(id), type = 0, tab = 0;
        char m[0x21] = {};
        if (o) Rd(o + 0x10, type);
        if (type && Rd(kQuestTable, tab) && tab) RdBuf(tab + type * 0x80 + 0x30, m, 0x20);
        for (char& ch : m) if (ch && (ch < 32 || ch > 126)) ch = '?';
        _snprintf_s(out, n, _TRUNCATE, "%lu (tipo %lu %s%s)", id, type, m[0] ? m : "-", ItemBound(id) ? ", di serie" : "");
    };
    DWORD a = 0, b = 0, hp = 0;
    Rd(c + 0x1a4 + 0xc * 4, a); Rd(c + 0x1a4 + 0xd * 4, b);
    const DWORD held = HeldOf(side);
    DWORD hm = 0; if (held) Rd(held + 4, hm);
    char da[96], db[96], dh[96];
    desc(a, da, sizeof(da)); desc(b, db, sizeof(db)); desc(hm, dh, sizeof(dh));
    Log("COMPAGNO: mani %s: mano 1 %s, mano 2 %s; in mano al giocatore %d %s", when, da, db, side, held ? dh : "niente");
    (void)hp;
}
// disegno di equipaggiamento (k 1) o statistiche (k 2) per il compagno della colonna; false = nessun compagno, disegna il chiamante
static bool CompDraw(int side, int k, DWORD self, DWORD ctx, DWORD a = 0, DWORD b = 0)
{
    const DWORD addr = CompAddrSide(side);
    if (!addr || (k != 1 && k != 2)) return false;
    // 0.56.35 (foto delle 16:36, "ci sono questi due riquadri a destra cosa sono?"): il gestore chiama il disegno anche con la finestra
    // nascosta (che poi esce subito): la mod scriveva lo stesso i rettangoli delle mani (cornici a zaino chiuso) e g_compSeenAt (la
    // vista non si chiudeva con lo zaino)
    DWORD wf = 0;
    if (!Rd(self + 0x10, wf) || !(wf & 1)) return false;
    if (k == 1) g_compSeenAt[side] = GetTickCount() | 1;
    CompLayout lay(k, self, addr);
    if (!lay.Ready(k)) return false;            // disposizione non ancora costruita: questo fotogramma e' dell'eroe
    bool ok;
    {
        CompHands hands(k, self, side);
        CompScope cs(addr, g_compSide[side].load(), false);
        ok = DwCallDraw(k, self, ctx, a, b);
        if (hands.on)                               // 0.56.33: dove sono adesso (con gli spostamenti della colonna), per la cornice
            for (int i = 0; i < 2; ++i) {
                int ex = 0, ey = 0; short wh[2] = {};
                AbsPos(g_hands.el[i], ex, ey); RdBuf(g_hands.el[i] + 0x2c, wh, sizeof(wh));
                DwStoreRect(g_handShown[side][i], RECT{ ex, ey, ex + wh[0], ey + wh[1] }, true);
            }
    }
    if (!ok) CompFault(side, k, "disegno");
    return true;
}
static void CompToastDrag()
{
    static DWORD at = 0;
    if (GetTickCount() - at < 2000) return;
    at = GetTickCount();
    OverlayCoopToast(Tr("Con la vista del compagno trascina con il clic sinistro"), 2500);
}
// eventi di zaino (k 0, solo con una colonna), equipaggiamento (k 1) e statistiche (k 2) per il compagno della colonna; fn fa la
// chiamata vera (gia' nel contesto della colonna: spostamenti, copia). true = gestito, r = risultato
template <class F> static bool CompInput(int side, int k, DWORD win, DWORD ev, char& r, F fn)
{
    const DWORD addr = CompAddrSide(side);
    if (!addr) return false;
    const DWORD id = g_compSide[side].load();
    const int type = DwEvType(ev);
    int b4 = 0, x = 0, y = 0;
    Rd(ev + 4, b4);
    const bool pos = (type == 2 || type == 3) && Rd(ev + 8, x) && Rd(ev + 12, y);
    if (k == 0) {                                 // (3) clic destro sugli oggetti dello zaino: sarebbe per l'eroe
        if (type != 2 || !pos || b4 != 2 || !InWin(win, x, y)) return false;
        CompToastDrag();
        Log("COMPAGNO: clic destro nello zaino in (%d,%d) con la vista del compagno: non passa", x, y);
        r = 1;
        return true;
    }
    if (k != 1 && k != 2) return false;
    if (k == 1 && type == 5 && b4 == 0x2c) {      // (3) "equipaggia questo" dallo zaino
        CompToastDrag();
        Log("COMPAGNO: oggetto mandato dallo zaino all'equipaggiamento del compagno: non passa (si trascina)");
        r = 0;
        return true;
    }
    if (type != 2 || !pos) return false;          // il resto (rilasci, messaggi del gioco) alla finestra come sempre
    if (k == 2 && !InWin(win, x, y)) return false;   // 0.56.33: fuori dalle statistiche non serve il compagno
    if (k == 1 && b4 == 2 && g_hands.win == win && g_hands.el[0] && (InWin(g_hands.el[0], x, y) || InWin(g_hands.el[1], x, y))) { r = 1; return true; }
    if (k == 1 && b4 == 2) {                      // (3) clic destro sulle sue caselle
        const bool in = InWin(win, x, y);
        if (in) CompToastDrag();
        r = in ? 1 : 0;
        return true;
    }
    if (k == 1 && b4 == 0 && g_hands.win == win && g_hands.el[0]) {   // (c) le mani: arma o scudo in mano = dare, a mani vuote = togliere
        int hit = -1;
        for (int i = 0; i < 2; ++i) if (InWin(g_hands.el[i], x, y)) hit = i;
        if (hit >= 0) {
            const bool give = HeldOf(side) != 0;
            const DWORD merc = MercWin();
            const int pk = MercSlotOf(merc, id);
            HandsLog("prima", side, addr);
            // 0.56.34: prova della 0.56.33 (16:20:51): a mani vuote 0x6d6840 toglie la seconda mano e, se e' vuota (o di serie), la
            // prima; l'arco di serie 694 (+0xb0 bit 8) e' stato staccato e perso ("appena ci clicco sparisce nel nulla"), come
            // farebbe il ritratto del gioco. Le armi di serie non si tolgono; per sostituirle (il gioco le perde) serve un secondo clic.
            DWORD h1 = 0, h2 = 0;
            Rd(addr + 0x1a4 + 0xc * 4, h1); Rd(addr + 0x1a4 + 0xd * 4, h2);
            const bool bound = (h1 && ItemBound(h1)) || (h2 && ItemBound(h2));
            // 0.56.35: prova delle 16:35:30-16:35:42: dato una torcia (21) al posto dell'arco di serie 694, la torcia va nella seconda
            // mano e l'arco esce; ripresa la torcia, il gioco rimette lo stesso 694 nella prima mano. Sostituire un'arma di serie quindi
            // non la perde (niente conferma); toglierla a mani vuote si' (0x556cf0 la stacca e basta).
            if (bound && !give && !CompBoundOk(false, id, "mano")) { r = 1; return true; }
            CompClassScope cls(addr);
            const CallResult c = give && pk > 0 ? CallThis1(0x006d6c70, merc, static_cast<DWORD>(pk)) : CallResult{};
            if (pk <= 0) Log("COMPAGNO: mano %d: il compagno %lu non e' fra i ritratti", hit + 1, id);
            else if (give && (!c.ok || !c.balanced || !(c.ret & 0xff))) {
                OverlayCoopToast(Tr("Il compagno non sa usare quest'arma"), 2500);
                Log("COMPAGNO: mano %d del compagno %lu: oggetto che non sa usare, rifiutato", hit + 1, id);
            } else {
                const CallResult g = CallThis1(0x006d6840, merc, static_cast<DWORD>(pk));
                if (!g.ok) { LogException("mani del compagno"); CompFault(side, k, "clic sulle mani"); }
                else {
                    Log("COMPAGNO: mano %d del compagno %lu: %s con la regola del ritratto (0x6d6840, ritratto %d)", hit + 1, id, give ? "oggetto dato" : "oggetto ripreso", pk);
                    HandsLog("dopo", side, addr);
                }
            }
            r = 1;
            return true;
        }
    }
    CompLayout lay(k, win, addr);
    if (!lay.Ready(k)) { r = 1; return true; }    // finestra non ancora pronta: il clic non passa
    if (k == 1 && b4 == 0) {                      // 0.56.34: anche le altre caselle: un oggetto di serie non si toglie
        for (int i = 0; i < 0x13; ++i) {
            DWORD e = 0, it = 0;
            if (i == 0xc || i == 0xd || !Rd(win + 0x15c + i * 4, e) || !e || !InWin(e, x, y)) continue;
            if (Rd(addr + 0x1a4 + i * 4, it) && it && ItemBound(it) && !CompBoundOk(HeldOf(side) != 0, id, "casella")) { r = 1; return true; }
            break;
        }
    }
    const LONG before = g_compRefused;
    int res = 0;
    {
        CompHands hands(k, win, side);
        CompScope cs(addr, id, k == 1);
        res = fn();
    }
    if (res < 0) { CompFault(side, k, "clic"); r = 1; return true; }
    if (g_compRefused != before) {
        OverlayCoopToast(Tr("Il compagno non sa usare quest'arma"), 2500);
        Log("COMPAGNO: clic in (%d,%d): arma che il compagno non sa usare, rifiutata", x, y);
    }
    else Log("COMPAGNO: clic in (%d,%d) su %s del compagno %lu (colonna %d)", x, y, kDualWin[k].name, id, side);
    r = static_cast<char>(res);
    return true;
}
static bool RightClick(DWORD ev)
{
    int b = 0;
    return DwEvType(ev) == 2 && Rd(ev + 4, b) && b == 2;
}
// clic destro a mani vuote sul ritratto di un compagno (finestra n.7): apre o chiude la vista della colonna di chi clicca
static bool CompanionClick(DWORD self, DWORD ev)
{
    if (!g_cfg.nativeCompanionView || !g_compFitOk || !g_uiPatched.load() || DwEvType(ev) != 2) return false;
    int btn = 0, x = 0, y = 0;
    if (!Rd(ev + 4, btn) || btn != 2 || !Rd(ev + 8, x) || !Rd(ev + 12, y)) return false;
    const int side = (g_m2 && Mouse2Cur() == g_m2) ? 2 : 1;   // 0.56.31: anche il secondo mouse
    if (HeldOf(side)) return false;
    // 0.56.29: prova della 0.56.28 (04/11 14:51, foto e registro): il clic destro e' arrivato in (1857,346), sul terzo ritratto,
    // quello dell'eroe 2 disegnato dalla mod (sotto la compagna 693, nel primo ritratto libero), che fuori dal disegno ha id 0:
    // niente vista e nessun avviso. Ora un avviso, e nel registro ogni clic destro sui ritratti con id e rettangoli.
    bool onPortrait = false;                    // 0.56.33: nel registro solo i clic sui ritratti (arrivano qui tutti)
    for (int k = 1; k <= 4; ++k) if (InWin(self + 0x2ec + (k - 1) * 0x194, x, y)) onPortrait = true;
    if (!onPortrait) return false;
    {
        char line[320]; int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "COMPAGNO: clic destro del giocatore %d sui ritratti in (%d,%d):", side, x, y);
        for (int k = 1; k <= 4 && n > 0 && n < static_cast<int>(sizeof(line)) - 64; ++k) {
            DWORD id = 0; int ex = 0, ey = 0; short wh[2] = {};
            const DWORD el = self + 0x2ec + (k - 1) * 0x194;
            Rd(self + 0x154 + k * 0x194, id); RdBuf(el + 0x2c, wh, sizeof(wh)); AbsPos(el, ex, ey);
            const bool h2 = !id && k == g_portraitSlot.load() && static_cast<LONG>(GetTickCount() - g_portraitAt.load()) < 1000;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %d=%s%lu (%d,%d %dx%d)", k, h2 ? "eroe 2 " : "", h2 ? 2ul : id, ex, ey, wh[0], wh[1]);
        }
        Log("%s", line);
    }
    for (int k = 1; k <= 4; ++k) {
        DWORD id = 0;
        const DWORD el = self + 0x2ec + (k - 1) * 0x194;
        if (!Rd(self + 0x154 + k * 0x194, id) || !InWin(el, x, y)) continue;
        if (!id) {
            if (k == g_portraitSlot.load() && static_cast<LONG>(GetTickCount() - g_portraitAt.load()) < 1000) {
                OverlayCoopToast(Tr("Questo e' l'eroe 2: per il compagno clic destro sul suo ritratto"), 3000);
                return true;
            }
            continue;
        }
        if (g_compSide[side].load() == id) { CompanionOff(side, "clic destro sul suo ritratto"); return true; }
        if (!CompanionAddr(id)) return false;
        if (!CompSideOpen(side)) {
            if (side == 2 || MemoryDualActive()) { OverlayCoopToast(Tr("Apri prima il tuo zaino"), 2500); return true; }
            if (g_gameHwnd.load()) {                // una colonna sola, zaino chiuso: si apre (tasto I)
                g_virt.keyUntil['I'].store(GetTickCount() + 80);
                const LPARAM lp = 1 | (0x17 << 16);
                PostMessageA(g_gameHwnd.load(), WM_KEYDOWN, 'I', lp);
                PostMessageA(g_gameHwnd.load(), WM_KEYUP, 'I', lp | (1 << 30) | (1u << 31));
            }
        }
        CompanionOn(side, id, "clic destro sul ritratto");
        return true;
    }
    return false;
}
// (e) esperienza ai compagni dei ritratti (thread del motore)
static void AddExperienceCompanions(DWORD add)
{
    const DWORD merc = MercWin();
    DWORD om = 0, pl = 0, loc = 0;
    if (!merc || !add || !Rd(kObjMgr, om) || !om || !Rd(kPlayer, pl) || !pl || !Rd(pl + 0x14, loc)) return;
    const DWORD h1 = ObjectAddr(om, loc);
    int ax = 0, ay = 0; BYTE az = 0;
    if (!h1 || !Rd(h1 + 0x1c, ax) || !Rd(h1 + 0x20, ay) || !Rd(h1 + 0x24, az)) return;
    for (int k = 1; k <= 4; ++k) {
        DWORD id = 0, back = 0;
        if (!Rd(merc + 0x154 + k * 0x194, id) || !id) continue;
        const DWORD c = CompanionAddr(id);
        if (!c || !Rd(c + 0x3ac, back) || back != c) continue;
        if (g_cfg.nativeXpRange > 0) {
            int bx = 0, by = 0; BYTE bz = 0;
            Rd(c + 0x1c, bx); Rd(c + 0x20, by); Rd(c + 0x24, bz);
            const double dx = ax - bx, dy = ay - by;
            if (az != bz || dx * dx + dy * dy > static_cast<double>(g_cfg.nativeXpRange) * g_cfg.nativeXpRange) continue;
        }
        DWORD x0 = 0, x1 = 0; WORD l0 = 0, l1 = 0;
        Rd(c + 0x3b4, x0); Rd(c + 0x3fe, l0);
        const CallResult cr = CallThis1(0x0057e410, c + 0x3a8, add);
        if (!cr.ok) { LogException("addExperience del compagno"); continue; }
        if (!cr.balanced) Log("COMPAGNO: ATTENZIONE: 0x57e410 ha lasciato lo stack sbilanciato (corretto)");
        Rd(c + 0x3b4, x1); Rd(c + 0x3fe, l1);
        static DWORD logAt = 0;
        if (l1 != l0 || GetTickCount() - logAt > 10000) { logAt = GetTickCount(); Log("COMPAGNO: esperienza al compagno %lu: +%lu, %lu -> %lu (livello %u)", id, add, x0, x1, l1); }
        if (l1 > l0) {
            WORD sp = 0, sl = 0; BYTE ap = 0; Rd(c + 0x3ea, sp); Rd(c + 0x3ec, sl); Rd(c + 0x3cb, ap);
            Log("COMPAGNO: il compagno %lu sale di livello: %u -> %u (punti attributo %u, punti abilita' %u, abilita' nuove %u)", id, l0, l1, ap, sp, sl);
            char t[128];
            _snprintf_s(t, sizeof(t), _TRUNCATE, Tr("Il compagno sale al livello %u: RT nello zaino per i suoi punti"), static_cast<unsigned>(l1));
            OverlayCoopToast(t, 5000);
            const CallResult cs = CallLevelUpSound();
            if (!cs.ok) LogException("suono della salita di livello del compagno");
        }
    }
}

// --- 0.56.37: registro dei compagni (fase 1, solo osservazione) ---------------------------------------------------------
// il tester, 04/11: un modo per reclutare di nuovo i compagni avuti nella storia ("un tab compagni ... con il loro equipaggiamento");
// "Laurelinad dovrebbe tornare alla fine della campagna", "molte volte i compagni nella storia possono morire definitivamente o
// andarsene per poi ritornare". Studio del 04/11: il gruppo e' l'elenco dell'eroe a +0x39c..+0x3a0 (voci da 9 byte: id, un
// dword, un segno a +8), salvato con l'eroe (cCreature vf20 0x54c070); il reclutamento e' 0x54d370 (sull'eroe: livello del
// compagno = quello della zona o dell'eroe, il piu' alto, con 0x565010; +0x251 = id dell'eroe, bit di +0x1f4; voce aggiunta con
// 0x54b600), il congedo 0x54d780 (toglie la voce, azzera +0x251 e i bit, e se il compagno e' a piu' di 600 dalla sua casa
// +0x2bd/+0x2c1 la casa diventa il punto in cui si trova: non lo cancella). Al caricamento (0x60aec0, 0x60be09..) le voci con il
// segno richiamano il compagno (0x54d370), quelle senza lo cancellano (0x5fc300). Prima di costruire il richiamo serve sapere se
// un compagno congedato resta davvero nel mondo, con lo stesso numero, dopo un cambio di zona e dopo un caricamento, o se lo
// cancella uno script: qui la mod legge soltanto (nessuna scrittura nel gioco). Ogni entrata e uscita dal gruppo va nel registro
// e in save\SacredPad_compagni.txt (una riga per fatto, con la classe dell'eroe); poi, per ogni compagno uscito, la
// mod guarda se c'e' ancora (vivo, morto, cancellato, con un altro numero dello stesso tipo, distanza dall'eroe): ogni 10 s nei
// primi 2 minuti, poi ogni minuto, e a ogni partita caricata anche per quelli scritti nel file in partite precedenti dello
// stesso eroe. Nel registro solo i cambiamenti.
static void CompRegPath(char* path, size_t n)   // save\SacredPad_compagni.txt nella cartella del gioco (quella della DLL)
{
    char d[MAX_PATH] = {};
    GetModuleFileNameA(SelfModule(), d, MAX_PATH);
    if (char* sl = strrchr(d, '\\')) *sl = 0;
    _snprintf_s(path, n, _TRUNCATE, "%s\\save\\SacredPad_compagni.txt", d);
}
struct CompRec {
    DWORD id = 0, type = 0, name = 0, hero = 0; WORD level = 0; BYTE flag = 0;
    bool inGroup = false; DWORD leftAt = 0, checkAt = 0; int last = -1;   // last: ultimo stato scritto
};
static std::vector<CompRec> g_compReg;   // thread della mod
static std::set<DWORD> g_copyTypesFile;  // 0.56.40: tipi con una copia attiva secondo il file (letti all'avvio)
static std::mutex g_cpMx;                // 0.56.40: dati della scheda Compagni (vedi piu' sotto)
static std::set<DWORD> g_copyTypes;      // 0.56.40: tipi con una copia attiva (thread della mod e del motore, sotto g_cpMx)
static void CompRegLine(const char* ev, const CompRec& r, const char* extra)
{
    SYSTEMTIME t; GetLocalTime(&t);
    char line[400];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u;%s;eroe %08lX;id %lu;tipo %lu;nome %08lX;livello %u;segno %u;%s\r\n",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, ev, r.hero, r.id, r.type, r.name, r.level, r.flag, extra ? extra : "");
    Log("COMPAGNI: %s: id %lu, tipo %lu, nome %08lX, livello %u, segno %u%s%s", ev, r.id, r.type, r.name, r.level, r.flag, extra && *extra ? ", " : "", extra ? extra : "");
    char path[MAX_PATH];
    CompRegPath(path, sizeof(path));
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, line, static_cast<DWORD>(strlen(line)), &w, nullptr);
    CloseHandle(f);
}
static void CompRegLoad()                  // compagni scritti nelle partite precedenti (per i controlli dopo un caricamento)
{
    char path[MAX_PATH];
    CompRegPath(path, sizeof(path));
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        CompRec r;
        unsigned long hero = 0, id = 0, type = 0, name = 0; unsigned lv = 0, fl = 0;
        const char* p = strstr(line, ";eroe ");
        if (!p || sscanf_s(p, ";eroe %lx;id %lu;tipo %lu;nome %lx;livello %u;segno %u", &hero, &id, &type, &name, &lv, &fl) != 6 || !id) continue;
        if (strstr(line, ";copia creata;")) g_copyTypesFile.insert(type);          // 0.56.40: copie ancora attive
        else if (strstr(line, ";copia cancellata;")) g_copyTypesFile.erase(type);
        if (strstr(line, ";copia ")) continue;
        bool found = false;
        for (CompRec& q : g_compReg) if (q.id == id && q.hero == hero) { q.type = type; q.name = name; q.level = static_cast<WORD>(lv); found = true; }
        if (found) continue;
        r.hero = hero; r.id = id; r.type = type; r.name = name; r.level = static_cast<WORD>(lv); r.flag = static_cast<BYTE>(fl);
        g_compReg.push_back(r);
        ++n;
    }
    fclose(f);
    if (n) Log("COMPAGNI: %d compagni dalle partite precedenti (save\\SacredPad_compagni.txt)", n);
}
static DWORD CompHeroKey(DWORD h)          // eroe: la sua classe (il testo del nome e' registrato a ogni avvio, non serve fra le partite)
{
    DWORD cls = 0;
    Rd(h + 0x10, cls);
    return cls;
}
// stato di un compagno uscito: 0 c'e' (vivo), 1 c'e' ma morto, 2 numero libero o di un'altra creatura (stesso tipo con un altro
// numero in *other), 3 non c'e' piu' nessuno del suo tipo
static int CompRegState(DWORD mgr, const CompRec& r, DWORD h, DWORD* other, double* dist)
{
    *other = 0; *dist = -1;
    DWORD c = ObjectAddr(mgr, r.id), type = 0;
    if (c && Rd(c + 0x10, type) && type == r.type) {
        int x = 0, y = 0, hx = 0, hy = 0;
        Rd(c + 0x1c, x); Rd(c + 0x20, y); Rd(h + 0x1c, hx); Rd(h + 0x20, hy);
        *dist = std::sqrt(static_cast<double>(x - hx) * (x - hx) + static_cast<double>(y - hy) * (y - hy));
        int cls = 0; bool dead = false; DWORD hp = 0;
        ReadKind(c, cls, dead); Rd(c + 0x4d8, hp);
        return (dead || !hp) ? 1 : 0;
    }
    DWORD beg = 0, end = 0;
    if (Rd(mgr + 4, beg) && Rd(mgr + 8, end) && end > beg && (end - beg) / 4 < 200000) {
        const DWORD n = (end - beg) / 4;
        for (DWORD i = 1; i < n && !*other; ++i) {
            const DWORD o = ObjectAddr(mgr, i);
            DWORD t = 0, on = 0;   // 0.56.41: con il nome del registro, se c'e' (le altre creature del tipo sono generiche)
            if (o && Rd(o + 0x10, t) && t == r.type && (!r.name || (Rd(o + 0x3c, on) && on == r.name))) *other = i;
        }
    }
    return *other ? 2 : 3;
}
static void CompanionRegisterTick(DWORD mgr, DWORD heroId, bool inWorld, DWORD now)
{
    static DWORD at = 0, heroWas = 0;
    static bool loaded = false;
    if (!inWorld || !mgr || !heroId || static_cast<LONG>(now - at) < 1000) return;
    at = now;
    const DWORD h = ObjectAddr(mgr, heroId);
    if (!h) return;
    if (!loaded) {
        loaded = true; CompRegLoad();
        if (!g_copyTypesFile.empty()) { std::lock_guard<std::mutex> lk(g_cpMx); for (DWORD t : g_copyTypesFile) g_copyTypes.insert(t); }
    }
    const DWORD key = CompHeroKey(h);
    DWORD beg = 0, end = 0;
    if (!Rd(h + 0x39c, beg) || !Rd(h + 0x3a0, end) || end < beg || (end - beg) % 9 || (end - beg) / 9 > 16) return;
    const int n = static_cast<int>((end - beg) / 9);
    DWORD ids[16] = {}; BYTE flags[16] = {};
    for (int i = 0; i < n; ++i) { Rd(beg + i * 9, ids[i]); Rd(beg + i * 9 + 8, flags[i]); }
    if (h != heroWas) {                            // partita caricata (o primo giro): elenco salvato con l'eroe e controlli
        heroWas = h;
        char e[160]; int m = 0; e[0] = 0;
        for (int i = 0; i < n; ++i) m += _snprintf_s(e + m, sizeof(e) - m, _TRUNCATE, "%s%lu (segno %u)", i ? ", " : "", ids[i], flags[i]);
        Log("COMPAGNI: eroe %08lX in gioco, gruppo: %s", key, n ? e : "nessuno");
        for (CompRec& r : g_compReg) if (r.hero == key && !r.inGroup) { r.checkAt = now; r.last = -1; if (!r.leftAt) r.leftAt = now; }
    }
    for (int i = 0; i < n; ++i) {                  // entrate
        if (!ids[i]) continue;
        CompRec* r = nullptr;
        for (CompRec& q : g_compReg) if (q.id == ids[i] && q.hero == key) r = &q;
        const DWORD c = ObjectAddr(mgr, ids[i]);
        DWORD type = 0, name = 0; WORD lv = 0;
        if (c) { Rd(c + 0x10, type); Rd(c + 0x3c, name); Rd(c + 0x3fe, lv); }
        if (r && r->type && type && r->type != type) r = nullptr;   // numero riusato da un'altra creatura
        if (!r) {
            CompRec nr; nr.hero = key; nr.id = ids[i]; nr.type = type; nr.name = name; nr.level = lv; nr.flag = flags[i]; nr.inGroup = true;
            g_compReg.push_back(nr);
            CompRegLine("entra nel gruppo", g_compReg.back(), "");
            continue;
        }
        r->type = type ? type : r->type; r->name = name ? name : r->name; r->level = lv ? lv : r->level; r->flag = flags[i];
        if (!r->inGroup) { r->inGroup = true; r->checkAt = 0; r->last = -1; CompRegLine("torna nel gruppo", *r, ""); }
    }
    for (CompRec& r : g_compReg) {                 // uscite e controlli
        if (r.hero != key) continue;
        bool still = false;
        for (int i = 0; i < n; ++i) if (ids[i] == r.id) still = true;
        if (r.inGroup && !still) {
            r.inGroup = false; r.leftAt = now; r.checkAt = now; r.last = -1;
            const DWORD c = ObjectAddr(mgr, r.id);
            DWORD hp = 0, home = 0; int x = 0, y = 0;
            if (c) { Rd(c + 0x4d8, hp); Rd(c + 0x1c, x); Rd(c + 0x20, y); Rd(c + 0x251, home); }
            char e[96];
            _snprintf_s(e, sizeof(e), _TRUNCATE, c ? "vita %lu, in (%d,%d), +0x251 %ld" : "creatura gia' sparita", hp, x, y, static_cast<long>(home));
            CompRegLine("esce dal gruppo", r, e);
        }
        if (r.inGroup || !r.checkAt || static_cast<LONG>(now - r.checkAt) < 0) continue;
        r.checkAt = now + (now - r.leftAt < 120000 ? 10000 : 60000);
        DWORD other = 0; double d = -1;
        const int st = CompRegState(mgr, r, h, &other, &d);
        if (st == r.last) continue;
        r.last = st;
        char e[96];
        if (st == 0) _snprintf_s(e, sizeof(e), _TRUNCATE, "vivo, a %.0f dall'eroe", d);
        else if (st == 1) _snprintf_s(e, sizeof(e), _TRUNCATE, "morto, a %.0f dall'eroe", d);
        else if (st == 2) _snprintf_s(e, sizeof(e), _TRUNCATE, "non con questo numero: stesso tipo con il numero %lu", other);
        else _snprintf_s(e, sizeof(e), _TRUNCATE, "non c'e' piu' nessuno del suo tipo");
        CompRegLine("controllo", r, e);
    }
}

// --- 0.56.40: scheda Compagni nello zaino (fase 2) ----------------------------------------------------------------------
// il tester, 04/11: "e se invece che nel menu esc mi metti una voce nell'inventario generico per i compagni? che la selezioni e ti
// apre un menu compagni", "non sarebbe meglio allora duplicare quel compagno quando decidi di reclutarlo? cosi' se piu' avanti
// nella storia c'e' di nuovo, noi non gli diamo fastidio". Prove della 0.56.37-0.56.39 (registro): un compagno congedato resta
// nel mondo e nel salvataggio, ma il numero cambia con la zona (693 -> 1740 -> 1401): si lavora per tipo.
// Scheda: le schede dello zaino (cUI_TabControl, TabCtlOf/TabsRect) non si toccano; la mod disegna una scheda "Compagni" dopo
// l'ultima (o sopra, a destra, se dopo non c'e' posto) e, quando la si sceglie, copre la finestra dello zaino con la sua pagina:
// un compagno per tipo fra quelli che il registro ha visto nel gruppo di questo eroe, con nome (testo +0x3c della creatura, dal
// gestore dei testi: 0x673300 su [0x17e6ee8], come per il nome dell'eroe 2), livello e stato. Azioni, nel thread del motore:
// "Recluta una copia" = una creatura nuova del suo tipo accanto all'eroe (0x5fbf90 sul gestore degli oggetti [0xad4bc0]: tipo,
// posizione come +0x18 dell'eroe {word, x, y, z}, 0, 1, 0; ret 0x14; come gli script, per es. 0x54ded3) e il reclutamento del
// gioco (0x54d370 sull'eroe: creatura, 1, 1, come al caricamento a 0x60be2c; ret 0xc); l'originale resta alla storia. Una copia
// sola per tipo, e solo se nessuno di quel tipo e' gia' nel gruppo. "Congeda" = congedo del gioco (0x54d780 sull'eroe: creatura,
// 1) e, per una copia, la cancellazione (0x5fc300 sul gestore: id, 0, 0, 0), cosi' non restano sosia nel mondo. Le copie vanno nel
// registro e nel file ("copia creata", "copia cancellata"); dopo un caricamento il membro del gruppo di un tipo con una copia
// attiva si considera la copia. Col pad si usa il cursore (A).
// 0.56.41 (il tester, 04/11, foto della 0.56.40: "la scheda compagni e' un po' sfasata, forse meglio metterla ad esempio accanto alla
// freccetta in basso a sinistra azzurra", "non si vede il cursore sopra la schermata della tua mod", "dovra' essere fatto anche per
// il pad 2"): la scheda accanto alla freccetta in basso a sinistra dello zaino (CompArrowRel; senza freccetta sotto l'angolo in
// basso a sinistra); i cursori dei due giocatori ridisegnati sopra la pagina e il pulsante sotto il cursore piu' chiaro (il gioco
// disegna il cursore prima dell'overlay); una scheda per colonna con i due zaini, ognuna del suo giocatore (clic del secondo mouse
// per la colonna 2). Il gruppo e' uno solo, quello dell'eroe 1 (come i ritratti e la vista del compagno): anche dalla colonna 2
// si recluta e si congeda per il gruppo. La copia prende il testo del nome dell'originale (+0x3c: la copia nasceva con 0 e il
// gioco scriveva il nome del tipo, "Elfo della foresta di Tyr-Fasul"), e lo stato di chi e' fuori dal gruppo si cerca fra le
// creature del tipo con quel nome (la 0.56.40 prendeva la prima del tipo: "Livello 27" era un'altra elfa).
struct CompPanelColData { bool open = false; RECT tabR{}, panelR{}; DWORD tabAt = 0; };
struct CompPanelData {
    CompPanelColData col[2];             // 0.56.41: colonna dell'eroe 1, colonna dell'eroe 2
    int n = 0; CompPanelRow rows[kCompPanelRows];
    DWORD rowType[kCompPanelRows] = {}; DWORD rowId[kCompPanelRows] = {}; DWORD rowName[kCompPanelRows] = {}; bool rowCopy[kCompPanelRows] = {};
};
static CompPanelData g_cp;               // scritto dal thread della mod (righe) e del motore (rettangoli), letto dall'overlay
static std::map<DWORD, std::string> g_nameCache;   // testo -> nome (thread del motore scrive, sotto g_cpMx)
static std::set<DWORD> g_copyIds;        // numeri delle copie in questa sessione
CallResult CallThis5(DWORD fn, DWORD thisp, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5)
{
    CallResult cr;
    DWORD before = 0, after = 0, r = 0;
    __try {
        __asm {
            mov  eax, fn
            mov  ecx, thisp
            mov  before, esp
            push a5
            push a4
            push a3
            push a2
            push a1
            call eax
            mov  after, esp
            mov  esp, before
            mov  r, eax
        }
        cr.ok = true; cr.ret = r; cr.balanced = (after == before);
    } __except (CaptureException(GetExceptionInformation())) {
        cr.ok = false;
    }
    return cr;
}
RECT MemoryCompRowButton(const RECT& p, int i)   // pulsante della riga i della pagina (lo usano l'overlay e i clic)
{
    const LONG top = p.top + 34 + i * 38;
    return RECT{ p.right - 190, top + 5, p.right - 14, top + 31 };
}
// 0.56.43 (il tester, 05/11: "cliccare su compagni col cursore del pad 1 e' stato difficilissimo"): il clic vale su tutta la riga, non
// solo sul pulsante
RECT MemoryCompRow(const RECT& p, int i)
{
    const LONG top = p.top + 34 + i * 38;
    return RECT{ p.left + 10, top, p.right - 10, top + 36 };
}
// 0.56.43: scheda Compagni della colonna (1 o 2) disegnata adesso, per LB/RB (Core.cpp): dopo l'ultima scheda del gioco
bool MemoryCompTab(int side, RECT& tab, bool& open)
{
    if (side < 1 || side > 2) return false;
    std::lock_guard<std::mutex> lk(g_cpMx);
    const CompPanelColData& c = g_cp.col[side - 1];
    if (!c.tabAt || static_cast<LONG>(GetTickCount() - c.tabAt) >= 250) return false;
    tab = c.tabR; open = c.open;
    return true;
}
bool MemoryCompPanel(CompPanelView& v)
{
    std::lock_guard<std::mutex> lk(g_cpMx);
    const DWORD now = GetTickCount();
    bool any = false;
    for (int s = 0; s < 2; ++s) {
        CompPanelColData& c = g_cp.col[s];
        if (!c.tabAt || static_cast<LONG>(now - c.tabAt) >= 250) { c.open = false; continue; }
        v.col[s].tab = true; v.col[s].open = c.open; v.col[s].tabR = c.tabR; v.col[s].panelR = c.panelR;
        any = true;
    }
    if (!any) return false;
    v.n = g_cp.n;
    for (int i = 0; i < g_cp.n && i < kCompPanelRows; ++i) v.rows[i] = g_cp.rows[i];
    DWORD m1 = 0; int x = 0, y = 0;                  // 0.56.41: cursori, da ridisegnare sopra la pagina
    if (Rd(0x00cdcadc, m1) && m1 && Rd(m1 + 4, x) && Rd(m1 + 8, y)) { v.curOn[0] = true; v.cur[0] = POINT{ x, y }; }
    if (MemoryMouse2Ready() && g_m2UiOn.load() && now - g_m2UiAt.load() < 300) { v.curOn[1] = true; v.cur[1] = POINT{ g_m2UiX.load(), g_m2UiY.load() }; }
    return true;
}
// 0.56.41: la freccetta azzurra in basso a sinistra dello zaino: un elemento piccolo e visibile fra i figli della finestra (cercato
// nella finestra e nella pagina dello zaino, come CollectChildren), nella meta' in basso e nel terzo a sinistra. Rettangolo relativo
// alla finestra (vale anche per la colonna spostata), rifatto ogni 2 s; la prima volta per ogni finestra gli elementi piccoli
// trovati vanno nel registro.
static bool CompArrowRel(DWORD win, int wx, int wy, int w, int h, RECT& rel)
{
    static DWORD cWin = 0, cAt = 0, loggedWin = 0, okWin = 0;
    static bool cOk = false;
    static RECT cRel{};
    const DWORD now = GetTickCount();
    if (win == cWin && now - cAt < 2000) { rel = cRel; return cOk; }
    cWin = win; cAt = now; cOk = false;
    const bool log = loggedWin != win;
    loggedWin = win;
    DWORD bases[2] = { win, 0 };
    const DWORD ctl = TabCtlOf(win);
    if (ctl) TabInvIndex(ctl, &bases[1]);
    std::vector<DWORD> seen;
    std::vector<DWORD> buf(0x1000 / 4);
    LONG best = -0x7FFFFFFF;
    int logged = 0;
    for (DWORD base : bases) {
        if (!base || !RdBuf(base, buf.data(), 0x1000)) continue;
        for (size_t i = 1; i < buf.size(); ++i) {
            const DWORD v = buf[i];
            DWORD e = 0, f = 0, p = 0;
            if (v >= 0x00880000u && v < 0x00900000u) e = base + static_cast<DWORD>(i * 4);   // incorporato
            else if (v >= 0x01000000u && v < 0x7FFF0000u && !(v & 3)) e = v;                  // puntato
            else continue;
            if (!Rd(e + 0x10, f) || !(f & 1)) continue;
            bool child = false;
            p = e;
            for (int d = 0; d < 5 && !child; ++d) { if (!Rd(p + 0x50, p) || !p) break; if (p == win) child = true; }
            if (!child || std::find(seen.begin(), seen.end(), e) != seen.end()) continue;
            seen.push_back(e);
            short sz[2] = {};
            if (!RdBuf(e + 0x2c, sz, sizeof(sz)) || sz[0] < 8 || sz[1] < 8 || sz[0] > 48 || sz[1] > 48) continue;
            int x = 0, y = 0;
            AbsPos(e, x, y);
            if (log && logged < 24) {
                ++logged;
                DWORD vt = 0; Rd(e, vt);
                Log("COMPAGNI: zaino: elemento piccolo vt %08lX in (%d,%d) %dx%d (finestra in %d,%d, %dx%d)", vt, x, y, sz[0], sz[1], wx, wy, w, h);
            }
            if (x < wx - 80 || x + sz[0] > wx + w / 3 || y < wy + h / 2 || y > wy + h + 80) continue;
            const LONG score = (y + sz[1]) - x;     // il piu' in basso e a sinistra
            if (score > best) { best = score; cRel = RECT{ x - wx, y - wy, x - wx + sz[0], y - wy + sz[1] }; cOk = true; }
        }
    }
    if (log) Log("COMPAGNI: zaino: freccetta in basso a sinistra %s (%ld,%ld)-(%ld,%ld) rispetto alla finestra", cOk ? "trovata in" : "non trovata, ultima", cRel.left, cRel.top, cRel.right, cRel.bottom);
    // 0.56.42 (foto della Steam Machine del 04/11 alle 23:34:48): con un'altra pagina (Arti) la freccetta non si vede e la scheda
    // scivolava di 30 pixel; resta dove era l'ultima volta che c'era
    if (cOk) okWin = win;
    else if (okWin == win) cOk = true;
    rel = cRel;
    return cOk;
}
// dal disegno dello zaino di una colonna (thread del motore, con gli spostamenti della colonna gia' applicati); side = eroe della
// colonna (1 o 2)
static void CompTabNote(DWORD win, int side)
{
    if (!g_cfg.nativeCompanionView || side < 1 || side > 2) return;
    DWORD f = 0; short wh[2] = {};
    if (!Rd(win + 0x10, f) || !(f & 1) || !RdBuf(win + 0x2c, wh, sizeof(wh)) || wh[0] < 200 || wh[1] < 100) return;
    const DWORD ctl = TabCtlOf(win);
    RECT tr{};
    if (!ctl || !TabsRect(ctl, tr)) return;
    int wx = 0, wy = 0;
    AbsPos(win, wx, wy);
    const LONG tw = 132, th = 28;                     // 0.56.43: piu' grande (era 112x22)
    RECT a{}, t{};
    // 0.56.48 (il tester, 06/11: "quel bottone compagni comunque e' orribile li' sotto nell'inventario", "io farei una scheda dopo combo,
    // anche a forma di icona cosi' ci sta senza spostare i bottoni dell'inventario"): una scheda a icona subito a destra delle schede
    // del gioco (Zaino, Arti di combattimento, Combo), alta come loro; a 1920 le schede finiscono a x 1464 e lo zaino a 1536. Se non
    // ci sta dentro la finestra, come prima accanto alla freccetta.
    // 0.56.49 (il tester, 06/11, sulle quattro proposte: "La A"): pastiglia come le schede del gioco, 46 pixel, alta come loro piu' 4
    const LONG iw = 46, ih = (tr.bottom - tr.top) + 4;
    if (tr.right > tr.left && tr.bottom > tr.top && tr.right + 6 + iw <= wx + wh[0]) {
        const LONG cy = (tr.top + tr.bottom) / 2 + 3;     // 0.56.51 (foto della 0.56.50: "va solo spostato verso il basso di poco poco")
        t = RECT{ tr.right + 6, cy - ih / 2, tr.right + 6 + iw, cy - ih / 2 + ih };
    }
    else if (CompArrowRel(win, wx, wy, wh[0], wh[1], a)) {   // 0.56.41: accanto alla freccetta, centrata con lei
        const LONG cy = wy + (a.top + a.bottom) / 2;
        t = RECT{ wx + a.right + 6, cy - th / 2, wx + a.right + 6 + tw, cy - th / 2 + th };
    }
    else t = RECT{ wx + 8, wy + wh[1] + 4, wx + 8 + tw, wy + wh[1] + 4 + th };   // senza freccetta: sotto l'angolo in basso a sinistra
    std::lock_guard<std::mutex> lk(g_cpMx);
    static LONG logged[2] = { -1, -1 };
    const LONG key = t.left ^ (t.top << 12) ^ (wx << 4);
    if (key != logged[side - 1]) { logged[side - 1] = key; Log("COMPAGNI: scheda Compagni della colonna %d in (%ld,%ld)-(%ld,%ld), schede del gioco (%ld,%ld)-(%ld,%ld), zaino (%d,%d) %dx%d", side, t.left, t.top, t.right, t.bottom, tr.left, tr.top, tr.right, tr.bottom, wx, wy, wh[0], wh[1]); }
    CompPanelColData& c = g_cp.col[side - 1];
    DwStoreRect(g_compTabShown[side - 1], t, true);  // 0.56.42: per i clic del pad 2
    c.tabR = t;
    c.panelR = RECT{ wx, wy, wx + wh[0], wy + wh[1] };
    c.tabAt = GetTickCount() | 1;
}
static void CompPanelLine(const char* ev, DWORD type, DWORD id, const char* extra)
{
    CompRec r; r.type = type; r.id = id;
    DWORD mgr = 0, pl = 0, loc = 0;
    if (Rd(kObjMgr, mgr) && mgr && Rd(kPlayer, pl) && pl && RdHero(pl, loc)) { const DWORD h = ObjectAddr(mgr, loc); if (h) r.hero = CompHeroKey(h); }
    const DWORD c = mgr ? ObjectAddr(mgr, id) : 0;
    if (c) { Rd(c + 0x3c, r.name); Rd(c + 0x3fe, r.level); }
    CompRegLine(ev, r, extra);
}
// --- 0.56.44: scheda della copia conservata al congedo (il tester, 05/11: "congedandola la mod deve ricopiare il personaggio e tutto
// il suo inventario"; prima tappa) ---------------------------------------------------------------------------------------------
// Studio del 05/11: il gioco tiene in memoria solo i personaggi della zona caricata (alle 10:09:48 della prova la Maegalcarwen vera
// non si trovava piu'), quindi una copia lasciata nel mondo non si ritroverebbe; e il tipo del compagno non ha un equipaggiamento
// di base (0x426580 ne da' uno solo ad alcune classi di eroi: l'arco di Maegalcarwen glielo ha dato la storia, comando degli script
// 0x482160 -> 0x54b530). Al congedo della copia: la sua scheda (da +0x3b4 esperienza a +0x3ff livello: i campi del controllo
// anti-trucchi 0x5909e0, cioe' esperienza, bonus +0x3c4.., abilita' +0x3cc.., livelli +0x3d4.., punti +0x3ea/+0x3ec, +0x3ee,
// attributi +0x3f2..+0x3fc, livello +0x3fe) e la vita +0x4d8 vanno in save\SacredPad_copie.txt (una riga per tipo); i suoi
// oggetti tornano nello zaino dell'eroe 1 (0x556cf0 toglie dallo slot e restituisce il numero; posa come Mouse2StowNow), o a
// terra accanto a lui se lo zaino e' pieno (0x5fd200, il "pop" del gestore degli oggetti); poi congedo e cancellazione come prima.
// Al reclutamento seguente la copia nuova riprende la scheda, se il gioco non le ha dato un livello piu' alto. Gli oggetti identici
// (con le proprieta' magiche) sono la seconda tappa: formato del salvataggio del gioco, ancora da studiare.
constexpr DWORD kSheetAt = 0x3b4, kSheetLen = 0x4c;
struct CopySheet { BYTE b[kSheetLen] = {}; DWORD hp = 0, hero = 0; WORD level = 0; };
static std::map<DWORD, CopySheet> g_copySheet;   // tipo -> scheda (sotto g_cpMx)
static bool g_copySheetLoaded = false;
static void CopySheetPath(char* path, size_t n)
{
    CompRegPath(path, n);
    if (char* sl = strrchr(path, '\\')) { *sl = 0; strncat_s(path, n, "\\SacredPad_copie.txt", _TRUNCATE); }
}
static void CopySheetLoadLocked()          // chi chiama tiene g_cpMx
{
    if (g_copySheetLoaded) return;
    g_copySheetLoaded = true;
    char path[MAX_PATH];
    CopySheetPath(path, sizeof(path));
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long type = 0, hero = 0, hp = 0; unsigned lv = 0;
        char hex[2 * kSheetLen + 8] = {};
        if (sscanf_s(line, "tipo %lu;eroe %lx;livello %u;vita %lu;scheda %159s", &type, &hero, &lv, &hp, hex, static_cast<unsigned>(sizeof(hex))) != 5) continue;
        if (strlen(hex) != 2 * kSheetLen) continue;
        CopySheet s; s.hero = hero; s.level = static_cast<WORD>(lv); s.hp = hp;
        bool ok = true;
        for (DWORD i = 0; i < kSheetLen && ok; ++i) { unsigned v = 0; char two[3] = { hex[2 * i], hex[2 * i + 1], 0 }; ok = sscanf_s(two, "%2x", &v) == 1; s.b[i] = static_cast<BYTE>(v); }
        if (ok) { g_copySheet[type] = s; ++n; }
    }
    fclose(f);
    if (n) Log("COMPAGNI: %d schede di copie congedate (save\\SacredPad_copie.txt)", n);
}
static void CopySheetSaveLocked()
{
    char path[MAX_PATH];
    CopySheetPath(path, sizeof(path));
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) { Log("COMPAGNI: ATTENZIONE: %s non scrivibile", path); return; }
    for (const auto& kv : g_copySheet) {
        fprintf(f, "tipo %lu;eroe %08lX;livello %u;vita %lu;scheda ", kv.first, kv.second.hero, kv.second.level, kv.second.hp);
        for (BYTE v : kv.second.b) fprintf(f, "%02X", v);
        fprintf(f, "\r\n");
    }
    fclose(f);
}
static void Show3D(DWORD obj)              // il contrario di Hide3D (oggetto posato a terra dopo un tentativo nello zaino)
{
    __try {
        const DWORD o3 = reinterpret_cast<DynCastFn>(0x008498a1)(obj, 0, 0x008ea650, 0x0094bce0, 0);
        DWORD vt = 0, fn = 0;
        if (o3 && Rd(o3, vt) && Rd(vt + 0x28, fn) && fn >= 0x401000 && fn < 0x88f000) reinterpret_cast<VfArg1Fn>(fn)(o3, 0, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// oggetti della copia nello zaino dell'eroe 1 (o a terra accanto a lui), nel thread del motore: numero di oggetti tolti
static int CopyGearOut(DWORD om, DWORD h, DWORD heroId, DWORD c, DWORD id)
{
    DWORD inv = 0, db = 0;
    Rd(kInvTable, inv); Rd(0x00aaa564, db);
    int moved = 0, ground = 0;
    for (DWORD slot = 0; slot < 0x12; ++slot) {
        DWORD it = 0;
        if (!Rd(c + 0x1a4 + slot * 4, it) || !it) continue;
        if (ItemBound(it)) { Log("COMPAGNI: copia %lu: oggetto %lu dello slot %lu legato al compagno, resta a lei", id, it, slot); continue; }
        const CallResult rr = CallThis2(0x00556cf0, c, slot, 0);
        if (!rr.ok || !rr.balanced) { LogException("oggetto tolto alla copia"); continue; }
        const DWORD got = rr.ret;
        if (!got) { Log("COMPAGNI: copia %lu: slot %lu, il gioco non ha restituito l'oggetto %lu", id, slot, it); continue; }
        const CallResult co = CallThis1(0x005fe550, om, got);
        const DWORD obj = co.ok ? co.ret : 0;
        DWORD otype = 0, w = 0, hh = 0;
        bool placed = false;
        if (obj && inv && db && heroId >= 1 && heroId <= 2 && Rd(obj + 0x10, otype) &&
            CallThis3(0x00428d60, db, otype, reinterpret_cast<DWORD>(&w), reinterpret_cast<DWORD>(&hh)).ok && w && hh) {
            Hide3D(obj);
            placed = StowInEntry(inv + heroId * kInvSize, got, w, hh) > 0;
            if (!placed) Show3D(obj);
        }
        if (!placed) {
            BYTE pos[0x10] = {};
            if (RdBuf(h + 0x18, pos, sizeof(pos))) {
                const CallResult pr = CallThis2(0x005fd200, om, got, reinterpret_cast<DWORD>(pos));
                if (pr.ok) ++ground; else LogException("oggetto della copia posato a terra");
            }
        }
        Log("COMPAGNI: copia %lu: oggetto %lu (tipo %lu) dello slot %lu %s", id, got, otype, slot, placed ? "nello zaino dell'eroe 1" : "a terra accanto all'eroe 1");
        ++moved;
    }
    if (ground) OverlayCoopToast(Tr("Zaino pieno: alcuni oggetti della copia sono a terra"), 4000);
    return moved;
}
static void CompRecruitCopy(DWORD type, DWORD name)  // thread del motore; name = testo del nome dell'originale (0 = nessuno)
{
    DWORD om = 0, mgr = 0, pl = 0, loc = 0;
    if (!Rd(0x00ad4bc0, om) || !om || !Rd(kObjMgr, mgr) || !mgr || !Rd(kPlayer, pl) || !pl || !RdHero(pl, loc)) return;
    const DWORD h = ObjectAddr(mgr, loc);
    DWORD beg = 0, end = 0;
    if (!h || !Rd(h + 0x39c, beg) || !Rd(h + 0x3a0, end) || end < beg || (end - beg) / 9 >= 4) { OverlayCoopToast(Tr("Il gruppo e' pieno"), 2500); return; }
    for (DWORD p = beg; p < end; p += 9) { DWORD id = 0, t = 0; const DWORD c = Rd(p, id) && id ? ObjectAddr(mgr, id) : 0; if (c && Rd(c + 0x10, t) && t == type) { OverlayCoopToast(Tr("E' gia' nel gruppo"), 2500); return; } }
    BYTE pos[0x10] = {};
    if (!RdBuf(h + 0x18, pos, sizeof(pos))) return;
    int x = 0; memcpy(&x, pos + 4, 4); x += 90; memcpy(pos + 4, &x, 4);
    const CallResult cr = CallThis5(0x005fbf90, om, type, reinterpret_cast<DWORD>(pos), 0, 1, 0);
    if (!cr.ok || !cr.balanced) { LogException("creazione della copia del compagno"); Log("COMPAGNI: ATTENZIONE: copia del tipo %lu non creata (%s)", type, cr.ok ? "pila sbilanciata" : "errore"); return; }
    DWORD id = cr.ret, c = ObjectAddr(mgr, id), t = 0;
    if (!c) { DWORD pid = 0; if (cr.ret > 0x10000 && Rd(cr.ret + 0xc, pid) && ObjectAddr(mgr, pid) == cr.ret) { c = cr.ret; id = pid; } }
    if (!c || !Rd(c + 0x10, t) || t != type) { Log("COMPAGNI: ATTENZIONE: 0x5fbf90 ha restituito %08lX: nessuna creatura del tipo %lu", cr.ret, type); return; }
    DWORD was = 0;                                  // 0.56.41: il nome dell'originale (la copia nasce con 0: nome del tipo)
    if (name && Rd(c + 0x3c, was) && was != name) { WriteDword(c + 0x3c, name); Log("COMPAGNI: copia %lu: nome %08lX dall'originale (era %08lX)", id, name, was); }
    const CallResult rr = CallThis3(0x0054d370, h, c, 1, 1);
    if (!rr.ok || !rr.balanced) { LogException("reclutamento della copia"); Log("COMPAGNI: ATTENZIONE: reclutamento della copia %lu non riuscito", id); return; }
    // 0.56.44: la scheda della copia congedata l'ultima volta (se il gioco non le ha appena dato un livello piu' alto)
    CopySheet sh; bool have = false;
    { std::lock_guard<std::mutex> lk(g_cpMx); g_copyTypes.insert(type); g_copyIds.insert(id); CopySheetLoadLocked(); const auto it = g_copySheet.find(type); if (it != g_copySheet.end()) { sh = it->second; have = true; } }
    bool restored = false;
    if (have) {
        WORD lvNow = 0;
        Rd(c + 0x3fe, lvNow);
        if (lvNow <= sh.level) {
            for (DWORD i = 0; i < kSheetLen; ++i) WriteByte(c + kSheetAt + i, sh.b[i]);
            if (sh.hp) WriteDword(c + 0x4d8, sh.hp);
            restored = true;
            Log("COMPAGNI: copia %lu: ripresa la sua scheda (livello %u, vita %lu)", id, sh.level, sh.hp);
        }
        else Log("COMPAGNI: copia %lu: il gioco le ha dato il livello %u, piu' alto della sua scheda (%u): scheda non ripresa", id, lvNow, sh.level);
    }
    CompPanelLine("copia creata", type, id, restored ? "con la sua scheda" : "");
    OverlayCoopToast(Tr(restored ? "Copia del compagno reclutata con la sua scheda" : "Copia del compagno reclutata"), 3000);
}
static void CompDismiss(DWORD type, DWORD id, bool copy)   // thread del motore
{
    DWORD om = 0, mgr = 0, pl = 0, loc = 0;
    if (!Rd(0x00ad4bc0, om) || !om || !Rd(kObjMgr, mgr) || !mgr || !Rd(kPlayer, pl) || !pl || !RdHero(pl, loc)) return;
    const DWORD h = ObjectAddr(mgr, loc), c = ObjectAddr(mgr, id);
    DWORD t = 0;
    if (!h || !c || !Rd(c + 0x10, t) || t != type) return;
    CompPanelLine(copy ? "copia congedata" : "congedato dalla scheda", type, id, "");
    int gear = 0;
    if (copy) {                                    // 0.56.44: scheda nel file e oggetti all'eroe 1, prima di cancellarla
        CopySheet sh;
        if (RdBuf(c + kSheetAt, sh.b, kSheetLen)) {
            Rd(c + 0x3fe, sh.level); Rd(c + 0x4d8, sh.hp); sh.hero = CompHeroKey(h);
            std::lock_guard<std::mutex> lk(g_cpMx);
            CopySheetLoadLocked();
            g_copySheet[type] = sh;
            CopySheetSaveLocked();
            Log("COMPAGNI: copia %lu: scheda salvata (livello %u, vita %lu)", id, sh.level, sh.hp);
        }
        gear = CopyGearOut(om, h, loc, c, id);
    }
    const CallResult cr = CallThis2(0x0054d780, h, c, 1);
    if (!cr.ok || !cr.balanced) { LogException("congedo del compagno"); return; }
    if (copy) {
        const CallResult dr = CallThis4(0x005fc300, om, id, 0, 0, 0);
        if (!dr.ok || !dr.balanced) { LogException("cancellazione della copia"); Log("COMPAGNI: ATTENZIONE: copia %lu non cancellata", id); }
        else CompPanelLine("copia cancellata", type, id, "");
        std::lock_guard<std::mutex> lk(g_cpMx);
        g_copyTypes.erase(type); g_copyIds.erase(id);
    }
    OverlayCoopToast(Tr(!copy ? "Compagno congedato" : gear ? "Copia congedata: i suoi oggetti sono nel tuo zaino" : "Copia del compagno congedata"), 3000);
}
// righe della pagina (thread della mod, ogni secondo mentre la scheda si vede)
static void CompPanelTick(DWORD mgr, DWORD heroId, DWORD now)
{
    static DWORD at = 0;
    bool show = false;
    { std::lock_guard<std::mutex> lk(g_cpMx); for (const CompPanelColData& c : g_cp.col) if (c.tabAt && static_cast<LONG>(now - c.tabAt) < 1000) show = true; }
    if (!show || static_cast<LONG>(now - at) < 700) return;
    at = now;
    const DWORD h = mgr && heroId ? ObjectAddr(mgr, heroId) : 0;
    if (!h) return;
    const DWORD key = CompHeroKey(h);
    int hx = 0, hy = 0; Rd(h + 0x1c, hx); Rd(h + 0x20, hy);
    // gruppo attuale: tipo -> id
    std::map<DWORD, DWORD> group;
    DWORD beg = 0, end = 0;
    if (Rd(h + 0x39c, beg) && Rd(h + 0x3a0, end) && end >= beg && (end - beg) / 9 <= 16)
        for (DWORD p = beg; p < end; p += 9) { DWORD id = 0, t = 0; const DWORD c = Rd(p, id) && id ? ObjectAddr(mgr, id) : 0; if (c && Rd(c + 0x10, t)) group[t] = id; }
    // tipi visti (registro), nell'ordine in cui sono entrati
    std::vector<std::pair<DWORD, DWORD>> types;   // tipo, testo del nome
    for (const CompRec& r : g_compReg) {
        if (r.hero != key || !r.type) continue;
        bool dup = false; for (auto& q : types) if (q.first == r.type) dup = true;
        if (!dup) types.push_back({ r.type, r.name });
    }
    for (auto& g : group) { bool dup = false; for (auto& q : types) if (q.first == g.first) dup = true; if (!dup) { DWORD nm = 0; const DWORD c = ObjectAddr(mgr, g.second); if (c) Rd(c + 0x3c, nm); types.push_back({ g.first, nm }); } }
    CompPanelData d;
    std::vector<DWORD> needNames;
    std::set<DWORD> copyTypes, copyIds;
    std::map<DWORD, WORD> sheetLv;                    // 0.56.44: copie congedate con la loro scheda
    { std::lock_guard<std::mutex> lk(g_cpMx); copyTypes = g_copyTypes; copyIds = g_copyIds; CopySheetLoadLocked(); for (const auto& kv : g_copySheet) sheetLv[kv.first] = kv.second.level; }
    for (auto& tp : types) {
        if (d.n >= kCompPanelRows) break;
        CompPanelRow& row = d.rows[d.n];
        const DWORD type = tp.first;
        DWORD c = 0, id = 0; WORD lv = 0;
        const auto gi = group.find(type);
        if (gi != group.end()) { id = gi->second; c = ObjectAddr(mgr, id); }
        bool dead = false; double dist = -1;
        if (!c) {                                     // fuori dal gruppo: una creatura viva del tipo nel mondo
            // 0.56.41: con il nome del registro, se c'e' (le altre creature del tipo sono generiche), e mai una copia
            DWORD ob = 0, oe = 0;
            if (Rd(mgr + 4, ob) && Rd(mgr + 8, oe) && oe > ob && (oe - ob) / 4 < 200000)
                for (DWORD i = 1; i < (oe - ob) / 4; ++i) {
                    const DWORD o = ObjectAddr(mgr, i);
                    DWORD t = 0, on = 0;
                    if (!o || !Rd(o + 0x10, t) || t != type || copyIds.count(i)) continue;
                    if (tp.second && (!Rd(o + 0x3c, on) || on != tp.second)) continue;
                    int cls = 0; bool dd = false; DWORD hp = 0; ReadKind(o, cls, dd); Rd(o + 0x4d8, hp);
                    int x = 0, y = 0; Rd(o + 0x1c, x); Rd(o + 0x20, y);
                    c = o; id = i; dead = dd || !hp;
                    dist = std::sqrt(static_cast<double>(x - hx) * (x - hx) + static_cast<double>(y - hy) * (y - hy));
                    if (!dead) break;
                }
        }
        if (c) Rd(c + 0x3fe, lv);
        DWORD nm = tp.second;
        if (c) { DWORD n2 = 0; if (Rd(c + 0x3c, n2) && n2) nm = n2; }
        std::string name;
        { std::lock_guard<std::mutex> lk(g_cpMx); const auto it = g_nameCache.find(nm); if (it != g_nameCache.end()) name = it->second; }
        if (name.empty() && nm) needNames.push_back(nm);
        if (name.empty()) { char b[32]; _snprintf_s(b, sizeof(b), _TRUNCATE, "%s %lu", Tr("Compagno"), type); name = b; }
        strncpy_s(row.name, name.c_str(), _TRUNCATE);
        const bool inGroup = gi != group.end();
        const bool isCopy = inGroup && (copyIds.count(id) || copyTypes.count(type));
        char st[64];
        // 0.56.54 (il tester, prova del 07/11: congedato l'originale 695 alle 15:55:07, "finche' la missione non e' finita e non va via di
        // sua spontanea volonta' non puo' andarsene prima"): il compagno che il gioco ha messo nel gruppo con la storia non si congeda
        // dalla scheda; se ne va solo quando lo decide la storia (Maegalcarwen alle 16:11:32). Si congedano solo le copie.
        const bool story = inGroup && !isCopy;
        if (inGroup) _snprintf_s(st, sizeof(st), _TRUNCATE, "%s %u, %s", Tr("Livello"), lv, Tr(isCopy ? "copia, con te" : "con te (storia)"));
        else if (sheetLv.count(type)) _snprintf_s(st, sizeof(st), _TRUNCATE, "%s %u, %s", Tr("Livello"), sheetLv[type], Tr("copia pronta"));
        else if (c && !dead) _snprintf_s(st, sizeof(st), _TRUNCATE, "%s %u, %s", Tr("Livello"), lv, Tr(dist >= 0 && dist < 2500 ? "nella storia, qui vicino" : "nella storia, lontano"));
        else if (c) _snprintf_s(st, sizeof(st), _TRUNCATE, "%s", Tr("nella storia, morto"));
        else _snprintf_s(st, sizeof(st), _TRUNCATE, "%s", Tr("non e' in questa zona"));
        strncpy_s(row.status, st, _TRUNCATE);
        strncpy_s(row.button, story ? "" : Tr(inGroup ? "Congeda" : "Recluta una copia"), _TRUNCATE);
        row.action = !story;
        d.rowType[d.n] = type; d.rowId[d.n] = inGroup ? id : 0; d.rowCopy[d.n] = isCopy; d.rowName[d.n] = tp.second ? tp.second : nm;
        ++d.n;
    }
    {
        std::lock_guard<std::mutex> lk(g_cpMx);
        g_cp.n = d.n;
        for (int i = 0; i < d.n; ++i) { g_cp.rows[i] = d.rows[i]; g_cp.rowType[i] = d.rowType[i]; g_cp.rowId[i] = d.rowId[i]; g_cp.rowCopy[i] = d.rowCopy[i]; g_cp.rowName[i] = d.rowName[i]; }
    }
    if (!needNames.empty())
        RunOnGameThread([needNames] {
            DWORD tm = 0;
            if (!Rd(0x017e6ee8, tm) || !tm) return;
            for (DWORD nm : needNames) {
                const CallResult cr = CallThis1(0x00673300, tm, nm);   // kGetText (testo -> stringa larga {inizio, fine, ...})
                DWORD s[2] = {};
                std::string out;
                if (cr.ok && cr.ret && cr.ret != tm + 0xc && RdBuf(cr.ret, s, sizeof(s)) && s[0] && s[1] > s[0] && (s[1] - s[0]) / 2 < 64) {
                    wchar_t w[64] = {};
                    RdBuf(s[0], w, s[1] - s[0]);
                    char a[128] = {};
                    WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof(a) - 1, nullptr, nullptr);
                    out = a;
                }
                if (out.empty()) continue;
                std::lock_guard<std::mutex> lk(g_cpMx);
                g_nameCache[nm] = out;
            }
        });
}
// clic sullo zaino (thread del motore): true = preso dalla scheda o dalla pagina. 0.56.41: una scheda per colonna, ognuna del suo
// giocatore (il clic del secondo mouse e' del giocatore 2); il clic sulla scheda o sulla pagina dell'altro non passa.
static bool CompPanelClick(DWORD ev)
{
    if (DwEvType(ev) != 2) return false;
    int btn = 0, x = 0, y = 0;
    if (!Rd(ev + 4, btn) || !Rd(ev + 8, x) || !Rd(ev + 12, y)) return false;
    const int who = (g_m2 && Mouse2Cur() == g_m2) ? 2 : 1;
    std::lock_guard<std::mutex> lk(g_cpMx);
    const DWORD now = GetTickCount();
    auto in = [&](const RECT& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    auto other = [&](int side) {
        static DWORD toastAt = 0;
        if (now - toastAt > 2000) { toastAt = now; OverlayCoopToast(Tr(side == 2 ? "Questa e' la colonna del giocatore 2" : "Questa e' la colonna del giocatore 1"), 2000); }
        Log("COMPAGNI: clic del giocatore %d in (%d,%d) sulla scheda Compagni della colonna %d: ignorato", who, x, y, side);
        return true;
    };
    for (int s = 0; s < 2; ++s) {
        CompPanelColData& c = g_cp.col[s];
        if (!c.tabAt || static_cast<LONG>(now - c.tabAt) >= 250) continue;
        const int side = s + 1;
        if (in(c.tabR)) {
            if (who != side) return other(side);
            if (btn == 0) { c.open = !c.open; Log("COMPAGNI: scheda Compagni della colonna %d %s", side, c.open ? "aperta" : "chiusa"); }
            return true;
        }
        if (!c.open) continue;
        if (!in(c.panelR)) {                           // una scheda del gioco o fuori: la pagina si chiude, il clic passa
            if (who == side && y < c.panelR.top && x >= c.panelR.left && x < c.panelR.right) { c.open = false; Log("COMPAGNI: scheda Compagni della colonna %d chiusa (clic sulle schede del gioco)", side); }
            continue;
        }
        if (who != side) return other(side);
        if (btn != 0) return true;
        for (int i = 0; i < g_cp.n; ++i) {
            if (!in(MemoryCompRow(c.panelR, i))) continue;   // 0.56.43: tutta la riga
            const DWORD type = g_cp.rowType[i], id = g_cp.rowId[i], name = g_cp.rowName[i];
            const bool copy = g_cp.rowCopy[i];
            if (!g_cp.rows[i].action) {               // 0.56.54: compagno della storia, niente congedo
                OverlayCoopToast(Tr("Questo compagno segue la storia: se ne andra' quando la missione lo lascia"), 2500);
                Log("COMPAGNI: scheda della colonna %d: riga %d (tipo %lu, id %lu): compagno della storia, niente congedo", side, i + 1, type, id);
                break;
            }
            Log("COMPAGNI: scheda della colonna %d: %s sulla riga %d (tipo %lu, id %lu%s)", side, id ? "Congeda" : "Recluta una copia", i + 1, type, id, copy ? ", copia" : "");
            if (id) RunOnGameThread([type, id, copy] { CompDismiss(type, id, copy); });
            else RunOnGameThread([type, name] { CompRecruitCopy(type, name); });
            break;
        }
        return true;                                  // un clic sulla pagina non arriva allo zaino ne' al mondo
    }
    return false;
}

// 0.56.54 (il tester, prova del 07/11: "se giocatore 1 tiene il puntatore su un oggetto dell'inventario e vede le sue statistiche,
// l'altro giocatore col puntatore su un oggetto non vede le statistiche", "devi far gestire due cose contemporaneamente"). Il
// riquadro delle statistiche e' un cUI_Tooltip del gestore: le finestre lo chiedono con 0x75b080 (oggetti: la pagina dello
// zaino 0x6c15a0, l'equipaggiamento, la merce) o 0x75b120 (testi), sempre sul tooltip numero [gestore+0x54] del vettore
// [gestore+0x124]; la richiesta scrive posizione, testo, oggetto e il fotogramma ([+0x21d4] = [0xcd69d0]), e il disegno
// (0x6e7670, tutti i tooltip del vettore da 0x758ab0) lo mostra solo se e' stato chiesto in questo fotogramma. Uno solo per
// tutti e due i giocatori: le due colonne se lo contendevano. Ora la colonna della copia (giocatore 2) ha un suo tooltip, creato
// con la funzione del gioco (0x75ac80, cUI_Manager::tooltipCreate, che restituisce il numero) e messo in [gestore+0x54] per la
// durata delle chiamate della copia; si spegne da solo come l'altro quando il suo cursore lascia l'oggetto. Se il gioco lo
// distrugge (0x7595d0 toglie quelli che nessuno ha registrato) se ne crea un altro.
struct DwTip2Scope {
    DWORD mgr = 0; WORD old = 0; bool on = false;
    static WORD& Idx() { static WORD i = 0xffff; return i; }
    static DWORD& Ptr() { static DWORD p = 0; return p; }
    static bool Valid(DWORD m) {
        DWORD b = 0, e = 0, p = 0;
        const WORD i = Idx();
        return i != 0xffff && Rd(m + 0x124, b) && Rd(m + 0x128, e) && e > b && i < (e - b) / 4 && Rd(b + i * 4u, p) && p && p == Ptr();
    }
    // la creazione aggiunge un posto al vettore dei tooltip: non durante il disegno dell'interfaccia (il vettore potrebbe
    // spostarsi), ma al prossimo giro del thread del motore; fino ad allora la copia usa ancora quello del gioco
    static void Create() {
        static int fails = 0;
        DWORD m = 0;
        WORD cur = 0;
        if (fails >= 3 || !Rd(kUIManager, m) || !m || Valid(m) || !Rd(m + 0x54, cur)) return;
        const CallResult cr = CallThis0(0x0075ac80, m);
        DWORD b = 0, e = 0, p = 0;
        const WORD i = static_cast<WORD>(cr.ret & 0xffff);
        if (!cr.ok || !Rd(m + 0x124, b) || !Rd(m + 0x128, e) || e <= b || i >= (e - b) / 4 || !Rd(b + i * 4u, p) || !p || i == cur) {
            ++fails;
            Log("ZAINI: tooltip del giocatore 2 NON creato (%s, numero %u), %d di 3: le statistiche restano una per volta", cr.ok ? "risultato inatteso" : "eccezione", i, fails);
            return;
        }
        Idx() = i; Ptr() = p;
        Log("ZAINI: tooltip del giocatore 2 creato (numero %u, %08lX; quello del gioco e' il %u): statistiche degli oggetti per tutti e due", i, p, cur);
    }
    explicit DwTip2Scope(bool want) {
        if (!want || !Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x54, old)) return;
        if (!Valid(mgr)) {
            static std::atomic<DWORD> askedAt{ 0 };
            const DWORD now = GetTickCount();
            if (now - askedAt.load() > 1000) { askedAt.store(now); RunOnGameThread([] { Create(); }); }
            return;
        }
        if (old == Idx()) return;
        on = SetWord(mgr + 0x54, Idx());
    }
    ~DwTip2Scope() { WORD cur = 0; if (on && Rd(mgr + 0x54, cur) && cur == Idx()) SetWord(mgr + 0x54, old); }
};

// 0.56.56 (il tester, 10/11: "le schede in contemporanea funzionano sugli articoli dell'inventario e del mercante, ma non su quello
// dell'equipaggiato"): l'equipaggiamento non passa da 0x75b080 e [gestore+0x54]. Il suo disegno (0x6b7da9) prende il tooltip
// con 0x75aed0(numero) e il numero e' quello della finestra, la WORD a +0x15a (subito prima dei 32 posti da +0x15c); vera e copia
// sono la stessa finestra, quindi lo stesso tooltip, e restava solo quello dell'ultima (la copia). Per la durata del disegno
// della copia +0x15a e' il numero del tooltip del giocatore 2.
struct DwEquipTip2 {
    DWORD a = 0; WORD old = 0; bool on = false;
    DwEquipTip2(int k, DWORD win, bool want) {
        DWORD m = 0, b = 0, e = 0, p = 0;
        if (k != 1 || !want || !Rd(kUIManager, m) || !m || !DwTip2Scope::Valid(m) || !Rd(win + 0x15a, old)) return;
        const WORD i2 = DwTip2Scope::Idx();
        // il numero di prima deve essere un tooltip vero del vettore: altrimenti +0x15a non e' quello che si crede
        if (old == i2 || !Rd(m + 0x124, b) || !Rd(m + 0x128, e) || e <= b || old >= (e - b) / 4 || !Rd(b + old * 4u, p) || !p) {
            static bool warned = false;
            if (!warned && old != i2) { warned = true; Log("ZAINI: equipaggiamento: +0x15a = %u non e' un tooltip, il secondo riquadro resta spento", old); }
            return;
        }
        a = win + 0x15a;
        on = SetWord(a, i2);
        static bool logged = false;
        if (on && !logged) { logged = true; Log("ZAINI: equipaggiamento: tooltip del giocatore 2 (numero %u al posto del %u) per la copia", i2, old); }
    }
    ~DwEquipTip2() { WORD cur = 0; if (on && Rd(a, cur) && cur == DwTip2Scope::Idx()) SetWord(a, old); }
};

static void DualDraw(int k, DWORD self, DWORD edx, DWORD ctx, DWORD a = 0, DWORD b = 0)
{
    DWORD mgr = 0, w = 0;
    if (g_dwNest || !Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + kDualWin[k].index * 4, w) || w != self) { DwOrigDraw(k, self, edx, ctx, a, b); return; }
    DualLayout L;
    if (!DualLayoutNow(L) || !L.vis[k] || (k == 0 && !g_dwGridOk)) {
        if (CompDraw(1, k, self, ctx, a, b)) return;  // 0.56.28: una colonna sola, del giocatore 1
        DwOrigDraw(k, self, edx, ctx, a, b);
        if (k == 0) CompTabNote(self, 1);             // 0.56.40: scheda Compagni
        return;
    }
    const int realSide = L.realIs2 ? 2 : 1, copySide = L.realIs2 ? 1 : 2;   // 0.56.31: il compagno di ogni colonna
    g_dwActiveAt.store(GetTickCount() | 1);
    if (k == 0) { g_dwInvX.store(L.r[0].left); g_dwInvY.store(L.r[0].top); g_dwInvRealDx.store(L.RealOff(0).x); }
    static bool toast = false;
    if (!toast) { toast = true; OverlayCoopToast(Tr("Due zaini: a sinistra l'eroe 1, a destra l'eroe 2"), 4000); }
    if (k == 0) g_dwInvCopyDx.store(L.CopyOff(0).x);
    if (k >= 3) {                                       // 0.56.9: mercante condiviso (0.56.14: anche fabbro e Maestro)
        // 0.56.10: finestra appena aperta (o riaperta): la copia riparte dalla scheda attiva, le fotografie si rifanno
        const DWORD last = g_dwShAt.load();
        if (!last || GetTickCount() - last > 3000 || g_dwShK.load() != k) {
            g_tabCopyM = TabState{}; g_mOrg[0] = MerchOrg{}; g_mOrg[1] = MerchOrg{}; g_purchOwner.store(0);
            Log("CONDIVISO: %s aperto, eroe 1 a %+ld, eroe 2 a %+ld", kDualWin[k].name, L.off1[k].x, L.off2[k].x);
        }
        g_dwMerX.store(L.r[k].left); g_dwMerY.store(L.r[k].top); g_dwMerRealDx.store(L.RealOff(k).x); g_dwMerCopyDx.store(L.CopyOff(k).x);
        g_dwShAt.store(GetTickCount() | 1); g_dwShK.store(k);
        if (k == 3) g_dwMerAt.store(GetTickCount() | 1);
        if (k != 3) {}
        else if (!PurchaseWin()) g_purchOwner.store(0);
        else if (!g_purchOwner.load() && GetTickCount() - g_merchClickAt.load() < 1500) {
            g_purchOwner.store(g_merchClicker.load());
            Log("MERCANTE: conferma dell'acquisto aperta dal giocatore %d", g_purchOwner.load());
        }
    }
    // 0.55.54: un lato per giocatore; senza lati scelti (finestra appena aperta dal gioco) si vede quello dell'eroe 1
    const bool s1 = g_dualSide1.load(), s2 = g_dualSide2.load();
    const bool realOn = L.realIs2 || s1 || !s2, copyOn = L.realIs2 || s2;
    const bool tabsOk = k != 0 || DwTabsReady(L);
    int mx = -100000, my = -100000;
    MouseUi(mx, my);
    const bool overCopy = tabsOk && copyOn && InRect(L.CopyHit(k), mx, my);
    // 0.56.0: con il secondo mouse la copia (dell'eroe 2) si disegna con il puntatore del giocatore 2
    LONG qx = 0, qy = 0;
    const bool m2 = Mouse2Want(qx, qy) && g_m2UiOn.load();
    const bool over2 = m2 ? (tabsOk && copyOn && InRect(L.CopyHit(k), qx, qy)) : overCopy;
    DwStoreRect(g_dwShown[0][k], L.RealHit(k), realOn);
    DwStoreRect(g_dwShown[1][k], L.CopyHit(k), copyOn && tabsOk);
    if (realOn) {
        ++g_dwNest;
        DwShift sh(k, self, L.RealOff(k));
        DwTipOff tip(k, self, overCopy);
        DwStatScope ss(k, self, 0);
        DwMouseHide mh(k == 2 && !InRect(L.RealHit(k), mx, my));
        const bool ok = CompDraw(realSide, k, self, ctx, a, b) || DwCallDraw(k, self, ctx, a, b);
        if (k == 0 && ok) CompTabNote(self, realSide);  // 0.56.40: scheda Compagni (0.56.41: di ogni colonna)
        if (k == 3 || k == 4 || k == kDwChest) MerchSnap(0, self, k);
        --g_dwNest;
        if (!ok) { g_dwFaults.fetch_add(1); Log("ZAINI: ATTENZIONE: errore nel disegno di %s (finestra vera), %ld di 3", kDualWin[k].name, g_dwFaults.load()); return; }
    }
    if (realOn) DwSnap(0, k, self, L.RealOff(k).x, L.realIs2 ? 2 : 1);
    if (!tabsOk || !copyOn) return;
    // 0.56.14: fabbro e Maestro: il lato dell'eroe 2 e' la sua istanza
    const DWORD tgt = DwHasInst(k) ? InstFor(k) : self;
    if (!tgt) return;
    if (DwHasInst(k)) { InstOpenNow(k, self, L.page); g_instDrawAt[k] = GetTickCount(); }
    DwCopyCtx c(k, L);
    bool ok = true;
    {
        Mouse2Scope ms(m2, qx, qy);
        DwTip2Scope t2(m2);                             // 0.56.54: il riquadro delle statistiche del giocatore 2
        DwEquipTip2 et(k, tgt, m2);                     // 0.56.56: anche per l'equipaggiamento (tooltip della finestra)
        MgrSlotScope sl(DwHasInst(k) ? kDualWin[k].index : -1, tgt);
        DwShift sh(k, tgt, L.CopyOff(k));
        DwTipOff tip(k, tgt, !over2);
        DwStatScope ss(k, tgt, 1);
        DwMouseHide mh(k == 2 && !over2);
        ok = CompDraw(copySide, k, tgt, ctx, a, b) || DwCallDraw(k, tgt, ctx, a, b);
        if (k == 0 && ok) CompTabNote(tgt, copySide);   // 0.56.41: scheda Compagni della colonna della copia
        if (k == 3 || k == 4 || k == kDwChest) MerchSnap(1, tgt, k);
    }
    if (ok) DwSnap(1, k, self, L.CopyOff(k).x, L.realIs2 ? 1 : 2);   // prima di Restore: la serie dei riquadri e' ancora quella della copia
    c.Restore();
    if (!ok) { g_dwFaults.fetch_add(1); Log("ZAINI: ATTENZIONE: errore nel disegno della copia di %s, %ld di 3 prima di spegnerla", kDualWin[k].name, g_dwFaults.load()); }
}

static int DwEvType(DWORD ev)                    // tipo dell'evento (metodo +4): 2 = clic del mouse
{
    DWORD vt = 0, fn = 0;
    if (!Rd(ev, vt) || !Rd(vt + 4, fn) || !fn) return -1;
    __try { return reinterpret_cast<EvTypeFn>(static_cast<uintptr_t>(fn))(ev, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static char DualInput(int k, DWORD self, DWORD edx, DWORD ev)
{
    DWORD mgr = 0, w = 0;
    if (!ev || g_dwNest || !Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + kDualWin[k].index * 4, w) || w != self) return g_dwInput[k](self, edx, ev);
    DualLayout L;
    if (k == 0 && CompPanelClick(ev)) return 1;         // 0.56.40: scheda e pagina Compagni
    if (!DualLayoutNow(L) || !L.vis[k] || (k == 0 && !g_dwGridOk)) {
        char cr = 0;                                    // 0.56.28: una colonna sola, del giocatore 1
        if (CompInput(1, k, self, ev, cr, [&] { return DwCallInput(k, self, ev); })) return cr;
        return g_dwInput[k](self, edx, ev);
    }
    const int realSide = L.realIs2 ? 2 : 1, copySide = L.realIs2 ? 1 : 2;   // 0.56.31
    int x = 0, y = 0;
    const int type = DwEvType(ev);
    const bool pos = (type == 2 || type == 3) && Rd(ev + 8, x) && Rd(ev + 12, y);
    const bool click = type == 2 && pos, release = type == 3 && pos;
    const bool tabsOk = k != 0 || g_tabCopy.have;
    const bool s1 = g_dualSide1.load(), s2 = g_dualSide2.load();
    const bool realOn = L.realIs2 || s1 || !s2, copyOn = L.realIs2 || s2;
    // 0.55.53 (controllo): gli eventi delle statistiche (0x6a6270) confrontano il clic anche con coordinate fisse dello schermo
    // (pulsante a x 0x2a8..0x30c y 0x32..0x46, oro da gettare a x 0x2d3..0x61f y 0x170..0x2f1), che con la finestra spostata
    // non la seguirebbero: per lei si sposta il clic all'indietro e la finestra resta dove la mette il gioco. Zaino ed
    // equipaggiamento non hanno coordinate fisse nei clic (e la griglia dello zaino usa l'origine del disegno spostato).
    const bool p1Game = P1GameMouse();                  // 0.56.0: clic del pad 1 in modalita' gioco: la copia non c'e'
    // 0.56.9: mercante condiviso: con la conferma di un acquisto aperta dall'altro giocatore un clic sulla merce aspetta
    if (k >= 3 && click) {
        const int who = (g_m2 && Mouse2Cur() == g_m2) ? 2 : 1;
        const int owner = k == 3 ? g_purchOwner.load() : 0;
        if (owner && owner != who && PurchaseWin()) {
            OverlayCoopToast(Tr(owner == 1 ? "Aspetta: il giocatore 1 sta confermando un acquisto" : "Aspetta: il giocatore 2 sta confermando un acquisto"), 2000);
            return 1;
        }
        // 0.56.10: con il secondo mouse ogni colonna del mercante e' di un giocatore: un clic sull'altra non compra per l'altro eroe
        // (la 0.56.9 lo prendeva come clic della copia, con l'eroe 2). I clic del pad 1 in modalita' gioco vanno al mondo come prima.
        if (MemoryMouse2Ready() && !(who == 1 && p1Game)) {
            const int copyHero = L.realIs2 ? 1 : 2;
            const bool inCopy = copyOn && tabsOk && InRect(L.CopyHit(k), x, y), inReal = realOn && InRect(L.RealHit(k), x, y);
            if ((inCopy && !inReal && who != copyHero) || (inReal && !inCopy && who == copyHero)) {
                static DWORD toastAt = 0;
                if (GetTickCount() - toastAt > 2000) {
                    toastAt = GetTickCount();
                    if (k == 3) OverlayCoopToast(Tr(who == 1 ? "Questo e' il mercante del giocatore 2" : "Questo e' il mercante del giocatore 1"), 2000);
                    else OverlayCoopToast(Tr(who == 1 ? "Questa e' la colonna del giocatore 2" : "Questa e' la colonna del giocatore 1"), 2000);
                }
                Log("CONDIVISO: clic del giocatore %d in (%d,%d) sulla colonna dell'altro (%s): ignorato", who, x, y, kDualWin[k].name);
                return 1;
            }
        }
        g_merchClicker.store(who);
        g_merchClickAt.store(GetTickCount());
    }
    if (click && copyOn && tabsOk && !p1Game && InRect(L.CopyHit(k), x, y)) {
        if (k == 0 && CompAddrSide(copySide) && RightClick(ev)) {   // 0.56.31: zaino della colonna col compagno
            CompToastDrag();
            Log("COMPAGNO: clic destro nello zaino della colonna %d con la vista del compagno: non passa", copySide);
            return 1;
        }
        DwCopyCtx c(k, L);
        int r = 0;
        if (k == 2) {
            const POINT o = L.CopyOff(k);
            DwStatScope ss(k, self, 1);
            WriteDword(ev + 8, static_cast<DWORD>(x - o.x)); WriteDword(ev + 12, static_cast<DWORD>(y - o.y));
            { char cr = 0; r = CompInput(copySide, 2, self, ev, cr, [&] { return DwCallInput(k, self, ev); }) ? cr : DwCallInput(k, self, ev); }
            WriteDword(ev + 8, static_cast<DWORD>(x)); WriteDword(ev + 12, static_cast<DWORD>(y));
        }
        else {
            const DWORD tgt = DwHasInst(k) ? InstFor(k) : self;   // 0.56.14: fabbro e Maestro dell'eroe 2
            if (DwHasInst(k) && tgt) InstOpenNow(k, self, L.page);
            MgrSlotScope sl(DwHasInst(k) ? kDualWin[k].index : -1, tgt);
            DwShift sh(k, tgt ? tgt : self, L.CopyOff(k));
            // 0.55.66: prova del 02/11 alle 19:29 ("se col pad 2 dall'inventario 2 premo B su una spada o altro, cerca di
            // equipaggiarlo al player 1"): il clic destro su un'arma dello zaino (0x6c0270, tipi 5 e 0xd) manda alla barra
            // (UI_WND_TASKBAR, evento 0x2b) "metti l'arma in uno slot", e la barra e' quella dell'eroe 1 (+0x154/+0x158; nel log
            // dopo ogni B la barra dell'eroe 1 cambiava). Per i clic della copia dello zaino la barra ha l'eroe della copia,
            // come per la seconda barra (TbOtherCtx).
            DWORD tb = 0, tvt = 0;
            if (k == 0 && Rd(mgr + 0x80, tb) && tb && Rd(tb, tvt) && tvt == kTbVt) {
                TbOtherCtx tc(tb, L.sd);
                r = DwCallInput(k, self, ev);
                tc.Restore();
            }
            else if (tgt) { char cr = 0; r = (k == 1 && CompInput(copySide, 1, tgt, ev, cr, [&] { return DwCallInput(k, tgt, ev); })) ? cr : DwCallInput(k, tgt, ev); }
        }
        c.Restore();
        if (k == 3 && !g_purchOwner.load() && PurchaseWin()) g_purchOwner.store(g_merchClicker.load());   // 0.56.9: conferma appena aperta
        int btn = 0; Rd(ev + 4, btn);
        Log("ZAINI: clic %s su %s dell'eroe %lu in (%d,%d)%s", btn == 2 ? "destro" : "sinistro", kDualWin[k].name, L.sd.otherId, x, y, r < 0 ? ": ERRORE" : "");
        if (r < 0) g_dwFaults.fetch_add(1);
        return 1;                                       // un clic sulla copia non arriva al mondo
    }
    // 0.55.54: lato dell'eroe 1 chiuso (la finestra del gioco resta aperta per l'eroe 2): i clic sulla sua zona vanno al mondo.
    // 0.55.60 (controllo): dopo la copia, non prima. Con il solo lato dell'eroe 2 la copia sta proprio sopra la finestra vera
    // (spostamenti 0 dalla 0.55.59) e il controllo messo prima mandava al mondo ogni clic del giocatore 2 sul suo zaino; con la
    // 0.55.58 succedeva dove le due zone si sovrapponevano ("Nuova abilita'" assegnata e l'eroe 1 che cammina).
    if (click && !realOn && InRect(L.RealHit(k), x, y)) return 0;
    // 0.55.60 (controllo): anche il rilascio. Il motore (0x618410) passa al mondo (0x617640) anche il rilascio del sinistro che
    // nessuna finestra prende, e li' il rilascio lontano dall'ultimo clic nel mondo e' l'ordine 4 (fermati) all'eroe 1: con le
    // finestre spostate ogni clic del giocatore 2 sulla copia fermava l'eroe 1 che il pad 1 stava muovendo o facendo attaccare.
    if (release && copyOn && tabsOk && !p1Game && InRect(L.CopyHit(k), x, y)) return 1;
    // 0.55.59: un clic dentro la finestra vera disegnata non arriva mai al mondo. Prova delle 16:08-16:15: il "+" delle abilita'
    // e "Nuova abilita'" facevano il loro lavoro ma il gestore delle statistiche non segnala il clic come preso, e il gioco lo
    // passava anche al mondo (l'eroe 1 camminava verso il punto); nel gioco normale non si vede perche' la finestra e' al suo posto.
    const bool insideReal = (click || release) && realOn && InRect(L.RealHit(k), x, y);
    // 0.55.66: prova del 02/11 alle 19:30 ("il pad 1 non puo' prendere in mano le abilita' della scheda Arti di combattimento"):
    // la pagina delle arti (0x6c2050) mette l'arte sul puntatore alla pressione, e il rilascio sopra la pagina la rimette giu'
    // (trascinamento). Il rilascio dentro la copia la mod lo tiene (0.55.60), e per il giocatore 2 l'arte restava in mano; per il
    // giocatore 1 il rilascio arrivava allo zaino e l'arte tornava al suo posto. Ora anche per lo zaino vero: si posa con un clic.
    if (release && k == 0 && insideReal) return 1;
    if (k == 0 && click && insideReal && CompAddrSide(realSide) && RightClick(ev)) {   // 0.56.31: zaino della colonna col compagno
        CompToastDrag();
        Log("COMPAGNO: clic destro nello zaino della colonna %d con la vista del compagno: non passa", realSide);
        return 1;
    }
    ++g_dwNest;
    char r = 0;
    if (k == 2) {
        const POINT o = L.RealOff(k);
        DwStatScope ss(k, self, 0);
        if (click) { WriteDword(ev + 8, static_cast<DWORD>(x - o.x)); WriteDword(ev + 12, static_cast<DWORD>(y - o.y)); }
        { char cr = 0; r = CompInput(realSide, 2, self, ev, cr, [&] { return DwCallInput(k, self, ev); }) ? cr : g_dwInput[k](self, edx, ev); }
        if (click) { WriteDword(ev + 8, static_cast<DWORD>(x)); WriteDword(ev + 12, static_cast<DWORD>(y)); }
    }
    else {
        DwShift sh(k, self, L.RealOff(k));
        { char cr = 0; r = (k == 1 && CompInput(realSide, 1, self, ev, cr, [&] { return DwCallInput(k, self, ev); })) ? cr : g_dwInput[k](self, edx, ev); }
    }
    --g_dwNest;
    if (k == 3 && click && !g_purchOwner.load() && PurchaseWin()) g_purchOwner.store(g_merchClicker.load());   // conferma appena aperta
    return insideReal ? 1 : r;
}

// 0.55.63: caselle dell'equipaggiamento e "+" delle statistiche dove sono disegnati, per la croce dei due pad (prova del 02/11:
// "il pad del giocatore 2 non naviga con le frecce dove inserisce le armature e nelle statistiche"). Le finestre restano al posto
// del gioco fuori dalle chiamate della mod e la serie dei riquadri dell'eroe 2 c'e' solo durante quelle della copia: dopo ogni
// disegno se ne fa una fotografia (posizione + spostamento del lato, eroe del lato), letta dal thread della mod per 300 ms.
static void DwScanEquip(DWORD win, LONG dx, std::vector<MenuItem>& out)   // come MemoryEquipSlots
{
    DWORD slots[32];
    if (!RdBuf(win + 0x15c, slots, sizeof(slots))) return;
    for (int k = 0; k < 32; ++k) {
        const DWORD e = slots[k];
        short wh[2] = {};
        if (!e || (e & 3) || !RdBuf(e + 0x2C, wh, sizeof(wh)) || wh[0] < 8 || wh[1] < 8) continue;
        MenuItem it;
        it.addr = e;
        Rd(e, it.vt);
        Rd(e + 0x10, it.flags);
        AbsPos(e, it.x, it.y);
        it.x += dx;
        it.w = wh[0]; it.h = wh[1];
        _snprintf_s(it.name, sizeof(it.name), _TRUNCATE, "casella %d", k);
        out.push_back(it);
    }
}
static void DwScanStat(DWORD win, LONG dx, std::vector<MenuItem>& out)    // come MemoryStatButtons
{
    std::vector<DWORD> buf(0xF80 / 4);
    if (!RdBuf(win, buf.data(), 0xF80)) return;
    for (size_t i = 1; i < buf.size(); ++i) {
        if (buf[i] != 0x00896308) continue;                       // cUI_Button2
        const DWORD b = win + static_cast<DWORD>(i * 4);
        DWORD f = 0; short wh[2] = {};
        if (!Rd(b + 0x10, f) || !(f & 1) || !RdBuf(b + 0x2C, wh, sizeof(wh)) || wh[0] < 6 || wh[1] < 6) continue;
        MenuItem it;
        it.addr = b; it.vt = 0x00896308; it.flags = f;
        AbsPos(b, it.x, it.y);
        it.x += dx;
        it.w = wh[0]; it.h = wh[1];
        _snprintf_s(it.name, sizeof(it.name), _TRUNCATE, "scheda +%03X", static_cast<unsigned>(i * 4));
        out.push_back(it);
    }
}
struct DwItems { std::vector<MenuItem> v; DWORD at = 0; int hero = 0; };
SRWLOCK g_dwItemsLock = SRWLOCK_INIT;
DwItems g_dwItems[2][kDw];               // [0] finestra vera, [1] copia
static void DwSnap(int side, int k, DWORD win, LONG dx, int hero)
{
    if (k != 1 && k != 2) return;                // 0.56.9: solo equipaggiamento e statistiche (non zaino ne' mercante)
    std::vector<MenuItem> v;
    if (k == 1) DwScanEquip(win, dx, v); else DwScanStat(win, dx, v);
    AcquireSRWLockExclusive(&g_dwItemsLock);
    g_dwItems[side][k].v.swap(v);
    g_dwItems[side][k].at = GetTickCount();
    g_dwItems[side][k].hero = hero;
    ReleaseSRWLockExclusive(&g_dwItemsLock);
}
// Con i due zaini attivi: aggiunge le caselle (k 1) o i pulsanti (k 2) disegnati per quell'eroe e restituisce vero (anche se
// non ce ne sono: il suo lato e' chiuso); altrimenti falso e la lettura resta quella di sempre.
bool DwItemsFor(int hero, int k, std::vector<MenuItem>& out)
{
    if (!MemoryDualActive() || k < 1 || k >= kDw) return false;
    AcquireSRWLockShared(&g_dwItemsLock);
    for (int s = 0; s < 2; ++s) {
        const DwItems& d = g_dwItems[s][k];
        if (d.hero == hero && static_cast<LONG>(GetTickCount() - d.at) < 300) out.insert(out.end(), d.v.begin(), d.v.end());
    }
    ReleaseSRWLockShared(&g_dwItemsLock);
    return true;
}
bool MemoryHero2Items(std::vector<MenuItem>& out)
{
    out.clear();
    if (!g_ok || !g_dualSide2.load() || !MemoryDualActive()) return false;
    DwItemsFor(2, 1, out);
    DwItemsFor(2, 2, out);
    return !out.empty();
}

// 0.55.62: punto dentro la finestra (metodo +0x1c, thiscall (x, y), ret 8). Il motore lo usa per decidere se un clic non
// preso dalle finestre va al mondo (0x75a780 dal ramo del clic di 0x618410): con le finestre spostate rispondeva per il posto
// del gioco, dove ora puo' non esserci niente (un clic li' veniva perso) mentre sopra le finestre disegnate altrove passava al
// mondo. Fuori dalle chiamate della mod (g_dwNest) e con i due zaini attivi risponde secondo i rettangoli disegnati.
typedef char(__fastcall* WinHitFn)(DWORD self, DWORD edx, int x, int y);
WinHitFn g_dwHit[kDw] = {};
static char DualHit(int k, DWORD self, DWORD edx, int x, int y)
{
    DWORD mgr = 0, w = 0;
    if (g_dwNest || !MemoryDualActive() || !Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x80 + kDualWin[k].index * 4, w) || w != self) return g_dwHit[k](self, edx, x, y);
    // 0.55.65 (controllo): solo con i rettangoli di questo disegno; se la finestra non e' piu' disegnata dalla mod (per esempio
    // mercante aperto: DualLayoutNow falso e disegno normale) risponde subito il gioco, non per 200 ms "nessuna finestra qui"
    bool fresh = false;
    const bool p1Game = P1GameMouse();                  // 0.56.0: per i clic del pad 1 in modalita' gioco la copia non c'e'
    for (int s = 0; s < 2; ++s) {
        const DwRectA& a = g_dwShown[s][k];
        if (static_cast<LONG>(GetTickCount() - a.at.load()) >= 200) continue;
        fresh = true;
        if (s == 1 && p1Game) continue;
        if (x >= a.l.load() && x < a.r.load() && y >= a.t.load() && y < a.b.load()) return 1;
    }
    return fresh ? 0 : g_dwHit[k](self, edx, x, y);
}
char __fastcall DualHitHook0(DWORD self, DWORD edx, int x, int y) { return DualHit(0, self, edx, x, y); }
char __fastcall DualHitHook1(DWORD self, DWORD edx, int x, int y) { return DualHit(1, self, edx, x, y); }
char __fastcall DualHitHook2(DWORD self, DWORD edx, int x, int y) { return DualHit(2, self, edx, x, y); }
char __fastcall DualHitHook3(DWORD self, DWORD edx, int x, int y) { return DualHit(3, self, edx, x, y); }
char __fastcall DualHitHook4(DWORD self, DWORD edx, int x, int y) { return DualHit(4, self, edx, x, y); }
char __fastcall DualHitHook5(DWORD self, DWORD edx, int x, int y) { return DualHit(5, self, edx, x, y); }
char __fastcall DualHitHook6(DWORD self, DWORD edx, int x, int y) { return DualHit(6, self, edx, x, y); }

void __fastcall DualDrawHook0(DWORD self, DWORD edx, DWORD ctx) { DualDraw(0, self, edx, ctx); }
void __fastcall DualDrawHook1(DWORD self, DWORD edx, DWORD ctx) { DualDraw(1, self, edx, ctx); }
void __fastcall DualDrawHook2(DWORD self, DWORD edx, DWORD ctx) { DualDraw(2, self, edx, ctx); }
void __fastcall DualDrawHook3(DWORD self, DWORD edx, DWORD ctx, DWORD a, DWORD b) { DualDraw(3, self, edx, ctx, a, b); }
void __fastcall DualDrawHook4(DWORD self, DWORD edx, DWORD ctx, DWORD a, DWORD b) { DualDraw(4, self, edx, ctx, a, b); }
void __fastcall DualDrawHook5(DWORD self, DWORD edx, DWORD ctx) { DualDraw(5, self, edx, ctx); }
void __fastcall DualDrawHook6(DWORD self, DWORD edx, DWORD ctx) { DualDraw(6, self, edx, ctx); }
char __fastcall DualInputHook0(DWORD self, DWORD edx, DWORD ev) { return DualInput(0, self, edx, ev); }
char __fastcall DualInputHook1(DWORD self, DWORD edx, DWORD ev) { return DualInput(1, self, edx, ev); }
char __fastcall DualInputHook2(DWORD self, DWORD edx, DWORD ev) { return DualInput(2, self, edx, ev); }
char __fastcall DualInputHook3(DWORD self, DWORD edx, DWORD ev) { return DualInput(3, self, edx, ev); }
char __fastcall DualInputHook4(DWORD self, DWORD edx, DWORD ev) { return DualInput(4, self, edx, ev); }
char __fastcall DualInputHook5(DWORD self, DWORD edx, DWORD ev) { return DualInput(5, self, edx, ev); }
char __fastcall DualInputHook6(DWORD self, DWORD edx, DWORD ev) { return DualInput(6, self, edx, ev); }

static bool PatchVtSlot(DWORD slotAddr, DWORD want, void* hook, DWORD& orig)
{
    DWORD cur = 0, old = 0;
    if (!Rd(slotAddr, cur) || cur != want) { Log("ZAINI: vtable inatteso a %08lX (%08lX invece di %08lX)", slotAddr, cur, want); return false; }
    void* slot = reinterpret_cast<void*>(static_cast<uintptr_t>(slotAddr));
    if (!VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    orig = cur;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(reinterpret_cast<uintptr_t>(hook)));
    VirtualProtect(slot, 4, old, &old);
    return true;
}

// 0.55.50: call 0x5dc900 a 0x6c1615 (nella pagina dello zaino) verso GridOriginStub, nel thread del motore.
static void InstallGridOriginNow()
{
    constexpr DWORD at = 0x006c1615;
    BYTE op = 0; LONG rel = 0;
    if (!Rd(at, op) || op != 0xE8 || !Rd(at + 1, rel) || static_cast<DWORD>(at + 5 + rel) != 0x005dc900) {
        Log("ZAINI: griglia della copia non spostabile: chiamata inattesa a %08lX (%02X, verso %08lX)", at, op, static_cast<DWORD>(at + 5 + rel));
        return;
    }
    DWORD old = 0;
    void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(at + 1));
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    *reinterpret_cast<LONG*>(p) = static_cast<LONG>(reinterpret_cast<uintptr_t>(&GridOriginStub)) - static_cast<LONG>(at + 5);
    VirtualProtect(p, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    g_dwGridOk = true;
    Log("ZAINI: origine della griglia spostabile (0x6c1615 -> GridOriginStub)");
}

// 0.55.57: ritratti in alto a destra (finestra n.7, cUI_Mercenary, vtable 0x8958cc: eventi +0x10 0x6d6d50, disegno +0x14
// 0x6d71d0). Prova della 0.55.56 (foto delle 15:35): le statistiche dell'eroe 2, a destra sopra il suo equipaggiamento, finivano
// sotto i ritratti. Con lo zaino dell'eroe 2 aperto i ritratti non si disegnano e i clic sulla loro zona non li prendono; gli
// altri eventi (vita, messaggi del gioco) li ricevono sempre.
WinInputFn g_mercInput = nullptr;
// 0.55.59: spento. Dalla 0.55.59 lo zaino dell'eroe 2 sta al posto del gioco e le sue statistiche non finiscono piu' sotto i
// ritratti; i ganci restano (nessun effetto) nel caso servano di nuovo.
bool MercHidden(DWORD self)
{
    (void)self;
    return false;
}
char __fastcall MercInputHook(DWORD self, DWORD edx, DWORD ev)
{
    if (ev && MercHidden(self) && DwEvType(ev) == 2) return 0;
    if (ev && CompanionClick(self, ev)) return 1;     // 0.56.28: vista del compagno
    return g_mercInput(self, edx, ev);
}

void InstallDualInventoryHooks()
{
    static bool tried = false, gridAsked = false, compAsked = false;
    if (!gridAsked) gridAsked = RunOnGameThread([] { InstallGridOriginNow(); });   // falso se il thread del motore non e' pronto
    if (!compAsked && g_cfg.nativeCompanionView) compAsked = RunOnGameThread([] { InstallCompanionFitNow(); });   // 0.56.28
    if (tried) return;
    tried = true;
    // prima si controlla tutto, poi si installa: o tutti i ganci o nessuno
    for (int k = 0; k < kDw; ++k) {
        DWORD d = 0, i = 0;
        if (!Rd(kDualWin[k].vt + kDualWin[k].slot, d) || d != kDualWin[k].draw || !Rd(kDualWin[k].vt + 0x10, i) || i != kDualWin[k].input) {
            Log("ZAINI: due zaini non attivati: vtable di %s inatteso (%08lX %08lX)", kDualWin[k].name, i, d);
            return;
        }
    }
    void* const draws[kDw] = { reinterpret_cast<void*>(&DualDrawHook0), reinterpret_cast<void*>(&DualDrawHook1), reinterpret_cast<void*>(&DualDrawHook2),
                               reinterpret_cast<void*>(&DualDrawHook3), reinterpret_cast<void*>(&DualDrawHook4), reinterpret_cast<void*>(&DualDrawHook5),
                               reinterpret_cast<void*>(&DualDrawHook6) };
    void* const inputs[kDw] = { reinterpret_cast<void*>(&DualInputHook0), reinterpret_cast<void*>(&DualInputHook1), reinterpret_cast<void*>(&DualInputHook2),
                                reinterpret_cast<void*>(&DualInputHook3), reinterpret_cast<void*>(&DualInputHook4), reinterpret_cast<void*>(&DualInputHook5),
                                reinterpret_cast<void*>(&DualInputHook6) };
    for (int k = 0; k < kDw; ++k) {
        DWORD od = 0, oi = 0;
        if (kDualWin[k].slot == 0x44) g_dwDraw3[k] = reinterpret_cast<WinDraw3Fn>(static_cast<uintptr_t>(kDualWin[k].draw));
        else g_dwDraw[k] = reinterpret_cast<WinDrawFn>(static_cast<uintptr_t>(kDualWin[k].draw));
        g_dwInput[k] = reinterpret_cast<WinInputFn>(static_cast<uintptr_t>(kDualWin[k].input));
        if (!PatchVtSlot(kDualWin[k].vt + kDualWin[k].slot, kDualWin[k].draw, draws[k], od)) return;
        if (!PatchVtSlot(kDualWin[k].vt + 0x10, kDualWin[k].input, inputs[k], oi)) return;
    }
    Log("ZAINI: due zaini attivati (ganci su disegno ed eventi di zaino, equipaggiamento, statistiche, mercante, fabbro, Maestro e deposito)");
    InstallPurchaseHook();                              // 0.56.9
    {   // 0.55.62: punto dentro (+0x1c); se manca, i clic funzionano come nella 0.55.61
        void* const hits[kDw] = { reinterpret_cast<void*>(&DualHitHook0), reinterpret_cast<void*>(&DualHitHook1), reinterpret_cast<void*>(&DualHitHook2),
                                  reinterpret_cast<void*>(&DualHitHook3), reinterpret_cast<void*>(&DualHitHook4), reinterpret_cast<void*>(&DualHitHook5),
                                  reinterpret_cast<void*>(&DualHitHook6) };
        DWORD h[kDw] = {};
        bool same = true;
        for (int k = 0; k < kDw; ++k) if (!Rd(kDualWin[k].vt + 0x1c, h[k]) || !h[k]) same = false;
        if (same) {
            int n = 0;
            for (int k = 0; k < kDw; ++k) {
                DWORD o = 0;
                g_dwHit[k] = reinterpret_cast<WinHitFn>(static_cast<uintptr_t>(h[k]));
                if (PatchVtSlot(kDualWin[k].vt + 0x1c, h[k], hits[k], o)) ++n;
            }
            Log("ZAINI: punto dentro le finestre secondo i posti disegnati (+0x1c: %08lX %08lX %08lX %08lX %08lX %08lX %08lX, %d di %d)", h[0], h[1], h[2], h[3], h[4], h[5], h[6], n, kDw);
        }
    }
    // il disegno dei ritratti ha gia' il gancio del ritratto dell'eroe 2 (MercDrawHook, 0.42): li' si salta; qui i clic
    DWORD mi = 0;
    g_mercInput = reinterpret_cast<WinInputFn>(static_cast<uintptr_t>(0x006d6d50));    // prima del gancio: mai un gancio senza originale
    if (PatchVtSlot(0x008958cc + 0x10, 0x006d6d50, reinterpret_cast<void*>(&MercInputHook), mi))
        Log("ZAINI: ritratti nascosti con lo zaino dell'eroe 2 aperto (clic su cUI_Mercenary)");
}

// I due zaini sono possibili adesso (thread della mod): co-op con l'eroe 2 vivo e assestato, deviazioni dell'interfaccia,
// nessuno scambio ne' salvataggio in corso, nessun pannello aperto oltre a zaino, equipaggiamento e statistiche.
// 0.55.66: qualunque cosa sul puntatore (oggetto, arte o combo: tipo 1, 2 o 3, come le crea 0x6c1a50), per il passaggio del
// mouse fra i due giocatori. Prova del 02/11 alle 19:31 (foto): l'arte presa dal giocatore 2 passava al puntatore del giocatore
// 1 appena lui muoveva lo stick, perche' MemoryHeldItem vede solo gli oggetti.
bool MemoryHeldAny()
{
    DWORD s = 0, p = 0;
    return Rd(0x00cdcadc, s) && s && Rd(s + 0x68, p) && p;
}
bool MemoryDualPossible()
{
    if (!g_ok || !g_cfg.nativeTwoInventories || !g_uiPatched.load() || g_saveOrig.load() || g_swapOrig.load() || g_dwFaults.load() >= 3 || !g_dwGridOk) return false;
    // 0.55.56 (controllo): con PadSource=None il giro di Core si ferma prima dei lati (pad 1 assente): il pad 2 resta allo scambio
    if (g_cfg.padSource == PadSource::None) return false;
    DWORD mgr = 0, om = 0, hp = 0;
    BYTE net = 0;
    if (!Rd(kUIManager, mgr) || !mgr || !Rd(kObjMgr, om) || !om || (Rd(kNetGame, net) && net)) return false;
    for (int i = 0; i < 23; ++i) {
        DWORD w = 0, f = 0;
        if (i == 0 || i == 1 || i == 2 || i == 6 || i == 7 || i == 8 || i == 9 || i == 18 || i == 19) continue;
        if ((i == 4 || i == 14 || i == 16 || (i == 3 && kShareSmith)) && MemoryMouse2Ready()) continue;   // 0.56.9 mercante, 0.56.14 Maestro, 0.56.54 deposito
        if (Rd(mgr + 0x80 + i * 4, w) && w && Rd(w + 0x10, f) && (f & 1)) return false;
    }
    const DWORD h2 = ObjectAddr(om, 2);
    return h2 && Rd(h2 + 0x4d8, hp) && hp && g_h2SeenAddr.load() == h2 && GetTickCount() - g_h2SeenAt.load() >= 2000;
}

// Griglia dello zaino dell'eroe 2 come la disegna la copia (origine della voce 2 scritta da GridOriginStub), per la croce del pad 2.
bool MemoryHero2Grid(GridInfo& g)
{
    DWORD table = 0;
    if (!g_ok || !MemoryDualActive() || !g_dualSide2.load() || !Rd(kInvTable, table) || !table) return false;
    if (!ReadGrid(table + 2 * kInvSize, g)) return false;
    g.inv = 2;
    g.kind = "zaino dell'eroe 2";
    return true;
}

// Schede della copia dello zaino (Zaino, Arti, Combo) nella posizione disegnata, con quella attiva per l'eroe 2 (LB/RB del pad 2).
bool MemoryDualCopyTabs(int& active, int& count, RECT* rects, int maxRects)
{
    DWORD mgr = 0, win = 0, beg = 0, end = 0;
    if (!g_ok || !MemoryDualActive() || !Rd(kUIManager, mgr) || !mgr || !Rd(mgr + 0x84, win) || !win) return false;
    const DWORD ctl = TabCtlOf(win);
    if (!ctl || !Rd(ctl + 0x78, beg) || !Rd(ctl + 0x7c, end) || end < beg || (end - beg) / 24 > 8) return false;
    count = static_cast<int>((end - beg) / 24);
    const TabState t = g_tabCopy;
    active = t.have ? t.active : 0;
    int wx = 0, wy = 0;
    AbsPos(win, wx, wy);
    for (int k = 0; k < count && k < maxRects; ++k) {
        DWORD b = 0; short wh[2] = {}; int x = 0, y = 0;
        if (!Rd(beg + k * 24 + 0x10, b) || !b || !RdBuf(b + 0x2c, wh, sizeof(wh))) return false;
        AbsPos(b, x, y);
        x += g_dwInvX.load() + g_dwInvCopyDx.load() - wx;
        y += g_dwInvY.load() - wy;
        rects[k] = RECT{ x, y, x + wh[0], y + wh[1] };
    }
    return count > 0;
}


// 0.52.3: cambio d'arma dell'eroe 2 (croce sinistra/destra del pad 2, come il pad 1): lo slot attuale e' il word +0xd58
// della sua voce dell'inventario ([0xad33c8] + 2 * 0x1180, come 0x5da070(0)), gli slot sbloccati vengono dal livello
// (+0x3fe; 1, 2, 8, 16, 30, come 0x57fb40). La scelta la fa il gioco: 0x6e17b0(slot, 1) sulla barra con l'eroe 2 al posto
// dell'eroe 1 (manda all'eroe 2 l'ordine 0x114 e aggiorna lo slot evidenziato), nel thread del motore.
void SwitchWeaponHero2(int step)
{
    RunOnGameThread([=] {
        const DWORD tb = VisibleWindow(0, kTbVt);
        DWORD inv = 0, mgr = 0;
        if (!tb || !Rd(0x00ad33c8, inv) || !inv || !Rd(kObjMgr, mgr) || !mgr) return;
        // 0.53.0: durante lo scambio della sola interfaccia la barra vera e' gia' dell'eroe 2 e non serve cambiarla
        TbSide sd;
        const bool copy = TaskbarSides(tb, sd) && sd.otherIs2;
        DWORD id = 0; Rd(tb + 0x154, id);
        if (!copy && !(g_swapUiLive.load() && id == 2)) return;
        const DWORD h2 = ObjectAddr(mgr, 2);
        WORD cur = 0, lvl = 0;
        if (!h2 || !Rd(inv + 2 * 0x1180 + 0xd58, cur) || !Rd(h2 + 0x3fe, lvl)) return;
        const int n = SlotCountOf(h2);
        if (n <= 1) { Log("PAD2: l'eroe 2 ha un solo slot d'arma (livello %u)", lvl); return; }
        const int next = ((cur % n) + step + n) % n;
        CallResult r{};
        if (copy) {
            TbOtherCtx c(tb, sd);
            r = CallThis2(0x006e17b0, tb, static_cast<DWORD>(next), 1);
            c.Restore();
        }
        else r = CallThis2(0x006e17b0, tb, static_cast<DWORD>(next), 1);
        if (!r.ok) { LogException("cambio d'arma dell'eroe 2"); return; }
        Log("PAD2: arma dell'eroe 2: slot %u -> %d (di %d)", cur + 1, next + 1, n);
    });
}

// Nome vero dell'eroe 2 (0.46.0, N5). Il nome di una creatura e' il testo numero +0x3c (0x44a570) letto con 0x673300
// (thiscall sul gestore dei testi, un argomento, ret 4; restituisce un puntatore a {inizio, fine} di una stringa UTF-16
// che chi chiama legge soltanto: statistiche, scritte di Alt, inventario, salvataggio). Il nome del giocatore e' il testo
// 0x2700, impostato dal menu dei personaggi con 0x6730d0, che pero' cambia solo testi gia' presenti nel database: per
// l'eroe 2 non c'e' un posto libero. Allora l'eroe 2 riceve in +0x3c un numero che il gioco non usa (kNameId2) e
// l'inizio di 0x673300 (mov eax,[esp+4] / push esi, 5 byte) diventa un salto a GetTextHook: per kNameId2 restituisce
// la stringa della mod con il nome letto dal file dell'eroe 2, per ogni altro numero rifa' le due istruzioni e torna
// a 0x673305.
constexpr DWORD kGetText = 0x00673300, kNameId2 = 0x7FFF2702;
// 0.46.1: 0x673300 restituisce risorsa+8 di un cScriptResource da 0x40 byte (0x6737e0: vtable, contatore, poi due stringhe
// UTF-16 a +8 e +0x14, due vettori a +0x20 e +0x2c, due campi a +0x38/+0x3c). Con i soli tre campi della stringa la scritta
// di Alt dell'eroe 2 non aveva il riquadro scuro (prova della 0.46.0): ora la struttura restituita e' una copia dei 0x38
// byte dopo +8 della risorsa del nome dell'eroe 1 (testo 0x2700), con la prima stringa sostituita dal nome dell'eroe 2.
// Le altre stringhe e i vettori puntano a quelli della risorsa dell'eroe 1, che chi chiama legge soltanto.
struct GameWStr { const wchar_t* begin; const wchar_t* end; const wchar_t* cap; DWORD rest[11]; };
wchar_t g_name2W[64] = {};
GameWStr g_name2Str = { g_name2W, g_name2W, g_name2W + 64, {} };
std::atomic<bool> g_name2Ready{ false };
DWORD g_getTextCont = 0x00673305;
std::atomic<bool> g_nameHookOn{ false };

__declspec(naked) void GetTextHook()
{
    __asm {
        mov eax, dword ptr [esp + 4]
        cmp eax, 0x7FFF2702
        je mine
        push esi
        jmp dword ptr [g_getTextCont]
    mine:
        mov eax, offset g_name2Str
        ret 4
    }
}

// Nel thread del motore, che e' quello che chiama 0x673300 per l'interfaccia: i 5 byte non cambiano mentre li esegue.
static void InstallNameHookNow()
{
    static const BYTE expect[5] = { 0x8B, 0x44, 0x24, 0x04, 0x56 };
    BYTE cur[5] = {};
    if (!RdBuf(kGetText, cur, 5) || memcmp(cur, expect, 5) != 0) {
        Log("NATIVO: nome dell'eroe 2: inizio di 0x673300 inatteso (%02X %02X %02X %02X %02X), gancio non installato", cur[0], cur[1], cur[2], cur[3], cur[4]);
        return;
    }
    BYTE jmp[5] = { 0xE9 };
    const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&GetTextHook)) - (kGetText + 5);
    memcpy(jmp + 1, &rel, 4);
    const bool ok = PatchCode(kGetText, jmp, 5);
    g_nameHookOn.store(ok);
    Log("NATIVO: nome dell'eroe 2: gancio su 0x673300 %s", ok ? "installato" : "NON installato (VirtualProtect)");
}

// 0.51.0: deviazione delle 158 chiamate delle finestre e della cintura (vedi kUiHeroCalls). Nel thread del motore, una volta: ogni punto deve
// essere ancora un "call rel32" verso la funzione attesa, altrimenti non si tocca nulla e lo scambio resta quello della 0.50.
void InstallUiSwapNow()
{
    struct Site { DWORD addr, target, stub; };
    std::vector<Site> sites;
    for (DWORD a : kUiHeroCalls) sites.push_back({ a, kGetHeroId, reinterpret_cast<DWORD>(&UiGetHeroStub) });
    for (DWORD a : kUiBeltCalls) sites.push_back({ a, kGetHeroId, reinterpret_cast<DWORD>(&UiBeltStub) });
    for (DWORD a : kUiPlayerCalls) sites.push_back({ a, kGetPlayer, reinterpret_cast<DWORD>(&UiPlayerStub) });
    int bad = 0;
    for (const Site& s : sites) {            // prima si controllano tutti
        BYTE op = 0; LONG rel = 0;
        if (!Rd(s.addr, op) || !Rd(s.addr + 1, rel) || op != 0xE8 || s.addr + 5 + rel != s.target) {
            if (bad < 5) Log("NATIVO: scambio dell'interfaccia: il punto %08lX non e' una chiamata a %08lX (byte %02X)", s.addr, s.target, op);
            ++bad;
        }
    }
    if (bad) { Log("NATIVO: scambio dell'interfaccia NON installato (%d punti su %u diversi dall'atteso): si usa lo scambio della 0.50", bad, static_cast<unsigned>(sites.size())); return; }
    int ok = 0;
    for (const Site& s : sites) {
        const LONG rel = static_cast<LONG>(s.stub - (s.addr + 5));
        if (PatchCode(s.addr + 1, reinterpret_cast<const BYTE*>(&rel), 4)) ++ok;
    }
    g_uiPatched.store(ok == static_cast<int>(sites.size()));
    Log("NATIVO: scambio dell'interfaccia: %d chiamate su %u deviate (GetHeroId %u, cintura %u, giocatore %u)%s", ok, static_cast<unsigned>(sites.size()),
        static_cast<unsigned>(sizeof(kUiHeroCalls) / 4), static_cast<unsigned>(sizeof(kUiBeltCalls) / 4), static_cast<unsigned>(sizeof(kUiPlayerCalls) / 4),
        g_uiPatched.load() ? "" : " - INCOMPLETO, si usa lo scambio della 0.50");
}

bool InstallNameHook()
{
    static bool asked = false;
    if (!asked) asked = RunOnGameThread([] { InstallNameHookNow(); });   // falso se il thread del motore non e' pronto: si riprova
    return g_nameHookOn.load();
}

// 0.55.24: diagnostica dello heap ([Native] HeapCheck=true). Prova del 30/10 alle 17:40: uscendo al menu principale eccezione in
// ntdll (0x936C5, liberazione dello heap) dentro cResourceMgr::disposeAll (0x65f610 <- 0x75a170 <- 0x7595d0), con indirizzi da
// immagine (0x62606060, 0x64646262) al posto dei puntatori dello heap: qualcuno aveva scritto oltre un blocco durante la partita.
// Ogni 3 s HeapValidate su tutti gli heap del processo (GetProcessHeaps): la prima volta che uno non e' valido lo scrive nel log,
// cosi' il momento dice dopo quale azione e' successo. HeapValidate blocca lo heap mentre lo percorre: solo per le prove.
// 0.55.26: dentro __try (SafeHeapValidate): uno heap creato senza blocco (HEAP_NO_SERIALIZE) che un altro thread sta cambiando
// puo' far fallire la lettura; la diagnostica non deve far cadere il gioco. -1 = eccezione durante il controllo.
static int SafeHeapValidate(HANDLE h)
{
    __try { return HeapValidate(h, 0, nullptr) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// 0.56.11: prova del 03/11 alle 16:30 (lo heap 1 di 17 rovinato fra le 16:29:59 e le 16:30:02, come nella prova della 0.56.5 alle
// 12:51 e prima del crash della 0.56.9 alle 16:01 in D3D10Warp): alla prima volta si cerca il blocco rovinato con HeapWalk
// (heap bloccato, nessuna scrittura nel log finche' e' bloccato) e si scrivono il blocco che non passa HeapValidate, quello
// prima e i byte intorno alla fine di quello prima: il primo dword di un oggetto del gioco e' il suo vtable.
struct HeapBad {
    DWORD walked = 0, err = 0; bool found = false, exc = false;
    void* p = nullptr; DWORD size = 0; BYTE head[32] = {};
    void* prev = nullptr; DWORD prevSize = 0; BYTE prevHead[16] = {}, prevEnd[48] = {};
};
static void HeapScanRaw(HANDLE h, HeapBad& b)
{
    PROCESS_HEAP_ENTRY e{};
    e.lpData = nullptr;
    void* prev = nullptr; DWORD prevSize = 0;
    if (!HeapLock(h)) return;
    __try {
        while (b.walked < 3000000 && HeapWalk(h, &e)) {
            ++b.walked;
            if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
            if (!HeapValidate(h, 0, e.lpData)) {
                b.found = true; b.p = e.lpData; b.size = e.cbData;
                memcpy(b.head, e.lpData, e.cbData < sizeof(b.head) ? e.cbData : sizeof(b.head));
                break;
            }
            prev = e.lpData; prevSize = e.cbData;
        }
        if (!b.found) b.err = GetLastError();
        b.prev = prev; b.prevSize = prevSize;
        if (prev) {
            memcpy(b.prevHead, prev, prevSize < sizeof(b.prevHead) ? prevSize : sizeof(b.prevHead));
            const BYTE* end = static_cast<const BYTE*>(prev) + prevSize;
            memcpy(b.prevEnd, end - 16, sizeof(b.prevEnd));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { b.exc = true; }
    HeapUnlock(h);
}
static void HeapFindBad(HANDLE h)
{
    static HeapBad b;
    b = HeapBad{};
    const DWORD t0 = GetTickCount();
    HeapScanRaw(h, b);
    auto hex = [](const BYTE* p, int n) { std::string s; char t[4]; for (int i = 0; i < n; ++i) { sprintf_s(t, "%02X", p[i]); s += t; if ((i & 3) == 3) s += ' '; } return s; };
    DWORD pv = 0; memcpy(&pv, b.prevHead, 4);
    Log("HEAP: ricerca nello heap %p: %lu voci in %lu ms%s; blocco che non passa: %p (%lu byte)%s", h, b.walked, GetTickCount() - t0,
        b.exc ? ", ECCEZIONE durante la ricerca" : "", b.p, b.size, b.found ? "" : " (nessuno: la ricerca si e' fermata prima)");
    if (b.found) Log("HEAP:   inizio del blocco rovinato: %s", hex(b.head, 32).c_str());
    Log("HEAP:   blocco prima: %p (%lu byte), primo dword %08lX%s, inizio %s", b.prev, b.prevSize, pv,
        (pv >= 0x00401000 && pv < 0x00a00000) ? " (indirizzo di Sacred.exe: vtable?)" : "", hex(b.prevHead, 16).c_str());
    Log("HEAP:   ultimi 16 byte del blocco prima e 32 dopo la sua fine: %s | %s", hex(b.prevEnd, 16).c_str(), hex(b.prevEnd + 16, 32).c_str());
    if (!b.found) Log("HEAP:   HeapWalk fermata con l'errore %lu", b.err);
}
static void HeapCheckTick()
{
    if (!g_cfg.nativeHeapCheck) return;
    static DWORD next = 0, okLogAt = 0;
    static std::set<HANDLE> bad;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - next) < 0) return;
    next = now + 1000;                      // 0.56.11: ogni secondo (era 3 s), per stringere il momento
    HANDLE heaps[128];
    const DWORD n = GetProcessHeaps(128, heaps);
    int okCount = 0;
    const DWORD t0 = GetTickCount();
    for (DWORD i = 0; i < n && i < 128; ++i) {
        const int r = SafeHeapValidate(heaps[i]);
        if (r == 1) { ++okCount; continue; }
        if (r < 0) { Log("HEAP: eccezione controllando lo heap %p (%lu di %lu): saltato", heaps[i], i + 1, n); continue; }
        if (bad.insert(heaps[i]).second) {
            Log("HEAP: lo heap %p (%lu di %lu) NON e' piu' valido (memoria scritta oltre un blocco)", heaps[i], i + 1, n);
            HeapFindBad(heaps[i]);                  // 0.56.11
        }
    }
    if (static_cast<LONG>(now - okLogAt) >= 60000) {
        okLogAt = now;
        Log("HEAP: %d heap validi su %lu (controllo in %lu ms)", okCount, n, GetTickCount() - t0);
    }
}

void MemoryNativeProbe(HWND hwnd)
{
    HeapCheckTick();
    Mouse2MemoryWatch();                    // 0.56.4
    if (g_ok && g_cfg.memoryDiag) InstallCrashVeh();
    if (!g_cfg.nativeEnabled) return;   // 0.49.0 (N6): co-op nativa spenta, niente ganci ne' controlli
    if (g_ok && g_cfg.nativePortrait) InstallPortraitHook();
    if (g_ok && g_cfg.nativeHero2Taskbar) InstallTaskbarHook();   // 0.52.0: seconda barra dell'eroe 2
    if (g_ok && g_cfg.nativeTwoInventories && g_cfg.nativeSwapUi) InstallDualInventoryHooks();   // 0.55.48: due zaini insieme
    if (g_ok && g_cfg.nativeMouse2 && g_cfg.nativeTwoInventories && g_cfg.nativeSwapUi) {   // 0.56.0: secondo mouse, una volta
        static bool m2Asked = false;
        if (!m2Asked) m2Asked = RunOnGameThread([] { Mouse2InstallNow(); });
    }
    if (g_ok && g_cfg.nativeSwap && g_cfg.nativeSwapUi) {   // 0.51.0: deviazioni dello scambio della sola interfaccia, una volta
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallUiSwapNow(); });   // falso se il thread del motore non e' pronto: si riprova
    }
    if (g_ok && g_cfg.nativeSaveHero2) {   // 0.54.1: l'eroe 2 si salva insieme alla partita (gancio in 0x60e872), una volta
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallSaveHookNow(); });
    }
    if (g_ok && g_cfg.nativePad2 && g_cfg.nativePad2WalkToUse) {   // 0.55.12: modo 1 dell'eroe 2 non fermato (gancio in 0x610fb7)
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallPad2StopHookNow(); });
    }
    if (g_ok && g_cfg.nativePad2 && g_cfg.nativePad2NoCtrl) {   // 0.55.37: Ctrl del pad 1 fuori dal gestore del giocatore 2
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallPad2HandlerHookNow(); });
    }
    if (g_ok && g_cfg.nativeRevealHero2) {   // 0.55.40: anche l'eroe 2 scopre la mappa intorno a se'
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallRevealHookNow(); });
        DWORD mgr = 0; BYTE net = 0;
        Rd(kObjMgr, mgr); Rd(kNetGame, net);
        const DWORD mine = g_h2MineAddr.load();
        g_revealHero2 = (mgr && !net && mine && ObjectAddr(mgr, 2) == mine) ? mine : 0;
    }
    if (g_ok && g_cfg.nativeQuestItems) {   // 0.55.44: script "Take:" degli oggetti raccolti dall'eroe 2 eseguiti per l'eroe 1
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallTakeHookNow(); });
        DWORD mgr = 0, player = 0, heroId = 0; BYTE net = 0;
        Rd(kObjMgr, mgr); Rd(kPlayer, player); Rd(kNetGame, net);
        const DWORD mine = g_h2MineAddr.load();
        const bool ok2 = mgr && !net && mine && ObjectAddr(mgr, 2) == mine && player && RdHero(player, heroId) && heroId && heroId != 2;
        g_takeHero1 = ok2 ? ObjectAddr(mgr, heroId) : 0;
        g_takeHero2 = ok2 ? mine : 0;
    }
    if (g_ok && g_cfg.nativeRewardPoints) {   // 0.56.53: punti attributo e abilita' delle missioni anche all'eroe 2
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallRewardPointsHookNow(); });
        RewardPointsTick();
    }
    if (g_ok && g_cfg.nativeKeepMap) {   // 0.55.39: la mappa scoperta non si perde quando compare l'eroe 2
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallExploredHookNow(); });
    }
    if (g_ok && g_cfg.nativeSharedLoot) {   // 0.55.39: bottino diviso fra le classi dei due eroi (getRandomItem)
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallLootHookNow(); });
    }
    if (g_ok && g_cfg.nativeShareHeal) {    // 0.56.16: cure delle arti per tutti e due gli eroi
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallHealHookNow(); });
    }
    if (g_ok && (g_cfg.nativeRewardRunes || g_cfg.nativeChestRunes)) RewardRuneTick(GetTickCount());   // 0.55.47: rune delle ricompense anche all'eroe 2 (0.56.45: e gemelle dei forzieri)
    if (g_ok && g_cfg.nativeQuestItems) {   // 0.55.38: oggetti delle missioni dell'eroe 2 all'eroe 1, ogni mezzo secondo
        static DWORD questAt = 0;
        const DWORD qn = GetTickCount();
        if (!questAt || static_cast<LONG>(qn - questAt) >= 500) {   // 0.55.46: anche dopo 24,8 giorni di accensione
            questAt = qn ? qn : 1;
            DWORD inv = 0, mgr = 0, b = 0, e = 0;
            if (Rd(kInvTable, inv) && inv && Rd(kObjMgr, mgr) && mgr && ObjectAddr(mgr, 2) && Rd(inv + 2 * kInvSize, b) &&
                Rd(inv + 2 * kInvSize + 4, e) && b && e > b && !g_hero2Saving.load())
                RunOnGameThread([] { QuestItemsToHero1Now(); });
        }
    }
    if (g_ok && g_cfg.nativeAudioBoth) {   // 0.55.13: con due eroi i suoni si misurano dal segmento fra gli eroi
        static bool asked = false;
        if (!asked) asked = RunOnGameThread([] { InstallAudioHooksNow(); });
    }
    if (g_ok) {   // fase 5: creazione automatica dell'eroe 2 e camera sul punto medio
        static DWORD worldSince = 0, lostSince = 0;
        static bool autoTried = false;
        const DWORD now = GetTickCount();
        DWORD mgr = 0, player = 0, heroId = 0, engine = 0;
        Rd(kObjMgr, mgr); Rd(kPlayer, player); Rd(kEngine, engine);
        if (player) RdHero(player, heroId);
        View vw;
        const bool inWorld = ReadView(vw) && mgr && heroId && ObjectAddr(mgr, heroId) && InWorld(ObjectAddr(mgr, heroId));
        NativeCoopMenu(hwnd, inWorld);
        NativePad2Tick(mgr, inWorld, now);
        if (inWorld) TrigNearRefresh(now);   // 0.56.53: trigger vicini per A e il portale automatico
        if (g_cfg.memoryDiag && inWorld) { DiagHoverFixed(now); DiagTriggers(now); }
        DialogWatchdog(mgr, heroId, inWorld, now);
        Hero2SaveWatch(inWorld, mgr && ObjectAddr(mgr, 2), now);
        {   // 0.45.5: copia dell'anti-trucchi riallineata all'eroe locale ogni 2 s mentre c'e' l'eroe 2
            static DWORD lastSync = 0;
            if (inWorld && player && mgr && ObjectAddr(mgr, 2) && !g_saveOrig.load() && static_cast<int>(now - lastSync) >= 2000) {
                lastSync = now;
                RunOnGameThread([=] {
                    DWORD m = 0, pl = 0, loc = 0; Rd(kObjMgr, m); Rd(kPlayer, pl);
                    if (!m || !pl || !Rd(pl + 0x14, loc) || !loc) return;
                    SyncAntiCheat(ObjectAddr(m, loc), nullptr);
                });
            }
        }
        CompanionRegisterTick(mgr, heroId, inWorld, now);   // 0.56.37: registro dei compagni (solo lettura)
        if (inWorld) CompPanelTick(mgr, heroId, now);        // 0.56.40: righe della scheda Compagni
        {   // livello e vita dei due eroi; l'eroe 2 morto si rialza (0.37.0)
            static WORD lv1 = 0, lv2 = 0; static DWORD hp2 = 0xFFFFFFFF, hp1 = 0xFFFFFFFF, lastCheck = 0, deadSince = 0, h1Back = 0;
            if (inWorld && static_cast<int>(now - lastCheck) >= 500) {
                lastCheck = now;
                const DWORD h1 = ObjectAddr(mgr, heroId), h2 = ObjectAddr(mgr, 2);
                WORD l1 = 0, l2 = 0; DWORD v1 = 0, v2 = 0;
                if (h1) { Rd(h1 + 0x3FE, l1); Rd(h1 + 0x4d8, v1); }
                if (h2) { Rd(h2 + 0x3FE, l2); Rd(h2 + 0x4d8, v2); }
                if (l1 && l1 != lv1) { Log("EROI: livello eroe 1 %u -> %u", lv1, l1); lv1 = l1; }
                if (l2 && l2 != lv2) { Log("EROI: livello eroe 2 %u -> %u", lv2, l2); lv2 = l2; }
                if (h2 && v2 != hp2) {
                    if ((hp2 == 0xFFFFFFFF) || (hp2 != 0 && v2 == 0) || (hp2 == 0 && v2 != 0)) { DWORD st = 0, md = 0; Rd(h2 + 0x150, st); Rd(h2 + 0x0fc, md); Log("EROI: vita eroe 2 %lu -> %lu (stato %04lX modo %04lX)", hp2 == 0xFFFFFFFF ? 0 : hp2, v2, st, md); }
                    if (v2 == 0 && hp2 != 0xFFFFFFFF && hp2 != 0) deadSince = now;
                    if (v2 != 0) deadSince = 0;
                    hp2 = v2;
                }
                if (h1 && v1 != hp1) {
                    if (hp1 == 0 && v1 != 0 && hp1 != 0xFFFFFFFF) h1Back = now;     // l'eroe 1 e' tornato in vita (punto di salvataggio)
                    hp1 = v1;
                }
                if (!h2) { hp2 = 0xFFFFFFFF; deadSince = 0; }
                {   // ricerca del campo dell'esperienza (0.37.2/3): cambi nella zona 0x3a0..0x440 dei due eroi, senza i campi rumorosi
                    static DWORD snap[2][0x30] = {}, snapAddr[2] = {}; static bool have[2] = {}; static int logged[2] = {};
                    const DWORD hs[2] = { h1, h2 };
                    for (int k = 0; k < 2; ++k) {
                        if (!hs[k]) { have[k] = false; continue; }
                        // 0.41.2: oggetto dell'eroe cambiato (altra partita caricata): il confronto con lo snapshot del vecchio
                        // eroe regalava all'eroe 2 la differenza di esperienza fra i due eroi 1
                        if (hs[k] != snapAddr[k]) { snapAddr[k] = hs[k]; have[k] = false; }
                        DWORD cur[0x30];
                        if (!RdBuf(hs[k] + 0x3a0, cur, sizeof(cur))) continue;
                        if (have[k] && g_cfg.nativeVerbose) for (int i = 0; i < 0x30 && logged[k] < 400; ++i) {
                            const DWORD off = 0x3a0 + i * 4;
                            if (off >= 0x408 && off <= 0x414) continue;       // campi che cambiano di continuo (prova 0.37.2)
                            if (cur[i] != snap[k][i]) { Log("STAT: eroe %d +0x%03lX %08lX -> %08lX", k + 1, off, snap[k][i], cur[i]); ++logged[k]; }
                        }
                        // esperienza condivisa (prova): l'aumento di +0x3B4 dell'eroe 1 si somma all'eroe 2.
                        // 0.42.0 (N10 della sezione 8c): solo se l'eroe 2 e' vivo e, con ShareXpRange > 0, abbastanza vicino
                        // all'eroe 1 (come i giocatori vicini del multigiocatore); altrimenti l'aumento va perso.
                        bool xpNear = v2 != 0;
                        if (xpNear && g_cfg.nativeXpRange > 0 && h2) {
                            int ax = 0, ay = 0, bx = 0, by = 0; BYTE az = 0, bz = 0;
                            Rd(h1 + 0x1c, ax); Rd(h1 + 0x20, ay); Rd(h1 + 0x24, az);
                            Rd(h2 + 0x1c, bx); Rd(h2 + 0x20, by); Rd(h2 + 0x24, bz);
                            const double dx = ax - bx, dy = ay - by;
                            xpNear = az == bz && dx * dx + dy * dy <= static_cast<double>(g_cfg.nativeXpRange) * g_cfg.nativeXpRange;
                        }
                        if (k == 0 && have[0] && g_cfg.nativeShareXp && h2) {
                            const DWORD oldX = snap[0][(0x3b4 - 0x3a0) / 4], newX = cur[(0x3b4 - 0x3a0) / 4];
                            if (newX > oldX && newX - oldX < 100000 && !xpNear) {
                                static DWORD lostLog = 0;
                                if (static_cast<int>(now - lostLog) > 5000) { lostLog = now; Log("EROI: esperienza +%lu non condivisa: eroe 2 %s", newX - oldX, v2 ? "lontano" : "a terra"); }
                            } else if (newX > oldX && newX - oldX < 100000) {
                                const DWORD gain = newX - oldX, add = gain * static_cast<DWORD>(g_cfg.nativeXpFactor);
                                // 0.45.3: con l'interfaccia sull'eroe 2 (Home) l'eroe 2 e' l'eroe "locale" e l'anti-trucchi del gioco
                                // confronta la sua esperienza con la copia protetta dell'eroe 1: niente scritture, si tiene da parte
                                // 0.51.0: con lo scambio della sola interfaccia l'eroe locale resta l'1, non serve tenerla da parte
                                if ((g_swapOrig.load() && !g_swapUi.load()) || g_saveOrig.load()) {
                                    g_xpPending += add;
                                    Log("EROI: esperienza +%lu per l'eroe 2 tenuta da parte (interfaccia sull'eroe 2): %lu in attesa", add, g_xpPending.load());
                                } else
                                // 0.41.2: lettura e scrittura nel thread del motore, come gli altri lavori sull'eroe 2
                                RunOnGameThread([=] { AddExperienceHero2(add); });     // 0.45.6: funzione del gioco (punti compresi)
                                (void)gain;
                            }
                        }
                        // 0.56.31: l'aumento dell'esperienza dell'eroe 1 anche ai compagni dei ritratti (CompanionXp), come all'eroe 2
                        if (k == 0 && have[0] && g_cfg.nativeCompanionXp) {
                            const DWORD oldX = snap[0][(0x3b4 - 0x3a0) / 4], newX = cur[(0x3b4 - 0x3a0) / 4];
                            if (newX > oldX && newX - oldX < 100000 && !g_saveOrig.load()) {
                                const DWORD add = (newX - oldX) * static_cast<DWORD>(g_cfg.nativeXpFactor);
                                RunOnGameThread([=] { AddExperienceCompanions(add); });
                            }
                        }
                        memcpy(snap[k], cur, sizeof(cur)); have[k] = true;
                    }
                }
                if (g_xpPending && h2 && (!g_swapOrig.load() || g_swapUi.load()) && !g_saveOrig.load()) {   // 0.45.3: esperienza tenuta da parte durante lo scambio
                    const DWORD add = g_xpPending.exchange(0);
                    RunOnGameThread([=] { AddExperienceHero2(add); });
                }
                if (h2 && v2 == 0 && deadSince && g_cfg.nativeRevive > 0) {
                    if (h1Back && static_cast<int>(now - h1Back) > 1500 && h1Back >= deadSince - 1000) { h1Back = 0; deadSince = now + 5000; ReviveHero2("l'eroe 1 e' tornato in vita"); }
                    else if (static_cast<int>(now - deadSince) > g_cfg.nativeRevive * 1000) { deadSince = now + 5000; ReviveHero2("tempo scaduto"); }
                }
            }
        }
        {   // 0.42.0 (N9 e N15 della sezione 8c): rialzo vicino al compagno e barra dell'eroe 2, ogni ~100 ms
            static DWORD lastBar = 0, nearSince = 0, revivedAt = 0, warnedFor = 0, nameFor = 0, near1Since = 0, revived1At = 0, near1Toast = 0;
            static std::string name2;
            if (static_cast<int>(now - lastBar) >= 100) {
                lastBar = now;
                const DWORD h1 = inWorld ? ObjectAddr(mgr, heroId) : 0, h2 = inWorld ? ObjectAddr(mgr, 2) : 0;
                if (!h1 || !h2 || !InWorld(h2)) { nearSince = 0; OverlayHero2(false, nullptr, 0, 0, 0, -1); }
                else {
                    if (h2 != nameFor) {      // nome dal file dell'eroe 2 scelto (il nome sopra la testa e' ancora quello dell'eroe 1)
                        nameFor = h2;
                        char path[MAX_PATH];
                        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\Save\\Hero%02d.pax", GameDir(), g_cfg.nativeHero2);
                        if (!ReadHeroName(path, name2)) name2 = Tr("Eroe 2");
                    }
                    // 0.45.9/0.46.0 (N5): il nome di una creatura e' il testo numero +0x3c (0x44a570). `sys loadplayer` lascia
                    // all'eroe 2 il numero dell'eroe 1 (0x2700), quindi scritte di Alt, statistiche e inventario dopo Home
                    // dicevano "Elfo oscuro". 0.46.0: +0x3c = kNameId2, che il gancio su 0x673300 traduce nel nome del file
                    // dell'eroe 2; senza gancio (0.45.9) +0x3c = 0 e il gioco usa il nome della classe.
                    if (g_cfg.nativeHero2Name) {
                        static DWORD nameLogFor = 0, nameTextFor = 0, nameLogWant = 0xFFFFFFFF;
                        if (nameTextFor != h2 && InstallNameHook()) {
                            wchar_t w[64] = {};
                            MultiByteToWideChar(CP_ACP, 0, name2.c_str(), -1, w, 63);
                            const std::wstring wn(w);
                            g_name2Ready.store(false);
                            // nel thread del motore: risorsa del testo 0x2700 come modello (0.46.1), poi il nome
                            if (RunOnGameThread([wn] {
                                    DWORD mgr = 0, res = 0;
                                    Rd(0x017e6ee8, mgr);               // gestore dei testi (0x672c50)
                                    if (mgr) {
                                        const CallResult cr = CallThis1(kGetText, mgr, 0x2700);
                                        if (cr.ok && cr.ret && cr.ret != mgr + 0xc) res = cr.ret;
                                    }
                                    DWORD tpl[14] = {};
                                    if (res && RdBuf(res, tpl, sizeof(tpl))) memcpy(g_name2Str.rest, tpl + 3, sizeof(g_name2Str.rest));
                                    const size_t n = (std::min)(wn.size(), static_cast<size_t>(63));
                                    memcpy(g_name2W, wn.c_str(), n * sizeof(wchar_t));
                                    g_name2W[n] = 0;
                                    g_name2Str.end = g_name2W + n;
                                    g_name2Ready.store(true);
                                    Log("NATIVO: nome dell'eroe 2: testo pronto (%u caratteri), modello dalla risorsa %08lX", static_cast<unsigned>(n), res);
                                })) nameTextFor = h2;
                        }
                        DWORD n1 = 0, n2 = 0; Rd(h1 + 0x3c, n1); Rd(h2 + 0x3c, n2);
                        const bool hook = g_nameHookOn.load() && nameTextFor == h2 && g_name2Ready.load();
                        const DWORD want = hook ? kNameId2 : 0;
                        if (n2 != want && (hook || n2 == n1)) {
                            if (nameLogFor != h2 || nameLogWant != want) {
                                nameLogFor = h2; nameLogWant = want;
                                Log("EROI: nome dell'eroe 2: testo %08lX (eroe 1 %08lX) -> %08lX (%s)", n2, n1, want, hook ? name2.c_str() : "nome della classe");
                            }
                            WriteDword(h2 + 0x3c, want);
                        } else if (nameLogFor != h2) { nameLogFor = h2; Log("EROI: nome dell'eroe 2: testo %08lX, eroe 1 %08lX (lasciato com'e')", n2, n1); }
                    }
                    int hp1 = 0, hp2v = 0, max2 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0; WORD l1 = 0, l2 = 0; BYTE z1 = 0, z2 = 0;
                    Rd(h1 + 0x4d8, hp1); Rd(h2 + 0x4d8, hp2v); Rd(h2 + 0x4d4, max2); Rd(h1 + 0x3FE, l1); Rd(h2 + 0x3FE, l2);
                    Rd(h1 + 0x1c, x1); Rd(h1 + 0x20, y1); Rd(h1 + 0x24, z1); Rd(h2 + 0x1c, x2); Rd(h2 + 0x20, y2); Rd(h2 + 0x24, z2);
                    if (h2 != warnedFor && l1 && l2) {     // N10: avviso una volta per eroe 2 se i livelli sono molto lontani
                        warnedFor = h2;
                        DWORD t1 = 0, t2 = 0; Rd(h1 + 0x10, t1); Rd(h2 + 0x10, t2);
                        Log("EROI: eroe 1 livello %u (tipo %lu), eroe 2 \"%s\" livello %u (tipo %lu)", l1, t1, name2.c_str(), l2, t2);
                        const int gap = static_cast<int>(l1) - static_cast<int>(l2);
                        if (gap > 20 || gap < -20) {
                            char t[160];
                            _snprintf_s(t, sizeof(t), _TRUNCATE, Tr("Attenzione: eroe 1 livello %u, eroe 2 livello %u"), l1, l2);
                            OverlayCoopToast(t, 6000);
                        }
                    }
                    int pct = -1;
                    const double dx = x1 - x2, dy = y1 - y2, d = sqrt(dx * dx + dy * dy);
                    const bool canNear = g_cfg.nativeReviveNear > 0 && hp2v == 0 && hp1 > 0 && z1 == z2 && d <= g_cfg.nativeReviveRange &&
                                         static_cast<int>(now - revivedAt) > 3000;
                    if (canNear) {
                        if (!nearSince) { nearSince = now; Log("EROI: l'eroe 1 e' vicino all'eroe 2 a terra (%.0f unita'): rialzo in corso", d); }
                        const int need = g_cfg.nativeReviveNear * 1000;
                        pct = static_cast<int>(static_cast<long long>(now - nearSince) * 100 / need);
                        if (pct >= 100) { pct = 100; nearSince = 0; revivedAt = now; ReviveHero2("rialzato dall'eroe 1"); }
                    } else if (nearSince) {
                        if (hp2v == 0) Log("EROI: rialzo interrotto (eroe 1 lontano o a terra)");
                        nearSince = 0;
                    }
                    // 0.56.16: e l'eroe 1 a terra con l'eroe 2 vivo vicino
                    const bool canNear1 = g_cfg.nativeReviveNear > 0 && hp1 == 0 && hp2v > 0 && z1 == z2 && d <= g_cfg.nativeReviveRange &&
                                          static_cast<int>(now - revived1At) > 3000 && h2 == g_h2MineAddr.load();
                    if (canNear1) {
                        if (!near1Since) {
                            near1Since = now;
                            Log("EROI: l'eroe 2 e' vicino all'eroe 1 a terra (%.0f unita'): rialzo in corso", d);
                        }
                        const int need = g_cfg.nativeReviveNear * 1000;
                        const int p1 = static_cast<int>(static_cast<long long>(now - near1Since) * 100 / need);
                        if (static_cast<int>(now - near1Toast) >= 500 && p1 < 100) {
                            near1Toast = now;
                            char t[120];
                            _snprintf_s(t, sizeof(t), _TRUNCATE, Tr("Il giocatore 2 rialza l'eroe 1: %d%%"), p1);
                            OverlayCoopToast(t, 700);
                        }
                        if (p1 >= 100) {
                            near1Since = 0; revived1At = now;
                            ReviveHero1("rialzato dall'eroe 2");
                            OverlayCoopToast(Tr("Eroe 1 rialzato dal compagno"), 2000);
                        }
                    } else if (near1Since) {
                        if (hp1 == 0) Log("EROI: rialzo dell'eroe 1 interrotto (eroe 2 lontano o a terra)");
                        near1Since = 0;
                    }
                    // con il ritratto del gioco attivo la barra serve solo a terra (avanzamento del rialzo)
                    const bool portrait = g_cfg.nativePortrait && static_cast<int>(now - g_portraitAt.load()) < 1000;
                    if (g_cfg.nativeHero2Bar && (!portrait || hp2v == 0)) OverlayHero2(true, name2.c_str(), l2, hp2v, max2, pct);
                    else OverlayHero2(false, nullptr, 0, 0, 0, -1);
                }
            }
        }
        if (inWorld) {
            lostSince = 0;
            HeroSwapTick(mgr, player, engine, true, now);
            if (!worldSince) worldSince = now;
            {   // nuova partita (prova del 27/10: caricando un salvataggio senza eroe 2 subito dopo uno con eroe 2 la
                // vista restava leggibile e l'eroe 2 non veniva ricreato): l'oggetto dell'eroe 1 cambia a ogni caricamento
                static DWORD lastHero1 = 0, missing2Since = 0;
                const DWORD h1 = ObjectAddr(mgr, heroId);
                if (h1 != lastHero1) {
                    if (lastHero1) Log("NATIVO: nuova partita rilevata (eroe 1 %08lX -> %08lX)", lastHero1, h1);
                    if (lastHero1 && g_m2) RunOnGameThread([] { Mouse2Forget("nuova partita"); });   // 0.56.3
                    if (lastHero1) RunOnGameThread([] { DwInstForget(); });                         // 0.56.14
                    lastHero1 = h1;
                    autoTried = false;
                    g_spawned.store(false);
                    g_ctl2.store(false);
                    worldSince = now;
                    missing2Since = 0;
                    if (!g_h2Spawning.load()) { g_h2MineAddr.store(0); g_h2MineFile.store(-1); }   // 0.54.0: l'eroe 2 di prima non vale piu'
                }
                {   // 0.54.0: oggetto 2 non creato dalla mod in questo mondo (arrivato con il salvataggio della partita, o dalla rete)
                    static DWORD foreignAddr = 0, foreignSince = 0;
                    static bool removeQueued = false;
                    const bool s1 = g_h2Spawning.load();
                    const DWORD raw = RawHero2(mgr);
                    const DWORD mine = g_h2MineAddr.load();
                    const bool busy = s1 || g_h2Spawning.load();
                    BYTE net = 0; Rd(kNetGame, net);
                    // Un oggetto 2 diverso da quello della mod non si "tiene" mai come suo, nemmeno nello stesso mondo: se il gioco
                    // lo ricreasse da se' (caso non visto) si toglie e si ricrea dal file; tenerlo per sbaglio salverebbe un altro
                    // eroe nel HeroNN.pax dell'eroe 2 della mod.
                    if (!raw || raw == mine || busy) {
                        foreignAddr = 0;
                        if (!raw && mine && !busy) g_h2MineAddr.store(0);   // cancellato dal gioco: l'indirizzo potrebbe tornare a un altro
                    } else if (net || heroId == 2) {
                        if (foreignAddr != raw) { foreignAddr = raw; Log("EROE2: oggetto 2 di una partita in rete (rete %u, eroe locale %lu): lasciato stare", net, heroId); }
                    } else if (!g_cfg.nativeCleanSaved) {
                        g_h2MineFile.store(-1);
                        g_h2MineAddr.store(raw);
                        Log("EROE2: eroe 2 del salvataggio (%08lX) tenuto (CleanSavedHero2=false), non si salvera' in nessun HeroNN.pax", raw);
                    } else {
                        if (foreignAddr != raw) {
                            foreignAddr = raw; foreignSince = now; removeQueued = false;
                            Log("EROE2: l'oggetto 2 (%08lX) e' arrivato con il salvataggio della partita: nascosto alla mod, tra poco si toglie", raw);
                        }
                        if (!removeQueued && static_cast<int>(now - foreignSince) >= 1500 && static_cast<int>(now - worldSince) >= 1500 &&
                            !g_swapOrig.load() && !g_hero2Saving.load() && engine) {
                            removeQueued = RunOnGameThread([=] { RemoveSavedHero2(raw); });
                        }
                    }
                }
                if (g_spawned.load() && !ObjectAddr(mgr, 2)) {      // l'eroe 2 e' sparito (area cambiata, morte e rimozione)
                    if (!missing2Since) missing2Since = now;
                    if (static_cast<int>(now - missing2Since) > 2500) { g_spawned.store(false); g_ctl2.store(false); autoTried = false; worldSince = now; missing2Since = 0; Log("NATIVO: l'eroe 2 non c'e' piu': si ricrea"); }
                } else missing2Since = 0;
            }
            // 0.55.33: creazione solo con l'eroe 1 fermo da 1 s (modo 0, stato a riposo) e senza finestre, dialoghi o menu, al
            // massimo 30 s dopo l'ingresso (poi basta niente finestre). Crash sulla Steam Machine del 31/10 alle 18:24:52, 1,2 s dopo
            // la creazione: l'eroe 1 stava andando ad aprire un forziere con il pad 1 (A 54 ms dopo, forziere aperto, oggetto caduto),
            // e la creazione lo sposta (il comando del gioco lo porta alla posizione del file dell'eroe 2, la mod lo rimette); il gioco
            // e' caduto nel disegno (0x62b310 -> 0x613ee0 su 0x182dd40 -> inserimento in un vettore da 24 byte, 0x633300) con il
            // vettore gia' rovinato. Il secondo crash (18:42:27, identico) e' avvenuto con l'eroe 1 fermo alla creazione: questa pista
            // e' caduta. In comune ai due: puntatore sulla runa di magia SPELL_UPGRADE_BURNINGLIGHT del forziere di partenza del
            // Serafino, crash circa 0,75 s dopo (vedi 'Secondo crash, con la 0.55.33' nel documento). L'attesa resta come prudenza.
            static DWORD calmSince = 0, calmLogAt = 0;
            bool calm = false;
            WORD md1 = 0; DWORD st1 = 0;
            if ((g_cfg.nativeAuto || g_coopArmed.load()) && !autoTried && !g_spawned.load() && static_cast<int>(now - worldSince) > 4000 && engine && !ObjectAddr(mgr, 2)) {
                const DWORD h1 = ObjectAddr(mgr, heroId);
                const bool idle = h1 && Rd(h1 + 0xfc, md1) && Rd(h1 + 0x150, st1) && md1 == 0 && (st1 & 0xffff) == 0;
                DialogInfo dd;
                const bool noUi = MemoryUIState() == 0 && !MemoryGameMenuOpen() && !MemoryDialog(dd);
                if (idle && noUi) { if (!calmSince) calmSince = now; }
                else calmSince = 0;
                calm = (calmSince && static_cast<int>(now - calmSince) >= 1000) || (noUi && static_cast<int>(now - worldSince) > 30000);
                if (!calm && static_cast<int>(now - calmLogAt) >= 5000) {
                    calmLogAt = now;
                    Log("NATIVO: creazione dell'eroe 2 in attesa: eroe 1 %s (modo %u, stato %08lX)%s", idle ? "fermo" : "in azione", md1, st1,
                        noUi ? "" : ", finestra o dialogo aperti");
                }
            }
            else calmSince = 0;
            if (calm) {
                autoTried = true;
                calmSince = 0;
                const int fileNo = g_cfg.nativeHero2;
                Log("NATIVO: creazione automatica dell'eroe 2 (hero%02d.pax), %.1f s dopo l'ingresso, eroe 1 modo %u stato %08lX", fileNo,
                    (now - worldSince) / 1000.0, md1, st1);
                RunOnGameThread([=] { SpawnHero2(engine, fileNo); });
            }
        } else {
            if (g_swapOrig.load()) HeroSwapTick(mgr, player, engine, false, now);
            worldSince = 0;
            if (!lostSince) lostSince = now;
            if (static_cast<int>(now - lostSince) > 2500) {     // menu o caricamento: una nuova partita ricrea l'eroe 2
                if (g_spawned.load()) Log("NATIVO: partita lasciata, stato dell'eroe 2 azzerato");
                g_spawned.store(false);
                g_ctl2.store(false);
                autoTried = false;
            }
        }
        {   // server: una volta per partita, 6 s dopo l'ingresso
            static bool srvDone = false;
            if (!inWorld) srvDone = false;
            else if (!srvDone && worldSince && static_cast<int>(now - worldSince) > 6000 && (g_cfg.nativeVerbose || g_cfg.nativeServerProbe)) {
                srvDone = true;
                FindAndInjectServer(g_cfg.nativeServerProbe);
            }
        }
        // Eroe 2 gia' presente all'ingresso in partita (per esempio ripreso da un salvataggio fatto con l'eroe 2):
        // lo si registra con i suoi campi, per capire cosa salva e ricarica il gioco.
        {
            static bool loggedExisting = false;
            if (!inWorld) loggedExisting = false;
            else if (!loggedExisting && !g_spawned.load() && worldSince && static_cast<int>(now - worldSince) > 3500 && mgr) {
                const DWORD h2 = ObjectAddr(mgr, 2);
                if (h2) {
                    loggedExisting = true;
                    DWORD ty = 0, fl = 0, st = 0, hp = 0, ow = 0; int x = 0, y = 0;
                    Rd(h2 + 0x10, ty); Rd(h2 + 0x14, fl); Rd(h2 + 0x150, st); Rd(h2 + 0x4d8, hp); Rd(h2 + 0x1ec, ow);
                    Rd(h2 + 0x1c, x); Rd(h2 + 0x20, y);
                    Log("NATIVO: l'eroe 2 esiste gia' all'ingresso in partita (%08lX): tipo %lu flag %08lX stato %08lX vita %lu prop %lu pos (%d,%d)", h2, ty, fl, st, hp, ow, x, y);
                    g_spawned.store(true);
                    autoTried = true;
                }
            }
        }
        // Salvataggi: nuovi o modificati nella cartella Save (file GAMExx.PAK e HeroNN.pax).
        {
            static DWORD nextScan = 0;
            struct Seen { FILETIME t; DWORD size; };
            static std::map<std::string, Seen> seen;
            static bool first = true;
            if (g_cfg.nativeVerbose && static_cast<int>(now - nextScan) >= 0) {
                nextScan = now + 2000;
                char pat[MAX_PATH];
                _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s/Save/*.*", GameDir());
                WIN32_FIND_DATAA fd;
                HANDLE h = FindFirstFileA(pat, &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        auto it = seen.find(fd.cFileName);
                        if (it == seen.end() || CompareFileTime(&it->second.t, &fd.ftLastWriteTime) != 0 || it->second.size != fd.nFileSizeLow) {
                            if (!first) Log("NATIVO: SALVATAGGIO: %s %s (%lu byte)", fd.cFileName, it == seen.end() ? "creato" : "modificato", fd.nFileSizeLow);
                            seen[fd.cFileName] = { fd.ftLastWriteTime, fd.nFileSizeLow };
                        }
                    } while (FindNextFileA(h, &fd));
                    FindClose(h);
                }
                first = false;
            }
        }
        if (g_cfg.nativeMonsterWatch && inWorld && ObjectAddr(mgr, heroId)) {   // il nemico piu' vicino all'eroe 1: chi scrive il suo stato
            static DWORD nextScan = 0, until = 0, cooldown = 0;
            if (until && static_cast<int>(now - until) >= 0) { DisarmWatch(); until = 0; cooldown = now + 15000; }
            if (!until && static_cast<int>(now - cooldown) >= 0 && static_cast<int>(now - nextScan) >= 0) {
                nextScan = now + 700;
                std::vector<Obj> cr; Obj hero{}; bool haveHero = false;
                if (ReadCreatures(cr, hero, haveHero) && haveHero) {
                    DWORD best = 0, bestId = 0; long long bestD = 1LL << 60; int bestCls = 0;
                    for (const Obj& o : cr) {
                        int cls = 0; bool dead = false;
                        if (!ReadKind(o.addr, cls, dead) || dead || !InMask(g_cfg.hostileMask, cls) || o.id <= 16) continue;
                        const long long dx = o.x - hero.x, dy = o.y - hero.y, d = dx * dx + dy * dy;
                        if (d < bestD) { bestD = d; best = o.addr; bestId = o.id; bestCls = cls; }
                    }
                    if (best && bestD < 2200LL * 2200LL) {
                        DWORD ty = 0; Rd(best + 0x10, ty);
                        Log("NATIVO: nemico sorvegliato: id %lu tipo %lu classe %d a %d unita' dall'eroe 1", bestId, ty, bestCls, static_cast<int>(std::sqrt(static_cast<double>(bestD))));
                        ArmWatch(best, bestId);
                        until = now + 40000;
                    }
                }
            }
        }
        if (g_cfg.nativePlayers) {
            const DWORD h2n = mgr ? ObjectAddr(mgr, 2) : 0;
            PlayerListSync(inWorld && h2n && InWorld(h2n), heroId);
        }
        const DWORD h2cam = (inWorld && mgr) ? ObjectAddr(mgr, 2) : 0;
        // 0.56.45 (prova del 05/11 alle 11:52:01: "la barra del giocatore 2 e' scomparsa e quella del giocatore 1 e' andata nel mezzo,
        // poi e' tornata"): l'eroe 2 e' uscito dal mondo (+0x14 bit 1) per un attimo, con lo stesso oggetto; la camera si spegneva e
        // si riaccendeva solo 2 s dopo (Hero2Settled), e con lei le barre ai lati. Ora, se l'oggetto resta lo stesso, fino a 1,5 s
        // fuori dal mondo la camera resta accesa (CameraShift non sposta nulla finche' e' fuori) e l'eroe 2 resta "assestato".
        static DWORD h2OutAt = 0;
        const bool h2In = h2cam && InWorld(h2cam);
        if (h2In || !h2cam) h2OutAt = 0;
        else if (!h2OutAt) h2OutAt = now ? now : 1;
        const bool h2Brief = h2cam && !h2In && h2OutAt && now - h2OutAt < 1500 && g_camOn.load();
        const bool settled = Hero2Settled((h2In || h2Brief) ? h2cam : 0, 2000);   // 0.53.5: 2 s dopo la comparsa
        const bool wantCam = g_cfg.nativeCamera && inWorld && h2cam && (h2In || h2Brief) && (settled || g_camOn.load());
        if (wantCam != g_camOn.load() && g_engineTid.load()) {
            g_camOn.store(wantCam);
            WatchSlot cur[4] = {};
            for (int i = 0; i < 3; ++i) cur[i] = g_watch[i];
            const bool ok = SetThreadWatch(g_engineTid.load(), cur);
            Log("NATIVO: camera sul punto medio %s (%s)", wantCam ? "attivata" : "disattivata", ok ? "ok" : "registri di debug non impostati");
        }
    }
    {   // Insert: crea il secondo eroe locale con il comando di debug del gioco (0.31.0)
        static bool prevIns = false;
        const bool dn = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        const bool e = dn && !prevIns;
        prevIns = dn;
        if (g_ok && g_cfg.memoryDiag && e) {
            const bool sh = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ct = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool al = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            DWORD engine = 0, ctl = 0;
            Rd(kEngine, engine);
            if (engine) Rd(engine + 0x13c, ctl);
            if (ct || al) {                // Ctrl+Insert: i comandi vanno all'eroe 2; Alt+Insert: tornano all'eroe 1
                const DWORD to = ct ? 2 : 1;
                RunOnGameThread([=] {
                    DWORD mgr = 0, player = 0, heroId = 0;
                    Rd(kObjMgr, mgr); Rd(kPlayer, player);
                    if (player) RdHero(player, heroId);
                    const DWORD target = to == 2 ? 2 : heroId;
                    const DWORD addr = mgr ? ObjectAddr(mgr, target) : 0;
                    if (!engine || !addr || !InWorld(addr)) {
                        Log("NATIVO: cambio di comando verso %lu annullato: l'oggetto non c'e' o e' fuori dal mondo", target);
                        return;
                    }
                    bool w = WriteDword(engine + 0x13c, target);
                    g_ctl2.store(to == 2 && w);
                    Log("NATIVO: [engine+0x13c] %lu -> %lu (%s)", ctl, target, w ? "ok" : "fallita");
                });
            } else {
                int fileNo = sh ? 6 : 7;
                for (int d = 0; d <= 9; ++d)   // Insert + una cifra (riga alta o tastierino) tenuta: file HeroNN
                    if ((GetAsyncKeyState('0' + d) & 0x8000) || (GetAsyncKeyState(VK_NUMPAD0 + d) & 0x8000)) fileNo = d;
                bool q = engine && RunOnGameThread([=] { SpawnHero2(engine, fileNo); });
                if (!q) Log("NATIVO: SpawnHero2 non accodata");
            }
        }
    }
    static bool prev = false;
    static DWORD trackAddr = 0, trackId = 0, trackUntil = 0, trackNext = 0;
    if (!g_ok || !g_cfg.memoryDiag) return;
    if (g_spawned.load() && (g_cfg.nativeVerbose || g_cfg.nativeDiagWatch)) {   // traccia dell'oggetto comandato ([engine+0x13c]): solo i dword che cambiano
        static DWORD trId = 0, trNext = 0, watchUntil = 0;
        static BYTE trPrev[0x400];
        static int trLines = 0;
        const DWORD now = GetTickCount();
        if (watchUntil && static_cast<int>(now - watchUntil) >= 0) { DisarmWatch(); watchUntil = 0; }
        if (static_cast<int>(now - trNext) >= 0 && trLines < 6000) {
            trNext = now + 250;
            DWORD mgr = 0, engine = 0, ctlId = 0;
            Rd(kObjMgr, mgr); Rd(kEngine, engine);
            if (engine) Rd(engine + 0x13c, ctlId);
            const DWORD addr = (mgr && ctlId) ? ObjectAddr(mgr, ctlId) : 0;
            BYTE cur[0x400];
            if (addr && RdBuf(addr, cur, sizeof(cur))) {
                if (ctlId != trId) {
                    Log("NATIVO: traccia: ora si segue l'oggetto %lu (%08lX)", ctlId, addr);
                    memcpy(trPrev, cur, sizeof(cur));
                    trId = ctlId;
                    if (g_cfg.nativeDiagWatch) { ArmWatch(addr, ctlId); watchUntil = GetTickCount() + 25000; }
                } else {
                    int n = 0;
                    char line[400]; line[0] = 0;
                    for (DWORD off = 0; off < sizeof(cur); off += 4) {
                        const DWORD x = *reinterpret_cast<DWORD*>(trPrev + off), y = *reinterpret_cast<DWORD*>(cur + off);
                        if (x == y) continue;
                        if (n < 30) {
                            char t[40]; _snprintf_s(t, sizeof(t), _TRUNCATE, " +%03lX:%lX>%lX", off, x, y);
                            strncat_s(line, t, _TRUNCATE);
                        }
                        ++n;
                    }
                    if (n) { Log("NATIVO: traccia %lu: %d dword cambiati:%s", ctlId, n, line); ++trLines; }
                    memcpy(trPrev, cur, sizeof(cur));
                }
            }
        }
    }
    if (trackAddr) {                       // segue la creatura che ha ricevuto l'ordine per 4 s
        DWORD now = GetTickCount();
        if (now >= trackNext) {
            trackNext = now + 400;
            DWORD tx = 0, ty = 0, st = 0;
            Rd(trackAddr + 0x1c, tx); Rd(trackAddr + 0x20, ty); Rd(trackAddr + 0x150, st);
            Log("NATIVO: seguo %lu pos (%d,%d) stato %08lX", trackId, static_cast<int>(tx), static_cast<int>(ty), st);
        }
        if (static_cast<int>(now - trackUntil) > 0) trackAddr = 0;
    }
    bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    std::vector<Obj> cr;
    Obj hero{};
    bool haveHero = false;
    if (!ReadCreatures(cr, hero, haveHero) || !haveHero) { Log("NATIVO: eroe non leggibile"); return; }
    DWORD engine = 0, player = 0, ctl = 0, heroId = 0;
    Rd(kEngine, engine); Rd(kPlayer, player);
    if (engine) Rd(engine + 0x13c, ctl);
    if (player) RdHero(player, heroId);

    // il compagno: la prima creatura viva che e' IsFollower
    DWORD follower = 0; Obj fo{};
    for (const Obj& o : cr) {
        int cls = 0; bool dead = false;
        if (ReadKind(o.addr, cls, dead) && !dead && IsFollower(o.addr)) { follower = o.addr; fo = o; break; }
    }

    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (alt) {                             // Alt+F11: ordine all'eroe remoto (altro giocatore), se c'e'
        DWORD target = 0; Obj to{};
        for (const Obj& o : cr) {
            DWORD ty = 0; Rd(o.addr + 0x1f0, ty);
            if (o.id >= 1 && o.id <= 16 && ty == 1) { target = o.addr; to = o; break; }
        }
        if (!target) Log("NATIVO: nessun eroe remoto (id 1..16, tipo 1) nel mondo");
        else {
            bool ok = SendMoveOrderMT(target, to.id, to.x + (shift ? -600 : 600), to.y, to.z, !ctrl);   // Alt+F11: modo 4 (Ctrl forzato); Ctrl+Alt+F11: modo 2 (clic normale)
            Log("NATIVO: ordine di movimento all'eroe REMOTO %lu (%08lX) da (%d,%d) a est/ovest: %s", to.id, to.addr,
                to.x, to.y, ok ? "consegnato" : "FALLITO");
            trackAddr = to.addr; trackId = to.id; trackUntil = GetTickCount() + 4000; trackNext = 0;
        }
    } else if (shift && !ctrl) {
        bool ok = SendMoveOrderMT(hero.addr, hero.id, hero.x + 600, hero.y, hero.z);
        Log("NATIVO: ordine di movimento all'eroe %08lX da (%d,%d) a (%d,%d): %s", hero.addr, hero.x, hero.y,
            hero.x + 600, hero.y, ok ? "consegnato" : "FALLITO");
        trackAddr = hero.addr; trackId = hero.id; trackUntil = GetTickCount() + 4000; trackNext = 0;
    } else if (ctrl && !shift) {
        if (!follower) { Log("NATIVO: nessun compagno al seguito"); }
        else {
            bool ok = SendMoveOrderMT(follower, fo.id, hero.x + 600, hero.y + 300, fo.z);
            Log("NATIVO: ordine di movimento al compagno %lu (%08lX) da (%d,%d) a (%d,%d): %s", fo.id, fo.addr,
                fo.x, fo.y, hero.x + 600, hero.y + 300, ok ? "consegnato" : "FALLITO");
            trackAddr = follower; trackId = fo.id; trackUntil = GetTickCount() + 4000; trackNext = 0;
        }
    } else if (ctrl && shift) {
        if (!follower || !engine) { Log("NATIVO: nessun compagno al seguito"); }
        else {
            if (!WriteDword(engine + 0x13c, fo.id)) Log("NATIVO: scrittura su engine+0x13c fallita");
            Log("NATIVO: [engine+0x13c] = %lu (compagno), prima %lu", fo.id, ctl);
        }
    }

    // confronto fra l'eroe locale e l'eroe remoto (replica di rete): campi diversi nei primi 0x600 byte
    if (alt && shift && ctrl == false) { /* Maiusc+Alt+F11 resta l'ovest */ }
    {
        static bool diffDone = false;
        const bool wantDiff = (GetKeyState(VK_CAPITAL) & 1) != 0;   // Bloc Maiusc acceso: attiva il confronto
        for (const Obj& o : cr) {
            DWORD ty = 0; Rd(o.addr + 0x1f0, ty);
            if (!(wantDiff && o.id >= 1 && o.id <= 16 && ty == 1)) continue;
            BYTE a[0x600], b[0x600];
            if (!RdBuf(hero.addr, a, sizeof(a)) || !RdBuf(o.addr, b, sizeof(b))) break;
            Log("NATIVO: confronto eroe locale %lu (%08lX) / eroe %lu (%08lX): dword diversi", hero.id, hero.addr, o.id, o.addr);
            int n = 0;
            for (DWORD off = 0; off < sizeof(a) && n < 140; off += 4) {
                DWORD x = *reinterpret_cast<DWORD*>(a + off), y = *reinterpret_cast<DWORD*>(b + off);
                if (x != y) { Log("NATIVO:   +0x%03lX  locale %08lX  remoto %08lX", off, x, y); ++n; }
            }
            diffDone = true;
            break;
        }
        (void)diffDone;
    }
    // registro (sempre)
    DWORD own = 0, typ = 0, fl = 0, st = 0;
    Rd(hero.addr + 0x1ec, own); Rd(hero.addr + 0x1f0, typ); Rd(hero.addr + 0x14, fl); Rd(hero.addr + 0x150, st);
    Log("NATIVO: giocatore %08lX [+0x14]=%lu, engine %08lX [+0x13c]=%lu; eroe %08lX id %lu pos (%d,%d,%u) "
        "prop %lu tipo %lu flag %08lX stato %08lX", player, heroId, engine, ctl, hero.addr, hero.id, hero.x, hero.y,
        hero.z, own, typ, fl, st);
    int n = 0;
    for (const Obj& o : cr) {
        DWORD ow = 0, ty = 0, f2 = 0;
        BYTE b39 = 0; Rd(o.addr + 0x39, b39);
        Rd(o.addr + 0x1ec, ow); Rd(o.addr + 0x1f0, ty); Rd(o.addr + 0x14, f2);
        bool interesting = (o.vt == kVtHero) || ty == 1 || (ow != 0) || IsFollower(o.addr);
        if (!interesting || n >= 24) continue;
        ++n;
        Log("NATIVO:   creatura %lu %08lX vt %08lX pos (%d,%d,%u) prop %lu [+0x39]=%u tipo %lu flag %08lX%s", o.id, o.addr, o.vt,
            o.x, o.y, o.z, ow, b39, ty, f2, IsFollower(o.addr) ? " [compagno]" : "");
    }
}

// Chiamata da OverlayEndScene dopo l'EndScene vero, nel thread del motore.
// Aggro verso l'eroe 2 (0.35.0). La sorveglianza hardware del 27/10 (0.34.3, log SacredPad_0343.log) mostra che i
// nemici sono simulati nello stesso processo, da un solo thread, e che la loro IA ("FUN_00542f20", "FUN_0054ee30")
// parte solo per le creature con +0x39 uguale all'id del giocatore locale (354 funzioni leggono [player+0x14])
// e valuta SOLO l'eroe locale (FUN_005fe550([player+0x14]) con le condizioni su +0xfc, +0x150, +0x4d8): non esiste
// nessun elenco di giocatori da estendere. Quando il nemico nota l'eroe 1 scrive `+0xfc` = 4 ("vai verso/attacca
// l'oggetto"), `+0x104` = id del bersaglio, `+0xfe` = 0 e accende `+0x1f4` bit 0. Qui si fa lo stesso per l'eroe 2:
// ogni ~60 ms, nel thread del motore e fra un fotogramma e l'altro, i nemici ostili e fermi (stato e modo 0,
// senza bersaglio) piu' vicini all'eroe 2 che all'eroe 1 ed entro il raggio di percezione vengono mandati
// verso l'eroe 2. Poi il passo normale della creatura (inseguimento e attacco) fa il resto.
bool WriteWord(DWORD addr, WORD v)
{
    __try { *reinterpret_cast<WORD*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void NativeAggroTick()
{
    static DWORD lastTick = 0;
    static int logged = 0;
    const DWORD now = GetTickCount();
    if (static_cast<int>(now - lastTick) < 60) return;
    lastTick = now;
    DWORD mgr = 0, player = 0, heroId = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player);
    if (!mgr || !player || !RdHero(player, heroId) || !heroId) return;
    const DWORD a1 = ObjectAddr(mgr, heroId), a2 = ObjectAddr(mgr, 2);
    if (!a1 || !a2 || !InWorld(a1) || !InWorld(a2)) return;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0; BYTE z1 = 0, z2 = 0;
    if (!Rd(a1 + 0x1c, x1) || !Rd(a1 + 0x20, y1) || !Rd(a2 + 0x1c, x2) || !Rd(a2 + 0x20, y2) || !Rd(a1 + 0x24, z1) || !Rd(a2 + 0x24, z2)) return;
    std::vector<Obj> cr; Obj hero{}; bool haveHero = false;
    if (!ReadCreatures(cr, hero, haveHero)) return;
    const long long R = g_cfg.nativeAggroRange;
    for (const Obj& o : cr) {
        if (o.id <= 16 || o.z != z2) continue;
        const long long dx2 = o.x - x2, dy2 = o.y - y2, d2 = dx2 * dx2 + dy2 * dy2;
        if (d2 > R * R) continue;
        const long long dx1 = o.x - x1, dy1 = o.y - y1, d1 = dx1 * dx1 + dy1 * dy1;
        // piu' vicino all'eroe 1: ci pensa l'IA, ma con i due eroi vicini (dopo il rialzo l'eroe 2 nasce accanto all'eroe 1,
        // prova del 27/10 con la 0.37.0) i nemici fermi con id dispari vanno comunque all'eroe 2: meta' ciascuno
        if (d1 <= d2 && (o.id & 1) == 0) continue;
        int cls = 0; bool dead = false;
        if (!ReadKind(o.addr, cls, dead) || dead || !InMask(g_cfg.hostileMask, cls) || IsFollower(o.addr)) continue;
        DWORD fl = 0, st = 0, md = 0, tg = 0, f4 = 0;
        if (!Rd(o.addr + 0x14, fl) || (fl & 2)) continue;         // fuori dal mondo
        Rd(o.addr + 0x150, st); Rd(o.addr + 0xfc, md); Rd(o.addr + 0x104, tg); Rd(o.addr + 0x1f4, f4);
        if ((st & 0xffff) != 0 || (md & 0xffff) != 0 || (tg != 0 && tg <= 16)) continue;   // gia' occupato o gia' su un eroe
        WriteWord(o.addr + 0xfe, 0);
        WriteDword(o.addr + 0x104, 2);
        WriteDword(o.addr + 0x1f4, f4 | 1);
        WriteWord(o.addr + 0xfc, 4);
        if (logged < 40) { ++logged; Log("NATIVO: aggro: il nemico %lu (classe %d) a %d unita' dall'eroe 2 e %d dall'eroe 1 va verso l'eroe 2", o.id, cls,
            static_cast<int>(std::sqrt(static_cast<double>(d2))), static_cast<int>(std::sqrt(static_cast<double>(d1)))); }
    }
}

// 0.53.2: le deviazioni dell'interfaccia seguono lo scambio chiesto da HeroSwapTick (g_swapUi), ma cambiano solo qui, fra un
// fotogramma e l'altro (vedi g_swapUiLive). L'indirizzo dell'eroe 2 si rilegge a ogni fotogramma: se non c'e' piu' (cambio
// di area, rimozione) le deviazioni si spengono invece di restituire un indirizzo vecchio.
void UiSwapLatch()
{
    if (!g_uiPatched.load()) return;
    const bool want = g_swapUi.load();
    DWORD h2 = 0;
    if (want) { DWORD mgr = 0; Rd(kObjMgr, mgr); h2 = mgr ? ObjectAddr(mgr, 2) : 0; }
    const bool was = g_swapUiLive.load();
    if (want && h2) { g_uiFakePlayer[0x14 / 4] = 2; g_uiHero = h2; g_uiOn = 1; }
    else { g_uiHero = 0; g_uiOn = 0; }
    g_swapUiLive.store(want && h2);
    if (was != (want && h2)) Log("SCAMBIO: finestre %s (thread del motore)", (want && h2) ? "sull'eroe 2" : "di nuovo sull'eroe locale");
}

// 0.55.41: oro condiviso (richiesta del tester del 31/10 sera: "voglio oro condiviso, non che prendi tutto x2 ma che entrambi gli
// eroi hanno lo stesso oro"). Studio: l'oro e' a +0x46 dell'oggetto delle statistiche della creatura (creatura+0x3a8), cioe' a
// creatura+0x3ee; il gioco lo cambia con 0x580030 (thiscall sulle statistiche, importo con segno; ret 4): con il limite
// 0x7ffffffe in alto e lo zero in basso; per l'eroe locale prima rifa' il controllo anti-trucchi 0x5909e0 (+0x3ee e' fra i campi
// della copia protetta: scriverlo a mano farebbe scattare "Cheater detected", vita a zero), poi suona le monete e aggiorna la
// copia protetta (VirtualProtect su [0x182dddc]). Quindi l'oro si cambia solo con 0x580030. Nel thread del motore, ogni 250 ms:
// alla comparsa dell'eroe 2 i due eroi partono dall'oro del piu' ricco (con il massimo, salvare e ricaricare non raddoppia
// niente); poi quello che uno guadagna o spende rispetto all'ultimo valore comune si da' o si toglie anche all'altro.
// Non durante lo scambio della 0.50 (eroe locale cambiato). [Native] SharedGold.
constexpr DWORD kAddGold = 0x00580030;
void SharedGoldTick()
{
    static DWORD nextAt = 0, lastH1 = 0, lastH2 = 0, lastSeq = 0;
    static int common = 0;
    static bool synced = false;
    const DWORD now = GetTickCount();
    if (nextAt && static_cast<LONG>(now - nextAt) < 0) return;   // 0.55.46: il primo giro anche dopo 24,8 giorni di accensione
    nextAt = now + 250;
    if (!nextAt) nextAt = 1;
    // 0.55.44: si riparte dal piu' ricco solo a una comparsa nuova dell'eroe 2. Prova del 01/11 alle 09:43: durante lo scambio
    // dell'interfaccia (mercante dell'eroe 2) il controllo si fermava e alla ripresa ripartiva dal massimo, cosi' le spese
    // dell'eroe 2 tornavano indietro ("e' come se avesse soldi infiniti": 6557 -> 2140 -> 6557, poi 683 -> 6557). Ora dopo una
    // pausa si riprende dalle differenze rispetto all'ultimo valore comune.
    const DWORD seq = g_h2SpawnSeq.load();
    if (seq != lastSeq) { lastSeq = seq; synced = false; }
    DWORD mgr = 0, player = 0, heroId = 0;
    BYTE net = 0;
    Rd(kObjMgr, mgr); Rd(kPlayer, player); Rd(kNetGame, net);
    const DWORD h2 = (g_cfg.nativeSharedGold && mgr && !net && !g_swapOrig.load() && !g_hero2Saving.load()) ? ObjectAddr(mgr, 2) : 0;
    const DWORD h1 = (h2 && player && RdHero(player, heroId) && heroId && heroId != 2) ? ObjectAddr(mgr, heroId) : 0;
    if (!h1 || !h2 || h2 != g_h2MineAddr.load()) return;   // pausa (scambio, salvataggio, menu): si riprende dalle differenze
    if (h1 != lastH1 || h2 != lastH2) { synced = false; lastH1 = h1; lastH2 = h2; }
    int g1 = 0, g2 = 0;
    if (!Rd(h1 + 0x3ee, g1) || !Rd(h2 + 0x3ee, g2) || g1 < 0 || g2 < 0) return;
    auto add = [&](DWORD hero, int amount) {
        const CallResult cr = CallThis1(kAddGold, hero + 0x3a8, static_cast<DWORD>(amount));
        if (!cr.ok) { LogException("oro condiviso"); return false; }
        if (!cr.balanced) Log("NATIVO: ATTENZIONE: 0x580030 ha lasciato lo stack sbilanciato (corretto)");
        return true;
    };
    if (!synced) {
        const int target = g1 > g2 ? g1 : g2;
        if (g1 < target && !add(h1, target - g1)) return;
        if (g2 < target && !add(h2, target - g2)) return;
        common = target;
        synced = true;
        Log("EROI: oro condiviso: eroe 1 %d, eroe 2 %d, si parte da %d", g1, g2, target);
        return;
    }
    const int d1 = g1 - common, d2 = g2 - common;
    if (!d1 && !d2) return;
    if (d1 && !add(h2, d1)) return;
    if (d2 && !add(h1, d2)) return;
    int n1 = 0, n2 = 0;
    Rd(h1 + 0x3ee, n1); Rd(h2 + 0x3ee, n2);
    common = n1;
    if (n1 != n2) { Log("EROI: oro condiviso: dopo il pareggio eroe 1 %d, eroe 2 %d (limite del gioco): si riparte", n1, n2); synced = false; }
}

void EngineThreadTick()
{
    {   // il thread che chiama EndScene e' quello del motore: serve alla camera anche senza lavori in coda
        const DWORD me = GetCurrentThreadId();
        if (g_engineTid.load() != me && g_jobCount.load() == 0) g_engineTid.store(me);
    }
    g_engineTickAt.store(GetTickCount());
    DwInstTick();                           // 0.56.14: fabbro e Maestro dell'eroe 2 chiusi quando il suo lato non si disegna piu'
    CompanionTick();                        // 0.56.28: vista del compagno chiusa con lo zaino o senza compagno
    UiSwapLatch();                          // 0.53.2: prima dei lavori in coda, che cosi' vedono lo scambio gia' applicato
    if (g_ctl2.load()) {                    // se l'eroe 2 sparisce mentre e' comandato, i comandi tornano all'eroe 1
        DWORD mgr = 0, player = 0, heroId = 0, engine = 0, ctl = 0;
        Rd(kObjMgr, mgr); Rd(kPlayer, player); Rd(kEngine, engine);
        if (player) RdHero(player, heroId);
        if (engine) Rd(engine + 0x13c, ctl);
        const DWORD a = mgr ? ObjectAddr(mgr, 2) : 0;
        if (ctl != 2 && ctl != 0) {         // qualcun altro (il gioco) ha gia' cambiato: non si controlla piu'
            g_ctl2.store(false);
        } else if (!a || !InWorld(a)) {
            if (engine && heroId) WriteDword(engine + 0x13c, heroId);
            g_ctl2.store(false);
            Log("NATIVO: l'eroe 2 non c'e' piu' nel mondo: comandi restituiti all'eroe %lu", heroId);
        }
    }
    if (g_cfg.nativeAggro) NativeAggroTick();
    Mouse2IdleTick();                       // 0.56.1: mano del secondo mouse fuori dalla partita
    Mouse2SwapCatch();                      // 0.56.7: oggetti messi dal gioco nella mano del giocatore 1 durante lo scambio
    TurnTrackTick();                        // 0.53.11: misura delle inversioni (diagnostica)
    Hero2AttackTick();                      // 0.55.8: attacco dell'eroe 2 con A tenuto, a ogni fotogramma
    SharedGoldTick();                       // 0.55.41: oro condiviso fra i due eroi
    if (g_cfg.nativeRevealHero2) RevealHero2Tick();   // 0.55.44: mappa scoperta anche intorno all'eroe 2
    AudioTraceTick();                       // 0.55.13: stato dell'audio (diagnostica)
    if (g_jobCount.load() == 0) return;
    const DWORD tid = GetCurrentThreadId();
    if (g_engineTid.exchange(tid) != tid) Log("NATIVO: lavori eseguiti nel thread del motore %lu", tid);
    for (;;) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lk(g_jobMutex);
            if (g_jobs.empty()) break;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        g_jobCount.fetch_sub(1);
        try { job(); } catch (...) { Log("NATIVO: eccezione C++ in un lavoro, ignorata"); }
    }
}

// ---------------------------------------------------------------------------------------------
// Sonda nel processo GameServer.exe (fase 5): la DLL proxy si carica anche nel server (importa VERSION.dll).
// Cerca nella memoria del server gli oggetti cCreature (vtable 0x52f9cc) e cCreatureHero (0x52fa24) con id
// da 1 a 16 e ne registra i campi, per confrontare l'eroe 1 con l'eroe 2 come li vede il server
// (i nemici non reagiscono all'eroe 2: prova del 27/10).
// ---------------------------------------------------------------------------------------------
void ServerProbe()
{
    static int round = 0;
    ++round;
    const DWORD vts[2] = { 0x0052F9CC, 0x0052FA24 };
    const char* vtName[2] = { "cCreature", "cCreatureHero" };
    int total[2] = { 0, 0 };
    int logged = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<BYTE> buf(1 << 20);
    uintptr_t a = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t maxA = std::min<uintptr_t>(reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress), 0x7FFF0000u);
    while (a < maxA) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress), re = rb + mbi.RegionSize;
        const bool ok = mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD) && rb >= 0x10000;
        if (ok) {
            for (uintptr_t c = rb; c < re; c += buf.size()) {
                const size_t n = std::min<uintptr_t>(buf.size(), re - c);
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c), buf.data(), n, &got) || got < 4) continue;
                for (size_t o = 0; o + 4 <= got; o += 4) {
                    const DWORD v = *reinterpret_cast<DWORD*>(&buf[o]);
                    int k = v == vts[0] ? 0 : (v == vts[1] ? 1 : -1);
                    if (k < 0) continue;
                    ++total[k];
                    const DWORD obj = static_cast<DWORD>(c + o);
                    DWORD id = 0;
                    if (!Rd(obj + 0xc, id) || id == 0 || id > 16 || logged >= 40) continue;
                    DWORD ty = 0, fl = 0, cls = 0, ow = 0, st = 0, hp = 0; int x = 0, y = 0; BYTE z = 0, b39 = 0;
                    Rd(obj + 0x10, ty); Rd(obj + 0x14, fl); Rd(obj + 0x1c, x); Rd(obj + 0x20, y); Rd(obj + 0x24, z); Rd(obj + 0x39, b39);
                    Rd(obj + 0x150, st); Rd(obj + 0x1ec, ow); Rd(obj + 0x1f0, cls); Rd(obj + 0x4d8, hp);
                    Log("SERVER: %s %08lX id %lu tipo %lu flag %08lX pos (%d,%d,%u) +0x39 %u stato %08lX prop %lu classe %lu vita %lu",
                        vtName[k], obj, id, ty, fl, x, y, z, b39, st, ow, cls, hp);
                    ++logged;
                }
            }
        }
        a = re;
        if (re <= rb) break;
    }
    Log("SERVER: giro %d: cCreature %d, cCreatureHero %d (eroi con id 1..16 registrati: %d)", round, total[0], total[1], logged);
}

// 0.53.6: solo con [Native] ServerProbe=true. Fino alla 0.53.5 partiva sempre: con lo schermo diviso GameServer.exe carica
// la DLL (sta nella cartella del gioco) e ogni 5 s la sonda leggeva tutta la memoria del server (log della Steam Machine del
// 29/10: 375 righe in 10 minuti), un carico inutile proprio nel processo che fa girare il mondo. Nel server non si legge
// la configurazione completa (LoadConfig): basta questa voce.
void ServerMain()
{
    char exe[MAX_PATH] = {}, ini[MAX_PATH] = {}, v[16] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* s = strrchr(exe, '\\')) *s = 0;
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\SacredPad.ini", exe);
    GetPrivateProfileStringA("Native", "ServerProbe", "false", v, sizeof(v), ini);
    if (_stricmp(v, "true") != 0 && strcmp(v, "1") != 0) { Log("SERVER: sonda del server spenta (ServerProbe=false)"); return; }
    Log("SERVER: sonda del server attiva (fase 5)");
    for (;;) {
        Sleep(5000);
        ServerProbe();
    }
}
