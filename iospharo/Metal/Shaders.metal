/*
 * Shaders.metal
 *
 * Metal shaders for rendering the Pharo display.
 * Renders the VM framebuffer as a textured full-screen quad.
 */

#include <metal_stdlib>
using namespace metal;

// Vertex output structure
struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
};

// Full-screen quad vertex shader
// Generates a full-screen quad from vertex IDs 0-3
vertex VertexOut vertexShader(uint vertexID [[vertex_id]]) {
    // Quad positions in normalized device coordinates
    float2 positions[] = {
        float2(-1.0, -1.0),  // Bottom-left
        float2( 1.0, -1.0),  // Bottom-right
        float2(-1.0,  1.0),  // Top-left
        float2( 1.0,  1.0)   // Top-right
    };

    // Texture coordinates (flipped Y for correct orientation)
    float2 texCoords[] = {
        float2(0.0, 1.0),  // Bottom-left
        float2(1.0, 1.0),  // Bottom-right
        float2(0.0, 0.0),  // Top-left
        float2(1.0, 0.0)   // Top-right
    };

    VertexOut out;
    out.position = float4(positions[vertexID], 0.0, 1.0);
    out.texCoord = texCoords[vertexID];

    return out;
}

// Fragment shader - samples the display texture
fragment float4 fragmentShader(VertexOut in [[stage_in]],
                               texture2d<float> displayTexture [[texture(0)]]) {
    // Use nearest-neighbor sampling for pixel-perfect rendering
    constexpr sampler textureSampler(mag_filter::nearest,
                                     min_filter::nearest,
                                     address::clamp_to_edge);

    // Sample the display texture
    float4 color = displayTexture.sample(textureSampler, in.texCoord);

    // The Pharo framebuffer is BGRA, Metal expects RGBA
    // Swizzle if necessary (depends on texture format)
    return color;
}

// Alternative fragment shader with bilinear filtering
// Use this for smooth scaling on high-DPI displays
fragment float4 fragmentShaderSmooth(VertexOut in [[stage_in]],
                                     texture2d<float> displayTexture [[texture(0)]]) {
    constexpr sampler textureSampler(mag_filter::linear,
                                     min_filter::linear,
                                     address::clamp_to_edge);

    return displayTexture.sample(textureSampler, in.texCoord);
}
