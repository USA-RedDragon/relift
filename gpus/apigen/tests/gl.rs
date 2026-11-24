//! The OpenGL-family path against a small synthetic registry: argument
//! slots, widened pointer-sized integers, tokens, terminated lists, buffer
//! offsets, hand commands and the refusals.

use relift_gpu_apigen::{Config, generate_from};

const XML: &str = r#"<registry>
<types>
  <type>typedef unsigned int <name>GLenum</name>;</type>
  <type>typedef int <name>GLsizei</name>;</type>
  <type>typedef double <name>GLdouble</name>;</type>
  <type requires="khrplatform">typedef khronos_ssize_t <name>GLsizeiptr</name>;</type>
  <type requires="khrplatform">typedef khronos_uint8_t <name>GLubyte</name>;</type>
  <type>typedef void *<name>EGLDisplay</name>;</type>
  <type>typedef intptr_t <name>EGLAttrib</name>;</type>
</types>
<enums><enum value="0x8894" name="GL_ARRAY_BUFFER_BINDING"/></enums>
<commands>
  <command><proto>void <name>glDepthRange</name></proto><param><ptype>GLdouble</ptype> <name>n</name></param><param><ptype>GLdouble</ptype> <name>f</name></param></command>
  <command><proto>void <name>glBufferData</name></proto><param><ptype>GLenum</ptype> <name>target</name></param><param><ptype>GLsizeiptr</ptype> <name>size</name></param><param>const void *<name>data</name></param></command>
  <command><proto>void <name>glVertexPointer</name></proto><param><ptype>GLsizei</ptype> <name>stride</name></param><param>const void *<name>pointer</name></param></command>
  <command><proto>const <ptype>GLubyte</ptype> *<name>glGetString</name></proto><param><ptype>GLenum</ptype> <name>name</name></param></command>
  <command><proto><ptype>EGLDisplay</ptype> <name>eglCurrent</name></proto><param><ptype>EGLDisplay</ptype> <name>dpy</name></param><param>const <ptype>EGLAttrib</ptype> *<name>attrib_list</name></param></command>
  <command><proto>void *<name>glMapBuffer</name></proto><param><ptype>GLenum</ptype> <name>target</name></param></command>
</commands>
<feature api="gl" name="GL_VERSION_1_0"><require>
  <command name="glDepthRange"/><command name="glBufferData"/><command name="glVertexPointer"/>
  <command name="glGetString"/><command name="eglCurrent"/><command name="glMapBuffer"/>
</require></feature>
</registry>"#;

const CFG: &str = r#"[target]
abi = "i386"
api = "gl"
soname = "libGL.so.1"
proc_address = "glXGetProcAddressARB"
[registry]
file = "gl.xml"
[api]
name = "gl"
features = ["GL_VERSION_1_0"]
[thunks]
hand_commands = ["glMapBuffer"]
[thunks.offsets]
GL_ARRAY_BUFFER_BINDING = ["glVertexPointer.pointer"]
[thunks.terminated]
EGLAttrib = "0x3038"
"#;

fn config(extra: &str) -> Config {
    let mut c = Config::default();
    c.merge(CFG).unwrap();
    c.merge(extra).unwrap();
    c
}

#[test]
fn thunks_have_the_expected_shape() {
    let files = generate_from(&[XML.to_string()], &config("")).unwrap();
    assert_eq!(files[0].name, "gl.c");
    let c = &files[0].text;
    assert!(
        c.contains("uint64_t b = tg_arg64(k, 8);"),
        "a double takes two slots"
    );
    assert!(c.contains("intptr_t a1 = (intptr_t)(int32_t)tg_arg32(k, 4);"));
    assert!(c.contains("const void *a2 = tg_ptr(k, g2);"));
    assert!(c.contains("const void *a1 = tg_gl_offset(k, g1, 0x8894);"));
    assert!(c.contains("tg_i386_set_eax(c, tg_str_out(k, r));"));
    assert!(c.contains("void *a0 = tg_unhandle(tg_arg32(k, 0), \"eglCurrent\");"));
    assert!(c.contains("const intptr_t *a1 = tg_gl_list(k, g1, 0x3038);"));
    assert!(c.contains("tg_i386_set_eax(c, tg_handle(r));"));
    assert!(c.contains("uint64_t tgh_glMapBuffer(rl_ctx *c, uint8_t *mem);"));
    assert!(c.contains("{\"glMapBuffer\", tgh_glMapBuffer},"));
    assert!(c.contains("const tg_lib tg_lib_gl = {\"libGL.so.1\", 6, tg_gl_table, tg_gl_load};"));
}

#[test]
fn refusals() {
    let e = generate_from(
        &[XML.to_string()],
        &config("[thunks]\nhand_commands = [\"glNope\"]\n"),
    );
    assert!(e.unwrap_err().0.contains("glNope"));
    let e = generate_from(
        &[XML.to_string()],
        &config("[thunks.offsets]\nGL_ARRAY_BUFFER_BINDING = [\"glVertexPointer.nope\"]\n"),
    );
    assert!(e.unwrap_err().0.contains("glVertexPointer.nope"));
    let mut c = Config::default();
    c.merge(&CFG.replace("hand_commands = [\"glMapBuffer\"]", ""))
        .unwrap();
    let e = generate_from(&[XML.to_string()], &c);
    assert!(e.unwrap_err().0.contains("glMapBuffer: pointer result"));
    let e = generate_from(&[XML.to_string(), XML.to_string()], &config(""));
    assert!(
        e.is_err(),
        "one registry file more than the configuration reads"
    );
}
