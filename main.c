#include "include/surface.h"
#include "tests.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <limits.h>

// Demo configuration
#define PARTICLE_COUNT     100
#define PARTICLE_RADIUS    2.0f // Increased to 10x the previous size
#define VOLUME_SIZE        32.0f // Increased to match larger particles
#define GRID_DIVISIONS_POW 5  // 2^4 = 16 divisions (reduced for debugging)
//#define TEST_MODE          true
#define TEST_MODE          false

// Global mesh variable that can be accessed by test functions
Mesh mesh;

// Add velocity to particles for movement
typedef struct {
    Vector3 velocity;
} ParticleVelocity;

// Generate random particles within a certain radius of the center
void GenerateRandomParticles(Particle* particles, ParticleVelocity* velocities, int count, float radius) {
    // Seed the random number generator
    srand((unsigned int)time(NULL));
    
    // Create particles with random positions and material IDs
    for (int i = 0; i < count; i++) {
        // Random position within a sphere
        float theta = 2.0f * PI * ((float)rand() / RAND_MAX);
        float phi = acosf(2.0f * ((float)rand() / RAND_MAX) - 1.0f);
        float r = radius * powf((float)rand() / RAND_MAX, 1.0f/3.0f);
        
        particles[i].position.x = r * sinf(phi) * cosf(theta);
        particles[i].position.y = r * sinf(phi) * sinf(theta);
        particles[i].position.z = r * cosf(phi);
        
        // Random material ID (0-9)
        particles[i].materialId = rand() % 10;
        
        // Random initial velocity (between -5 and 5 in each dimension)
        velocities[i].velocity.x = 20.0f * ((float)rand() / RAND_MAX - 0.5f);
        velocities[i].velocity.y = 10.0f * ((float)rand() / RAND_MAX - 0.5f);
        velocities[i].velocity.z = 20.0f * ((float)rand() / RAND_MAX - 0.5f);
    }
}

int main(int argc, char** argv) {
    // Initialization
    const int screenWidth  = 1280;
    const int screenHeight = 800;

    InitWindow(screenWidth, screenHeight, "SurfaceLib - Particle Isosurface Demo");

    // Define the camera to look into our 3d world
    Camera camera     = { 0 };
    camera.position   = (Vector3){ 16.0f, 16.0f, 16.0f };
    camera.target     = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy       = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // Create particles and prepare for test or random mode
    Particle* particles = (Particle*)malloc(PARTICLE_COUNT * sizeof(Particle));
    
    // Velocities for the particles
    ParticleVelocity* particleVelocities = (ParticleVelocity*)malloc(PARTICLE_COUNT * sizeof(ParticleVelocity));
    
    Bounds volume;
    int particleCount;


    // Random particles case
    GenerateRandomParticles(particles, particleVelocities, PARTICLE_COUNT, VOLUME_SIZE * 0.5f);
    
    // Define the volume bounds
    volume = (Bounds) {
        .center      = { 0.0f, 0.0f, 0.0f },
        .size        = { VOLUME_SIZE, VOLUME_SIZE, VOLUME_SIZE },
        .divisionPow = GRID_DIVISIONS_POW
    };
    
    // Use all particles
    particleCount = PARTICLE_COUNT;

    
    // Create material for rendering
    Material material = LoadMaterialDefault();
    
    // Display modes
    bool  wireframe      = true;      // Toggle for wireframe rendering
    bool  showParticles  = true;  // Toggle for showing particle spheres
    bool  showMeshBounds = true; // Toggle for showing mesh bounds
    bool  showTriangles  = true;  // Toggle for showing individual triangles
    
    
    // Mesh bounds
    Vector3 meshMin    = { 0 };
    Vector3 meshMax    = { 0 };
    Vector3 meshCenter = { 0 };
    Vector3 meshSize   = { 0 };
    
    
    SetTargetFPS(60);               // Set our game to run at 60 frames-per-second

    
    // Check for command line arguments
    if (argc > 1) {
        if (strcmp(argv[1], "run_tests") == 0) {
            // Run all tests
            return run_all_tests() ? 0 : 1;
        }
        if (strcmp(argv[1], "test_watertight") == 0) {
            // Run just the watertight test on the default test case
            Mesh testMesh = create_two_spheres_mesh();
            bool success = test_watertight_mesh(&testMesh);
            UnloadMesh(testMesh);
            return success ? 0 : 1;
        }
        if (strcmp(argv[1], "test_orientation") == 0) {
            // Run just the orientation test on the default test case
            Mesh testMesh = create_two_spheres_mesh();
            bool success = test_consistent_triangle_orientation(&testMesh);
            UnloadMesh(testMesh);
            return success ? 0 : 1;
        }
    }
    
    // Normal mode or TEST_MODE from define
    if (TEST_MODE) {
        run_all_tests();
    }
    else
    {
        Material defaultMat = LoadMaterialDefault();

        // Main game loop
        while (!WindowShouldClose()) {  // Detect window close button or ESC key
            UpdateCamera(&camera, CAMERA_ORBITAL);      // Update camera
            
            // Calculate delta time
            float deltaTime = GetFrameTime();
            
            // Update particle positions and handle bouncing
            for (int i = 0; i < particleCount; i++) {
                // Update position based on velocity
                particles[i].position.x += particleVelocities[i].velocity.x * deltaTime;
                particles[i].position.y += particleVelocities[i].velocity.y * deltaTime;
                particles[i].position.z += particleVelocities[i].velocity.z * deltaTime;
                
                // Calculate volume boundaries (half size from center)
                float boundX = volume.size.x * 0.5f - PARTICLE_RADIUS;
                float boundY = volume.size.y * 0.5f - PARTICLE_RADIUS;
                float boundZ = volume.size.z * 0.5f - PARTICLE_RADIUS;
                
                // Bounce off boundaries with slight damping (0.9)
                if (fabs(particles[i].position.x) > boundX) {
                    particles[i].position.x = (particles[i].position.x > 0) ? boundX : -boundX;
                    particleVelocities[i].velocity.x *= -0.9f;
                }
                
                if (fabs(particles[i].position.y) > boundY) {
                    particles[i].position.y = (particles[i].position.y > 0) ? boundY : -boundY;
                    particleVelocities[i].velocity.y *= -0.9f;
                }
                
                if (fabs(particles[i].position.z) > boundZ) {
                    particles[i].position.z = (particles[i].position.z > 0) ? boundZ : -boundZ;
                    particleVelocities[i].velocity.z *= -0.9f;
                }
            }
                
            // Regenerate the mesh with updated particle positions
            //if (mesh.vertices) UnloadMesh(mesh);
            mesh = GenerateMesh(particles, PARTICLE_RADIUS, particleCount, volume);

            
            // Upload mesh data to GPU (VAO)
            UploadMesh(&mesh, false);

            if (IsKeyPressed(KEY_SPACE)) { wireframe     = !wireframe;       }
            if (IsKeyPressed(KEY_P))     { showParticles = !showParticles;   }
            if (IsKeyPressed(KEY_B))     { showMeshBounds = !showMeshBounds; }
            if (IsKeyPressed(KEY_T))     { showTriangles = !showTriangles;   }
            
            // Draw
            BeginDrawing();
                ClearBackground(BLACK);
                
                BeginMode3D(camera);
                    // Draw the bounding box
                    DrawCubeWires((Vector3){ 0, 0, 0 }, VOLUME_SIZE, VOLUME_SIZE, VOLUME_SIZE, DARKGRAY);
                    
                    // Draw all particles as small spheres if enabled
                    if (showParticles) {
                        for (int i = 0; i < particleCount; i++) {
                            DrawSphere(particles[i].position, PARTICLE_RADIUS*0.1, GetMaterialColor(particles[i].materialId));
                        }
                    }
                    

                    
                    if (wireframe) {
                        // Use wireframe mode
                        rlEnableWireMode();
                        DrawMesh(mesh, defaultMat, MatrixIdentity());
                        rlDisableWireMode();
                    } else {
                        // Draw solid model with a bright color
                        DrawMesh(mesh, defaultMat, MatrixIdentity());
                    }
                    
                    
                    DrawGrid(10, 5.0f);        // Draw a grid
                    
                    // Draw mesh bounds if enabled
                    if (showMeshBounds) {
                        DrawCubeWires(meshCenter, meshSize.x, meshSize.y, meshSize.z, RED);
                        DrawCube(meshCenter, 0.5f, 0.5f, 0.5f, ORANGE); // Draw center point
                    }

                EndMode3D();
                
                // UI
                DrawFPS(10, 10);
                DrawText("SurfaceLib - Particle Isosurface Demo", 10, 40, 20, DARKGRAY);
                DrawText(TextFormat("Particles: %d", particleCount), 10, 70, 12, DARKGRAY);
                DrawText(TextFormat("Triangles: %d", mesh.triangleCount), 10, 90, 12, DARKGRAY);
                DrawText(TextFormat("Vertices: %d", mesh.vertexCount), 10, 110, 12, DARKGRAY);
                DrawText(TextFormat("Grid Resolution: %dx%dx%d", 1<<GRID_DIVISIONS_POW, 1<<GRID_DIVISIONS_POW, 1<<GRID_DIVISIONS_POW), 10, 130, 12, DARKGRAY);
                DrawText("Press SPACE to toggle wireframe mode", 10, 150, 12, DARKGRAY);
                DrawText("Press P to show/hide particles", 10, 170, 12, DARKGRAY);
                DrawText("Press B to show/hide mesh bounds", 10, 190, 12, DARKGRAY);
                DrawText("Press F to cycle through performance modes", 10, 210, 12, DARKGRAY);
                DrawText("Use mouse to orbit camera", 10, 230, 12, DARKGRAY);
                
                // Draw mesh bounds information
                DrawText(TextFormat("Mesh Min: (%.1f, %.1f, %.1f)", meshMin.x, meshMin.y, meshMin.z), 10, 250, 12, DARKGRAY);
                DrawText(TextFormat("Mesh Max: (%.1f, %.1f, %.1f)", meshMax.x, meshMax.y, meshMax.z), 10, 270, 12, DARKGRAY);
                DrawText(TextFormat("Mesh Center: (%.1f, %.1f, %.1f)", meshCenter.x, meshCenter.y, meshCenter.z), 10, 290, 12, DARKGRAY);
                
            EndDrawing();

            // Update the model with the new mesh
            UnloadMesh(mesh);
        }
    }
    


    // Cleanup
    free(particles);
    free(particleVelocities);
    //UnloadMesh(mesh);
    UnloadMaterial(material);

    CloseWindow();        // Close window and OpenGL context

    return 0;
}