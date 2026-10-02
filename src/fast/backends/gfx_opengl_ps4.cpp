#include "ship/window/Window.h"
// OpenGL ES 2.0 renderer for PS4 (Piglet). Derived from gfx_opengl.cpp, with everything that needs
// desktop GL / GLES3 (MSAA, glBlitFramebuffer, VAOs, depth read back, sized formats) replaced.
#if defined(ENABLE_OPENGL) && defined(__PS4__)

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <map>
#include <unordered_map>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#include "fast/backends/gfx_opengl.h"
#include "ship/window/gui/Gui.h"
#include <prism/processor.h>
#include <fstream>
#include "ship/Context.h"
#include "ship/resource/factory/ShaderFactory.h"
#include "fast/interpreter.h"
#include "ship/config/ConsoleVariable.h"
#include "fast/backends/gfx_opengl_ps4_shaders.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include "ship/port/ps4/Ps4Platform.h"

namespace Fast {
int GfxRenderingAPIOGL::GetMaxTextureSize() {
    GLint max_texture_size;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    return max_texture_size;
}

const char* GfxRenderingAPIOGL::GetName() {
    return "OpenGL";
}

GfxClipParameters GfxRenderingAPIOGL::GetClipParameters() {
    return { false, mFrameBuffers[mCurrentFrameBuffer].invertY };
}

static void VertexArraySetAttribs(ShaderProgram* prg) {
    size_t numFloats = prg->numFloats;
    size_t pos = 0;

    for (int i = 0; i < prg->numAttribs; i++) {
        if (prg->attribLocations[i] >= 0) {
            glEnableVertexAttribArray(prg->attribLocations[i]);
            glVertexAttribPointer(prg->attribLocations[i], prg->attribSizes[i], GL_FLOAT, GL_FALSE,
                                  numFloats * sizeof(float), (void*)(pos * sizeof(float)));
        }
        pos += prg->attribSizes[i];
    }
}

void GfxRenderingAPIOGL::SetUniforms(ShaderProgram* prg) const {
    glUniform1i(prg->frameCountLocation, mFrameCount);
    glUniform1f(prg->noiseScaleLocation, mCurrentNoiseScale);
}

void GfxRenderingAPIOGL::SetPerDrawUniforms() {
    if (mCurrentShaderProgram->usedTextures[0] || mCurrentShaderProgram->usedTextures[1]) {
        GLint filtering[2] = { textures[mCurrentTextureIds[0]].filtering, textures[mCurrentTextureIds[1]].filtering };
        glUniform1iv(mCurrentShaderProgram->texture_filtering_location, 2, filtering);

        GLint width[2] = { textures[mCurrentTextureIds[0]].width, textures[mCurrentTextureIds[1]].width };
        glUniform1iv(mCurrentShaderProgram->texture_width_location, 2, width);

        GLint height[2] = { textures[mCurrentTextureIds[0]].height, textures[mCurrentTextureIds[1]].height };
        glUniform1iv(mCurrentShaderProgram->texture_height_location, 2, height);
    }
}

void GfxRenderingAPIOGL::UnloadShader(ShaderProgram* old_prg) {
    if (old_prg != nullptr && old_prg == mLastLoadedShader) {
        for (unsigned int i = 0; i < old_prg->numAttribs; i++) {
            if (old_prg->attribLocations[i] >= 0) {
                glDisableVertexAttribArray(old_prg->attribLocations[i]);
            }
        }
        mLastLoadedShader = nullptr;
    }
}

void GfxRenderingAPIOGL::LoadShader(ShaderProgram* new_prg) {
    // if (!new_prg) return;
    mCurrentShaderProgram = new_prg;
    if (new_prg != mLastLoadedShader) {
        glUseProgram(new_prg->openglProgramId);
        VertexArraySetAttribs(new_prg);
        mLastLoadedShader = new_prg;
    }
    SetUniforms(new_prg);
}

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

static const char* shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                      bool first_cycle, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return first_cycle ? (with_alpha ? "texVal0" : "texVal0.rgb")
                                   : (with_alpha ? "texVal1" : "texVal1.rgb");
            case SHADER_TEXEL0A:
                return first_cycle
                           ? (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "vec3(texVal0.a, texVal0.a, texVal0.a)"))
                           : (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "vec3(texVal1.a, texVal1.a, texVal1.a)"));
            case SHADER_TEXEL1A:
                return first_cycle
                           ? (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "vec3(texVal1.a, texVal1.a, texVal1.a)"))
                           : (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "vec3(texVal0.a, texVal0.a, texVal0.a)"));
            case SHADER_TEXEL1:
                return first_cycle ? (with_alpha ? "texVal1" : "texVal1.rgb")
                                   : (with_alpha ? "texVal0" : "texVal0.rgb");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return first_cycle ? "texVal0.a" : "texVal1.a";
            case SHADER_TEXEL0A:
                return first_cycle ? "texVal0.a" : "texVal1.a";
            case SHADER_TEXEL1A:
                return first_cycle ? "texVal1.a" : "texVal0.a";
            case SHADER_TEXEL1:
                return first_cycle ? "texVal1.a" : "texVal0.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
    return "";
}

bool get_bool(prism::ContextTypes* value) {
    if (std::holds_alternative<int>(*value)) {
        return std::get<int>(*value) == 1;
    }
    return false;
}

prism::ContextTypes* append_formula(prism::ContextTypes* _, prism::ContextTypes* a_arg, prism::ContextTypes* a_single,
                                    prism::ContextTypes* a_mult, prism::ContextTypes* a_mix,
                                    prism::ContextTypes* a_with_alpha, prism::ContextTypes* a_only_alpha,
                                    prism::ContextTypes* a_alpha, prism::ContextTypes* a_first_cycle) {
    auto c = std::get<prism::MTDArray<int>>(*a_arg);
    bool do_single = get_bool(a_single);
    bool do_multiply = get_bool(a_mult);
    bool do_mix = get_bool(a_mix);
    bool with_alpha = get_bool(a_with_alpha);
    bool only_alpha = get_bool(a_only_alpha);
    bool opt_alpha = get_bool(a_alpha);
    bool first_cycle = get_bool(a_first_cycle);
    std::string out = "";
    if (do_single) {
        out += shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    } else if (do_multiply) {
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " * ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
    } else if (do_mix) {
        out += "mix(";
        out += shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += ")";
    } else {
        out += "(";
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " - ";
        out += shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ") * ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += " + ";
        out += shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    }
    return new prism::ContextTypes{ out };
}

std::optional<std::string> opengl_include_fs(const std::string& path) {
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    auto res = std::static_pointer_cast<Ship::Shader>(
        Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path, true, init));
    if (res == nullptr) {
        return std::nullopt;
    }
    auto inc = static_cast<std::string*>(res->GetRawPointer());
    return *inc;
}

std::string GfxRenderingAPIOGL::BuildFsShader(const CCFeatures& cc_features) {
    prism::Processor processor;
    prism::ContextItems mContext = {
        { "o_c", M_ARRAY(cc_features.c, int, 2, 2, 4) },
        { "o_alpha", cc_features.opt_alpha },
        { "o_fog", cc_features.opt_fog },
        { "o_texture_edge", cc_features.opt_texture_edge },
        { "o_noise", cc_features.opt_noise },
        { "o_2cyc", cc_features.opt_2cyc },
        { "o_alpha_threshold", cc_features.opt_alpha_threshold },
        { "o_invisible", cc_features.opt_invisible },
        { "o_grayscale", cc_features.opt_grayscale },
        { "o_textures", M_ARRAY(cc_features.usedTextures, bool, 2) },
        { "o_masks", M_ARRAY(cc_features.used_masks, bool, 2) },
        { "o_blend", M_ARRAY(cc_features.used_blend, bool, 2) },
        { "o_clamp", M_ARRAY(cc_features.clamp, bool, 2, 2) },
        { "o_inputs", cc_features.numInputs },
        { "o_do_mix", M_ARRAY(cc_features.do_mix, bool, 2, 2) },
        { "o_do_single", M_ARRAY(cc_features.do_single, bool, 2, 2) },
        { "o_do_multiply", M_ARRAY(cc_features.do_multiply, bool, 2, 2) },
        { "o_color_alpha_same", M_ARRAY(cc_features.color_alpha_same, bool, 2) },
        { "FILTER_THREE_POINT", FILTER_THREE_POINT },
        { "FILTER_LINEAR", FILTER_LINEAR },
        { "FILTER_NONE", FILTER_NONE },
        { "srgb_mode", mSrgbMode },
        { "SHADER_0", SHADER_0 },
        { "SHADER_INPUT_1", SHADER_INPUT_1 },
        { "SHADER_INPUT_2", SHADER_INPUT_2 },
        { "SHADER_INPUT_3", SHADER_INPUT_3 },
        { "SHADER_INPUT_4", SHADER_INPUT_4 },
        { "SHADER_INPUT_5", SHADER_INPUT_5 },
        { "SHADER_INPUT_6", SHADER_INPUT_6 },
        { "SHADER_INPUT_7", SHADER_INPUT_7 },
        { "SHADER_TEXEL0", SHADER_TEXEL0 },
        { "SHADER_TEXEL0A", SHADER_TEXEL0A },
        { "SHADER_TEXEL1", SHADER_TEXEL1 },
        { "SHADER_TEXEL1A", SHADER_TEXEL1A },
        { "SHADER_1", SHADER_1 },
        { "SHADER_COMBINED", SHADER_COMBINED },
        { "SHADER_NOISE", SHADER_NOISE },
        { "o_three_point_filtering", mCurrentFilterMode == FILTER_THREE_POINT },
        { "append_formula", (InvokeFunc)append_formula },
        { "GLSL_VERSION", "precision mediump float;" },
        { "attr", "varying" },
        { "opengles", false },
        { "core_opengl", false },
        { "texture", "texture2D" },
        { "vOutColor", "gl_FragColor" },
    };
    processor.populate(mContext);
    processor.load(std::string(gPs4FragmentShaderTemplate));
    processor.bind_include_loader(opengl_include_fs);
    auto result = processor.process();
    // SPDLOG_INFO("=========== FRAGMENT SHADER ============");
    // SPDLOG_INFO(result);
    // SPDLOG_INFO("========================================");
    return result;
}

static size_t numFloats = 0;

static prism::ContextTypes* UpdateFloats(prism::ContextTypes* _, prism::ContextTypes* num) {
    numFloats += std::get<int>(*num);
    return nullptr;
}

static std::string BuildVsShader(const CCFeatures& cc_features) {
    numFloats = 4;
    prism::Processor processor;
    prism::ContextItems mContext = { { "o_textures", M_ARRAY(cc_features.usedTextures, bool, 2) },
                                     { "o_clamp", M_ARRAY(cc_features.clamp, bool, 2, 2) },
                                     { "o_fog", cc_features.opt_fog },
                                     { "o_grayscale", cc_features.opt_grayscale },
                                     { "o_alpha", cc_features.opt_alpha },
                                     { "o_inputs", cc_features.numInputs },
                                     { "update_floats", (InvokeFunc)UpdateFloats },
                                     { "GLSL_VERSION", "" },
                                     { "attr", "attribute" },
                                     { "out", "varying" },
                                     { "opengles", true } };
    processor.populate(mContext);

    processor.load(std::string(gPs4VertexShaderTemplate));
    processor.bind_include_loader(opengl_include_fs);
    auto result = processor.process();
    // SPDLOG_INFO("=========== VERTEX SHADER ============");
    // SPDLOG_INFO(result);
    // SPDLOG_INFO("========================================");
    return result;
}

static void Ps4LogShaderSource(const std::string& source) {
    size_t lineNumber = 1;
    size_t start = 0;
    while (start < source.size()) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos) {
            end = source.size();
        }
        SPDLOG_ERROR("{:4}: {}", lineNumber++, source.substr(start, end - start));
        start = end + 1;
    }
}

static GLuint Ps4CompileShader(GLenum type, const std::string& source, const char* what) {
    const GLchar* src = source.data();
    const GLint length = (GLint)source.size();
    GLint success = GL_FALSE;

    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, &length);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char errorLog[2048] = { 0 };
        GLsizei logLength = 0;
        glGetShaderInfoLog(shader, sizeof(errorLog) - 1, &logLength, errorLog);
        SPDLOG_ERROR("[PS4] {} shader compilation failed: {}", what, errorLog);
        Ps4LogShaderSource(source);
        spdlog::default_logger()->flush();
        abort();
    }
    return shader;
}

static GLuint Ps4LinkProgram(GLuint vertexShader, GLuint fragmentShader) {
    GLint success = GL_FALSE;
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char errorLog[2048] = { 0 };
        GLsizei logLength = 0;
        glGetProgramInfoLog(program, sizeof(errorLog) - 1, &logLength, errorLog);
        SPDLOG_ERROR("[PS4] shader program link failed: {}", errorLog);
        spdlog::default_logger()->flush();
        abort();
    }
    return program;
}

// ---------------------------------------------------------------------------------------------
// Shader warm-up list
//
// Compiling a shader pair with Sony's runtime compiler takes around 200 ms, a visible hitch every
// time the game uses a new color combiner. Piglet doesn't hand out program binaries
// (GL_PROGRAM_BINARY_LENGTH is rejected), so they can't be cached on disk. Instead the ids of
// every combiner the game has ever asked for are remembered in a text file and compiled up front
// at the next start, behind the system's splash screen.
// ---------------------------------------------------------------------------------------------

static bool sWarmingUp = false;
static constexpr size_t kMaxWarmUpShaders = 512;

static std::string Ps4ShaderListPath() {
    return Ship::Context::GetPathRelativeToAppDirectory("ps4_shaders.txt");
}

static void Ps4RememberShader(uint64_t shaderId0, uint32_t shaderId1) {
    if (sWarmingUp) {
        return;
    }
    FILE* file = fopen(Ps4ShaderListPath().c_str(), "a");
    if (file != nullptr) {
        fprintf(file, "%016llx %08x\n", (unsigned long long)shaderId0, (unsigned int)shaderId1);
        fclose(file);
    }
}

void GfxRenderingAPIOGL::WarmUpShaders() {
    std::vector<std::pair<uint64_t, uint32_t>> ids;
    FILE* file = fopen(Ps4ShaderListPath().c_str(), "r");
    if (file != nullptr) {
        unsigned long long id0 = 0;
        unsigned int id1 = 0;
        while (ids.size() < kMaxWarmUpShaders && fscanf(file, "%llx %x", &id0, &id1) == 2) {
            ids.emplace_back((uint64_t)id0, (uint32_t)id1);
        }
        fclose(file);
    }
    if (ids.empty()) {
        SPDLOG_INFO("[PS4] no shader list yet, shaders will be compiled as the game needs them");
        return;
    }

    const Uint64 start = SDL_GetPerformanceCounter();
    size_t compiled = 0;
    sWarmingUp = true;
    for (const auto& id : ids) {
        if (LookupShader(id.first, id.second) == nullptr) {
            CreateAndLoadNewShader(id.first, id.second);
            compiled++;
        }
    }
    sWarmingUp = false;
    UnloadShader(mLastLoadedShader);

    const double seconds = (double)(SDL_GetPerformanceCounter() - start) / (double)SDL_GetPerformanceFrequency();
    SPDLOG_INFO("[PS4] warmed up {} shaders in {:.1f} s", compiled, seconds);
}

ShaderProgram* GfxRenderingAPIOGL::CreateAndLoadNewShader(uint64_t shader_id0, uint32_t shader_id1) {
    CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);
    const auto fs_buf = BuildFsShader(cc_features);
    const auto vs_buf = BuildVsShader(cc_features);
    // Runtime compilation goes through Sony's shader compiler module and is not fast: keep a
    // trace of how many shaders the game asked for and how long each one took.
    static int sShaderCount = 0;
    const Uint64 compileStart = SDL_GetPerformanceCounter();
    GLuint vertex_shader = Ps4CompileShader(GL_VERTEX_SHADER, vs_buf, "vertex");
    GLuint fragment_shader = Ps4CompileShader(GL_FRAGMENT_SHADER, fs_buf, "fragment");
    GLuint shader_program = Ps4LinkProgram(vertex_shader, fragment_shader);
    const double compileMs =
        (double)(SDL_GetPerformanceCounter() - compileStart) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    ++sShaderCount;
    if (!sWarmingUp) {
        // Only the ones that still interrupted the game are worth a line in the log.
        SPDLOG_INFO("[PS4] shader #{} ({:016X}/{:08X}) compiled while playing in {:.1f} ms", sShaderCount, shader_id0,
                    shader_id1, compileMs);
    }
    Ps4RememberShader(shader_id0, shader_id1);

    size_t cnt = 0;

    struct ShaderProgram* prg = &mShaderProgramPool[std::make_pair(shader_id0, shader_id1)];
    prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attribSizes[cnt] = 4;
    ++cnt;

    for (int i = 0; i < 2; i++) {
        if (cc_features.usedTextures[i]) {
            char name[32];
            snprintf(name, sizeof(name), "aTexCoord%d", i);
            prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
            prg->attribSizes[cnt] = 2;
            ++cnt;

            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    snprintf(name, sizeof(name), "aTexClamp%s%d", j == 0 ? "S" : "T", i);
                    prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
                    prg->attribSizes[cnt] = 1;
                    ++cnt;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attribSizes[cnt] = 4;
        ++cnt;
    }

    if (cc_features.opt_grayscale) {
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aGrayscaleColor");
        prg->attribSizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.numInputs; i++) {
        char name[16];
        snprintf(name, sizeof(name), "aInput%d", i + 1);
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attribSizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->openglProgramId = shader_program;
    prg->numInputs = cc_features.numInputs;
    prg->usedTextures[0] = cc_features.usedTextures[0];
    prg->usedTextures[1] = cc_features.usedTextures[1];
    prg->usedTextures[2] = cc_features.used_masks[0];
    prg->usedTextures[3] = cc_features.used_masks[1];
    prg->usedTextures[4] = cc_features.used_blend[0];
    prg->usedTextures[5] = cc_features.used_blend[1];
    prg->numFloats = numFloats;
    prg->numAttribs = cnt;

    prg->frameCountLocation = glGetUniformLocation(shader_program, "frame_count");
    prg->noiseScaleLocation = glGetUniformLocation(shader_program, "noise_scale");
    prg->texture_width_location = glGetUniformLocation(shader_program, "texture_width");
    prg->texture_height_location = glGetUniformLocation(shader_program, "texture_height");
    prg->texture_filtering_location = glGetUniformLocation(shader_program, "texture_filtering");

    LoadShader(prg);

    if (cc_features.usedTextures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
    }
    if (cc_features.usedTextures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
    }
    if (cc_features.used_masks[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexMask0");
        glUniform1i(sampler_location, 2);
    }
    if (cc_features.used_masks[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexMask1");
        glUniform1i(sampler_location, 3);
    }
    if (cc_features.used_blend[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexBlend0");
        glUniform1i(sampler_location, 4);
    }
    if (cc_features.used_blend[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexBlend1");
        glUniform1i(sampler_location, 5);
    }

    return prg;
}

struct ShaderProgram* GfxRenderingAPIOGL::LookupShader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = mShaderProgramPool.find(std::make_pair(shader_id0, shader_id1));
    return it == mShaderProgramPool.end() ? nullptr : &it->second;
}

void GfxRenderingAPIOGL::ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    *numInputs = prg->numInputs;
    usedTextures[0] = prg->usedTextures[0];
    usedTextures[1] = prg->usedTextures[1];
}

GLuint GfxRenderingAPIOGL::NewTexture() {
    GLuint ret;
    glGenTextures(1, &ret);
    textures.resize(std::max(textures.size(), (size_t)ret + 1));
    return ret;
}

void GfxRenderingAPIOGL::DeleteTexture(uint32_t texID) {
    glDeleteTextures(1, &texID);
}

void GfxRenderingAPIOGL::SelectTexture(int tile, GLuint texture_id) {
    if (mLastActiveTexture != tile) {
        mLastActiveTexture = tile;
        glActiveTexture(GL_TEXTURE0 + tile);
    }
    if (mLastBoundTextures[tile] != texture_id) {
        mLastBoundTextures[tile] = texture_id;
        glBindTexture(GL_TEXTURE_2D, texture_id);
    }
    mCurrentTextureIds[tile] = texture_id;
    mCurrentTile = tile;
}

void GfxRenderingAPIOGL::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    textures[mCurrentTextureIds[mCurrentTile]].width = width;
    textures[mCurrentTextureIds[mCurrentTile]].height = height;
}

// GLES2 has no mirror-clamp wrap mode. The fragment shader already clamps the coordinates
// when the N64 tile asks for it, so a mirrored repeat gives the same picture.
#undef GL_MIRROR_CLAMP_TO_EDGE
#define GL_MIRROR_CLAMP_TO_EDGE GL_MIRRORED_REPEAT

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GL_CLAMP_TO_EDGE;
        case G_TX_MIRROR | G_TX_WRAP:
            return GL_MIRRORED_REPEAT;
        case G_TX_MIRROR | G_TX_CLAMP:
            return GL_MIRROR_CLAMP_TO_EDGE;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return GL_REPEAT;
    }
    return 0;
}

void GfxRenderingAPIOGL::SetSamplerParameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    if (mLastActiveTexture != tile) {
        mLastActiveTexture = tile;
        glActiveTexture(GL_TEXTURE0 + tile);
    }
    const GLint filter = linear_filter && mCurrentFilterMode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    textures[mCurrentTextureIds[tile]].filtering = !linear_filter ? FILTER_LINEAR : FILTER_THREE_POINT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
}

void GfxRenderingAPIOGL::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = z_upd;
}

void GfxRenderingAPIOGL::SetZmodeDecal(bool zmode_decal) {
    mCurrentZmodeDecal = zmode_decal;
}

void GfxRenderingAPIOGL::SetViewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

void GfxRenderingAPIOGL::SetScissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

void GfxRenderingAPIOGL::SetUseAlpha(bool use_alpha) {
    int8_t val = use_alpha ? 1 : 0;
    if (mLastBlendEnabled != val) {
        mLastBlendEnabled = val;
        if (use_alpha) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }
    }
}

void GfxRenderingAPIOGL::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (mCurrentDepthTest != mLastDepthTest || mCurrentDepthMask != mLastDepthMask) {
        mLastDepthTest = mCurrentDepthTest;
        mLastDepthMask = mCurrentDepthMask;

        if (mCurrentDepthTest || mLastDepthMask) {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(mLastDepthMask ? GL_TRUE : GL_FALSE);
            glDepthFunc(mCurrentDepthTest ? (mCurrentZmodeDecal ? GL_LEQUAL : GL_LESS) : GL_ALWAYS);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }

    if (mCurrentZmodeDecal != mLastZmodeDecal) {
        mLastZmodeDecal = mCurrentZmodeDecal;
        if (mCurrentZmodeDecal) {
            // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
            // fighting
            const int n64modeFactor = 120;
            const int noVanishFactor = 100;
            GLfloat SSDB = -2;
            switch (Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_Z_FIGHTING_MODE, 0)) {
                // scaled z-fighting (N64 mode like)
                case 1:
                    if (mFrameBuffers.size() >
                        mCurrentFrameBuffer) { // safety check for vector size can probably be removed
                        SSDB = -1.0f * (GLfloat)mFrameBuffers[mCurrentFrameBuffer].height / n64modeFactor;
                    }
                    break;
                // no vanishing paths
                case 2:
                    if (mFrameBuffers.size() >
                        mCurrentFrameBuffer) { // safety check for vector size can probably be removed
                        SSDB = -1.0f * (GLfloat)mFrameBuffers[mCurrentFrameBuffer].height / noVanishFactor;
                    }
                    break;
                // disabled
                case 0:
                default:
                    SSDB = -2;
            }
            glPolygonOffset(SSDB, -2);
            glEnable(GL_POLYGON_OFFSET_FILL);
        } else {
            glPolygonOffset(0, 0);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    SetPerDrawUniforms();

    // printf("flushing %d tris\n", buf_vbo_num_tris);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

// Depth/stencil storage used by offscreen framebuffers. Picked once from the extension list and
// downgraded at runtime if Piglet reports an incomplete framebuffer with it.
static GLenum sDepthFormat = GL_DEPTH_COMPONENT16;
static bool sPackedDepthStencil = false;

static bool Ps4HasExtension(const char* name) {
    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
    return extensions != nullptr && strstr(extensions, name) != nullptr;
}

static void Ps4AttachDepth(GLuint rbo) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
    if (sPackedDepthStencil) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
    }
}

static const char* Ps4GlString(GLenum name) {
    const char* str = (const char*)glGetString(name);
    return str != nullptr ? str : "(null)";
}

// Texture bindings are cached by SelectTexture(), so anything that binds a texture behind its
// back has to put the previous one back.
#define PS4_RESTORE_TEXTURE_BINDING() \
    glBindTexture(GL_TEXTURE_2D, mLastActiveTexture >= 0 ? mLastBoundTextures[mLastActiveTexture] : 0)

void GfxRenderingAPIOGL::Init() {
    SPDLOG_INFO("[PS4] GL_VENDOR: {}", Ps4GlString(GL_VENDOR));
    SPDLOG_INFO("[PS4] GL_RENDERER: {}", Ps4GlString(GL_RENDERER));
    SPDLOG_INFO("[PS4] GL_VERSION: {}", Ps4GlString(GL_VERSION));
    SPDLOG_INFO("[PS4] GL_SHADING_LANGUAGE_VERSION: {}", Ps4GlString(GL_SHADING_LANGUAGE_VERSION));
    SPDLOG_INFO("[PS4] GL_EXTENSIONS: {}", Ps4GlString(GL_EXTENSIONS));

    glGenBuffers(1, &mOpenglVbo);
    glBindBuffer(GL_ARRAY_BUFFER, mOpenglVbo);

    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    mFrameBuffers.resize(1); // for the default screen buffer

    if (Ps4HasExtension("GL_OES_packed_depth_stencil")) {
        sDepthFormat = GL_DEPTH24_STENCIL8_OES;
        sPackedDepthStencil = true;
    } else if (Ps4HasExtension("GL_OES_depth24")) {
        sDepthFormat = GL_DEPTH_COMPONENT24_OES;
    } else if (Ps4HasExtension("GL_OES_depth32")) {
        // What Piglet actually offers. 16 bits are not enough once clip space z has been squeezed
        // (see the vertex shader), distant geometry z-fights.
        sDepthFormat = GL_DEPTH_COMPONENT32_OES;
    } else {
        sDepthFormat = GL_DEPTH_COMPONENT16;
    }
    SPDLOG_INFO("[PS4] offscreen depth format: 0x{:04X}", (unsigned int)sDepthFormat);

    // No multisampled renderbuffers on GLES2.
    mMaxMsaaLevel = 1;

    WarmUpShaders();
    Ship::Ps4::HideSplashScreen();
}

void GfxRenderingAPIOGL::OnResize() {
}

void GfxRenderingAPIOGL::StartFrame() {
    mFrameCount++;
}

void GfxRenderingAPIOGL::EndFrame() {
    glFlush();
}

void GfxRenderingAPIOGL::FinishRender() {
}

int GfxRenderingAPIOGL::CreateFramebuffer() {
    GLuint clrbuf;
    glGenTextures(1, &clrbuf);
    glBindTexture(GL_TEXTURE_2D, clrbuf);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Framebuffer textures are rarely power-of-two sized, which GLES2 only guarantees with clamping.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    PS4_RESTORE_TEXTURE_BINDING();

    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, sDepthFormat, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);

    size_t i = mFrameBuffers.size();
    mFrameBuffers.resize(i + 1);

    mFrameBuffers[i].fbo = fbo;
    mFrameBuffers[i].clrbuf = clrbuf;
    mFrameBuffers[i].clrbufMsaa = 0;
    mFrameBuffers[i].rbo = rbo;

    textures.resize(std::max(textures.size(), (size_t)clrbuf + 1));

    return i;
}

void GfxRenderingAPIOGL::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                                     bool can_extract_depth) {
    FramebufferOGL& fb = mFrameBuffers[fb_id];

    width = std::max(width, 1U);
    height = std::max(height, 1U);
    msaa_level = 1;

    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    if (fb_id != 0) {
        const bool sizeChanged = fb.width != width || fb.height != height;

        if (sizeChanged) {
            glBindTexture(GL_TEXTURE_2D, fb.clrbuf);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
            PS4_RESTORE_TEXTURE_BINDING();
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.clrbuf, 0);

            if (fb.clrbuf < textures.size()) {
                textures[fb.clrbuf].width = width;
                textures[fb.clrbuf].height = height;
            }
        }

        if (has_depth_buffer && (sizeChanged || !fb.has_depth_buffer)) {
            glBindRenderbuffer(GL_RENDERBUFFER, fb.rbo);
            glRenderbufferStorage(GL_RENDERBUFFER, sDepthFormat, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }

        if (!fb.has_depth_buffer && has_depth_buffer) {
            Ps4AttachDepth(fb.rbo);
        } else if (fb.has_depth_buffer && !has_depth_buffer) {
            Ps4AttachDepth(0);
        }

        if (sizeChanged || fb.has_depth_buffer != has_depth_buffer) {
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE && has_depth_buffer && sDepthFormat != GL_DEPTH_COMPONENT16) {
                SPDLOG_WARN("[PS4] framebuffer {} incomplete (0x{:04X}) with depth format 0x{:04X}, using 16 bit depth",
                            fb_id, (unsigned int)status, (unsigned int)sDepthFormat);
                Ps4AttachDepth(0);
                sDepthFormat = GL_DEPTH_COMPONENT16;
                sPackedDepthStencil = false;
                glBindRenderbuffer(GL_RENDERBUFFER, fb.rbo);
                glRenderbufferStorage(GL_RENDERBUFFER, sDepthFormat, width, height);
                glBindRenderbuffer(GL_RENDERBUFFER, 0);
                Ps4AttachDepth(fb.rbo);
                status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            }
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                SPDLOG_ERROR("[PS4] framebuffer {} ({}x{}) is incomplete: 0x{:04X}", fb_id, width, height,
                             (unsigned int)status);
            }
        }
    }

    fb.width = width;
    fb.height = height;
    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
    fb.invertY = opengl_invertY;
}

void GfxRenderingAPIOGL::StartDrawToFramebuffer(int fb_id, float noise_scale) {
    FramebufferOGL& fb = mFrameBuffers[fb_id];

    if (noise_scale != 0.0f) {
        mCurrentNoiseScale = 1.0f / noise_scale;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
    mCurrentFrameBuffer = fb_id;
}

void GfxRenderingAPIOGL::ClearFramebuffer(bool color, bool depth) {
    if (mLastScissorEnabled != 0) {
        mLastScissorEnabled = 0;
        glDisable(GL_SCISSOR_TEST);
    }
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear((color ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0));
    glDepthMask(mCurrentDepthMask ? GL_TRUE : GL_FALSE);
    if (mLastScissorEnabled != 1) {
        mLastScissorEnabled = 1;
        glEnable(GL_SCISSOR_TEST);
    }
}

void GfxRenderingAPIOGL::ResolveMSAAColorBuffer(int fb_id_target, int fb_id_source) {
    // MSAA is never enabled on PS4 (mMaxMsaaLevel == 1), nothing to resolve.
}

void* GfxRenderingAPIOGL::GetFramebufferTextureId(int fb_id) {
    return (void*)(uintptr_t)mFrameBuffers[fb_id].clrbuf;
}

void GfxRenderingAPIOGL::SelectTextureFb(int fb_id) {
    // glDisable(GL_DEPTH_TEST);
    int tile = 0;
    SelectTexture(tile, mFrameBuffers[fb_id].clrbuf);
}

// Draws the [srcX0,srcX1]x[srcY0,srcY1] texel rectangle of `texture` over the
// [dstX0,dstX1]x[dstY0,dstY1] pixel rectangle of `dstFbo`. Stand-in for glBlitFramebuffer, which
// GLES2 lacks; like it, all coordinates have their origin at the bottom left.
void GfxRenderingAPIOGL::BlitTexture(GLuint texture, int texWidth, int texHeight, int srcX0, int srcY0, int srcX1,
                                     int srcY1, GLuint dstFbo, int dstX0, int dstY0, int dstX1, int dstY1) {
    if (texWidth <= 0 || texHeight <= 0 || dstX0 == dstX1 || dstY0 == dstY1) {
        return;
    }

    if (mBlitProgram == 0) {
        static const std::string vs = "attribute vec2 aPos;\n"
                                      "attribute vec2 aUv;\n"
                                      "varying vec2 vUv;\n"
                                      "void main() {\n"
                                      "    vUv = aUv;\n"
                                      "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
                                      "}\n";
        static const std::string fs = "precision mediump float;\n"
                                      "varying vec2 vUv;\n"
                                      "uniform sampler2D uTex;\n"
                                      "void main() {\n"
                                      "    gl_FragColor = vec4(texture2D(uTex, vUv).rgb, 1.0);\n"
                                      "}\n";
        GLuint vertexShader = Ps4CompileShader(GL_VERTEX_SHADER, vs, "blit vertex");
        GLuint fragmentShader = Ps4CompileShader(GL_FRAGMENT_SHADER, fs, "blit fragment");
        mBlitProgram = Ps4LinkProgram(vertexShader, fragmentShader);
        mBlitPosLocation = glGetAttribLocation(mBlitProgram, "aPos");
        mBlitUvLocation = glGetAttribLocation(mBlitProgram, "aUv");
        glUseProgram(mBlitProgram);
        glUniform1i(glGetUniformLocation(mBlitProgram, "uTex"), 0);
    }

    // A mirrored destination rectangle is the same as a mirrored source one.
    if (dstX1 < dstX0) {
        std::swap(dstX0, dstX1);
        std::swap(srcX0, srcX1);
    }
    if (dstY1 < dstY0) {
        std::swap(dstY0, dstY1);
        std::swap(srcY0, srcY1);
    }

    // Save the state the interpreter believes is still set.
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    const GLboolean depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean polygonOffsetEnabled = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    GLboolean depthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

    ShaderProgram* previousProgram = mLastLoadedShader;
    if (previousProgram != nullptr) {
        for (unsigned int i = 0; i < previousProgram->numAttribs; i++) {
            if (previousProgram->attribLocations[i] >= 0) {
                glDisableVertexAttribArray(previousProgram->attribLocations[i]);
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_FALSE);
    glViewport(dstX0, dstY0, dstX1 - dstX0, dstY1 - dstY0);

    const float u0 = (float)srcX0 / (float)texWidth;
    const float u1 = (float)srcX1 / (float)texWidth;
    const float v0 = (float)srcY0 / (float)texHeight;
    const float v1 = (float)srcY1 / (float)texHeight;
    const float vertices[] = {
        -1.0f, -1.0f, u0, v0, 1.0f, -1.0f, u1, v0, -1.0f, 1.0f, u0, v1,
        1.0f,  -1.0f, u1, v0, 1.0f, 1.0f,  u1, v1, -1.0f, 1.0f, u0, v1,
    };

    glUseProgram(mBlitProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    // mOpenglVbo stays bound to GL_ARRAY_BUFFER for the whole lifetime of the renderer and
    // DrawTriangles() re-uploads it on every draw, so it can be borrowed here.
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glEnableVertexAttribArray(mBlitPosLocation);
    glVertexAttribPointer(mBlitPosLocation, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(mBlitUvLocation);
    glVertexAttribPointer(mBlitUvLocation, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(mBlitPosLocation);
    glDisableVertexAttribArray(mBlitUvLocation);

    // Restore.
    glBindTexture(GL_TEXTURE_2D, mLastBoundTextures[0]);
    if (mLastActiveTexture > 0) {
        glActiveTexture(GL_TEXTURE0 + mLastActiveTexture);
    }
    if (previousProgram != nullptr) {
        glUseProgram(previousProgram->openglProgramId);
        VertexArraySetAttribs(previousProgram);
    }
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (depthTestEnabled) {
        glEnable(GL_DEPTH_TEST);
    }
    if (blendEnabled) {
        glEnable(GL_BLEND);
    }
    if (scissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
    }
    if (polygonOffsetEnabled) {
        glEnable(GL_POLYGON_OFFSET_FILL);
    }
    glDepthMask(depthMask);
}

void GfxRenderingAPIOGL::CopyFramebuffer(int fb_dst_id, int fb_src_id, int srcX0, int srcY0, int srcX1, int srcY1,
                                         int dstX0, int dstY0, int dstX1, int dstY1) {
    if (fb_dst_id >= (int)mFrameBuffers.size() || fb_src_id >= (int)mFrameBuffers.size()) {
        return;
    }

    const FramebufferOGL src = mFrameBuffers[fb_src_id];
    const FramebufferOGL dst = mFrameBuffers[fb_dst_id];

    // Adjust y values for non-inverted source frame buffers because opengl uses bottom left for origin
    if (!src.invertY) {
        int temp = srcY1 - srcY0;
        srcY1 = src.height - srcY0;
        srcY0 = srcY1 - temp;
    }

    // Flip the y values
    if (src.invertY != dst.invertY) {
        std::swap(srcY0, srcY1);
    }

    GLuint srcTexture = src.clrbuf;
    int srcWidth = src.width;
    int srcHeight = src.height;

    if (fb_src_id == 0) {
        // The screen can't be sampled: grab the requested rectangle into a scratch texture first.
        const int x0 = std::max(std::min(srcX0, srcX1), 0);
        const int y0 = std::max(std::min(srcY0, srcY1), 0);
        const int x1 = std::min(std::max(srcX0, srcX1), (int)src.width);
        const int y1 = std::min(std::max(srcY0, srcY1), (int)src.height);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }

        if (mLastActiveTexture != 0) {
            glActiveTexture(GL_TEXTURE0);
        }
        if (mBlitScratchTexture == 0) {
            glGenTextures(1, &mBlitScratchTexture);
            glBindTexture(GL_TEXTURE_2D, mBlitScratchTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            textures.resize(std::max(textures.size(), (size_t)mBlitScratchTexture + 1));
        } else {
            glBindTexture(GL_TEXTURE_2D, mBlitScratchTexture);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, x0, y0, x1 - x0, y1 - y0, 0);
        // BlitTexture() restores the texture unit/binding the interpreter expects.

        srcTexture = mBlitScratchTexture;
        srcWidth = x1 - x0;
        srcHeight = y1 - y0;
        srcX0 -= x0;
        srcX1 -= x0;
        srcY0 -= y0;
        srcY1 -= y0;
    }

    BlitTexture(srcTexture, srcWidth, srcHeight, srcX0, srcY0, srcX1, srcY1, dst.fbo, dstX0, dstY0, dstX1, dstY1);

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);
}

void GfxRenderingAPIOGL::ReadFramebufferToCPU(int fb_id, uint32_t width, uint32_t height, uint16_t* rgba16_buf) {
    if (fb_id >= (int)mFrameBuffers.size()) {
        return;
    }

    // GLES2 only guarantees RGBA/UNSIGNED_BYTE read back, convert to RGBA5551 by hand.
    std::vector<uint8_t> rgba32(static_cast<size_t>(width) * height * 4);

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[fb_id].fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba32.data());
    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);

    const size_t pixels = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixels; i++) {
        const uint8_t r = rgba32[i * 4 + 0];
        const uint8_t g = rgba32[i * 4 + 1];
        const uint8_t b = rgba32[i * 4 + 2];
        const uint8_t a = rgba32[i * 4 + 3];
        rgba16_buf[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7));
    }
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIOGL::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;

    // The depth buffer can't be read back on GLES2. Report "nothing in front" so that light glows
    // and lens flares, the only users of this, are always drawn.
    for (const auto& coord : coordinates) {
        res.emplace(coord, (uint16_t)0xFFFC);
    }

    return res;
}

void GfxRenderingAPIOGL::SetTextureFilter(FilteringMode mode) {
    gfx_texture_cache_clear();
    mCurrentFilterMode = mode;
}

FilteringMode GfxRenderingAPIOGL::GetTextureFilter() {
    return mCurrentFilterMode;
}

void GfxRenderingAPIOGL::SetSrgbMode() {
    mSrgbMode = true;
}

ImTextureID GfxRenderingAPIOGL::GetTextureById(int id) {
    return reinterpret_cast<ImTextureID>(id);
}
} // namespace Fast
#endif
