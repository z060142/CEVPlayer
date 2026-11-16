#!/usr/bin/env bash
# Builds an "AssimpSDK" for the CECE fork (HPirani/CryEngine5-7-Assimp is no longer public):
# Assimp 5.4.3 from source, layout expected by Tools/CMake/modules/AssimpSdk.cmake and RCCommonMacros.cmake:
#   AssimpSDK/include, AssimpSDK/code, AssimpSDK/lib/{Debug,Release}/{assimp-vc143-mt[d],assimp-vc143-mt[d]-static-crt,zlibstatic[d]}.lib
set -euo pipefail
src=D:/coding11/_tools/assimp/assimp-5.4.3
out=${1:?target AssimpSDK dir}
cm="D:/coding11/CryENGINE-5-7-community/Tools/CMake/Win32/bin/cmake.exe"
common=(-G "Visual Studio 17 2022" -A x64 -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DBUILD_SHARED_LIBS=OFF -DASSIMP_BUILD_ZLIB=ON -DASSIMP_WARNINGS_AS_ERRORS=OFF -DASSIMP_INSTALL=OFF -DASSIMP_NEW_INTERFACE=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
mkdir -p "$out/lib/Debug" "$out/lib/Release"
for crt in dyn static; do
  b=D:/coding11/_tools/assimp/build-$crt
  if [ $crt = static ]; then extra=(-DUSE_STATIC_CRT=ON); suffix=-static-crt; else extra=(-DUSE_STATIC_CRT=OFF); suffix=; fi
  "$cm" -S "$src" -B "$b" "${common[@]}" "${extra[@]}"
  for cfg in Debug Release; do
    "$cm" --build "$b" --config $cfg --target assimp -- -m
    d=; [ $cfg = Debug ] && d=d
    cp "$b/lib/$cfg/assimp-vc143-mt$d.lib" "$out/lib/$cfg/assimp-vc143-mt$d$suffix.lib"
    if [ $crt = dyn ]; then cp "$b/contrib/zlib/$cfg/zlibstatic$d.lib" "$out/lib/$cfg/zlibstatic$d.lib" 2>/dev/null || find "$b" -name "zlibstatic$d.lib" -exec cp {} "$out/lib/$cfg/" \; ; fi
  done
done
mkdir -p "$out/include" "$out/code"
cp -r "$src/include/assimp" "$out/include/"
cp "$(dirname "$b")/build-dyn/include/assimp/config.h" "$out/include/assimp/config.h"
cp -r "$src/code/." "$out/code/"
[ -d "$src/contrib" ] && mkdir -p "$out/contrib" || true
echo ok
