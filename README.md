# beemu

BeOS R5 / Dano emulator for Apple Silicon. JIT-only x86-32 -> ARM64, SMP-capable,
stock BeOS drivers only in the guest. Firmware: SeaBIOS + SeaVGABIOS.

## Build (macOS, Apple Silicon)

    brew install cmake sdl3 nasm qemu   # qemu: for its SeaBIOS / SeaVGABIOS images (and libslirp, for networking)
    cmake -B build
    cmake --build build
    ./build/beemu                                   # SeaBIOS, no disks
    qemu-img convert -O raw beos-5.img beos-5.raw   # QCOW2 -> raw (sparse)
    ./build/beemu --hda beos-5.raw --ram 256        # guest writes are discarded unless --writable

Networking: an RTL8139 and an NE2000 PCI card with user-mode NAT (DHCP, gateway 10.0.2.2 = the Mac,
DNS 10.0.2.3). BeOS uses whichever card its Network settings name, so images installed under QEMU
(NE2000) and ones set up for the RealTek 8139 both work unchanged; `--nic rtl8139|ne2k` keeps one card.
`--hostfwd tcp:8080:80` forwards a Mac port to the guest; `--net none` / `--nic none` turn it off.

Video: VESA through SeaVGABIOS by default. `--video g400` adds an emulated Matrox G400 MAX that BeOS R5's
own g400 driver accelerates (2D fills and blits, hardware cursor); pick resolution and depth in Screen prefs.

Processors: `--cpus 1-8` (default 1). BeOS R5 and Dano boot with all of them (local APICs, I/O APIC, MP table).
They all share one Mac core for now, so more CPUs don't make BeOS faster yet. Idle BeOS CPUs spin unless the
guest is told it may halt them: add the line `hlt enabled` to `/boot/home/config/settings/kernel/drivers/kernel`
inside BeOS (a stock kernel setting), or a multi-CPU machine keeps one Mac core busy.

Saved machines: `--save-vm NAME` stores the machine the other options describe (disks, RAM, CPUs, cards) and
`--vm NAME` starts it (options given with it override its settings); `--list-vms` lists them, `--menu` opens
the launcher window. They are small text files in `~/Library/Application Support/beemu/VMs`.

Sound: an Ensoniq AudioPCI ES1370 (BeOS R5 and Dano's es137x driver) plays through the Mac's audio output.
`--sound sb16` swaps in a Sound Blaster 16 PnP for R5's awe64 driver instead (R5 only), `--sound none` removes it,
and `--wav out.wav` records what the guest plays.

In the window: click to capture the mouse, Ctrl+Option to release it. Cmd is the BeOS Command key,
Option is the BeOS Option key.

## Mac app

    cmake --build build --target app       # -> build/beemu.app (drag it to /Applications)

Double-click it: the launcher lists your machines. New… sets one up: a raw BeOS disk image and/or a CD
image (.iso), what to start from, memory, 1-8 processors, video card (VESA, or the Matrox G400 for BeOS
R5's g400 driver), sound card, network card, and whether BeOS's disk writes are saved into the image
(off by default: they are discarded on quit). Select a machine and press Start (or Return, or double-click).
Edit…, Duplicate and Delete (which removes only the settings, never the images) do what they say.
The disk the app used to remember becomes the first machine. The app carries SDL3, libslirp and SeaBIOS with their
licenses, and is signed ad hoc, not notarized: on another Mac, right-click > Open the first time.
With a per-app firewall (LuLu, Little Snitch), allow beemu's outgoing connections, or web pages in
BeOS hang at "Opening..." (a rebuilt app may need allowing again).

## Layout
- core/      CPU state, decoder, JIT, MMU, devices (no SDL includes)
- platform/  SDL3 backend behind platform.h, saved machines (vmstore.c), launcher window (launcher_mac.m, AppKit)
- bios/      SeaBIOS / SeaVGABIOS sources and VBE mode table
- tools/     make_app.py (builds beemu.app), bfs_read.py (copy files out of a BeOS image); resources/: app icon, firmware license texts
- tests/     unit tests, nasm guest programs (tests/asm), x86 cross-checks under Rosetta 2

## Build order
1. Platform window + test pattern  (done)
2. Guest CPU state + decoder       (done: core/decode.c, tests/test_decode.c)
3. Baseline JIT (done: integer ISA minus BCD and a few rare ops, inline + C helpers; core/jit.c, helpers.c, system.c)
3b. Lazy flags (done: core/flags.c, validated against real x86 hardware, used by the JIT)
4. Memory bus (done: core/mem.c) + MMU + software TLB
5. Devices + fw_cfg until SeaBIOS reaches POST (done: core/machine.c, core/dev/)
6. IDE/ATAPI disks, exact x87, paging (done): BeOS R5 boots to the desktop on one CPU
7. VBE modes, input (keyboard and mouse done), network (RTL8139 or NE2000 PCI + user-mode NAT done), sound (ES1370, SB16 PnP done)
8. SMP (done: up to 8 CPUs, `--cpus`): local APIC, I/O APIC, MP tables, INIT/SIPI, IPIs; one host thread for all CPUs
9. Dano (boots, also with several CPUs)
10. Optimizations: block chaining, inline TLB fast path, inline flags, jump cache (done); host register mapping
11. More cores, optional per-CPU host threads
12. Optional high-end video card (Matrox G400 with its 2D engine done: `--video g400`)
