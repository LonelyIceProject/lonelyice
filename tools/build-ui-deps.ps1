# Builds static FreeType, SDL3, RmlUi, StormLib (client MPQs) and miniz (plugin packages) for LonelyIce into deps\{freetype,sdl3,rmlui,stormlib,miniz}.
# Sources are cloned into deps\src on the first run (git needed). Usage: tools\build-ui-deps.ps1 [-Jobs 8]
param([int]$Jobs = [Environment]::ProcessorCount)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$src  = "$root\deps\src"
$bld  = "$root\deps\src\_build"
$log  = "$bld\ui-deps.log"
New-Item -ItemType Directory -Force $bld | Out-Null
"" | Set-Content $log

$sources = @{
    freetype = @('https://github.com/freetype/freetype.git', 'VER-2-13-3')
    sdl3     = @('https://github.com/libsdl-org/SDL.git', 'release-3.2.24')
    rmlui    = @('https://github.com/mikke89/RmlUi.git', '6.1')
    stormlib = @('https://github.com/ladislav-zezula/StormLib.git', 'v9.30')
    miniz    = @('https://github.com/richgel999/miniz.git', '3.0.2')
}
foreach ($name in $sources.Keys) {
    if (-not (Test-Path "$src\$name\CMakeLists.txt")) {
        git clone --depth 1 --branch $sources[$name][1] $sources[$name][0] "$src\$name" *>> $log
        if ($LASTEXITCODE -ne 0) { throw "clone $name failed, see $log" }
    }
}

function Build($name, [string[]]$opts) {
    $b = "$bld\$name"
    cmake -S "$src\$name" -B $b -G "Visual Studio 17 2022" -A x64 "-DCMAKE_INSTALL_PREFIX=$root\deps\$name" `
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DCMAKE_POLICY_DEFAULT_CMP0091=NEW @opts *>> $log
    if ($LASTEXITCODE -ne 0) { throw "configure $name failed, see $log" }
    cmake --build $b --config Release --parallel $Jobs *>> $log
    if ($LASTEXITCODE -ne 0) { throw "build $name failed, see $log" }
    cmake --install $b --config Release *>> $log
    if ($LASTEXITCODE -ne 0) { throw "install $name failed, see $log" }
    "$name OK"
}

Build freetype @('-DBUILD_SHARED_LIBS=OFF', '-DFT_DISABLE_ZLIB=ON', '-DFT_DISABLE_BZIP2=ON', '-DFT_DISABLE_PNG=ON',
    '-DFT_DISABLE_HARFBUZZ=ON', '-DFT_DISABLE_BROTLI=ON')
Build sdl3 @('-DSDL_SHARED=OFF', '-DSDL_STATIC=ON', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF')
Build stormlib @('-DBUILD_SHARED_LIBS=OFF', '-DSTORM_UNICODE=ON', '-DSTORM_USE_BUNDLED_LIBRARIES=ON', '-DSTORM_BUILD_TESTS=OFF')
Build miniz @('-DBUILD_SHARED_LIBS=OFF', '-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTS=OFF', '-DINSTALL_PROJECT=ON')
Build rmlui @('-DBUILD_SHARED_LIBS=OFF', '-DRMLUI_SAMPLES=OFF', '-DRMLUI_FONT_ENGINE=freetype', '-DBUILD_TESTING=OFF',
    "-DFreetype_ROOT=$root\deps\freetype", "-DCMAKE_PREFIX_PATH=$root\deps\freetype")
