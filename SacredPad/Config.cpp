// Config.cpp - lettura di SacredPad.ini dalla cartella del gioco
// Linguaggio: C++17, MSVC, Win32 (x86).
// Se il file o una voce mancano si usano i valori predefiniti di Config.

#include "SacredPad.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

Config g_cfg;

namespace {

char g_ini[MAX_PATH] = {};

void ReadStr(const char* sec, const char* key, const char* def, char* out, DWORD size)
{
    GetPrivateProfileStringA(sec, key, def, out, size, g_ini);
    // Toglie un eventuale commento in linea ("valore ; commento") e gli spazi finali.
    for (char* p = out; *p; ++p) if (*p == ';') { *p = '\0'; break; }
    for (size_t n = strlen(out); n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'); --n) out[n - 1] = '\0';
}

int ReadInt(const char* sec, const char* key, int def)
{
    char buf[64], d[32];
    _snprintf_s(d, sizeof(d), _TRUNCATE, "%d", def);
    ReadStr(sec, key, d, buf, sizeof(buf));
    return atoi(buf);
}

float ReadFloat(const char* sec, const char* key, float def)
{
    char buf[64], d[32];
    _snprintf_s(d, sizeof(d), _TRUNCATE, "%g", def);
    ReadStr(sec, key, d, buf, sizeof(buf));
    return static_cast<float>(atof(buf));   // il punto e' il separatore decimale
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int   Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Elenco di numeri separati da virgole ("2,12") -> maschera di bit (valori 0..31).
unsigned ReadMask(const char* sec, const char* key, unsigned def)
{
    char buf[128];
    ReadStr(sec, key, "", buf, sizeof(buf));
    if (!buf[0]) return def;
    unsigned m = 0;
    for (char* p = buf; *p; ) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        int v = atoi(p);
        if (v >= 0 && v < 32) m |= 1u << v;
        while (*p && *p != ',') ++p;
    }
    return m;
}

} // namespace

void SaveConfigValue(const char* section, const char* key, const char* value)
{
    if (g_ini[0] && WritePrivateProfileStringA(section, key, value, g_ini)) Log("Ini: [%s] %s=%s salvato", section, key, value);
}

void LoadConfig()
{
    _snprintf_s(g_ini, sizeof(g_ini), _TRUNCATE, "%s\\SacredPad.ini", GameDir());
    bool exists = GetFileAttributesA(g_ini) != INVALID_FILE_ATTRIBUTES;

    Config d;   // valori predefiniti
    char src[32];
    ReadStr("General", "Language", "Auto", g_cfg.language, sizeof(g_cfg.language));   // 0.48.0
    ReadStr("General", "PadSource", "Auto", src, sizeof(src));
    if      (_stricmp(src, "XInput")   == 0) g_cfg.padSource = PadSource::XInput;
    else if (_stricmp(src, "Keyboard") == 0) g_cfg.padSource = PadSource::Keyboard;
    else if (_stricmp(src, "Laptop")   == 0) g_cfg.padSource = PadSource::Laptop;
    else if (_stricmp(src, "None") == 0 || _stricmp(src, "Mouse") == 0) g_cfg.padSource = PadSource::None;   // 0.55.14
    else                                     g_cfg.padSource = PadSource::Auto;

    g_cfg.controllerIndex = ReadInt("General", "ControllerIndex", d.controllerIndex);
    g_cfg.tickMs          = ReadInt("General", "TickMs", d.tickMs);
    g_cfg.leftDeadzone    = ReadFloat("Sticks", "LeftDeadzone", d.leftDeadzone);
    g_cfg.moveRadius      = ReadFloat("Sticks", "MoveRadius", d.moveRadius);
    g_cfg.centerOffsetX   = ReadFloat("Screen", "CenterOffsetX", d.centerOffsetX);
    g_cfg.centerOffsetY   = ReadFloat("Screen", "CenterOffsetY", d.centerOffsetY);
    char aim[32];
    ReadStr("Aim", "AimMode", "Melee", aim, sizeof(aim));
    g_cfg.aimMode         = (_stricmp(aim, "Ranged") == 0) ? AimMode::Ranged : AimMode::Melee;
    g_cfg.rightDeadzone   = ReadFloat("Aim", "RightDeadzone", d.rightDeadzone);
    g_cfg.aimMinRadius    = ReadFloat("Aim", "AimMinRadius", d.aimMinRadius);
    g_cfg.aimRadius       = ReadFloat("Aim", "AimRadius", d.aimRadius);
    g_cfg.rangedDefault   = ReadFloat("Aim", "RangedDefaultRadius", d.rangedDefault);
    g_cfg.reclickDistance = ReadFloat("Aim", "ReclickDistance", d.reclickDistance);
    g_cfg.artClickDelayMs = ReadInt("Keys", "ArtClickDelayMs", d.artClickDelayMs);
    // UnlockedSlots sostituisce WeaponSlots (versione 0.5.1 e precedenti), letto se manca.
    g_cfg.unlockedSlots   = ReadInt("Keys", "UnlockedSlots", ReadInt("Keys", "WeaponSlots", d.unlockedSlots));
    g_cfg.artHoldMs       = ReadInt("Keys", "ArtHoldMs", d.artHoldMs);
    g_cfg.artMoveLockMs   = ReadInt("Keys", "ArtMoveLockMs", d.artMoveLockMs);
    g_cfg.holdReclickMs   = ReadInt("Aim", "HoldReclickMs", d.holdReclickMs);
    char mm[32];
    ReadStr("Keys", "MinimapMethod", "Tab", mm, sizeof(mm));
    g_cfg.minimapMethod   = (_stricmp(mm, "Middle") == 0) ? 1 : 0;
    g_cfg.minimapHoldMs   = ReadInt("Keys", "MinimapHoldMs", d.minimapHoldMs);
    g_cfg.zoomHoldMs      = Clamp(ReadInt("Keys", "ZoomHoldMs", d.zoomHoldMs), 0, 2000);   // 0.55.31
    {   // 0.56.38: portali attraversati camminando
        char ap[16];
        ReadStr("Keys", "AutoPortal", d.autoPortal ? "true" : "false", ap, sizeof(ap));
        g_cfg.autoPortal = (_stricmp(ap, "false") != 0 && strcmp(ap, "0") != 0);
    }
    {   // 0.56.25: pozioni di croce su / giu', con la lettera del gioco (Q, W, E, R) o il tipo (2..5)
        static const char* const kName[2] = { "DpadUpPotion", "DpadDownPotion" };
        for (int k = 0; k < 2; ++k) {
            char pv[16];
            ReadStr("Keys", kName[k], k == 0 ? "R" : "W", pv, sizeof(pv));
            char c = pv[0];
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            g_cfg.dpadPotion[k] = c == 'Q' ? 2 : c == 'W' ? 3 : c == 'E' ? 4 : c == 'R' ? 5 : (c >= '2' && c <= '5') ? c - '0' : d.dpadPotion[k];
        }
    }
    {
        char zi[16];
        ReadStr("Keys", "ZoomInvert", d.zoomInvert ? "true" : "false", zi, sizeof(zi));
        g_cfg.zoomInvert = (_stricmp(zi, "true") == 0 || strcmp(zi, "1") == 0);
    }
    ReadStr("Keys", "MinimapMode", "Toggle", mm, sizeof(mm));
    g_cfg.minimapToggle   = (_stricmp(mm, "Hold") != 0);
    g_cfg.cursorSpeed     = ReadFloat("Cursor", "CursorSpeed", d.cursorSpeed);
    g_cfg.cursorFine      = ReadFloat("Cursor", "CursorFine", d.cursorFine);
    g_cfg.cursorCurve     = ReadFloat("Cursor", "CursorCurve", d.cursorCurve);
    g_cfg.cursorNudge     = ReadFloat("Cursor", "CursorNudge", d.cursorNudge);
    g_cfg.viewHoldMs      = ReadInt("Cursor", "ViewHoldMs", d.viewHoldMs);
    char sc[16];
    ReadStr("Cursor", "StartInCursor", d.startInCursor ? "true" : "false", sc, sizeof(sc));
    g_cfg.startInCursor   = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Memory", "MemoryEnabled", d.memoryEnabled ? "true" : "false", sc, sizeof(sc));
    g_cfg.memoryEnabled   = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Memory", "MemoryDiag", d.memoryDiag ? "true" : "false", sc, sizeof(sc));
    g_cfg.memoryDiag      = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.memoryDiagMs    = ReadInt("Memory", "MemoryDiagMs", d.memoryDiagMs);
    ReadStr("Memory", "WarnWrongExe", d.warnWrongExe ? "true" : "false", sc, sizeof(sc));
    g_cfg.warnWrongExe    = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    if (g_cfg.memoryDiagMs < 500 || g_cfg.memoryDiagMs > 60000) g_cfg.memoryDiagMs = d.memoryDiagMs;
    // 0.49.0 (N6): [Native] Enabled=false spegne tutta la co-op nativa (menu, eroe 2, ganci, salvataggi, raccolta, scambio):
    // la mod torna quella del ramo main 0.21 (voce "Co-op locale" a schermo diviso).
    ReadStr("Native", "Enabled", d.nativeEnabled ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeEnabled   = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "AutoSpawn", d.nativeAuto ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeAuto      = (_stricmp(sc, "true") == 0 || strcmp(sc, "1") == 0);
    g_cfg.nativeHero2     = Clamp(ReadInt("Native", "Hero2File", d.nativeHero2), 0, 99);
    ReadStr("Native", "ServerProbe", "false", sc, sizeof(sc));
    g_cfg.nativeServerProbe = (_stricmp(sc, "true") == 0 || strcmp(sc, "1") == 0);
    ReadStr("Native", "HeapCheck", "false", sc, sizeof(sc));   // 0.55.24: diagnostica dello heap
    g_cfg.nativeHeapCheck = (_stricmp(sc, "true") == 0 || strcmp(sc, "1") == 0);
    ReadStr("Native", "PlayerList", "false", sc, sizeof(sc));
    g_cfg.nativePlayers = (_stricmp(sc, "true") == 0 || strcmp(sc, "1") == 0);
    ReadStr("Native", "MonsterWatch", "false", sc, sizeof(sc));
    g_cfg.nativeMonsterWatch = (_stricmp(sc, "true") == 0 || strcmp(sc, "1") == 0);
    ReadStr("Native", "DiagWatch", "false", sc, sizeof(sc));
    g_cfg.nativeDiagWatch = (_stricmp(sc, "true") == 0 || strcmp(sc, "1") == 0);
    g_cfg.nativeRevive = Clamp(ReadInt("Native", "ReviveSeconds", d.nativeRevive), 0, 300);
    g_cfg.nativeReviveNear = Clamp(ReadInt("Native", "ReviveNearSeconds", d.nativeReviveNear), 0, 30);
    g_cfg.nativeReviveRange = Clamp(ReadInt("Native", "ReviveRange", d.nativeReviveRange), 100, 1000);
    g_cfg.nativeXpRange = Clamp(ReadInt("Native", "ShareXpRange", d.nativeXpRange), 0, 10000);
    ReadStr("Native", "Hero2Bar", d.nativeHero2Bar ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeHero2Bar  = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Hero2Taskbar", d.nativeHero2Taskbar ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeHero2Taskbar = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.nativeHero2TaskbarDx = Clamp(ReadInt("Native", "Hero2TaskbarOffset", d.nativeHero2TaskbarDx), -3000, 3000);
    ReadStr("Native", "Hero1TaskbarLeft", d.nativeHero1TaskbarLeft ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeHero1TaskbarLeft = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "DirectMove", d.nativeDirectMove ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeDirectMove = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Pad2", d.nativePad2 ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativePad2      = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.nativePad2Index = Clamp(ReadInt("Native", "Pad2Index", d.nativePad2Index), -1, 3);
    ReadStr("Native", "Pad2Keyboard", d.nativePad2Keyboard ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativePad2Keyboard = (_stricmp(sc, "true") == 0 || strcmp(sc, "1") == 0);
    ReadStr("Native", "Pad2WalkToUse", d.nativePad2WalkToUse ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativePad2WalkToUse = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Pad2NoCtrl", d.nativePad2NoCtrl ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativePad2NoCtrl = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Hero2QuestItems", d.nativeQuestItems ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeQuestItems = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "SharedLoot", d.nativeSharedLoot ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeSharedLoot = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "KeepMap", d.nativeKeepMap ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeKeepMap = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "RevealHero2", d.nativeRevealHero2 ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeRevealHero2 = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "SharedGold", d.nativeSharedGold ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeSharedGold = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "ArtOnHero2", d.nativeArtOnHero2 ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeArtOnHero2 = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "RewardRunes", d.nativeRewardRunes ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeRewardRunes = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "ChestRunes", d.nativeChestRunes ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeChestRunes = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "RewardPoints", d.nativeRewardPoints ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeRewardPoints = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "TwoInventories", d.nativeTwoInventories ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeTwoInventories = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "CompanionView", d.nativeCompanionView ? "true" : "false", sc, sizeof(sc));   // 0.56.28
    g_cfg.nativeCompanionView = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "CompanionXp", d.nativeCompanionXp ? "true" : "false", sc, sizeof(sc));   // 0.56.31
    g_cfg.nativeCompanionXp = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "CompanionAnyClass", d.nativeCompanionAnyClass ? "true" : "false", sc, sizeof(sc));   // 0.56.34
    g_cfg.nativeCompanionAnyClass = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Mouse2", d.nativeMouse2 ? "true" : "false", sc, sizeof(sc));   // 0.56.0
    g_cfg.nativeMouse2 = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "AudioBothHeroes", d.nativeAudioBoth ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeAudioBoth = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Pad2EdgeLimit", d.nativePad2EdgeLimit ? "true" : "false", sc, sizeof(sc));   // 0.55.30
    g_cfg.nativePad2EdgeLimit = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.nativePad2Dead  = ReadFloat("Native", "Pad2Deadzone", d.nativePad2Dead);
    if (g_cfg.nativePad2Dead < 0.1f || g_cfg.nativePad2Dead > 0.9f) g_cfg.nativePad2Dead = d.nativePad2Dead;
    ReadStr("Native", "SaveHero2", d.nativeSaveHero2 ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeSaveHero2 = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "CleanSavedHero2", d.nativeCleanSaved ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeCleanSaved = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Hero2Portrait", d.nativePortrait ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativePortrait  = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Hero2Name", d.nativeHero2Name ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeHero2Name = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "ShareXp", d.nativeShareXp ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeShareXp = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.nativeXpFactor = Clamp(ReadInt("Native", "XpFactor", d.nativeXpFactor), 1, 100);
    ReadStr("Native", "SwapHero", d.nativeSwap ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeSwap = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "SwapUi", d.nativeSwapUi ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeSwapUi = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Menu", d.nativeMenu ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeMenu = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    // 0.55.27: acceso se manca (prova del 31/10 sulla Steam Machine: ini senza [Native], nemici che ignoravano l'eroe 2)
    ReadStr("Native", "Aggro", d.nativeAggro ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeAggro = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.nativeAggroRange = Clamp(ReadInt("Native", "AggroRange", d.nativeAggroRange), 200, 2000);
    ReadStr("Native", "Camera", d.nativeCamera ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeCamera    = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "Verbose", d.nativeVerbose ? "true" : "false", sc, sizeof(sc));
    g_cfg.nativeVerbose   = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Lock", "LockEnabled", d.lockEnabled ? "true" : "false", sc, sizeof(sc));
    g_cfg.lockEnabled     = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Pad", "Vibration", d.vibration ? "true" : "false", sc, sizeof(sc));
    g_cfg.vibration       = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Overlay", "PlayerMarks", d.playerMarks ? "true" : "false", sc, sizeof(sc));   // 0.56.16
    g_cfg.playerMarks     = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Native", "ShareHeal", d.nativeShareHeal ? "true" : "false", sc, sizeof(sc));  // 0.56.16
    g_cfg.nativeShareHeal = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.vibrationPower  = ReadInt("Pad", "VibrationPower", d.vibrationPower);
    if (g_cfg.vibrationPower < 0) g_cfg.vibrationPower = 0;
    if (g_cfg.vibrationPower > 100) g_cfg.vibrationPower = 100;
    ReadStr("Overlay", "GridMarkers", d.gridMarkers ? "true" : "false", sc, sizeof(sc));
    g_cfg.gridMarkers     = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Overlay", "PadLayout", "Steam", sc, sizeof(sc));
    g_cfg.padLayout       = _stricmp(sc, "Xbox") == 0 ? 1 : (_stricmp(sc, "DualSense") == 0 ? 2 : 0);
    {   // 0.51.3: controller del giocatore 2, se manca come quello del giocatore 1
        char s2[32];
        ReadStr("Overlay", "Pad2Layout", sc, s2, sizeof(s2));
        g_cfg.pad2Layout = _stricmp(s2, "Xbox") == 0 ? 1 : (_stricmp(s2, "DualSense") == 0 ? 2 : 0);
    }
    ReadStr("Overlay", "Enabled", d.overlayEnabled ? "true" : "false", sc, sizeof(sc));
    g_cfg.overlayEnabled  = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Overlay", "ShowVersion", "true", sc, sizeof(sc));   // 0.56.58: true, Menu o false
    g_cfg.showVersion     = _stricmp(sc, "menu") == 0 ? 1 : (_stricmp(sc, "false") == 0 || strcmp(sc, "0") == 0) ? 0 : 2;
    ReadStr("Lock", "PreviewTarget", d.previewTarget ? "true" : "false", sc, sizeof(sc));
    g_cfg.previewTarget   = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.lockRange       = Clamp(ReadFloat("Lock", "LockRange", d.lockRange), 0.05f, 0.9f);
    g_cfg.lockAngle       = Clamp(ReadFloat("Lock", "LockAngle", d.lockAngle), 5.0f, 90.0f);
    g_cfg.lockNear        = Clamp(ReadFloat("Lock", "LockNear", d.lockNear), 0.0f, 0.5f);
    g_cfg.lockOffsetY     = Clamp(ReadFloat("Lock", "LockOffsetY", d.lockOffsetY), -0.2f, 0.2f);
    g_cfg.talkRange       = Clamp(ReadFloat("Lock", "TalkRange", d.talkRange), 0.0f, 0.6f);
    g_cfg.talkCycleMs     = Clamp(ReadInt("Lock", "TalkCycleMs", d.talkCycleMs), 0, 10000);
    // Ostili: l'elenco HostileTypes se presente, altrimenti tutti i tipi non elencati in NeutralTypes.
    g_cfg.neutralMask     = ReadMask("Lock", "NeutralTypes", d.neutralMask);
    g_cfg.hostileMask     = ReadMask("Lock", "HostileTypes", ~g_cfg.neutralMask);
    ReadStr("General", "KeyLog", d.keyLog ? "true" : "false", sc, sizeof(sc));
    g_cfg.keyLog          = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Cursor", "AutoCursor", d.autoCursor ? "true" : "false", sc, sizeof(sc));
    g_cfg.autoCursor      = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Sticks", "Unstick", d.unstick ? "true" : "false", sc, sizeof(sc));
    g_cfg.unstick         = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    g_cfg.talkMask        = ReadMask("Lock", "TalkTypes", d.talkMask);

    // Co-op locale (0.19.0)
    char co[MAX_PATH];
    ReadStr("Coop", "Role", "Off", co, sizeof(co));
    // La variabile d'ambiente SACREDPAD_COOP (impostata dalla sessione co-op) vale piu' dell'ini.
    char envRole[16] = "";
    if (GetEnvironmentVariableA("SACREDPAD_COOP", envRole, sizeof(envRole)) > 0) strcpy_s(co, envRole);
    g_cfg.coopRole        = _stricmp(co, "Host") == 0 ? 1 : (_stricmp(co, "Guest") == 0 ? 2 : 0);
    // Nelle due istanze della co-op a schermo diviso (Role Host o Guest, anche dalla variabile SACREDPAD_COOP) la co-op
    // nativa non serve e non deve partire (voce del menu, eroe 2, ganci): ciascuna finestra ha il suo eroe.
    if (g_cfg.coopRole != 0) g_cfg.nativeEnabled = false;
    if (!g_cfg.nativeEnabled) {   // tutto il blocco nativo spento, comprese le prove
        g_cfg.nativeMenu = g_cfg.nativeAuto = g_cfg.nativeServerProbe = g_cfg.nativePlayers = false;
        g_cfg.nativeMonsterWatch = g_cfg.nativeDiagWatch = g_cfg.nativePad2 = g_cfg.nativeSwap = false;
        g_cfg.nativeShareXp = g_cfg.nativeAggro = g_cfg.nativeCamera = g_cfg.nativeSaveHero2 = g_cfg.nativePortrait = false;
        g_cfg.nativeHero2Name = g_cfg.nativeHero2Bar = g_cfg.nativeVerbose = false;
        g_cfg.nativeHero2Taskbar = false;
        g_cfg.nativeTwoInventories = false;
        g_cfg.nativeCompanionView = false;
        g_cfg.nativeCompanionXp = false;
        g_cfg.nativeCompanionAnyClass = false;
        g_cfg.nativeMouse2 = false;
    }
    ReadStr("Coop", "Nav", "true", sc, sizeof(sc));
    g_cfg.coopNav         = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Coop", "Enabled", "true", sc, sizeof(sc));
    g_cfg.coopEnabled     = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Coop", "IgnoreMouse", g_cfg.coopRole ? "true" : "false", sc, sizeof(sc));
    g_cfg.coopIgnoreMouse = g_cfg.coopRole && (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);   // solo in sessione co-op
    ReadStr("Coop", "Borderless", "true", sc, sizeof(sc));
    g_cfg.coopBorderless  = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Coop", "WindowRect", "", co, sizeof(co));
    sscanf_s(co, "%d,%d,%d,%d", &g_cfg.coopX, &g_cfg.coopY, &g_cfg.coopW, &g_cfg.coopH);
    ReadStr("Coop", "GuestRect", "", co, sizeof(co));
    sscanf_s(co, "%d,%d,%d,%d", &g_cfg.coopGuestX, &g_cfg.coopGuestY, &g_cfg.coopGuestW, &g_cfg.coopGuestH);
    // Disposizione automatica (0.20.5): due finestre WindowSize affiancate e centrate sullo schermo; se non ci stanno
    // si mettono ai due bordi (sovrapposte). Layout=Manual usa WindowRect e GuestRect.
    ReadStr("Coop", "Layout", "Auto", co, sizeof(co));
    if (_stricmp(co, "Manual") != 0) {
        int ww = 1280, hh = 720;
        ReadStr("Coop", "WindowSize", "1280x720", co, sizeof(co));
        sscanf_s(co, "%dx%d", &ww, &hh);
        int gap = Clamp(ReadInt("Coop", "Gap", 40), 0, 400);
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        // 0.49.3: dentro il desktop virtuale di Wine (host e ospite) lo schermo e' il desktop, grande quanto lo schermo
        // vero letto dal gioco normale all'avvio; il gioco normale passa la misura con SACREDPAD_SCREEN.
        char scr[32] = "";
        if (g_cfg.coopRole != 0 && GetEnvironmentVariableA("SACREDPAD_SCREEN", scr, sizeof(scr)) > 0) {
            int ew = 0, eh = 0;
            if (sscanf_s(scr, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) { sw = ew; sh = eh; }
        }
        g_cfg.coopScreenW = sw; g_cfg.coopScreenH = sh;
        const bool fits = 2 * ww <= sw;
        if (fits && 2 * ww + gap > sw) gap = sw - 2 * ww;      // lo spazio fra le finestre si riduce, ma restano affiancate
        const int hx = fits ? (sw - 2 * ww - gap) / 2 : 0;
        const int gx = fits ? hx + ww + gap : sw - ww;
        const int y  = (sh - hh) / 2 > 0 ? (sh - hh) / 2 : 0;
        g_cfg.coopX = hx; g_cfg.coopY = y; g_cfg.coopW = ww; g_cfg.coopH = hh;
        g_cfg.coopGuestX = gx; g_cfg.coopGuestY = y; g_cfg.coopGuestW = ww; g_cfg.coopGuestH = hh;
        if (g_cfg.coopRole == 2) { g_cfg.coopX = gx; }
        Log("  Coop: schermo %dx%d, finestre %dx%d %s", sw, sh, ww, hh, fits ? "affiancate" : "ai bordi, sovrapposte");
    }
    ReadStr("Coop", "AfterSession", "Quit", sc, sizeof(sc));
    g_cfg.coopReturnNormal = (_stricmp(sc, "Normal") == 0);
    ReadStr("Coop", "WineDesktop", "Auto", sc, sizeof(sc));
    g_cfg.coopWineDesktop = _stricmp(sc, "Off") == 0 ? 0 : (_stricmp(sc, "On") == 0 ? 1 : 2);
    ReadStr("Coop", "HookWindows", "true", sc, sizeof(sc));
    g_cfg.coopHookWindows = (_stricmp(sc, "false") != 0 && strcmp(sc, "0") != 0);
    ReadStr("Coop", "Backdrop", "false", sc, sizeof(sc));
    g_cfg.coopBackdrop    = (_stricmp(sc, "true") == 0 || strcmp(sc, "1") == 0);
    g_cfg.coopGuestPad    = Clamp(ReadInt("Coop", "GuestControllerIndex", d.coopGuestPad), 0, 3);
    g_cfg.coopGuestMusic  = Clamp(ReadInt("Coop", "GuestMusicVolume", d.coopGuestMusic), -1, 200);
    ReadStr("Coop", "LaunchKey", "F8", co, sizeof(co));
    g_cfg.coopLaunchVk    = (co[0] == 'F' || co[0] == 'f') && atoi(co + 1) >= 1 && atoi(co + 1) <= 12 ? VK_F1 + atoi(co + 1) - 1 : 0;
    char defDir[MAX_PATH];
    _snprintf_s(defDir, sizeof(defDir), _TRUNCATE, "%s P2", GameDir());
    ReadStr("Coop", "GuestDir", defDir, g_cfg.coopGuestDir, sizeof(g_cfg.coopGuestDir));

    // Limiti di sicurezza contro valori sbagliati nell'ini.
    if (g_cfg.controllerIndex < 0 || g_cfg.controllerIndex > 3) g_cfg.controllerIndex = 0;
    if (g_cfg.tickMs < 2 || g_cfg.tickMs > 50) g_cfg.tickMs = d.tickMs;
    g_cfg.leftDeadzone  = Clamp(g_cfg.leftDeadzone, 0.05f, 0.9f);
    g_cfg.moveRadius    = Clamp(g_cfg.moveRadius, 0.03f, 0.45f);
    g_cfg.centerOffsetX = Clamp(g_cfg.centerOffsetX, -0.4f, 0.4f);
    g_cfg.centerOffsetY = Clamp(g_cfg.centerOffsetY, -0.4f, 0.4f);
    g_cfg.rightDeadzone = Clamp(g_cfg.rightDeadzone, 0.05f, 0.9f);
    g_cfg.aimRadius     = Clamp(g_cfg.aimRadius, 0.05f, 0.6f);
    g_cfg.aimMinRadius  = Clamp(g_cfg.aimMinRadius, 0.0f, g_cfg.aimRadius);
    g_cfg.rangedDefault = Clamp(g_cfg.rangedDefault, 0.05f, 0.6f);
    g_cfg.reclickDistance = Clamp(g_cfg.reclickDistance, 0.01f, 0.5f);
    if (g_cfg.artClickDelayMs < 0 || g_cfg.artClickDelayMs > 500) g_cfg.artClickDelayMs = d.artClickDelayMs;
    if (g_cfg.unlockedSlots < 0 || g_cfg.unlockedSlots > 5) g_cfg.unlockedSlots = d.unlockedSlots;
    if (g_cfg.artHoldMs < 0 || g_cfg.artHoldMs > 500) g_cfg.artHoldMs = d.artHoldMs;
    if (g_cfg.artMoveLockMs < 0 || g_cfg.artMoveLockMs > 2000) g_cfg.artMoveLockMs = d.artMoveLockMs;
    if (g_cfg.holdReclickMs != 0 && (g_cfg.holdReclickMs < 150 || g_cfg.holdReclickMs > 5000)) g_cfg.holdReclickMs = d.holdReclickMs;
    if (g_cfg.minimapHoldMs < 20 || g_cfg.minimapHoldMs > 1000) g_cfg.minimapHoldMs = d.minimapHoldMs;
    g_cfg.cursorSpeed = Clamp(g_cfg.cursorSpeed, 0.1f, 5.0f);
    g_cfg.cursorFine  = Clamp(g_cfg.cursorFine, 0.05f, 1.0f);
    g_cfg.cursorCurve = Clamp(g_cfg.cursorCurve, 1.0f, 4.0f);
    g_cfg.cursorNudge = Clamp(g_cfg.cursorNudge, 0.001f, 0.1f);
    if (g_cfg.viewHoldMs < 100 || g_cfg.viewHoldMs > 2000) g_cfg.viewHoldMs = d.viewHoldMs;

    Log("Configurazione %s: %s", exists ? "letta da" : "predefinita (file assente)", g_ini);
    Log("  PadSource=%s ControllerIndex=%d TickMs=%d LeftDeadzone=%.2f MoveRadius=%.3f CenterOffset=(%.3f, %.3f)",
        g_cfg.padSource == PadSource::XInput ? "XInput" :
        g_cfg.padSource == PadSource::Keyboard ? "Keyboard" :
        g_cfg.padSource == PadSource::Laptop ? "Laptop" :
        g_cfg.padSource == PadSource::None ? "None" : "Auto",
        g_cfg.controllerIndex, g_cfg.tickMs, g_cfg.leftDeadzone, g_cfg.moveRadius,
        g_cfg.centerOffsetX, g_cfg.centerOffsetY);
    Log("  AimMode=%s RightDeadzone=%.2f AimMinRadius=%.3f AimRadius=%.3f RangedDefaultRadius=%.3f",
        g_cfg.aimMode == AimMode::Ranged ? "Ranged" : "Melee",
        g_cfg.rightDeadzone, g_cfg.aimMinRadius, g_cfg.aimRadius, g_cfg.rangedDefault);
    Log("  ReclickDistance=%.3f HoldReclickMs=%d ArtClickDelayMs=%d ArtHoldMs=%d ArtMoveLockMs=%d UnlockedSlots=%d",
        g_cfg.reclickDistance, g_cfg.holdReclickMs, g_cfg.artClickDelayMs, g_cfg.artHoldMs, g_cfg.artMoveLockMs,
        g_cfg.unlockedSlots);
    Log("  LockEnabled=%s LockRange=%.2f LockAngle=%.0f LockNear=%.2f LockOffsetY=%.3f TalkRange=%.2f TalkCycleMs=%d HostileTypes=0x%X TalkTypes=0x%X Unstick=%s",
        g_cfg.lockEnabled ? "true" : "false", g_cfg.lockRange, g_cfg.lockAngle, g_cfg.lockNear, g_cfg.lockOffsetY,
        g_cfg.talkRange, g_cfg.talkCycleMs, g_cfg.hostileMask, g_cfg.talkMask, g_cfg.unstick ? "true" : "false");
    Log("  PreviewTarget=%s", g_cfg.previewTarget ? "true" : "false");
    Log("  Overlay Enabled=%s", g_cfg.overlayEnabled ? "true" : "false");
    Log("  MinimapMethod=%s MinimapMode=%s MinimapHoldMs=%d ZoomHoldMs=%d ZoomInvert=%s DpadUpPotion=%d DpadDownPotion=%d",
        g_cfg.minimapMethod ? "Middle" : "Tab", g_cfg.minimapToggle ? "Toggle" : "Hold", g_cfg.minimapHoldMs,
        g_cfg.zoomHoldMs, g_cfg.zoomInvert ? "true" : "false", g_cfg.dpadPotion[0], g_cfg.dpadPotion[1]);
    if (g_cfg.coopRole)
        Log("  Coop: Role=%s IgnoreMouse=%s WindowRect=%d,%d,%d,%d GuestRect=%d,%d,%d,%d GuestPad=%d LaunchVk=0x%X GuestDir=%s",
            g_cfg.coopRole == 1 ? "Host" : "Guest", g_cfg.coopIgnoreMouse ? "true" : "false", g_cfg.coopX, g_cfg.coopY,
            g_cfg.coopW, g_cfg.coopH, g_cfg.coopGuestX, g_cfg.coopGuestY, g_cfg.coopGuestW, g_cfg.coopGuestH,
            g_cfg.coopGuestPad, g_cfg.coopLaunchVk, g_cfg.coopGuestDir);
    Log("  CursorSpeed=%.2f CursorFine=%.2f CursorCurve=%.1f CursorNudge=%.3f ViewHoldMs=%d StartInCursor=%s AutoCursor=%s",
        g_cfg.cursorSpeed, g_cfg.cursorFine, g_cfg.cursorCurve, g_cfg.cursorNudge, g_cfg.viewHoldMs,
        g_cfg.startInCursor ? "true" : "false", g_cfg.autoCursor ? "true" : "false");
}

// 0.56.25: tipi delle pozioni (tabella dei tasti di Sacred, 0x618a85): 1 medicamentosa (rossa, Spazio), 2 Morte dei Non-morti
// (viola scura, Q), 3 Mentore (blu, W), 4 antivipera (verde, E), 5 concentrazione (gialla, R)
const char* PotionName(int kind)
{
    switch (kind) {
    case 1: return "Pozione di cura";
    case 2: return "Non-morti";
    case 3: return "Mentore";
    case 4: return "Antidoto";
    default: return "Concentrazione";
    }
}
