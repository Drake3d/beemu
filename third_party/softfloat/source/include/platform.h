/* beemu platform header for Berkeley SoftFloat 3e (Apple Silicon, Clang). */
#ifndef LITTLEENDIAN
#define LITTLEENDIAN 1          /* also set for every user of softfloat.h by CMake: struct layouts depend on it */
#endif
#define INLINE static inline   /* every file gets its own copy: links at any optimization level */
#ifndef THREAD_LOCAL
#define THREAD_LOCAL _Thread_local   /* also set for every user by CMake (see LITTLEENDIAN) */
#endif
#define SOFTFLOAT_BUILTIN_CLZ 1
#define SOFTFLOAT_INTRINSIC_INT128 1
#include "opts-GCC.h"
