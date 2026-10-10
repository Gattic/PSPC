# plugins

This folder is part of pspc's memory stick: whatever is here appears in `ms0:/`, next to
`<pspc dir>\ms0`. Put M33 plugins here the way you would on a PSP running 4.01 M33.

```
plugins\
  seplugins\
    game.txt          plugins for games and homebrew
    vsh.txt           plugins for the XMB (pspc --vsh)
    pops.txt          plugins for PS1 games
  darkfrost\
    darkfrost.prx     a plugin, with the folders it uses for its own files
```

Each list has one plugin per line, as an `ms0:/` path:

```
ms0:/darkfrost/darkfrost.prx 1
ms0:/seplugins/other.prx 0
```

A trailing `1` or `0` turns a plugin on or off. A line without one is on, unless
`seplugins\conf.bin` (M33's recovery-menu settings, 5 bytes per list) says otherwise. You don't
need a Pandora battery or the recovery menu: listed plugins load when the game starts. The
launcher's **Plugins** button lists them with checkboxes, adds a PRX (copied into `seplugins\`)
and removes entries.

Files a plugin writes (cheat codes, RAM dumps, screenshots, settings) go into its folder here,
because that's where `ms0:/darkfrost/...` points. Folders that exist in both places resolve to
`<pspc dir>\ms0` first.

`pspc.ini` settings: `plugins=0` stops loading plugins, and `plugins_dir=<folder>` uses another
folder. On the command line, use `--no-plugins` or `--plugins <folder>`.

Plugins with kernel-only buttons: VOL+ = Page Up, VOL- = Page Down, NOTE = N, SCREEN = B. On a
controller, click the right stick for VOL+ and the left stick for VOL-.

DarkFrost works as on a PSP: in the game it shows "Double Tap the HOME button then press Vol +, -".
Press HOME (Esc, or the controller's guide button) twice to open and close the "Quit the game?"
screen (DarkFrost borrows its screen buffer), then VOL+ and VOL- together (Page Up + Page Down, or
both stick clicks) to open DarkFrost.

Everything in this folder except this README is ignored by git.
