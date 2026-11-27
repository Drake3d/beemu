; Self-checking guest test programs, run by tests/test_sys.c.
; The test device on the I/O bus:
;   out 0xE0      test finished (followed by hlt)
;   out 0xE1, eax a check failed; eax = source line
;   out 0xE2      one check ran
;   0xE3          echo register: reads return the last value written
; CHECK a, b compares with cmp, so it clobbers the arithmetic flags.

%macro CHECK 2
    cmp %1, %2
    je %%ok
    push eax
    mov eax, __LINE__
    out 0xE1, eax
    pop eax
%%ok:
    out 0xE2, al
%endmacro

%macro DONE 0
    out 0xE0, al
    hlt
%endmacro

; GDT descriptor: base, limit (20 bits), access byte, flags nibble (G, D/B)
%macro DESC 4
    dw (%2) & 0xFFFF
    dw (%1) & 0xFFFF
    db ((%1) >> 16) & 0xFF
    db %3
    db (((%2) >> 16) & 0x0F) | ((%4) << 4)
    db ((%1) >> 24) & 0xFF
%endmacro

; 32-bit interrupt gate to selector 0x08: handler offset, DPL
%macro GATE 2
    dw (%1) & 0xFFFF
    dw 0x08
    db 0
    db 0x8E | ((%2) << 5)
    dw (%1) >> 16
%endmacro
