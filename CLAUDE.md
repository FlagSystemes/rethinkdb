# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Configure:**
```bash
./configure --allow-fetch          # Fetch and build missing dependencies automatically
./configure CXX=clang++ --ccache   # Use clang++ with ccache
```

**Build:**
```bash
make -j$(nproc)                    # Build with all available cores
make DEBUG=1 -j$(nproc)           # Debug build (used in CI)
make support                       # Build dependencies only
make rethinkdb                     # Build server executable only
```

Build output lands in `build/debug/` or `build/release/`.

## Testing

**Unit tests** (C++/Google Test, in `src/unittest/`):
```bash
make unit
test/run -H unit --verbose
```

**Integration tests** (Python-based):
```bash
test/run --timeout 300 --jobs 4 -o /tmp/test_output -H all '!unit' '!cpplint' '!long' '!disabled'
```

**Long-running tests:**
```bash
test/run --timeout 900 --jobs 4 -H long '!disabled'
```

**Run a single test:**
```bash
test/run -H <test_name>
```

## Linting

```bash
./scripts/check_style.sh           # Run cpplint on entire codebase
./scripts/check_style.sh file.cc   # Check specific file
```

Style rules are in `src/CPPLINT.cfg`. Full coding conventions are documented in `STYLE.md`.

Key style rules:
- Include order: parent `.hpp` → C headers → C++ headers → Boost → local headers
- No non-const lvalue references (except return values)
- Use `DISABLE_COPYING` macro for non-copyable types
- Always use braces with `if`/`for`/`while`

## Architecture Overview

RethinkDB is a distributed NoSQL database with realtime changefeed support. The server is written in C++ with an event-driven, coroutine-based concurrency model.

**Entry point:** `src/main.cc` → `src/clustering/administration/main/command_line.cc` → `src/clustering/administration/main/serve.cc` (`do_serve()`)

### Concurrency Model

- Per-thread event loops with cooperative coroutines (no blocking on event threads)
- A separate blocker pool handles blocking I/O
- Inter-server communication uses mailboxes and a distributed directory (`src/clustering/generic/`)

### Query Execution Pipeline

1. Client connects via TCP; query received by `rdb_query_server_t`
2. ReQL query compiled by `ql::compile_term()` into a tree of `ql::term_t` objects (`src/rdb_protocol/terms/`)
3. Each term's `eval()` executes the operation
4. Table operations become `read_t` / `write_t` requests routed via `table_query_client_t`
5. Requests reach `primary_query_server_t` on the appropriate shard
6. Executed against `store_t` which operates on the B-tree storage layer

### Storage Stack

- **B-tree** (`src/btree/`): Core key-value storage structure
- **Buffer cache** (`src/buffer_cache/`): Page cache managing B-tree blocks (`page_cache_t`)
- **Serializer** (`src/serializer/`): Log-structured persistent storage (`log_serializer_t`)

### Clustering & Consensus

- Raft consensus used for **table metadata only** (not user queries)
- Custom Raft implementation: `raft_member_t` in `src/clustering/generic/raft_core.hpp`
- `contract_coordinator_t` (leader) generates contracts; `contract_executor_t` (all servers) executes them
- Table lifecycle managed by `multi_table_manager_t` / `table_manager_t` in `src/clustering/table_manager/`

### Changefeeds

Changefeeds use a push model: shards maintain `changefeed::server_t` (part of `store_t`) and push updates to `changefeed::client_t` mailboxes on query servers. See `src/rdb_protocol/changefeed.hpp`.

### Key Source Directories

| Directory | Purpose |
|-----------|---------|
| `src/arch/` | OS/threading abstraction (thread pool, event loop, file/network I/O) |
| `src/btree/` | B-tree storage engine |
| `src/buffer_cache/` | Page cache for B-tree blocks |
| `src/serializer/` | Log-structured persistent storage |
| `src/rdb_protocol/` | ReQL query language, protocol, core types (`datum_t`, `val_t`) |
| `src/rdb_protocol/terms/` | Implementations of all ReQL operations (~100+ term classes) |
| `src/clustering/` | All cluster management subsystems |
| `src/clustering/administration/` | Server startup, admin HTTP API, web UI |
| `src/clustering/table_contract/` | Table contract system (coordinator + executor) |
| `src/clustering/immediate_consistency/` | Consistency guarantees for query routing |
| `src/clustering/query_routing/` | Query distribution and shard routing |
| `src/clustering/generic/` | Raft, mailbox, distributed directory |
| `src/concurrency/` | Coroutine coordination primitives |
| `src/rpc/` | RPC/networking layer (mailbox implementation) |
| `src/extproc/` | Sandboxed external process integration |
| `src/http/` | HTTP server for the admin UI |
| `src/unittest/` | Unit test suite (98 test files) |
| `test/` | Integration/scenario/ReQL/performance tests |

### Key Types

- `ql::datum_t` — A ReQL value (string, number, array, object, null, etc.)
- `ql::val_t` — Any ReQL expression result (datum, table, sequence, etc.)
- `read_t` / `write_t` — Table operation requests sent through the cluster
- `read_response_t` / `write_response_t` — Corresponding responses

### Build System

GNU Make-based. Top-level `Makefile` delegates to `mk/main.mk`. Configuration is done by the `./configure` bash script, which writes `config.mk`. Dependencies can be auto-fetched with `--allow-fetch`; fetched sources land in `build/support_*/`.
