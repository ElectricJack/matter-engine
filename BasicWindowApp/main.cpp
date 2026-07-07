#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

extern "C" {
    GLFWwindow* glfwGetCurrentContext();
}

int main(void)
{
    // Initialization
    const int screenWidth = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "Raylib + ImGui - Rotating Cube");

    // Get GLFW window handle from raylib
    GLFWwindow* window = glfwGetCurrentContext();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Define the camera to look into our 3d world
    Camera camera = { 0 };
    camera.position = (Vector3){ 5.0f, 5.0f, 5.0f };
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    SetTargetFPS(60);               // Set our game to run at 60 frames-per-second
    
    // ImGui state
    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    float cube_rotation_speed = 0.4f;
    Vector3 cube_color = {1.0f, 0.0f, 0.0f}; // RGB for RED
    
    // Main game loop
    while (!WindowShouldClose())    // Detect window close button or ESC key
    {
        // Update
        UpdateCamera(&camera, CAMERA_ORBITAL);      // Update camera
        
        // Calculate rotation angle based on time
        float rotationY = GetTime() * cube_rotation_speed;

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        // 2. Show a simple window that we create ourselves. We use a Begin/End pair to created a named window.
        {
            static float f = 0.0f;
            static int counter = 0;

            ImGui::Begin("Cube Controls");                          // Create a window called "Cube Controls" and append into it.

            ImGui::Text("Control the rotating cube:");               // Display some text (you can use a format strings too)
            ImGui::Checkbox("Demo Window", &show_demo_window);      // Edit bools storing our window open/close state
            ImGui::Checkbox("Another Window", &show_another_window);

            ImGui::SliderFloat("Rotation Speed", &cube_rotation_speed, 0.0f, 2.0f);            // Edit 1 float using a slider from 0.0f to 2.0f
            ImGui::ColorEdit3("Cube Color", (float*)&cube_color); // Edit 3 floats representing a color

            if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
                counter++;
            ImGui::SameLine();
            ImGui::Text("counter = %d", counter);

            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
            ImGui::End();
        }

        // 3. Show another simple window.
        if (show_another_window)
        {
            ImGui::Begin("Another Window", &show_another_window);   // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
            ImGui::Text("Hello from another window!");
            if (ImGui::Button("Close Me"))
                show_another_window = false;
            ImGui::End();
        }

        // Draw
        BeginDrawing();
            ClearBackground(RAYWHITE);
            
            BeginMode3D(camera);
                // Draw rotating cube with imgui-controlled color
                Color cubeColor = {(unsigned char)(cube_color.x * 255), (unsigned char)(cube_color.y * 255), (unsigned char)(cube_color.z * 255), 255};

                // Rotate the model matrix around the cube before drawing
                rlPushMatrix();
                rlRotatef(rotationY * RAD2DEG, 0.0f, 1.0f, 0.0f);
                DrawCube((Vector3){0.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 2.0f, cubeColor);

                // Draw cube wireframe for better visualization
                DrawCubeWires((Vector3){0.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 2.0f, MAROON);
                rlPopMatrix();

                DrawGrid(10, 1.0f);        // Draw a grid
            EndMode3D();
            
            DrawFPS(10, 10);
            DrawText("Raylib + ImGui Example", 10, 40, 20, DARKGRAY);
            
            // Render ImGui
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            
        EndDrawing();
    }

    // ImGui Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // De-Initialization
    CloseWindow();        // Close window and OpenGL context

    return 0;
}