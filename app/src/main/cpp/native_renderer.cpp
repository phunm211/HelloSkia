#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include "include/ports/SkFontMgr_directory.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "gpu/ganesh/gl/GrGLDirectContext.h"
#include "gpu/ganesh/gl/GrGLBackendSurface.h"
#include "gpu/ganesh/SkSurfaceGanesh.h"
#include "SkFont.h"
#include "gpu/ganesh/gl/GrGLAssembleInterface.h"

#define LOG_TAG "NativeRenderer"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
static ANativeWindow* nativeWindow = nullptr;
static EGLDisplay eglDisplay = EGL_NO_DISPLAY;
static EGLSurface eglSurface = EGL_NO_SURFACE;
static EGLContext eglContext = EGL_NO_CONTEXT;

sk_sp<GrDirectContext> grContext;
sk_sp<SkSurface> skSurface;

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeInit(JNIEnv* env, jobject, jobject surface) {
    // 1. Get Native Window
    nativeWindow = ANativeWindow_fromSurface(env, surface);
    if (!nativeWindow) {
        LOGE("nativeInit: ANativeWindow_fromSurface failed. jsurface: %p", surface);
        return;
    }
    LOGI("nativeInit: ANativeWindow_fromSurface successful (nativeWindow: %p)", nativeWindow);

    // 2. Get EGL Display
    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY) {
        LOGE("nativeInit: eglGetDisplay failed.");
        ANativeWindow_release(nativeWindow); // Dọn dẹp window đã lấy
        nativeWindow = nullptr;
        return;
    }
    LOGI("nativeInit: eglGetDisplay successful (eglDisplay: %p)", eglDisplay);

    // 3. Initialize EGL
    EGLint majorVersion, minorVersion;
    if (!eglInitialize(eglDisplay, &majorVersion, &minorVersion)) {
        LOGE("nativeInit: eglInitialize failed.");
        eglDisplay = EGL_NO_DISPLAY; // Đánh dấu là không có display hợp lệ
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
        return;
    }
    LOGI("nativeInit: EGL Initialized. Version: %d.%d", majorVersion, minorVersion);

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

    EGLint esVersionToTry = 3; // Thử ES3 trước
    if (!eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfigs) || numConfigs < 1) {
        LOGI("nativeInit: eglChooseConfig for ES%d failed or no configs found (numConfigs: %d). Error: 0x%x. Trying ES2...", esVersionToTry, numConfigs, eglGetError());
        esVersionToTry = 2;
        configAttribs[1] = EGL_OPENGL_ES2_BIT; // Chuyển sang ES2
        LOGI("nativeInit: Attempting to choose EGLConfig for OpenGL ES %d", esVersionToTry);
        if (!eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfigs) || numConfigs < 1) {
            LOGE("nativeInit: eglChooseConfig for ES%d also failed or no configs found (numConfigs: %d).", esVersionToTry, numConfigs);
            return;
        }
    }
    LOGI("nativeInit: eglChooseConfig successful for ES%d (numConfigs: %d, selectedConfig: %p)", esVersionToTry, numConfigs, eglConfig);
    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    eglContext = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext == EGL_NO_CONTEXT) {
        LOGE("nativeInit: eglCreateContext failed for ES version %d.", esVersionToTry);
        return;
    }

    eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, nativeWindow, nullptr);
    if (eglSurface == EGL_NO_SURFACE) {
        LOGE("nativeInit: eglCreateWindowSurface failed!");
        // Gọi eglGetError() NGAY LẬP TỨC để lấy mã lỗi chính xác
        EGLint error = eglGetError();
        LOGE("nativeInit: eglCreateWindowSurface error code: 0x%x", error);

        // Phân tích một số mã lỗi phổ biến:
        switch (error) {
            case EGL_BAD_MATCH:
                LOGE("EGL_BAD_MATCH: Check if native_window or native_display is valid, "
                     "or if an EGLConfig does not support rendering to a window, "
                     "or if the EGLConfig does not support the EGL_SURFACE_TYPE attribute EGL_WINDOW_BIT.");
                break;
            case EGL_BAD_CONFIG:
                LOGE("EGL_BAD_CONFIG: egl_config is not a valid EGLConfig.");
                break;
            case EGL_BAD_NATIVE_WINDOW:
                LOGE("EGL_BAD_NATIVE_WINDOW: native_window is not a valid window.");
                break;
            case EGL_BAD_ALLOC:
                LOGE("EGL_BAD_ALLOC: Allocation failed (out of memory or other resources).");
                break;
            case EGL_BAD_ATTRIBUTE:
                LOGE("EGL_BAD_ATTRIBUTE: One or more attributes in attrib_list is invalid or "
                     "inconsistent (e.g., an attribute repeated).");
                break;
            default:
                LOGE("EGL_UNKNOWN_ERROR: An unknown error occurred.");
        }

        // Dọn dẹp EGLContext đã được tạo trước đó
        if (eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay, eglContext);
            eglContext = EGL_NO_CONTEXT;
        }
        // Dọn dẹp EGLDisplay
        if (eglDisplay != EGL_NO_DISPLAY) {
            eglTerminate(eglDisplay);
            eglDisplay = EGL_NO_DISPLAY;
        }
        // Dọn dẹp NativeWindow
        if (nativeWindow) {
            ANativeWindow_release(nativeWindow);
            nativeWindow = nullptr;
        }
        return; // Thoát khỏi hàm init vì đã thất bại
    }
    LOGI("nativeInit: eglCreateWindowSurface successful (eglSurface: %p)", eglSurface);

    // 6. Make EGLContext Current
    LOGI("nativeInit: Making EGL context current...");
    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
        LOGE("nativeInit: eglMakeCurrent failed.");
        return;
    }
    LOGI("nativeInit: eglMakeCurrent successful.");

    auto ptr = eglGetProcAddress("glGetString");
    if (!ptr) {
        LOGE("eglGetProcAddress(glGetString) returned nullptr!");
    } else {
        LOGI("eglGetProcAddress(glGetString) OK: %p", ptr);
    }



    // 7. Initialize Skia GrContext
    LOGI("nativeInit: Creating GrGLInterface...");
    auto interface = GrGLMakeAssembledInterface(
            nullptr,
            [](void*, const char name[]) -> GrGLFuncPtr {
                auto ptr = eglGetProcAddress(name);
                if (!ptr) {
                    LOGE("eglGetProcAddress failed for %s", name);
                }
                return reinterpret_cast<GrGLFuncPtr>(ptr);
            });
    if (!interface) {
        LOGE("nativeInit: GrGLMakeNativeInterface failed to create an interface.");
        return;
    }
    LOGI("nativeInit: GrGLInterface created successfully.");

    LOGI("nativeInit: Creating GrDirectContext...");
    grContext = GrDirectContexts::MakeGL(interface);
    if (!grContext) {
        LOGE("nativeInit: GrDirectContexts::MakeGL failed to create a context.");
        // Interface được GrDirectContext sở hữu, không cần giải phóng riêng nếu MakeGL thành công.
        // Nếu MakeGL thất bại, interface sẽ tự động được giải phóng (do là sk_sp).
        return;
    }
    LOGI("nativeInit: GrDirectContext created successfully (grContext: %p)", grContext.get());

    int width = ANativeWindow_getWidth(nativeWindow);
    int height = ANativeWindow_getHeight(nativeWindow);

    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = 0;
    fbInfo.fFormat = GL_RGBA8;

    int sampleCount = 0;     // không dùng MSAA
    int stencilBits = 8;     // stencil buffer 8 bits

    auto backendRT = GrBackendRenderTargets::MakeGL(width, height, sampleCount, stencilBits, fbInfo);

    SkColorType colorType = kRGBA_8888_SkColorType;
    skSurface = SkSurfaces::WrapBackendRenderTarget(
            grContext.get(), backendRT,
            kBottomLeft_GrSurfaceOrigin,
            colorType,
            SkColorSpace::MakeSRGB(),
            nullptr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeRender(JNIEnv*, jobject) {
    if (!skSurface) return;
    SkCanvas* canvas = skSurface->getCanvas();

    canvas->clear(SK_ColorWHITE);

    SkPaint paint;
    paint.setColor(SK_ColorGREEN);
    canvas->drawCircle(200, 200, 100, paint);

    paint.setAntiAlias(true);
    paint.setColor(SK_ColorBLUE);

    auto data = SkData::MakeFromFileName("/system/fonts/Roboto-Regular.ttf");
    if (!data) {
        LOGE("Failed to load font file");
        return;
    }


    sk_sp<SkFontMgr> fontMgr = SkFontMgr_New_Custom_Directory("/system/fonts");
    sk_sp<SkTypeface> typeface = fontMgr->matchFamilyStyle("Roboto", SkFontStyle());

    SkFont font(typeface, 50);

    canvas->drawString("Hello anh An béo by Skia", 100, 400, font, paint);

    grContext->flushAndSubmit();
    eglSwapBuffers(eglDisplay, eglSurface);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_helloskia_MainActivity_nativeDestroy(JNIEnv*, jobject) {
    skSurface.reset();
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