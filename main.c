#include "include/open_particle_surface.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

// Demo configuration
#define PARTICLE_COUNT         1000000  // Target 1 million particles
#define PARTICLE_RADIUS        5.0f     // Particle radius
#define VOLUME_SIZE            100.0f   // Overall volume size
#define INITIAL_PARTICLE_COUNT 10000    // Start with fewer particles and grow
#define MAX_DIRTY_UPDATES      3        // Maximum dirty bound updates per frame

// Generate noise-based particles that form weblike structures
void GenerateWeblikeParticles(int count) {
    // Seed the random number generator
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
    
    // Track how many particles we successfully create
    int addedCount = 0;
    
    // Generate particles along strands or clusters
    for (int i = 0; i < count && addedCount < count; i++) {
        // Choose a random starting point for a strand
        Vector3 strandCenter = {
            ((float)rand() / RAND_MAX - 0.5f) * VOLUME_SIZE * 0.8f,
            ((float)rand() / RAND_MAX - 0.5f) * VOLUME_SIZE * 0.8f,
            ((float)rand() / RAND_MAX - 0.5f) * VOLUME_SIZE * 0.8f
        };
        
        // Determine strand direction
        Vector3 strandDir = {
            ((float)rand() / RAND_MAX - 0.5f) * 2.0f,
            ((float)rand() / RAND_MAX - 0.5f) * 2.0f,
            ((float)rand() / RAND_MAX - 0.5f) * 2.0f
        };
        
        // Normalize the direction
        float magnitude = sqrtf(strandDir.x*strandDir.x + strandDir.y*strandDir.y + strandDir.z*strandDir.z);
        if (magnitude > 0) {
            strandDir.x /= magnitude;
            strandDir.y /= magnitude;
            strandDir.z /= magnitude;
        }
        
        // Create a random cluster of particles along this strand
        int clusterSize = 100 + rand() % 150; // 100-250 particles per cluster
        for (int j = 0; j < clusterSize && addedCount < count; j++) {
            // Position along the strand with some randomness
            float t = ((float)rand() / RAND_MAX) * 15.0f; // Position along strand
            
            // Calculate position with some noise
            Vector3 offset = {
                ((float)rand() / RAND_MAX - 0.5f) * 3.0f,
                ((float)rand() / RAND_MAX - 0.5f) * 3.0f,
                ((float)rand() / RAND_MAX - 0.5f) * 3.0f
            };
            
            // Final position
            Vector3 position = {
                strandCenter.x + strandDir.x * t + offset.x,
                strandCenter.y + strandDir.y * t + offset.y,
                strandCenter.z + strandDir.z * t + offset.z
            };
            
            // Create the particle in the system - we don't care where it goes internally
            int materialId = rand() % 10; // Random material
            ParticleHandle handle = CreateParticle(position, materialId);
            
            if (handle != -1) {
                addedCount++;
            }
        }
    }
    
    printf("Added %d particles, total: %d\n", addedCount, GetParticleCount());
}

// Add a chunk of new particles at a specified world space location
void AddParticlesAtLocation(Vector3 location, int count) {
    int addedCount = 0;
    
    // Generate particles in a spherical cluster around the location
    for (int i = 0; i < count && addedCount < count; i++) {
        // Generate a random offset in a sphere
        float radius = 5.0f * powf((float)rand() / RAND_MAX, 1.0f/3.0f);
        float theta = 2.0f * PI * ((float)rand() / RAND_MAX);
        float phi = acosf(2.0f * ((float)rand() / RAND_MAX) - 1.0f);
        
        // Calculate position
        Vector3 position = {
            location.x + radius * sinf(phi) * cosf(theta),
            location.y + radius * sinf(phi) * sinf(theta),
            location.z + radius * cosf(phi)
        };
        
        // Create the particle in the system
        int materialId = rand() % 10; // Random material
        ParticleHandle handle = CreateParticle(position, materialId);
        
        if (handle != -1) {
            addedCount++;
        }
    }
    
    printf("Added %d particles at location, total: %d\n", addedCount, GetParticleCount());
}

int main(void) {
    // Initialization
    const int screenWidth = 1280;
    const int screenHeight = 800;

    printf("Starting OpenParticleSurfaceLib - Dynamic Particle Mesh Demo\n");
    InitWindow(screenWidth, screenHeight, "OpenParticleSurfaceLib - Dynamic Particle Mesh Demo");
    printf("Window initialized successfully\n");

    // Define the camera to look into our 3d world
    Camera camera = { 0 };
    camera.position = (Vector3){ 0.0f, 10.0f, 30.0f }; // Position camera closer for debug
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    printf("Camera position: (%.2f, %.2f, %.2f)\n", camera.position.x, camera.position.y, camera.position.z);

    // Initialize particle system
    InitializeParticleSystem(PARTICLE_COUNT, PARTICLE_RADIUS);
    
    // Generate initial particles - start with just a few for debugging
    GenerateWeblikeParticles(10); // Small batch for debugging
    
    // Display modes
    bool wireframe = true;     // Toggle for wireframe rendering
    bool showParticles = false; // Toggle for showing particle positions
    bool showBounds = false;    // Toggle for showing spatial hash bounds
    bool useInstancing = true;  // Toggle for using instanced rendering
    
    // FPS counter variables
    float timeSinceLastParticleAddition = 0.0f;
    
    SetTargetFPS(60);               // Set target to run at 60 frames-per-second
    
    // Main game loop
    // Skip to the next frame immediately for debugging
    static int frameCount = 0;
    while (!WindowShouldClose()) {
        frameCount++;
        // if (frameCount > 5) {
        //     printf("Exiting after 5 frames for debugging\n");
        //     break;
        // }
        // Update
        // Only update camera rotation, not position
        if (IsKeyDown(KEY_LEFT_ALT)) {
            UpdateCamera(&camera, CAMERA_ORBITAL);
        }
        
        // Calculate delta time
        float deltaTime = GetFrameTime();
        
        // Add more particles gradually
        timeSinceLastParticleAddition += deltaTime;
        if (timeSinceLastParticleAddition > 5.0f && GetParticleCount() < PARTICLE_COUNT) {
            // For debugging, only add one small batch
            static bool addedExtraParticles = false;
            if (!addedExtraParticles) {
                printf("Adding additional test particles\n");
                GenerateWeblikeParticles(5); // Add just a few more for testing
                addedExtraParticles = true; // Only add once for debugging
            }
            timeSinceLastParticleAddition = 0.0f;
        }
        
        // Update particle system (regenerate meshes for dirty regions)
        int updatedCells = UpdateParticleSystem(MAX_DIRTY_UPDATES);
        
        // Check for key presses
        if (IsKeyPressed(KEY_SPACE)) wireframe = !wireframe;
        if (IsKeyPressed(KEY_P)) showParticles = !showParticles;
        if (IsKeyPressed(KEY_B)) showBounds = !showBounds;
        if (IsKeyPressed(KEY_I)) useInstancing = !useInstancing;
        
        // Camera control with keys
        if (IsKeyDown(KEY_W)) camera.position.z -= 2.0f;
        if (IsKeyDown(KEY_S)) camera.position.z += 2.0f;
        if (IsKeyDown(KEY_A)) camera.position.x -= 2.0f;
        if (IsKeyDown(KEY_D)) camera.position.x += 2.0f;
        if (IsKeyDown(KEY_Q)) camera.position.y -= 2.0f;
        if (IsKeyDown(KEY_E)) camera.position.y += 2.0f;
        if (IsKeyPressed(KEY_R)) {
            // Reset camera to initial position
            camera.position = (Vector3){ 0.0f, 10.0f, 30.0f };
            camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
        }
        
        // Add particles at mouse click location
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Ray ray = GetMouseRay(GetMousePosition(), camera);
            
            // Calculate intersection with a plane at y=0
            float t = -ray.position.y / ray.direction.y;
            if (t > 0) {
                Vector3 hitPoint = {
                    ray.position.x + ray.direction.x * t,
                    ray.position.y + ray.direction.y * t,
                    ray.position.z + ray.direction.z * t
                };
                
                // Add particles at hit location
                AddParticlesAtLocation(hitPoint, 500);
            }
        }

        // Draw
        BeginDrawing();
            ClearBackground(BLACK);
            
            BeginMode3D(camera);
                // Draw the main volume bounds
                DrawCubeWires((Vector3){ 0, 0, 0 }, VOLUME_SIZE, VOLUME_SIZE, VOLUME_SIZE, DARKGRAY);
                
                // Draw particles if enabled
                if (showParticles) {
                    DrawParticles(useInstancing, 50000);
                }
                
                // Create default material for mesh rendering
                static Material defaultMaterial;
                static bool materialInitialized = false;
                
                if (!materialInitialized) {
                    defaultMaterial = LoadMaterialDefault();
                    materialInitialized = true;
                }
                
                // Draw particle meshes
                DrawParticleMeshes(defaultMaterial, wireframe);
                
                // Draw debug visualization if enabled
                DrawParticleSystemDebug(showBounds);
                
                DrawGrid(10, 10.0f);        // Draw a grid
            EndMode3D();
            
            // UI
            DrawFPS(10, 10);
            DrawText("OpenParticleSurfaceLib - Dynamic Particle Mesh Demo", 10, 40, 20, WHITE);
            DrawText(TextFormat("Particles: %d / %d", GetParticleCount(), GetParticleCapacity()), 10, 70, 12, WHITE);
            
            // Get system stats
            int activeCellCount = 0;
            int dirtyRegionCount = 0;
            int meshVertexCount = 0;
            GetParticleSystemStats(&activeCellCount, &dirtyRegionCount, &meshVertexCount);
            
            DrawText(TextFormat("Active Cells: %d, Dirty: %d", activeCellCount, dirtyRegionCount), 10, 90, 12, WHITE);
            DrawText(TextFormat("Total Mesh Vertices: %d", meshVertexCount), 10, 110, 12, WHITE);
            DrawText(TextFormat("Max Dirty Updates Per Frame: %d", MAX_DIRTY_UPDATES), 10, 130, 12, WHITE);
            DrawText(TextFormat("FPS: %d", GetFPS()), 10, 150, 12, WHITE);
            DrawText("Press SPACE to toggle wireframe mode", 10, 160, 12, WHITE);
            DrawText("Press P to show/hide particles", 10, 180, 12, WHITE);
            DrawText("Press I to toggle instanced rendering", 10, 200, 12, WHITE);
            DrawText("Press B to show/hide spatial hash bounds", 10, 220, 12, WHITE);
            DrawText("Click to add particles at location", 10, 240, 12, WHITE);
            DrawText("Hold ALT + mouse to orbit camera", 10, 260, 12, WHITE);
            DrawText("WASD keys to move camera, QE for up/down, R to reset", 10, 280, 12, WHITE);
            
            if (showParticles) {
                DrawText(TextFormat("Instanced Rendering: %s", useInstancing ? "ON" : "OFF"), 10, 300, 12, WHITE);
            }
            
        EndDrawing();
    }

    // Cleanup
    ShutdownParticleSystem();
    CloseWindow();        // Close window and OpenGL context

    return 0;
}