Firmware in this folder of beemu.app

bios.bin            SeaBIOS (https://www.seabios.org), the build QEMU ships
vgabios-stdvga.bin  SeaVGABIOS (part of SeaBIOS), the build QEMU ships

Both are copied unmodified from the QEMU installation on the machine that built
this app (Homebrew: /opt/homebrew/share/qemu). They run inside the emulated PC;
beemu itself does not link them. SeaBIOS is free software under the GNU Lesser
General Public License version 3 (COPYING.LESSER, which builds on the GNU GPL
version 3 in COPYING). Source code: https://www.seabios.org/Download and QEMU's
roms/seabios submodule (https://gitlab.com/qemu-project/seabios), at the version
printed in the boot log ("SeaBIOS (version ...)").
