/**
 * bridge_121.cpp - LegoClicker Native Bridge for Minecraft 1.21 (LWJGL3/ImGui)
 * Build: 2026-02-20
 *
 * Architecture:
 *  - Hooks wglSwapBuffers -> renders ImGui overlay every frame.
 *  - Hooks WndProc -> intercepts INSERT/ESC, blocks game input when GUI is open.
 *  - Hooks glfwSetInputMode -> detects when Minecraft naturally re-grabs cursor.
 *  - Maintains TCP server (port 25590) for bidirectional comms with C# Loader.
 *
 * Cursor management (zero-flick):
 *  - Open GUI: call glfwSetInputMode(CURSOR_NORMAL), block WM_INPUT.
 *  - Close GUI: block WM_INPUT indefinitely, do NOT call CURSOR_DISABLED.
 *  - When Minecraft's own game tick calls glfwSetInputMode(CURSOR_DISABLED):
 *    our hooked function clears the WM_INPUT block and forwards to real GLFW.
 *  - This means Minecraft handles its own re-grab at its own timing, zero delta,
 *    zero camera flick.
 *
 * Inventory detection: glfwGetInputMode polling.
 *  - If cursor mode is NORMAL and our GUI is NOT open, a Minecraft screen is open.
 *
 * Command API (bridge -> C#): action names match GameStateClient.HandleBridgeCommand
 *  toggleArmed, setMinCPS, setMaxCPS, toggleJitter, toggleClickInChests,
 *  toggleNametags, toggleChestEsp, toggleClosestPlayerInfo,
 *  toggleNametagHealth, toggleNametagArmor, toggleRight, toggleBreakBlocks
 */
#include <winsock2.h>
#include <windows.h>
#include <jni.h>
#include <jvmti.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <GL/gl.h>
#include "gl_loader.h"
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

// MinGW's <GL/gl.h> may not declare modern GL enums used with glGetIntegerv.
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER_BINDING
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif

// Custom extension in our vendored imgui_impl_opengl3.cpp
void ImGui_ImplOpenGL3_SetSkipGLDeletes(bool skip);

// Forward decls (some helpers are defined later in this translation unit)
static void Log(const std::string& msg);
static jclass LoadClassWithLoader(JNIEnv* env, jobject cl, const char* name);
static std::string GetClassNameFromClass(JNIEnv* env, jclass cls);
static std::string CallTextToString(JNIEnv* env, jobject textObj);

// ===================== MUTEX =====================
class Mutex {
    CRITICAL_SECTION cs;
public:
    Mutex()  { InitializeCriticalSection(&cs); }
    ~Mutex() { DeleteCriticalSection(&cs); }
    void lock()   { EnterCriticalSection(&cs); }
    void unlock() { LeaveCriticalSection(&cs); }
};
class LockGuard {
    Mutex& m;
public:
    LockGuard(Mutex& m) : m(m) { m.lock(); }
    ~LockGuard() { m.unlock(); }
};

// ===================== CONFIG (mirrors C# Clicker state) =====================
struct Config {
    bool  armed          = false;
    bool  clicking       = false;
    float minCPS         = 10.0f;
    float maxCPS         = 14.0f;
    bool  jitter         = false;
    bool  clickInChests  = false;
    bool  aimAssist      = false;
    bool  nametags       = false;
    bool  chestEsp       = false;
    bool  closestPlayer  = false;
    bool  nametagHealth  = true;
    bool  nametagArmor   = true;
    int   nametagMaxCount = 8;
    int   chestEspMaxCount = 5;
    bool  rightClick     = false;
    float rightMinCPS    = 10.0f;
    float rightMaxCPS    = 14.0f;
    bool  rightBlockOnly = false;
    bool  breakBlocks    = false;
    bool  gtbHelper      = false;
    int   gtbCount       = 0;
    std::string gtbHint;
    std::string gtbPreview;
};
static Config g_config;
static Mutex  g_configMutex;

// ===================== PENDING COMMANDS (bridge -> C#) =====================
static std::vector<std::string> g_pendingCmds;
static Mutex g_cmdMutex;

static void SendCmd(const char* action) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"cmd\",\"action\":\"%s\"}\n", action);
    LockGuard lk(g_cmdMutex);
    g_pendingCmds.push_back(buf);
}
static void SendCmdFloat(const char* action, float val) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"cmd\",\"action\":\"%s\",\"value\":%.2f}\n", action, val);
    LockGuard lk(g_cmdMutex);
    g_pendingCmds.push_back(buf);
}

// ===================== CONFIG PARSER =====================
static void ParseConfig(const std::string& line) {
    auto getStr = [&](const char* key) -> std::string {
        std::string k = std::string("\"") + key + "\":";
        size_t p = line.find(k);
        if (p == std::string::npos) return "";
        p += k.length();
        if (p >= line.size()) return "";
        if (line[p] == '"') {
            size_t e = line.find('"', p+1);
            return (e == std::string::npos) ? "" : line.substr(p+1, e-p-1);
        }
        size_t e = line.find_first_of(",}", p);
        return (e == std::string::npos) ? line.substr(p) : line.substr(p, e-p);
    };
    auto getBool  = [&](const char* k) { return getStr(k) == "true"; };
    auto getFloat = [&](const char* k) -> float {
        std::string v = getStr(k);
        if (v.empty()) return 0.0f;
        try { return std::stof(v); } catch(...) { return 0.0f; }
    };
    auto getInt = [&](const char* k, int def) -> int {
        std::string v = getStr(k);
        if (v.empty()) return def;
        try { return std::stoi(v); } catch(...) { return def; }
    };

    if (getStr("type") != "config") return;

    LockGuard lk(g_configMutex);
    g_config.armed         = getBool("armed");
    g_config.clicking      = getBool("clicking");
    g_config.minCPS        = getFloat("minCPS");
    g_config.maxCPS        = getFloat("maxCPS");
    g_config.jitter        = getBool("jitter");
    g_config.clickInChests = getBool("clickInChests");
    g_config.aimAssist     = getBool("aimAssist");
    g_config.nametags      = getBool("nametags");
    g_config.chestEsp      = getBool("chestEsp");
    g_config.closestPlayer = getBool("closestPlayerInfo");
    g_config.nametagHealth = getBool("nametagShowHealth");
    g_config.nametagArmor  = getBool("nametagShowArmor");
    g_config.nametagMaxCount = (std::max)(1, (std::min)(20, getInt("nametagMaxCount", g_config.nametagMaxCount)));
    g_config.chestEspMaxCount = (std::max)(1, (std::min)(20, getInt("chestEspMaxCount", g_config.chestEspMaxCount)));
    g_config.rightClick    = getBool("right");
    g_config.rightMinCPS   = getFloat("rightMinCPS");
    g_config.rightMaxCPS   = getFloat("rightMaxCPS");
    g_config.rightBlockOnly= getBool("rightBlock");
    g_config.breakBlocks   = getBool("breakBlocks");
    g_config.gtbHelper     = getBool("gtbHelper");
    g_config.gtbHint       = getStr("gtbHint");
    g_config.gtbCount      = getInt("gtbCount", 0);
    g_config.gtbPreview    = getStr("gtbPreview");
}

// ===================== GLOBALS =====================
static bool g_running          = true;
static bool g_imguiInitialized = false;
static bool g_ShowMenu         = false;
static bool g_realGuiOpen      = false;

// Track the OpenGL context used for ImGui rendering. Minecraft/Lunar can recreate GL contexts
// (resolution/fullscreen changes, GPU resets, etc.). If we keep using stale GL objects, the
// driver may crash inside SwapBuffers/flipFrame.
static HGLRC g_imguiGlrc = nullptr;
static bool  g_imguiGlBackendReady = false;

// If the host recreates the OpenGL context, we need to tear down and re-init ImGui's
// OpenGL backend. We MUST avoid calling glDelete* on stale object IDs when the old
// context isn't current (can delete unrelated objects and corrupt rendering).
static bool  g_imguiPendingBackendReset = false;
static HGLRC g_imguiPendingGlrc = nullptr;

// Render-thread scheduled actions (WndProc just flips these flags).
static volatile LONG g_reqOpenMenu  = 0;
static volatile LONG g_reqCloseMenu = 0;

// When > 0, the block expiry time (ms). WM_INPUT absorbed until GetTickCount() > this.
// Set when closing GUI; expires after 250ms so even fast mouse movements
// caused by accumulated raw deltas are swallowed before Minecraft processes them.
static volatile DWORD g_blockUntilMs = 0;
// When non-zero, time when we should re-enable GLFW raw mouse motion after closing GUI.
static volatile DWORD g_enableRawMouseAtMs = 0;

static HWND   g_hwnd     = nullptr;
static WNDPROC o_WndProc = nullptr;
static JavaVM* g_jvm     = nullptr;

// Cached game ClassLoader (global ref) used for safe class loads.
static jobject g_gameClassLoader = nullptr;

// JNI globals: chat screen cursor + game state
static jobject   g_mcInstance      = nullptr;
static jmethodID g_setScreenMethod = nullptr; // Minecraft.setScreen(Screen)
static jclass    g_chatScreenClass = nullptr;
static jmethodID g_chatScreenCtor  = nullptr;
static int       g_chatCtorKind    = 0; // 0=()V, 1=(String)V, 2=(String,Z)V
static jfieldID  g_screenField     = nullptr; // Minecraft.currentScreen/screen
static bool      g_chatJniReady    = false;
static bool      g_stateJniReady   = false;
static std::string g_screenType;              // FQ name of Screen base class

// Per-frame JNI state (read in SwapBuffers, consumed in TCP)
static std::string g_jniScreenName;
static std::string g_jniActionBar;
static bool        g_jniGuiOpen     = false;
static bool        g_jniInWorld     = false;
static bool        g_jniLookingAtBlock = false;
static bool        g_jniBreakingBlock  = false;
static bool        g_jniHoldingBlock   = false;
static Mutex       g_jniStateMtx;
static std::string g_lastLoggedScreen;
static jfieldID    g_inGameHudField_121 = nullptr; // MinecraftClient.inGameHud
static std::vector<jfieldID> g_hudTextFields_121;  // InGameHud Text fields
static DWORD       g_lastHudTextProbeMs = 0;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Matrix4x4 {
    float m[16];
};

static jclass    g_renderSystemClass_121 = nullptr;
static jmethodID g_getProjectionMatrix_121 = nullptr;
static jmethodID g_getModelViewMatrix_121  = nullptr;

static jclass    g_matrix4fClass_121 = nullptr;
static jfieldID  g_matrixM00=nullptr, g_matrixM01=nullptr, g_matrixM02=nullptr, g_matrixM03=nullptr;
static jfieldID  g_matrixM10=nullptr, g_matrixM11=nullptr, g_matrixM12=nullptr, g_matrixM13=nullptr;
static jfieldID  g_matrixM20=nullptr, g_matrixM21=nullptr, g_matrixM22=nullptr, g_matrixM23=nullptr;
static jfieldID  g_matrixM30=nullptr, g_matrixM31=nullptr, g_matrixM32=nullptr, g_matrixM33=nullptr;
static jmethodID g_matrixGetFloatArray_121 = nullptr; // Matrix4f.get(float[]) -> float[]

// JNI globals: camera extraction
static jfieldID  g_gameRendererField_121 = nullptr; // Lnet/minecraft/class_757;
static jfieldID  g_gameRendererCameraField_121 = nullptr; // Lnet/minecraft/class_4184;

static jclass    g_cameraClass_121 = nullptr;
static jfieldID  g_cameraPosF_121= nullptr;
static jfieldID  g_cameraYawF_121 = nullptr;
static jfieldID  g_cameraPitchF_121 = nullptr;

static jclass    g_vec3dClass_121  = nullptr;
static jfieldID  g_vec3dX_121=nullptr, g_vec3dY_121=nullptr, g_vec3dZ_121=nullptr;

// Cached Lunar Client saved-matrix field IDs (set once by bg thread, valid for lifetime)
static jfieldID  g_lunarProjField_121 = nullptr;
static jfieldID  g_lunarViewField_121 = nullptr;

// Shared camera state: written by background thread, read by render thread (no JNI on render thread)
struct BgCamState {
    double camX = 0, camY = 0, camZ = 0;
    float  yaw = 0, pitch = 0;
    bool   camFound = false;
    Matrix4x4 proj = {}, view = {};
    bool   matsOk = false;
};
static BgCamState  g_bgCamState = {};
static Mutex       g_bgCamMutex;

// Removed EnsureReflectInvokeCaches / FindZeroArgMethodReturningClass as we fetch Camera directly via fields now.

static bool ReadMatrix4f(JNIEnv* env, jobject matObj, Matrix4x4& out) {
    if (!matObj) return false;

    // Only use safe direct field reads — no CallObjectMethod on the render thread.
    const bool haveFields =
        g_matrixM00 && g_matrixM01 && g_matrixM02 && g_matrixM03 &&
        g_matrixM10 && g_matrixM11 && g_matrixM12 && g_matrixM13 &&
        g_matrixM20 && g_matrixM21 && g_matrixM22 && g_matrixM23 &&
        g_matrixM30 && g_matrixM31 && g_matrixM32 && g_matrixM33;

    if (!haveFields) return false;

    out.m[0]  = env->GetFloatField(matObj, g_matrixM00);
    out.m[1]  = env->GetFloatField(matObj, g_matrixM01);
    out.m[2]  = env->GetFloatField(matObj, g_matrixM02);
    out.m[3]  = env->GetFloatField(matObj, g_matrixM03);
    out.m[4]  = env->GetFloatField(matObj, g_matrixM10);
    out.m[5]  = env->GetFloatField(matObj, g_matrixM11);
    out.m[6]  = env->GetFloatField(matObj, g_matrixM12);
    out.m[7]  = env->GetFloatField(matObj, g_matrixM13);
    out.m[8]  = env->GetFloatField(matObj, g_matrixM20);
    out.m[9]  = env->GetFloatField(matObj, g_matrixM21);
    out.m[10] = env->GetFloatField(matObj, g_matrixM22);
    out.m[11] = env->GetFloatField(matObj, g_matrixM23);
    out.m[12] = env->GetFloatField(matObj, g_matrixM30);
    out.m[13] = env->GetFloatField(matObj, g_matrixM31);
    out.m[14] = env->GetFloatField(matObj, g_matrixM32);
    out.m[15] = env->GetFloatField(matObj, g_matrixM33);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return true;
}

static double CallDoubleNoArgs(JNIEnv* env, jobject obj, jmethodID method) {
    if (!obj || !method) return 0.0;
    double res = env->CallDoubleMethod(obj, method);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0.0; }
    return res;
}

static float CallFloatNoArgs(JNIEnv* env, jobject obj, jmethodID method) {
    if (!obj || !method) return 0.0f;
    float res = env->CallFloatMethod(obj, method);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0.0f; }
    return res;
}


struct LegoVec3 { double x, y, z; };

static bool WorldToScreen(LegoVec3 pos, LegoVec3 camPos, const Matrix4x4& view, const Matrix4x4& proj, int winW, int winH, float* sx, float* sy) {
    // 1. Translate point to view space (Entity pos - Camera pos)
    float dx = (float)(pos.x - camPos.x);
    float dy = (float)(pos.y - camPos.y);
    float dz = (float)(pos.z - camPos.z);

    // 2. Multiply by ModelView matrix (JOML Matrix4f is column-major)
    float clipX = dx * view.m[0] + dy * view.m[4] + dz * view.m[8] + view.m[12];
    float clipY = dx * view.m[1] + dy * view.m[5] + dz * view.m[9] + view.m[13];
    float clipZ = dx * view.m[2] + dy * view.m[6] + dz * view.m[10] + view.m[14];
    float clipW = dx * view.m[3] + dy * view.m[7] + dz * view.m[11] + view.m[15];

    // 3. Multiply by Projection matrix
    float ndcX = clipX * proj.m[0] + clipY * proj.m[4] + clipZ * proj.m[8] + clipW * proj.m[12];
    float ndcY = clipX * proj.m[1] + clipY * proj.m[5] + clipZ * proj.m[9] + clipW * proj.m[13];
    float ndcZ = clipX * proj.m[2] + clipY * proj.m[6] + clipZ * proj.m[10] + clipW * proj.m[14];
    float ndcW = clipX * proj.m[3] + clipY * proj.m[7] + clipZ * proj.m[11] + clipW * proj.m[15];

    if (!std::isfinite(ndcW) || ndcW < 0.1f) return false;

    // 4. Perspective Divide
    ndcX /= ndcW;
    ndcY /= ndcW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return false;

    // 5. Map to screen (Vulkan/DirectX y-axis mapping used in modern MC? No, OpenGL is +Y up. 
    // Minecraft maps top-left as 0,0 for GUI. ndcY +1 is top, -1 is bottom. 
    // So 1.0f - ndcY will map top to 0.)
    *sx = (ndcX + 1.0f) * 0.5f * (float)winW;
    *sy = (1.0f - ndcY) * 0.5f * (float)winH;
    if (!std::isfinite(*sx) || !std::isfinite(*sy)) return false;

    return true;
}

// WorldToScreen using camera angles (yaw/pitch from JNI) -- no OpenGL state needed.
// yawDeg:   Minecraft entity yaw (0=south, 90=west, 180=north, 270=east)
// pitchDeg: Minecraft entity pitch (+ve = looking down, -ve = looking up)
// fovDeg:   Vertical field-of-view in degrees (default MC is 70).
static bool WorldToScreen_Angles(LegoVec3 worldPos, LegoVec3 camPos,
                                  float yawDeg, float pitchDeg, float fovDeg,
                                  int winW, int winH, float* sx, float* sy) {
    float dx = (float)(worldPos.x - camPos.x);
    float dy = (float)(worldPos.y - camPos.y);
    float dz = (float)(worldPos.z - camPos.z);

    const float PI = 3.14159265f;
    float yaw   = yawDeg   * (PI / 180.0f);
    float pitch = pitchDeg * (PI / 180.0f);
    float sinY  = sinf(yaw),   cosY  = cosf(yaw);
    float sinP  = sinf(pitch), cosP  = cosf(pitch);

    // Minecraft axes: +X east, +Y up, +Z south
    // Forward vector (direction camera faces, yaw=0 → south)
    float fX = -sinY * cosP;
    float fY = -sinP;
    float fZ =  cosY * cosP;

    // Build an orthonormal basis using cross products to avoid handedness mistakes.
    // right = normalize(worldUp x forward) so that yaw=0 -> right = +X (east)
    const float upX0 = 0.0f, upY0 = 1.0f, upZ0 = 0.0f;
    float rX = upY0 * fZ - upZ0 * fY;
    float rY = upZ0 * fX - upX0 * fZ;
    float rZ = upX0 * fY - upY0 * fX;
    float rLen = sqrtf(rX * rX + rY * rY + rZ * rZ);
    if (rLen < 1e-6f) {
        // Looking straight up/down; pick an arbitrary right.
        rX = 1.0f; rY = 0.0f; rZ = 0.0f;
        rLen = 1.0f;
    }
    rX /= rLen; rY /= rLen; rZ /= rLen;

    // up = forward x right (right-handed)
    float uX = fY * rZ - fZ * rY;
    float uY = fZ * rX - fX * rZ;
    float uZ = fX * rY - fY * rX;

    // Project delta onto view axes
    float vFwd   = dx * fX + dy * fY + dz * fZ;
    float vRight = dx * rX + dy * rY + dz * rZ;
    float vUp    = dx * uX + dy * uY + dz * uZ;

    if (!std::isfinite(vFwd) || vFwd < 0.1f) return false;   // behind camera

    float aspect     = (float)winW / (float)winH;
    float tanHalfFov = tanf(fovDeg * 0.5f * (PI / 180.0f));

    float ndcX = (vRight / vFwd) / (tanHalfFov * aspect);
    float ndcY = (vUp    / vFwd) / tanHalfFov;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return false;

    // Don't early-reject based on NDC bounds; callers can clamp/skip based on their own logic.
    // This prevents boxes from disappearing when some corners are off-screen.

    *sx = (ndcX + 1.0f) * 0.5f * (float)winW;
    *sy = (1.0f - ndcY) * 0.5f * (float)winH;
    if (!std::isfinite(*sx) || !std::isfinite(*sy)) return false;
    return true;
}

// ===================== RENDER MODULE STATE (1.21 incremental) =====================
static std::string g_closestName;
static double      g_closestDist = -1.0;
static DWORD       g_lastClosestUpdateMs = 0;

struct PlayerData121 {
    std::string name;
    double dist;
    double ex, ey, ez;
    double hp;
    int armor;
    std::string heldItem;
};
static std::vector<PlayerData121> g_playerList;
static Mutex g_playerListMutex;
static DWORD g_lastPlayerListUpdateMs = 0;

struct ChestData121 { double x, y, z; double dist; };
static std::vector<ChestData121> g_chestList;
static Mutex g_chestListMutex;
static DWORD g_lastChestScanMs = 0;
// Chunk-based block entity access (1.21: no flat list on world, BEs live in WorldChunk.blockEntities Map)
static jmethodID g_worldGetChunkMethod_121       = nullptr; // World.getChunk(II) -> WorldChunk
static jfieldID  g_chunkBlockEntitiesMapField_121 = nullptr; // WorldChunk.blockEntities: Map<BlockPos,BE>
static jclass    g_blockEntityClass_121           = nullptr;

// BlockEntity.getPos() -> BlockPos
static jmethodID g_beGetPos_121 = nullptr; 
static jfieldID  g_blockPosX_121 = nullptr;
static jfieldID  g_blockPosY_121 = nullptr;
static jfieldID  g_blockPosZ_121 = nullptr;
static jclass    g_blockPosClass_121 = nullptr;

// Direct BlockEntity.pos field access (avoids beGetPos_121 CallObjectMethod per chest)
static jfieldID  g_beBlockPosField_121    = nullptr;
// Direct Entity.pos field (Vec3d) — avoids getX/Y/Z CallDoubleMethod × N players
static jfieldID  g_entityPosField_121     = nullptr;
// HashMap.table[] direct access — eliminates values().iterator() call chain
static jclass    g_javaHashMapClass       = nullptr; // global ref for IsInstanceOf guard
static jfieldID  g_javaHashMapTableField  = nullptr;
static jfieldID  g_javaHMNodeValueField   = nullptr;
static jfieldID  g_javaHMNodeNextField    = nullptr;
static jfieldID  g_javaHMNodeKeyField     = nullptr; // Map key = BlockPos directly
// BlockPos coordinate methods (fallback when direct fields fail)
static jmethodID g_blockPosGetX_121       = nullptr;
static jmethodID g_blockPosGetY_121       = nullptr;
static jmethodID g_blockPosGetZ_121       = nullptr;
// World transition guard: skip chunk scanning while joining servers
static volatile DWORD g_worldTransitionEndMs = 0;

// Chest detection helpers (mapping/obf-robust)
static jclass g_blockStateClass_121 = nullptr; // net.minecraft.class_2680
static jclass g_blockClass_121      = nullptr; // net.minecraft.class_2248
static jmethodID g_beGetCachedState_121 = nullptr;
static jmethodID g_stateGetBlock_121    = nullptr;
static jmethodID g_blockGetTranslationKey_121 = nullptr;

// HitResult -> Type (for lookingAtBlock)
static jmethodID g_hitResultGetType_121 = nullptr;

// JNI caches for closest player
static jfieldID  g_worldField_121  = nullptr; // MinecraftClient.world
static jfieldID  g_playerField_121 = nullptr; // MinecraftClient.player
static jfieldID  g_worldPlayersListField_121 = nullptr; // ClientWorld.<players list>
static jclass    g_playerEntityClass_121 = nullptr; // net.minecraft.class_1657
static jmethodID g_getX_121 = nullptr;
static jmethodID g_getY_121 = nullptr;
static jmethodID g_getZ_121 = nullptr;
static jmethodID g_getYaw_121 = nullptr;
static jmethodID g_getPitch_121 = nullptr;
static jmethodID g_getHealth_121 = nullptr;
static jmethodID g_getName_121 = nullptr;      // Entity.getName() -> Text
static jmethodID g_textGetString_121 = nullptr; // Text.getString() -> String
static jmethodID g_getArmor_121 = nullptr;      // LivingEntity.getArmor() -> I
static jmethodID g_getMainHandStack_121 = nullptr; // LivingEntity.getMainHandStack() -> ItemStack
static jmethodID g_itemStackGetName_121 = nullptr; // ItemStack.getName() -> Text
static jmethodID g_itemStackGetDamage_121 = nullptr; // ItemStack.getDamage() -> I
static jmethodID g_itemStackGetMaxDamage_121 = nullptr; // ItemStack.getMaxDamage() -> I

// Stable name fallback (server-independent): GameProfile.getName()
static jclass    g_gameProfileClass_121 = nullptr; // com.mojang.authlib.GameProfile
static jmethodID g_getGameProfile_121 = nullptr;   // PlayerEntity.getGameProfile() -> GameProfile
static jmethodID g_gameProfileGetName_121 = nullptr; // GameProfile.getName() -> String

static jclass    g_itemStackClass_121 = nullptr;

static int GetEntityArmor(JNIEnv* env, jobject entity) {
    if (!env || !entity || !g_getArmor_121) return 0;
    int armor = env->CallIntMethod(entity, g_getArmor_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return armor;
}

static std::string CallTextToString(JNIEnv* env, jobject textObj); // forward decl

static std::string GetHeldItemInfo(JNIEnv* env, jobject entity) {
    if (!env || !entity || !g_getMainHandStack_121 || !g_itemStackGetName_121) return "";
    jobject stack = env->CallObjectMethod(entity, g_getMainHandStack_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ""; }
    if (!stack) return "";

    std::string result = "";
    
    // Name
    jobject textObj = env->CallObjectMethod(stack, g_itemStackGetName_121);
    if (!env->ExceptionCheck() && textObj) {
        result = CallTextToString(env, textObj);
        env->DeleteLocalRef(textObj);
    } else env->ExceptionClear();

    // Limit item name to ~14 chars
    if (result.length() > 14) result = result.substr(0, 14) + "..";

    // Damage
    if (g_itemStackGetDamage_121 && g_itemStackGetMaxDamage_121) {
        int dmg = env->CallIntMethod(stack, g_itemStackGetDamage_121);
        if (env->ExceptionCheck()) env->ExceptionClear();
        int maxDmg = env->CallIntMethod(stack, g_itemStackGetMaxDamage_121);
        if (env->ExceptionCheck()) env->ExceptionClear();
        
        if (maxDmg > 0) { // maxDmg > 0 means it's a damagable tool
            int remaining = maxDmg - dmg;
            char buf[32];
            snprintf(buf, sizeof(buf), " (%d/%d)", remaining, maxDmg);
            result += buf;
        }
    }

    env->DeleteLocalRef(stack);
    return result;
}

static void EnsureGameProfileCaches(JNIEnv* env, jobject anyPlayerObj) {
    if (!env) return;
    if (!g_gameProfileClass_121) {
        jclass c = env->FindClass("com/mojang/authlib/GameProfile");
        if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
        if (c) { g_gameProfileClass_121 = (jclass)env->NewGlobalRef(c); env->DeleteLocalRef(c); }
    }
    if (g_gameProfileClass_121 && !g_gameProfileGetName_121) {
        g_gameProfileGetName_121 = env->GetMethodID(g_gameProfileClass_121, "getName", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_gameProfileGetName_121 = nullptr; }
    }
    if (!g_getGameProfile_121 && anyPlayerObj && g_gameProfileClass_121) {
        jclass pCls = env->GetObjectClass(anyPlayerObj);
        if (pCls && !env->ExceptionCheck()) {
            // Common names across mappings
            g_getGameProfile_121 = env->GetMethodID(pCls, "getGameProfile", "()Lcom/mojang/authlib/GameProfile;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getGameProfile_121 = nullptr; }
            if (!g_getGameProfile_121) {
                g_getGameProfile_121 = env->GetMethodID(pCls, "method_7334", "()Lcom/mojang/authlib/GameProfile;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getGameProfile_121 = nullptr; }
            }
            env->DeleteLocalRef(pCls);
        } else {
            env->ExceptionClear();
        }
    }
}

static std::string GetStablePlayerName(JNIEnv* env, jobject playerObj) {
    if (!env || !playerObj) return "";

    // Primary: Entity.getName() -> Text -> String
    std::string name;
    if (g_getName_121) {
        jobject textObj = env->CallObjectMethod(playerObj, g_getName_121);
        if (env->ExceptionCheck()) { env->ExceptionClear(); textObj = nullptr; }
        name = textObj ? CallTextToString(env, textObj) : "";
        if (textObj) env->DeleteLocalRef(textObj);
    }

    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i > 0) s.erase(0, i);
    };
    trim(name);

    // If server/display overrides produce blank or "-", fall back to GameProfile.getName()
    if (!name.empty() && name != "-") return name;

    EnsureGameProfileCaches(env, playerObj);
    if (!g_getGameProfile_121 || !g_gameProfileGetName_121) return name;

    jobject gp = env->CallObjectMethod(playerObj, g_getGameProfile_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); gp = nullptr; }
    if (!gp) return name;
    jstring js = (jstring)env->CallObjectMethod(gp, g_gameProfileGetName_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); js = nullptr; }
    if (js) {
        const char* cs = env->GetStringUTFChars(js, nullptr);
        if (cs) { name = cs; env->ReleaseStringUTFChars(js, cs); }
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(gp);
    trim(name);
    return name;
}

// HitResult / crosshair target caches (for lookingAtBlock)
static jfieldID g_crosshairTargetField_121 = nullptr; // MinecraftClient.<hitResult>
static jclass   g_hitResultClass_121 = nullptr;       // net.minecraft.class_239
static jclass   g_blockHitResultClass_121 = nullptr;  // net.minecraft.class_3965

static void EnsureHitResultCaches(JNIEnv* env) {
    if (!env || !g_gameClassLoader) return;
    if (!g_hitResultClass_121) {
        jclass c = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_239");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (c) { g_hitResultClass_121 = (jclass)env->NewGlobalRef(c); env->DeleteLocalRef(c); }
    }
    if (!g_blockHitResultClass_121) {
        // Yarn 1.21.x BlockHitResult is class_3965; deobf name included as fallback.
        const char* names[] = { "net.minecraft.class_3965", "net.minecraft.util.hit.BlockHitResult", nullptr };
        for (int i = 0; names[i]; i++) {
            jclass c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (c) { g_blockHitResultClass_121 = (jclass)env->NewGlobalRef(c); env->DeleteLocalRef(c); break; }
        }
    }
}

static jmethodID ResolveHitResultGetType(JNIEnv* env) {
    if (!env) return nullptr;
    if (g_hitResultGetType_121) return g_hitResultGetType_121;
    EnsureHitResultCaches(env);
    if (!g_hitResultClass_121) return nullptr;

    // Most stable route: HitResult.getType() : HitResult.Type (enum)
    // Try a few known signatures across mappings.
    const char* names[] = { "getType", "method_17783", nullptr };
    const char* sigs[] = {
        "()Lnet/minecraft/class_239$class_240;",     // Yarn inner class name
        "()Lnet/minecraft/class_239$Type;",          // alternate inner name
        "()Lnet/minecraft/util/hit/HitResult$Type;", // deobf
        "()Lnet/minecraft/client/renderer/HitResult$Type;", // older package
        nullptr
    };

    for (int ni = 0; names[ni]; ni++) {
        for (int si = 0; sigs[si]; si++) {
            jmethodID mid = env->GetMethodID(g_hitResultClass_121, names[ni], sigs[si]);
            if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
            if (mid) { g_hitResultGetType_121 = mid; return mid; }
        }
    }
    return nullptr;
}

static bool IsHitResultBlock(JNIEnv* env, jobject hitObj) {
    if (!env || !hitObj) return false;

    // Fast path removed: In 1.21, looking at air still returns a BlockHitResult but with type MISS.

    // Robust path: HitResult.getType().name() == "BLOCK"
    jmethodID mGetType = ResolveHitResultGetType(env);
    if (!mGetType) return false;

    jobject typeObj = env->CallObjectMethod(hitObj, mGetType);
    if (env->ExceptionCheck()) { env->ExceptionClear(); typeObj = nullptr; }
    if (!typeObj) return false;

    jclass enumCls = env->GetObjectClass(typeObj);
    jmethodID mName = enumCls ? env->GetMethodID(enumCls, "name", "()Ljava/lang/String;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mName = nullptr; }
    bool out = false;
    if (enumCls && mName) {
        jstring jn = (jstring)env->CallObjectMethod(typeObj, mName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); jn = nullptr; }
        if (jn) {
            const char* cn = env->GetStringUTFChars(jn, nullptr);
            if (cn) {
                out = (std::string(cn) == "BLOCK");
                env->ReleaseStringUTFChars(jn, cn);
            }
            env->DeleteLocalRef(jn);
        }
    }
    if (enumCls) env->DeleteLocalRef(enumCls);
    env->DeleteLocalRef(typeObj);
    return out;
}

static void EnsureCrosshairTargetField(JNIEnv* env, jclass mcCls) {
    if (!env || !mcCls || g_crosshairTargetField_121) return;
    EnsureHitResultCaches(env);
    // Fast path for common Yarn field name.
    jfieldID fid = env->GetFieldID(mcCls, "field_1765", "Lnet/minecraft/class_239;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }

    // Some runtimes keep a readable name; the descriptor is still the obf HitResult type.
    if (!fid) {
        fid = env->GetFieldID(mcCls, "crosshairTarget", "Lnet/minecraft/class_239;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
    }

    // Reflection scan fallback: find any field of type HitResult.
    if (!fid && g_hitResultClass_121) {
        jclass cClass = env->FindClass("java/lang/Class");
        jclass cField = env->FindClass("java/lang/reflect/Field");
        jmethodID mGetFields = cClass ? env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
        jmethodID mFType = cField ? env->GetMethodID(cField, "getType", "()Ljava/lang/Class;") : nullptr;
        jmethodID mFName = cField ? env->GetMethodID(cField, "getName", "()Ljava/lang/String;") : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); mGetFields = nullptr; mFType = nullptr; mFName = nullptr; }

        if (cClass && cField && mGetFields && mFType && mFName) {
            jobjectArray fields = (jobjectArray)env->CallObjectMethod(mcCls, mGetFields);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fields = nullptr; }
            if (fields) {
                jsize fc = env->GetArrayLength(fields);
                for (int i = 0; i < fc; i++) {
                    jobject rf = env->GetObjectArrayElement(fields, i);
                    if (!rf) continue;
                    jclass ft = (jclass)env->CallObjectMethod(rf, mFType);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); ft = nullptr; }
                    bool typeMatch = (ft && env->IsSameObject(ft, g_hitResultClass_121) == JNI_TRUE);
                    if (ft) env->DeleteLocalRef(ft);
                    if (!typeMatch) { env->DeleteLocalRef(rf); continue; }

                    jstring jfn = (jstring)env->CallObjectMethod(rf, mFName);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); jfn = nullptr; }
                    if (!jfn) { env->DeleteLocalRef(rf); continue; }
                    const char* cfn = env->GetStringUTFChars(jfn, nullptr);
                    std::string fn = cfn ? cfn : "";
                    if (cfn) env->ReleaseStringUTFChars(jfn, cfn);
                    env->DeleteLocalRef(jfn);

                    jfieldID tryFid = env->GetFieldID(mcCls, fn.c_str(), "Lnet/minecraft/class_239;");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); tryFid = nullptr; }
                    env->DeleteLocalRef(rf);
                    if (tryFid) { fid = tryFid; Log("Discovered crosshairTarget field: " + fn); break; }
                }
                env->DeleteLocalRef(fields);
            }
        } else {
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (cClass) env->DeleteLocalRef(cClass);
        if (cField) env->DeleteLocalRef(cField);
    }

    if (fid) g_crosshairTargetField_121 = fid;
}


static bool IsJavaList(JNIEnv* env, jobject obj) {
    if (!obj) return false;
    jclass cList = env->FindClass("java/util/List");
    if (!cList || env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    bool ok = env->IsInstanceOf(obj, cList) == JNI_TRUE;
    env->DeleteLocalRef(cList);
    return ok;
}

static jmethodID FindZeroArgMethodReturning(JNIEnv* env, jclass owner, const std::string& returnTypeName, const char* preferredName1, const char* preferredName2, const char* sig) {
    if (!owner) return nullptr;

    if (preferredName1) {
        jmethodID mid = env->GetMethodID(owner, preferredName1, sig);
        if (!env->ExceptionCheck() && mid) return mid;
        env->ExceptionClear();
    }
    if (preferredName2) {
        jmethodID mid = env->GetMethodID(owner, preferredName2, sig);
        if (!env->ExceptionCheck() && mid) return mid;
        env->ExceptionClear();
    }

    // Reflection scan fallback: find a 0-arg method with the expected return type.
    jclass cClass = env->FindClass("java/lang/Class");
    jmethodID mGetMethods = cClass ? env->GetMethodID(cClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mGetMethods = nullptr; }

    jclass cMethod = env->FindClass("java/lang/reflect/Method");
    jmethodID mParams = cMethod ? env->GetMethodID(cMethod, "getParameterTypes", "()[Ljava/lang/Class;") : nullptr;
    jmethodID mRet    = cMethod ? env->GetMethodID(cMethod, "getReturnType", "()Ljava/lang/Class;") : nullptr;
    jmethodID mName   = cMethod ? env->GetMethodID(cMethod, "getName", "()Ljava/lang/String;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mParams = nullptr; mRet = nullptr; mName = nullptr; }

    if (!cClass || !cMethod || !mGetMethods || !mParams || !mRet || !mName) {
        env->ExceptionClear();
        if (cClass) env->DeleteLocalRef(cClass);
        if (cMethod) env->DeleteLocalRef(cMethod);
        return nullptr;
    }

    jobjectArray methods = (jobjectArray)env->CallObjectMethod(owner, mGetMethods);
    if (env->ExceptionCheck()) { env->ExceptionClear(); methods = nullptr; }
    if (!methods) {
        env->DeleteLocalRef(cClass);
        env->DeleteLocalRef(cMethod);
        return nullptr;
    }

    jsize mc = env->GetArrayLength(methods);
    for (int i = 0; i < mc; i++) {
        jobject m = env->GetObjectArrayElement(methods, i);
        if (!m) continue;
        jobjectArray params = (jobjectArray)env->CallObjectMethod(m, mParams);
        if (env->ExceptionCheck()) { env->ExceptionClear(); params = nullptr; }
        if (!params) { env->DeleteLocalRef(m); continue; }
        if (env->GetArrayLength(params) != 0) { env->DeleteLocalRef(params); env->DeleteLocalRef(m); continue; }
        env->DeleteLocalRef(params);

        jclass rt = (jclass)env->CallObjectMethod(m, mRet);
        if (env->ExceptionCheck()) { env->ExceptionClear(); rt = nullptr; }
        if (!rt) { env->DeleteLocalRef(m); continue; }
        std::string rtn = GetClassNameFromClass(env, rt);
        env->DeleteLocalRef(rt);
        if (rtn != returnTypeName) { env->DeleteLocalRef(m); continue; }

        jstring jmn = (jstring)env->CallObjectMethod(m, mName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); jmn = nullptr; }
        if (!jmn) { env->DeleteLocalRef(m); continue; }
        const char* cmn = env->GetStringUTFChars(jmn, nullptr);
        std::string name = cmn ? cmn : "";
        if (cmn) env->ReleaseStringUTFChars(jmn, cmn);
        env->DeleteLocalRef(jmn);

        jmethodID mid = env->GetMethodID(owner, name.c_str(), sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
        env->DeleteLocalRef(m);
        if (mid) {
            env->DeleteLocalRef(methods);
            env->DeleteLocalRef(cClass);
            env->DeleteLocalRef(cMethod);
            return mid;
        }
    }

    env->DeleteLocalRef(methods);
    env->DeleteLocalRef(cClass);
    env->DeleteLocalRef(cMethod);
    return nullptr;
}

static jmethodID FindZeroArgMethodReturningClass(JNIEnv* env, jclass owner, jclass expectedRetClass, const char* preferredName1, const char* preferredName2, const char* sig) {
    if (!owner || !expectedRetClass) return nullptr;

    if (preferredName1) {
        jmethodID mid = env->GetMethodID(owner, preferredName1, sig);
        if (!env->ExceptionCheck() && mid) return mid;
        env->ExceptionClear();
    }
    if (preferredName2) {
        jmethodID mid = env->GetMethodID(owner, preferredName2, sig);
        if (!env->ExceptionCheck() && mid) return mid;
        env->ExceptionClear();
    }

    // Reflection scan fallback: find a 0-arg method with the exact expected return type class.
    jclass cClass = env->FindClass("java/lang/Class");
    jmethodID mGetMethods = cClass ? env->GetMethodID(cClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mGetMethods = nullptr; }

    jclass cMethod = env->FindClass("java/lang/reflect/Method");
    jmethodID mParams = cMethod ? env->GetMethodID(cMethod, "getParameterTypes", "()[Ljava/lang/Class;") : nullptr;
    jmethodID mRet    = cMethod ? env->GetMethodID(cMethod, "getReturnType", "()Ljava/lang/Class;") : nullptr;
    jmethodID mName   = cMethod ? env->GetMethodID(cMethod, "getName", "()Ljava/lang/String;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mParams = nullptr; mRet = nullptr; mName = nullptr; }

    if (!cClass || !cMethod || !mGetMethods || !mParams || !mRet || !mName) {
        env->ExceptionClear();
        if (cClass) env->DeleteLocalRef(cClass);
        if (cMethod) env->DeleteLocalRef(cMethod);
        return nullptr;
    }

    jobjectArray methods = (jobjectArray)env->CallObjectMethod(owner, mGetMethods);
    if (env->ExceptionCheck()) { env->ExceptionClear(); methods = nullptr; }
    if (!methods) {
        env->DeleteLocalRef(cClass);
        env->DeleteLocalRef(cMethod);
        return nullptr;
    }

    jsize mc = env->GetArrayLength(methods);
    for (int i = 0; i < mc; i++) {
        jobject m = env->GetObjectArrayElement(methods, i);
        if (!m) continue;
        jobjectArray params = (jobjectArray)env->CallObjectMethod(m, mParams);
        if (env->ExceptionCheck()) { env->ExceptionClear(); params = nullptr; }
        if (!params) { env->DeleteLocalRef(m); continue; }
        if (env->GetArrayLength(params) != 0) { env->DeleteLocalRef(params); env->DeleteLocalRef(m); continue; }
        env->DeleteLocalRef(params);

        jclass rt = (jclass)env->CallObjectMethod(m, mRet);
        if (env->ExceptionCheck()) { env->ExceptionClear(); rt = nullptr; }
        if (!rt) { env->DeleteLocalRef(m); continue; }
        
        bool match = env->IsSameObject(rt, expectedRetClass);
        env->DeleteLocalRef(rt);
        if (!match) { env->DeleteLocalRef(m); continue; }

        jstring jmn = (jstring)env->CallObjectMethod(m, mName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); jmn = nullptr; }
        if (!jmn) { env->DeleteLocalRef(m); continue; }
        const char* cmn = env->GetStringUTFChars(jmn, nullptr);
        std::string name = cmn ? cmn : "";
        if (cmn) env->ReleaseStringUTFChars(jmn, cmn);
        env->DeleteLocalRef(jmn);

        // We found a method returning exactly the right class!
        // We still need its signature, since the return type might be obfuscated. 
        // Calling GetMethodID with the original sig might fail.
        // Wait, if we found it through reflection, we can just get the method ID by calling env->FromReflectedMethod(m)!
        jmethodID mid = env->FromReflectedMethod(m);
        env->DeleteLocalRef(m);
        if (mid) {
            env->DeleteLocalRef(methods);
            env->DeleteLocalRef(cClass);
            env->DeleteLocalRef(cMethod);
            return mid;
        }
    }

    env->DeleteLocalRef(methods);
    env->DeleteLocalRef(cClass);
    env->DeleteLocalRef(cMethod);
    return nullptr;
}

static jclass g_chestBlockEntityClass_121 = nullptr; // net.minecraft.class_2595 (ChestBlockEntity)

static bool IsChestBlockEntity(JNIEnv* env, jobject be) {
    if (!env || !be) return false;
    
    // Cache the ChestBlockEntity class (class_2595 per Yarn 1.21 mappings)
    if (!g_chestBlockEntityClass_121) {
        jclass c = nullptr;
        if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2595");
        if (!c) {
            c = env->FindClass("net/minecraft/class_2595");
            if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
        }
        if (c) { g_chestBlockEntityClass_121 = (jclass)env->NewGlobalRef(c); env->DeleteLocalRef(c); }
        
        if (!g_chestBlockEntityClass_121) {
            static bool loggedC = false;
            if (!loggedC) { Log("IsChestBlockEntity: failed to find class_2595 (ChestBlockEntity)!"); loggedC = true; }
            return false;
        }
    }
    // Also try EnderChest and BarrelBlockEntity via separate class (class_2600 Barrel in 1.20.x, class_1993 Ender)
    // but for simplicity, only check class_2595 (regular chest)
    if (!g_chestBlockEntityClass_121) return false;
    
    return (env->IsInstanceOf(be, g_chestBlockEntityClass_121) == JNI_TRUE);
}

static void EnsureClosestPlayerCaches(JNIEnv* env);
static void EnsureEntityMethods(JNIEnv* env, jobject entObj);

static void UpdatePlayerListOverlay(JNIEnv* env) {
    DWORD now = GetTickCount();
    if (now - g_lastPlayerListUpdateMs < 100) return;
    g_lastPlayerListUpdateMs = now;

    Config cfg;
    { LockGuard lk(g_configMutex); cfg = g_config; }
    int maxPlayersToProcess = 1;
    if (cfg.nametags) maxPlayersToProcess = (std::max)(maxPlayersToProcess, (std::max)(1, (std::min)(20, cfg.nametagMaxCount)));
    if (cfg.aimAssist) maxPlayersToProcess = (std::max)(maxPlayersToProcess, 12);

    // Accumulate in localList; atomically publish to g_playerList at scope exit.
    std::vector<PlayerData121> localList;
    struct PublishOnExit {
        std::vector<PlayerData121>& local;
        ~PublishOnExit() { LockGuard lk(g_playerListMutex); g_playerList.swap(local); }
    } pub{localList};

    EnsureClosestPlayerCaches(env);
    if (!g_mcInstance || !g_worldField_121 || !g_playerField_121 || !g_worldPlayersListField_121) return;

    jobject worldObj = env->GetObjectField(g_mcInstance, g_worldField_121);
    jobject selfObj = env->GetObjectField(g_mcInstance, g_playerField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); worldObj = nullptr; selfObj = nullptr; }
    if (!worldObj || !selfObj) { if (worldObj) env->DeleteLocalRef(worldObj); if (selfObj) env->DeleteLocalRef(selfObj); return; }

    EnsureEntityMethods(env, selfObj);

    jobject listObj = env->GetObjectField(worldObj, g_worldPlayersListField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); listObj = nullptr; }
    if (!listObj) { env->DeleteLocalRef(worldObj); env->DeleteLocalRef(selfObj); return; }

    // Use Collection.toArray() — one CallObjectMethod instead of N get(i) calls
    jclass colCls2 = env->FindClass("java/util/Collection");
    jmethodID mToArr = colCls2 ? env->GetMethodID(colCls2, "toArray", "()[Ljava/lang/Object;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mToArr = nullptr; }
    if (colCls2) env->DeleteLocalRef(colCls2);
    if (!mToArr) { env->DeleteLocalRef(listObj); env->DeleteLocalRef(worldObj); env->DeleteLocalRef(selfObj); return; }

    jobjectArray entArr = (jobjectArray)env->CallObjectMethod(listObj, mToArr);
    if (env->ExceptionCheck()) { env->ExceptionClear(); entArr = nullptr; }
    if (!entArr) { env->DeleteLocalRef(listObj); env->DeleteLocalRef(worldObj); env->DeleteLocalRef(selfObj); return; }
    int size = (int)env->GetArrayLength(entArr);

    // Get self position — use bgCamState for XZ, or fallback to CallDoubleMethod
    double sx = 0, sy = 0, sz = 0;
    { LockGuard lk(g_bgCamMutex); sx = g_bgCamState.camX; sy = g_bgCamState.camY; sz = g_bgCamState.camZ; }
    if (sx == 0 && sy == 0 && sz == 0) {
        sx = CallDoubleNoArgs(env, selfObj, g_getX_121);
        sy = CallDoubleNoArgs(env, selfObj, g_getY_121);
        sz = CallDoubleNoArgs(env, selfObj, g_getZ_121);
    }

    // 1. Fetch lightweight info (XYZ only) — use Entity.pos field if available
    struct LightweightEntity {
        jobject obj;
        double dist;
        double x, y, z;
    };
    std::vector<LightweightEntity> lwList;

    for (int i = 0; i < size && i < 128; i++) {
        jobject entObj = env->GetObjectArrayElement(entArr, i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); entObj = nullptr; }
        if (!entObj) continue;

        // Skip self
        if (env->IsSameObject(entObj, selfObj)) { env->DeleteLocalRef(entObj); continue; }

        EnsureEntityMethods(env, entObj);
        double ex = 0, ey = 0, ez = 0;
        // Prefer direct Entity.pos field (zero CallDoubleMethod)
        if (g_entityPosField_121 && g_vec3dX_121 && g_vec3dY_121 && g_vec3dZ_121) {
            jobject posVec = env->GetObjectField(entObj, g_entityPosField_121);
            if (env->ExceptionCheck()) { env->ExceptionClear(); posVec = nullptr; }
            if (posVec) {
                ex = env->GetDoubleField(posVec, g_vec3dX_121);
                ey = env->GetDoubleField(posVec, g_vec3dY_121);
                ez = env->GetDoubleField(posVec, g_vec3dZ_121);
                env->ExceptionClear();
                env->DeleteLocalRef(posVec);
            } else {
                ex = CallDoubleNoArgs(env, entObj, g_getX_121);
                ey = CallDoubleNoArgs(env, entObj, g_getY_121);
                ez = CallDoubleNoArgs(env, entObj, g_getZ_121);
            }
        } else {
            ex = CallDoubleNoArgs(env, entObj, g_getX_121);
            ey = CallDoubleNoArgs(env, entObj, g_getY_121);
            ez = CallDoubleNoArgs(env, entObj, g_getZ_121);
        }
        double dx = ex - sx, dy = ey - sy, dz = ez - sz;
        double dist = sqrt(dx*dx + dy*dy + dz*dz);

        lwList.push_back({entObj, dist, ex, ey, ez});
    }
    env->DeleteLocalRef(entArr);

    // Sort by distance
    std::sort(lwList.begin(), lwList.end(), [](const LightweightEntity& a, const LightweightEntity& b) {
        return a.dist < b.dist;
    });

    // 2. Process heavy JNI only on nearest N (config-driven)
    int processedCount = 0;
    for (auto& lw : lwList) {
        if (processedCount < maxPlayersToProcess) {
            std::string name = GetStablePlayerName(env, lw.obj);
            if (name.empty()) name = "Player";

            double hp = 20.0;
            if (g_getHealth_121) {
                hp = env->CallFloatMethod(lw.obj, g_getHealth_121);
                if (env->ExceptionCheck()) { env->ExceptionClear(); hp = 20.0; }
            }
            
            int armor = GetEntityArmor(env, lw.obj);
            std::string held = GetHeldItemInfo(env, lw.obj);

            localList.emplace_back(PlayerData121{name, lw.dist, lw.x, lw.y, lw.z, hp, armor, held});
            processedCount++;
        }
        env->DeleteLocalRef(lw.obj);
    }

    // Clean up local references exactly once.
    if (listObj) env->DeleteLocalRef(listObj);
    if (worldObj) env->DeleteLocalRef(worldObj);
    if (selfObj) env->DeleteLocalRef(selfObj);
}

// --- Chunk-based block entity discovery helpers (replaces flat-list approach) ---

// Ensure the base BlockEntity class is cached.
static void EnsureBlockEntityClass(JNIEnv* env) {
    if (g_blockEntityClass_121) return;
    jclass be = nullptr;
    if (g_gameClassLoader) be = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2586");
    if (!be) {
        be = env->FindClass("net/minecraft/class_2586");
        if (env->ExceptionCheck()) { env->ExceptionClear(); be = nullptr; }
    }
    if (be) { g_blockEntityClass_121 = (jclass)env->NewGlobalRef(be); env->DeleteLocalRef(be); }
    if (!g_blockEntityClass_121) {
        static bool logged = false;
        if (!logged) { Log("EnsureBlockEntityClass: failed to find class_2586 (BlockEntity)!"); logged = true; }
    }
}

// Try known getChunk(int,int) signatures directly; only fall back to reflection if needed.
static bool EnsureChunkAccess(JNIEnv* env, jobject worldObj) {
    if (g_worldGetChunkMethod_121) return true;
    if (!worldObj) return false;

    jclass worldCls = env->GetObjectClass(worldObj);
    if (!worldCls) return false;

    // 1) Try the exact signature discovered in previous sessions.
    struct { const char* name; const char* sig; } directTries[] = {
        { "method_22338", "(II)Lnet/minecraft/class_1922;" },
        { "getChunk",     "(II)Lnet/minecraft/class_1922;" },
        { "method_22338", "(II)Lnet/minecraft/class_2818;" },
        { "getChunk",     "(II)Lnet/minecraft/class_2818;" },
    };
    for (auto& t : directTries) {
        jmethodID mid = env->GetMethodID(worldCls, t.name, t.sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (mid) {
            g_worldGetChunkMethod_121 = mid;
            Log(std::string("Found getChunk (direct): ") + t.name + " " + t.sig);
            env->DeleteLocalRef(worldCls);
            return true;
        }
    }

    // 2) Reflection fallback – walk superclass chain for any (int,int)->Object method.
    jclass cClass  = env->FindClass("java/lang/Class");
    jclass cMethod = env->FindClass("java/lang/reflect/Method");
    if (env->ExceptionCheck() || !cClass || !cMethod) {
        env->ExceptionClear();
        if (cClass ) env->DeleteLocalRef(cClass);
        if (cMethod) env->DeleteLocalRef(cMethod);
        env->DeleteLocalRef(worldCls);
        return false;
    }

    jmethodID mDeclMethods = env->GetMethodID(cClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jmethodID mGetSuper    = env->GetMethodID(cClass, "getSuperclass",      "()Ljava/lang/Class;");
    jmethodID mMName       = env->GetMethodID(cMethod,"getName",            "()Ljava/lang/String;");
    jmethodID mMParams     = env->GetMethodID(cMethod,"getParameterTypes",  "()[Ljava/lang/Class;");
    jmethodID mMRet        = env->GetMethodID(cMethod,"getReturnType",      "()Ljava/lang/Class;");
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (!mDeclMethods || !mGetSuper || !mMName || !mMParams || !mMRet) {
        env->DeleteLocalRef(cClass); env->DeleteLocalRef(cMethod); env->DeleteLocalRef(worldCls);
        static bool lg = false;
        if (!lg) { Log("EnsureChunkAccess: reflection method IDs unavailable"); lg = true; }
        return false;
    }

    std::vector<jclass> chain;
    jclass cur = (jclass)env->NewLocalRef(worldCls);
    while (cur) {
        chain.push_back(cur);
        jclass sup = (jclass)env->CallObjectMethod(cur, mGetSuper);
        if (env->ExceptionCheck()) { env->ExceptionClear(); sup = nullptr; }
        cur = sup;
    }

    bool found = false;
    for (jclass cls : chain) {
        if (found) break;
        jobjectArray methods = (jobjectArray)env->CallObjectMethod(cls, mDeclMethods);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!methods) continue;

        jsize mc = env->GetArrayLength(methods);
        for (int i = 0; i < mc && !found; i++) {
            jobject m = env->GetObjectArrayElement(methods, i);
            if (!m) continue;

            jobjectArray params = (jobjectArray)env->CallObjectMethod(m, mMParams);
            if (env->ExceptionCheck()) { env->ExceptionClear(); params = nullptr; }
            bool intInt = false;
            if (params && env->GetArrayLength(params) == 2) {
                jobject p0 = env->GetObjectArrayElement(params, 0);
                jobject p1 = env->GetObjectArrayElement(params, 1);
                std::string n0 = p0 ? GetClassNameFromClass(env, (jclass)p0) : "";
                std::string n1 = p1 ? GetClassNameFromClass(env, (jclass)p1) : "";
                intInt = (n0 == "int" && n1 == "int");
                if (p0) env->DeleteLocalRef(p0);
                if (p1) env->DeleteLocalRef(p1);
            }
            if (params) env->DeleteLocalRef(params);
            if (!intInt) { env->DeleteLocalRef(m); continue; }

            jstring jmn = (jstring)env->CallObjectMethod(m, mMName);
            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(m); continue; }
            const char* cmn = jmn ? env->GetStringUTFChars(jmn, nullptr) : nullptr;
            std::string mname = cmn ? cmn : "";
            if (cmn) env->ReleaseStringUTFChars(jmn, cmn);
            if (jmn) env->DeleteLocalRef(jmn);

            jobject retCls = env->CallObjectMethod(m, mMRet);
            if (env->ExceptionCheck()) { env->ExceptionClear(); retCls = nullptr; }
            std::string retName = retCls ? GetClassNameFromClass(env, (jclass)retCls) : "";
            if (retCls) env->DeleteLocalRef(retCls);
            env->DeleteLocalRef(m);

            if (retName.empty() || retName == "void" || retName == "int" ||
                retName == "long" || retName == "boolean" || retName == "float" || retName == "double")
                continue;

            std::string retDesc = retName;
            for (char& c : retDesc) if (c == '.') c = '/';
            std::string sig = "(II)L" + retDesc + ";";

            jmethodID mid = env->GetMethodID(cls, mname.c_str(), sig.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
            if (mid) {
                g_worldGetChunkMethod_121 = mid;
                Log("Found getChunk (reflection): " + mname + " " + sig);
                found = true;
            }
        }
        env->DeleteLocalRef(methods);
    }

    for (jclass cls : chain) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(cClass);
    env->DeleteLocalRef(cMethod);
    env->DeleteLocalRef(worldCls);

    if (!found) {
        static bool logged = false;
        if (!logged) { Log("EnsureChunkAccess: no getChunk(II) method found on world class!"); logged = true; }
    }
    return found;
}

// Find the blockEntities Map field on WorldChunk (class_2818) or its parent class Chunk (class_2791).
// Walks the full class hierarchy and VALIDATES each Map field by checking if its values
// are actually instanceof BlockEntity (class_2586).  This is necessary because class_2818
// has field_27222 (blockEntityTickers map) whose values are NOT BlockEntity instances —
// they are class_5564 wrappers around BlockEntityTickInvoker.  The real blockEntities map
// lives on the parent class class_2791 with a version-specific obfuscated name.
static bool EnsureChunkBEMap(JNIEnv* env, jobject chunkObj) {
    if (g_chunkBlockEntitiesMapField_121) return true;
    if (!chunkObj) return false;

    // ---------- Step 1: Load WorldChunk class (class_2818) explicitly ----------
    jclass worldChunkCls = nullptr;
    if (g_gameClassLoader)
        worldChunkCls = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2818");
    if (!worldChunkCls) {
        worldChunkCls = env->FindClass("net/minecraft/class_2818");
        if (env->ExceptionCheck()) { env->ExceptionClear(); worldChunkCls = nullptr; }
    }

    bool isWorldChunk = worldChunkCls && (env->IsInstanceOf(chunkObj, worldChunkCls) == JNI_TRUE);
    if (!isWorldChunk) {
        if (worldChunkCls) env->DeleteLocalRef(worldChunkCls);
        return false;
    }

    EnsureBlockEntityClass(env);

    // Fast path: try known field names directly before the expensive reflection+validation loop.
    // field_34543 on class_2791 (Chunk) was confirmed as the blockEntities map by debug log.
    // GetFieldID searches the class and its entire superclass chain, so using worldChunkCls works.
    {
        const char* knownNames[] = { "field_34543", "blockEntities", nullptr };
        for (int i = 0; knownNames[i]; i++) {
            jfieldID fid = env->GetFieldID(worldChunkCls, knownNames[i], "Ljava/util/Map;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
            if (fid) {
                g_chunkBlockEntitiesMapField_121 = fid;
                Log("EnsureChunkBEMap: fast-path found blockEntities map: " + std::string(knownNames[i]));
                env->DeleteLocalRef(worldChunkCls);
                return true;
            }
        }
    }

    // ---------- Step 2: Reflection helpers ----------
    jclass cClass = env->FindClass("java/lang/Class");
    jclass cField = env->FindClass("java/lang/reflect/Field");
    jmethodID mGetDeclFields = cClass ? env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
    jmethodID mGetSuper      = cClass ? env->GetMethodID(cClass, "getSuperclass", "()Ljava/lang/Class;") : nullptr;
    jmethodID mFName  = cField ? env->GetMethodID(cField, "getName", "()Ljava/lang/String;") : nullptr;
    jmethodID mFType  = cField ? env->GetMethodID(cField, "getType", "()Ljava/lang/Class;")  : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass mapInterface = env->FindClass("java/util/Map");
    jmethodID mSize = mapInterface ? env->GetMethodID(mapInterface, "size", "()I") : nullptr;
    jmethodID mVals = mapInterface ? env->GetMethodID(mapInterface, "values", "()Ljava/util/Collection;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass colCls = env->FindClass("java/util/Collection");
    jmethodID mIter = colCls ? env->GetMethodID(colCls, "iterator", "()Ljava/util/Iterator;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass itCls = env->FindClass("java/util/Iterator");
    jmethodID mHN  = itCls ? env->GetMethodID(itCls, "hasNext", "()Z") : nullptr;
    jmethodID mNxt = itCls ? env->GetMethodID(itCls, "next", "()Ljava/lang/Object;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    bool canReflect = mGetDeclFields && mGetSuper && mFName && mFType
                   && mSize && mVals && mIter && mHN && mNxt;

    // ---------- Step 3: Walk the class hierarchy and find validated Map field ----------
    if (canReflect) {
        jclass currentClass = (jclass)env->NewLocalRef(worldChunkCls);
        int depth = 0;
        while (currentClass && depth < 10) {
            std::string clsName = GetClassNameFromClass(env, currentClass);
            if (clsName == "java.lang.Object") { env->DeleteLocalRef(currentClass); break; }

            jobjectArray fields = (jobjectArray)env->CallObjectMethod(currentClass, mGetDeclFields);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fields = nullptr; }
            if (fields) {
                jsize fc = env->GetArrayLength(fields);
                Log("EnsureChunkBEMap scanning " + clsName + " (" + std::to_string(fc) + " fields)");

                for (int i = 0; i < fc; i++) {
                    jobject fld = env->GetObjectArrayElement(fields, i);
                    if (!fld) continue;

                    jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); ft = nullptr; }

                    bool isMap = ft && mapInterface && (env->IsAssignableFrom(ft, mapInterface) == JNI_TRUE);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); isMap = false; }

                    if (isMap) {
                        // Get field name
                        jstring jfn = (jstring)env->CallObjectMethod(fld, mFName);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); jfn = nullptr; }
                        std::string fn = "?";
                        if (jfn) {
                            const char* c = env->GetStringUTFChars(jfn, nullptr);
                            fn = c ? c : "?"; if (c) env->ReleaseStringUTFChars(jfn, c);
                            env->DeleteLocalRef(jfn);
                        }

                        // Get fieldID — try Ljava/util/Map; first, then actual type
                        jfieldID fid = env->GetFieldID(worldChunkCls, fn.c_str(), "Ljava/util/Map;");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
                        if (!fid) {
                            std::string tn = GetClassNameFromClass(env, ft);
                            std::string desc = "L" + tn + ";";
                            for (char& cc : desc) if (cc == '.') cc = '/';
                            fid = env->GetFieldID(worldChunkCls, fn.c_str(), desc.c_str());
                            if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
                        }

                        if (fid) {
                            jobject mapObj = env->GetObjectField(chunkObj, fid);
                            if (env->ExceptionCheck()) { env->ExceptionClear(); mapObj = nullptr; }
                            if (mapObj) {
                                int mapSz = env->CallIntMethod(mapObj, mSize);
                                if (env->ExceptionCheck()) { env->ExceptionClear(); mapSz = 0; }

                                Log("  Map candidate: " + fn + " on " + clsName + " size=" + std::to_string(mapSz));

                                if (mapSz > 0 && g_blockEntityClass_121) {
                                    // Validate: check if first value is instanceof BlockEntity
                                    jobject col = env->CallObjectMethod(mapObj, mVals);
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); col = nullptr; }
                                    if (col) {
                                        jobject it = env->CallObjectMethod(col, mIter);
                                        if (env->ExceptionCheck()) { env->ExceptionClear(); it = nullptr; }
                                        if (it) {
                                            jboolean hn = env->CallBooleanMethod(it, mHN);
                                            if (env->ExceptionCheck()) { env->ExceptionClear(); hn = false; }
                                            if (hn) {
                                                jobject val = env->CallObjectMethod(it, mNxt);
                                                if (env->ExceptionCheck()) { env->ExceptionClear(); val = nullptr; }
                                                if (val) {
                                                    bool isBE = (env->IsInstanceOf(val, g_blockEntityClass_121) == JNI_TRUE);
                                                    if (env->ExceptionCheck()) { env->ExceptionClear(); isBE = false; }
                                                    jclass vCls = env->GetObjectClass(val);
                                                    std::string vType = vCls ? GetClassNameFromClass(env, vCls) : "?";
                                                    if (vCls) env->DeleteLocalRef(vCls);
                                                    Log("    First value: " + vType + " isBE=" + (isBE ? "true" : "false"));

                                                    if (isBE) {
                                                        g_chunkBlockEntitiesMapField_121 = fid;
                                                        Log("VALIDATED blockEntities map: " + fn + " on " + clsName);
                                                        env->DeleteLocalRef(val);
                                                        env->DeleteLocalRef(it);
                                                        env->DeleteLocalRef(col);
                                                        env->DeleteLocalRef(mapObj);
                                                        if (ft) env->DeleteLocalRef(ft);
                                                        env->DeleteLocalRef(fld);
                                                        env->DeleteLocalRef(fields);
                                                        env->DeleteLocalRef(currentClass);
                                                        if (cClass) env->DeleteLocalRef(cClass);
                                                        if (cField) env->DeleteLocalRef(cField);
                                                        if (mapInterface) env->DeleteLocalRef(mapInterface);
                                                        if (colCls) env->DeleteLocalRef(colCls);
                                                        if (itCls) env->DeleteLocalRef(itCls);
                                                        env->DeleteLocalRef(worldChunkCls);
                                                        return true;
                                                    }
                                                    env->DeleteLocalRef(val);
                                                }
                                            }
                                            env->DeleteLocalRef(it);
                                        }
                                        env->DeleteLocalRef(col);
                                    }
                                }
                                env->DeleteLocalRef(mapObj);
                            }
                        }
                    }
                    if (ft) env->DeleteLocalRef(ft);
                    env->DeleteLocalRef(fld);
                }
                env->DeleteLocalRef(fields);
            }

            // Move to parent class
            jclass parent = (jclass)env->CallObjectMethod(currentClass, mGetSuper);
            if (env->ExceptionCheck()) { env->ExceptionClear(); parent = nullptr; }
            env->DeleteLocalRef(currentClass);
            currentClass = parent;
            depth++;
        }
    }

    // ---------- Cleanup ----------
    static bool loggedFail = false;
    if (!loggedFail) {
        loggedFail = true;
        Log("EnsureChunkBEMap: no Map with BlockEntity values found in hierarchy (chunk may be empty — will retry)");
    }
    if (cClass) env->DeleteLocalRef(cClass);
    if (cField) env->DeleteLocalRef(cField);
    if (mapInterface) env->DeleteLocalRef(mapInterface);
    if (colCls) env->DeleteLocalRef(colCls);
    if (itCls) env->DeleteLocalRef(itCls);
    env->DeleteLocalRef(worldChunkCls);
    return false;
}

static void EnsureBlockPosCache(JNIEnv* env, jobject beObj) {
    if (!env || !beObj) return;

    if (!g_beGetPos_121) {
        jclass beCls = env->GetObjectClass(beObj);
        if (beCls) {
            g_beGetPos_121 = env->GetMethodID(beCls, "getPos", "()Lnet/minecraft/class_2338;");
            if (env->ExceptionCheck() || !g_beGetPos_121) {
                env->ExceptionClear();
                g_beGetPos_121 = env->GetMethodID(beCls, "method_11014", "()Lnet/minecraft/class_2338;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_beGetPos_121 = nullptr; }
            }
            // Also try getCachedState approach as position source
            if (!g_beGetPos_121) {
                // Attempt: some versions rename this to pos() or getBlockPos()
                g_beGetPos_121 = env->GetMethodID(beCls, "getBlockPos", "()Lnet/minecraft/class_2338;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_beGetPos_121 = nullptr; }
            }
            env->DeleteLocalRef(beCls);
        }
    }

    if (!g_blockPosClass_121 && g_gameClassLoader) {
        jclass bp = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2338");
        if (!bp) { bp = env->FindClass("net/minecraft/class_2338"); if (env->ExceptionCheck()) { env->ExceptionClear(); bp = nullptr; } }
        if (bp) {
            g_blockPosClass_121 = (jclass)env->NewGlobalRef(bp);
            env->DeleteLocalRef(bp);
            // Lunar 1.21 runtime: BlockPos/Vec3i appears as x=field_11175, y=field_11174, z=field_11173
            g_blockPosX_121 = env->GetFieldID(g_blockPosClass_121, "field_11175", "I");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosX_121 = env->GetFieldID(g_blockPosClass_121, "x", "I"); if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosX_121 = nullptr; } }
            g_blockPosY_121 = env->GetFieldID(g_blockPosClass_121, "field_11174", "I");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosY_121 = env->GetFieldID(g_blockPosClass_121, "y", "I"); if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosY_121 = nullptr; } }
            g_blockPosZ_121 = env->GetFieldID(g_blockPosClass_121, "field_11173", "I");
            if (env->ExceptionCheck()) { env->ExceptionClear();
                g_blockPosZ_121 = env->GetFieldID(g_blockPosClass_121, "field_11172", "I");
                if (env->ExceptionCheck()) { env->ExceptionClear();
                    g_blockPosZ_121 = env->GetFieldID(g_blockPosClass_121, "z", "I");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosZ_121 = nullptr; }
                }
            }

            // If Z field failed, try parent class Vec3i (class_2382) and also try
            // enumerating all int fields to find the 3rd one
            if (!g_blockPosZ_121) {
                // Try Vec3i parent class directly
                jclass vec3iCls = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2382");
                if (!vec3iCls) { vec3iCls = env->FindClass("net/minecraft/class_2382"); if (env->ExceptionCheck()) { env->ExceptionClear(); vec3iCls = nullptr; } }
                if (vec3iCls) {
                    g_blockPosZ_121 = env->GetFieldID(vec3iCls, "field_11173", "I");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosZ_121 = nullptr; }
                    if (!g_blockPosZ_121) {
                        g_blockPosZ_121 = env->GetFieldID(vec3iCls, "field_11172", "I");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosZ_121 = nullptr; }
                    }
                    if (!g_blockPosZ_121) {
                        g_blockPosZ_121 = env->GetFieldID(vec3iCls, "z", "I");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosZ_121 = nullptr; }
                    }
                    // Also try re-resolving X/Y on Vec3i if they failed on BlockPos
                    if (!g_blockPosX_121) {
                        g_blockPosX_121 = env->GetFieldID(vec3iCls, "field_11175", "I");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosX_121 = nullptr; }
                    }
                    if (!g_blockPosY_121) {
                        g_blockPosY_121 = env->GetFieldID(vec3iCls, "field_11174", "I");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosY_121 = nullptr; }
                    }
                    env->DeleteLocalRef(vec3iCls);
                }
            }

            // Last resort: dynamically find all 3 int fields via getDeclaredFields
            // Vec3i has exactly 3 int fields (x, y, z) — find them by reflection
            if (!g_blockPosX_121 || !g_blockPosY_121 || !g_blockPosZ_121) {
                jclass vec3iCls2 = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2382");
                if (!vec3iCls2) { vec3iCls2 = env->FindClass("net/minecraft/class_2382"); if (env->ExceptionCheck()) { env->ExceptionClear(); vec3iCls2 = nullptr; } }
                if (vec3iCls2) {
                    jclass classCls = env->FindClass("java/lang/Class");
                    jmethodID getDeclaredFields = classCls ? env->GetMethodID(classCls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
                    if (env->ExceptionCheck()) { env->ExceptionClear(); getDeclaredFields = nullptr; }
                    jclass fieldCls = env->FindClass("java/lang/reflect/Field");
                    jmethodID getType = fieldCls ? env->GetMethodID(fieldCls, "getType", "()Ljava/lang/Class;") : nullptr;
                    jmethodID getName = fieldCls ? env->GetMethodID(fieldCls, "getName", "()Ljava/lang/String;") : nullptr;
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (getDeclaredFields && getType && getName) {
                        jobjectArray fields = (jobjectArray)env->CallObjectMethod(vec3iCls2, getDeclaredFields);
                        if (!env->ExceptionCheck() && fields) {
                            jclass intTypeCls = env->FindClass("java/lang/Integer");
                            jfieldID intTypeField = intTypeCls ? env->GetStaticFieldID(intTypeCls, "TYPE", "Ljava/lang/Class;") : nullptr;
                            jobject intPrimCls = intTypeField ? env->GetStaticObjectField(intTypeCls, intTypeField) : nullptr;
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            
                            jsize nFields = env->GetArrayLength(fields);
                            std::vector<std::string> intFieldNames;
                            for (jsize fi = 0; fi < nFields && intFieldNames.size() < 3; fi++) {
                                jobject f = env->GetObjectArrayElement(fields, fi);
                                if (!f) continue;
                                jobject fType = env->CallObjectMethod(f, getType);
                                if (fType && intPrimCls && env->IsSameObject(fType, intPrimCls)) {
                                    jstring fname = (jstring)env->CallObjectMethod(f, getName);
                                    if (fname) {
                                        const char* cstr = env->GetStringUTFChars(fname, nullptr);
                                        if (cstr) { intFieldNames.push_back(cstr); env->ReleaseStringUTFChars(fname, cstr); }
                                        env->DeleteLocalRef(fname);
                                    }
                                }
                                if (fType) env->DeleteLocalRef(fType);
                                env->DeleteLocalRef(f);
                            }
                            if (intFieldNames.size() >= 3) {
                                Log("Vec3i int fields: " + intFieldNames[0] + ", " + intFieldNames[1] + ", " + intFieldNames[2]);
                                if (!g_blockPosX_121) {
                                    g_blockPosX_121 = env->GetFieldID(vec3iCls2, intFieldNames[0].c_str(), "I");
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosX_121 = nullptr; }
                                }
                                if (!g_blockPosY_121) {
                                    g_blockPosY_121 = env->GetFieldID(vec3iCls2, intFieldNames[1].c_str(), "I");
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosY_121 = nullptr; }
                                }
                                if (!g_blockPosZ_121) {
                                    g_blockPosZ_121 = env->GetFieldID(vec3iCls2, intFieldNames[2].c_str(), "I");
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosZ_121 = nullptr; }
                                }
                            } else {
                                Log("Vec3i: found only " + std::to_string(intFieldNames.size()) + " int fields");
                            }
                            if (intPrimCls) env->DeleteLocalRef(intPrimCls);
                            if (intTypeCls) env->DeleteLocalRef(intTypeCls);
                            env->DeleteLocalRef(fields);
                        } else env->ExceptionClear();
                    }
                    if (classCls) env->DeleteLocalRef(classCls);
                    if (fieldCls) env->DeleteLocalRef(fieldCls);
                    env->DeleteLocalRef(vec3iCls2);
                }
            }
        }
    }
    // Try to find BlockEntity.pos field directly (avoids getPos() CallObjectMethod per chest)
    if (!g_beBlockPosField_121 && g_blockPosClass_121 && beObj) {
        jclass beCls2 = env->GetObjectClass(beObj);
        if (beCls2) {
            std::string sig = "Lnet/minecraft/class_2338;";
            g_beBlockPosField_121 = env->GetFieldID(beCls2, "field_11177", sig.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear();
                g_beBlockPosField_121 = env->GetFieldID(beCls2, "pos", sig.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_beBlockPosField_121 = nullptr; }
            }
            env->DeleteLocalRef(beCls2);
        }
    }

    // BlockPos coordinate methods — fallback when direct fields fail (returns primitive int,
    // no classloader descriptor resolution issues)
    if (!g_blockPosGetX_121 && g_blockPosClass_121) {
        const char* xNames[] = { "getX", "method_4900", nullptr };
        for (int i = 0; xNames[i] && !g_blockPosGetX_121; i++) {
            g_blockPosGetX_121 = env->GetMethodID(g_blockPosClass_121, xNames[i], "()I");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosGetX_121 = nullptr; }
        }
    }
    if (!g_blockPosGetY_121 && g_blockPosClass_121) {
        const char* yNames[] = { "getY", "method_4898", nullptr };
        for (int i = 0; yNames[i] && !g_blockPosGetY_121; i++) {
            g_blockPosGetY_121 = env->GetMethodID(g_blockPosClass_121, yNames[i], "()I");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosGetY_121 = nullptr; }
        }
    }
    if (!g_blockPosGetZ_121 && g_blockPosClass_121) {
        const char* zNames[] = { "getZ", "method_4896", nullptr };
        for (int i = 0; zNames[i] && !g_blockPosGetZ_121; i++) {
            g_blockPosGetZ_121 = env->GetMethodID(g_blockPosClass_121, zNames[i], "()I");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosGetZ_121 = nullptr; }
        }
    }

    static bool s_bpDiag = false;
    if (!s_bpDiag) {
        s_bpDiag = true;
        Log(std::string("BlockPosCache: beGetPos=") + (g_beGetPos_121 ? "1" : "0")
            + " beField=" + (g_beBlockPosField_121 ? "1" : "0")
            + " bpClass=" + (g_blockPosClass_121 ? "1" : "0")
            + " bpX=" + (g_blockPosX_121 ? "1" : "0")
            + " bpY=" + (g_blockPosY_121 ? "1" : "0")
            + " bpZ=" + (g_blockPosZ_121 ? "1" : "0")
            + " getX=" + (g_blockPosGetX_121 ? "1" : "0")
            + " getY=" + (g_blockPosGetY_121 ? "1" : "0")
            + " getZ=" + (g_blockPosGetZ_121 ? "1" : "0"));
    }
}

static void EnsureHashMapDirectFields(JNIEnv* env) {
    if (g_javaHashMapTableField) return;
    jclass hmCls = env->FindClass("java/util/HashMap");
    if (!hmCls) { env->ExceptionClear(); return; }
    g_javaHashMapTableField = env->GetFieldID(hmCls, "table", "[Ljava/util/HashMap$Node;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_javaHashMapTableField = nullptr; }
    // Keep a global ref so we can IsInstanceOf-check every map before direct access
    if (!g_javaHashMapClass)
        g_javaHashMapClass = (jclass)env->NewGlobalRef(hmCls);
    env->DeleteLocalRef(hmCls);
    jclass nodeCls = env->FindClass("java/util/HashMap$Node");
    if (!nodeCls) { env->ExceptionClear(); return; }
    g_javaHMNodeValueField = env->GetFieldID(nodeCls, "value", "Ljava/lang/Object;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_javaHMNodeValueField = nullptr; }
    g_javaHMNodeKeyField   = env->GetFieldID(nodeCls, "key",   "Ljava/lang/Object;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_javaHMNodeKeyField = nullptr; }
    g_javaHMNodeNextField  = env->GetFieldID(nodeCls, "next",  "Ljava/util/HashMap$Node;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_javaHMNodeNextField = nullptr; }
    env->DeleteLocalRef(nodeCls);
    if (g_javaHashMapTableField && g_javaHMNodeValueField && g_javaHMNodeNextField)
        Log(std::string("HashMap direct iteration fields ready. keyField=") + (g_javaHMNodeKeyField ? "1" : "0"));
}

// Read x,y,z from a BlockPos jobject. Returns true if all three coords obtained.
// Priority: direct field reads (fast) → method calls (safe on bg thread, handles Lunar remapping)
static bool ReadBlockPosCoords(JNIEnv* env, jobject bp, double& outX, double& outY, double& outZ) {
    if (!bp) return false;
    bool hasX = false, hasY = false, hasZ = false;
    if (g_blockPosX_121) { outX = env->GetIntField(bp, g_blockPosX_121); if (env->ExceptionCheck()) env->ExceptionClear(); else hasX = true; }
    if (g_blockPosY_121) { outY = env->GetIntField(bp, g_blockPosY_121); if (env->ExceptionCheck()) env->ExceptionClear(); else hasY = true; }
    if (g_blockPosZ_121) { outZ = env->GetIntField(bp, g_blockPosZ_121); if (env->ExceptionCheck()) env->ExceptionClear(); else hasZ = true; }
    if (!hasX && g_blockPosGetX_121) { outX = env->CallIntMethod(bp, g_blockPosGetX_121); if (env->ExceptionCheck()) { env->ExceptionClear(); } else hasX = true; }
    if (!hasY && g_blockPosGetY_121) { outY = env->CallIntMethod(bp, g_blockPosGetY_121); if (env->ExceptionCheck()) { env->ExceptionClear(); } else hasY = true; }
    if (!hasZ && g_blockPosGetZ_121) { outZ = env->CallIntMethod(bp, g_blockPosGetZ_121); if (env->ExceptionCheck()) { env->ExceptionClear(); } else hasZ = true; }
    return hasX && hasY && hasZ;
}

static void UpdateChestList(JNIEnv* env) {
    DWORD now = GetTickCount();
    if (now - g_lastChestScanMs < 100) return; // 0.1s throttle for responsive nearest-chest updates
    // Skip during world transitions (joining server / loading terrain) to avoid racing
    // with the render thread tearing down the old world's chunk data.
    if (now < g_worldTransitionEndMs) return;
    g_lastChestScanMs = now;
    std::vector<ChestData121> localList;

    if (!g_mcInstance || !g_worldField_121 || !g_playerField_121) return;
    jobject worldObj = env->GetObjectField(g_mcInstance, g_worldField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); worldObj = nullptr; }
    if (!worldObj) return;

    // Player position — use bg cam state (already read this iteration; no extra JNI)
    double sx = 0, sy = 0, sz = 0;
    { LockGuard lk(g_bgCamMutex); sx = g_bgCamState.camX; sy = g_bgCamState.camY; sz = g_bgCamState.camZ; }

    EnsureBlockEntityClass(env);
    if (!EnsureChunkAccess(env, worldObj)) { env->DeleteLocalRef(worldObj); return; }

    // Ensure direct HashMap field access (eliminates Call*Method for BE iteration)
    EnsureHashMapDirectFields(env);

    int pcx = (int)std::floor(sx) >> 4;
    int pcz = (int)std::floor(sz) >> 4;
    const int RANGE = 4; // ±4 = 9×9 = 81 chunks (vs previous 17×17 = 289)

    static DWORD lastLogMs = 0;
    bool shouldLog = (now - lastLogMs > 5000);
    static bool loggedMapDiag = false;
    int totalChests = 0;
    int totalBEsScanned = 0;
    bool sawHashMap = false;
    bool usedDirectPath = false;
    bool usedFallbackPath = false;

    for (int dx = -RANGE; dx <= RANGE; dx++) {
        for (int dz = -RANGE; dz <= RANGE; dz++) {
            // Re-check transition guard inside the loop too
            if (GetTickCount() < g_worldTransitionEndMs) break;
            jobject chunkObj = env->CallObjectMethod(worldObj, g_worldGetChunkMethod_121, pcx + dx, pcz + dz);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!chunkObj) continue;

            if (!g_chunkBlockEntitiesMapField_121)
                EnsureChunkBEMap(env, chunkObj);

            if (g_chunkBlockEntitiesMapField_121) {
                jobject mapObj = env->GetObjectField(chunkObj, g_chunkBlockEntitiesMapField_121);
                if (env->ExceptionCheck()) { env->ExceptionClear(); mapObj = nullptr; }
                if (mapObj) {
                    if (!loggedMapDiag) {
                        loggedMapDiag = true;
                        Log("ChestESP: blockEntities map found on chunk.");
                    }
                    // Iterate via HashMap.table[] — zero Call*Method
                    // SAFETY: only use direct field access if mapObj is actually a HashMap
                    // (Lunar might use a custom Map impl; wrong field offset = ACCESS_VIOLATION)
                    bool iterated = false;
                    bool isHashMap = (g_javaHashMapClass && 
                                      env->IsInstanceOf(mapObj, g_javaHashMapClass) == JNI_TRUE);
                    if (isHashMap) sawHashMap = true;
                    if (isHashMap && g_javaHashMapTableField && g_javaHMNodeValueField && g_javaHMNodeNextField) {
                        jobjectArray tbl = (jobjectArray)env->GetObjectField(mapObj, g_javaHashMapTableField);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); tbl = nullptr; }
                        if (tbl) {
                            jsize tlen = env->GetArrayLength(tbl);
                            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(tbl); tbl = nullptr; }
                            if (tbl) {
                            iterated = true;
                            usedDirectPath = true;
                            for (jsize ti = 0; ti < tlen; ti++) {
                                jobject node = env->GetObjectArrayElement(tbl, ti);
                                if (env->ExceptionCheck()) { env->ExceptionClear(); node = nullptr; }
                                while (node) {
                                    jobject be = env->GetObjectField(node, g_javaHMNodeValueField);
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); be = nullptr; }
                                    if (be) {
                                        totalBEsScanned++;
                                        if (IsChestBlockEntity(env, be)) {
                                            totalChests++;
                                            EnsureBlockPosCache(env, be);
                                            double cx2 = 0.5, cy2 = 0.0, cz2 = 0.5;
                                            bool gotPos = false;
                                            int posPath = 0;
                                            // Path 1: read BlockPos directly from the map KEY (no getPos() call)
                                            if (!gotPos && g_javaHMNodeKeyField) {
                                                jobject keyBp = env->GetObjectField(node, g_javaHMNodeKeyField);
                                                if (env->ExceptionCheck()) { env->ExceptionClear(); keyBp = nullptr; }
                                                if (keyBp) {
                                                    // Verify key is actually a BlockPos before reading fields
                                                    bool isBP = g_blockPosClass_121 && env->IsInstanceOf(keyBp, g_blockPosClass_121);
                                                    if (isBP) {
                                                        gotPos = ReadBlockPosCoords(env, keyBp, cx2, cy2, cz2);
                                                        if (gotPos) { cx2 += 0.5; cz2 += 0.5; posPath = 1; }
                                                    }
                                                    static bool s_keyDiag = false;
                                                    if (!s_keyDiag) {
                                                        s_keyDiag = true;
                                                        jclass keyCls = env->GetObjectClass(keyBp);
                                                        std::string keyClsName = keyCls ? GetClassNameFromClass(env, keyCls) : "null";
                                                        if (keyCls) env->DeleteLocalRef(keyCls);
                                                        Log("ChestKey diag: isBP=" + std::to_string(isBP) + " keyClass=" + keyClsName
                                                            + " gotPos=" + std::to_string(gotPos)
                                                            + " cx=" + std::to_string(cx2) + " cy=" + std::to_string(cy2) + " cz=" + std::to_string(cz2));
                                                    }
                                                    env->DeleteLocalRef(keyBp);
                                                }
                                            }
                                            // Path 2: direct field on BlockEntity
                                            if (!gotPos && g_beBlockPosField_121) {
                                                jobject bp = env->GetObjectField(be, g_beBlockPosField_121);
                                                if (env->ExceptionCheck()) { env->ExceptionClear(); bp = nullptr; }
                                                if (bp) {
                                                    gotPos = ReadBlockPosCoords(env, bp, cx2, cy2, cz2);
                                                    if (gotPos) { cx2 += 0.5; cz2 += 0.5; posPath = 2; }
                                                    env->DeleteLocalRef(bp);
                                                }
                                            }
                                            // Path 3: call getPos() on BlockEntity
                                            if (!gotPos && g_beGetPos_121) {
                                                jobject bp = env->CallObjectMethod(be, g_beGetPos_121);
                                                if (env->ExceptionCheck()) { env->ExceptionClear(); bp = nullptr; }
                                                if (bp) {
                                                    gotPos = ReadBlockPosCoords(env, bp, cx2, cy2, cz2);
                                                    if (gotPos) { cx2 += 0.5; cz2 += 0.5; posPath = 3; }
                                                    env->DeleteLocalRef(bp);
                                                }
                                            }
                                            static bool s_posDiag = false;
                                            if (!s_posDiag) {
                                                s_posDiag = true;
                                                Log("ChestPos diag: gotPos=" + std::to_string(gotPos) + " path=" + std::to_string(posPath)
                                                    + " pos=(" + std::to_string(cx2) + "," + std::to_string(cy2) + "," + std::to_string(cz2) + ")"
                                                    + " beGetPos=" + std::to_string(g_beGetPos_121 ? 1 : 0)
                                                    + " beField=" + std::to_string(g_beBlockPosField_121 ? 1 : 0)
                                                    + " keyField=" + std::to_string(g_javaHMNodeKeyField ? 1 : 0));
                                            }
                                            if (gotPos) {
                                                double ddx = cx2-sx, ddy = cy2-sy, ddz = cz2-sz;
                                                localList.push_back({cx2, cy2, cz2, std::sqrt(ddx*ddx+ddy*ddy+ddz*ddz)});
                                            }
                                        }
                                        env->DeleteLocalRef(be);
                                    }
                                    jobject nxt = env->GetObjectField(node, g_javaHMNodeNextField);
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); nxt = nullptr; }
                                    env->DeleteLocalRef(node);
                                    node = nxt;
                                }
                            }
                            env->DeleteLocalRef(tbl);
                            } // inner if(tbl) after GetArrayLength check
                        } // outer if(tbl)
                    }
                    if (!iterated) {
                        usedFallbackPath = true;
                        // Fallback: old iterator approach if direct fields failed
                        jclass mapCls = env->FindClass("java/util/Map");
                        jmethodID mEntrySet = mapCls ? env->GetMethodID(mapCls, "entrySet", "()Ljava/util/Set;") : nullptr;
                        if (env->ExceptionCheck()) { env->ExceptionClear(); mEntrySet = nullptr; }
                        jclass colCls = env->FindClass("java/util/Collection");
                        jmethodID mIter = colCls ? env->GetMethodID(colCls, "iterator", "()Ljava/util/Iterator;") : nullptr;
                        if (env->ExceptionCheck()) { env->ExceptionClear(); mIter = nullptr; }
                        jclass itCls = env->FindClass("java/util/Iterator");
                        jmethodID mHN = itCls ? env->GetMethodID(itCls, "hasNext", "()Z") : nullptr;
                        jmethodID mNxt2 = itCls ? env->GetMethodID(itCls, "next", "()Ljava/lang/Object;") : nullptr;
                        if (env->ExceptionCheck()) { env->ExceptionClear(); mHN = mNxt2 = nullptr; }
                        jclass entryCls = env->FindClass("java/util/Map$Entry");
                        jmethodID mGetKey = entryCls ? env->GetMethodID(entryCls, "getKey", "()Ljava/lang/Object;") : nullptr;
                        jmethodID mGetVal = entryCls ? env->GetMethodID(entryCls, "getValue", "()Ljava/lang/Object;") : nullptr;
                        if (env->ExceptionCheck()) { env->ExceptionClear(); mGetKey = mGetVal = nullptr; }
                        if (mEntrySet && mIter && mHN && mNxt2 && mGetKey && mGetVal) {
                            jobject col = env->CallObjectMethod(mapObj, mEntrySet);
                            if (env->ExceptionCheck()) { env->ExceptionClear(); col = nullptr; }
                            if (col) {
                                jobject it = env->CallObjectMethod(col, mIter);
                                if (env->ExceptionCheck()) { env->ExceptionClear(); it = nullptr; }
                                if (it) {
                                    while (true) {
                                        jboolean hn = env->CallBooleanMethod(it, mHN);
                                        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
                                        if (!hn) break;
                                        jobject entryObj = env->CallObjectMethod(it, mNxt2);
                                        if (env->ExceptionCheck()) { env->ExceptionClear(); if (entryObj) env->DeleteLocalRef(entryObj); break; }
                                        if (!entryObj) continue;
                                        jobject keyBp = env->CallObjectMethod(entryObj, mGetKey);
                                        if (env->ExceptionCheck()) { env->ExceptionClear(); keyBp = nullptr; }
                                        jobject be = env->CallObjectMethod(entryObj, mGetVal);
                                        if (env->ExceptionCheck()) { env->ExceptionClear(); if (keyBp) env->DeleteLocalRef(keyBp); env->DeleteLocalRef(entryObj); break; }
                                        env->DeleteLocalRef(entryObj);
                                        if (!be) continue;
                                        totalBEsScanned++;
                                        if (IsChestBlockEntity(env, be)) {
                                            totalChests++;
                                            EnsureBlockPosCache(env, be);
                                            double cx2 = 0.5, cy2 = 0.0, cz2 = 0.5;
                                            bool gotPos = false;
                                            if (!gotPos && keyBp && g_blockPosClass_121 && env->IsInstanceOf(keyBp, g_blockPosClass_121)) {
                                                gotPos = ReadBlockPosCoords(env, keyBp, cx2, cy2, cz2);
                                                if (gotPos) { cx2 += 0.5; cz2 += 0.5; }
                                            }
                                            if (!gotPos && g_beGetPos_121) {
                                                jobject bp = env->CallObjectMethod(be, g_beGetPos_121);
                                                if (!env->ExceptionCheck() && bp) {
                                                    gotPos = ReadBlockPosCoords(env, bp, cx2, cy2, cz2);
                                                    if (gotPos) { cx2 += 0.5; cz2 += 0.5; }
                                                    env->DeleteLocalRef(bp);
                                                } else env->ExceptionClear();
                                            }
                                            if (!gotPos && g_beBlockPosField_121) {
                                                jobject bp = env->GetObjectField(be, g_beBlockPosField_121);
                                                if (!env->ExceptionCheck() && bp) {
                                                    gotPos = ReadBlockPosCoords(env, bp, cx2, cy2, cz2);
                                                    if (gotPos) { cx2 += 0.5; cz2 += 0.5; }
                                                    env->DeleteLocalRef(bp);
                                                } else env->ExceptionClear();
                                            }
                                            if (gotPos) {
                                                double ddx = cx2-sx, ddy = cy2-sy, ddz = cz2-sz;
                                                localList.push_back({cx2, cy2, cz2, std::sqrt(ddx*ddx+ddy*ddy+ddz*ddz)});
                                            }
                                        }
                                        if (keyBp) env->DeleteLocalRef(keyBp);
                                        env->DeleteLocalRef(be);
                                    }
                                    env->DeleteLocalRef(it);
                                }
                                env->DeleteLocalRef(col);
                            }
                        }
                        if (mapCls) env->DeleteLocalRef(mapCls);
                        if (colCls) env->DeleteLocalRef(colCls);
                        if (itCls)  env->DeleteLocalRef(itCls);
                        if (entryCls) env->DeleteLocalRef(entryCls);
                    }
                    env->DeleteLocalRef(mapObj);
                }
            }
            env->DeleteLocalRef(chunkObj);
        }
    }

    if (shouldLog) { lastLogMs = now; Log("UpdateChestList: found " + std::to_string(totalChests) + " chests, listed=" + std::to_string(localList.size()) + ", scanned " + std::to_string(totalBEsScanned) + " BEs, hmDirect=" + std::to_string(g_javaHashMapTableField ? 1 : 0) + " hashMapSeen=" + std::to_string(sawHashMap ? 1 : 0) + " usedDirect=" + std::to_string(usedDirectPath ? 1 : 0) + " usedFallback=" + std::to_string(usedFallbackPath ? 1 : 0) + " bePos=" + std::to_string(g_beGetPos_121 ? 1 : 0) + " bpX=" + std::to_string(g_blockPosX_121 ? 1 : 0)); }

    env->DeleteLocalRef(worldObj);

    std::sort(localList.begin(), localList.end(), [](const ChestData121& a, const ChestData121& b){ return a.dist < b.dist; });
    { LockGuard lk(g_chestListMutex); g_chestList.swap(localList); }
}

static std::string CallTextToString(JNIEnv* env, jobject textObj) {
    if (!textObj) return "";
    
    // In 1.21, Text is an interface (net.minecraft.class_2561).
    // GetMethodID might fail on the concrete class if it doesn't override it.
    jclass textCls = nullptr;
    if (g_gameClassLoader) {
        textCls = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2561");
    }
    if (!textCls) {
        textCls = env->FindClass("net/minecraft/class_2561");
        if (env->ExceptionCheck()) { env->ExceptionClear(); textCls = nullptr; }
    }
    // Fallback just in case
    if (!textCls) textCls = env->GetObjectClass(textObj);
    if (!textCls) return "";

    if (!g_textGetString_121) {
        g_textGetString_121 = env->GetMethodID(textCls, "getString", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_textGetString_121 = nullptr; }
        if (!g_textGetString_121) {
            g_textGetString_121 = env->GetMethodID(textCls, "method_10851", "()Ljava/lang/String;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_textGetString_121 = nullptr; }
        }

        // Last resort: some clients rename this method differently.
        // Scan declared methods for any 0-arg method returning String and cache it.
        if (!g_textGetString_121) {
            jclass cClass = env->FindClass("java/lang/Class");
            jclass cMethod = env->FindClass("java/lang/reflect/Method");
            jclass cString = env->FindClass("java/lang/String");
            jmethodID mGetMethods = cClass ? env->GetMethodID(cClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;") : nullptr;
            jmethodID mParams = cMethod ? env->GetMethodID(cMethod, "getParameterTypes", "()[Ljava/lang/Class;") : nullptr;
            jmethodID mRet    = cMethod ? env->GetMethodID(cMethod, "getReturnType", "()Ljava/lang/Class;") : nullptr;
            jmethodID mName   = cMethod ? env->GetMethodID(cMethod, "getName", "()Ljava/lang/String;") : nullptr;
            if (env->ExceptionCheck()) { env->ExceptionClear(); mGetMethods = nullptr; mParams = nullptr; mRet = nullptr; mName = nullptr; }

            if (cClass && cMethod && cString && mGetMethods && mParams && mRet && mName) {
                jobjectArray methods = (jobjectArray)env->CallObjectMethod(textCls, mGetMethods);
                if (env->ExceptionCheck()) { env->ExceptionClear(); methods = nullptr; }
                if (methods) {
                    jsize mc = env->GetArrayLength(methods);
                    if (mc > 128) mc = 128;
                    for (int i = 0; i < mc; i++) {
                        jobject m = env->GetObjectArrayElement(methods, i);
                        if (!m) continue;

                        jobjectArray params = (jobjectArray)env->CallObjectMethod(m, mParams);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); params = nullptr; }
                        if (!params) { env->DeleteLocalRef(m); continue; }
                        bool zeroArgs = (env->GetArrayLength(params) == 0);
                        env->DeleteLocalRef(params);
                        if (!zeroArgs) { env->DeleteLocalRef(m); continue; }

                        jobject rt = env->CallObjectMethod(m, mRet);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); rt = nullptr; }
                        bool isString = (rt && env->IsSameObject(rt, cString) == JNI_TRUE);
                        if (rt) env->DeleteLocalRef(rt);
                        if (!isString) { env->DeleteLocalRef(m); continue; }

                        jstring jmn = (jstring)env->CallObjectMethod(m, mName);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); jmn = nullptr; }
                        if (!jmn) { env->DeleteLocalRef(m); continue; }
                        const char* cmn = env->GetStringUTFChars(jmn, nullptr);
                        std::string name = cmn ? cmn : "";
                        if (cmn) env->ReleaseStringUTFChars(jmn, cmn);
                        env->DeleteLocalRef(jmn);

                        jmethodID mid = env->GetMethodID(textCls, name.c_str(), "()Ljava/lang/String;");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
                        env->DeleteLocalRef(m);
                        if (mid) { g_textGetString_121 = mid; break; }
                    }
                    env->DeleteLocalRef(methods);
                }
            } else {
                if (env->ExceptionCheck()) env->ExceptionClear();
            }

            if (cClass) env->DeleteLocalRef(cClass);
            if (cMethod) env->DeleteLocalRef(cMethod);
            if (cString) env->DeleteLocalRef(cString);
        }
    }
    std::string r;
    if (g_textGetString_121) {
        jstring js = (jstring)env->CallObjectMethod(textObj, g_textGetString_121);
        if (!env->ExceptionCheck() && js) {
            const char* cs = env->GetStringUTFChars(js, nullptr);
            if (cs) {
                r = cs;
                env->ReleaseStringUTFChars(js, cs);
            } else {
                r = "";
            }
            env->DeleteLocalRef(js);
        } else {
            env->ExceptionClear();
        }
    }
    env->DeleteLocalRef(textCls);
    return r;
}

static void EnsureClosestPlayerCaches(JNIEnv* env) {
    if (!g_mcInstance) return;

    if (!g_playerEntityClass_121) {
        // PlayerEntity (Yarn): net.minecraft.class_1657
        jclass c = nullptr;
        if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_1657");
        if (!c) {
            c = env->FindClass("net/minecraft/class_1657");
            if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
        }
        if (c) g_playerEntityClass_121 = (jclass)env->NewGlobalRef(c);
    }

    jclass mcCls = env->GetObjectClass(g_mcInstance);
    if (!mcCls) return;

    if (!g_worldField_121) {
        g_worldField_121 = env->GetFieldID(mcCls, "field_1687", "Lnet/minecraft/class_638;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_worldField_121 = nullptr; }
        if (!g_worldField_121) {
            g_worldField_121 = env->GetFieldID(mcCls, "world", "Lnet/minecraft/client/world/ClientWorld;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_worldField_121 = nullptr; }
        }
    }
    if (!g_playerField_121) {
        g_playerField_121 = env->GetFieldID(mcCls, "field_1724", "Lnet/minecraft/class_746;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_playerField_121 = nullptr; }
        if (!g_playerField_121) {
            g_playerField_121 = env->GetFieldID(mcCls, "player", "Lnet/minecraft/client/player/LocalPlayer;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_playerField_121 = nullptr; }
        }
    }

    env->DeleteLocalRef(mcCls);
}

static void DiscoverWorldPlayersListField(JNIEnv* env, jobject worldObj) {
    if (!worldObj || g_worldPlayersListField_121 || !g_playerEntityClass_121) return;

    jclass worldCls = env->GetObjectClass(worldObj);
    if (!worldCls) return;

    // Use reflection once to find a List field that contains PlayerEntity.
    jclass cClass = env->FindClass("java/lang/Class");
    jmethodID mGetFields = cClass ? env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mGetFields = nullptr; }
    if (!mGetFields) { if (cClass) env->DeleteLocalRef(cClass); env->DeleteLocalRef(worldCls); return; }

    jclass cField = env->FindClass("java/lang/reflect/Field");
    jmethodID mFType = cField ? env->GetMethodID(cField, "getType", "()Ljava/lang/Class;") : nullptr;
    jmethodID mFName = cField ? env->GetMethodID(cField, "getName", "()Ljava/lang/String;") : nullptr;
    jmethodID mFMod  = cField ? env->GetMethodID(cField, "getModifiers", "()I") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mFType = mFName = mFMod = nullptr; }

    jclass cMod = env->FindClass("java/lang/reflect/Modifier");
    jmethodID mIsStatic = cMod ? env->GetStaticMethodID(cMod, "isStatic", "(I)Z") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mIsStatic = nullptr; }

    jclass listClass = env->FindClass("java/util/List");

    jobjectArray fields = (jobjectArray)env->CallObjectMethod(worldCls, mGetFields);
    if (env->ExceptionCheck() || !fields) {
        env->ExceptionClear();
        if (listClass) env->DeleteLocalRef(listClass);
        if (cMod) env->DeleteLocalRef(cMod);
        if (cField) env->DeleteLocalRef(cField);
        if (cClass) env->DeleteLocalRef(cClass);
        env->DeleteLocalRef(worldCls);
        return;
    }

    jsize fc = env->GetArrayLength(fields);
    for (int i = 0; i < fc; i++) {
        jobject fld = env->GetObjectArrayElement(fields, i);
        if (!fld) continue;
        jint mod = mFMod ? env->CallIntMethod(fld, mFMod) : 0;
        if (mIsStatic && env->CallStaticBooleanMethod(cMod, mIsStatic, mod)) { env->DeleteLocalRef(fld); continue; }
        jclass ft = mFType ? (jclass)env->CallObjectMethod(fld, mFType) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); ft = nullptr; }
        if (!ft) { env->DeleteLocalRef(fld); continue; }
        bool isList = (listClass && env->IsAssignableFrom(ft, listClass));
        if (!isList) { env->DeleteLocalRef(ft); env->DeleteLocalRef(fld); continue; }

        // Get field name
        jstring jfn = mFName ? (jstring)env->CallObjectMethod(fld, mFName) : nullptr;
        if (env->ExceptionCheck() || !jfn) { env->ExceptionClear(); env->DeleteLocalRef(ft); env->DeleteLocalRef(fld); continue; }
        const char* cfn = env->GetStringUTFChars(jfn, nullptr);
        std::string fn = cfn ? cfn : "";
        env->ReleaseStringUTFChars(jfn, cfn);
        env->DeleteLocalRef(jfn);

        // Resolve jfieldID and sample list content
        jfieldID fid = env->GetFieldID(worldCls, fn.c_str(), "Ljava/util/List;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
        if (fid) {
            jobject listObj = env->GetObjectField(worldObj, fid);
            if (!env->ExceptionCheck() && listObj) {
                jclass lCls = env->GetObjectClass(listObj);
                jmethodID mSize = lCls ? env->GetMethodID(lCls, "size", "()I") : nullptr;
                jmethodID mGet  = lCls ? env->GetMethodID(lCls, "get", "(I)Ljava/lang/Object;") : nullptr;
                if (env->ExceptionCheck()) { env->ExceptionClear(); mSize = mGet = nullptr; }
                if (mSize && mGet) {
                    jint sz = env->CallIntMethod(listObj, mSize);
                    if (!env->ExceptionCheck() && sz > 0) {
                        jobject first = env->CallObjectMethod(listObj, mGet, 0);
                        if (!env->ExceptionCheck() && first) {
                            if (env->IsInstanceOf(first, g_playerEntityClass_121)) {
                                g_worldPlayersListField_121 = fid;
                                Log("Discovered world players list field: " + fn);
                                env->DeleteLocalRef(first);
                                env->DeleteLocalRef(listObj);
                                if (lCls) env->DeleteLocalRef(lCls);
                                env->DeleteLocalRef(ft);
                                env->DeleteLocalRef(fld);
                                break;
                            }
                            env->DeleteLocalRef(first);
                        } else {
                            env->ExceptionClear();
                        }
                    } else {
                        env->ExceptionClear();
                    }
                }
                if (lCls) env->DeleteLocalRef(lCls);
                env->DeleteLocalRef(listObj);
            } else {
                env->ExceptionClear();
            }
        }

        env->DeleteLocalRef(ft);
        env->DeleteLocalRef(fld);
        if (g_worldPlayersListField_121) break;
    }

    env->DeleteLocalRef(fields);
    if (listClass) env->DeleteLocalRef(listClass);
    if (cMod) env->DeleteLocalRef(cMod);
    if (cField) env->DeleteLocalRef(cField);
    if (cClass) env->DeleteLocalRef(cClass);
    env->DeleteLocalRef(worldCls);
}

static void EnsureEntityMethods(JNIEnv* env, jobject entObj) {
    if (!entObj) return;
    jclass entCls = env->GetObjectClass(entObj);
    if (entCls) {
        if (!g_getX_121) {
            g_getX_121 = env->GetMethodID(entCls, "getX", "()D");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getX_121 = nullptr; }
            if (!g_getX_121) {
                g_getX_121 = env->GetMethodID(entCls, "method_23317", "()D");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getX_121 = nullptr; }
            }
        }
        if (!g_getY_121) {
            g_getY_121 = env->GetMethodID(entCls, "getY", "()D");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getY_121 = nullptr; }
            if (!g_getY_121) {
                g_getY_121 = env->GetMethodID(entCls, "method_23318", "()D");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getY_121 = nullptr; }
            }
        }
        if (!g_getZ_121) {
            g_getZ_121 = env->GetMethodID(entCls, "getZ", "()D");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getZ_121 = nullptr; }
            if (!g_getZ_121) {
                g_getZ_121 = env->GetMethodID(entCls, "method_23321", "()D");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getZ_121 = nullptr; }
            }
        }
        if (!g_getYaw_121) {
            g_getYaw_121 = env->GetMethodID(entCls, "getYaw", "()F");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getYaw_121 = nullptr; }
            if (!g_getYaw_121) {
                g_getYaw_121 = env->GetMethodID(entCls, "method_5669", "()F");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getYaw_121 = nullptr; }
            }
        }
        if (!g_getPitch_121) {
            g_getPitch_121 = env->GetMethodID(entCls, "getPitch", "()F");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getPitch_121 = nullptr; }
            if (!g_getPitch_121) {
                g_getPitch_121 = env->GetMethodID(entCls, "method_5667", "()F");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getPitch_121 = nullptr; }
            }
        }
        if (!g_getHealth_121) {
            g_getHealth_121 = env->GetMethodID(entCls, "getHealth", "()F");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getHealth_121 = nullptr; }
            if (!g_getHealth_121) {
                g_getHealth_121 = env->GetMethodID(entCls, "method_6032", "()F");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getHealth_121 = nullptr; }
            }
        }
        if (!g_getName_121) {
            g_getName_121 = env->GetMethodID(entCls, "getName", "()Lnet/minecraft/class_2561;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getName_121 = nullptr; }
            if (!g_getName_121) {
                g_getName_121 = env->GetMethodID(entCls, "method_5477", "()Lnet/minecraft/class_2561;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getName_121 = nullptr; }
            }
        }
        if (!g_getArmor_121) {
            g_getArmor_121 = env->GetMethodID(entCls, "getArmor", "()I");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getArmor_121 = nullptr; }
            if (!g_getArmor_121) {
                g_getArmor_121 = env->GetMethodID(entCls, "method_6096", "()I");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getArmor_121 = nullptr; }
            }
        }
        // Discover Entity.pos field (Vec3d) for zero-JNI position reads
        if (!g_entityPosField_121) {
            // Yarn: field_5979 / interp name "pos" on Entity class
            g_entityPosField_121 = env->GetFieldID(entCls, "field_5979", "Lnet/minecraft/class_243;");
            if (env->ExceptionCheck()) { env->ExceptionClear();
                g_entityPosField_121 = env->GetFieldID(entCls, "pos", "Lnet/minecraft/class_243;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_entityPosField_121 = nullptr; }
            }
        }
        if (!g_getMainHandStack_121) {
            g_getMainHandStack_121 = env->GetMethodID(entCls, "getMainHandStack", "()Lnet/minecraft/class_1799;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getMainHandStack_121 = nullptr; }
            if (!g_getMainHandStack_121) {
                g_getMainHandStack_121 = env->GetMethodID(entCls, "method_6047", "()Lnet/minecraft/class_1799;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getMainHandStack_121 = nullptr; }
            }
        }
        
        if (!g_itemStackClass_121) {
            jclass c = nullptr;
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_1799");
            if (!c) {
                c = env->FindClass("net/minecraft/class_1799");
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
            if (c) { 
                g_itemStackClass_121 = (jclass)env->NewGlobalRef(c); 
                env->DeleteLocalRef(c); 
                
                g_itemStackGetName_121 = env->GetMethodID(g_itemStackClass_121, "getName", "()Lnet/minecraft/class_2561;");
                if (env->ExceptionCheck() || !g_itemStackGetName_121) { env->ExceptionClear(); g_itemStackGetName_121 = env->GetMethodID(g_itemStackClass_121, "method_7964", "()Lnet/minecraft/class_2561;"); if (env->ExceptionCheck()) env->ExceptionClear(); }
                
                g_itemStackGetDamage_121 = env->GetMethodID(g_itemStackClass_121, "getDamage", "()I");
                if (env->ExceptionCheck() || !g_itemStackGetDamage_121) { env->ExceptionClear(); g_itemStackGetDamage_121 = env->GetMethodID(g_itemStackClass_121, "method_7936", "()I"); if (env->ExceptionCheck()) env->ExceptionClear(); }
                
                g_itemStackGetMaxDamage_121 = env->GetMethodID(g_itemStackClass_121, "getMaxDamage", "()I");
                if (env->ExceptionCheck() || !g_itemStackGetMaxDamage_121) { env->ExceptionClear(); g_itemStackGetMaxDamage_121 = env->GetMethodID(g_itemStackClass_121, "method_7919", "()I"); if (env->ExceptionCheck()) env->ExceptionClear(); }
            }
        }

        // Prep stable-name fallback caches (GameProfile) using this entity object.
        EnsureGameProfileCaches(env, entObj);
        env->DeleteLocalRef(entCls);
    }
}

static void UpdateClosestPlayerOverlay(JNIEnv* env) {
    // Throttle heavy JNI work.
    DWORD now = GetTickCount();
    if (now - g_lastClosestUpdateMs < 100) return;
    g_lastClosestUpdateMs = now;

    EnsureClosestPlayerCaches(env);
    if (!g_worldField_121 || !g_playerField_121) return;

    jobject worldObj = env->GetObjectField(g_mcInstance, g_worldField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); worldObj = nullptr; }
    jobject selfObj = env->GetObjectField(g_mcInstance, g_playerField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); selfObj = nullptr; }
    if (!worldObj || !selfObj) {
        if (worldObj) env->DeleteLocalRef(worldObj);
        if (selfObj) env->DeleteLocalRef(selfObj);
        return;
    }

    EnsureEntityMethods(env, selfObj);
    if (!g_getX_121 || !g_getY_121 || !g_getZ_121) {
        env->DeleteLocalRef(worldObj);
        env->DeleteLocalRef(selfObj);
        return;
    }

    double sx = CallDoubleNoArgs(env, selfObj, g_getX_121);
    double sy = CallDoubleNoArgs(env, selfObj, g_getY_121);
    double sz = CallDoubleNoArgs(env, selfObj, g_getZ_121);

    DiscoverWorldPlayersListField(env, worldObj);
    if (!g_worldPlayersListField_121) {
        env->DeleteLocalRef(worldObj);
        env->DeleteLocalRef(selfObj);
        return;
    }

    jobject listObj = env->GetObjectField(worldObj, g_worldPlayersListField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); listObj = nullptr; }
    if (!listObj) {
        env->DeleteLocalRef(worldObj);
        env->DeleteLocalRef(selfObj);
        return;
    }

    jclass lCls = env->GetObjectClass(listObj);
    jmethodID mSize = lCls ? env->GetMethodID(lCls, "size", "()I") : nullptr;
    jmethodID mGet  = lCls ? env->GetMethodID(lCls, "get", "(I)Ljava/lang/Object;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mSize = mGet = nullptr; }
    if (!mSize || !mGet) {
        if (lCls) env->DeleteLocalRef(lCls);
        env->DeleteLocalRef(listObj);
        env->DeleteLocalRef(worldObj);
        env->DeleteLocalRef(selfObj);
        return;
    }

    jint count = env->CallIntMethod(listObj, mSize);
    if (env->ExceptionCheck()) { env->ExceptionClear(); count = 0; }

    std::string bestName;
    double bestDist = -1.0;

    for (int i = 0; i < count; i++) {
        jobject p = env->CallObjectMethod(listObj, mGet, (jint)i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); p = nullptr; }
        if (!p) continue;
        if (env->IsSameObject(p, selfObj)) { env->DeleteLocalRef(p); continue; }

        double px = CallDoubleNoArgs(env, p, g_getX_121);
        double py = CallDoubleNoArgs(env, p, g_getY_121);
        double pz = CallDoubleNoArgs(env, p, g_getZ_121);
        double dx = px - sx, dy = py - sy, dz = pz - sz;
        double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (bestDist < 0 || dist < bestDist) {
            bestDist = dist;
            bestName = GetStablePlayerName(env, p);
        }

        env->DeleteLocalRef(p);
    }

    if (lCls) env->DeleteLocalRef(lCls);
    env->DeleteLocalRef(listObj);
    env->DeleteLocalRef(worldObj);
    env->DeleteLocalRef(selfObj);

    g_closestName = bestName;
    g_closestDist = bestDist;
}

typedef BOOL (WINAPI* TwglSwapBuffers)(HDC);
static TwglSwapBuffers o_wglSwapBuffers = nullptr;

typedef void* (*PFN_glfwGetCurrentContext)();
typedef void  (*PFN_glfwSetInputMode)(void* window, int mode, int value);
typedef int   (*PFN_glfwGetInputMode)(void* window, int mode);
static PFN_glfwGetCurrentContext glfwGetCurrentContext_fn = nullptr;
static PFN_glfwSetInputMode      glfwSetInputMode_fn     = nullptr;
static PFN_glfwGetInputMode      glfwGetInputMode_fn     = nullptr;

#define GLFW_CURSOR            0x00033001
#define GLFW_CURSOR_NORMAL     0x00034001
#define GLFW_CURSOR_DISABLED   0x00034003
#define GLFW_RAW_MOUSE_MOTION  0x00033005

// ===================== LOGGER =====================
static std::string g_logPath = "bridge_121_debug.log";

static void Log(const std::string& msg) {
    std::ofstream out(g_logPath, std::ios_base::app);
    out << msg << "\n";
}

static bool FileExistsA(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

static bool IsLikelyFontBinary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    unsigned char hdr[4] = {0};
    f.read((char*)hdr, 4);
    if (f.gcount() < 4) return false;

    // TrueType/OpenType headers: 00 01 00 00, "OTTO", "true", "ttcf"
    if (hdr[0] == 0x00 && hdr[1] == 0x01 && hdr[2] == 0x00 && hdr[3] == 0x00) return true;
    if (hdr[0] == 'O' && hdr[1] == 'T' && hdr[2] == 'T' && hdr[3] == 'O') return true;
    if (hdr[0] == 't' && hdr[1] == 'r' && hdr[2] == 'u' && hdr[3] == 'e') return true;
    if (hdr[0] == 't' && hdr[1] == 't' && hdr[2] == 'c' && hdr[3] == 'f') return true;
    return false;
}

static std::string GetBridgeDir() {
    size_t pos = g_logPath.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    return g_logPath.substr(0, pos);
}

// ===================== JNI HELPERS (ported from 1.8.9 bridge) =====================
static std::string GetClassNameFromClass(JNIEnv* env, jclass cls) {
    if (!cls) return "";
    jclass classClass = env->FindClass("java/lang/Class");
    if (!classClass || env->ExceptionCheck()) { env->ExceptionClear(); return ""; }
    jmethodID getName = env->GetMethodID(classClass, "getName", "()Ljava/lang/String;");
    if (!getName || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(classClass); return ""; }
    jstring jn = (jstring)env->CallObjectMethod(cls, getName);
    if (!jn || env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(classClass); return ""; }
    const char* cn = env->GetStringUTFChars(jn, nullptr);
    std::string r = "";
    if (cn) {
        r = cn;
        env->ReleaseStringUTFChars(jn, cn);
    } else {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(jn);
    env->DeleteLocalRef(classClass);
    return r;
}

static jfieldID FindFieldByType(JNIEnv* env, jclass targetClass, const std::string& typeSig) {
    if (!targetClass) return nullptr;
    jclass cls = env->FindClass("java/lang/Class");
    jmethodID getFields = env->GetMethodID(cls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jclass fldCls = env->FindClass("java/lang/reflect/Field");
    jmethodID getType = env->GetMethodID(fldCls, "getType", "()Ljava/lang/Class;");
    jmethodID getName = env->GetMethodID(fldCls, "getName", "()Ljava/lang/String;");
    
    jobjectArray fields = (jobjectArray)env->CallObjectMethod(targetClass, getFields);
    jsize count = fields ? env->GetArrayLength(fields) : 0;
    jfieldID res = nullptr;
    
    for (int i = 0; i < count; i++) {
        jobject f = env->GetObjectArrayElement(fields, i);
        if (!f) continue;
        jclass t = (jclass)env->CallObjectMethod(f, getType);
        std::string tName = GetClassNameFromClass(env, t);
        if (t) env->DeleteLocalRef(t);
        
        std::string expected = typeSig;
        if (expected.length() > 2 && expected[0] == 'L' && expected.back() == ';') {
            expected = expected.substr(1, expected.length() - 2);
            std::replace(expected.begin(), expected.end(), '/', '.');
        }
        if (tName == expected) {
            jstring jName = (jstring)env->CallObjectMethod(f, getName);
            const char* n = env->GetStringUTFChars(jName, nullptr);
            res = env->GetFieldID(targetClass, n, typeSig.c_str());
            env->ReleaseStringUTFChars(jName, n);
            env->DeleteLocalRef(jName);
            env->DeleteLocalRef(f);
            break;
        }
        env->DeleteLocalRef(f);
    }
    if (fields) env->DeleteLocalRef(fields);
    env->DeleteLocalRef(fldCls);
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return res;
}

static jobject GetGameClassLoader(JNIEnv* env) {
    jclass cThread = env->FindClass("java/lang/Thread");
    jmethodID mGetAll = env->GetStaticMethodID(cThread, "getAllStackTraces", "()Ljava/util/Map;");
    jobject map = env->CallStaticObjectMethod(cThread, mGetAll);
    jclass cMap = env->FindClass("java/util/Map");
    jobject set = env->CallObjectMethod(map, env->GetMethodID(cMap, "keySet", "()Ljava/util/Set;"));
    jclass cSet = env->FindClass("java/util/Set");
    jobjectArray threads = (jobjectArray)env->CallObjectMethod(set, env->GetMethodID(cSet, "toArray", "()[Ljava/lang/Object;"));
    jsize count = env->GetArrayLength(threads);
    jmethodID mName = env->GetMethodID(cThread, "getName", "()Ljava/lang/String;");
    jmethodID mGetCL = env->GetMethodID(cThread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    // Look for "Render thread" (1.21) or "Client thread" (1.8.9)
    for (int i = 0; i < count; i++) {
        jobject t = env->GetObjectArrayElement(threads, i);
        jstring jn = (jstring)env->CallObjectMethod(t, mName);
        const char* cn = env->GetStringUTFChars(jn, nullptr);
        bool found = (strstr(cn, "Render thread") != nullptr ||
                      strstr(cn, "Client thread") != nullptr ||
                      strstr(cn, "main") != nullptr);
        env->ReleaseStringUTFChars(jn, cn);
        if (found) {
            jobject cl = env->CallObjectMethod(t, mGetCL);
            if (cl) return cl;
        }
    }
    return nullptr;
}

static jclass LoadClassWithLoader(JNIEnv* env, jobject cl, const char* name) {
    jclass cCL = env->FindClass("java/lang/ClassLoader");
    jmethodID m = env->GetMethodID(cCL, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    std::string dn = name; std::replace(dn.begin(), dn.end(), '/', '.');
    jstring jdn = env->NewStringUTF(dn.c_str());
    jclass cls = (jclass)env->CallObjectMethod(cl, m, jdn);
    env->DeleteLocalRef(jdn);
    env->DeleteLocalRef(cCL);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return cls;
}

// ===================== JNI DISCOVERY (ported from 1.8.9, adapted for 1.21) =====================
// Uses JVMTI to scan loaded classes, finds Minecraft by singleton pattern,
// discovers screen field by method hierarchy walking, finds ChatScreen + setScreen.
static bool DiscoverJniMappings(JNIEnv* env) {
    Log("Starting JNI discovery for 1.21...");

    // Get JVMTI
    jvmtiEnv* jvmti = nullptr;
    if (g_jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_2) != JNI_OK || !jvmti) {
        Log("ERROR: Failed to get JVMTI"); return false;
    }
    jint classCount = 0; jclass* classes = nullptr;
    jvmti->GetLoadedClasses(&classCount, &classes);
    Log("Loaded classes: " + std::to_string(classCount));

    // Get game classloader
    jobject gcl = GetGameClassLoader(env);
    if (!gcl) { Log("ERROR: No game classloader found"); jvmti->Deallocate((unsigned char*)classes); return false; }
    Log("Game classloader found.");

    // Store classloader globally for later lazy class loads.
    if (!g_gameClassLoader) {
        g_gameClassLoader = env->NewGlobalRef(gcl);
        Log("Stored game classloader global ref.");
    }

    // Reflection setup
    jclass cClass = env->FindClass("java/lang/Class");
    jmethodID mGetName    = env->GetMethodID(cClass, "getName", "()Ljava/lang/String;");
    jmethodID mGetFields  = env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jmethodID mGetMethods = env->GetMethodID(cClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jmethodID mGetSuper   = env->GetMethodID(cClass, "getSuperclass", "()Ljava/lang/Class;");

    jclass cField = env->FindClass("java/lang/reflect/Field");
    jmethodID mFType = env->GetMethodID(cField, "getType", "()Ljava/lang/Class;");
    jmethodID mFName = env->GetMethodID(cField, "getName", "()Ljava/lang/String;");
    jmethodID mFMod  = env->GetMethodID(cField, "getModifiers", "()I");

    jclass cMethod = env->FindClass("java/lang/reflect/Method");
    jmethodID mMName   = env->GetMethodID(cMethod, "getName", "()Ljava/lang/String;");
    jmethodID mMRet    = env->GetMethodID(cMethod, "getReturnType", "()Ljava/lang/Class;");
    jmethodID mMParams = env->GetMethodID(cMethod, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID mMMod    = env->GetMethodID(cMethod, "getModifiers", "()I");

    jclass cMod = env->FindClass("java/lang/reflect/Modifier");
    jmethodID mIsStatic = env->GetStaticMethodID(cMod, "isStatic", "(I)Z");

    // ---- Step 1: Find Minecraft class by known name or singleton scan ----
    jclass mcClass = nullptr;
    std::string mcName;

    // Try known names first
    const char* knownMC[] = {"net.minecraft.client.Minecraft", nullptr};
    for (int i = 0; knownMC[i]; i++) {
        jclass c = LoadClassWithLoader(env, gcl, knownMC[i]);
        if (c) { mcClass = c; mcName = knownMC[i]; Log("Found MC by name: " + mcName); break; }
    }

    // Fallback: scan all classes for singleton pattern (like 1.8.9)
    // Collect all candidates and pick the best one (most fields, prefer net.minecraft)
    if (!mcClass) {
        jclass bestCls = nullptr;
        std::string bestName;
        int bestFields = 0;

        for (int i = 0; i < classCount; i++) {
            jclass cls = classes[i];
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            jstring jn = (jstring)env->CallObjectMethod(cls, mGetName);
            if (!jn || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            const char* cn = env->GetStringUTFChars(jn, nullptr);
            std::string name = cn ? cn : "";
            if (cn) env->ReleaseStringUTFChars(jn, cn);
            // Skip JDK/library/launcher/Lunar classes
            if (name.find("java.") == 0 || name.find("sun.") == 0 || name.find("javax.") == 0 ||
                name.find("com.sun.") == 0 || name.find("org.") == 0 || name.find("jdk.") == 0 ||
                name.find("com.google.") == 0 || name.find("io.") == 0 || name[0] == '[' ||
                name.find("com.moonsworth.") == 0 || name.find("com.lunarclient.") == 0 ||
                name.find("lunar.") == 0) continue;

            jobjectArray fields = (jobjectArray)env->CallObjectMethod(cls, mGetFields);
            if (!fields || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            jsize fc = env->GetArrayLength(fields);
            bool hasSelf = false; int objCount = 0;
            for (int f = 0; f < fc; f++) {
                jobject fld = env->GetObjectArrayElement(fields, f);
                if (!fld) continue;
                jint mod = env->CallIntMethod(fld, mFMod);
                bool isS = env->CallStaticBooleanMethod(cMod, mIsStatic, mod);
                jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
                if (!ft || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                std::string tn = GetClassNameFromClass(env, ft);
                if (isS && tn == name) hasSelf = true;
                if (!isS) objCount++;
            }
            if (hasSelf && objCount > 15) {
                Log("Singleton candidate: " + name + " (" + std::to_string(objCount) + " fields)");
                // Prefer candidate with more fields, or net.minecraft over anything else
                bool better = false;
                if (!bestCls) better = true;
                else if (name.find("net.minecraft") == 0 && bestName.find("net.minecraft") != 0) better = true;
                else if (objCount > bestFields && bestName.find("net.minecraft") != 0) better = true;
                if (better) {
                    bestCls = cls;
                    bestName = name;
                    bestFields = objCount;
                }
            }
        }
        if (bestCls) {
            mcClass = (jclass)env->NewGlobalRef(bestCls);
            mcName = bestName;
            Log("Selected MC class: " + mcName + " (" + std::to_string(bestFields) + " fields)");
        }
    }
    if (!mcClass) { Log("ERROR: Minecraft class not found"); jvmti->Deallocate((unsigned char*)classes); return false; }

    // ---- Step 2: Find MC singleton instance ----
    jobjectArray mcFields = (jobjectArray)env->CallObjectMethod(mcClass, mGetFields);
    jsize mcFC = env->GetArrayLength(mcFields);
    jfieldID singletonField = nullptr;

    for (int f = 0; f < mcFC; f++) {
        jobject fld = env->GetObjectArrayElement(mcFields, f);
        if (!fld) continue;
        jint mod = env->CallIntMethod(fld, mFMod);
        bool isS = env->CallStaticBooleanMethod(cMod, mIsStatic, mod);
        jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
        if (!ft || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        std::string tn = GetClassNameFromClass(env, ft);
        jstring jfn = (jstring)env->CallObjectMethod(fld, mFName);
        const char* cfn = env->GetStringUTFChars(jfn, nullptr);
        std::string fn = cfn ? cfn : "";
        if (cfn) env->ReleaseStringUTFChars(jfn, cfn);
        if (isS && tn == mcName) {
            std::string sig = "L" + mcName + ";"; std::replace(sig.begin(), sig.end(), '.', '/');
            singletonField = env->GetStaticFieldID(mcClass, fn.c_str(), sig.c_str());
            if (env->ExceptionCheck()) env->ExceptionClear();
            Log("Singleton field: " + fn);
        }
    }
    if (!singletonField) { Log("ERROR: No singleton field"); jvmti->Deallocate((unsigned char*)classes); return false; }

    jobject mcInst = env->GetStaticObjectField(mcClass, singletonField);
    if (!mcInst) { Log("ERROR: MC instance null"); jvmti->Deallocate((unsigned char*)classes); return false; }
    Log("Got Minecraft instance.");

    // Diagnostics disabled

    // ---- Step 3: Find screen field by walking class hierarchy ----
    // In 1.21 the screen field type has render/tick methods (1.8.9 used drawScreen/drawDefaultBackground)
    std::string screenType;
    for (int f = 0; f < mcFC; f++) {
        jobject fld = env->GetObjectArrayElement(mcFields, f);
        if (!fld) continue;
        jint mod = env->CallIntMethod(fld, mFMod);
        if (env->CallStaticBooleanMethod(cMod, mIsStatic, mod)) continue; // skip static
        jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
        if (!ft || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        std::string tn = GetClassNameFromClass(env, ft);
        jstring jfn = (jstring)env->CallObjectMethod(fld, mFName);
        const char* cfn = env->GetStringUTFChars(jfn, nullptr);
        std::string fn = cfn ? cfn : "";
        if (cfn) env->ReleaseStringUTFChars(jfn, cfn);

        // Walk hierarchy to check for screen-like signatures
        jclass walk = ft; int depth = 0;
        bool isScreen = false;
        while (walk && depth < 8) {
            jobjectArray methods = (jobjectArray)env->CallObjectMethod(walk, mGetMethods);
            if (methods && !env->ExceptionCheck()) {
                jsize mc2 = env->GetArrayLength(methods);
                for (int m2 = 0; m2 < mc2; m2++) {
                    jobject mth = env->GetObjectArrayElement(methods, m2);
                    if (!mth) continue;
                    jstring jmn = (jstring)env->CallObjectMethod(mth, mMName);
                    const char* cmn = env->GetStringUTFChars(jmn, nullptr);
                    if (cmn) {
                        std::string s_cmn(cmn);
                        if (s_cmn == "renderBackground" || s_cmn == "drawScreen" ||
                            s_cmn == "drawDefaultBackground" || s_cmn == "method_25420" ||
                            s_cmn == "method_25394") {
                            isScreen = true;
                        }
                        env->ReleaseStringUTFChars(jmn, cmn);
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (isScreen) break;
            walk = (jclass)env->CallObjectMethod(walk, mGetSuper);
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            depth++;
        }
        if (isScreen && !g_screenField) {
            std::string sig = "L" + tn + ";"; std::replace(sig.begin(), sig.end(), '.', '/');
            g_screenField = env->GetFieldID(mcClass, fn.c_str(), sig.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_screenField = nullptr; }
            if (g_screenField) {
                screenType = tn;
                g_screenType = tn;
                Log("Screen field: " + fn + " type=" + tn);
            }
        }
    }

    // ---- Step 4: Find setScreen method (takes Screen type, returns void) ----
    // 1.21 Mojmap: setScreen(Screen)  |  1.8.9: displayGuiScreen(GuiScreen)
    if (!screenType.empty()) {
        std::string screenSig = "L" + screenType + ";";
        std::replace(screenSig.begin(), screenSig.end(), '.', '/');
        std::string fullSig = "(" + screenSig + ")V";

        // Prefer known method names first (Yarn commonly: method_1507).
        g_setScreenMethod = env->GetMethodID(mcClass, "setScreen", fullSig.c_str());
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_setScreenMethod = nullptr; }
        if (!g_setScreenMethod) {
            g_setScreenMethod = env->GetMethodID(mcClass, "method_1507", fullSig.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_setScreenMethod = nullptr; }
        }
        if (g_setScreenMethod) {
            Log(std::string("setScreen method (preferred): sig=") + fullSig);
        }

        // Fallback: heuristic scan (only if preferred names failed).
        if (!g_setScreenMethod) {
            jobjectArray methods = (jobjectArray)env->CallObjectMethod(mcClass, mGetMethods);
            if (methods && !env->ExceptionCheck()) {
                jsize mc2 = env->GetArrayLength(methods);
                for (int m2 = 0; m2 < mc2; m2++) {
                    jobject mth = env->GetObjectArrayElement(methods, m2);
                    if (!mth) continue;
                    // Check: 1 param of Screen type, void return
                    jobjectArray params = (jobjectArray)env->CallObjectMethod(mth, mMParams);
                    if (!params || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                    if (env->GetArrayLength(params) != 1) continue;
                    jclass pt = (jclass)env->GetObjectArrayElement(params, 0);
                    std::string ptn = GetClassNameFromClass(env, pt);
                    if (ptn != screenType) continue;
                    jclass rt = (jclass)env->CallObjectMethod(mth, mMRet);
                    std::string rtn = GetClassNameFromClass(env, rt);
                    if (rtn != "void") continue;
                    // Get method name and resolve it
                    jstring jmn = (jstring)env->CallObjectMethod(mth, mMName);
                    const char* cmn = env->GetStringUTFChars(jmn, nullptr);
                    std::string mname = cmn; env->ReleaseStringUTFChars(jmn, cmn);
                    g_setScreenMethod = env->GetMethodID(mcClass, mname.c_str(), fullSig.c_str());
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_setScreenMethod = nullptr; continue; }
                    Log("setScreen method (scanned): " + mname + " sig=" + fullSig);
                    break;
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // ---- Step 5: Find ChatScreen (subclass of Screen with String constructor) ----
    if (!screenType.empty()) {
        // Try known names first
        const char* knownChat[] = {
            "net.minecraft.client.gui.screens.ChatScreen",
            "net.minecraft.client.gui.GuiChat",
            "net.minecraft.class_408", // Fabric ChatScreen
            "net.minecraft.class_437", // Fabric Screen (fallback empty screen)
            nullptr
        };
        for (int i = 0; knownChat[i]; i++) {
            jclass c = LoadClassWithLoader(env, gcl, knownChat[i]);
            if (c) {
                // Constructors vary in 1.21 clients (some take String, some String+bool, some empty).
                // Use reflection to find an allowed ctor and then resolve it to a jmethodID.
                jclass cClass = env->FindClass("java/lang/Class");
                jmethodID mGetCtors = cClass ? env->GetMethodID(cClass, "getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;") : nullptr;
                if (env->ExceptionCheck()) { env->ExceptionClear(); mGetCtors = nullptr; }

                jclass cCtor = env->FindClass("java/lang/reflect/Constructor");
                jmethodID mParams = cCtor ? env->GetMethodID(cCtor, "getParameterTypes", "()[Ljava/lang/Class;") : nullptr;
                if (env->ExceptionCheck()) { env->ExceptionClear(); mParams = nullptr; }

                jclass cString = env->FindClass("java/lang/String");
                jclass cBoolPrim = env->FindClass("java/lang/Boolean"); // only used for name; primitive check via class name

                int kind = -1;
                std::string sig;
                if (mGetCtors && mParams) {
                    jobjectArray ctors = (jobjectArray)env->CallObjectMethod(c, mGetCtors);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); ctors = nullptr; }
                    if (ctors) {
                        jsize cc = env->GetArrayLength(ctors);
                        for (int ci = 0; ci < cc; ci++) {
                            jobject ctorObj = env->GetObjectArrayElement(ctors, ci);
                            if (!ctorObj) continue;
                            jobjectArray params = (jobjectArray)env->CallObjectMethod(ctorObj, mParams);
                            if (env->ExceptionCheck()) { env->ExceptionClear(); params = nullptr; }
                            if (!params) { env->DeleteLocalRef(ctorObj); continue; }

                            jsize pc = env->GetArrayLength(params);
                            if (pc == 0) {
                                kind = 0; sig = "()V";
                            } else if (pc == 1) {
                                jclass p0 = (jclass)env->GetObjectArrayElement(params, 0);
                                if (p0 && cString && env->IsAssignableFrom(p0, cString)) {
                                    kind = 1; sig = "(Ljava/lang/String;)V";
                                }
                                if (p0) env->DeleteLocalRef(p0);
                            } else if (pc == 2) {
                                jclass p0 = (jclass)env->GetObjectArrayElement(params, 0);
                                jclass p1 = (jclass)env->GetObjectArrayElement(params, 1);
                                std::string p1n = p1 ? GetClassNameFromClass(env, p1) : "";
                                if (p0 && cString && env->IsAssignableFrom(p0, cString) && (p1n == "boolean" || p1n == "java.lang.Boolean")) {
                                    kind = 2; sig = "(Ljava/lang/String;Z)V";
                                }
                                if (p0) env->DeleteLocalRef(p0);
                                if (p1) env->DeleteLocalRef(p1);
                            }

                            env->DeleteLocalRef(params);
                            env->DeleteLocalRef(ctorObj);
                            if (kind != -1) break;
                        }
                        env->DeleteLocalRef(ctors);
                    }
                }

                if (cBoolPrim) env->DeleteLocalRef(cBoolPrim);
                if (cString) env->DeleteLocalRef(cString);
                if (cCtor) env->DeleteLocalRef(cCtor);
                if (cClass) env->DeleteLocalRef(cClass);

                if (kind != -1) {
                    jmethodID ctor = env->GetMethodID(c, "<init>", sig.c_str());
                    if (env->ExceptionCheck()) { env->ExceptionClear(); ctor = nullptr; }
                    if (ctor) {
                        g_chatScreenClass = (jclass)env->NewGlobalRef(c);
                        g_chatScreenCtor  = ctor;
                        g_chatCtorKind    = kind;
                        Log("Found ChatScreen by name: " + std::string(knownChat[i]) + " ctorSig=" + sig);
                        break;
                    }
                }
            }
        }
        // Fallback: scan loaded classes for Screen subclass with String ctor
        if (!g_chatScreenClass) {
            jclass screenCls = LoadClassWithLoader(env, gcl, screenType.c_str());
            for (int i = 0; i < classCount && !g_chatScreenClass; i++) {
                jclass cls = classes[i];
                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                // Must be subclass of Screen
                if (screenCls && !env->IsAssignableFrom(cls, screenCls)) continue;
                // Check for String constructor (1.8.9)
                jmethodID ctor = env->GetMethodID(cls, "<init>", "(Ljava/lang/String;)V");
                if (env->ExceptionCheck()) { env->ExceptionClear(); ctor = nullptr; }
                // Check for empty constructor (1.21)
                if (!ctor) {
                    ctor = env->GetMethodID(cls, "<init>", "()V");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); ctor = nullptr; }
                }
                if (!ctor) continue;
                
                // Check class name contains "Chat" or "chat"
                std::string cn = GetClassNameFromClass(env, cls);
                if (cn.find("Chat") != std::string::npos || cn.find("chat") != std::string::npos) {
                    g_chatScreenClass = (jclass)env->NewGlobalRef(cls);
                    g_chatScreenCtor  = ctor;
                    Log("Found ChatScreen by scan: " + cn);
                }
            }
        }
    }

    // ---- Step 6: Store globals ----
    g_mcInstance = env->NewGlobalRef(mcInst);
    g_stateJniReady = (g_screenField != nullptr);
    g_chatJniReady  = (g_setScreenMethod != nullptr && g_chatScreenClass != nullptr && g_chatScreenCtor != nullptr);


    if (!g_renderSystemClass_121) {
        jclass rsCls = LoadClassWithLoader(env, gcl, "com.mojang.blaze3d.systems.RenderSystem");
        if (rsCls) {
            g_renderSystemClass_121 = (jclass)env->NewGlobalRef(rsCls);
            g_getProjectionMatrix_121 = env->GetStaticMethodID(rsCls, "getProjectionMatrix", "()Lorg/joml/Matrix4f;");
            if (!g_getProjectionMatrix_121) {
                env->ExceptionClear();
                g_getProjectionMatrix_121 = env->GetStaticMethodID(rsCls, "getProjectionMatrix", "()Lnet/minecraft/class_10366;");
            }
            g_getModelViewMatrix_121 = env->GetStaticMethodID(rsCls, "getModelViewMatrix", "()Lorg/joml/Matrix4f;");
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(rsCls);
        } else {
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    if (!g_matrix4fClass_121) {
        jclass m4Cls = LoadClassWithLoader(env, gcl, "org.joml.Matrix4f");
        if (m4Cls) {
            g_matrix4fClass_121 = (jclass)env->NewGlobalRef(m4Cls);
            g_matrixM00 = env->GetFieldID(m4Cls, "m00", "F"); g_matrixM01 = env->GetFieldID(m4Cls, "m01", "F"); g_matrixM02 = env->GetFieldID(m4Cls, "m02", "F"); g_matrixM03 = env->GetFieldID(m4Cls, "m03", "F");
            g_matrixM10 = env->GetFieldID(m4Cls, "m10", "F"); g_matrixM11 = env->GetFieldID(m4Cls, "m11", "F"); g_matrixM12 = env->GetFieldID(m4Cls, "m12", "F"); g_matrixM13 = env->GetFieldID(m4Cls, "m13", "F");
            g_matrixM20 = env->GetFieldID(m4Cls, "m20", "F"); g_matrixM21 = env->GetFieldID(m4Cls, "m21", "F"); g_matrixM22 = env->GetFieldID(m4Cls, "m22", "F"); g_matrixM23 = env->GetFieldID(m4Cls, "m23", "F");
            g_matrixM30 = env->GetFieldID(m4Cls, "m30", "F"); g_matrixM31 = env->GetFieldID(m4Cls, "m31", "F"); g_matrixM32 = env->GetFieldID(m4Cls, "m32", "F"); g_matrixM33 = env->GetFieldID(m4Cls, "m33", "F");
            if (!g_matrixGetFloatArray_121) {
                g_matrixGetFloatArray_121 = env->GetMethodID(m4Cls, "get", "([F)[F");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_matrixGetFloatArray_121 = nullptr; }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(m4Cls);
        } else {
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // Resolve GameRenderer to get Camera without invoking crashing reflection scans
    g_gameRendererField_121 = FindFieldByType(env, mcClass, "Lnet/minecraft/class_757;");
    if (g_gameRendererField_121) {
        jclass grCls = LoadClassWithLoader(env, gcl, "net.minecraft.class_757");
        if (grCls) {
            g_gameRendererCameraField_121 = env->GetFieldID(grCls, "field_18765", "Lnet/minecraft/class_4184;");
            env->DeleteLocalRef(grCls);
        }
    }

    jclass camCls = LoadClassWithLoader(env, gcl, "net.minecraft.class_4184");
    if (camCls) {
        g_cameraClass_121 = (jclass)env->NewGlobalRef(camCls);
        g_cameraPosF_121 = env->GetFieldID(camCls, "field_18712", "Lnet/minecraft/class_243;");
        g_cameraPitchF_121 = env->GetFieldID(camCls, "field_18714", "F");
        if (env->ExceptionCheck()) env->ExceptionClear();
        g_cameraYawF_121 = env->GetFieldID(camCls, "field_18715", "F");
        env->ExceptionClear();
        env->DeleteLocalRef(camCls);
    }

    jclass vecCls = LoadClassWithLoader(env, gcl, "net.minecraft.class_243");
    if (vecCls) {
        g_vec3dClass_121 = (jclass)env->NewGlobalRef(vecCls);
        g_vec3dX_121 = env->GetFieldID(vecCls, "field_1352", "D");
        g_vec3dY_121 = env->GetFieldID(vecCls, "field_1351", "D");
        g_vec3dZ_121 = env->GetFieldID(vecCls, "field_1350", "D");
        env->ExceptionClear();
        env->DeleteLocalRef(vecCls);
    }


    // Resolution diagnostics
    Log(std::string("Mapped GameRenderer=") + (g_gameRendererField_121 ? "1" : "0") + 
        ", cameraField=" + (g_gameRendererCameraField_121 ? "1" : "0"));
    Log(std::string("Mapped camPos=") + (g_cameraPosF_121 ? "1" : "0") +
        ", camYaw=" + (g_cameraYawF_121 ? "1" : "0") +
        ", camPitch=" + (g_cameraPitchF_121 ? "1" : "0"));
    Log(std::string("Mapped vec3dX=") + (g_vec3dX_121 ? "1" : "0") +
        ", vec3dY=" + (g_vec3dY_121 ? "1" : "0") +
        ", vec3dZ=" + (g_vec3dZ_121 ? "1" : "0"));
    Log(std::string("Mapped RenderSystem=") + (g_renderSystemClass_121 ? "1" : "0") +
        ", getProj=" + (g_getProjectionMatrix_121 ? "1" : "0") +
        ", getModelView=" + (g_getModelViewMatrix_121 ? "1" : "0"));
    Log(std::string("Mapped Matrix4f=") + (g_matrix4fClass_121 ? "1" : "0") +
        ", getFloatArray=" + (g_matrixGetFloatArray_121 ? "1" : "0") +
        ", m00=" + (g_matrixM00 ? "1" : "0") + " m11=" + (g_matrixM11 ? "1" : "0") +
        " m22=" + (g_matrixM22 ? "1" : "0") + " m33=" + (g_matrixM33 ? "1" : "0"));

    Log("Discovery complete: stateJniReady=" + std::string(g_stateJniReady ? "true" : "false")
        + " chatJniReady=" + std::string(g_chatJniReady ? "true" : "false"));

    jvmti->Deallocate((unsigned char*)classes);
    return true;
}

// ===================== CHAT SCREEN HELPERS (cursor) =====================
// Like 1.8.9: open a real MC screen so MC releases mouse natively.
// Falls back to direct GLFW call if JNI not ready.
static void OpenChatScreen() {
    if (g_chatJniReady && g_jvm) {
        JNIEnv* env = nullptr;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env) {
            jobject chatInst = nullptr;
            if (g_chatCtorKind == 0) {
                chatInst = env->NewObject(g_chatScreenClass, g_chatScreenCtor);
            } else if (g_chatCtorKind == 1) {
                jstring empty = env->NewStringUTF("");
                chatInst = env->NewObject(g_chatScreenClass, g_chatScreenCtor, empty);
                env->DeleteLocalRef(empty);
            } else if (g_chatCtorKind == 2) {
                jstring empty = env->NewStringUTF("");
                chatInst = env->NewObject(g_chatScreenClass, g_chatScreenCtor, empty, (jboolean)JNI_FALSE);
                env->DeleteLocalRef(empty);
            }
            
            if (!env->ExceptionCheck() && chatInst) {
                env->CallVoidMethod(g_mcInstance, g_setScreenMethod, chatInst);
                env->ExceptionClear();
                env->DeleteLocalRef(chatInst);
                return; // MC handles cursor natively
            }
            env->ExceptionClear();
        }
    }
    // GLFW fallback (when JNI not ready or failed)
    if (glfwGetCurrentContext_fn && glfwSetInputMode_fn) {
        void* win = glfwGetCurrentContext_fn();
        if (win) {
            glfwSetInputMode_fn(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            // Disable raw mouse motion while our GUI is open to prevent camera drift.
            if (glfwSetInputMode_fn) glfwSetInputMode_fn(win, GLFW_RAW_MOUSE_MOTION, 0);
        }
    }
}
static void CloseChatScreen() {
    if (g_chatJniReady && g_jvm) {
        JNIEnv* env = nullptr;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env) {
            env->CallVoidMethod(g_mcInstance, g_setScreenMethod, nullptr);
            env->ExceptionClear();
        }
    }
    // GLFW safety: ensure Minecraft is put back into mouse-captured mode.
    if (glfwGetCurrentContext_fn && glfwSetInputMode_fn) {
        void* win = glfwGetCurrentContext_fn();
        if (win) {
            // Order matters: disable cursor first, then (later) re-enable raw mouse.
            // Keeping raw mouse OFF briefly reduces post-close flick.
            glfwSetInputMode_fn(win, GLFW_RAW_MOUSE_MOTION, 0);
            glfwSetInputMode_fn(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }
    // Always block WM_INPUT briefly after closing (swallow any buffered deltas).
    DWORD now = GetTickCount();
    // Keep this short: long WM_INPUT blocking feels like cursor/control is stuck.
    g_blockUntilMs = now + 180;
    g_enableRawMouseAtMs = now + 80;
}

// ===================== JNI GAME STATE READING =====================
// Called from ChestScanThreadProc (background thread) every 50ms — NOT from the render thread.
// Moving this here eliminates CallObjectMethod (getSuperclass, getMainHandStack, etc.) from the
// render thread which caused nvoglv64.dll AVX2 crashes (see 1.21_MAPPINGS.md).
static bool IsInWorldNow(JNIEnv* env) {
    if (!env || !g_mcInstance) return false;
    if (!g_worldField_121) {
        jclass mcCls = env->GetObjectClass(g_mcInstance);
        if (mcCls) {
            g_worldField_121 = env->GetFieldID(mcCls, "field_1687", "Lnet/minecraft/class_638;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_worldField_121 = nullptr; }
            if (!g_worldField_121) {
                g_worldField_121 = env->GetFieldID(mcCls, "world", "Lnet/minecraft/class_638;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_worldField_121 = nullptr; }
            }
            env->DeleteLocalRef(mcCls);
        }
    }
    if (!g_worldField_121) return false;
    jobject worldCheck = env->GetObjectField(g_mcInstance, g_worldField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); worldCheck = nullptr; }
    if (!worldCheck) return false;
    env->DeleteLocalRef(worldCheck);
    return true;
}

static void EnsureHudTextFields(JNIEnv* env, jclass mcCls, jobject hudObj) {
    if (!env || !mcCls) return;

    if (!g_inGameHudField_121) {
        g_inGameHudField_121 = env->GetFieldID(mcCls, "field_1705", "Lnet/minecraft/class_329;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_inGameHudField_121 = nullptr; }
        if (!g_inGameHudField_121) {
            g_inGameHudField_121 = env->GetFieldID(mcCls, "inGameHud", "Lnet/minecraft/class_329;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_inGameHudField_121 = nullptr; }
        }
    }

    if (!hudObj) return;
    if (!g_hudTextFields_121.empty()) return;
    DWORD now = GetTickCount();
    if (now - g_lastHudTextProbeMs < 2000) return;
    g_lastHudTextProbeMs = now;

    jclass hudCls = env->GetObjectClass(hudObj);
    if (!hudCls || env->ExceptionCheck()) { env->ExceptionClear(); return; }

    auto addHudTextField = [&](const char* name) {
        jfieldID fid = env->GetFieldID(hudCls, name, "Lnet/minecraft/class_2561;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
        if (!fid) return;
        for (auto existing : g_hudTextFields_121)
            if (existing == fid) return;
        g_hudTextFields_121.push_back(fid);
    };

    const char* knownNames[] = {
        "field_2015",      // common overlay/action bar text field
        "overlayMessage",
        "field_2014",
        "title",
        "subtitle",
        nullptr
    };
    for (int i = 0; knownNames[i]; i++) addHudTextField(knownNames[i]);

    if (g_hudTextFields_121.empty()) {
        jclass cClass = env->FindClass("java/lang/Class");
        jclass cField = env->FindClass("java/lang/reflect/Field");
        jclass cMod = env->FindClass("java/lang/reflect/Modifier");
        if (env->ExceptionCheck()) { env->ExceptionClear(); cClass = nullptr; cField = nullptr; cMod = nullptr; }
        if (cClass && cField && cMod) {
            jmethodID mGetFields = env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
            jmethodID mFName = env->GetMethodID(cField, "getName", "()Ljava/lang/String;");
            jmethodID mFType = env->GetMethodID(cField, "getType", "()Ljava/lang/Class;");
            jmethodID mFMod  = env->GetMethodID(cField, "getModifiers", "()I");
            jmethodID mIsStatic = env->GetStaticMethodID(cMod, "isStatic", "(I)Z");
            if (env->ExceptionCheck()) { env->ExceptionClear(); mGetFields = nullptr; }

            if (mGetFields && mFName && mFType && mFMod && mIsStatic) {
                jobjectArray fields = (jobjectArray)env->CallObjectMethod(hudCls, mGetFields);
                if (env->ExceptionCheck()) { env->ExceptionClear(); fields = nullptr; }
                if (fields) {
                    jsize count = env->GetArrayLength(fields);
                    for (jsize i = 0; i < count; i++) {
                        jobject fld = env->GetObjectArrayElement(fields, i);
                        if (!fld) continue;

                        jint mod = env->CallIntMethod(fld, mFMod);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
                        if (env->CallStaticBooleanMethod(cMod, mIsStatic, mod) == JNI_TRUE) {
                            env->DeleteLocalRef(fld);
                            continue;
                        }

                        jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); ft = nullptr; }
                        std::string typeName = ft ? GetClassNameFromClass(env, ft) : "";
                        if (ft) env->DeleteLocalRef(ft);
                        if (typeName != "net.minecraft.class_2561") { env->DeleteLocalRef(fld); continue; }

                        jstring jfn = (jstring)env->CallObjectMethod(fld, mFName);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); jfn = nullptr; }
                        if (!jfn) { env->DeleteLocalRef(fld); continue; }
                        const char* cfn = env->GetStringUTFChars(jfn, nullptr);
                        std::string fn = cfn ? cfn : "";
                        if (cfn) env->ReleaseStringUTFChars(jfn, cfn);
                        env->DeleteLocalRef(jfn);

                        if (!fn.empty()) addHudTextField(fn.c_str());
                        env->DeleteLocalRef(fld);
                    }
                    env->DeleteLocalRef(fields);
                }
            }
        }
        if (cClass) env->DeleteLocalRef(cClass);
        if (cField) env->DeleteLocalRef(cField);
        if (cMod) env->DeleteLocalRef(cMod);
    }

    env->DeleteLocalRef(hudCls);
}

static void UpdateJniState() {
    if (!g_stateJniReady || !g_jvm || !g_mcInstance || !g_screenField) return;
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) != JNI_OK || !env) return;

    jobject scr = env->GetObjectField(g_mcInstance, g_screenField);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    bool guiOpen = (scr != nullptr);
    bool inWorld = false;
    std::string screenName;
    std::string actionBarText;
    if (scr) {
        // Build a name chain: "thisClass|super1|super2...".
        // This makes C# GUI detection robust even when only base classes are known.
        jclass classClass = env->FindClass("java/lang/Class");
        jmethodID mGetSuper = nullptr;
        if (classClass) {
            mGetSuper = env->GetMethodID(classClass, "getSuperclass", "()Ljava/lang/Class;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); mGetSuper = nullptr; }
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }

        jclass walk = env->GetObjectClass(scr);
        int depth = 0;
        while (walk && depth < 6) {
            std::string cn = GetClassNameFromClass(env, walk);
            if (!cn.empty()) {
                if (!screenName.empty()) screenName += "|";
                screenName += cn;
            }
            jclass superC = nullptr;
            if (mGetSuper) {
                superC = (jclass)env->CallObjectMethod(walk, mGetSuper);
                if (env->ExceptionCheck()) { env->ExceptionClear(); superC = nullptr; }
            }
            // walk is a local ref from GetObjectClass; safe to delete.
            env->DeleteLocalRef(walk);
            walk = superC;
            depth++;
        }
        if (walk) env->DeleteLocalRef(walk);
        if (classClass) env->DeleteLocalRef(classClass);
        env->DeleteLocalRef(scr);
    }

    // Debug: log screen changes (helps diagnose Click-in-Chests matching).
    if (screenName != g_lastLoggedScreen) {
        if (!screenName.empty())
            Log("Screen chain: " + screenName);
        g_lastLoggedScreen = screenName;
        // Any screen change (especially connecting/loading) = world may be transitioning.
        // Pause chunk scanning for 5 seconds to avoid racing with world teardown.
        g_worldTransitionEndMs = GetTickCount() + 5000;
        // Force re-discovery of chunk BE map field on new server
        g_chunkBlockEntitiesMapField_121 = nullptr;
    }

    // ===== lookingAtBlock (crosshair hit = BLOCK) =====
    bool lookingAtBlock = false;

    // Find crosshairTarget / hitResult (field name varies across clients/mappings)
    jclass mcCls = env->GetObjectClass(g_mcInstance);
    if (mcCls) {
        // If a GUI/screen is open, do not report crosshair state (prevents "pause" in menus).
        if (guiOpen) {
            lookingAtBlock = false;
        }

        // If world is null (main menu / not in-game), treat as not looking at a block.
        inWorld = IsInWorldNow(env);
        if (!inWorld) lookingAtBlock = false;

        EnsureCrosshairTargetField(env, mcCls);
        jfieldID hitFld = g_crosshairTargetField_121;
        if (!guiOpen && hitFld) {
            jobject hitObj = env->GetObjectField(g_mcInstance, hitFld);
            if (env->ExceptionCheck()) { env->ExceptionClear(); hitObj = nullptr; }
            if (hitObj) {
                lookingAtBlock = IsHitResultBlock(env, hitObj);
                env->DeleteLocalRef(hitObj);
            }
        }

        // ===== breakingBlock (actual mining) =====
        bool breakingBlock = false;
        jfieldID imFld = env->GetFieldID(mcCls, "field_1761", "Lnet/minecraft/class_636;"); // ClientPlayerInteractionManager
        if (env->ExceptionCheck()) { env->ExceptionClear(); imFld = nullptr; }
        if (imFld) {
            jobject imObj = env->GetObjectField(g_mcInstance, imFld);
            if (imObj && !env->ExceptionCheck()) {
                jclass imCls = env->GetObjectClass(imObj);
                if (imCls) {
                    jfieldID brkFld = env->GetFieldID(imCls, "field_3716", "Z"); // breakingBlock (common Yarn name)
                    if (env->ExceptionCheck()) { env->ExceptionClear(); brkFld = nullptr; }
                    if (brkFld) {
                        breakingBlock = (env->GetBooleanField(imObj, brkFld) == JNI_TRUE);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); breakingBlock = false; }
                    }
                    env->DeleteLocalRef(imCls);
                }
                env->DeleteLocalRef(imObj);
            } else env->ExceptionClear();
        }

        // Fallback: if we can't read the internal breaking flag, derive it from input + target.
        // This is more reliable across clients/mappings and avoids "pause just by looking".
        if (!breakingBlock) {
            SHORT lmb = GetAsyncKeyState(VK_LBUTTON);
            bool lmbDown = (lmb & 0x8000) != 0;
            // Consider it "breaking" when LMB is held and hit result is a block.
            // (Don't depend on cursor mode; and don't require guiOpen==false here because GUI clicks are blocked anyway.)
            if (!guiOpen && lmbDown && lookingAtBlock) breakingBlock = true;
        }

        // ===== holdingBlock (main hand item is BlockItem) =====
        bool holdingBlock = false;
        jfieldID plFld = env->GetFieldID(mcCls, "field_1724", "Lnet/minecraft/class_746;"); // client player
        if (env->ExceptionCheck()) { env->ExceptionClear(); plFld = nullptr; }
        if (plFld) {
            jobject plObj = env->GetObjectField(g_mcInstance, plFld);
            if (plObj && !env->ExceptionCheck()) {
                jclass plCls = env->GetObjectClass(plObj);
                if (plCls) {
                    // Resolve getMainHandStack() lazily.
                    static jmethodID s_getMainHand = nullptr;
                    if (!s_getMainHand) {
                        s_getMainHand = env->GetMethodID(plCls, "getMainHandStack", "()Lnet/minecraft/class_1799;");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); s_getMainHand = nullptr; }
                        if (!s_getMainHand) {
                            s_getMainHand = env->GetMethodID(plCls, "method_6047", "()Lnet/minecraft/class_1799;");
                            if (env->ExceptionCheck()) { env->ExceptionClear(); s_getMainHand = nullptr; }
                        }
                    }

                    jobject stackObj = (s_getMainHand) ? env->CallObjectMethod(plObj, s_getMainHand) : nullptr;
                    if (env->ExceptionCheck()) { env->ExceptionClear(); stackObj = nullptr; }
                    if (stackObj) {
                        jclass stCls = env->GetObjectClass(stackObj);
                        static jmethodID s_getItem = nullptr;
                        if (stCls) {
                            if (!s_getItem) {
                                s_getItem = env->GetMethodID(stCls, "getItem", "()Lnet/minecraft/class_1792;");
                                if (env->ExceptionCheck()) { env->ExceptionClear(); s_getItem = nullptr; }
                                if (!s_getItem) {
                                    s_getItem = env->GetMethodID(stCls, "method_7909", "()Lnet/minecraft/class_1792;");
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); s_getItem = nullptr; }
                                }
                            }
                            if (s_getItem) {
                                jobject itemObj = env->CallObjectMethod(stackObj, s_getItem);
                                if (env->ExceptionCheck()) { env->ExceptionClear(); itemObj = nullptr; }

                                // Resolve BlockItem class lazily.
                                static jclass s_blockItemCls = nullptr;
                                if (!s_blockItemCls && g_gameClassLoader) {
                                    jclass c = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_1747");
                                    if (c) s_blockItemCls = (jclass)env->NewGlobalRef(c);
                                }
                                if (!s_blockItemCls) {
                                    jclass c = env->FindClass("net/minecraft/class_1747");
                                    if (c && !env->ExceptionCheck()) s_blockItemCls = (jclass)env->NewGlobalRef(c);
                                    else env->ExceptionClear();
                                }

                                if (itemObj && s_blockItemCls) {
                                    holdingBlock = (env->IsInstanceOf(itemObj, s_blockItemCls) == JNI_TRUE);
                                }

                                if (itemObj) env->DeleteLocalRef(itemObj);
                            }
                            env->DeleteLocalRef(stCls);
                        }
                        env->DeleteLocalRef(stackObj);
                    }
                    env->DeleteLocalRef(plCls);
                }
                env->DeleteLocalRef(plObj);
            } else env->ExceptionClear();
        }

        if (inWorld) {
            EnsureHudTextFields(env, mcCls, nullptr);
            if (g_inGameHudField_121) {
                jobject hudObj = env->GetObjectField(g_mcInstance, g_inGameHudField_121);
                if (env->ExceptionCheck()) { env->ExceptionClear(); hudObj = nullptr; }
                if (hudObj) {
                    EnsureHudTextFields(env, mcCls, hudObj);
                    for (auto fid : g_hudTextFields_121) {
                        jobject txtObj = env->GetObjectField(hudObj, fid);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); txtObj = nullptr; }
                        if (!txtObj) continue;

                        std::string txt = CallTextToString(env, txtObj);
                        env->DeleteLocalRef(txtObj);
                        if (txt.empty() || txt.find('_') == std::string::npos) continue;
                        if (actionBarText.empty() || txt.size() > actionBarText.size())
                            actionBarText = txt;
                    }
                    env->DeleteLocalRef(hudObj);
                }
            }
        }

        env->DeleteLocalRef(mcCls);

        { LockGuard lk(g_jniStateMtx);
            g_jniScreenName = screenName;
            g_jniActionBar = actionBarText;
            g_jniGuiOpen = guiOpen;
            g_jniInWorld = inWorld;
            g_jniLookingAtBlock = lookingAtBlock;
            g_jniBreakingBlock = breakingBlock;
            g_jniHoldingBlock = holdingBlock;
        }
        return;
    }

    { LockGuard lk(g_jniStateMtx);
        g_jniScreenName = screenName;
        g_jniActionBar = actionBarText;
        g_jniGuiOpen = guiOpen;
        g_jniInWorld = inWorld;
        g_jniLookingAtBlock = lookingAtBlock;
        g_jniBreakingBlock = false;
        g_jniHoldingBlock = false;
    }
}

// ===================== WNDPROC HOOK =====================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // 1.21: external GUI (WPF) toggle. Do not open an in-game clickgui.
    if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) {
        SendCmd("toggleExternalGui");
        return TRUE;
    }

    // Absorb WM_INPUT while GUI is open OR while timer is active.
    // This prevents raw mouse deltas from moving the camera while controlling ImGui.
    if (uMsg == WM_INPUT && (g_ShowMenu || GetTickCount() < g_blockUntilMs)) {
        return DefWindowProcA(hWnd, uMsg, wParam, lParam);
    }

    if (g_imguiInitialized && g_ShowMenu) {
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) return TRUE;
        if (uMsg >= WM_KEYFIRST   && uMsg <= WM_KEYLAST)   return TRUE;
    }

    return CallWindowProc(o_WndProc, hWnd, uMsg, wParam, lParam);
}

// ===================== IMGUI STYLE =====================
static void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 0.0f;
    s.FrameRounding     = 2.0f;
    s.GrabRounding      = 2.0f;
    s.ScrollbarRounding = 0.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowPadding     = ImVec2(0, 0);
    s.FramePadding      = ImVec2(4, 3);
    s.ItemSpacing       = ImVec2(6, 5);
    s.GrabMinSize       = 8.0f;

    s.Colors[ImGuiCol_WindowBg]         = ImVec4(0.10f,  0.10f,  0.11f,  0.98f);
    s.Colors[ImGuiCol_ChildBg]          = ImVec4(0.084f, 0.084f, 0.090f, 0.98f);
    s.Colors[ImGuiCol_Border]           = ImVec4(0.17f,  0.17f,  0.19f,  1.0f);
    s.Colors[ImGuiCol_Text]             = ImVec4(0.93f,  0.94f,  0.96f,  1.0f);
    s.Colors[ImGuiCol_TextDisabled]     = ImVec4(0.45f,  0.46f,  0.50f,  1.0f);
    s.Colors[ImGuiCol_Header]           = ImVec4(0.36f,  0.24f,  0.52f,  0.70f);
    s.Colors[ImGuiCol_HeaderHovered]    = ImVec4(0.44f,  0.30f,  0.64f,  0.86f);
    s.Colors[ImGuiCol_HeaderActive]     = ImVec4(0.52f,  0.34f,  0.74f,  1.00f);
    s.Colors[ImGuiCol_FrameBg]          = ImVec4(0.15f,  0.15f,  0.20f,  1.00f);
    s.Colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.20f,  0.20f,  0.28f,  1.00f);
    s.Colors[ImGuiCol_FrameBgActive]    = ImVec4(0.25f,  0.25f,  0.34f,  1.00f);
    s.Colors[ImGuiCol_SliderGrab]       = ImVec4(0.65f,  0.45f,  0.92f,  1.00f);
    s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.73f,  0.55f,  0.98f,  1.00f);
    s.Colors[ImGuiCol_CheckMark]        = ImVec4(0.86f,  0.74f,  1.00f,  1.00f);
    s.Colors[ImGuiCol_Button]           = ImVec4(0.22f,  0.12f,  0.14f,  0.90f);
    s.Colors[ImGuiCol_ButtonHovered]    = ImVec4(0.65f,  0.12f,  0.18f,  1.00f);
    s.Colors[ImGuiCol_ButtonActive]     = ImVec4(0.80f,  0.15f,  0.20f,  1.00f);
    s.Colors[ImGuiCol_Separator]        = ImVec4(0.22f,  0.22f,  0.25f,  1.0f);
    s.Colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.08f,  0.08f,  0.09f,  1.0f);
    s.Colors[ImGuiCol_ScrollbarGrab]    = ImVec4(0.25f,  0.25f,  0.30f,  1.0f);
    s.Colors[ImGuiCol_PopupBg]          = ImVec4(0.08f,  0.08f,  0.09f,  0.98f);
}

// ===================== RENDER CLICKGUI =====================
static void RenderClickGUI() {
    ImGuiIO& io = ImGui::GetIO();
    ApplyStyle();

    // Semi-transparent dark overlay over the entire screen (like 1.8.9)
    ImDrawList* bgDl = ImGui::GetBackgroundDrawList();
    bgDl->AddRectFilled(ImVec2(0,0), io.DisplaySize, IM_COL32(0, 0, 0, 140));

    // Window size fixed at 640x460, centered on first show
    const float WIN_W = 640.0f, WIN_H = 460.0f;
    ImGui::SetNextWindowSize(ImVec2(WIN_W, WIN_H), ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2((io.DisplaySize.x - WIN_W) * 0.5f,
               (io.DisplaySize.y - WIN_H) * 0.5f),
        ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = true;
    ImGui::Begin("##lc_gui", &open,
        ImGuiWindowFlags_NoTitleBar    |
        ImGuiWindowFlags_NoCollapse    |
        ImGuiWindowFlags_NoResize      |
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    if (!open) {
        // X button — close GUI
        g_ShowMenu = false;
        CloseChatScreen();
        ImGui::End();
        return;
    }

    ImVec2 wPos = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- Header background (slightly different shade from window bg) ----
    const float HEADER_H = 34.0f;
    dl->AddRectFilled(wPos,
                      ImVec2(wPos.x + WIN_W, wPos.y + HEADER_H),
                      IM_COL32(22, 22, 24, 252));
    dl->AddLine(ImVec2(wPos.x, wPos.y + HEADER_H),
                ImVec2(wPos.x + WIN_W, wPos.y + HEADER_H),
                IM_COL32(43, 43, 47, 255));
    // Left accent bar (2px teal)
    dl->AddRectFilled(wPos, ImVec2(wPos.x + 2, wPos.y + WIN_H),
                      IM_COL32(168, 219, 255, 255));

    // Title
    ImGui::SetCursorPos(ImVec2(12, 8));
    ImGui::Text("LegoClicker");
    ImGui::SameLine();
    ImGui::SetCursorPosX(130);
    ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.50f, 1.0f), "- Internal ClickGUI");

    // X close button
    ImGui::SetCursorPos(ImVec2(WIN_W - 27, 9));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
    if (ImGui::Button("X##close", ImVec2(18, 16))) {
        g_ShowMenu = false;
        CloseChatScreen();
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();

    // ---- Three-column layout ----
    // accent(2) | sidebar(120) | gap(2) | modules(220) | gap(2) | settings(294)
    // Total: 2+120+2+220+2+294 = 640 ✓
    static int selCategory = 0;
    static int selModule   = 0;

    const float PANEL_Y  = HEADER_H;
    const float PANEL_H  = WIN_H - HEADER_H;
    const float ACCENT_W = 2.0f;
    const float GAP      = 2.0f;
    const float SIDE_W   = 120.0f;
    const float MOD_W    = 220.0f;
    const float SET_W    = WIN_W - ACCENT_W - SIDE_W - GAP*2 - MOD_W - GAP; // 294

    // Get config snapshot
    Config cfg;
    { LockGuard lk(g_configMutex); cfg = g_config; }

    // --- Sidebar (Categories) ---
    ImGui::SetCursorPos(ImVec2(ACCENT_W, PANEL_Y));
    ImGui::BeginChild("##sidebar", ImVec2(SIDE_W, PANEL_H), false);
    ImGui::SetCursorPosY(8);
    auto catStyle = [](bool sel) {
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.64f, 1.0f, 1.0f));
        else     ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.82f, 0.85f, 1.0f));
    };
    catStyle(selCategory == 0);
    if (ImGui::Selectable("  Combat", selCategory == 0, 0, ImVec2(SIDE_W-4, 28))) {
        if (selCategory != 0) { selCategory = 0; selModule = 0; }
    }
    ImGui::PopStyleColor();
    catStyle(selCategory == 1);
    if (ImGui::Selectable("  Render", selCategory == 1, 0, ImVec2(SIDE_W-4, 28))) {
        if (selCategory != 1) { selCategory = 1; selModule = 0; }
    }
    ImGui::PopStyleColor();
    ImGui::EndChild();

    // --- Modules ---
    float modX = ACCENT_W + SIDE_W + GAP;
    ImGui::SetCursorPos(ImVec2(modX, PANEL_Y));
    ImGui::BeginChild("##modules", ImVec2(MOD_W, PANEL_H), false);

    // Helper: draw a module row with a checkbox toggle and selectable label.
    // enabled=true -> label is teal/active, false -> dim
    auto ModuleRow = [&](const char* id, const char* label, bool enabled,
                         int modIdx, const char* toggleCmd) -> bool {
        bool en = enabled;
        ImGui::PushID(id);
        if (ImGui::Checkbox("##tog", &en)) SendCmd(toggleCmd);
        ImGui::PopID();
        ImGui::SameLine(0, 6);
        bool sel = (selModule == modIdx);
        if (enabled)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.90f, 0.70f, 1.0f));
        else
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.82f, 0.85f, 1.0f));
        if (ImGui::Selectable(label, &sel, 0, ImVec2(MOD_W - 50, 22)))
            selModule = modIdx;
        ImGui::PopStyleColor();
        return false;
    };

    if (selCategory == 0) {
        // Combat
        ModuleRow("ac",   "AutoClicker",   cfg.armed,      0, "toggleArmed");
        ModuleRow("aa",   "Aim Assist",    cfg.aimAssist,  1, "toggleAimAssist");
    } else {
        // Render
        ModuleRow("ce",  "Chest ESP",      cfg.chestEsp,   0, "toggleChestEsp");
        ModuleRow("nt",  "Nametags",       cfg.nametags,   1, "toggleNametags");
        ModuleRow("cp",  "Closest Player", cfg.closestPlayer, 2, "toggleClosestPlayerInfo");
        ModuleRow("gtb", "GTB Helper",     cfg.gtbHelper,  3, "toggleGtbHelper");
    }
    ImGui::EndChild();

    // --- Settings ---
    float setX = modX + MOD_W + GAP;
    ImGui::SetCursorPos(ImVec2(setX, PANEL_Y));
    ImGui::BeginChild("##settings", ImVec2(SET_W, PANEL_H), false);
    ImGui::SetCursorPos(ImVec2(8, 6));
    ImGui::TextColored(ImVec4(0.78f, 0.64f, 1.0f, 1.0f), "Settings");
    ImGui::Separator();
    ImGui::Spacing();

    if (selCategory == 0 && selModule == 0) {
        // ---- AutoClicker settings ----
        ImGui::TextColored(ImVec4(0.55f, 0.90f, 0.70f, 1.0f),
                           cfg.armed ? (cfg.clicking ? "CLICKING" : "ARMED") : "DISABLED");
        ImGui::Spacing();

        int minCps = (int)cfg.minCPS;
        if (ImGui::SliderInt("Min CPS", &minCps, 1, 20))
            SendCmdFloat("setMinCPS", (float)minCps);

        int maxCps = (int)cfg.maxCPS;
        if (ImGui::SliderInt("Max CPS", &maxCps, 1, 20))
            SendCmdFloat("setMaxCPS", (float)maxCps);

        ImGui::Spacing();

        bool jitter = cfg.jitter;
        if (ImGui::Checkbox("Jitter", &jitter)) SendCmd("toggleJitter");

        bool cic = cfg.clickInChests;
        if (ImGui::Checkbox("Click in Chests", &cic)) SendCmd("toggleClickInChests");

        bool brk = cfg.breakBlocks;
        if (ImGui::Checkbox("Pause when breaking", &brk)) SendCmd("toggleBreakBlocks");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.78f, 0.64f, 1.0f, 1.0f), "Right Click");

        bool rc = cfg.rightClick;
        if (ImGui::Checkbox("Enable Right Click", &rc)) SendCmd("toggleRight");

        if (cfg.rightClick) {
            int rMin = (int)cfg.rightMinCPS;
            if (ImGui::SliderInt("R.Min CPS", &rMin, 1, 20))
                SendCmdFloat("setRightMinCPS", (float)rMin);

            int rMax = (int)cfg.rightMaxCPS;
            if (ImGui::SliderInt("R.Max CPS", &rMax, 1, 20))
                SendCmdFloat("setRightMaxCPS", (float)rMax);

            bool rbo = cfg.rightBlockOnly;
            if (ImGui::Checkbox("Block Only", &rbo)) SendCmd("toggleRightBlockOnly");
        }

    } else if (selCategory == 0 && selModule == 1) {
        bool aa = cfg.aimAssist;
        if (ImGui::Checkbox("Enable Aim Assist", &aa)) SendCmd("toggleAimAssist");
        ImGui::TextDisabled("Adjust FOV/range/strength in Control Center.");

    } else if (selCategory == 1 && selModule == 0) {
        ImGui::TextDisabled("No extra settings.");

    } else if (selCategory == 1 && selModule == 1) {
        bool health = cfg.nametagHealth;
        if (ImGui::Checkbox("Show Health", &health)) SendCmd("toggleNametagHealth");
        bool armor = cfg.nametagArmor;
        if (ImGui::Checkbox("Show Armor", &armor)) SendCmd("toggleNametagArmor");

    } else if (selCategory == 1 && selModule == 2) {
        ImGui::TextDisabled("No extra settings.");
    } else if (selCategory == 1 && selModule == 3) {
        ImGui::TextDisabled("In-game hint panel only.");
    }

    ImGui::EndChild();
    ImGui::End();
}

// ===================== SCREEN DETECTION =====================
// Combine GLFW cursor polling (fast) with JNI screen name (accurate).
static void UpdateRealGuiState() {
    // JNI state (screen name) is already updated in UpdateJniState() each frame.
    // g_realGuiOpen = GLFW cursor visible AND not our own GUI.
    if (!glfwGetCurrentContext_fn || !glfwGetInputMode_fn) {
        g_realGuiOpen = false;
        return;
    }
    void* win = glfwGetCurrentContext_fn();
    if (!win) { g_realGuiOpen = false; return; }
    int mode = glfwGetInputMode_fn(win, GLFW_CURSOR);
    // Only report realGuiOpen when cursor is NORMAL and it's NOT our own GUI
    // and the JNI screen is NOT the ChatScreen we opened for cursor unlock
    if (mode == GLFW_CURSOR_NORMAL && !g_ShowMenu) {
        std::string sn;
        { LockGuard lk(g_jniStateMtx); sn = g_jniScreenName; }
        // "ChatScreen" is our own cursor-unlock screen, not a real Minecraft GUI
        g_realGuiOpen = !sn.empty() && sn != "ChatScreen";
    } else {
        g_realGuiOpen = false;
    }
}

// ===================== CHEST SCAN BACKGROUND THREAD =====================
// UpdateChestList calls Java methods (CallObjectMethod) which MUST NOT run on the render thread
// (see 1.21_MAPPINGS.md critical warning). This thread attaches its own JNI env and runs the
// chest scan independently, protecting g_chestList with g_chestListMutex.

// Reads camera position, yaw, pitch, and Lunar projection/view matrices on the BACKGROUND thread.
// Stores the result in g_bgCamState (under g_bgCamMutex) so the render thread can use it with
// zero JNI calls.
static void ReadCameraState(JNIEnv* env) {
    if (!g_mcInstance || !g_gameRendererField_121 || !g_gameRendererCameraField_121) return;

    BgCamState cs = {};

    jobject gr = env->GetObjectField(g_mcInstance, g_gameRendererField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); gr = nullptr; }
    if (gr) {
        jobject camera = env->GetObjectField(gr, g_gameRendererCameraField_121);
        if (!env->ExceptionCheck() && camera) {
            // Yaw/pitch from Camera fields (if mapped)
            if (g_cameraYawF_121)   cs.yaw   = env->GetFloatField(camera, g_cameraYawF_121);
            if (g_cameraPitchF_121) cs.pitch = env->GetFloatField(camera, g_cameraPitchF_121);
            env->ExceptionClear();

            // Fallback: yaw/pitch from player entity (CallFloatMethod is safe on bg thread)
            if (cs.yaw == 0.0f && cs.pitch == 0.0f && g_playerField_121) {
                jobject playerObj = env->GetObjectField(g_mcInstance, g_playerField_121);
                if (!env->ExceptionCheck() && playerObj) {
                    if (!g_getYaw_121 || !g_getPitch_121) EnsureEntityMethods(env, playerObj);
                    if (g_getYaw_121)   cs.yaw   = env->CallFloatMethod(playerObj, g_getYaw_121);
                    if (g_getPitch_121) cs.pitch = env->CallFloatMethod(playerObj, g_getPitch_121);
                    env->ExceptionClear();
                    env->DeleteLocalRef(playerObj);
                } else env->ExceptionClear();
            }

            // Camera position (Vec3d)
            if (g_cameraPosF_121) {
                jobject vec = env->GetObjectField(camera, g_cameraPosF_121);
                if (!env->ExceptionCheck() && vec) {
                    if (g_vec3dX_121) cs.camX = env->GetDoubleField(vec, g_vec3dX_121);
                    if (g_vec3dY_121) cs.camY = env->GetDoubleField(vec, g_vec3dY_121);
                    if (g_vec3dZ_121) cs.camZ = env->GetDoubleField(vec, g_vec3dZ_121);
                    cs.camFound = true;
                    env->DeleteLocalRef(vec);
                } else env->ExceptionClear();
            }
            env->DeleteLocalRef(camera);
        } else env->ExceptionClear();

        // Matrices: cache Lunar saved-field IDs on first call
        if (!g_lunarProjField_121 || !g_lunarViewField_121) {
            jclass grCls = env->GetObjectClass(gr);
            if (grCls) {
                g_lunarProjField_121 = env->GetFieldID(grCls, "lunar$savedProjection$v1_19_3", "Lorg/joml/Matrix4f;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_lunarProjField_121 = nullptr; }
                g_lunarViewField_121 = env->GetFieldID(grCls, "lunar$savedModelView$v1_19_3", "Lorg/joml/Matrix4f;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_lunarViewField_121 = nullptr; }
                env->DeleteLocalRef(grCls);
            }
        }
        if (g_lunarProjField_121 && g_lunarViewField_121) {
            jobject pO = env->GetObjectField(gr, g_lunarProjField_121);
            jobject vO = env->GetObjectField(gr, g_lunarViewField_121);
            if (pO && vO) {
                if (ReadMatrix4f(env, pO, cs.proj) && ReadMatrix4f(env, vO, cs.view))
                    cs.matsOk = true;
            }
            if (pO) env->DeleteLocalRef(pO);
            if (vO) env->DeleteLocalRef(vO);
        }
        env->DeleteLocalRef(gr);
    }

    // Final fallback: Camera.pos Vec3d was null (common in Lunar Client).
    // Use player entity position + eye height so camFound is valid for ESP.
    if (!cs.camFound && g_playerField_121) {
        jobject pFb = env->GetObjectField(g_mcInstance, g_playerField_121);
        if (!env->ExceptionCheck() && pFb) {
            if (!g_entityPosField_121) EnsureEntityMethods(env, pFb);
            if (g_entityPosField_121) {
                jobject posVec = env->GetObjectField(pFb, g_entityPosField_121);
                if (!env->ExceptionCheck() && posVec) {
                    if (g_vec3dX_121) cs.camX = env->GetDoubleField(posVec, g_vec3dX_121);
                    if (g_vec3dY_121) cs.camY = env->GetDoubleField(posVec, g_vec3dY_121) + 1.62;
                    if (g_vec3dZ_121) cs.camZ = env->GetDoubleField(posVec, g_vec3dZ_121);
                    cs.camFound = true;
                    env->DeleteLocalRef(posVec);
                } else env->ExceptionClear();
            }
            env->DeleteLocalRef(pFb);
        } else env->ExceptionClear();
    }

    static bool s_camDiag = false;
    if (!s_camDiag && cs.camFound) {
        s_camDiag = true;
        Log(std::string("CamState: camFound=1 pos=(") + std::to_string(cs.camX) + ","
            + std::to_string(cs.camY) + "," + std::to_string(cs.camZ)
            + ") yaw=" + std::to_string(cs.yaw) + " pitch=" + std::to_string(cs.pitch)
            + " matsOk=" + (cs.matsOk ? "1" : "0"));
    }

    { LockGuard lk(g_bgCamMutex); g_bgCamState = cs; }
}

static DWORD WINAPI ChestScanThreadProc(LPVOID) {
    JNIEnv* env = nullptr;
    if (!g_jvm || g_jvm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK) return 1;
    while (g_running) {
        Config cfg;
        { LockGuard lk(g_configMutex); cfg = g_config; }
        if (g_stateJniReady) {
            // All CallObjectMethod work runs here — never on the render thread.
            ReadCameraState(env);   // camera pos/yaw/pitch/matrices → g_bgCamState
            UpdateJniState(); // screen detection, holdingBlock, lookingAtBlock
            bool inWorldNow = IsInWorldNow(env);
            { LockGuard lk(g_jniStateMtx); g_jniInWorld = inWorldNow; }
            if (inWorldNow) {
                if (cfg.closestPlayer)
                    UpdateClosestPlayerOverlay(env);
                if (cfg.nametags || cfg.closestPlayer || cfg.aimAssist)
                    UpdatePlayerListOverlay(env);
                if (cfg.chestEsp)
                    UpdateChestList(env);
            } else {
                { LockGuard lk(g_playerListMutex); g_playerList.clear(); }
                { LockGuard lk2(g_chestListMutex); g_chestList.clear(); }
                { LockGuard lk3(g_bgCamMutex); g_bgCamState = BgCamState(); }
            }
        } else {
            { LockGuard lk(g_playerListMutex); g_playerList.clear(); }
            { LockGuard lk2(g_chestListMutex); g_chestList.clear(); }
            { LockGuard lk3(g_bgCamMutex); g_bgCamState = BgCamState(); }
        }
        Sleep(50); // each function has its own internal throttle
    }
    g_jvm->DetachCurrentThread();
    return 0;
}

// ===================== HOOKED SwapBuffers =====================
// Frame counter: skip first few frames after GL backend init to let driver stabilize.
static int g_imguiWarmupFrames = 0;
// Two-phase init: phase 1 = ImGui context + Win32 (no GL), phase 2 = GL backend (deferred).
static bool g_imguiPhase1Done = false;

BOOL WINAPI hwglSwapBuffers(HDC hDc) {
    if (!hDc) return o_wglSwapBuffers(hDc);

    // If there is no current GL context, do not touch OpenGL/ImGui.
    HGLRC currentRc = wglGetCurrentContext();
    if (!currentRc) return o_wglSwapBuffers(hDc);

    HWND window = WindowFromDC(hDc);
    if (!window || !IsWindow(window)) return o_wglSwapBuffers(hDc);

    // Only run on the main GLFW game window
    char cls[256] = {};
    if (!GetClassNameA(window, cls, sizeof(cls)-1))
        return o_wglSwapBuffers(hDc);
    if (strcmp(cls, "GLFW30") != 0)
        return o_wglSwapBuffers(hDc);

    // ── Phase 1: ImGui context + Win32 backend (NO OpenGL calls at all) ──
    // Runs on the very first GLFW swap call.  We must not touch GL here so the
    // NVIDIA driver's internal swap-chain state is completely undisturbed.
    if (!g_imguiPhase1Done) {
        g_hwnd = window;
        g_imguiGlrc = currentRc;

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        // DPI-aware font config (no GL – just configures atlas data)
        float dpiScale = 1.0f;
        {
            UINT dpi = 96;
            HMODULE hUser32 = GetModuleHandleA("user32.dll");
            if (hUser32) {
                typedef UINT (WINAPI* FnGetDpiForWindow)(HWND);
                auto fn = (FnGetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
                if (fn) dpi = fn(g_hwnd);
            }
            dpiScale = (dpi > 0) ? ((float)dpi / 96.0f) : 1.0f;
            if (dpiScale < 0.75f) dpiScale = 0.75f;
            if (dpiScale > 2.5f) dpiScale = 2.5f;
        }
        io.Fonts->Clear();
        ImFontConfig fontCfg;
        fontCfg.RasterizerDensity = 1.0f;
        fontCfg.SizePixels = 16.0f * dpiScale;
        fontCfg.OversampleH = 3;
        fontCfg.OversampleV = 2;
        fontCfg.PixelSnapH = true;
        ImFont* loadedFont = nullptr;
        std::string bridgeDir = GetBridgeDir();
        std::vector<std::string> fontCandidates = {
            bridgeDir + "\\minecraftia.ttf",
            bridgeDir + "\\Minecraftia.ttf",
            bridgeDir + "\\Data\\minecraftia.ttf",
            bridgeDir + "\\Data\\Minecraftia.ttf",
            "C:\\Windows\\Fonts\\minecraftia.ttf",
            "C:\\Windows\\Fonts\\Minecraftia.ttf"
        };

        for (const auto& fontPath : fontCandidates) {
            if (!FileExistsA(fontPath)) continue;
            if (!IsLikelyFontBinary(fontPath)) {
                Log("Skipping invalid font file (not binary TTF/OTF): " + fontPath);
                continue;
            }
            loadedFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontCfg.SizePixels, &fontCfg);
            if (loadedFont) {
                Log("Loaded ImGui font: " + fontPath);
                break;
            }
        }

        if (!loadedFont) {
            io.Fonts->AddFontDefault(&fontCfg);
            Log("Minecraftia not found, using default ImGui font.");
        }
        io.FontGlobalScale = 1.0f;
        ImGuiStyle& st = ImGui::GetStyle();
        st.ScaleAllSizes(dpiScale);

        ImGui_ImplWin32_InitForOpenGL(g_hwnd);
        o_WndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

        g_imguiPhase1Done = true;
        g_imguiWarmupFrames = 3; // let 3 clean frames pass before touching GL
        Log("ImGui phase-1 done (context + Win32). GL backend deferred.");
        return o_wglSwapBuffers(hDc);
    }

    // Warmup: let clean frames pass between phases / after GL init.
    if (g_imguiWarmupFrames > 0) {
        g_imguiWarmupFrames--;
        return o_wglSwapBuffers(hDc);
    }

    // If the GL context was recreated, defer a safe OpenGL backend reset.
    // Do NOT shutdown/reinit immediately: doing glDelete* on stale IDs in a different
    // context can corrupt Minecraft/Lunar rendering and may trigger nvoglv64.dll crashes.
    if (g_imguiInitialized && g_imguiGlBackendReady && g_imguiGlrc && currentRc != g_imguiGlrc) {
        Log("OpenGL context changed; scheduling ImGui OpenGL backend reset.");
        g_imguiPendingBackendReset = true;
        g_imguiPendingGlrc = currentRc;
        g_imguiWarmupFrames = 3;
        return o_wglSwapBuffers(hDc);
    }

    // Execute the pending reset on a clean frame, then defer actual Init to a later frame.
    if (g_imguiPendingBackendReset) {
        Log("ImGui: executing deferred OpenGL backend shutdown (skip GL deletes)");
        ImGui_ImplOpenGL3_SetSkipGLDeletes(true);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplOpenGL3_SetSkipGLDeletes(false);

        g_imguiGlBackendReady = false;
        g_imguiInitialized = false;
        g_imguiGlrc = g_imguiPendingGlrc;
        g_imguiPendingBackendReset = false;
        g_imguiWarmupFrames = 3;
        return o_wglSwapBuffers(hDc);
    }

    // ── Phase 2: OpenGL 3.3 backend init (compiles shaders, uploads font atlas) ──
    // Runs on a SEPARATE frame well after phase 1.  We return immediately after
    // so the driver only sees our GL init work, with no rendering or ImGui draw
    // calls mixed in.
    if (!g_imguiInitialized) {
        static bool glModernLoadedLogged = false;
        if (!glModernLoadedLogged) {
            bool ok = LoadModernOpenGL();
            Log(std::string("Modern OpenGL loader: ") + (ok ? "ok" : "FAILED"));
            glModernLoadedLogged = true;
        }

        // ImGui init can touch GL bindings. Backup/restore the critical state so we don't
        // leave Minecraft/Lunar in a weird state for the next frame.
        GLint last_program = 0;
        GLint last_active_texture = 0;
        GLint last_texture_2d = 0;
        GLint last_array_buffer = 0;
        GLint last_element_array_buffer = 0;
        GLint last_vertex_array = 0;
    #ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
        GLint last_pixel_unpack_buffer = 0;
    #endif

        glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture_2d);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer);
    #ifdef GL_VERTEX_ARRAY_BINDING
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
    #endif
    #ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &last_pixel_unpack_buffer);
    #endif

        ImGui_ImplOpenGL3_Init("#version 330 core");

        if (glUseProgram_)
            glUseProgram_((GLuint)last_program);
        if (glActiveTexture_)
            glActiveTexture_((GLenum)last_active_texture);
        glBindTexture(GL_TEXTURE_2D, (GLuint)last_texture_2d);
        if (glBindBuffer_) {
            glBindBuffer_(GL_ARRAY_BUFFER, (GLuint)last_array_buffer);
            glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, (GLuint)last_element_array_buffer);
        }
    #ifdef GL_VERTEX_ARRAY_BINDING
        if (glBindVertexArray_)
            glBindVertexArray_((GLuint)last_vertex_array);
    #endif
    #ifdef GL_PIXEL_UNPACK_BUFFER_BINDING
        if (glBindBuffer_)
            glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, (GLuint)last_pixel_unpack_buffer);
    #endif

        g_imguiGlBackendReady = true;
        g_imguiInitialized = true;
        g_imguiGlrc = currentRc;
        g_imguiWarmupFrames = 3; // 3 more clean frames before rendering
        Log("ImGui phase-2 done (OpenGL 3.3 core GL backend). Ready to render.");
        return o_wglSwapBuffers(hDc);
    }

    // JNI game state (screen name, holdingBlock etc.) is updated by the background thread.
    // Start chest scan background thread once as soon as the JVM is available.
    // The thread calls UpdateChestList which uses CallObjectMethod — forbidden on the render thread.
    {
        static bool s_chestThreadStarted = false;
        if (g_jvm && !s_chestThreadStarted) {
            s_chestThreadStarted = true;
            CreateThread(nullptr, 0, ChestScanThreadProc, nullptr, 0, nullptr);
        }
    }

    // Update closest player / nametags modules — MOVED to background thread (ChestScanThreadProc).
    // Calling CallObjectMethod on the render thread causes nvoglv64.dll crashes (see 1.21_MAPPINGS.md).

    // Update inventory detection
    UpdateRealGuiState();

    // Render ImGui
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 1.21: no in-game clickgui (external WPF window is used instead)

    // Render overlay text for closest player (when enabled and menu closed)
    {
        Config cfg;
        { LockGuard lk(g_configMutex); cfg = g_config; }
        bool inWorld = false;
        { LockGuard lk(g_jniStateMtx); inWorld = g_jniInWorld; }
        if (!g_ShowMenu && inWorld) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImGuiIO& io = ImGui::GetIO();
            BgCamState cpCamState;
            { LockGuard lk(g_bgCamMutex); cpCamState = g_bgCamState; }

            // Snapshot player list under lock (background thread updates it concurrently).
            std::vector<PlayerData121> playerSnap;
            { LockGuard lk(g_playerListMutex); playerSnap = g_playerList; }

            // ── Closest Player: styled HUD box above hotbar ───
            if (cfg.closestPlayer && !playerSnap.empty()) {
                const auto& cp = playerSnap[0];
                const float fontSz   = ImGui::GetFontSize();
                const float smallSz  = std::floor(fontSz * 0.82f);
                const float padX     = 10.0f;
                const float padY     = 6.0f;
                const float boxW     = 220.0f;
                const float hpBarH   = 4.0f;
                const float gapRow   = 3.0f;

                // Build text rows
                char nameRow[64];
                const float PI = 3.14159265f;
                float yaw = cpCamState.yaw * (PI / 180.0f);
                float fwdX = -sinf(yaw), fwdZ = cosf(yaw);
                float rightX = fwdZ, rightZ = -fwdX;
                double toX = cp.ex - cpCamState.camX;
                double toZ = cp.ez - cpCamState.camZ;
                double relFwd = toX * fwdX + toZ * fwdZ;
                double relRight = toX * rightX + toZ * rightZ;
                char dirArrow = '^';
                if (fabs(relRight) > fabs(relFwd)) dirArrow = (relRight >= 0.0) ? '>' : '<';
                else dirArrow = (relFwd >= 0.0) ? '^' : 'v';
                snprintf(nameRow, sizeof(nameRow), "%c %s  %.0fm", dirArrow, cp.name.c_str(), cp.dist);

                // Stats row
                std::string statsRow;
                if (cfg.nametagHealth) {
                    char hb[24]; snprintf(hb, sizeof(hb), "HP %.0f/20", cp.hp);
                    statsRow += hb;
                }
                if (cfg.nametagArmor && cp.armor > 0) {
                    if (!statsRow.empty()) statsRow += "  |  ";
                    char ab[16]; snprintf(ab, sizeof(ab), "ARM %d", cp.armor);
                    statsRow += ab;
                }
                if (!cp.heldItem.empty()) {
                    if (!statsRow.empty()) statsRow += "  ";
                    statsRow += cp.heldItem;
                }

                // Layout
                ImVec2 nameSz  = ImGui::CalcTextSize(nameRow);
                ImVec2 statsSz = statsRow.empty() ? ImVec2{0,0} : ImGui::CalcTextSize(statsRow.c_str());

                float contentW = std::max({boxW, nameSz.x + padX * 2, statsSz.x + padX * 2});
                float contentH = padY + fontSz + gapRow;
                contentH += hpBarH + gapRow;
                if (!statsRow.empty()) contentH += smallSz + gapRow;
                contentH += padY;

                float cx = io.DisplaySize.x * 0.5f;
                float by = io.DisplaySize.y - 120.0f;   // 120px from bottom = above hotbar + labels

                float rx = std::floor(cx - contentW * 0.5f);
                float ry = std::floor(by - contentH);

                ImVec2 pMin(rx, ry);
                ImVec2 pMax(rx + contentW, ry + contentH);

                // Background + outline
                fg->AddRectFilled(pMin, pMax, IM_COL32(10, 10, 18, 210), 6.0f);
                fg->AddRect(pMin, pMax, IM_COL32(80, 120, 255, 120), 6.0f, 0, 1.0f);

                float curY = ry + padY;

                // Name + distance row (white)
                float ntx = std::floor(cx - nameSz.x * 0.5f);
                fg->AddText(ImVec2(ntx + 1, curY + 1), IM_COL32(0,0,0,160), nameRow);
                fg->AddText(ImVec2(ntx, curY), IM_COL32(255, 255, 255, 240), nameRow);
                curY += fontSz + gapRow;

                // HP bar
                float hpPct = std::max(0.0f, std::min((float)cp.hp / 20.0f, 1.0f));
                float barX0 = rx + padX;
                float barX1 = rx + contentW - padX;
                float barFill = barX0 + (barX1 - barX0) * hpPct;
                ImU32 barCol = IM_COL32((int)(255*(1.0f-hpPct)), (int)(220*hpPct+35), 60, 255);
                fg->AddRectFilled(ImVec2(barX0, curY), ImVec2(barX1, curY + hpBarH), IM_COL32(40,40,40,200), 2.0f);
                fg->AddRectFilled(ImVec2(barX0, curY), ImVec2(barFill, curY + hpBarH), barCol, 2.0f);
                curY += hpBarH + gapRow;

                // Stats row (smaller, soft colour)
                if (!statsRow.empty()) {
                    float stx = std::floor(cx - statsSz.x * 0.5f);
                    fg->AddText(ImGui::GetFont(), smallSz, ImVec2(stx + 1, curY + 1), IM_COL32(0,0,0,160), statsRow.c_str());
                    ImU32 sCol = cp.hp <= 6.0f ? IM_COL32(255, 100, 100, 240) : IM_COL32(160, 200, 255, 230);
                    fg->AddText(ImGui::GetFont(), smallSz, ImVec2(stx, curY), sCol, statsRow.c_str());
                }
            }

            // ── Shared camera state — populated by background thread, ZERO JNI here ──
            LegoVec3 sharedCam = {0,0,0};
            float sharedYaw = 0.0f, sharedPitch = 0.0f;
            bool sharedCamFound = false;
            Matrix4x4 sharedProj = {}, sharedView = {};
            bool sharedMatsOk = false;

            if (cfg.nametags || cfg.chestEsp) {
                BgCamState cs;
                { LockGuard lk(g_bgCamMutex); cs = g_bgCamState; }
                sharedCam      = { cs.camX, cs.camY, cs.camZ };
                sharedYaw      = cs.yaw;
                sharedPitch    = cs.pitch;
                sharedCamFound = cs.camFound;
                sharedProj     = cs.proj;
                sharedView     = cs.view;
                sharedMatsOk   = cs.matsOk;
            }

            const DWORD overlayNowMs = GetTickCount();
            const float overlaySmoothAlpha = (std::max)(0.15f, (std::min)(0.65f, io.DeltaTime * 14.0f));
            struct SmoothedPoint { std::string key; float sx, sy; DWORD lastSeenMs; };
            struct SmoothedRect  { std::string key; float minSX, minSY, maxSX, maxSY; DWORD lastSeenMs; };
            static std::vector<SmoothedPoint> s_nametagSmooth;
            static std::vector<SmoothedRect>  s_chestSmooth;
            s_nametagSmooth.erase(std::remove_if(s_nametagSmooth.begin(), s_nametagSmooth.end(),
                [&](const SmoothedPoint& p) { return (overlayNowMs - p.lastSeenMs) > 1200; }), s_nametagSmooth.end());
            s_chestSmooth.erase(std::remove_if(s_chestSmooth.begin(), s_chestSmooth.end(),
                [&](const SmoothedRect& r) { return (overlayNowMs - r.lastSeenMs) > 1200; }), s_chestSmooth.end());

            int drawnTags = -1;

            // ── Nametags: no JNI on render thread, all data from background-thread snapshots ──
            if (cfg.nametags && sharedCamFound && sharedMatsOk) {
                drawnTags = 0;
                const int nametagRenderCap = (std::max)(1, (std::min)(20, cfg.nametagMaxCount));

                if (!playerSnap.empty()) {
                    const LegoVec3  cam      = sharedCam;
                    const float     yaw      = sharedYaw;
                    const float     pitch    = sharedPitch;
                    const Matrix4x4 projMat  = sharedProj;
                    const Matrix4x4 viewMat  = sharedView;
                    const bool      matsOk   = sharedMatsOk;
                    const float fov  = 70.0f;
                    const int   winW = (int)io.DisplaySize.x;
                    const int   winH = (int)io.DisplaySize.y;

                    for (const auto& it : playerSnap) {
                        if (drawnTags >= nametagRenderCap) break;
                        // Filter out hologram/NPC lines that are just dashes
                        bool isFakeDash = true;
                        for (char c : it.name) {
                            if (c != '-' && c != '=' && c != ' ' && c != '_' && c != '[' && c != ']') {
                                isFakeDash = false;
                                break;
                            }
                        }
                        if (isFakeDash && it.name.length() > 0) continue;

                        // Centre of player's head
                        LegoVec3 headPos = { it.ex, it.ey + 2.05, it.ez };
                        float sx = 0, sy = 0;

                        bool projected = false;
                        if (matsOk) {
                            projected = WorldToScreen(headPos, cam, viewMat, projMat, winW, winH, &sx, &sy);
                        } else {
                            projected = WorldToScreen_Angles(headPos, cam, yaw, pitch, fov, winW, winH, &sx, &sy);
                        }

                        if (!projected) continue;

                        std::string smoothKey = it.name;
                        bool smoothFound = false;
                        for (auto& sm : s_nametagSmooth) {
                            if (sm.key == smoothKey) {
                                sm.sx += (sx - sm.sx) * overlaySmoothAlpha;
                                sm.sy += (sy - sm.sy) * overlaySmoothAlpha;
                                sm.lastSeenMs = overlayNowMs;
                                sx = sm.sx;
                                sy = sm.sy;
                                smoothFound = true;
                                break;
                            }
                        }
                        if (!smoothFound) s_nametagSmooth.push_back({ smoothKey, sx, sy, overlayNowMs });

                        // Scale text down with distance, keep readable minimum
                        float val       = 1.0f - (float)(it.dist / 64.0f);
                        float nameScale = std::max(0.65f, std::min(val, 1.0f));

                        std::string nameText = it.name;
                        std::string statsText = "";
                                    if (cfg.nametagHealth) {
                                        char hpBuf[32]; snprintf(hpBuf, sizeof(hpBuf), "%.0f HP", it.hp);
                                        statsText += hpBuf;
                                    }
                                    if (cfg.nametagArmor && it.armor > 0) {
                                        char armBuf[32]; snprintf(armBuf, sizeof(armBuf), "%s%d ARM", statsText.empty() ? "" : " | ", it.armor);
                                        statsText += armBuf;
                                    }

                                    const float nameFontSize = std::floor(ImGui::GetFontSize() * nameScale);
                                    const float infoFontSize = std::floor(nameFontSize * 0.85f);
                                    
                                    ImVec2 nameSz = ImGui::CalcTextSize(nameText.c_str());
                                    nameSz.x *= nameScale; nameSz.y *= nameScale;
                                    
                                    ImVec2 statsSz = {0,0};
                                    if (!statsText.empty()) {
                                        statsSz = ImGui::CalcTextSize(statsText.c_str());
                                        statsSz.x *= (infoFontSize / ImGui::GetFontSize());
                                        statsSz.y *= (infoFontSize / ImGui::GetFontSize());
                                    }
                                    
                                    ImVec2 itemSz = {0,0};
                                    if (!it.heldItem.empty()) {
                                        itemSz = ImGui::CalcTextSize(it.heldItem.c_str());
                                        itemSz.x *= (infoFontSize / ImGui::GetFontSize());
                                        itemSz.y *= (infoFontSize / ImGui::GetFontSize());
                                    }

                                    float maxW = std::max({nameSz.x, statsSz.x, itemSz.x});
                                    float totalH = nameSz.y;
                                    if (statsSz.y > 0) totalH += statsSz.y + 2.0f;
                                    if (itemSz.y > 0) totalH += itemSz.y + 2.0f;

                                    float pad = std::floor(4.0f * nameScale);
                                    float rx = std::floor(sx - maxW / 2.0f);
                                    float ry = std::floor(sy - totalH - pad * 2.0f);

                                    ImVec2 pMin = ImVec2(rx - pad, ry);
                                    ImVec2 pMax = ImVec2(rx + maxW + pad, ry + totalH + pad * 2.0f + 2.0f);

                                    fg->AddRectFilled(pMin, pMax, IM_COL32(0, 0, 0, 160), 3.0f);

                                    float curY = ry + pad;
                                    
                                    // Name
                                    float nx = std::floor(sx - nameSz.x / 2.0f);
                                    fg->AddText(ImGui::GetFont(), nameFontSize, ImVec2(nx + 1, curY + 1), IM_COL32(0, 0, 0, 255), nameText.c_str());
                                    fg->AddText(ImGui::GetFont(), nameFontSize, ImVec2(nx, curY), IM_COL32(255, 255, 255, 250), nameText.c_str());
                                    curY += nameSz.y + 2.0f;
                                    
                                    // Stats
                                    if (!statsText.empty()) {
                                        float stx = std::floor(sx - statsSz.x / 2.0f);
                                        ImU32 statCol = it.hp <= 8.0 ? IM_COL32(255, 100, 100, 250) : IM_COL32(200, 220, 255, 250);
                                        fg->AddText(ImGui::GetFont(), infoFontSize, ImVec2(stx + 1, curY + 1), IM_COL32(0, 0, 0, 255), statsText.c_str());
                                        fg->AddText(ImGui::GetFont(), infoFontSize, ImVec2(stx, curY), statCol, statsText.c_str());
                                        curY += statsSz.y + 2.0f;
                                    }
                                    
                                    // Item
                                    if (!it.heldItem.empty()) {
                                        float itx = std::floor(sx - itemSz.x / 2.0f);
                                        fg->AddText(ImGui::GetFont(), infoFontSize, ImVec2(itx + 1, curY + 1), IM_COL32(0, 0, 0, 255), it.heldItem.c_str());
                                        fg->AddText(ImGui::GetFont(), infoFontSize, ImVec2(itx, curY), IM_COL32(255, 200, 80, 250), it.heldItem.c_str());
                                    }

                                    if (cfg.nametagHealth) {
                                        float hpPct = std::max(0.0f, std::min((float)it.hp / 20.0f, 1.0f));
                                        float barW  = (pMax.x - pMin.x) * hpPct;
                                        ImU32 col   = IM_COL32((int)(255 * (1.0f - hpPct)), (int)(220 * hpPct + 35), 60, 255);
                                        fg->AddRectFilled(ImVec2(pMin.x, pMax.y),
                                                          ImVec2(pMin.x + barW, pMax.y + std::floor(3.0f * nameScale)), col);
                                    }
                                    drawnTags++;
                                }
                        } // sharedCamFound && playerSnap
            } // cfg.nametags

            // ── Chest ESP: draw bounding rect over each nearby chest ──
            if (cfg.chestEsp && sharedCamFound && sharedMatsOk) {
                LockGuard chestLk(g_chestListMutex); // protect g_chestList from background scan thread
                // Use shared camera data (same source as nametags – no redundant JNI fetch)
                const LegoVec3   espCam   = sharedCam;
                const float      espYaw   = sharedYaw;
                const float      espPitch = sharedPitch;
                const bool       espMatsOk = sharedMatsOk;
                const Matrix4x4  espProj  = sharedProj;
                const Matrix4x4  espView  = sharedView;

                // One-time diagnostic: log projection state on first ESP frame
                static bool s_espDiagLogged = false;
                if (!s_espDiagLogged && !g_chestList.empty()) {
                    s_espDiagLogged = true;
                    const auto& ch0 = g_chestList[0];
                    float csx = 0, csy = 0;
                    LegoVec3 center = { ch0.x, ch0.y + 0.5, ch0.z };
                    bool ok = espMatsOk
                        ? WorldToScreen(center, espCam, espView, espProj, (int)io.DisplaySize.x, (int)io.DisplaySize.y, &csx, &csy)
                        : WorldToScreen_Angles(center, espCam, espYaw, espPitch, 70.0f, (int)io.DisplaySize.x, (int)io.DisplaySize.y, &csx, &csy);
                    Log(std::string("ChestESP diag: matsOk=") + (espMatsOk?"1":"0")
                        + " yaw=" + std::to_string(espYaw) + " pitch=" + std::to_string(espPitch)
                        + " cam=(" + std::to_string(espCam.x) + "," + std::to_string(espCam.y) + "," + std::to_string(espCam.z) + ")"
                        + " chest=(" + std::to_string(ch0.x) + "," + std::to_string(ch0.y) + "," + std::to_string(ch0.z) + ")"
                        + " projected=" + (ok?"1":"0") + " sx=" + std::to_string(csx) + " sy=" + std::to_string(csy));
                }
                {
                    const int winW = (int)io.DisplaySize.x;
                    const int winH = (int)io.DisplaySize.y;
                    const float fov = 70.0f;
                    const ImU32 espColor    = IM_COL32(255, 165, 0, 220);  // orange
                    const ImU32 espColorFar = IM_COL32(255, 255, 80, 180); // yellow-ish far
                    const ImU32 espBg       = IM_COL32(0, 0, 0, 90);
                    const float lineThick   = 1.5f;

                    constexpr double kChestEspMaxRenderDist = 64.0;
                    const size_t maxChestRenderCount = (size_t)(std::max)(1, (std::min)(20, cfg.chestEspMaxCount));
                    struct RenderChestCandidate { const ChestData121* chest; double dist; };
                    std::vector<RenderChestCandidate> renderChests;
                    renderChests.reserve((std::min)(g_chestList.size(), maxChestRenderCount));
                    for (const auto& ch : g_chestList) {
                        double dx = ch.x - espCam.x;
                        double dy = ch.y - espCam.y;
                        double dz = ch.z - espCam.z;
                        double distNow = std::sqrt(dx*dx + dy*dy + dz*dz);
                        if (distNow > kChestEspMaxRenderDist) continue;
                        size_t insertAt = 0;
                        while (insertAt < renderChests.size() && renderChests[insertAt].dist <= distNow) insertAt++;
                        if (insertAt >= maxChestRenderCount) continue;
                        renderChests.insert(renderChests.begin() + static_cast<std::vector<RenderChestCandidate>::difference_type>(insertAt), { &ch, distNow });
                        if (renderChests.size() > maxChestRenderCount) renderChests.pop_back();
                    }

                    for (const auto& candidate : renderChests) {
                        const auto& ch = *candidate.chest;
                        const double chestDist = candidate.dist;
                        // A chest occupies one block: x±0.5 (from center), y to y+1, z±0.5
                        // We project 8 corners and find screen AABB
                        const double offsets[8][3] = {
                            {-0.5, 0.0, -0.5}, {0.5, 0.0, -0.5}, {-0.5, 0.0, 0.5}, {0.5, 0.0, 0.5},
                            {-0.5, 1.0, -0.5}, {0.5, 1.0, -0.5}, {-0.5, 1.0, 0.5}, {0.5, 1.0, 0.5}
                        };

                        float minSX = 999999, minSY = 999999, maxSX = -999999, maxSY = -999999;
                        int projectedCorners = 0;
                        for (int c = 0; c < 8; c++) {
                            LegoVec3 corner = { ch.x + offsets[c][0], ch.y + offsets[c][1], ch.z + offsets[c][2] };
                            float csx = 0, csy = 0;
                            bool ok = espMatsOk
                                ? WorldToScreen(corner, espCam, espView, espProj, winW, winH, &csx, &csy)
                                : WorldToScreen_Angles(corner, espCam, espYaw, espPitch, fov, winW, winH, &csx, &csy);
                            if (!ok) continue;
                            if (csx < minSX) minSX = csx;
                            if (csy < minSY) minSY = csy;
                            if (csx > maxSX) maxSX = csx;
                            if (csy > maxSY) maxSY = csy;
                            projectedCorners++;
                        }
                        if (projectedCorners < 4) continue; // skip if mostly off-screen

                        // Clamp to viewport
                        minSX = std::max(minSX, 0.0f); minSY = std::max(minSY, 0.0f);
                        maxSX = std::min(maxSX, (float)winW); maxSY = std::min(maxSY, (float)winH);
                        if (maxSX <= minSX || maxSY <= minSY) continue;

                        char chestKeyBuf[64];
                        snprintf(chestKeyBuf, sizeof(chestKeyBuf), "%d,%d,%d",
                                 (int)std::floor(ch.x), (int)std::floor(ch.y), (int)std::floor(ch.z));
                        std::string chestKey(chestKeyBuf);
                        bool chestSmoothFound = false;
                        for (auto& sm : s_chestSmooth) {
                            if (sm.key == chestKey) {
                                sm.minSX += (minSX - sm.minSX) * overlaySmoothAlpha;
                                sm.minSY += (minSY - sm.minSY) * overlaySmoothAlpha;
                                sm.maxSX += (maxSX - sm.maxSX) * overlaySmoothAlpha;
                                sm.maxSY += (maxSY - sm.maxSY) * overlaySmoothAlpha;
                                sm.lastSeenMs = overlayNowMs;
                                minSX = sm.minSX; minSY = sm.minSY;
                                maxSX = sm.maxSX; maxSY = sm.maxSY;
                                chestSmoothFound = true;
                                break;
                            }
                        }
                        if (!chestSmoothFound) s_chestSmooth.push_back({ chestKey, minSX, minSY, maxSX, maxSY, overlayNowMs });

                        // Color fades with distance: close = orange, far = yellow
                        float t = std::min((float)(chestDist / 40.0f), 1.0f);
                        ImU32 c0r = (ImU32)(255);
                        ImU32 c0g = (ImU32)(165 + (int)(90 * t));
                        ImU32 c0b = (ImU32)(0 + (int)(80 * t));
                        ImU32 boxColor = IM_COL32(c0r, c0g, c0b, (int)(220 - 40 * t));

                        // Background
                        fg->AddRectFilled(ImVec2(minSX, minSY), ImVec2(maxSX, maxSY), espBg);
                        // Outline
                        fg->AddRect(ImVec2(minSX, minSY), ImVec2(maxSX, maxSY), boxColor, 0.0f, 0, lineThick);
                    }
                }
            } // cfg.chestEsp

            if (cfg.gtbHelper) {
                std::string hint = cfg.gtbHint;
                std::string preview = cfg.gtbPreview;
                if (hint.empty() || hint == "-") hint = "waiting for hint...";
                if (preview == "-") preview.clear();

                std::vector<std::string> lines;
                if (!preview.empty()) {
                    std::stringstream ss(preview);
                    std::string part;
                    while (std::getline(ss, part, ',')) {
                        size_t b = part.find_first_not_of(' ');
                        size_t e = part.find_last_not_of(' ');
                        if (b == std::string::npos || e == std::string::npos) continue;
                        std::string clean = part.substr(b, e - b + 1);
                        if (clean.size() > 56) clean = clean.substr(0, 56) + "...";
                        lines.push_back(clean);
                    }
                    if (lines.empty()) lines.push_back(preview);
                }

                const int maxLines = 8;
                if ((int)lines.size() > maxLines) {
                    lines.resize(maxLines);
                    if (!lines.empty()) lines.back() += " ...";
                }

                const float panelW = (std::min)(360.0f, io.DisplaySize.x - 20.0f);
                const float lineH = ImGui::GetFontSize() + 2.0f;
                const float panelH = 18.0f + lineH + (lines.empty() ? 0.0f : (8.0f + lineH * (float)lines.size())) + 10.0f;
                const float x1 = io.DisplaySize.x - 10.0f;
                const float y1 = io.DisplaySize.y - 10.0f;
                const float x0 = (std::max)(10.0f, x1 - panelW);
                const float y0 = (std::max)(10.0f, y1 - panelH);

                fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 150), 5.0f);
                fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 210, 90, 220), 5.0f, 0, 1.2f);

                char hintBuf[320];
                snprintf(hintBuf, sizeof(hintBuf), "GTB: %s (%d)", hint.c_str(), (std::max)(0, cfg.gtbCount));
                fg->AddText(ImVec2(x0 + 9.0f, y0 + 8.0f), IM_COL32(255, 226, 150, 245), hintBuf);

                float y = y0 + 8.0f + lineH + 6.0f;
                for (const auto& line : lines) {
                    std::string row = "- " + line;
                    fg->AddText(ImVec2(x0 + 10.0f, y), IM_COL32(240, 240, 240, 235), row.c_str());
                    y += lineH;
                }
            }

            // Module list (top-right) - original-like (right aligned colored bars)
            struct ModLine { const char* text; ImU32 accent; float width; };
            ModLine mods[16];
            int modCount = 0;

            auto pushMod = [&](const char* text, ImU32 accent) {
                if (!text || !*text) return;
                ModLine m{ text, accent, ImGui::CalcTextSize(text).x };
                mods[modCount++] = m;
            };

            char acBuf[64];
            if (cfg.armed) {
                int lo = (int)cfg.minCPS;
                int hi = (int)cfg.maxCPS;
                if (hi < lo) std::swap(hi, lo);
                snprintf(acBuf, sizeof(acBuf), "Autoclicker %d-%d", lo, hi);
                pushMod(acBuf, IM_COL32(60, 220, 120, 255));
            }
            if (cfg.clickInChests) pushMod("Click in Chests", IM_COL32(50, 200, 220, 255));
            if (cfg.closestPlayer) pushMod("Closest Player", IM_COL32(60, 170, 255, 255));
            if (cfg.rightClick)    pushMod("Rightclick", IM_COL32(255, 170, 60, 255));
            if (cfg.aimAssist)    pushMod("Aim Assist", IM_COL32(167, 125, 255, 255));
            if (cfg.chestEsp)      pushMod("Chest ESP", IM_COL32(120, 120, 255, 255));
            if (cfg.nametags)      pushMod("Nametags", IM_COL32(185, 85, 255, 255));
            if (cfg.gtbHelper)     pushMod("GTB Helper", IM_COL32(255, 210, 90, 255));
            if (cfg.jitter)        pushMod("Jitter", IM_COL32(255, 80, 120, 255));
            if (cfg.breakBlocks)   pushMod("Break Blocks", IM_COL32(220, 220, 220, 255));

            // Sort by width descending (staggered original look)
            for (int a = 0; a < modCount; a++) {
                for (int b = a + 1; b < modCount; b++) {
                    if (mods[b].width > mods[a].width) {
                        ModLine tmp = mods[a]; mods[a] = mods[b]; mods[b] = tmp;
                    }
                }
            }

            const float marginX = 10.0f;
            const float marginY = 10.0f;
            const float padX = 8.0f;
            const float padY = 3.0f;
            const float barW = 3.0f;
            const float gapY = 2.0f;
            const float fontH = ImGui::GetFontSize();
            float y = marginY;
            for (int i = 0; i < modCount; i++) {
                const ModLine& m = mods[i];
                ImVec2 textSz = ImGui::CalcTextSize(m.text);
                float boxW = barW + padX + textSz.x + padX;
                float boxH = padY + fontH + padY;
                float x0 = io.DisplaySize.x - marginX - boxW;
                float x1 = io.DisplaySize.x - marginX;
                float y0 = y;
                float y1 = y + boxH;

                // Background
                fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 140));
                // Accent strip
                fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + barW, y1), m.accent);
                // Subtle outline
                fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 40));
                // Text shadow + text
                ImVec2 tx = ImVec2(x0 + barW + padX, y0 + padY);
                fg->AddText(ImVec2(tx.x + 1, tx.y + 1), IM_COL32(0, 0, 0, 180), m.text);
                fg->AddText(tx, IM_COL32(255, 255, 255, 235), m.text);

                y += boxH + gapY;
            }
        }
    }

    ImGui::Render();
    // Avoid driver issues when minimized / zero-sized backbuffer.
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x > 1.0f && io.DisplaySize.y > 1.0f) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    // Flush all ImGui GL commands before handing control back to the NVIDIA driver's
    // swap implementation.  Without this, pending draw calls may reference ImGui GL
    // objects that the driver hasn't seen yet, causing an EXCEPTION_ACCESS_VIOLATION
    // inside nvoglv64.dll.
    glFlush();

    return o_wglSwapBuffers(hDc);
}

// ===================== MAIN THREAD =====================
DWORD WINAPI MainThread(LPVOID) {
    Log("=== bridge_121.dll loaded ===");

    // Wait for JVM
    HMODULE hJvm = nullptr;
    for (int i = 0; i < 60 && !hJvm; i++) {
        hJvm = GetModuleHandleA("jvm.dll");
        if (!hJvm) Sleep(500);
    }
    if (!hJvm) { Log("ERROR: jvm.dll not found."); return 0; }

    typedef jint (JNICALL* FnGetVMs)(JavaVM**, jsize, jsize*);
    FnGetVMs fn = (FnGetVMs)GetProcAddress(hJvm, "JNI_GetCreatedJavaVMs");
    if (!fn) { Log("ERROR: JNI_GetCreatedJavaVMs not found."); return 0; }

    JavaVM* jvm = nullptr; jsize cnt = 0;
    for (int i = 0; i < 10; i++) {
        fn(&jvm, 1, &cnt);
        if (jvm && cnt > 0) break;
        Sleep(1000);
    }
    if (!jvm || cnt == 0) { Log("ERROR: No JVM."); return 0; }
    g_jvm = jvm;
    Log("JVM found.");

    // Install MinHook
    if (MH_Initialize() != MH_OK) { Log("ERROR: MinHook init failed."); return 0; }

    // Hook wglSwapBuffers (for ImGui rendering)
    HMODULE hOgl = GetModuleHandleA("opengl32.dll");
    void* pSwap  = (void*)GetProcAddress(hOgl, "wglSwapBuffers");
    if (!pSwap) { Log("ERROR: wglSwapBuffers not found."); return 0; }
    if (MH_CreateHook(pSwap, (void*)hwglSwapBuffers, (void**)&o_wglSwapBuffers) != MH_OK ||
        MH_EnableHook(pSwap) != MH_OK) {
        Log("ERROR: Failed to hook wglSwapBuffers."); return 0;
    }
    Log("wglSwapBuffers hooked.");

    // ---- Load GLFW function pointers (no hook needed; just direct calls) ----
    for (int attempt = 0; attempt < 30; attempt++) {
        const char* names[] = { "glfw.dll", "glfw64.dll", nullptr };
        for (int i = 0; names[i]; i++) {
            HMODULE hGlfw = GetModuleHandleA(names[i]);
            if (!hGlfw) continue;
            glfwGetCurrentContext_fn = (PFN_glfwGetCurrentContext)GetProcAddress(hGlfw, "glfwGetCurrentContext");
            glfwSetInputMode_fn      = (PFN_glfwSetInputMode)     GetProcAddress(hGlfw, "glfwSetInputMode");
            glfwGetInputMode_fn      = (PFN_glfwGetInputMode)     GetProcAddress(hGlfw, "glfwGetInputMode");
            if (glfwGetCurrentContext_fn && glfwSetInputMode_fn) {
                Log("GLFW function pointers loaded from: " + std::string(names[i]));
                goto glfw_done;
            }
        }
        Sleep(500);
    }
    Log("WARNING: GLFW not found. Cursor control will use fallback.");
glfw_done:;

    // ---- JNI Discovery (JVMTI class scan, like 1.8.9 bridge) ----
    // Run on this thread which we attach to the JVM.
    // Wait a bit for Minecraft to finish initializing.
    Sleep(5000);
    {
        JNIEnv* denv = nullptr;
        bool attached = false;
        if (g_jvm->GetEnv((void**)&denv, JNI_VERSION_1_8) != JNI_OK) {
            g_jvm->AttachCurrentThread((void**)&denv, nullptr);
            attached = true;
        }
        if (denv) {
            // Retry discovery a few times (MC may not be fully loaded yet)
            for (int attempt = 0; attempt < 5; attempt++) {
                if (DiscoverJniMappings(denv)) break;
                Log("Discovery attempt " + std::to_string(attempt+1) + " failed, retrying in 3s...");
                Sleep(3000);
            }
        }
        if (attached) g_jvm->DetachCurrentThread();
    }

    // TCP Server
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr = {}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(25590);
    bind(srv, (sockaddr*)&addr, sizeof(addr));
    listen(srv, 1);
    Log("TCP server listening on port 25590.");

    setlocale(LC_NUMERIC, "C");

    while (g_running) {
        SOCKET cli = accept(srv, nullptr, nullptr);
        if (cli == INVALID_SOCKET) { Sleep(100); continue; }
        Log("C# Loader connected.");
        u_long nb = 1; ioctlsocket(cli, FIONBIO, &nb);

        std::string readBuf;
        while (g_running) {
            // Send state
            {
                std::string sn;
                std::string actionBar;
                bool jniGui;
                bool lookBlock;
                bool breakBlock;
                bool holdBlock;
                { LockGuard lk(g_jniStateMtx);
                    sn = g_jniScreenName;
                    actionBar = g_jniActionBar;
                    jniGui = g_jniGuiOpen;
                    lookBlock = g_jniLookingAtBlock;
                    breakBlock = g_jniBreakingBlock;
                    holdBlock = g_jniHoldingBlock;
                }

                // guiOpen = our menu OR JNI reports a Minecraft screen is open
                // (but ChatScreen opened by US for cursor unlock is not a "real" gui)
                bool anyGui = g_ShowMenu || (jniGui && sn != "ChatScreen");

                // JSON-escape the screen name
                std::string snEsc;
                for (char c : sn) { if (c == '"' || c == '\\') snEsc += '\\'; snEsc += c; }
                std::string actionEsc;
                for (char c : actionBar) { if (c == '"' || c == '\\') actionEsc += '\\'; actionEsc += c; }

                std::vector<PlayerData121> players;
                { LockGuard lk(g_playerListMutex); players = g_playerList; }

                BgCamState camState;
                { LockGuard lk(g_bgCamMutex); camState = g_bgCamState; }

                int winW = 1920, winH = 1080;
                if (g_hwnd && IsWindow(g_hwnd)) {
                    RECT rc{};
                    if (GetClientRect(g_hwnd, &rc)) {
                        winW = (std::max)(1, (int)(rc.right - rc.left));
                        winH = (std::max)(1, (int)(rc.bottom - rc.top));
                    }
                }

                std::string state;
                state.reserve(4096);
                state += "{\"type\":\"state\",\"guiOpen\":";
                state += anyGui ? "true" : "false";
                state += ",\"screenName\":\"";
                state += snEsc;
                state += "\",\"actionBar\":\"";
                state += actionEsc;
                state += "\",\"health\":20,\"posX\":0,\"posY\":0,\"posZ\":0";
                state += ",\"holdingBlock\":";
                state += holdBlock ? "true" : "false";
                state += ",\"lookingAtBlock\":";
                state += lookBlock ? "true" : "false";
                state += ",\"breakingBlock\":";
                state += breakBlock ? "true" : "false";
                state += ",\"entities\":[";

                bool first = true;
                LegoVec3 camPos = { camState.camX, camState.camY, camState.camZ };
                for (const auto& p : players) {
                    float sx = -1.0f, sy = -1.0f;
                    bool projected = false;

                    if (camState.camFound) {
                        auto projectPoint = [&](const LegoVec3& pos, float* outX, float* outY) -> bool {
                            return camState.matsOk
                                ? WorldToScreen(pos, camPos, camState.view, camState.proj, winW, winH, outX, outY)
                                : WorldToScreen_Angles(pos, camPos, camState.yaw, camState.pitch, 70.0f, winW, winH, outX, outY);
                        };

                        // Aim-assist target point: closest projected point on player's body box to screen center.
                        const double halfW = 0.30;
                        const double xOffsets[3] = { -halfW, 0.0, halfW };
                        const double zOffsets[3] = { -halfW, 0.0, halfW };
                        const double yOffsets[5] = { 0.15, 0.55, 0.95, 1.35, 1.75 };
                        const double centerX = winW * 0.5;
                        const double centerY = winH * 0.5;

                        double bestScore = 1e30;
                        float bestSx = -1.0f, bestSy = -1.0f;
                        bool bestFound = false;
                        for (double yo : yOffsets) {
                            for (double xo : xOffsets) {
                                for (double zo : zOffsets) {
                                    LegoVec3 bodyPoint = { p.ex + xo, p.ey + yo, p.ez + zo };
                                    float tx = -1.0f, ty = -1.0f;
                                    if (!projectPoint(bodyPoint, &tx, &ty)) continue;
                                    double dx = tx - centerX;
                                    double dy = ty - centerY;
                                    double score = dx * dx + dy * dy;
                                    if (score < bestScore) {
                                        bestScore = score;
                                        bestSx = tx;
                                        bestSy = ty;
                                        bestFound = true;
                                    }
                                }
                            }
                        }

                        if (bestFound) {
                            sx = bestSx;
                            sy = bestSy;
                            projected = true;
                        } else {
                            LegoVec3 fallbackPos = { p.ex, p.ey + 1.575, p.ez };
                            projected = projectPoint(fallbackPos, &sx, &sy);
                        }
                    }

                    std::string nameEsc;
                    for (char c : p.name) { if (c == '"' || c == '\\') nameEsc += '\\'; nameEsc += c; }

                    if (!first) state += ",";
                    first = false;
                    state += "{\"sx\":";
                    state += std::to_string(projected ? sx : -1.0f);
                    state += ",\"sy\":";
                    state += std::to_string(projected ? sy : -1.0f);
                    state += ",\"dist\":";
                    state += std::to_string(p.dist);
                    state += ",\"name\":\"";
                    state += nameEsc;
                    state += "\",\"hp\":";
                    state += std::to_string(p.hp);
                    state += "}";
                }
                state += "]}\n";
                send(cli, state.c_str(), (int)state.size(), 0);
            }

            // Send pending GUI commands
            {
                std::vector<std::string> cmds;
                { LockGuard lk(g_cmdMutex); cmds.swap(g_pendingCmds); }
                for (const auto& c : cmds)
                    send(cli, c.c_str(), (int)c.size(), 0);
            }

            // Receive config from C#
            {
                char buf[4096];
                int r = recv(cli, buf, sizeof(buf)-1, 0);
                if (r > 0) {
                    buf[r] = 0; readBuf += buf;
                    size_t pos;
                    while ((pos = readBuf.find('\n')) != std::string::npos) {
                        std::string pkt = readBuf.substr(0, pos);
                        readBuf.erase(0, pos+1);
                        if (!pkt.empty()) ParseConfig(pkt);
                    }
                } else if (r == 0) break;
            }

            Sleep(50);
        }
        closesocket(cli);
        Log("C# Loader disconnected.");
    }

    closesocket(srv);
    WSACleanup();
    MH_Uninitialize();
    return 0;
}

// ===================== DLL ENTRY =====================
extern "C" __declspec(dllexport) void Dummy121() {}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        
        char path[MAX_PATH];
        if (GetModuleFileNameA(hModule, path, MAX_PATH)) {
            std::string fullPath(path);
            size_t  pos = fullPath.find_last_of("\\/");
            if (pos != std::string::npos) {
                g_logPath = fullPath.substr(0, pos) + "\\bridge_121_debug.log";
            }
        }
        
        std::ofstream(g_logPath, std::ios_base::trunc)
            << "=== bridge_121.dll DLL_PROCESS_ATTACH ===\n";
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
