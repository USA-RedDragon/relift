/**
 * @file glx_scenario.h
 * @brief The GL scenario over GLX on an X server: a GLX pbuffer and context
 * from a chosen FBConfig, the visual queries, then gl_scenario.h's draws.
 * Built natively for i386 (glx_native.c) and through the thunks
 * (glx_harness.c). The includer gives the GL scenario's needs plus x_open,
 * x_screen and x_close (the guest's own X11).
 */
enum {
    GLX_VENDOR_ = 1,
    GLX_RGBA_ = 4,
    GLX_DOUBLEBUFFER_ = 5,
    GLX_RED_SIZE_ = 8,
    GLX_GREEN_SIZE_ = 9,
    GLX_BLUE_SIZE_ = 10,
    GLX_ALPHA_SIZE_ = 11,
    GLX_DEPTH_SIZE_ = 12,
    GLX_DRAWABLE_TYPE_ = 0x8010,
    GLX_RENDER_TYPE_ = 0x8011,
    GLX_VISUAL_ID_ = 0x800B,
    GLX_PBUFFER_BIT_ = 4,
    GLX_RGBA_BIT_ = 1,
    GLX_RGBA_TYPE_ = 0x8014,
    GLX_PBUFFER_WIDTH_ = 0x8041,
    GLX_PBUFFER_HEIGHT_ = 0x8040,
    GLX_WIDTH_ = 0x801D,
    VI_visualid = 4,
    VI_depth = 12,
    VI_class = 16,
};

static gaddr glx_ints(const int32_t *v, size_t n)
{
    gaddr a = galloc(4 * n);
    memcpy(gp(a), v, 4 * n);
    return a;
}

static gaddr glx_dpy, glx_pb, glx_ctx;

static void glx_context(void)
{
    gaddr dpy = x_open();
    int screen = x_screen(dpy);
    gaddr maj = galloc(4), min = galloc(4);
    int ok = (int)GLC("glXQueryVersion", dpy, maj, min);
    printf("GLX %d %u.%u vendor %s\n", ok, rd32(maj), rd32(min), (const char *)gp(GLC("glXQueryServerString", dpy, screen, GLX_VENDOR_)));
    const int32_t va[] = {GLX_RGBA_, GLX_DOUBLEBUFFER_, GLX_DEPTH_SIZE_, 24, 0};
    gaddr vi = GLC("glXChooseVisual", dpy, screen, glx_ints(va, 5));
    printf("visual %d depth %u class %u\n", vi != 0, vi ? rd32(vi + VI_depth) : 0, vi ? rd32(vi + VI_class) : 0);
    const int32_t fa[] = {GLX_DRAWABLE_TYPE_, GLX_PBUFFER_BIT_, GLX_RENDER_TYPE_, GLX_RGBA_BIT_, GLX_RED_SIZE_, 8, GLX_GREEN_SIZE_, 8,
                          GLX_BLUE_SIZE_, 8, GLX_ALPHA_SIZE_, 8, GLX_DEPTH_SIZE_, 24, 0};
    gaddr n = galloc(4);
    gaddr cfgs = GLC("glXChooseFBConfig", dpy, screen, glx_ints(fa, 15), n);
    printf("fbconfigs %d\n", cfgs != 0 && rd32(n) > 0);
    gaddr cfg = rd32(cfgs);
    gaddr vid = galloc(4);
    GLC("glXGetFBConfigAttrib", dpy, cfg, GLX_VISUAL_ID_, vid);
    gaddr fvi = GLC("glXGetVisualFromFBConfig", dpy, cfg);
    printf("fbconfig visual matches %d\n", fvi && rd32(fvi + VI_visualid) == rd32(vid));
    const int32_t pa[] = {GLX_PBUFFER_WIDTH_, W, GLX_PBUFFER_HEIGHT_, H, 0};
    gaddr pb = GLC("glXCreatePbuffer", dpy, cfg, glx_ints(pa, 5));
    gaddr ctx = GLC("glXCreateNewContext", dpy, cfg, GLX_RGBA_TYPE_, 0, 1);
    ok = (int)GLC("glXMakeContextCurrent", dpy, pb, pb, ctx);
    gaddr w = galloc(4);
    GLC("glXQueryDrawable", dpy, pb, GLX_WIDTH_, w);
    int same_dpy = GLC("glXGetCurrentDisplay") == dpy;
    int direct = (int)GLC("glXIsDirect", dpy, ctx);
    gaddr name = galloc(8);
    memcpy(gp(name), "glClear", 8);
    int proc = GLC("glXGetProcAddressARB", name) != 0;
    printf("current %d width %u display %d direct %d proc %d\n", ok, rd32(w), same_dpy, direct, proc);
    glx_dpy = dpy;
    glx_pb = pb;
    glx_ctx = ctx;
}

/** @brief Releases the context and pbuffer before the program ends. */
static void glx_teardown(void)
{
    GLC("glXMakeContextCurrent", glx_dpy, 0, 0, 0);
    GLC("glXDestroyContext", glx_dpy, glx_ctx);
    GLC("glXDestroyPbuffer", glx_dpy, glx_pb);
    printf("released\n");
}
