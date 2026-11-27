; Starts in real mode at 0000:7C00 (like a boot sector), then:
; real-mode INT/IRET and #DE through the IVT, far call/retf, LES; switch to
; protected mode (lgdt, CR0.PE, far jmp); segment loads from the GDT; INT and
; faults through the IDT (error codes); LTR, IRET to ring 3, the I/O
; permission bitmap, privileged instructions at CPL 3, and an INT back to
; ring 0 on the TSS stack.
bits 16
org 0x7C00
%include "check.inc"
%define ABS(x) ((x) - $$ + 0x7C00)

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000

    ; ---- real mode: INT n through the IVT ----
    mov word [0x40 * 4], int40
    mov word [0x40 * 4 + 2], 0
    mov ax, 1
    int 0x40
    CHECK ax, 2
    CHECK sp, 0x7000

    ; ---- real mode: #DE returns to the faulting instruction ----
    mov word [0 * 4], de_real
    mov word [0 * 4 + 2], 0
    xor bx, bx
    mov ax, 10
    div bl                  ; 2 bytes; the handler skips it
    CHECK word [de_count], 1

    ; ---- far call / retf, LES ----
    call 0:farfn
    CHECK cx, 0x77
    les di, [farptr]
    CHECK di, 0x1234
    mov ax, es
    CHECK ax, 0x5678
    xor ax, ax
    mov es, ax

    ; ---- enter protected mode ----
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm32

int40:
    inc ax
    iret

de_real:
    push bp
    mov bp, sp
    add word [bp + 2], 2
    inc word [de_count]
    pop bp
    iret

farfn:
    mov cx, 0x77
    retf

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    mov ax, cs
    CHECK ax, 0x08

    mov dword [0x1000], 0x600DF00D      ; selector 0x18 has base 0x1000
    mov ax, 0x18
    mov fs, ax
    CHECK dword [fs:0], 0x600DF00D

    ; ---- INT and faults through the IDT ----
    lidt [idt_desc]
    mov eax, 5
    int 0x30
    CHECK eax, 6

    mov dword [gp_skip], 2
    mov ax, 0x80            ; beyond the GDT limit
    mov gs, ax              ; #GP(0x80)
    CHECK dword [gp_count], 1
    CHECK dword [gp_err], 0x80

    xor edx, edx
    mov eax, 1
    xor ecx, ecx
    div ecx                 ; #DE, handler skips the 2-byte div
    CHECK word [de_count], 2

    ; ---- to ring 3 ----
    mov ax, 0x30
    ltr ax
    pushfd
    pop eax
    and eax, ~0x3000        ; IOPL 0: I/O goes through the TSS bitmap
    push dword 0x2B         ; user SS
    push dword 0x8F000      ; user ESP
    push eax
    push dword 0x23         ; user CS
    push dword user_code
    iretd

user_code:
    mov ax, ds              ; DS held a DPL 0 selector: nulled on the way out
    CHECK ax, 0
    mov ax, 0x2B
    mov ds, ax
    mov es, ax
    mov ax, cs
    CHECK ax, 0x23

    mov dword [gp_skip], 2
    out 0xE4, al            ; denied by the bitmap -> #GP(0)
    CHECK dword [gp_count], 2
    CHECK dword [gp_err], 0

    mov dword [gp_skip], 1
    hlt                     ; privileged -> #GP(0)
    CHECK dword [gp_count], 3

    mov dword [gp_skip], 2
    int 0x30                ; gate DPL 0 < CPL 3 -> #GP(vector * 8 + 2)
    CHECK dword [gp_count], 4
    CHECK dword [gp_err], 0x182

    int 0x31                ; DPL 3 gate: back to ring 0, finishes the test
    CHECK eax, 0xBAD        ; not reached

; ---- ring 0 handlers ----
int30_handler:
    inc eax
    iretd

gp_handler:                 ; error code on the stack
    push eax
    mov eax, [esp + 4]
    mov [gp_err], eax
    inc dword [gp_count]
    mov eax, [gp_skip]
    add [esp + 8], eax      ; skip the faulting instruction
    pop eax
    add esp, 4
    iretd

de_pm:
    add dword [esp], 2
    inc word [de_count]
    iretd

int31_handler:              ; came from ring 3: EIP CS EFLAGS ESP SS on the TSS stack
    mov ax, cs
    CHECK ax, 0x08
    mov ax, ss
    CHECK ax, 0x10
    CHECK esp, 0x90000 - 20
    CHECK dword [esp + 4], 0x23
    CHECK dword [esp + 12], 0x8F000
    CHECK dword [esp + 16], 0x2B
    DONE

; ---- data ----
align 8
gdt:
    dq 0
    DESC 0, 0xFFFFF, 0x9A, 0xC          ; 0x08 ring 0 code
    DESC 0, 0xFFFFF, 0x92, 0xC          ; 0x10 ring 0 data
    DESC 0x1000, 0xFFFF, 0x92, 0x4      ; 0x18 data, base 0x1000
    DESC 0, 0xFFFFF, 0xFA, 0xC          ; 0x20 ring 3 code
    DESC 0, 0xFFFFF, 0xF2, 0xC          ; 0x28 ring 3 data
    DESC ABS(tss), tss_end - tss - 1, 0x89, 0x0   ; 0x30 32-bit TSS
gdt_end:

gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

idt:
%assign i 0
%rep 0x32
  %if i == 0
    GATE ABS(de_pm), 0
  %elif i == 13
    GATE ABS(gp_handler), 0
  %elif i == 0x30
    GATE ABS(int30_handler), 0
  %elif i == 0x31
    GATE ABS(int31_handler), 3
  %else
    dq 0
  %endif
  %assign i i + 1
%endrep
idt_end:

idt_desc:
    dw idt_end - idt - 1
    dd idt

align 4
tss:
    dd 0                    ; 0x00 link
    dd 0x90000              ; 0x04 esp0
    dd 0x10                 ; 0x08 ss0
    times 22 dd 0           ; 0x0C .. 0x63
    dw 0                    ; 0x64 trap
    dw 0x68                 ; 0x66 I/O bitmap offset
    times 0x1C db 0xFF      ; ports 0x00 .. 0xDF denied
    db 0xF0                 ; ports 0xE0 .. 0xE3 allowed, 0xE4 .. 0xE7 denied
    times 4 db 0xFF
tss_end:

farptr:   dw 0x1234, 0x5678
de_count: dw 0
align 4
gp_skip:  dd 0
gp_count: dd 0
gp_err:   dd 0
