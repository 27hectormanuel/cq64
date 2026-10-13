// SPDX-License-Identifier: GPL-3.0-or-later
// OpenGL ES 1.x / 2.0 imports. Arguments are mostly scalars; pointer arguments are guest
// addresses, except vertex/index "pointers" that are buffer offsets while a VBO is bound.
#include "abi.h"

#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
GLuint g_array_buffer = 0, g_element_buffer = 0;

const void *client_ptr(u32 p, GLuint bound) {
    return bound ? (const void *)(uintptr_t)p : g2h(p);
}

void t_glBindBuffer(GLenum target, GLuint b) {
    if (target == GL_ARRAY_BUFFER) g_array_buffer = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = b;
    glBindBuffer(target, b);
}
THUNK(glBindBuffer, t_glBindBuffer);
void t_glDeleteBuffers(GLsizei n, G<const GLuint> b) {
    for (GLsizei i = 0; i < n; i++) {
        if (b.get()[i] == g_array_buffer) g_array_buffer = 0;
        if (b.get()[i] == g_element_buffer) g_element_buffer = 0;
    }
    glDeleteBuffers(n, b);
}
THUNK(glDeleteBuffers, t_glDeleteBuffers);

void t_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, u32 p) {
    glVertexAttribPointer(i, size, type, norm, stride, client_ptr(p, g_array_buffer));
}
THUNK(glVertexAttribPointer, t_glVertexAttribPointer);
void t_glVertexPointer(GLint size, GLenum type, GLsizei stride, u32 p) {
    glVertexPointer(size, type, stride, client_ptr(p, g_array_buffer));
}
THUNK(glVertexPointer, t_glVertexPointer);
void t_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, u32 p) {
    glTexCoordPointer(size, type, stride, client_ptr(p, g_array_buffer));
}
THUNK(glTexCoordPointer, t_glTexCoordPointer);
void t_glColorPointer(GLint size, GLenum type, GLsizei stride, u32 p) {
    glColorPointer(size, type, stride, client_ptr(p, g_array_buffer));
}
THUNK(glColorPointer, t_glColorPointer);
void t_glNormalPointer(GLenum type, GLsizei stride, u32 p) {
    glNormalPointer(type, stride, client_ptr(p, g_array_buffer));
}
THUNK(glNormalPointer, t_glNormalPointer);
void t_glDrawElements(GLenum mode, GLsizei count, GLenum type, u32 p) {
    glDrawElements(mode, count, type, client_ptr(p, g_element_buffer));
}
THUNK(glDrawElements, t_glDrawElements);

std::mutex g_str_lock;
std::unordered_map<GLenum, u32> g_strings;
u32 t_glGetString(GLenum name) {
    std::lock_guard<std::mutex> lk(g_str_lock);
    auto it = g_strings.find(name);
    if (it != g_strings.end()) return it->second;
    const char *s = (const char *)glGetString(name);
    if (!s) return 0;
    LOGI("glGetString(0x%x) = %s", name, s);
    return g_strings[name] = mem::hstrdup(s);
}
THUNK(glGetString, t_glGetString);

void t_glShaderSource(GLuint sh, GLsizei count, G<const u32> strs, G<const GLint> lens) {
    std::vector<const char *> v(count);
    for (GLsizei i = 0; i < count; i++) v[i] = (const char *)g2h(strs.get()[i]);
    glShaderSource(sh, count, v.data(), lens);
}
THUNK(glShaderSource, t_glShaderSource);

// Buffer mapping: the guest writes into a staging copy that is uploaded on unmap
// (OES_mapbuffer / EXT_map_buffer_range mappings are write-only in practice here).
struct Mapping {
    u32 guest = 0, size = 0, offset = 0;
};
std::unordered_map<GLenum, Mapping> g_mappings;
u32 map_buffer(GLenum target, u32 offset, u32 length) {
    if (!length) {
        GLint size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        length = (u32)size - offset;
    }
    Mapping m{mem::map(length), length, offset};
    g_mappings[target] = m;
    return m.guest;
}
u32 t_glMapBufferOES(GLenum target, GLenum) { return map_buffer(target, 0, 0); }
THUNK(glMapBufferOES, t_glMapBufferOES);
u32 t_glMapBufferRangeEXT(GLenum target, s32 offset, s32 length, GLbitfield) {
    return map_buffer(target, (u32)offset, (u32)length);
}
THUNK(glMapBufferRangeEXT, t_glMapBufferRangeEXT);
GLboolean t_glUnmapBufferOES(GLenum target) {
    auto it = g_mappings.find(target);
    if (it == g_mappings.end()) return GL_FALSE;
    glBufferSubData(target, it->second.offset, it->second.size, g2h(it->second.guest));
    mem::unmap(it->second.guest, it->second.size);
    g_mappings.erase(it);
    return GL_TRUE;
}
THUNK(glUnmapBufferOES, t_glUnmapBufferOES);
THUNK(glUnmapBufferEXT, t_glUnmapBufferOES);
void t_glGetBufferPointervOES(GLenum target, GLenum, G<u32> out) {
    auto it = g_mappings.find(target);
    *out.get() = it == g_mappings.end() ? 0 : it->second.guest;
}
THUNK(glGetBufferPointervOES, t_glGetBufferPointervOES);
void t_glFlushMappedBufferRangeEXT(GLenum, s32, s32) {}
THUNK(glFlushMappedBufferRangeEXT, t_glFlushMappedBufferRangeEXT);

// ---- straightforward pass-through ----
#define P(T) G<T>
void t_glActiveTexture(GLenum a) { glActiveTexture(a); } THUNK(glActiveTexture, t_glActiveTexture);
void t_glAttachShader(GLuint p, GLuint s) { glAttachShader(p, s); } THUNK(glAttachShader, t_glAttachShader);
void t_glBindAttribLocation(GLuint p, GLuint i, P(const char) n) { glBindAttribLocation(p, i, n); } THUNK(glBindAttribLocation, t_glBindAttribLocation);
void t_glBindFramebuffer(GLenum t, GLuint f) { glBindFramebuffer(t, f); } THUNK(glBindFramebuffer, t_glBindFramebuffer);
void t_glBindRenderbuffer(GLenum t, GLuint r) { glBindRenderbuffer(t, r); } THUNK(glBindRenderbuffer, t_glBindRenderbuffer);
void t_glBindTexture(GLenum t, GLuint x) { glBindTexture(t, x); } THUNK(glBindTexture, t_glBindTexture);
void t_glBlendEquationSeparate(GLenum a, GLenum b) { glBlendEquationSeparate(a, b); } THUNK(glBlendEquationSeparate, t_glBlendEquationSeparate);
void t_glBlendFunc(GLenum a, GLenum b) { glBlendFunc(a, b); } THUNK(glBlendFunc, t_glBlendFunc);
void t_glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { glBlendFuncSeparate(a, b, c, d); } THUNK(glBlendFuncSeparate, t_glBlendFuncSeparate);
THUNK(glBlendFuncSeparateOES, t_glBlendFuncSeparate);
void t_glBufferData(GLenum t, s32 s, P(const void) d, GLenum u) { glBufferData(t, s, d, u); } THUNK(glBufferData, t_glBufferData);
void t_glBufferSubData(GLenum t, s32 o, s32 s, P(const void) d) { glBufferSubData(t, o, s, d); } THUNK(glBufferSubData, t_glBufferSubData);
GLenum t_glCheckFramebufferStatus(GLenum t) { return glCheckFramebufferStatus(t); } THUNK(glCheckFramebufferStatus, t_glCheckFramebufferStatus);
THUNK(glCheckFramebufferStatusOES, t_glCheckFramebufferStatus);
void t_glClear(GLbitfield m) { glClear(m); } THUNK(glClear, t_glClear);
void t_glClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); } THUNK(glClearColor, t_glClearColor);
void t_glClearDepthf(float d) { glClearDepthf(d); } THUNK(glClearDepthf, t_glClearDepthf);
void t_glClearStencil(GLint s) { glClearStencil(s); } THUNK(glClearStencil, t_glClearStencil);
void t_glColor4f(float r, float g, float b, float a) { glColor4f(r, g, b, a); } THUNK(glColor4f, t_glColor4f);
void t_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { glColorMask(r, g, b, a); } THUNK(glColorMask, t_glColorMask);
void t_glCompileShader(GLuint s) { glCompileShader(s); } THUNK(glCompileShader, t_glCompileShader);
void t_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, P(const void) d) { glCompressedTexImage2D(t, l, f, w, h, b, s, d); } THUNK(glCompressedTexImage2D, t_glCompressedTexImage2D);
void t_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) { glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h); } THUNK(glCopyTexSubImage2D, t_glCopyTexSubImage2D);
GLuint t_glCreateProgram() { return glCreateProgram(); } THUNK(glCreateProgram, t_glCreateProgram);
GLuint t_glCreateShader(GLenum t) { return glCreateShader(t); } THUNK(glCreateShader, t_glCreateShader);
void t_glCullFace(GLenum m) { glCullFace(m); } THUNK(glCullFace, t_glCullFace);
void t_glDeleteFramebuffers(GLsizei n, P(const GLuint) f) { glDeleteFramebuffers(n, f); } THUNK(glDeleteFramebuffers, t_glDeleteFramebuffers);
THUNK(glDeleteFramebuffersOES, t_glDeleteFramebuffers);
void t_glDeleteProgram(GLuint p) { glDeleteProgram(p); } THUNK(glDeleteProgram, t_glDeleteProgram);
void t_glDeleteRenderbuffers(GLsizei n, P(const GLuint) r) { glDeleteRenderbuffers(n, r); } THUNK(glDeleteRenderbuffers, t_glDeleteRenderbuffers);
void t_glDeleteShader(GLuint s) { glDeleteShader(s); } THUNK(glDeleteShader, t_glDeleteShader);
void t_glDeleteTextures(GLsizei n, P(const GLuint) t) { glDeleteTextures(n, t); } THUNK(glDeleteTextures, t_glDeleteTextures);
void t_glDepthFunc(GLenum f) { glDepthFunc(f); } THUNK(glDepthFunc, t_glDepthFunc);
void t_glDepthMask(GLboolean f) { glDepthMask(f); } THUNK(glDepthMask, t_glDepthMask);
void t_glDisable(GLenum c) { glDisable(c); } THUNK(glDisable, t_glDisable);
void t_glDisableClientState(GLenum c) { glDisableClientState(c); } THUNK(glDisableClientState, t_glDisableClientState);
void t_glDisableVertexAttribArray(GLuint i) { glDisableVertexAttribArray(i); } THUNK(glDisableVertexAttribArray, t_glDisableVertexAttribArray);
void t_glDrawArrays(GLenum m, GLint f, GLsizei c) { glDrawArrays(m, f, c); } THUNK(glDrawArrays, t_glDrawArrays);
void t_glDrawTexfOES(float x, float y, float z, float w, float h) { glDrawTexfOES(x, y, z, w, h); } THUNK(glDrawTexfOES, t_glDrawTexfOES);
void t_glEnable(GLenum c) { glEnable(c); } THUNK(glEnable, t_glEnable);
void t_glEnableClientState(GLenum c) { glEnableClientState(c); } THUNK(glEnableClientState, t_glEnableClientState);
void t_glEnableVertexAttribArray(GLuint i) { glEnableVertexAttribArray(i); } THUNK(glEnableVertexAttribArray, t_glEnableVertexAttribArray);
void t_glFinish() { glFinish(); } THUNK(glFinish, t_glFinish);
void t_glFlush() { glFlush(); } THUNK(glFlush, t_glFlush);
void t_glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint r) { glFramebufferRenderbuffer(t, a, rt, r); } THUNK(glFramebufferRenderbuffer, t_glFramebufferRenderbuffer);
void t_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint x, GLint l) { glFramebufferTexture2D(t, a, tt, x, l); } THUNK(glFramebufferTexture2D, t_glFramebufferTexture2D);
THUNK(glFramebufferTexture2DOES, t_glFramebufferTexture2D);
THUNK(glBindFramebufferOES, t_glBindFramebuffer);
void t_glFrontFace(GLenum m) { glFrontFace(m); } THUNK(glFrontFace, t_glFrontFace);
void t_glGenBuffers(GLsizei n, P(GLuint) b) { glGenBuffers(n, b); } THUNK(glGenBuffers, t_glGenBuffers);
void t_glGenFramebuffers(GLsizei n, P(GLuint) f) { glGenFramebuffers(n, f); } THUNK(glGenFramebuffers, t_glGenFramebuffers);
THUNK(glGenFramebuffersOES, t_glGenFramebuffers);
void t_glGenRenderbuffers(GLsizei n, P(GLuint) r) { glGenRenderbuffers(n, r); } THUNK(glGenRenderbuffers, t_glGenRenderbuffers);
void t_glGenTextures(GLsizei n, P(GLuint) t) { glGenTextures(n, t); } THUNK(glGenTextures, t_glGenTextures);
void t_glGetBooleanv(GLenum p, P(GLboolean) v) { glGetBooleanv(p, v); } THUNK(glGetBooleanv, t_glGetBooleanv);
GLenum t_glGetError() { return glGetError(); } THUNK(glGetError, t_glGetError);
void t_glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, P(GLint) v) { glGetFramebufferAttachmentParameteriv(t, a, p, v); } THUNK(glGetFramebufferAttachmentParameteriv, t_glGetFramebufferAttachmentParameteriv);
void t_glGetIntegerv(GLenum p, P(GLint) v) { glGetIntegerv(p, v); } THUNK(glGetIntegerv, t_glGetIntegerv);
void t_glGetProgramInfoLog(GLuint p, GLsizei n, P(GLsizei) l, P(char) s) { glGetProgramInfoLog(p, n, l, s); } THUNK(glGetProgramInfoLog, t_glGetProgramInfoLog);
void t_glGetProgramiv(GLuint p, GLenum n, P(GLint) v) { glGetProgramiv(p, n, v); } THUNK(glGetProgramiv, t_glGetProgramiv);
void t_glGetRenderbufferParameteriv(GLenum t, GLenum p, P(GLint) v) { glGetRenderbufferParameteriv(t, p, v); } THUNK(glGetRenderbufferParameteriv, t_glGetRenderbufferParameteriv);
void t_glGetShaderInfoLog(GLuint s, GLsizei n, P(GLsizei) l, P(char) x) { glGetShaderInfoLog(s, n, l, x); } THUNK(glGetShaderInfoLog, t_glGetShaderInfoLog);
void t_glGetShaderiv(GLuint s, GLenum n, P(GLint) v) { glGetShaderiv(s, n, v); } THUNK(glGetShaderiv, t_glGetShaderiv);
GLint t_glGetUniformLocation(GLuint p, P(const char) n) { return glGetUniformLocation(p, n); } THUNK(glGetUniformLocation, t_glGetUniformLocation);
GLboolean t_glIsFramebuffer(GLuint f) { return glIsFramebuffer(f); } THUNK(glIsFramebuffer, t_glIsFramebuffer);
GLboolean t_glIsRenderbuffer(GLuint r) { return glIsRenderbuffer(r); } THUNK(glIsRenderbuffer, t_glIsRenderbuffer);
void t_glLineWidth(float w) { glLineWidth(w); } THUNK(glLineWidth, t_glLineWidth);
void t_glLinkProgram(GLuint p) { glLinkProgram(p); } THUNK(glLinkProgram, t_glLinkProgram);
void t_glLoadIdentity() { glLoadIdentity(); } THUNK(glLoadIdentity, t_glLoadIdentity);
void t_glMatrixMode(GLenum m) { glMatrixMode(m); } THUNK(glMatrixMode, t_glMatrixMode);
void t_glOrthof(float l, float r, float b, float t, float n, float f) { glOrthof(l, r, b, t, n, f); } THUNK(glOrthof, t_glOrthof);
void t_glPixelStorei(GLenum p, GLint v) { glPixelStorei(p, v); } THUNK(glPixelStorei, t_glPixelStorei);
void t_glPopMatrix() { glPopMatrix(); } THUNK(glPopMatrix, t_glPopMatrix);
void t_glPushMatrix() { glPushMatrix(); } THUNK(glPushMatrix, t_glPushMatrix);
void t_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, P(void) d) { glReadPixels(x, y, w, h, f, t, d); } THUNK(glReadPixels, t_glReadPixels);
void t_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { glRenderbufferStorage(t, f, w, h); } THUNK(glRenderbufferStorage, t_glRenderbufferStorage);
void t_glRotatef(float a, float x, float y, float z) { glRotatef(a, x, y, z); } THUNK(glRotatef, t_glRotatef);
void t_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { glScissor(x, y, w, h); } THUNK(glScissor, t_glScissor);
void t_glShaderBinary(GLsizei n, P(const GLuint) s, GLenum f, P(const void) b, GLsizei l) { glShaderBinary(n, s, f, b, l); } THUNK(glShaderBinary, t_glShaderBinary);
void t_glStencilFunc(GLenum f, GLint r, GLuint m) { glStencilFunc(f, r, m); } THUNK(glStencilFunc, t_glStencilFunc);
void t_glStencilFuncSeparate(GLenum fa, GLenum f, GLint r, GLuint m) { glStencilFuncSeparate(fa, f, r, m); } THUNK(glStencilFuncSeparate, t_glStencilFuncSeparate);
void t_glStencilMask(GLuint m) { glStencilMask(m); } THUNK(glStencilMask, t_glStencilMask);
void t_glStencilMaskSeparate(GLenum f, GLuint m) { glStencilMaskSeparate(f, m); } THUNK(glStencilMaskSeparate, t_glStencilMaskSeparate);
void t_glStencilOp(GLenum a, GLenum b, GLenum c) { glStencilOp(a, b, c); } THUNK(glStencilOp, t_glStencilOp);
void t_glStencilOpSeparate(GLenum f, GLenum a, GLenum b, GLenum c) { glStencilOpSeparate(f, a, b, c); } THUNK(glStencilOpSeparate, t_glStencilOpSeparate);
void t_glTexEnvf(GLenum t, GLenum p, float v) { glTexEnvf(t, p, v); } THUNK(glTexEnvf, t_glTexEnvf);
void t_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, P(const void) d) { glTexImage2D(t, l, i, w, h, b, f, ty, d); } THUNK(glTexImage2D, t_glTexImage2D);
void t_glTexParameteri(GLenum t, GLenum p, GLint v) { glTexParameteri(t, p, v); } THUNK(glTexParameteri, t_glTexParameteri);
void t_glTexParameteriv(GLenum t, GLenum p, P(const GLint) v) { glTexParameteriv(t, p, v); } THUNK(glTexParameteriv, t_glTexParameteriv);
void t_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, P(const void) d) { glTexSubImage2D(t, l, x, y, w, h, f, ty, d); } THUNK(glTexSubImage2D, t_glTexSubImage2D);
void t_glTranslatef(float x, float y, float z) { glTranslatef(x, y, z); } THUNK(glTranslatef, t_glTranslatef);
void t_glUniform1i(GLint l, GLint v) { glUniform1i(l, v); } THUNK(glUniform1i, t_glUniform1i);
void t_glUniform1iv(GLint l, GLsizei n, P(const GLint) v) { glUniform1iv(l, n, v); } THUNK(glUniform1iv, t_glUniform1iv);
void t_glUniform4f(GLint l, float a, float b, float c, float d) { glUniform4f(l, a, b, c, d); } THUNK(glUniform4f, t_glUniform4f);
void t_glUniform4fv(GLint l, GLsizei n, P(const GLfloat) v) { glUniform4fv(l, n, v); } THUNK(glUniform4fv, t_glUniform4fv);
void t_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean tr, P(const GLfloat) v) { glUniformMatrix4fv(l, n, tr, v); } THUNK(glUniformMatrix4fv, t_glUniformMatrix4fv);
void t_glUseProgram(GLuint p) { glUseProgram(p); } THUNK(glUseProgram, t_glUseProgram);
void t_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { glViewport(x, y, w, h); } THUNK(glViewport, t_glViewport);
#undef P
}  // namespace
