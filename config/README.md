# Configuration

`config.example.json` is the only file in this directory that is tracked by git.

To configure the miner:

```bash
cp config/config.example.json config/config.json
# edit config/config.json with your own pool and wallet
./build/bin/azd-miner --config config/config.json
```

`config/config.json` is listed in `.gitignore` and must never be committed —
it contains your wallet address and pool password.

## Fields

| Key | Meaning |
| --- | --- |
| `miner.name` | Free-form name for this rig, shown in the dashboard. |
| `miner.device` | CUDA device index (ignored without CUDA). |
| `miner.cpuThreads` | CPU reference-search threads. `0` connects without hashing. The CPU path is a correctness oracle, not a production miner. |
| `pool.host` | Stratum V1 hostname. Empty means "not configured" and the miner refuses to start. |
| `pool.port` | Stratum V1 port. |
| `pool.username` | Usually `wallet.workername`. **Your** address — nothing is baked into the software. |
| `pool.password` | Pool password (most pools accept `x`). Never returned by the API and never logged. |
| `gpu.temperatureWarning` | Warn above this temperature (°C). |
| `gpu.temperatureStop` | Stop mining above this temperature (°C). Never disable this. |
| `dashboard.host` | Bind address. Keep it `127.0.0.1`: the API has no authentication. |
| `dashboard.port` | Dashboard port. |

Anything you leave out keeps its default, so a partial config file is valid.
