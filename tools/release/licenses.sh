#!/bin/sh
# Writes LICENSES.txt for a download: the notices of everything built into the program.
#   tools/release/licenses.sh [out] [--windows]    (default build/release/LICENSES.txt; --windows adds
#                                                  the MinGW-w64 runtime the Windows exe links in)
# SDL3's, FreeType's and zlib's come from their release tarballs in build/deps (tools/mac/deps.sh).
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-build/release/LICENSES.txt}
D=build/deps
[ -f $D/SDL3.tar.gz ] && [ -f $D/freetype.tar.xz ] && [ -f $D/zlib.tar.gz ] || tools/mac/deps.sh
mkdir -p "$(dirname "$OUT")"
sec() { printf '\n\n==============================================================================\n%s\n==============================================================================\n\n' "$1"; }
{
echo "Third-party software in Virtua Fighter 3 Recomp, and its licences."
sec "MAME (BSD-3-Clause): the Motorola 68000 core and the Yamaha SCSP and its DSP, as Virtua Fighter 2 Recomp has them, and the Model 3 sound board's map"
cat <<'BSD'
From MAME (https://github.com/mamedev/mame):
  cpu/m68000 (68000 core and its generated handlers): Olivier Galibert
  sound/scsp.cpp, sound/scspdsp.cpp (SCSP and its DSP): ElSemi, R. Belmont; thanks to kingshriek
  emu/diserial.cpp (serial link): Carl, Miodrag Milanovic
  sega/model3.cpp (the sound board's map): R. Belmont, Ville Linde

Copyright-holders: Olivier Galibert, ElSemi, R. Belmont, Carl, Miodrag Milanovic, Ville Linde, and the
MAME team.

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions
   and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of
   conditions and the following disclaimer in the documentation and/or other materials provided
   with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
BSD
sec "SLJIT, Zoltan Herczeg (BSD-2-Clause): the native code the game is recompiled to"; cat third_party/sljit/LICENSE
sec "RmlUi (MIT)"; cat third_party/rmlui/LICENSE.txt
sec "robin_hood and itlib (MIT), built into RmlUi"; cat third_party/rmlui/Include/RmlUi/Core/Containers/LICENSE.txt
sec "SDL 3 (zlib)"; tar xzf $D/SDL3.tar.gz -O SDL3-3.4.18/LICENSE.txt
sec "FreeType (FreeType License)"
echo "Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved."
echo; tar xJf $D/freetype.tar.xz -O freetype-2.14.3/docs/FTL.TXT
sec "zlib, Jean-loup Gailly and Mark Adler"; tar xzf $D/zlib.tar.gz -O zlib-1.3.1/LICENSE
sec "Barlow Condensed font (SIL Open Font License 1.1)"; cat assets/fonts/OFL.txt
sec "flag-icons, Panayiotis Lipiridis (MIT): the countries' flags"; cat assets/ui/flags-LICENSE.txt
if [ "$2" = --windows ]; then
	sec "MinGW-w64 runtime (the Windows build links it in)"; cat tools/win/licenses/mingw-w64-runtime.txt
	sec "winpthreads, the MinGW-w64 project (the Windows build links it in)"; cat tools/win/licenses/winpthreads.txt
fi
} > "$OUT"
