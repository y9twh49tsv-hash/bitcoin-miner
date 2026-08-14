# AZD BITCOIN MINER

A real Bitcoin SHA-256d miner foundation in C++20, with a portable CPU core, an
optional CUDA GPU backend, a Stratum V1 client, a localhost API and a dark
dashboard.

> **On profitability:** GPU Bitcoin mining is technically possible, but
> dedicated SHA-256 ASIC miners are substantially more efficient. This project
> makes no profitability claims.

> **On honesty:** this miner never fabricates data. Hashrate, shares,
> temperature, power, utilization, blocks and pool status are only ever reported
> from things that actually happened. Anything that cannot be measured — GPU
> telemetry without CUDA, a best-share difficulty before any share exists — is
> reported as unavailable (`null` / `N/A`), never as a placeholder number.

---

## Status

| Phase | Component | State |
| --- | --- | --- |
| 1 | SHA-256 / SHA-256d | ✅ Complete — FIPS 180-4 vectors pass |
| 2 | 80-byte block header + **genesis block test** | ✅ **Genesis hash reproduces exactly** |
| 3 | Compact nBits, 256-bit targets, share difficulty | ✅ Complete — exact integer arithmetic |
| 4 | Merkle root (branch + full tree) | ✅ Complete — verified against block 170 |
| 5 | Stratum V1 architecture | ✅ Implemented — verified against a local mock pool, ⚠️ not yet run against a real pool |
| 6 | Mining work model | ✅ Complete |
| 7 | CPU reference nonce search | ✅ Complete — recovers the real genesis nonce |
| 8 | CUDA interface + kernel | ⚠️ Written, **never compiled or run on a GPU** |
| 9 | Thread-safe statistics | ✅ Complete |
| 10 | Localhost API | ✅ Complete for the listed endpoints |
| 11 | Dashboard | ✅ Complete |
| 12 | Configuration | ✅ Complete |
| 13–14 | Build scripts | ✅ Complete |

**Test suite: 9 suites, all passing without CUDA.**

---

## Architecture

```
                    ┌──────────────┐   HTTP    ┌───────────────┐
                    │  dashboard/  │◄─────────►│  api::Server  │  127.0.0.1
                    │ HTML/CSS/JS  │           └───────┬───────┘
                    └──────────────┘                   │
                                                       ▼
    ┌─────────────┐   TCP/JSON   ┌────────────────────────────────┐
    │ Stratum V1  │◄────────────►│    mining::MinerController     │
    │    pool     │              │  jobs · workers · statistics   │
    └─────────────┘              └───────┬────────────────┬───────┘
                                         │                │
                              ┌──────────▼──────┐  ┌──────▼────────────┐
                              │ CpuReferenceMiner│ │ CudaMiningBackend │
                              │  (the oracle)    │ │  (optional, .cu)  │
                              └──────────┬───────┘ └──────┬────────────┘
                                         │                │
                                    ┌────▼────────────────▼────┐
                                    │  bitcoin:: primitives    │
                                    │ sha256d · header · target│
                                    │ merkle  · UInt256        │
                                    └──────────────────────────┘
```

```
src/
├── main.cpp              CLI entry point
├── util/                 hex, JSON parser, logging  (no third-party deps)
├── core/                 configuration
├── bitcoin/              sha256, hash, uint256, block_header, difficulty, merkle
├── mining/               work, backend interface, miner (CPU + controller), stats
├── stratum/              message framing/parsing, TCP client
├── cuda/                 cuda_backend.hpp/.cu  +  cuda_backend_stub.cpp
└── api/                  HTTP server
```

The layout extends the requested structure with three additions:
`bitcoin/uint256` (exact 256-bit target arithmetic), `bitcoin/hash` (the
byte-order-explicit `Hash256` type), and `util/` + `core/` (JSON, hex, logging,
config) so that no third-party library is needed.

### Byte order

The most error-prone part of any miner, so it is explicit everywhere:

| Value | Stored as | Notes |
| --- | --- | --- |
| `Hash256` | internal order | exactly what SHA256d emits and what the header contains |
| Display hash | byte-reversed | `toDisplayHex()` / `fromDisplayHex()` only |
| Merkle branches | internal order | concatenated directly, no reversal |
| Stratum `prevhash` | word-swapped | bytes reversed **within** each 4-byte word |
| `version` / `nbits` / `ntime` | host `uint32` | parsed from big-endian hex, serialized little-endian |

The hash-versus-target comparison is exact 256-bit integer arithmetic. Floating
point is used only for display, in functions named `...Approximate()`.

---

## Build — cloud / Linux (no CUDA)

Requires only a C++20 compiler and CMake 3.20+.

```bash
cmake -S . -B build -DAZD_ENABLE_CUDA=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Or:

```bash
./scripts/build-cloud.sh          # configure + build + test + self-test
./scripts/build-cloud.sh --clean  # from scratch
```

`AZD_ENABLE_CUDA` defaults to `OFF`, so a missing `nvcc` never breaks this
build. The CUDA source file is not even added to the build in that case.

## Build — Windows 11

```powershell
.\scripts\build-windows.ps1              # CPU only, Visual Studio C++
.\scripts\build-windows.ps1 -Cuda        # + GPU backend (needs the CUDA Toolkit)
.\scripts\build-windows.ps1 -Cuda -Nvml  # + temperature / power / utilization
```

Manually:

```powershell
cmake -S . -B build -DAZD_ENABLE_CUDA=OFF
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

## Enabling CUDA later (RTX 5070)

1. Install the **NVIDIA CUDA Toolkit** (and a driver supporting your card).
2. Configure with CUDA on, targeting Blackwell:

   ```powershell
   cmake -S . -B build -DAZD_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=120
   cmake --build build --config Release
   ```

   `120` is `sm_120`, the RTX 50 series (Blackwell) architecture. The default
   in `CMakeLists.txt` builds `86;89;120` so one binary covers RTX 30/40/50.
3. Verify the device is really seen:

   ```powershell
   .\build\bin\Release\azd-miner.exe --devices
   ```

   Without CUDA this prints `unavailable -- CUDA backend not compiled`. It never
   claims a GPU that is not there.
4. Add `-DAZD_ENABLE_NVML=ON` for temperature, power and utilization. Without
   NVML those fields stay `N/A` — they are never estimated.

**Before trusting the GPU:** run `--selftest` and confirm the CUDA path agrees
with the CPU reference. Every GPU-found nonce is re-verified on the CPU at
runtime, and a mismatch is reported as a backend bug rather than submitted.

---

## Tests

```bash
ctest --test-dir build --output-on-failure
```

| Suite | Covers |
| --- | --- |
| `test_sha256` | FIPS 180-4 vectors, streaming/one-shot equivalence, SHA-256d |
| `test_block_header` | 80-byte layout, endianness, **genesis block hash**, block 1 |
| `test_difficulty` | nBits decode/encode, 256-bit compare, share difficulty |
| `test_merkle` | branch folding, full tree, odd-node duplication, block 170 |
| `test_stratum` | line framing incl. partial TCP frames, all message types |
| `test_work` | coinbase assembly, extranonce2, header build, CPU nonce search |
| `test_json` | parser correctness and rejection of malformed input |
| `test_stats` | counters start at zero and only move on real activity |
| `test_api` | routing, config, **password never in any GET response** |

Tests never open a network connection — Stratum is exercised with local sample
messages, so `ctest` never touches a real pool and no share is ever fabricated.

The built-in self-test performs real hashing:

```bash
./build/bin/azd-miner --selftest
```

It verifies the genesis hash and then actually searches for the genesis nonce
with the CPU reference miner, reporting the true hash count and elapsed time.

### End-to-end test against a local mock pool

`scripts/mock_pool.py` is a development-only Stratum V1 server that verifies
every submitted share independently with Python's `hashlib` — it accepts a
share only if it can reproduce the header from the submitted parameters and
confirm the hash really meets the target.

```bash
python3 scripts/mock_pool.py --port 3333 --difficulty 0.001   # terminal 1
./build/bin/azd-miner --config config/config.json --start     # terminal 2
```

This exercises the real network path: subscribe → authorize → set_difficulty →
notify → submit → accept, including job switching and partial TCP frames. It is
not part of `ctest`, because unit tests never open a socket.

---

## Configuration

```bash
cp config/config.example.json config/config.json
# edit config/config.json — it is gitignored
./build/bin/azd-miner --config config/config.json
```

```json
{
  "miner":     { "name": "AZD-PC-01", "device": 0, "cpuThreads": 1 },
  "pool":      { "host": "", "port": 3333, "username": "", "password": "x" },
  "gpu":       { "temperatureWarning": 75, "temperatureStop": 85 },
  "dashboard": { "host": "127.0.0.1", "port": 8080 }
}
```

`pool.username` is normally `yourwallet.workername`. **No wallet address exists
anywhere in this source tree** — you supply your own, and `config/config.json`
is gitignored so it is never committed.

### CLI

```
azd-miner [--config <path>] [--selftest] [--devices] [--start]
          [--no-dashboard] [--dashboard-root <dir>] [--port <n>] [--verbose]
```

### API

| Method | Endpoint | Purpose |
| --- | --- | --- |
| GET | `/api/status` | miner state, backend, full statistics |
| GET | `/api/gpu` | device info and telemetry (`null` when unavailable) |
| GET | `/api/pool` | connection state and share counts |
| GET | `/api/stats` | statistics only |
| GET | `/api/config` | configuration **without** the password |
| PUT | `/api/config` | update configuration (accepts a password; write-only) |
| POST | `/api/miner/start` | start mining (fails if no pool is configured) |
| POST | `/api/miner/stop` | stop mining |

---

## Dashboard

Open `http://127.0.0.1:8080` while the miner runs.

Dark, responsive, Bitcoin-orange, with cards for mining status, GPU,
hashrate, temperature, power, utilization, pool, accepted/rejected shares,
best difficulty and session time, plus START/STOP controls and pool settings.

Until the GPU backend exists, every GPU field displays **N/A** — not a fake
value. The password field is write-only: the API never returns it, so it is
never populated from a response.

---

## Security principles

- **No developer wallet, no developer fee.** Nothing is diverted anywhere.
- **The dashboard binds to `127.0.0.1`** and has no authentication. Binding
  elsewhere logs a loud warning; do not expose it to a network.
- **Passwords are write-only.** `GET` responses carry `passwordSet: true|false`
  and never the value. `redactSecret()` guards log lines. A test asserts that no
  GET response body contains the configured password.
- **Path traversal is rejected** when serving dashboard files.
- **Untrusted pool input.** Every Stratum message is length-checked and
  validated; malformed input is dropped, never mined. The line buffer refuses
  unbounded input.
- **Thermal protection stays on.** There is no override, and no BIOS,
  overvolting or power-limit manipulation anywhere in this project.
- **Credentials are never committed** — `config/config.json`, `.env` and
  friends are gitignored.

---

## Known validation gaps

Stated plainly rather than glossed over:

1. **No REAL pool session has been run.** The full flow was exercised against
   `scripts/mock_pool.py` (see below) and every submitted share was accepted,
   but a production pool may differ in extranonce handling, version rolling or
   difficulty policy.
2. **The CUDA kernel has never been compiled or executed.** No benchmark number
   for it appears anywhere in this repository, and none should be added until
   it is measured on real hardware.
3. **The Stratum `prevhash` word-swap** is derived from cgminer's data path and
   unit-tested, but should be confirmed against a live pool's first
   `mining.notify`.

---

## License / disclaimer

Provided as-is, with no warranty and no profitability claim. You are
responsible for your own hardware, electricity costs and pool credentials.
