; 32-bit flat protected mode (set up by the harness: CS/DS/ES/SS base 0, 4 GB,
; CPL 0, ESP = 0x80000). Branches, string ops, stack frames, flag transfers,
; misc integer ops, I/O, CPUID, TSC/MSR, descriptor-table registers.
bits 32
org 0x10000
%include "check.inc"

start:
    ; ---- conditional jumps, LOOP family, SETcc, CMOVcc ----
    xor eax, eax
    mov ecx, 10
.sum:
    add eax, ecx
    dec ecx
    jnz .sum
    CHECK eax, 55

    xor eax, eax
    mov ecx, 5
.lp:
    add eax, 2
    loop .lp
    CHECK eax, 10
    CHECK ecx, 0

    xor ecx, ecx
    mov eax, 1
    jecxz .z1
    mov eax, 2
.z1:
    CHECK eax, 1

    mov ecx, 10
    xor ebx, ebx
.ln:
    inc ebx
    cmp ebx, 3
    loopne .ln              ; runs while ebx != 3
    CHECK ebx, 3
    CHECK ecx, 7

    mov eax, -1
    cmp eax, 1
    setl bl                 ; signed: -1 < 1
    setb cl                 ; unsigned: 0xffffffff < 1 is false
    seta dl
    CHECK bl, 1
    CHECK cl, 0
    CHECK dl, 1

    xor esi, esi
    cmp eax, 1
    jg .bad1
    ja .ok1
.bad1:
    mov esi, 1
.ok1:
    CHECK esi, 0

    mov eax, 5
    mov ebx, 7
    mov ecx, 9
    cmp eax, ebx
    cmovl eax, ecx
    cmovg ebx, ecx
    CHECK eax, 9
    CHECK ebx, 7
    mov dword [scratch], 0x77
    cmp eax, 9
    cmove edx, [scratch]
    CHECK edx, 0x77

    mov byte [scratch], 0xAA
    cmp eax, 9
    sete byte [scratch]
    CHECK byte [scratch], 1

    ; ---- string instructions ----
    cld
    mov esi, src_str
    mov edi, buf
    mov ecx, 5
    rep movsb
    CHECK ecx, 0
    CHECK esi, src_str + 5
    CHECK edi, buf + 5
    CHECK dword [buf], 'hell'
    CHECK byte [buf + 4], 'o'

    mov edi, buf
    mov eax, 0x11223344
    mov ecx, 4
    rep stosd
    CHECK dword [buf + 12], 0x11223344
    CHECK edi, buf + 16

    mov edi, src_str        ; strlen with repne scasb
    xor al, al
    mov ecx, -1
    repne scasb
    not ecx
    dec ecx
    CHECK ecx, 5

    mov esi, s1             ; first difference with repe cmpsb
    mov edi, s2
    mov ecx, 8
    repe cmpsb
    setb dl                 ; 'X' < 'd'
    CHECK ecx, 4
    CHECK esi, s1 + 4
    CHECK dl, 1

    std
    mov esi, src_str + 4
    lodsb
    lodsb
    cld
    CHECK al, 'l'
    CHECK esi, src_str + 2

    xor ecx, ecx            ; rep with a zero count does nothing
    mov edi, buf
    rep stosb
    CHECK edi, buf

    ; ---- pusha / popa ----
    mov eax, 1
    mov ebx, 2
    mov ecx, 3
    mov edx, 4
    mov esi, 5
    mov edi, 6
    mov ebp, 7
    mov [saved_esp], esp
    pushad
    mov eax, [saved_esp]
    CHECK [esp + 12], eax   ; the ESP slot holds the value from before pushad
    CHECK dword [esp + 28], 1
    CHECK dword [esp], 6
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    popad
    CHECK eax, 1
    CHECK edx, 4
    CHECK edi, 6
    CHECK ebp, 7
    CHECK esp, [saved_esp]

    ; ---- enter / leave ----
    mov ebp, 0x1234
    mov ebx, esp
    enter 16, 0
    lea ecx, [ebx - 4]
    CHECK ebp, ecx
    lea ecx, [ebx - 20]
    CHECK esp, ecx
    leave
    CHECK esp, ebx
    CHECK ebp, 0x1234

    mov ebp, data_frame + 4  ; nesting level 2 copies one outer frame pointer
    mov ebx, esp
    enter 8, 2
    CHECK dword [ebx - 8], 0xCAFE
    lea ecx, [ebx - 4]
    CHECK [ebx - 12], ecx
    CHECK ebp, ecx
    lea ecx, [ebx - 20]
    CHECK esp, ecx
    leave
    CHECK esp, ebx
    CHECK ebp, data_frame + 4

    ; ---- xlat, lahf / sahf, pushf / popf ----
    mov ebx, table
    mov al, 2
    xlatb
    CHECK al, 30

    mov ah, 0xC1            ; SF ZF CF
    sahf
    setz bl
    sets cl
    setc dl
    setp dh
    CHECK bl, 1
    CHECK cl, 1
    CHECK dl, 1
    CHECK dh, 0

    mov eax, 1
    cmp eax, 1
    lahf
    CHECK ah, 0x46          ; ZF PF and the always-one bit

    pushfd                  ; the ID flag can be toggled: CPUID is present
    pop eax
    mov ebx, eax
    xor eax, 0x200000
    push eax
    popfd
    pushfd
    pop eax
    xor eax, ebx
    and eax, 0x200000
    CHECK eax, 0x200000
    push ebx
    popfd

    ; ---- sign extension, bswap, mul/div, shifts (spot checks) ----
    mov eax, 0x12345680
    cbw
    CHECK eax, 0x1234FF80
    cwde
    CHECK eax, 0xFFFFFF80
    cdq
    CHECK edx, 0xFFFFFFFF
    mov eax, 0x7000
    cwd
    CHECK dx, 0

    mov eax, 0x11223344
    bswap eax
    CHECK eax, 0x44332211

    mov eax, 100
    mov ecx, 7
    xor edx, edx
    div ecx
    CHECK eax, 14
    CHECK edx, 2
    mov eax, -100
    cdq
    idiv ecx
    CHECK eax, -14
    CHECK edx, -2
    mov eax, 0x10000
    mul eax
    CHECK edx, 1
    CHECK eax, 0
    imul eax, ecx, -3       ; ecx = 7
    CHECK eax, -21
    mov eax, 6
    imul eax, -3
    CHECK eax, -18
    mov eax, 0x40000000
    imul eax, eax, 4        ; overflows: CF = OF = 1
    seto bl
    CHECK eax, 0
    CHECK bl, 1

    mov eax, 0x80000001
    shl eax, 1
    setc bl
    CHECK eax, 2
    CHECK bl, 1
    mov eax, 0x80000000
    sar eax, 4
    CHECK eax, 0xF8000000
    mov eax, 0x12345678
    rol eax, 8
    CHECK eax, 0x34567812
    mov eax, 0x12345678
    mov edx, 0xABCDEF01
    shld eax, edx, 8
    CHECK eax, 0x345678AB

    mov eax, 0x50
    bsf ecx, eax
    bsr edx, eax
    CHECK ecx, 4
    CHECK edx, 6
    mov dword [scratch], 0
    bts dword [scratch], 33   ; immediate bit offsets wrap within the dword
    CHECK dword [scratch], 2
    mov dword [buf], 0
    mov dword [buf + 4], 0
    mov ecx, 35
    bts [buf], ecx            ; register bit offsets reach the next dword
    CHECK dword [buf + 4], 8

    ; ---- cmpxchg, xadd, cmpxchg8b, lock ----
    mov dword [scratch], 5
    mov eax, 5
    mov ecx, 9
    lock cmpxchg [scratch], ecx
    CHECK dword [scratch], 9
    mov eax, 1
    cmpxchg [scratch], ecx
    CHECK eax, 9

    mov dword [scratch], 10
    mov eax, 3
    lock xadd [scratch], eax
    CHECK eax, 10
    CHECK dword [scratch], 13

    mov dword [q], 1
    mov dword [q + 4], 2
    mov eax, 1
    mov edx, 2
    mov ebx, 0xAAAA
    mov ecx, 0xBBBB
    lock cmpxchg8b [q]
    sete byte [scratch]
    CHECK byte [scratch], 1
    CHECK dword [q], 0xAAAA
    CHECK dword [q + 4], 0xBBBB
    xor eax, eax
    xor edx, edx
    cmpxchg8b [q]
    CHECK eax, 0xAAAA
    CHECK edx, 0xBBBB

    mov dword [scratch], 5
    lock add dword [scratch], 3
    CHECK dword [scratch], 8

    ; ---- port I/O ----
    mov eax, 0xDEADBEEF
    out 0xE3, eax
    xor eax, eax
    in eax, 0xE3
    CHECK eax, 0xDEADBEEF
    mov dx, 0xE3
    mov al, 0x5A
    out dx, al
    xor eax, eax
    in al, dx
    CHECK eax, 0x5A
    mov dx, 0x2F8           ; nothing there: reads all ones
    in ax, dx
    CHECK ax, 0xFFFF

    mov esi, src_str
    mov ecx, 3
    mov dx, 0xE3
    rep outsb               ; echo keeps the last byte, 'l'
    in al, dx
    CHECK al, 'l'
    mov edi, buf
    mov ecx, 2
    rep insb
    CHECK word [buf], 'll'

    ; ---- CPUID, TSC, MSRs ----
    xor eax, eax
    cpuid
    CHECK eax, 2
    CHECK ebx, 'Genu'
    CHECK edx, 'ineI'
    CHECK ecx, 'ntel'
    mov eax, 1
    cpuid
    shr eax, 8
    and eax, 0xF
    CHECK eax, 6            ; family 6
    and edx, 1 << 15
    CHECK edx, 1 << 15      ; CMOV

    rdtsc
    mov esi, eax
    mov edi, edx
    rdtsc
    sub eax, esi
    sbb edx, edi
    CHECK edx, 0            ; time moved forward by less than 4 seconds

    mov ecx, 0x10           ; write the TSC, read it back
    mov eax, 1000
    xor edx, edx
    wrmsr
    rdmsr
    CHECK edx, 0
    mov ecx, 0x1B           ; APIC base: enabled, BSP
    rdmsr
    CHECK eax, 0xFEE00900

    ; ---- descriptor-table registers ----
    lgdt [gdtd]
    sgdt [buf]
    CHECK word [buf], 0x17
    CHECK dword [buf + 2], 0x12340
    smsw ax
    and ax, 1
    CHECK ax, 1

    DONE

align 4096
src_str:    db 'hello', 0
s1:         db 'abcXefgh'
s2:         db 'abcdefgh'
table:      db 10, 20, 30
align 4
data_frame: dd 0xCAFE
saved_esp:  dd 0
scratch:    dd 0
q:          dq 0
gdtd:       dw 0x17
            dd 0x12340
buf:        times 64 db 0
