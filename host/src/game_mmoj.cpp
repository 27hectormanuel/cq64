// SPDX-License-Identifier: CC0-1.0
// Middle Manager of Justice (com.doublefine.mmoj): Moai SDK (Lua) game in libmmoj.so.
// Java drives the engine from a GLSurfaceView (MoaiView) through the AKU* natives; the game data
// is an OBB expansion file that Java mounts with AKUMountVirtualDirectory.
#include "devtools.h"
#include "game.h"
#include "loader.h"
#include "natives.h"

#include <unistd.h>

namespace game {
const char *const kPackage = "com.doublefine.mmoj";
const std::vector<GuestLib> kLibs = {
    {"libguest_rt.so", "libguestrt.so"},
    {"libguest_fmodex.so", "libfmodex.so"},
    {"libguest_fmodevent.so", "libfmodevent.so"},
    {"libguest_mmoj.so", "libmmoj.so"},
};

// The OBB stays in shared storage (Android/obb/<package>/), which an app can read without
// permissions, so no path mappings are needed.
void setup() {}
void after_load(Module *) {}

// Moai renders from Java (MoaiRenderer.onDrawFrame -> AKURender), so frames are paced and counted
// here rather than in eglSwapBuffers.
namespace {
using RenderFn = void (*)(JNIEnv *, jclass);
RenderFn g_render;
void paced_render(JNIEnv *env, jclass cls) {
    devtools::frame_presented();
    g_render(env, cls);
}

// MoaiActivity.onDestroy calls AKUFinalize, whose teardown reads through a NULL Lua state in
// MOAIInputMgr's destructor (it would also crash on a real 32-bit phone, just silently at exit).
// End the process cleanly instead; Android starts a fresh one next time.
void finalize_exit(JNIEnv *, jclass) {
    LOGI("AKUFinalize: exiting");
    _exit(0);
}
}  // namespace

void register_natives(JNIEnv *env) {
    devtools::register_natives(env);  // com.doublefine.mmoj.DevTools: frame cap from Game Mode
    for (size_t i = 0; i < g_native_count; i++) {
        NativeDef &n = g_natives[i];
        void *fn;
        if (strcmp(n.name, "AKURender") == 0) {
            g_render = (RenderFn)n.fn;
            fn = (void *)&paced_render;
        } else if (strcmp(n.name, "AKUFinalize") == 0) {
            fn = (void *)&finalize_exit;
        } else {
            continue;
        }
        jclass c = env->FindClass(n.cls);
        JNINativeMethod m{n.name, n.sig, fn};
        if (!c || env->RegisterNatives(c, &m, 1) != JNI_OK) fatal("cannot wrap %s", n.name);
        env->DeleteLocalRef(c);
    }
}
void poll_debug() {}
}  // namespace game

// Generated from the APK's native method declarations (all exported by libmmoj/libfmodex).
NativeDef g_natives[] = {
    N(0, "com/ziplinegames/moai/Moai", "AKUAppBackButtonPressed", "()Z", jboolean),
    N(1, "com/ziplinegames/moai/Moai", "AKUAppDialogDismissed", "(I)V", void, jint),
    N(2, "com/ziplinegames/moai/Moai", "AKUAppDidStartSession", "(Z)V", void, jboolean),
    N(3, "com/ziplinegames/moai/Moai", "AKUAppWillEndSession", "()V", void),
    N(4, "com/ziplinegames/moai/Moai", "AKUCreateContext", "()I", jint),
    N(5, "com/ziplinegames/moai/Moai", "AKUDetectGfxContext", "()V", void),
    N(6, "com/ziplinegames/moai/Moai", "AKUEnqueueLevelEvent", "(IIFFF)V", void, jint, jint, jfloat, jfloat, jfloat),
    N(7, "com/ziplinegames/moai/Moai", "AKUEnqueueTouchEvent", "(IIIZIII)V", void, jint, jint, jint, jboolean, jint, jint, jint),
    N(8, "com/ziplinegames/moai/Moai", "AKUExtLoadLuacrypto", "()V", void),
    N(9, "com/ziplinegames/moai/Moai", "AKUExtLoadLuacurl", "()V", void),
    N(10, "com/ziplinegames/moai/Moai", "AKUExtLoadLuasocket", "()V", void),
    N(11, "com/ziplinegames/moai/Moai", "AKUExtLoadLuasql", "()V", void),
    N(12, "com/ziplinegames/moai/Moai", "AKUFMODInit", "()V", void),
    N(13, "com/ziplinegames/moai/Moai", "AKUFMODRelease", "()V", void),
    N(14, "com/ziplinegames/moai/Moai", "AKUFinalize", "()V", void),
    N(15, "com/ziplinegames/moai/Moai", "AKUFmodDesignerUpdate", "(F)V", void, jfloat),
    N(16, "com/ziplinegames/moai/Moai", "AKUGetSimStep", "()D", jdouble),
    N(17, "com/ziplinegames/moai/Moai", "AKUInit", "()V", void),
    N(18, "com/ziplinegames/moai/Moai", "AKUMountVirtualDirectory", "(Ljava/lang/String;Ljava/lang/String;)V", void, O, O),
    N(19, "com/ziplinegames/moai/Moai", "AKUPause", "(Z)V", void, jboolean),
    N(20, "com/ziplinegames/moai/Moai", "AKURender", "()V", void),
    N(21, "com/ziplinegames/moai/Moai", "AKUReserveInputDeviceSensors", "(II)V", void, jint, jint),
    N(22, "com/ziplinegames/moai/Moai", "AKUReserveInputDevices", "(I)V", void, jint),
    N(23, "com/ziplinegames/moai/Moai", "AKURunScript", "(Ljava/lang/String;)V", void, O),
    N(24, "com/ziplinegames/moai/Moai", "AKUSetConnectionType", "(J)V", void, jlong),
    N(25, "com/ziplinegames/moai/Moai", "AKUSetContext", "(I)V", void, jint),
    N(26, "com/ziplinegames/moai/Moai", "AKUSetDeviceProperties", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", void, O, O, O, O, O, O, O, O, O, jint, O, O, O),
    N(27, "com/ziplinegames/moai/Moai", "AKUSetDocumentDirectory", "(Ljava/lang/String;)V", void, O),
    N(28, "com/ziplinegames/moai/Moai", "AKUSetInputConfigurationName", "(Ljava/lang/String;)V", void, O),
    N(29, "com/ziplinegames/moai/Moai", "AKUSetInputDevice", "(ILjava/lang/String;)V", void, jint, O),
    N(30, "com/ziplinegames/moai/Moai", "AKUSetInputDeviceCompass", "(IILjava/lang/String;)V", void, jint, jint, O),
    N(31, "com/ziplinegames/moai/Moai", "AKUSetInputDeviceKeyboard", "(IILjava/lang/String;)V", void, jint, jint, O),
    N(32, "com/ziplinegames/moai/Moai", "AKUSetInputDeviceLevel", "(IILjava/lang/String;)V", void, jint, jint, O),
    N(33, "com/ziplinegames/moai/Moai", "AKUSetInputDeviceLocation", "(IILjava/lang/String;)V", void, jint, jint, O),
    N(34, "com/ziplinegames/moai/Moai", "AKUSetInputDeviceTouch", "(IILjava/lang/String;)V", void, jint, jint, O),
    N(35, "com/ziplinegames/moai/Moai", "AKUSetScreenSize", "(II)V", void, jint, jint),
    N(36, "com/ziplinegames/moai/Moai", "AKUSetViewSize", "(II)V", void, jint, jint),
    N(37, "com/ziplinegames/moai/Moai", "AKUSetWorkingDirectory", "(Ljava/lang/String;)V", void, O),
    N(38, "com/ziplinegames/moai/Moai", "AKUUntzInit", "()V", void),
    N(39, "com/ziplinegames/moai/Moai", "AKUUpdate", "()V", void),
    N(40, "com/ziplinegames/moai/Moai", "DFEffectsInit", "()V", void),
    N(41, "com/ziplinegames/moai/Moai", "DFMmojInit", "()V", void),
    N(42, "com/ziplinegames/moai/MoaiAmazonBilling", "AKUNotifyAmazonBillingSupported", "(Z)V", void, jboolean),
    N(43, "com/ziplinegames/moai/MoaiAmazonBilling", "AKUNotifyAmazonItemResponse", "(Ljava/lang/String;)V", void, O),
    N(44, "com/ziplinegames/moai/MoaiAmazonBilling", "AKUNotifyAmazonPurchaseResponseReceived", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", void, jint, O, O, O, O, O),
    N(45, "com/ziplinegames/moai/MoaiAmazonBilling", "AKUNotifyAmazonPurchaseStateChanged", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", void, jint, O, O, O, O),
    N(46, "com/ziplinegames/moai/MoaiAmazonBilling", "AKUNotifyAmazonRestoreResponseReceived", "(IZLjava/lang/String;)V", void, jint, jboolean, O),
    N(47, "com/ziplinegames/moai/MoaiAmazonBilling", "AKUNotifyAmazonUserIdDetermined", "(ILjava/lang/String;)V", void, jint, O),
    N(48, "com/ziplinegames/moai/MoaiGoogleBilling", "AKUNotifyGooglePurchaseResponseReceived", "(ILjava/lang/String;)V", void, jint, O),
    N(49, "com/ziplinegames/moai/MoaiGooglePushReceiver", "AKUNotifyGooglePushRemoteNotificationReceived", "([Ljava/lang/String;[Ljava/lang/String;)V", void, O, O),
    N(50, "com/ziplinegames/moai/MoaiGooglePushReceiver", "AKUNotifyGooglePushRemoteNotificationRegistrationComplete", "(ILjava/lang/String;)V", void, jint, O),
    N(51, "com/ziplinegames/moai/MoaiLocalNotificationReceiver", "AKUNotifyLocalNotificationReceived", "([Ljava/lang/String;[Ljava/lang/String;)V", void, O, O),
    N(52, "com/ziplinegames/moai/MoaiMoviePlayer", "AKUNotifyMoviePlayerCompleted", "()V", void),
    N(53, "com/ziplinegames/moai/MoaiMoviePlayer", "AKUNotifyMoviePlayerReady", "()V", void),
    N(54, "org/fmod/FMODAudioDevice", "fmodGetInfo", "(I)I", jint, jint),
    N(55, "org/fmod/FMODAudioDevice", "fmodProcess", "(Ljava/nio/ByteBuffer;)I", jint, O),
};
const size_t g_native_count = sizeof g_natives / sizeof g_natives[0];
