# Contraction Hierarchies Query Stack

This project packages the full workflow we use for answering shortest-path queries with Contraction Hierarchies (CH). The stack is split into three layers that share the same shortcut table:

1. **Spark preprocessing** – builds the shortcut catalog from the raw graph, enriches it with H3 hierarchy metadata, and checkpoints the final Parquet dataset. See the `spark/` folder for notebooks and scripts.
2. **Python validation** – the Jupyter notebook in `spark/shortcut_edges.ipynb` reconstructs the CH adjacency, runs bidirectional Dijkstra with hierarchy-aware filtering, and records timing for ad-hoc queries.
3. **C++ query engine** – a zero-copy loader plus a highly tuned bidirectional search for production latency.

---

## Shortcut Layout

Each shortcut row produced by Spark contains:

| Column          | Type    | Meaning                                                 |
|-----------------|---------|---------------------------------------------------------|
| `incoming_edge` | uint32  | Tail edge id (source side of the shortcut)              |
| `outgoing_edge` | uint32  | Head edge id (target side of the shortcut)              |
| `cost`          | float64 | Travel cost accumulated along the shortcut              |
| `via_edge`      | uint32  | Edge to use when unpacking (optional, default 0)        |
| `cell`          | uint64  | H3 ancestor cell that bounds the shortcut (optional)    |
| `inside`        | int8    | Hierarchy direction flag (+1 up, 0 lateral, -1 down)    |

The Python and C++ layers keep the exact same interpretation:
- Forward search only expands shortcuts where `inside == 1` (pure upward).
- Backward search only expands shortcuts where `inside == -1` (pure downward).
- Meeting in the middle reconstructs the path by stitching the forward parent and backward successor chains.

---

## Python Prototype

The notebook builds adjacency dictionaries, runs the bidirectional Dijkstra helper, and prints:
- total distance (or `-1` if unreachable),
- the list of edge ids on the reconstructed path, and
- timing in milliseconds via `perf_counter`.

Feel free to swap the random source/target sampler with concrete queries or batch workloads when validating data quality.

---

## C++ Query Engine

The production implementation lives in `cpp/`:

- `include/shortcut_graph.hpp` – in-memory CH representation, bidirectional search, and query API.
- `src/shortcut_graph.cpp` – Parquet loader (Arrow/Parquet) and hierarchy-aware routing logic.
- `src/h3_utils.cpp` – helpers that wrap the H3 C API for ancestor and LCA lookups.
- `src/main.cpp` – CLI entry point for single or batched queries.

### Dependencies

Install a recent C++ toolchain (CMake ≥ 3.20, GCC ≥ 11 or Clang ≥ 13) plus Apache Arrow/Parquet and libh3. Two common options:

**System packages (Ubuntu 22.04 or newer)**

```bash
sudo apt update
sudo apt install build-essential cmake libarrow-dev libparquet-dev libh3-dev
```

If your distro does not ship up-to-date Arrow binaries, add Apache's APT repository first:

```bash
wget https://apache.jfrog.io/artifactory/arrow/$(lsb_release -sc)/apache-arrow-apt-source-latest-$(lsb_release -sc).deb
sudo apt install ./apache-arrow-apt-source-latest-$(lsb_release -sc).deb
sudo apt update
sudo apt install build-essential cmake libarrow-dev libparquet-dev libh3-dev
```

**Conda environment (cross-platform)**

If you do not already have Conda, install Miniconda locally:

```bash
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh
bash Miniconda3-latest-Linux-x86_64.sh -b -p "$HOME/miniconda3"
source "$HOME/miniconda3/etc/profile.d/conda.sh"
```

Create and activate the environment with all required libraries:

```bash
conda create -y -n ch-query-engine -c conda-forge \
	cmake ninja compilers arrow-cpp parquet-cpp libh3
conda activate ch-query-engine
```

The build picks up Arrow and H3 headers from the active environment. If you launch a new shell, run `source "$HOME/miniconda3/etc/profile.d/conda.sh"` followed by `conda activate ch-query-engine` before building. Adjust `CMAKE_PREFIX_PATH` if you place the libraries in a non-standard location.

**H3 library**

Some Linux distributions and conda channels do not publish the H3 C library (`libh3`). In that case, build it from source (the snippet installs H3 into the active Conda environment so the headers and library live under `$CONDA_PREFIX`):

```bash
source "$HOME/miniconda3/etc/profile.d/conda.sh"
conda activate ch-query-engine

git clone https://github.com/uber/h3.git "$HOME/src/h3"
cmake -S "$HOME/src/h3" -B "$HOME/src/h3/build" \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=ON \
	-DCMAKE_INSTALL_PREFIX="$CONDA_PREFIX"
cmake --build "$HOME/src/h3/build" --target install -- -j"$(nproc)"
```

If the install step fails due to permissions, rerun it with `sudo cmake --install "$HOME/src/h3/build"`. When H3 is installed outside the Conda prefix, export `H3_INCLUDE_DIR` and `H3_LIBRARY` to point at the correct locations before running `./run_cpp.sh`.

### Build & Run

```bash
./build_cpp.sh
```

Pass additional CMake options after the script (for example `./build_cpp.sh -DCMAKE_BUILD_TYPE=Debug`).

To compile and immediately execute a query:

```bash
./run_cpp.sh --shortcuts /path/to/shortcuts.parquet \
		 --edges /path/to/Burnaby_driving_simplified_edges_with_h3.csv \
		 --source 1593 --target 4835
```

Flags:

- `--shortcuts` *(required)*: Parquet file or directory containing the shortcut table.
- `--edges`: CSV with columns `id`, `incoming_cell`, `lca_res` for H3 ancestor lookups.
- `--source`, `--target`: Edge ids for the query (repeat the pair to run multiple queries).
- `--random N`: Sample N random queries (requires `--edges` for hierarchy-aware filters).
- `--seed S`: Seed for the random sampler.
- `--config PATH`: Optional INI-style file that supplies default values for `shortcuts`, `edges`, `random`, and `seed`.

Example config (`config/shortcut_router.cfg`):

```
shortcuts = /absolute/path/to/shortcuts
edges = /absolute/path/to/edges.csv
random = 25
seed = 2024
```

Values provided on the command line always override the config file. Queries (via `--source/--target` or `--random`) must still be supplied either directly or through the config defaults.

The CLI prints distance, reconstructed path (edge ids), and runtime per query. When edge metadata is provided, hierarchy constraints (`inside == ±1` and H3 parent checks) are enforced exactly as in the notebook.

---

## End-to-End Flow

1. Generate shortcuts with the Spark pipeline (or load an existing Parquet file).
2. Validate structure, hierarchy flags, and cost distributions in the notebook.
3. Feed the Parquet file to the C++ binary for production queries or service integration.

Additional background on the data schema and routing algorithm is available in `docs/algorithm.md`.

This separation keeps heavy preprocessing in Spark, rapid iteration in Python, and low-latency serving in C++ while sharing one consistent data contract.