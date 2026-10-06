# Storage check regression test

This standalone test builds the production worker and process wrapper against a mock child process.
It checks successful completion, failure diagnostics without a trailing newline, and a nonzero exit after
the completion marker. It does not start a game server or open databases. Temporary files stay in the test build.

```sh
cmake -S tests/storage-check -B build-storage-check -DLONELYICE_CORE_DIR=/path/to/core
cmake --build build-storage-check
ctest --test-dir build-storage-check --output-on-failure
```
