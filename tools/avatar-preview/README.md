# M5Stack-Avatar SDL preview

This desktop preview uses the installed official `M5Stack-Avatar` library. It
cycles through the retained built-ins (`neutral`, `happy`, `sleepy`) and all
17 SVG-derived expressions without flashing the StackChan.

## Install once

```sh
brew install sdl2-compat platformio
```

## Build

```sh
cd /Users/gongxiyue/Documents/StackChan/tools/avatar-preview
pio run
```

## Run

```sh
.pio/build/native/program
```

Close the SDL window, or press Control-C in the terminal, to stop.
