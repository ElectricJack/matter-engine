#ifndef CELL_VISITOR_H
#define CELL_VISITOR_H

// ---------------------------------------------------------------------------
// libs/MatterSurfaceLib/include/cell_visitor.h
// ---------------------------------------------------------------------------
// Abstract visitor interfaces for the Cell/Cluster hierarchy (cell.h,
// cluster.h). Declaring them here keeps traversal out of Cell and Cluster
// themselves and lets a consumer add a pass without touching MatterSurfaceLib.
//
// Dispatch is deliberately shallow: Cell::accept() calls visit_cell(*this) and
// Cluster::accept() calls visit_cluster(*this) -- neither recurses. A visitor
// that wants a cluster's cells must walk them itself (e.g. via
// Cluster::get_cells_in_region()).
//
// Status: no concrete visitor ships in this repository today. Both interfaces
// exist for consumers to implement; treat them as an extension point rather
// than as something with an in-tree reference implementation to copy.
// ---------------------------------------------------------------------------

// Phase 4 (Step 4) of docs/superpowers/plans/2026-07-25-mathlib-and-raylib-removal.md:
// this header used to include raylib.h for Matrix. It is C++-only (no C
// consumer), so it uses matter_math.h's mm::Mat4 instead.
#include "matter_math.h"

// Forward declarations
struct Cell;
class Cluster;

// Abstract visitor interface for Cell and Cluster operations
//
// Implementations are supplied by the caller and must outlive the accept()
// call they are passed to; the visited Cell/Cluster is handed over as a const
// reference and is not retained by the visitor infrastructure. Both visit
// methods are pure, so a visitor that only cares about one level must still
// provide an empty override for the other.
class CellVisitor {
public:
    virtual ~CellVisitor() = default;

    // Visit methods for different types
    virtual void visit_cell(const Cell& cell) = 0;
    virtual void visit_cluster(const Cluster& cluster) = 0;
};

// Visitor for rendering cells with transformation support
//
// Adds the render-specific surface: a transformed visit (the Cell's own bounds
// are in cluster-local space, so a renderer needs the cluster's local->world
// matrix passed alongside) plus wireframe state. Note that
// visit_cell_transformed() is NOT reached through Cell::accept(), which only
// calls the untransformed visit_cell() -- a renderer drives this entry point
// itself.
class CellRenderVisitor : public CellVisitor {
public:
    virtual ~CellRenderVisitor() = default;

    // Additional methods specific to rendering
    virtual void visit_cell_transformed(const Cell& cell, const mm::Mat4& transform) = 0;
    virtual void set_wireframe_mode(bool wireframe) = 0;
    virtual bool get_wireframe_mode() const = 0;
};

#endif // CELL_VISITOR_H 