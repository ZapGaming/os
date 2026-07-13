#!/bin/sh
# Builds zapos_disk.img: a 64MB raw FAT32 disk image (no partition table --
# a "superfloppy" layout, so the FAT32 boot sector sits at LBA 0) with a
# few sample files for the File Manager / filesystem demo. Requires mtools
# (mformat/mmd/mcopy) -- works without root or loop devices.
set -e

cd "$(dirname "$0")/.."
IMG=zapos_disk.img
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mformat -F -v ZAPOS -i "$IMG" ::

cat > "$STAGE/README.TXT" <<'EOF'
Welcome to the ZapOS filesystem.

This file lives on a real FAT32 disk image (zapos_disk.img),
read by a from-scratch ATA PIO driver and a from-scratch FAT32
driver -- no existing filesystem code was used.

Open the File Manager window to browse around.
EOF

cat > "$STAGE/ABOUTFS.TXT" <<'EOF'
ZapOS filesystem support:
 - drivers/ata.c  : ATA PIO disk driver (IDENTIFY, PIO read/write)
 - fs/fat32.c     : FAT32 driver (BPB parsing, FAT walking,
                    directory listing, file read + write)
 - gui/compositor : the File Manager window that browses it

Try writing NOTES.TXT from within ZapOS (via the File Manager)
and rebooting -- it persists.
EOF

: > "$STAGE/NOTES.TXT"

# Sample scripts for the Terminal's "js"/"python" builtins (see
# gui/shell.c) -- exercise a recursive function, a loop, and (for
# Python specifically) real float division, to make sure a fresh disk
# image always has something to try both interpreters on immediately.
cat > "$STAGE/HELLO.JS" <<'EOF'
function fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
console.log("Hello from the JS engine!");
for (var i = 0; i < 8; i++) {
    console.log("fib(" + i + ") = " + fib(i));
}
EOF

cat > "$STAGE/HELLO.PY" <<'EOF'
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print("Hello from the Python interpreter!")
print("1 / 2 =", 1 / 2)
print("7 // 2 =", 7 // 2)
for i in range(8):
    print("fib(" + str(i) + ") =", fib(i))
EOF

mmd -i "$IMG" ::/DOCS
mcopy -i "$IMG" "$STAGE/README.TXT" ::/README.TXT
mcopy -i "$IMG" "$STAGE/NOTES.TXT" ::/NOTES.TXT
mcopy -i "$IMG" "$STAGE/ABOUTFS.TXT" ::/DOCS/ABOUTFS.TXT
mcopy -i "$IMG" "$STAGE/HELLO.JS" ::/HELLO.JS
mcopy -i "$IMG" "$STAGE/HELLO.PY" ::/HELLO.PY

# A short 48kHz/16-bit/stereo test tone for the File Manager's audio
# playback (net/ac97.c only supports that exact format -- no
# resampling). Generated on the fly rather than committed as a binary
# asset; skipped gracefully if python3 isn't available.
if command -v python3 >/dev/null 2>&1; then
  python3 - "$STAGE/SONG.WAV" <<'PYEOF'
import wave, struct, math, sys

rate = 48000
seconds = 2.5
amplitude = 12000

with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    frames = bytearray()
    n = int(rate * seconds)
    for i in range(n):
        t = i / rate
        # a little melody instead of a flat tone: three notes in sequence
        freq = 440.0 if t < seconds / 3 else (554.37 if t < 2 * seconds / 3 else 659.25)
        sample = int(amplitude * math.sin(2 * math.pi * freq * t))
        frames += struct.pack("<hh", sample, sample)
    w.writeframes(bytes(frames))
PYEOF
  mcopy -i "$IMG" "$STAGE/SONG.WAV" ::/SONG.WAV
  echo "Added SONG.WAV (48kHz/16-bit/stereo test tone)"
else
  echo "python3 not found -- skipping SONG.WAV test asset"
fi

# A tiny standalone ELF32 executable for the File Manager's "run .ELF"
# feature (kernel/elf.c), built fresh here rather than committed as a
# binary asset -- same freestanding flags as the kernel itself, linked
# at the loader's fixed user-program base (userprogs/user.ld).
if command -v gcc >/dev/null 2>&1 && command -v ld >/dev/null 2>&1; then
  gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin -nostdlib -O2 \
      -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
      -c userprogs/hello.c -o "$STAGE/hello.o"
  ld -m elf_i386 -T userprogs/user.ld -nostdlib -o "$STAGE/TEST.ELF" "$STAGE/hello.o"
  mcopy -i "$IMG" "$STAGE/TEST.ELF" ::/TEST.ELF
  echo "Added TEST.ELF (sample ELF32 program for the File Manager to run)"

  # A second, deliberately misbehaving program that proves the ELF
  # loader's per-process isolation actually contains a bad program
  # instead of just being decoration -- see userprogs/evil.c.
  gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin -nostdlib -O2 \
      -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
      -c userprogs/evil.c -o "$STAGE/evil.o"
  ld -m elf_i386 -T userprogs/user.ld -nostdlib -o "$STAGE/EVIL.ELF" "$STAGE/evil.o"
  mcopy -i "$IMG" "$STAGE/EVIL.ELF" ::/EVIL.ELF
  echo "Added EVIL.ELF (misbehaving program to test isolation containment)"

  # Two float-arithmetic programs (same source, userprogs/fputest.c,
  # built twice with different constants) that prove the scheduler's
  # per-task FPU save/restore (kernel/fpu.c) actually isolates FPU state
  # between tasks -- unlike TEST/EVIL/DOOM, these are compiled WITHOUT
  # -mgeneral-regs-only/-mno-80387, since they need real x87 float math.
  gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin -nostdlib -O2 \
      -mno-sse -mno-sse2 -mno-mmx \
      -DFPUTEST_NAME='"FPUTEST1"' -DFPUTEST_A=3.5f -DFPUTEST_B=2.0f -DFPUTEST_EXPECT=7 \
      -c userprogs/fputest.c -o "$STAGE/fputest1.o"
  ld -m elf_i386 -T userprogs/user.ld -nostdlib -o "$STAGE/FPUTES1.ELF" "$STAGE/fputest1.o"
  mcopy -i "$IMG" "$STAGE/FPUTES1.ELF" ::/FPUTES1.ELF

  gcc -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin -nostdlib -O2 \
      -mno-sse -mno-sse2 -mno-mmx \
      -DFPUTEST_NAME='"FPUTEST2"' -DFPUTEST_A=9.0f -DFPUTEST_B=-3.0f -DFPUTEST_EXPECT=-27 \
      -c userprogs/fputest.c -o "$STAGE/fputest2.o"
  ld -m elf_i386 -T userprogs/user.ld -nostdlib -o "$STAGE/FPUTES2.ELF" "$STAGE/fputest2.o"
  mcopy -i "$IMG" "$STAGE/FPUTES2.ELF" ::/FPUTES2.ELF
  echo "Added FPUTES1.ELF/FPUTES2.ELF (float cross-contamination test pair)"

  # A real, complete port of DOOM (doomgeneric, see userprogs/doom/) --
  # freestanding, no FPU, its own libc (doomlibc.c), and the shareware
  # IWAD linked directly into the binary's .data section (ld -r -b
  # binary) so it needs no file-read syscall at all. Built the same way
  # as TEST.ELF/EVIL.ELF, just with many more source files and its own
  # include shims (userprogs/doom/zapos/).
  if [ -f userprogs/doom/doom1.wad ]; then
    DOOMDIR=userprogs/doom
    DOOMOBJDIR="$STAGE/doomobj"
    mkdir -p "$DOOMOBJDIR"
    DOOMFLAGS="-m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
      -fno-builtin -nostdlib -nostdinc -O1 \
      -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mgeneral-regs-only \
      -I$DOOMDIR -I$DOOMDIR/zapos -isystem $(gcc -m32 -print-file-name=include)"
    for f in "$DOOMDIR"/*.c; do
      base=$(basename "$f" .c)
      gcc $DOOMFLAGS -c "$f" -o "$DOOMOBJDIR/$base.o"
    done
    (cd "$DOOMDIR" && ld -m elf_i386 -r -b binary -o "$DOOMOBJDIR/wad.o" doom1.wad)
    ld -m elf_i386 -T userprogs/user.ld -nostdlib -o "$STAGE/DOOM.ELF" "$DOOMOBJDIR"/*.o
    mcopy -i "$IMG" "$STAGE/DOOM.ELF" ::/DOOM.ELF
    echo "Added DOOM.ELF (full DOOM port, shareware WAD embedded)"
  else
    echo "userprogs/doom/doom1.wad not found -- skipping DOOM.ELF"
  fi
else
  echo "gcc/ld not found -- skipping TEST.ELF/EVIL.ELF/DOOM.ELF test assets"
fi

echo "Built $IMG"
mdir -i "$IMG" ::

if command -v qemu-img >/dev/null 2>&1; then
  qemu-img convert -f raw -O vmdk "$IMG" zapos_disk.vmdk
  echo "Also built zapos_disk.vmdk (for VMware/VirtualBox -- attach as an IDE disk)"
fi
