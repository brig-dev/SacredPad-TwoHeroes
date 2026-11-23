# Testing SacredPad: Two Heroes

Thank you for trying the beta. One report with the log is worth more than ten "it doesn't work".

## Before you start

- Check the Requirements in the [README](README.md): with a different `Sacred.exe` only the basic controller works,
  and the mod tells you in a window at start.
- In the main menu the top of the screen must say "SacredPad 0.9.0". If it does not, the DLL is not loaded: check
  that `version.dll` is next to `Sacred.exe` (and, on Proton, the launch options).

## What to try

Alone, with one controller:

1. Walk around, open doors and go through portals just by walking into them.
2. Fight with A and with the arts on X, Y, RB, RT, LT; aim with the right stick; switch weapons with the D-pad.
3. Talk to characters and open chests with A; accept and decline in dialogues with A and B.
4. Inventory (View held), merchant, chest, blacksmith, combo master: move items, buy, sell, equip.
5. The help (Menu held) and the controller style (Xbox, PlayStation, Steam).

With two controllers (main menu > Two players):

6. Hero 2 appears, moves, fights, uses arts and potions, picks up items.
7. Both inventories open at the same time; items moved between the two heroes.
8. A merchant used by both players at once; the shared chest.
9. A quest finished together: does hero 2 get experience, a reward next to them, the points?
10. Save, quit, load: hero 2 comes back as before.
11. A hero falls: the other one revives them.

## Sending a report

1. Close the game after the problem (the log is complete only after closing).
2. Find the log in the game folder:
   - `SacredPad.log` is the latest session;
   - every earlier session is kept in the `SacredPad_log` folder, named with the date and time it ended
     (`SacredPad_20261110_164006.log`), so nothing is lost if you played again in the meantime. The newest 50 are kept
     (`[Log] Keep` in `SacredPad.ini`; 0 keeps them all).
3. Write what you did, what you expected and what happened, with the time if you can (every line of the log starts with
   the time), and a screenshot if it shows something on screen.
4. Send it as a GitHub Issue (attach the file) or in the comments on Nexus Mods. For a quick question you can paste just
   the lines around the time of the problem; lines with `ATTENZIONE` (warning) or `ECCEZIONE` (exception) are the
   interesting ones.

The log is in Italian: no problem, send it as it is. It does not contain your user name (the Windows user folder is
written as `%USERPROFILE%`, the Linux one under Proton as `\home\%USER%`). It contains the game folder, the controllers, the game windows and what the mod did.

Useful to know: Windows or Steam Deck / Steam Machine, Proton version, controller model, game language, and which
`Sacred.exe` you use (the first lines of the log say whether it was recognised).
