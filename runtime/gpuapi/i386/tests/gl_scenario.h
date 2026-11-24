/**
 * @file gl_scenario.h
 * @brief One OpenGL 1.x/2.x workload, built two ways like the zlib one: a
 * native i386 program against the real i386 libGL (gl_native.c) and the
 * host through the generated thunks (gl_harness.c). Both make the same
 * calls with the same i386 argument slots and print the same report.
 *
 * The includer defines gaddr, galloc, gp and glc(name, slots, n), which
 * calls the named GL function with n 32-bit cdecl argument slots and
 * returns eax.
 */
#include <stdio.h>
#include <string.h>

/** @brief The GL constants the scenario names. */
enum {
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_MODELVIEW = 0x1700,
    GL_MODELVIEW_MATRIX = 0x0BA6,
    GL_PROJECTION = 0x1701,
    GL_READ_WRITE = 0x88BA,
    GL_RGBA = 0x1908,
    GL_TEXTURE_COORD_ARRAY = 0x8078,
    GL_TRIANGLES = 4,
    GL_TRIANGLE_FAN = 6,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_VERTEX_ARRAY = 0x8074,
    GL_VERTEX_ARRAY_POINTER = 0x808E,
    GL_WRITE_ONLY = 0x88B9,
};

#define GLC(name, ...)                                                                             \
    ({                                                                                             \
        const uint32_t s_[] = {0, __VA_ARGS__};                                                    \
        glc(name, s_ + 1, sizeof s_ / 4 - 1);                                                      \
    })
#define GL0(name) glc(name, NULL, 0)

static uint32_t fbits(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

static uint32_t dlo(double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    return (uint32_t)v;
}

static uint32_t dhi(double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    return (uint32_t)(v >> 32);
}

static gaddr gdata(const void *p, size_t n)
{
    gaddr a = galloc(n);
    memcpy(gp(a), p, n);
    return a;
}

static gaddr gstr(const char *s)
{
    return gdata(s, strlen(s) + 1);
}

static uint32_t rd32(gaddr a)
{
    uint32_t v;
    memcpy(&v, gp(a), 4);
    return v;
}

static uint32_t fnv(gaddr a, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ gp(a)[i]) * 16777619u;
    return h;
}

enum {
    W = 64,
    H = 64,
};

static void report_pixels(const char *what)
{
    gaddr px = galloc(W * H * 4);
    GLC("glReadPixels", 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("%s: pixels %08x corner %08x center %08x error %x\n", what, fnv(px, W * H * 4), rd32(px),
           rd32(px + ((H / 2) * W + W / 2) * 4), GL0("glGetError"));
}

static gaddr shader(uint32_t kind, const char *src)
{
    uint32_t s = GLC("glCreateShader", kind);
    gaddr strs = galloc(8);
    uint32_t a = gstr(src);
    memcpy(gp(strs), &a, 4);
    GLC("glShaderSource", s, 1, strs, 0);
    GLC("glCompileShader", s);
    gaddr ok = galloc(4);
    GLC("glGetShaderiv", s, GL_COMPILE_STATUS, ok);
    printf("shader %u compiled %u\n", s, rd32(ok));
    return s;
}

static void scenario(void)
{
    printf("GL_VERSION %s\n", (const char *)gp(GLC("glGetString", 0x1F02)));
    printf("GL_SHADING_LANGUAGE_VERSION %s\n", (const char *)gp(GLC("glGetString", 0x8B8C)));
    gaddr iv = galloc(16);
    GLC("glGetIntegerv", GL_MAX_TEXTURE_SIZE, iv);
    printf("max texture size %u\n", rd32(iv));
    GLC("glViewport", 0, 0, W, H);

    GLC("glClearColor", fbits(0.25f), fbits(0.5f), fbits(0.75f), fbits(1.0f));
    GLC("glClearDepth", dlo(1.0), dhi(1.0));
    GLC("glClear", 0x4000 | 0x100);
    report_pixels("clear");

    GLC("glMatrixMode", GL_PROJECTION);
    GL0("glLoadIdentity");
    GLC("glOrtho", dlo(-1), dhi(-1), dlo(1), dhi(1), dlo(-1), dhi(-1), dlo(1), dhi(1), dlo(-2), dhi(-2), dlo(2), dhi(2));
    GLC("glMatrixMode", GL_MODELVIEW);
    GL0("glLoadIdentity");
    GLC("glRotatef", fbits(30.0f), 0, 0, fbits(1.0f));
    GLC("glBegin", GL_TRIANGLES);
    GLC("glColor3f", fbits(1.0f), 0, 0);
    GLC("glVertex2f", fbits(-0.8f), fbits(-0.8f));
    GLC("glColor3f", 0, fbits(1.0f), 0);
    GLC("glVertex2f", fbits(0.8f), fbits(-0.6f));
    GLC("glColor3d", dlo(0), dhi(0), dlo(0), dhi(0), dlo(1), dhi(1));
    GLC("glVertex3d", dlo(0.1), dhi(0.1), dlo(0.9), dhi(0.9), dlo(0), dhi(0));
    GL0("glEnd");
    report_pixels("immediate triangle");
    gaddr mat = galloc(64);
    GLC("glGetFloatv", GL_MODELVIEW_MATRIX, mat);
    printf("modelview %08x\n", fnv(mat, 64));
    gaddr dmat = galloc(128);
    GLC("glGetDoublev", 0x0BA6, dmat);
    printf("modelview d %08x\n", fnv(dmat, 128));

    static const uint8_t tex[4 * 4 * 4] = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
        0, 255, 255, 255, 255, 0, 255, 255, 9, 99, 199, 255, 50, 60, 70, 128,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        200, 100, 50, 25, 12, 6, 3, 1, 0, 0, 0, 0, 255, 255, 255, 255,
    };
    gaddr names = galloc(8);
    GLC("glGenTextures", 2, names);
    GLC("glBindTexture", 0x0DE1, rd32(names));
    GLC("glTexParameteri", 0x0DE1, 0x2801, 0x2600);
    GLC("glTexParameteri", 0x0DE1, 0x2800, 0x2600);
    GLC("glTexImage2D", 0x0DE1, 0, 0x1908, 4, 4, 0, 0x1908, 0x1401, gdata(tex, sizeof tex));
    gaddr back = galloc(sizeof tex);
    GLC("glGetTexImage", 0x0DE1, 0, 0x1908, 0x1401, back);
    printf("texture %u round trip %d\n", rd32(names), memcmp(gp(back), tex, sizeof tex) == 0);
    GLC("glEnable", 0x0DE1);
    GL0("glLoadIdentity");
    static const float quad[] = {-0.9f, -0.9f, 0, 0, 0.9f, -0.9f, 1, 0, 0.9f, 0.9f, 1, 1, -0.9f, 0.9f, 0, 1};
    gaddr q = gdata(quad, sizeof quad);
    GLC("glColor4f", fbits(1), fbits(1), fbits(1), fbits(1));
    GLC("glEnableClientState", GL_VERTEX_ARRAY);
    GLC("glEnableClientState", GL_TEXTURE_COORD_ARRAY);
    GLC("glVertexPointer", 2, 0x1406, 16, q);
    GLC("glTexCoordPointer", 2, 0x1406, 16, q + 8);
    GLC("glDrawArrays", GL_TRIANGLE_FAN, 0, 4);
    GLC("glDisableClientState", 0x8074);
    GLC("glDisableClientState", 0x8078);
    GLC("glDisable", 0x0DE1);
    report_pixels("textured client-array quad");
    gaddr ptr = galloc(4);
    GLC("glGetPointerv", GL_VERTEX_ARRAY_POINTER, ptr);
    printf("vertex array pointer is the guest's: %d\n", rd32(ptr) == q);

    uint32_t vs = shader(0x8B31, "#version 120\nattribute vec2 pos;\nvarying vec2 uv;\n"
                                 "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n");
    uint32_t fs = shader(0x8B30, "#version 120\nvarying vec2 uv;\nuniform vec4 tint;\nuniform mat2 m;\n"
                                 "void main() { vec2 t = m * uv; gl_FragColor = vec4(t, sin(uv.x * 9.0) * 0.5 + 0.5, 1.0) * tint; }\n");
    uint32_t bad = shader(0x8B30, "#version 120\nvoid main() { gl_FragColor = nope; }\n");
    gaddr log = galloc(512), len = galloc(4);
    GLC("glGetShaderInfoLog", bad, 512, len, log);
    printf("bad shader log length > 0: %d\n", rd32(len) > 0);
    uint32_t prog = GL0("glCreateProgram");
    GLC("glAttachShader", prog, vs);
    GLC("glAttachShader", prog, fs);
    GLC("glBindAttribLocation", prog, 3, gstr("pos"));
    GLC("glLinkProgram", prog);
    gaddr ok = galloc(4);
    GLC("glGetProgramiv", prog, GL_LINK_STATUS, ok);
    printf("program %u linked %u\n", prog, rd32(ok));
    gaddr shs = galloc(16), cnt = galloc(4);
    GLC("glGetAttachedShaders", prog, 4, cnt, shs);
    printf("attached %u: %u %u\n", rd32(cnt), rd32(shs), rd32(shs + 4));
    GLC("glUseProgram", prog);
    uint32_t tint = GLC("glGetUniformLocation", prog, gstr("tint"));
    uint32_t m = GLC("glGetUniformLocation", prog, gstr("m"));
    GLC("glUniform4f", tint, fbits(1), fbits(0.5f), fbits(1), fbits(1));
    static const float mm[] = {0.5f, 0.25f, -0.25f, 1.0f};
    GLC("glUniformMatrix2fv", m, 1, 0, gdata(mm, sizeof mm));
    static const float tri[] = {-1, -1, 1, -1, -1, 1, 1, 1, -1, 1, 1, -1};
    gaddr bufs = galloc(4);
    GLC("glGenBuffers", 1, bufs);
    GLC("glBindBuffer", 0x8892, rd32(bufs));
    GLC("glBufferData", 0x8892, sizeof tri, gdata(tri, sizeof tri), 0x88E4);
    gaddr sub = galloc(sizeof tri);
    GLC("glGetBufferSubData", 0x8892, 0, sizeof tri, sub);
    printf("buffer round trip %d\n", memcmp(gp(sub), tri, sizeof tri) == 0);
    GLC("glEnableVertexAttribArray", 3);
    GLC("glVertexAttribPointer", 3, 2, 0x1406, 0, 0, 0);
    GLC("glDrawArrays", 4, 0, 6);
    GLC("glBindBuffer", 0x8892, 0);
    report_pixels("shader quad");
    gaddr fv = galloc(16);
    GLC("glGetUniformfv", prog, tint, fv);
    printf("tint read back %08x\n", fnv(fv, 16));

    static const uint32_t idx[] = {0, 1, 2};
    static const float tri2[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f};
    GLC("glUniform4f", tint, fbits(0.2f), fbits(1), fbits(0.2f), fbits(1));
    GLC("glVertexAttribPointer", 3, 2, 0x1406, 0, 0, gdata(tri2, sizeof tri2));
    gaddr counts = gdata((const uint32_t[]){3, 3}, 8);
    gaddr ilist = galloc(8);
    uint32_t i0 = gdata(idx, sizeof idx);
    memcpy(gp(ilist), &i0, 4);
    memcpy(gp(ilist) + 4, &i0, 4);
    GLC("glMultiDrawElements", 4, counts, 0x1405, ilist, 2);
    report_pixels("multi draw elements from client memory");
    GLC("glBindBuffer", 0x8892, rd32(bufs));
    gaddr map = GLC("glMapBuffer", 0x8892, GL_READ_WRITE);
    printf("mapped %d first %08x error %x\n", map != 0, map ? rd32(map) : 0, GL0("glGetError"));
    gaddr mp = galloc(4);
    GLC("glGetBufferPointerv", 0x8892, 0x88BD, mp);
    printf("map pointer is the mapping: %d\n", rd32(mp) == map);
    float shrink = 0.5f;
    for (int i = 0; i < 12; i++) {
        float v;
        memcpy(&v, gp(map) + 4 * i, 4);
        v *= shrink;
        memcpy(gp(map) + 4 * i, &v, 4);
    }
    printf("unmap %u\n", GLC("glUnmapBuffer", 0x8892));
    GLC("glGetBufferPointerv", 0x8892, 0x88BD, mp);
    printf("unmapped pointer %08x\n", rd32(mp));
    GLC("glGetBufferSubData", 0x8892, 0, sizeof tri, sub);
    printf("written through the mapping %08x\n", fnv(sub, sizeof tri));
    gaddr wmap = GLC("glMapBuffer", 0x8892, GL_WRITE_ONLY);
    static const float small[] = {-0.2f, -0.2f, 0.2f, -0.2f, -0.2f, 0.2f, 0.2f, 0.2f, -0.2f, 0.2f, 0.2f, -0.2f};
    memcpy(gp(wmap), small, sizeof small);
    GLC("glUnmapBuffer", 0x8892);
    GLC("glVertexAttribPointer", 3, 2, 0x1406, 0, 0, 0);
    GLC("glUniform4f", tint, fbits(1), fbits(1), fbits(0), fbits(1));
    GLC("glDrawArrays", 4, 0, 6);
    GLC("glBindBuffer", 0x8892, 0);
    report_pixels("drawn from a write-only mapping");
    GLC("glBindBuffer", 0x8892, rd32(bufs));
    GLC("glVertexAttribPointer", 3, 2, 0x1406, 0, 0, 24);
    gaddr ebuf = galloc(4);
    GLC("glGenBuffers", 1, ebuf);
    GLC("glBindBuffer", 0x8893, rd32(ebuf));
    static const uint32_t eidx[] = {7, 0, 1, 2};
    GLC("glBufferData", 0x8893, sizeof eidx, gdata(eidx, sizeof eidx), 0x88E4);
    GLC("glUniform4f", tint, fbits(0), fbits(1), fbits(1), fbits(1));
    GLC("glDrawElements", 4, 3, 0x1405, 4);
    gaddr vp = galloc(4);
    GLC("glGetVertexAttribPointerv", 3, 0x8645, vp);
    printf("attribute offset read back %u\n", rd32(vp));
    GLC("glBindBuffer", 0x8893, 0);
    GLC("glBindBuffer", 0x8892, 0);
    report_pixels("drawn from buffer offsets");
    gaddr pbuf = galloc(4);
    GLC("glGenBuffers", 1, pbuf);
    GLC("glBindBuffer", 0x88EB, rd32(pbuf));
    GLC("glBufferData", 0x88EB, 16 + 64, 0, 0x88E9);
    GLC("glReadPixels", 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, 16);
    gaddr packed = galloc(64);
    GLC("glGetBufferSubData", 0x88EB, 16, 64, packed);
    GLC("glBindBuffer", 0x88EB, 0);
    printf("read into a pack buffer offset %08x error %x\n", fnv(packed, 64), GL0("glGetError"));
    GLC("glShaderSource", vs, (uint32_t)-1, 0, 0);
    printf("negative count error %x\n", GL0("glGetError"));
    GL0("glFinish");
}
