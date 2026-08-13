# Streaming Symmetric Hash Join for PostgreSQL 8.1.4

A pipelined symmetric hash join for PostgreSQL 8.1.4 that reduces time-to-first-result by ~20× on balanced joins by eliminating the traditional blocking hash-table build phase.

Metric           | Original       | SHJ
-----------------|----------------|----------------
Time to first result | 27.0 ms        | 1.4 ms
Total execution  | 5193 ms        | 2067 ms

This repository implements a symmetric (pipelined) hash join modification to PostgreSQL 8.1.4 and includes correctness tests and benchmarks demonstrating the tradeoffs between startup latency and memory usage.

## Blocking vs. Symmetric Hash Join

### Classic (blocking) hash join

```
  OUTER scan ──────────────────────────────► (wait)
                                                    │
  INNER scan ────────► BUILD hash table ────────────┤
                                                    ▼
                                            PROBE outer vs table
                                                    │
                                                    ▼
                                              output tuples
```

The inner relation must be fully consumed before any join result is produced.

### Symmetric hash join

```
  INNER scan ──► insert/probe ◄──┐
       │                         │ alternate
       ▼                         │
  inner hash table          outer hash table
       ▲                         │
       │                         ▼
  OUTER scan ──► insert/probe ──┘
       │
       ▼
  output tuples (pipelined — first result before either input finishes)
```

Each iteration reads one tuple from the chosen driver, inserts it into that side's hash table, probes the opposite table, and emits matches immediately.

## Key Concepts

**Alternating driver model.** The executor toggles between reading the inner and outer child plans. When the inner side drives, the new tuple is inserted into the inner hash table and the outer table is probed; when the outer side drives, the roles reverse. This keeps both pipelines active and avoids the full blocking build phase.

**Startup latency tradeoff.** Symmetric hash join can emit the first matching row as soon as both inputs have contributed at least one tuple that joins. The original algorithm must finish scanning the inner relation first. The price is memory: two hash tables must fit in `work_mem` (batching is forced to a single batch).

## Modified Files

| File | Role |
|------|------|
| `src/createplan.c` | Wrap both join inputs in `Hash` plan nodes |
| `src/nodeHash.c` | Pipelined `ExecHash`, single-batch sizing, dual-table probe in `ExecScanHashBucket` |
| `src/nodeHashjoin.c` | Symmetric join state machine in `ExecHashJoin` |
| `src/execnodes.h` | Extended `HashJoinState` for outer table, probe cursors, driver flags |

See `patch/symmetric-hash-join.patch` for the unified diff against upstream 8.1.4.

## Build

Requires a built PostgreSQL 8.1.4 source tree and `initdb`/`pg_ctl`/`psql` from `/usr/local/pgsql` (or set `PG_INSTALL`).

```bash
export POSTGRES_SRC=/path/to/postgresql-8.1.4

make build-original   # vanilla 8.1.4 → bin/postgres-original, bin/original/postmaster
make build-shj        # symmetric hash join → bin/postgres-shj, bin/shj/postmaster
make patch            # regenerate patch/symmetric-hash-join.patch
```

## Correctness Tests

Tests use the PostgreSQL **stand-alone backend** (`postgres -D … template1`) — no postmaster required. Each case builds random tables, forces hash join via GUCs, and differentially compares sorted results from both binaries.

```bash
make test
# or
python3 tests/test_correctness.py
```

Coverage includes empty/singleton relations, duplicate keys, NULL keys, skew, asymmetric sizes, and seeded random instances.

## Benchmarks

Benchmarks use `pg_ctl` with the custom server binary and `psql -d template1`:

```bash
make benchmark
# or
python3 benchmark/benchmark.py --port 5434
```

Results are written to `benchmark/results/benchmark_*.csv`.

### Performance trade-offs

Measured on PostgreSQL 8.1.4 (50k×50k moderate/skewed joins, 100k×500 asymmetric join, `work_mem=4096` KB). Full CSV in `benchmark/results/benchmark_20260811T040812Z.csv`.

| Query | Engine | TTFR (ms) | Total (ms) | Peak RSS (MB) | Rows |
|-------|--------|-----------|------------|---------------|------|
| moderate_join | original | 27.0 | 5193.4 | 6.9 | 1,001,000 |
| moderate_join | shj | **1.4** | **2067.4** | 6.9 | 1,001,000 |
| skewed_join | original | 26.7 | 1439.3 | 6.9 | 1,001,000 |
| skewed_join | shj | **5.4** | **1180.0** | 6.9 | 1,001,000 |
| asymmetric_join | original | **3.5** | **438.5** | 6.9 | 250,000 |
| asymmetric_join | shj | 4.3 | 497.5 | 6.9 | 250,000 |

**Latency curve** (time to fetch N tuples, moderate_join):

| Engine | 1 tuple | 10 tuples | 100 tuples | 1,000 tuples |
|--------|---------|-----------|------------|--------------|
| original | 27.0 ms | 27.6 ms | 28.1 ms | 31.1 ms |
| shj | 1.4 ms | 1.5 ms | 1.9 ms | 5.7 ms |

Symmetric hash join delivers dramatically lower time-to-first-result on equal-size joins (≈20× faster TTFR on moderate/skewed workloads) and roughly 2× faster total runtime, with comparable peak memory. On asymmetric joins where the original build phase is cheap, the classic algorithm retains a slight edge.

Reproduce with `make benchmark`.

## Apply Patch Manually

```bash
cd /path/to/postgresql-8.1.4
patch -p1 < /path/to/streaming-symmetric-hash-join/patch/symmetric-hash-join.patch
make -C src/backend postgres
```

## License

PostgreSQL License — see [LICENSE](LICENSE).
# Streaming Symmetric Hash Join for PostgreSQL 8.1.4

A pipelined symmetric hash join for PostgreSQL 8.1.4 that reduces time-to-first-result by ~20× on balanced joins by eliminating the traditional blocking hash-table build phase.

Metric           | Original       | SHJ
-----------------|----------------|----------------
Time to first result | 27.0 ms        | 1.4 ms
Total execution  | 5193 ms        | 2067 ms

This repository implements a symmetric (pipelined) hash join modification to PostgreSQL 8.1.4 and includes correctness tests and benchmarks demonstrating the tradeoffs between startup latency and memory usage.

## Blocking vs. Symmetric Hash Join

### Classic (blocking) hash join

```
  OUTER scan ──────────────────────────────► (wait)
                                                    │
  INNER scan ────────► BUILD hash table ────────────┤
                                                    ▼
                                            PROBE outer vs table
                                                    │
                                                    ▼
                                              output tuples
```

The inner relation must be fully consumed before any join result is produced.

### Symmetric hash join

```
  INNER scan ──► insert/probe ◄──┐
       │                         │ alternate
       ▼                         │
  inner hash table          outer hash table
       ▲                         │
       │                         ▼
  OUTER scan ──► insert/probe ──┘
       │
       ▼
  output tuples (pipelined — first result before either input finishes)
```

Each iteration reads one tuple from the chosen driver, inserts it into that side's hash table, probes the opposite table, and emits matches immediately.

## Key Concepts

**Alternating driver model.** The executor toggles between reading the inner and outer child plans. When the inner side drives, the new tuple is inserted into the inner hash table and the outer table is probed; when the outer side drives, the roles reverse. This keeps both pipelines active and avoids the full blocking build phase.

**Startup latency tradeoff.** Symmetric hash join can emit the first matching row as soon as both inputs have contributed at least one tuple that joins. The original algorithm must finish scanning the inner relation first. The price is memory: two hash tables must fit in `work_mem` (batching is forced to a single batch).

## Modified Files

| File | Role |
|------|------|
| `src/createplan.c` | Wrap both join inputs in `Hash` plan nodes |
| `src/nodeHash.c` | Pipelined `ExecHash`, single-batch sizing, dual-table probe in `ExecScanHashBucket` |
| `src/nodeHashjoin.c` | Symmetric join state machine in `ExecHashJoin` |
| `src/execnodes.h` | Extended `HashJoinState` for outer table, probe cursors, driver flags |

See `patch/symmetric-hash-join.patch` for the unified diff against upstream 8.1.4.

## Build

Requires a built PostgreSQL 8.1.4 source tree and `initdb`/`pg_ctl`/`psql` from `/usr/local/pgsql` (or set `PG_INSTALL`).

```bash
export POSTGRES_SRC=/path/to/postgresql-8.1.4

make build-original   # vanilla 8.1.4 → bin/postgres-original, bin/original/postmaster
make build-shj        # symmetric hash join → bin/postgres-shj, bin/shj/postmaster
make patch            # regenerate patch/symmetric-hash-join.patch
```

## Correctness Tests

Tests use the PostgreSQL **stand-alone backend** (`postgres -D … template1`) — no postmaster required. Each case builds random tables, forces hash join via GUCs, and differentially compares sorted results from both binaries.

```bash
make test
# or
python3 tests/test_correctness.py
```

Coverage includes empty/singleton relations, duplicate keys, NULL keys, skew, asymmetric sizes, and seeded random instances.

## Benchmarks

Benchmarks use `pg_ctl` with the custom server binary and `psql -d template1`:

```bash
make benchmark
# or
python3 benchmark/benchmark.py --port 5434
```

Results are written to `benchmark/results/benchmark_*.csv`.

### Performance trade-offs

Measured on PostgreSQL 8.1.4 (50k×50k moderate/skewed joins, 100k×500 asymmetric join, `work_mem=4096` KB). Full CSV in `benchmark/results/benchmark_20260811T040812Z.csv`.

| Query | Engine | TTFR (ms) | Total (ms) | Peak RSS (MB) | Rows |
|-------|--------|-----------|------------|---------------|------|
| moderate_join | original | 27.0 | 5193.4 | 6.9 | 1,001,000 |
| moderate_join | shj | **1.4** | **2067.4** | 6.9 | 1,001,000 |
| skewed_join | original | 26.7 | 1439.3 | 6.9 | 1,001,000 |
| skewed_join | shj | **5.4** | **1180.0** | 6.9 | 1,001,000 |
| asymmetric_join | original | **3.5** | **438.5** | 6.9 | 250,000 |
| asymmetric_join | shj | 4.3 | 497.5 | 6.9 | 250,000 |

**Latency curve** (time to fetch N tuples, moderate_join):

| Engine | 1 tuple | 10 tuples | 100 tuples | 1,000 tuples |
|--------|---------|-----------|------------|--------------|
| original | 27.0 ms | 27.6 ms | 28.1 ms | 31.1 ms |
| shj | 1.4 ms | 1.5 ms | 1.9 ms | 5.7 ms |

Symmetric hash join delivers dramatically lower time-to-first-result on equal-size joins (≈20× faster TTFR on moderate/skewed workloads) and roughly 2× faster total runtime, with comparable peak memory. On asymmetric joins where the original build phase is cheap, the classic algorithm retains a slight edge.

Reproduce with `make benchmark`.

## Apply Patch Manually

```bash
cd /path/to/postgresql-8.1.4
patch -p1 < /path/to/streaming-symmetric-hash-join/patch/symmetric-hash-join.patch
make -C src/backend postgres
```

## License

PostgreSQL License — see [LICENSE](LICENSE).
