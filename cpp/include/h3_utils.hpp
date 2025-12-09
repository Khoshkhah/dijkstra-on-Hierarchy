/**
 * @file h3_utils.hpp
 * @brief H3 geospatial indexing utility functions.
 *
 * Provides helper functions for working with Uber's H3 hierarchical geospatial
 * indexing system. These utilities support the Contraction Hierarchy routing
 * algorithm by enabling cell ancestor lookups and lowest common ancestor (LCA)
 * computation between H3 cells.
 *
 * @see https://h3geo.org/ for H3 documentation
 */

#pragma once

#include <cstdint>

/**
 * @brief Find the ancestor of an H3 cell at a given resolution.
 * @param cell The H3 cell index (64-bit integer representation).
 * @param res Target resolution (0-15, where 0 is coarsest).
 * @return The ancestor cell at the specified resolution, or 0 if invalid.
 */
uint64_t h3_find_ancestor(uint64_t cell, int res);

/**
 * @brief Find the lowest common ancestor (LCA) of two H3 cells.
 * @param cell_a First H3 cell index.
 * @param cell_b Second H3 cell index.
 * @return The LCA cell, or 0 if no common ancestor exists.
 */
uint64_t h3_find_lca(uint64_t cell_a, uint64_t cell_b);

/**
 * @brief Get the resolution of an H3 cell.
 * @param cell The H3 cell index.
 * @return Resolution (0-15), or -1 if the cell is invalid.
 */
int h3_resolution(uint64_t cell);

/**
 * @brief Get the boundary coordinates of an H3 cell.
 * @param cell The H3 cell index.
 * @return Vector of {lat, lon} pairs (degrees).
 */
#include <vector>
#include <utility>
std::vector<std::pair<double, double>> h3_cell_boundary(uint64_t cell);

// Returns cell ID for lat/lon at resolution
uint64_t h3_lat_lng_to_cell(double lat, double lon, int res);
