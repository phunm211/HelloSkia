#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include "include/ports/SkFontMgr_directory.h"
#include "include/ports/SkFontMgr_android.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkSurfaceProps.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "gpu/ganesh/gl/GrGLAssembleInterface.h"

#define LOG_TAG "NativeRenderer"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

static ANativeWindow* nativeWindow = nullptr;
static EGLDisplay eglDisplay = EGL_NO_DISPLAY;
static EGLSurface eglSurface = EGL_NO_SURFACE;
static EGLContext eglContext = EGL_NO_CONTEXT;

static sk_sp<GrDirectContext> grContext;
static sk_sp<SkSurface> mainSurface;

static int surfaceWidth = 0;
static int surfaceHeight = 0;
static float globalTextSizePx = 190.0f;
static int globalBoldWeight = 700;

// 3 GL Textures
static GLuint glTextureIds[3] = {0, 0, 0};
static int texWidth = 0;
static int texHeight = 0;

static void cleanupGLTextures() {
    if (glTextureIds[0] != 0 || glTextureIds[1] != 0 || glTextureIds[2] != 0) {
        glDeleteTextures(3, glTextureIds);
        glTextureIds[0] = 0;
        glTextureIds[1] = 0;
        glTextureIds[2] = 0;
    }
}

static void createGLTextures(int width, int height) {
    cleanupGLTextures();
    texWidth = width;
    texHeight = height;

    glGenTextures(3, glTextureIds);
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, glTextureIds[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texWidth, texHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    LOGI("Created 3 GL Textures: [%u, %u, %u] size %dx%d", glTextureIds[0], glTextureIds[1], glTextureIds[2], texWidth, texHeight);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeInit(JNIEnv* env, jobject, jobject surface, jint width, jint height, jfloat textSizePx, jint boldWeight) {
    surfaceWidth = width;
    surfaceHeight = height;
    if (textSizePx > 0.0f) {
        globalTextSizePx = textSizePx;
    }
    if (boldWeight > 0) {
        globalBoldWeight = boldWeight;
    }
    LOGI("nativeInit: textSizePx = %f, boldWeight = %d", globalTextSizePx, globalBoldWeight);

    // 1. Get Native Window
    nativeWindow = ANativeWindow_fromSurface(env, surface);
    if (!nativeWindow) {
        LOGE("nativeInit: ANativeWindow_fromSurface failed.");
        return;
    }

    // 2. Get EGL Display
    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY) {
        LOGE("nativeInit: eglGetDisplay failed.");
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
        return;
    }

    // 3. Initialize EGL
    EGLint majorVersion, minorVersion;
    if (!eglInitialize(eglDisplay, &majorVersion, &minorVersion)) {
        LOGE("nativeInit: eglInitialize failed.");
        eglDisplay = EGL_NO_DISPLAY;
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
        return;
    }

    EGLConfig eglConfig;
    EGLint numConfigs;
    EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 16,
            EGL_NONE
    };

    if (!eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfigs) || numConfigs < 1) {
        configAttribs[1] = EGL_OPENGL_ES2_BIT;
        if (!eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfigs) || numConfigs < 1) {
            LOGE("nativeInit: eglChooseConfig failed.");
            return;
        }
    }

    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext == EGL_NO_CONTEXT) {
        LOGE("nativeInit: eglCreateContext failed.");
        return;
    }

    eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, nativeWindow, nullptr);
    if (eglSurface == EGL_NO_SURFACE) {
        LOGE("nativeInit: eglCreateWindowSurface failed!");
        return;
    }

    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
        LOGE("nativeInit: eglMakeCurrent failed.");
        return;
    }

    // 4. Initialize Skia GrDirectContext
    auto interface = GrGLMakeAssembledInterface(
            nullptr,
            [](void*, const char name[]) -> GrGLFuncPtr {
                return reinterpret_cast<GrGLFuncPtr>(eglGetProcAddress(name));
            });
    if (!interface) {
        LOGE("nativeInit: GrGLMakeAssembledInterface failed.");
        return;
    }

    grContext = GrDirectContexts::MakeGL(interface);
    if (!grContext) {
        LOGE("nativeInit: GrDirectContexts::MakeGL failed.");
        return;
    }

    // 5. Wrap Main Window RenderTarget
    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = 0;
    fbInfo.fFormat = GL_RGBA8;

    auto backendRT = GrBackendRenderTargets::MakeGL(surfaceWidth, surfaceHeight, 0, 8, fbInfo);
    mainSurface = SkSurfaces::WrapBackendRenderTarget(
            grContext.get(), backendRT,
            kBottomLeft_GrSurfaceOrigin,
            kRGBA_8888_SkColorType,
            SkColorSpace::MakeSRGB(),
            nullptr);

    // 6. Create 3 OpenGL textures for 3 cells
    int cellHeight = surfaceHeight / 3;
    createGLTextures(surfaceWidth, cellHeight);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeResize(JNIEnv*, jobject, jint width, jint height, jfloat textSizePx, jint boldWeight) {
    surfaceWidth = width;
    surfaceHeight = height;
    if (textSizePx > 0.0f) {
        globalTextSizePx = textSizePx;
    }
    if (boldWeight > 0) {
        globalBoldWeight = boldWeight;
    }
    if (!grContext) return;

    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = 0;
    fbInfo.fFormat = GL_RGBA8;

    auto backendRT = GrBackendRenderTargets::MakeGL(surfaceWidth, surfaceHeight, 0, 8, fbInfo);
    mainSurface = SkSurfaces::WrapBackendRenderTarget(
            grContext.get(), backendRT,
            kBottomLeft_GrSurfaceOrigin,
            kRGBA_8888_SkColorType,
            SkColorSpace::MakeSRGB(),
            nullptr);

    int cellHeight = surfaceHeight / 3;
    createGLTextures(surfaceWidth, cellHeight);
}

// Render content into a single backend texture
static void renderCellToTexture(GLuint texId, int w, int h, bool isBold, bool isItalic, const char* text) {
    GrGLTextureInfo glInfo;
    glInfo.fTarget = GL_TEXTURE_2D;
    glInfo.fID = texId;
    glInfo.fFormat = GL_RGBA8;

    auto backendTex = GrBackendTextures::MakeGL(w, h, skgpu::Mipmapped::kNo, glInfo);

    // Matching Android HWUI text gamma & contrast:
    // Android HWUI uses text gamma ~1.4 for black text on white background and kUnknown pixel geometry
    SkSurfaceProps surfaceProps(0, kUnknown_SkPixelGeometry, 0.0f, 1.4f);

    sk_sp<SkSurface> cellSurface = SkSurfaces::WrapBackendTexture(
            grContext.get(),
            backendTex,
            kTopLeft_GrSurfaceOrigin,
            0, // sampleCnt
            kRGBA_8888_SkColorType,
            SkColorSpace::MakeSRGB(),
            &surfaceProps);

    if (!cellSurface) {
        LOGE("Failed to wrap backend texture %u", texId);
        return;
    }

    SkCanvas* canvas = cellSurface->getCanvas();

    // 1. Draw cell background (matching cell_border.xml #F8F9FA)
    canvas->clear(SkColorSetRGB(248, 249, 250));

    // 2. Draw cell border (1px #CCCCCC)
    SkPaint borderPaint;
    borderPaint.setStyle(SkPaint::kStroke_Style);
    borderPaint.setColor(SkColorSetRGB(204, 204, 204));
    borderPaint.setStrokeWidth(2.0f);
    canvas->drawRect(SkRect::MakeWH(w, h), borderPaint);

    // 3. Resolve base typeface matching "sans-serif" Normal (Weight 400) - exactly what TextView uses
    static sk_sp<SkFontMgr> androidFontMgr = nullptr;
    if (!androidFontMgr) {
        androidFontMgr = SkFontMgr_New_Android(nullptr);
        if (!androidFontMgr) {
            androidFontMgr = SkFontMgr_New_Custom_Directory("/system/fonts");
        }
    }

    sk_sp<SkTypeface> typeface;
    if (androidFontMgr) {
        typeface = androidFontMgr->matchFamilyStyle("sans-serif", SkFontStyle::Normal());
        if (!typeface) {
            typeface = androidFontMgr->matchFamilyStyle(nullptr, SkFontStyle::Normal());
        }
    }

    SkString familyName("Unknown");
    SkString psName("Unknown");
    if (typeface) {
        typeface->getFamilyName(&familyName);
        typeface->getPostScriptName(&psName);
        LOGI("[Skia Cell texId=%u] Base font: Family='%s', PostScript='%s', isBold=%b, isItalic=%b",
             texId, familyName.c_str(), psName.c_str(), isBold, isItalic);
    } else {
        LOGE("[Skia Cell texId=%u] Typeface is NULL!", texId);
    }

    // Use exact pixel size measured directly from Android TextView
    SkFont font(typeface, globalTextSizePx);
    // Use Greyscale Anti-Aliasing (same as Android mobile OLED screens)
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setSubpixel(true);

    // Matching Android HWUI: large text disables hinting (kNone) and uses linear metrics
    font.setHinting(SkFontHinting::kNone);
    font.setLinearMetrics(true);

    // Match Android TextView's paint.setTextSkewX(-0.25f)
    if (isItalic) {
        font.setSkewX(-0.25f);
    }

    SkPaint textPaint;
    textPaint.setColor(SkColorSetRGB(25, 118, 210)); // #1976D2 matching left column
    textPaint.setAntiAlias(true);

    // Option A: Android HWUI implements paint.setFakeBoldText(true) by applying stroke-and-fill:
    // strokeWidth = textSize / 30.0f (~7.2px for 216px text).
    // This perfectly preserves the inner counter (aperture) of glyphs like 'e'
    // without over-expanding like FreeType's FT_GlyphSlot_Embolden (1/24).
    if (isBold) {
        textPaint.setStyle(SkPaint::kStrokeAndFill_Style);
        textPaint.setStrokeWidth(globalTextSizePx / 30.0f);
        textPaint.setStrokeJoin(SkPaint::kRound_Join);
        textPaint.setStrokeCap(SkPaint::kRound_Cap);
    }

    // Measure and center text horizontally & vertically exactly like Android TextView
    SkFontMetrics metrics;
    font.getMetrics(&metrics);

    SkRect bounds;
    SkScalar textWidth = font.measureText(text, strlen(text), SkTextEncoding::kUTF8, &bounds, &textPaint);

    // Horizontal centering:
    SkScalar x = (w - textWidth) / 2.0f;

    // Vertical centering: Android TextView aligns text based on font metrics:
    // Middle of font line is (ascent + descent) / 2.
    // To center vertically in box of height h: baseline = h/2 - (ascent + descent)/2
    SkScalar y = (h / 2.0f) - ((metrics.fAscent + metrics.fDescent) / 2.0f);

    canvas->drawString(text, x, y, font, textPaint);

    // Flush commands to the texture
    grContext->flushAndSubmit();
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeRender(JNIEnv*, jobject) {
    if (!mainSurface || !grContext) return;

    int cellW = texWidth;
    int cellH = texHeight;

    // --- BƯỚC 1: Render 3 nội dung vào 3 OpenGL Textures matching TextView ---
    // Texture 0: Normal (isBold=false, isItalic=false)
    renderCellToTexture(glTextureIds[0], cellW, cellH, false, false, "Hello");

    // Texture 1: Bold (isBold=true, isItalic=false)
    renderCellToTexture(glTextureIds[1], cellW, cellH, true, false, "Hello");

    // Texture 2: Bold Italic (isBold=true, isItalic=true)
    renderCellToTexture(glTextureIds[2], cellW, cellH, true, true, "Hello");

    // --- BƯỚC 2: Render thẳng 3 Texture lên Main Canvas ---
    SkCanvas* mainCanvas = mainSurface->getCanvas();
    mainCanvas->clear(SK_ColorWHITE);

    for (int i = 0; i < 3; ++i) {
        GrGLTextureInfo glInfo;
        glInfo.fTarget = GL_TEXTURE_2D;
        glInfo.fID = glTextureIds[i];
        glInfo.fFormat = GL_RGBA8;

        auto backendTex = GrBackendTextures::MakeGL(cellW, cellH, skgpu::Mipmapped::kNo, glInfo);

        // BorrowTextureFrom: Zero-copy GPU texture handle wrapper (no CPU readback)
        sk_sp<SkImage> textureImage = SkImages::BorrowTextureFrom(
                grContext.get(),
                backendTex,
                kTopLeft_GrSurfaceOrigin,
                kRGBA_8888_SkColorType,
                kPremul_SkAlphaType,
                SkColorSpace::MakeSRGB());

        if (textureImage) {
            float yPos = static_cast<float>(i * cellH);
            mainCanvas->drawImage(textureImage, 0.0f, yPos);
        } else {
            LOGE("Failed to borrow texture %u", glTextureIds[i]);
        }
    }

    grContext->flushAndSubmit();
    eglSwapBuffers(eglDisplay, eglSurface);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeDestroy(JNIEnv*, jobject) {
    cleanupGLTextures();

    mainSurface.reset();
    grContext.reset();

    if (eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay, eglContext);
        }
        if (eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay, eglSurface);
        }
        eglTerminate(eglDisplay);
    }

    if (nativeWindow) {
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
    }

    eglDisplay = EGL_NO_DISPLAY;
    eglContext = EGL_NO_CONTEXT;
    eglSurface = EGL_NO_SURFACE;
}