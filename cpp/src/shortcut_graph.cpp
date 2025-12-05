/**
 * @file shortcut_graph.cpp
 * @brief Implementation of the Contraction Hierarchy graph and query engine.
 *
 * This file implements the ShortcutGraph class which provides:
 * - Parquet loading for shortcut edges (Apache Arrow/Parquet)
 * - CSV loading for edge metadata (H3 cells, lengths)
 * - Hierarchy-aware bidirectional Dijkstra search
 * - Shortcut path expansion to base edges
 *
 * The bidirectional search uses H3 geospatial cells to constrain exploration:
 * - Forward search: only upward shortcuts (inside == +1)
 * - Backward search: only downward shortcuts (inside == -1) plus lateral
 *   shortcuts (inside == 0) when at the highest common ancestor cell
 *
 * @note Requires Apache Arrow, Parquet, and libh3 libraries.
 */

#include "shortcut_graph.hpp"

#include "h3_utils.hpp"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <h3api.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

using arrow::Array;
using arrow::Status;
using ColumnVector = std::vector<uint32_t>;

std::shared_ptr<arrow::Table> read_parquet_table(const std::string& path) {
    auto maybe_file = arrow::io::ReadableFile::Open(path);
    if (!maybe_file.ok()) {
        throw std::runtime_error("Failed to open parquet file: " + maybe_file.status().ToString());
    }
    std::shared_ptr<arrow::io::ReadableFile> infile = *maybe_file;
    std::unique_ptr<parquet::arrow::FileReader> reader;
    Status st = parquet::arrow::OpenFile(infile, arrow::default_memory_pool(), &reader);
    if (!st.ok()) {
        throw std::runtime_error("Failed to create parquet reader: " + st.ToString());
    }
    std::shared_ptr<arrow::Table> table;
    st = reader->ReadTable(&table);
    if (!st.ok()) {
        throw std::runtime_error("Failed to read parquet table: " + st.ToString());
    }
    return table;
}

std::vector<std::shared_ptr<arrow::Table>> collect_tables(const std::string& parquet_path) {
    std::vector<std::shared_ptr<arrow::Table>> tables;
    namespace fs = std::filesystem;
    const fs::path base_path(parquet_path);
    if (fs::is_directory(base_path)) {
        for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto ext = entry.path().extension().string();
            if (ext == ".parquet" || ext == ".pq") {
                tables.push_back(read_parquet_table(entry.path().string()));
            }
        }
    } else {
        tables.push_back(read_parquet_table(parquet_path));
    }
    if (tables.empty()) {
        throw std::runtime_error("No parquet files found at: " + parquet_path);
    }
    return tables;
}

template <typename CType>
CType get_value(const std::shared_ptr<Array>& array, int64_t index) {
    switch (array->type_id()) {
        case arrow::Type::UINT8:
            return static_cast<CType>(std::static_pointer_cast<arrow::UInt8Array>(array)->Value(index));
        case arrow::Type::INT8:
            return static_cast<CType>(std::static_pointer_cast<arrow::Int8Array>(array)->Value(index));
        case arrow::Type::UINT16:
            return static_cast<CType>(std::static_pointer_cast<arrow::UInt16Array>(array)->Value(index));
        case arrow::Type::INT16:
            return static_cast<CType>(std::static_pointer_cast<arrow::Int16Array>(array)->Value(index));
        case arrow::Type::UINT32:
            return static_cast<CType>(std::static_pointer_cast<arrow::UInt32Array>(array)->Value(index));
        case arrow::Type::INT32:
            return static_cast<CType>(std::static_pointer_cast<arrow::Int32Array>(array)->Value(index));
        case arrow::Type::UINT64:
            return static_cast<CType>(std::static_pointer_cast<arrow::UInt64Array>(array)->Value(index));
        case arrow::Type::INT64:
            return static_cast<CType>(std::static_pointer_cast<arrow::Int64Array>(array)->Value(index));
        case arrow::Type::FLOAT:
            return static_cast<CType>(std::static_pointer_cast<arrow::FloatArray>(array)->Value(index));
        case arrow::Type::DOUBLE:
            return static_cast<CType>(std::static_pointer_cast<arrow::DoubleArray>(array)->Value(index));
        default:
            throw std::runtime_error("Unsupported Arrow type for numeric conversion");
    }
}

struct ColumnIndices {
    int incoming = -1;
    int outgoing = -1;
    int cost = -1;
    int via = -1;
    int cell = -1;
    int inside = -1;
};

ColumnIndices resolve_columns(const std::shared_ptr<arrow::Schema>& schema) {
    ColumnIndices idx;
    idx.incoming = schema->GetFieldIndex("incoming_edge");
    idx.outgoing = schema->GetFieldIndex("outgoing_edge");
    idx.cost = schema->GetFieldIndex("cost");
    idx.via = schema->GetFieldIndex("via_edge");
    idx.cell = schema->GetFieldIndex("cell");
    idx.inside = schema->GetFieldIndex("inside");
    if (idx.incoming < 0 || idx.outgoing < 0 || idx.cost < 0 || idx.inside < 0) {
        throw std::runtime_error("Parquet file missing required columns");
    }
    return idx;
}

struct CsvColumns {
    int id = -1;
    int incoming_cell = -1;
    int lca_res = -1;
    int length = -1;
};

CsvColumns resolve_csv_columns(const std::vector<std::string>& headers) {
    CsvColumns cols;
    for (size_t i = 0; i < headers.size(); ++i) {
        const std::string& header = headers[i];
        if (header == "id") {
            cols.id = static_cast<int>(i);
        } else if (header == "incoming_cell") {
            cols.incoming_cell = static_cast<int>(i);
        } else if (header == "lca_res") {
            cols.lca_res = static_cast<int>(i);
        } else if (header == "length") {
            cols.length = static_cast<int>(i);
        }
    }
    if (cols.id < 0) {
        throw std::runtime_error("CSV metadata missing id column");
    }
    if (cols.incoming_cell < 0) {
        throw std::runtime_error("CSV metadata missing incoming_cell column");
    }
    if (cols.lca_res < 0) {
        throw std::runtime_error("CSV metadata missing lca_res column");
    }
    return cols;
}

std::string trim_copy(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> parts;
    std::string current;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                current.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (ch == ',' && !in_quotes) {
            parts.push_back(trim_copy(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(trim_copy(current));
    return parts;
}

}  // namespace

void ShortcutGraph::load_shortcuts(const std::string& parquet_path) {
    shortcuts_.clear();
    fwd_adj_.clear();
    bwd_adj_.clear();
    shortcut_lookup_.clear();

    const auto tables = collect_tables(parquet_path);
    uint32_t max_edge = 0;

    for (const auto& table : tables) {
        const auto schema = table->schema();
        const auto idx = resolve_columns(schema);
        arrow::TableBatchReader reader(*table);
        std::shared_ptr<arrow::RecordBatch> batch;
        while (true) {
            Status st = reader.ReadNext(&batch);
            if (!st.ok()) {
                throw std::runtime_error("Failed to read record batch: " + st.ToString());
            }
            if (!batch) {
                break;
            }
            const auto& columns = batch->columns();
            const auto incoming = columns[idx.incoming];
            const auto outgoing = columns[idx.outgoing];
            const auto cost = columns[idx.cost];
            const std::shared_ptr<Array> via = idx.via >= 0 ? columns[idx.via] : nullptr;
            const std::shared_ptr<Array> cell = idx.cell >= 0 ? columns[idx.cell] : nullptr;
            const auto inside = columns[idx.inside];
            const int64_t rows = batch->num_rows();
            for (int64_t i = 0; i < rows; ++i) {
                Shortcut sc{};
                sc.from = get_value<uint32_t>(incoming, i);
                sc.to = get_value<uint32_t>(outgoing, i);
                sc.cost = get_value<double>(cost, i);
                sc.via_edge = via ? get_value<uint32_t>(via, i) : 0U;
                sc.cell = cell ? get_value<uint64_t>(cell, i) : 0ULL;
                sc.inside = static_cast<int8_t>(get_value<int32_t>(inside, i));
                max_edge = std::max({max_edge, sc.from, sc.to});
                shortcuts_.push_back(sc);
            }
        }
    }

    fwd_adj_.assign(static_cast<size_t>(max_edge) + 1, {});
    bwd_adj_.assign(static_cast<size_t>(max_edge) + 1, {});

    for (uint32_t idx = 0; idx < shortcuts_.size(); ++idx) {
        const auto& sc = shortcuts_[idx];
        if (sc.from >= fwd_adj_.size()) {
            continue;
        }
        fwd_adj_[sc.from].push_back(idx);
        if (sc.to < bwd_adj_.size()) {
            bwd_adj_[sc.to].push_back(idx);
        }
        // Build lookup for expand_shortcut_path: key = (from << 32 | to)
        const uint64_t key = (static_cast<uint64_t>(sc.from) << 32) | sc.to;
        // Keep the first (or could keep minimum cost) shortcut for each pair
        if (shortcut_lookup_.find(key) == shortcut_lookup_.end()) {
            shortcut_lookup_[key] = idx;
        }
    }
}

void ShortcutGraph::load_edge_metadata(const std::string& csv_path) {
    edge_meta_.clear();
    std::ifstream file(csv_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open edge metadata CSV: " + csv_path);
    }
    std::string header_line;
    if (!std::getline(file, header_line)) {
        throw std::runtime_error("Edge metadata CSV is empty: " + csv_path);
    }
    const auto headers = split_csv_line(header_line);
    const auto cols = resolve_csv_columns(headers);

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const auto parts = split_csv_line(line);
        if (static_cast<int>(parts.size()) <= std::max({cols.id, cols.incoming_cell, cols.lca_res})) {
            continue;
        }
        const uint32_t id = static_cast<uint32_t>(std::stoul(parts[cols.id]));
        EdgeMeta meta;
        meta.incoming_cell = static_cast<uint64_t>(std::stoull(parts[cols.incoming_cell]));
        meta.lca_res = std::stoi(parts[cols.lca_res]);
        // Load edge length if available
        if (cols.length >= 0 && static_cast<int>(parts.size()) > cols.length && !parts[cols.length].empty()) {
            meta.length = std::stod(parts[cols.length]);
        }
        edge_meta_[id] = meta;
    }
}

ShortcutGraph::HighCell ShortcutGraph::compute_high_cell(uint32_t source_edge, uint32_t target_edge) const {
    const auto it_src = edge_meta_.find(source_edge);
    const auto it_dst = edge_meta_.find(target_edge);
    if (it_src == edge_meta_.end() || it_dst == edge_meta_.end()) {
        return {};
    }
    const uint64_t src_cell = h3_find_ancestor(it_src->second.incoming_cell, it_src->second.lca_res);
    const uint64_t dst_cell = h3_find_ancestor(it_dst->second.incoming_cell, it_dst->second.lca_res);
    const uint64_t lca = h3_find_lca(src_cell, dst_cell);
    HighCell high;
    high.cell = lca;
    high.res = h3_resolution(lca);
    return high;
}

bool ShortcutGraph::parent_check(uint64_t child_cell, uint64_t parent_cell, int parent_res) {
    if (parent_cell == 0 || parent_res < 0) {
        return true;
    }
    if (child_cell == 0) {
        return false;
    }
    const int child_res = h3_resolution(child_cell);
    if (parent_res > child_res) {
        return false;
    }
    H3Index derived = 0;
    const H3Error status = cellToParent(child_cell, parent_res, &derived);
    if (status != E_SUCCESS) {
        return false;
    }
    return derived == parent_cell;
}

QueryResult ShortcutGraph::run_bidirectional(uint32_t source_edge, uint32_t target_edge, const QueryContext& ctx) const {
    if (fwd_adj_.empty()) {
        return {-1.0, {}, false};
    }
    const size_t edge_count = fwd_adj_.size();
    if (source_edge >= edge_count || target_edge >= edge_count) {
        return {-1.0, {}, false};
    }
    if (source_edge == target_edge) {
        return {0.0, {source_edge}, true};
    }

    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> dist_fwd(edge_count, inf);
    std::vector<double> dist_bwd(edge_count, inf);
    std::vector<int32_t> parent_fwd(edge_count, -1);
    std::vector<int32_t> parent_bwd(edge_count, -1);

    struct Node {
        double distance;
        uint32_t edge;
    };
    auto cmp = [](const Node& lhs, const Node& rhs) { return lhs.distance > rhs.distance; };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq_fwd(cmp);
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq_bwd(cmp);

    dist_fwd[source_edge] = 0.0;
    dist_bwd[target_edge] = 0.0;
    parent_fwd[source_edge] = static_cast<int32_t>(source_edge);
    parent_bwd[target_edge] = static_cast<int32_t>(target_edge);
    pq_fwd.push({0.0, source_edge});
    pq_bwd.push({0.0, target_edge});

    double best = inf;
    uint32_t meeting_edge = std::numeric_limits<uint32_t>::max();

    const auto& high = ctx.high_cell;

    while (!pq_fwd.empty() || !pq_bwd.empty()) {
        if (!pq_fwd.empty()) {
            const Node curr = pq_fwd.top();
            pq_fwd.pop();
            if (curr.distance > dist_fwd[curr.edge]) {
                goto backward_step;
            }
            if (curr.distance >= best) {
                goto backward_step;
            }
            const auto& adjacency = fwd_adj_[curr.edge];
            for (uint32_t idx : adjacency) {
                const auto& sc = shortcuts_[idx];
                if (sc.inside != 1) {
                    continue;
                }
                if (!parent_check(sc.cell, high.cell, high.res)) {
                    continue;
                }
                const double candidate = curr.distance + sc.cost;
                if (candidate < dist_fwd[sc.to]) {
                    dist_fwd[sc.to] = candidate;
                    parent_fwd[sc.to] = static_cast<int32_t>(curr.edge);
                    pq_fwd.push({candidate, sc.to});
                    if (dist_bwd[sc.to] < inf) {
                        const double total = candidate + dist_bwd[sc.to];
                        if (total < best) {
                            best = total;
                            meeting_edge = sc.to;
                        }
                    }
                }
            }
        }
    backward_step:
        if (!pq_bwd.empty()) {
            const Node curr = pq_bwd.top();
            pq_bwd.pop();
            if (curr.distance > dist_bwd[curr.edge]) {
                goto termination_check;
            }
            if (curr.distance >= best) {
                goto termination_check;
            }
            const auto& adjacency = bwd_adj_[curr.edge];
            for (uint32_t idx : adjacency) {
                const auto& sc = shortcuts_[idx];
                const bool allow_lateral_at_high = ( sc.inside == 0 && sc.cell == high.cell);
                if (sc.inside != -1 && !allow_lateral_at_high) {
                    continue;
                }
                if (!parent_check(sc.cell, high.cell, high.res)) {
                   continue;
                }
                const uint32_t prev = sc.from;
                const double candidate = curr.distance + sc.cost;
                if (candidate < dist_bwd[prev]) {
                    dist_bwd[prev] = candidate;
                    parent_bwd[prev] = static_cast<int32_t>(curr.edge);
                    pq_bwd.push({candidate, prev});
                    if (dist_fwd[prev] < inf) {
                        const double total = dist_fwd[prev] + candidate;
                        if (total < best) {
                            best = total;
                            meeting_edge = prev;
                        }
                    }
                }
            }
        }
    termination_check:
        if (best < inf) {
            bool should_break = true;
            if (!pq_fwd.empty() && pq_fwd.top().distance < best) {
                should_break = false;
            }
            if (!pq_bwd.empty() && pq_bwd.top().distance < best) {
                should_break = false;
            }
            if (should_break) {
                break;
            }
        }
    }

    if (!(best < inf) || meeting_edge == std::numeric_limits<uint32_t>::max()) {
        return {-1.0, {}, false};
    }

    std::vector<uint32_t> forward_path;
    uint32_t current = meeting_edge;
    forward_path.push_back(meeting_edge);
    while (current != source_edge) {
        int32_t parent = parent_fwd[current];
        if (parent < 0 || static_cast<uint32_t>(parent) == current) {
            return {-1.0, {}, false};
        }
        current = static_cast<uint32_t>(parent);
        forward_path.push_back(current);
    }
    std::reverse(forward_path.begin(), forward_path.end());

    std::vector<uint32_t> backward_path;
    current = meeting_edge;
    while (current != target_edge) {
        int32_t next = parent_bwd[current];
        if (next < 0 || static_cast<uint32_t>(next) == current) {
            return {-1.0, {}, false};
        }
        current = static_cast<uint32_t>(next);
        backward_path.push_back(current);
    }

    std::vector<uint32_t> path = forward_path;
    path.insert(path.end(), backward_path.begin(), backward_path.end());
    
    // Add target edge cost to the total distance
    const auto it_target = edge_meta_.find(target_edge);
    double target_cost = 0.0;
    if (it_target != edge_meta_.end()) {
        target_cost = it_target->second.length;
    }
    
    return {best + target_cost, path, true};
}

QueryResult ShortcutGraph::query(uint32_t source_edge, uint32_t target_edge) const {
    const QueryContext ctx{compute_high_cell(source_edge, target_edge)};
    return run_bidirectional(source_edge, target_edge, ctx);
}

double ShortcutGraph::get_edge_length(uint32_t edge_id) const {
    const auto it = edge_meta_.find(edge_id);
    if (it == edge_meta_.end()) {
        return 0.0;
    }
    return it->second.length;
}

ExpandedResult ShortcutGraph::expand_shortcut_path(const std::vector<uint32_t>& shortcut_path) const {
    if (shortcut_path.empty()) {
        return {{}, true};
    }
    if (shortcut_path.size() == 1) {
        return {{shortcut_path[0]}, true};
    }
    
    // Helper lambda to recursively expand a pair (u, v)
    std::function<std::vector<uint32_t>(uint32_t, uint32_t, std::unordered_set<uint64_t>&)> expand_pair;
    expand_pair = [this, &expand_pair](uint32_t u, uint32_t v, std::unordered_set<uint64_t>& visited) -> std::vector<uint32_t> {
        const uint64_t key = (static_cast<uint64_t>(u) << 32) | v;
        
        // Cycle detection
        if (visited.count(key)) {
            return {u, v};
        }
        visited.insert(key);
        
        // Look up the shortcut
        auto it = shortcut_lookup_.find(key);
        if (it == shortcut_lookup_.end()) {
            // No shortcut entry, these are consecutive base edges
            return {u, v};
        }
        
        const auto& sc = shortcuts_[it->second];
        const uint32_t via = sc.via_edge;
        
        if (via == 0 || via == u) {
            // Base edge (no intermediate or via equals source)
            return {u, v};
        }
        
        // Recursively expand: u -> via and via -> v
        auto left = expand_pair(u, via, visited);
        auto right = expand_pair(via, v, visited);
        
        // Merge, avoiding duplicate at the junction
        if (!right.empty() && !left.empty() && right[0] == left.back()) {
            left.insert(left.end(), right.begin() + 1, right.end());
        } else {
            left.insert(left.end(), right.begin(), right.end());
        }
        return left;
    };
    
    // Expand each consecutive pair and merge
    std::vector<uint32_t> base_edges;
    for (size_t i = 0; i + 1 < shortcut_path.size(); ++i) {
        std::unordered_set<uint64_t> visited;
        auto expanded = expand_pair(shortcut_path[i], shortcut_path[i + 1], visited);
        for (uint32_t e : expanded) {
            if (base_edges.empty() || base_edges.back() != e) {
                base_edges.push_back(e);
            }
        }
    }
    
    return {base_edges, true};
}

std::vector<std::pair<uint32_t, uint32_t>>
ShortcutGraph::sample_random_pairs(std::size_t count, uint32_t seed) const {
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    if (fwd_adj_.empty()) {
        return pairs;
    }
    std::vector<uint32_t> nodes;
    nodes.reserve(fwd_adj_.size());
    for (uint32_t idx = 0; idx < fwd_adj_.size(); ++idx) {
        if (!fwd_adj_[idx].empty() || !bwd_adj_[idx].empty()) {
            nodes.push_back(idx);
        }
    }
    if (nodes.size() < 2) {
        return pairs;
    }
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> dist(0, nodes.size() - 1);
    pairs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        uint32_t source = nodes[dist(rng)];
        uint32_t target = nodes[dist(rng)];
        if (source == target) {
            target = nodes[(dist(rng) + 1) % nodes.size()];
        }
        pairs.emplace_back(source, target);
    }
    return pairs;
}
