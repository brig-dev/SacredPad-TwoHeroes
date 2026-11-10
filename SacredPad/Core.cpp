// Core.cpp - thread principale: trova la finestra, legge il pad e guida
// l'input virtuale (fase 2, passo 2: aggancio del bersaglio).
// Linguaggio: C++17, MSVC, Win32 (x86).
//
// Schema dei comandi (nomi Xbox, tra parentesi DualSense):
//   Stick sinistro   movimento (puntatore attorno al personaggio + sinistro premuto)
//   Stick destro     mirino attorno al personaggio
//   A (Croce)        attacco/interazione: clic sinistro (Ctrl+clic se AimMode=Ranged);
//                    clic normale se il punto viene dal mouse o dal trackpad (parla, apre, raccoglie)
//   X (Quadrato)     arte slot 1: tasto 6 poi clic destro nel punto di mira
//   Y (Triangolo)    arte slot 2 (7)      RB (R1) arte slot 3 (8)
//   RT (R2)          arte slot 4 (9)      LT (L2) arte slot 5 (0)
//   LB (L1)          pozione di cura (Spazio)
//   B (Cerchio)      breve: raccogli tutto (A), al rilascio; tenuto: clic sull'icona del
//                    cavallo accanto al ritratto (richiama il cavallo, sali/scendi)
//   Croce sx/dx      arma precedente/successiva (1..UnlockedSlots)
//   Croce su/giu'    pozione di concentrazione (R, gialla) / del Mentore (W, blu; 0.56.25, prima antidoto E; scelte con
//                    [Keys] DpadUpPotion / DpadDownPotion), al rilascio; tenute: zoom (rotellina
//                    avanti / indietro, 0.55.31, [Keys] ZoomHoldMs)
//   Pressione stick destro   Alt tenuto (oggetti e nomi)
//   Pressione stick sinistro minimappa: Tab fisico tenuto premuto (Sacred la mostra
//                    solo finche' il Tab e' giu'); MinimapMode=Toggle apre/chiude
//                    a ogni pressione, Hold la mostra finche' si tiene premuto
//   View             passa da modalita' gioco a modalita' cursore e viceversa, senza
//                    inviare tasti al gioco; tenuto premuto: inventario (I) + cambio di modalita'
//   Menu             menu di gioco (Esc); dalla modalita' gioco entra anche in modalita'
//                    cursore, in modalita' cursore invia solo Esc e non cambia modalita'
// All'avvio la mod e' in modalita' cursore (StartInCursor=true), perche' il
// gioco si apre sui menu.
//
// Modalita' cursore (inventario, negozi, dialoghi, menu): stick sinistro =
// cursore libero, stick destro = cursore lento, croce = piccoli spostamenti,
// A = pulsante sinistro, B = pulsante destro (seguono il tasto: si puo'
// trascinare). Movimento e attacchi sono sospesi. Dal mercante (0.9.7):
// X = vendi l'oggetto sotto il cursore, LB/RB = scheda Armi/Armatura/Varie.
// Tenendo A o un'arte la direzione si cambia con lo stick destro, con lo
// stick sinistro (come in Diablo IV: si attacca verso lo stick invece di
// muoversi) o col mouse; se il punto si sposta oltre ReclickDistance il clic
// viene ripetuto (Sacred altrimenti resta sul primo bersaglio).
// Aggancio (fase 2): senza mouse, A e le arti puntano la creatura ostile viva
// piu' adatta nella direzione corrente (stick destro, stick sinistro o ultimo
// movimento), letta dalla memoria del gioco; con A tenuto il puntatore la
// segue e, quando muore, passa alla successiva. Senza nemici, A parla con
// l'NPC piu' vicino in quella direzione (dal 0.10.0, da fermi, il puntatore mostra in
// anticipo chi A sceglierebbe: PreviewTick) (entro TalkRange; in movimento fino a
// LockRange nella direzione del movimento). Un NPC, forziere o oggetto vicino
// vince su un nemico lontano (oltre LockNear). Con A tenuto e senza bersaglio
// si cerca un nemico nella direzione dello stick ogni 200 ms (0.9.2).
// Un'arte premuta mentre si tiene A interrompe l'attacco, si lancia nello
// stesso punto e, finche' A resta premuto, l'attacco riprende.
// Il mouse vero riprende il controllo appena lo si muove; premendo poi A o
// un'arte il pad riparte dal punto indicato col mouse.

#include "SacredPad.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

std::atomic<HWND> g_gameHwnd{ nullptr };

namespace {

// --- Ricerca della finestra e diagnostica ------------------------------------
struct FindCtx { DWORD pid; HWND best; LONG bestArea; };

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd)) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->bestArea) { ctx->best = hwnd; ctx->bestArea = area; }
    return TRUE;
}

HWND FindGameWindow()
{
    FindCtx ctx{ GetCurrentProcessId(), nullptr, 0 };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

void LogWindow(HWND hwnd)
{
    char cls[128] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    RECT cr;
    GetClientRect(hwnd, &cr);
    POINT o{ 0, 0 };
    ClientToScreen(hwnd, &o);
    Log("Finestra del gioco: hwnd %p, classe \"%s\", area client %ldx%ld, origine (%ld,%ld)",
        hwnd, cls, cr.right, cr.bottom, o.x, o.y);
}

// --- Invio degli eventi al gioco --------------------------------------------
LPARAM ClientParam(HWND hwnd)
{
    POINT p{ g_virt.x.load(), g_virt.y.load() };
    ScreenToClient(hwnd, &p);
    return MAKELPARAM(static_cast<SHORT>(p.x), static_cast<SHORT>(p.y));
}

WPARAM ButtonFlags()
{
    WPARAM w = 0;
    if (g_virt.lmb.load())          w |= MK_LBUTTON;
    if (g_virt.rmb.load())          w |= MK_RBUTTON;
    if (VirtualKeyDown(VK_CONTROL)) w |= MK_CONTROL;
    if (VirtualKeyDown(VK_SHIFT))   w |= MK_SHIFT;
    return w;
}

// 0.56.7: scambio dell'interfaccia (mercante, deposito, fabbro, maestri, stalliere) guidato dal pad 2 con il secondo mouse: la
// logica del cursore del pad 1, che durante lo scambio lavora con lo stato del pad 2, esce sul secondo mouse (posizione e
// pulsanti, eventi del gioco mandati dalla mod) invece che sul mouse del gioco (g_virt, messaggi alla finestra, mouse vero).
// Acceso in Tick solo fra la sostituzione del pad e la fine del giro.
bool g_ptrToM2 = false;
LONG g_m2OutX = 0, g_m2OutY = 0;
bool g_m2OutL = false, g_m2OutR = false;
void M2OutRelease()
{
    if (g_m2OutL) { MemoryMouse2Button(0, false, g_m2OutX, g_m2OutY); g_m2OutL = false; }
    if (g_m2OutR) { MemoryMouse2Button(2, false, g_m2OutX, g_m2OutY); g_m2OutR = false; }
}

void SendMove(HWND hwnd)
{
    if (g_ptrToM2) { MemoryMouse2Ui(true, g_m2OutX, g_m2OutY); return; }
    PostMessageA(hwnd, WM_MOUSEMOVE, ButtonFlags(), ClientParam(hwnd));
}
void SendLeftDown(HWND hwnd)
{
    if (g_ptrToM2) { g_m2OutL = true; MemoryMouse2Button(0, true, g_m2OutX, g_m2OutY); return; }
    g_virt.lmb.store(true);  PostMessageA(hwnd, WM_LBUTTONDOWN, ButtonFlags(), ClientParam(hwnd));
}
void SendLeftUp(HWND hwnd)
{
    if (g_ptrToM2) { g_m2OutL = false; MemoryMouse2Button(0, false, g_m2OutX, g_m2OutY); return; }
    g_virt.lmb.store(false); PostMessageA(hwnd, WM_LBUTTONUP,   ButtonFlags(), ClientParam(hwnd));
}
void SendRightDown(HWND hwnd)
{
    if (g_ptrToM2) { g_m2OutR = true; MemoryMouse2Button(2, true, g_m2OutX, g_m2OutY); return; }
    g_virt.rmb.store(true);  PostMessageA(hwnd, WM_RBUTTONDOWN, ButtonFlags(), ClientParam(hwnd));
}
void SendRightUp(HWND hwnd)
{
    if (g_ptrToM2) { g_m2OutR = false; MemoryMouse2Button(2, false, g_m2OutX, g_m2OutY); return; }
    g_virt.rmb.store(false); PostMessageA(hwnd, WM_RBUTTONUP,   ButtonFlags(), ClientParam(hwnd));
}

// Tasti: messaggi WM_KEYDOWN/WM_KEYUP con codice virtuale e di scansione (il
// gioco usa entrambi) + stato visibile da GetAsyncKeyState/GetKeyboardState.
// Il ciclo dei messaggi del gioco li passa a TranslateMessage come i tasti veri.
struct Key { UINT vk; UINT scan; const char* name; };

const Key kSpace{ VK_SPACE,  0x39, "Spazio (pozione di cura)" };
const Key kKeyA { 'A',       0x1E, "A (raccogli tutto)" };
const Key kKeyR { 'R',       0x13, "R (pozione di concentrazione)" };
const Key kKeyE { 'E',       0x12, "E (antidoto)" };
const Key kKeyQ { 'Q',       0x10, "Q (pozione dei Non-morti)" };   // 0.56.25
const Key kKeyW { 'W',       0x11, "W (pozione del Mentore)" };
const Key& PotionKey(int kind) { return kind == 2 ? kKeyQ : kind == 3 ? kKeyW : kind == 4 ? kKeyE : kKeyR; }
const Key kKeyI { 'I',       0x17, "I (inventario)" };
const Key kTab  { VK_TAB,    0x0F, "Tab (minimappa)" };
const Key kEsc  { VK_ESCAPE, 0x01, "Esc (menu)" };
const Key kEnter{ VK_RETURN, 0x1C, "Invio" };
const Key kCtrl { VK_CONTROL,0x1D, "Ctrl" };
const Key kAlt  { VK_MENU,   0x38, "Alt" };
const Key kShift{ VK_SHIFT,  0x2A, "Shift" };
const Key kDigit[10] = {   // tasti da 0 a 9 della riga numerica
    { '0', 0x0B, "0" }, { '1', 0x02, "1" }, { '2', 0x03, "2" }, { '3', 0x04, "3" }, { '4', 0x05, "4" },
    { '5', 0x06, "5" }, { '6', 0x07, "6" }, { '7', 0x08, "7" }, { '8', 0x09, "8" }, { '9', 0x0A, "9" },
};

void PostKey(HWND hwnd, const Key& k, bool down)
{
    LPARAM lp = static_cast<LPARAM>(1u | (k.scan << 16));
    if (!down) lp |= static_cast<LPARAM>((1u << 30) | (1u << 31));
    PostMessageA(hwnd, down ? WM_KEYDOWN : WM_KEYUP, k.vk, lp);
}

// 0.55.37: nello schermo diviso il Tab fisico arriva solo alla finestra in primo piano, l'unica in cui il gioco lo legge. Prova
// del 31/10 alle 23:24: L3 del pad 1 nell'host e minimappa aperta nella finestra dell'ospite, che era in primo piano ("la minimappa
// del pad 1 si apre nella schermata 2"). Prima del Tab la finestra di chi preme si porta in primo piano (le due istanze lavorano
// anche senza il focus); un attimo di attesa perche' il cambio arrivi prima del tasto.
void MinimapTakeFocus(HWND hwnd)
{
    if (!g_cfg.coopRole || !hwnd) return;
    const HWND fg = GetForegroundWindow();
    if (fg == hwnd) return;
    BOOL ok = SetForegroundWindow(hwnd);
    const DWORD err = ok ? 0 : GetLastError();
    // 0.55.44: prova del 01/11 sulla Steam Machine: SetForegroundWindow rifiutata ogni volta. Seconda strada: l'input di questo
    // thread si aggancia a quello della finestra in primo piano (AttachThreadInput), poi di nuovo SetForegroundWindow; in piu'
    // ogni istanza in primo piano autorizza l'altra (AllowSetForegroundWindow in CoopTick).
    if (!ok && fg) {
        const DWORD fgTid = GetWindowThreadProcessId(fg, nullptr), me = GetCurrentThreadId();
        const BOOL att = fgTid && fgTid != me && AttachThreadInput(me, fgTid, TRUE);
        BringWindowToTop(hwnd);
        ok = SetForegroundWindow(hwnd);
        if (att) AttachThreadInput(me, fgTid, FALSE);
    }
    if (ok) Sleep(40);
    Log("Minimappa: finestra portata in primo piano per il Tab (%s, prima %p, errore %lu, ora %p)", ok ? "ok" : "non riuscito",
        static_cast<void*>(fg), err, static_cast<void*>(GetForegroundWindow()));
}

// Evento fisico con SendInput (tastiera vera o pulsante centrale del mouse).
void SendSystem(const Key& k, bool middle, bool down)
{
    INPUT in{};
    if (middle) {
        in.type       = INPUT_MOUSE;
        in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }
    else {
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = static_cast<WORD>(k.vk);
        in.ki.wScan   = static_cast<WORD>(k.scan);
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    }
    if (SendInput(1, &in, sizeof(INPUT)) != 1)
        Log("avviso: SendInput fallita (%lu)", GetLastError());
}

void TapKey(HWND hwnd, const Key& k)
{
    g_virt.keyUntil[k.vk].store(GetTickCount() + 80);   // visto "premuto" per 80 ms
    PostKey(hwnd, k, true);
    PostKey(hwnd, k, false);
}

void HoldKey(HWND hwnd, const Key& k, bool down)
{
    if (VirtualKeyDown(static_cast<int>(k.vk)) == down) return;
    g_virt.keyUntil[k.vk].store(down ? VirtualInput::kHeld : 0);
    PostKey(hwnd, k, down);
}

// --- Stato del ciclo principale ----------------------------------------------
enum class Click { None, Left, Right };

struct Loop {
    HWND  hwnd        = nullptr;
    bool  moveHeld    = false;      // sinistro tenuto dallo stick sinistro
    Click held        = Click::None; // clic tenuto da A o da un'arte
    WORD  heldMask    = 0;          // pulsante del pad che lo tiene
    bool  pendingArt  = false;      // tasto dello slot inviato, clic destro in attesa
    DWORD pendingDue  = 0;
    bool  pendingRelease = false;   // pulsante gia' rilasciato durante l'attesa
    bool  releaseWanted = false;    // rilascio chiesto prima della durata minima del clic dell'arte
    bool  moveLocked  = false;      // movimento sospeso dopo un'arte
    bool  resumeA     = false;      // arte lanciata interrompendo A: riprendere l'attacco dopo
    bool  plainA      = false;      // A puntato col mouse/trackpad o agganciato: clic senza Ctrl
    bool  plainArt    = false;      // arte su un bersaglio agganciato: clic destro senza Ctrl
    bool  artOnHero2  = false;      // 0.55.42: arte mirata sull'eroe 2: clic destro semplice su di lui (cure)
    bool  locked      = false;      // bersaglio agganciato da seguire
    Target lock;
    DWORD lockScan    = 0;          // ultima nuova ricerca con lo stick destro
    DWORD talkSkip[8] = {};         // A ripetuto: NPC/oggetti gia' scelti da saltare
    int   talkSkipN   = 0;
    DWORD talkLast    = 0;          // istante dell'ultima scelta con A di un NPC/oggetto
    // A ripetuto mentre l'eroe cammina verso il bersaglio scelto: si resta su quello
    // (0.9.2 solo NPC, dal 0.9.3 anche forzieri e oggetti a terra)
    Target talkPick;                // ultimo NPC, forziere o oggetto scelto con A (addr = 0: nessuno)
    DWORD usedCheck   = 0;          // ultimo controllo "forziere raggiunto"
    float talkPickMin = 0;          // distanza minima raggiunta dall'eroe (per il log)
    // B: breve = raccogli tutto (al rilascio), tenuto = smonta da cavallo (0.9.3)
    DWORD bSince      = 0;          // istante della pressione di B (0 = non premuto)
    bool  bLong       = false;      // pressione lunga gia' gestita
    int   dismountStep = 0;         // clic sull'icona del cavallo: 1 = puntatore spostato, 2 = sinistro giu'
    // Clic programmato in modalita' cursore (0.9.7: vendita con X, schede del mercante con LB/RB)
    int   mcStep      = 0;          // 1 = puntatore spostato, 2 = sinistro giu', 3 = su, poi fine
    DWORD mcAt        = 0;
    bool  mcShift     = false;      // Shift tenuto durante il clic (vendita)
    bool  mcRestore   = false;      // alla fine il cursore torna dov'era (schede)
    float mcRX = 0, mcRY = 0;
    POINT mcReal{ 0, 0 };           // posizione del mouse vero da rimettere a fine clic (0.9.13)
    bool  mcRealMoved = false;
    bool  merchantOpen = false;     // finestra del mercante (n.4) vista aperta
    bool  busyOpen    = false;      // finestra di scelta dei portali visibile (0.10.5)
    DWORD portalUntil = 0;          // 0.56.38: portale appena toccato: niente ordini di movimento fino a qui
    DWORD portalCheckAt = 0;
    int   merchantTab = 0;          // scheda attiva secondo la mod: 0 Armi, 1 Armatura, 2 Varie
    DWORD merchantDumpAt = 0;       // controllo della scheda attiva dopo un clic sulla scheda
    int   tabRetry    = 0;          // clic sulla scheda ripetuti perche' il gioco non l'ha cambiata
    int   tabQueue    = 0;          // LB/RB premuti durante un clic: passi in attesa (+ avanti, - indietro)
    DWORD previewAt   = 0;          // anteprima del bersaglio: ultimo calcolo
    DWORD previewId   = 0;          // id + 1 del bersaglio mostrato (0 = nessuno), per il log
    DWORD dismountAt  = 0;
    float talkPickDist = 0;         // sua distanza dall'eroe nel mondo al momento della scelta
    bool  talkDialogSeen = false;   // dopo quella scelta si e' aperto un dialogo
    bool  talkSame    = false;      // l'ultima FindTalk ha ripreso lo stesso NPC (per il log)
    bool  talkImportant = false;    // l'ultimo NPC scelto ha un servizio o un dialogo (0.10.4)
    DWORD talkRepeatId = 0;         // ... e il suo id, con quante volte A e' tornato su di lui
    int   talkRepeats = 0;
    bool  lockSearch  = false;      // A tenuto senza bersaglio: si cerca un nemico nella direzione dello stick (0.9.2)
    DWORD lastCombat  = 0;          // ultimo attacco o arte: per kCombatMs niente forzieri lontani ne' priorita' agli oggetti (0.9.4)
    // Movimento bloccato: il personaggio non avanza pur con lo stick inclinato
    DWORD stuckCheck  = 0;
    int   stuckX = 0, stuckY = 0;
    bool  stuckValid  = false;
    int   stuckLevel  = 0;          // 0 normale, 1 punto piu' lontano, 2/3 ruotato di +-35 gradi
    int   uiState     = -2;         // ultimo stato per la modalita' cursore automatica (-2 = mai letto)
    int   winState    = -2;         // ultimo MemoryUIState() (solo le 23 finestre)
    DWORD dlgWatchUntil = 0;        // dopo un clic su un NPC: si cerca la finestra del dialogo fino a qui
    DWORD p2InteractAt = 0;         // 0.55.0: ultima interazione dell'eroe 2 con A del pad 2 (avviso alla finestra che si apre)
    DWORD p2ClosedAt = 0;           // 0.55.49: finestre chiuse con il puntatore al pad 2 (ritorno al pad 1 dopo 900 ms)
    DWORD loadWatchUntil = 0;       // 0.49.8: dopo il caricamento solo la ricerca vicina (non quella completa su A/B)
    DWORD loadDumpAt = 0;           // 0.55.27: elenco dei candidati dei dialoghi nel log 3 s dopo il caricamento
    DWORD zSince[2] = {}, zNext[2] = {};   // 0.55.31: croce su (0) e giu' (1) tenute: zoom
    bool  zDone[2] = {};
    bool  p2DlgClickNoPad1 = false; // 0.55.29: clic del pad 2 su un dialogo senza pad 1: a clic finito il mouse torna al giocatore 1
    DWORD dlgQuick    = 0;          // ultima ricerca vicina
    DWORD dlgSince    = 0;          // 0.55.63: dialogo visto da qui (senza finestre): conta solo dopo 150 ms
    DWORD dlgFull     = 0;          // ultima ricerca completa
    int   dlgClick    = 0;          // clic su Rifiuta in corso: 1 = puntatore spostato, 2 = pulsante giu'
    DWORD dlgClickAt  = 0;
    DWORD uiCheck     = 0;
    DWORD moveLockUntil = 0;        // ... fino a questo istante
    POINT clickPt{ 0, 0 };          // dove e' stato fatto l'ultimo clic (client)
    DWORD clickTime   = 0;
    // Clic di A "posato" (0.11.5): il gioco decide che cosa c'e' sotto il clic con cio' che sta
    // sotto il puntatore, aggiornato una volta per fotogramma. Il puntatore va sul bersaglio,
    // il clic parte dopo kSettleMs, resta giu' almeno kSettleMs e dopo il rilascio il puntatore
    // resta fermo kAfterMs (anteprima sospesa).
    bool  pendingA    = false;      // clic di A in attesa che il puntatore si posi
    DWORD pendingAAt  = 0;          // quando premere
    bool  settledA    = false;      // il clic in corso e' un clic posato (durata minima)
    DWORD stillUntil  = 0;          // anteprima sospesa fino a questo momento
    // Bersaglio a fuoco (0.11.8, richiesta del tester del 23/10): NPC, forziere, porta o oggetto
    // che A usera'. Cambia solo con lo stick (sinistro: cono della direzione; destro: mirino);
    // da fermi resta lo stesso finche' e' valido. Sopra di lui la mod disegna "( A )".
    bool  gridBlocked = false;      // ultimo passo nella griglia fermo al bordo (0.13.0)
    // Porta appena cliccata con A (0.13.2): esclusa dal bersaglio a fuoco per kDoorCooldownMs. Il suo
    // stato durante l'apertura non e' affidabile e il secondo A la richiudeva (prova delle 20:12,
    // porta 1485 OBJECT_DOOR_DUNGEON02_A e mobile 1659).
    DWORD startSince  = 0;          // Start premuto da (0 = no): tenuto apre l'aiuto (0.15.0)
    bool  startLong   = false;
    bool  helpOpen    = false;
    RECT  escItem{};                // voce "Controller" sopra il menu Esc (0.15.1)
    bool  escItemOn   = false;
    RECT  mainRow{};                // voce "Co-op locale" del menu principale (0.20.1)
    bool  mainRowOn   = false;
    bool  lmbWas      = false;
    DWORD navAt       = 0;          // co-op: prossimo passo della navigazione automatica
    int   navStage    = 0;          // ultimo stato registrato nel log
    int   navSub      = 0;          // ospite nella lobby: 0 = clic sulla partita, 1 = clic su Unisciti
    DWORD navJoinAt   = 0;
    bool  navDone     = false;      // la navigazione automatica e' finita (una sola volta per sessione)
    DWORD navStart    = 0;
    int   navClicks   = 0, navJoins = 0;
    DWORD navLastClick = 0;
    bool  navMenuDone = false;      // raggiunta la scheda personaggi: niente piu' clic nei menu (se si torna indietro)
    bool  navHold     = false;      // questa istanza tiene il mutex dei clic
    DWORD doorUsedId  = 0xffffffffu;
    DWORD doorUsedUntil = 0;
    // Porta appena toccata dallo stick o da A (0.16.0, richiesta del tester del 24/10): per kStickDoorMs
    // i clic di movimento non cadono su di lei, cosi' non si richiude mentre ci si passa attraverso.
    DWORD stickDoorId = 0xffffffffu;
    DWORD stickDoorUntil = 0;
    bool  hoverWait   = false;      // attesa che il gioco non indichi piu' un oggetto (0.16.1)
    bool  moveRetryPress = false;   // pressione di un nuovo tentativo contro un ostacolo in attesa
    DWORD watchDoorId = 0xffffffffu;   // ultima porta indicata dal gioco sotto il puntatore e il suo stato (0.16.2)
    DWORD watchDoorState = 0;
    int   bumpStep    = 0;          // porta a spinta (0.17.1): 1 puntatore sulla porta, 2 tasto giu', 3 attesa apertura
    DWORD bumpId      = 0, bumpAt = 0, bumpLastId = 0xffffffffu, bumpLastAt = 0;
    DWORD bumpClickAt = 0;          // 0.56.42: clic sulla porta; posizione dell'eroe all'ultimo controllo
    int   bumpHx = 0, bumpHy = 0;
    LONG  bumpX       = 0, bumpY = 0;
    int   helpRow     = 0;          // riga scelta nell'aiuto: 0 controller, 1 vibrazione (0.17.0)
    int   helpPage    = 0;          // 0.55.68: pagina dell'aiuto (LB / RB)
    DWORD vibTestUntil = 0;         // impulso di prova appena attivata la vibrazione
    DWORD hoverWaitUntil = 0;
    Target focus;
    bool  focusOn     = false;
    DWORD focusAt     = 0;
    float focusLift   = 0;          // dai piedi alla cima del suo rettangolo (pixel), per "( A )"
    bool  moveSettling = false;     // movimento: puntatore spostato, clic in attesa (0.11.7)
    DWORD moveSettleAt = 0;
    WORD  prevButtons = 0;
    POINT lastReal{ 0, 0 };
    DWORD dirDiagAt   = 0;          // 0.53.8: ultima verifica della direzione della destinazione
    bool  directMoving = false;     // 0.53.9: eroe 1 mosso con ordini diretti (camera sul punto medio)
    DWORD directAt    = 0;          // ultimo ordine mandato
    DWORD directStopAt = 0;         // 0.53.10: arresto rimandato dopo il rilascio (0 = nessuno)
    float directDx = 0, directDy = 0;   // e la sua direzione
    LONG  mouseAcc    = 0;          // 0.53.3: strada fatta dal mouse vero nell'ultimo tratto (pixel)
    DWORD mouseAccAt  = 0;          // inizio di quel tratto
    POINT lastSent{ -1, -1 };
    float dirX = 1.0f, dirY = 0.0f; // ultima direzione (schermo, y in basso)
    int   weapon      = 1;          // slot arma corrente secondo SacredPad
    bool  minimapDown = false;      // Tab (o centrale) fisico attualmente premuto
    DWORD minimapSince = 0;         // istante della pressione
    bool  minimapRelease = false;   // rilascio richiesto, appena trascorsa la durata minima
    int   minimapOwner = 0;         // 0.55.16: chi l'ha aperta, pad 1 o pad 2 (con Hold la chiude il suo rilascio)
    DWORD lsTapAt[3] = {};          // 0.56.16: ultima pressione di L3 del pad 1 e del pad 2 (doppio L3: raggiungi il compagno)
    bool  altP1 = false, altP2 = false;   // 0.55.16: R3 tenuto dal pad 1 / dal pad 2 (Alt resta giu' finche' uno dei due lo tiene)
    WORD  pad2Prev = 0;             // 0.55.16: pulsanti del pad 2 al giro prima (NativePad2Buttons)
    int   ptrSrc = 1;               // 0.50.0 (N4): chi comanda il puntatore, pad 1 o pad 2 (0.55.18: qui, non piu' statico nel blocco)
    DWORD invOpenAt = 0;            // 0.50.0 (N4): zaino dell'eroe 2 da aprire dopo lo scambio
    // 0.55.54: due zaini, fase 2 (Pad2Pointer): lati, secondo puntatore e titolare del mouse del gioco
    int   dualOwner = 1;            // giocatore che ha il mouse del gioco (g_virt) con il puntatore del giocatore 2 acceso
    DWORD dualOpenAt = 0;           // tasto I mandato per aprire un lato: la sincronia aspetta 600 ms
    DWORD p1InputAt = 0, p2InputAt = 0;   // ultimo uso dei due pad (stick o pulsanti)
    bool  p2On = false;             // puntatore del giocatore 2 acceso (suo lato aperto)
    float p2x = 0, p2y = 0;         // sua posizione (client)
    bool  p2Left = false, p2Right = false;
    bool  p2WorldA = false, p2WorldB = false;   // 0.56.2: A / B del giocatore 2 nel mondo (modalita' cursore), da rilasciare
    DWORD p2OffAt = 0;                          // 0.56.2: ultimo giro con il puntatore del giocatore 2 acceso
    bool  p2SideWas = false;                    // 0.56.4: lato del giocatore 2 aperto al giro prima
    bool  p1Kept = false, p1KeepCursor = false; // 0.56.7: cursore del pad 1 tenuto da parte durante lo scambio sul secondo mouse
    float p1KeepX = 0, p1KeepY = 0;
    WORD  p2Prev = 0;
    DWORD p2Tick = 0;
    int   p2mc = 0;                 // clic a tempo sulle schede della copia: 1 spostato, 2 giu', 3 su
    DWORD p2mcAt = 0;
    POINT p2Real{ 0, 0 };
    bool  p2RealMoved = false;
    int   heldBy = 0;               // giocatore con l'oggetto in mano (0 = nessuno)
    DWORD p2FreezeUntil = 0, p1FreezeUntil = 0;   // 0.55.59: puntatore fermo dopo un clic (vedi Pad2Pointer)
    DWORD p1AtkAt = 0, p1AtkId = 0; // 0.55.55: attacco dell'eroe 1 con ordini diretti (mouse del gioco al giocatore 2)
    DWORD lastStatus  = 0;
    // Modalita' cursore
    bool  cursorMode  = false;
    float cx = 0, cy = 0;           // posizione del cursore in coordinate client (con frazioni di pixel)
    bool  cursorLeft  = false;      // A tenuto in modalita' cursore
    bool  cursorRight = false;      // B tenuto in modalita' cursore
    DWORD lastTick    = 0;
    DWORD viewSince   = 0;          // istante della pressione di View (0 = non premuto)
    bool  viewLong    = false;      // pressione lunga gia' gestita
    bool  started     = false;      // modalita' iniziale gia' impostata
};

void SetVirtualClient(Loop& s, LONG cx, LONG cy, LONG w, LONG h)
{
    if (cx < 1) cx = 1;
    if (cy < 1) cy = 1;
    if (cx > w - 2) cx = w - 2;
    if (cy > h - 2) cy = h - 2;
    if (g_ptrToM2) { g_m2OutX = cx; g_m2OutY = cy; MemoryMouse2Ui(true, cx, cy); return; }   // 0.56.7
    POINT p{ cx, cy };
    ClientToScreen(s.hwnd, &p);
    g_virt.x.store(p.x);
    g_virt.y.store(p.y);
}

POINT VirtualClient(HWND hwnd)
{
    if (g_ptrToM2) return POINT{ g_m2OutX, g_m2OutY };   // 0.56.7
    POINT p{ g_virt.x.load(), g_virt.y.load() };
    ScreenToClient(hwnd, &p);
    return p;
}

void MoveIfChanged(Loop& s)
{
    POINT p{ g_virt.x.load(), g_virt.y.load() };
    if (p.x != s.lastSent.x || p.y != s.lastSent.y) {
        SendMove(s.hwnd);
        s.lastSent = p;
    }
}

void ReleaseAll(Loop& s, const char* why)
{
    if (g_ptrToM2) M2OutRelease();                  // 0.56.7
    if (s.hwnd && !g_ptrToM2) {
        if (g_virt.lmb.load()) SendLeftUp(s.hwnd);
        if (g_virt.rmb.load()) SendRightUp(s.hwnd);
        HoldKey(s.hwnd, kCtrl, false);
        if (!s.altP2) HoldKey(s.hwnd, kAlt, false);   // 0.55.16: l'Alt del pad 2 resta (lo rilascia il suo R3)
        HoldKey(s.hwnd, kShift, false);
    }
    s.altP1 = false;
    s.dismountStep = 0;
    if (s.mcStep && s.mcRealMoved) {               // clic programmato interrotto: mouse vero al suo posto
        s.mcRealMoved = false;
        if (RealSetCursorPos(s.mcReal.x, s.mcReal.y)) s.lastReal = s.mcReal;
    }
    s.mcStep = 0;
    if (s.moveHeld || s.held != Click::None || s.pendingArt) Log("Rilascio dei pulsanti virtuali (%s)", why);
    s.moveHeld = false;
    s.held = Click::None;
    s.heldMask = 0;
    s.pendingArt = s.pendingRelease = s.releaseWanted = false;
    s.resumeA = false;
    s.locked = false;
    s.cursorLeft = s.cursorRight = false;
}

void Activate(const POINT& screen)
{
    if (g_ptrToM2) return;                          // 0.56.7: il puntatore e' il secondo mouse, sempre acceso
    if (g_virt.active.load()) return;
    g_virt.x.store(screen.x);
    g_virt.y.store(screen.y);
    g_virt.active.store(true);
    Log("Modalita' pad ATTIVA (puntatore virtuale)");
}

void Deactivate(Loop& s, const char* why)
{
    if (g_ptrToM2) { M2OutRelease(); return; }      // 0.56.7: il mouse vero non toglie il puntatore al giocatore 2
    if (!g_virt.active.load()) return;
    ReleaseAll(s, why);
    g_virt.active.store(false);
    s.lastSent = POINT{ -1, -1 };
    Log("Modalita' pad disattivata: %s", why);
}

bool StickDir(float x, float y, float deadzone, float& dx, float& dy, float& t)
{
    float mag = std::sqrt(x * x + y * y);
    if (mag <= deadzone) return false;
    dx = x / mag;
    dy = -y / mag;                       // stick in alto = schermo in alto
    t  = (mag - deadzone) / (1.0f - deadzone);
    if (t > 1.0f) t = 1.0f;
    return true;
}

// Dati del fotogramma corrente, calcolati una volta per tick.
std::atomic<bool> g_pad1AHeld{ false };   // 0.56.47: il pad 1 tiene A (clic tenuto)

struct Frame {
    LONG  w, h;
    float hf, centerX, centerY;
    bool  ranged, moving, aiming;
    float mdx, mdy, adx, ady;
    LONG  aimX, aimY;
    POINT real;
    bool  mouseMoved;   // il mouse vero si e' mosso in questo tick
    DWORD now;
};

// Slot sbloccati (armi e arti): fissi da UnlockedSlots, oppure dal livello dell'eroe
// (sbloccati ai livelli 1, 2, 8, 16, 30). Letto al massimo una volta al secondo.
int UnlockedSlots(DWORD now)
{
    if (g_cfg.unlockedSlots > 0) return g_cfg.unlockedSlots;
    static DWORD last = 0;
    static int slots = 5, lastLevel = -1;
    if (last && now - last < 1000) return slots;
    last = now ? now : 1;
    int lv = 0;
    if (MemoryHeroLevel(lv)) {
        static const int kAt[5] = { 1, 2, 8, 16, 30 };
        int n = 0;
        for (int at : kAt) if (lv >= at) ++n;
        if (lv != lastLevel) Log("Eroe a livello %d: %d slot sbloccati", lv, n);
        lastLevel = lv;
        slots = n;
    }
    else {
        if (lastLevel != 0) Log("Livello dell'eroe non leggibile: 5 slot");
        lastLevel = 0;
        slots = 5;
    }
    return slots;
}

// --- Aggancio -------------------------------------------------------------------
// Dopo un attacco con A o un'arte, per questo tempo A non cerca forzieri e oggetti oltre
// TalkRange e non preferisce un oggetto vicino a un nemico lontano (0.9.4: alla prova della
// 0.9.3, in combattimento, A sceglieva forzieri a 500 pixel).
constexpr DWORD kCombatMs = 4000;
bool InCombat(const Loop& s, const Frame& f) { return s.lastCombat && f.now - s.lastCombat < kCombatMs; }
POINT LockPoint(const Target& t, const Frame& f)
{
    // 0.11.6: se possibile il punto in cui la scelta del gioco cade proprio sul bersaglio.
    LONG px = 0, py = 0;
    if (MemoryPickPoint(t.id, px, py)) return POINT{ px, py };
    // Creature, forzieri (anche i barili) e porte: sopra la base, sul corpo. Oggetti a terra: poco sopra.
    const bool flat = (t.cls < 0 && strcmp(t.kind, "forziere") != 0 && strncmp(t.kind, "porta", 5) != 0);
    float up = (flat ? g_cfg.lockOffsetY * 0.3f : g_cfg.lockOffsetY) * f.hf;
    return POINT{ static_cast<LONG>(t.sx), static_cast<LONG>(t.sy - up) };
}

// Cerca un bersaglio nella direzione corrente (s.dirX, s.dirY) dal personaggio.
bool FindLock(Loop& s, const Frame& f, bool talk, Target& out)
{
    float range = (talk ? g_cfg.talkRange : g_cfg.lockRange) * f.hf;
    // Per parlare va bene un NPC vicino in qualsiasi direzione (quello davanti e' preferito).
    float nearR = talk ? range : g_cfg.lockNear * f.hf;
    return MemoryFindTarget(talk, f.centerX, f.centerY, s.dirX, s.dirY, range, nearR, out);
}

// NPC, forziere o oggetto per A. Se A viene ripremuto entro TalkCycleMs si saltano quelli
// gia' scelti, cosi' si passa al successivo; finiti i candidati si ricomincia dal migliore.
// Il compagno al seguito e' l'ultimo del giro; alla prima pressione vale solo se e' nella
// direzione dello stick. In movimento la ricerca arriva fino a LockRange nella direzione
// del movimento (fermi: TalkRange in ogni direzione).
// La scelta e' separata dall'aggiornamento del giro (0.10.0): ChooseTalk non cambia lo stato e
// serve anche all'anteprima, FindTalk sceglie e registra la scelta.
struct TalkChoice { Target t; bool cycle = false, same = false, restart = false; float d = 0; };

bool ChooseTalk(const Loop& s, const Frame& f, TalkChoice& c)
{
    // 0.11.8: se c'e' un bersaglio a fuoco, A usa quello e nient'altro (niente giro).
    if (s.focusOn) {
        c.t = s.focus;
        float d = 0;
        c.same = s.talkPick.addr && s.talkPick.id == c.t.id && !s.talkDialogSeen &&
                 MemoryHeroDistance(s.talkPick, d) && d < s.talkPickDist - 10.0f;
        c.d = d;
        return MemoryTrackTarget(c.t);
    }
    c.cycle = false;                                 // giro di A ripetuto tolto nella 0.11.8
    const int skipN = c.cycle ? s.talkSkipN : 0;
    // A ripetuto mentre l'eroe sta ancora andando verso il bersaglio appena scelto (nessun
    // dialogo aperto e distanza diminuita): si resta su quello invece di passare oltre. Il
    // 22/10 alle 20:46 A premuto ogni 0.2 s cambiava forziere a ogni pressione e l'eroe non
    // arrivava mai a nessuno.
    float d = 0;
    if (c.cycle && s.talkPick.addr && !s.talkDialogSeen && MemoryHeroDistance(s.talkPick, d) &&
        d < s.talkPickDist - 10.0f) {
        Target t = s.talkPick;
        if (MemoryTrackTarget(t)) { c.t = t; c.same = true; c.d = d; return true; }
    }
    const float nearR = g_cfg.talkRange * f.hf;
    float range = nearR;
    if (f.moving && !InCombat(s, f) && g_cfg.lockRange * f.hf > range) range = g_cfg.lockRange * f.hf;
    // Senza giro (0.11.8) l'elenco da saltare serve solo per la porta appena usata (0.13.2).
    const bool doorCool = static_cast<LONG>(f.now - s.doorUsedUntil) < 0;
    bool ok = MemoryFindTarget(true, f.centerX, f.centerY, s.dirX, s.dirY, range, nearR, c.t,
                               skipN ? s.talkSkip : (doorCool ? &s.doorUsedId : nullptr),
                               skipN ? skipN : (doorCool ? 1 : 0), 0);   // 0.17.6: mai il compagno
    if (!ok && skipN > 0) {                          // finiti i candidati: si ricomincia dal migliore
        c.restart = true;
        ok = MemoryFindTarget(true, f.centerX, f.centerY, s.dirX, s.dirY, range, nearR, c.t, nullptr, 0, 0);
    }
    return ok;
}

void SetFocus(Loop& s, const Target& t, const char* why)
{
    if (!s.focusOn || s.focus.id != t.id) {
        char type[64] = "";
        MemoryTypeName(t.addr, type, sizeof(type));
        Log("Bersaglio a fuoco: %s%s%s id %lu tipo %d %s (%s)", t.kind, t.name[0] ? " " : "", t.name, t.id, t.cls, type, why);
    }
    if (!s.focusOn || s.focus.id != t.id) s.focusLift = -1.0f;   // ancora da leggere
    s.focus = t;
    s.focusOn = true;
}

void DropFocus(Loop& s, const char* why)
{
    if (s.focusOn) Log("Bersaglio a fuoco lasciato: id %lu (%s)", s.focus.id, why);
    s.focusOn = false;
}

constexpr DWORD kDoorCooldownMs = 8000;
constexpr DWORD kStickDoorMs = 2000;
constexpr DWORD kHoverWaitMs = 300;

// Ogni tick: posizione del bersaglio per l'indicatore; ogni 60 ms: cambio o perdita del bersaglio.
void FocusTick(Loop& s, const Frame& f)
{
    // 0.56.18: chiusa una finestra o un dialogo con un PNG a fuoco, quel PNG passa in secondo piano per 2 s (0.56.19) e il bersaglio si
    // lascia: se poi ci si sposta verso un barile, A apre il barile e non di nuovo il mercante
    {
        static int lastUi = 0;
        const int ui = MemoryUIState();
        if (lastUi == 1 && ui == 0 && s.focusOn && s.focus.cls >= 0 && s.focus.cls != 4) {
            MemoryNoteUsedNpc(s.focus.id);
            DropFocus(s, "finestra o dialogo chiusi");
        }
        if (ui >= 0) lastUi = ui;
    }
    const bool doorCool = static_cast<LONG>(f.now - s.doorUsedUntil) < 0;
    const DWORD* skip = doorCool ? &s.doorUsedId : nullptr;
    const int nSkip = doorCool ? 1 : 0;
    if (doorCool && s.focusOn && s.focus.id == s.doorUsedId) DropFocus(s, "porta appena usata");
    if (!s.cursorMode) for (int k = 0; k < 4; ++k) OverlaySetGrid(k, false, 0, 0, 0, 0);
    if (!g_cfg.lockEnabled || s.cursorMode) { OverlaySetMarker(false, 0, 0); OverlaySetMarkerGround(1, false, 0, 0); return; }
    if (s.focusOn && !MemoryTrackTarget(s.focus)) DropFocus(s, "non piu' visibile");
    // 0.14.1: il cavallo sotto l'eroe vuol dire in sella (stessa regola della scelta, 20 pixel):
    // niente icona per montare.
    if (s.focusOn && s.focus.cls == 4 && std::hypot(s.focus.sx - f.centerX, s.focus.sy - f.centerY) < 0.04f * f.hf)
        DropFocus(s, "in sella");
    if (f.now - s.focusAt >= 60) {
        s.focusAt = f.now;
        const float farR = g_cfg.lockRange * f.hf;
        if (s.focusOn && (!MemoryTargetValid(s.focus) ||
                          std::hypot(s.focus.sx - f.centerX, s.focus.sy - f.centerY) > farR * 1.3f))
            DropFocus(s, "usato o troppo lontano");
        Target t;
        if (f.aiming) {                               // stick destro: il piu' vicino al mirino
            const float aimBody = g_cfg.lockOffsetY * f.hf;
            if (MemoryFindTarget(true, static_cast<float>(f.aimX), f.aimY + aimBody, 0, 0, 0.10f * f.hf, 0, t, skip, nSkip, 0, false, false, true))
                SetFocus(s, t, "mirino");                // 0.56.18: col mirino anche i PNG generici (scelta voluta)
        }
        else if (f.moving) {                          // stick sinistro: cio' che sta davanti
            // Il bersaglio attuale e' ancora davanti? (coseno con la direzione dello stick)
            float cur = -2.0f;
            if (s.focusOn) {
                const float vx = s.focus.sx - f.centerX, vy = s.focus.sy - f.centerY;
                const float d = std::hypot(vx, vy), ml = std::hypot(f.mdx, f.mdy);
                cur = (d > 1.0f && ml > 0.1f) ? (vx * f.mdx + vy * f.mdy) / (d * ml) : 1.0f;
            }
            const float cosKeep = std::cos(25.0f * 3.14159265f / 180.0f);   // ancora "guardato"
            // 0.11.9: entro 0.06 dell'altezza un candidato vale in ogni direzione (l'elfa col "!"
            // sta quasi addosso all'eroe e il cono non la prendeva mai, prova delle 15:22).
            if (MemoryFindTarget(true, f.centerX, f.centerY, f.mdx, f.mdy, farR, 0.06f * f.hf, t, skip, nSkip, 0, true)) {
                // Isteresi: il bersaglio attuale resta finche' e' davanti, salvo che il nuovo abbia
                // un'icona e lui no (alle 15:22:22 oscillava fra forziere 1363 e oggetto 992).
                const bool keep = s.focusOn && t.id != s.focus.id && cur >= cosKeep;
                if (!keep) SetFocus(s, t, "direzione dello stick");
            }
            else if (s.focusOn && cur < std::cos(g_cfg.lockAngle * 1.5f * 3.14159265f / 180.0f))
                DropFocus(s, "fuori dalla direzione");    // non lo si sta piu' guardando
        }
        else if (!s.focusOn) {                        // fermi e senza bersaglio: uno, una volta
            const float nearR = g_cfg.talkRange * f.hf;
            if (MemoryFindTarget(true, f.centerX, f.centerY, s.dirX, s.dirY, nearR, nearR, t, skip, nSkip, 0))
                SetFocus(s, t, "vicino, da fermi");
        }
    }
    if (s.focusOn && s.focusAt == f.now) {            // cima del rettangolo del gioco (ogni 60 ms)
        RECT r;
        s.focusLift = MemoryTargetRect(s.focus.id, r) ? s.focus.sy - r.top + 0.02f * f.hf : 0.17f * f.hf;
    }
    const int icon = s.focus.cls == 4 ? 2 : s.focus.cls >= 0 ? 1 : 0;   // cavallo, persona, oggetto
    if (s.focusOn) OverlaySetMarker(true, static_cast<LONG>(s.focus.sx), static_cast<LONG>(s.focus.sy - (s.focusLift < 0 ? 0.17f * f.hf : s.focusLift) - 0.02f * f.hf), icon);
    else OverlaySetMarker(false, 0, 0);
    // 0.56.18: alone a terra sotto il bersaglio; non per le porte (il loro punto e' alzato a meta' porta: resta l'icona)
    if (s.focusOn && !strstr(s.focus.kind, "porta")) OverlaySetMarkerGround(1, true, static_cast<LONG>(s.focus.sx), static_cast<LONG>(s.focus.sy));
    else OverlaySetMarkerGround(1, false, 0, 0);
}

bool FindTalk(Loop& s, const Frame& f, Target& out)
{
    s.talkSame = false;
    TalkChoice c;
    const bool ok = ChooseTalk(s, f, c);
    if (!c.cycle || c.restart) s.talkSkipN = 0;
    if (!ok) return false;
    out = c.t;
    if (c.same) {
        s.talkPick = c.t;
        s.talkPickDist = c.d;
        s.talkLast = f.now;
        s.talkSame = true;
        return true;
    }
    if (s.talkSkipN == static_cast<int>(sizeof(s.talkSkip) / sizeof(s.talkSkip[0]))) s.talkSkipN = 0;
    s.talkSkip[s.talkSkipN++] = out.id;
    s.talkLast = f.now;
    s.talkImportant = strstr(out.kind, "(dialogo)") || strstr(out.kind, "(servizio)") ||
                       strstr(out.kind, "(icona)");
    if (out.id != s.talkRepeatId) { s.talkRepeatId = out.id; s.talkRepeats = 0; }
    s.talkPick = Target{};
    s.talkDialogSeen = false;
    float d = 0;
    if (MemoryHeroDistance(out, d)) { s.talkPick = out; s.talkPickDist = s.talkPickMin = d; }
    return true;
}

// B tenuto: clic sull'icona del cavallo accanto al ritratto in alto a destra, che in Sacred
// richiama il cavallo e fa salire o scendere (0.9.6; nella 0.9.3 era Shift+clic sul terreno,
// che non richiamava il cavallo). Posizione dall'F7 del 22/10 alle 20:24, mouse in (1885,106)
// a 1920x1080: l'interfaccia non scala ed e' ancorata al bordo destro. Il clic va in tre tempi
// (puntatore sull'icona, giu' dopo 60 ms, su dopo altri 60) come per i pulsanti dei dialoghi;
// nel frattempo movimento e attacchi sono sospesi.
void StartHorseIcon(Loop& s, const Frame& f)
{
    // 0.17.5: senza cavallo l'icona non c'e' e il clic nel punto fisso (f.w - 35, 106) cadeva sul terreno:
    // l'eroe andava verso l'alto a destra. Ora si clicca l'icona solo se il gioco la mostra, al suo centro.
    LONG px = 0, py = 0;
    if (!MemoryHorseIcon(px, py)) { Log("B tenuto: l'eroe non ha un cavallo, niente clic"); return; }
    if (s.moveHeld) { SendLeftUp(s.hwnd); s.moveHeld = false; }
    if (!g_virt.active.load()) {
        POINT p{ px, py };
        ClientToScreen(s.hwnd, &p);
        Activate(p);
    }
    SetVirtualClient(s, px, py, f.w, f.h);
    SendMove(s.hwnd);
    s.dismountStep = 1;
    s.dismountAt = f.now + 60;
    Log("B tenuto: clic sull'icona del cavallo in (%ld,%ld)", px, py);
}

// Priorita' (0.9.2): un nemico oltre LockNear non vale piu' di un NPC, forziere o oggetto
// vicino all'eroe nella direzione dello stick (lo stipo delle 20:30 nella prova lunga).
bool PreferInteraction(Loop& s, const Frame& f, const Target& enemy, bool log = true)
{
    const float dE = std::hypot(enemy.sx - f.centerX, enemy.sy - f.centerY);
    if (dE <= g_cfg.lockNear * f.hf) return false;
    Target o;
    const float lim = 0.75f * g_cfg.lockNear * f.hf;
    if (!MemoryFindTarget(true, f.centerX, f.centerY, s.dirX, s.dirY, lim, 0.06f * f.hf, o)) return false;
    const float dT = std::hypot(o.sx - f.centerX, o.sy - f.centerY);
    if (dT * 2.0f >= dE) return false;
    if (log) Log("Priorita': %s id %lu a %.0f px invece del nemico id %lu a %.0f px", o.kind, o.id, dT, enemy.id, dE);
    return true;
}

// --- Anteprima del bersaglio (0.10.0, voce 3 della sezione 13, livello 1) ---------
// Da fermi, senza clic in corso ne' mirino, ogni 120 ms il puntatore virtuale si posa su cio'
// che A sceglierebbe: il gioco lo evidenzia da solo (nome, cursore d'interazione o di
// combattimento), cosi' si vede chi A prendera' prima di premerlo. La scelta e' la stessa di
// PrepareTarget senza mouse ne' mirino (nemico, poi NPC/forziere/oggetto/cavallo, con il giro di
// A ripetuto) e non cambia lo stato del giro. Senza bersaglio il puntatore va dove A colpirebbe
// (mira a distanza) o resta dov'e'. Il puntatore senza pulsanti premuti non fa agire l'eroe.
constexpr DWORD kSettleMs = 100, kAfterMs = 150;

// Il puntatore virtuale e' ora sopra un oggetto che il gioco sceglierebbe al clic?
bool OnObjectNow(const Loop& s, const Frame& f)
{
    const POINT p = VirtualClient(s.hwnd);
    return MemoryPointOnCreature(static_cast<float>(p.x), static_cast<float>(p.y), f.hf);
}

void PreviewTick(Loop& s, const Frame& f)
{
    if (!g_cfg.previewTarget || !g_cfg.lockEnabled || !g_virt.active.load()) return;
    if (f.now - s.previewAt < 120) return;
    if (static_cast<LONG>(f.now - s.stillUntil) < 0) return;   // subito dopo un clic posato
    // Nella 0.11.3 l'anteprima restava ferma 800 ms dopo ogni clic (il 23/10 alle 13:31:33 e
    // 13:31:59 il negozio si era aperto 100-180 ms dopo che l'anteprima aveva portato il puntatore
    // sul mercante, subito dopo un clic su un altro bersaglio), ma con quella pausa i clic di A
    // non facevano piu' muovere l'eroe ne' parlare (prova delle 13:51): tolta, causa da capire.
    s.previewAt = f.now;
    Target t, e;
    const char* what = nullptr;
    POINT p = VirtualClient(s.hwnd);
    bool enemy = FindLock(s, f, false, e);
    if (enemy && !InCombat(s, f) && PreferInteraction(s, f, e, false)) enemy = false;
    TalkChoice c;
    if (enemy) { t = e; what = "nemico"; p = LockPoint(e, f); }
    else if (ChooseTalk(s, f, c)) { t = c.t; what = t.kind; p = LockPoint(t, f); }
    else if (f.ranged) {
        const float r = g_cfg.rangedDefault * f.hf;
        p = POINT{ static_cast<LONG>(f.centerX + s.dirX * r), static_cast<LONG>(f.centerY + s.dirY * r) };
    }
    SetVirtualClient(s, p.x, p.y, f.w, f.h);
    MoveIfChanged(s);
    // Il mouse vero resta dov'e' (0.10.2). Nella 0.10.1 l'anteprima lo portava sul bersaglio, ma
    // l'evidenziazione del gioco non compariva lo stesso, e il clic con cui riparte il movimento
    // finiva sul personaggio sotto il mouse vero: si aprivano da soli dialoghi e la finestra dello
    // stalliere (prova del 23/10, 01:11:06-01:11:14).
    const DWORD id = what ? t.id + 1 : 0;           // +1: anche l'id 0 conta come bersaglio
    if (id != s.previewId) {
        s.previewId = id;
        if (what) Log("Anteprima: %s%s%s id %lu tipo %d in (%ld,%ld)", what, t.name[0] ? " " : "", t.name, t.id, t.cls, p.x, p.y);
        else      Log("Anteprima: nessun bersaglio");
    }
}

// Sceglie dove cliccare per A o per un'arte e prepara il puntatore.
// plain: true se il clic deve essere senza Ctrl (punto indicato col mouse o bersaglio agganciato).
POINT PrepareTarget(Loop& s, const Frame& f, const char*& how, bool* plain = nullptr, bool allowTalk = false)
{
    const bool fromMouse = !g_virt.active.load();
    const bool mousePointed = fromMouse && !f.aiming;
    if (plain) *plain = mousePointed;
    s.locked = false;
    s.lockSearch = !mousePointed;             // falso anche per NPC e oggetti, qui sotto
    if (fromMouse) Activate(f.real);                       // riparte da dove puntava il mouse
    if (s.moveHeld) { SendLeftUp(s.hwnd); s.moveHeld = false; }

    static char desc[96];
    POINT target = VirtualClient(s.hwnd);
    how = "punto del puntatore";
    Target t;
    // Con lo stick destro inclinato si sceglie la creatura piu' vicina al mirino
    // (sul corpo, poco sopra i piedi): utile fra piu' persone vicine.
    const float aimRange = 0.10f * f.hf, aimBody = g_cfg.lockOffsetY * f.hf;
    bool aimPicked = false;
    if (!mousePointed && f.aiming) {
        if (MemoryFindTarget(false, static_cast<float>(f.aimX), f.aimY + aimBody, 0, 0, aimRange, 0, t)) {
            s.lock = t; s.locked = true; aimPicked = true;
            target = LockPoint(t, f);
            _snprintf_s(desc, sizeof(desc), _TRUNCATE, "mirino: nemico id %lu tipo %d", t.id, t.cls);
            how = desc;
            if (plain) *plain = true;
        }
        else if (allowTalk && MemoryFindTarget(true, static_cast<float>(f.aimX), f.aimY + aimBody, 0, 0, aimRange, 0, t,
                                               nullptr, 0, 2, false, false, true)) {   // 0.56.18: col mirino anche i PNG generici
            aimPicked = true;
            s.lockSearch = false;
            target = LockPoint(t, f);
            _snprintf_s(desc, sizeof(desc), _TRUNCATE, "mirino: %s id %lu tipo %d", t.kind, t.id, t.cls);
            how = desc;
            if (plain) *plain = true;
        }
    }
    bool enemy = !aimPicked && !mousePointed && FindLock(s, f, false, t);
    if (enemy && allowTalk && !InCombat(s, f) && PreferInteraction(s, f, t)) enemy = false;
    if (aimPicked) {}
    else if (enemy) {
        s.lock = t;
        s.locked = true;
        target = LockPoint(t, f);
        _snprintf_s(desc, sizeof(desc), _TRUNCATE, "aggancio: nemico id %lu tipo %d", t.id, t.cls);
        how = desc;
        if (plain) *plain = true;
    }
    else if (!mousePointed && allowTalk && FindTalk(s, f, t)) {
        s.lockSearch = false;
        SetFocus(s, t, "A");
        if (strncmp(t.kind, "porta", 5) == 0) {
            s.doorUsedId = t.id; s.doorUsedUntil = f.now + kDoorCooldownMs;
            s.stickDoorId = t.id; s.stickDoorUntil = f.now + kStickDoorMs;
        }
        target = LockPoint(t, f);
        if (t.id & 0x80000000u) MemoryMarkFixedUsed(t.id);   // 0.55.38: nascondiglio magico o portale, una volta
        _snprintf_s(desc, sizeof(desc), _TRUNCATE, "aggancio: %s%s%s id %lu tipo %d%s", t.kind, t.name[0] ? " " : "",
                    t.name, t.id, t.cls,
                    s.talkSame ? " (stesso, l'eroe ci sta andando)" : s.talkSkipN > 1 ? " (successivo)" : "");
        how = desc;
        if (plain) *plain = true;
    }
    else if (f.aiming) {
        target = POINT{ f.aimX, f.aimY };
        how = "mira dello stick destro";
    }
    else if (fromMouse) {
        how = "punto indicato col mouse";
    }
    else if (f.ranged) {
        float r = g_cfg.rangedDefault * f.hf;
        target = POINT{ static_cast<LONG>(f.centerX + s.dirX * r), static_cast<LONG>(f.centerY + s.dirY * r) };
        how = "direzione dell'ultimo movimento";
    }
    SetVirtualClient(s, target.x, target.y, f.w, f.h);
    return VirtualClient(s.hwnd);
}

void ClickDown(Loop& s, Click c, const Frame& f)
{
    // Ctrl ("fermati e attacca") serve quando il clic cade sul terreno; con il
    // punto indicato dal mouse/trackpad si usa il clic normale, cosi' su un NPC
    // si parla invece di attaccarlo (solo per A).
    // Lo stesso per un bersaglio agganciato: il clic cade sulla creatura.
    const bool plain = (c == Click::Left) ? s.plainA : s.plainArt;
    if (f.ranged && !plain) HoldKey(s.hwnd, kCtrl, true);
    SendMove(s.hwnd);
    if (c == Click::Left) SendLeftDown(s.hwnd); else SendRightDown(s.hwnd);
    s.held = c;
    s.clickPt = VirtualClient(s.hwnd);
    s.clickTime = f.now;
}

void ClickUp(Loop& s)
{
    if (s.held == Click::Left)  SendLeftUp(s.hwnd);
    if (s.held == Click::Right) SendRightUp(s.hwnd);
    HoldKey(s.hwnd, kCtrl, false);
    s.held = Click::None;
    s.heldMask = 0;
}

// Con il clic tenuto: il punto segue il mouse vero, lo stick destro o lo stick
// sinistro (in quest'ordine) e, se si sposta abbastanza, il clic viene ripetuto.
void UpdateHeldClick(Loop& s, const Frame& f)
{
    bool switched = false;
    if (s.locked) {
        Target t;
        if (f.mouseMoved) {
            s.locked = false;
            Log("Aggancio annullato: mouse mosso");
        }
        else if (f.aiming && f.now - s.lockScan >= 150) {
            // Lo stick destro sceglie un altro bersaglio nella sua direzione.
            s.lockScan = f.now;
            if (FindLock(s, f, false, t) && t.id != s.lock.id) {
                s.lock = t;
                switched = true;
                Log("Aggancio: nuovo bersaglio con lo stick destro, id %lu tipo %d", t.id, t.cls);
            }
            else if (!MemoryTrackTarget(s.lock)) s.locked = false;
        }
        else if (!MemoryTrackTarget(s.lock)) {
            // Bersaglio morto o sparito: il successivo nella stessa direzione.
            if (FindLock(s, f, false, t)) {
                s.lock = t;
                switched = true;
                Log("Aggancio: bersaglio perso, nuovo bersaglio id %lu tipo %d", t.id, t.cls);
            }
            else {
                s.locked = false;
                Log("Aggancio: bersaglio perso, nessun altro nemico");
                if (s.held == Click::Left && f.ranged && s.plainA) {   // si torna a "fermati e attacca"
                    s.plainA = false;
                    HoldKey(s.hwnd, kCtrl, true);
                }
            }
        }
    }
    // A tenuto senza bersaglio (morto senza altri nemici in quella direzione, o nessuno
    // alla pressione): ogni 200 ms si cerca un nemico nella direzione dello stick (0.9.2).
    if (!s.locked && s.lockSearch && s.held == Click::Left && s.heldMask == PAD_A && !f.mouseMoved &&
        (f.moving || f.aiming) && f.now - s.lockScan >= 200) {
        s.lockScan = f.now;
        Target t;
        if (FindLock(s, f, false, t)) {
            s.lock = t;
            s.locked = true;
            switched = true;
            if (!s.plainA) { s.plainA = true; HoldKey(s.hwnd, kCtrl, false); }   // clic normale sulla creatura
            Log("Aggancio: nuovo bersaglio con A tenuto nella direzione dello stick, id %lu tipo %d", t.id, t.cls);
        }
    }
    if (s.locked) {
        POINT lp = LockPoint(s.lock, f);
        SetVirtualClient(s, lp.x, lp.y, f.w, f.h);
    }
    else if (f.mouseMoved) {
        g_virt.x.store(f.real.x);
        g_virt.y.store(f.real.y);
    }
    else if (f.aiming) {
        SetVirtualClient(s, f.aimX, f.aimY, f.w, f.h);
    }
    else if (f.moving) {
        float r = (f.ranged ? g_cfg.rangedDefault : g_cfg.moveRadius) * f.hf;
        SetVirtualClient(s, static_cast<LONG>(f.centerX + f.mdx * r), static_cast<LONG>(f.centerY + f.mdy * r), f.w, f.h);
    }
    MoveIfChanged(s);
    POINT p = VirtualClient(s.hwnd);
    LONG dx = p.x - s.clickPt.x, dy = p.y - s.clickPt.y;
    float limit = g_cfg.reclickDistance * f.hf;
    const bool moved = switched ||
                       (static_cast<float>(dx * dx + dy * dy) > limit * limit && f.now - s.clickTime >= 120);
    // Con A tenuto Sacred continua solo sul primo bersaglio: un clic periodico
    // fa ripartire l'attacco quando il bersaglio muore o sparisce.
    // Non serve con un bersaglio agganciato o con il clic normale (NPC, punto del mouse).
    const bool periodic = s.held == Click::Left && !s.locked && !s.plainA && g_cfg.holdReclickMs > 0 &&
                          f.now - s.clickTime >= static_cast<DWORD>(g_cfg.holdReclickMs);
    if (moved || periodic) {
        Click c = s.held;
        if (c == Click::Left) { SendLeftUp(s.hwnd); SendLeftDown(s.hwnd); }
        else                  { SendRightUp(s.hwnd); SendRightDown(s.hwnd); }
        s.clickPt = p;
        s.clickTime = f.now;
        if (moved) Log("Nuovo %s sulla mira spostata (%ld,%ld)", c == Click::Left ? "clic" : "clic destro", p.x, p.y);
    }
}

// Pulsanti delle arti: maschera del pad -> slot 1..5 (tasti 6, 7, 8, 9, 0).
struct ArtButton { WORD mask; int slot; const char* name; };
const ArtButton kArts[] = {
    { PAD_X,  1, "X" }, { PAD_Y, 2, "Y" }, { PAD_RB, 3, "RB" }, { PAD_RT, 4, "RT" }, { PAD_LT, 5, "LT" },
};
const Key& ArtKey(int slot) { return kDigit[(slot + 5) % 10]; }   // 1->6 ... 4->9, 5->0

// --- Modalita' cursore ---------------------------------------------------------
void SyncCursorFromVirtual(Loop& s)
{
    POINT p = VirtualClient(s.hwnd);
    s.cx = static_cast<float>(p.x);
    s.cy = static_cast<float>(p.y);
}

void SetCursorMode(Loop& s, const Frame& f, bool on, const char* why)
{
    if (s.cursorMode == on) return;
    ReleaseAll(s, on ? "entrata in modalita' cursore" : "uscita dalla modalita' cursore");
    s.cursorMode = on;
    OverlaySetCursorMode(on);
    if (on) {
        if (!g_virt.active.load()) Activate(f.real);    // parte da dove si trova il mouse
        SyncCursorFromVirtual(s);
        SendMove(s.hwnd);
    }
    Log("Modalita' cursore %s (%s)", on ? "ATTIVA" : "disattivata", why);
}

// 0.51.3: aiuto (Esc -> Controller o Start tenuto) del giocatore 1 o 2. Con quello del giocatore 2 Core usa lo stato del
// pad 2 (sostituzione in Tick, come per il puntatore) e il disegno usa il suo controller; si parte dalla sua riga.
int g_helpOwner = 1;
void OpenHelp(Loop& s, int owner, const char* why)
{
    s.helpOpen = true;
    g_helpOwner = owner == 2 ? 2 : 1;
    ReleaseAll(s, "aiuto aperto");
    OverlaySetHelpOwner(g_helpOwner);
    NativePad2SetInHelp(g_helpOwner == 2);
    s.helpRow = (g_helpOwner == 2 && !g_cfg.coopRole) ? 1 : 0;   // 0.51.5: righe 0 giocatore 1, 1 giocatore 2, 2 vibrazione
    OverlaySetHelpRow(s.helpRow);
    s.helpPage = 0;
    OverlaySetHelpPage(0);
    OverlaySetHelpButtons(0);
    OverlaySetHelp(true);
    Log("Aiuto aperto (%s, giocatore %d)", why, g_helpOwner);
}
void CloseHelp(Loop& s, const char* why)
{
    s.helpOpen = false;
    OverlaySetHelp(false);
    OverlaySetHelpButtons(0);
    NativePad2SetInHelp(false);
    g_helpOwner = 1;
    OverlaySetHelpOwner(1);
    Log("Aiuto chiuso (%s)", why);
}

// Stick -> spostamento: zona morta, curva di risposta, velocita' in pixel/secondo.
void StickVelocity(float x, float y, float deadzone, float speed, float& vx, float& vy)
{
    vx = vy = 0;
    float mag = std::sqrt(x * x + y * y);
    if (mag <= deadzone) return;
    float t = (mag - deadzone) / (1.0f - deadzone);
    if (t > 1.0f) t = 1.0f;
    float k = std::pow(t, g_cfg.cursorCurve) * speed / mag;
    vx = x * k;
    vy = -y * k;                          // stick in alto = schermo in alto
}

// Zaino aperto (0.12.0): la croce sposta il cursore di casella in casella, saltando le altre
// caselle dello stesso oggetto; il cursore va al centro dell'oggetto o della casella vuota.
// Fuori dalla griglia la prima pressione porta sulla casella in alto a sinistra.
// Dal 0.12.2 piu' griglie (merce del mercante e zaino): oltre il bordo di una griglia il cursore
// passa alla griglia vicina in quella direzione, nella stessa colonna o riga.
// Dal 0.18.3, con controls: oltre il bordo si passa a un'altra griglia solo se allineata (stesse righe
// per sinistra/destra, stesse colonne per su/giu'); se no, e c'e' un comando in quella direzione
// (incastonature e martello del fabbro, equipaggiamento...), ci si ferma e decide MenuStep; altrimenti
// la griglia piu' vicina in quella direzione come prima (nel fabbro destra portava sempre allo zaino).
bool GridStep(Loop& s, const Frame& f, const GridInfo* gs, int n, WORD pressed, const std::vector<MenuItem>* controls)
{
    s.gridBlocked = false;
    int dc = 0, dr = 0;
    if (pressed & PAD_DPAD_LEFT)  dc = -1;
    if (pressed & PAD_DPAD_RIGHT) dc = 1;
    if (pressed & PAD_DPAD_UP)    dr = -1;
    if (pressed & PAD_DPAD_DOWN)  dr = 1;
    if ((!dc && !dr) || n < 1) return false;
    auto inside = [&](const GridInfo& g, float x, float y) {
        return x >= g.x && y >= g.y && x < g.x + g.cols * 32 && y < g.y + g.rows * 32;
    };
    int gi = -1;
    for (int k = 0; k < n; ++k) if (inside(gs[k], s.cx, s.cy)) gi = k;
    int col = 0, row = 0;
    if (gi < 0) {                                        // fuori: la griglia piu' vicina, prima casella
        float best = 1e9f;
        for (int k = 0; k < n; ++k) {
            const float d = std::hypot(gs[k].x - s.cx, gs[k].y - s.cy);
            if (d < best) { best = d; gi = k; }
        }
    }
    else {
        const GridInfo& g = gs[gi];
        col = static_cast<int>((s.cx - g.x) / 32.0f);
        row = static_cast<int>((s.cy - g.y) / 32.0f);
        int cur = -1, cw = 1, ch = 1;
        if (!MemoryGridCell(g, row * g.cols + col, cur, cw, ch)) cur = -1;
        for (;;) {
            const int nc = col + dc, nr = row + dr;
            if (nc < 0 || nr < 0 || nc >= g.cols || nr >= g.rows) {
                // Bordo: griglia vicina in quella direzione, alla stessa x (o y).
                const float px = g.x + col * 32 + 16.0f, py = g.y + row * 32 + 16.0f;
                int next = -1, loose = -1; float bestD = 1e9f, looseD = 1e9f;
                for (int k = 0; k < n; ++k) {
                    if (k == gi) continue;
                    const GridInfo& o = gs[k];
                    const float ocx = o.x + o.cols * 16.0f, ocy = o.y + o.rows * 16.0f;
                    if ((dr < 0 && ocy >= g.y) || (dr > 0 && ocy <= g.y + g.rows * 32) ||
                        (dc < 0 && ocx >= g.x) || (dc > 0 && ocx <= g.x + g.cols * 32)) continue;
                    const float d = std::hypot(ocx - px, ocy - py);
                    const bool aligned = dr ? (px >= o.x && px < o.x + o.cols * 32) : (py >= o.y && py < o.y + o.rows * 32);
                    if (aligned && d < bestD) { bestD = d; next = k; }
                    if (d < looseD) { looseD = d; loose = k; }
                }
                if (next < 0 && controls) {
                    for (const MenuItem& it : *controls) {
                        const float vx = it.x + it.w / 2.0f - px, vy = it.y + it.h / 2.0f - py;
                        const float along = vx * dc + vy * dr, side = std::fabs(vx * dr - vy * dc);
                        if (along >= 4.0f && side <= along * 2.0f) { s.gridBlocked = true; break; }
                    }
                    if (s.gridBlocked) break;                    // decide MenuStep
                }
                if (next < 0) next = loose;
                if (next < 0) { s.gridBlocked = true; break; }   // nessuna griglia: si resta (o equipaggiamento)
                const GridInfo& o = gs[next];
                gi = next;
                col = dr ? static_cast<int>((px - o.x) / 32.0f) : (dc > 0 ? 0 : o.cols - 1);
                row = dc ? static_cast<int>((py - o.y) / 32.0f) : (dr > 0 ? 0 : o.rows - 1);
                col = col < 0 ? 0 : (col >= o.cols ? o.cols - 1 : col);
                row = row < 0 ? 0 : (row >= o.rows ? o.rows - 1 : row);
                break;
            }
            col = nc; row = nr;
            int a2 = -1, aw = 1, ah = 1;
            if (!MemoryGridCell(g, row * g.cols + col, a2, aw, ah)) a2 = -1;
            if (a2 >= 0 && a2 == cur) continue;                                // stesso oggetto
            break;
        }
    }
    const GridInfo& g = gs[gi];
    int anchor = -1, w = 1, h = 1;
    float tx = g.x + col * 32 + 16.0f, ty = g.y + row * 32 + 16.0f;
    if (MemoryGridCell(g, row * g.cols + col, anchor, w, h)) {            // centro dell'oggetto
        tx = g.x + (anchor % g.cols) * 32 + w * 16.0f;
        ty = g.y + (anchor / g.cols) * 32 + h * 16.0f;
    }
    else anchor = -1;
    if (!g_virt.active.load()) { Activate(f.real); SyncCursorFromVirtual(s); }
    s.cx = tx; s.cy = ty;
    SetVirtualClient(s, static_cast<LONG>(tx), static_cast<LONG>(ty), f.w, f.h);
    SendMove(s.hwnd);
    Log("%s: casella (%d,%d)%s -> cursore (%.0f,%.0f)", g.kind, col, row, anchor >= 0 ? " con oggetto" : "", tx, ty);
    return true;
}

// Menu (0.12.3): la croce porta il cursore al pulsante piu' vicino nella direzione premuta
// (distanza lungo la direzione piu' due volte lo scarto laterale); se non ce n'e', al piu' vicino.
void StartMacroClick(Loop& s, const Frame& f, LONG x, LONG y, bool shift, bool restore);

bool MenuStep(Loop& s, const Frame& f, const std::vector<MenuItem>& items, WORD pressed, bool* moved = nullptr)
{
    if (moved) *moved = false;
    float dx = 0, dy = 0;
    if (pressed & PAD_DPAD_LEFT)  dx = -1;
    if (pressed & PAD_DPAD_RIGHT) dx = 1;
    if (pressed & PAD_DPAD_UP)    dy = -1;
    if (pressed & PAD_DPAD_DOWN)  dy = 1;
    if ((!dx && !dy) || items.empty()) return false;
    // Barra (0.12.8): con il cursore sopra, sinistra/destra la spostano del 10% con un clic.
    if (dx && !dy) {
        for (const MenuItem& it : items) {
            if (!it.slider || s.cx < it.x || s.cx >= it.x + it.w || s.cy < it.y || s.cy >= it.y + it.h) continue;
            float nx = s.cx + dx * it.w / 10.0f;
            if (nx < it.x + 1.0f) nx = it.x + 1.0f;
            if (nx > it.x + it.w - 1.0f) nx = it.x + it.w - 1.0f;
            s.cx = nx;
            StartMacroClick(s, f, static_cast<LONG>(nx), static_cast<LONG>(s.cy), false, false);
            Log("Menu: barra \"%s\" %s, clic in (%.0f,%.0f)", it.name, dx > 0 ? "aumentata" : "diminuita", nx, s.cy);
            return true;
        }
    }
    const MenuItem* best = nullptr;
    float bestS = 1e9f;
    const MenuItem* nearest = nullptr;
    float nearD = 1e9f;
    bool onItem = false;
    for (const MenuItem& it : items)
        if (s.cx >= it.x && s.cx < it.x + it.w && s.cy >= it.y && s.cy < it.y + it.h) onItem = true;
    for (const MenuItem& it : items) {
        const float cx = it.x + it.w / 2.0f, cy = it.y + it.h / 2.0f;
        const float vx = cx - s.cx, vy = cy - s.cy;
        const float d = std::hypot(vx, vy);
        if (d < nearD) { nearD = d; nearest = &it; }
        if (s.cx >= it.x && s.cx < it.x + it.w && s.cy >= it.y && s.cy < it.y + it.h) continue;   // quello attuale
        const float along = vx * dx + vy * dy, side = std::fabs(vx * dy - vy * dx);
        if (along < 4.0f || side > along * 2.0f) continue;      // non in quella direzione
        const float sc = along + 2.0f * side;
        if (sc < bestS) { bestS = sc; best = &it; }
    }
    const MenuItem* to = best ? best : (onItem ? nullptr : nearest);
    if (!to) return true;                                         // al bordo: si resta
    if (moved) *moved = true;
    const float tx = to->x + to->w / 2.0f, ty = to->y + to->h / 2.0f;
    if (!g_virt.active.load()) { Activate(f.real); SyncCursorFromVirtual(s); }
    s.cx = tx; s.cy = ty;
    SetVirtualClient(s, static_cast<LONG>(tx), static_cast<LONG>(ty), f.w, f.h);
    SendMove(s.hwnd);
    char cn[40] = "";
    Log("Menu: pulsante %s \"%s\" (%d,%d) %dx%d -> cursore (%.0f,%.0f)", to->vt == 0x896308 ? "Button2" :
        to->vt == 0x89633c ? "RadioButton2" : to->vt == 0x8963c0 ? "Combobox" : to->vt == 0x896700 ? "StaticText64" :
        to->vt == 0 ? "punto fisso" : "StaticText64FX",
        to->name, to->x, to->y, to->w, to->h, tx, ty);
    (void)cn;
    return true;
}

void CursorTick(Loop& s, const Frame& f, const PadState& pad, WORD pressed, WORD released)
{
    // Elenco "Co-op" della campagna cooperativa aperto (0.47.3): croce su/giu' sceglie, A conferma, B annulla; il resto
    // del pad non tocca il gioco (lo stick muove comunque il puntatore, che puo' indicare una riga dell'elenco).
    if (g_cfg.nativeMenu && NativeCoopPickerOpen()) {
        const int move = ((pressed & PAD_DPAD_DOWN) ? 1 : 0) - ((pressed & PAD_DPAD_UP) ? 1 : 0);
        if (move || (pressed & (PAD_A | PAD_B))) NativeCoopPickerPad(move, (pressed & PAD_A) != 0, (pressed & PAD_B) != 0);
        // il clic con cui A ha aperto l'elenco va comunque rilasciato (altrimenti il pulsante virtuale resta premuto)
        if ((released & PAD_A) && s.cursorLeft) { SendLeftUp(s.hwnd); s.cursorLeft = false; }
        if ((released & PAD_B) && s.cursorRight) { SendRightUp(s.hwnd); s.cursorRight = false; }
        return;
    }
    // Conferma con due pulsanti (0.17.4, richiesta del tester): "Caricare la partita!" e simili. All'apertura il
    // cursore va su OK; A = OK, B = Annulla (clic sul pulsante, poi il cursore torna dov'era); la croce passa
    // da un pulsante all'altro (pulsanti del menu, come prima).
    {
        static int lastN = 0;
        RECT b[4] = {};
        const int n = MemoryConfirmDlg(b, 4);
        if (n >= 2) {
            if (lastN < 2) {
                s.cx = (b[0].left + b[0].right) / 2.0f; s.cy = (b[0].top + b[0].bottom) / 2.0f;
                if (!g_virt.active.load()) { Activate(f.real); }
                SetVirtualClient(s, static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), f.w, f.h);
                SendMove(s.hwnd);
                Log("Conferma aperta: %d pulsanti, cursore su OK in (%.0f,%.0f)", n, s.cx, s.cy);
            }
            if (pressed & (PAD_A | PAD_B)) {
                // 0.18.1: A clicca il pulsante sotto il cursore (spostato con la croce), OK se non e' su nessuno;
                // nella 0.17.5 A cliccava sempre OK e con il cursore su Annulla il gioco usciva al menu.
                int k = (pressed & PAD_A) ? 0 : n - 1;
                if (pressed & PAD_A)
                    for (int i = 0; i < n; ++i)
                        if (s.cx >= b[i].left && s.cx < b[i].right && s.cy >= b[i].top && s.cy < b[i].bottom) k = i;
                const RECT& r = b[k];
                StartMacroClick(s, f, (r.left + r.right) / 2, (r.top + r.bottom) / 2, false, false);
                Log("Conferma: %s = clic sul pulsante %d di %d (%s)", (pressed & PAD_A) ? "A" : "B", k + 1, n,
                    k == 0 ? "OK" : k == n - 1 ? "Annulla" : "altro");
                lastN = n;
                return;
            }
        }
        lastN = n;
    }
    // 0.56.54: conferma dell'acquisto del mercante (prova del 07/11 alle 16:11:58): A = "Si'" (compra), B = "No", senza portare il
    // cursore sul pulsante. Solo se l'ha aperta il giocatore 1 (quella del giocatore 2 la chiude il suo pad, Pad2Pointer).
    {
        int owner = 0;
        RECT yes{}, no{};
        if ((pressed & (PAD_A | PAD_B)) && !s.mcStep && MemoryPurchaseButtons(owner, yes, no) && owner != 2) {
            const RECT& r = (pressed & PAD_A) ? yes : no;
            const LONG x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
            StartMacroClick(s, f, x, y, false, false);
            Log("Mercante: conferma dell'acquisto, %s = clic su \"%s\" in (%ld,%ld)", (pressed & PAD_A) ? "A" : "B", (pressed & PAD_A) ? "Si'" : "No", x, y);
            return;
        }
    }
    // Griglie (zaino, merce, dal 0.18.0 fabbro, deposito, cubo, scambio): lettura ogni 200 ms (per i "+"
    // di verifica) e a ogni pressione della croce.
    static DWORD gridAt = 0;
    static DWORD gridLogged[4] = {};
    const WORD dpad = PAD_DPAD_LEFT | PAD_DPAD_RIGHT | PAD_DPAD_UP | PAD_DPAD_DOWN;
    if ((pressed & dpad) || f.now - gridAt >= 200) {
        gridAt = f.now;
        GridInfo gs[4];
        int n = 0;
        if (MemoryMerchantGrid(gs[n])) ++n;
        n += MemoryWindowGrids(gs + n, 3 - n);
        if (MemoryHeroGrid(gs[n])) ++n;
        for (int k = 0; k < 4; ++k) {
            if (k < n) {
                const DWORD key = gs[k].entry ^ (static_cast<DWORD>(gs[k].ox) << 16) ^ gs[k].oy;
                if (gridLogged[k] != key) {
                    gridLogged[k] = key;
                    Log("Griglia %s: %dx%d, origine (%d,%d), pagina in (%d,%d)", gs[k].kind, gs[k].cols, gs[k].rows,
                        gs[k].ox, gs[k].oy, gs[k].pageX, gs[k].pageY);
                }
                OverlaySetGrid(k, g_cfg.gridMarkers, gs[k].x, gs[k].y, gs[k].cols, gs[k].rows);
            }
            else { gridLogged[k] = 0; OverlaySetGrid(k, false, 0, 0, 0, 0); }
        }
        // Equipaggiamento (0.13.0): con il cursore su una sua casella la croce va alla casella vicina;
        // senza caselle in quella direzione si torna nella griglia piu' vicina. Dal bordo di una
        // griglia senza altre griglie oltre si entra nell'equipaggiamento.
        std::vector<MenuItem> equip;
        if (n && (pressed & dpad)) {
            MemoryEquipSlots(equip); MemoryStatButtons(equip);      // 0.17.5: "+" della scheda
            MemoryWindowControls(equip, f.w, f.h);                  // 0.18.2: fabbro, maestro
        }
        bool onEquip = false;
        for (const MenuItem& it : equip)
            if (s.cx >= it.x && s.cx < it.x + it.w && s.cy >= it.y && s.cy < it.y + it.h) onEquip = true;
        if (onEquip) {
            bool moved = false;
            MenuStep(s, f, equip, pressed, &moved);
            if (!moved) {
                // Torna alla griglia: la casella della griglia piu' vicina al cursore.
                const GridInfo* bg = nullptr; float bd = 1e9f;
                for (int k = 0; k < n; ++k) {
                    const float gx = gs[k].x + gs[k].cols * 16.0f, gy = gs[k].y + gs[k].rows * 16.0f;
                    const float d = std::hypot(gx - s.cx, gy - s.cy);
                    if (d < bd) { bd = d; bg = &gs[k]; }
                }
                if (bg) {
                    int col = static_cast<int>((s.cx - bg->x) / 32.0f), row = static_cast<int>((s.cy - bg->y) / 32.0f);
                    col = col < 0 ? 0 : (col >= bg->cols ? bg->cols - 1 : col);
                    row = row < 0 ? 0 : (row >= bg->rows ? bg->rows - 1 : row);
                    s.cx = bg->x + col * 32 + 16.0f; s.cy = bg->y + row * 32 + 16.0f;
                    SetVirtualClient(s, static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), f.w, f.h);
                    SendMove(s.hwnd);
                    Log("Equipaggiamento -> %s: casella (%d,%d)", bg->kind, col, row);
                }
            }
            pressed &= static_cast<WORD>(~dpad);
        }
        else if (n && GridStep(s, f, gs, n, pressed, &equip)) {
            if (s.gridBlocked && !equip.empty()) {
                bool moved = false;
                MenuStep(s, f, equip, pressed, &moved);
                if (moved) Log("Zaino -> equipaggiamento");
            }
            pressed &= static_cast<WORD>(~dpad);
        }
        else if (!n && (pressed & dpad)) {
            std::vector<MenuItem> items;
            bool charSelect = false;
            bool any = MemoryMenuItems(items, &charSelect);
            if (MemoryWindowControls(items, f.w, f.h)) any = true;            // 0.18.2: schede Arti/Combo, maestro
            // Scelta del personaggio (0.12.8): i personaggi sono modelli 3D, non pulsanti; con i
            // pulsanti di quella schermata visibili si aggiungono come destinazioni gli 8 punti
            // presi dallo screenshot del 23/10 (1920x1080, scalati sulla finestra).
            if (s.escItemOn) {                           // voce "Controller" sopra il menu Esc
                MenuItem it;
                it.x = s.escItem.left; it.y = s.escItem.top;
                it.w = s.escItem.right - s.escItem.left; it.h = s.escItem.bottom - s.escItem.top;
                strcpy_s(it.name, "SACREDPAD_HELP");
                items.push_back(it);
                any = true;
            }
            if (s.mainRowOn) {                           // voce "Co-op locale" del menu principale
                MenuItem it;
                it.x = s.mainRow.left; it.y = s.mainRow.top;
                it.w = s.mainRow.right - s.mainRow.left; it.h = s.mainRow.bottom - s.mainRow.top;
                strcpy_s(it.name, "SACREDPAD_COOP");
                items.push_back(it);
                any = true;
            }
            if (charSelect) {
                static const POINT kHeroes[8] = { { 190, 780 }, { 410, 560 }, { 680, 600 }, { 1000, 660 },
                                                  { 1185, 540 }, { 1385, 620 }, { 1530, 560 }, { 1680, 760 } };
                for (const POINT& p : kHeroes) {
                    MenuItem it;
                    it.w = 80; it.h = 80;
                    it.x = static_cast<int>(p.x * f.w / 1920.0f) - 40;
                    it.y = static_cast<int>(p.y * f.h / 1080.0f) - 40;
                    strcpy_s(it.name, "personaggio");
                    items.push_back(it);
                }
                any = true;
            }
            static bool noneLogged = false;
            if (!any && !noneLogged) { noneLogged = true; Log("Menu: nessun pulsante trovato nelle finestre aperte"); }
            if (any) noneLogged = false;
            if (any) {
                static size_t lastCount = 0;
                if (items.size() != lastCount) {
                    lastCount = items.size();
                    Log("Menu: %u pulsanti visibili", static_cast<unsigned>(items.size()));
                    for (const MenuItem& it : items)
                        Log("  @%08lX \"%s\" (%d,%d) %dx%d stato %08lX", it.addr, it.name, it.x, it.y, it.w, it.h, it.flags);
                }
                // Salvataggi (0.17.1): croce giu' sull'ultima riga o su sulla prima riga della lista = clic
                // sulla freccia della barra (la lista scorre di una riga), il cursore resta sulla riga.
                const MenuItem* cur = nullptr;
                for (const MenuItem& it : items)
                    if (s.cx >= it.x && s.cx < it.x + it.w && s.cy >= it.y && s.cy < it.y + it.h) cur = &it;
                const bool down = (pressed & PAD_DPAD_DOWN) != 0, up = (pressed & PAD_DPAD_UP) != 0;
                RECT ru{}, rd{};
                if (cur && ((down && strcmp(cur->name, "SAVE_ROW_3") == 0) || (up && strcmp(cur->name, "SAVE_ROW_1") == 0)) &&
                    MemorySaveScroll(ru, rd)) {
                    const RECT& r = down ? rd : ru;
                    StartMacroClick(s, f, (r.left + r.right) / 2, (r.top + r.bottom) / 2, false, true);
                    Log("Salvataggi: lista %s di una riga (clic sulla freccia)", down ? "giu'" : "su");
                    pressed &= static_cast<WORD>(~dpad);
                }
                else if (MenuStep(s, f, items, pressed)) pressed &= static_cast<WORD>(~dpad);
            }
        }
    }

    HWND hwnd = s.hwnd;
    float dt = (f.now - s.lastTick) / 1000.0f;
    if (dt < 0.0f || dt > 0.05f) dt = 0.008f;

    float speed = g_cfg.cursorSpeed * f.hf;
    float vx1, vy1, vx2, vy2;
    StickVelocity(pad.lx, pad.ly, g_cfg.leftDeadzone, speed, vx1, vy1);
    StickVelocity(pad.rx, pad.ry, g_cfg.rightDeadzone, speed * g_cfg.cursorFine, vx2, vy2);
    // 0.55.59: come per il pad 2 (Pad2Pointer): fermo 150 ms alla pressione e al rilascio di A e B
    if ((pressed | released) & (PAD_A | PAD_B)) s.p1FreezeUntil = f.now + 150;
    if (static_cast<LONG>(f.now - s.p1FreezeUntil) < 0) { vx1 = vy1 = vx2 = vy2 = 0.0f; }
    float dx = (vx1 + vx2) * dt, dy = (vy1 + vy2) * dt;

    float step = g_cfg.cursorNudge * f.hf;
    if (pressed & PAD_DPAD_LEFT)  dx -= step;
    if (pressed & PAD_DPAD_RIGHT) dx += step;
    if (pressed & PAD_DPAD_UP)    dy -= step;
    if (pressed & PAD_DPAD_DOWN)  dy += step;

    const WORD clickMask = PAD_A | PAD_B;
    if (dx != 0.0f || dy != 0.0f || (pressed & clickMask)) {
        if (!g_virt.active.load()) { Activate(f.real); SyncCursorFromVirtual(s); }   // dopo l'uso del mouse
        s.cx += dx;
        s.cy += dy;
        float maxX = static_cast<float>(f.w - 2), maxY = static_cast<float>(f.h - 2);
        s.cx = s.cx < 1.0f ? 1.0f : (s.cx > maxX ? maxX : s.cx);
        s.cy = s.cy < 1.0f ? 1.0f : (s.cy > maxY ? maxY : s.cy);
        SetVirtualClient(s, static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), f.w, f.h);
        MoveIfChanged(s);
        // Diagnostica (0.15.8): alla prova delle 17:03 lo stick non muoveva il cursore nella conferma
        // "Caricare la partita!" (il mouse si'); una riga ogni 2 s con il puntatore virtuale e quello vero.
        static DWORD stickLogAt = 0;
        if ((vx1 != 0.0f || vy1 != 0.0f) && f.now - stickLogAt >= 2000) {
            stickLogAt = f.now;
            POINT rp{};
            RealGetCursorPos(&rp);
            Log("Cursore: stick -> (%ld,%ld), virtuale %s, mouse vero (%ld,%ld)", static_cast<LONG>(s.cx),
                static_cast<LONG>(s.cy), g_virt.active.load() ? "attivo" : "spento", rp.x, rp.y);
        }
    }

    // Voce "Controller" sopra il menu Esc (0.15.1): A su di lei apre l'aiuto invece di cliccare.
    if ((pressed & PAD_A) && s.escItemOn && s.cx >= s.escItem.left && s.cx < s.escItem.right &&
        s.cy >= s.escItem.top && s.cy < s.escItem.bottom) {
        OpenHelp(s, NativePad2HasPointer() ? 2 : 1, "voce Controller del menu");   // 0.51.3: di chi ha il puntatore
        return;
    }
    // Voce "Co-op locale" del menu principale (0.20.1): A sulla voce avvia la sessione. Con Menu=true (0.47.3) A fa un
    // clic normale, che la voce "Co-op" di Memory.cpp vede da g_virt.lmb e apre l'elenco.
    if ((pressed & PAD_A) && !g_cfg.nativeMenu && s.mainRowOn && s.cx >= s.mainRow.left && s.cx < s.mainRow.right &&
        s.cy >= s.mainRow.top && s.cy < s.mainRow.bottom) {
        CoopStartFromMenu(hwnd);
        return;
    }
    // A e B seguono il tasto: pressione e rilascio separati (trascinamento possibile).
    if ((pressed & PAD_A) && !s.cursorLeft) {
        SendLeftDown(hwnd);
        s.cursorLeft = true;
        // 0.55.38: senza finestre aperte anche cio' che il gioco sceglie sotto il cursore (cose che A non trova ancora)
        char what[112] = "";
        if (MemoryUIState() == 0) MemoryPickDescribe(static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), what, sizeof(what));
        Log("Cursore: sinistro giu' in (%ld,%ld)%s%s", static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), what[0] ? " su " : "", what);
    }
    if ((released & PAD_A) && s.cursorLeft) { SendLeftUp(hwnd); s.cursorLeft = false; }
    if ((pressed & PAD_B) && !s.cursorRight) {
        SendRightDown(hwnd);
        s.cursorRight = true;
        Log("Cursore: destro giu' in (%ld,%ld)", static_cast<LONG>(s.cx), static_cast<LONG>(s.cy));
    }
    if ((released & PAD_B) && s.cursorRight) { SendRightUp(hwnd); s.cursorRight = false; }
}

// --- Mercante (0.9.7) -----------------------------------------------------------
// Schede Armi, Armatura, Varie della finestra del mercante: centri a 1920x1080 dallo
// screenshot del tester del 22/10/2026 (l'interfaccia di Sacred non scala). All'apertura la
// mod registra nel log il rettangolo della finestra, per correggere i punti se servisse.
const POINT kMerchantTab[3] = { { 962, 25 }, { 1110, 25 }, { 1261, 25 } };
const char* const kMerchantTabName[3] = { "Armi", "Armatura", "Varie" };

// Clic sinistro programmato in tre tempi (puntatore sul punto, giu' dopo 60 ms, su dopo
// altri 60) come per i pulsanti dei dialoghi; con shift Shift resta premuto per tutto il
// clic (Shift+clic su un oggetto dell'inventario dal mercante = vendita); con restore alla
// fine il cursore torna dove si trovava.
// Con Shift i tempi sono piu' lunghi (0.9.8): 100 ms prima del clic, 100 ms giu' e Shift
// tenuto altri 250 ms. Nella 0.9.7 Shift si rilasciava 40 ms dopo il clic e alla prova del
// 23/10 la prima pressione di X ha preso l'oggetto invece di venderlo (clic senza Shift).
// Dal 0.9.9 anche il clic sulle schede usa 100 ms prima e 100 ms giu', e il cursore torna
// indietro 100 ms dopo il rilascio (prima 60, 60 e 40): alla prova della 0.9.8 le schede
// cambiavano solo ogni tanto.
constexpr DWORD kShiftPreMs = 100, kShiftDownMs = 100, kShiftPostMs = 250, kTabPostMs = 100;
// Schede (0.9.12): alla prova della 0.9.11 la mod leggeva giusti scheda attiva e pulsanti, ma il
// gioco ignorava circa due clic su tre nel punto corretto. Ipotesi: sul PC (d3d11warp, pochi
// fotogrammi al secondo) l'interfaccia controlla il pulsante una volta per fotogramma e un clic
// di 100 ms puo' cadere fra due controlli. Clic lungo, e ripetuto se la scheda non cambia.
// 0.9.13: la causa vera era il mouse vero lontano dalla scheda (vedi StartMacroClick); con il
// mouse vero sul punto i clic vanno a segno, quindi dal 0.9.14 i tempi tornano brevi.
constexpr DWORD kTabPreMs = 80, kTabDownMs = 80, kTabAfterMs = 80, kTabCheckMs = 300;
constexpr int   kTabRetries = 2;
void StartMacroClick(Loop& s, const Frame& f, LONG x, LONG y, bool shift, bool restore)
{
    if (!g_virt.active.load()) { Activate(f.real); SyncCursorFromVirtual(s); }
    s.mcRX = s.cx; s.mcRY = s.cy;
    s.mcShift = shift; s.mcRestore = restore;
    if (shift) HoldKey(s.hwnd, kShift, true);
    SetVirtualClient(s, x, y, f.w, f.h);
    s.cx = static_cast<float>(x); s.cy = static_cast<float>(y);
    // Anche il mouse vero va sul punto (0.9.13): alle prove del 23/10 i clic sulle schede
    // funzionavano solo dopo che il mouse vero era stato portato sulla finestra del mercante
    // (00:47:57 e 00:51:35, poi una decina di clic di fila tutti presi). s.lastReal si aggiorna
    // perche' lo spostamento non sembri il giocatore che usa il mouse.
    s.mcRealMoved = false;
    if (!g_ptrToM2 && RealGetCursorPos(&s.mcReal)) {   // 0.56.7: con il secondo mouse il mouse vero resta dov'e'
        POINT p{ x, y };
        ClientToScreen(s.hwnd, &p);
        if (RealSetCursorPos(p.x, p.y)) { s.mcRealMoved = true; s.lastReal = p; }
        // 0.53.3: prova sulla Steam Machine del 29/10, schede del mercante dell'ospite non prese anche senza il mouse
        // che si muoveva: si controlla se sotto Wine (desktop virtuale, gamescope) il mouse vero va davvero sul punto.
        POINT q{};
        static DWORD warnAt = 0;
        if (RealGetCursorPos(&q) && (q.x != p.x || q.y != p.y || !s.mcRealMoved) && f.now - warnAt > 5000) {
            warnAt = f.now;
            Log("Clic della mod: mouse vero chiesto in (%ld,%ld), e' in (%ld,%ld)%s", p.x, p.y, q.x, q.y,
                s.mcRealMoved ? "" : " (SetCursorPos fallita)");
        }
    }
    SendMove(s.hwnd);
    s.mcStep = 1;
    s.mcAt = f.now + (shift ? kShiftPreMs : kTabPreMs);
}

void RestoreRealCursor(Loop& s)
{
    if (!s.mcRealMoved) return;
    s.mcRealMoved = false;
    if (RealSetCursorPos(s.mcReal.x, s.mcReal.y)) s.lastReal = s.mcReal;
}

void MacroClickTick(Loop& s, const Frame& f)
{
    if (!s.mcStep || static_cast<LONG>(f.now - s.mcAt) < 0) return;
    if (s.mcShift) HoldKey(s.hwnd, kShift, true);   // ancora giu' (non viene ripetuto se lo e' gia')
    if (s.mcStep == 1)      { SendLeftDown(s.hwnd); s.mcStep = 2; s.mcAt = f.now + (s.mcShift ? kShiftDownMs : kTabDownMs); }
    else if (s.mcStep == 2) { SendLeftUp(s.hwnd);   s.mcStep = 3; s.mcAt = f.now + (s.mcShift ? kShiftPostMs : kTabAfterMs); }
    else {
        if (s.mcShift) HoldKey(s.hwnd, kShift, false);
        RestoreRealCursor(s);
        if (s.mcRestore) {
            s.cx = s.mcRX; s.cy = s.mcRY;
            SetVirtualClient(s, static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), f.w, f.h);
            SendMove(s.hwnd);
        }
        s.mcStep = 0;
    }
}

// Vibrazione (0.17.0, richiesta del tester): solo quando il ritratto dell'eroe lampeggia di rosso. Il
// gioco (0x6d74b0) lo fa lampeggiare con vita/vita massima sotto 0.25 (costante a 0x88f7c4), intensita'
// (sin(t * 4) + 1) / 2 con t = [0xcd69d8]: un impulso vicino al massimo di ogni lampeggio, cioe' uno
// ogni 1,6 s circa. Niente vibrazione a ogni colpo. Ferma se il tempo del gioco non avanza (pausa,
// menu), con l'eroe morto o con la vibrazione disattivata nell'aiuto.
void VibrationTick(Loop& s, const Frame& f)
{
    static bool low = false;
    static double lastT = 0.0;
    static DWORD tMovedAt = 0;
    const float power = g_cfg.vibrationPower / 100.0f;
    if (static_cast<LONG>(f.now - s.vibTestUntil) < 0) { PadVibrate(power, power * 0.5f); return; }
    float frac = 1.0f; double t = 0.0;
    const bool ok = MemoryHeroHealth(frac, t);
    if (ok && t != lastT) { lastT = t; tMovedAt = f.now; }
    const bool running = ok && f.now - tMovedAt < 300;
    const bool nowLow = ok && frac > 0.0f && frac < 0.25f;
    if (nowLow != low) {
        low = nowLow;
        Log("Vita %s il 25%% (%.0f%%): vibrazione %s", low ? "sotto" : "sopra", frac * 100.0f,
            !low ? "ferma" : g_cfg.vibration ? "a ogni lampeggio" : "disattivata nell'aiuto");
    }
    if (!g_cfg.vibration || !low || !running) { PadVibrate(0.0f, 0.0f); return; }
    const double b = (std::sin(t * 4.0) + 1.0) * 0.5;      // intensita' del lampeggio del gioco
    if (b > 0.85) PadVibrate(power, power * 0.5f);
    else          PadVibrate(0.0f, 0.0f);
}

// Navigazione automatica verso la lobby (0.20.4): in sessione co-op l'host e l'ospite arrivano da soli dal menu
// principale alla scelta dell'eroe (che resta a mano). Nella lobby l'host sceglie a mano la partita (Crea); quando
// l'host e' in partita, l'ospite sceglie la prima partita della lista e preme Unisciti, finche' non e' dentro.
void NavTick(Loop& s, const Frame& f0)
{
    if (g_cfg.coopRole == 0 || !g_cfg.coopNav) return;
    static HANDLE clickMutex = CreateMutexA(nullptr, FALSE, "Local\\SacredPad_Click");   // un solo clic alla volta fra le istanze
    if (s.navHold && !s.mcStep) { ReleaseMutex(clickMutex); s.navHold = false; }
    if (s.navDone) return;
    if (!s.navStart) s.navStart = f0.now;
    if (f0.now - s.navStart > 600000) { s.navDone = true; Log("Nav: tempo scaduto, navigazione automatica finita"); return; }
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) && f0.now - s.navStart > 1500) { s.navDone = true; Log("Nav: annullata con Esc"); return; }
    Frame f = f0;                                  // la dimensione della finestra serve ai clic
    RECT cr{};
    GetClientRect(s.hwnd, &cr);
    f.w = cr.right; f.h = cr.bottom;
    if (s.mcStep || static_cast<LONG>(f.now - s.navAt) < 0) return;
    static HANDLE evHost = CreateEventA(nullptr, TRUE, FALSE, "Local\\SacredPad_HostInGame");
    const bool inGame = MemoryWindowOpen(0);
    auto stage = [&](int st, const char* msg) { if (s.navStage != st) { s.navStage = st; Log("Nav: %s", msg); } };
    if (inGame) {
        if (g_cfg.coopRole == 1 && evHost) SetEvent(evHost);
        stage(99, "in partita, navigazione finita");
        s.navDone = true;
        return;
    }
    auto click = [&](const RECT& r, const char* what) {
        if (!clickMutex || WaitForSingleObject(clickMutex, 0) != WAIT_OBJECT_0) { s.navAt = f.now + 200; return; }   // l'altra istanza sta cliccando
        s.navHold = true;
        StartMacroClick(s, f, (r.left + r.right) / 2, (r.top + r.bottom) / 2, false, false);
        s.mcAt = f.now + 700;                    // 0.20.12: il primo clic su Multiplayer spesso non partiva: il menu vuole il puntatore fermo sulla voce un momento
        s.navAt = f.now + 2500;
        s.navLastClick = f.now;
        Log("Nav: clic su %s (%ld,%ld)", what, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
    };
    RECT r{};
    if (MemoryNetButton("UI_NET_BTN_CREAT", r)) {                     // lobby
        s.navMenuDone = true;
        if (g_cfg.coopRole == 1) { stage(3, "host nella lobby: scegli le opzioni e premi Crea"); return; }
        if (!evHost || WaitForSingleObject(evHost, 0) != WAIT_OBJECT_0) { stage(4, "ospite nella lobby, attesa che l'host sia in partita"); return; }
        stage(5, "ospite: cerca la partita dell'host");
        if (static_cast<LONG>(f.now - s.navJoinAt) < 0) return;
        if (s.navSub == 0) {
            const RECT row{ 75, 50, 530, 72 };                       // prima riga della lista Partite
            click(row, "prima partita della lista");
            s.navSub = 1;
        } else if (++s.navJoins > 12) {
            s.navDone = true;
            Log("Nav: l'ospite non e' riuscito a entrare dopo 12 tentativi, navigazione finita");
        } else {
            RECT j{};
            if (MemoryNetButton("UI_NET_BTN_JOIN", j)) click(j, "Unisciti");
            s.navSub = 0;
            s.navJoinAt = f.now + 4000;
        }
        return;
    }
    RECT a{};
    if (MemoryNetButton("UI_BTN_ACCEPT", a)) { s.navMenuDone = true; stage(2, "scelta dell'eroe: sceglilo e premi Accetta"); return; }
    if (s.navMenuDone) return;                    // tornati indietro dalla scelta dell'eroe: decide l'utente
    std::vector<MenuItem> items;
    bool cs = false;
    if (MemoryMenuItems(items, &cs)) {
        // 0.20.11: durante il cambio di schermata il gioco elenca ancora "Multiplayer" e gia' "rete locale", che stanno
        // nella stessa posizione di "Campagna Underworld": un clic su "rete locale" dato troppo presto finiva li'
        // (il tester, 25/10). "Rete locale" si clicca solo quando "Multiplayer" non e' piu' fra le voci, o dopo 7 s.
        const MenuItem* multi = nullptr; const MenuItem* lan = nullptr;
        for (const MenuItem& it : items) {
            if (strcmp(it.name, "UI_LABEL_MULTIPL") == 0) multi = &it;
            else if (strcmp(it.name, "UI_LABEL_MP_LAN") == 0) lan = &it;
        }
        const bool lanOk = lan && !multi;
        // "Multiplayer" non si ripete prima di 8 s: nella schermata di scelta, la stessa posizione e' "Internet".
        // Se "Multiplayer" resta fra le voci dopo 8 s il clic non e' partito: si ripete quello (nella schermata di scelta la
        // stessa posizione e' "Internet", innocua; "rete locale" li' sopra e' "Campagna Underworld", che avvia una partita).
        const bool multiOk = multi && (s.navClicks == 0 || f.now - s.navLastClick > 8000);
        const MenuItem* pick = lanOk ? lan : (multiOk ? multi : nullptr);
        if (!pick) { s.navAt = f.now + 400; return; }
        stage(1, "menu principale / multiplayer");
        if (++s.navClicks > 10) { s.navDone = true; Log("Nav: troppi clic nel menu, navigazione finita"); return; }
        RECT rr{ pick->x, pick->y, pick->x + pick->w, pick->y + pick->h };
        const bool isLan = (pick == lan);
        click(rr, isLan ? "UI_LABEL_MP_LAN" : "UI_LABEL_MULTIPL");
        if (isLan) s.navMenuDone = true;   // da qui in poi la scheda personaggi e' dell'utente
        return;
    }
}

// 0.55.29 (revisione del 31/10): dialoghi del pad 2 senza pad 1 (PadSource=None, o controller del giocatore 1 spento o scarico).
// Il giro di Tick finisce prima del blocco automatico delle finestre e di quello dei dialoghi, quindi A e B del pad 2 in un
// dialogo non facevano nulla e la finestra non si cercava nemmeno. Qui il minimo che serve: la finestra si cerca a fette mentre
// si aspetta (interazione dell'eroe 2, eroe 2 che parla), A = Invio (OK o Accetta), B = clic sull'ultimo pulsante (Rifiuta) con
// il clic programmato, che rimette il mouse vero dov'era; eroe 2 che parla e finestra non trovata: A = Invio.
void Pad2DialogsWithoutPad1(Loop& s, Frame& f, HWND hwnd)
{
    {   // partita caricata (stato delle finestre da -1 a 0): oggetti nuovi, come nel blocco automatico delle finestre
        static int lastWin = -9;
        const int win = MemoryUIState();
        if (lastWin == -1 && win == 0) {
            MemoryDialogReset(); MemoryUsedReset(); MemoryBusyReset();
            s.loadWatchUntil = f.now + 20000;
            MemoryDialogTrustNew(f.now + 20000);
            Log("Dialoghi: partita caricata senza pad 1, finestre cercate di nuovo");
        }
        lastWin = win;
    }
    if (NativePad2TakeInteract()) { s.dlgWatchUntil = f.now + 8000; s.p2InteractAt = f.now; }
    const WORD p2Dlg = NativePad2TakeDialogBtn();
    const bool watch = static_cast<LONG>(s.dlgWatchUntil - f.now) > 0 || static_cast<LONG>(s.loadWatchUntil - f.now) > 0 ||
                       NativePad2Talking();
    DialogInfo d;
    if (!p2Dlg) {
        if (watch && f.now - s.dlgQuick >= 50) { s.dlgQuick = f.now; if (!MemoryDialog(d)) MemoryDialogScanStep(); }
        return;
    }
    if (s.mcStep) return;                          // un clic programmato e' gia' in corso
    bool open = MemoryDialog(d);
    if (!open && f.now - s.dlgFull >= 2000) {
        s.dlgFull = f.now;
        MemoryDialogRescan(false);
        open = MemoryDialog(d);
    }
    if (open) {
        if (d.unverified) MemoryDialogActed(d.addr);
        if ((p2Dlg & PAD_A) || d.buttons < 2) {
            TapKey(hwnd, kEnter);
            Log("Dialogo (%d,%d %dx%d, %d pulsanti), senza pad 1: %s del pad 2 = Invio", d.x, d.y, d.w, d.h, d.buttons, (p2Dlg & PAD_A) ? "A" : "B");
        }
        else {
            RECT cr0{};
            GetClientRect(hwnd, &cr0);
            f.w = cr0.right; f.h = cr0.bottom;
            const RECT& r = d.btn[d.buttons - 1];
            const LONG bx = (r.left + r.right) / 2, by = (r.top + r.bottom) / 2;
            StartMacroClick(s, f, bx, by, false, true);
            s.p2DlgClickNoPad1 = true;
            Log("Dialogo (%d,%d %dx%d, %d pulsanti), senza pad 1: B del pad 2 = clic sul pulsante %d in (%ld,%ld)", d.x, d.y, d.w, d.h,
                d.buttons, d.buttons, bx, by);
        }
        if (d.buttons >= 1) s.dlgWatchUntil = 0;
    }
    else if (NativePad2Talking() && (p2Dlg & PAD_A)) {
        TapKey(hwnd, kEnter);
        Log("Dialogo dell'eroe 2 non trovato in memoria, senza pad 1: A del pad 2 = Invio");
    }
}

// --- 0.55.54: due zaini, fase 2 (un lato e un puntatore per giocatore) ------------------------------------------------
// Lati (Memory.cpp, MemoryDualSide): View tenuto del pad 1 e Back del pad 2 aprono e chiudono ciascuno il suo; il tasto I del
// gioco parte solo per aprire il primo lato o chiudere l'ultimo, perche' la finestra vera e' una sola.
void DualToggle(Loop& s, const Frame& f, int who, const char* why)
{
    const int other = who == 1 ? 2 : 1;
    if (!MemoryWindowOpen(1)) {
        MemoryDualSetSide(who, true);
        MemoryDualSetSide(other, false);
        TapKey(s.hwnd, kKeyI);
        s.dualOpenAt = f.now ? f.now : 1;
        Log("ZAINI: lato dell'eroe %d aperto (%s), finestra del gioco aperta (%s)", who, why, kKeyI.name);
        if (who == 2) OverlayCoopToast(Tr("Giocatore 2: il tuo zaino e' a destra, Back per chiuderlo"), 3000);
    }
    else if (MemoryDualSide(who)) {
        // con un oggetto in mano il lato resta aperto: chiudendolo l'oggetto passerebbe al puntatore dell'altro giocatore.
        // 0.56.0: con il secondo mouse ognuno ha la sua mano: il giocatore 1 chiude come nel gioco normale (l'oggetto resta sul suo
        // cursore), il giocatore 2 deve posarlo (non puo' ancora gettarlo a terra)
        // 0.56.4: per il giocatore 2 l'oggetto torna nel suo zaino e il lato si chiude (vedi Pad2PointerM2)
        if (MemoryMouse2Ready() && who == 2 && MemoryMouse2Held()) {
            MemoryMouse2Stow("lato chiuso con un oggetto in mano");
            OverlayCoopToast(Tr("Giocatore 2: l'oggetto in mano e' tornato nello zaino"), 2500);
        }
        else if (MemoryMouse2Ready() ? false : MemoryHeldAny()) {
            OverlayCoopToast(Tr("Posa prima l'oggetto che hai in mano"), 2500);
            Log("ZAINI: lato dell'eroe %d non chiuso (%s): oggetto in mano", who, why);
            return;
        }
        MemoryDualSetSide(who, false);
        if (!MemoryDualSide(other)) {
            // 0.56.9: dal mercante Esc chiude il negozio (0.56.14: anche fabbro e Maestro)
            TapKey(s.hwnd, (MemoryWindowOpen(4) || MemoryWindowOpen(3) || MemoryWindowOpen(14) || MemoryWindowOpen(16)) ? kEsc : kKeyI);   // 0.56.54: e il deposito
            // 0.55.57: anche la chiusura aspetta (prova delle 15:36:12: la finestra ancora aperta per un attimo era presa per
            // "aperta dal gioco" e il lato dell'eroe 1 si riapriva)
            s.dualOpenAt = f.now ? f.now : 1;
            Log("ZAINI: lato dell'eroe %d chiuso (%s), era l'ultimo: finestra del gioco chiusa (%s)", who, why, kKeyI.name);
        }
        else {
            Log("ZAINI: lato dell'eroe %d chiuso (%s), l'altro resta aperto", who, why);
            // 0.56.9: dal mercante condiviso il negozio resta all'altro; la sessione pero' e' del PNG con chi gli ha parlato
            const int sw = MemorySharedWin();
            if (sw == 4) OverlayCoopToast(Tr("Negozio: l'altro giocatore sta ancora comprando"), 2500);
            else if (sw >= 0) OverlayCoopToast(Tr("L'altro giocatore la sta ancora usando"), 2500);
        }
    }
    else {
        MemoryDualSetSide(who, true);
        Log("ZAINI: lato dell'eroe %d aperto (%s)", who, why);
        if (who == 2) OverlayCoopToast(Tr("Giocatore 2: il tuo zaino e' a destra, Back per chiuderlo"), 3000);
    }
}

// Lati allineati alla finestra del gioco: chiusa (tasto I, Esc, X, il gioco) = tutti e due chiusi; aperta senza lati (tasto I
// della tastiera, il gioco) = quello dell'eroe 1, come nel gioco normale. Per 600 ms dopo il tasto I della mod si aspetta.
void DualSync(Loop& s, const Frame& f)
{
    const bool open = MemoryWindowOpen(1);
    const bool pending = s.dualOpenAt && f.now - s.dualOpenAt < 600;
    if (open) s.dualOpenAt = 0;
    if (pending) return;
    if (!open && (MemoryDualSide(1) || MemoryDualSide(2))) {
        MemoryDualSetSide(1, false); MemoryDualSetSide(2, false);
        Log("ZAINI: finestra del gioco chiusa: lati chiusi");
    }
    else if (open && !MemoryDualSide(1) && !MemoryDualSide(2)) {
        // 0.56.9: negozio aperto perche' l'eroe 2 e' andato a parlare con il mercante: e' il suo lato (l'eroe 1 resta a giocare)
        // 0.56.14: anche fabbro e Maestro
        const int sw2 = MemoryPad2ShopWin();
        if (sw2 >= 0 && MemoryWindowOpen(sw2) && MemoryMouse2Ready()) {
            MemoryPad2MerchantClear();
            MemoryDualSetSide(2, true);
            Log("ZAINI: finestra n.%d aperta dall'eroe 2: lato dell'eroe 2 (il giocatore 1 puo' aprire il suo con View tenuto o A sul PNG)", sw2);
            OverlayCoopToast(Tr(sw2 == 4 ? "Giocatore 2: il negozio e' tuo, il giocatore 1 puo' unirsi" : "Giocatore 2: la finestra e' tua, il giocatore 1 puo' unirsi"), 3000);
        }
        else {
            MemoryDualSetSide(1, true);
            Log("ZAINI: finestra aperta dal gioco: lato dell'eroe 1");
        }
    }
}

// Croce del pad 2 sulle caselle del suo zaino (come GridStep per il pad 1, su una griglia sola: quella dell'eroe 2).
// 0.55.63 (prova del 02/11: "il pad del giocatore 2 non naviga con le frecce dove inserisce le armature e nelle statistiche"):
// anche le caselle dell'equipaggiamento e i "+" delle statistiche dell'eroe 2 (MemoryHero2Items, dove sono disegnati), come
// GridStep e MenuStep per il pad 1: dal bordo della griglia si va alla casella o al pulsante piu' vicino in quella direzione, e
// da li' si torna alla griglia quando in quella direzione non c'e' altro.
static bool Pad2ItemTo(float x, float y, const std::vector<MenuItem>& items, int dc, int dr, float& tx, float& ty, const MenuItem** pick)
{
    float bestS = 1e9f;
    bool found = false;
    for (const MenuItem& it : items) {
        if (x >= it.x && x < it.x + it.w && y >= it.y && y < it.y + it.h) continue;   // quello attuale
        const float cx = it.x + it.w / 2.0f, cy = it.y + it.h / 2.0f;
        const float vx = cx - x, vy = cy - y;
        const float along = vx * dc + vy * dr, side = std::fabs(vx * dr - vy * dc);
        if (along < 4.0f || side > along * 2.0f) continue;                            // non in quella direzione
        const float sc = along + 2.0f * side;
        if (sc < bestS) { bestS = sc; tx = cx; ty = cy; found = true; if (pick) *pick = &it; }
    }
    return found;
}

bool Pad2GridStep(Loop& s, WORD pressed)
{
    int dc = 0, dr = 0;
    if (pressed & PAD_DPAD_LEFT)  dc = -1;
    if (pressed & PAD_DPAD_RIGHT) dc = 1;
    if (pressed & PAD_DPAD_UP)    dr = -1;
    if (pressed & PAD_DPAD_DOWN)  dr = 1;
    if (!dc && !dr) return false;
    GridInfo g;
    bool haveGrid = MemoryHero2Grid(g);
    // 0.56.9: dal mercante condiviso anche la merce del suo lato: si lavora sulla griglia su cui sta il puntatore; dal bordo di una
    // si passa all'altra se sta in quella direzione
    GridInfo gm;
    const bool haveMerch = MemoryMerchantCopyGrid(gm);
    auto inGrid = [&](const GridInfo& q) { return s.p2x >= q.x && s.p2y >= q.y && s.p2x < q.x + q.cols * 32 && s.p2y < q.y + q.rows * 32; };
    if (haveMerch && (inGrid(gm) || !haveGrid)) { g = gm; haveGrid = true; }
    else if (haveMerch && haveGrid && inGrid(g)) {
        const bool edge = (dr < 0 && s.p2y < g.y + 32) || (dr > 0 && s.p2y >= g.y + (g.rows - 1) * 32) ||
                          (dc < 0 && s.p2x < g.x + 32) || (dc > 0 && s.p2x >= g.x + (g.cols - 1) * 32);
        const float mcx = gm.x + gm.cols * 16.0f, mcy = gm.y + gm.rows * 16.0f;
        if (edge && ((mcx - s.p2x) * dc + (mcy - s.p2y) * dr) > 0) {           // la merce sta in quella direzione
            int col = static_cast<int>((s.p2x - gm.x) / 32.0f), row = static_cast<int>((s.p2y - gm.y) / 32.0f);
            col = col < 0 ? 0 : (col >= gm.cols ? gm.cols - 1 : col);
            row = row < 0 ? 0 : (row >= gm.rows ? gm.rows - 1 : row);
            s.p2x = gm.x + col * 32 + 16.0f; s.p2y = gm.y + row * 32 + 16.0f;
            Log("Pad 2: zaino -> merce, puntatore (%.0f,%.0f)", s.p2x, s.p2y);
            return true;
        }
    }
    if (haveMerch && haveGrid && g.entry == gm.entry && inGrid(gm)) {
        GridInfo gz;
        const bool edge = (dr < 0 && s.p2y < gm.y + 32) || (dr > 0 && s.p2y >= gm.y + (gm.rows - 1) * 32) ||
                          (dc < 0 && s.p2x < gm.x + 32) || (dc > 0 && s.p2x >= gm.x + (gm.cols - 1) * 32);
        if (edge && MemoryHero2Grid(gz)) {
            const float zcx = gz.x + gz.cols * 16.0f, zcy = gz.y + gz.rows * 16.0f;
            if (((zcx - s.p2x) * dc + (zcy - s.p2y) * dr) > 0) {               // lo zaino sta in quella direzione
                int col = static_cast<int>((s.p2x - gz.x) / 32.0f), row = static_cast<int>((s.p2y - gz.y) / 32.0f);
                col = col < 0 ? 0 : (col >= gz.cols ? gz.cols - 1 : col);
                row = row < 0 ? 0 : (row >= gz.rows ? gz.rows - 1 : row);
                s.p2x = gz.x + col * 32 + 16.0f; s.p2y = gz.y + row * 32 + 16.0f;
                Log("Pad 2: merce -> zaino, puntatore (%.0f,%.0f)", s.p2x, s.p2y);
                return true;
            }
        }
    }
    std::vector<MenuItem> items;
    MemoryHero2Items(items);
    if (!haveGrid && items.empty()) return false;
    const bool inside = haveGrid && s.p2x >= g.x && s.p2y >= g.y && s.p2x < g.x + g.cols * 32 && s.p2y < g.y + g.rows * 32;
    float tx = 0, ty = 0;
    const MenuItem* pick = nullptr;
    if (!inside) {
        // equipaggiamento, statistiche o fuori da tutto: prima un'altra casella o un pulsante in quella direzione
        if (Pad2ItemTo(s.p2x, s.p2y, items, dc, dr, tx, ty, &pick)) {
            s.p2x = tx; s.p2y = ty;
            Log("Pad 2: %s dell'eroe 2 -> puntatore (%.0f,%.0f)", pick ? pick->name : "pulsante", tx, ty);
            return true;
        }
        if (!haveGrid) return true;                                          // nessuna griglia: si resta
    }
    int col = 0, row = 0;
    if (!inside) {                                                           // si torna alla casella piu' vicina della griglia
        col = static_cast<int>((s.p2x - g.x) / 32.0f);
        row = static_cast<int>((s.p2y - g.y) / 32.0f);
        col = col < 0 ? 0 : (col >= g.cols ? g.cols - 1 : col);
        row = row < 0 ? 0 : (row >= g.rows ? g.rows - 1 : row);
    }
    else {
        col = static_cast<int>((s.p2x - g.x) / 32.0f);
        row = static_cast<int>((s.p2y - g.y) / 32.0f);
        int cur = -1, cw = 1, ch = 1;
        if (!MemoryGridCell(g, row * g.cols + col, cur, cw, ch)) cur = -1;
        for (;;) {
            const int nc = col + dc, nr = row + dr;
            if (nc < 0 || nr < 0 || nc >= g.cols || nr >= g.rows) {
                // bordo: la casella dell'equipaggiamento o il pulsante in quella direzione, altrimenti si resta
                const float px = g.x + col * 32 + 16.0f, py = g.y + row * 32 + 16.0f;
                if (Pad2ItemTo(px, py, items, dc, dr, tx, ty, &pick)) {
                    s.p2x = tx; s.p2y = ty;
                    Log("Pad 2: zaino -> %s dell'eroe 2, puntatore (%.0f,%.0f)", pick ? pick->name : "pulsante", tx, ty);
                    return true;
                }
                break;
            }
            col = nc; row = nr;
            int a2 = -1, aw = 1, ah = 1;
            if (!MemoryGridCell(g, row * g.cols + col, a2, aw, ah)) a2 = -1;
            if (a2 >= 0 && a2 == cur) continue;                              // stesso oggetto
            break;
        }
    }
    int anchor = -1, w = 1, h = 1;
    tx = g.x + col * 32 + 16.0f; ty = g.y + row * 32 + 16.0f;
    if (MemoryGridCell(g, row * g.cols + col, anchor, w, h)) {
        tx = g.x + (anchor % g.cols) * 32 + w * 16.0f;
        ty = g.y + (anchor / g.cols) * 32 + h * 16.0f;
    }
    s.p2x = tx; s.p2y = ty;
    Log("%s: casella (%d,%d) -> puntatore del giocatore 2 (%.0f,%.0f)", g.kind, col, row, tx, ty);
    return true;
}

void Pad2Release(Loop& s, const char* why)
{
    if (s.p2Left) { SendLeftUp(s.hwnd); s.p2Left = false; }
    if (s.p2Right) { SendRightUp(s.hwnd); s.p2Right = false; }
    if (s.p2mc >= 2) SendLeftUp(s.hwnd);
    if (s.p2mc && s.p2RealMoved) { s.p2RealMoved = false; if (RealSetCursorPos(s.p2Real.x, s.p2Real.y)) s.lastReal = s.p2Real; }
    s.p2mc = 0;
    (void)why;
}

// Il mouse del gioco (g_virt) e' di un giocatore alla volta: del "titolare", che il gioco disegna con il suo cursore; l'altro
// lo disegna l'overlay. Passa all'altro giocatore quando lui usa un pulsante (o muove lo stick e il titolare e' fermo da 300
// ms), ma mai mentre il titolare tiene un pulsante, fa un clic a tempo o ha un oggetto in mano: l'oggetto in mano e' uno solo e
// segue il mouse del gioco, quindi l'altro aspetta (avviso). Con il pad 1 fuori dalla modalita' cursore il mouse e' del pad 2.
// 0.56.0: con il secondo mouse (MemoryMouse2Ready) il giocatore 2 ha un mouse del gioco suo: il cursore lo disegna il gioco con
// le stesse forme di quello del giocatore 1, i suoi clic sono eventi del gioco che la mod passa alle finestre con il secondo mouse
// attivo, e il mouse del gioco (g_virt) resta sempre del giocatore 1: niente titolare, niente attese, una mano per giocatore.
void Pad2PointerM2(Loop& s, const Frame& f)
{
    // 0.56.2: anche in modalita' cursore del giocatore 2 (View premuto), senza il suo zaino aperto
    const bool side = MemoryDualSide(2) && MemoryDualActive();
    const bool curMode = MemoryPad2CursorMode();
    // 0.56.4: crash del 03/11 alle 12:26 (zaino chiuso dal gioco con un oggetto in mano al giocatore 2 e poi riaperto): chiuso il suo
    // lato per qualunque motivo (Y, View, Esc, diario, il gioco), anche in modalita' cursore, cio' che ha in mano torna nel suo zaino
    const bool sideFlag = MemoryDualSide(2);
    if (s.p2SideWas && !sideFlag) MemoryMouse2Stow("lato del giocatore 2 chiuso");
    s.p2SideWas = sideFlag;
    const bool want = (side || curMode) && !NativePad2HasPointer() && !s.helpOpen;
    PadState p2; int idx2 = -1;
    const bool have = want && PadRead2(p2, idx2);
    s.dualOwner = 1;
    if (!have) {
        if (s.p2On) {
            s.p2On = false;
            if (s.p2Left) { MemoryMouse2Button(0, false, static_cast<LONG>(s.p2x), static_cast<LONG>(s.p2y)); s.p2Left = false; }
            if (s.p2Right) { MemoryMouse2Button(2, false, static_cast<LONG>(s.p2x), static_cast<LONG>(s.p2y)); s.p2Right = false; }
            if (s.p2WorldA) { MemoryPad2WorldButton(0, false, 0, 0); s.p2WorldA = false; }
            s.p2WorldB = false;
            MemoryMouse2Ui(false, 0, 0);
            Log("ZAINI: puntatore del giocatore 2 spento (%s)", !want ? "lato chiuso" : "pad 2 non letto");
        }
        OverlaySetPointers(false, false, 0, 0, 0, 0, 0);
        return;
    }
    if (!s.p2On) {
        s.p2On = true;
        s.p2Prev = p2.buttons;                            // niente fronti finti: i pulsanti gia' premuti contano dal rilascio
        s.p2Tick = f.now;
        GridInfo g;
        if (side && MemoryHero2Grid(g)) { s.p2x = g.x + 16.0f; s.p2y = g.y + 16.0f; }
        else if (!side && s.p2x >= 1.0f && s.p2y >= 1.0f && s.p2x < f.w && s.p2y < f.h && f.now - s.p2OffAt < 3000) {}   // dove era
        else {
            LONG wx = 0, wy = 0;                          // modalita' cursore: parte dal suo cursore nel mondo
            if (!side && MemoryMouse2WorldPos(wx, wy)) { s.p2x = static_cast<float>(wx); s.p2y = static_cast<float>(wy); }
            else { s.p2x = f.w * 0.75f; s.p2y = f.h * 0.5f; }
        }
        Log("ZAINI: puntatore del giocatore 2 acceso in (%.0f,%.0f), con il secondo mouse del gioco (%s)", s.p2x, s.p2y,
            side ? "il suo zaino" : "modalita' cursore");
    }
    s.p2OffAt = f.now;
    const WORD b = static_cast<WORD>(p2.buttons | (p2.rt > 0.5f ? PAD_RT : 0));   // 0.56.31: RT come pulsante
    const WORD pressed = static_cast<WORD>(b & ~s.p2Prev), released = static_cast<WORD>(s.p2Prev & ~b);
    s.p2Prev = b;
    float dt = (f.now - s.p2Tick) / 1000.0f;
    if (dt < 0.0f || dt > 0.05f) dt = 0.008f;
    s.p2Tick = f.now;
    const float speed = g_cfg.cursorSpeed * f.hf;
    float vx1, vy1, vx2, vy2;
    StickVelocity(p2.lx, p2.ly, g_cfg.leftDeadzone, speed, vx1, vy1);
    StickVelocity(p2.rx, p2.ry, g_cfg.rightDeadzone, speed * g_cfg.cursorFine, vx2, vy2);
    s.p2x += (vx1 + vx2) * dt;
    s.p2y += (vy1 + vy2) * dt;
    const WORD dpad = PAD_DPAD_LEFT | PAD_DPAD_RIGHT | PAD_DPAD_UP | PAD_DPAD_DOWN;
    if ((pressed & dpad) && side) Pad2GridStep(s, pressed);
    const float maxX = static_cast<float>(f.w - 2), maxY = static_cast<float>(f.h - 2);
    s.p2x = s.p2x < 1.0f ? 1.0f : (s.p2x > maxX ? maxX : s.p2x);
    s.p2y = s.p2y < 1.0f ? 1.0f : (s.p2y > maxY ? maxY : s.p2y);
    const LONG px = static_cast<LONG>(s.p2x), py = static_cast<LONG>(s.p2y);
    MemoryMouse2Ui(true, px, py);
    if ((pressed & PAD_Y) && side) {                      // Y: chiude il suo lato
        if (s.p2Left) { MemoryMouse2Button(0, false, px, py); s.p2Left = false; }
        if (s.p2Right) { MemoryMouse2Button(2, false, px, py); s.p2Right = false; }
        DualToggle(s, f, 2, "Y del pad 2");
        return;
    }
    if ((pressed & PAD_RT) && side) MemoryCompanionCycle(2);   // 0.56.31: RT gira la colonna del giocatore 2
    // LB / RB: scheda precedente / successiva della copia dello zaino, con un clic del secondo mouse al centro della scheda
    const bool shop = side && (MemoryMerchantShared() || MemorySharedWin() == 14);   // 0.56.9 mercante, 0.56.14 schede del Maestro
    if ((pressed & (PAD_LB | PAD_RB)) && shop) {
        int a = 0, n = 0; RECT tr[8] = {};
        if (MemoryMerchantCopyTabs(a, n, tr, 8) && n > 0) {
            const bool next = (pressed & PAD_RB) != 0;
            const int to = (a + (next ? 1 : n - 1)) % n;
            const LONG x = (tr[to].left + tr[to].right) / 2, y = (tr[to].top + tr[to].bottom) / 2;
            MemoryMouse2Button(0, true, x, y);
            MemoryMouse2Button(0, false, x, y);
            Log("%s del pad 2: scheda %d -> %d di %d del mercante (lato dell'eroe 2), clic in (%ld,%ld)", next ? "RB" : "LB", a + 1, to + 1, n, x, y);
        }
        else Log("%s del pad 2: schede del mercante non leggibili", (pressed & PAD_RB) ? "RB" : "LB");
    }
    else if ((pressed & (PAD_LB | PAD_RB)) && side) {
        int a = 0, n = 0; RECT tr[8] = {};
        if (MemoryDualCopyTabs(a, n, tr, 8) && n > 0) {
            const bool next = (pressed & PAD_RB) != 0;
            int to = (a + (next ? 1 : n - 1)) % n;
            RECT ct{}; bool copen = false;                    // 0.56.43: scheda Compagni dopo Combo, come per il pad 1
            if (MemoryCompTab(2, ct, copen)) {
                if (copen) to = next ? 0 : n - 1;
                else if ((next && a == n - 1) || (!next && a == 0)) to = -1;
            }
            const RECT& q = to < 0 ? ct : tr[to];
            const LONG x = (q.left + q.right) / 2, y = (q.top + q.bottom) / 2;
            MemoryMouse2Button(0, true, x, y);
            MemoryMouse2Button(0, false, x, y);
            if (to < 0) Log("%s del pad 2: scheda %d -> scheda Compagni dello zaino dell'eroe 2, clic in (%ld,%ld)", next ? "RB" : "LB", a + 1, x, y);
            else Log("%s del pad 2: scheda %d -> %d di %d dello zaino dell'eroe 2, clic in (%ld,%ld)", next ? "RB" : "LB", a + 1, to + 1, n, x, y);
        }
        else Log("%s del pad 2: schede dello zaino dell'eroe 2 non leggibili", (pressed & PAD_RB) ? "RB" : "LB");
    }
    // A = sinistro, B = destro, solo sulle finestre dei due zaini (e sulla sua barra con qualcosa in mano, per posare un'arte o
    // una pozione): fuori il clic andrebbe al mondo
    // 0.56.2: in modalita' cursore i clic fuori dalle sue finestre vanno all'eroe 2 nel mondo (NativePad2Tick)
    // 0.56.3 (controllo): la sua barra sempre (armi, arti, pozioni; gli altri pulsanti la barra li ignora per il secondo mouse)
    const bool onUi = (side && MemoryDualPointInUi(px, py)) || MemoryHero2BarPoint(px, py);
    // 0.56.6: con lo zaino aperto e un oggetto in mano, A sul terreno lo getta a terra (come il giocatore 1)
    const bool dropHere = !onUi && MemoryMouse2Held() && !MemoryMouse2OverUi();
    // 0.56.54: conferma dell'acquisto aperta dal giocatore 2: A = "Si'", B = "No" (come il pad 1 in CursorTick)
    {
        int owner = 0;
        RECT yes{}, no{};
        if ((pressed & (PAD_A | PAD_B)) && !s.p2Left && !s.p2Right && MemoryPurchaseButtons(owner, yes, no) && owner == 2) {
            const RECT& r = (pressed & PAD_A) ? yes : no;
            const LONG x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
            MemoryMouse2Button(0, true, x, y);
            MemoryMouse2Button(0, false, x, y);
            Log("Mercante: conferma dell'acquisto del giocatore 2, %s = clic su \"%s\" in (%ld,%ld)", (pressed & PAD_A) ? "A" : "B", (pressed & PAD_A) ? "Si'" : "No", x, y);
            return;
        }
    }
    if ((pressed & (PAD_A | PAD_B)) && !onUi && !curMode && !((pressed & PAD_A) && dropHere))
        Log("Puntatore del giocatore 2: clic in (%ld,%ld) fuori dagli zaini, ignorato", px, py);
    if ((pressed & PAD_A) && !s.p2Left && !s.p2WorldA) {
        if (onUi) { MemoryMouse2Button(0, true, px, py); s.p2Left = true; }
        else if (dropHere && !curMode) { MemoryMouse2Drop(px, py); Log("Puntatore del giocatore 2: A sul terreno con un oggetto in mano in (%ld,%ld): lo getta", px, py); }
        else if (curMode) { MemoryPad2WorldButton(0, true, px, py); s.p2WorldA = true; }
    }
    if ((pressed & PAD_B) && !s.p2Right && !s.p2WorldB) {
        if (onUi) { MemoryMouse2Button(2, true, px, py); s.p2Right = true; }
        else if (curMode) { MemoryPad2WorldButton(2, true, px, py); s.p2WorldB = true; }
    }
    if ((released & PAD_A) && s.p2Left) { MemoryMouse2Button(0, false, px, py); s.p2Left = false; }
    if ((released & PAD_B) && s.p2Right) { MemoryMouse2Button(2, false, px, py); s.p2Right = false; }
    if ((released & PAD_A) && s.p2WorldA) { MemoryPad2WorldButton(0, false, px, py); s.p2WorldA = false; }
    // 0.56.9: X dal mercante vende l'oggetto sotto il cursore (Shift+clic, come X del pad 1)
    if ((pressed & PAD_X) && shop && MemoryMerchantShared() && onUi && !s.p2Left) {
        MemoryMouse2Button(0, true, px, py, true);
        MemoryMouse2Button(0, false, px, py, true);
        Log("X del pad 2: vendita al mercante, Shift+clic in (%ld,%ld)", px, py);
    }
    if ((released & PAD_B) && s.p2WorldB) { MemoryPad2WorldButton(2, false, px, py); s.p2WorldB = false; }
    OverlaySetPointers(true, s.cursorMode, static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), px, py, 0);
}

void Pad2Pointer(Loop& s, const Frame& f)
{
    MemoryPad1GameMode(!s.cursorMode);
    if (MemoryMouse2Ready()) { Pad2PointerM2(s, f); return; }
    const bool want = MemoryDualSide(2) && MemoryDualActive() && !NativePad2HasPointer() && !s.helpOpen;
    PadState p2; int idx2 = -1;
    const bool have = want && PadRead2(p2, idx2);
    if (!have) {
        if (s.p2On) {
            s.p2On = false;
            if (s.dualOwner == 2) Pad2Release(s, "puntatore del giocatore 2 spento");
            s.p2mc = 0;
            if (s.dualOwner == 2 && s.cursorMode) {
                SetVirtualClient(s, static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), f.w, f.h);
                SendMove(s.hwnd);
            }
            s.dualOwner = 1;
            Log("ZAINI: puntatore del giocatore 2 spento (%s)", !want ? "lato chiuso" : "pad 2 non letto");
        }
        OverlaySetPointers(false, false, 0, 0, 0, 0, 1);
        return;
    }
    if (!s.p2On) {
        s.p2On = true;
        s.p2Prev = p2.buttons;                            // niente fronti finti: i pulsanti gia' premuti contano dal rilascio
        s.p2Tick = f.now;
        GridInfo g;
        if (MemoryHero2Grid(g)) { s.p2x = g.x + 16.0f; s.p2y = g.y + 16.0f; }
        else { s.p2x = f.w * 0.75f; s.p2y = f.h * 0.5f; }
        if (!s.cursorMode) {
            ReleaseAll(s, "puntatore al giocatore 2");    // il pad 1 gioca: niente clic a meta' sul mondo
            s.dualOwner = 2;
        }
        Log("ZAINI: puntatore del giocatore 2 acceso in (%.0f,%.0f), mouse del gioco al giocatore %d", s.p2x, s.p2y, s.dualOwner);
    }
    const WORD b = p2.buttons;
    const WORD pressed = static_cast<WORD>(b & ~s.p2Prev), released = static_cast<WORD>(s.p2Prev & ~b);
    s.p2Prev = b;
    float dt = (f.now - s.p2Tick) / 1000.0f;
    if (dt < 0.0f || dt > 0.05f) dt = 0.008f;
    s.p2Tick = f.now;
    const float speed = g_cfg.cursorSpeed * f.hf;
    float vx1, vy1, vx2, vy2;
    StickVelocity(p2.lx, p2.ly, g_cfg.leftDeadzone, speed, vx1, vy1);
    StickVelocity(p2.rx, p2.ry, g_cfg.rightDeadzone, speed * g_cfg.cursorFine, vx2, vy2);
    bool moved = vx1 != 0.0f || vy1 != 0.0f || vx2 != 0.0f || vy2 != 0.0f;
    // 0.55.59: prova del 01/11 alle 16:07 ("A del giocatore 2 prende le cose una casella piu' in basso"): il gioco legge la
    // posizione del mouse quando elabora il clic (GetCursorPos, cioe' g_virt in quel momento), non quella del messaggio; con lo
    // stick ancora inclinato in quei 30 ms il puntatore si era gia' spostato di circa una casella (clic a (1794,447), preso a
    // (1774,415)). Alla pressione e al rilascio di A e B il puntatore resta fermo per 150 ms.
    if ((pressed | released) & (PAD_A | PAD_B)) s.p2FreezeUntil = f.now + 150;
    if (static_cast<LONG>(f.now - s.p2FreezeUntil) < 0) { vx1 = vy1 = vx2 = vy2 = 0.0f; }
    s.p2x += (vx1 + vx2) * dt;
    s.p2y += (vy1 + vy2) * dt;
    const WORD dpad = PAD_DPAD_LEFT | PAD_DPAD_RIGHT | PAD_DPAD_UP | PAD_DPAD_DOWN;
    if ((pressed & dpad) && Pad2GridStep(s, pressed)) moved = true;
    const float maxX = static_cast<float>(f.w - 2), maxY = static_cast<float>(f.h - 2);
    s.p2x = s.p2x < 1.0f ? 1.0f : (s.p2x > maxX ? maxX : s.p2x);
    s.p2y = s.p2y < 1.0f ? 1.0f : (s.p2y > maxY ? maxY : s.p2y);
    const WORD act = static_cast<WORD>(pressed & (PAD_A | PAD_B | PAD_LB | PAD_RB | dpad));
    if (moved || act) s.p2InputAt = f.now;
    // oggetto in mano: di chi aveva il mouse quando e' comparso
    const bool held = MemoryHeldAny();      // 0.55.66: anche un'arte o una combo
    if (held && !s.heldBy) { s.heldBy = s.dualOwner; Log("ZAINI: oggetto in mano al giocatore %d", s.heldBy); }
    if (!held) s.heldBy = 0;
    // Y: chiude il suo lato (anche senza il mouse)
    if (pressed & PAD_Y) {
        if (s.dualOwner == 2) Pad2Release(s, "Y del pad 2");
        DualToggle(s, f, 2, "Y del pad 2");
        return;
    }
    const bool p1Busy = s.cursorLeft || s.cursorRight || s.mcStep || s.dlgClick || s.heldBy == 1;
    if (s.dualOwner == 1) {
        if (!s.cursorMode && !p1Busy) {
            s.dualOwner = 2;
            Log("ZAINI: mouse del gioco al giocatore 2 (il giocatore 1 gioca)");
        }
        else if (act || moved) {
            const bool p1Recent = f.now - s.p1InputAt < 300;
            if (!p1Busy && (act || !p1Recent)) {
                s.dualOwner = 2;
                Log("ZAINI: mouse del gioco al giocatore 2");
            }
            else if (act & (PAD_A | PAD_B | PAD_LB | PAD_RB)) {
                static DWORD toastAt = 0;
                if (f.now - toastAt > 2000) {
                    toastAt = f.now;
                    OverlayCoopToast(Tr(s.heldBy == 1 ? "Aspetta: il giocatore 1 ha un oggetto in mano" : "Aspetta: il giocatore 1 sta usando il puntatore"), 2000);
                }
            }
        }
    }
    if (s.dualOwner == 2) {
        if (!g_virt.active.load()) Activate(f.real);   // 0.55.56 (controllo): dopo il mouse vero il puntatore virtuale va riacceso
        // clic sulle schede della copia (LB/RB), in tre tempi come StartMacroClick, con il mouse vero sul punto
        if (s.p2mc && static_cast<LONG>(f.now - s.p2mcAt) >= 0) {
            if (s.p2mc == 1) { SendLeftDown(s.hwnd); s.p2mc = 2; s.p2mcAt = f.now + 80; }
            else if (s.p2mc == 2) { SendLeftUp(s.hwnd); s.p2mc = 3; s.p2mcAt = f.now + 80; }
            else {
                if (s.p2RealMoved) { s.p2RealMoved = false; if (RealSetCursorPos(s.p2Real.x, s.p2Real.y)) s.lastReal = s.p2Real; }
                s.p2mc = 0;
            }
        }
        if (!s.p2mc && (pressed & (PAD_LB | PAD_RB))) {
            int a = 0, n = 0; RECT tr[8] = {};
            if (MemoryDualCopyTabs(a, n, tr, 8) && n > 0) {
                const bool next = (pressed & PAD_RB) != 0;
                const int to = (a + (next ? 1 : n - 1)) % n;
                const LONG x = (tr[to].left + tr[to].right) / 2, y = (tr[to].top + tr[to].bottom) / 2;
                SetVirtualClient(s, x, y, f.w, f.h);
                s.p2RealMoved = false;
                if (RealGetCursorPos(&s.p2Real)) {
                    POINT p{ x, y };
                    ClientToScreen(s.hwnd, &p);
                    if (RealSetCursorPos(p.x, p.y)) { s.p2RealMoved = true; s.lastReal = p; }
                }
                SendMove(s.hwnd);
                s.p2mc = 1; s.p2mcAt = f.now + 80;
                Log("%s del pad 2: scheda %d -> %d di %d dello zaino dell'eroe 2, clic in (%ld,%ld)", next ? "RB" : "LB", a + 1, to + 1, n, x, y);
            }
            else Log("%s del pad 2: schede dello zaino dell'eroe 2 non leggibili", (pressed & PAD_RB) ? "RB" : "LB");
        }
        if (!s.p2mc) {
            SetVirtualClient(s, static_cast<LONG>(s.p2x), static_cast<LONG>(s.p2y), f.w, f.h);
            MoveIfChanged(s);
            // 0.55.56 (controllo): solo sulle finestre dei due zaini; fuori il clic andrebbe al mondo, cioe' all'eroe 1
            // 0.55.67 (controllo): anche sulla sua barra, ma solo con qualcosa in mano (un'arte o una pozione da posare): senza, i
            // pulsanti della barra farebbero partire lo scambio dell'interfaccia, che chiude i due zaini
            const bool onUi = MemoryDualPointInUi(static_cast<LONG>(s.p2x), static_cast<LONG>(s.p2y)) ||
                              (s.heldBy == 2 && MemoryHero2BarPoint(static_cast<LONG>(s.p2x), static_cast<LONG>(s.p2y)));
            if ((pressed & (PAD_A | PAD_B)) && !onUi)
                Log("Puntatore del giocatore 2: clic in (%.0f,%.0f) fuori dagli zaini, ignorato", s.p2x, s.p2y);
            if ((pressed & PAD_A) && !s.p2Left && onUi) {
                SendLeftDown(s.hwnd); s.p2Left = true;
                Log("Puntatore del giocatore 2: sinistro giu' in (%.0f,%.0f)", s.p2x, s.p2y);
            }
            if ((pressed & PAD_B) && !s.p2Right && onUi) {
                SendRightDown(s.hwnd); s.p2Right = true;
                Log("Puntatore del giocatore 2: destro giu' in (%.0f,%.0f)", s.p2x, s.p2y);
            }
        }
    }
    if ((released & PAD_A) && s.p2Left) { SendLeftUp(s.hwnd); s.p2Left = false; }
    if ((released & PAD_B) && s.p2Right) { SendRightUp(s.hwnd); s.p2Right = false; }
    OverlaySetPointers(true, s.cursorMode, static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), static_cast<LONG>(s.p2x), static_cast<LONG>(s.p2y), s.dualOwner);
}

void Tick(Loop& s)
{
    g_ptrToM2 = false;                  // 0.56.7: il puntatore del giro prima non vale per l'inizio di questo
    HWND hwnd = s.hwnd;
    if (!s.started) {
        s.started = true;
        if (g_cfg.startInCursor) {
            // Il puntatore virtuale si attiva al primo uso dello stick o di A/B
            // (CursorTick), partendo dalla posizione del mouse.
            s.cursorMode = true;
            OverlaySetCursorMode(true);
            Log("Modalita' cursore ATTIVA (avvio)");
        }
    }
    Frame f{};
    f.now = GetTickCount();
    g_pad1AHeld.store(s.held != Click::None && (s.heldMask & PAD_A) != 0);   // 0.56.47: per la traccia dell'attacco del pad 2

    // Rilascio della pressione fisica della minimappa, appena trascorsa la
    // durata minima. Avviene anche se il gioco non e' piu' in primo piano.
    if (s.minimapDown && s.minimapRelease && f.now - s.minimapSince >= static_cast<DWORD>(g_cfg.minimapHoldMs)) {
        SendSystem(kTab, g_cfg.minimapMethod == 1, false);
        s.minimapDown = s.minimapRelease = false;
        Log("Minimappa chiusa");
    }

    // Il mouse vero ha la precedenza: se si muove, il pad si fa da parte.
    // 0.53.3: solo con un movimento voluto, almeno kMouseTakeoverPx pixel di strada in kMouseTakeoverMs. Prova sulla
    // Steam Machine del 29/10: un mouse lasciato sul divano si spostava di pochi pixel alla volta (x 47, 51, 57...) e a
    // ogni spostamento la mod spegneva il puntatore del pad, interrompendo i clic (schede del mercante del giocatore 2) e
    // lasciando il gioco a seguire il mouse (eroe 1 che andava da solo). Mai durante un clic della mod (mcStep), che
    // sposta il mouse vero anche lui. Nello schermo diviso il mouse e' condiviso e, con IgnoreMouse (predefinito), non
    // spegne il pad di nessuna finestra: le due finestre si usano con i pad (prima, dalla 0.20.7, lo spegneva nella
    // finestra in primo piano, e il clic sulle schede di una finestra, che sposta il mouse vero, spegneva l'altra).
    constexpr LONG kMouseTakeoverPx = 40;
    constexpr DWORD kMouseTakeoverMs = 300;
    if (RealGetCursorPos(&f.real)) {
        const LONG dx = f.real.x - s.lastReal.x, dy = f.real.y - s.lastReal.y;
        s.lastReal = f.real;
        if (dx || dy) {
            if (!s.mouseAccAt || f.now - s.mouseAccAt > kMouseTakeoverMs) { s.mouseAcc = 0; s.mouseAccAt = f.now; }
            s.mouseAcc += static_cast<LONG>(std::sqrt(static_cast<float>(dx * dx + dy * dy)) + 0.5f);
        }
        const bool busy = s.moveHeld || s.held != Click::None || s.pendingArt || s.mcStep != 0;
        f.mouseMoved = s.mouseAcc >= kMouseTakeoverPx && f.now - s.mouseAccAt <= kMouseTakeoverMs && !g_cfg.coopIgnoreMouse;
        if (f.mouseMoved) s.mouseAcc = 0;               // un solo segnale per tratto
        if (f.mouseMoved && g_virt.active.load() && !busy)
            Deactivate(s, "mouse mosso");
    }

    CoopTick(hwnd);
    NavTick(s, f);
    // In co-op ogni istanza lavora anche senza il focus (il pad e' suo, il mouse vero e' condiviso).
    if (GetForegroundWindow() != hwnd && !g_cfg.coopRole) {
        PadVibrate(0.0f, 0.0f);
        if (s.minimapDown) s.minimapRelease = true;   // mai un Tab incastrato fuori dal gioco
        s.altP2 = false;                              // 0.55.16: niente Alt del pad 2 fuori dal gioco (torna al rientro se e' ancora tenuto)
        s.pad2Prev = 0;
        ReleaseAll(s, "finestra non in primo piano");
        s.prevButtons = 0;
        return;
    }
    MemoryDiagTick(f.now);
    MemoryDiagProbe(hwnd);
    MemoryDiagUI(f.now);
    MemoryDiagScanUI();
    MemoryDiagDump();
    MemoryNativeProbe(hwnd);
    {   // 0.55.16: R3 e L3 del pad 2 (stato da NativePad2Tick, chiamata qui sopra): Alt e minimappa come quelli del pad 1 e
        // condivisi con lui. Prima di PadRead, cosi' valgono anche con il pad 1 scollegato o con PadSource=None.
        const WORD b2 = NativePad2Buttons();
        const WORD p2 = static_cast<WORD>(b2 & ~s.pad2Prev), r2 = static_cast<WORD>(s.pad2Prev & ~b2);
        s.pad2Prev = b2;
        if (p2 & PAD_RS) { s.altP2 = true; HoldKey(hwnd, kAlt, true); Log("R3 del pad 2: Alt premuto"); }
        if (r2 & PAD_RS) { s.altP2 = false; if (!s.altP1) HoldKey(hwnd, kAlt, false); Log("R3 del pad 2: Alt rilasciato"); }
        if (p2 & PAD_LS) {
            // 0.56.16: doppio L3 (due pressioni entro 400 ms): l'eroe 2 raggiunge l'eroe 1; la minimappa si apre e si richiude
            if (s.lsTapAt[2] && f.now - s.lsTapAt[2] < 400) { s.lsTapAt[2] = 0; MemoryJoinCompanion(2); }
            else s.lsTapAt[2] = f.now ? f.now : 1;
            if (!s.minimapDown) {
                MinimapTakeFocus(hwnd);
                SendSystem(kTab, g_cfg.minimapMethod == 1, true);
                s.minimapDown = true;
                s.minimapRelease = false;
                s.minimapSince = f.now;
                s.minimapOwner = 2;
                Log("L3 del pad 2: minimappa aperta (%s fisico, modalita' %s)",
                    g_cfg.minimapMethod == 1 ? "clic centrale" : "Tab", g_cfg.minimapToggle ? "Toggle" : "Hold");
            }
            else if (g_cfg.minimapToggle) s.minimapRelease = true;   // seconda pressione (di uno dei due): chiude
        }
        if ((r2 & PAD_LS) && s.minimapDown && !g_cfg.minimapToggle && s.minimapOwner == 2)
            s.minimapRelease = true;                    // Hold: chiude al rilascio di chi l'ha aperta
    }
    {   // F9 (0.20.3): elenco dei pulsanti visibili della schermata corrente
        static bool f9 = false;
        const bool d = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (d && !f9) MemoryNetDump();
        f9 = d;
    }
    MemoryDialogWatch();
    UnlockedSlots(f.now);                       // registra nel log i cambi di livello

    // Forziere o oggetto scelto con A: quando l'eroe lo raggiunge A non lo sceglie piu' (0.9.3
    // forzieri, dal 0.9.6 anche gli oggetti: i cSpecial3D del 22/10 non sparivano mai). Distanza
    // di 120 unita' (i forzieri si aprono a 50-94). Un oggetto raccolto sparisce dal vettore.
    // 0.11.11: i forzieri no (aperti = +0x64 0xAA, li esclude gia' MemoryFindTarget) ne' le porte, e
    // solo 600 ms dopo la scelta: alle 15:33:47 il baule 1342 era stato segnato "raggiunto" 18 ms
    // dopo A, prima ancora del clic, perche' l'eroe gli stava gia' a 104 unita'.
    if (s.talkPick.addr && s.talkPick.cls < 0 && MemoryIsChest(s.talkPick)) s.talkPick = Target{};
    if (s.talkPick.addr && strncmp(s.talkPick.kind, "porta", 5) == 0) s.talkPick = Target{};
    if (s.talkPick.addr && s.talkPick.cls < 0 && f.now - s.usedCheck >= 250 && f.now - s.talkLast >= 600) {
        s.usedCheck = f.now;
        float d = 0;
        const bool ok = MemoryHeroDistance(s.talkPick, d);
        if (ok && d < s.talkPickMin) s.talkPickMin = d;
        if (ok && d < 120.0f) {
            MemoryMarkUsed(s.talkPick);
            Log("%s id %lu raggiunto (distanza %.0f): A non lo sceglie piu'",
                MemoryIsChest(s.talkPick) ? "Forziere" : "Oggetto", s.talkPick.id, d);
            s.talkPick = Target{};
        }
        else if (!ok || f.now - s.talkLast > 10000) {
            Log("%s id %lu %s (distanza minima %.0f)", MemoryIsChest(s.talkPick) ? "Forziere" : "Oggetto",
                s.talkPick.id, ok ? "non raggiunto" : "sparito (raccolto?)", s.talkPickMin);
            s.talkPick = Target{};
        }
    }

    PadState pad;
    PadRead(pad);
    {   // 0.49.4: traccia del collegamento del pad
        static int lastConn = -1;
        if (static_cast<int>(pad.connected) != lastConn) { lastConn = pad.connected; Log("Pad 1: %s", pad.connected ? "collegato" : "NON collegato"); }
    }
    // --- 0.50.0 (N4, fase A): puntatore del giocatore 2 ---
    // Lo scambio partito dal Back del pad 2 da' a lui anche il puntatore: da qui in giu' tutta la logica del pad 1 (modalita'
    // cursore, caselle dello zaino con la croce, A/B, trascinamento, schede, mercante...) lavora con lo stato del pad 2, e il
    // pad 1 resta fermo (0.51.0: con lo scambio della sola interfaccia il suo stick muove l'eroe 1, piu' sotto). Back e Start del pad 2 si tolgono: Back lo gestisce NativePad2Tick (ritorno all'eroe 1), Start
    // sarebbe Esc e chiuderebbe lo scambio. Se il pad 2 non si legge piu', il puntatore torna al pad 1 finche' non si ricollega.
    bool usingP2 = false;
    g_ptrToM2 = false;                  // 0.56.7: acceso piu' sotto, con lo scambio e il secondo mouse
    const PadState pad1 = pad;          // 0.51.0: con il puntatore al pad 2, lo stick del pad 1 muove ancora l'eroe 1
    // 0.51.3: aiuto aperto dal pad 2 (Start tenuto, richiesta da NativePad2Tick); mentre e' suo, anche qui si usa il pad 2
    if (NativePad2TakeHelpReq() && !s.helpOpen) OpenHelp(s, 2, "Start tenuto sul pad 2");
    const bool helpP2 = s.helpOpen && g_helpOwner == 2;
    if (NativePad2HasPointer() || helpP2) {
        static bool p2Lost = false;
        PadState p2; int idx2 = -1;
        if (PadRead2(p2, idx2)) {
            if (p2Lost) { p2Lost = false; Log("N4: pad 2 di nuovo letto, puntatore al giocatore 2"); }
            // con l'aiuto Start resta (lo chiude, come per il pad 1); con il puntatore no (Esc chiuderebbe lo scambio)
            p2.buttons = static_cast<WORD>(p2.buttons & ~(helpP2 ? PAD_BACK : (PAD_BACK | PAD_START)));
            pad = p2;
            usingP2 = true;
        } else if (!p2Lost) {
            p2Lost = true;
            Log("N4: pad 2 non letto, il puntatore torna al pad 1 finche' non si ricollega");
            OverlayCoopToast(Tr("Giocatore 2: controller scollegato, il puntatore torna al giocatore 1"), 3000);
        }
    }
    // 0.55.27: menu del gioco aperto (Esc, opzioni, salvataggi): lo usa anche il pad 2, insieme al pad 1. Croce, A, B e Y si
    // sommano a quelli del pad 1 e vale lo stick piu' inclinato dei due; Start e Back no (Start lo gestisce NativePad2Tick, che
    // chiude il menu come l'ha aperto), LB e i grilletti no (in modalita' cursore LB beve la pozione dell'eroe 1). Prova del
    // 31/10 sulla Steam Machine: dopo Start del pad 2 le voci si scorrevano solo con il pad 1. Senza pad 1 (PadSource=None) il
    // menu resta al mouse e il pad 2 lo chiude con Start.
    if (!usingP2 && pad.connected && g_cfg.nativePad2 && MemoryGameMenuOpen()) {
        PadState p2; int idx2 = -1;
        if (PadRead2(p2, idx2)) {
            pad.buttons |= static_cast<WORD>(p2.buttons & (PAD_DPAD_UP | PAD_DPAD_DOWN | PAD_DPAD_LEFT | PAD_DPAD_RIGHT | PAD_A | PAD_B | PAD_Y));
            if (p2.lx * p2.lx + p2.ly * p2.ly > pad.lx * pad.lx + pad.ly * pad.ly) { pad.lx = p2.lx; pad.ly = p2.ly; }
            if (p2.rx * p2.rx + p2.ry * p2.ry > pad.rx * pad.rx + pad.ry * pad.ry) { pad.rx = p2.rx; pad.ry = p2.ry; }
        }
    }
    if (!pad.connected) {
        PadVibrate(0.0f, 0.0f);
        if (s.minimapDown && s.minimapOwner != 2) s.minimapRelease = true;   // 0.55.16: quella del pad 2 resta
        // 0.55.18: senza pad 1 (PadSource=None: il giocatore 1 usa mouse e tastiera) il giro finisce qui, prima del blocco del
        // puntatore del giocatore 2: il ritorno del puntatore dopo il Back del pad 2 non si vedeva (restavano il cursore del
        // giocatore 2, la modalita' cursore e il puntatore virtuale, con il mouse vero che non comandava) e l'evento 2 (zaino da
        // chiudere) non si leggeva. Qui si fa il ritorno al mouse; l'evento 1 (puntatore al pad 2) si rimette in coda, lo legge
        // il blocco del puntatore al giro dopo, con il pad 2 letto.
        if (s.ptrSrc == 2) {
            s.ptrSrc = 1;
            OverlaySetCursorPlayer2(false);
            if (s.cursorMode) SetCursorMode(s, f, false, "puntatore al giocatore 1, senza pad 1");
            Deactivate(s, "puntatore al giocatore 1, senza pad 1: mouse");
            Log("N4: puntatore al giocatore 1 (nessun pad 1: mouse e tastiera)");
        }
        const int ev = NativePad2UiEvent();
        if (ev == 1) NativePad2UiEventPutBack(1);
        else if (ev) {
            s.invOpenAt = 0;
            if (ev == 2 && MemoryWindowOpen(1)) { TapKey(hwnd, kKeyI); Log("N4: Back del pad 2, zaino chiuso (%s)", kKeyI.name); }
        }
        Pad2DialogsWithoutPad1(s, f, hwnd);        // 0.55.29: A e B del pad 2 nei dialoghi anche senza pad 1
        // 0.20.8: senza pad (l'ospite sul PC) i clic della navigazione automatica devono comunque finire:
        // ReleaseAll azzerava il clic programmato e il gioco non lo vedeva mai.
        if (s.mcStep) {
            RECT cr0{};
            GetClientRect(hwnd, &cr0);
            f.w = cr0.right; f.h = cr0.bottom;
            MacroClickTick(s, f);
        }
        else ReleaseAll(s, "pad scollegato");
        if (!s.mcStep && s.p2DlgClickNoPad1) {         // 0.55.29: clic sul dialogo finito, il mouse torna al giocatore 1
            s.p2DlgClickNoPad1 = false;
            Deactivate(s, "clic del pad 2 sul dialogo finito, senza pad 1: mouse al giocatore 1");
        }
        s.prevButtons = 0;
        return;
    }
    // Grilletti trattati come pulsanti.
    if (pad.lt > 0.5f) pad.buttons |= PAD_LT;
    if (pad.rt > 0.5f) pad.buttons |= PAD_RT;

    {   // 0.50.0 (N4): cambio di chi comanda il puntatore e apertura/chiusura dello zaino dell'eroe 2
        DWORD& openAt = s.invOpenAt;
        const int src = usingP2 ? 2 : 1;
        const bool swapM2 = usingP2 && !helpP2 && NativePad2HasPointer() && MemoryMouse2Ready();   // 0.56.7
        if (src != s.ptrSrc) {
            ReleaseAll(s, src == 2 ? "puntatore al giocatore 2" : "puntatore al giocatore 1");
            s.prevButtons = pad.buttons;          // nessun fronte finto: i pulsanti gia' premuti contano solo dal rilascio
            s.ptrSrc = src;
            Log("N4: puntatore al pad %d%s", src, swapM2 ? " (secondo mouse: il mouse del giocatore 1 resta suo)" : "");
            // 0.56.7: con il secondo mouse il cursore del pad 1 si tiene da parte e il giocatore 2 parte dal centro della sua meta'
            if (src == 2 && swapM2) {
                s.p1Kept = true; s.p1KeepX = s.cx; s.p1KeepY = s.cy; s.p1KeepCursor = s.cursorMode;
                s.cx = f.w * 0.75f; s.cy = f.h * 0.5f;
                g_m2OutX = static_cast<LONG>(s.cx); g_m2OutY = static_cast<LONG>(s.cy);
            }
            if (src == 1 && s.p1Kept) {
                s.p1Kept = false;
                M2OutRelease();
                MemoryMouse2Ui(false, 0, 0);
                MemoryMouse2Stow("fine dello scambio dell'interfaccia");
                s.cx = s.p1KeepX; s.cy = s.p1KeepY;
                if (s.cursorMode != s.p1KeepCursor) SetCursorMode(s, f, s.p1KeepCursor, "giocatore 1 dopo lo scambio");
                Log("N4: cursore del pad 1 rimesso in (%.0f,%.0f), modalita' %s", s.cx, s.cy, s.cursorMode ? "cursore" : "gioco");
            }
            // a finestre gia' chiuse la modalita' automatica non scatterebbe piu' (reagisce solo ai cambi): il pad 1 torna a giocare
            if (src == 1 && s.cursorMode && s.uiState == 0) SetCursorMode(s, f, false, "puntatore al giocatore 1, finestre chiuse");
        }
        OverlaySetCursorPlayer2(src == 2);
        // 0.56.7: da qui il puntatore del pad 2 esce sul secondo mouse, disegnato dal gioco con il "2" accanto
        g_ptrToM2 = swapM2;
        MemorySwapMouse2(swapM2);
        if (swapM2) {
            g_m2OutX = static_cast<LONG>(s.cx); g_m2OutY = static_cast<LONG>(s.cy);
            MemoryMouse2Ui(true, g_m2OutX, g_m2OutY);
            OverlaySetCursor2Num(true, g_m2OutX, g_m2OutY);
        }
        const int ev = NativePad2UiEvent();
        if (ev == 1) {
            SetCursorMode(s, f, true, "giocatore 2");
            OverlayCoopToast(Tr("Giocatore 2: interfaccia del tuo eroe, Back per tornare"), 3000);
            // zaino dopo che il thread del motore ha cambiato l'eroe locale; 0.55.0: non sopra un dialogo, il mercante o i portali;
            // 0.55.2: nemmeno con lo scambio partito da solo per un servizio (la finestra la apre il PNG all'arrivo dell'eroe 2)
            // 0.55.37: lo zaino solo con Back tenuto, come View del pad 1 (prova del 31/10 sera: "modalita' cursore del pad 2 apre
            // l'inventario del giocatore 2"); premuto e lasciato da' solo il puntatore e l'interfaccia dell'eroe 2
            if (!MemoryWindowOpen(1) && s.uiState <= 0 && !NativePad2AutoSwap())
                openAt = f.now + static_cast<DWORD>(g_cfg.viewHoldMs > 150 ? g_cfg.viewHoldMs : 150);
        }
        else if (ev == 2) {
            openAt = 0;
            // 0.55.50: con i due zaini lo zaino resta aperto (e' anche dell'eroe 1); lo chiude Back tenuto, come View
            if (MemoryWindowOpen(1) && !MemoryDualActive()) { TapKey(hwnd, kKeyI); Log("N4: Back del pad 2, zaino chiuso (%s)", kKeyI.name); }
        }
        if (NativePad2TakeInvToggle() && usingP2) {
            openAt = 0;
            TapKey(hwnd, kKeyI);
            Log("N4: Back del pad 2 tenuto, zaino %s (%s)", MemoryWindowOpen(1) ? "chiuso" : "aperto", kKeyI.name);
        }
        else if (ev == 3) openAt = 0;
        if (openAt && static_cast<LONG>(f.now - openAt) >= 0) {
            openAt = 0;
            PadState pb; int ib = -1;
            const bool backHeld = PadRead2(pb, ib) && (pb.buttons & PAD_BACK);
            if (NativePad2HasPointer() && !MemoryWindowOpen(1) && backHeld) { TapKey(hwnd, kKeyI); Log("N4: Back del pad 2 tenuto, zaino dell'eroe 2 aperto (%s)", kKeyI.name); }
        }
        // 0.51.0 (N4 fase B): con lo scambio della sola interfaccia il mondo continua; il pad 1 non ha il puntatore (e' del pad
        // 2), quindi il suo stick sinistro manda ordini di movimento diretti all'eroe 1 (MemoryMoveHero1 non fa nulla con lo
        // scambio della 0.50, che ferma il mondo). Solo movimento: niente attacchi ne' arti finche' il pad 2 usa l'interfaccia.
        if (usingP2 && pad1.connected) {
            static DWORD moveAt = 0, atkAt = 0;
            static bool moving = false;
            static WORD prev1 = 0;
            float mdx = 0, mdy = 0, mt = 0;
            bool stick = StickDir(pad1.lx, pad1.ly, g_cfg.leftDeadzone, mdx, mdy, mt);
            // 0.56.7: con il secondo mouse il giocatore 1 combatte anche (come con i due zaini nella 0.55.55): A tenuto attacca il
            // nemico nella direzione dello stick (o il piu' vicino), X Y RB RT LT lanciano le arti 1-5, tutto con ordini diretti
            if (swapM2) {
                WORD b1 = pad1.buttons;
                if (pad1.lt > 0.5f) b1 |= PAD_LT;
                if (pad1.rt > 0.5f) b1 |= PAD_RT;
                const WORD p1 = static_cast<WORD>(b1 & ~prev1);
                prev1 = b1;
                auto enemy = [&](DWORD& id) {
                    Target t;
                    const bool dir = mdx != 0.0f || mdy != 0.0f;
                    if (MemoryFindTarget(false, f.centerX, f.centerY, dir ? mdx : 0.0f, dir ? mdy : 0.0f, g_cfg.lockRange * f.hf,
                                         g_cfg.lockNear * f.hf, t) && t.cls >= 0) { id = t.id; return true; }
                    return false;
                };
                if ((b1 & PAD_A) && static_cast<LONG>(f.now - atkAt) >= 250) {
                    atkAt = f.now;
                    DWORD id = 0;
                    if (enemy(id)) MemoryHero1Attack(id);
                }
                for (const ArtButton& ab : kArts) {
                    if (!(p1 & ab.mask)) continue;
                    DWORD id = 0;
                    enemy(id);
                    MemoryHero1Art(ab.slot - 1, id);
                    Log("N4: %s del pad 1 durante lo scambio: arte %d %s", ab.name, ab.slot, id ? "sul nemico" : "sull'eroe");
                }
                if (b1 & PAD_A) stick = false;           // con A tenuto lo stick mira
            }
            if (stick && static_cast<LONG>(f.now - moveAt) >= 150) {
                moveAt = f.now;
                const bool sent = MemoryMoveHero1(mdx, mdy);
                if (sent && !moving) Log("N4: il pad 1 muove l'eroe 1 mentre il pad 2 usa l'interfaccia");
                moving = sent;
            }
            else if (!stick) moving = false;
        }
    }

    RECT cr;
    GetClientRect(hwnd, &cr);
    f.w = cr.right; f.h = cr.bottom;
    f.hf = static_cast<float>(f.h);
    f.centerX = f.w * 0.5f + g_cfg.centerOffsetX * f.hf;
    f.centerY = f.h * 0.5f + g_cfg.centerOffsetY * f.hf;
    {   // 0.49.2: con la camera sul punto medio dei due eroi l'eroe 1 non e' al centro: stick e mira partono da lui
        // 0.53.4: dai piedi dell'eroe come li disegna il gioco (MemoryCameraHeroScreen). Prima lo scarto della proiezione,
        // sbagliato con la camera spostata (prova sulla Steam Machine del 30/10: l'eroe 1 tirato verso sinistra), e in
        // piu' sommato al CenterOffsetY, che gia' porta il centro sui piedi.
        float hx = 0, hy = 0;
        if (MemoryCameraHeroScreen(hx, hy)) { f.centerX = hx + g_cfg.centerOffsetX * f.hf; f.centerY = hy; }
    }
    f.ranged  = (g_cfg.aimMode == AimMode::Ranged);

    float mt = 0, at = 0;
    f.moving = StickDir(pad.lx, pad.ly, g_cfg.leftDeadzone, f.mdx, f.mdy, mt);
    f.aiming = StickDir(pad.rx, pad.ry, g_cfg.rightDeadzone, f.adx, f.ady, at);
    if (f.aiming) {
        float r = (g_cfg.aimMinRadius + (g_cfg.aimRadius - g_cfg.aimMinRadius) * at) * f.hf;
        f.aimX = static_cast<LONG>(f.centerX + f.adx * r);
        f.aimY = static_cast<LONG>(f.centerY + f.ady * r);
        s.dirX = f.adx; s.dirY = f.ady;
    }
    else if (f.moving) {
        s.dirX = f.mdx; s.dirY = f.mdy;
    }

    const WORD buttons  = pad.buttons;
    const WORD pressed  = buttons & ~s.prevButtons;
    const WORD released = ~buttons & s.prevButtons;
    s.prevButtons = buttons;

    // 0.55.54: due zaini, un lato per giocatore (lati allineati alla finestra, Back del pad 2) e puntatore del giocatore 2
    DualSync(s, f);
    if (NativePad2TakeSideReq() && MemoryDualPossible()) DualToggle(s, f, 2, "Back del pad 2");
    {
        float a = 0, b = 0, c = 0;
        if ((pressed & ~PAD_BACK) || StickDir(pad.lx, pad.ly, g_cfg.leftDeadzone, a, b, c) || StickDir(pad.rx, pad.ry, g_cfg.rightDeadzone, a, b, c))
            s.p1InputAt = f.now;
    }
    Pad2Pointer(s, f);

    // --- Aiuto (0.15.0): Start tenuto 600 ms apre o chiude; dentro, croce sx/dx = controller, B o
    // Start = chiudi; con l'aiuto aperto gli altri comandi sono fermi. Start breve resta il menu (Esc,
    // ora al rilascio).
    constexpr DWORD kHelpHoldMs = 600;
    if (pressed & PAD_START) { s.startSince = f.now; s.startLong = false; }
    if ((buttons & PAD_START) && s.startSince && !s.startLong && f.now - s.startSince >= kHelpHoldMs) {
        s.startLong = true;
        if (s.helpOpen) CloseHelp(s, "Start tenuto");
        else OpenHelp(s, 1, "Start tenuto");      // il pad 2 lo apre da NativePad2Tick (richiesta raccolta in Tick)
    }
    VibrationTick(s, f);
    if (s.helpOpen) {
        // 0.17.0: croce su/giu' sceglie la riga (Controller, Vibrazione; 0.51.3: Controller del giocatore 2), sinistra/destra la cambia.
        if (pressed & (PAD_DPAD_UP | PAD_DPAD_DOWN)) {
            // 0.53.3: nello schermo diviso niente riga del giocatore 2 (0.55.25: tolta la quarta riga, raccolta automatica)
            const int step = (pressed & PAD_DPAD_DOWN) ? 1 : 2;
            const int rows = 4;                     // 0.56.16: quarta riga, indicatori dei giocatori
            const int stepR = (pressed & PAD_DPAD_DOWN) ? 1 : rows - 1;
            (void)step;
            do s.helpRow = (s.helpRow + stepR) % rows; while (g_cfg.coopRole && s.helpRow == 1);
            OverlaySetHelpRow(s.helpRow);
        }
        // 0.55.68: LB / RB sfogliano le pagine; i tasti premuti (e gli stick inclinati) si accendono nel disegno
        if (pressed & (PAD_LB | PAD_RB)) {
            s.helpPage = (s.helpPage + ((pressed & PAD_RB) ? 1 : 2)) % 3;
            OverlaySetHelpPage(s.helpPage);
        }
        {
            WORD lit = buttons;
            if (std::fabs(pad.lx) > 0.5f || std::fabs(pad.ly) > 0.5f) lit |= PAD_LS;
            if (std::fabs(pad.rx) > 0.5f || std::fabs(pad.ry) > 0.5f) lit |= PAD_RS;
            OverlaySetHelpButtons(lit);
        }
        // 0.51.5: righe 0 controller del giocatore 1, 1 controller del giocatore 2, 2 vibrazione; 0.55.68: solo la croce
        // sinistra / destra cambia (A si accende senza cambiare niente, come gli altri tasti)
        const WORD change = static_cast<WORD>(pressed & (PAD_DPAD_LEFT | PAD_DPAD_RIGHT));
        const int step = (pressed & PAD_DPAD_LEFT) ? 2 : 1;
        if (change && s.helpRow == 0) {
            g_cfg.padLayout = (g_cfg.padLayout + step) % 3;
            SaveConfigValue("Overlay", "PadLayout", PadLayoutName(g_cfg.padLayout));
            Log("Aiuto: controller %s", PadLayoutName(g_cfg.padLayout));
        }
        else if (change && s.helpRow == 1) {
            g_cfg.pad2Layout = (g_cfg.pad2Layout + step) % 3;
            SaveConfigValue("Overlay", "Pad2Layout", PadLayoutName(g_cfg.pad2Layout));
            Log("Aiuto: controller del giocatore 2 %s", PadLayoutName(g_cfg.pad2Layout));
        }
        else if (change && s.helpRow == 3) {      // 0.56.16
            g_cfg.playerMarks = !g_cfg.playerMarks;
            SaveConfigValue("Overlay", "PlayerMarks", g_cfg.playerMarks ? "true" : "false");
            Log("Aiuto: indicatori dei giocatori %s", g_cfg.playerMarks ? "accesi" : "spenti");
        }
        else if (change) {
            g_cfg.vibration = !g_cfg.vibration;
            SaveConfigValue("Pad", "Vibration", g_cfg.vibration ? "true" : "false");
            if (g_cfg.vibration) s.vibTestUntil = f.now + 250;      // impulso di prova
            Log("Aiuto: vibrazione %s", g_cfg.vibration ? "attiva" : "disattivata");
        }
        const bool closeStart = (released & PAD_START) && s.startSince && !s.startLong;
        if ((pressed & PAD_B) || closeStart) CloseHelp(s, (pressed & PAD_B) ? "B" : "Start");
        if (released & PAD_START) s.startSince = 0;
        return;
    }

    // --- Modalita' cursore automatica (fase 2): segue le finestre del gioco ---
    // Si reagisce solo ai cambiamenti, cosi' Select e Start restano liberi di
    // cambiare modalita' a mano.
    if (g_cfg.autoCursor && f.now - s.uiCheck >= 50) {
        s.uiCheck = f.now;
        if (NativePad2TakeInteract()) {      // 0.55.0: l'eroe 2 va verso un PNG o un oggetto: come dopo un clic del pad 1
            s.dlgWatchUntil = f.now + 8000;
            s.p2InteractAt = f.now;
        }
        int win = MemoryUIState();
        // 0.55.54: aperto solo il lato dell'eroe 2: per il pad 1 nessuna finestra, continua a giocare
        if (win == 1 && MemoryDualSide(2) && !MemoryDualSide(1) && MemoryDualPossible()) win = 0;
        int ui = win;
        DialogInfo dd;
        if (s.winState == -1 && win == 0) {   // partita caricata: oggetti nuovi
            MemoryDialogReset(); MemoryUsedReset(); MemoryBusyReset();
            // 0.49.4: la schermata "Il sogno" dell'esercitazione nasce dopo la prima ricerca delle finestre dei dialoghi
            // (13:00:12 ricerca, 13:00:18 comparsa): per 20 s si cerca vicino ogni 300 ms, cosi' A funziona al primo colpo.
            // 0.49.8: con un tempo suo, perche' dlgWatchUntil fa anche la ricerca completa (circa 150 ms) a ogni A/B senza
            // dialogo, cioe' scatti a ogni attacco nei primi 20 s di ogni partita.
            s.loadWatchUntil = f.now + 20000;
            MemoryDialogTrustNew(f.now + 20000);   // 0.55.27: anche "Il sogno" gia' aperto alla ricerca (Steam Machine, 31/10)
            s.loadDumpAt = f.now + 3000;
        }
        if (s.loadDumpAt && static_cast<LONG>(f.now - s.loadDumpAt) >= 0) {
            s.loadDumpAt = 0;
            if (win == 0) { Log("Dialoghi: candidati 3 s dopo il caricamento:"); MemoryDialogDump(); }
        }
        // 0.14.0, diagnostica una tantum: elementi della barra in basso (cUI_Taskbar2, finestra n.0),
        // per mettere i simboli dei tasti sotto gli slot delle arti.
        static bool taskbarDumped = false;
        if (win == 0 && !taskbarDumped && g_cfg.memoryDiag) { taskbarDumped = true; MemoryWindowDump(0, "barra in basso"); }
        if (win == 0 && MemoryDialog(dd)) ui = 1;                     // anche i dialoghi con gli NPC
        // 0.55.29: mentre l'eroe 2 parla anche con una finestra del giocatore 1 aperta (zaino, mercante); anche mentre parla
        // l'eroe 1 (se ci arriva dopo gli 8 s dell'attesa partita con A)
        else if ((win == 0 && (static_cast<LONG>(s.dlgWatchUntil - f.now) > 0 || static_cast<LONG>(s.loadWatchUntil - f.now) > 0 ||
                               MemoryHeroTalking())) ||
                 NativePad2Talking()) {
            // Dopo un clic su un NPC (il personaggio a volte deve prima raggiungerlo) il
            // gioco crea la finestra del dialogo solo la prima volta: la si cerca finche' si aspetta.
            // 0.55.28: con la ricerca completa a fette (24 MB ogni 50 ms, tutta la memoria in circa un secondo) invece di quella
            // vicina ogni 300 ms, che sulla Steam Machine non vedeva le finestre nate lontano dalle altre (31/10, 0.55.27: il
            // dialogo dell'eroe 2 trovato dopo 16 s, la domanda sull'esercitazione mai); anche mentre l'eroe 2 parla.
            s.dlgQuick = f.now;
            MemoryDialogScanStep();
            if (MemoryDialog(dd)) ui = 1;
        }
        // Finestra di scelta dei portali (0.10.5): anche lei porta in modalita' cursore.
        s.busyOpen = (win == 0 && ui != 1 && MemoryBusyDlg());
        // 0.55.63: prova del 02/11 alle 18:24 (log): vicino a un PNG un "dialogo" senza pulsanti compariva per un solo controllo
        // (circa 60 ms) ogni secondo, e ogni volta il pad 1 passava alla modalita' cursore e prendeva il mouse del gioco: con lo
        // zaino dell'eroe 2 aperto un A del pad 1 e' diventato un clic nello zaino dell'eroe 2 (oggetto preso alle 18:24:26). Un
        // dialogo senza finestre del gioco conta solo se c'e' ancora dopo 150 ms.
        const bool dlgOnly = !s.busyOpen && ui == 1 && win == 0;
        if (!dlgOnly) s.dlgSince = 0;
        else if (s.uiState != 1) {
            if (!s.dlgSince) s.dlgSince = f.now ? f.now : 1;
            if (f.now - s.dlgSince < 150) ui = s.uiState;            // non ancora: come prima
        }
        if (s.busyOpen) ui = 1;
        else if (dlgOnly && ui == 1) { s.dlgWatchUntil = 0; s.talkDialogSeen = true; }
        s.winState = win;
        {   // 0.49.4: traccia dei cambi di stato (la schermata "Il sogno" non veniva vista)
            static int lastWin = -9, lastUi = -9;
            if (win != lastWin || ui != lastUi) { Log("UI: stato finestre %d, interfaccia %d", win, ui); lastWin = win; lastUi = ui; }
        }
        if (ui != s.uiState) {
            // 0.55.0: finestra aperta dall'interazione dell'eroe 2: al giocatore 2 si dice come usarla (i dialoghi con A e B,
            // il mercante e la scelta dei portali prendendo il puntatore con Back)
            if (ui == 1 && s.p2InteractAt && f.now - s.p2InteractAt < 10000 && !usingP2) {
                const bool dlg = win != 1 && !s.busyOpen;
                Log("PAD2: %s dopo l'interazione dell'eroe 2", win == 1 ? "finestra aperta" : s.busyOpen ? "finestra dei portali" : "dialogo aperto");
                OverlayCoopToast(Tr(dlg ? "Giocatore 2: A conferma, B rifiuta" : "Giocatore 2: Back per usare la finestra"), 4000);
            }
            if (ui == 1) s.p2InteractAt = 0;
            if (s.uiState != -2) {
                if (ui == 1)       SetCursorMode(s, f, true,  win == 1 ? "finestra aperta" :
                                                              s.busyOpen ? "finestra dei portali" : "dialogo aperto");
                // 0.50.0 (N4): con il puntatore al giocatore 2 si resta in modalita' cursore anche a finestre chiuse,
                // altrimenti lo stick del pad 2 comanderebbe il movimento (clic nel mondo) invece del puntatore
                else if (ui == 0 && usingP2) {
                    Log("N4: finestre chiuse, si resta in modalita' cursore (puntatore al giocatore 2)");
                    if (s.uiState == 1) s.p2ClosedAt = f.now ? f.now : 1;   // 0.55.49: se restano chiuse, puntatore al pad 1
                }
                else if (ui == 0)  SetCursorMode(s, f, false, s.uiState == -1 ? "partita caricata" : "finestre chiuse");
                else               SetCursorMode(s, f, true,  "menu principale");
            }
            s.uiState = ui;
        }
        // 0.55.49: il giocatore 2 chiude le sue finestre e il puntatore torna da solo al giocatore 1 (prova del 01/11: "ogni
        // tanto rimane la modalita' cursore del pg 2 e da' fastidio"; nel log della sera del 31/10 dopo ogni "finestre chiuse"
        // veniva un Back del pad 2 dopo 2 secondi). Si aspetta 900 ms di finestre chiuse: lo scambio richiude e riapre lo zaino
        // (tasto I due volte, 400 ms fra i due). Con un oggetto in mano no. Il puntatore preso con Back premuto e lasciato,
        // senza finestre, resta al giocatore 2 come prima.
        if (s.p2ClosedAt) {
            if (!usingP2 || s.uiState != 0) s.p2ClosedAt = 0;
            else if (static_cast<LONG>(f.now - s.p2ClosedAt) >= 900) {
                s.p2ClosedAt = 0;
                if (!MemoryHeldAny()) {
                    NativePad2ReturnPointer();
                    Log("N4: finestre del giocatore 2 chiuse, puntatore al giocatore 1");
                }
            }
        }
    }

    // --- Dialogo con un NPC aperto (fase 2): A = Invio (OK, Accetta), B = Rifiuta ---
    // Il pulsante Rifiuta sta in basso a destra del dialogo: dagli screenshot del
    // 22/10/2026 a 1080p, x = sinistra + 68% della larghezza, y = basso - 15 pixel
    // (OK e Accetta/Rifiuta a 14-15 pixel dal bordo inferiore letto in memoria).
    // Il clic e' diviso in tre tempi (puntatore sul pulsante, giu', su) come quello
    // del cursore, perche' l'interfaccia vuole vedere il puntatore sopra il pulsante.
    if (s.dlgClick && static_cast<LONG>(f.now - s.dlgClickAt) >= 0) {
        if (s.dlgClick == 1) { SendLeftDown(hwnd); s.dlgClick = 2; s.dlgClickAt = f.now + 60; }
        else                 { SendLeftUp(hwnd);   s.dlgClick = 0; }
    }
    // 0.55.0: A o B del pad 2 con un dialogo aperto (NativePad2Tick) valgono come quelli del pad 1
    const WORD p2Dlg = NativePad2TakeDialogBtn();
    const WORD dlgPressed = p2Dlg ? p2Dlg : static_cast<WORD>(pressed & (PAD_A | PAD_B));
    // 0.55.29: A e B del pad 2 valgono anche con una finestra del giocatore 1 aperta (s.winState 1: zaino, mercante): prima, se
    // il giocatore 1 guardava lo zaino mentre l'eroe 2 parlava con un personaggio, il giocatore 2 non poteva rispondere
    if (g_cfg.lockEnabled && dlgPressed && (s.winState != 1 || p2Dlg)) {
        DialogInfo d;
        bool open = MemoryDialog(d);
        // 0.55.28: anche per A o B del pad 2 con l'eroe 2 che parla (NativePad2Tick li manda qui anche senza finestra trovata)
        if (!open && (static_cast<LONG>(s.dlgWatchUntil - f.now) > 0 || p2Dlg) && f.now - s.dlgFull >= 2000) {
            // Ultima possibilita': ricerca completa (circa 150 ms) prima di cliccare sul mondo.
            s.dlgFull = f.now;
            MemoryDialogRescan(false);
            open = MemoryDialog(d);
            if (!open) MemoryDialogDump();
        }
        // 0.55.28: l'eroe 2 parla (modo 10, il dialogo del gioco e' aperto) ma la finestra non si trova: A del pad 2 = Invio, che
        // nel dialogo e' OK o Accetta (come A quando la finestra e' trovata). B non si puo' dare senza la posizione del pulsante.
        if (!open && p2Dlg && NativePad2Talking()) {
            if (p2Dlg & PAD_A) { TapKey(hwnd, kEnter); Log("Dialogo dell'eroe 2 non trovato in memoria: A del pad 2 = Invio"); }
            else Log("Dialogo dell'eroe 2 non trovato in memoria: B del pad 2 ignorato");
        }
        // 0.55.29: lo stesso per il pad 1 con l'eroe 1 che parla: A va al dialogo (Invio) invece di cliccare nel mondo
        else if (!open && !p2Dlg && (dlgPressed & PAD_A) && MemoryHeroTalking()) {
            TapKey(hwnd, kEnter);
            Log("Dialogo dell'eroe 1 non trovato in memoria: A del pad 1 = Invio");
            s.prevButtons = buttons;
            s.lastTick = f.now;
            return;
        }
        if (open) {
            // A = primo pulsante (OK, Accetta), B = ultimo (Rifiuta; con un solo pulsante
            // e' di nuovo OK). Le posizioni vengono dal gioco; se mancano, A = Invio.
            const bool useA = (dlgPressed & PAD_A) != 0;
            // A ripetuto: dopo aver accettato si riparte dal piu' vicino; dopo aver rifiutato
            // con B, il prossimo A (entro TalkCycleMs) va al successivo.
            // Un dialogo con un solo pulsante (OK) e' solo una battuta: il giro continua.
            // Dal 0.10.4, dopo la battuta (OK) di un NPC con un servizio o un dialogo di missione, il
            // prossimo A torna una volta su di lui: spesso e' solo la prima parte del discorso (Anurf,
            // 23/10 alle 09:53: dopo la sua battuta A era passato a un soldato e all'elfa).
            if (useA && d.buttons >= 2) s.talkSkipN = 0;
            else if (useA && s.talkImportant && s.talkRepeats < 1 && s.talkSkipN > 0 &&
                     s.talkSkip[s.talkSkipN - 1] == s.talkRepeatId) {
                --s.talkSkipN;                       // non piu' saltato: il prossimo A lo sceglie di nuovo
                ++s.talkRepeats;
                s.talkLast = f.now;
                Log("Dialogo: battuta di un NPC utile (id %lu), il prossimo A torna su di lui", s.talkRepeatId);
            }
            else                        s.talkLast = f.now;
            // 0.55.27: finestra mai vista nascosta (dopo il caricamento): se fra 1,5 s e' ancora li' uguale, era una copia vecchia
            if (d.unverified) MemoryDialogActed(d.addr);
            // A = Invio (verificato su OK e Accetta); B = clic sull'ultimo pulsante se ce ne
            // sono almeno due, altrimenti anche B chiude con Invio.
            if (useA || d.buttons < 2) {
                TapKey(hwnd, kEnter);
                Log("Dialogo (%d,%d %dx%d, %d pulsanti): %s = Invio", d.x, d.y, d.w, d.h, d.buttons, useA ? "A" : "B");
            }
            else if (d.buttons == 0) {
                if (useA) { TapKey(hwnd, kEnter); Log("Dialogo (%d,%d %dx%d) senza pulsanti letti: A = Invio", d.x, d.y, d.w, d.h); }
                else Log("Dialogo (%d,%d %dx%d) senza pulsanti letti: B ignorato", d.x, d.y, d.w, d.h);
            }
            else {
                const RECT& r = d.btn[useA ? 0 : d.buttons - 1];
                LONG bx = (r.left + r.right) / 2, by = (r.top + r.bottom) / 2;
                if (!g_virt.active.load()) Activate(f.real);
                SetVirtualClient(s, bx, by, f.w, f.h);
                if (s.cursorMode) { s.cx = static_cast<float>(bx); s.cy = static_cast<float>(by); }
                SendMove(hwnd);
                s.dlgClick = 1;
                s.dlgClickAt = f.now + 60;
                Log("Dialogo (%d,%d %dx%d, %d pulsanti): %s = clic sul pulsante %d in (%ld,%ld)", d.x, d.y, d.w, d.h,
                    d.buttons, useA ? "A" : "B", useA ? 1 : d.buttons, bx, by);
            }
            s.prevButtons = buttons;
            s.lastTick = f.now;
            return;
        }
    }

    // --- Mercante (0.9.9): stato letto a ogni giro, anche fuori dalla modalita' cursore. Nella
    // 0.9.8 si leggeva solo in modalita' cursore, cosi' chiudendo il mercante la mod restava
    // "mercante aperto" e alla riapertura non ripartiva dalla scheda Armi.
    {
        const bool merchant = s.winState == 1 && MemoryWindowOpen(4);
        if (merchant != s.merchantOpen) {
            s.merchantOpen = merchant;
            s.merchantDumpAt = 0;
            if (merchant) {
                RECT r{};
                MemoryWindowOpen(4, &r);
                // Il gioco riapre il mercante sull'ultima scheda usata (il tester, 23/10/2026): la scheda
                // attiva si legge dalla memoria (0.9.11).
                int a = 0, n = 0; RECT tr[8] = {};
                if (MemoryMerchantTabs(a, n, tr, 8)) {
                    s.merchantTab = a;
                    Log("Mercante aperto: finestra (%ld,%ld)-(%ld,%ld), scheda attiva %d di %d", r.left, r.top, r.right,
                        r.bottom, a + 1, n);
                    for (int i = 0; i < n && i < 8; ++i)
                        Log("  scheda %d: (%ld,%ld)-(%ld,%ld)", i + 1, tr[i].left, tr[i].top, tr[i].right, tr[i].bottom);
                }
                else {
                    Log("Mercante aperto: finestra (%ld,%ld)-(%ld,%ld), schede non leggibili, scheda %s dal conto della mod",
                        r.left, r.top, r.right, r.bottom, kMerchantTabName[s.merchantTab]);
                    MemoryWindowDump(4, "apertura");
                }
            }
            else Log("Mercante chiuso");
        }
    }

    // --- View: breve = cambio di modalita' senza tasti; tenuto = inventario + cambio ---
    if (pressed & PAD_BACK) { s.viewSince = f.now ? f.now : 1; s.viewLong = false; }
    if ((buttons & PAD_BACK) && s.viewSince && !s.viewLong &&
        f.now - s.viewSince >= static_cast<DWORD>(g_cfg.viewHoldMs)) {
        s.viewLong = true;
        if (MemoryDualPossible()) {                 // 0.55.54: con i due zaini apre e chiude solo il lato dell'eroe 1
            DualToggle(s, f, 1, "View tenuto");
            SetCursorMode(s, f, MemoryDualSide(1), "inventario");
            // 0.55.56 (controllo): con il mouse del gioco al giocatore 2 SetCursorMode prende la posizione del suo puntatore;
            // quello del pad 1 parte invece dalla sua parte dello schermo
            if (s.p2On && s.dualOwner == 2 && MemoryDualSide(1)) { s.cx = f.w * 0.25f; s.cy = f.h * 0.5f; }
        }
        else {
            TapKey(hwnd, kKeyI);
            Log("View tenuto: %s", kKeyI.name);
            SetCursorMode(s, f, !s.cursorMode, "inventario");
        }
    }
    if ((released & PAD_BACK) && s.viewSince) {
        if (!s.viewLong) SetCursorMode(s, f, !s.cursorMode, "View");
        s.viewSince = 0;
    }
    // --- Menu: Esc; dalla modalita' gioco entra anche in modalita' cursore ---
    // Dal 0.15.0 al rilascio di Start, e solo se non era tenuto (tenuto = aiuto).
    if ((released & PAD_START) && s.startSince) {
        // 0.56.9: dal mercante condiviso, con tutti e due i lati aperti, Start chiude solo il lato del giocatore 1 (Esc chiuderebbe il
        // negozio anche al giocatore 2); premuto di nuovo apre il menu
        if (!s.startLong && MemorySharedWin() >= 0 && MemoryDualSide(1) && MemoryDualSide(2)) {   // 0.56.14: tutte le condivise
            DualToggle(s, f, 1, "Start dal mercante condiviso");
            SetCursorMode(s, f, false, "Start dal mercante condiviso");
        }
        else if (!s.startLong) {
            TapKey(hwnd, kEsc);
            Log("Menu: %s", kEsc.name);
            SetCursorMode(s, f, true, "menu");
        }
        s.startSince = 0;
    }

    FocusTick(s, f);
    // Simboli dei tasti sopra gli slot delle arti sbloccati (0.14.2), solo in modalita' gioco.
    static DWORD slotsAt = 0;
    if (f.now - slotsAt >= 500) {
        slotsAt = f.now;
        POINT c[5];
        const int n = s.cursorMode ? 0 : MemoryTaskbarArts(c, 5);
        OverlaySetSlotLabels(n, n ? c : nullptr);
        POINT pc[3];                                    // 0.56.23: tasti delle pozioni
        const int pn = s.cursorMode ? 0 : MemoryTaskbarPotions(pc);
        OverlaySetPotionLabels(1, pn, pc);
    }
    // Voce "Controller" nel menu Esc (0.15.3): riga in piu' in cima al menu, con font e colori delle voci.
    {
        // 0.18.9: la voce "tremava" (il tester, 25/10): il log della 0.18.8 la mostrava spostata di (3,3) quando la
        // prima voce del gioco era evidenziata; ora la riga si calcola da una voce non evidenziata
        // (MemoryEscMenuRow). Il log resta se il rettangolo cambia ancora.
        static RECT loggedR{};
        static DWORD logAt = 0;
        EscRow row;
        s.escItemOn = MemoryEscMenuRow(row);
        if (s.escItemOn) {
            if (memcmp(&row.r, &loggedR, sizeof(RECT)) && loggedR.right && f.now - logAt > 1000) {
                logAt = f.now;
                Log("Menu Esc: voce Controller spostata da (%ld,%ld)-(%ld,%ld) a (%ld,%ld)-(%ld,%ld)", loggedR.left, loggedR.top,
                    loggedR.right, loggedR.bottom, row.r.left, row.r.top, row.r.right, row.r.bottom);
            }
            loggedR = row.r;
            s.escItem = row.r;
        }
        else loggedR = RECT{};
        const bool hov = s.escItemOn && s.cursorMode && s.cx >= s.escItem.left && s.cx < s.escItem.right &&
                         s.cy >= s.escItem.top && s.cy < s.escItem.bottom;
        OverlaySetEscItem(s.escItemOn, s.escItem, hov, row.font, row.color, row.hoverColor);
    }
    // Voce "Co-op locale" nel menu principale (0.20.1): solo nel gioco normale, e la ricerca delle voci (circa 30 ms)
    // si fa quattro volte al secondo.
    {
        static DWORD mmAt = 0;
        static EscRow mmRow;
        // 0.47.3: con Menu=true la riga e' la voce "Co-op" di Memory.cpp (disegno e clic li fa lei), ma la si cerca anche
        // qui perche' la croce del pad la trovi fra le voci del menu e A ci faccia un clic.
        if (g_cfg.coopRole == 0 && (g_cfg.coopEnabled || g_cfg.nativeMenu) && !CoopSessionActive()) {
            if (!s.cursorMode) s.mainRowOn = false;     // in partita niente ricerche
            else if (f.now - mmAt >= 250) {
                mmAt = f.now;
                s.mainRowOn = MemoryMainMenuRow(mmRow);
                if (s.mainRowOn) s.mainRow = mmRow.r;
            }
        }
        else s.mainRowOn = false;
        // 0.20.10: il mouse vero e il puntatore del pad sono due cose diverse (la 0.20.9 guardava solo il secondo e il
        // clic del mouse non scattava mai).
        POINT mp = f.real;
        ScreenToClient(hwnd, &mp);
        auto inRow = [&](LONG x, LONG y) { return s.mainRowOn && x >= s.mainRow.left && x < s.mainRow.right && y >= s.mainRow.top && y < s.mainRow.bottom; };
        const bool mouseHov = inRow(mp.x, mp.y);
        const bool padHov = g_virt.active.load() && inRow(static_cast<LONG>(s.cx), static_cast<LONG>(s.cy));
        if (!g_cfg.nativeMenu) OverlaySetMainRow(s.mainRowOn, s.mainRow, mouseHov || padHov, mmRow.font, mmRow.color, mmRow.hoverColor);
        const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (lmb && !s.lmbWas && mouseHov && !g_cfg.nativeMenu) CoopStartFromMenu(hwnd);
        s.lmbWas = lmb;
    }
    if (s.cursorMode) {
        s.zSince[0] = s.zSince[1] = 0;   // 0.55.31: croce premuta qui: niente pozione al rilascio in modalita' gioco
        // 0.55.54: il mouse del gioco e' del giocatore 2 (Pad2Pointer). Il pad 1 lo riprende con un pulsante, o con lo stick se il
        // giocatore 2 e' fermo da 300 ms, ma non mentre lui tiene un pulsante, fa un clic a tempo o ha un oggetto in mano: allora
        // il puntatore del pad 1 si muove lo stesso (lo disegna l'overlay) e i clic aspettano, con un avviso.
        if (s.p2On && s.dualOwner == 2) {
            const WORD act1 = static_cast<WORD>(pressed & (PAD_A | PAD_B | PAD_LB | PAD_RB | PAD_X | PAD_DPAD_LEFT | PAD_DPAD_RIGHT | PAD_DPAD_UP | PAD_DPAD_DOWN));
            float sx = 0, sy = 0, st = 0;
            const bool moved1 = StickDir(pad.lx, pad.ly, g_cfg.leftDeadzone, sx, sy, st) || StickDir(pad.rx, pad.ry, g_cfg.rightDeadzone, sx, sy, st);
            const bool p2Busy = s.p2Left || s.p2Right || s.p2mc || s.heldBy == 2;
            const bool p2Recent = f.now - s.p2InputAt < 300;
            if ((act1 || moved1) && !p2Busy && (act1 || !p2Recent)) {
                s.dualOwner = 1;
                SetVirtualClient(s, static_cast<LONG>(s.cx), static_cast<LONG>(s.cy), f.w, f.h);
                SendMove(hwnd);
                Log("ZAINI: mouse del gioco al giocatore 1");
            }
            else {
                if (act1 & (PAD_A | PAD_B | PAD_LB | PAD_RB)) {
                    static DWORD toastAt = 0;
                    if (f.now - toastAt > 2000) {
                        toastAt = f.now;
                        OverlayCoopToast(Tr(s.heldBy == 2 ? "Aspetta: il giocatore 2 ha un oggetto in mano" : "Aspetta: il giocatore 2 sta usando il puntatore"), 2000);
                    }
                }
                if (moved1) {
                    float dt = (f.now - s.lastTick) / 1000.0f;
                    if (dt < 0.0f || dt > 0.05f) dt = 0.008f;
                    const float speed = g_cfg.cursorSpeed * f.hf;
                    float vx1, vy1, vx2, vy2;
                    StickVelocity(pad.lx, pad.ly, g_cfg.leftDeadzone, speed, vx1, vy1);
                    StickVelocity(pad.rx, pad.ry, g_cfg.rightDeadzone, speed * g_cfg.cursorFine, vx2, vy2);
                    s.cx += (vx1 + vx2) * dt; s.cy += (vy1 + vy2) * dt;
                    const float maxX = static_cast<float>(f.w - 2), maxY = static_cast<float>(f.h - 2);
                    s.cx = s.cx < 1.0f ? 1.0f : (s.cx > maxX ? maxX : s.cx);
                    s.cy = s.cy < 1.0f ? 1.0f : (s.cy > maxY ? maxY : s.cy);
                }
                if (pressed & PAD_Y) {
                    DualToggle(s, f, 1, "Y");
                    SetCursorMode(s, f, MemoryDualSide(1), "Y, lato dell'eroe 1");
                }
                s.lastTick = f.now;
                return;
            }
        }
        MacroClickTick(s, f);
        if (pressed & PAD_RT) MemoryCompanionCycle(1);   // 0.56.31: RT gira la colonna del giocatore 1 (il suo eroe, i compagni)
        // Mercante aperto (finestra n.4): X vende l'oggetto sotto il cursore (Shift+clic), LB/RB
        // passano alla scheda precedente/successiva. Fuori dal mercante X non fa nulla: Shift+clic
        // nell'inventario getterebbe via l'oggetto. LB resta la pozione di cura.
        const bool merchant = s.merchantOpen;
        int tabActive = 0, tabCount = 0;
        RECT tabRect[8] = {};
        if (merchant && s.merchantDumpAt && !s.mcStep && static_cast<LONG>(f.now - s.merchantDumpAt) >= 0) {
            s.merchantDumpAt = 0;                         // verifica dopo il clic
            if (MemoryMerchantTabs(tabActive, tabCount, tabRect, 8)) {
                if (tabActive == s.merchantTab)
                    Log("Mercante: scheda attiva %d di %d%s", tabActive + 1, tabCount, s.tabRetry ? " (dopo un nuovo clic)" : "");
                else if (s.tabRetry < kTabRetries && s.merchantTab < tabCount) {
                    // Il gioco non ha preso il clic: lo stesso pulsante di nuovo.
                    ++s.tabRetry;
                    const RECT& r = tabRect[s.merchantTab];
                    const LONG x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
                    StartMacroClick(s, f, x, y, false, true);
                    s.merchantDumpAt = f.now + kTabPreMs + kTabDownMs + kTabAfterMs + kTabCheckMs;
                    Log("Mercante: scheda attiva %d invece di %d, nuovo clic %d in (%ld,%ld)", tabActive + 1,
                        s.merchantTab + 1, s.tabRetry, x, y);
                }
                else Log("Mercante: scheda attiva %d invece di %d, il gioco non ha preso i clic", tabActive + 1,
                         s.merchantTab + 1);
            }
        }
        // LB/RB premuti durante un clic sulla scheda finiscono in coda (0.9.14: prima andavano persi),
        // al massimo due passi per verso; ripartono appena il clic precedente e' finito, contando dalla
        // scheda appena cliccata (il gioco potrebbe non aver ancora aggiornato l'indice in memoria).
        int tabDir = (merchant && (pressed & (PAD_LB | PAD_RB))) ? ((pressed & PAD_RB) ? 1 : -1) : 0;
        if (tabDir && s.mcStep) {
            s.tabQueue += tabDir;
            if (s.tabQueue > 2) s.tabQueue = 2;
            if (s.tabQueue < -2) s.tabQueue = -2;
            tabDir = 0;
        }
        bool tabQueued = false;
        if (merchant && !tabDir && !s.mcStep && s.tabQueue) {
            tabDir = s.tabQueue > 0 ? 1 : -1;
            s.tabQueue -= tabDir;
            tabQueued = true;
        }
        if (!merchant) s.tabQueue = 0;
        if (merchant && !s.mcStep && tabDir) {
            const bool next = tabDir > 0;
            // Dal 0.9.11 la scheda attiva e i pulsanti si leggono dalla memoria; il conto della mod e
            // i punti fissi restano solo se la lettura non riesce.
            if (MemoryMerchantTabs(tabActive, tabCount, tabRect, 8)) {
                if (tabQueued && s.merchantTab < tabCount) tabActive = s.merchantTab;
                s.merchantTab = (tabActive + (next ? 1 : tabCount - 1)) % tabCount;
                const RECT& r = tabRect[s.merchantTab];
                LONG x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
                // Rettangolo non plausibile (vuoto o fuori schermo): punto fisso della stessa scheda.
                if (r.right <= r.left || r.bottom <= r.top || x < 0 || y < 0 || x >= f.w || y >= f.h) {
                    const POINT& t = kMerchantTab[s.merchantTab % 3];
                    x = t.x; y = t.y;
                }
                StartMacroClick(s, f, x, y, false, true);
                Log("%s: scheda %d -> %d di %d del mercante, clic in (%ld,%ld) sul pulsante (%ld,%ld)-(%ld,%ld)",
                    next ? "RB" : "LB", tabActive + 1, s.merchantTab + 1, tabCount, x, y, r.left, r.top, r.right, r.bottom);
            }
            else {
                s.merchantTab = (s.merchantTab + (next ? 1 : 2)) % 3;
                const POINT& t = kMerchantTab[s.merchantTab];
                StartMacroClick(s, f, t.x, t.y, false, true);
                Log("%s: schede non leggibili, scheda %s del mercante dal conto della mod, clic in (%ld,%ld)",
                    next ? "RB" : "LB", kMerchantTabName[s.merchantTab], t.x, t.y);
            }
            s.tabRetry = 0;
            s.merchantDumpAt = f.now + kTabPreMs + kTabDownMs + kTabAfterMs + kTabCheckMs;
        }
        // Inventario senza mercante (0.12.6, richiesta del tester): LB/RB passano alla scheda
        // precedente/successiva (Zaino, Arti di combattimento, Combo) con un clic sul suo pulsante.
        // 0.55.54: con i due zaini solo se il lato dell'eroe 1 e' aperto (altrimenti le sue schede sono nascoste e il clic andrebbe al mondo)
        const bool invTabs = !merchant && MemoryWindowOpen(1) && (!MemoryDualActive() || MemoryDualSide(1));
        if (invTabs && !s.mcStep && (pressed & (PAD_LB | PAD_RB))) {
            int a = 0, n = 0;
            RECT tr[8] = {};
            if (MemoryWindowTabs(1, a, n, tr, 8)) {
                const bool next = (pressed & PAD_RB) != 0;
                int to = (a + (next ? 1 : n - 1)) % n;
                // 0.56.43: la scheda Compagni e' la quarta, dopo Combo (aperta: RB va alla prima scheda, LB all'ultima del gioco)
                RECT ct{}; bool copen = false;
                if (MemoryCompTab(1, ct, copen)) {
                    if (copen) to = next ? 0 : n - 1;
                    else if ((next && a == n - 1) || (!next && a == 0)) to = -1;
                }
                const RECT& q = to < 0 ? ct : tr[to];
                const LONG x = (q.left + q.right) / 2, y = (q.top + q.bottom) / 2;
                StartMacroClick(s, f, x, y, false, true);
                if (to < 0) Log("%s: scheda %d -> scheda Compagni dell'inventario, clic in (%ld,%ld)", next ? "RB" : "LB", a + 1, x, y);
                else Log("%s: scheda %d -> %d di %d dell'inventario, clic in (%ld,%ld)", next ? "RB" : "LB", a + 1, to + 1, n, x, y);
            }
            else Log("%s: schede dell'inventario non leggibili", (pressed & PAD_RB) ? "RB" : "LB");
        }
        // Dal mercante LB non beve mai (0.9.13: nella 0.9.12, con un clic sulla scheda in corso, LB
        // finiva qui e il personaggio beveva la pozione); nemmeno nell'inventario (0.12.6).
        if ((pressed & PAD_LB) && !merchant && !invTabs) { TapKey(hwnd, kSpace); Log("LB: %s", kSpace.name); }
        if (merchant && !s.mcStep && (pressed & PAD_X)) {
            if (!g_virt.active.load()) { Activate(f.real); SyncCursorFromVirtual(s); }
            const LONG x = static_cast<LONG>(s.cx), y = static_cast<LONG>(s.cy);
            StartMacroClick(s, f, x, y, true, false);
            Log("X: vendita al mercante, Shift+clic in (%ld,%ld)", x, y);
        }
        // Y con la finestra dei portali: clic su "Annulla" (0.10.5), col mouse vero sul pulsante.
        RECT cancel{};
        if ((pressed & PAD_Y) && s.busyOpen && !s.mcStep && MemoryBusyDlg(&cancel) && cancel.right > cancel.left) {
            const LONG x = (cancel.left + cancel.right) / 2, y = (cancel.top + cancel.bottom) / 2;
            StartMacroClick(s, f, x, y, false, false);
            Log("Y: finestra dei portali, clic su Annulla in (%ld,%ld)", x, y);
        }
        // 0.55.54: con i due zaini Y chiude solo il lato dell'eroe 1 (l'altro resta al giocatore 2)
        else if ((pressed & PAD_Y) && s.winState == 1 && MemoryDualActive() && MemoryDualPossible()) {
            DualToggle(s, f, 1, "Y");
            SetCursorMode(s, f, MemoryDualSide(1), "Y, lato dell'eroe 1");
        }
        // Y: chiude le finestre aperte (mercante, inventario...) senza andare sulla X.
        else if ((pressed & PAD_Y) && s.winState == 1) {
            LONG bx = 0, by = 0;
            if (MemoryHorseButton(bx, by)) {
                // Lo stalliere non si chiude con Esc: clic su "Continua", in tre tempi come nei dialoghi.
                if (!g_virt.active.load()) Activate(f.real);
                SetVirtualClient(s, bx, by, f.w, f.h);
                s.cx = static_cast<float>(bx); s.cy = static_cast<float>(by);
                SendMove(hwnd);
                s.dlgClick = 1;
                s.dlgClickAt = f.now + 60;
                Log("Y: stalliere, clic sul pulsante in (%ld,%ld)", bx, by);
            }
            else { TapKey(hwnd, kEsc); Log("Y: chiusura delle finestre (%s)", kEsc.name); }
        }
        if (!s.mcStep) CursorTick(s, f, pad, pressed, released);   // fermo durante un clic programmato
        s.lastTick = f.now;
        return;
    }
    s.lastTick = f.now;

    // 0.55.54: lato dell'eroe 2 aperto e pad 1 in modalita' gioco: il mouse del gioco e' del giocatore 2, quindi il pad 1 muove
    // l'eroe 1 con ordini diretti (come durante lo scambio della 0.51) e LB beve la pozione (tasto). View tenuto apre il lato
    // dell'eroe 1, Start il menu (piu' sopra). 0.55.55 (il tester: "prima se il giocatore 1 apriva lo zaino, il giocatore 2 poteva
    // attaccare i nemici"): anche A attacca (il nemico nella direzione dello stick, o il piu' vicino, ogni 250 ms mentre e'
    // tenuto) e X, Y, RB, RT, LT lanciano le arti 1-5 (sul nemico, o sull'eroe stesso), sempre con ordini diretti.
    // 0.56.0: con il secondo mouse il mouse del gioco resta del giocatore 1: il pad 1 gioca come sempre, con i clic del gioco
    if (s.p2On && !MemoryMouse2Ready()) {
        if (pressed & PAD_LB) { TapKey(hwnd, kSpace); Log("LB: %s", kSpace.name); }
        if (pressed & PAD_B) { TapKey(hwnd, kKeyA); Log("ZAINI: B del pad 1 senza mouse: %s", kKeyA.name); }   // 0.55.63
        if ((buttons & PAD_A) && static_cast<LONG>(f.now - s.p1AtkAt) >= 250) {
            s.p1AtkAt = f.now;
            Target t;
            if (FindLock(s, f, false, t) && t.cls >= 0) {
                MemoryHero1Attack(t.id);
                if (t.id != s.p1AtkId) { s.p1AtkId = t.id; s.lastCombat = f.now; Log("ZAINI: A del pad 1 senza mouse: attacco diretto su id %lu tipo %d", t.id, t.cls); }
            }
            else if (pressed & PAD_A) {
                // 0.55.63 (prova del 02/11: "non potevo aprire il forziere"): senza nemici A usa il bersaglio a fuoco o quello vicino
                // (forziere, PNG, porta, oggetto a terra) come A in modalita' gioco, con l'ordine di tipo 7 del clic del gioco
                // ("vai e usa"), lo stesso dell'attacco diretto
                TalkChoice c;
                if (ChooseTalk(s, f, c) && c.t.id) {
                    MemoryHero1Attack(c.t.id);
                    Log("ZAINI: A del pad 1 senza mouse: %s%s%s id %lu (ordine diretto)", c.t.kind, c.t.name[0] ? " " : "", c.t.name, c.t.id);
                }
                else Log("ZAINI: A del pad 1 senza mouse: nessun nemico e nessun oggetto vicino");
            }
        }
        if (released & PAD_A) s.p1AtkId = 0;
        for (const ArtButton& ab : kArts) {
            if (!(pressed & ab.mask)) continue;
            Target t;
            const DWORD tid = (FindLock(s, f, false, t) && t.cls >= 0) ? t.id : 0;
            MemoryHero1Art(ab.slot - 1, tid);
            s.lastCombat = f.now;
            Log("ZAINI: %s del pad 1 senza mouse: arte %d %s", ab.name, ab.slot, tid ? "sul nemico" : "sull'eroe");
        }
        static DWORD moveAt = 0;
        static bool moving = false;
        float mdx = 0, mdy = 0, mt = 0;
        const bool stick = !(buttons & PAD_A) && StickDir(pad.lx, pad.ly, g_cfg.leftDeadzone, mdx, mdy, mt);   // con A tenuto lo stick mira
        if (stick && static_cast<LONG>(f.now - moveAt) >= 150) {
            moveAt = f.now;
            const bool sent = MemoryMoveHero1(mdx, mdy);
            if (sent && !moving) Log("ZAINI: il pad 1 muove l'eroe 1 mentre il giocatore 2 usa il suo zaino");
            moving = sent;
        }
        else if (!stick) moving = false;
        return;
    }

    // Clic sull'icona del cavallo: giu', poi su (StartHorseIcon).
    if (s.dismountStep && static_cast<LONG>(f.now - s.dismountAt) >= 0) {
        if (s.dismountStep == 1) { SendLeftDown(hwnd); s.dismountStep = 2; s.dismountAt = f.now + 60; }
        else                     { SendLeftUp(hwnd);   s.dismountStep = 0; }
    }

    // --- Tasti semplici ---
    if (pressed & PAD_LB)    { TapKey(hwnd, kSpace); Log("LB: %s", kSpace.name); }
    // B: breve = raccogli tutto, inviato al rilascio; tenuto = smonta da cavallo (0.9.3).
    if (pressed & PAD_B) { s.bSince = f.now ? f.now : 1; s.bLong = false; }
    if ((buttons & PAD_B) && s.bSince && !s.bLong && f.now - s.bSince >= static_cast<DWORD>(g_cfg.viewHoldMs) &&
        s.held == Click::None && !s.pendingArt && !s.dismountStep) {
        s.bLong = true;
        StartHorseIcon(s, f);
    }
    if ((released & PAD_B) && s.bSince) {
        if (!s.bLong) { TapKey(hwnd, kKeyA); Log("B: %s", kKeyA.name); }
        s.bSince = 0;
    }
    // Croce su/giu': pozioni di g_cfg.dpadPotion (0.56.25: concentrazione R / Mentore W; prima concentrazione / antidoto E).
    // 0.55.31: tenute oltre ZoomHoldMs = zoom (rotellina avanti /
    // indietro, ripetuto ogni 500 ms; richiesta del tester del 31/10: col pad non c'era modo di cambiare la visuale); la pozione
    // allora si beve al rilascio, solo se non c'e' stato zoom. ZoomHoldMs=0: come prima, pozione alla pressione.
    {
        static const WORD kZBtn[2] = { PAD_DPAD_UP, PAD_DPAD_DOWN };
        for (int k = 0; k < 2; ++k) {
            const Key& potion = PotionKey(g_cfg.dpadPotion[k]);
            const char* name = k == 0 ? "Croce su" : "Croce giu'";
            if (g_cfg.zoomHoldMs <= 0) { if (pressed & kZBtn[k]) { TapKey(hwnd, potion); Log("%s: %s", name, potion.name); } continue; }
            if (pressed & kZBtn[k]) { s.zSince[k] = f.now ? f.now : 1; s.zNext[k] = 0; s.zDone[k] = false; }
            if ((buttons & kZBtn[k]) && s.zSince[k] && f.now - s.zSince[k] >= static_cast<DWORD>(g_cfg.zoomHoldMs) &&
                (!s.zNext[k] || static_cast<LONG>(f.now - s.zNext[k]) >= 0)) {
                GameZoomStep(k == 0 ? 1 : -1, k == 0 ? "Croce su tenuta" : "Croce giu' tenuta");
                s.zDone[k] = true;
                s.zNext[k] = f.now + 500;
            }
            if ((released & kZBtn[k]) && s.zSince[k]) {
                if (!s.zDone[k]) { TapKey(hwnd, potion); Log("%s: %s", name, potion.name); }
                s.zSince[k] = 0;
            }
        }
    }
    if (pressed & PAD_LS) {
        // 0.56.16: doppio L3 anche dal pad 1: richiama l'eroe 2 accanto all'eroe 1
        if (s.lsTapAt[1] && f.now - s.lsTapAt[1] < 400) { s.lsTapAt[1] = 0; MemoryJoinCompanion(1); }
        else s.lsTapAt[1] = f.now ? f.now : 1;
        if (!s.minimapDown) {
            MinimapTakeFocus(hwnd);
            SendSystem(kTab, g_cfg.minimapMethod == 1, true);
            s.minimapDown = true;
            s.minimapRelease = false;
            s.minimapSince = f.now;
            s.minimapOwner = 1;
            Log("L3: minimappa aperta (%s fisico, modalita' %s)",
                g_cfg.minimapMethod == 1 ? "clic centrale" : "Tab", g_cfg.minimapToggle ? "Toggle" : "Hold");
        }
        else if (g_cfg.minimapToggle) {
            s.minimapRelease = true;                    // seconda pressione: chiude
        }
    }
    if ((released & PAD_LS) && s.minimapDown && !g_cfg.minimapToggle && s.minimapOwner == 1)
        s.minimapRelease = true;                        // Hold: chiude al rilascio (0.55.16: se l'ha aperta il pad 1)

    if (pressed & (PAD_DPAD_LEFT | PAD_DPAD_RIGHT)) {
        int n = UnlockedSlots(f.now);
        if (s.weapon > n) s.weapon = n;
        s.weapon = (pressed & PAD_DPAD_RIGHT) ? (s.weapon % n) + 1 : ((s.weapon + n - 2) % n) + 1;
        TapKey(hwnd, kDigit[s.weapon]);
        Log("Croce %s: arma slot %d", (pressed & PAD_DPAD_RIGHT) ? "destra" : "sinistra", s.weapon);
    }
    if (pressed & PAD_RS)  { s.altP1 = true; HoldKey(hwnd, kAlt, true); Log("R3: Alt premuto"); }
    if (released & PAD_RS) { s.altP1 = false; if (!s.altP2) HoldKey(hwnd, kAlt, false); Log("R3: Alt rilasciato"); }   // 0.55.16

    // Clic di A posato: il puntatore e' sul bersaglio da kSettleMs, ora si preme.
    if (s.pendingA && static_cast<LONG>(f.now - s.pendingAAt) >= 0) {
        s.pendingA = false;
        ClickDown(s, Click::Left, f);
        s.heldMask = PAD_A;
        s.settledA = true;
        if (!(buttons & PAD_A)) s.releaseWanted = true;  // A gia' rilasciato: clic breve
    }

    const bool idle = (s.held == Click::None) && !s.pendingArt && !s.dismountStep && !s.pendingA;

    // 0.56.9: negozio aperto dal giocatore 2 e lato del giocatore 1 chiuso: A sul mercante apre il suo lato invece di parlargli
    // (gli toglierebbe la sessione: il PNG ha un interlocutore solo)
    bool joinShop = false;
    // 0.56.10: prova 13 del 03/11 ("il giocatore 1 si e' unito solo con Ins tenuto, 0 non funzionava"): il mercante ha l'icona del
    // servizio sopra la testa e per la scelta di A e' un "NPC (icona)", non "(servizio)"; e MemoryMerchantShared dipendeva dal
    // disegno del mercante, che non passava dal gancio (Memory.cpp, kDualWin). Ora: mercante aperto con il solo lato dell'eroe 2 e
    // A che sceglie un mercante (ChooseTalk, senza toccare il giro dei bersagli di FindTalk).
    // 0.56.14: anche fabbro e Maestro (la finestra che apre il PNG scelto, se e' aperta)
    if ((pressed & PAD_A) && idle && MemoryMouse2Ready() && MemoryDualSide(2) && !MemoryDualSide(1) &&
        (MemoryWindowOpen(4) || MemoryWindowOpen(3) || MemoryWindowOpen(14))) {
        TalkChoice tc;
        int jw = -1;
        if (ChooseTalk(s, f, tc) && (jw = MemorySharedServiceWin(tc.t.id)) >= 0 && MemoryWindowOpen(jw)) {
            joinShop = true;
            DualToggle(s, f, 1, "A sul mercante condiviso");
            SetCursorMode(s, f, true, "mercante condiviso");
        }
    }
    // --- A: attacco / interazione ---
    if ((pressed & PAD_A) && idle && !joinShop) {
        const char* how;
        bool pointed = false;
        POINT t = PrepareTarget(s, f, how, &pointed, true);
        s.plainA = pointed;
        if (s.lockSearch) s.lastCombat = f.now;          // attacco (non NPC, oggetto o punto del mouse)
        if (s.plainA) {                                  // bersaglio preciso: clic posato
            SendMove(hwnd);
            s.pendingA = true;
            s.pendingAAt = f.now + kSettleMs;
            s.releaseWanted = false;
        }
        else {
            ClickDown(s, Click::Left, f);
            s.heldMask = PAD_A;
            s.settledA = false;
        }
        Log("A premuto: %s in (%ld,%ld), %s%s", (f.ranged && !s.plainA) ? "Ctrl+clic" : "clic", t.x, t.y, how,
            s.plainA ? " (clic tra 100 ms)" : "");
        MemoryDiagClick(t.x, t.y);
        if (strstr(how, "NPC")) s.dlgWatchUntil = f.now + 8000;         // nei prossimi secondi: si apre un dialogo?
    }

    // --- Arti di combattimento: tasto dello slot, poi clic destro ---
    // Anche con A tenuto: l'attacco si interrompe e riprende dopo l'arte.
    else if (idle || (s.held == Click::Left && s.heldMask == PAD_A && !s.pendingArt)) {
        for (const ArtButton& a : kArts) {
            if (!(pressed & a.mask)) continue;
            const int unlocked = UnlockedSlots(f.now);
            if (a.slot > unlocked) {
                // Su uno slot vuoto Sacred lascerebbe selezionata l'arte precedente
                // e il clic destro lancerebbe quella: non si invia nulla.
                Log("%s premuto: arte slot %d non sbloccato (%d sbloccati), ignorato",
                    a.name, a.slot, unlocked);
                break;
            }
            const char* how;
            POINT t;
            if (s.held == Click::Left) {                // A tenuto: stesso punto dell'attacco
                ClickUp(s);
                s.resumeA = true;
                t = VirtualClient(hwnd);
                how = s.locked ? "bersaglio agganciato dall'attacco in corso (A interrotto)"
                               : "punto dell'attacco in corso (A interrotto)";
            }
            else t = PrepareTarget(s, f, how);
            s.plainArt = s.locked;
            // 0.55.42: arte mirata sull'eroe 2 (prova del 31/10 alle 23:59: "l'elfo silvano voleva curarmi ma non poteva", arte
            // dello slot 2 con la mira dello stick destro e Ctrl+clic destro sul terreno). Studio: la cura e' un effetto (ObjectFX,
            // "do healing to %d", 0x5b582f) che cura la creatura sua bersaglio (+0x60) con un ordine al suo metodo 0x18; con Ctrl
            // l'arte va su un punto, senza creatura. Se il punto dell'arte cade sull'eroe 2 (suo rettangolo, piu' un margine) e non
            // c'e' un nemico agganciato, clic destro semplice nel punto in cui il gioco sceglie l'eroe 2, come col mouse.
            s.artOnHero2 = false;
            // 0.55.46 (controllo globale del 01/11): solo con la mira dello stick destro. Prima bastava che il punto dell'arte
            // cadesse sull'eroe 2, anche nella direzione dell'ultimo movimento: con gli eroi vicini un'arte d'attacco poteva
            // finire su di lui.
            if (!s.locked && f.aiming && g_cfg.nativeArtOnHero2 && MemoryNativeCameraOn()) {
                RECT r2{};
                LONG px = 0, py = 0;
                const LONG m = static_cast<LONG>(0.03f * f.hf);
                if (MemoryTargetRect(2, r2) && t.x >= r2.left - m && t.x <= r2.right + m && t.y >= r2.top - m && t.y <= r2.bottom + m &&
                    MemoryPickPoint(2, px, py)) {
                    t = POINT{ px, py };
                    SetVirtualClient(s, px, py, f.w, f.h);
                    s.plainArt = true;
                    s.artOnHero2 = true;
                    how = "eroe 2 (clic destro semplice su di lui)";
                }
            }
            s.lastCombat = f.now;
            SendMove(hwnd);
            TapKey(hwnd, ArtKey(a.slot));
            s.pendingArt = true;
            s.pendingDue = f.now + static_cast<DWORD>(g_cfg.artClickDelayMs);
            s.pendingRelease = false;
            s.heldMask = a.mask;
            Log("%s premuto: arte slot %d (tasto %s), bersaglio (%ld,%ld), %s",
                a.name, a.slot, ArtKey(a.slot).name, t.x, t.y, how);
            break;
        }
    }

    // Clic destro dell'arte dopo l'attesa.
    if (s.pendingArt) {
        if (released & s.heldMask) s.pendingRelease = true;
        if (static_cast<LONG>(f.now - s.pendingDue) >= 0) {
            LONG h2x = 0, h2y = 0;
            if (s.artOnHero2 && MemoryPickPoint(2, h2x, h2y)) SetVirtualClient(s, h2x, h2y, f.w, f.h);   // 0.55.42
            else if (s.locked) {
                if (MemoryTrackTarget(s.lock)) { POINT lp = LockPoint(s.lock, f); SetVirtualClient(s, lp.x, lp.y, f.w, f.h); }
            }
            else if (f.aiming) SetVirtualClient(s, f.aimX, f.aimY, f.w, f.h);
            WORD mask = s.heldMask;
            ClickDown(s, Click::Right, f);
            s.heldMask = mask;
            s.pendingArt = false;
            POINT p = VirtualClient(hwnd);
            Log("  %s in (%ld,%ld)", (f.ranged && !s.plainArt) ? "Ctrl+clic destro" : "clic destro", p.x, p.y);
            s.releaseWanted = s.pendingRelease;
        }
    }
    // Clic tenuto da A o da un'arte.
    else if (s.held != Click::None) {
        if (s.held == Click::Right || s.lockSearch) s.lastCombat = f.now;   // attacco in corso
        UpdateHeldClick(s, f);
        if (released & s.heldMask) s.releaseWanted = true;
        // Il clic destro di un'arte resta giu' almeno ArtHoldMs, e dopo un'arte il
        // movimento attende ArtMoveLockMs: altrimenti il clic sinistro del
        // movimento, ripreso subito, annulla il lancio (arte con lo stick inclinato).
        const bool art = (s.held == Click::Right);
        const DWORD minHold = art ? static_cast<DWORD>(g_cfg.artHoldMs) : s.settledA ? kSettleMs : 0;
        if (s.releaseWanted && f.now - s.clickTime >= minHold) {
            if (s.settledA) { s.settledA = false; s.stillUntil = f.now + kAfterMs; }
            ClickUp(s);
            s.releaseWanted = false;
            if (art) { s.moveLocked = true; s.moveLockUntil = f.now + static_cast<DWORD>(g_cfg.artMoveLockMs); }
            Log("Rilasciato");
            if (!(art && s.resumeA && (buttons & PAD_A))) s.locked = false;
            if (art && s.resumeA) {
                s.resumeA = false;
                if (buttons & PAD_A) {                  // A ancora premuto: l'attacco riprende
                    if (s.locked) {
                        if (MemoryTrackTarget(s.lock)) { POINT lp = LockPoint(s.lock, f); SetVirtualClient(s, lp.x, lp.y, f.w, f.h); }
                        else s.locked = false;
                    }
                    ClickDown(s, Click::Left, f);
                    s.heldMask = PAD_A;
                    POINT p = VirtualClient(hwnd);
                    Log("A ancora premuto: attacco ripreso in (%ld,%ld)", p.x, p.y);
                }
            }
        }
    }

    // --- Movimento (solo senza clic in corso e non subito dopo un'arte) ---
    // Porte (0.16.0): lo stick muove l'eroe premendo il sinistro in un punto davanti a lui; se il punto
    // cade su una porta la pressione la apre o la chiude, e contro lo stipite i nuovi tentativi
    // ("Movimento bloccato") la aprivano e chiudevano di continuo. Regola chiesta dal tester: la porta
    // appena toccata dallo stick (o da A) per 2 s non viene piu' toccata; poi la si puo' richiudere.
    // 0.16.2: nella prova delle 17:37 (0.16.1) nessuna pressione cadeva sulla porta 1485, eppure si apriva
    // e chiudeva: col sinistro tenuto il gioco continua a seguire l'oggetto sotto il puntatore, e lo stick
    // ce lo fa passare sopra camminando. Si guarda allora lo stato della porta indicata dal gioco: appena
    // cambia (aperta o chiusa), per kStickDoorMs il punto di movimento resta fuori da lei, anche col
    // tasto gia' tenuto.
    {
        DWORD hid = 0, st = 0;
        if (MemoryHoverObject(hid) && hid != s.watchDoorId && MemoryDoorState(hid, st)) {
            s.watchDoorId = hid;
            s.watchDoorState = st;
        }
        if (s.watchDoorId != 0xffffffffu && MemoryDoorState(s.watchDoorId, st) && st != s.watchDoorState) {
            s.watchDoorState = st;
            s.stickDoorId = s.watchDoorId;
            s.stickDoorUntil = f.now + kStickDoorMs;
            Log("Porta id %lu %s: per %lu ms il movimento con lo stick non la tocca", s.watchDoorId,
                st == 0xAA ? "aperta" : "chiusa", kStickDoorMs);
        }
    }
    // 0.17.1 (richiesta del tester): lo stick non tocca nessuna porta; le porte chiuse si aprono quando
    // l'eroe ci sbatte contro (vedi sotto). Si provano prima punti piu' vicini nella stessa direzione, cosi'
    // l'eroe arriva fino alla porta invece di girarle intorno, poi piu' lontani, poi ruotati.
    // 0.17.5: non solo le porte, qualunque oggetto. Col tasto tenuto il gioco segue l'oggetto sotto il
    // puntatore: passando sopra oggetti a terra o nemici l'eroe si fermava e ripartiva (prova delle 18:17 e
    // 18:18, dopo aver ucciso tre nemici, nessun evento della mod nel log).
    auto avoidStickDoor = [&](float mx, float my, float r, LONG& cx, LONG& cy) {
        DWORD id = 0;
        if (!MemoryPickObject(cx, cy, id)) return;
        static const float ks[] = { 0.6f, 0.35f, 0.2f, 1.6f, 2.2f };
        static const float as[] = { 0.0f, 0.35f, -0.35f, 0.7f, -0.7f };   // 0, +-20, +-40 gradi
        for (float a : as) for (float k : ks) {
            const float c = std::cos(a), sn = std::sin(a);
            const float rx = mx * c - my * sn, ry = mx * sn + my * c;
            const LONG px = static_cast<LONG>(f.centerX + rx * r * k), py = static_cast<LONG>(f.centerY + ry * r * k);
            if (px < 1 || py < 1 || px >= f.w - 1 || py >= f.h - 1) continue;
            DWORD other = 0;
            if (MemoryPickObject(px, py, other)) continue;
            static DWORD movedLog = 0;
            if (f.now - movedLog >= 500) {
                movedLog = f.now;
                Log("Movimento: punto (%ld,%ld) sull'oggetto id %lu, spostato in (%ld,%ld)", cx, cy, id, px, py);
            }
            cx = px; cy = py;
            return;
        }
        static DWORD heroLog = 0;
        if (f.now - heroLog >= 500) { heroLog = f.now; Log("Movimento: nessun punto libero dall'oggetto id %lu, punto sull'eroe", id); }
        cx = static_cast<LONG>(f.centerX); cy = static_cast<LONG>(f.centerY);
    };
    if (s.moveLocked && static_cast<LONG>(f.now - s.moveLockUntil) >= 0) s.moveLocked = false;
    // Porta a spinta (0.17.1): l'eroe e' bloccato e davanti a lui, nella direzione dello stick, c'e' una
    // porta chiusa. Si lascia il tasto, si porta il puntatore sulla porta (punto scelto dal gioco per lei),
    // si aspetta che il gioco la indichi (al massimo 300 ms), poi clic; la porta resta evitata per 2 s.
    auto findDoorAhead = [&](DWORD& id, LONG& px, LONG& py, bool nearOnly) {
        const float r = g_cfg.moveRadius * f.hf;
        static const float ks[] = { 0.2f, 0.35f, 0.5f, 0.7f, 1.0f };
        static const float as[] = { 0.0f, 0.26f, -0.26f };             // 0, +-15 gradi
        for (float a : as) for (float k : ks) {
            if (nearOnly && (k > 0.36f || a != 0.0f)) continue;
            const float c = std::cos(a), sn = std::sin(a);
            const float rx = f.mdx * c - f.mdy * sn, ry = f.mdx * sn + f.mdy * c;
            const LONG x = static_cast<LONG>(f.centerX + rx * r * k), y = static_cast<LONG>(f.centerY + ry * r * k);
            DWORD d = 0, st = 0;
            if (!MemoryDoorAt(x, y, d) || !MemoryDoorState(d, st) || st == 0xAA) continue;   // solo chiuse
            if (d == s.bumpLastId && f.now - s.bumpLastAt < 1500) continue;                // appena provata
            id = d; px = x; py = y;
            return true;
        }
        return false;
    };
    auto startBump = [&](DWORD did, LONG dx, LONG dy, const char* why) {
        LONG kx = dx, ky = dy;
        MemoryPickPoint(did, kx, ky);                  // punto dove il gioco sceglie proprio la porta
        if (s.moveHeld) { SendLeftUp(hwnd); s.moveHeld = false; }
        s.moveSettling = false;
        s.bumpStep = 1; s.bumpId = did; s.bumpX = kx; s.bumpY = ky; s.bumpAt = f.now + 300;
        Log("Porta id %lu: %s, la si apre", did, why);
    };
    // 0.53.10: arresto rimandato del movimento diretto (vedi sotto): parte se nel frattempo lo stick non ha ripreso
    if (s.directStopAt && static_cast<LONG>(f.now - s.directStopAt) >= 0) {
        s.directStopAt = 0;
        if (!s.directMoving) MemoryStopHero1();
    }
    if (s.bumpStep) {
        if (s.bumpStep == 1) {
            SetVirtualClient(s, s.bumpX, s.bumpY, f.w, f.h);
            MoveIfChanged(s);
            DWORD h = 0;
            const bool onDoor = MemoryHoverObject(h) && h == s.bumpId;
            if (onDoor || static_cast<LONG>(f.now - s.bumpAt) >= 0) {
                SendLeftDown(hwnd);
                s.bumpStep = 2;
                s.bumpAt = f.now + 60;
                Log("Porta id %lu: clic per aprirla%s", s.bumpId, onDoor ? "" : " (il gioco non la indicava ancora)");
            }
        }
        else if (s.bumpStep == 2 && static_cast<LONG>(f.now - s.bumpAt) >= 0) {
            SendLeftUp(hwnd);
            s.bumpStep = 3;
            s.bumpAt = f.now + 1500;
            s.bumpClickAt = f.now;
            if (!MemoryHeroPos(s.bumpHx, s.bumpHy)) { s.bumpHx = 0; s.bumpHy = 0; }
            s.bumpLastId = s.bumpId; s.bumpLastAt = f.now;
            s.stickDoorId = s.bumpId; s.stickDoorUntil = f.now + kStickDoorMs;
            s.stuckValid = false; s.stuckLevel = 0;
        }
        else if (s.bumpStep == 3) {
            // 0.17.2: fermi finche' la porta non e' aperta (al massimo 1,5 s). Nella 0.17.1 il movimento
            // ripartiva 100 ms dopo il clic e annullava l'azione dell'eroe (prova delle 18:01: porte 1485 e
            // 1653 cliccate ma non aperte).
            DWORD st = 0;
            const bool open = MemoryDoorState(s.bumpId, st) && st == 0xAA;
            // 0.56.42 (prova sulla Steam Machine del 04/11 alle 23:31:31: porta 1539 "non aperta entro 1,5 s", aperta poi con A in 0,7 s
            // da vicino): con l'eroe ancora lontano il gioco lo fa camminare fino alla porta e dopo 1,5 s il movimento ripartiva e
            // annullava l'ordine. Se l'eroe si sta ancora muovendo si aspetta ancora (400 ms alla volta, al massimo 3,5 s dal clic).
            if (!open && static_cast<LONG>(f.now - s.bumpAt) >= 0 && f.now - s.bumpClickAt < 3500) {
                int hx = 0, hy = 0;
                if (MemoryHeroPos(hx, hy) && std::abs(hx - s.bumpHx) + std::abs(hy - s.bumpHy) >= 12) {
                    s.bumpAt = f.now + 400;
                    Log("Porta id %lu: l'eroe sta ancora andando verso la porta (%d,%d), si aspetta", s.bumpId, hx, hy);
                }
                s.bumpHx = hx; s.bumpHy = hy;
            }
            if (open || static_cast<LONG>(f.now - s.bumpAt) >= 0) {
                Log("Porta id %lu %s", s.bumpId, open ? "aperta, si riparte" : "non aperta entro 1,5 s, si riparte");
                s.bumpStep = 0;
                s.stickDoorUntil = f.now + kStickDoorMs;   // 2 s contati dall'apertura
            }
        }
    }
    else if (s.held == Click::None && !s.pendingArt && !s.moveLocked && !s.dismountStep && !s.pendingA) {
        if (f.moving) {
            // 0.56.38: dentro un portale l'ordine "vai e usa" (MemoryAutoPortal), poi 1,2 s senza movimento. 0.56.39 (prova delle
            // 17:30-17:33, da soli, nessuna riga PORTALE): vale anche per il movimento con i clic sul terreno, non solo per gli
            // ordini diretti della co-op; il tasto del movimento tenuto si lascia, altrimenti il gioco seguirebbe il clic
            if (static_cast<LONG>(f.now - s.portalCheckAt) >= 0) {
                s.portalCheckAt = f.now + 150;
                if (MemoryAutoPortal(f.mdx, f.mdy)) {
                    s.portalUntil = f.now + 1200;
                    if (s.moveHeld) { SendLeftUp(hwnd); s.moveHeld = false; }
                }
            }
            if (s.portalUntil && static_cast<LONG>(f.now - s.portalUntil) < 0) return;
            s.portalUntil = 0;
            // 0.17.3: porta chiusa appena davanti all'eroe nella direzione dello stick (entro 0.35 del raggio
            // di movimento): la si apre subito, senza aspettare il blocco. Nella 0.17.2 il blocco serviva due
            // controlli a 400 ms (prima porta aperta dopo 1,6 s) e con spostamenti brevi non arrivava mai
            // (seconda porta mai aperta, prova delle 18:03).
            {
                DWORD did = 0; LONG dx = 0, dy = 0;
                if (findDoorAhead(did, dx, dy, true)) { startBump(did, dx, dy, "porta chiusa davanti all'eroe"); return; }
            }
            // 0.53.9: con la camera sul punto medio dei due eroi niente clic sul terreno: ordini di movimento diretti all'eroe
            // 1. Prova sul PC del 30/10 (11:13:11, diagnostica della 0.53.8): clic a sinistra dell'eroe, destinazione data dal
            // gioco circa 300 pixel a destra e sempre quella (177767,135156) a ogni nuovo clic: il gioco trasforma il clic sul
            // terreno con una camera spostata solo in parte rispetto a quella con cui disegna, e lo scarto cresce con la
            // distanza fra i due eroi (era il difetto della Steam Machine). Gli ordini partono dalla posizione dell'eroe e
            // dalla direzione dello stick nel mondo, senza passare dal centro della vista. Il puntatore resta davanti
            // all'eroe solo per vedere la direzione; attacchi, dialoghi e porte restano clic sugli oggetti (rettangoli del
            // gioco, che vanno a segno). DirectMove=false nell'ini torna ai clic.
            // 0.55.36: se il gioco ha appena rifiutato il punto davanti sul piano dell'eroe (piano di sopra della Vampira, prove del
            // 31/10 alle 19:36 e 19:44) si usano per 2,5 s i clic sul terreno, che passano dalla conversione schermo-mondo del gioco
            // (il terreno sotto il puntatore, anche di un altro piano) e lasciano al gioco la strada, scale comprese
            const bool dmBlocked = MemoryDirectMoveBlocked();
            {
                static bool wasBlocked = false;
                if (dmBlocked != wasBlocked) {
                    wasBlocked = dmBlocked;
                    if (g_cfg.nativeDirectMove && MemoryNativeCameraOn())
                        Log("Movimento: %s", dmBlocked ? "clic sul terreno (il gioco rifiuta i punti davanti sul piano dell'eroe)" : "di nuovo ordini diretti");
                }
            }
            if (g_cfg.nativeDirectMove && MemoryNativeCameraOn() && !dmBlocked) {
                s.directStopAt = 0;                        // 0.53.10: ripartito prima dell'arresto rimandato
                if (s.moveHeld) { SendLeftUp(hwnd); s.moveHeld = false; }
                s.moveSettling = false; s.hoverWait = false; s.stuckLevel = 0; s.stuckValid = false;
                const float rr = g_cfg.moveRadius * f.hf;
                const LONG px = static_cast<LONG>(f.centerX + f.mdx * rr), py = static_cast<LONG>(f.centerY + f.mdy * rr);
                if (!g_virt.active.load()) { POINT p{ px, py }; ClientToScreen(hwnd, &p); Activate(p); }
                SetVirtualClient(s, px, py, f.w, f.h);
                MoveIfChanged(s);
                const float turn = f.mdx * s.directDx + f.mdy * s.directDy;   // coseno con l'ultima direzione mandata
                if (!s.directMoving || f.now - s.directAt >= 150 || turn < 0.95f) {
                    if (MemoryMoveHero1(f.mdx, f.mdy)) {
                        if (!s.directMoving) Log("Movimento diretto: inizio verso (%.2f,%.2f), camera sul punto medio", f.mdx, f.mdy);
                        s.directMoving = true; s.directAt = f.now; s.directDx = f.mdx; s.directDy = f.mdy;
                    }
                }
                if (g_virt.active.load() && f.now - s.lastStatus > 5000) {
                    s.lastStatus = f.now;
                    Log("Stato: sorgente %s, sx (%.2f, %.2f), dx (%.2f, %.2f), pulsanti 0x%04X, mira %s",
                        PadSourceName(), pad.lx, pad.ly, pad.rx, pad.ry, buttons, f.ranged ? "Ranged" : "Melee");
                }
                return;
            }
            // Controllo del blocco: ogni 400 ms la posizione dell'eroe deve cambiare.
            bool retry = false;
            if (s.moveHeld && g_cfg.unstick && static_cast<LONG>(f.now - s.stuckCheck) >= 0) {
                s.stuckCheck = f.now + 400;
                int hx = 0, hy = 0;
                if (MemoryHeroPos(hx, hy)) {
                    DWORD did = 0; LONG dx = 0, dy = 0;
                    if (s.stuckValid && std::abs(hx - s.stuckX) + std::abs(hy - s.stuckY) < 20 && findDoorAhead(did, dx, dy, false)) {
                        startBump(did, dx, dy, "movimento bloccato contro la porta chiusa");
                        s.stuckX = hx; s.stuckY = hy;
                        return;
                    }
                    if (s.stuckValid && std::abs(hx - s.stuckX) + std::abs(hy - s.stuckY) < 20) {
                        s.stuckLevel = s.stuckLevel % 3 + 1;
                        retry = true;
                        Log("Movimento bloccato: tentativo %d (%s)", s.stuckLevel,
                            s.stuckLevel == 1 ? "punto piu' lontano" : s.stuckLevel == 2 ? "ruotato a destra" : "ruotato a sinistra");
                    }
                    s.stuckX = hx; s.stuckY = hy; s.stuckValid = true;
                }
            }
            float r = g_cfg.moveRadius * f.hf;
            float mx = f.mdx, my = f.mdy;
            if (s.stuckLevel > 0) {
                r *= 1.8f;
                float a = s.stuckLevel == 2 ? 0.61f : s.stuckLevel == 3 ? -0.61f : 0.0f;   // 35 gradi
                float c = std::cos(a), sn = std::sin(a);
                float rx = mx * c - my * sn, ry = mx * sn + my * c;
                mx = rx; my = ry;
            }
            // Al momento della pressione il punto non deve cadere su una persona o
            // su un nemico (si aprirebbe un dialogo o partirebbe un attacco):
            // si prova un punto piu' vicino, poi uno piu' lontano.
            if (!s.moveHeld || retry) {
                const float tries[] = { 1.0f, 0.6f, 1.6f, 0.35f };
                for (float k : tries) {
                    float px = f.centerX + mx * r * k, py = f.centerY + my * r * k;
                    if (!MemoryPointOnCreature(px, py, f.hf)) {          // porte comprese (0.17.1)
                        if (k != 1.0f) Log("Movimento: punto spostato (x%.2f) per non cliccare su una creatura", k);
                        r *= k;
                        break;
                    }
                }
            }
            LONG cx = static_cast<LONG>(f.centerX + mx * r);
            LONG cy = static_cast<LONG>(f.centerY + my * r);
            avoidStickDoor(mx, my, r, cx, cy);          // anche col tasto tenuto (0.16.2)
            if (!g_virt.active.load()) { POINT p{ cx, cy }; ClientToScreen(hwnd, &p); Activate(p); }
            SetVirtualClient(s, cx, cy, f.w, f.h);
            MoveIfChanged(s);
            if (retry) {                               // nuovo clic sul nuovo punto (sotto, con l'attesa)
                SendLeftUp(hwnd);
                s.moveHeld = false;
                s.moveRetryPress = true;                 // la nuova pressione tiene il livello del tentativo
            }
            // 0.11.7: se il puntatore era su un bersaglio (anteprima o A appena premuto), il
            // primo clic di movimento aspetta kSettleMs: il gioco sceglie cio' che sta sotto il
            // puntatore un fotogramma prima e senza attesa il clic andava all'NPC (prova delle
            // 15:02:16, dialogo con Anurf aperto muovendo lo stick).
            if (!s.moveHeld && !s.moveSettling && (s.previewId != 0 || f.now - s.clickTime < 1000) &&
                OnObjectNow(s, f)) {
                s.moveSettling = true;
                s.moveSettleAt = f.now + kSettleMs;
                s.previewId = 0;
            }
            // 0.16.1: il gioco sceglie l'oggetto sotto il puntatore un fotogramma prima del clic. Alla prova
            // delle 17:34 l'anteprima aveva lasciato il puntatore sulla porta 1485; alla ripartenza il punto
            // libero veniva impostato e premuto nello stesso istante, e il clic andava alla porta (aperta e
            // chiusa a ogni ripartenza). La verifica sopra guardava il punto nuovo, non la scelta del gioco:
            // ora si legge la scelta del gioco e si aspetta (al massimo kHoverWaitMs) che non sia un oggetto.
            DWORD hoverId = 0, underDoor = 0;
            const bool doorUnder = MemoryDoorAt(cx, cy, underDoor);
            const bool coolUnder = doorUnder && underDoor == s.stickDoorId && static_cast<LONG>(f.now - s.stickDoorUntil) < 0;
            if (!s.moveHeld && MemoryHoverObject(hoverId) && !(doorUnder && hoverId == underDoor && !coolUnder)) {
                if (!s.hoverWait) { s.hoverWait = true; s.hoverWaitUntil = f.now + kHoverWaitMs; }
                if (static_cast<LONG>(f.now - s.hoverWaitUntil) < 0) {
                    s.moveSettling = true;
                    s.moveSettleAt = f.now + 20;
                }
                else {
                    Log("Movimento: il gioco indica ancora l'oggetto id %lu dopo %lu ms", hoverId, kHoverWaitMs);
                    DWORD did = 0;
                    if (static_cast<LONG>(f.now - s.stickDoorUntil) < 0 && hoverId == s.stickDoorId &&
                        MemoryDoorAt(cx, cy, did) && did == hoverId) {
                        s.moveSettling = true;           // la porta appena toccata: niente clic
                        s.moveSettleAt = f.now + 20;
                    }
                }
            }
            else s.hoverWait = false;
            if (s.moveSettling && static_cast<LONG>(f.now - s.moveSettleAt) >= 0) s.moveSettling = false;
            if (!s.moveHeld && !s.moveSettling) {
                s.hoverWait = false;
                SendLeftDown(hwnd);
                s.moveHeld = true;
                s.stuckValid = false;
                if (!s.moveRetryPress) s.stuckLevel = 0;
                s.moveRetryPress = false;
                s.stuckCheck = f.now + 400;
                Log("Movimento: inizio verso (%ld,%ld)", cx, cy);
            }
            // 0.53.8: diagnostica della direzione (prova sul PC del 30/10, tastierino 4 tenuto ed eroe andato a destra):
            // ogni 400 ms col tasto tenuto si confronta la destinazione data dal gioco all'eroe con lo stick. Una riga solo
            // quando sono lontane piu' di 60 gradi, con il punto della mod e il centro usato.
            if (s.moveHeld && f.now - s.dirDiagAt >= 400) {
                s.dirDiagAt = f.now;
                float ddx = 0, ddy = 0; int hx = 0, hy = 0, tx = 0, ty = 0;
                const float n = std::sqrt(mx * mx + my * my);
                if (n > 0.01f && MemoryHeroDestDir(ddx, ddy, hx, hy, tx, ty)) {
                    const float dot = (ddx * mx + ddy * my) / n;
                    if (dot < 0.5f)
                        Log("Movimento: ATTENZIONE destinazione del gioco verso (%.2f,%.2f), stick verso (%.2f,%.2f); punto (%ld,%ld), "
                            "centro (%.0f,%.0f); eroe (%d,%d) destinazione (%d,%d)", ddx, ddy, mx / n, my / n, cx, cy, f.centerX, f.centerY,
                            hx, hy, tx, ty);
                }
            }
        }
        else {
            if (s.directMoving) {                          // 0.53.9: stick rilasciato, l'eroe si ferma
                // 0.53.10: arresto rimandato di 150 ms. Prova del 30/10 sul PC: passando dal 4 al 6 fra il rilascio e la
                // nuova pressione passavano 13-170 ms, l'eroe riceveva "fermati" e subito "riparti" e si fermava davvero
                // prima di girarsi ("ci metteva un nanosecondo di piu'"). Se la nuova direzione arriva prima, niente arresto.
                s.directMoving = false;
                s.directStopAt = f.now + 150;
                Log("Movimento diretto: fine");
            }
            s.stuckLevel = 0;
            s.stuckValid = false;
            s.moveRetryPress = false;
            s.hoverWait = false;
            s.moveSettling = false;
            if (s.moveHeld) {
                SendLeftUp(hwnd);
                s.moveHeld = false;
                Log("Movimento: fine");
            }
            if (f.aiming) {                                // mostra il mirino senza attaccare
                if (!g_virt.active.load()) { POINT p{ f.aimX, f.aimY }; ClientToScreen(hwnd, &p); Activate(p); }
                SetVirtualClient(s, f.aimX, f.aimY, f.w, f.h);
                MoveIfChanged(s);
                s.previewId = 0;
            }
            else PreviewTick(s, f);                        // da fermi: il puntatore su cio' che A sceglierebbe
        }
    }

    if (g_virt.active.load() && f.now - s.lastStatus > 5000) {
        s.lastStatus = f.now;
        Log("Stato: sorgente %s, sx (%.2f, %.2f), dx (%.2f, %.2f), pulsanti 0x%04X, mira %s",
            PadSourceName(), pad.lx, pad.ly, pad.rx, pad.ry, buttons,
            f.ranged ? "Ranged" : "Melee");
    }
}

} // namespace

DWORD WINAPI CoreThread(LPVOID)
{
    {   // dentro GameServer.exe (anche lui importa VERSION.dll) niente ganci ne' pad: solo la sonda del server
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (strstr(exe, "GameServer.exe") || strstr(exe, "gameserver.exe")) { ServerMain(); return 0; }
    }
    Log("Thread principale avviato");
    LoadConfig();
    CoopRecoverSettings();
    InstallHooks();
    PadInit();
    MemoryInit();

    Loop s;
    for (int i = 0; i < 480 && !s.hwnd; ++i) {   // fino a 2 minuti
        Sleep(250);
        s.hwnd = FindGameWindow();
    }
    if (!s.hwnd) { Log("Finestra del gioco non trovata entro 2 minuti: SacredPad inattivo"); return 0; }
    g_gameHwnd.store(s.hwnd);
    LogWindow(s.hwnd);
    // 0.56.58 (pubblicazione): con un Sacred.exe diverso da pureHD 2.0.2.118 la lettura della memoria e' spenta e con lei la scritta
    // sopra il gioco (usa il motore del gioco): si avvisa con una finestra di Windows, una volta per avvio, e con "No" non piu'
    // (WarnWrongExe=false nell'ini). Resta il controller di base. Senza proprietaria: una MessageBox disabilita la finestra che la
    // possiede finche' e' aperta, e dietro al gioco a schermo intero lo avrebbe bloccato senza che si vedesse perche'.
    if (g_cfg.memoryEnabled && g_cfg.warnWrongExe && !MemoryReady()) {
        if (HANDLE th = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(3000);
            const int r = MessageBoxA(nullptr, Tr("Questo Sacred.exe non e' quello per cui e' fatta SacredPad (pureHD 2.0.2.118).\n\nFunziona solo il controller di base: niente co-op, aggancio dei bersagli, due zaini e finestre della mod. Vedi \"Requirements\" nel README.\n\nMostrare di nuovo questo avviso al prossimo avvio?"),
                                      "SacredPad", MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
            Log("Avviso dell'eseguibile non riconosciuto mostrato: %s", r == IDNO ? "No (WarnWrongExe=false)" : "Si'");
            if (r == IDNO) SaveConfigValue("Memory", "WarnWrongExe", "false");
            return 0;
        }, nullptr, 0, nullptr)) CloseHandle(th);
    }
    RealGetCursorPos(&s.lastReal);

    for (;;) {
        Sleep(static_cast<DWORD>(g_cfg.tickMs));
        if (!IsWindow(s.hwnd)) {
            HWND h = FindGameWindow();
            if (!h) continue;
            s.hwnd = h;
            g_gameHwnd.store(h);
            LogWindow(h);
        }
        Tick(s);
        OverlayTick(GetTickCount());
    }
}

// 0.56.47: fuori dallo spazio anonimo (dichiarata in SacredPad.h, usata da Memory.cpp)
bool MemoryPad1AHeld() { return g_pad1AHeld.load(); }
