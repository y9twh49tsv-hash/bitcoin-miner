# CLAUDE.md — AZD Bitcoin Miner

Guidance for future Claude Code sessions working in this repository.
Read this before changing anything.

---

## PERMANENT PROJECT RULES

These are not style preferences. They are the rules that make this project
trustworthy. Do not relax them, do not "temporarily" work around them, and do
not add a flag that disables them.

1. **Never fabricate mining statistics.** Hashrate, shares, temperature, power,
   utilization, blocks and pool status may only ever change because the program
   really did the thing being reported. No seeding, no placeholder values, no
   "reasonable defaults", no smoothing toward an expected number. If a value
   cannot be measured, report it as unavailable (`null` in JSON, `N/A` in the
   dashboard) — never as `0` and never as a guess.
2. **Never insert a developer wallet.** No Bitcoin address, pool username or
   payout destination belongs anywhere in this source tree. The user supplies
   their own via config or the dashboard.
3. **No developer fee.** No share, no nonce range, no fraction of time is
   diverted anywhere. There is no donation mode to add.
4. **Never mine when status says stopped.** Worker threads check the run state
   and exit their loops promptly. Stopping means hashing actually stops.
5. **CPU SHA-256d is the correctness reference.** `src/bitcoin/sha256.cpp` and
   `CpuReferenceMiner` define what "correct" means for this project.
6. **Every CUDA result must be CPU verified.** A nonce found on the GPU is
   recomputed with `CpuReferenceMiner::verify()` before it is submitted or
   counted. A result that fails verification is a backend bug: log it loudly,
   do not submit it.
7. **CUDA must remain optional so cloud CI works.** `AZD_ENABLE_CUDA` defaults
   to `OFF`, and the whole project must configure, build and pass tests without
   nvcc present. Never add an unconditional CUDA dependency to `azd_core`.
8. **Never disable GPU thermal protection.** The temperature warning/stop
   thresholds stay. Do not add an override switch.
9. **Never modify GPU BIOS.** Out of scope, permanently.
10. **Never overvolt the GPU.** No voltage, power-limit or clock manipulation.
11. **Do not expose pool passwords in logs or APIs.** The password goes on the
    wire to the pool because Stratum requires it, and nowhere else. `GET`
    endpoints expose `passwordSet` (a boolean), never the value. Use
    `util::redactSecret()` if a password must be mentioned in a log line.
12. **Changes should include tests where appropriate.** New protocol handling,
    new arithmetic and new parsing all need tests. Bug fixes get a regression
    test.

---

## Target hardware

The home machine this project is ultimately built for:

```
GPU: NVIDIA GeForce RTX 5070   (Blackwell, compute capability 12.0 -> sm_120)
OS:  Windows 11
```

The cloud development container has **no GPU and no CUDA toolkit**, which is
precisely why rule 7 exists. Build and test here; enable CUDA there.

---

## Build

```bash
# The build that must always work (cloud, CI, any machine with a C++20 compiler)
cmake -S . -B build -DAZD_ENABLE_CUDA=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# GPU build, only on a machine with the NVIDIA CUDA Toolkit
cmake -S . -B build -DAZD_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=120
```

Helper scripts: `scripts/build-cloud.sh`, `scripts/build-windows.ps1 [-Cuda]`.

Run `./build/bin/azd-miner --selftest` after any change to the hashing path.

---

## Architecture

```
src/
  util/       hex, JSON (hand-written, no third-party deps), logging
  core/       configuration load/merge/serialize (password write-only)
  bitcoin/    SHA-256, SHA-256d, Hash256, UInt256, block header, difficulty, merkle
  mining/     work model, backend interface, CPU reference miner, controller, statistics
  stratum/    line framing, message parse/build, TCP client with reconnection
  cuda/       CudaMiningBackend: real .cu implementation + honest non-CUDA stub
  api/        localhost HTTP server and JSON endpoints
dashboard/    static HTML/CSS/JS served by the API server
tests/        one executable per area, registered with ctest
```

Dependency direction is strictly downward: `bitcoin` knows nothing about
`stratum`, `stratum` knows nothing about `api`.

### Byte order — read this before touching hashes

This is where miners silently break.

- `Hash256` stores **internal byte order** — what SHA256d outputs, and what
  goes into the serialized header.
- Display order (block explorers, RPC, the genesis hash you can look up) is the
  **byte reverse**. Convert only through `toDisplayHex()` / `fromDisplayHex()`.
- Merkle branches from `mining.notify` are already in internal order and are
  concatenated directly — no reversal.
- Stratum's `prevhash` is the odd one: reverse the bytes **within each 4-byte
  word**, keeping word positions (`swapWordBytes`). This is derived from
  cgminer's data path and is covered by tests in `tests/test_stratum.cpp`.
- `version`, `nbits` and `ntime` arrive as big-endian hex and are stored as
  host integers, serialized little-endian.

### Exact arithmetic

The final hash-versus-target comparison is 256-bit integer arithmetic
(`UInt256`). Never route it through `double`. `toDoubleApproximate()` and
`shareDifficultyOfHashApproximate()` exist for display only and are named to
say so.

---

## Current status

| Area | State |
| --- | --- |
| SHA-256 / SHA-256d | Complete, tested against FIPS vectors |
| Block header + genesis test | Complete, genesis and block 1 both verified |
| Difficulty / 256-bit targets | Complete, tested |
| Merkle (branch + full tree) | Complete, verified against block 170 |
| Stratum V1 messages | Complete, parser tested with local samples |
| Stratum TCP client | Implemented; validated end-to-end against `scripts/mock_pool.py`; **not yet run against a real pool** |
| Mining work model | Complete, tested |
| CPU reference miner | Complete, recovers the genesis nonce |
| CUDA backend | Kernel written, **never executed on a GPU** |
| Statistics | Complete, tested |
| Local API + dashboard | Complete for the listed endpoints |

### Known validation gaps

Be honest about these; do not mark them done until they are really done.

1. **No REAL pool session has ever run.** The full flow (subscribe, authorize,
   set_difficulty, notify, submit, accept) has been exercised against
   `scripts/mock_pool.py`, which re-derives every submitted share with Python's
   `hashlib` and accepted 100% of them. That validates the protocol and the
   hashing pipeline against an independent implementation, but a real pool may
   still differ in extranonce handling, version rolling or difficulty policy.
2. **The CUDA kernel has never been compiled or executed.** There are no
   benchmark numbers in this repository, and none may be added until they are
   measured on real hardware.
3. **Stratum `prevhash` word-swap** is derived and unit-tested but should be
   confirmed against a live pool's first `mining.notify` (compare the decoded
   value with the current chain tip).

---

## Working agreements

- Tests never open a network connection. Pool behaviour is tested with local
  sample messages.
- No third-party dependencies without a strong reason: the project must build
  on a bare container and a fresh Windows box.
- Warnings are treated as defects; the tree builds clean with `-Wall -Wextra
  -Wpedantic` and `/W4`.
- Never commit `build/`, `config/config.json`, credentials or binaries.
- When you cannot verify something, say so in the code comment and in the
  status table above rather than implying it works.
