/**
 * @file h3_utils.cpp
 * @brief Implementation of H3 geospatial utility functions.
 *
 * This file provides the implementation for H3 cell hierarchy operations
 * used by the Contraction Hierarchy routing algorithm. It wraps the libh3
 * C API to provide convenient C++ functions for:
 * - Finding ancestor cells at specified resolutions
 * - Computing lowest common ancestors between cells
 * - Querying cell resolution levels
 *
 * @note Requires libh3 to be installed and linked.
 */

#include "h3_utils.hpp"

#include <h3api.h>

namespace {

/**
 * @brief Internal helper to reduce a cell to a target resolution.
 * @param cell The source H3 cell.
 * @param target_res The desired resolution.
 * @return The parent cell at target_res, or 0 on failure.
 */

uint64_t reduce_to_resolution(uint64_t cell, int target_res) {
    if (cell == 0) {
        return 0;
    }
    if (target_res < 0) {
        return 0;
    }
    const int cell_res = getResolution(cell);
    if (target_res > cell_res) {
        return cell;
    }
    if (target_res == cell_res) {
        return cell;
    }
    H3Index parent = 0;
    const H3Error status = cellToParent(cell, target_res, &parent);
    if (status != E_SUCCESS) {
        return 0;
    }
    return parent;
}

}  // namespace

uint64_t h3_find_ancestor(uint64_t cell, int res) {
    if (cell == 0) {
        return 0;
    }
    if (res < 0) {
        return 0;
    }
    return reduce_to_resolution(cell, res);
}

uint64_t h3_find_lca(uint64_t cell_a, uint64_t cell_b) {
    if (cell_a == 0 || cell_b == 0) {
        return 0;
    }
    int res_a = getResolution(cell_a);
    int res_b = getResolution(cell_b);
    int res = res_a < res_b ? res_a : res_b;

    while (res >= 0) {
        const uint64_t ancestor_a = reduce_to_resolution(cell_a, res);
        const uint64_t ancestor_b = reduce_to_resolution(cell_b, res);
        if (ancestor_a != 0 && ancestor_a == ancestor_b) {
            return ancestor_a;
        }
        --res;
    }
    return 0;
}

int h3_resolution(uint64_t cell) {
    if (cell == 0) {
        return -1;
    }
    return getResolution(cell);
}

std::vector<std::pair<double, double>> h3_cell_boundary(uint64_t cell) {
    if (cell == 0) return {};
    
    CellBoundary boundary;
    cellToBoundary(cell, &boundary);
    
    std::vector<std::pair<double, double>> coords;
    coords.reserve(boundary.numVerts);
    for (int i = 0; i < boundary.numVerts; ++i) {
        // H3 returns rads, convert to degrees
        double lat = radsToDegs(boundary.verts[i].lat);
        double lon = radsToDegs(boundary.verts[i].lng);
        coords.push_back({lat, lon});
    }
    return coords;
}

uint64_t h3_lat_lng_to_cell(double lat, double lon, int res) {
    if (res < 0 || res > 15) return 0;
    
    LatLng location;
    location.lat = degsToRads(lat);
    location.lng = degsToRads(lon);
    
    H3Index cell;
    H3Error err = latLngToCell(&location, res, &cell);
    if (err != E_SUCCESS) {
        return 0;
    }
    return cell;
}
