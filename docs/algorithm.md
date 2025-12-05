# Shortcut Router Algorithm Guide

## Overview

The C++ query engine answers shortest-path queries over Contraction Hierarchies (CH) that were
precomputed in the Spark pipeline. Runtime performance comes from two ingredients:

1. **Compressed adjacency**: every shortcut row encodes a high-level edge in the contracted graph.
2. **Hierarchy-aware bidirectional Dijkstra**: forward search moves strictly upward in the hierarchy,
   backward search moves strictly downward, and the two meet in the middle.

The binary loads shortcuts from Apache Parquet, enriches them with optional road-edge metadata from
CSV, and exposes a CLI for individual or random queries.

## Shortcut Parquet Schema

Each record represents a directed shortcut between two contracted edges. Required fields:

| Column          | Type      | Description                                                         |
|-----------------|-----------|---------------------------------------------------------------------|
| `incoming_edge` | `uint32`  | Tail edge identifier (source side of the shortcut).                 |
| `outgoing_edge` | `uint32`  | Head edge identifier (target side of the shortcut).                 |
| `cost`          | `float64` | Total traversal cost along the shortcut (time, distance, etc.).     |
| `inside`        | `int8`    | Hierarchy direction flag: `+1` upward, `0` lateral, `-1` downward.  |

Optional fields:

| Column       | Type     | Description                                                                   |
|--------------|----------|-------------------------------------------------------------------------------|
| `via_edge`   | `uint32` | Intermediate edge for path expansion (0 if direct edge).                      |
| `cell`       | `uint64` | H3 ancestor cell bounding the shortcut.                                      |

### Via Edge Semantics

The `via_edge` field enables recursive shortcut expansion. For a shortcut `(u, v)`:
- If `via_edge == 0` or `via_edge == v`: This is a base edge (no intermediate).
- Otherwise: The path from `u` to `v` goes through `via_edge`, meaning the shortcut can be
  expanded as `(u → via_edge) + (via_edge → v)`. Each of these may themselves be shortcuts
  requiring further expansion.

## Edge Metadata CSV Schema

When present, metadata enforces the H3 hierarchy constraints used during routing. Required columns:

| Column            | Type     | Description                                                    |
|-------------------|----------|----------------------------------------------------------------|
| `id`              | `uint32` | Edge identifier (joins against `incoming_edge` / `outgoing_edge`). |
| `incoming_cell`   | `uint64` | H3 cell identifier attached to the edge.                        |
| `lca_res`         | `int32`  | Resolution of the precomputed lowest common ancestor cell.     |

Optional columns:

| Column   | Type      | Description                                                       |
|----------|-----------|-------------------------------------------------------------------|
| `length` | `float64` | Edge length/cost used to compute total path distance.             |

Additional columns (geometry, cost, classifications) are ignored by the CLI but may be retained for
validation or downstream processing.

Rows are parsed with quoted-field support, so geometry stored as `"LINESTRING(...)"` is handled
correctly.

## Bidirectional Search

1. **Preprocessing**: the loader builds forward and backward adjacency lists keyed by edge ID. Each
   shortcut is stored once and referenced by index from both lists. A lookup table mapping
   `(from, to)` pairs to shortcut indices is also built for path expansion.
2. **High cell computation** (optional): when edge metadata is available, the code derives the
   highest common H3 cell for the source/target pair. This restricts exploration to shortcuts whose
   parent cell matches the computed high cell and resolution.
3. **Forward search**: expands edges with `inside == +1`, accumulating distances inside a min-heap.
4. **Backward search**: runs in parallel using edges with `inside == -1`. Once the traversal is in the
   highest common cell, lateral shortcuts tagged with `inside == 0` are also permitted so long as
   they stay within that cell.
5. **Meeting point**: whenever a node explored from one side appears in the other frontier, the code
   updates the best-known cost. The algorithm terminates once both heaps exceed that bound.
6. **Path reconstruction**: parent pointers from the forward and backward phases are concatenated to
   produce the final shortcut edge sequence.
7. **Destination edge cost**: the length of the target edge (from metadata) is added to the total
   distance, ensuring the full traversal cost is reported.

## Path Expansion

The query result contains a **shortcut path** – a sequence of edge IDs that may include contracted
shortcuts. To recover the actual road-network edges:

1. For each consecutive pair `(u, v)` in the shortcut path, look up the shortcut record.
2. If `via_edge` is non-zero and differs from `u`, recursively expand `(u, via_edge)` and
   `(via_edge, v)`.
3. Merge the expanded sequences, removing duplicates at junctions.

The `expand_shortcut_path()` method implements this recursive unpacking. Example output:

```
Shortcut path: 1593 -> 34486 -> 24508 -> 8504 -> 4835
Expanded base edge path: 1593 -> 34486 -> 24508 -> 16528 -> 8504 -> 4835
```

## Configuration File

A simple INI-style text file can provide default inputs. Recognized keys:

| Key        | Meaning                                  | Example value                                            |
|------------|------------------------------------------|----------------------------------------------------------|
| `shortcuts`| Parquet file or directory                | `/data/shortcuts/Burnaby_shortcuts_final`                |
| `edges`    | Edge metadata CSV                        | `/data/edges/Burnaby_driving_simplified_edges_with_h3.csv` |
| `random`   | Number of random queries to run          | `50`                                                     |
| `seed`     | RNG seed for random query sampling       | `2024`                                                   |

Command-line arguments take precedence over values in the config file. Store the config under
`config/` or any convenient location and pass it with `--config /path/to/file`.

## API Reference

### QueryResult

```cpp
struct QueryResult {
    double distance;           // Total cost including destination edge length
    std::vector<uint32_t> path; // Shortcut-level edge sequence
    bool reachable;            // True if a path was found
};
```

### ExpandedResult

```cpp
struct ExpandedResult {
    std::vector<uint32_t> base_edges; // Fully expanded edge sequence
    bool success;                      // True if expansion succeeded
};
```

### Key Methods

| Method | Description |
|--------|-------------|
| `query(source, target)` | Run bidirectional search and return `QueryResult` |
| `expand_shortcut_path(path)` | Expand shortcut path to base edges, return `ExpandedResult` |
| `get_edge_length(edge_id)` | Get length of a single edge from metadata |

## Typical Workflow

1. **Install prerequisites** – follow the instructions in the top-level `readme.md` to activate the
   `ch-query-engine` Conda environment, install Arrow/Parquet, and build libh3.
2. **Build the binary** – run `./build_cpp.sh` (optionally appending extra CMake flags).
3. **Run queries** – use `./run_cpp.sh` (or the compiled `build/shortcut_router`) with explicit
   arguments or a configuration file.

Example query:

```bash
./build/shortcut_router --config config/shortcut_router.cfg --source 1593 --target 4835
```

Output:

```
Query 1593 -> 4835
  Distance (including destination edge): 433.75
  Destination edge cost: 267.604
  Shortcut path length: 5 edges
  Shortcut path: 1593 -> 34486 -> 24508 -> 8504 -> 4835
  Expanded base edge path length: 6 edges
  Expanded path: 1593 -> 34486 -> 24508 -> 16528 -> 8504 -> 4835
  Runtime: 3.03 ms
```

Refer to the README for detailed build/run commands, environment setup, and usage examples.
