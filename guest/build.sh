#!/bin/bash
# SPDX-License-Identifier: CC0-1.0
# Builds libguestrt.so: the ARM32 runtime loaded into the emulator alongside the game.
set -e
NDK=${NDK:?set NDK to the Android NDK root}
TC=${NDK_TC:-$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/ | head -1)}
CC=$TC/bin/armv7a-linux-androideabi21-clang
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../out}
mkdir -p "$OUT"
# compiler-rt's __aeabi_* helpers are hidden; re-export them so the game can import them.
AEABI="__aeabi_idiv __aeabi_idivmod __aeabi_uidiv __aeabi_uidivmod __aeabi_ldivmod __aeabi_uldivmod
       __aeabi_d2lz __aeabi_d2ulz __aeabi_f2lz __aeabi_f2ulz __aeabi_l2d __aeabi_l2f __aeabi_ul2d __aeabi_ul2f
       __aeabi_memcpy __aeabi_memcpy4 __aeabi_memcpy8 __aeabi_memmove __aeabi_memmove4 __aeabi_memmove8
       __aeabi_memset __aeabi_memset4 __aeabi_memset8 __aeabi_memclr __aeabi_memclr4 __aeabi_memclr8"
printf "%s\n" $AEABI > "$OUT/aeabi.syms"
BUILTINS=$OUT/libbuiltins-export.a
$TC/bin/llvm-objcopy --set-symbols-visibility="$OUT/aeabi.syms"=default \
    "$(ls "$TC"/lib/clang/*/lib/linux/libclang_rt.builtins-arm-android.a | sort -V | tail -1)" "$BUILTINS"
UNDEF=$(for s in $AEABI; do printf -- "-Wl,-u,%s " $s; done)

CFLAGS="-O2 -marm -mfloat-abi=softfp -mfpu=vfpv3-d16 -fPIC -ffreestanding -fno-builtin -fno-stack-protector
        -fvisibility=hidden -Wno-everything"
$CC $CFLAGS -shared -nostdlib -Wl,-soname,libguestrt.so -Wl,-Bsymbolic -Wl,--no-undefined-version \
    -Wl,--hash-style=both -Wl,-z,norelro \
    "$HERE/rt_string.c" "$HERE/rt_stdio.c" "$HERE/rt_malloc.c" "$HERE/rt_asm.S" \
    $UNDEF "$BUILTINS" \
    -o "$OUT/libguestrt.so"
echo "built $OUT/libguestrt.so"
