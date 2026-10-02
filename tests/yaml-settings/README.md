This standalone test compiles the production settings model, YAML profile reader/writer, launcher settings, plugin schema reader, and internal default-template reader. Small translation and path conversion helpers replace dependencies unrelated to the YAML behavior under test.

```sh
cmake -S tests/yaml-settings -B build-yaml-tests -DLONELYICE_CORE_DIR=/path/to/azerothcore
cmake --build build-yaml-tests
ctest --test-dir build-yaml-tests --output-on-failure
```

The test checks typed values, exact 64-bit integers, existing local overrides, unknown mappings and sequences, package metadata, explicit empty schema defaults, strict input validation, and dirty-state retention after a write error. It also verifies that a generated `.conf` file cannot override YAML values or receive UI edits. Windows injects a read-only profile; POSIX injects a directory permission failure unless running as root. Fixtures are retained in the temporary directory reported by the test.
