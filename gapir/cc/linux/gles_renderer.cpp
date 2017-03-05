/*
 * Copyright (C) 2017 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gapir/cc/gles_gfx_api.h"
#include "gapir/cc/gles_renderer.h"

#include "core/cc/gl/formats.h"
#include "core/cc/log.h"

#include <cstring>
#include <X11/Xresource.h>

namespace gapir {
namespace {

typedef XID GLXPbuffer;
typedef XID GLXDrawable;
typedef /*struct __GLXcontextRec*/ void *GLXContext;
typedef /*struct __GLXFBConfigRec*/ void *GLXFBConfig;

enum {
    // Used by glXChooseFBConfig.
    GLX_RED_SIZE      = 8,
    GLX_GREEN_SIZE    = 9,
    GLX_BLUE_SIZE     = 10,
    GLX_ALPHA_SIZE    = 11,
    GLX_DEPTH_SIZE    = 12,
    GLX_STENCIL_SIZE  = 13,
    GLX_DRAWABLE_TYPE = 0x8010,
    GLX_RENDER_TYPE   = 0x8011,
    GLX_RGBA_BIT      = 0x00000001,
    GLX_PBUFFER_BIT   = 0x00000004,

    // Used by glXCreateNewContext.
    GLX_RGBA_TYPE = 0x8014,

    // Used by glXCreatePbuffer.
    GLX_PBUFFER_HEIGHT = 0x8040,
    GLX_PBUFFER_WIDTH  = 0x8041,

    // Attribute name for glXCreateContextAttribsARB.
    GLX_CONTEXT_MAJOR_VERSION_ARB             = 0x2091,
    GLX_CONTEXT_MINOR_VERSION_ARB             = 0x2092,
    GLX_CONTEXT_FLAGS_ARB                     = 0x2094,
    GLX_CONTEXT_PROFILE_MASK_ARB              = 0x9126,

    // Attribute value for glXCreateContextAttribsARB.
    GLX_CONTEXT_DEBUG_BIT_ARB                 = 0x0001,
    GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB    = 0x0002,
    GLX_CONTEXT_CORE_PROFILE_BIT_ARB          = 0x0001,
    GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x0002,
};

extern "C" {

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attrib_list, int *nelements);
GLXContext glXCreateNewContext(Display *dpy, GLXFBConfig config, int render_type,
                               GLXContext share_list, Bool direct);
GLXPbuffer glXCreatePbuffer(Display *dpy, GLXFBConfig config, const int *attrib_list);
void glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf);
Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx);
Bool glXQueryVersion(Display *dpy, int *maj, int *min);
void glXDestroyContext(Display *dpy, GLXContext ctx);
void* glXGetProcAddress(const char* procName);
typedef GLXContext (*glXCreateContextAttribsARBProc)(Display *dpy, GLXFBConfig config, GLXContext share_context,
                                                     Bool direct, const int *attrib_list);

} // extern "C"

class GlesRendererImpl : public GlesRenderer {
public:
    GlesRendererImpl();
    virtual ~GlesRendererImpl() override;

    virtual Api* api() override;
    virtual void setBackbuffer(Backbuffer backbuffer) override;
    virtual void bind() override;
    virtual void unbind() override;
    virtual const char* name() override;
    virtual const char* extensions() override;
    virtual const char* vendor() override;
    virtual const char* version() override;

private:
    void reset();
    void createPbuffer(int width, int height);

    Backbuffer mBackbuffer;
    bool mBound;
    bool mNeedsResolve;
    Gles mApi;
    std::string mExtensions;
    bool mQueriedExtensions;

    Display *mDisplay;
    GLXContext mContext;
    GLXPbuffer mPbuffer;
    GLXFBConfig mFBConfig;
};

GlesRendererImpl::GlesRendererImpl()
        : mBound(false)
        , mNeedsResolve(false)
        , mDisplay(nullptr)
        , mContext(nullptr)
        , mPbuffer(0) {

    mDisplay = XOpenDisplay(nullptr);
    if (mDisplay == nullptr) {
        GAPID_FATAL("Unable to to open X display");
    }

    int major;
    int minor;
    if (!glXQueryVersion(mDisplay, &major, &minor) || (major == 1 && minor < 3)) {
        GAPID_FATAL("GLX 1.3+ unsupported by X server (was %d.%d)", major, minor);
    }

    // Initialize with a default target.
    setBackbuffer(Backbuffer(
          8, 8,
          core::gl::GL_RGBA8,
          core::gl::GL_DEPTH24_STENCIL8,
          core::gl::GL_DEPTH24_STENCIL8));
}

GlesRendererImpl::~GlesRendererImpl() {
    reset();

    if (mDisplay != nullptr) {
        XCloseDisplay(mDisplay);
    }
}

Api* GlesRendererImpl::api() {
  return &mApi;
}

void GlesRendererImpl::reset() {
    unbind();

    if (mContext != nullptr) {
        glXDestroyContext(mDisplay, mContext);
        mContext = nullptr;
    }

    if (mPbuffer != 0) {
        glXDestroyPbuffer(mDisplay, mPbuffer);
        mPbuffer = 0;
    }

    mBackbuffer = Backbuffer();
}

void GlesRendererImpl::createPbuffer(int width, int height) {
    if (mPbuffer != 0) {
        glXDestroyPbuffer(mDisplay, mPbuffer);
        mPbuffer = 0;
    }
    const int pbufferAttribs[] = {
        GLX_PBUFFER_WIDTH, width,
        GLX_PBUFFER_HEIGHT, height,
        None
    };
    mPbuffer = glXCreatePbuffer(mDisplay, mFBConfig, pbufferAttribs);
}

void GlesRendererImpl::setBackbuffer(Backbuffer backbuffer) {
    if (mBackbuffer == backbuffer) {
        return; // No change
    }

    if (mBackbuffer.format == backbuffer.format) {
        // Only a resize is necessary
        GAPID_INFO("Resizing renderer: %dx%d -> %dx%d",
                mBackbuffer.width, mBackbuffer.height, backbuffer.width, backbuffer.height);
        createPbuffer(backbuffer.width, backbuffer.height);
        glXMakeContextCurrent(mDisplay, mPbuffer, mPbuffer, mContext);
        mBackbuffer = backbuffer;
        return;
    }

    const bool wasBound = mBound;

    reset();

    int r = 8, g = 8, b = 8, a = 8, d = 24, s = 8;
    core::gl::getColorBits(backbuffer.format.color, r, g, b, a);
    core::gl::getDepthBits(backbuffer.format.depth, d);
    core::gl::getStencilBits(backbuffer.format.stencil, s);

    const int visualAttribs[] = {
        GLX_RED_SIZE, r,
        GLX_GREEN_SIZE, g,
        GLX_BLUE_SIZE, b,
        GLX_ALPHA_SIZE, a,
        GLX_DEPTH_SIZE, d,
        GLX_STENCIL_SIZE, s,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        None
    };
    int fbConfigsCount;
    GLXFBConfig *fbConfigs = glXChooseFBConfig(
            mDisplay, DefaultScreen(mDisplay), visualAttribs, &fbConfigsCount);
    if (fbConfigs == nullptr) {
        GAPID_FATAL("Unable to find a suitable X framebuffer config");
    }
    mFBConfig = fbConfigs[0];
    XFree(fbConfigs);

    glXCreateContextAttribsARBProc glXCreateContextAttribsARB =
        (glXCreateContextAttribsARBProc)glXGetProcAddress("glXCreateContextAttribsARB");
    if (glXCreateContextAttribsARB == nullptr) {
        GAPID_FATAL("Unable to get address of glXCreateContextAttribsARB");
    }
    // Try to get the most recent version of OpenGL available.
    // The implementation is always free to return a newer version.
    // However some implementations will return the precise version,
    // so if we request 3.2, we would get 3.2 even if new is available.
    struct { int major; int minor; } gl_versions[] = {
        {4, 5}, // Compatible with OpenGL ES 3.1
        {4, 4},
        {4, 3}, // Compatible with OpenGL ES 3.0
        {4, 2},
        {4, 1}, // Compatible with OpenGL ES 2.0
        {4, 0},
        {3, 3},
        {3, 2}, // Introduces core profile
    };
    // Prevent X from taking down the process if the GL version is not supported.
    auto oldHandler = XSetErrorHandler([](Display*, XErrorEvent*)->int{ return 0; });
    for (auto gl_version : gl_versions) {
        // List of name-value pairs.
        const int contextAttribs[] = {
            GLX_RENDER_TYPE, GLX_RGBA_TYPE,
            GLX_CONTEXT_MAJOR_VERSION_ARB, gl_version.major,
            GLX_CONTEXT_MINOR_VERSION_ARB, gl_version.minor,
            GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_DEBUG_BIT_ARB,
            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None,
        };
        mContext = glXCreateContextAttribsARB(
            mDisplay, mFBConfig, /* shared */ nullptr, /* direct */ True, contextAttribs);
        if (mContext != nullptr) {
            break;
        }
    }
    XSetErrorHandler(oldHandler);
    if (mContext == nullptr) {
        GAPID_FATAL("Failed to create glX context");
    }
    XSync(mDisplay, False);

    createPbuffer(backbuffer.width, backbuffer.height);

    mBackbuffer = backbuffer;
    mNeedsResolve = true;

    if (wasBound) {
        bind();
    }
}

void GlesRendererImpl::bind() {
    if (!mBound) {
        if (!glXMakeContextCurrent(mDisplay, mPbuffer, mPbuffer, mContext)) {
            GAPID_FATAL("Unable to make GLX context current");
        }

        mBound = true;

        if (mNeedsResolve) {
            mNeedsResolve = false;
            mApi.resolve();
        }
    }
}

void GlesRendererImpl::unbind() {
    if (mBound) {
        // TODO: glXMakeContextCurrent(...)
        mBound = false;
    }
}

const char* GlesRendererImpl::name() {
    return reinterpret_cast<const char*>(
        mApi.mFunctionStubs.glGetString(Gles::GLenum::GL_RENDERER));
}

const char* GlesRendererImpl::extensions() {
    if (!mQueriedExtensions) {
        mQueriedExtensions = true;
        int32_t n, i;
        mApi.mFunctionStubs.glGetIntegerv(Gles::GLenum::GL_NUM_EXTENSIONS, &n);
        for (i = 0; i < n; i++) {
            if (i > 0) {
              mExtensions += " ";
            }
            mExtensions += reinterpret_cast<const char*>(
                mApi.mFunctionStubs.glGetStringi(Gles::GLenum::GL_EXTENSIONS, i));
        }
    }
    return &mExtensions[0];
}

const char* GlesRendererImpl::vendor() {
    return reinterpret_cast<const char*>(
        mApi.mFunctionStubs.glGetString(Gles::GLenum::GL_VENDOR));
}

const char* GlesRendererImpl::version() {
    return reinterpret_cast<const char*>(
        mApi.mFunctionStubs.glGetString(Gles::GLenum::GL_VERSION));
}

} // anonymous namespace

GlesRenderer* GlesRenderer::create() {
    return new GlesRendererImpl();
}

}  // namespace gapir
