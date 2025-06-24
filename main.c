#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "include/scene.h"
#include "include/bvh.h"

int main(void)
{
    const int screenWidth = 800;
    const int screenHeight = 600;

    InitWindow(screenWidth, screenHeight, "GPU Ray Trace Example");
    SetTargetFPS(60);

    // Create test scene
    Scene* scene = scene_create_test_scene();
    if (!scene) {
        printf("Failed to create scene\n");
        CloseWindow();
        return 1;
    }
    
    printf("Scene created with %d triangles and %d BVH nodes\n", 
           scene->triangle_count, scene->bvh->node_count);
    
    // Flatten BVH for GPU upload
    BVHNode* gpu_nodes = NULL;
    int* gpu_indices = NULL;
    int gpu_node_count = 0;
    int gpu_index_count = 0;
    
    bvh_flatten_for_gpu(scene->bvh, &gpu_nodes, &gpu_indices, &gpu_node_count, &gpu_index_count);
    printf("BVH flattened: %d nodes, %d triangle indices\n", gpu_node_count, gpu_index_count);
    
    // Create GPU textures for BVH data
    Texture2D bvhNodesTexture = {0};
    Texture2D trianglesTexture = {0};
    
    if (gpu_nodes && scene->triangles) {
        // Pack BVH nodes into texture (each node = 3 rows of RGBA32F)
        // Row 0: aabbMin.xyz + isLeaf flag
        // Row 1: aabbMax.xyz + childOrTriangleStart  
        // Row 2: triangleCount + padding
        int nodeTextureWidth = gpu_node_count;
        int nodeTextureHeight = 3;
        float* nodeTextureData = (float*)malloc(nodeTextureWidth * nodeTextureHeight * 4 * sizeof(float));
        
        for (int i = 0; i < gpu_node_count; i++) {
            BVHNode* node = &gpu_nodes[i];
            int baseIdx = i * 4;
            
            // Row 0: aabbMin + isLeaf
            nodeTextureData[baseIdx + 0] = node->aabb.min.x;
            nodeTextureData[baseIdx + 1] = node->aabb.min.y;
            nodeTextureData[baseIdx + 2] = node->aabb.min.z;
            nodeTextureData[baseIdx + 3] = node->is_leaf ? 1.0f : 0.0f;
            
            // Row 1: aabbMax + childOrTriangleStart
            int row1Idx = nodeTextureWidth * 4 + baseIdx;
            nodeTextureData[row1Idx + 0] = node->aabb.max.x;
            nodeTextureData[row1Idx + 1] = node->aabb.max.y;
            nodeTextureData[row1Idx + 2] = node->aabb.max.z;
            nodeTextureData[row1Idx + 3] = (float)(node->is_leaf ? node->leaf.triangle_start : node->internal.left_child);
            
            // Row 2: triangleCount + padding
            int row2Idx = nodeTextureWidth * 8 + baseIdx;
            nodeTextureData[row2Idx + 0] = (float)(node->is_leaf ? node->leaf.triangle_count : 0);
            nodeTextureData[row2Idx + 1] = 0.0f;
            nodeTextureData[row2Idx + 2] = 0.0f;
            nodeTextureData[row2Idx + 3] = 0.0f;
        }
        
        // Create BVH nodes texture
        Image nodeImage = {
            .data = nodeTextureData,
            .width = nodeTextureWidth,
            .height = nodeTextureHeight,
            .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,
            .mipmaps = 1
        };
        bvhNodesTexture = LoadTextureFromImage(nodeImage);
        SetTextureFilter(bvhNodesTexture, TEXTURE_FILTER_POINT); // No filtering for data texture
        
        // Pack triangle data into texture (each triangle = 4 rows of RGBA32F)
        // Row 0: v0.xyz + materialId
        // Row 1: v1.xyz + padding
        // Row 2: v2.xyz + padding
        // Row 3: normal.xyz + padding
        int triTextureWidth = scene->triangle_count;
        int triTextureHeight = 4;
        float* triTextureData = (float*)malloc(triTextureWidth * triTextureHeight * 4 * sizeof(float));
        
        for (int i = 0; i < scene->triangle_count; i++) {
            Triangle* tri = &scene->triangles[i];
            int baseIdx = i * 4;
            
            // Row 0: v0 + materialId
            triTextureData[baseIdx + 0] = tri->v0.x;
            triTextureData[baseIdx + 1] = tri->v0.y;
            triTextureData[baseIdx + 2] = tri->v0.z;
            triTextureData[baseIdx + 3] = (float)tri->material_id;
            
            // Row 1: v1
            int row1Idx = triTextureWidth * 4 + baseIdx;
            triTextureData[row1Idx + 0] = tri->v1.x;
            triTextureData[row1Idx + 1] = tri->v1.y;
            triTextureData[row1Idx + 2] = tri->v1.z;
            triTextureData[row1Idx + 3] = 0.0f;
            
            // Row 2: v2
            int row2Idx = triTextureWidth * 8 + baseIdx;
            triTextureData[row2Idx + 0] = tri->v2.x;
            triTextureData[row2Idx + 1] = tri->v2.y;
            triTextureData[row2Idx + 2] = tri->v2.z;
            triTextureData[row2Idx + 3] = 0.0f;
            
            // Row 3: normal
            int row3Idx = triTextureWidth * 12 + baseIdx;
            triTextureData[row3Idx + 0] = tri->normal.x;
            triTextureData[row3Idx + 1] = tri->normal.y;
            triTextureData[row3Idx + 2] = tri->normal.z;
            triTextureData[row3Idx + 3] = 0.0f;
        }
        
        // Create triangle texture
        Image triImage = {
            .data = triTextureData,
            .width = triTextureWidth,
            .height = triTextureHeight,
            .format = PIXELFORMAT_UNCOMPRESSED_R32G32B32A32,
            .mipmaps = 1
        };
        trianglesTexture = LoadTextureFromImage(triImage);
        SetTextureFilter(trianglesTexture, TEXTURE_FILTER_POINT);
        
        free(nodeTextureData);
        free(triTextureData);
        
        printf("BVH textures created: nodes %dx%d, triangles %dx%d\n", 
               nodeTextureWidth, nodeTextureHeight, triTextureWidth, triTextureHeight);
    }

    // Load unified raytracing shader
    Shader raytraceShader = LoadShader(NULL, "shaders/raytrace_unified.fs");
    
    if (raytraceShader.id == 0) {
        printf("Failed to load unified raytracing shader, falling back to rasterization\n");
    } else {
        printf("Unified raytracing shader loaded successfully (ID: %d)\n", raytraceShader.id);
    }

    // Get shader uniform locations
    int cameraPosLoc     = GetShaderLocation(raytraceShader, "cameraPos");
    int cameraTargetLoc  = GetShaderLocation(raytraceShader, "cameraTarget");
    int cameraUpLoc      = GetShaderLocation(raytraceShader, "cameraUp");
    int cameraFovyLoc    = GetShaderLocation(raytraceShader, "cameraFovy");
    int screenSizeLoc    = GetShaderLocation(raytraceShader, "screenSize");
    int nodeCountLoc        = GetShaderLocation(raytraceShader, "nodeCount");
    int triangleCountLoc    = GetShaderLocation(raytraceShader, "triangleCount");
    int debugModeLoc        = GetShaderLocation(raytraceShader, "debugMode");
    int intersectionModeLoc = GetShaderLocation(raytraceShader, "intersectionMode");
    
    // Texture locations (unified shader always supports both)
    int bvhNodesTextureLoc  = GetShaderLocation(raytraceShader, "bvhNodesTexture");
    int trianglesTextureLoc = GetShaderLocation(raytraceShader, "trianglesTexture");
    
    // Debug uniform locations
    printf("Uniform locations: debugMode=%d, screenSize=%d, cameraPos=%d\n", 
           debugModeLoc, screenSizeLoc, cameraPosLoc);

    // Debug and rendering state
    int debugMode = 0;        // 0=ray dirs, 1=uv coords, 2=ray dirs alt, 3=hit test, 4=distance, 5=full raytracing, 6=BVH traversal
    int intersectionMode = 1; // 0=brute force, 1=BVH (default to BVH for performance)

    // Define camera
    Camera camera     = { 0 };
    camera.position   = (Vector3){ 3.0f, 2.0f, 5.0f };
    camera.target     = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up         = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy       = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // No render texture needed for simple approach

    bool useRaytracing = (raytraceShader.id != 0);
    
    // Main game loop
    while (!WindowShouldClose())
    {
        // Toggle between raytracing and rasterization
        if (IsKeyPressed(KEY_SPACE)) {
            useRaytracing = !useRaytracing && (raytraceShader.id != 0);
        }
        
        // Debug mode controls (only when raytracing is active)
        if (useRaytracing && raytraceShader.id != 0) {
            if (IsKeyPressed(KEY_ONE)) debugMode = 0;   // Ray directions
            if (IsKeyPressed(KEY_TWO)) debugMode = 1;   // UV coordinates
            if (IsKeyPressed(KEY_THREE)) debugMode = 2; // Ray directions alt
            if (IsKeyPressed(KEY_FOUR)) debugMode = 3;  // Hit testing
            if (IsKeyPressed(KEY_FIVE)) debugMode = 4;  // Distance visualization
            if (IsKeyPressed(KEY_SIX)) debugMode = 5;   // Full raytracing
            if (IsKeyPressed(KEY_SEVEN)) debugMode = 6; // BVH traversal (same as 5, but explicit)
            
            // Intersection mode controls
            if (IsKeyPressed(KEY_B)) {
                intersectionMode = (intersectionMode == 0) ? 1 : 0;
                printf("Intersection mode: %s\n", intersectionMode == 0 ? "Brute Force" : "BVH");
            }
        }
        
        // Update camera
        UpdateCamera(&camera, CAMERA_FREE);

        // Draw
        BeginDrawing();
            ClearBackground(BLACK);
            
            if (useRaytracing && raytraceShader.id != 0) {
                // Raytracing mode - use shader for fullscreen quad
                BeginShaderMode(raytraceShader);
                
                // Set shader uniforms
                SetShaderValue(raytraceShader, cameraPosLoc, &camera.position, SHADER_UNIFORM_VEC3);
                SetShaderValue(raytraceShader, cameraTargetLoc, &camera.target, SHADER_UNIFORM_VEC3);
                SetShaderValue(raytraceShader, cameraUpLoc, &camera.up, SHADER_UNIFORM_VEC3);
                SetShaderValue(raytraceShader, cameraFovyLoc, &camera.fovy, SHADER_UNIFORM_FLOAT);
                Vector2 screenSize = {(float)screenWidth, (float)screenHeight};
                SetShaderValue(raytraceShader, screenSizeLoc, &screenSize, SHADER_UNIFORM_VEC2);
                SetShaderValue(raytraceShader, nodeCountLoc, &scene->bvh->node_count, SHADER_UNIFORM_INT);
                SetShaderValue(raytraceShader, triangleCountLoc, &scene->triangle_count, SHADER_UNIFORM_INT);
                SetShaderValue(raytraceShader, debugModeLoc, &debugMode, SHADER_UNIFORM_INT);
                SetShaderValue(raytraceShader, intersectionModeLoc, &intersectionMode, SHADER_UNIFORM_INT);
                
                // Bind triangle texture (always available)
                if (trianglesTexture.id != 0 && trianglesTextureLoc != -1) {
                    SetShaderValueTexture(raytraceShader, trianglesTextureLoc, trianglesTexture);
                }
                
                // Bind BVH nodes texture (available when BVH textures were created)
                if (bvhNodesTexture.id != 0 && bvhNodesTextureLoc != -1) {
                    SetShaderValueTexture(raytraceShader, bvhNodesTextureLoc, bvhNodesTexture);
                }
                
                // Draw fullscreen rectangle
                DrawRectangle(0, 0, screenWidth, screenHeight, WHITE);
                
                EndShaderMode();
                
                // UI overlay
                DrawText("RAYTRACING MODE", 10, 40, 20, GREEN);
                DrawText("Press SPACE to toggle rasterization", 10, 70, 16, LIGHTGRAY);
                
                // Debug mode info
                const char* debugModeNames[] = {
                    "Ray Directions", "UV Coordinates", "Ray Directions Alt", 
                    "Triangle Intersection", "Distance Visualization", "Full Raytracing", "BVH Traversal"
                };
                DrawText(TextFormat("Debug Mode %d: %s", debugMode, debugModeNames[debugMode]), 
                         10, 100, 16, YELLOW);
                DrawText("Press 1-7 to change debug mode", 10, 120, 14, LIGHTGRAY);
                
                // Show intersection mode
                const char* intersectionModeText = intersectionMode == 0 ? "Brute Force" : "BVH";
                DrawText(TextFormat("Intersection: %s (Press B to toggle)", intersectionModeText), 
                         10, 140, 14, SKYBLUE);
            } else {
                // Rasterization mode
                BeginMode3D(camera);
                    // Draw scene using traditional rasterization
                    DrawCube((Vector3){-2.0f, 0.0f, 0.0f}, 1.0f, 1.0f, 1.0f, RED);
                    DrawCube((Vector3){2.0f, 0.0f, 0.0f}, 1.0f, 1.0f, 1.0f, BLUE);
                    DrawPlane((Vector3){0.0f, -2.0f, 0.0f}, (Vector2){4.0f, 4.0f}, GREEN);
                    DrawSphere((Vector3){0.0f, 1.0f, 1.0f}, 0.5f, YELLOW);
                    DrawSphere((Vector3){0.0f, 1.0f, -1.0f}, 0.3f, MAGENTA);
                    DrawGrid(10, 1.0f);
                EndMode3D();
                
                // UI overlay
                DrawText("RASTERIZATION MODE", 10, 40, 20, YELLOW);
                if (raytraceShader.id != 0) {
                    DrawText("Press SPACE to toggle raytracing", 10, 70, 16, LIGHTGRAY);
                } else {
                    DrawText("Raytracing shader failed to load", 10, 70, 16, RED);
                }
            }
            
            DrawFPS(10, 10);
            DrawText("Use WASD to move, mouse to look around", 10, screenHeight - 30, 16, LIGHTGRAY);
            
        EndDrawing();
    }

    // Cleanup
    if (raytraceShader.id != 0) UnloadShader(raytraceShader);
    if (bvhNodesTexture.id != 0) UnloadTexture(bvhNodesTexture);
    if (trianglesTexture.id != 0) UnloadTexture(trianglesTexture);
    if (gpu_nodes) free(gpu_nodes);
    if (gpu_indices) free(gpu_indices);
    scene_destroy(scene);
    CloseWindow();
    return 0;
}