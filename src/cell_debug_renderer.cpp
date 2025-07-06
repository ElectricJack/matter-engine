#include "../include/cell_debug_renderer.h"
#include "../include/cell.h"
#include "../include/cluster.h"

CellDebugRenderer::CellDebugRenderer()
    : wireframe_mode_(false),
      show_bounds_(true),
      show_meshes_(true),
      default_material_{0},
      material_initialized_(false) {
}

void CellDebugRenderer::visit_cell(const Cell& cell) {
    visit_cell_transformed(cell, MatrixIdentity());
}

void CellDebugRenderer::visit_cluster(const Cluster& cluster) {
    if (show_meshes_) {
        // Use the cluster's visit_cells method to render all cells with proper transforms
        cluster.visit_cells(*this);
    }
}

void CellDebugRenderer::visit_cell_transformed(const Cell& cell, const Matrix& transform) {
    if (show_meshes_ && cell.has_meshes && !cell.material_meshes.empty()) {
        if (wireframe_mode_) {
            render_cell_wireframe(cell, transform);
        } else {
            render_cell_meshes(cell, transform);
        }
    }
    
    if (show_bounds_) {
        render_cell_debug_bounds(cell);
    }
}

void CellDebugRenderer::render_cell_meshes(const Cell& cell, const Matrix& transform) const {
    ensure_material_initialized();
    
    // Draw solid meshes normally
    for (const auto& mesh_entry : cell.material_meshes) {
        const Mesh& mesh = mesh_entry.second;
        if (mesh.vertexCount > 0) {
            DrawMesh(mesh, default_material_, transform);
        }
    }
}

void CellDebugRenderer::render_cell_wireframe(const Cell& cell, const Matrix& transform) const {
    // Render wireframe by drawing mesh triangles as lines
    for (const auto& mesh_entry : cell.material_meshes) {
        const Mesh& mesh = mesh_entry.second;
        if (mesh.vertexCount > 0 && mesh.triangleCount > 0 && mesh.vertices && mesh.indices) {
            // Draw wireframe triangles
            for (int i = 0; i < mesh.triangleCount; i++) {
                // Get the three vertices of the triangle
                int idx0 = mesh.indices[i * 3 + 0];
                int idx1 = mesh.indices[i * 3 + 1];
                int idx2 = mesh.indices[i * 3 + 2];
                
                // Get vertex positions
                Vector3 v0 = {
                    mesh.vertices[idx0 * 3 + 0],
                    mesh.vertices[idx0 * 3 + 1],
                    mesh.vertices[idx0 * 3 + 2]
                };
                Vector3 v1 = {
                    mesh.vertices[idx1 * 3 + 0],
                    mesh.vertices[idx1 * 3 + 1],
                    mesh.vertices[idx1 * 3 + 2]
                };
                Vector3 v2 = {
                    mesh.vertices[idx2 * 3 + 0],
                    mesh.vertices[idx2 * 3 + 1],
                    mesh.vertices[idx2 * 3 + 2]
                };
                
                // Transform vertices by the transform matrix
                v0 = Vector3Transform(v0, transform);
                v1 = Vector3Transform(v1, transform);
                v2 = Vector3Transform(v2, transform);
                
                // Draw the three edges of the triangle
                DrawLine3D(v0, v1, WHITE);
                DrawLine3D(v1, v2, WHITE);
                DrawLine3D(v2, v0, WHITE);
            }
        }
    }
}

void CellDebugRenderer::render_cell_debug_bounds(const Cell& cell) const {
    // Draw wireframe cube for cell bounds
    Vector3 size = Vector3Subtract(cell.max_bound, cell.min_bound);
    Color color = cell.is_dirty ? RED : (cell.has_meshes ? GREEN : GRAY);
    
    DrawCubeWires(cell.center, size.x, size.y, size.z, color);
    
    // Draw a small sphere at the center
    DrawSphere(cell.center, 0.1f, color);
}

void CellDebugRenderer::render_cluster_debug_bounds(const Cluster& cluster) const {
    // Temporarily set bounds rendering to true for this call
    bool original_show_bounds = show_bounds_;
    const_cast<CellDebugRenderer*>(this)->show_bounds_ = true;
    
    // Visit all cells to render their debug bounds
    cluster.visit_all_cells(const_cast<CellDebugRenderer&>(*this));
    
    // Restore original bounds setting
    const_cast<CellDebugRenderer*>(this)->show_bounds_ = original_show_bounds;
}

void CellDebugRenderer::ensure_material_initialized() const {
    if (!material_initialized_) {
        default_material_ = LoadMaterialDefault();
        material_initialized_ = true;
    }
} 