; Paging (32-bit flat mode from the harness, 4 MB RAM). Page directory at 1 MB:
;   PDE 0 -> page table at 0x101000, identity map of 0-4 MB (user, writable) except
;            0x300000 not present, 0x301000 read-only, 0x302000 supervisor-only,
;            0x303000 -> physical 0x304000, 0x306000 not present
;   PDE 1 -> 4 MB PSE page: linear 0x400000-0x7FFFFF aliases physical 0-4 MB
; Checks remapping, accessed/dirty bits, #PF (not present, write protect with
; CR0.WP, user access to supervisor pages, instruction fetch) with CR2 and error
; codes and restart, stale TLB until INVLPG / CR3 reload, code run through an alias,
; and self-modifying code written through the alias.
bits 32
org 0x10000
%include "check.inc"
%define ABS(x) ((x) - $$ + 0x10000)
%define PD  0x100000
%define PT0 0x101000
%define ALIAS 0x400000

start:
    lgdt [gdt_desc]
    jmp 0x08:.cs
.cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    lidt [idt_desc]

    ; ---- page tables ----
    mov edi, PT0
    mov eax, 7                          ; present, writable, user
    mov ecx, 1024
.fill:
    stosd
    add eax, 0x1000
    loop .fill
    mov dword [PT0 + 0x300 * 4], 0x300006   ; not present
    mov dword [PT0 + 0x301 * 4], 0x301005   ; read-only
    mov dword [PT0 + 0x302 * 4], 0x302003   ; supervisor only
    mov dword [PT0 + 0x303 * 4], 0x304007   ; remapped
    mov dword [PT0 + 0x306 * 4], 0x306006   ; not present
    mov edi, PD
    xor eax, eax
    mov ecx, 1024
    rep stosd
    mov dword [PD], PT0 | 7
    mov dword [PD + 4], 0x00000087          ; 4 MB page (PS) at physical 0

    mov eax, cr4
    or eax, 0x10                        ; PSE
    mov cr4, eax
    mov eax, PD
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000                  ; PG
    mov cr0, eax

    ; ---- remapping, 4 MB alias, accessed / dirty ----
    mov dword [0x303000], 0x1234
    CHECK dword [ALIAS + 0x304000], 0x1234
    CHECK dword [PT0 + 0x303 * 4], 0x304067     ; accessed + dirty
    mov eax, [0x305000]
    CHECK dword [PT0 + 0x305 * 4], 0x305027     ; accessed only
    mov eax, [PD]
    and eax, 0x20
    CHECK eax, 0x20

    ; ---- #PF: not present ----
    mov dword [fix_or], 1
    mov eax, [0x300010]
    CHECK dword [pf_count], 1
    CHECK dword [pf_cr2], 0x300010
    CHECK dword [pf_err], 0             ; supervisor read, not present

    ; ---- write protection: allowed for the supervisor until CR0.WP ----
    mov dword [0x301000], 5
    CHECK dword [pf_count], 1
    mov eax, cr0
    or eax, 0x10000                     ; WP
    mov cr0, eax
    mov dword [fix_or], 2
    mov dword [0x301004], 6
    CHECK dword [pf_count], 2
    CHECK dword [pf_err], 3             ; present, write
    CHECK dword [0x301004], 6

    ; ---- TLB: stale until INVLPG / CR3 reload ----
    CHECK dword [0x303000], 0x1234
    mov dword [PT0 + 0x303 * 4], 0x305007
    mov dword [ALIAS + 0x305000], 0xABCD
    CHECK dword [0x303000], 0x1234      ; still the cached mapping
    invlpg [0x303000]
    CHECK dword [0x303000], 0xABCD
    mov dword [PT0 + 0x303 * 4], 0x304007
    CHECK dword [0x303000], 0xABCD
    mov eax, cr3
    mov cr3, eax
    CHECK dword [0x303000], 0x1234

    ; ---- code through the alias, self-modifying code through the alias ----
    call smc_fn
    CHECK eax, 1
    mov byte [ALIAS + smc_fn + 1], 2
    call smc_fn
    CHECK eax, 2
    call ALIAS + smc_fn
    CHECK eax, 2

    ; ---- instruction fetch from a missing page ----
    mov dword [fix_or], 0
    mov dword [fix_set], fetch_page + 7
    call 0x306000
    CHECK eax, 0x77
    CHECK dword [pf_count], 3
    CHECK dword [pf_cr2], 0x306000
    CHECK dword [pf_err], 0
    mov dword [fix_set], 0

    ; ---- user mode: supervisor page -> #PF with U set ----
    mov ax, 0x30
    ltr ax
    pushfd
    pop eax
    or eax, 0x3000                      ; IOPL 3: CHECK's port I/O works in ring 3
    push dword 0x2B
    push dword 0x8F000
    push eax
    push dword 0x23
    push dword user_code
    iretd

user_code:
    mov ax, 0x2B
    mov ds, ax
    mov es, ax
    mov dword [fix_or], 4               ; the handler grants user access
    mov eax, [0x302000]
    CHECK dword [pf_count], 4
    CHECK dword [pf_err], 5             ; present, read, user
    CHECK dword [pf_cr2], 0x302000
    int 0x31                            ; back to ring 0: done

; ---- handlers ----
pf_handler:                             ; error code on the stack
    push eax
    push ebx
    mov eax, [esp + 8]
    mov [pf_err], eax
    mov eax, cr2
    mov [pf_cr2], eax
    inc dword [pf_count]
    shr eax, 12
    and eax, 0x3FF
    lea ebx, [PT0 + eax * 4]
    mov eax, [fix_set]
    test eax, eax
    jz .or
    mov [ebx], eax
    jmp .inv
.or:
    mov eax, [fix_or]
    or [ebx], eax
.inv:
    mov eax, cr2
    invlpg [eax]
    pop ebx
    pop eax
    add esp, 4
    iretd

int31_handler:
    mov ax, cs
    CHECK ax, 0x08
    DONE

smc_fn:
    mov eax, 1
    ret

; ---- data ----
align 8
gdt:
    dq 0
    DESC 0, 0xFFFFF, 0x9A, 0xC          ; 0x08 ring 0 code
    DESC 0, 0xFFFFF, 0x92, 0xC          ; 0x10 ring 0 data
    dq 0                                ; 0x18
    DESC 0, 0xFFFFF, 0xFA, 0xC          ; 0x20 ring 3 code
    DESC 0, 0xFFFFF, 0xF2, 0xC          ; 0x28 ring 3 data
    DESC ABS(tss), tss_end - tss - 1, 0x89, 0x0   ; 0x30 TSS
gdt_end:
gdt_desc: dw gdt_end - gdt - 1
          dd gdt
idt:
%assign v 0
%rep 0x32
  %if v == 14
    GATE ABS(pf_handler), 0
  %elif v == 0x31
    GATE ABS(int31_handler), 3
  %else
    dq 0
  %endif
  %assign v v + 1
%endrep
idt_end:
idt_desc: dw idt_end - idt - 1
          dd idt
align 4
tss:
    dd 0, 0x90000, 0x10
    times 22 dd 0
    dw 0, 0x68
tss_end:
pf_count: dd 0
pf_cr2:   dd 0
pf_err:   dd 0
fix_or:   dd 0
fix_set:  dd 0

align 4096
fetch_page:
    mov eax, 0x77
    ret
