#!/bin/bash
# The Windows release, cross-compiled on the Mac with MinGW-w64 (brew install mingw-w64), as
# vf1-recomp's: build/win/VirtuaFighter3Recomp-windows.zip with VirtuaFighter3.exe, SDL3.dll, the
# assets, README and LICENSES. No ROMs or anything made from them: the player chooses their own
# vf3.zip at the first launch. SDL3, FreeType and zlib come from the checked tarballs
# tools/mac/deps.sh fetches.
set -e
cd "$(dirname "$0")/../.."
SDL_VERSION=3.4.18
FT_VERSION=2.14.3
ZLIB_VERSION=1.3.1
T=x86_64-w64-mingw32
D=$PWD/build/win
mkdir -p $D
[ -f build/deps/SDL3.tar.gz ] && [ -f build/deps/zlib.tar.gz ] || tools/mac/deps.sh

cat > $D/toolchain.cmake <<TC
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $T-gcc)
set(CMAKE_CXX_COMPILER $T-g++)
set(CMAKE_RC_COMPILER $T-windres)
set(CMAKE_FIND_ROOT_PATH $(dirname $(dirname $(which $T-gcc)))/$T)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
TC

# SDL3 (SDL3.dll, shipped next to the game), built once
if [ ! -f $D/sdl/bin/SDL3.dll ]; then
	rm -rf $D/SDL-src $D/sdl-build
	tar xzf build/deps/SDL3.tar.gz -C $D && mv $D/SDL3-$SDL_VERSION $D/SDL-src
	cmake -S $D/SDL-src -B $D/sdl-build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$D/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$D/sdl -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF >/dev/null
	cmake --build $D/sdl-build -j$(nproc 2>/dev/null || echo 8) >/dev/null
	cmake --install $D/sdl-build >/dev/null
fi
# FreeType (static), built once
if [ ! -f $D/ft/lib/libfreetype.a ]; then
	rm -rf $D/ft-src $D/ft-build
	tar xJf build/deps/freetype.tar.xz -C $D && mv $D/freetype-$FT_VERSION $D/ft-src
	cmake -S $D/ft-src -B $D/ft-build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$D/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$D/ft -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
		-DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON >/dev/null
	cmake --build $D/ft-build -j$(nproc 2>/dev/null || echo 8) >/dev/null
	cmake --install $D/ft-build >/dev/null
fi
# zlib (static), built once
if [ ! -f $D/zlib/lib/libz.a ]; then
	rm -rf $D/zlib-src && tar xzf build/deps/zlib.tar.gz -C $D && mv $D/zlib-$ZLIB_VERSION $D/zlib-src
	make -C $D/zlib-src -f win32/Makefile.gcc PREFIX=$T- libz.a >/dev/null
	mkdir -p $D/zlib/lib $D/zlib/include
	cp $D/zlib-src/libz.a $D/zlib/lib/ && cp $D/zlib-src/zlib.h $D/zlib-src/zconf.h $D/zlib/include/
fi

# the icon, the manifest (UTF-8 file names) and the version (Properties > Details), as resources: an unsigned
# exe that says nothing about itself is what antivirus heuristics flag most. Between tags (or before the
# first) the version is a commit: its numbers are then 0.0.0.0, the strings carry the commit.
VER=${VF3_VERSION:-$(git describe --tags --always 2>/dev/null || echo v0.0.0)}   # (VF3_VERSION=v0.1.0 for a release)
if echo "$VER" | grep -qE '^v?[0-9]+(\.[0-9]+)*(-(alpha|beta|rc)[0-9]*)?(-[0-9]+-g[0-9a-f]+)?$'; then   # (a label, as v0.1.0-alpha, left out of the numbers)
	V4=$(echo "$VER" | sed -E 's/^v//; s/-([0-9]+)-g[0-9a-f]+$/.\1/; s/-(alpha|beta|rc)[0-9]*//' | awk -F. '{ printf "%d,%d,%d,%d", $1, $2, $3, $4 }')
else
	V4=0,0,0,0
fi
cat > $D/vf3.rc <<RC
1 ICON "$PWD/assets/icon/VF3.ico"
1 24 "$PWD/tools/win/VirtuaFighter3.manifest"
1 VERSIONINFO
FILEVERSION $V4
PRODUCTVERSION $V4
FILEOS 0x40004
FILETYPE 0x1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "Virtua Fighter 3 Recomp"
      VALUE "FileDescription", "Virtua Fighter 3 Recomp"
      VALUE "FileVersion", "$VER"
      VALUE "InternalName", "VirtuaFighter3"
      VALUE "LegalCopyright", "Unofficial fan-made port. Not affiliated with or endorsed by SEGA."
      VALUE "OriginalFilename", "VirtuaFighter3.exe"
      VALUE "ProductName", "Virtua Fighter 3 Recomp"
      VALUE "ProductVersion", "$VER"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
RC
$T-windres $D/vf3.rc -O coff -o $D/vf3_res.o

# the game: libgcc, libstdc++ and winpthreads linked in, so SDL3.dll is the only DLL it brings
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) CXX=${WIN_CXX:-$T-g++} OBJ=build/win/obj BIN=build/win/bin \
	EXTRA_CFLAGS="-I$D/zlib/include" \
	LDLIBS="$D/vf3_res.o -L$D/zlib/lib -lz -lws2_32 -mwindows -static -Wl,--stack,16777216" \
	SDL_CFLAGS="-I$D/sdl/include" SDL_LIBS="$D/sdl/lib/libSDL3.dll.a" \
	FT_CFLAGS="-I$D/ft/include/freetype2" FT_LIBS="$D/ft/lib/libfreetype.a" \
	build/win/bin/vf3

# the package
P=$D/VirtuaFighter3
rm -rf $P && mkdir -p $P/assets
cp build/win/bin/vf3.exe $P/VirtuaFighter3.exe
$T-strip $P/VirtuaFighter3.exe
cp $D/sdl/bin/SDL3.dll $P/
mkdir -p $P/assets/icon && cp -R assets/ui assets/fonts $P/assets/ && cp assets/icon/icon.png $P/assets/icon/
cp tools/win/README.txt $P/
tools/release/licenses.sh $P/LICENSES.txt --windows
rm -f $D/VirtuaFighter3Recomp-windows.zip
(cd $D && zip -qr VirtuaFighter3Recomp-windows.zip VirtuaFighter3)
ls -la $P $D/VirtuaFighter3Recomp-windows.zip
