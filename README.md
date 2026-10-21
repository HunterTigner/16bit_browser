# Pixel16 Web OS

Pixel16 draws its entire old-school desktop and app windows in a low-resolution
canvas rendered by the [WebAssembly kernel](./pixel16-kernel.c). The HTML page
contains only the canvas and [small JavaScript browser bridge](./pixel16-host.js)
needed for mouse/keyboard/clipboard input, persistent local storage, and opening
website tabs. The standalone compiled kernel is [pixel16.wasm](./pixel16.wasm).
Buttons, menus, file handling, the editor, calculator, terminal, boot screen,
pixel graphics, and system screen are all drawn and handled inside WebAssembly.
The kernel implements a 16-bit memory space, a process table, and the virtual
C: drive. The canvas uses a custom 16-pixel retro arrow cursor.

Open **SYSTEM** from the desktop to choose mouse or keyboard navigation. Press
Enter or click the setting to change it; the choice is saved in the browser.
Keyboard mode uses arrow keys and Enter for desktop/menu navigation;
text-entry keys remain available in either mode.
Open **TERMINAL** from the desktop or Start menu for simulated commands:
`HELP`, `DIR`, `TYPE`, `ECHO`, `CLS`, `VER`, `MEM`, `EXIT`, `CD`, `PWD`,
`DRIVE`, `DISK`/`DISC`, `COPY`, `DEL`, `REN`, and `FIND`, plus aliases.
`CD D:` selects a virtual drive. `DISK ADD [MB]` creates an empty virtual
drive; `DISK REMOVE D:` removes one after confirmation. `DISK CUT D:` shows
the maximum number of partitions, and `DISK CUT D: 3` requests that many
empty partitions; type `YES` to confirm or `NO` to cancel. `C:` is protected
and stores Pixel16 files; added virtual drives are empty metadata-only
volumes. The virtual drive list is saved in browser storage. These commands
cannot access or modify real computer disks.

`FET C:\ NAME.EXT https://example.com/file.html` downloads a public HTTP(S)
text file into the root of Pixel16's virtual C: drive. Names are shortened to
10 letters and extensions to 4 letters; an omitted extension is taken from the
URL. HTML and Markdown text are supported. Downloads are limited to 8 KB,
require the remote server to allow browser CORS access, and do not follow
redirects. Other virtual drives and computer filesystem paths are not
download destinations.

## Run it

From this folder, run:

```powershell
py -m http.server 8000
```

Open <http://localhost:8000/> in your browser. A local server is necessary
because browsers do not let an HTML `file://` page fetch a separate
WebAssembly module.

## Rebuild the kernel

Use a Clang toolchain with the `wasm32` target and `wasm-ld`:

```powershell
clang --target=wasm32 -O2 -fno-builtin -nostdlib -Wl,--no-entry -Wl,--export-all -Wl,--export-memory -Wl,--initial-memory=20971520 -Wl,--max-memory=33554432 -Wl,--strip-all -o pixel16.wasm pixel16-kernel.c
```

WebAssembly runs in a browser sandbox, not as a bootable hardware operating
system. It cannot reserve real computer RAM, access a physical drive, or display
a third-party webpage inside the canvas. The browser bridge saves virtual disk
files to local storage and opens requested websites in a separate browser tab.
