#ifndef CELL_DEBUG_RENDERER_H
#define CELL_DEBUG_RENDERER_H

#include "cell_visitor.h"
#include "raylib.h"

// Forward declarations
extern "C" {
    Material LoadMaterialDefault(void);
    void DrawMesh(Mesh mesh, Material material, Matrix transform);
    void DrawLine3D(Vector3 startPos, Vector3 endPos, Color color);
    Matrix MatrixIdentity(void);
    Matrix MatrixTranslate(float x, float y, float z);
    void DrawCubeWires(Vector3 position, float width, float height, float length, Color color);
    void DrawSphere(Vector3 centerPos, float radius, Color color);
    Vector3 Vector3Add(Vector3 v1, Vector3 v2);
    Vector3 Vector3Subtract(Vector3 v1, Vector3 v2);
    Vector3 Vector3Transform(Vector3 v, Matrix mat);
    float Vector3Length(Vector3 v);
}

class CellDebugRenderer : public CellRenderVisitor {
public:
    CellDebugRenderer();
    ~CellDebugRenderer() = default;
    
    // CellVisitor interface
    void visit_cell(const Cell& cell) override;
    void visit_cluster(const Cluster& cluster) override;
    
    // CellRenderVisitor interface
    void visit_cell_transformed(const Cell& cell, const Matrix& transform) override;
    void set_wireframe_mode(bool wireframe) override { wireframe_mode_ = wireframe; }
    bool get_wireframe_mode() const override { return wireframe_mode_; }
    
    // Debug rendering options
    void set_show_bounds(bool show_bounds) { show_bounds_ = show_bounds; }
    bool get_show_bounds() const { return show_bounds_; }
    
    void set_show_meshes(bool show_meshes) { show_meshes_ = show_meshes; }
    bool get_show_meshes() const { return show_meshes_; }
    
    // Specialized methods for cluster debug rendering
    void render_cluster_debug_bounds(const Cluster& cluster) const;

private:
    bool wireframe_mode_;
    bool show_bounds_;
    bool show_meshes_;
    mutable Material default_material_;
    mutable bool material_initialized_;
    
    // Helper methods for rendering
    void render_cell_meshes(const Cell& cell, const Matrix& transform) const;
    void render_cell_wireframe(const Cell& cell, const Matrix& transform) const;
    void render_cell_debug_bounds(const Cell& cell) const;
    void ensure_material_initialized() const;
};

#endif // CELL_DEBUG_RENDERER_H 