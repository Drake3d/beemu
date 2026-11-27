; IDE / ATAPI device test (32-bit flat mode, set up by the harness).
; Primary master: 1 MB hard disk image, every dword of sector s = 0x5A000000 | s.
; Secondary master: 16-sector ISO image, every dword of block b = 0xCD000000 | b.
; Bus-master registers at 0xC000 (BAR 4 programmed by the harness).
bits 32
org 0x10000
%include "check.inc"
%define PRI 0x1F0
%define SEC 0x170
%define BM  0xC000

start:
    ; ---- IDENTIFY DEVICE ----
    mov ebp, PRI
    xor eax, eax
    xor ecx, ecx
    mov bx, 0xE0EC          ; bh = device (LBA, master), bl = command
    call ata_cmd
    call status
    CHECK al, 0x58          ; DRDY DSC DRQ
    mov edi, buf
    mov ecx, 256
    mov dx, PRI
    rep insw
    CHECK word [buf], 0x0040
    CHECK dword [buf + 120], 2048       ; words 60-61: sectors
    CHECK dword [buf + 54], 'EBME'      ; model "BEEMU ...", bytes swapped within each word
    call status
    CHECK al, 0x50
    mov dx, BM + 2                      ; the bus-master Interrupt bit follows INTRQ even without DMA
    in al, dx                           ; (BeOS's driver checks it to claim the shared IRQ)
    and al, 4
    CHECK al, 4
    mov al, 4
    out dx, al                          ; write 1 to clear
    in al, dx
    and al, 4
    CHECK al, 0

    ; ---- READ SECTORS (PIO), two sectors ----
    mov eax, 5
    mov cl, 2
    mov bx, 0xE020
    call ata_cmd
    mov edi, buf
    mov ecx, 512
    mov dx, PRI
    rep insw
    CHECK dword [buf], 0x5A000005
    CHECK dword [buf + 508], 0x5A000005
    CHECK dword [buf + 512], 0x5A000006
    call status
    CHECK al, 0x50

    ; ---- WRITE SECTORS, then read it back (goes to the RAM overlay) ----
    mov edi, buf
    mov eax, 0xC3C3C3C3
    mov ecx, 128
    rep stosd
    mov eax, 7
    mov cl, 1
    mov bx, 0xE030
    call ata_cmd
    call status
    CHECK al, 0x58          ; ready for data, no interrupt yet
    mov esi, buf
    mov ecx, 256
    mov dx, PRI
    rep outsw
    call status
    CHECK al, 0x50
    mov eax, 7
    mov cl, 1
    mov bx, 0xE020
    call ata_cmd
    mov edi, buf
    mov ecx, 256
    mov dx, PRI
    rep insw
    CHECK dword [buf + 100], 0xC3C3C3C3

    ; ---- SET MULTIPLE 4 + READ MULTIPLE 8 sectors ----
    xor eax, eax
    mov cl, 4
    mov bx, 0xE0C6
    call ata_cmd
    call status
    CHECK al, 0x50
    mov eax, 16
    mov cl, 8
    mov bx, 0xE0C4
    call ata_cmd
    mov edi, buf
    mov ecx, 8 * 256
    mov dx, PRI
    rep insw
    CHECK dword [buf + 7 * 512], 0x5A000017

    ; ---- READ SECTORS EXT (LBA48 registers written twice) ----
    mov dx, PRI + 2
    xor al, al
    out dx, al
    mov al, 1
    out dx, al
    mov dx, PRI + 3
    xor al, al
    out dx, al
    mov al, 9
    out dx, al
    mov dx, PRI + 4
    xor al, al
    out dx, al
    out dx, al
    mov dx, PRI + 5
    out dx, al
    out dx, al
    mov dx, PRI + 6
    mov al, 0x40
    out dx, al
    mov dx, PRI + 7
    mov al, 0x24
    out dx, al
    mov edi, buf
    mov ecx, 256
    mov dx, PRI
    rep insw
    CHECK dword [buf], 0x5A000009

    ; ---- out of range -> IDNF/ABRT ----
    mov eax, 2047
    mov cl, 2
    mov bx, 0xE020
    call ata_cmd
    call status
    CHECK al, 0x51          ; ERR
    mov dx, PRI + 1
    in al, dx
    CHECK al, 0x14

    ; ---- READ DMA through the bus master ----
    mov dword [prd], buf2
    mov dword [prd + 4], 0x80000000 | 1024
    mov dx, BM + 4
    mov eax, prd
    out dx, eax
    mov eax, 20
    mov cl, 2
    mov bx, 0xE0C8
    call ata_cmd
    mov dx, BM
    mov al, 0x09            ; start, device -> memory
    out dx, al
    CHECK dword [buf2], 0x5A000014
    CHECK dword [buf2 + 1020], 0x5A000015
    mov dx, BM + 2
    in al, dx
    and al, 7
    CHECK al, 4             ; interrupt, no longer active, no error
    mov al, 4
    out dx, al              ; clear the interrupt bit
    in al, dx
    and al, 7
    CHECK al, 0
    mov dx, BM
    xor al, al
    out dx, al
    call status
    CHECK al, 0x50

    ; ==== ATAPI on the secondary channel ====
    mov dx, SEC + 6
    mov al, 0xA0
    out dx, al
    mov dx, 0x376           ; software reset
    mov al, 4
    out dx, al
    xor al, al
    out dx, al
    mov dx, SEC + 4
    in al, dx
    CHECK al, 0x14          ; ATAPI signature
    inc dx
    in al, dx
    CHECK al, 0xEB

    mov dx, SEC + 7         ; IDENTIFY PACKET DEVICE
    mov al, 0xA1
    out dx, al
    mov edi, buf
    mov ecx, 256
    mov dx, SEC
    rep insw
    CHECK word [buf], 0x85C0

    mov esi, pkt_inquiry
    mov cx, 2048
    call atapi_cmd
    mov dx, SEC + 4
    in al, dx
    CHECK al, 36            ; byte count of this DRQ block
    mov dx, SEC + 2
    in al, dx
    CHECK al, 2             ; interrupt reason: data to host
    mov edi, buf
    mov ecx, 18
    mov dx, SEC
    rep insw
    CHECK byte [buf], 5     ; CD-ROM
    CHECK dword [buf + 8], 'BEEM'
    mov dx, SEC + 2
    in al, dx
    CHECK al, 3             ; command complete
    call status2
    CHECK al, 0x50

    mov esi, pkt_capacity
    mov cx, 2048
    call atapi_cmd
    mov edi, buf
    mov ecx, 4
    mov dx, SEC
    rep insw
    CHECK dword [buf], 0x0F000000       ; last block 15, big-endian
    CHECK dword [buf + 4], 0x00080000   ; 2048

    mov esi, pkt_read               ; READ(10) block 3, 2 blocks, 2048 bytes per DRQ
    mov cx, 2048
    call atapi_cmd
    mov edi, buf
    mov ecx, 2048
    mov dx, SEC
    rep insw
    CHECK dword [buf], 0xCD000003
    CHECK dword [buf + 2048], 0xCD000004
    call status2
    CHECK al, 0x50

    mov esi, pkt_bad                ; unknown opcode -> CHECK CONDITION
    mov cx, 2048
    call atapi_cmd
    call status2
    CHECK al, 0x41                  ; DRDY ERR
    mov dx, SEC + 1
    in al, dx
    CHECK al, 0x50                  ; sense key 5 (illegal request) in the high nibble
    mov esi, pkt_sense
    mov cx, 2048
    call atapi_cmd
    mov edi, buf
    mov ecx, 9
    mov dx, SEC
    rep insw
    CHECK byte [buf + 2], 5
    CHECK byte [buf + 12], 0x20

    ; ---- ATAPI READ(10) by DMA (secondary bus master at BM + 8) ----
    mov dword [prd], buf2
    mov dword [prd + 4], 0x80000000 | 2048
    mov dx, BM + 8 + 4
    mov eax, prd
    out dx, eax
    mov dx, SEC + 1
    mov al, 1                       ; DMA
    out dx, al
    mov dx, SEC + 7
    mov al, 0xA0
    out dx, al
    mov esi, pkt_read1
    mov ecx, 6
    mov dx, SEC
    rep outsw
    mov dx, BM + 8
    mov al, 0x09
    out dx, al
    CHECK dword [buf2], 0xCD00000A
    CHECK dword [buf2 + 2044], 0xCD00000A
    mov dx, SEC + 2
    in al, dx
    CHECK al, 3
    mov dx, BM + 8
    xor al, al
    out dx, al

    DONE

; ebp = command block base, eax = LBA (28 bit), cl = count, bh = device bits, bl = command
ata_cmd:
    push eax
    lea edx, [ebp + 2]
    mov al, cl
    out dx, al
    pop eax
    push eax
    inc edx
    out dx, al
    shr eax, 8
    inc edx
    out dx, al
    shr eax, 8
    inc edx
    out dx, al
    shr eax, 8
    and al, 0x0F
    or al, bh
    inc edx
    out dx, al
    inc edx
    mov al, bl
    out dx, al
    pop eax
    ret

status:                     ; al = primary status & (BSY|DRDY|DSC|DRQ|ERR)
    mov dx, PRI + 7
    in al, dx
    and al, 0xD9
    ret

status2:
    mov dx, SEC + 7
    in al, dx
    and al, 0xD9
    ret

; esi = 12-byte packet, cx = byte count limit (PIO)
atapi_cmd:
    mov dx, SEC + 1
    xor al, al
    out dx, al
    mov dx, SEC + 4
    mov al, cl
    out dx, al
    inc dx
    mov al, ch
    out dx, al
    mov dx, SEC + 7
    mov al, 0xA0
    out dx, al
    mov ecx, 6
    mov dx, SEC
    rep outsw
    ret

align 4
pkt_inquiry:  db 0x12, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0
pkt_capacity: db 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
pkt_read:     db 0x28, 0, 0, 0, 0, 3, 0, 0, 2, 0, 0, 0
pkt_read1:    db 0x28, 0, 0, 0, 0, 10, 0, 0, 1, 0, 0, 0
pkt_bad:      db 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
pkt_sense:    db 0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0

align 4096
prd:  dd 0, 0
align 16
buf:  times 8192 db 0
buf2: times 4096 db 0
