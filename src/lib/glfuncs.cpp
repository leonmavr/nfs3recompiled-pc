#include <lib/glfuncs.h>
#include <x86.h>
#include <SDL3/SDL.h>

namespace win32
{

#if NFS_GL_NEEDS_LOADER

PFNGLBLENDFUNCSEPARATEPROC       glBlendFuncSeparate;
PFNGLATTACHSHADERPROC            glAttachShader;
PFNGLCOMPILESHADERPROC           glCompileShader;
PFNGLCREATEPROGRAMPROC           glCreateProgram;
PFNGLDELETEPROGRAMPROC           glDeleteProgram;
PFNGLCREATESHADERPROC            glCreateShader;
PFNGLDELETESHADERPROC            glDeleteShader;
PFNGLGETSHADERIVPROC             glGetShaderiv;
PFNGLGETPROGRAMIVPROC            glGetProgramiv;
PFNGLGETPROGRAMINFOLOGPROC       glGetProgramInfoLog;
PFNGLGETSHADERINFOLOGPROC        glGetShaderInfoLog;
PFNGLLINKPROGRAMPROC             glLinkProgram;
PFNGLSHADERSOURCEPROC            glShaderSource;
PFNGLUSEPROGRAMPROC              glUseProgram;
PFNGLGETATTRIBLOCATIONPROC       glGetAttribLocation;
PFNGLVERTEXATTRIBPOINTERPROC     glVertexAttribPointer;
PFNGLGENVERTEXARRAYSPROC         glGenVertexArrays;
PFNGLDELETEVERTEXARRAYSPROC      glDeleteVertexArrays;
PFNGLBINDVERTEXARRAYPROC         glBindVertexArray;
PFNGLBUFFERDATAPROC              glBufferData;
PFNGLGENBUFFERSPROC              glGenBuffers;
PFNGLDELETEBUFFERSPROC           glDeleteBuffers;
PFNGLBINDBUFFERPROC              glBindBuffer;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
PFNGLGETUNIFORMLOCATIONPROC      glGetUniformLocation;
PFNGLUNIFORMMATRIX4FVPROC        glUniformMatrix4fv;
PFNGLUNIFORM1IPROC               glUniform1i;
PFNGLUNIFORM2FPROC               glUniform2f;
PFNGLUNIFORM3FPROC               glUniform3f;
PFNGLUNIFORM3IPROC               glUniform3i;
PFNGLUNIFORM1FPROC               glUniform1f;
PFNGLUNIFORM1FVPROC              glUniform1fv;
NfsCullFaceProc                  glCullFace;
NfsFrontFaceProc                 glFrontFace;
NfsScissorProc                   glScissor;
PFNGLACTIVETEXTUREPROC           glActiveTexture;
PFNGLTEXSTORAGE2DPROC            glTexStorage2D;
PFNGLGENFRAMEBUFFERSPROC         glGenFramebuffers;
PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers;
PFNGLBINDFRAMEBUFFERPROC         glBindFramebuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC    glFramebufferTexture2D;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC glGetFramebufferAttachmentParameteriv;
PFNGLBLITFRAMEBUFFERPROC         glBlitFramebuffer;
PFNGLGENRENDERBUFFERSPROC        glGenRenderbuffers;
PFNGLDELETERENDERBUFFERSPROC     glDeleteRenderbuffers;
PFNGLBINDRENDERBUFFERPROC        glBindRenderbuffer;
PFNGLRENDERBUFFERSTORAGEPROC     glRenderbufferStorage;
PFNGLDRAWBUFFERSPROC             glDrawBuffers;
PFNGLGENSAMPLERSPROC             glGenSamplers;
PFNGLDELETESAMPLERSPROC          glDeleteSamplers;
PFNGLBINDSAMPLERPROC             glBindSampler;
PFNGLSAMPLERPARAMETERIPROC       glSamplerParameteri;
PFNGLSAMPLERPARAMETERFPROC       glSamplerParameterf;
PFNGLGENERATEMIPMAPPROC          glGenerateMipmap;

namespace
{

template< typename T >
void load(T& fn, const char* name)
{
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (!fn)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GL entry point not available: %s", name);
    }
    NFS2_ASSERT(fn);
}

}

void loadGlFunctions()
{
    static bool s_loaded = false;
    if (s_loaded)
        return;

    load(glBlendFuncSeparate,       "glBlendFuncSeparate");
    load(glAttachShader,            "glAttachShader");
    load(glCompileShader,           "glCompileShader");
    load(glCreateProgram,           "glCreateProgram");
    load(glDeleteProgram,           "glDeleteProgram");
    load(glCreateShader,            "glCreateShader");
    load(glDeleteShader,            "glDeleteShader");
    load(glGetShaderiv,             "glGetShaderiv");
    load(glGetProgramiv,            "glGetProgramiv");
    load(glGetProgramInfoLog,       "glGetProgramInfoLog");
    load(glGetShaderInfoLog,        "glGetShaderInfoLog");
    load(glLinkProgram,             "glLinkProgram");
    load(glShaderSource,            "glShaderSource");
    load(glUseProgram,              "glUseProgram");
    load(glGetAttribLocation,       "glGetAttribLocation");
    load(glVertexAttribPointer,     "glVertexAttribPointer");
    load(glGenVertexArrays,         "glGenVertexArrays");
    load(glDeleteVertexArrays,      "glDeleteVertexArrays");
    load(glBindVertexArray,         "glBindVertexArray");
    load(glBufferData,              "glBufferData");
    load(glGenBuffers,              "glGenBuffers");
    load(glDeleteBuffers,           "glDeleteBuffers");
    load(glBindBuffer,              "glBindBuffer");
    load(glEnableVertexAttribArray, "glEnableVertexAttribArray");
    load(glGetUniformLocation,      "glGetUniformLocation");
    load(glUniformMatrix4fv,        "glUniformMatrix4fv");
    load(glUniform1i,               "glUniform1i");
    load(glUniform2f,               "glUniform2f");
    load(glUniform3f,               "glUniform3f");
    load(glUniform3i,               "glUniform3i");
    load(glUniform1f,               "glUniform1f");
    load(glUniform1fv,              "glUniform1fv");
    load(glCullFace,                "glCullFace");
    load(glFrontFace,               "glFrontFace");
    load(glScissor,                 "glScissor");
    load(glActiveTexture,           "glActiveTexture");
    load(glTexStorage2D,            "glTexStorage2D");
    load(glGenFramebuffers,         "glGenFramebuffers");
    load(glDeleteFramebuffers,      "glDeleteFramebuffers");
    load(glBindFramebuffer,         "glBindFramebuffer");
    load(glFramebufferTexture2D,    "glFramebufferTexture2D");
    load(glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    load(glGetFramebufferAttachmentParameteriv, "glGetFramebufferAttachmentParameteriv");
    load(glBlitFramebuffer,         "glBlitFramebuffer");
    load(glGenRenderbuffers,        "glGenRenderbuffers");
    load(glDeleteRenderbuffers,     "glDeleteRenderbuffers");
    load(glBindRenderbuffer,        "glBindRenderbuffer");
    load(glRenderbufferStorage,     "glRenderbufferStorage");
    load(glDrawBuffers,             "glDrawBuffers");
    load(glGenSamplers,             "glGenSamplers");
    load(glDeleteSamplers,          "glDeleteSamplers");
    load(glBindSampler,             "glBindSampler");
    load(glSamplerParameteri,       "glSamplerParameteri");
    load(glSamplerParameterf,       "glSamplerParameterf");
    load(glGenerateMipmap,          "glGenerateMipmap");

    s_loaded = true;
}

#else

void loadGlFunctions()
{
    /* GLES 3.0 links these directly. */
}

#endif

}
