#ifndef CELL_VISITOR_H
#define CELL_VISITOR_H

#include "raylib.h"

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
    virtual void visit_cell_transformed(const Cell& cell, const Matrix& transform) = 0;
    virtual void set_wireframe_mode(bool wireframe) = 0;
    virtual bool get_wireframe_mode() const = 0;
};

#endif // CELL_VISITOR_H 