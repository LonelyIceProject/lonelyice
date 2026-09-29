#!/usr/bin/env bash
# Builds static FreeType, SDL3, RmlUi, StormLib (client MPQs), miniz (plugin packages) and libcurl (downloads) for LonelyIce
# into deps/{freetype,sdl3,rmlui,stormlib,miniz,curl} on Linux and macOS (the Windows version is build-ui-deps.ps1).
# Sources are cloned/downloaded into deps/src on the first run (git needed). Usage: tools/build-ui-deps.sh
# Parallel jobs: the number of CPUs, or JOBS when it is set and smaller (JOBS=4 tools/build-ui-deps.sh).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/deps/src"
bld="$root/deps/src/_build"
log="$bld/ui-deps.log"
mkdir -p "$bld"
: > "$log"

fail() {
    echo "$1, see $log" >&2
    exit 1
}

# Parallel jobs
if command -v nproc > /dev/null 2>&1; then
    cpus="$(nproc)"
elif command -v sysctl > /dev/null 2>&1; then
    cpus="$(sysctl -n hw.ncpu)"
else
    cpus=2
fi
jobs="$cpus"
if [[ -n "${JOBS:-}" ]]; then
    if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -lt 1 ]]; then
        echo "JOBS must be a positive number" >&2
        exit 1
    fi
    if [[ "$JOBS" -lt "$jobs" ]]; then
        jobs="$JOBS"
    fi
fi

sha256() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

download() {
    if command -v curl > /dev/null 2>&1; then
        curl -fL --retry 3 -o "$2" "$1" >> "$log" 2>&1
    else
        wget -O "$2" "$1" >> "$log" 2>&1
    fi
}

clone() {
    local name="$1" url="$2" tag="$3"
    if [[ ! -f "$src/$name/CMakeLists.txt" ]]; then
        git clone --depth 1 --branch "$tag" "$url" "$src/$name" >> "$log" 2>&1 || fail "clone $name failed"
    fi
}

clone freetype https://github.com/freetype/freetype.git VER-2-13-3
clone sdl3 https://github.com/libsdl-org/SDL.git release-3.2.24
clone rmlui https://github.com/mikke89/RmlUi.git 6.1
clone stormlib https://github.com/ladislav-zezula/StormLib.git v9.30
clone miniz https://github.com/richgel999/miniz.git 3.0.2

# libcurl comes as a release tarball (checked against its published SHA-256).
curl_version="8.22.0"
curl_sha256="d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1"
if [[ ! -f "$src/curl/CMakeLists.txt" ]]; then
    tgz="$bld/curl-$curl_version.tar.gz"
    download "https://curl.se/download/curl-$curl_version.tar.gz" "$tgz" || fail "download curl failed"
    if [[ "$(sha256 "$tgz")" != "$curl_sha256" ]]; then
        echo "curl-$curl_version.tar.gz checksum mismatch" >&2
        exit 1
    fi
    tar -xzf "$tgz" -C "$src" >> "$log" 2>&1 || fail "unpack curl failed"
    rm -rf "$src/curl"
    mv "$src/curl-$curl_version" "$src/curl"
fi

build() {
    local name="$1"
    shift
    local b="$bld/$name"
    cmake -S "$src/$name" -B "$b" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$root/deps/$name" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" >> "$log" 2>&1 || fail "configure $name failed"
    cmake --build "$b" --config Release --parallel "$jobs" >> "$log" 2>&1 || fail "build $name failed"
    cmake --install "$b" --config Release >> "$log" 2>&1 || fail "install $name failed"
    echo "$name OK"
}

build freetype -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
build sdl3 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
# Paths are char (UTF-8) outside Windows: no STORM_UNICODE.
build stormlib -DBUILD_SHARED_LIBS=OFF -DSTORM_UNICODE=OFF -DSTORM_USE_BUNDLED_LIBRARIES=ON -DSTORM_BUILD_TESTS=OFF
build miniz -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_PROJECT=ON
build rmlui -DBUILD_SHARED_LIBS=OFF -DRMLUI_SAMPLES=OFF -DRMLUI_FONT_ENGINE=freetype -DBUILD_TESTING=OFF \
    -DFreetype_ROOT="$root/deps/freetype" -DCMAKE_PREFIX_PATH="$root/deps/freetype"

# HTTP(S) only, TLS through OpenSSL 3 (the one the server uses too), no compression/IDN/PSL/SSH, library only.
openssl_opts=()
if [[ "$(uname -s)" == "Darwin" ]] && command -v brew > /dev/null 2>&1; then
    if openssl_root="$(brew --prefix openssl@3 2> /dev/null)" && [[ -d "$openssl_root" ]]; then
        openssl_opts+=("-DOPENSSL_ROOT_DIR=$openssl_root")
    fi
fi
build curl -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
    -DHTTP_ONLY=ON -DCURL_USE_SCHANNEL=OFF -DCURL_USE_OPENSSL=ON -DCURL_USE_LIBPSL=OFF \
    -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
    -DUSE_LIBIDN2=OFF -DUSE_APPLE_IDN=OFF -DUSE_NGHTTP2=OFF -DCURL_USE_PKGCONFIG=OFF "${openssl_opts[@]+"${openssl_opts[@]}"}"
