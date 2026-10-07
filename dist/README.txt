GModLight - Garry's Mod's physgun, weapons and spawn menu inside Dying Light
===========================================================================

Dying Light stays the real game. Garry's Mod runs hidden next to it, and its
physgun, weapons, spawn menu and hands are drawn into Dying Light's view. Grab
and throw zombies with the physgun, shoot them with Garry's Mod weapons, fire
rockets into Dying Light's streets.

NEEDS
  - Dying Light (Steam)
  - Garry's Mod (Steam), on the 64-bit branch:
      Steam > Garry's Mod > Properties > Betas > "x86-64 - Chromium + 64-bit binaries"
  - Windows 10/11, Steam running

INSTALL
  1. Close Dying Light and Garry's Mod.
  2. Unzip this folder anywhere and double-click Install.bat.
     (It finds both games through Steam. If it can't, open PowerShell here and run:
      powershell -ExecutionPolicy Bypass -File install.ps1 -DyingLight "D:\...\Dying Light" -GarrysMod "D:\...\GarrysMod")
  3. Start Dying Light from Steam. Garry's Mod starts by itself, hidden, at
     Dying Light's main menu. Load into the game and press F2.

  Updating: download the new version and run its Install.bat. Your settings
  (GModLight.ini in the Dying Light folder) are kept.

UNINSTALL
  Double-click Uninstall.bat.

PLAY SINGLE PLAYER ONLY
  Set Dying Light's game visibility to private and don't join or host co-op
  with this installed.

CONTROLS
  F2              Garry's Mod hands on/off (physgun and weapons)
  F1 / hold Q     Spawn menu
  hold C          Context menu
  Mouse buttons   Fire / physgun grab and freeze
  Mouse wheel     Weapon wheel; pushes/pulls what the physgun holds
  1-0             Weapon slots
  R, E, Z         Reload (unfreeze), use (rotate with the physgun), undo
  F7              Hide/show everything Garry's Mod draws
  F9              Show the zombie stand-in boxes (debug)
  Keys and options: GModLight.ini in the Dying Light folder.

IF SOMETHING GOES WRONG
  - Logs: "Dying Light\GModLight.log" and "GarrysMod\garrysmod\gmodlight.log".
    Please attach both when reporting a problem.
  - Stutter: GModLight caps Dying Light at 60 fps ([Performance] MaxFPS in
    GModLight.ini); lower it if your PC struggles.
  - Garry's Mod crashes on start: too many Workshop addons can do that. GModLight
    starts it with -noworkshop, but if you start Garry's Mod yourself first,
    close it and let Dying Light start it.
  - Another mod uses xinput1_3.dll in the Dying Light folder: the installer stops
    rather than replace it. Move that file away first.

Updates, source and problem reports: https://github.com/GooseMcGee/DyingLight-GMod
