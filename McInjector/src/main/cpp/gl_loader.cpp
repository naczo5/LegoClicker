#include "gl_loader.h"
#include <iostream>

PFNGLACTIVETEXTUREPROC glActiveTexture_ = nullptr;
PFNGLBINDBUFFERPROC glBindBuffer_ = nullptr;
PFNGLBINDVERTEXARRAYPROC glBindVertexArray_ = nullptr;
PFNGLBUFFERDATAPROC glBufferData_ = nullptr;
PFNGLGENBUFFERSPROC glGenBuffers_ = nullptr;
PFNGLGENVERTEXARRAYSPROC glGenVertexArrays_ = nullptr;
PFNGLUSEPROGRAMPROC glUseProgram_ = nullptr;
PFNGLUNIFORM1IPROC glUniform1i_ = nullptr;
PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv_ = nullptr;

void* GetGLFuncAddress(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    if(p == 0 || (p == (void*)0x1) || (p == (void*)0x2) || (p == (void*)0x3) || (p == (void*)-1)) {
        HMODULE module = GetModuleHandleA("opengl32.dll");
        p = (void*)GetProcAddress(module, name);
    }
    return p;
}

bool LoadModernOpenGL() {
    glActiveTexture_ = (PFNGLACTIVETEXTUREPROC)GetGLFuncAddress("glActiveTexture");
    glBindBuffer_ = (PFNGLBINDBUFFERPROC)GetGLFuncAddress("glBindBuffer");
    glBindVertexArray_ = (PFNGLBINDVERTEXARRAYPROC)GetGLFuncAddress("glBindVertexArray");
    glBufferData_ = (PFNGLBUFFERDATAPROC)GetGLFuncAddress("glBufferData");
    glGenBuffers_ = (PFNGLGENBUFFERSPROC)GetGLFuncAddress("glGenBuffers");
    glGenVertexArrays_ = (PFNGLGENVERTEXARRAYSPROC)GetGLFuncAddress("glGenVertexArrays");
    glUseProgram_ = (PFNGLUSEPROGRAMPROC)GetGLFuncAddress("glUseProgram");
    glUniform1i_ = (PFNGLUNIFORM1IPROC)GetGLFuncAddress("glUniform1i");
    glUniformMatrix4fv_ = (PFNGLUNIFORMMATRIX4FVPROC)GetGLFuncAddress("glUniformMatrix4fv");
    
    return glActiveTexture_ && glBindBuffer_ && glBindVertexArray_ && glBufferData_ && glGenBuffers_ && glGenVertexArrays_ && glUseProgram_;
}
