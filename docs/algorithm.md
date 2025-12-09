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

## One-to-One Shortest Path Algorithm

The `query()` method implements a bidirectional Dijkstra search with hierarchical filtering for finding the shortest path between a single source and target edge. This is the core routing algorithm used for point-to-point queries.

### Algorithm Overview

```
function query(source_edge, target_edge):
    // Compute hierarchical context
    high_cell = compute_high_cell(source_edge, target_edge)
    
    // Initialize data structures
    dist_fwd[all edges] = infinity
    dist_bwd[all edges] = infinity
    parent_fwd[all edges] = -1
    parent_bwd[all edges] = -1
    pq_fwd = empty min-heap
    pq_bwd = empty min-heap
    best = infinity
    meeting_edge = null
    
    // Initialize forward search from source
    dist_fwd[source_edge] = 0.0
    parent_fwd[source_edge] = source_edge
    pq_fwd.push((0.0, source_edge))
    
    // Initialize backward search from target
    target_cost = get_edge_cost(target_edge)
    dist_bwd[target_edge] = target_cost
    parent_bwd[target_edge] = target_edge
    pq_bwd.push((target_cost, target_edge))
    
    // Bidirectional search
    while not pq_fwd.empty() and not pq_bwd.empty():
        // Alternate between forward and backward search
        
        // FORWARD STEP
        if not pq_fwd.empty():
            (curr_dist, curr_edge) = pq_fwd.pop()
            
            if curr_dist >= best:
                goto termination_check
            
            // Expand forward using upward shortcuts (inside == 1)
            for each shortcut from curr_edge where shortcut.inside == 1:
                // Apply hierarchical filtering
                if not parent_check(shortcut, high_cell):
                    continue
                
                next_edge = shortcut.to
                candidate = curr_dist + shortcut.cost
                
                if candidate < dist_fwd[next_edge]:
                    dist_fwd[next_edge] = candidate
                    parent_fwd[next_edge] = curr_edge
                    pq_fwd.push((candidate, next_edge))
                    
                    // Check if backward search reached this edge
                    if dist_bwd[next_edge] < infinity:
                        total = candidate + dist_bwd[next_edge]
                        if total < best:
                            best = total
                            meeting_edge = next_edge
        
        // BACKWARD STEP
        if not pq_bwd.empty():
            (curr_dist, curr_edge) = pq_bwd.pop()
            
            if curr_dist >= best:
                goto termination_check
            
            // Expand backward using downward shortcuts (inside == -1)
            // Lateral shortcuts (inside == 0) allowed only within high_cell
            for each shortcut to curr_edge where shortcut.inside in {-1, 0}:
                // Apply hierarchical filtering
                if not parent_check(shortcut, high_cell):
                    continue
                
                prev_edge = shortcut.from
                candidate = curr_dist + shortcut.cost
                
                if candidate < dist_bwd[prev_edge]:
                    dist_bwd[prev_edge] = candidate
                    parent_bwd[prev_edge] = curr_edge
                    pq_bwd.push((candidate, prev_edge))
                    
                    // Check if forward search reached this edge
                    if dist_fwd[prev_edge] < infinity:
                        total = dist_fwd[prev_edge] + candidate
                        if total < best:
                            best = total
                            meeting_edge = prev_edge
        
        termination_check:
            // Early termination check
            if not pq_fwd.empty() and not pq_bwd.empty():
                if pq_fwd.top().distance + pq_bwd.top().distance >= best:
                    break  // No better path can be found
    
    // Reconstruct path from meeting edge
    path = []
    
    // Backward from meeting to source
    curr = meeting_edge
    while parent_fwd[curr] != curr:
        path.prepend(curr)
        curr = parent_fwd[curr]
    path.prepend(curr)  // Add source edge
    
    // Forward from meeting to target
    curr = meeting_edge
    while parent_bwd[curr] != curr:
        curr = parent_bwd[curr]
        path.append(curr)
    
    return (best, path)
```

### Hierarchical Filtering

The `parent_check()` function ensures shortcuts respect the H3 hierarchy:

```
function parent_check(shortcut, high_cell):
    // Shortcuts must be within or crossing into the high_cell
    if shortcut.cell == high_cell.id and shortcut.cell_res == high_cell.res:
        return true  // Shortcut is within the high cell
    
    // Check if shortcut's parent cell matches high_cell
    shortcut_parent = h3_get_parent(shortcut.cell, high_cell.res)
    if shortcut_parent == high_cell.id:
        return true  // Shortcut crosses into high cell
    
    return false  // Shortcut is outside the search scope
```

### Key Features

1. **Hierarchical Context**: Computes the highest common H3 cell for source and target to restrict the search space

2. **Directional Filtering**:
   - Forward search: only upward shortcuts (`inside == 1`)
   - Backward search: downward (`inside == -1`) and lateral (`inside == 0`) shortcuts

3. **Lateral Edge Restriction**: Lateral shortcuts are only allowed within the `high_cell` to prevent exploring irrelevant regions

4. **Early Termination**: When `pq_fwd.top() + pq_bwd.top() >= best`, no better path exists

5. **Target Edge Cost**: Included in backward search initialization to account for traversing the target edge

## Many-to-Many Shortest Path Algorithm

The `query_multi_optimized()` method implements a bidirectional search that can handle multiple source and target edges simultaneously. This is used for KNN (k-nearest neighbors) routing where we want to find the shortest path among k candidate sources and k candidate targets.

### Algorithm Overview

```
function query_multi_optimized(source_edges[], target_edges[], source_distances[], target_distances[]):
    // Initialize data structures
    dist_fwd[all edges] = infinity
    dist_bwd[all edges] = infinity
    parent_fwd[all edges] = -1
    parent_bwd[all edges] = -1
    pq_fwd = empty min-heap
    pq_bwd = empty min-heap
    best = infinity
    meeting_edge = null
    
    // Initialize forward search from ALL source edges
    for each (edge, dist) in (source_edges, source_distances):
        dist_fwd[edge] = dist  // Approach distance from query point
        parent_fwd[edge] = edge
        pq_fwd.push((dist, edge))
    
    // Initialize backward search from ALL target edges
    for each (edge, dist) in (target_edges, target_distances):
        edge_cost = get_edge_cost(edge)  // Travel time of the target edge
        init_dist = edge_cost + dist  // Edge cost + egress distance
        dist_bwd[edge] = init_dist
        parent_bwd[edge] = edge
        pq_bwd.push((init_dist, edge))
    
    // Bidirectional search
    while not pq_fwd.empty() and not pq_bwd.empty():
        // Alternate between forward and backward search
        
        // FORWARD STEP
        if not pq_fwd.empty():
            (curr_dist, curr_edge) = pq_fwd.pop()
            
            if curr_dist >= best:
                goto termination_check
            
            // Expand forward using upward shortcuts (inside == 1)
            for each shortcut from curr_edge where shortcut.inside == 1:
                next_edge = shortcut.to
                candidate = curr_dist + shortcut.cost
                
                if candidate < dist_fwd[next_edge]:
                    dist_fwd[next_edge] = candidate
                    parent_fwd[next_edge] = curr_edge
                    pq_fwd.push((candidate, next_edge))
                    
                    // Check if backward search reached this edge
                    if dist_bwd[next_edge] < infinity:
                        total = candidate + dist_bwd[next_edge]
                        if total < best:
                            best = total
                            meeting_edge = next_edge
        
        // BACKWARD STEP
        if not pq_bwd.empty():
            (curr_dist, curr_edge) = pq_bwd.pop()
            
            if curr_dist >= best:
                goto termination_check
            
            // Expand backward using downward and lateral shortcuts (inside == -1 or 0)
            for each shortcut to curr_edge where shortcut.inside in {-1, 0}:
                prev_edge = shortcut.from
                candidate = curr_dist + shortcut.cost
                
                if candidate < dist_bwd[prev_edge]:
                    dist_bwd[prev_edge] = candidate
                    parent_bwd[prev_edge] = curr_edge
                    pq_bwd.push((candidate, prev_edge))
                    
                    // Check if forward search reached this edge
                    if dist_fwd[prev_edge] < infinity:
                        total = dist_fwd[prev_edge] + candidate
                        if total < best:
                            best = total
                            meeting_edge = prev_edge
        
        termination_check:
            // Early termination check
            // NOTE: Only works correctly for single source and single target!
            // With multiple targets, pq_bwd.top() might be from a different target,
            // causing premature termination. So we disable it for multi-target queries.
            single_source_target = (source_edges.size == 1 and target_edges.size == 1)
            
            if single_source_target and not pq_fwd.empty() and not pq_bwd.empty():
                if pq_fwd.top().distance + pq_bwd.top().distance >= best:
                    break  // No better path can be found
    
    // Reconstruct path from meeting edge
    path = []
    
    // Backward from meeting to source
    curr = meeting_edge
    while parent_fwd[curr] != curr:
        path.prepend(curr)
        curr = parent_fwd[curr]
    path.prepend(curr)  // Add source edge
    
    // Forward from meeting to target
    curr = meeting_edge
    while parent_bwd[curr] != curr:
        curr = parent_bwd[curr]
        path.append(curr)
    
    return (best, path)
```

### Key Differences from Single Source/Target

1. **Multiple Initialization**: Both forward and backward searches start from multiple edges simultaneously, simulating a "dummy" source/target node connected to all candidates.

2. **Distance Initialization**:
   - Source edges: initialized with approach distance (cost to reach from query point)
   - Target edges: initialized with edge cost + egress distance (includes traversal cost)

3. **Termination Condition**: The standard early termination check (`pq_fwd.top() + pq_bwd.top() >= best`) is **disabled for multiple targets** because it can cause premature termination. With multiple targets, `pq_bwd.top()` might be from a different target than the optimal one, leading to incorrect early stopping.

4. **Path Selection**: The algorithm automatically selects the source and target that yield the shortest total path among all combinations.

### Performance Characteristics

- **Single source/target**: Uses early termination for optimal performance
- **Multiple sources/targets**: Explores full search space for correctness (slightly slower but correct)
- **Time Complexity**: O((|S| + |T| + |E|) log |V|) where S = sources, T = targets, E = edges explored
- **Space Complexity**: O(|V|) for distance and parent arrays
