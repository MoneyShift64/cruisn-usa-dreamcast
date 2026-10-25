#!/bin/bash
# Build the Dreamcast port of the race (the decompiled game code running on
# the Dreamcast) and a disc image:
#     source /opt/toolchains/dc/kos/environ.sh
#     cd dc/game && ./build.sh
# Needs baserom.z64 in the cruisn folder (the game's data is converted from it).
set -e
cd "$(dirname "$0")"
ROOT=../..
mkdir -p obj cd

# 0. your ROM: Cruis'n USA (USA) (Rev 1), big-endian .z64, as baserom.z64 in the top folder
if [ ! -f $ROOT/baserom.z64 ]; then
    echo "baserom.z64 not found: copy your own Cruis'n USA (USA) (Rev 1) ROM to the top folder as baserom.z64"
    exit 1
fi
SUM=$( (shasum -a 1 $ROOT/baserom.z64 2>/dev/null || sha1sum $ROOT/baserom.z64) | cut -d' ' -f1)
if [ "$SUM" != "71bb3d8850b6a4a294aeca2abad1f936e4f85f0f" ]; then
    echo "baserom.z64 is not Cruis'n USA (USA) (Rev 1) in .z64 byte order (SHA-1 $SUM)"
    exit 1
fi

# 1. the game's variables and files, from the ROM
python3 $ROOT/tools/port_data_apply.py gen
python3 $ROOT/tools/port_assets.py cd/ASSETS.BIN     # (always: it includes the port's own files)
python3 $ROOT/tools/port_sound.py $ROOT/baserom.z64 cd/SOUND.BIN   # (always: it includes the port's own tunes)

# 2. compile
U=$ROOT/port/ultra_include
GAMEFLAGS="-O2 -DPORT -DPORT_F64_IS_FLOAT -fsingle-precision-constant -I$ROOT/include -I$U -I$U/PR -I$U/compiler/gcc -D_LANGUAGE_C -DF3D_GBI -D_FINALROM -DNDEBUG -fno-strict-aliasing -fpermissive -w"
OBJS=""
for f in $(ls $ROOT/src/*.c $ROOT/src/race/*.c $ROOT/src/audio/*.c $ROOT/src/frontend/*.c | grep -v '/boot\.c$'); do
    o=obj/$(echo $f | sed 's|^\.\./\.\./||; s|/|_|g; s|\.c$|.o|')
    echo "  $f"
    kos-cc $GAMEFLAGS -c $f -o $o        # always: unzipped files keep their old dates
    OBJS="$OBJS $o"
done
kos-cc $GAMEFLAGS -I$ROOT/port -c $ROOT/port/port_main.c -o obj/port_main.o
kos-cc $GAMEFLAGS -I$ROOT/port -c $ROOT/port/frontend_stubs.c -o obj/frontend_stubs.o
kos-cc $GAMEFLAGS -I$ROOT/port -c $ROOT/port/save.c -o obj/port_save.o
kos-cc $GAMEFLAGS -O3 -g -I$ROOT/port -c $ROOT/port/audio.c -o obj/port_audio.o
for f in $ROOT/port/segments.c $ROOT/port/repl_tex.c plat_dc.c gen/relocs.c gen/stubs.c; do
    o=obj/$(basename $f .c).o
    kos-cc -O2 -fsingle-precision-constant -fno-strict-aliasing -w -I$ROOT/port -c $f -o $o
    OBJS="$OBJS $o"
done
# the N64 graphics translation is most of the frame: the fastest settings
kos-cc -O3 -g -ffast-math -fsingle-precision-constant -fno-strict-aliasing -w -I$ROOT/port -c $ROOT/port/gfx.c -o obj/gfx.o
OBJS="$OBJS obj/gfx.o"
kos-cc -c gen/segs.S -o obj/segs.o -Wa,-Igen
OBJS="$OBJS obj/port_main.o obj/frontend_stubs.o obj/port_save.o obj/port_audio.o obj/segs.o"

# 3. link (gen/globals.ld places the game's variables inside the segments)
kos-cc -o cruisn-game.elf $OBJS gen/globals.ld -lm

# 4. disc image
M=$(command -v mkdcdisc || find "$HOME/dreamcast-tools" "$HOME/dreamcast-projects" /opt /usr/local -name mkdcdisc -type f 2>/dev/null | head -1)
"$M" -e cruisn-game.elf -D cd -n "CRUISN USA" -o cruisn-game.cdi --allow-overwrite
echo "built cruisn-game.cdi"
