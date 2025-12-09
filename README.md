# Dijkstra on Hierarchy

The core C++20 library implementing the Contraction Hierarchies (CH) routing algorithm with H3 geospatial constraints.

## 🧠 Core Features

-   **H3 Integration**: Uses H3 (v4) for hierarchical spatial partitioning and pruning.
-   **Contraction Hierarchies**: Bidirectional search on a pre-processed shortcut graph.
-   **Performance**: Sub-millisecond queries using optimized adjacency lists and priority queues.
-   **Parquet/Arrow**: Zero-copy loading of large graph datasets using Apache Arrow.
-   **Flexibility**: Supports both "One-to-One" (Point-to-Point) and "Many-to-Many" (Multi-Target) Dijkstra searches.

## 🛠️ Key Components

### `ShortcutGraph`
The main graph data structure.
-   **Adjacency Lists**: Separated into forward (`inside=+1`) and backward (`inside=-1`) directions.
-   **Metadata**: Stores `EdgeMeta` (length, cost, `incoming_cell`, `outgoing_cell`, `lca_res`) for full context.
-   **Edge Expansion**: Capable of unpacking shortcuts into their constituent base edges.

### `H3Utils`
Helper utilities for H3 operations.
-   `h3_lat_lng_to_cell`: Geopoint to Cell ID.
-   `h3_cell_boundary`: Cell ID to Polygon Boundary (GeoJSON ready).
-   `h3_find_ancestor`: Hierarchy traversal.

## 🏗️ Build

```bash
mkdir build && cd build
cmake ..
make -j
```

**Requirements:**
-   C++20 compliant compiler (GCC 10+, Clang 11+)
-   Apache Arrow & Parquet
-   H3 Library (v4)
-   Google Test (for tests)
