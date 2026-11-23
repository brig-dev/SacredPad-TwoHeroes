// SacredPad.h - dichiarazioni comuni a tutti i moduli
// Linguaggio: C++17. Compilatore: MSVC (Visual Studio Community 2026), Win32 (x86).
#pragma once

#if !defined(_M_IX86)
#error "SacredPad va compilato per Win32 (x86): Sacred.exe e' a 32 bit."
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <vector>
#include <string>

#define SACREDPAD_VERSION "0.9.0 beta"

// --- SacredPad.cpp: log e informazioni generali ------------------------------
void        Log(const char* fmt, ...);
const char* GameDir();       // cartella del gioco (dove sta version.dll)
HMODULE     SelfModule();    // handle di questa DLL

// --- Lang.cpp: testi nella lingua del gioco (0.48.0) ---------------------------
const char* PotionName(int kind);     // 0.56.25: nome breve della pozione del tipo 1..5 (italiano, da passare a Tr)
const char* Tr(const char* italiano);   // traduzione del testo visibile (stesso testo se manca o se la lingua e' l'italiano)

// --- Config.cpp: SacredPad.ini -------------------------------------------------
enum class PadSource { Auto, XInput, Keyboard, Laptop, None };   // Laptop = simulatore senza tastierino; None (0.55.14) = mouse e tastiera
enum class AimMode   { Melee, Ranged };   // corpo a corpo / distanza

struct Config {
    PadSource padSource       = PadSource::Auto;
    char      language[16]    = "Auto"; // 0.48.0: lingua dei testi della mod (Auto, it, en, de, fr, es)
    int       controllerIndex = 0;      // 0 = primo pad XInput
    int       tickMs          = 8;      // periodo del ciclo principale (ms)
    float     leftDeadzone    = 0.24f;  // zona morta stick sinistro (0..1)
    float     moveRadius      = 0.17f;  // distanza puntatore-personaggio, frazione dell'altezza
    float     centerOffsetX   = 0.0f;   // correzione del centro del personaggio,
    float     centerOffsetY   = 0.055f; //   frazioni dell'altezza della finestra
    AimMode   aimMode         = AimMode::Melee;
    float     rightDeadzone   = 0.20f;  // zona morta stick destro (0..1)
    float     aimMinRadius    = 0.08f;  // distanza del mirino con lo stick appena inclinato
    float     aimRadius       = 0.42f;  // distanza del mirino con lo stick tutto inclinato
    float     rangedDefault   = 0.30f;  // distanza dell'attacco a distanza senza mira
    float     reclickDistance = 0.06f;  // con il tasto tenuto, nuovo clic se la mira si sposta oltre (frazione altezza)
    int       artClickDelayMs = 50;     // attesa tra il tasto dello slot e il clic destro
    int       unlockedSlots   = 0;      // slot sbloccati (armi 1..n e arti 1..n); 0 = dal livello letto in memoria
    int       artHoldMs       = 80;     // durata minima del clic destro di un'arte
    int       artMoveLockMs   = 300;    // dopo un'arte il movimento riprende solo trascorso questo tempo
    int       holdReclickMs   = 600;    // con A tenuto, nuovo clic periodico (0 = disattivato)
    int       minimapMethod   = 0;      // 0 = Tab fisico, 1 = clic centrale fisico (SendInput)
    int       minimapHoldMs   = 150;    // durata minima della pressione per la minimappa
    int       zoomHoldMs      = 350;    // 0.55.31: croce su/giu' tenuta oltre questo tempo = zoom (rotellina); 0 = niente zoom
    bool      zoomInvert      = false;  // 0.55.31: croce su = rotellina indietro (se lo zoom va al contrario)
    bool      autoPortal      = true;   // 0.56.38: col movimento diretto, camminando dentro un portale lo si attraversa
    int       dpadPotion[2]   = { 5, 3 };   // 0.56.25: tipi delle pozioni di croce su / giu' (2 Q, 3 W, 4 E, 5 R; [Keys] DpadUpPotion...)
    bool      minimapToggle   = true;   // true = premi per aprire/chiudere, false = tieni premuto
    float     cursorSpeed     = 0.9f;   // modalita' cursore: velocita' massima, altezze al secondo
    float     cursorFine      = 0.3f;   // stick destro: frazione della velocita' (movimento fine)
    float     cursorCurve     = 2.0f;   // curva di risposta dello stick (1 = lineare)
    float     cursorNudge     = 0.015f;  // passo della croce, frazione dell'altezza
    int       viewHoldMs      = 400;    // View tenuto oltre questo tempo: inventario (I) + cambio di modalita'
    bool      startInCursor   = true;   // all'avvio la mod e' in modalita' cursore (il gioco si apre sui menu)
    bool      memoryEnabled   = true;   // lettura della memoria del gioco (fase 2)
    bool      memoryDiag      = false;  // diagnostica della memoria nel log (0.56.58: spenta se manca nell'ini, per la release)
    bool      warnWrongExe    = true;   // 0.56.58: finestra di avviso se Sacred.exe non e' quello riconosciuto
    int       memoryDiagMs    = 3000;   // periodo della diagnostica
    bool      nativeAuto      = false;  // fase 5: crea da solo l'eroe 2 quando si entra in partita
    int       nativeHero2     = 7;      // fase 5: file Save/HeroNN.pax dell'eroe 2
    bool      nativeEnabled   = true;  // fase 5 (0.49.0, N6): interruttore generale della co-op nativa
    bool      nativeServerProbe = false; // fase 5: carica la DLL anche in GameServer.exe e ne registra le creature
    bool      nativeHeapCheck = false;   // 0.55.24: diagnostica, HeapValidate su tutti gli heap ogni 3 s (crash all'uscita al menu)
    bool      nativePlayers   = false; // fase 5: l'eroe 2 entra nell'elenco dei giocatori (bersagli dei nemici)
    bool      nativeMonsterWatch = false; // fase 5: punti di controllo hardware sul nemico piu' vicino (chi scrive stato e modo)
    bool      nativeDiagWatch = false;    // fase 5: punti di controllo sull'oggetto comandato a ogni cambio di comando
    int       nativeRevive    = 30;    // fase 5: secondi dopo la morte dell'eroe 2 prima che si rialzi accanto all'eroe 1 (0 = mai)
    int       nativeReviveNear = 3;    // fase 5 (0.42.0): secondi che l'eroe 1 deve restare vicino all'eroe 2 a terra per rialzarlo (0 = spento)
    int       nativeReviveRange = 250; // fase 5 (0.42.0): distanza massima per il rialzo vicino, in unita' di mondo
    int       nativeXpRange   = 1500;  // fase 5 (0.42.0): l'esperienza condivisa arriva all'eroe 2 solo entro questa distanza (0 = ovunque)
    bool      nativeHero2Bar  = true;  // fase 5 (0.42.0): nome, livello e vita dell'eroe 2 disegnati in alto al centro
    bool      nativeHero2Taskbar = true; // 0.52.0 (N4 fase C): seconda barra in basso (armi, arti, cintura) dell'eroe 2
    int       nativeHero2TaskbarDx = 0;  // spostamento della seconda barra in pixel (0 = barre ai lati)
    bool      nativeHero1TaskbarLeft = true; // 0.52.8: con due giocatori barra dell'eroe 1 a sinistra e dell'eroe 2 a destra
    bool      nativeDirectMove = true;   // 0.53.9: con la camera sul punto medio lo stick muove l'eroe 1 con ordini diretti, non con clic
    bool      nativePad2      = true;  // fase 5 (0.44.0): il secondo controller comanda l'eroe 2
    int       nativePad2Index = -1;    // fase 5 (0.44.0): slot XInput del pad 2 (-1 = il primo collegato diverso dal pad 1)
    bool      nativePad2Keyboard = false; // 0.55.1: con PadSource=Keyboard le lettere fanno il pad 2 (prove senza controller)
    bool      nativePad2WalkToUse = true; // 0.55.12: il gestore del giocatore 2 non ferma piu' il modo 1 (vai e usa) dell'eroe 2
    bool      nativePad2NoCtrl = true;   // 0.55.37: il gestore del giocatore 2 non vede il Ctrl del pad 1 (eroe 2 che colpiva sul posto)
    bool      nativeQuestItems = true;   // 0.55.38: gli oggetti delle missioni raccolti dall'eroe 2 passano all'eroe 1
    bool      nativeSharedLoot = true;   // 0.55.39: con l'eroe 2 un oggetto creato su due e' per la sua classe
    bool      nativeKeepMap = true;      // 0.55.39: alla comparsa dell'eroe 2 la mappa scoperta del suo file si somma a quella del mondo
    bool      nativeRevealHero2 = true;  // 0.55.40: anche l'eroe 2 scopre la mappa intorno a se'
    bool      nativeSharedGold = true;   // 0.55.41: i due eroi hanno lo stesso oro
    bool      nativeArtOnHero2 = true;   // 0.55.42: arte del pad 1 mirata sull'eroe 2 = clic destro semplice su di lui (cure)
    bool      nativeRewardRunes = true;  // 0.55.47: runa di classe data dalla missione all'eroe 1 -> una runa della classe dell'eroe 2
    bool      nativeChestRunes = true;   // 0.56.45: runa di un eroe da un forziere appena aperto -> una runa della classe dell'altro
    bool      nativeRewardPoints = true; // 0.56.53: punti attributo e abilita' dati dalle missioni anche all'eroe 2
    bool      nativeTwoInventories = true; // 0.55.48: zaino ed equipaggiamento dei due eroi insieme (copia disegnata dell'eroe 2)
    bool      nativeCompanionView = true;  // 0.56.28: clic destro sul ritratto di un compagno = suo equipaggiamento e sue statistiche
    bool      nativeCompanionXp = true;    // 0.56.31: l'esperienza dell'eroe 1 anche ai compagni dei ritratti (livelli e punti)
    bool      nativeCompanionAnyClass = true; // 0.56.34: ai compagni armature, gioielli e armi di qualunque classe (requisiti a parte)
    bool      nativeMouse2 = true;         // 0.56.0: secondo mouse del gioco per il giocatore 2 (cursore e mano propri)
    bool      nativeAudioBoth = true;    // 0.55.13: con due eroi i suoni si misurano dal segmento fra gli eroi, non dal punto medio
    bool      nativePad2EdgeLimit = true; // 0.55.30: lo stick del pad 2 non porta l'eroe 2 oltre il bordo dello schermo o del guinzaglio
    float     nativePad2Dead  = 0.35f; // fase 5 (0.44.0): zona morta dello stick sinistro del pad 2
    bool      nativeSaveHero2 = true;  // fase 5 (0.45.0): a ogni salvataggio della partita l'eroe 2 si salva nel suo HeroNN.pax (con copia)
    bool      nativeCleanSaved = true; // 0.54.0: l'eroe 2 arrivato con il salvataggio della partita si toglie (copia in Save\SacredPad_backup)
    bool      nativePortrait  = true;  // fase 5 (0.43.0): ritratto dell'eroe 2 disegnato dal gioco sotto quello dell'eroe 1
    bool      nativeHero2Name = true;  // fase 5 (0.45.9/0.46.0, N5): nome dell'eroe 2 dal suo file (gancio su 0x673300) invece che dall'eroe 1
    bool      nativeSwap      = true;  // fase 5: tasto Home = l'interfaccia (inventario, statistiche) passa all'eroe 2 (prova)
    bool      nativeSwapUi    = true;  // 0.51.0 (N4 fase B): lo scambio devia solo l'interfaccia, l'eroe locale resta l'1 (il mondo non si ferma)
    int       nativeXpFactor  = 1;     // fase 5: moltiplicatore dell'esperienza condivisa con l'eroe 2 (prova: 10 per arrivare presto a un nuovo livello)
    bool      nativeShareXp   = true;  // fase 5: l'esperienza guadagnata dall'eroe 1 (+0x3B4, da verificare) si somma anche all'eroe 2
    bool      nativeMenu      = true;  // fase 5: voce "Due giocatori" nel menu principale (schermo diviso o scelta dell'eroe 2)
    bool      nativeAggro     = true;  // fase 5: i nemici vicini all'eroe 2 vanno verso di lui (0.55.27: acceso anche senza la voce nell'ini)
    int       nativeAggroRange = 800;  // fase 5: raggio di percezione dell'eroe 2, in unita' di mondo
    bool      nativeCamera    = true;   // fase 5: con l'eroe 2 la camera segue il punto medio dei due eroi
    bool      nativeVerbose   = false;   // fase 5: registro dettagliato (censimento, confronto, STAT, traccia, salvataggi, server)
    bool      lockEnabled     = true;   // aggancio automatico del bersaglio
    float     lockRange       = 0.45f;  // distanza massima del bersaglio, frazione dell'altezza
    float     lockAngle       = 50.0f;  // semiapertura del cono nella direzione dello stick, gradi
    float     lockNear        = 0.12f;  // fuori dal cono: solo bersagli entro questa distanza
    float     lockOffsetY     = 0.03f;  // il clic cade sopra i piedi della creatura, frazione dell'altezza
    float     talkRange       = 0.22f;  // distanza massima per parlare con un NPC con A
    int       talkCycleMs     = 3000;   // A ripetuto entro questo tempo: si passa al prossimo NPC/oggetto (0 = no)
    unsigned  neutralMask     = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7);   // tipi non ostili (NeutralTypes)
    unsigned  hostileMask     = ~((1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7)); // ostili: tutti gli altri, o HostileTypes
    bool      unstick         = true;   // movimento: se il personaggio non avanza, prova altri punti
    bool      autoCursor      = true;   // modalita' cursore automatica con le finestre del gioco
    bool      previewTarget   = true;   // da fermi il puntatore si posa su cio' che A sceglierebbe (0.10.0)
    bool      keyLog          = false;  // diagnostica: ogni tasto visto dal simulatore nel log (0.56.58: spenta se manca nell'ini)
    bool      overlayEnabled  = true;   // scritte della mod sopra il gioco (fase 3)
    int       showVersion     = 2;      // 0.56.58: scritta della versione in alto: 2 sempre, 1 solo nei menu (fuori dal mondo), 0 mai
    bool      gridMarkers     = false;  // "+" al centro delle caselle e cornici dei pulsanti (verifica)
    bool      vibration       = true;   // vibrazione quando la vita lampeggia (sotto il 25%), [Pad] Vibration
    bool      playerMarks     = true;   // 0.56.16: anello e freccia dei due eroi, [Overlay] PlayerMarks (anche dalla schermata Controller)
    bool      nativeShareHeal = true;   // 0.56.16: le cure delle arti valgono per tutti e due gli eroi, [Native] ShareHeal
    int       vibrationPower  = 50;     // forza in percentuale, [Pad] VibrationPower
    int       padLayout       = 0;      // nomi dei tasti nell'aiuto e sopra le arti: 0 Steam, 1 Xbox, 2 DualSense
    int       pad2Layout      = 0;      // 0.51.3: lo stesso per il controller del giocatore 2 ([Overlay] Pad2Layout)
    unsigned  talkMask        = (1u << 3) | (1u << 4) | (1u << 7);   // tipi con cui parlare (TalkTypes; 4 = cavalli)
    // Co-op locale (0.19.0), sezione [Coop]
    int       coopRole        = 0;      // 0 = spenta, 1 = Host (lancia la seconda istanza), 2 = Guest
    bool      coopNav         = true;   // navigazione automatica dei menu verso la lobby (host e ospite)
    bool      coopEnabled     = true;   // nel gioco normale il tasto LaunchKey avvia la sessione co-op
    bool      coopIgnoreMouse = false;  // il mouse vero non spegne il pad e il pad lavora anche senza il focus
    bool      coopBorderless  = true;   // finestra senza bordo alla posizione WindowRect
    int       coopX = 0, coopY = 0, coopW = 0, coopH = 0;                    // WindowRect di questa istanza (w = 0: non toccare)
    int       coopGuestX = 0, coopGuestY = 0, coopGuestW = 0, coopGuestH = 0; // GuestRect: dove sta l'ospite
    int       coopScreenW = 0, coopScreenH = 0;   // 0.49.3: schermo letto all'avvio (desktop virtuale di Wine della co-op)
    int       coopGuestPad    = 1;      // indice XInput del pad dell'ospite
    int       coopGuestMusic  = 0;      // 0.55.37: volume della musica dell'ospite (MUSICVOLUME); -1 = come nel suo Settings.cfg
    int       coopLaunchVk    = VK_F8;  // tasto di prova che avvia la seconda istanza (0 = nessuno)
    bool      coopReturnNormal = false; // a fine sessione la prima istanza torna al menu normale (false: il gioco si chiude)
    bool      coopHookWindows = true;   // intercetta SetWindowPos/MoveWindow del gioco (per escluderla in caso di crash)
    int       coopWineDesktop = 2;      // desktop virtuale di Wine per le due istanze: 0 = no, 1 = si', 2 = solo se gira su Wine/Proton (Auto)
    bool      coopBackdrop    = false;  // finestra nera a tutto schermo dietro alle due istanze (host)
    char      coopGuestDir[MAX_PATH] = "";   // cartella della seconda copia del gioco
};
extern Config g_cfg;
void LoadConfig();
void SaveConfigValue(const char* section, const char* key, const char* value);   // scrive una voce nell'ini

// --- Coop.cpp: seconda istanza e finestre affiancate (fase 4) -------------------
void CoopTick(HWND hwnd);
void CoopStartFromMenu(HWND hwnd);   // voce "Co-op locale" del menu principale
bool CoopSessionActive();
void CoopRecoverSettings();   // all'avvio normale: ripristina Settings.cfg di una sessione finita male
void CoopWindowCall(const char* fn, HWND h, int x, int y, int cx, int cy, UINT flags);   // diagnostica
void CoopWindowOverride(HWND h, int& x, int& y);                                         // posizione voluta
void HookWindowCalls();                                                                  // Hooks.cpp

// --- Pad.cpp: lettura del controller (XInput o simulatore da tastierino) -----
enum : WORD {   // stessi valori dei pulsanti XInput
    PAD_DPAD_UP = 0x0001, PAD_DPAD_DOWN = 0x0002, PAD_DPAD_LEFT = 0x0004, PAD_DPAD_RIGHT = 0x0008,
    PAD_START   = 0x0010, PAD_BACK      = 0x0020, PAD_LS        = 0x0040, PAD_RS         = 0x0080,
    PAD_LB      = 0x0100, PAD_RB        = 0x0200,
    PAD_A       = 0x1000, PAD_B         = 0x2000, PAD_X         = 0x4000, PAD_Y          = 0x8000,
    // bit liberi di XInput usati per i grilletti trattati come pulsanti
    PAD_LT      = 0x0400, PAD_RT        = 0x0800,
};

struct PadState {
    bool  connected = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;   // stick, -1..1, y positiva = in alto
    float lt = 0, rt = 0;                   // grilletti, 0..1
    WORD  buttons = 0;
};

void        PadInit();
void        PadRead(PadState& out);
void        PadVibrate(float left, float right);   // motori 0..1 (solo XInput; 0, 0 = ferma)
const char* PadSourceName();
bool        PadRead2(PadState& out, int& index);       // pad del giocatore 2 (co-op nativa); index = slot XInput
void        PadVibrate2(int index, float left, float right);   // 0.55.29: vibrazione del pad 2 (slot XInput di PadRead2)
constexpr int kPad2KeyboardIndex = 9;                   // 0.55.1: index di PadRead2 per il pad 2 da tastiera (Pad2Keyboard)

// --- Hooks.cpp: input virtuale visto dal gioco ------------------------------
struct VirtualInput {
    std::atomic<bool> active{ false };  // true = il gioco vede il puntatore virtuale
    std::atomic<LONG> x{ 0 }, y{ 0 };   // posizione del puntatore, coordinate SCHERMO
    std::atomic<bool> lmb{ false };     // pulsante sinistro virtuale premuto
    std::atomic<bool> rmb{ false };     // pulsante destro virtuale premuto
    // Tasti della tastiera "premuti" dal pad, visti da GetAsyncKeyState e
    // GetKeyboardState: 0 = rilasciato, kHeld = tenuto, altro = scadenza (GetTickCount).
    static constexpr DWORD kHeld = 0xFFFFFFFFu;
    std::atomic<DWORD> keyUntil[256] = {};
};
extern VirtualInput g_virt;
bool VirtualKeyDown(int vk);
extern std::atomic<DWORD> g_p2HandlerTid, g_p2HandlerAt, g_p2CtrlUntil;   // 0.55.37: gestore del giocatore 2 in corso, Ctrl del pad 2
extern std::atomic<DWORD> g_noCtrlTid;
void MusicMute(bool on);   // 0.55.45: musica (stream di Miles) spenta o riaccesa in questa copia del gioco   // 0.55.14: per questo thread GetAsyncKeyState dice "Ctrl su" (ordini della mod all'eroe 2)

bool InstallHooks();
BOOL RealGetCursorPos(LPPOINT p);       // posizione del mouse vero
BOOL RealSetCursorPos(int x, int y);    // sposta il mouse vero (coordinate schermo)

// --- Memory.cpp: lettura della memoria del gioco (fase 2) ------------------
void MemoryInit();
bool MemoryReady();                      // Sacred.exe riconosciuto e lettura attiva
void MemoryDiagTick(DWORD now);
void MemoryDiagClick(LONG cx, LONG cy);
void MemoryDiagProbe(HWND hwnd);
void MemoryDiagUI(DWORD now);
void MemoryDiagScanUI();
void MemoryDiagDump();
void MemoryNativeProbe(HWND hwnd);   // fase 5: co-op nativa (eroe 2, menu, camera, raccolta, scambio) e prove con F11
void EngineThreadTick();             // dal thread del motore (EndScene agganciato): esegue i lavori in coda
void ServerMain();                   // fase 5: sonda dentro GameServer.exe
bool MemoryHeroLevel(int& level);
bool MemoryHorseButton(LONG& x, LONG& y);  // stalliere: centro dell'ultimo pulsante attivo   // livello dell'eroe (1..216)        // F5: dati del giocatore su file
int  MemoryUIState();
bool MemoryHeroesScreen(POINT* p, bool* on, int& w, int& h, float* dir = nullptr);   // 0.56.16: i due eroi sullo schermo (thread del motore); 0.56.18: dir[4] = dove guardano
void MemoryNoteUsedNpc(DWORD id);                             // 0.56.18: PNG appena usato (finestra o dialogo chiusi): in secondo piano per qualche secondo
void MemoryJoinCompanion(int who);                            // 0.56.16: l'eroe 2 raggiunge l'eroe 1 (doppio L3 di un pad)
bool MemoryWindowOpen(int index, RECT* rect = nullptr);
bool MemoryGameMenuOpen();      // 0.55.27: menu Esc, opzioni o salvataggi aperti (partita in pausa)
bool MemoryHeroTalking();       // 0.55.29: l'eroe 1 parla con un PNG (modo 10)
bool MemoryDirectMoveBlocked(); // 0.55.36: il gioco ha appena rifiutato il punto davanti all'eroe 1: clic sul terreno per 2,5 s
void GameZoomStep(int dir, const char* who);   // 0.55.31: zoom di un passo (rotellina avanti dir > 0, indietro dir < 0)
// Zaino dell'eroe visibile (finestra n.1, scheda dello zaino attiva): griglia in coordinate client.
struct GridInfo { DWORD entry = 0; int inv = 0, cols = 0, rows = 0, ox = 0, oy = 0, pageX = 0, pageY = 0, x = 0, y = 0;
                  const char* kind = ""; };
bool MemoryHeroGrid(GridInfo& g);
bool MemoryMerchantGrid(GridInfo& g);      // merce della scheda attiva del mercante
int MemoryWindowGrids(GridInfo* out, int max);   // fabbro, deposito, cubo, scambio aperti (0.18.0)
struct MenuItem;
bool MemoryEquipSlots(std::vector<MenuItem>& out);
bool MemoryWindowControls(std::vector<MenuItem>& out, int clientW, int clientH);   // fabbro, maestro, schede Arti/Combo (0.18.2, aggiunti a out)
bool MemoryStatButtons(std::vector<MenuItem>& out);  // "+" della scheda del personaggio (aggiunti a out)   // caselle dell'equipaggiamento (finestra n.2)
int  MemoryTaskbarArts(POINT* centers, int n);        // cerchi visibili delle arti nella barra in basso (centri)
int  MemoryTaskbarPotions(POINT* centers);            // 0.56.23: cerchi delle pozioni 1 (cura), 4 (antidoto), 5 (concentrazione)
// Riga "Controller" nel menu Esc (0.15.3): allarga il menu di una riga (e lo rimette com'era alla chiusura).
struct EscRow { RECT r{}; DWORD font = 0, color = 0, hoverColor = 0; };
bool MemoryEscMenuRow(EscRow& row);
bool NativeCoopPickerOpen();                                   // 0.47.3: elenco "Co-op" aperto (Memory.cpp)
void NativeCoopPickerPad(int move, bool ok, bool cancel);      // croce, A e B del pad per l'elenco
bool NativePad2TakeInvToggle();   // 0.55.50: il pad 2 con il puntatore ha tenuto Back: zaino aperto o chiuso (tasto I)
bool MemoryDualActive();          // 0.55.50: i due zaini sono disegnati (ultimi 400 ms)
bool MemoryDualSide(int who);     // 0.55.54: lato dello zaino dell'eroe 1 o 2 aperto
bool MemoryCompanionCycle(int who); // 0.56.31: RT in modalita' cursore: la colonna del giocatore passa al compagno seguente
int  MemoryCompHands(RECT* r);      // 0.56.33: caselle delle mani dei compagni disegnate adesso (al massimo 4), per la cornice
// 0.56.40: scheda Compagni nello zaino (disegnata dall'overlay, clic e azioni in Memory.cpp); 0.56.41: una per colonna, con i
// cursori dei due giocatori da ridisegnare sopra la pagina
constexpr int kCompPanelRows = 6;
struct CompPanelRow { char name[48] = ""; char status[64] = ""; char button[32] = ""; bool action = false; };
struct CompPanelCol { bool tab = false, open = false; RECT tabR{}, panelR{}; };
struct CompPanelView { CompPanelCol col[2]; int n = 0; CompPanelRow rows[kCompPanelRows]; bool curOn[2] = {}; POINT cur[2] = {}; };
bool MemoryCompPanel(CompPanelView& v);
RECT MemoryCompRowButton(const RECT& panel, int i);
RECT MemoryCompRow(const RECT& panel, int i);         // 0.56.43: riga intera (clic e luce)
bool MemoryCompTab(int side, RECT& tab, bool& open);  // 0.56.43: scheda Compagni della colonna, per LB/RB
void MemoryDualSetSide(int who, bool on);
bool MemoryDualPossible();        // 0.55.54: eroe 2 pronto e nessun pannello oltre a zaino, equipaggiamento e statistiche
bool MemoryDualPointInUi(LONG x, LONG y);   // 0.55.56: il punto cade su una delle finestre dei due zaini disegnate
bool MemoryHero2Grid(GridInfo& g);   // 0.55.54: griglia dello zaino dell'eroe 2 come la disegna la copia
bool MemoryHero2Items(std::vector<MenuItem>& out);   // 0.55.63: caselle dell'equipaggiamento e "+" delle statistiche dell'eroe 2 disegnati
bool MemoryDualCopyTabs(int& active, int& count, RECT* rects, int maxRects);   // 0.55.54: schede della copia dello zaino
bool NativePad2TakeSideReq();
void MemoryHero1Attack(DWORD enemyId);   // 0.55.55: attacco dell'eroe 1 con un ordine diretto (mouse del gioco al giocatore 2)
void MemoryHero1Art(int slot, DWORD target);   // 0.55.55: arte dello slot 0..4 dell'eroe 1 con un ordine diretto     // 0.55.54: Back del pad 2 con i due zaini: apri o chiudi il suo lato
void NativePad2ReturnPointer();   // 0.55.49: il giocatore 2 ha chiuso le finestre: puntatore al pad 1 (solo durante il suo scambio)
bool NativePad2HasPointer();   // 0.50.0 (N4): scambio partito dal pad 2, il puntatore e' suo (Memory.cpp)
int  NativePad2UiEvent();      // 1 = puntatore al pad 2, 2 = restituito con Back del pad 2, 3 = restituito per altro; 0 nessuno
void NativePad2UiEventPutBack(int ev);   // 0.55.18: rimette un evento letto troppo presto (Core senza pad 1)
bool NativePad2TakeHelpReq();  // 0.51.3: il pad 2 ha tenuto Start (apre l'aiuto con il suo controller)
bool NativePad2TakeInteract(); // 0.55.0: l'eroe 2 e' partito verso un PNG, un forziere, una porta o un oggetto fisso (A del pad 2)
WORD NativePad2TakeDialogBtn(); // 0.55.0: PAD_A o PAD_B del pad 2 con un dialogo aperto (0 = nessuno)
WORD NativePad2Buttons();       // 0.55.16: pulsanti del pad 2 in modalita' gioco (0 se non letti da 200 ms), per R3 e L3
bool NativePad2InGame();        // 0.55.27: pad 2 letto in modalita' gioco negli ultimi 300 ms (simboli sulla sua barra)
bool NativePad2Talking();       // 0.55.28: eroe 2 nel modo 10 (parla con un PNG) negli ultimi 300 ms
bool NativePad2AutoSwap();     // 0.55.2: interfaccia passata da sola all'eroe 2 per un mercante, un fabbro o uno stalliere
void NativePad2SetInHelp(bool on);   // 0.51.3: l'aiuto e' del pad 2 (NativePad2Tick si ferma, Core.cpp usa il pad 2)
bool MemoryHeldAny();   // 0.55.66: qualunque cosa sul puntatore (oggetto, arte, combo)
// 0.56.0: secondo mouse del gioco per il giocatore 2 (Memory.cpp, "Secondo mouse"): cursore disegnato dal gioco, mano propria
bool MemoryMouse2Ready();                                   // attivo: i clic del giocatore 2 non usano piu' il mouse del gioco
void MemoryMouse2Ui(bool on, LONG x, LONG y);               // puntatore del giocatore 2 nelle finestre (Core.cpp, Pad2Pointer)
void MemoryMouse2Button(int btn, bool down, LONG x, LONG y, bool shift = false);   // pulsante (0 sinistro, 2 destro); shift: con Shift tenuto
bool MemoryMouse2Held();                                    // il giocatore 2 ha qualcosa in mano
bool MemoryMouse2OverUi();                                  // il suo cursore e' sopra una finestra del gioco
DWORD MemoryMouse2Hover();                                  // oggetto del mondo sotto il suo cursore (0 = nessuno)
void MemoryMouse2Stow(const char* why);                     // 0.56.4: cio' che ha in mano torna nel suo zaino (thread del motore)
void MemoryMouse2Drop(LONG x, LONG y);                      // 0.56.6: getta a terra l'oggetto che ha in mano, verso il punto (x, y)
void MemorySwapMouse2(bool on);                             // 0.56.7: scambio dell'interfaccia guidato dal pad 2 sul secondo mouse
void MemoryPad1GameMode(bool on);                           // il pad 1 e' in modalita' gioco (i suoi clic sulla copia vanno al mondo)
bool MemoryPad2CursorMode();                                // 0.56.2: modalita' cursore del giocatore 2 (View premuto, come il pad 1)
bool MemoryMouse2WorldPos(LONG& x, LONG& y);                // 0.56.2: dov'e' il suo cursore nel gioco normale (se disegnato adesso)
// 0.56.9: mercante condiviso (Memory.cpp, "Mercante condiviso")
bool MemoryMerchantShared();                                // il mercante e' disegnato a due lati (ultimi 400 ms)
bool MemoryMerchantCopyTabs(int& active, int& count, RECT* rects, int maxRects);   // schede della copia (eroe 2)
bool MemoryMerchantCopyGrid(GridInfo& g);                   // merce come la disegna la copia
LONG MemoryMerchantRealDx();                                // spostamento del lato dell'eroe 1 (0 se non condiviso)
void MemoryMerchantFixGrid(GridInfo& g);                    // origine della merce del lato dell'eroe 1
bool MemoryPad2MerchantRecent();                            // l'eroe 2 e' appena andato a parlare con un mercante
void MemoryPad2MerchantClear();
bool MemoryIsMerchant(DWORD id);                            // 0.56.10: la creatura id e' un mercante (+0x200 bit 0x2000)
// 0.56.14: mercante, fabbro e Maestro condivisi (finestre n.4, n.3, n.14 del gestore)
int MemorySharedWin();                                      // finestra condivisa disegnata a due lati (ultimi 400 ms), -1 nessuna
LONG MemorySharedRealDx();                                  // spostamento del lato dell'eroe 1 della finestra condivisa
int MemorySharedServiceWin(DWORD id);                       // finestra che apre la creatura id (4, 3, 14) se si puo' condividere
int MemoryPad2ShopWin();                                    // finestra condivisa a cui l'eroe 2 e' appena andato (-1 nessuna)
void MemoryPad2WorldButton(int btn, bool down, LONG x, LONG y);   // 0.56.2: suo clic fuori dalle sue finestre (NativePad2Tick)
bool MemoryHero2BarPoint(LONG x, LONG y);   // 0.55.67: il punto cade sulla barra dell'eroe 2 disegnata
bool MemoryHeldItem(DWORD* type, DWORD* id);   // oggetto tenuto sul puntatore ([[0xcdcadc]+0x68], tipo 1)
bool MemoryHero2TaskbarOnTop();   // 0.52.7: la seconda barra e' disegnata in alto (la scritta della mod va a sinistra)
bool MemoryMoveHero1(float sdx, float sdy);
void DiagHoverFixed(DWORD now);
void DiagTriggers(DWORD now);   // 0.56.47: con MemoryDiag, voci della tabella dei trigger vicine all'eroe 1
bool MemoryPad1AHeld();   // 0.56.47: il pad 1 tiene A (per la traccia dell'attacco del pad 2)   // 0.56.46: con MemoryDiag, oggetto fisso della mappa sotto il cursore del gioco nel log
bool MemoryAutoPortal(float sdx, float sdy);   // 0.56.38: eroe 1 dentro il rettangolo di un portale: ordine "vai e usa" come il clic (0.56.42: o vicino, con lo stick verso di lui)    // 0.51.0: ordine di movimento all'eroe 1 verso una direzione dello schermo (scambio dell'interfaccia o camera sul punto medio)
bool MemoryStopHero1();                        // 0.53.9: l'eroe 1 si ferma dove e'
bool MemoryNativeCameraOn();                   // 0.53.9: camera sul punto medio dei due eroi accesa
bool MemoryMainMenuRowShownNow();    // 0.49.7: le voci del menu principale sono visibili adesso (letto dal disegno)
bool MemoryMainMenuRow(EscRow& row);   // riga libera fra "Extra" e "Esci" del menu principale (0.20.1)
// Schede della finestra n. index (1 = inventario: Zaino, Arti, Combo): attiva, numero, pulsanti.
bool MemoryWindowTabs(int index, int& active, int& count, RECT* rects, int maxRects);
// Pulsanti visibili dei menu e delle finestre aperte (0.12.3), coordinate client.
struct MenuItem { DWORD addr = 0, vt = 0, flags = 0; int x = 0, y = 0, w = 0, h = 0; char name[17] = ""; bool slider = false; };
bool MemoryMenuItems(std::vector<MenuItem>& out, bool* charSelect = nullptr);
bool MemoryLiveWindowClass(const char* cls);       // finestra viva di questa classe (0.20.4)
bool MemoryNetButton(const char* name, RECT& r);      // pulsante di nome dato dentro una finestra viva
void MemoryNetDump();   // F9: pulsanti e testi visibili di qualunque schermata (0.20.3)
bool MemorySaveScroll(RECT& up, RECT& down);
bool MemoryHorseIcon(LONG& x, LONG& y);        // icona del cavallo visibile (l'eroe ha un cavallo), centro
bool MemoryPurchaseButtons(int& owner, RECT& yes, RECT& no);   // 0.56.54: conferma dell'acquisto del mercante aperta: "Si'" e "No"
int  MemoryConfirmDlg(RECT* btns, int maxBtns);   // pulsanti della conferma visibile (cUI_BusyDlg), da sinistra   // frecce della lista dei salvataggi (client)   // charSelect: scelta del personaggio aperta
bool MemoryGridCell(const GridInfo& g, int idx, int& anchor, int& w, int& h);   // finestra n. index del cUI_Manager visibile (4 = mercante)
void MemoryWindowDump(int index, const char* why);       // diagnostica: elementi cUI_ dentro la finestra
// Schede del mercante: indice della scheda attiva, numero di schede e rettangoli (client) dei pulsanti.
bool MemoryMerchantTabs(int& active, int& count, RECT* rects, int maxRects);
// Dialogo con un NPC aperto: rettangolo e pulsanti (al massimo 4, coordinate client).
struct DialogInfo { int x = 0, y = 0, w = 0, h = 0, buttons = 0; RECT btn[4] = {}; DWORD addr = 0; bool unverified = false; };
bool MemoryDialog(DialogInfo& d);
// 0.55.27: fino a until (GetTickCount) vale anche una finestra mai vista nascosta (schermata "Il sogno" gia' aperta alla
// ricerca dopo il caricamento); se dopo A o B (MemoryDialogActed) resta aperta e' una copia vecchia e si scarta.
void MemoryDialogTrustNew(DWORD until);
void MemoryDialogActed(DWORD addr);
void MemoryDialogDump();
void MemoryDialogWatch();
void MemoryDialogReset();
// Finestra di scelta dei portali (cUI_BusyDlg "DEFAULT_BUSYDLG") visibile; cancel = pulsante Annulla.
bool MemoryBusyDlg(RECT* cancel = nullptr);
void MemoryBusyReset();
void MemoryDialogRescan(bool quick);
void MemoryDialogScanStep();   // 0.55.28: ricerca completa a fette (24 MB per chiamata) mentre si aspetta un dialogo
// cls: tipo della creatura, oppure -1 per un oggetto (forziere, oggetto a terra); kind: nome breve per il log;
// vt: vtable dell'oggetto; name: classe RTTI degli oggetti (e +0x64 dei forzieri), per il log
struct Target { DWORD addr = 0, id = 0, vt = 0; int cls = 0; float sx = 0, sy = 0; const char* kind = ""; char name[40] = ""; };
bool MemoryFindTarget(bool talk, float ox, float oy, float dx, float dy, float range, float nearRadius, Target& out,
                      const DWORD* skip = nullptr, int nSkip = 0, int follower = 0, bool aim = false,
                      bool forHero2 = false,    // 0.55.0: per l'eroe 2 niente oggetti a terra (sono di B) ne' cavalli
                      bool allNpc = false);     // 0.56.18: anche i PNG generici (scelta voluta: mirino, cursore del pad 2)
bool MemoryTrackTarget(Target& t);
const char* HeroClassName(int cls);   // 0.55.43: nome della classe (1 Serafina ... 9 Diavola), anche per l'elenco Due giocatori
void MemoryMarkFixedUsed(DWORD id);   // 0.55.38: oggetto fisso della mappa appena scelto con A: 6 s fuori dai candidati
bool MemoryPickDescribe(LONG x, LONG y, char* out, size_t size);   // 0.55.38: cio' che il gioco sceglie sotto il punto (log)
bool MemoryTargetValid(Target& t);
bool MemoryTypeName(DWORD addr, char* out, size_t size);   // nome del tipo (+0x10), per il log     // visibile e ancora da usare (bersaglio a fuoco, 0.11.8)
bool MemoryHeroPos(int& x, int& y);
bool MemoryCameraHeroOffset(float& fx, float& fy);   // 0.49.2: scarto dell'eroe dal centro con la camera sul punto medio
bool MemoryCameraHeroScreen(float& sx, float& sy);   // 0.53.4: piedi dell'eroe sullo schermo (rettangoli del gioco) con la camera sul punto medio
bool MemoryHeroDestDir(float& dx, float& dy, int& hx, int& hy, int& tx, int& ty);   // 0.53.8: direzione della destinazione dell'eroe (diagnostica)
bool MemoryHeroHealth(float& frac, double& gameTime);   // vita/vita massima e tempo del lampeggio
bool MemoryHeroDistance(const Target& t, float& d);   // distanza nel mondo fra l'eroe e il bersaglio
bool MemoryIsChest(const Target& t);
void MemoryMarkUsed(const Target& t);                 // forziere aperto: A non lo sceglie piu'
void MemoryUsedReset();                               // partita caricata
bool MemoryPointOnCreature(float px, float py, float hf);
bool MemoryDoorAt(LONG x, LONG y, DWORD& id);         // porta che il clic sceglierebbe nel punto
bool MemoryDoorState(DWORD id, DWORD& state);        // +0x64 della porta (0, 0xAA aperta, 0xAB chiusa)
bool MemoryHoverObject(DWORD& id);
bool MemoryPickObject(LONG x, LONG y, DWORD& id);     // oggetto (eroe escluso) che il clic sceglierebbe nel punto                    // oggetto scelto dal gioco sotto il puntatore (eroe escluso)
// Punto (client) dove il gioco sceglierebbe proprio l'oggetto id (lista dei rettangoli del gioco).
bool MemoryPickPoint(DWORD id, LONG& x, LONG& y);
bool MemoryTargetRect(DWORD id, RECT& r);   // rettangolo dell'oggetto nella lista del gioco

// --- Overlay.cpp: scritte sopra il gioco con il font di Sacred (fase 3) ----------
void OverlayTick(DWORD now);              // aggancia EndScene del device quando esiste
void OverlaySetCursorMode(bool on);       // modalita' mostrata nella scritta
void OverlaySetCursorPlayer2(bool on);    // 0.50.0: il puntatore e' del giocatore 2 (scritta "cursore del giocatore 2")
void OverlaySetHelpOwner(int player);     // 0.51.3: aiuto aperto dal giocatore 1 o 2 (disegno con il suo controller)
void OverlaySetMarker(bool show, LONG x, LONG y, int icon = 0);   // icon: 0 azione, 1 dialogo, 2 cavallo
void OverlaySetMarker2(bool show, LONG x, LONG y, int icon = 0);
void OverlaySetMarkerGround(int who, bool show, LONG x, LONG y);   // 0.56.18: punto a terra del bersaglio (alone)
void OverlaySetCursor2Num(bool on, LONG x, LONG y);   // 0.56.1: "2" accanto al cursore del giocatore 2 nel mondo
void OverlaySetPointers(bool on, bool p1Show, LONG x1, LONG y1, LONG x2, LONG y2, int owner);   // 0.55.54: due puntatori  // 0.55.2: bersaglio di A dell'eroe 2 (con un "2" accanto)
void OverlaySetTwoPlayers(bool on);       // 0.55.4: c'e' l'eroe 2: anche l'icona del pad 1 ha il suo numero ("1")
void OverlaySetGrid(int index, bool show, LONG x, LONG y, int cols, int rows);
void OverlaySetSlotLabels(int count, const POINT* centers);   // simboli dei tasti sopra gli slot (0 = spenti)
void OverlaySetSlotLabels2(int count, const POINT* centers);  // 0.55.27: gli stessi sulla barra dell'eroe 2 (pad 2), dal suo disegno
void OverlaySetPotionLabels(int who, int count, const POINT* centers);   // 0.56.23: tasti delle pozioni (cura, antidoto, concentrazione)
void OverlaySetHelp(bool open);
void OverlaySetHelpPage(int page);                             // 0.55.68: pagina dell'aiuto (0 in gioco, 1 cursore e zaino, 2 due giocatori)
void OverlaySetHelpButtons(WORD buttons);                       // 0.55.68: tasti premuti dal giocatore dell'aiuto (si accendono)
void OverlaySetHelpRow(int row);                               // riga scelta nell'aiuto: 0 controller, 1 vibrazione                                // schermata di aiuto (Start tenuto)
void OverlaySetEscItem(bool on, const RECT& r, bool hover, DWORD font, DWORD color, DWORD hoverColor);
void OverlaySetMainLabel(const char* literal);   // testo della riga del menu principale (stringa statica)
int OverlayPickRows();   // 0.55.52: righe che l'elenco Due giocatori mostra a questa risoluzione
void OverlayCoopPicker(bool open, const std::vector<std::string>& lines, int sel, int top, int hover);
void OverlayCoopStatus(const char* text);
void OverlayCoopToast(const char* text, DWORD ms);
void OverlayHero2(bool show, const char* name, int level, int hp, int maxHp, int revivePct);   // barra dell'eroe 2 (revivePct < 0: nessun rialzo in corso)
int  OverlayCoopPickerHit(int x, int y, int cw, int ch, int n, int top);
void OverlaySetMainRow(bool on, const RECT& r, bool hover, DWORD font, DWORD color, DWORD hoverColor);   // "Co-op locale" nel menu principale   // voce "Controller"
const char* PadLayoutName(int layout);                         // "Steam", "Xbox", "DualSense"   // verifica delle griglie (0, 1)   // "( A )" sopra il bersaglio a fuoco (client)

// --- Core.cpp: ciclo principale ------------------------------------------------
extern std::atomic<HWND> g_gameHwnd;
DWORD WINAPI CoreThread(LPVOID);
