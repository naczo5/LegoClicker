// Minimal OpenGL 3 Function Loader for bridge_121
#pragma once
#include <windows.h>
#include <GL/gl.h>

// Typedefs for Modern OpenGL functions we need for ImGui and Custom Rendering
typedef void (WINAPI * PFNGLACTIVETEXTUREPROC) (GLenum texture);
typedef void (WINAPI * PFNGLBINDBUFFERPROC) (GLenum target, GLuint buffer);
typedef void (WINAPI * PFNGLBINDVERTEXARRAYPROC) (GLuint array);
typedef void (WINAPI * PFNGLBUFFERDATAPROC) (GLenum target, ptrdiff_t size, const GLvoid *data, GLenum usage);
typedef void (WINAPI * PFNGLGENBUFFERSPROC) (GLsizei n, GLuint *buffers);
typedef void (WINAPI * PFNGLGENVERTEXARRAYSPROC) (GLsizei n, GLuint *arrays);
typedef void (WINAPI * PFNGLUSEPROGRAMPROC) (GLuint program);
typedef void (WINAPI * PFNGLUNIFORM1IPROC) (GLint location, GLint v0);
typedef void (WINAPI * PFNGLUNIFORMMATRIX4FVPROC) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

// Globals
extern PFNGLACTIVETEXTUREPROC glActiveTexture_;
extern PFNGLBINDBUFFERPROC glBindBuffer_;
extern PFNGLBINDVERTEXARRAYPROC glBindVertexArray_;
extern PFNGLBUFFERDATAPROC glBufferData_;
extern PFNGLGENBUFFERSPROC glGenBuffers_;
extern PFNGLGENVERTEXARRAYSPROC glGenVertexArrays_;
extern PFNGLUSEPROGRAMPROC glUseProgram_;
extern PFNGLUNIFORM1IPROC glUniform1i_;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv_;

bool LoadModernOpenGL();
