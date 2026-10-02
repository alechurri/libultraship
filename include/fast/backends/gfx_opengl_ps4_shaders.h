#pragma once
#ifdef __PS4__

// Prism templates for the PS4 (Piglet) renderer.
//
// Piglet compiles GLSL ES 1.00, so compared to shaders/opengl/default.shader.* these:
//  - use attribute/varying/texture2D/gl_FragColor,
//  - never index a uniform array with a non-constant expression,
//  - avoid textureSize() and mix() with a boolean selector.
// They are embedded in the executable so the port doesn't depend on the shader stored in soh.o2r.

namespace Fast {

static const char* const gPs4VertexShaderTemplate = R"PS4SHADER(@prism(type='vertex', name='Fast3D Vertex Shader (PS4)', version='1.0.0', description='GLSL ES 1.00 variant for Piglet', author='Emill & Prism Team')

attribute vec4 aVtxPos;

@for(i in 0..2)
    @if(o_textures[i])
        attribute vec2 aTexCoord@{i};
        varying vec2 vTexCoord@{i};
        @{update_floats(2)}
        @for(j in 0..2)
            @if(o_clamp[i][j])
                @if(j == 0)
                    attribute float aTexClampS@{i};
                    varying float vTexClampS@{i};
                @else
                    attribute float aTexClampT@{i};
                    varying float vTexClampT@{i};
                @end
                @{update_floats(1)}
            @end
        @end
    @end
@end

@if(o_fog)
    attribute vec4 aFog;
    varying vec4 vFog;
    @{update_floats(4)}
@end

@if(o_grayscale)
    attribute vec4 aGrayscaleColor;
    varying vec4 vGrayscaleColor;
    @{update_floats(4)}
@end

@for(i in 0..o_inputs)
    @if(o_alpha)
        attribute vec4 aInput@{i + 1};
        varying vec4 vInput@{i + 1};
        @{update_floats(4)}
    @else
        attribute vec3 aInput@{i + 1};
        varying vec3 vInput@{i + 1};
        @{update_floats(3)}
    @end
@end

void main() {
     @for(i in 0..2)
        @if(o_textures[i])
            vTexCoord@{i} = aTexCoord@{i};
            @for(j in 0..2)
                @if(o_clamp[i][j])
                    @if(j == 0)
                        vTexClampS@{i} = aTexClampS@{i};
                    @else
                        vTexClampT@{i} = aTexClampT@{i};
                    @end
                @end
            @end
        @end
    @end
    @if(o_fog)
        vFog = aFog;
    @end
    @if(o_grayscale)
        vGrayscaleColor = aGrayscaleColor;
    @end
    @for(i in 0..o_inputs)
        vInput@{i + 1} = aInput@{i + 1};
    @end
    gl_Position = aVtxPos;
    // No depth clamp on GLES: squeeze clip space z instead so geometry beyond the far plane survives.
    gl_Position.z *= 0.3;
}
)PS4SHADER";

static const char* const gPs4FragmentShaderTemplate = R"PS4SHADER(@prism(type='fragment', name='Fast3D Fragment Shader (PS4)', version='1.0.0', description='GLSL ES 1.00 variant for Piglet', author='Emill & Prism Team')

precision mediump float;

@for(i in 0..2)
    @if(o_textures[i])
        varying vec2 vTexCoord@{i};
        @for(j in 0..2)
            @if(o_clamp[i][j])
                @if(j == 0)
                    varying float vTexClampS@{i};
                @else
                    varying float vTexClampT@{i};
                @end
            @end
        @end
    @end
@end

@if(o_fog) varying vec4 vFog;
@if(o_grayscale) varying vec4 vGrayscaleColor;

@for(i in 0..o_inputs)
    @if(o_alpha)
        varying vec4 vInput@{i + 1};
    @else
        varying vec3 vInput@{i + 1};
    @end
@end

@if(o_textures[0]) uniform sampler2D uTex0;
@if(o_textures[1]) uniform sampler2D uTex1;

@if(o_masks[0]) uniform sampler2D uTexMask0;
@if(o_masks[1]) uniform sampler2D uTexMask1;

@if(o_blend[0]) uniform sampler2D uTexBlend0;
@if(o_blend[1]) uniform sampler2D uTexBlend1;

uniform int frame_count;
uniform float noise_scale;

uniform int texture_width[2];
uniform int texture_height[2];
uniform int texture_filtering[2];

#define TEX_OFFSET(off) texture2D(tex, texCoord - off / texSize)
#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)

float random(in vec3 value) {
    float r = dot(sin(value), vec3(12.9898, 78.233, 37.719));
    return fract(sin(r) * 143758.5453);
}

vec4 fromLinear(vec4 linearRGB) {
    vec3 cutoff = step(linearRGB.rgb, vec3(0.0031308));
    vec3 higher = vec3(1.055) * pow(linearRGB.rgb, vec3(1.0 / 2.4)) - vec3(0.055);
    vec3 lower = linearRGB.rgb * vec3(12.92);
    return vec4(mix(higher, lower, cutoff), linearRGB.a);
}

vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {
    vec2 offset = fract(texCoord*texSize - vec2(0.5));
    offset -= step(1.0, offset.x + offset.y);
    vec4 c0 = TEX_OFFSET(offset);
    vec4 c1 = TEX_OFFSET(vec2(offset.x - sign(offset.x), offset.y));
    vec4 c2 = TEX_OFFSET(vec2(offset.x, offset.y - sign(offset.y)));
    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);
}

vec4 hookTexture2D(in int filtering, in sampler2D tex, in vec2 uv, in vec2 texSize) {
@if(o_three_point_filtering)
    if (filtering == @{FILTER_THREE_POINT}) {
        return filter3point(tex, uv, texSize);
    }
@end
    return texture2D(tex, uv);
}

void main() {
    @for(i in 0..2)
        @if(o_textures[i])
            @{s = o_clamp[i][0]}
            @{t = o_clamp[i][1]}

            vec2 texSize@{i} = vec2(float(texture_width[@{i}]), float(texture_height[@{i}]));

            @if(!s && !t)
                vec2 vTexCoordAdj@{i} = vTexCoord@{i};
            @else
                @if(s && t)
                    vec2 vTexCoordAdj@{i} = clamp(vTexCoord@{i}, 0.5 / texSize@{i}, vec2(vTexClampS@{i}, vTexClampT@{i}));
                @elseif(s)
                    vec2 vTexCoordAdj@{i} = vec2(clamp(vTexCoord@{i}.s, 0.5 / texSize@{i}.s, vTexClampS@{i}), vTexCoord@{i}.t);
                @else
                    vec2 vTexCoordAdj@{i} = vec2(vTexCoord@{i}.s, clamp(vTexCoord@{i}.t, 0.5 / texSize@{i}.t, vTexClampT@{i}));
                @end
            @end

            vec4 texVal@{i} = hookTexture2D(texture_filtering[@{i}], uTex@{i}, vTexCoordAdj@{i}, texSize@{i});

            @if(o_masks[i])
                vec2 maskSize@{i} = texSize@{i};

                vec4 maskVal@{i} = hookTexture2D(texture_filtering[@{i}], uTexMask@{i}, vTexCoordAdj@{i}, maskSize@{i});

                @if(o_blend[i])
                    vec4 blendVal@{i} = hookTexture2D(texture_filtering[@{i}], uTexBlend@{i}, vTexCoordAdj@{i}, texSize@{i});
                @else
                    vec4 blendVal@{i} = vec4(0.0, 0.0, 0.0, 0.0);
                @end

                texVal@{i} = mix(texVal@{i}, blendVal@{i}, maskVal@{i}.a);
            @end
        @end
    @end

    @if(o_alpha)
        vec4 texel;
    @else
        vec3 texel;
    @end

    @if(o_2cyc)
        @{f_range = 2}
    @else
        @{f_range = 1}
    @end

    @for(c in 0..f_range)
        @if(c == 1)
            @if(o_alpha)
                @if(o_c[c][1][2] == SHADER_COMBINED)
                    texel.a = WRAP(texel.a, -1.01, 1.01);
                @else
                    texel.a = WRAP(texel.a, -0.51, 1.51);
                @end
            @end

            @if(o_c[c][0][2] == SHADER_COMBINED)
                texel.rgb = WRAP(texel.rgb, -1.01, 1.01);
            @else
                texel.rgb = WRAP(texel.rgb, -0.51, 1.51);
            @end
        @end

        @if(!o_color_alpha_same[c] && o_alpha)
            texel = vec4(@{
            append_formula(o_c[c], o_do_single[c][0],
                           o_do_multiply[c][0], o_do_mix[c][0], false, false, true, c == 0)
            }, @{append_formula(o_c[c], o_do_single[c][1],
                           o_do_multiply[c][1], o_do_mix[c][1], true, true, true, c == 0)
            });
        @else
            texel = @{append_formula(o_c[c], o_do_single[c][0],
                           o_do_multiply[c][0], o_do_mix[c][0], o_alpha, false,
                           o_alpha, c == 0)};
        @end
    @end

    texel = WRAP(texel, -0.51, 1.51);
    texel = clamp(texel, 0.0, 1.0);
    // TODO discard if alpha is 0?
    @if(o_fog)
        @if(o_alpha)
            texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);
        @else
            texel = mix(texel, vFog.rgb, vFog.a);
        @end
    @end

    @if(o_texture_edge && o_alpha)
        if (texel.a > 0.19) texel.a = 1.0; else discard;
    @end

    @if(o_alpha && o_noise)
        texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + texel.a, 0.0, 1.0));
    @end

    @if(o_grayscale)
        float intensity = (texel.r + texel.g + texel.b) / 3.0;
        vec3 new_texel = vGrayscaleColor.rgb * intensity;
        texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);
    @end

    @if(o_alpha)
        @if(o_alpha_threshold)
            if (texel.a < 8.0 / 256.0) discard;
        @end
        @if(o_invisible)
            texel.a = 0.0;
        @end
        gl_FragColor = texel;
    @else
        gl_FragColor = vec4(texel, 1.0);
    @end

    @if(srgb_mode)
        gl_FragColor = fromLinear(gl_FragColor);
    @end
}
)PS4SHADER";

} // namespace Fast

#endif // __PS4__
