# beemu: BeOS emulator for Apple Silicon

Read this first. It is the handoff from the design conversation that started the project.

## Goal
Run **BeOS R5 and Dano** on Apple Silicon Macs, fast and as compatible as possible.
"No fudges" means **no custom drivers or add-ons inside the guest**: stock BeOS drivers only.
All compatibility work lives in the emulator, which emulates hardware BeOS already has drivers for.

## Decisions already made (don't relitigate without asking)
- **CPU:** JIT-only x86-32 -> ARM64 (Pentium 3-class: x87, MMX, SSE). **No interpreter.**
  Rare or complex instructions go through C helper functions called from generated code.
- **SMP:** design for N CPUs from day one (`cpu_t cpus[MAX_CPUS]`, no global CPU state). Start with 2.
  Round-robin on one host thread first; per-CPU host threads later. Needs local APIC, I/O APIC,
  MP tables, INIT/SIPI, IPIs, TLB shootdowns. LOCK-prefixed ops map to ARM64 LSE atomics.
- **Firmware:** SeaBIOS + SeaVGABIOS (QEMU's), built from source. Needs fw_cfg (ports 0x510/0x511),
  Bochs VBE "dispi" registers (0x1CE/0x1CF), PIIX3/4 IDE, PIC/PIT/RTC/CMOS/DMA, PS/2, i440FX PCI host bridge.
  Debug console on port 0x402/0x403 routed to stdout.
- **Video:** VGA + VBE 2.0+ baseline (works with BeOS's stock VESA driver, custom mode list in the
  SeaVGABIOS build). Plus an optional, off-by-default emulated high-end period card: the Matrox G400
  (`--video g400`, `core/dev/g400.c`) runs BeOS R5's own stock g400 driver and accelerant.
- **Other devices:** i440FX + PIIX3 chipset, ATAPI CD, RTL8139 or NE2000 PCI NIC, PS/2 input. Sound: Ensoniq
  ES1370 (default; R5 and Dano's es137x) and, as an option, a Sound Blaster 16 PnP for R5's awe64
  driver (R5 and Dano have no plain sb16 driver; Dano has no Sound Blaster driver at all).
- **Frontend:** SDL3 behind the thin interface in `platform/platform.h`
  (`platform_present_frame`, `platform_poll_event`, ...). Native Metal/CoreAudio can replace it later.
  The core must never include SDL.
- **Disk images:** the emulator ships with none. Users supply their own. Test with a QEMU-made BeOS disk
  converted to raw (`qemu-img convert -O raw disk.qcow2 beos.img`).
- **Host:** arm64 macOS only. 16 KB host pages vs 4 KB guest pages. JIT memory uses `MAP_JIT` plus
  `pthread_jit_write_protect_np` plus `sys_icache_invalidate` (already working here).

## Layout
- `core/cpu.h,c`      guest CPU state (`cpu_t`), reset state, lazy-flag fields, interrupt hooks
- `core/decode.h,c`   x86-32 decoder + `insn_format()` trace printer (pure function, no CPU state)
- `core/flags.h,c`    lazy EFLAGS evaluation and the 16 condition codes
- `core/mem.h,c`      physical memory bus (RAM/ROM/MMIO pages, BIOS aliasing, self-modifying-code hook)
- `core/io.h,c`       port I/O bus (IN/OUT space; unclaimed ports read all ones)
- `core/system.h,c`   system architecture: guest linear memory access, stack, descriptor loads,
                      interrupt/exception delivery and fault unwinding, far jmp/call/ret, IRET,
                      LLDT/LTR, CRs, MSRs, CPUID, TSC, I/O permission bitmap
- `core/helpers.h,c`  C implementations of the instructions the JIT doesn't generate inline
- `core/a64.h`        ARM64 instruction encoders
- `core/jit.h,c`      baseline JIT: block translator, helper-record arena, block cache, dispatcher
                      (takes hardware interrupts between blocks)
- `core/machine.h,c`  the PC: RAM, BIOS ROM, i440FX PAM shadowing, chipset PCI functions, ports
                      0x80/0x92/0x402, run loop on host time, reset, diagnostics (`machine_dump`)
- `core/clock.h`      host monotonic clock; devices take a `clock_fn` so tests can fake time
- `core/dev/`         devices (`fbrows.h`: row cache shared by the renderers): `pic` (2x 8259 + ELCR), `pit` (8254 + port 0x61), `cmos` (RTC/NVRAM),
                      `ps2` (8042, keyboard, mouse, HID->set 1), `pci` (config mechanism #1, BARs),
                      `fwcfg` (QEMU fw_cfg, port I/O), `vga` (VGA registers, Bochs VBE, PCI ROM BAR, renderer),
                      `g400` (Matrox G400 MAX at 00:05.0, off by default: 32 MB VRAM, 2D engine, DAC, cursor),
                      `disk` (raw image backend, RAM write overlay), `ide` (PIIX3 IDE: ATA disks, ATAPI CD,
                      bus-master DMA), `rtl8139` (RTL8139C NIC at 00:03.0, C-mode TX/RX, 93C46 EEPROM),
                      `ne2k` (NE2000 PCI = RTL8029, DP8390 core; at 00:04.0 next to the RTL8139 by
                      default, `--nic ne2k` / `--nic rtl8139` for just one card at 00:03.0),
                      `es1370` (Ensoniq AudioPCI at 00:06.0, default sound card), `dma` (two 8237s,
                      page registers), `sb16` (Sound Blaster 16 PnP: ISA Plug and Play, DSP, CT1745 mixer)
- `core/net.h`        network backend interface (frames in/out); the core never includes a backend
- `core/sound.h`      sound output interface: 16-bit stereo at SOUND_RATE (44100) to a callback
- `main.c`            frontend: options (`--hda..--hdd`, `--cdrom`, `--boot c|d`, `--writable`, firmware,
                      `--ram`, `--nic`, `--net`, `--mac`, `--hostfwd`, `--video`, `--sound es1370|sb16|none`,
                      `--wav FILE`, `--cpus 1-8`, `--vm`/`--save-vm`/`--list-vms`/`--menu`), SDL loop + audio,
                      `--headless`/`--screenshot`/`--script`
- `core/dev/apic.h,c` local APICs (one per CPU) + I/O APIC, only when `--cpus` > 1 (see "SMP")
- `platform/`         SDL3 backend; `png.c` screenshot writer (no SDL); `net_slirp.c` user-mode
                      networking through Homebrew's libslirp (optional at build time); `vmstore.c` saved
                      machines (plain C); `launcher_mac.m` the launcher window (AppKit, ARC, no nib)
- `tests/`            `test_decode`, `test_mem`, `test_jit`, `test_flags`; `test_sys` (guest programs in
                      `tests/asm/*.asm`, nasm); `test_alu` + `ref_alu_x86` and `test_flags_x86` (Rosetta);
                      `test_devices` (fake clock; includes RTL8139 TX/RX/EEPROM/ring wrap, NE2000 PROM/TX/RX/ring wrap, G400 fills/blit/status/ROM overlap, APIC); `test_bios` (SeaBIOS end to end, also with 2/4/8 CPUs); `test_vmstore`
- `bios/`             optional local firmware: `bios.bin`, `vgabios-stdvga.bin` (looked up before Homebrew's)
- `tools/bfs_read.py` read-only BeFS reader: list a directory or copy a file out of a raw image
                      (how the stock drivers were pulled out for disassembly)
- `tools/make_app.py` builds `beemu.app` (`cmake --build build --target app`): copies SDL3.framework and
                      the Homebrew dylibs (libslirp, glib, libintl, pcre2) into Contents/Frameworks with
                      @rpath install names, SeaBIOS + LGPLv3 texts into Resources/firmware, library
                      licenses into Resources/licenses, then signs ad hoc (not notarized)
- `resources/`        app icon (1024 px PNG, made for beemu: no Be logo), firmware license texts
- `saved-machines/`   the user's two saved machines (BeOS R5, BeOS Dano) copied out of Application
                      Support for a move to another Mac; README.txt says how to put them back

## Build and test
    brew install cmake sdl3 nasm libslirp      # libslirp: optional, for networking (QEMU pulls it in too)
    cmake -B build && cmake --build build      # RelWithDebInfo (-O2 -g) by default; -DCMAKE_BUILD_TYPE=Debug for -O0
    cd build && ctest --output-on-failure      # or run ./build/test_* individually
    ./build/beemu                              # SDL3 window: SeaBIOS boots, then "No bootable device"
    ./build/beemu --headless 3 --screenshot s.png   # no window; prints debug log, screen text, CPU state
    cmake --build build --target app           # build/beemu.app
In the app (detected by running from `*.app/Contents/MacOS`): firmware comes from Contents/Resources/firmware
first; with no disk arguments and no `--headless` it shows the machine launcher (also `--menu` anywhere),
and reports fatal errors in an alert. Finder's `-psn_` argument is ignored.
Saved machines (2026-11-05): one `NAME.beemuvm` text file each (`name`, `hda`, `cdrom`, `boot disk|cd`, `ram`,
`cpus`, `video vesa|g400`, `sound es1370|sb16|none`, `nic both|rtl8139|ne2k|none`, `keep_disk_changes`) in
~/Library/Application Support/beemu/VMs (`BEEMU_VM_DIR` overrides; tests use it). The launcher selects the
machine started last (`last-vm`); the old `last-disk` + `video` files become a machine called "BeOS" the
first time the list is empty. `--save-vm` stores absolute paths. `platform_ui_init()` (SDL_Init video)
must run before the launcher so SDL sets up NSApplication and its menu as usual; the launcher runs
`runModalForWindow` and the SDL window opens afterwards. `BEEMU_LAUNCHER_SNAPSHOT=f.png`
(+ `BEEMU_LAUNCHER_SNAPSHOT_EDIT=g.png`) renders the windows to PNGs and quits, `BEEMU_LAUNCHER_AUTOSTART=1`
starts the selection after 0.5 s: how the launcher was checked (Claude can't click it).
`test_sys` needs nasm; the x86 cross-checks need Rosetta 2 (`softwareupdate --install-rosetta`);
`test_bios` needs QEMU's firmware (`brew install qemu`, or copy the two images into `bios/`).
Each is skipped, with a message, when its tool is missing.
Sanitizers (keep this clean; undefined behavior aborts instead of just printing):

    cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
          -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1"
    cmake --build build-asan -j8 && (cd build-asan && ctest -j4)

It found two x87 bugs once made strict: SoftFloat's `s_addMagsExtF80` normalized a zero significand
for +0 + +0 (shift by 64; patched, see third_party/softfloat/VENDORED.md) and FBSTP negated INT64_MIN.

Firmware: for now beemu uses QEMU's prebuilt SeaBIOS 1.17 `bios.bin` and SeaVGABIOS `vgabios-stdvga.bin`
(LGPLv3, separate images, not shipped). Building our own (custom VBE mode list) needs the SeaBIOS source;
`i686-elf-gcc` from Homebrew is already installed on this Mac.

Debugging aids (environment variables): `BEEMU_FULL_FRAMES=1` converts and presents whole frames
(bypasses the row cache, to rule it out); `BEEMU_HEADLESS_FRAMES=1` renders frames at 60 Hz in
`--headless` (frame cost without SDL); `BEEMU_STATS=1` also prints the window loop's per-frame render and
present times. `BEEMU_STATS=1` prints JIT counters at exit (blocks translated,
code-page invalidations, full flushes, chain links) and the hottest dispatcher entry addresses;
`BEEMU_NO_FASTMEM=1`, `BEEMU_NO_CHAIN=1`, `BEEMU_NO_INLINE_FLAGS=1`, `BEEMU_NO_JUMP_CACHE=1` turn off
the inline memory path, block chaining, inline condition evaluation, the jump cache (to bisect a JIT
bug); `BEEMU_NO_INLINE_ALU=1` (shifts/MUL), `BEEMU_NO_X87_INLINE=1` (x87 moves), `BEEMU_NO_X87_JIT=1`
(x87 entry-point calls), `BEEMU_REGCACHE=1` (turns the guest register cache on), `BEEMU_NO_FPU_FAST=1` / `BEEMU_NO_FPU_NATIVE=1` (C fast paths / integer
arithmetic in them) do the same for the newer code; `BEEMU_STEP=1` translates one instruction per block;
`BEEMU_TRACE=lo-hi` (hex linear addresses) prints each block's instruction and registers to stderr;
`BEEMU_TRACE_INT=1` logs software interrupts; `BEEMU_TRACE_IDE=1` logs IDE commands, resets and
bus-master starts (plus register reads for a while after each IDENTIFY); `BEEMU_TRACE_IRQ=<n>` logs PIC
state when IRQ n changes; `BEEMU_TRACE_VEC=1` logs delivered hardware interrupt vectors (timer left out); `BEEMU_TRACE_NET=1` logs every
Ethernet frame (direction, size, IPv4 addresses/ports/protocol); `BEEMU_TRACE_NIC=1` logs RTL8139 register
accesses (not 9346CR bit-banging or ISR reads) or NE2000 register accesses (not the data port or ISR reads); `BEEMU_TRACE_SOUND=1` logs ES1370 register writes, interrupts and when the guest rewrites each half of
the DAC2 buffer (and whether with silence), and SB16 Plug and Play traffic, activation, DSP commands,
mixer writes, block ends and acks; `BEEMU_TRACE_PS2=1` logs
8042 commands and every byte the guest reads or writes; `BEEMU_DUMP=addr:len BEEMU_DUMP_FILE=f` writes guest physical memory at the end of
`--headless` (the BeOS kernel text is identity-mapped at 1 MB, so `ndisasm -b 32 -o <addr> f` works);
`BEEMU_DUMP_LIN=addr:len[,addr:len] BEEMU_DUMP_FILE=f` dumps *linear* ranges through CPU 0's page tables
to `f_<addr>.bin` (user code where a guest app crashed: combine with the stop dump's recent blocks).
`--headless` ends with `machine_dump()`: CPU state, stack words, and the last distinct blocks executed.
QEMU traces device activity too, e.g. `-trace ide_exec_cmd -trace ide_ioport_write -D q.txt`: comparing
its IDE register sequence with `BEEMU_TRACE_IDE` found the bus-master Interrupt bit problem.
Reference behavior: run the same firmware in QEMU with its debug console, e.g.
`qemu-system-i386 -M pc -cpu pentium3 -m 64 -nodefaults -vga std -display none
 -chardev file,id=d,path=q.txt -device isa-debugcon,iobase=0x402,chardev=d`, and compare.
Relocated SeaBIOS init code runs at a different address than in the ROM: subtract the delta printed in
"Relocating init from X to Y" before disassembling (`ndisasm -b 32 -o 0xE0000 bios.bin`).

## Status
**BeOS R5 boots to the desktop.** `./build/beemu --hda beos-5.raw --ram 256` (convert the QCOW2 first with
`qemu-img convert -O raw beos-5.img beos-5.raw`; guest writes go to a RAM overlay unless `--writable`).
SeaBIOS boots the disk, the loader sets VESA mode 0x11A (1280x1024x16), the kernel turns paging on, the
Tracker + Deskbar show the desktop, **18 s** after power-on (QEMU: ~40 s). The timings below were measured
with `--nic rtl8139`, where the test image (set up for an NE2000) shows a "network device (interface0)
failed to initialize" alert that served as the end-of-boot marker; with the default two cards there is
no alert (see below), so pass `--nic rtl8139` to reproduce them.

**Speed (2026-10-25).** Measured with timed screenshots (first frame with the network alert) and an
in-guest benchmark (Terminal: `time gcc -O2 -c` of an empty file; a 1000-iteration bash loop):

| step                                    | boot | gcc    | bash loop |
|-----------------------------------------|------|--------|-----------|
| before (CMake built everything at -O0)  | 46 s |   -    |    -      |
| -O2                                     | 25 s |   -    |    -      |
| code writes invalidate one page, not all| 23 s | 0.84 s | 2.45 s    |
| inline fast-TLB memory path             | 20 s | 0.54 s | 1.28 s    |
| block chaining                          | 19 s | 0.43 s | 1.01 s    |
| inline Jcc/SETcc/CMOVcc and carry       | 19 s | 0.37 s | 0.82 s    |
| jump cache (ret, indirect, cross-page)  | 18 s | 0.32 s | 0.68 s    |

Later: SoftFloat's state as plain globals instead of `_Thread_local` (`_tlv_get_addr` was 12% of an
FPU-heavy guest): GLTeapot 14.9 -> 16.9 FPS.
**GLTeapot (2026-11-06).** Benchmark: `--ram 256 --nic rtl8139 --headless 125`, the keyboard path below
to a Terminal, `/boot/beos/apps/GLTeapot &` at 88 s, screenshots at 108/113/118/123 s, read the FPS
counter in the window (it swings +-3 FPS with the teapot's angle: average four readings). 1 CPU, VESA:
25.3 before this round (x87 JIT calls), 27.0 with the new x87 forms, 30.7 with inline shifts/MUL and
native x87 arithmetic too, ~32 with the x87 moves as ARM64 code, 33-35 on a quiet Mac now (see x87 speed). Machine settings matter more: with 4 CPUs and no `hlt enabled` the idle CPUs
take turns spinning on the one host thread (15.3 FPS vs 19.1 with `hlt enabled`, 19.4 with 1 CPU,
all on the G400); on the G400 R5 runs this disk at 8 bpp, so every teapot frame is dithered to 256
colors in the guest (19.2 vs 26.2 on VESA, same machine); 16 or 32 bpp in Screen preferences avoids it.
Profile during the teapot (sample, leaf functions): before, 50% x87, 32% generated code, 10%
`helper_exec` (shifts by immediate, MUL r32, the missing x87 forms); after, `helper_exec` 0.4%,
generated code 43%, x87 ~50% of which ~20 points are call plumbing. The SDL window loop no longer blocks on vsync: it ran
the guest 12 ms and then waited for the display (up to a whole refresh on 120 Hz screens), leaving
the emulated CPU idle ~30% of the time; frames are now paced by the clock (60 Hz, no vsync wait).
Note R5's OpenGL is software (Mesa) and x87-heavy; an accelerated card would speed up 2D only.
The cache churn was the big hidden cost: BeOS keeps data in its code pages, and every such write used
to flush the whole cache (5.3 million translations in a 30 s boot; now ~370,000). Boot time has a floor:
the boot loader busy-waits on RDTSC for fixed wall-clock delays (`call` into an rdtsc routine at
0x41202C, `ret`, compare; 92% of all dispatcher entries during boot), so CPU speedups show up more
in the in-guest numbers than in boot time. Dispatcher entries during boot went from 70 million to
3.4 million with the jump cache. Remaining profile (boot): mostly generated code itself, then
`helper_exec` + `mach_absolute_time` (that loop's RDTSC) and `cpu_calc_flags` called from C helpers.

**Networking works (2026-10-25).** By default (`--nic both`) there are two cards: an RTL8139 at PCI 00:03.0
(I/O BAR 0, memory BAR 1, INTA -> PIIX3 PIRQC -> IRQ 11) and an NE2000-compatible RTL8029 at 00:04.0
(INTA -> PIRQD -> IRQ 11, MAC one higher), so a BeOS install configured for either (`rtl8139`, or
`etherpci` as QEMU installs are) finds its card; the other sits idle. BeOS R5's stock drivers and
net_server add-ons drive them. PCI INTA# of slot s goes to PIRQ (s - 1) & 3, as QEMU and SeaBIOS
compute it: SeaBIOS writes the resulting IRQ into INTERRUPT_LINE and BeOS's drivers trust it. An
earlier `s & 3` happened to agree for slot 3 only; a card in slot 4 then interrupted on an IRQ
BeOS had left masked (DHCP OFFER received, never read). The host side is libslirp user-mode NAT (QEMU's "-netdev user"): 10.0.2.15 by DHCP, gateway
and host 10.0.2.2, DNS 10.0.2.3, outgoing TCP/UDP as ordinary Mac sockets, no root. DNS queries to
10.0.2.3 never reach libslirp: `platform/net_dns.c` answers them with the Mac's own resolver
(getaddrinfo on worker threads; A and PTR, empty answers for the rest), because libslirp sends raw UDP
to the first nameserver in /etc/resolv.conf, which on the test Mac (an ISP resolver) doesn't answer at
all. Ping to the internet doesn't work (user-mode NAT limitation, as in QEMU). Firewall apps that
filter per program (LuLu on the test Mac, Little Snitch) must be told to allow beemu / beemu.app, or
outgoing connections hang at "Opening..." while DNS still works (it goes through mDNSResponder).
`--hostfwd tcp:8080:80` forwards 127.0.0.1:8080 on the Mac to the guest. Checked headless: DHCP at boot,
ping 10.0.2.2 (4-9 ms), NetPositive loads a page from a web server on the Mac, and a 3 MB download
arrives at ~2.9 MB/s with the right checksum (the 32 KB receive ring wraps ~100 times).
**NE2000 PCI for images installed under QEMU (2026-10-25).** QEMU's default NIC is ne2k_pci, so such
images (the test image included) are set up for R5's stock `etherpci` driver. The default machine has a
Realtek RTL8029 (PCI 10EC:8029, the card QEMU emulates) at 00:04.0 next to the RTL8139, so the
unmodified test image gets networking with no settings changed (`--nic ne2k` alone also works). Checked headless: DHCP lease at boot,
ping 10.0.2.2, NetPositive loads a page from `python3 -m http.server` on the Mac, and a 3 MB download
arrives at ~2.9 MB/s with the right cksum. With `--nic rtl8139` alone such images need the RealTek 8139
picked in Network preferences (or `DEVICECONFIG = rtl8139`, `DEVICELINK = /dev/net/rtl8139/0` in
~/config/settings/network).
Testing note: Claude's command sandbox blocks raw UDP to the internet; run network tests unsandboxed.
Reference runs in QEMU can be driven headless through its monitor socket (sendkey, mouse_move,
screendump; QEMU moves the BeOS pointer ~7.5% less per packet than beemu).
Device model notes (`core/dev/rtl8139.c`): registers are byte-granular (wide accesses compose; a dword
TSD write or the TSD byte holding OWN starts a send); RX records are [status, length incl. CRC, frame
padded to 60, CRC32], dword aligned; RCR.WRAP lets a record run past the ring end into the driver's
slack, otherwise it wraps; a frame that doesn't fit sets ISR.RXOVW and bumps MPC. Frames the NIC can't
take yet wait in a 128-frame queue in machine.c; the backend is polled every 0.5 ms of host time and a
halted guest wakes within 1 ms.
NE2000 notes (`core/dev/ne2k.c`): follows QEMU's ne2000.c, which this image's `etherpci` was installed
against. 32-byte I/O BAR: 0x00-0x0F DP8390 registers (pages 0-2 by CR.PS; page 3 has the RTL8029
CONFIG2/3 values), 0x10-0x17 data port (remote DMA, byte or word by DCR.WTS, dword = 4 bytes; RSAR wraps
at PSTOP; RDC when RBCR runs out, also at once for a zero count), 0x18-0x1F reset port (a read resets).
Card memory: PROM at 0x00-0x1F (MAC, then 0x57 0x57 at 14-15; every byte stored twice, so byte-mode
reads see each byte doubled and word-mode words are byte|byte<<8), packet RAM 0x4000-0xBFFF (32 KB like
QEMU; the real chip has 16 KB). RX header = [RSR, next page, length]; the length counts header + frame,
no CRC (QEMU's convention), though the next page leaves room for one. CURR never catches up with BNRY:
a frame needing as many pages as are free waits in the queue (ISR.OVW is set if it is offered anyway).
A stopped chip (CR.STP) drops frames. Not done: the Send Packet command, loopback, tally counters.

**Keyboard and mouse work.** Click in the window to capture the mouse (relative mode); Ctrl+Option releases
it, and so does switching away (all held keys and buttons are released in the guest then). On the Mac,
Cmd sends PC Alt (the BeOS Command key) and Option sends the PC Windows key (the BeOS Option key), so
Cmd+W closes a window as BeOS users expect; the mapping lives in `platform/platform_sdl3.c`. Checked
headless with `--script`: Enter dismisses the alert, Alt+W closes a Tracker window, a click opens the
Be menu. The bug was in the 8042: reading port 0x60 must drop the IRQ line before the next byte is
loaded, or the next byte raises no new edge and BeOS's handler never sees it (`test_devices` checks this).

`--script FILE` (with `--headless N`) feeds input at host-time seconds, one event per line
(`<sec> mark <label>` prints the guest CPU's busy time since the previous mark: host time not spent
sleeping in HLT, a cost measure for a fixed workload; `<sec> quit` ends the run; scripts also run in
window mode, e.g. with `SDL_VIDEODRIVER=dummy` for an invisible window):
`<sec> key <hex HID usage> [down|up|tap]`, `<sec> type <text>` (`\n` Enter, `\t` Tab, `\\` backslash;
about 0.1 s per character, so later events must leave room), `<sec> mouse <dx> <dy> [buttons]` (relative,
accelerated by BeOS), `<sec> moveto <x> <y>` (absolute screen pixels, within ~2 px: corner, then 1-count
packets at the measured ~1.92 px each), `<sec> click [left|right|middle]`, `<sec> shot <file.png>`.
Events run in order, so one that starts late delays the rest.

**Apps checked (R5, headless, 2026-10-25).** A reliable launch path is keyboard-only: Enter (dismiss the
network alert) at ~64 s, click the desktop, type `BeOS` + Alt+O, `apps` + Alt+O, `Terminal` + Alt+O, then
launch apps from the shell (`/boot/beos/apps/<name> &`). R5's bash 2 history-expands `!`, so avoid `$!`.
All of these open and behave as on hardware: StyledEdit, NetPositive (renders its local welcome page),
MediaPlayer, ShowImage, Pulse, Clock, People, DiskProbe, Expander, Magnify, 3dmiX, SoundRecorder,
CDPlayer, MidiPlayer, GLTeapot (~16 FPS), Chart, Minesweeper, CodyCam, Installer, SoftwareValet, BeIDE,
SCSIProbe, SerialConnect, PoorMan, TV, RealPlayer, `sysinfo`. Expected complaints only (before sound cards existed): no audio
server, no CD-ROM, no video input, no SCSI. gcc 2.95 compiles and runs a long long / math-library
test with exact results (`gcc -O2` of a small file: 4.5 s).

The last blocker was the PIIX bus-master status Interrupt bit: real hardware (and QEMU) set it on every
rising edge of the drive's INTRQ, DMA or not, and BeOS's IDE interrupt handler checks it to decide the
shared IRQ is its own. Without it the handler declined the interrupt, IDENTIFY timed out, and the kernel
panicked "boot device not found".

Paging is done (`core/system.c`): two-level walk, 4 MB PSE pages, PGE global pages, 1024-entry
software TLB per CPU with permission/dirty state, #PF with CR2 and error codes, accessed/dirty bits,
CR0.WP, INVLPG, flushes on CR0/CR3/CR4 writes; implicit supervisor accesses for descriptor tables,
TSS and interrupt delivery. The JIT fetches through the MMU, caches blocks by physical address, never
lets a block cross a 4 KB page, and stores the instruction's EIP before each memory helper so #PF
reports it. Multi-write instructions (PUSHA, ENTER, far CALL, CMPXCHG8B) probe all pages first.
`test_sys` "paging" (27 checks) covers remaps, PSE alias, A/D bits, not-present / WP / user faults with
restart, stale TLB until INVLPG / CR3 reload, code via an alias, SMC via an alias, fetch faults.
CPUID now also reports PSE and PGE. P6 performance counters (MSR 0xC1/0xC2, 0x186/0x187) and RDPMC
exist because the BeOS kernel programs them (event 0x79, clocks unhalted, counts with the TSC).
Fixed on the way: a PIT one-shot (mode 0) reloaded and expiring within one machine slice lost its IRQ
edge; loading a count now lowers IRQ 0 immediately like OUT does on real hardware.

**Matrox G400 (`--video g400`, off by default).** BeOS R5's stock `g400` kernel driver and
`g400.accelerant` drive it to the full desktop. The VGA card stays as the boot display; the frontend
switches to the G400 once the driver puts the CRTC in MGA mode (CRTCEXT3 bit 7). What R5's driver
needs (found by disassembling it; the binaries are not stripped):
- PCI 102B:0525; BAR 0 framebuffer (32 MB, driver maps 16), BAR 1 the 16 KB register aperture, BAR 2
  ILOAD (writes ignored), ROM BAR of zeros: with no "PInS" block the driver uses built-in clock limits.
- The driver enables the ROM BAR at the framebuffer's address and turns it off again. `pci.c` now maps
  overlapping BARs again after any unmap; before that the first 64 KB of VRAM vanished (no desktop
  icons, empty Deskbar).
- STATUS (0x1E14) must show bit 17 (ENDPRDMASTS, DMA idle) and bit 16 clear (engine idle):
  `AcquireEngine` spins on it. Interrupts: vertical line (IEN/ICLEAR bit 5) as vertical blank.
- 2D: RectangleFill/SpanFill/Invert are TRAP with zero slopes (`DWGCTL 000C7844`), started at
  LEN+0x100 (0x1D5C); scrolling is BITBLT (`040C4008`), TransBlit uses a color key. R5 sets PITCH bit 15
  (ylin): YDST is then a line address / 32 and steps by pitch/32 per line; YTOP/YBOT are line addresses.
  Operations finish when started (no FIFO, always idle).
- Display: depth from XMULCTRL (8 bpp palette, 15/16/24/32), pitch CRTC13 + CRTCEXT0 in 16-byte units.
  The DAC cursor (64x64, 2 planes of 8 bytes per line) stores each plane line as a little-endian 64-bit
  word with the leftmost pixel in bit 63. With the disk set up under QEMU, R5 picks 1280x1024x8.
  `BEEMU_TRACE_G400=1` logs register accesses (FIFOSTATUS/STATUS/INSTS1 polling left out).
- The engine runs on the guest's time (synchronously, on the CPU thread), so solid fills and plain
  copies with all planes enabled go through memset/memmove spans; everything else (ROPs, plane masks,
  color keys, 24 bpp) takes the per-pixel path. `test_devices` runs the same checks through both.
- Speed vs VESA (2026-10-25, `--nic rtl8139`, Terminal open, `mark` script command, two runs each):
  CPU-seconds busy for `cat` of a 2000-line file x3: VESA 4.2-4.6, G400 4.2-4.3; dragging the Terminal
  window 200 steps: VESA 1.8-2.2, G400 1.9-2.0 (per-pixel engine before the fast paths: 2.2-2.3). The
  same within noise: the emulated framebuffer is host RAM, so unaccelerated drawing isn't penalized as
  on real PCI hardware. Frame conversion used to cost 2.0 ms/frame for VESA 1280x1024x16 and 1.16 ms
  for the G400 at 8 bpp, on the guest's thread; see "Frames" below for what replaced it.
- The app asks VESA / Matrox G400 after the disk picker (remembered in Application Support/beemu/video;
  Return keeps the last choice); `--video` on the command line skips the question.

**Frames (2026-11-02).** The window loop runs on the guest's thread, so frame work is guest time lost.
- Renderers keep a copy of the raw guest rows (`core/dev/fbrows.h`) and convert only rows whose bytes
  changed; the key (mode, size, pitch, start address, and the palette in 8 bpp) forces a full redraw
  when it changes. 15/16 bpp convert through 64K-entry tables, 8 bpp through a per-frame palette
  table. `vga_t`/`g400_t` report the changed rows (`dirty_y0/y1`), `machine_frame_rows()` passes them
  on (the whole frame when the displayed card switches), `machine_frame_invalidate()` drops the caches.
- G400 cursor: drawn into the frame. When it moves or changes, or a row under it is reconverted, all
  its rows are converted again and it is drawn once (XGA mode inverts: never twice on a row); the
  rows it left are forced to reconvert.
- `platform_present_rows()` uploads only the changed rows and skips the present when nothing changed
  (window events and a 0.5 s refresh still present).
- `test_devices` checks 400 random changes per renderer (pixels, palette, scroll, depth, cursor
  moves/image/mode) against a full render, and that every changed row is in the dirty range;
  mutation-tested (palette left out of the key; cursor not redrawn over a changed row).
- Measured in the window loop (SDL `dummy` driver, software renderer, `BEEMU_STATS=1`, the Terminal
  `cat` + drag script): render + upload + present per frame VESA 1.85 -> 0.28 ms, G400 1.49 -> 0.21
  ms (old way: `BEEMU_FULL_FRAMES=1`, which still has the lookup tables); 80% of frames need no
  present, 47 of 1024 rows change per frame on average. Headless with a frame every 1/60 s
  (`BEEMU_HEADLESS_FRAMES=1`) the remaining cost is within run-to-run noise.
- Caveat: `mark` busy times in window mode came out ~4x the headless ones while in-guest `time cat`
  differed by only ~6% (0.72 vs 0.67 s), so don't compare window-mode `mark` numbers with headless ones.
- Not tried yet in a visible Metal window by Claude (only the dummy driver): check that partial
  uploads and skipped presents look right on screen.

**Sound (2026-11-02).** `--sound es1370` (default), `sb16` or `none`; `--wav FILE` records what the card
plays (any mode), the window also plays it through SDL audio (a queue capped at 0.25 s). Cards produce
16-bit stereo at 44100 Hz as host time passes (`core/sound.h`), read guest memory (ES1370 bus master,
SB16 through the 8237) at the guest's rate, resample linearly and return the time of their next
interrupt to the run loop, so a halted guest wakes for it.
- What R5 ships (listed with `tools/bfs_read.py`): `es137x` (ES1370/ES1371, also in Dano), `awe64`
  (Sound Blaster AWE64/SB16 PnP, R5 only), `i801` (AC'97), `emu10k`, `cs4236`, `opti931`, `sonic_vibes`,
  `ymf715`, ... No plain sb16 driver.
- ES1370 (`core/dev/es1370.c`), from R5's es137x: playback on DAC2 (page 0xC: 0x38 address, 0x3C size
  in dwords - 1, high half = play position; 0x28 samples per interrupt - 1), rate in CTRL bits 16-28
  (1411200 / rate - 2), SCTRL bits 2-3 format; interrupts acknowledged by clearing the channel's SCTRL
  interrupt enable (byte writes to 0x21). The media server keeps DAC2 running (16 KB buffer, an
  interrupt every 2048 frames = 46 ms) from boot; the handler zero-fills a half if the writer is late.
  DAC1 plays too; the ADC records silence. AK4531 master/voice/FM volumes and mutes apply.
- SB16 (`core/dev/sb16.c`): R5's `awe64` takes any audio function CTL003x/CTL004x from the ISA
  config manager, so the card is an SB16 PnP (CTL0028, audio function CTL0031). BeOS's
  `busses/config_manager/isa` runs ISA Plug and Play itself when there's no PnP BIOS (SeaBIOS has
  none): initiation key on 0x279, read port 0x203, serial isolation, CSN, then reads the resource data
  straight away. Gotcha: after isolation the resource pointer is already past the 9-byte serial id
  (isolation shifted it out); starting over at the id made BeOS parse the id as tags and drop the card.
  BeOS assigns 0x220 / MPU 0x300 / IRQ 5 / DMA 1 and 5, activates, and the driver plays 16-bit stereo
  auto-init (`B6 30 lo hi`) at 44100 Hz in 23 ms blocks, pausing (D5/D6) when its writer is late
  (silence is played while paused). Output gain 0x41/0x42 (BeOS sets x2) is treated as x2 = unity.
  No OPL3 FM, no EMU8000, MPU-401 only acks commands.
- Choppy sound (reported in the app, 2026-11-02). Two causes, measured separately:
  1. Host side: samples went to SDL as soon as they were made, so the device ran dry between bursts
     (8 dry-outs in ~30 s on the real device). `platform_audio_write` now puts 60 ms of silence in
     front of the data whenever the queue has emptied (`BEEMU_AUDIO_CUSHION_MS`, 0 = off): 1 dry-out.
     `BEEMU_STATS=1` prints dry-outs, padding and drops at exit.
  2. Guest side, the bigger one: whole 46 ms halves of silence when BeOS's mixer misses its deadline.
     Counted with `--wav` + a gap finder (runs of >= 10 ms of exact zeros inside sound) over the
     MediaPlayer MP3 script: quiet Mac, headless 3-6 gaps after the first 20 s of a 45 s stretch, a
     real Metal window 29 (most in the first 15 s of playback, while MediaPlayer starts); all cores
     busy (10 `yes` processes) ~150 either way. The host thread isn't frozen (update gaps > 10 ms are
     rare; presents never took over 3.8 ms); the guest is just too slow at those moments: ~60% of its
     time during playback is x87 (BeOS's mixer and MP3 decoder work in floating point). After the x87
     work below: quiet Mac 0-1 gaps per 45 s of steady playback; fully loaded Mac still ~160 (the
     scheduler then seems to run the emulator on efficiency cores at times).
  `platform_boost_thread()` (QoS user-interactive + an NSProcessInfo latency-critical activity, so
  App Nap and timer coalescing can't delay the emulator; `BEEMU_NO_BOOST=1` skips it) made no
  difference in the load test; it's a precaution for a hidden or occluded app window.
  Also fixed on the way: `BEEMU_STATS`' hot-address table probed up to 4096 slots per dispatch once
  full, which made the dispatcher itself the top of every profile taken with stats on.
- `dma` (`core/dev/dma.c`): two 8237s with page registers, auto-init, address decrement, masks,
  terminal count status; 16-bit channels address words in 128 KB pages. Always present.
- Checked: both cards play R5's startup sound at boot; Dano's startup sound plays on the ES1370; an
  MP3 in MediaPlayer at an idle desktop plays in real time at ~40% guest CPU. Against a host decode of
  the same MP3, 3-second windows of the ES1370 recording match exactly (correlation 1.00); the SB16's
  have a few dropouts (its 23 ms blocks are half the ES1370's, so a busy guest misses more deadlines).
  At boot and while apps start, both cards get short gaps: BeOS's mixer is late, not the card (the
  host thread is mostly asleep then). `test_devices` has sample-exact output, interrupt timing, acks,
  pause, rate conversion and mute for the ES1370; 8237 transfers; SB16 isolation, checksum, resource
  data, configuration, DSP reset/version, mixer IRQ/DMA registers, 16-bit DMA playback and acks.

**x87 is done, exact 80-bit** (decision made: soft-float, Berkeley SoftFloat 3e vendored in
`third_party/softfloat`, see its VENDORED.md). `core/fpu.c` implements all D8-DF instructions with every
exception (masked responses, #MF, #NM/TS), RC/PC, C0-C3, tags, FNSAVE/FRSTOR/FNSTENV; `core/fpu_math.c`
computes transcendentals in quad precision, FSIN/FCOS reduced with Intel's 66-bit pi. Verified by
`test_x87`: 68 exact values from 60-digit decimal arithmetic (bit-exact in all rounding modes) and about
126,000 vectors over 63 forms against x87 under Rosetta. Rosetta is NOT exact for transcendentals
(double-precision cores for FYL2XP1/FPREM1, missing flags for F2XM1 of denormals): every disagreement was
checked against exact arithmetic and ours was right; the test documents those limits. `test_sys` has an
x87 guest program (#MF, #NM, FNSAVE, FCMOV, rounding modes). CPUID now reports FPU.

**x87 speed (2026-11-02).** Measured with temporary in-process timers (not `sample`: it bills time spent
in JIT code to the last C frame, which made `fpu_exec` look like 60% of the CPU at one point and the
x87 histogram look small at another): during MP3 playback in MediaPlayer BeOS's mixer and decoder run
~10.7 million x87 instructions per second, ~60% of the busy guest CPU. Top: FLD m32 (93M per 35 s),
FMUL m32 (67M), FADDP (56M), FXCH (39M), FSTP m32 (38M), FADD/FSUB m32 (21M); control word 037F
(all masked, extended precision, nearest) 92% of the time, 0C7F for float-to-int.
- `fpu_exec` is now a small front end; the general code (`fpu_exec_general`, noinline, big frame) runs
  only when the front end can't. With every exception masked, no #MF pending and ordinary operands
  (normal or zero) it does: FLD/FST(P) m32/m64, FILD m16/32/64, FIST(P) m16/32/64 (integer code:
  bit conversion, rounding by RC, PE, C1 = rounded up), FLDCW/FNSTCW, FLD ST(i), FXCH, and
  arithmetic in all forms (D8/DC/DA/DE memory and D8/DC/DE register) by calling SoftFloat directly,
  accepting the result only if it raised nothing beyond inexact. Anything else (NaN, infinity,
  denormal, out of range, overflow, underflow, unmasked exceptions) goes to the general code, which
  repeats the instruction from scratch. `BEEMU_NO_FPU_FAST=1` turns the fast paths off.
- SoftFloat now reports whether `roundPackToExtF80` rounded away from zero (VENDORED.md), so C1 no
  longer costs a second, round-toward-zero computation for every inexact result.
- `cpu_read_lin`/`cpu_write_lin` (every C helper's memory access) check the fast TLB first and refill
  it after a slow access, like the JIT's own `h_read`/`h_write`; fixed-size copies, no library calls.
- Checks: the Rosetta cross-check covers every fast-pathed form (all masked, random RC/PC); `test_x87`
  also runs 4 million random states/operands (all RC/PC, empty slots, NaN/inf/denormal/unsupported,
  values at float and integer limits, sometimes unmasked) through both paths and requires identical
  registers, tags, status and memory. Mutation-tested (tie rounding, inverted C1).
- Result: x87 time in a 35 s playback window 8.77 -> 7.34 s; guest CPU during playback 4.22-4.28 ->
  3.52-3.56 s per 10 s; dropouts in steady playback on a quiet Mac 3-6 -> 0-1 per 45 s.
- JIT direct calls (2026-11-05): for the forms `fpu_jit_handles()` accepts, generated code
  (`tr_fpu` in jit.c) computes the linear address and calls `fpu_jit_mem(cpu, lin, off, eip,
  FOP | seg << 16)` or `fpu_jit_reg(cpu, eip, FOP)` directly: no helper record decoding, no generic
  dispatch. They share the fast-path code above with memory through the fast TLB only
  (`cpu_fast_host` in system.h), so they never fault, and they write the FPU pointer registers
  (FIP/FCS/FOP/FDP/FDS) only on success; on 1 ("declined": TLB miss, unusual operand, unmasked
  exception, stack overflow...) nothing has changed and the normal `helper_exec` call follows.
  `BEEMU_NO_X87_JIT=1` turns it off. A lean FLD m32 measured 3.8 ns vs 9.9 ns through `fpu_exec`.
  `test_x87` also calls the entry points on every random state: when they accept (1.3M of 4M cases)
  the whole result incl. pointer registers must equal the general code's, and a decline must leave
  the state byte-identical (mutation-tested: FDP off by one). Guest CPU during playback 3.51 -> 3.01 s
  per 10 s (4.25 -> 3.01 since the start of the x87 work, -29%).
- GLTeapot round (2026-11-06). More fast forms: FST(P) ST(i) (+ the DD/DF/D9 aliases), FLD/FSTP m80,
  FCHS, FABS, FLD1..FLDZ (`fpu_const`, shared with the general code), FCOM/FCOMP/FUCOM/FUCOMP ST(i),
  FCOMPP, FUCOMPP, FCOM(P)/FICOM(P) with memory, FCOMI/FUCOMI(P) (`cmp_ord`: integer compare of
  ordinary values) and FNSTSW AX (a control instruction: no pointers). `fast_reg_form()` lists the
  register forms for both `fpu_exec` and `fpu_jit_handles`. Exact integer arithmetic
  (`native_arith`): ADD/SUB/SUBR/MUL/DIV/DIVR of two normal operands at PC = 64 bits, any RC, with
  64x64->128 products, 128/64 quotients, aligned 128-bit sums with a sticky bit; it returns false (to
  SoftFloat) for zeros, an exactly-zero difference, and results out of the normal range.
  `BEEMU_NO_FPU_NATIVE=1` turns it off. `test_x87` now also compares EAX, EFLAGS and 16 bytes of
  memory, includes all new forms, and generates short significands (exact products and ties);
  mutation-tested (sticky borrow in subtraction, ties to even). Division can never produce an exact tie
  (a quotient of two 64-bit significands is never a midpoint), so that branch is unreachable.
- Teapot x87 mix (1.75 billion ops in a run, 0.6 million declined): FSTP m32, FXCH, FLD m32, FADDP,
  FLDCW (60M) + FNSTCW + FISTP (float -> int with a control word swap), FMUL m32. Mostly data moves.
- x87 moves as ARM64 code (2026-11-06, `x87_inline()` in jit.c, before the entry-point call in
  `tr_fpu`): FXCH, FLD ST(i), FST(P) ST(i) (+ aliases), FLD m32/m64, FST(P) m32, FLDCW, FNSTCW. Each
  checks first (ES pending, CR0.EM/TS, register empty/full, NaN/inf/denormal/unsupported values, fast
  TLB miss, FST m32 only with PM masked, its result in float range) and branches to
  the usual entry-point call + helper on any of them before changing state. Tags are recomputed from
  the values (FLDENV/FRSTOR can leave tags that disagree with them, and set_preg recomputes). Pointer
  registers as the general code. `BEEMU_NO_X87_INLINE=1` / `jit_set_x87_inline()` turn it off.
  `test_x87` runs every random case through generated code too (flat protected-mode copy of the
  state, instruction at the same eip, fast TLB primed) and compares everything with the general code;
  mutation-tested (FST m32 tie rounding, FXCH C1, FLD m32 bias, FLDCW ES, the zero tag).
  Teapot, 12 readings each: 26.6 (new forms only) -> 29.7 (+ inline ALU, native arithmetic) -> 31.7.
- x87 arithmetic and conversions as ARM64 code (2026-11-06, same place): FST(P) m32/m64 and
  FIST(P) m16/m32 in every rounding mode (`x_round_up`: fpu.c's round_up as CSELs on a 64-bit
  discarded part with half = 2^63), FADD/FSUB/FSUBR/FMUL with m32/m64 and in the D8/DC/DE register
  forms. The arithmetic itself (`x_arith_compute`: native_arith in ARM64, MUL/UMULH for the 128-bit
  product, a 128-bit aligned add/subtract with a sticky bit, CLZ normalization, plus exact zero
  operands: x*0, x+0, 0+0 with the RC = down sign rule) is three shared routines (add, mul, sub)
  emitted after the exit stub at jit_create and called with BL (`jit_t.x87_core`); only operand
  checks/loads and the write-back are inline. It needs PM masked, PC = 64 bits, zero or normal
  operands; exact-zero differences of nonzero operands, results outside the normal range, FIST
  results that don't fit (and -2^31 / -2^15) go to the C path. Division stays in C (no 128/64 divide).
  test_x87's generator now also sets register-arithmetic operands 0/1/2/63/64/65/127/128/129
  exponents apart (the smaller often a power of two): without it a broken sticky bit at d = 64 went
  unseen. Mutation-tested: chop, ties to even, mul exponent, d = 64 sticky, sticky borrow, FIST
  below one half and C1, FST m64 bias, SUBR operand order, normalization shift, zero product sign,
  0+0 when rounding down, zero tag, float32 denormal let through. (Two mutations are equivalent:
  the carry-out sticky bit is always 0 there, as in native_arith.)
  Microbenchmark (guest loop through the JIT, ns per iteration, C path -> inline): fld/fmul m32/fadd
  m32/fstp 47 -> 34, with a zero operand 52 -> 23; fld/fld/fmul st/faddp/fstp 50 -> 44, zero 56 -> 30.
  Teapot: no measurable change (on vs off in the same binary, interleaved: 33.75 vs 33.8; x87
  arithmetic is no longer what limits it). In the teapot 20-38% of FMUL m32 / FADD m32 / FADDP had
  a zero operand, which before this went through SoftFloat on both paths.
  Benchmark lesson: FPS readings move by 3-4 FPS with other load on the Mac (a build tool at 100%
  once made a good change look 15% slower). Compare builds interleaved, ideally the same binary with
  an environment switch, and check `uptime` first.

**Guest register cache (2026-11-06, tried; off by default, `BEEMU_REGCACHE=1`).** Up to 4 guest
GPRs per block in x25..x28 (`regcache_t` in jit.c; the frame now saves x25..x28, PROLOGUE_WORDS 7,
EPILOGUE_W0_WORDS 7). Bindings only at instruction starts (`rc_bind_insn`: operands, address
registers, ESP for stack ops, ECX/EAX/EDX implicit uses; LRU eviction), so a bound register is
current on every path; all guest state is in cpu_t at block entry and exit (`rc_flush` in the
epilogue, before chained and jump-cache exits). Around C calls: write back before callees that
read registers or can fault (`reads_gprs`: all but h_cond, cpu_get_cf, cpu_flush_flags,
fpu_jit_mem), reload after helper_exec and fpu_jit_reg (`writes_gprs`). 8/16-bit writes use BFI. A
dropped instruction (ST_FAIL) restores the bindings with its code. Correct (all suites incl. ASan
pass with it on; test_jit checks the cached code shape), but slower: interleaved in-guest runs,
cache on vs off, bash `while [ $i -lt 10000 ]` loop 6.25/5.94 vs 5.94/5.73 s, warm `gcc -O2 -c`
0.142/0.146 vs 0.133/0.135 s; teapot 32.3 vs 34.05 FPS (noise level). cpu_t loads hit L1 and stores
forward, x86 blocks are short (every Jcc ends one), so a cached register is rarely reused before
the write-back, and binding loads + write-backs + reloads around helper_exec cost more than they
save. What might make it pay: look ahead over the block and bind only registers used twice or
more, skip the load when the first access is a full write, and blocks that continue past Jcc
(superblocks), so registers stay cached longer.
Integer benchmark used (`ibench`): the keyboard path to a Terminal, then three
`i=0; time while [ $i -lt 10000 ]; do i=$((i+1)); done` and three `time gcc -O2 -c /tmp/x.c`,
screenshot at 172 s; run builds interleaved. It is much steadier than the teapot's FPS counter.

**SMP (2026-11-05).** `--cpus N` (1-8, default 1; per machine in the launcher). With N > 1 the machine
gets local APICs (xAPIC at 0xFEE00000, version 0x11, IDs 0..N-1) and an I/O APIC (0xFEC00000, 24 pins,
ISA IRQ n -> pin n, IRQ 0 also -> pin 2), CPUID reports the APIC bit, SeaBIOS starts the APs with
INIT/SIPI, counts them (CMOS 0x5F / fw_cfg), prints "Found N cpu(s)" and builds the MP table. N = 1 is
the old machine exactly (no APIC at all).
- `core/dev/apic.c`: every CPU sees its own local APIC at the same address; MMIO goes to `apic_sys.cur`,
  which the run loop sets before running a CPU. TPR/PPR, IRR/ISR/TMR, EOI (level-triggered vectors send
  EOI to the I/O APIC: remote IRR clears, a still-asserted pin delivers again), LVT, timer (bus clock
  1 GHz, divider, one-shot/periodic; `apic_update` returns the next due time so a halted machine wakes
  for it), IPIs (fixed, lowest priority, INIT, INIT de-assert ignored, SIPI, shorthands, physical / flat /
  cluster logical). The 8259 reaches CPU 0 through LINT0 ExtINT (virtual wire) or when the APIC is
  globally disabled (`pic.out` -> `apic_set_pic`). `BEEMU_TRACE_APIC=1` logs IPIs, deliveries, writes.
- Run loop (`machine_run`): round robin on one host thread, slice = BLOCKS_PER_SLICE / 4 when N > 1.
  CPUs waiting for SIPI, or halted with nothing deliverable, are skipped; devices update between CPUs;
  the thread sleeps only when no CPU ran. PAUSE ends its block when N > 1 (`jit_set_smp`) so spin-waits
  give the other CPUs their turn. INIT resets a CPU (`cpu_init_ipi`), SIPI starts it at vector << 12.
- BeOS R5 starts the APs itself (INIT/SIPI again), uses the local APIC timer one-shot (divide 4, vector
  0x60) and flat logical destinations (LDR = 1 << cpu), writes TSC = 0 on every CPU. sysinfo shows N
  Pentium IIIs, Pulse/top show the load spread. Dano boots with 2 and 4.
- Idle cost: R5's idle loop (0x129777 -> [0x187704] = 0x159D0C) halts only if byte [0x1876F0] is set,
  which is true on one CPU and false on SMP unless the stock kernel settings file
  (/boot/home/config/settings/kernel/drivers/kernel) says `hlt enabled`. Without it idle APs spin and
  the host thread is 100% busy; with it an idle desktop costs 10% (2 CPUs) / 13% (4) vs 14% for 1 CPU.
  That is a guest setting, not a driver, so it is within "no fudges"; the launcher's edit sheet says so.
- No speedup yet: all CPUs share one host thread. LOCK still needs nothing (CPUs switch only between
  blocks); per-CPU host threads will need LSE atomics and real TLB shootdowns (the guest's own
  shootdown IPIs + INVLPG already work).
- Seen on the way, not SMP-specific: `shutdown -r` in R5 ends in a kernel debugger page fault in
  input_server (eip=1), with 1 CPU too.

**SeaBIOS POSTs.** QEMU's SeaBIOS 1.17 runs on the emulated i440FX/PIIX3 machine: PCI enumeration and BAR
assignment (same addresses as QEMU), MP/SMBIOS/PIR tables, TSC calibration against the PIT, SeaVGABIOS
from the VGA ROM BAR (VBE detected through the Bochs dispi registers), text mode with the banner on screen,
ATA controller probing, PS/2 keyboard init, then "No bootable device" (no disks yet). From reset to that
point takes about 0.1 s of host time. Keyboard input reaches SeaBIOS (ESC opens its boot menu).

Done and passing on the Mac (11 ctest suites, also clean under strict ASan + UBSan): IDE/ATAPI (`test_sys` "ide":
43 in-guest checks covering IDENTIFY, PIO read/write, READ MULTIPLE, LBA48, errors, bus-master DMA,
ATAPI INQUIRY/READ CAPACITY/READ(10)/REQUEST SENSE, ATAPI DMA; the image file must stay untouched),
SDL3 window, CPU state,
decoder (46 tests), memory bus (31), JIT slices 1, 2a and 2b (`test_jit`, 167 checks, including fast-TLB, chaining, jump-cache, inline-flag and code-page invalidation cases), system tests
(`test_sys`: two self-checking guest programs with 129 in-guest checks, plus triple fault and REP
resumption), and two cross-checks against real x86 behavior via Rosetta 2:
- `alu_vs_x86`: about 240,000 random vectors over 84 instruction forms (incl. immediate shifts, 3-operand
  IMUL, and MUL/IMUL/shift followed by ADC or SETC to check the lazy CF they leave) (ALU, INC/DEC/NEG/NOT, all shifts
  and rotates, SHLD/SHRD, MUL/IMUL/DIV/IDIV, BT*, BSF/BSR, XADD, CMPXCHG) run natively by
  `ref_alu_x86` and through our JIT; registers and every flag the manual defines must match.
  It already caught one bug (CMPXCHG flag direction).
- `flags_vs_x86`: the 453,596-check lazy-flags cross-check from `test_flags`, built for x86-64.

What runs now: the whole Pentium III integer ISA except BCD ops, BOUND, ARPL, LAR/LSL, VERR/VERW,
SYSENTER/SYSEXIT, and x87/MMX/SSE (those end the block with `JIT_EXIT_UNSUPPORTED`). Real mode and
protected mode at all four rings, with segmentation, IDT/IVT interrupts and exceptions (error codes,
inter-privilege stack switch via a 32-bit TSS), far transfers, IRET, I/O permission checks.

How the JIT works (comments at the top of `core/jit.c`'s translation section have the details):
- Block prologue saves x19..x24 and x30; `cpu_t*` lives in x19. w20..w24 hold values across helper
  calls, w9..w12 are short-lived scratch. Every block exit runs the epilogue.
- Inline: mov, ALU ops, inc/dec/not/neg, xchg, movzx/movsx, lea, push/pop, near call/ret/jmp, leave,
  Jcc, LOOP/LOOPE/LOOPNE/JCXZ, SETcc, CMOVcc, flag-bit ops, SHL/SHR/SAR by an immediate or CL (all
  sizes; a CL count of 0 branches around the whole thing and leaves the lazy op unknown), MUL/IMUL r/m32
  and IMUL r32, r/m32 [, imm] (UMULL/SMULL; lazy flags `CC_OP_MUL32`: cc_src1 = overflow, so CF = OF
  from it, SF/ZF/PF from the low half). `BEEMU_NO_INLINE_ALU=1` sends them back to `helper_exec`. Jcc/LOOP end the block with two exits;
  conditions come from `cpu_eval_cond` (lazy flags).
- Everything else goes through `helper_exec(cpu, rec)` (`core/helpers.c`): the decoded instruction is
  copied into a record in a per-JIT arena. A nonzero return ends the block with that exit code.
  `helper_supports()` decides what goes there and which instructions must end the block (control
  transfers, mov to SS/CR0, popf, lmsw, ...).
- Guest registers stay in `cpu_t`; 8/16-bit access uses LDRB/LDRH/STRB/STRH on the right byte.
- Memory: generated code first checks the per-CPU fast TLB (`ftlb` in `cpu.h`, 1024 entries indexed
  by linear page, separate read/write x user/supervisor tags, host address = addend + linear). A hit
  is a plain ARM64 load/store (~12 instructions); a miss or a page-crossing access calls
  `h_read`/`h_write`, which go through `cpu_read_lin`/`cpu_write_lin` (MMU) and then refill the entry.
  Only RAM is ever in it (no MMIO), and write tags never cover ROM or pages holding translated code,
  so those writes always reach the bus and its code-write check. It is flushed with the MMU TLB (CR3,
  CR0/CR4 changes), per page on INVLPG, on any memory-map change (`mem_set_map_change_cb`), and loses
  its write tags for a page that becomes code. No segment limit checks yet.
- Faults: helpers set `cpu->eip` to the instruction, then `cpu_fault()` delivers the exception and
  `longjmp`s to `jit_run`, abandoning the block. Inline memory accesses store the instruction's eip
  before calling their helper. Fault during delivery = #DF; again = shutdown.
- The block cache key is (physical address, CS base, EIP, code32, stack32, CPL==3); blocks never cross a
  4 KB page. CS:EIP is in the key because translations embed absolute eips (call return addresses,
  branch targets): BeOS maps shared add-on text into teams at different bases, and keying by physical
  address alone once ran a block translated at 0x8000A000 in Deskbar at 0xEA57B000, so `call next;
  pop ebx` jumped into the heap (Deskbar crashed whenever networking was restarted).
  Exit codes: `JIT_EXIT_CONTINUE` (0) is internal. Blocks live in a pool (hash buckets + a list per
  physical page); each page also has a 64-bit mask of the 64-byte chunks holding translated bytes. A
  write to a code page invalidates that page's blocks only if it hits a marked chunk. Full flushes
  happen only when the code buffer (64 MB) or the pool (32K blocks) fills, or the memory map changes.
- Block chaining: direct jumps (jmp/call rel, both sides of jcc/loop, falling off a full block) to
  an eip in the block's own linear page end with a patchable slot; the dispatcher links it to the next
  block's body once seen. Each chained exit stores eip, decrements `cpu->chain_budget` (so jit_run's
  block budget and time slices stay intact) and leaves when an interrupt is deliverable or a device set
  `exit_request`. Same-page targets keep links valid in every address space; an invalidated block's
  first body word becomes a jump to a shared exit stub, so stale links fall back to the dispatcher.
- Jump cache (`cpu->jc`, 4096 entries): `ret`, indirect jmp/call, and direct jumps to another page
  (which can't be linked permanently) hash the target's linear address and branch to the cached body
  when lin and block mode match; misses return to the dispatcher, which fills the entry for every
  block it finds. Cleared with the fast TLB (CR3/CR0/CR4 writes, memory-map changes), per page on
  INVLPG, and on full flushes (bodies are reused). Entries for invalidated blocks are harmless (their
  body jumps to the exit stub).
- Inline flags: the translator tracks which lazy-flag op the last inline instruction set
  (`ctx.cc_known`; C helpers and CLC/STC/CMC reset it). Jcc/SETcc/CMOVcc/LOOPcc then rebuild ARM
  flags with one CMP (SUB/CMP/NEG), CMN (ADD) or TST (logic ops; ZF/SF for anything) on the stored
  fields, narrow sizes shifted to bit 31, and CSET. INC/DEC/ADC/SBB get the old CF the same way.
  Parity and the remaining CF/OF cases call `h_cond`/`cpu_get_cf`. `test_jit` checks all 16
  conditions for 12 ops x 3 sizes x 169 operand pairs against `cpu_eval_cond`.
- Long REP string ops stop after 64K iterations and resume in a new block (eip stays on the instruction).
- CPUID reports a Pentium III (family 6 model 7) with FPU, PSE, TSC, MSR, CX8, PGE and CMOV. Turn on
  MMX/FXSR/SSE/APIC/MTRR/SEP in `system.c` as each is implemented.

## Next steps (in order)
1. No app tested so far hits an emulator bug (list under Status). Next candidates for speed: the
   common x87 forms are ARM64 code now (see "x87 speed"; the teapot no longer gains from x87 work);
   the guest register cache was tried and is slower as built (see "Guest register cache": it would
   need per-block lookahead and superblocks first); keeping flag operands in registers instead of
   cpu_t, cheaper RDTSC. Measure with the in-guest integer benchmark, builds interleaved. Input leftovers: IntelliMouse wheel
   (the mouse answers ID 0 to BeOS's 200/100/80 probe now), Mac ISO keyboard key swaps if needed.
2. A few inline instructions still change a register before a later memory write can fault (POP to
   memory updates ESP first, LEAVE sets ESP before its pop): make them restartable if a guest needs it.
3. Raise #UD for invalid opcodes (now `JIT_EXIT_INVALID`) and #MF for unmasked x87 exceptions.
4. MMX and SSE (BeOS sets CR4.OSFXSR, so FXSAVE/FXRSTOR too), then CPUID bits for them.
5. Sound leftovers if wanted: OPL3 FM and MIDI on the SB16, ES1371/AC'97. Per-CPU host threads (the
   only way extra guest CPUs add speed; needs LSE atomics for LOCK, TLB shootdowns, a thread-safe
   device side), more optimizations (see 1). The R5 `shutdown -r` input_server crash. G400: more 2D ops if a guest uses them (lines, host-to-screen ILOAD), 3D later.

## Conventions and gotchas
- Verification: when something has a hardware ground truth, test against it. Two ways here:
  inline asm on x86-64 (`test_flags`), and x86-64 reference programs run under Rosetta 2
  (`ref_alu_x86` feeds `test_alu`). To cover a new instruction form, add a line to
  `tests/alu_forms.h`. Compare only flags the manual defines (see `defined_flags()` in `test_alu.c`).
- Guest-level tests: write them in `tests/asm/*.asm` with the `CHECK` macro from `check.inc`; add the
  program to the `foreach` in CMakeLists.txt and a `run_guest()` line in `test_sys.c` with the exact
  number of CHECKs it should run.
- ARM64 code generation can only be executed on the Mac, not in Linux x86 sandboxes. `test_jit` skips
  its execution section on non-ARM64 hosts. Translation structure tests run anywhere.
- JIT encodings: the ARM64 encoders in `core/a64.h` are checked against known words in `test_jit`.
  Add a known-good word for any new encoder.
- Lazy flags state layout is documented above `cc_op_t` in `core/cpu.h`. SHL/SHR/SAR, CMPXCHG, XADD
  and string compares stay lazy; rotates, SHLD/SHRD, MUL/IMUL, BT*, BSF/BSR compute flags eagerly
  and set `CC_OP_EFLAGS` (MUL/IMUL in generated code use `CC_OP_MUL*` instead; same flag values).
  CMPXCHG's flags are those of `CMP dest, accumulator`.
- `decode_insn()` returns OP_UNSUPPORTED with the correct length for x87/MMX/SSE so decoding never loses sync.
- A test-writing slip once failed the JIT execution tests: when patching bytes in a test program, check the
  offset against the opcode layout (patch the immediate, not the opcode).
- x87: exact 80-bit via SoftFloat (decided). SoftFloat users must see the same `LITTLEENDIAN` and
  `THREAD_LOCAL` as the library (CMake exports them); a mismatch once corrupted memory silently.
  C1 ("rounded up") costs a second computation for inexact results; making it lazy is a possible
  speedup. VGA planar mode 12h (BeOS's early boot screen) still renders gray.
- Interrupts are taken between blocks, so a block ends one instruction after STI (SeaBIOS's
  `sti; nop; cli` window) and after any inline store that hits translated code (the BeOS loader patches
  its own `int N`). Stores done by C helpers (string ops, pushes in helpers) don't end the block yet.
- Debug aids: `BEEMU_TRACE_INT=1` logs software interrupts; `BEEMU_DUMP=addr:len BEEMU_DUMP_FILE=f`
  dumps guest memory at the end of `--headless`; the dump shows PIC/PIT state.
- Known decoder gaps: BCD ops, bound, arpl, lar/lsl, verr/verw, sysenter decode as unsupported.
- LOCK is accepted on lockable instructions with a memory destination (invalid uses exit as
  `JIT_EXIT_INVALID`). That is correct while all guest CPUs share one host thread and switch only
  between blocks; per-CPU host threads will need LSE atomics. REP on non-string instructions is ignored.
- libslirp upgrades (Homebrew) leave a stale path in the CMake cache: `cmake -B build -U 'SLIRP*'
  -U 'pkgcfg_lib_SLIRP*'` (same for build-asan).
- Not emulated yet: CLI/STI IOPL checks (inline, no #GP at CPL 3), the STI / MOV SS interrupt shadow,
  TF single-step and debug-register breakpoints, segment limit/type checks on data accesses, call gates,
  task switches, VM86 and 16-bit TSSs (the last few stop with `JIT_EXIT_UNSUPPORTED`), CR0.PG
  (`mov cr0` with PG stops with `JIT_EXIT_UNSUPPORTED`). Exception escalation is simplified: any
  fault while delivering one becomes #DF.
- Self-modifying code: a write that hits translated bytes invalidates that physical page's blocks; the
  running block stops after the current instruction (`smc_hit`). Helper records are never freed on
  flush (a block may be running), only reused by later translations. Code-buffer writes nest
  (`write_begin`/`write_end` keep a depth) because a page invalidation can happen mid-translation.
- A `pci_dev_t`'s `opaque` belongs to its device (the NIC, the IDE controller, ...); only the PIIX3's is
  the machine. Machine-side `written` callbacks for a device must get back to the machine some other
  way (the NICs' `intx_opaque`). Wiring the NICs to the PIIX3's callback once made beemu write
  through a stray pointer into the PIC after BeOS enabled the card; it went unseen for a while because
  the NIC lives inside machine_t (ASan can't flag that), now `pci_routing_written` asserts its device.
- Mutation-testing a fix (break it, check a test fails, restore): rebuild with `--clean-first` after
  restoring. Make compares timestamps at one-second resolution here, and a restore within the same
  second once left a test binary linked against the broken library (test_bios "failed" for no reason).
- SMP note for later: the JIT keeps a list of the CPUs that ran on it (`register_cpu`) to fix their fast
  TLBs when a page becomes code; with per-CPU threads that needs a shootdown instead.
- Memory RMW ops write memory before recording flags; when page faults arrive, make sure a faulting
  instruction leaves all guest state (including the lazy-flag fields) untouched.
- Machine-level simplifications (fine for SeaBIOS; revisit if a guest needs them): A20 is never gated;
  PAM "write-only" shadow is treated as read/write; SMRAM is not decoded; the VGA legacy window maps VRAM
  linearly (no planes), so planar 16-color modes render as a gray screen; VGA BAR 2 (MMIO registers) is
  sized but not decoded; no ACPI PM device (SeaBIOS times with the TSC instead); the machine is SMP only with `--cpus` > 1.
- SeaBIOS tries "Floppy" before "Hard Disk" when there are no drives at all: with an empty boot list its
  fallback adds floppy first. QEMU differs only because its kvmvapic option ROM adds a boot vector.
  With real disks the CMOS boot order (hard disk, CD-ROM, floppy) applies.
- fw_cfg selectors: bit 14 is the write flag, bit 15 selects the arch-local namespace (0x8000 = legacy
  ACPI tables). Masking with 0x3FFF once turned 0x8000 into the "QEMU" signature item.
- OUT/OUTS end their block so device side effects (IRQs, reset requests via `cpu->exit_request`)
  take effect before the next instruction.
- Disks: raw images only (QCOW2 is detected and rejected with the conversion command). Guest writes go to
  an in-memory overlay unless `--writable` is given, so the user's image is never modified by default.
  CD images are always read-only. IDE commands complete instantly (no BSY phase); data goes straight
  between the image and guest memory.
- IDE register reads for a missing slave on a populated channel return 0 (like QEMU); a channel with no
  devices is not claimed at all, so its ports float (0xFF) and SeaBIOS skips it quickly.
- Licensing: SeaBIOS is LGPLv3. Ship it as a separate image; publish any modifications.
