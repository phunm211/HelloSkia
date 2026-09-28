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

#include <vector>
#include <string>
#include <sstream>
#include "include/core/SkTextBlob.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkBlendMode.h"

// Helper to decode a single UTF-8 codepoint from a byte stream
static SkUnichar nextUtf8Char(const char*& ptr, const char* end) {
    if (ptr >= end) return 0;
    unsigned char c = (unsigned char)*ptr++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) {
        if (ptr >= end) return 0;
        SkUnichar u = (c & 0x1F) << 6;
        u |= (*ptr++ & 0x3F);
        return u;
    }
    if ((c & 0xF0) == 0xE0) {
        if (ptr + 1 >= end) return 0;
        SkUnichar u = (c & 0x0F) << 12;
        u |= ((*ptr++ & 0x3F) << 6);
        u |= (*ptr++ & 0x3F);
        return u;
    }
    if ((c & 0xF8) == 0xF0) {
        if (ptr + 2 >= end) return 0;
        SkUnichar u = (c & 0x07) << 18;
        u |= ((*ptr++ & 0x3F) << 12);
        u |= ((*ptr++ & 0x3F) << 6);
        u |= (*ptr++ & 0x3F);
        return u;
    }
    return c;
}

// Draw a single line with dynamic font fallback for Korean, Emoji, Hebrew, etc.
static void drawLineWithFallback(SkCanvas* canvas, const std::string& line, SkScalar x, SkScalar y,
                                 SkFontMgr* fontMgr, sk_sp<SkTypeface> baseTypeface,
                                 SkScalar textSize, bool isBold, bool isItalic, const SkPaint& textPaint) {
    if (line.empty()) return;

    struct GlyphItem {
        sk_sp<SkTypeface> typeface;
        SkGlyphID glyphId;
        SkScalar advance;
    };

    std::vector<GlyphItem> glyphs;
    const char* ptr = line.data();
    const char* end = ptr + line.size();

    // Cache recently resolved typefaces to minimize SkFontMgr queries
    sk_sp<SkTypeface> lastTf = baseTypeface;

    SkFontStyle targetStyle = isBold ? (isItalic ? SkFontStyle::BoldItalic() : SkFontStyle::Bold())
                                     : (isItalic ? SkFontStyle::Italic() : SkFontStyle::Normal());

    while (ptr < end) {
        SkUnichar u = nextUtf8Char(ptr, end);
        if (u == 0) break;

        sk_sp<SkTypeface> matchedTf;
        SkGlyphID gid = 0;

        // 1. Try base typeface first
        if (baseTypeface) {
            gid = baseTypeface->unicharToGlyph(u);
            if (gid != 0) {
                matchedTf = baseTypeface;
            }
        }

        // 2. Try last matched fallback typeface
        if (gid == 0 && lastTf && lastTf != baseTypeface) {
            gid = lastTf->unicharToGlyph(u);
            if (gid != 0) {
                matchedTf = lastTf;
            }
        }

        // 3. Query system fallback font manager for this specific codepoint
        // Note: Use SkFontStyle::Normal() so we resolve the base fallback font (e.g. Regular),
        // exactly matching how TextView handles fallback fonts with fakeBold.
        if (gid == 0 && fontMgr) {
            matchedTf = fontMgr->matchFamilyStyleCharacter("sans-serif", SkFontStyle::Normal(), nullptr, 0, u);
            if (matchedTf) {
                gid = matchedTf->unicharToGlyph(u);
            }
        }

        // Fallback to base if still 0
        if (!matchedTf) {
            matchedTf = baseTypeface;
        }
        lastTf = matchedTf;

        SkFont subFont(matchedTf, textSize);
        subFont.setEdging(SkFont::Edging::kAntiAlias);
        subFont.setSubpixel(true);
        subFont.setHinting(SkFontHinting::kNone);
        subFont.setLinearMetrics(true);
        if (isItalic) subFont.setSkewX(-0.25f);

        SkScalar width = 0;
        // Measure with fake bold stroke width if applicable
        if (isBold && !matchedTf->isBold()) {
            subFont.getWidthsBounds(&gid, 1, &width, nullptr, &textPaint);
        } else {
            subFont.getWidths(&gid, 1, &width);
        }

        glyphs.push_back({matchedTf, gid, width});
    }

    // Now group consecutive glyphs with the same typeface into SkTextBlob runs
    SkScalar currX = x;
    size_t i = 0;
    while (i < glyphs.size()) {
        sk_sp<SkTypeface> tf = glyphs[i].typeface;
        size_t runStart = i;
        while (i < glyphs.size() && glyphs[i].typeface == tf) {
            i++;
        }
        int count = static_cast<int>(i - runStart);

        SkFont runFont(tf, textSize);
        runFont.setEdging(SkFont::Edging::kAntiAlias);
        runFont.setSubpixel(true);
        runFont.setHinting(SkFontHinting::kNone);
        runFont.setLinearMetrics(true);
        if (isItalic) runFont.setSkewX(-0.25f);

        SkTextBlobBuilder builder;
        const auto& runBuffer = builder.allocRunPosH(runFont, count, 0.0f);

        SkScalar runStartX = currX;
        for (int k = 0; k < count; ++k) {
            runBuffer.glyphs[k] = glyphs[runStart + k].glyphId;
            runBuffer.pos[k] = currX - runStartX;
            currX += glyphs[runStart + k].advance;
        }

        sk_sp<SkTextBlob> blob = builder.make();
        if (blob) {
            // Check if this font is a color emoji font (e.g. NotoColorEmoji)
            SkString familyName;
            if (tf) tf->getFamilyName(&familyName);
            bool isEmoji = (strstr(familyName.c_str(), "Emoji") != nullptr);

            if (isEmoji) {
                // Emoji fonts are multicolored bitmaps; draw with pure color-filterless paint
                SkPaint emojiPaint;
                emojiPaint.setAntiAlias(true);
                canvas->drawTextBlob(blob, runStartX, y, emojiPaint);
            } else if (tf && tf->isBold()) {
                // If the font is already intrinsically bold, draw with normal fill to prevent double bolding
                SkPaint normalPaint;
                normalPaint.setColor(textPaint.getColor());
                normalPaint.setAntiAlias(true);
                canvas->drawTextBlob(blob, runStartX, y, normalPaint);
            } else {
                canvas->drawTextBlob(blob, runStartX, y, textPaint);
            }
        }
    }
}

// Measure width of a single line using font fallback
static SkScalar measureLineWidth(const std::string& line, SkFontMgr* fontMgr,
                                 sk_sp<SkTypeface> baseTypeface, SkScalar textSize,
                                 bool isBold, const SkPaint& textPaint) {
    if (line.empty()) return 0.0f;
    const char* ptr = line.data();
    const char* end = ptr + line.size();
    SkScalar totalW = 0.0f;
    sk_sp<SkTypeface> lastTf = baseTypeface;

    while (ptr < end) {
        SkUnichar u = nextUtf8Char(ptr, end);
        if (u == 0) break;

        sk_sp<SkTypeface> matchedTf;
        SkGlyphID gid = 0;
        if (baseTypeface) {
            gid = baseTypeface->unicharToGlyph(u);
            if (gid != 0) matchedTf = baseTypeface;
        }
        if (gid == 0 && lastTf && lastTf != baseTypeface) {
            gid = lastTf->unicharToGlyph(u);
            if (gid != 0) matchedTf = lastTf;
        }
        if (gid == 0 && fontMgr) {
            matchedTf = fontMgr->matchFamilyStyleCharacter("sans-serif", SkFontStyle::Normal(), nullptr, 0, u);
            if (matchedTf) gid = matchedTf->unicharToGlyph(u);
        }
        if (!matchedTf) matchedTf = baseTypeface;
        lastTf = matchedTf;

        SkFont subFont(matchedTf, textSize);
        SkScalar width = 0;
        if (isBold && !matchedTf->isBold()) {
            subFont.getWidthsBounds(&gid, 1, &width, nullptr, &textPaint);
        } else {
            subFont.getWidths(&gid, 1, &width);
        }
        totalW += width;
    }
    return totalW;
}

// Render content into a single backend texture
static void renderCellToTexture(GLuint texId, int w, int h, bool isBold, bool isItalic, const char* text) {
    GrGLTextureInfo glInfo;
    glInfo.fTarget = GL_TEXTURE_2D;
    glInfo.fID = texId;
    glInfo.fFormat = GL_RGBA8;

    auto backendTex = GrBackendTextures::MakeGL(w, h, skgpu::Mipmapped::kNo, glInfo);

    // Matching Android HWUI text gamma & contrast
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

    // 3. Resolve base typeface matching "sans-serif" Normal (Weight 400)
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

    SkPaint textPaint;
    textPaint.setColor(SkColorSetRGB(25, 118, 210)); // #1976D2 matching left column
    textPaint.setAntiAlias(true);

    // Option A: Android HWUI implements paint.setFakeBoldText(true) by applying stroke-and-fill
    if (isBold) {
        textPaint.setStyle(SkPaint::kStrokeAndFill_Style);
        textPaint.setStrokeWidth(globalTextSizePx / 30.0f);
        textPaint.setStrokeJoin(SkPaint::kRound_Join);
        textPaint.setStrokeCap(SkPaint::kRound_Cap);
    }

    // Split text into individual lines
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, '\n')) {
        lines.push_back(item);
    }

    SkFont baseFont(typeface, globalTextSizePx);
    SkFontMetrics metrics;
    baseFont.getMetrics(&metrics);

    // Line spacing matching Android TextView:
    // Android TextView uses font.getFontSpacing() = descent - ascent + leading
    SkScalar lineHeight = metrics.fDescent - metrics.fAscent + metrics.fLeading;
    SkScalar totalTextBlockHeight = lines.size() * lineHeight;

    // Start baseline y for vertical centering
    SkScalar startBaselineY = (h - totalTextBlockHeight) / 2.0f - metrics.fAscent;

    for (size_t lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        const std::string& curLine = lines[lineIdx];
        SkScalar lineWidth = measureLineWidth(curLine, androidFontMgr.get(), typeface, globalTextSizePx,
                                              isBold, textPaint);
        SkScalar x = (w - lineWidth) / 2.0f;
        SkScalar y = startBaselineY + lineIdx * lineHeight;

        drawLineWithFallback(canvas, curLine, x, y, androidFontMgr.get(), typeface,
                             globalTextSizePx, isBold, isItalic, textPaint);
    }

    // Flush commands to the texture
    grContext->flushAndSubmit();
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeRender(JNIEnv*, jobject) {
    if (!mainSurface || !grContext) return;

    int cellW = texWidth;
    int cellH = texHeight;

    const char* sampleText = "Hello\n안녕하세요\n😀🎉🚀\nשלום";

    // --- BƯỚC 1: Render 3 nội dung vào 3 OpenGL Textures matching TextView ---
    // Texture 0: Normal (isBold=false, isItalic=false)
    renderCellToTexture(glTextureIds[0], cellW, cellH, false, false, sampleText);

    // Texture 1: Bold (isBold=true, isItalic=false)
    renderCellToTexture(glTextureIds[1], cellW, cellH, true, false, sampleText);

    // Texture 2: Bold Italic (isBold=true, isItalic=true)
    renderCellToTexture(glTextureIds[2], cellW, cellH, true, true, sampleText);

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