// SPDX-License-Identifier: MIT
// Video presentation shaders (Shader Model 5.0, compiled offline with fxc).
//
// Input planes are always bound as single-slice Texture2DArray views so the
// same shaders serve D3D11VA decoder texture arrays (slice selected through
// the view's FirstArraySlice) and uploaded/copied textures.
//
// Luma filtering: Catmull-Rom bicubic evaluated with 9 bilinear fetches
// (weights combined per axis) for upscaling; 4-tap supersampling over the
// output pixel footprint for downscaling; plain bilinear near 1:1.

cbuffer present_constants : register(b0)
{
    float4 color_row0;      // rgb.r = dot(color_row0.xyz, ycbcr) + color_row0.w
    float4 color_row1;
    float4 color_row2;
    float2 luma_size;       // luma texture size in texels (including alignment padding)
    float2 visible_scale;   // visible picture size / texture size
    float filter_mode;      // 0 bilinear, 1 catmull-rom, 2 supersample
    float3 padding;
};

Texture2DArray<float> luma_plane : register(t0);
Texture2DArray<float2> chroma_plane : register(t1);   // NV12 interleaved CbCr
Texture2DArray<float> cb_plane : register(t1);        // I420 Cb (aliases t1)
Texture2DArray<float> cr_plane : register(t2);        // I420 Cr
SamplerState linear_clamp : register(s0);

struct vs_output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Full-viewport triangle from the vertex id (no vertex buffer).
vs_output vs_fullscreen(uint vertex_id : SV_VertexID)
{
    vs_output output;
    float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.uv = corner;
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float sample_luma_bilinear(float2 uv)
{
    return luma_plane.SampleLevel(linear_clamp, float3(uv, 0.0), 0.0);
}

float sample_luma_catmull_rom(float2 uv)
{
    float2 sample_position = uv * luma_size;
    float2 texel_center = floor(sample_position - 0.5) + 0.5;
    float2 f = sample_position - texel_center;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    // the two middle taps of each axis are merged into one bilinear fetch
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;

    float2 position0 = (texel_center - 1.0) / luma_size;
    float2 position3 = (texel_center + 2.0) / luma_size;
    float2 position12 = (texel_center + offset12) / luma_size;

    float result = 0.0;
    result += luma_plane.SampleLevel(linear_clamp, float3(position0.x, position0.y, 0.0), 0.0) * w0.x * w0.y;
    result += luma_plane.SampleLevel(linear_clamp, float3(position12.x, position0.y, 0.0), 0.0) * w12.x * w0.y;
    result += luma_plane.SampleLevel(linear_clamp, float3(position3.x, position0.y, 0.0), 0.0) * w3.x * w0.y;
    result += luma_plane.SampleLevel(linear_clamp, float3(position0.x, position12.y, 0.0), 0.0) * w0.x * w12.y;
    result += luma_plane.SampleLevel(linear_clamp, float3(position12.x, position12.y, 0.0), 0.0) * w12.x * w12.y;
    result += luma_plane.SampleLevel(linear_clamp, float3(position3.x, position12.y, 0.0), 0.0) * w3.x * w12.y;
    result += luma_plane.SampleLevel(linear_clamp, float3(position0.x, position3.y, 0.0), 0.0) * w0.x * w3.y;
    result += luma_plane.SampleLevel(linear_clamp, float3(position12.x, position3.y, 0.0), 0.0) * w12.x * w3.y;
    result += luma_plane.SampleLevel(linear_clamp, float3(position3.x, position3.y, 0.0), 0.0) * w3.x * w3.y;
    return result;
}

// Average of four bilinear taps spread over the output pixel footprint
// (screen-space derivatives of the texture coordinate): area-like
// filtering when shrinking.  Derivatives are taken outside any branch.
float sample_luma_supersample(float2 uv, float2 step_x, float2 step_y)
{
    float result = sample_luma_bilinear(uv - step_x - step_y);
    result += sample_luma_bilinear(uv + step_x - step_y);
    result += sample_luma_bilinear(uv - step_x + step_y);
    result += sample_luma_bilinear(uv + step_x + step_y);
    return result * 0.25;
}

float sample_luma(float2 uv)
{
    float2 step_x = ddx(uv) * 0.25;
    float2 step_y = ddy(uv) * 0.25;
    float result = 0.0;
    [branch] if (filter_mode > 1.5)
    {
        result = sample_luma_supersample(uv, step_x, step_y);
    }
    else if (filter_mode > 0.5)
    {
        result = sample_luma_catmull_rom(uv);
    }
    else
    {
        result = sample_luma_bilinear(uv);
    }
    return result;
}

float4 to_rgb(float3 ycbcr)
{
    float3 rgb = float3(dot(color_row0.xyz, ycbcr) + color_row0.w,
                        dot(color_row1.xyz, ycbcr) + color_row1.w,
                        dot(color_row2.xyz, ycbcr) + color_row2.w);
    return float4(saturate(rgb), 1.0);
}

float4 ps_nv12(vs_output input) : SV_Target
{
    float2 uv = input.uv * visible_scale;
    float luma = sample_luma(uv);
    float2 chroma = chroma_plane.SampleLevel(linear_clamp, float3(uv, 0.0), 0.0);
    return to_rgb(float3(luma, chroma));
}

float4 ps_i420(vs_output input) : SV_Target
{
    float2 uv = input.uv * visible_scale;
    float luma = sample_luma(uv);
    float cb = cb_plane.SampleLevel(linear_clamp, float3(uv, 0.0), 0.0);
    float cr = cr_plane.SampleLevel(linear_clamp, float3(uv, 0.0), 0.0);
    return to_rgb(float3(luma, cb, cr));
}
