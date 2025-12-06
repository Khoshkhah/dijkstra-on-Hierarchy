/**
 * @file main.cpp
 * @brief Command-line interface for the Contraction Hierarchy query engine.
 *
 * This program provides a CLI for querying shortest paths using precomputed
 * Contraction Hierarchies. It supports:
 * - Loading shortcuts from Parquet and edge metadata from CSV
 * - Individual queries via --source/--target
 * - Random query sampling via --random
 * - Configuration files for default paths
 *
 * Usage:
 *   shortcut_router --shortcuts PATH [--edges PATH] [--source ID --target ID]
 *                   [--random COUNT] [--seed SEED] [--config FILE]
 *
 * Output includes:
 * - Total distance (including destination edge cost)
 * - Shortcut-level path
 * - Expanded base edge path
 * - Query runtime in milliseconds
 *
 * @see docs/algorithm.md for algorithm details
 * @see readme.md for build and usage instructions
 */

#include "shortcut_graph.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace {

/**
 * @brief Parsed command-line arguments.
 */
struct Arguments {
    std::string shortcuts_path;
    std::string edges_path;
    std::string config_path;
    std::vector<std::pair<uint32_t, uint32_t>> queries;
    std::vector<uint32_t> source_edges;
    std::vector<double> source_distances;
    std::vector<uint32_t> target_edges;
    std::vector<double> target_distances;
    std::size_t random_queries = 0;
    uint32_t seed = 42;
    bool optimized = false;  // Use optimized multi-source multi-target algorithm
};

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " --shortcuts PATH [--edges PATH] [--source ID --target ID]"
              << " [--sources ID1,ID2,... --source-dists D1,D2,... --targets ID1,ID2,... --target-dists D1,D2,...]"
              << " [--optimized] [--random COUNT] [--seed SEED]\n";
}

std::string trim_copy(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

std::unordered_map<std::string, std::string> load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open config file: " + path);
    }
    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        const auto pos = trimmed.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const std::string key = trim_copy(trimmed.substr(0, pos));
        const std::string value = trim_copy(trimmed.substr(pos + 1));
        if (!key.empty()) {
            values[key] = value;
        }
    }
    return values;
}

Arguments parse_args(int argc, char** argv) {
    Arguments args;
    std::unordered_map<std::string, std::string> config_values;

    for (int i = 1; i < argc; ++i) {
        const std::string_view token(argv[i]);
        if (token == "--config") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--config requires a path argument");
            }
            args.config_path = argv[++i];
            config_values = load_config(args.config_path);
        }
    }

    if (const auto it = config_values.find("shortcuts"); it != config_values.end()) {
        args.shortcuts_path = it->second;
    }
    if (const auto it = config_values.find("edges"); it != config_values.end()) {
        args.edges_path = it->second;
    }
    if (const auto it = config_values.find("random"); it != config_values.end()) {
        args.random_queries = static_cast<std::size_t>(std::stoull(it->second));
    }
    if (const auto it = config_values.find("seed"); it != config_values.end()) {
        args.seed = static_cast<uint32_t>(std::stoul(it->second));
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view token(argv[i]);
        if (token == "--config") {
            ++i;
            continue;
        } else if (token == "--shortcuts" && i + 1 < argc) {
            args.shortcuts_path = argv[++i];
        } else if (token == "--edges" && i + 1 < argc) {
            args.edges_path = argv[++i];
        } else if (token == "--source" && i + 1 < argc) {
            const uint32_t source = static_cast<uint32_t>(std::stoul(argv[++i]));
            if (i + 2 >= argc || std::string_view(argv[i + 1]) != "--target") {
                throw std::runtime_error("--source must be followed by --target");
            }
            const uint32_t target = static_cast<uint32_t>(std::stoul(argv[i + 2]));
            i += 2;
            args.queries.emplace_back(source, target);
        } else if (token == "--target") {
            throw std::runtime_error("--target must be paired with --source preceding it");
        } else if (token == "--sources" && i + 1 < argc) {
            // Parse comma-separated list of source edge IDs
            std::string sources_str = argv[++i];
            std::stringstream ss(sources_str);
            std::string item;
            while (std::getline(ss, item, ',')) {
                args.source_edges.push_back(std::stoul(item));
            }
        } else if (token == "--source-dists" && i + 1 < argc) {
            // Parse comma-separated list of source distances
            std::string dists_str = argv[++i];
            std::stringstream ss(dists_str);
            std::string item;
            while (std::getline(ss, item, ',')) {
                args.source_distances.push_back(std::stod(item));
            }
        } else if (token == "--targets" && i + 1 < argc) {
            // Parse comma-separated list of target edge IDs
            std::string targets_str = argv[++i];
            std::stringstream ss(targets_str);
            std::string item;
            while (std::getline(ss, item, ',')) {
                args.target_edges.push_back(std::stoul(item));
            }
        } else if (token == "--target-dists" && i + 1 < argc) {
            // Parse comma-separated list of target distances
            std::string dists_str = argv[++i];
            std::stringstream ss(dists_str);
            std::string item;
            while (std::getline(ss, item, ',')) {
                args.target_distances.push_back(std::stod(item));
            }
        } else if (token == "--random" && i + 1 < argc) {
            args.random_queries = static_cast<std::size_t>(std::stoull(argv[++i]));
        } else if (token == "--seed" && i + 1 < argc) {
            args.seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (token == "--optimized") {
            args.optimized = true;
        } else if (token == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error(std::string("Unknown argument: ") + std::string(token));
        }
    }
    if (args.shortcuts_path.empty()) {
        throw std::runtime_error("--shortcuts is required");
    }
    
    // Check if we have multi-source multi-target query
    bool has_multi = !args.source_edges.empty() && !args.target_edges.empty();
    bool has_single = !args.queries.empty();
    bool has_random = args.random_queries > 0;
    
    if (!has_multi && !has_single && !has_random) {
        throw std::runtime_error("Provide --source/--target, --sources/--targets, or --random");
    }
    
    if (has_multi) {
        if (args.source_edges.size() != args.source_distances.size()) {
            throw std::runtime_error("--sources and --source-dists must have same length");
        }
        if (args.target_edges.size() != args.target_distances.size()) {
            throw std::runtime_error("--targets and --target-dists must have same length");
        }
    }
    
    return args;
}

void print_result(const std::pair<uint32_t, uint32_t>& query, const QueryResult& result, 
                  const ExpandedResult& expanded, double target_cost, double elapsed_ms) {
    std::cout << "Query " << query.first << " -> " << query.second << "\n";
    if (!result.reachable) {
        std::cout << "  No path found\n";
        std::cout << "  Runtime: " << elapsed_ms << " ms\n";
        return;
    }
    std::cout << "  Distance (including destination edge): " << result.distance << "\n";
    std::cout << "  Destination edge cost: " << target_cost << "\n";
    std::cout << "  Shortcut path length: " << result.path.size() << " edges\n";
    
    // Print shortcut path
    std::cout << "  Shortcut path: ";
    if (result.path.size() <= 10) {
        for (size_t i = 0; i < result.path.size(); ++i) {
            std::cout << result.path[i];
            if (i + 1 < result.path.size()) {
                std::cout << " -> ";
            }
        }
    } else {
        for (size_t i = 0; i < 5; ++i) {
            std::cout << result.path[i] << " -> ";
        }
        std::cout << "... -> ";
        for (size_t i = result.path.size() - 5; i < result.path.size(); ++i) {
            std::cout << result.path[i];
            if (i + 1 < result.path.size()) {
                std::cout << " -> ";
            }
        }
    }
    std::cout << "\n";
    
    // Print expanded base edge path
    if (expanded.success && !expanded.base_edges.empty()) {
        std::cout << "  Expanded base edge path length: " << expanded.base_edges.size() << " edges\n";
        std::cout << "  Expanded path: ";
        // Always print full path
        for (size_t i = 0; i < expanded.base_edges.size(); ++i) {
            std::cout << expanded.base_edges[i];
            if (i + 1 < expanded.base_edges.size()) {
                std::cout << " -> ";
            }
        }
        std::cout << "\n";
    }
    
    std::cout << "  Runtime: " << elapsed_ms << " ms\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Arguments args = parse_args(argc, argv);
        ShortcutGraph graph;
        std::cout << "Loading shortcuts from " << args.shortcuts_path << "..." << std::endl;
        graph.load_shortcuts(args.shortcuts_path);
        if (!args.edges_path.empty()) {
            std::cout << "Loading edge metadata from " << args.edges_path << "..." << std::endl;
            graph.load_edge_metadata(args.edges_path);
        }

        // Handle multi-source multi-target query
        if (!args.source_edges.empty() && !args.target_edges.empty()) {
            const auto start = std::chrono::high_resolution_clock::now();
            QueryResult result;
            
            if (args.optimized) {
                // Use optimized O(E log V) multi-source multi-target algorithm
                result = graph.query_multi_optimized(
                    args.source_edges, 
                    args.target_edges,
                    args.source_distances,
                    args.target_distances
                );
            } else {
                // Use batch processing O(N*M*E log V) algorithm
                result = graph.query_multi(
                    args.source_edges, 
                    args.target_edges,
                    args.source_distances,
                    args.target_distances
                );
            }
            
            const auto end = std::chrono::high_resolution_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
            
            if (!result.reachable) {
                std::cout << "No path found between source and target edge sets." << std::endl;
                return 1;
            }
            
            const ExpandedResult expanded = graph.expand_shortcut_path(result.path);
            
            std::cout << "Multi-source multi-target query";
            if (args.optimized) {
                std::cout << " (optimized)";
            }
            std::cout << ":\n";
            std::cout << "  Distance (total including approach distances): " << result.distance << "\n";
            std::cout << "  Shortcut path length: " << result.path.size() << " edges\n";
            std::cout << "  Shortcut path: ";
            for (size_t i = 0; i < result.path.size(); ++i) {
                std::cout << result.path[i];
                if (i + 1 < result.path.size()) {
                    std::cout << " -> ";
                }
            }
            std::cout << "\n";
            
            if (expanded.success && !expanded.base_edges.empty()) {
                std::cout << "  Expanded base edge path length: " << expanded.base_edges.size() << " edges\n";
                std::cout << "  Expanded path: ";
                for (size_t i = 0; i < expanded.base_edges.size(); ++i) {
                    std::cout << expanded.base_edges[i];
                    if (i + 1 < expanded.base_edges.size()) {
                        std::cout << " -> ";
                    }
                }
                std::cout << "\n";
            }
            std::cout << "  Runtime: " << elapsed_ms << " ms\n";
            
            return 0;
        }

        if (args.random_queries > 0) {
            const auto pairs = graph.sample_random_pairs(args.random_queries, args.seed);
            if (pairs.empty()) {
                std::cout << "No eligible nodes for random sampling." << std::endl;
            }
            for (const auto& query : pairs) {
                const auto start = std::chrono::high_resolution_clock::now();
                const QueryResult result = graph.query(query.first, query.second);
                const ExpandedResult expanded = graph.expand_shortcut_path(result.path);
                const double target_cost = graph.get_edge_length(query.second);
                const auto end = std::chrono::high_resolution_clock::now();
                const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
                print_result(query, result, expanded, target_cost, elapsed_ms);
            }
        }

        for (const auto& query : args.queries) {
            const auto start = std::chrono::high_resolution_clock::now();
            const QueryResult result = graph.query(query.first, query.second);
            const ExpandedResult expanded = graph.expand_shortcut_path(result.path);
            const double target_cost = graph.get_edge_length(query.second);
            const auto end = std::chrono::high_resolution_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
            print_result(query, result, expanded, target_cost, elapsed_ms);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}
