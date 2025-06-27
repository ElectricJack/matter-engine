// Material Properties System
// Comprehensive material definition with PBR, emission, translucency, and surface properties

struct MaterialProperties
{
    // PBR properties
    vec3 albedo;           // Base color/diffuse reflectance
    float roughness;       // Surface roughness (0 = mirror, 1 = completely rough)
    float metallic;        // Metallic factor (0 = dielectric, 1 = metallic)
    
    // Emission properties
    float emission;        // Emission strength (0 = no emission, >0 = emissive)
    
    // Translucency and refraction
    float translucency;    // Translucency factor (0 = opaque, 1 = fully translucent)
    float ior;             // Index of refraction (1.0 = air, 1.5 = glass, etc.)
    
    // Surface properties
    bool flatShading;      // true = flat shaded, false = smooth normals
};

// Material lookup table - defines all materials used in the scene
MaterialProperties getMaterialProperties(int materialId)
{
    MaterialProperties mat;
    
    // Initialize defaults
    mat.albedo = vec3(0.5, 0.5, 0.5);
    mat.roughness = 0.5;
    mat.metallic = 0.0;
    mat.emission = 0.0;
    mat.translucency = 0.0;
    mat.ior = 1.0;

    // If the materialId is greater than 1M, then that's a flag to enable
    // smooth shading. Otherwise it's flat shading.
    int smooth_normals_offset = 1000000;
    mat.flatShading = true;
    if (materialId >= smooth_normals_offset) {
        materialId -= smooth_normals_offset;
        mat.flatShading = false;
    }
    
    // Material definitions based on scene setup
    if (materialId == 0) {
        // Red semi-metallic with slight emission
        mat.albedo = vec3(0.8, 0.2, 0.2);
        mat.roughness = 0.2;
        mat.metallic = 0.6;
        mat.emission = 0.1;  // Slight red glow
        mat.translucency = 0.0;
        mat.ior = 1.0;
    } 
    else if (materialId == 1) {
        // Blue diffuse sphere with smooth normals
        mat.albedo = vec3(0.2, 0.3, 0.8);
        mat.roughness = 0.7;
        mat.metallic = 0.1;
        mat.emission = 0.0;
        mat.translucency = 0.0;
        mat.ior = 1.0;
    } 
    else if (materialId == 2) {
        // Green diffuse ground with flat shading
        mat.albedo = vec3(0.3, 0.7, 0.3);
        mat.roughness = 0.9;
        mat.metallic = 0.0;
        mat.emission = 0.0;
        mat.translucency = 0.0;
        mat.ior = 1.0;
    } 
    else if (materialId == 3) {
        // Yellow/Gold metallic sphere with smooth normals
        mat.albedo = vec3(0.8, 0.7, 0.3);
        mat.roughness = 0.05;
        mat.metallic = 1.0;
        mat.emission = 0.0;
        mat.translucency = 0.0;
        mat.ior = 1.0;
    } 
    else if (materialId == 4) {
        // White translucent glass with smooth normals
        mat.albedo = vec3(0.9, 0.9, 0.9);
        mat.roughness = 0.02;
        mat.metallic = 0.0;
        mat.emission = 0.0;
        mat.translucency = 0.8; // Highly translucent
        mat.ior = 1.5; // Glass IOR
    }
    else if (materialId == 5) {
        // Bright emissive light source
        mat.albedo = vec3(1.0, 0.9, 0.7); // Warm white light
        mat.roughness = 1.0;
        mat.metallic = 0.0;
        mat.emission = 5.0; // Strong emission
        mat.translucency = 0.0;
        mat.ior = 1.0;
    }
    else if (materialId == 6) {
        // Colored glass - green tinted
        mat.albedo = vec3(0.2, 0.9, 0.3);
        mat.roughness = 0.01;
        mat.metallic = 0.0;
        mat.emission = 0.0;
        mat.translucency = 0.9;
        mat.ior = 1.52; // Crown glass IOR
    }
    else if (materialId == 7) {
        // Water-like material
        mat.albedo = vec3(0.2, 0.4, 0.8);
        mat.roughness = 0.05;
        mat.metallic = 0.0;
        mat.emission = 0.0;
        mat.translucency = 0.7;
        mat.ior = 1.33; // Water IOR
    }
    else {
        // Default gray metallic material
        mat.albedo = vec3(0.6, 0.6, 0.6);
        mat.roughness = 0.1;
        mat.metallic = 0.8;
        mat.emission = 0.0;
        mat.translucency = 0.0;
        mat.ior = 1.0;
    }
    
    return mat;
}

// Utility function to check if a material is emissive
bool isMaterialEmissive(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.emission > 0.0;
}

// Utility function to check if a material is translucent
bool isMaterialTranslucent(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.translucency > 0.0;
}

// Utility function to get emission color
vec3 getMaterialEmission(int materialId)
{
    MaterialProperties mat = getMaterialProperties(materialId);
    return mat.albedo * mat.emission;
}