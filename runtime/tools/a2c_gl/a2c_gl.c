/**
 * @file a2c_gl.c
 * @brief Measures an NVIDIA GPU's alpha to coverage through OpenGL, where GL_NV_alpha_to_coverage_dither_control
 * picks the dither the console's NVN state picks (dither on: the 2x2 footprint, off: 1x1; NVN's
 * BindMultisampleState, SDK 22.2.8). It writes the table test_nvn_a2c.c writes (RELIFT_A2C_TABLE): a header line,
 * then for each sample count and alpha step a of 255 the mask of each pixel of an 8x8 target, rows first, then the
 * least alpha (as float bits, found by bisection) of each coverage level: each alpha at which the target's covered
 * samples grow.
 *
 * Usage: a2c_gl on|off|default [samples...] (default 1 2 4 8 16). A count the driver gives fewer or more samples
 * for than asked is reported in the header and skipped. Headless: an EGL pbuffer-less context on the first EGL
 * device. Build: see runtime/tools/a2c_gl/README.md.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIDE 8u
#define STEPS 256u

#ifndef GL_ALPHA_TO_COVERAGE_DITHER_DEFAULT_NV
#define GL_ALPHA_TO_COVERAGE_DITHER_DEFAULT_NV 0x934D
#define GL_ALPHA_TO_COVERAGE_DITHER_ENABLE_NV 0x934E
#define GL_ALPHA_TO_COVERAGE_DITHER_DISABLE_NV 0x934F
#endif

typedef void(APIENTRYP dither_fn)(GLenum mode);

#define GLFNS(X)                                                                                                       \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                                                             \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                                                             \
    X(PFNGLTEXIMAGE2DMULTISAMPLEPROC, glTexImage2DMultisample)                                                         \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                                     \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                                     \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                                           \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                                       \
    X(PFNGLGETTEXLEVELPARAMETERIVPROC, glGetTexLevelParameteriv)                                                       \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                                           \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                                           \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                                         \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                                             \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                                                   \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                                         \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                                           \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                                           \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                                               \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                                                                 \
    X(PFNGLUNIFORM1UIPROC, glUniform1ui)                                                                               \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                                               \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                                     \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                                     \
    X(PFNGLVIEWPORTPROC, glViewport)                                                                                   \
    X(PFNGLENABLEPROC, glEnable)                                                                                       \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                                                               \
    X(PFNGLCLEARPROC, glClear)                                                                                         \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                                               \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                                               \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                                               \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                                                                       \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                                               \
    X(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData)                                                                   \
    X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute)                                                                     \
    X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier)                                                                         \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                                                         \
    X(PFNGLGETSTRINGPROC, glGetString)

#define DECL(t, n) static t n##_;
GLFNS(DECL)
#undef DECL
static dither_fn dither_;

static const char *const vs_src = "#version 450\n"
                                  "void main() {\n"
                                  "    vec2 q = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                                  "    gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);\n"
                                  "}\n";
static const char *const fs_src = "#version 450\n"
                                  "uniform float alpha;\n"
                                  "layout(location = 0) out vec4 o;\n"
                                  "void main() { o = vec4(1.0, 1.0, 1.0, alpha); }\n";
static const char *const cs_src = "#version 450\n"
                                  "layout(local_size_x = 1) in;\n"
                                  "layout(binding = 0) uniform sampler2DMS ms;\n"
                                  "layout(std430, binding = 0) buffer Out { uint o[]; };\n"
                                  "uniform uint samples;\n"
                                  "void main() {\n"
                                  "    ivec2 q = ivec2(gl_WorkGroupID.xy);\n"
                                  "    uint m = 0u;\n"
                                  "    for (uint s = 0u; s < samples; s++)\n"
                                  "        m |= (texelFetch(ms, q, int(s)).r > 0.5 ? 1u : 0u) << s;\n"
                                  "    o[q.y * 8 + q.x] = m;\n"
                                  "}\n";

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader_(kind);
    glShaderSource_(s, 1, &src, NULL);
    glCompileShader_(s);
    GLint ok = 0;
    glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog_(s, sizeof log, NULL, log);
        fprintf(stderr, "a2c_gl: shader: %s\n", log);
        exit(1);
    }
    return s;
}

static GLuint program(const char *a, GLenum ka, const char *b, GLenum kb)
{
    GLuint p = glCreateProgram_();
    glAttachShader_(p, shader(ka, a));
    if (b)
        glAttachShader_(p, shader(kb, b));
    glLinkProgram_(p);
    GLint ok = 0;
    glGetProgramiv_(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "a2c_gl: link failed\n");
        exit(1);
    }
    return p;
}

static void open_context(void)
{
    PFNEGLQUERYDEVICESEXTPROC query = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDeviceEXT devs[8];
    EGLint ndev = 0;
    if (!query || !platform || !query(8, devs, &ndev) || ndev < 1) {
        fprintf(stderr, "a2c_gl: no EGL device\n");
        exit(1);
    }
    EGLDisplay d = platform(EGL_PLATFORM_DEVICE_EXT, devs[0], NULL);
    if (!eglInitialize(d, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "a2c_gl: EGL initialization failed\n");
        exit(1);
    }
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint ncfg = 0;
    eglChooseConfig(d, cfg_attr, &cfg, 1, &ncfg);
    static const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                       EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                       EGL_NONE };
    EGLContext c = eglCreateContext(d, ncfg ? cfg : NULL, EGL_NO_CONTEXT, ctx_attr);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
        fprintf(stderr, "a2c_gl: no OpenGL 4.5 context\n");
        exit(1);
    }
#define LOAD(t, n) n##_ = (t)eglGetProcAddress(#n);
    GLFNS(LOAD)
#undef LOAD
    dither_ = (dither_fn)eglGetProcAddress("glAlphaToCoverageDitherControlNV");
}

/** @brief The target, programs and readback buffer of one sample count. */
typedef struct rig {
    GLuint tex, fb, draw, read, buf, vao;
    GLint alpha_at, samples_at;
    uint32_t samples;
} rig;

/** @brief Makes the target of n samples; false (with got set) when the driver gives another count. */
static bool rig_up(rig *k, uint32_t n, GLint *got)
{
    memset(k, 0, sizeof *k);
    k->samples = n;
    glGenTextures_(1, &k->tex);
    glBindTexture_(GL_TEXTURE_2D_MULTISAMPLE, k->tex);
    glTexImage2DMultisample_(GL_TEXTURE_2D_MULTISAMPLE, (GLsizei)n, GL_RGBA8, SIDE, SIDE, GL_TRUE);
    glGetTexLevelParameteriv_(GL_TEXTURE_2D_MULTISAMPLE, 0, GL_TEXTURE_SAMPLES, got);
    if ((uint32_t)*got != n)
        return false;
    glGenFramebuffers_(1, &k->fb);
    glBindFramebuffer_(GL_FRAMEBUFFER, k->fb);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, k->tex, 0);
    if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    k->draw = program(vs_src, GL_VERTEX_SHADER, fs_src, GL_FRAGMENT_SHADER);
    k->read = program(cs_src, GL_COMPUTE_SHADER, NULL, 0);
    k->alpha_at = glGetUniformLocation_(k->draw, "alpha");
    k->samples_at = glGetUniformLocation_(k->read, "samples");
    glGenBuffers_(1, &k->buf);
    glBindBuffer_(GL_SHADER_STORAGE_BUFFER, k->buf);
    glBufferData_(GL_SHADER_STORAGE_BUFFER, SIDE * SIDE * 4, NULL, GL_DYNAMIC_READ);
    glGenVertexArrays_(1, &k->vao);
    return true;
}

/** @brief The mask of each pixel at alpha, into masks. */
static void measure(rig *k, float alpha, uint32_t masks[SIDE * SIDE])
{
    glBindFramebuffer_(GL_FRAMEBUFFER, k->fb);
    glViewport_(0, 0, SIDE, SIDE);
    glClearColor_(0, 0, 0, 0);
    glClear_(GL_COLOR_BUFFER_BIT);
    glUseProgram_(k->draw);
    glUniform1f_(k->alpha_at, alpha);
    glBindVertexArray_(k->vao);
    glDrawArrays_(GL_TRIANGLES, 0, 3);
    glMemoryBarrier_(GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram_(k->read);
    glUniform1ui_(k->samples_at, k->samples);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture_(GL_TEXTURE_2D_MULTISAMPLE, k->tex);
    glBindBufferBase_(GL_SHADER_STORAGE_BUFFER, 0, k->buf);
    glDispatchCompute_(SIDE, SIDE, 1);
    glMemoryBarrier_(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer_(GL_SHADER_STORAGE_BUFFER, k->buf);
    glGetBufferSubData_(GL_SHADER_STORAGE_BUFFER, 0, SIDE * SIDE * 4, masks);
}

static uint32_t covered(rig *k, float alpha)
{
    uint32_t m[SIDE * SIDE], total = 0;
    measure(k, alpha, m);
    for (uint32_t i = 0; i < SIDE * SIDE; i++)
        total += (uint32_t)__builtin_popcount(m[i] & ((1u << k->samples) - 1u));
    return total;
}

static float f32_of(uint32_t b)
{
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

/** @brief The least alpha (float bits, in [0, 1]) covering at least want samples of the target. */
static uint32_t threshold(rig *k, uint32_t want)
{
    uint32_t lo = 0, hi = 0x3f800000u;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (covered(k, f32_of(mid)) >= want)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    GLenum m = !strcmp(mode, "on")        ? GL_ALPHA_TO_COVERAGE_DITHER_ENABLE_NV
               : !strcmp(mode, "off")     ? GL_ALPHA_TO_COVERAGE_DITHER_DISABLE_NV
               : !strcmp(mode, "default") ? GL_ALPHA_TO_COVERAGE_DITHER_DEFAULT_NV
                                          : 0;
    if (!m) {
        fprintf(stderr, "usage: a2c_gl on|off|default [samples...]\n");
        return 2;
    }
    open_context();
    if (!dither_) {
        fprintf(stderr, "a2c_gl: no GL_NV_alpha_to_coverage_dither_control\n");
        return 1;
    }
    printf("# %s (%s), dither %s: samples, alpha step of %u, then the mask of each pixel of an %ux%u target, rows "
           "first\n",
           (const char *)glGetString_(GL_RENDERER), (const char *)glGetString_(GL_VERSION), mode, STEPS - 1, SIDE,
           SIDE);
    static const uint32_t defaults[] = { 1, 2, 4, 8, 16 };
    uint32_t counts[16], ncounts = 0;
    for (int i = 2; i < argc && ncounts < 16; i++)
        counts[ncounts++] = (uint32_t)strtoul(argv[i], NULL, 0);
    if (!ncounts)
        for (uint32_t i = 0; i < 5; i++)
            counts[ncounts++] = defaults[i];
    glEnable_(GL_MULTISAMPLE);
    glEnable_(GL_SAMPLE_ALPHA_TO_COVERAGE);
    dither_(m);
    for (uint32_t c = 0; c < ncounts; c++) {
        uint32_t n = counts[c];
        rig k;
        GLint got = 0;
        if (!rig_up(&k, n, &got)) {
            printf("# %u samples: the driver gives %d, skipped\n", n, got);
            continue;
        }
        uint32_t masks[SIDE * SIDE];
        for (uint32_t a = 0; a < STEPS; a++) {
            measure(&k, (float)a / (float)(STEPS - 1), masks);
            printf("%u %u\t", n, a);
            for (uint32_t i = 0; i < SIDE * SIDE; i++)
                printf("%s%x", i ? " " : "", masks[i] & ((1u << n) - 1u));
            printf("\n");
        }
        uint32_t full = n * SIDE * SIDE, level = 0;
        for (uint32_t want = 1; want <= full;) {
            uint32_t t = threshold(&k, want);
            uint32_t got_samples = covered(&k, f32_of(t));
            printf("threshold %u %u\t%08x\n", n, ++level, t);
            want = got_samples + 1;
        }
        fflush(stdout);
    }
    return 0;
}
