# Import fidelity fixes — 2026-10-02

Base: `2114a791fd6cd8c9d019494e0a704798d93416d1`.
Work is isolated on `fix/import-musical-fidelity-20261002`; main is unchanged.

## Reproduced before the change

- An eighth grace before a whole-note C/E chord moved the following 4/4
  measure from tick 3840 to 4320. Only C had repaid the grace's borrowed time.
- Adopting a CC64 lane starting with 127 at tick zero emitted 0 at frame zero.
- Selecting P2 written in 3/4 after an all-parts read retained P1's 4/4 map.

## Verified after the change

`tests/import_fidelity_tests.cpp` exercises equal/unequal principal chord
durations, cross-bar graces, a second voice, and a following undecorated chord.
Original chord releases and following measure starts remain unchanged.

Controller checks cover CC64/CC11, nondefault time-zero values, later changes,
constant lanes (including the default), unified undo/redo, and document reopen.

Part selection runs inside the XML reader, while local meters still exist.
Checks cover initial 3/4 and a later 2/4 change, project/XML round trips, pedal
channel remapping, invalid selection preserving the caller's Score, and the
unchanged strict all-parts rejection. Tempo remains score-global.

Local commands and results:

```sh
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
# 48/48 passed; build log contains no warnings/errors.

cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-sanitize -j 4 --target \
  daw_import_fidelity_tests daw_score_import_repair_tests daw_performance_tests
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure \
  -R 'daw_(import_fidelity|score_import_repair|performance)_tests'
# 3/3 passed. Address/undefined-behavior checks; leak detection disabled.

python3 scripts/check_import_fidelity.py --build build
# PASS selected meter=3/4 with later change; pedal adoption/undo;
# exact document reopen; plugins_opened=0 output_devices_opened=0.
```

An initial ad-hoc CLI assertion incorrectly expected the project token
`time_signature`; the actual saved format uses `meter`. The saved value was
already correct. The permanent CLI check above uses the real format and passed.

No hardware/listening or corpus-wide accuracy claim is made. Old projects
with discarded source information need re-import, not automatic rewriting.
No workflow was dispatched or retried; the commit uses `[skip ci]`.
