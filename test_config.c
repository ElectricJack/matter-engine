#include "include/surface.h"
#include "include/particle.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Simple test to demonstrate configuration options
int main() {
    printf("Testing SurfaceLib configuration options...\n\n");
    
    // Create a simple test particle setup
    const int particleCount = 100;
    Particle* particles = (Particle*)malloc(particleCount * sizeof(Particle));
    
    // Initialize particles in a simple pattern
    for (int i = 0; i < particleCount; i++) {
        particles[i].position.x = (float)(i % 10) * 0.5f;
        particles[i].position.y = (float)(i / 10) * 0.5f; 
        particles[i].position.z = 0.0f;
        particles[i].materialId = i % 3; // Three different materials
    }
    
    // Set up volume bounds
    Bounds volume;
    volume.center = (Vector3){2.5f, 2.5f, 0.0f};
    volume.size = (Vector3){6.0f, 6.0f, 3.0f};
    volume.divisionPow = 6; // 64^3 grid
    
    float particleRadius = 0.3f;
    
    // Test 1: Default configuration (memory reuse + edge deduplication enabled)
    printf("=== Test 1: Default Configuration ===\n");
    printf("Memory reuse: ENABLED, Edge deduplication: ENABLED\n");
    clock_t start = clock();
    Mesh mesh1 = GenerateMesh(particles, particleRadius, particleCount, volume);
    clock_t end = clock();
    double time1 = ((double)(end - start)) / CLOCKS_PER_SEC * 1000.0;
    printf("Vertices: %d, Triangles: %d, Time: %.3f ms\n\n", 
           mesh1.vertexCount, mesh1.triangleCount, time1);
    
    // Test 2: Memory reuse enabled, edge deduplication disabled
    printf("=== Test 2: Edge Deduplication Disabled ===\n");
    printf("Memory reuse: ENABLED, Edge deduplication: DISABLED\n");
    MeshGenerationConfig config2 = GetDefaultMeshConfig();
    config2.enableEdgeDeduplication = false;
    start = clock();
    Mesh mesh2 = GenerateMeshWithConfig(particles, particleRadius, particleCount, volume, config2);
    end = clock();
    double time2 = ((double)(end - start)) / CLOCKS_PER_SEC * 1000.0;
    printf("Vertices: %d, Triangles: %d, Time: %.3f ms\n\n", 
           mesh2.vertexCount, mesh2.triangleCount, time2);
    
    // Test 3: Both optimizations disabled
    printf("=== Test 3: All Optimizations Disabled ===\n");
    printf("Memory reuse: DISABLED, Edge deduplication: DISABLED\n");
    MeshGenerationConfig config3 = GetDefaultMeshConfig();
    config3.enableEdgeDeduplication = false;
    config3.enableMemoryReuse = false;
    start = clock();
    Mesh mesh3 = GenerateMeshWithConfig(particles, particleRadius, particleCount, volume, config3);
    end = clock();
    double time3 = ((double)(end - start)) / CLOCKS_PER_SEC * 1000.0;
    printf("Vertices: %d, Triangles: %d, Time: %.3f ms\n\n", 
           mesh3.vertexCount, mesh3.triangleCount, time3);
    
    // Performance comparison
    printf("=== Performance Comparison ===\n");
    printf("Default (optimized):     %.3f ms (%d vertices)\n", time1, mesh1.vertexCount);
    printf("No edge deduplication:   %.3f ms (%d vertices) [%.1fx vertices]\n", 
           time2, mesh2.vertexCount, (float)mesh2.vertexCount / mesh1.vertexCount);
    printf("No optimizations:        %.3f ms (%d vertices) [%.2fx slower]\n", 
           time3, mesh3.vertexCount, time3 / time1);
    
    // Clean up
    if (mesh1.vertices) free(mesh1.vertices);
    if (mesh1.normals) free(mesh1.normals);
    if (mesh1.indices) free(mesh1.indices);
    if (mesh1.colors) free(mesh1.colors);
    
    if (mesh2.vertices) free(mesh2.vertices);
    if (mesh2.normals) free(mesh2.normals);
    if (mesh2.indices) free(mesh2.indices);
    if (mesh2.colors) free(mesh2.colors);
    
    if (mesh3.vertices) free(mesh3.vertices);
    if (mesh3.normals) free(mesh3.normals);
    if (mesh3.indices) free(mesh3.indices);
    if (mesh3.colors) free(mesh3.colors);
    
    free(particles);
    
    // Clean up memory pool
    SurfaceLibCleanup();
    
    printf("\nConfiguration test completed!\n");
    return 0;
}