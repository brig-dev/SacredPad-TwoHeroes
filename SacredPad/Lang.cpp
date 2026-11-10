// Lang.cpp - testi della mod nella lingua del gioco (0.48.0, N17)
// Linguaggio: C++17. Compilatore: MSVC, Win32 (x86).
//
// Ogni testo che il giocatore vede passa da Tr(testo italiano): se la lingua scelta non e' l'italiano e il testo e'
// nella tabella, si restituisce la traduzione, altrimenti il testo com'e' (un testo dimenticato resta in italiano,
// non rompe niente). Le traduzioni stanno nella codifica Windows-1252, quella del font del gioco per le lingue
// occidentali, scritte con escape \x (la tabella e' generata da uno script: le parti "..." "..." separano un escape
// dalle lettere che seguono). Gli specificatori % sono gli stessi, nello stesso ordine, in tutte le lingue.
// Il registro SacredPad.log resta in italiano.
//
// Lingua: [General] Language = Auto | it | en | de | fr | es. Auto: una riga di Settings.cfg del gioco con "LANG" nel
// nome, altrimenti la lingua dell'interfaccia di Windows; le lingue senza tabella usano l'inglese.
#include "SacredPad.h"
#include <cstdio>
#include <map>
#include <string>
#include <cctype>

namespace {
struct Row { const char* it; const char* en; const char* de; const char* fr; const char* es; };
const Row kRows[] = {
    { "copia pronta", "copy ready", "Kopie bereit", "copie pr\xEAte", "copia lista" },
    { "Copia del compagno reclutata con la sua scheda", "Companion copy recruited with its sheet", "Kopie des Gef\xE4hrten mit ihren Werten angeworben", "Copie du compagnon recrut\xE9" "e avec sa fiche", "Copia del compa\xF1" "ero reclutada con su ficha" },
    { "Copia congedata: i suoi oggetti sono nel tuo zaino", "Copy dismissed: its items are in your backpack", "Kopie entlassen: ihre Gegenst\xE4nde sind in deinem Rucksack", "Copie cong\xE9" "di\xE9" "e : ses objets sont dans ton sac", "Copia despedida: sus objetos est\xE1n en tu mochila" },
    { "Zaino pieno: alcuni oggetti della copia sono a terra", "Backpack full: some of the copy's items are on the ground", "Rucksack voll: einige Gegenst\xE4nde der Kopie liegen am Boden", "Sac plein : des objets de la copie sont au sol", "Mochila llena: algunos objetos de la copia est\xE1n en el suelo" },
    { "Il gruppo e' pieno", "The party is full", "Die Gruppe ist voll", "Le groupe est complet", "El grupo est\xE1 lleno" },
    { "E' gia' nel gruppo", "Already in the party", "Schon in der Gruppe", "D\xE9j\xE0 dans le groupe", "Ya est\xE1 en el grupo" },
    { "Copia del compagno reclutata", "Companion copy recruited", "Kopie des Gef\xE4hrten angeworben", "Copie du compagnon recrut\xE9" "e", "Copia del compa\xF1" "ero reclutada" },
    { "Copia del compagno congedata", "Companion copy dismissed", "Kopie des Gef\xE4hrten entlassen", "Copie du compagnon cong\xE9" "di\xE9" "e", "Copia del compa\xF1" "ero despedida" },
    { "Compagno congedato", "Companion dismissed", "Gef\xE4hrte entlassen", "Compagnon cong\xE9" "di\xE9", "Compa\xF1" "ero despedido" },
    { "Compagno", "Companion", "Gef\xE4hrte", "Compagnon", "Compa\xF1" "ero" },
    { "Livello", "Level", "Stufe", "Niveau", "Nivel" },
    { "copia, con te", "copy, with you", "Kopie, bei dir", "copie, avec toi", "copia, contigo" },
    { "con te", "with you", "bei dir", "avec toi", "contigo" },
    { "con te (storia)", "with you (story)", "bei dir (Geschichte)", "avec toi (histoire)", "contigo (historia)" },
    { "Questo compagno segue la storia: se ne andra' quando la missione lo lascia",
      "This companion follows the story: they will leave when the quest lets them go",
      "Dieser Gefaehrte folgt der Geschichte: er geht, wenn die Quest ihn entlaesst",
      "Ce compagnon suit l'histoire : il partira quand la quete le laissera partir",
      "Este companero sigue la historia: se ira cuando la mision lo deje" },
    { "nella storia, qui vicino", "in the story, nearby", "in der Geschichte, in der N\xE4he", "dans l'histoire, tout pr\xE8s", "en la historia, cerca" },
    { "nella storia, lontano", "in the story, far away", "in der Geschichte, weit weg", "dans l'histoire, loin", "en la historia, lejos" },
    { "nella storia, morto", "in the story, dead", "in der Geschichte, tot", "dans l'histoire, mort", "en la historia, muerto" },
    { "non e' in questa zona", "not in this area", "nicht in diesem Gebiet", "pas dans cette zone", "no est\xE1 en esta zona" },
    { "Congeda", "Dismiss", "Entlassen", "Cong\xE9" "dier", "Despedir" },
    { "Recluta una copia", "Recruit a copy", "Kopie anwerben", "Recruter une copie", "Reclutar una copia" },
    { "Compagni", "Companions", "Gef\xE4hrten", "Compagnons", "Compa\xF1" "eros" },
    { "Compagni conosciuti in questa avventura", "Companions met in this adventure", "Gef\xE4hrten aus diesem Abenteuer", "Compagnons rencontr\xE9s dans cette aventure", "Compa\xF1" "eros de esta aventura" },
    { "Nessuno per ora: chi entra nel tuo gruppo compare qui", "No one yet: whoever joins your party appears here", "Noch niemand: wer deiner Gruppe beitritt, erscheint hier", "Personne pour l'instant : qui rejoint ton groupe appara\xEEt ici", "Nadie por ahora: quien se une a tu grupo aparece aqu\xED" },
    { "E' un oggetto di serie del compagno: non si toglie, si puo' solo sostituire", "This is one of the companion's own items: it cannot be removed, only replaced", "Das ist ein eigener Gegenstand des Gef\xE4" "hrten: er kann nur ersetzt werden", "C'est un objet d'origine du compagnon : on ne peut que le remplacer", "Es un objeto propio del compa\xF1" "ero: no se quita, solo se sustituye" },
    { "Clicca di nuovo per sostituire l'oggetto di serie: andra' perso", "Click again to replace the companion's own item: it will be lost", "Nochmal klicken, um den eigenen Gegenstand zu ersetzen: er geht verloren", "Clique encore pour remplacer l'objet d'origine : il sera perdu", "Haz clic otra vez para sustituir el objeto propio: se perder\xE1" },
    { "Nessun compagno al seguito", "No companion is following you", "Kein Gef\xE4" "hrte folgt dir", "Aucun compagnon ne te suit", "Ning\xFA" "n compa\xF1" "ero te sigue" },
    { "Il compagno sale al livello %u: RT nello zaino per i suoi punti", "The companion reaches level %u: RT in the backpack for their points", "Der Gef\xE4" "hrte erreicht Stufe %u: RT im Rucksack f\xFC" "r seine Punkte", "Le compagnon passe au niveau %u : RT dans le sac pour ses points", "El compa\xF1" "ero sube al nivel %u: RT en la mochila para sus puntos" },
    { "Questo e' l'eroe 2: per il compagno clic destro sul suo ritratto", "This is hero 2: right-click the companion's portrait instead", "Das ist Held 2: f\xFC" "r den Gef\xE4" "hrten Rechtsklick auf sein Portr\xE4" "t", "C'est le h\xE9" "ros 2 : clic droit sur le portrait du compagnon", "Este es el h\xE9" "roe 2: clic derecho en el retrato del compa\xF1" "ero" },
    { "Compagno: le sue armi nelle due caselle sotto l'equipaggiamento", "Companion: their weapons in the two slots below the equipment", "Gef\xE4" "hrte: seine Waffen in den zwei Feldern unter der Ausr\xFC" "stung", "Compagnon : ses armes dans les deux cases sous l'\xE9" "quipement", "Compa\xF1" "ero: sus armas en las dos casillas bajo el equipo" },
    { "Vista del compagno chiusa", "Companion view closed", "Gef\xE4" "hrtenansicht geschlossen", "Vue du compagnon ferm\xE9" "e", "Vista del compa\xF1" "ero cerrada" },
    { "Con la vista del compagno trascina con il clic sinistro", "In companion view, drag with the left click", "In der Gef\xE4" "hrtenansicht mit Linksklick ziehen", "En vue du compagnon, fais glisser avec le clic gauche", "En la vista del compa\xF1" "ero, arrastra con el clic izquierdo" },
    { "Il compagno non sa usare quest'arma", "The companion cannot use this weapon", "Der Gef\xE4" "hrte kann diese Waffe nicht benutzen", "Le compagnon ne sait pas utiliser cette arme", "El compa\xF1" "ero no sabe usar esta arma" },
    { "Prima torna al tuo eroe", "Switch back to your hero first", "Wechsle zuerst zu deinem Helden zur\xFC" "ck", "Reviens d'abord \xE0" " ton h\xE9" "ros", "Vuelve primero a tu h\xE9" "roe" },
    { "Apri prima il tuo zaino", "Open your backpack first", "\xD6" "ffne zuerst deinen Rucksack", "Ouvre d'abord ton sac", "Abre primero tu mochila" },
    { "Vista del compagno: errore, spenta", "Companion view: error, turned off", "Gef\xE4" "hrtenansicht: Fehler, ausgeschaltet", "Vue du compagnon : erreur, d\xE9" "sactiv\xE9" "e", "Vista del compa\xF1" "ero: error, desactivada" },
    { "Mentore", "Mentor", "Mentor", "Mentor", "Mentor" },
    { "Non-morti", "Undead", "Untote", "Morts-vivants", "No muertos" },
    { "Indicatori dei giocatori", "Player markers", "Spielermarkierungen", "Marqueurs des joueurs", "Indicadores de jugadores" },
    { "accesi", "on", "an", "activ\xE9s", "activados" },
    { "spenti", "off", "aus", "d\xE9sactiv\xE9s", "desactivados" },
    { "Il giocatore 2 rialza l'eroe 1: %d%%", "Player 2 is reviving hero 1: %d%%", "Spieler 2 belebt Held 1: %d%%", "Le joueur 2 rel\xE8ve le h\xE9ros 1 : %d%%", "El jugador 2 reanima al h\xE9roe 1: %d%%" },
    { "Eroe 1 rialzato dal compagno", "Hero 1 revived by the companion", "Held 1 vom Gef\xE4hrten wiederbelebt", "H\xE9ros 1 relev\xE9 par le compagnon", "H\xE9roe 1 reanimado por el compa\xF1" "ero" },
    { "Raggiungi il compagno: servono tutti e due vivi", "Join companion: both must be alive", "Zum Gef\xE4hrten: beide m\xFCssen leben", "Rejoindre le compagnon : les deux doivent \xEAtre en vie", "Unirse al compa\xF1" "ero: ambos deben estar vivos" },
    { "Raggiungi il compagno: siete su piani diversi", "Join companion: you are on different levels", "Zum Gef\xE4hrten: ihr seid auf verschiedenen Ebenen", "Rejoindre le compagnon : vous \xEAtes \xE0 des niveaux diff\xE9rents", "Unirse al compa\xF1" "ero: est\xE1is en niveles distintos" },
    { "L'eroe 2 ha raggiunto l'eroe 1", "Hero 2 joined hero 1", "Held 2 ist bei Held 1", "Le h\xE9ros 2 a rejoint le h\xE9ros 1", "El h\xE9roe 2 se uni\xF3 al h\xE9roe 1" },
    { "Il fabbro lo sta usando l'altro giocatore: uno alla volta", "The other player is using the blacksmith: one at a time", "Der andere Spieler benutzt den Schmied: einer nach dem anderen", "L'autre joueur utilise le forgeron : un \xE0 la fois", "El otro jugador est\xE1 usando al herrero: uno a la vez" },
    { "Questa e' la colonna del giocatore 2", "This is player 2's column", "Das ist die Spalte von Spieler 2", "C'est la colonne du joueur 2", "Esta es la columna del jugador 2" },
    { "Questa e' la colonna del giocatore 1", "This is player 1's column", "Das ist die Spalte von Spieler 1", "C'est la colonne du joueur 1", "Esta es la columna del jugador 1" },
    { "L'altro giocatore la sta ancora usando", "The other player is still using it", "Der andere Spieler benutzt es noch", "L'autre joueur l'utilise encore", "El otro jugador todav\xED" "a lo est\xE1 usando" },
    { "Giocatore 2: la finestra e' tua, il giocatore 1 puo' unirsi", "Player 2: the window is yours, player 1 can join", "Spieler 2: das Fenster geh\xF6rt dir, Spieler 1 kann mitmachen", "Joueur 2 : la fen\xEAtre est \xE0 toi, le joueur 1 peut se joindre", "Jugador 2: la ventana es tuya, el jugador 1 puede unirse" },
    { "Questo e' il mercante del giocatore 2", "This is player 2's merchant", "Das ist der H\xE4ndler von Spieler 2", "C'est le marchand du joueur 2", "Este es el mercader del jugador 2" },
    { "Questo e' il mercante del giocatore 1", "This is player 1's merchant", "Das ist der H\xE4ndler von Spieler 1", "C'est le marchand du joueur 1", "Este es el mercader del jugador 1" },
    { "Aspetta: il giocatore 1 sta confermando un acquisto", "Wait: player 1 is confirming a purchase", "Warte: Spieler 1 best\xE4tigt einen Kauf", "Attends : le joueur 1 confirme un achat", "Espera: el jugador 1 est\xE1 confirmando una compra" },
    { "Aspetta: il giocatore 2 sta confermando un acquisto", "Wait: player 2 is confirming a purchase", "Warte: Spieler 2 best\xE4tigt einen Kauf", "Attends : le joueur 2 confirme un achat", "Espera: el jugador 2 est\xE1 confirmando una compra" },
    { "Giocatore 2: il negozio e' tuo, il giocatore 1 puo' unirsi", "Player 2: the shop is yours, player 1 can join", "Spieler 2: der Laden geh\xF6rt dir, Spieler 1 kann dazukommen", "Joueur 2 : la boutique est \xE0 toi, le joueur 1 peut se joindre", "Jugador 2: la tienda es tuya, el jugador 1 puede unirse" },
    { "Negozio: l'altro giocatore sta ancora comprando", "Shop: the other player is still shopping", "Laden: der andere Spieler kauft noch ein", "Boutique : l'autre joueur ach\xE8te encore", "Tienda: el otro jugador sigue comprando" },
    { "Giocatore 2: l'oggetto in mano e' tornato nello zaino", "Player 2: the held item went back into the backpack", "Spieler 2: der Gegenstand ist zur\xFC" "ck im Rucksack", "Joueur 2 : l'objet en main est retourn\xE9 dans le sac", "Jugador 2: el objeto en la mano volvi\xF3 a la mochila" },
    { "Zaino dell'eroe 2 pieno: l'oggetto in mano e' andato all'eroe 1", "Hero 2's backpack is full: the held item went to hero 1", "Rucksack von Held 2 voll: der Gegenstand ging an Held 1", "Sac du h\xE9ros 2 plein : l'objet en main est all\xE9 au h\xE9ros 1", "Mochila del h\xE9roe 2 llena: el objeto en la mano fue al h\xE9roe 1" },
    { "Zaini pieni: l'oggetto del giocatore 2 e' in mano al giocatore 1", "Backpacks full: player 2's item is now in player 1's hand", "Rucks\xE4" "cke voll: der Gegenstand von Spieler 2 ist in der Hand von Spieler 1", "Sacs pleins : l'objet du joueur 2 est dans la main du joueur 1", "Mochilas llenas: el objeto del jugador 2 est\xE1 en la mano del jugador 1" },
    { "Giocatore 2: posa l'oggetto che hai in mano", "Player 2: put down the item you are holding", "Spieler 2: leg den Gegenstand ab, den du h\xE4ltst", "Joueur 2 : pose l'objet que tu tiens", "Jugador 2: suelta el objeto que tienes en la mano" },
    { "Giocatore 2: modalita' cursore", "Player 2: cursor mode", "Spieler 2: Cursormodus", "Joueur 2 : mode curseur", "Jugador 2: modo cursor" },
    { "Giocatore 2: modalita' gioco", "Player 2: game mode", "Spieler 2: Spielmodus", "Joueur 2 : mode jeu", "Jugador 2: modo juego" },
    { "Giocatore 2: l'oggetto in mano si posa nello zaino", "Player 2: put the held item into the backpack", "Spieler 2: den Gegenstand in den Rucksack legen", "Joueur 2 : l'objet en main se pose dans le sac", "Jugador 2: el objeto en la mano se deja en la mochila" },
    { "tieni: zaino a destra", "hold: backpack on the right", "halten: Rucksack rechts", "maintenir : sac \xE0 droite", "mantener: mochila a la derecha" },
    { "Un mouse per giocatore", "One mouse per player", "Eine Maus pro Spieler", "Une souris par joueur", "Un rat\xF3n por jugador" },
    { "Giocatore 2 col cursore", "Player 2 with the cursor", "Spieler 2 mit dem Cursor", "Joueur 2 avec le curseur", "Jugador 2 con el cursor" },
    { "Due cursori del gioco", "Two game cursors", "Zwei Spiel-Cursor", "Deux curseurs du jeu", "Dos cursores del juego" },
    { "il 2 ha il numero accanto", "number 2 next to player 2's", "die 2 steht neben dem von Spieler 2", "le 2 a son num\xE9ro \xE0 c\xF4t\xE9", "el 2 lleva su n\xFAmero al lado" },
    { "Ognuno ha la sua mano", "Each one has their own hand", "Jeder hat seine eigene Hand", "Chacun a sa propre main", "Cada uno tiene su mano" },
    { "oggetti presi insieme", "items held at the same time", "Gegenst\xE4nde gleichzeitig halten", "objets pris en m\xEAme temps", "objetos cogidos a la vez" },
    { "Cursore del giocatore 2", "Player 2's cursor", "Cursor von Spieler 2", "Curseur du joueur 2", "Cursor del jugador 2" },
    { "View premuto, come il pad 1", "press View, like pad 1", "View dr\xFC" "cken, wie Pad 1", "appuyer sur View, comme la manette 1", "pulsar View, como el mando 1" },
    { "Clic: attacca, usa, cammina", "Click: attack, use, walk", "Klick: angreifen, benutzen, gehen", "Clic : attaquer, utiliser, marcher", "Clic: atacar, usar, caminar" },
    { "tieni sul terreno: cammina", "hold on the ground: walk", "auf dem Boden halten: gehen", "maintenir au sol : marcher", "mantener en el suelo: caminar" },
    { "Arte della sua barra", "Art from their bar", "Kunst aus seiner Leiste", "Art de sa barre", "Arte de su barra" },
    { "sul nemico indicato", "on the pointed enemy", "auf den gezeigten Gegner", "sur l'ennemi indiqu\xE9", "sobre el enemigo se\xF1" "alado" },
    { "tieni: il suo zaino", "hold: their backpack", "halten: sein Rucksack", "maintenir : son sac", "mantener: su mochila" },
    { "Stick sx", "Left stick", "Linker Stick", "Stick gauche", "Stick izquierdo" },
    { "Stick dx", "Right stick", "Rechter Stick", "Stick droit", "Stick derecho" },
    { "Croce su", "D-pad up", "Steuerkreuz hoch", "Croix haut", "Cruceta arriba" },
    { "Croce sx/dx", "D-pad left/right", "Steuerkreuz links/rechts", "Croix gauche/droite", "Cruceta izq./der." },
    { "Croce giu'", "D-pad down", "Steuerkreuz runter", "Croix bas", "Cruceta abajo" },
    { "Triangolo", "Triangle", "Dreieck", "Triangle", "Tri\xE1ngulo" },
    { "Quadrato", "Square", "Quadrat", "Carr\xE9", "Cuadrado" },
    { "Cerchio", "Circle", "Kreis", "Rond", "C\xEDrculo" },
    { "X (croce)", "X (cross)", "X (Kreuz)", "X (croix)", "X (equis)" },
    { "Arte 5", "Art 5", "Kunst 5", "Art 5", "Arte 5" },
    { "Arte 4", "Art 4", "Kunst 4", "Art 4", "Arte 4" },
    { "Arte 3", "Art 3", "Kunst 3", "Art 3", "Arte 3" },
    { "Arte 2", "Art 2", "Kunst 2", "Art 2", "Arte 2" },
    { "Arte 1", "Art 1", "Kunst 1", "Art 1", "Arte 1" },
    { "Pozione di cura", "Healing potion", "Heiltrank", "Potion de soin", "Poci\xF3n de curaci\xF3n" },
    { "Cursore (tenuto: inventario)", "Cursor (hold: inventory)", "Cursor (halten: Inventar)", "Curseur (maintenu : inventaire)", "Cursor (mantener: inventario)" },
    { "Movimento (premuto: minimappa)", "Movement (press: minimap)", "Bewegung (dr\xFC" "cken: Minikarte)", "D\xE9placement (appui : mini-carte)", "Movimiento (pulsar: minimapa)" },
    { "Pozione di concentrazione", "Concentration potion", "Konzentrationstrank", "Potion de concentration", "Poci\xF3n de concentraci\xF3n" },
    { "Arma precedente / successiva", "Previous / next weapon", "Vorherige / n\xE4" "chste Waffe", "Arme pr\xE9" "c\xE9" "dente / suivante", "Arma anterior / siguiente" },
    { "Antidoto", "Antidote", "Gegengift", "Antidote", "Ant\xED" "doto" },
    { "Concentrazione (tenuto: zoom +)", "Concentration (hold: zoom +)", "Konzentration (halten: Zoom +)", "Concentration (maintenir : zoom +)", "Concentraci\xF3n (mantener: zoom +)" },
    { "Antidoto (tenuto: zoom -)", "Antidote (hold: zoom -)", "Gegengift (halten: Zoom -)", "Antidote (maintenir : zoom -)", "Ant\xED" "doto (mantener: zoom -)" },
    { "Menu (tenuto: aiuto)", "Menu (hold: help)", "Men\xFC (halten: Hilfe)", "Menu (maintenu : aide)", "Men\xFA (mantener: ayuda)" },
    { "Raccogli (tenuto: cavallo)", "Pick up (hold: horse)", "Aufheben (halten: Pferd)", "Ramasser (maintenu : cheval)", "Recoger (mantener: caballo)" },
    { "Attacca e interagisci", "Attack and interact", "Angreifen, interagieren", "Attaquer, interagir", "Atacar e interactuar" },
    { "Mira (premuto: oggetti)", "Aim (press: items)", "Zielen (dr\xFC" "cken: Gegenst\xE4nde)", "Vis\xE9" "e (appui : objets)", "Apuntar (pulsar: objetos)" },
    // 0.51.6: elenco "Due giocatori" a due colonne (la tabulazione separa il dettaglio)
    { "Schermo diviso\tfinestre sovrapposte: serve uno schermo largo %d (questo %d)", "Split screen\twindows overlap: needs a %d-wide screen (this is %d)",
      "Geteilter Bildschirm\tFenster \xFC" "berlappen: Bildschirm mit %d Breite n\xF6tig (dieser %d)", "\xC9" "cran partag\xE9\tfen\xEAtres superpos\xE9" "es : \xE9" "cran de %d de large n\xE9" "cessaire (celui-ci %d)",
      "Pantalla dividida\tventanas superpuestas: hace falta una pantalla de %d de ancho (esta %d)" },
    { "Schermo diviso\tmanca la cartella del gioco copiata con \" P2\" nel nome", "Split screen\tneeds a copy of the game folder with \" P2\" in its name",
      "Geteilter Bildschirm\tbraucht eine Kopie des Spielordners mit \" P2\" im Namen", "\xC9" "cran partag\xE9\til faut une copie du dossier du jeu avec \" P2\" dans le nom",
      "Pantalla dividida\thace falta una copia de la carpeta del juego con \" P2\" en el nombre" },
    { "Schermo diviso\tdue finestre, un pad ciascuno", "Split screen\ttwo windows, one pad each", "Geteilter Bildschirm\tzwei Fenster, je ein Pad",
      "\xC9" "cran partag\xE9\tdeux fen\xEAtres, une manette chacun", "Pantalla dividida\tdos ventanas, un mando cada uno" },
    { "Stesso schermo, solo eroe 1\tpartita normale", "Same screen, hero 1 only\tnormal game", "Gleicher Bildschirm, nur Held 1\tnormales Spiel",
      "M\xEAme \xE9" "cran, h\xE9ros 1 seul\tpartie normale", "Misma pantalla, solo h\xE9roe 1\tpartida normal" },
    { "Stesso schermo con %s\tHero%02d", "Same screen with %s\tHero%02d", "Gleicher Bildschirm mit %s\tHero%02d",
      "M\xEAme \xE9" "cran avec %s\tHero%02d", "Misma pantalla con %s\tHero%02d" },
    // 0.55.51: menu Due giocatori rifatto
    { "Stesso schermo con %s", "Same screen with %s", "Gleicher Bildschirm mit %s", "M\xEAme \xE9" "cran avec %s", "Misma pantalla con %s" },
    { "%s   Hero%02d   giocato il %s", "%s   Hero%02d   played %s", "%s   Hero%02d   gespielt %s", "%s   Hero%02d   jou\xE9 le %s",
      "%s   Hero%02d   jugado el %s" },
    { "LIV.", "LVL", "STUFE", "NIV.", "NIV." },
    { "scegli", "choose", "w\xE4hlen", "choisir", "elegir" },
    { "conferma", "confirm", "best\xE4tigen", "valider", "confirmar" },
    { "annulla", "cancel", "abbrechen", "annuler", "cancelar" },
    { "Tastiera: frecce, Invio, Esc   Mouse: clic", "Keyboard: arrows, Enter, Esc   Mouse: click", "Tastatur: Pfeile, Eingabe, Esc   Maus: Klick",
      "Clavier : fl\xE8" "ches, Entr\xE9" "e, \xC9" "chap   Souris : clic", "Teclado: flechas, Intro, Esc   Rat\xF3n: clic" },
    // 0.51.7: classe e livello dell'eroe 2 nell'elenco
    { "Stesso schermo con %s\t%s, liv. %d   Hero%02d", "Same screen with %s\t%s, lvl %d   Hero%02d", "Gleicher Bildschirm mit %s\t%s, Stufe %d   Hero%02d",
      "M\xEAme \xE9" "cran avec %s\t%s, niv. %d   Hero%02d", "Misma pantalla con %s\t%s, niv. %d   Hero%02d" },
    { "Serafina", "Seraphim", "Seraphim", "S\xE9raphine", "Seraf\xEDn" },
    { "Gladiatore", "Gladiator", "Gladiator", "Gladiateur", "Gladiador" },
    { "Mago", "Battle Mage", "Kampfmagier", "Mage de combat", "Mago de batalla" },
    { "Elfo oscuro", "Dark Elf", "Dunkelelf", "Elfe noir", "Elfo oscuro" },
    { "Elfa dei boschi", "Wood Elf", "Waldelfe", "Elfe des bois", "Elfa del bosque" },
    { "Vampira", "Vampiress", "Vampirin", "Vampiresse", "Vampiresa" },
    { "Nano", "Dwarf", "Zwerg", "Nain", "Enano" },
    { "Diavola", "Daemon", "D\xE4mon", "D\xE9mon", "Demonio" },
    // 0.51.5: impostazioni dell'aiuto
    { "Controller giocatore 1", "Player 1 controller", "Controller Spieler 1", "Manette joueur 1", "Mando jugador 1" },
    { "Controller giocatore 2", "Player 2 controller", "Controller Spieler 2", "Manette joueur 2", "Mando jugador 2" },
    { "Controller", "Controller", "Controller", "Manette", "Mando" },   // 0.53.3: schermo diviso, un controller per finestra
    { "Vibrazione a vita bassa", "Low health vibration", "Vibration bei wenig Leben", "Vibration \xE0 faible vie", "Vibraci\xF3n con poca vida" },
    { "Croce su / giu': scegli   Croce sinistra / destra o %s: cambia   %s o %s: chiudi",
      "D-pad up / down: choose   D-pad left / right or %s: change   %s or %s: close",
      "Steuerkreuz hoch / runter: w\xE4hlen   links / rechts oder %s: \xE4ndern   %s oder %s: schlie\xDF" "en",
      "Croix haut / bas : choisir   gauche / droite ou %s : changer   %s ou %s : fermer",
      "Cruceta arriba / abajo: elegir   izquierda / derecha o %s: cambiar   %s o %s: cerrar" },
    // 0.51.4: aiuto del giocatore 2
    { "Interfaccia dell'eroe 2", "Hero 2 interface", "Oberfl\xE4" "che von Held 2", "Interface du h\xE9ros 2", "Interfaz del h\xE9roe 2" },
    { "Movimento", "Movement", "Bewegung", "D\xE9placement", "Movimiento" },
    { "Armi: non ancora", "Weapons: not yet", "Waffen: noch nicht", "Armes : pas encore", "Armas: a\xFAn no" },
    { "Raccogli (tenuto: automatica)", "Pick up (hold: automatic)", "Aufheben (halten: automatisch)", "Ramasser (maintenu : auto)", "Recoger (mantener: autom\xE1tico)" },
    { "Interagisci o attacca", "Interact or attack", "Interagieren oder angreifen", "Interagir ou attaquer", "Interactuar o atacar" },
    { "Non ancora", "Not yet", "Noch nicht", "Pas encore", "A\xFAn no" },
    { "Premuto: oggetti (Alt)", "Press: items (Alt)", "Dr\xFC" "cken: Gegenst\xE4nde (Alt)", "Appui : objets (Alt)", "Pulsar: objetos (Alt)" },
    { "Giocatore 2: il tuo eroe non ha un cavallo", "Player 2: your hero has no horse", "Spieler 2: dein Held hat kein Pferd", "Joueur 2 : ton h\xE9ros n'a pas de cheval", "Jugador 2: tu h\xE9roe no tiene caballo" },
    { "SacredPad - comandi del controller: %s", "SacredPad - controller commands: %s", "SacredPad - Controller-Befehle: %s", "SacredPad - commandes de la manette : %s", "SacredPad - controles del mando: %s" },
    { "Modalita' cursore (inventario, mercante, menu)", "Cursor mode (inventory, merchant, menus)", "Cursormodus (Inventar, H\xE4ndler, Men\xFCs)", "Mode curseur (inventaire, marchand, menus)", "Modo cursor (inventario, mercader, men\xFAs)" },
    { "Stick: cursore   Croce: caselle e pulsanti   %s: clic   %s: clic destro   %s: chiudi la finestra", "Stick: cursor   D-pad: slots and buttons   %s: click   %s: right click   %s: close the window", "Stick: Cursor   Steuerkreuz: Felder und Kn\xF6pfe   %s: Klick   %s: Rechtsklick   %s: Fenster schlie\xDF" "en", "Stick : curseur   Croix : cases et boutons   %s : clic   %s : clic droit   %s : fermer la fen\xEAtre", "Stick: cursor   Cruceta: casillas y botones   %s: clic   %s: clic derecho   %s: cerrar la ventana" },
    { "%s: vendi (mercante)   %s / %s: schede (mercante, inventario)   %s: torna al gioco", "%s: sell (merchant)   %s / %s: tabs (merchant, inventory)   %s: back to the game", "%s: verkaufen (H\xE4ndler)   %s / %s: Reiter (H\xE4ndler, Inventar)   %s: zur\xFC" "ck zum Spiel", "%s : vendre (marchand)   %s / %s : onglets (marchand, inventaire)   %s : retour au jeu", "%s: vender (mercader)   %s / %s: pesta\xF1" "as (mercader, inventario)   %s: volver al juego" },
    { "<   Controller: %s   >", "<   Controller: %s   >", "<   Controller: %s   >", "<   Manette : %s   >", "<   Mando: %s   >" },
    { "Controller: %s", "Controller: %s", "Controller: %s", "Manette : %s", "Mando: %s" },
    { "<   Vibrazione a vita bassa: %s   >", "<   Low health vibration: %s   >", "<   Vibration bei wenig Leben: %s   >", "<   Vibration si peu de vie : %s   >", "<   Vibraci\xF3n con poca vida: %s   >" },
    { "Vibrazione a vita bassa: %s", "Low health vibration: %s", "Vibration bei wenig Leben: %s", "Vibration si peu de vie : %s", "Vibraci\xF3n con poca vida: %s" },
    { "attiva", "on", "an", "activ\xE9" "e", "activada" },
    { "disattivata", "off", "aus", "d\xE9sactiv\xE9" "e", "desactivada" },
    { "Croce su / giu': scegli   Croce sinistra / destra: cambia   %s o %s: chiudi", "D-pad up / down: choose   D-pad left / right: change   %s or %s: close", "Steuerkreuz hoch / runter: w\xE4hlen   links / rechts: \xE4ndern   %s oder %s: schlie\xDF" "en", "Croix haut / bas : choisir   gauche / droite : changer   %s ou %s : fermer", "Cruceta arriba / abajo: elegir   izquierda / derecha: cambiar   %s o %s: cerrar" },
    { "SacredPad %s - modalita' %s", "SacredPad %s - mode: %s", "SacredPad %s - Modus: %s", "SacredPad %s - mode : %s", "SacredPad %s - modo: %s" },
    { "cursore", "cursor", "Cursor", "curseur", "cursor" },
    { "gioco", "game", "Spiel", "jeu", "juego" },
    { "SacredPad - comandi del controller del giocatore 2: %s", "SacredPad - player 2 controller: %s", "SacredPad - Controller von Spieler 2: %s", "SacredPad - manette du joueur 2 : %s", "SacredPad - mando del jugador 2: %s" },
    { "<   Controller del giocatore 2: %s   >", "<   Player 2 controller: %s   >", "<   Controller von Spieler 2: %s   >", "<   Manette du joueur 2 : %s   >", "<   Mando del jugador 2: %s   >" },
    { "Controller del giocatore 2: %s", "Player 2 controller: %s", "Controller von Spieler 2: %s", "Manette du joueur 2 : %s", "Mando del jugador 2: %s" },
    { "cursore del giocatore 2", "player 2 cursor", "Cursor von Spieler 2", "curseur du joueur 2", "cursor del jugador 2" },
    { "Giocatore 2: interfaccia del tuo eroe, Back per tornare", "Player 2: your hero's interface, Back to return", "Spieler 2: Men\xFC" "s deines Helden, Back zum Zur\xFC" "ckkehren", "Joueur 2 : interface de ton h\xE9ros, Back pour revenir", "Jugador 2: interfaz de tu h\xE9roe, Back para volver" },
    { "Oggetto in mano: posalo nello zaino dell'altro eroe", "Item in hand: drop it into the other hero's backpack", "Gegenstand in der Hand: lege ihn in den Rucksack des anderen Helden", "Objet en main : pose-le dans le sac de l'autre h\xE9ros", "Objeto en la mano: su\xE9ltalo en la mochila del otro h\xE9roe" },
    { "Giocatore 2: controller scollegato, il puntatore torna al giocatore 1", "Player 2: controller disconnected, the pointer goes back to player 1", "Spieler 2: Controller getrennt, der Zeiger geht an Spieler 1 zur\xFC" "ck", "Joueur 2 : manette d\xE9" "connect\xE9" "e, le pointeur revient au joueur 1", "Jugador 2: mando desconectado, el puntero vuelve al jugador 1" },
    { "%s  -  liv. %d   %d / %d", "%s  -  lvl %d   %d / %d", "%s  -  Stufe %d   %d / %d", "%s  -  niv. %d   %d / %d", "%s  -  niv. %d   %d / %d" },
    { "%s  -  liv. %d   A TERRA", "%s  -  lvl %d   DOWN", "%s  -  Stufe %d   AM BODEN", "%s  -  niv. %d   \xC0 TERRE", "%s  -  niv. %d   EN EL SUELO" },
    { "Eroe 2", "Hero 2", "Held 2", "H\xE9ros 2", "H\xE9roe 2" },
    { "Due giocatori", "Two players", "Zwei Spieler", "Deux joueurs", "Dos jugadores" },
    { "Co-op locale", "Local co-op", "Lokaler Koop", "Coop locale", "Cooperativo local" },
    { "Schermo diviso, o stesso schermo con l'eroe del giocatore 2", "Split screen, or same screen with player 2's hero", "Geteilter Bildschirm oder gleicher Bildschirm mit dem Helden von Spieler 2", "\xC9" "cran partag\xE9, ou m\xEAme \xE9" "cran avec le h\xE9ros du joueur 2", "Pantalla dividida, o misma pantalla con el h\xE9roe del jugador 2" },
    { "Su / Giu o croce: scegli   Invio, clic o A: conferma   Esc o B: annulla", "Up / Down or D-pad: choose   Enter, click or A: confirm   Esc or B: cancel", "Hoch / Runter oder Steuerkreuz: w\xE4hlen   Eingabe, Klick oder A: best\xE4tigen   Esc oder B: abbrechen", "Haut / Bas ou croix : choisir   Entr\xE9" "e, clic ou A : valider   \xC9" "chap ou B : annuler", "Arriba / Abajo o cruceta: elegir   Intro, clic o A: confirmar   Esc o B: cancelar" },
    { "Schermo diviso (due finestre)", "Split screen (two windows)", "Geteilter Bildschirm (zwei Fenster)", "\xC9" "cran partag\xE9 (deux fen\xEAtres)", "Pantalla dividida (dos ventanas)" },
    { "Stesso schermo: nessun eroe 2 (partita normale)", "Same screen: no hero 2 (normal game)", "Gleicher Bildschirm: kein Held 2 (normales Spiel)", "M\xEAme \xE9" "cran : pas de h\xE9ros 2 (partie normale)", "Misma pantalla: sin h\xE9roe 2 (partida normal)" },
    { "Stesso schermo, eroe 2: %s   (Hero%02d)", "Same screen, hero 2: %s   (Hero%02d)", "Gleicher Bildschirm, Held 2: %s   (Hero%02d)", "M\xEAme \xE9" "cran, h\xE9ros 2 : %s   (Hero%02d)", "Misma pantalla, h\xE9roe 2: %s   (Hero%02d)" },
    { "Schermo diviso: avvio delle due finestre", "Split screen: starting the two windows", "Geteilter Bildschirm: die zwei Fenster starten", "\xC9" "cran partag\xE9 : lancement des deux fen\xEAtres", "Pantalla dividida: iniciando las dos ventanas" },
    { "Partita normale: nessun eroe 2", "Normal game: no hero 2", "Normales Spiel: kein Held 2", "Partie normale : pas de h\xE9ros 2", "Partida normal: sin h\xE9roe 2" },
    { "Eroe 2: %s. Ora avvia o carica una partita.", "Hero 2: %s. Now start or load a game.", "Held 2: %s. Jetzt ein Spiel starten oder laden.", "H\xE9ros 2 : %s. Lancez ou chargez une partie.", "H\xE9roe 2: %s. Ahora inicia o carga una partida." },
    { "Due giocatori: eroe 2 = %s", "Two players: hero 2 = %s", "Zwei Spieler: Held 2 = %s", "Deux joueurs : h\xE9ros 2 = %s", "Dos jugadores: h\xE9roe 2 = %s" },
    { "Raccolta automatica dell'eroe 2: attiva", "Hero 2 auto pickup: on", "Automatisches Aufheben von Held 2: an", "Ramassage auto du h\xE9ros 2 : activ\xE9", "Recogida autom\xE1tica del h\xE9roe 2: activada" },
    { "Raccolta automatica dell'eroe 2: spenta", "Hero 2 auto pickup: off", "Automatisches Aufheben von Held 2: aus", "Ramassage auto du h\xE9ros 2 : d\xE9sactiv\xE9", "Recogida autom\xE1tica del h\xE9roe 2: desactivada" },
    { "Giocatore 2: nessun oggetto vicino", "Player 2: no item nearby", "Spieler 2: kein Gegenstand in der N\xE4he", "Joueur 2 : aucun objet proche", "Jugador 2: ning\xFAn objeto cerca" },
    { "Giocatore 2: controller collegato", "Player 2: controller connected", "Spieler 2: Controller verbunden", "Joueur 2 : manette connect\xE9" "e", "Jugador 2: mando conectado" },
    { "Giocatore 2: controller scollegato", "Player 2: controller disconnected", "Spieler 2: Controller getrennt", "Joueur 2 : manette d\xE9" "connect\xE9" "e", "Jugador 2: mando desconectado" },
    { "Eroe 2 salvato (livello %u)", "Hero 2 saved (level %u)", "Held 2 gespeichert (Stufe %u)", "H\xE9ros 2 sauvegard\xE9 (niveau %u)", "H\xE9roe 2 guardado (nivel %u)" },
    { "Oggetto delle missioni dato all'eroe 1", "Quest item given to hero 1", "Questgegenstand an Held 1 gegeben",
      "Objet de qu\xEAte donn\xE9 au h\xE9ros 1", "Objeto de misi\xF3n dado al h\xE9roe 1" },
    { "Eroe 2: progressi recuperati dal salvataggio (livello %d)", "Hero 2: progress recovered from the save (level %d)",
      "Held 2: Fortschritt aus dem Spielstand wiederhergestellt (Stufe %d)", "H\xE9ros 2 : progression r\xE9" "cup\xE9r\xE9" "e de la sauvegarde (niveau %d)",
      "H\xE9roe 2: progreso recuperado de la partida guardada (nivel %d)" },
    { "Giocatore 2: avvicinati alla porta", "Player 2: move closer to the door", "Spieler 2: geh n\xE4her an die T\xFCr", "Joueur 2 : approche-toi de la porte", "Jugador 2: ac\xE9rcate a la puerta" },
    { "Giocatore 2: A conferma, B rifiuta", "Player 2: A confirms, B declines", "Spieler 2: A best\xE4tigt, B lehnt ab", "Joueur 2 : A valide, B refuse", "Jugador 2: A confirma, B rechaza" },
    { "Giocatore 2: Back per usare la finestra", "Player 2: Back to use the window", "Spieler 2: Back, um das Fenster zu benutzen", "Joueur 2 : Back pour utiliser la fen\xEAtre", "Jugador 2: Back para usar la ventana" },
    { "Partita per un giocatore: eroe 2 del salvataggio tolto", "Single player game: hero 2 from the save removed", "Einzelspieler: Held 2 aus dem Spielstand entfernt", "Partie solo : h\xE9ros 2 de la sauvegarde retir\xE9", "Partida de un jugador: h\xE9roe 2 de la partida guardada quitado" },
    { "Eroe 2 del salvataggio non tolto: copia di sicurezza non riuscita", "Hero 2 from the save not removed: backup failed", "Held 2 aus dem Spielstand nicht entfernt: Sicherung fehlgeschlagen", "H\xE9ros 2 de la sauvegarde non retir\xE9 : copie de s\xE9" "curit\xE9 \xE9" "chou\xE9" "e", "H\xE9roe 2 de la partida guardada no quitado: copia de seguridad fallida" },
    { "Attenzione: eroe 1 livello %u, eroe 2 livello %u", "Warning: hero 1 level %u, hero 2 level %u", "Achtung: Held 1 Stufe %u, Held 2 Stufe %u", "Attention : h\xE9ros 1 niveau %u, h\xE9ros 2 niveau %u", "Atenci\xF3n: h\xE9roe 1 nivel %u, h\xE9roe 2 nivel %u" },
    { "Ricompensa: una runa per l'eroe 2, a terra accanto a lui", "Reward: a rune for hero 2, on the ground next to him", "Belohnung: eine Rune f\xFCr Held 2, neben ihm am Boden",
      "R\xE9" "compense : une rune pour le h\xE9ros 2, au sol \xE0 c\xF4t\xE9 de lui", "Recompensa: una runa para el h\xE9roe 2, en el suelo a su lado" },
    // 0.56.53: punti delle missioni
    { "Missione: punti anche per l'eroe 2", "Quest: points for hero 2 too", "Auftrag: Punkte auch f\xFC" "r Held 2", "Qu\xEA" "te : des points aussi pour le h\xE9ros 2",
      "Misi\xF3n: puntos tambi\xE9n para el h\xE9roe 2" },
    // 0.56.45: rune gemelle dei forzieri
    { "Forziere: una runa anche per l'altro eroe, a terra", "Chest: a rune for the other hero too, on the ground", "Truhe: auch eine Rune f\xFC" "r den anderen Helden, am Boden",
      "Coffre : une rune aussi pour l'autre h\xE9ros, au sol", "Cofre: una runa tambi\xE9n para el otro h\xE9roe, en el suelo" },
    { "Questo Sacred.exe non e\' quello per cui e\' fatta SacredPad (pureHD 2.0.2.118).\n\nFunziona solo il controller di base: niente co-op, aggancio dei bersagli, due zaini e finestre della mod. Vedi \"Requirements\" nel README.\n\nMostrare di nuovo questo avviso al prossimo avvio?",
      "This Sacred.exe is not the one SacredPad is made for (pureHD 2.0.2.118).\n\nOnly the basic controller works: no co-op, no target lock, no two inventories, no mod windows. See \"Requirements\" in the README.\n\nShow this message again next time?",
      "Diese Sacred.exe ist nicht die, f\xFCr die SacredPad gemacht ist (pureHD 2.0.2.118).\n\nNur die Grundsteuerung mit dem Controller funktioniert: kein Koop, keine Zielerfassung, keine zwei Inventare, keine Fenster der Mod. Siehe \"Requirements\" in der README.\n\nDiese Meldung beim n\xE4" "chsten Start wieder anzeigen?",
      "Ce Sacred.exe n\'est pas celui pour lequel SacredPad est fait (pureHD 2.0.2.118).\n\nSeule la manette de base fonctionne : pas de coop, pas de verrouillage de cible, pas de deux inventaires, pas de fen\xEAtres du mod. Voir \"Requirements\" dans le README.\n\nAfficher ce message au prochain d\xE9marrage ?",
      "Este Sacred.exe no es el que necesita SacredPad (pureHD 2.0.2.118).\n\nSolo funciona el mando b\xE1sico: sin cooperativo, sin fijaci\xF3n de objetivo, sin dos inventarios, sin ventanas del mod. Consulta \"Requirements\" en el README.\n\n\xBFMostrar este aviso la pr\xF3xima vez?" },
    { "Ricompensa anche per l'eroe 2, a terra", "Reward for hero 2 too, on the ground", "Belohnung auch f\xFC" "r Held 2, am Boden",
      "R\xE9" "compense aussi pour le h\xE9ros 2, au sol", "Recompensa tambi\xE9n para el h\xE9roe 2, en el suelo" },
    // 0.55.54: due zaini, fase 2
    { "Posa prima l'oggetto che hai in mano", "Put down the item you are holding first", "Lege zuerst den Gegenstand ab, den du h\xE4ltst",
      "Pose d'abord l'objet que tu tiens", "Suelta primero el objeto que tienes en la mano" },
    { "Il tuo zaino (premuto o tenuto)", "Your inventory (press or hold)", "Dein Inventar (dr\xFC" "cken oder halten)", "Ton inventaire (appui ou maintien)",
      "Tu inventario (pulsar o mantener)" },
    { "Giocatore 2: il tuo zaino e' a destra, Back per chiuderlo", "Player 2: your inventory is on the right, Back to close it",
      "Spieler 2: dein Inventar ist rechts, Back zum Schlie\xDF" "en", "Joueur 2 : ton inventaire est \xE0 droite, Back pour le fermer",
      "Jugador 2: tu inventario est\xE1 a la derecha, Back para cerrarlo" },
    { "Aspetta: il giocatore 1 ha un oggetto in mano", "Wait: player 1 is holding an item", "Warte: Spieler 1 h\xE4lt einen Gegenstand",
      "Attends : le joueur 1 tient un objet", "Espera: el jugador 1 tiene un objeto en la mano" },
    { "Aspetta: il giocatore 1 sta usando il puntatore", "Wait: player 1 is using the pointer", "Warte: Spieler 1 benutzt den Zeiger",
      "Attends : le joueur 1 utilise le pointeur", "Espera: el jugador 1 est\xE1 usando el puntero" },
    { "Aspetta: il giocatore 2 ha un oggetto in mano", "Wait: player 2 is holding an item", "Warte: Spieler 2 h\xE4lt einen Gegenstand",
      "Attends : le joueur 2 tient un objet", "Espera: el jugador 2 tiene un objeto en la mano" },
    { "Aspetta: il giocatore 2 sta usando il puntatore", "Wait: player 2 is using the pointer", "Warte: Spieler 2 benutzt den Zeiger",
      "Attends : le joueur 2 utilise le pointeur", "Espera: el jugador 2 est\xE1 usando el puntero" },
    { "Due zaini: a sinistra l'eroe 1, a destra l'eroe 2", "Two inventories: hero 1 on the left, hero 2 on the right",
      "Zwei Inventare: links Held 1, rechts Held 2", "Deux inventaires : h\xE9ros 1 \xE0 gauche, h\xE9ros 2 \xE0 droite",
      "Dos inventarios: h\xE9roe 1 a la izquierda, h\xE9roe 2 a la derecha" },
    // 0.55.68: nuova schermata dei comandi
    { "In gioco", "In game", "Im Spiel",
      "En jeu", "En juego" },
    { "Cursore e zaino", "Cursor and inventory", "Cursor und Inventar",
      "Curseur et inventaire", "Cursor e inventario" },
    { "Il tuo zaino", "Your inventory", "Dein Inventar",
      "Ton inventaire", "Tu inventario" },
    { "premuto o tenuto", "press or hold", "dr\xFC" "cken oder halten",
      "appuyer ou maintenir", "pulsar o mantener" },
    { "Cursore", "Cursor", "Cursor",
      "Curseur", "Cursor" },
    { "tieni: il tuo zaino", "hold: your inventory", "halten: dein Inventar",
      "maintenir : ton inventaire", "mantener: tu inventario" },
    { "Muovi", "Move", "Bewegen",
      "Se d\xE9placer", "Moverse" },
    { "premuto: minimappa", "click: minimap", "dr\xFC" "cken: Minikarte",
      "clic : minicarte", "pulsar: minimapa" },
    { "Su", "Up", "Hoch",
      "Haut", "Arriba" },
    { "Giu'", "Down", "Runter",
      "Bas", "Abajo" },
    { "Concentrazione", "Concentration", "Konzentration",
      "Concentration", "Concentraci\xF3n" },
    { "tieni: zoom avanti", "hold: zoom in", "halten: hineinzoomen",
      "maintenir : zoom avant", "mantener: acercar" },
    { "Arma prec. / succ.", "Prev. / next weapon", "Waffe zur\xFC" "ck / vor",
      "Arme pr\xE9" "c. / suiv.", "Arma ant. / sig." },
    { "tieni: zoom indietro", "hold: zoom out", "halten: herauszoomen",
      "maintenir : zoom arri\xE8re", "mantener: alejar" },
    { "Menu", "Menu", "Men\xFC",
      "Menu", "Men\xFA" },
    { "tieni: questo aiuto", "hold: this help", "halten: diese Hilfe",
      "maintenir : cette aide", "mantener: esta ayuda" },
    { "Raccogli tutto", "Pick up all", "Alles aufheben",
      "Tout ramasser", "Recoger todo" },
    { "tieni: cavallo", "hold: horse", "halten: Pferd",
      "maintenir : cheval", "mantener: caballo" },
    { "tieni + stick: scegli", "hold + stick: choose", "halten + Stick: w\xE4hlen",
      "maintenir + stick : choisir", "mantener + stick: elegir" },
    { "Mira", "Aim", "Zielen",
      "Viser", "Apuntar" },
    { "premuto: nomi oggetti", "click: item names", "dr\xFC" "cken: Gegenstandsnamen",
      "clic : noms des objets", "pulsar: nombres de objetos" },
    { "Nel cursore", "In cursor mode", "Im Cursormodus",
      "En mode curseur", "En modo cursor" },
    { "Muovi il cursore", "Move the cursor", "Cursor bewegen",
      "D\xE9placer le curseur", "Mover el cursor" },
    { "Croce", "D-pad", "Steuerkreuz",
      "Croix", "Cruceta" },
    { "Caselle e pulsanti", "Slots and buttons", "Felder und Schaltfl\xE4" "chen",
      "Cases et boutons", "Casillas y botones" },
    { "Clic", "Click", "Klick",
      "Clic", "Clic" },
    { "prendi e posa", "pick up and drop", "nehmen und ablegen",
      "prendre et poser", "coger y soltar" },
    { "Clic destro", "Right click", "Rechtsklick",
      "Clic droit", "Clic derecho" },
    { "usa, equipaggia", "use, equip", "benutzen, ausr\xFCsten",
      "utiliser, \xE9quiper", "usar, equipar" },
    { "Chiudi la finestra", "Close the window", "Fenster schlie\xDF" "en",
      "Fermer la fen\xEAtre", "Cerrar la ventana" },
    { "Schede", "Tabs", "Reiter",
      "Onglets", "Pesta\xF1" "as" },
    { "Scheda precedente", "Previous tab", "Vorheriger Reiter",
      "Onglet pr\xE9" "c\xE9" "dent", "Pesta\xF1" "a anterior" },
    { "zaino, arti, combo", "inventory, arts, combos", "Inventar, K\xFCnste, Kombos",
      "inventaire, arts, combos", "inventario, artes, combos" },
    { "Scheda successiva", "Next tab", "N\xE4" "chster Reiter",
      "Onglet suivant", "Pesta\xF1" "a siguiente" },
    { "anche dal mercante", "also at merchants", "auch beim H\xE4ndler",
      "aussi chez le marchand", "tambi\xE9n con el mercader" },
    { "Torna al gioco", "Back to the game", "Zur\xFC" "ck zum Spiel",
      "Retour au jeu", "Volver al juego" },
    { "Mercante", "Merchant", "H\xE4ndler",
      "Marchand", "Mercader" },
    { "Vendi", "Sell", "Verkaufen",
      "Vendre", "Vender" },
    { "l'oggetto sotto il cursore", "the item under the cursor", "den Gegenstand unter dem Cursor",
      "l'objet sous le curseur", "el objeto bajo el cursor" },
    { "Esci dal negozio", "Leave the shop", "Laden verlassen",
      "Quitter la boutique", "Salir de la tienda" },
    { "Il mouse del gioco", "The game mouse", "Die Spielmaus",
      "La souris du jeu", "El rat\xF3n del juego" },
    { "Lo prende chi preme", "Whoever presses takes it", "Wer dr\xFC" "ckt, bekommt sie",
      "Celui qui appuie la prend", "Lo coge quien pulsa" },
    { "o chi muove lo stick", "or moves the stick", "oder den Stick bewegt",
      "ou bouge le stick", "o mueve el stick" },
    { "Puntatori numerati", "Numbered pointers", "Nummerierte Zeiger",
      "Pointeurs num\xE9rot\xE9s", "Punteros numerados" },
    { "1 oro, 2 azzurro", "1 gold, 2 light blue", "1 gold, 2 hellblau",
      "1 or, 2 bleu clair", "1 oro, 2 azul claro" },
    { "Qualcosa in mano", "Holding something", "Etwas in der Hand",
      "Quelque chose en main", "Algo en la mano" },
    { "l'altro aspetta", "the other waits", "der andere wartet",
      "l'autre attend", "el otro espera" },
    { "Pad 1 con lo zaino del 2", "Pad 1 with inventory 2 open", "Pad 1 bei offenem Inventar 2",
      "Manette 1, inventaire 2 ouvert", "Mando 1 con inventario 2 abierto" },
    { "Attacca, apre, parla", "Attacks, opens, talks", "Greift an, \xF6" "ffnet, redet",
      "Attaque, ouvre, parle", "Ataca, abre, habla" },
    { "forzieri e personaggi", "chests and characters", "Truhen und Figuren",
      "coffres et personnages", "cofres y personajes" },
    { "Arti", "Arts", "K\xFCnste",
      "Arts", "Artes" },
    { "anche Y, RB, RT, LT", "also Y, RB, RT, LT", "auch Y, RB, RT, LT",
      "aussi Y, RB, RT, LT", "tambi\xE9n Y, RB, RT, LT" },
    { "Giocatore 1", "Player 1", "Spieler 1",
      "Joueur 1", "Jugador 1" },
    { "Giocatore 2", "Player 2", "Spieler 2",
      "Joueur 2", "Jugador 2" },
    { "tieni: zaino a sinistra", "hold: inventory on the left", "halten: Inventar links",
      "maintenir : inventaire \xE0 gauche", "mantener: inventario a la izquierda" },
    { "premi: zaino a destra", "press: inventory on the right", "dr\xFC" "cken: Inventar rechts",
      "appuyer : inventaire \xE0 droite", "pulsar: inventario a la derecha" },
    { "Chiudi il tuo lato", "Close your side", "Deine Seite schlie\xDF" "en",
      "Fermer ton c\xF4t\xE9", "Cerrar tu lado" },
    { "l'altro resta aperto", "the other stays open", "die andere bleibt offen",
      "l'autre reste ouvert", "el otro sigue abierto" },
    { "Premi un tasto: si accende   %s / %s: pagina   Croce su / giu': scegli   sinistra / destra: cambia   %s: chiudi", "Press a button: it lights up   %s / %s: page   D-pad up / down: choose   left / right: change   %s: close", "Taste dr\xFC" "cken: sie leuchtet   %s / %s: Seite   Kreuz hoch / runter: w\xE4hlen   links / rechts: \xE4ndern   %s: schlie\xDF" "en",
      "Appuie sur un bouton : il s'allume   %s / %s : page   Croix haut / bas : choisir   gauche / droite : changer   %s : fermer", "Pulsa un bot\xF3n: se ilumina   %s / %s: p\xE1gina   Cruceta arriba / abajo: elegir   izquierda / derecha: cambiar   %s: cerrar" },
};

int CodeToLang(const char* v)
{
    char c[4] = {};
    int n = 0;
    for (const char* p = v; *p && n < 3; ++p) if (isalpha(static_cast<unsigned char>(*p))) c[n++] = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    if (n < 2) return -1;
    if (c[0] == 'i' && c[1] == 't') return 0;
    if ((c[0] == 'e' && c[1] == 'n') || (c[0] == 'u' && c[1] == 's') || (c[0] == 'u' && c[1] == 'k')) return 1;
    if ((c[0] == 'd' && c[1] == 'e') || (c[0] == 'g' && c[1] == 'e')) return 2;
    if (c[0] == 'f' && c[1] == 'r') return 3;
    if ((c[0] == 'e' && c[1] == 's') || (c[0] == 's' && c[1] == 'p')) return 4;
    return -2;   // lingua riconosciuta come presente ma senza tabella: inglese
}

int DetectLang()   // 0 it, 1 en, 2 de, 3 fr, 4 es
{
    const char* names[] = { "italiano", "inglese", "tedesco", "francese", "spagnolo" };
    if (_stricmp(g_cfg.language, "Auto") != 0) {
        int l = CodeToLang(g_cfg.language);
        if (l < 0) l = 1;
        Log("LINGUA: %s (dall'ini: %s)", names[l], g_cfg.language);
        return l;
    }
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\Settings.cfg", GameDir());
    if (FILE* f = nullptr; fopen_s(&f, path, "r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char up[256] = {};
            for (int i = 0; line[i] && i < 255; ++i) up[i] = static_cast<char>(toupper(static_cast<unsigned char>(line[i])));
            const char* colon = strchr(line, ':');
            if (!strstr(up, "LANG") || !colon || strstr(up, "LANG") > up + (colon - line)) continue;
            int l = CodeToLang(colon + 1);
            if (l == -1) continue;
            if (l < 0) l = 1;
            fclose(f);
            line[strcspn(line, "\r\n")] = 0;
            Log("LINGUA: %s (Settings.cfg: \"%s\")", names[l], line);
            return l;
        }
        fclose(f);
    }
    int l = 1;
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_ITALIAN: l = 0; break;
    case LANG_GERMAN:  l = 2; break;
    case LANG_FRENCH:  l = 3; break;
    case LANG_SPANISH: l = 4; break;
    default:           l = 1; break;
    }
    Log("LINGUA: %s (lingua di Windows %04X, Settings.cfg senza voce della lingua)", names[l], GetUserDefaultUILanguage());
    return l;
}
}

// Tabella e lingua si preparano una volta sola, al primo uso (statiche locali: inizializzazione sicura fra i thread,
// perche' Tr si chiama sia dal thread della DLL sia da quello del disegno).
const char* Tr(const char* it)
{
    if (!it) return it;
    static const int lang = DetectLang();
    if (lang == 0) return it;
    static const std::map<std::string, const Row*> idx = [] {
        std::map<std::string, const Row*> m;
        for (const Row& r : kRows) m[r.it] = &r;
        return m;
    }();
    const auto f = idx.find(it);
    if (f == idx.end()) return it;
    const Row& r = *f->second;
    const char* t = lang == 1 ? r.en : lang == 2 ? r.de : lang == 3 ? r.fr : r.es;
    return t ? t : it;
}
