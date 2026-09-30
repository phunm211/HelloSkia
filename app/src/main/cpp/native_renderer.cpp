#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <android/font.h>
#include <android/font_matcher.h>
#include <android/system_fonts.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include <memory>
#include <unistd.h>

#include "modules/skshaper/include/SkShaper.h"
#include "modules/skshaper/include/SkShaper_harfbuzz.h"
#include "modules/skunicode/include/SkUnicode_icu.h"
#include "include/ports/SkFontMgr_empty.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkStream.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkSurfaceProps.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkBlendMode.h"
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
static std::string gAppFontPath = "";

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
Java_com_example_helloskia_MainActivity_nativeInit(JNIEnv* env, jobject, jobject surface, jint width, jint height, jfloat textSizePx, jint boldWeight, jstring fontPath) {
    surfaceWidth = width;
    surfaceHeight = height;
    if (textSizePx > 0.0f) {
        globalTextSizePx = textSizePx;
    }
    if (boldWeight > 0) {
        globalBoldWeight = boldWeight;
    }
    if (fontPath != nullptr) {
        const char* cPath = env->GetStringUTFChars(fontPath, nullptr);
        if (cPath) {
            gAppFontPath = cPath;
            env->ReleaseStringUTFChars(fontPath, cPath);
        }
    } else {
        gAppFontPath = "";
    }
    LOGI("nativeInit: textSizePx = %f, boldWeight = %d, fontPath = '%s'",
         globalTextSizePx, globalBoldWeight, gAppFontPath.c_str());

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
Java_com_example_helloskia_MainActivity_nativeResize(JNIEnv* env, jobject, jint width, jint height, jfloat textSizePx, jint boldWeight, jstring fontPath) {
    surfaceWidth = width;
    surfaceHeight = height;
    if (textSizePx > 0.0f) {
        globalTextSizePx = textSizePx;
    }
    if (boldWeight > 0) {
        globalBoldWeight = boldWeight;
    }
    if (fontPath != nullptr) {
        const char* cPath = env->GetStringUTFChars(fontPath, nullptr);
        if (cPath) {
            gAppFontPath = cPath;
            env->ReleaseStringUTFChars(fontPath, cPath);
        }
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

// Global cache for Typefaces resolved via AFontMatcher file paths to prevent redundant re-reading
static std::unordered_map<std::string, sk_sp<SkTypeface>> gFontCache;
static AFontMatcher* gFontMatcher = nullptr;
// Detected system font family: "sec" on Samsung, "" (default) on AOSP
static std::string gSystemFontFamily = "sec";
// True when Regular and Bold resolve to same static file → use Skia embolden
static bool gUseFakeBold = false;

static sk_sp<SkFontMgr> getEmptyFontMgr() {
    static sk_sp<SkFontMgr> mgr = SkFontMgr_New_Custom_Empty();
    return mgr;
}

static AFontMatcher* getFontMatcher() {
    if (!gFontMatcher) {
        gFontMatcher = AFontMatcher_create();
    }
    return gFontMatcher;
}

// Convert UTF-8 to UTF-16 for AFontMatcher_match
static std::vector<uint16_t> utf8ToUtf16(const char* utf8, size_t utf8Bytes) {
    std::vector<uint16_t> utf16;
    const char* ptr = utf8;
    const char* end = utf8 + utf8Bytes;
    while (ptr < end) {
        SkUnichar u = nextUtf8Char(ptr, end);
        if (u == 0) break;
        if (u <= 0xFFFF) {
            utf16.push_back((uint16_t)u);
        } else {
            u -= 0x10000;
            utf16.push_back((uint16_t)((u >> 10) + 0xD800));
            utf16.push_back((uint16_t)((u & 0x3FF) + 0xDC00));
        }
    }
    return utf16;
}

static sk_sp<SkTypeface> createWeightTypeface(sk_sp<SkTypeface> tf, float weight) {
    if (!tf) return nullptr;
    SkFontArguments::VariationPosition::Coordinate coord = {
        SkSetFourByteTag('w', 'g', 'h', 't'), weight
    };
    SkFontArguments args;
    args.setVariationDesignPosition({ &coord, 1 });
    auto cloned = tf->makeClone(args);
    return cloned ? cloned : tf;
}

// Custom FontRunIterator that resolves fonts using Android NDK's AFontMatcher
class AFontMatcherRunIterator : public SkShaper::FontRunIterator {
public:
    AFontMatcherRunIterator(const char* utf8, size_t utf8Bytes, SkScalar textSize, bool isBold, bool isItalic,
                            sk_sp<SkTypeface> baseTypeface, bool forceFakeBold = false)
            : fUtf8(utf8), fUtf8Bytes(utf8Bytes), fTextSize(textSize), fIsBold(isBold), fIsItalic(isItalic),
              fBaseTypeface(baseTypeface), fForceFakeBold(forceFakeBold), fCurrentOffset(0) {
        consume();
    }

    void consume() override {
        if (fCurrentOffset >= fUtf8Bytes) {
            return;
        }

        const char* textStart = fUtf8 + fCurrentOffset;
        size_t remainingBytes = fUtf8Bytes - fCurrentOffset;

        // Check first codepoint
        const char* tempPtr = textStart;
        SkUnichar firstChar = nextUtf8Char(tempPtr, textStart + remainingBytes);
        size_t firstCharBytes = tempPtr - textStart;

        // If baseTypeface has the glyph, consume all consecutive characters supported by baseTypeface
        if (fBaseTypeface && fBaseTypeface->unicharToGlyph(firstChar) != 0) {
            setupFont(fBaseTypeface);
            const char* walk = tempPtr;
            while (walk < textStart + remainingBytes) {
                const char* prev = walk;
                SkUnichar u = nextUtf8Char(walk, textStart + remainingBytes);
                if (u == 0 || fBaseTypeface->unicharToGlyph(u) == 0) {
                    walk = prev;
                    break;
                }
            }
            fCurrentOffset = walk - fUtf8;
            return;
        }

        // Otherwise, use AFontMatcher to discover font from Android OS
        std::vector<uint16_t> utf16 = utf8ToUtf16(textStart, remainingBytes);
        AFontMatcher* matcher = getFontMatcher();
        AFontMatcher_setStyle(matcher, fIsBold ? 700 : 400, fIsItalic);
        uint32_t runLengthUtf16 = 0;
        AFont* font = AFontMatcher_match(matcher, gSystemFontFamily.c_str(), utf16.data(), (uint32_t)utf16.size(), &runLengthUtf16);

        sk_sp<SkTypeface> matchedTf = nullptr;
        if (font) {
            const char* path = AFont_getFontFilePath(font);
            if (path) {
                int ttcIndex = (int)AFont_getCollectionIndex(font);
                size_t axisCount = AFont_getAxisCount(font);
                std::string cacheKey = std::string(path) + "_" + std::to_string(ttcIndex) + "_" + (fIsBold ? "B" : "N") + "_" + (fIsItalic ? "I" : "N");
                auto it = gFontCache.find(cacheKey);
                if (it != gFontCache.end()) {
                    matchedTf = it->second;
                } else {
                    SkFontArguments args;
                    args.setCollectionIndex(ttcIndex);
                    std::vector<SkFontArguments::VariationPosition::Coordinate> coords;
                    if (axisCount > 0) {
                        coords.resize(axisCount);
                        for (size_t i = 0; i < axisCount; ++i) {
                            coords[i].axis = AFont_getAxisTag(font, i);
                            coords[i].value = AFont_getAxisValue(font, i);
                        }
                        args.setVariationDesignPosition({ coords.data(), (int)coords.size() });
                    }
                    std::unique_ptr<SkStreamAsset> stream = SkStream::MakeFromFile(path);
                    if (stream) {
                        matchedTf = getEmptyFontMgr()->makeFromStream(std::move(stream), args);
                    }
                    if (!matchedTf) {
                        matchedTf = getEmptyFontMgr()->makeFromFile(path, ttcIndex);
                    }
                    if (matchedTf) {
                        gFontCache[cacheKey] = matchedTf;
                    }
                }
            }
            AFont_close(font);
        }

        if (!matchedTf) {
            matchedTf = fBaseTypeface;
        }

        setupFont(matchedTf);

        // Convert runLengthUtf16 back to UTF-8 byte count
        size_t consumedUtf8Bytes = 0;
        const char* walk = textStart;
        uint32_t walkedUtf16 = 0;
        while (walk < textStart + remainingBytes && walkedUtf16 < runLengthUtf16) {
            SkUnichar u = nextUtf8Char(walk, textStart + remainingBytes);
            if (u == 0) break;
            walkedUtf16 += (u > 0xFFFF) ? 2 : 1;
        }
        consumedUtf8Bytes = walk - textStart;
        if (consumedUtf8Bytes == 0) {
            consumedUtf8Bytes = firstCharBytes > 0 ? firstCharBytes : 1;
        }
        fCurrentOffset += consumedUtf8Bytes;
    }

    size_t endOfCurrentRun() const override {
        return fCurrentOffset;
    }

    bool atEnd() const override {
        return fCurrentOffset >= fUtf8Bytes;
    }

    const SkFont& currentFont() const override {
        return fCurrentFont;
    }

private:
    void setupFont(sk_sp<SkTypeface> tf) {
        fCurrentFont = SkFont(tf, fTextSize);
        fCurrentFont.setEdging(SkFont::Edging::kAntiAlias);
        fCurrentFont.setSubpixel(true);
        fCurrentFont.setHinting(SkFontHinting::kNone);
        fCurrentFont.setLinearMetrics(true);
        // Embolden when requested explicitly or when font has no separate bold file (e.g., Samsung DroidSans)
        if (fForceFakeBold || (fIsBold && gUseFakeBold)) {
            fCurrentFont.setEmbolden(true);
        }
        if (fIsItalic) {
            fCurrentFont.setSkewX(-0.25f);
        }
    }

    const char* fUtf8;
    size_t fUtf8Bytes;
    SkScalar fTextSize;
    bool fIsBold;
    bool fIsItalic;
    sk_sp<SkTypeface> fBaseTypeface;
    bool fForceFakeBold;
    size_t fCurrentOffset;
    SkFont fCurrentFont;
};

// Custom RunHandler that wraps SkTextBlobBuilderRunHandler and records total line advance
class CenteringRunHandler : public SkShaper::RunHandler {
public:
    CenteringRunHandler(const char* utf8Text, SkPoint offset)
        : fInner(utf8Text, offset), fTotalAdvanceX(0.0f) {}

    void beginLine() override {
        fInner.beginLine();
        fTotalAdvanceX = 0.0f;
    }
    void runInfo(const RunInfo& info) override {
        fInner.runInfo(info);
        fTotalAdvanceX += info.fAdvance.fX;
    }
    void commitRunInfo() override {
        fInner.commitRunInfo();
    }
    Buffer runBuffer(const RunInfo& info) override {
        return fInner.runBuffer(info);
    }
    void commitRunBuffer(const RunInfo& info) override {
        fInner.commitRunBuffer(info);
    }
    void commitLine() override {
        fInner.commitLine();
    }

    sk_sp<SkTextBlob> makeBlob() { return fInner.makeBlob(); }
    SkScalar width() const { return fTotalAdvanceX; }

private:
    SkTextBlobBuilderRunHandler fInner;
    SkScalar fTotalAdvanceX;
};

// Render content into a single backend texture
static void renderCellToTexture(GLuint texId, int w, int h, bool isBold, bool isItalic, const char* text, bool forceFakeBold = false) {
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

    // 3. Sử dụng SkFontMgr_New_Custom_Empty() thay cho SkFontMgr_New_Android
    sk_sp<SkFontMgr> emptyFontMgr = getEmptyFontMgr();

    // Load base typeface dynamically using AFontMatcher or system OneUI font.
    static sk_sp<SkTypeface> baseTypefaceRegular = nullptr;
    static sk_sp<SkTypeface> baseTypefaceBold = nullptr;
    if (!baseTypefaceRegular) {
        // Step 0: Check for fontPath explicitly passed down from Java / App.
        // Reason: Android TextView has ground truth font selection (e.g. OneUISans-VF.ttf or custom font).
        // Passing fontPath directly from App guarantees 100% font fidelity without hardcoding or heuristics.
        if (!gAppFontPath.empty() && access(gAppFontPath.c_str(), R_OK) == 0) {
            LOGI("[Font] Using explicit fontPath from App: %s", gAppFontPath.c_str());
            SkFontArguments argsReg;
            SkFontArguments::VariationPosition::Coordinate coordReg = { SkSetFourByteTag('w', 'g', 'h', 't'), 400.0f };
            argsReg.setVariationDesignPosition({ &coordReg, 1 });
            auto streamReg = SkStream::MakeFromFile(gAppFontPath.c_str());
            if (streamReg) baseTypefaceRegular = emptyFontMgr->makeFromStream(std::move(streamReg), argsReg);
            if (!baseTypefaceRegular) baseTypefaceRegular = emptyFontMgr->makeFromFile(gAppFontPath.c_str(), 0);

            if (baseTypefaceRegular) {
                if (globalBoldWeight > 0) {
                    baseTypefaceBold = createWeightTypeface(baseTypefaceRegular, (float)globalBoldWeight);
                    LOGI("[Font] Base Bold created from explicit fontPath via VF wght=%d", globalBoldWeight);
                } else {
                    baseTypefaceBold = baseTypefaceRegular;
                }
            }
        }

        // If not Samsung or file missing, fallback to dynamic detection via AFontMatcher
        if (!baseTypefaceRegular) {
        static bool familyDetected = false;
        if (!familyDetected) {
            familyDetected = true;
            uint16_t probe[] = { 'A' };
            AFontMatcher* matcher = getFontMatcher();
            AFontMatcher_setStyle(matcher, 700, false);
            AFont* testFont = AFontMatcher_match(matcher, "sec", probe, 1, nullptr);
            if (testFont) {
                const char* path = AFont_getFontFilePath(testFont);
                size_t axisCount = AFont_getAxisCount(testFont);
                bool hasSec = (path != nullptr);
                bool secIsVF = (axisCount > 0);
                LOGI("[Font] Family detect: 'sec' exists=%d isVF=%d axes=%zu path=%s",
                     hasSec, secIsVF, axisCount, path ? path : "null");
                if (!hasSec) {
                    gSystemFontFamily = "";
                }
                AFont_close(testFont);
            } else {
                gSystemFontFamily = "";
                LOGI("[Font] Family detect: 'sec' not found, using system default");
            }
        }

        uint16_t probe[] = { 'A' };
        AFontMatcher* matcher = getFontMatcher();

        // --- Load Regular base ---
        AFontMatcher_setStyle(matcher, 400, false);
        AFont* fontRegular = AFontMatcher_match(matcher, gSystemFontFamily.c_str(), probe, 1, nullptr);

        if (fontRegular) {
            const char* path = AFont_getFontFilePath(fontRegular);
            int ttcIndex = (int)AFont_getCollectionIndex(fontRegular);
            size_t axisCount = AFont_getAxisCount(fontRegular);
            LOGI("[Font] Regular -> path=%s ttcIdx=%d axes=%zu", path ? path : "null", ttcIndex, axisCount);

            if (path) {
                SkFontArguments args;
                args.setCollectionIndex(ttcIndex);
                std::vector<SkFontArguments::VariationPosition::Coordinate> coords;
                if (axisCount > 0) {
                    coords.resize(axisCount);
                    for (size_t i = 0; i < axisCount; ++i) {
                        coords[i].axis  = AFont_getAxisTag(fontRegular, i);
                        coords[i].value = AFont_getAxisValue(fontRegular, i);
                    }
                    args.setVariationDesignPosition({ coords.data(), (int)coords.size() });
                }
                auto stream = SkStream::MakeFromFile(path);
                if (stream) baseTypefaceRegular = emptyFontMgr->makeFromStream(std::move(stream), args);
                if (!baseTypefaceRegular) baseTypefaceRegular = emptyFontMgr->makeFromFile(path, ttcIndex);
            }
            AFont_close(fontRegular);
        }

        // --- Load Bold base ---
        AFontMatcher_setStyle(matcher, 700, false);
        AFont* fontBold = AFontMatcher_match(matcher, gSystemFontFamily.c_str(), probe, 1, nullptr);
        if (fontBold) {
            const char* path = AFont_getFontFilePath(fontBold);
            int ttcIndex = (int)AFont_getCollectionIndex(fontBold);
            size_t axisCount = AFont_getAxisCount(fontBold);
            LOGI("[Font] Bold -> path=%s ttcIdx=%d axes=%zu", path ? path : "null", ttcIndex, axisCount);

            if (path) {
                bool isVF = baseTypefaceRegular &&
                            (std::string(path).find("-VF") != std::string::npos ||
                             axisCount > 0);
                bool isSameFile = false;
                {
                    AFont* regCheck = AFontMatcher_match(getFontMatcher(), gSystemFontFamily.c_str(), probe, 1, nullptr);
                    if (regCheck) {
                        const char* regPath = AFont_getFontFilePath(regCheck);
                        if (regPath && path && std::string(regPath) == std::string(path)) {
                            isSameFile = true;
                        }
                        AFont_close(regCheck);
                    }
                }

                if (isVF) {
                    if (globalBoldWeight > 0) {
                        baseTypefaceBold = createWeightTypeface(baseTypefaceRegular, (float)globalBoldWeight);
                        LOGI("[Font] Bold strategy: clone VF with wght=%d (explicit weight > 0)", globalBoldWeight);
                    } else {
                        baseTypefaceBold = baseTypefaceRegular;
                        LOGI("[Font] Bold strategy: VF axis ignored (weight <= 0)");
                    }
                } else if (isSameFile) {
                    baseTypefaceBold = baseTypefaceRegular;
                    gUseFakeBold = true;
                    LOGI("[Font] Bold strategy: same file detected → Skia embolden (fake bold)");
                } else {
                    SkFontArguments args;
                    args.setCollectionIndex(ttcIndex);
                    auto stream = SkStream::MakeFromFile(path);
                    if (stream) baseTypefaceBold = emptyFontMgr->makeFromStream(std::move(stream), args);
                    if (!baseTypefaceBold) baseTypefaceBold = emptyFontMgr->makeFromFile(path, ttcIndex);
                    LOGI("[Font] Bold strategy: load separate bold file");
                }
            }
            AFont_close(fontBold);
        }

        // Final fallback: if still null, use hardcoded paths
        if (!baseTypefaceRegular) {
            baseTypefaceRegular = emptyFontMgr->makeFromFile("/system/fonts/RobotoStatic-Regular.ttf");
            if (!baseTypefaceRegular)
                baseTypefaceRegular = emptyFontMgr->makeFromFile("/system/fonts/Roboto-Regular.ttf");
        }
        if (!baseTypefaceBold && baseTypefaceRegular) {
            if (globalBoldWeight > 0) {
                baseTypefaceBold = createWeightTypeface(baseTypefaceRegular, (float)globalBoldWeight);
            } else {
                baseTypefaceBold = baseTypefaceRegular;
            }
        }
        }
    }

    // Nếu forceFakeBold = true (hàng 3): Dùng font Regular (wght=400, không set trục 700), nhưng bật fake bold
    sk_sp<SkTypeface> curBaseTypeface = forceFakeBold ? baseTypefaceRegular : ((isBold && baseTypefaceBold) ? baseTypefaceBold : baseTypefaceRegular);

    // 4. Khởi tạo SkShaper (HarfBuzz) và SkUnicode (ICU)
    static std::unique_ptr<SkShaper> shaper = nullptr;
    if (!shaper) {
        shaper = SkShaper::Make(emptyFontMgr);
    }

    SkPaint textPaint;
    textPaint.setColor(SkColorSetRGB(25, 118, 210)); // #1976D2 matching left column
    textPaint.setAntiAlias(true);
    textPaint.setStyle(SkPaint::kFill_Style);

    // Split text into individual lines
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, '\n')) {
        lines.push_back(item);
    }

    SkFont baseFont(curBaseTypeface, globalTextSizePx);
    if (forceFakeBold) {
        baseFont.setEmbolden(true);
    }
    SkFontMetrics metrics;
    baseFont.getMetrics(&metrics);

    SkScalar lineHeight = metrics.fDescent - metrics.fAscent + metrics.fLeading;
    SkScalar totalTextBlockHeight = lines.size() * lineHeight;
    SkScalar fontPaddingOffset = globalTextSizePx * (4.0f / 46.75f);
    SkScalar startY = (h - totalTextBlockHeight) / 2.0f + fontPaddingOffset;

    for (size_t lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        const std::string& curLine = lines[lineIdx];
        if (curLine.empty()) continue;

        // Shape with HarfBuzz & AFontMatcher
        AFontMatcherRunIterator fontIter(curLine.data(), curLine.size(), globalTextSizePx, isBold, isItalic, curBaseTypeface, forceFakeBold);
        std::unique_ptr<SkShaper::BiDiRunIterator> bidiIter =
                SkShaper::MakeBiDiRunIterator(curLine.data(), curLine.size(), 0);
        std::unique_ptr<SkShaper::ScriptRunIterator> scriptIter =
                SkShapers::HB::ScriptRunIterator(curLine.data(), curLine.size());
        std::unique_ptr<SkShaper::LanguageRunIterator> langIter =
                SkShaper::MakeStdLanguageRunIterator(curLine.data(), curLine.size());

        CenteringRunHandler handler(curLine.c_str(), {0, 0});
        if (bidiIter && scriptIter && langIter && shaper) {
            shaper->shape(curLine.data(), curLine.size(),
                          fontIter, *bidiIter, *scriptIter, *langIter,
                          (SkScalar)w, &handler);
        }

        sk_sp<SkTextBlob> blob = handler.makeBlob();
        SkScalar lineWidth = handler.width();

        // Center horizontally per line: (w - lineWidth) / 2
        SkScalar x = (w - lineWidth) / 2.0f;
        SkScalar y = startY + lineIdx * lineHeight;

        if (blob) {
            canvas->drawTextBlob(blob, x, y, textPaint);
        }
    }

    // Flush commands to the texture
    grContext->flushAndSubmit();
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeRender(JNIEnv*, jobject) {
    if (!mainSurface || !grContext) return;

    int cellW = texWidth;
    int cellH = texHeight;

    const char* sampleText = "Hello\n안녕하세요\n😀🎉🚀\nשלום\nمرحبا";

    // --- BƯỚC 1: Render 3 nội dung vào 3 OpenGL Textures matching TextView ---
    // Texture 0: Normal (isBold=false, isItalic=false, forceFakeBold=false) -> wght=400
    renderCellToTexture(glTextureIds[0], cellW, cellH, false, false, sampleText, false);

    // Texture 1: True Bold (isBold=true, isItalic=false, forceFakeBold=false) -> wght=700 (True VF Bold axis)
    renderCellToTexture(glTextureIds[1], cellW, cellH, true, false, sampleText, false);

    // Texture 2: Weight 400 + Fake Bold (isBold=true, isItalic=false, forceFakeBold=true) -> wght=400 + Skia embolden
    renderCellToTexture(glTextureIds[2], cellW, cellH, true, false, sampleText, true);

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