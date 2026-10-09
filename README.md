# matricX-cowork

Windows C++ version of `mouse_wander.ps1`. Moves the mouse smoothly around the
primary screen for 60 seconds and shows a smooth green edge glow
throughout the animation, plus a small "Press esc to quit" popup at the
upper-left with 10px padding. The glow has no grain, leaves the center clear,
and fades in and out. Escape restores manual movement immediately while
the overlay finishes its short fade-out.
Manual mouse movement is blocked during the animation. Press Escape to stop
early and restore manual movement. Mouse buttons, scrolling, and the keyboard
remain available.
During the run, standard Windows cursors use the supplied `akar-icons_cursor.png`,
rendered at 32 logical pixels, scaled
for the display DPI with high-quality transparent edges. The original PNG is
unchanged. Keep this image beside the executable.
The saved Windows cursor theme is reloaded on Escape, normal exit, or a console
close/Ctrl+C notification. Applications using their own custom cursors may retain those.

## Build

With MinGW-w64:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -static mouse_wander.cpp -o mouse_wander.exe -luser32 -lgdi32 -lgdiplus
```

Or from a Visual Studio Developer Command Prompt:

```bat
cl /std:c++17 /EHsc /W4 mouse_wander.cpp user32.lib gdi32.lib gdiplus.lib /Fe:mouse_wander.exe
```

## Run

```powershell
.\mouse_wander.exe
```

The program starts moving the mouse immediately. It does not click or type.

To recover the normal Windows cursors after a forced termination or an older run:

```powershell
.\mouse_wander.exe --restore-cursor
```
