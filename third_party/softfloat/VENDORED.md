# Berkeley SoftFloat Release 3e (vendored)

Source: https://github.com/ucb-bar/berkeley-softfloat-3, commit 497c81c1f5016dc6ef5b3219ece2f3c261d4d8f1 (Fri Oct 23 16:48:46 2026 -0700)
(Release 3e plus later upstream build fixes). The later bfloat16 (`bf16`) additions are left out. License: BSD 3-clause, see COPYING.txt.

What is here: source/include, the files the upstream `build/Linux-x86_64-GCC` Makefile compiles
(with `SOFTFLOAT_FAST_INT64`), and the `8086` specialization (x87 NaN rules: the default NaN is the
"real indefinite", propagation picks the larger significand). `sources.cmake` lists them.
`platform.h` is ours (and CMake defines `LITTLEENDIAN=1` and `THREAD_LOCAL=_Thread_local` for every user of `softfloat.h`: struct layouts and the thread-local globals must match the library, or callers silently corrupt memory). `INLINE` is `static inline` so the int128 helpers link at any optimization level: little-endian, GCC/Clang builtins, and `THREAD_LOCAL = _Thread_local` so the
rounding mode, precision and exception flags are per thread.
One local fix in `source/s_addMagsExtF80.c` (marked `beemu:`): adding two operands with exponent 0
whose significands sum to 0 (e.g. +0 + +0) called `softfloat_normSubnormalExtF80Sig(0)`, a shift by 64
(undefined behavior; UBSan flags it from FADD). It now rounds exponent 0 / significand 0 directly to a
signed zero, the result the shift happened to give on ARM64. Re-apply it when updating.
Second local change (beemu, for speed): `s_roundPackToExtF80.c` sets two new globals declared in
`softfloat.h` / defined in `softfloat_state.c`, `softfloat_extF80RoundKnown` and
`softfloat_extF80RoundedUp`: on its normal paths (not tiny, not overflowing) whether rounding moved the
result away from zero. The x87 C1 bit needs exactly that; before, every inexact FADD/FMUL/FDIV/... was
computed a second time in round-toward-zero mode to find it. The tiny and overflow paths leave Known
false and the caller still recomputes. Re-apply both when updating.
Unmodified upstream files otherwise. To update, copy the same lists from a newer checkout.
