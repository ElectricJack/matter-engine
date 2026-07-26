#ifndef CELL_VISITOR_H
#define CELL_VISITOR_H

// Phase 4 (Step 4) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md:
// this header used to include raylib.h for Matrix. It is C++-only (no C
// consumer), so it uses matter_math.h's mm::Mat4 instead.
#include "matter_math.h"

// Forward declarations
struct Cell;
class Cluster;

// Abstract visitor interface for Cell and Cluster operations
class CellVisitor {
public:
    virtual ~CellVisitor() = default;

    // Visit methods for different types
    virtual void visit_cell(const Cell& cell) = 0;
    virtual void visit_cluster(const Cluster& cluster) = 0;
};

// Visitor for rendering cells with transformation support
class CellRenderVisitor : public CellVisitor {
public:
    virtual ~CellRenderVisitor() = default;

    // Additional methods specific to rendering
    virtual void visit_cell_transformed(const Cell& cell, const mm::Mat4& transform) = 0;
    virtual void set_wireframe_mode(bool wireframe) = 0;
    virtual bool get_wireframe_mode() const = 0;
};

#endif // CELL_VISITOR_H 