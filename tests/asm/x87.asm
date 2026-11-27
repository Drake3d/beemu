; x87 system behavior (32-bit flat mode from the harness; this program loads its own
; GDT/IDT for #MF and #NM): FNINIT state, stack overflow, unmasked exceptions -> #MF
; at the next FPU instruction, CR0.TS -> #NM (lazy FPU switching), FNSAVE/FRSTOR,
; FNSTENV tags, FCMOV, FCOMI + jcc, FIST under each rounding mode, FXAM.
bits 32
org 0x10000
%include "check.inc"
%define ABS(x) ((x) - $$ + 0x10000)

start:
    lgdt [gdt_desc]
    jmp 0x08:.cs_ok
.cs_ok:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    lidt [idt_desc]
    mov eax, cr0
    or eax, 0x20            ; CR0.NE: native x87 error reporting (#MF)
    and eax, ~0x0C          ; EM = TS = 0
    mov cr0, eax

    ; ---- FNINIT state ----
    fninit
    fnstcw [w]
    CHECK word [w], 0x037F
    fnstsw ax
    CHECK ax, 0
    fnstenv [env]
    CHECK word [env + 8], 0xFFFF        ; all tags empty
    fld1
    fnstenv [env]
    CHECK word [env + 8], 0x3FFF        ; ST0 = physical register 7: valid

    ; ---- compare + status word + sahf, FCOMI ----
    fninit
    fld1                    ; ST1 = 1
    fldz                    ; ST0 = 0
    fcom st1                ; 0 < 1: C0
    fnstsw ax
    and ax, 0x4500
    CHECK ax, 0x0100
    fnstsw ax
    sahf
    mov ebx, 0
    jnb .nb
    mov ebx, 1
.nb:
    CHECK ebx, 1
    fcomi st0, st1          ; CF = 1, ZF = 0
    setb bl
    sete cl
    CHECK bl, 1
    CHECK cl, 0

    ; ---- FCMOV ----
    mov eax, 1
    cmp eax, 2              ; CF = 1
    fcmovb st0, st1         ; ST0 = 1
    fld1
    fcomip st0, st1         ; equal: ZF = 1, pops
    sete bl
    CHECK bl, 1

    ; ---- FXAM ----
    fninit
    fxam                    ; empty: C3 + C0
    fnstsw ax
    and ax, 0x4500
    CHECK ax, 0x4100
    fldpi
    fchs
    fxam                    ; normal, negative: C2 + C1
    fnstsw ax
    and ax, 0x4700
    CHECK ax, 0x0600

    ; ---- stack overflow (masked): 9th push -> IE SF C1, indefinite ----
    fninit
    mov ecx, 9
.push:
    fld1
    loop .push
    fnstsw ax
    and ax, 0x0241
    CHECK ax, 0x0241
    fstp tword [t]
    CHECK word [t + 8], 0xFFFF          ; real indefinite
    CHECK dword [t + 4], 0xC0000000

    ; ---- FIST under the four rounding modes (2.5 and -2.5) ----
    fninit
    mov word [w], 0x037F    ; nearest
    call fist_2_5
    CHECK dword [i], 2
    CHECK dword [i + 4], -2
    mov word [w], 0x077F    ; down
    call fist_2_5
    CHECK dword [i], 2
    CHECK dword [i + 4], -3
    mov word [w], 0x0B7F    ; up
    call fist_2_5
    CHECK dword [i], 3
    CHECK dword [i + 4], -2
    mov word [w], 0x0F7F    ; chop
    call fist_2_5
    CHECK dword [i], 2
    CHECK dword [i + 4], -2

    ; ---- FNSAVE / FRSTOR round trip ----
    fninit
    fldpi
    fld1
    fnsave [save]
    fnstsw ax               ; FNSAVE reinitializes
    CHECK ax, 0
    fldz                    ; disturb
    frstor [save]
    fstp tword [t]          ; 1.0
    CHECK word [t + 8], 0x3FFF
    CHECK dword [t + 4], 0x80000000
    fstp tword [t]          ; pi
    CHECK dword [t + 4], 0xC90FDAA2

    ; ---- unmasked exception: #MF at the next FPU instruction ----
    fninit
    mov word [w], 0x037B    ; unmask ZE
    fldcw [w]
    fld1
    fldz
    fdivp st1, st0          ; 1/0: ZE unmasked -> destination untouched, ES set, no fault yet
    CHECK dword [mf_count], 0
    fnstsw ax
    and ax, 0x0084
    CHECK ax, 0x0084        ; ZE + ES
    fld1                    ; waiting instruction -> #MF; the handler clears the exceptions
    CHECK dword [mf_count], 1
    fnstsw ax
    and ax, 0x00FF
    CHECK ax, 0

    ; ---- CR0.TS: #NM on the first FPU instruction, handler does CLTS ----
    mov eax, cr0
    or eax, 8
    mov cr0, eax
    fld1                    ; #NM, then retried after CLTS
    CHECK dword [nm_count], 1
    fwait                   ; TS clear now: no fault
    CHECK dword [nm_count], 1
    mov eax, cr0
    and eax, 8
    CHECK eax, 0

    DONE

fist_2_5:
    fldcw [w]
    fld dword [two_5]
    fistp dword [i]
    fld dword [two_5]
    fchs
    fistp dword [i + 4]
    ret

mf_handler:
    inc dword [mf_count]
    fnclex
    iretd

nm_handler:
    inc dword [nm_count]
    clts
    iretd

align 8
gdt:
    dq 0
    DESC 0, 0xFFFFF, 0x9A, 0xC
    DESC 0, 0xFFFFF, 0x92, 0xC
gdt_end:
gdt_desc: dw gdt_end - gdt - 1
          dd gdt
idt:
%assign v 0
%rep 17
  %if v == 7
    GATE ABS(nm_handler), 0
  %elif v == 16
    GATE ABS(mf_handler), 0
  %else
    dq 0
  %endif
  %assign v v + 1
%endrep
idt_end:
idt_desc: dw idt_end - idt - 1
          dd idt

align 4
two_5:    dd 2.5
w:        dw 0
align 4
i:        dd 0, 0
mf_count: dd 0
nm_count: dd 0
t:        times 16 db 0
env:      times 28 db 0
save:     times 108 db 0
