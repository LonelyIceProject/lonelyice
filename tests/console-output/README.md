# Console protocol regression

Build `test.cpp` together with `src/common/Platform.cpp`, with `src/common` on the include path,
using C++20 and threading support. On Windows link shell32, ole32, uuid and psapi as for StorageCheck tests.

Or run `cmake -S tests/console-output -B build/console-output`,
`cmake --build build/console-output` and `ctest --test-dir build/console-output --output-on-failure`.

The child reproduces the POSIX console appender's colour-reset placement after a newline. It proves
that the previous prefix parser loses a successful deploy with exit code zero, while colour stripping
recognizes it. Three-byte pipe reads cover escape/marker fragmentation; plain success, missing markers
and database failures retain their previous meaning. No game server or databases are started.
