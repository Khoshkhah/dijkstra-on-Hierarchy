/**
 * @file shortcut_graph.hpp
 * @brief Contraction Hierarchy graph and query engine.
 *
 * This header defines the ShortcutGraph class which loads precomputed
 * Contraction Hierarchy shortcuts from Parquet files and answers shortest
 * path queries using hierarchy-aware bidirectional Dijkstra search.
 *
 * Key features:
 * - Loads shortcuts from Apache Parquet format
 * - Loads edge metadata (H3 cells, lengths) from CSV
 * - Bidirectional Dijkstra with H3 hierarchy constraints
 * - Path expansion from shortcuts to base edges
 * - Includes destination edge cost in total distance
 *
 * @see docs/algorithm.md for detailed algorithm documentation
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Result of a shortest path query.
 */
struct QueryResult {
    double distance;              ///< Total path cost including destination edge length
    std::vector<uint32_t> path;   ///< Sequence of edge IDs (shortcut-level)
    bool reachable;               ///< True if a path was found
};

/**
 * @brief Result of expanding a shortcut path to base edges.
 */
struct ExpandedResult {
    std::vector<uint32_t> base_edges;  ///< Fully expanded edge sequence
    bool success;                       ///< True if expansion succeeded
};

/**
 * @brief Contraction Hierarchy graph for shortest path queries.
 *
 * ShortcutGraph loads precomputed shortcuts from Parquet files and optional
 * edge metadata from CSV, then answers shortest path queries using hierarchy-
 * aware bidirectional Dijkstra search.
 *
 * The graph uses H3 geospatial cells to constrain search space, ensuring
 * that forward search only explores upward shortcuts and backward search
 * only explores downward shortcuts (plus lateral at the meeting point).
 *
 * Example usage:
 * @code
 *   ShortcutGraph graph;
 *   graph.load_shortcuts("/path/to/shortcuts.parquet");
 *   graph.load_edge_metadata("/path/to/edges.csv");
 *   
 *   QueryResult result = graph.query(source_edge, target_edge);
 *   if (result.reachable) {
 *       std::cout << "Distance: " << result.distance << std::endl;
 *       ExpandedResult expanded = graph.expand_shortcut_path(result.path);
 *       // expanded.base_edges contains the full edge sequence
 *   }
 * @endcode
 */
class ShortcutGraph {
public:
    /**
     * @brief Load shortcuts from a Parquet file or directory.
     * @param parquet_path Path to .parquet file or directory containing parquet files.
     * @throws std::runtime_error if loading fails.
     */
    void load_shortcuts(const std::string& parquet_path);
    
    /**
     * @brief Load edge metadata from a CSV file.
     * @param csv_path Path to CSV with columns: id, incoming_cell, lca_res, length.
     * @throws std::runtime_error if loading fails.
     */
    void load_edge_metadata(const std::string& csv_path);

    /**
     * @brief Run a shortest path query between two edges.
     * @param source_edge Source edge ID.
     * @param target_edge Target edge ID.
     * @return QueryResult with distance (including target edge length), path, and reachability.
     */
    QueryResult query(uint32_t source_edge, uint32_t target_edge) const;
    
    /**
     * @brief Find shortest path from any source edge to any target edge.
     * @param source_edges Vector of source edge IDs.
     * @param target_edges Vector of target edge IDs.
     * @param source_distances Distance from origin point to each source edge.
     * @param target_distances Distance from each target edge to destination point.
     * @return QueryResult with best path including approach distances.
     * 
     * This is more efficient than testing all combinations separately because
     * it runs fewer Dijkstra searches and reuses computation.
     */
    QueryResult query_multi(
        const std::vector<uint32_t>& source_edges,
        const std::vector<uint32_t>& target_edges,
        const std::vector<double>& source_distances,
        const std::vector<double>& target_distances) const;
    
    /**
     * @brief Optimized multi-source multi-target query using single bidirectional search.
     * @param source_edges Vector of source edge IDs.
     * @param target_edges Vector of target edge IDs.
     * @param source_distances Distance from origin point to each source edge.
     * @param target_distances Distance from each target edge to destination point.
     * @return QueryResult with best path including approach distances.
     * 
     * This uses true multi-source multi-target Dijkstra with a single bidirectional
     * search instead of N×M separate queries. Much faster for large candidate sets.
     * Complexity: O(E log V) instead of O(N×M×E log V).
     */
    QueryResult query_multi_optimized(
        const std::vector<uint32_t>& source_edges,
        const std::vector<uint32_t>& target_edges,
        const std::vector<double>& source_distances,
        const std::vector<double>& target_distances) const;
    
    /**
     * @brief Expand a shortcut path into base edges using via_edge mapping.
     * @param shortcut_path Sequence of edge IDs from query result.
     * @return ExpandedResult with fully expanded base edge sequence.
     *
     * Recursively expands each (u, v) pair using via_edge until all shortcuts
     * are resolved to base edges.
     */
    ExpandedResult expand_shortcut_path(const std::vector<uint32_t>& shortcut_path) const;
    
    /**
     * @brief Get the length/cost of a single edge from metadata.
     * @param edge_id Edge ID to look up.
     * @return Edge length, or 0.0 if not found.
     */
    double get_edge_length(uint32_t edge_id) const;

    /**
     * @brief Generate random source-target pairs for benchmarking.
     * @param count Number of pairs to generate.
     * @param seed RNG seed for reproducibility.
     * @return Vector of (source, target) pairs.
     */
    std::vector<std::pair<uint32_t, uint32_t>> sample_random_pairs(std::size_t count, uint32_t seed) const;

private:
    /**
     * @brief Internal representation of a shortcut edge.
     */
    struct Shortcut {
        uint32_t from;      ///< Source edge ID
        uint32_t to;        ///< Target edge ID
        double cost;        ///< Traversal cost
        uint32_t via_edge;  ///< Intermediate edge for expansion (0 if direct)
        uint64_t cell;      ///< H3 cell bounding this shortcut
        int8_t inside;      ///< Direction: +1 upward, 0 lateral, -1 downward
    };

    /**
     * @brief Metadata for a road network edge.
     */
    struct EdgeMeta {
        uint64_t incoming_cell = 0;  ///< H3 cell of the edge
        int lca_res = -1;            ///< Precomputed LCA resolution
        double length = 0.0;         ///< Edge length/cost
    };

    /**
     * @brief Highest common ancestor cell for a query.
     */
    struct HighCell {
        uint64_t cell = 0;  ///< H3 cell ID
        int res = -1;       ///< Cell resolution
    };

    /**
     * @brief Context passed to bidirectional search.
     */
    struct QueryContext {
        HighCell high_cell;  ///< Highest common ancestor constraint
    };

    HighCell compute_high_cell(uint32_t source_edge, uint32_t target_edge) const;
    QueryResult run_bidirectional(uint32_t source_edge, uint32_t target_edge, const QueryContext& ctx) const;

    static bool parent_check(uint64_t child_cell, uint64_t parent_cell, int parent_res);

    std::vector<Shortcut> shortcuts_;              ///< All loaded shortcuts
    std::vector<std::vector<uint32_t>> fwd_adj_;   ///< Forward adjacency (from -> [shortcut indices])
    std::vector<std::vector<uint32_t>> bwd_adj_;
    std::unordered_map<uint32_t, EdgeMeta> edge_meta_;
    
    // Lookup: (from << 32 | to) -> shortcut index for expand_shortcut_path
    std::unordered_map<uint64_t, uint32_t> shortcut_lookup_;
};
