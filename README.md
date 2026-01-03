# pocketbook-gopher-browser
Mostly vibe-coded gopher browser for pocketbook e-ink bookreader

## Build

```sh
export FRSCSDK=$HOME/path/to/pocketbook-sdk/FRSCSDK

${FRSCSDK}/bin/arm-none-linux-gnueabi-g++ ./gopher_browser.cpp -o gopher-browser.app -linkview 2>&1
```

## Install

Copy `gopher-browser.app` to `applications` directory on the device.
