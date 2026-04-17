/**
 * bridge_261.cpp - LegoClicker Native Bridge for Lunar 26.1 (Fabric)
 * Build: 2026-02-20
 *
 * Architecture:
 *  - Hooks wglSwapBuffers -> renders ImGui overlay every frame.
 *  - Hooks WndProc -> blocks game input when GUI is open.
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
 *  toggleNametagHealth, toggleNametagArmor, toggleNametagHideVanilla, toggleRight, toggleBreakBlocks
 */
#include <winsock2.h>
#include <windows.h>
#include <jni.h>
#include <jvmti.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <unordered_map>
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

static bool g_trace261Enabled = false;

static void TraceValue261(const char* fn, const char* key, const std::string& value) {
    if (!g_trace261Enabled) return;
    Log(std::string("TRACE|") + (fn ? fn : "?") + "|" + (key ? key : "?") + "|" + value);
}

static void TraceBranch261(const char* fn, const char* branch, bool taken) {
    if (!g_trace261Enabled) return;
    Log(std::string("TRACE|") + (fn ? fn : "?") + "|" + (branch ? branch : "?") + "|" + (taken ? "1" : "0"));
}

static bool TraceDecision261(const char* fn, const char* branch, bool decision) {
    TraceBranch261(fn, branch, decision);
    return decision;
}

#define TRACE261_PATH(pathValue) TraceValue261(__FUNCTION__, "path", (pathValue))
#define TRACE261_VALUE(key, value) TraceValue261(__FUNCTION__, (key), (value))
#define TRACE261_BRANCH(branch, taken) TraceBranch261(__FUNCTION__, (branch), (taken))
#define TRACE261_IF(branch, expr) TraceDecision261(__FUNCTION__, (branch), (expr))

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
    bool  triggerbot     = false;
    bool  nametags       = false;
    bool  chestEsp       = false;
    bool  showModuleList = true;
    bool  closestPlayer  = false;
    bool  nametagHealth  = true;
    bool  nametagArmor   = true;
    bool  nametagHideVanilla = false;
    int   nametagMaxCount = 8;
    int   chestEspMaxCount = 5;
    bool  rightClick     = false;
    int   moduleListStyle = 0;
    bool  showLogo       = true;
    std::string guiTheme = "Default";
    float rightMinCPS    = 10.0f;
    float rightMaxCPS    = 14.0f;
    bool  rightBlockOnly = false;
    bool  breakBlocks    = false;
    bool  gtbHelper      = false;
    int   gtbCount       = 0;
    std::string gtbHint;
    std::string gtbPreview;
    bool  reachEnabled   = false;
    float reachMin       = 3.0f;
    float reachMax       = 6.0f;
    int   reachChance    = 100;
    bool  velocityEnabled = false;
    int   velocityHorizontal = 100;
    int   velocityVertical = 100;
    int   velocityChance = 100;
    int   reloadMappingsNonce = 0;
};
static Config g_config;
static Mutex  g_configMutex;
static volatile LONG g_forceGlobalJniRemap_121 = 0;

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
    TRACE261_PATH("enter");
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
        std::string res = (e == std::string::npos) ? line.substr(p) : line.substr(p, e-p);
        // trim leading spaces just in case
        while(!res.empty() && res[0] == ' ') res.erase(0, 1);
        return res;
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

    bool isConfig = TRACE261_IF("isConfigPacket", getStr("type") == "config");
    if (!isConfig) return;

    LockGuard lk(g_configMutex);
    int prevReloadNonce = g_config.reloadMappingsNonce;
    g_config.armed         = getBool("armed");
    g_config.clicking      = getBool("clicking");
    g_config.minCPS        = getFloat("minCPS");
    g_config.maxCPS        = getFloat("maxCPS");
    g_config.jitter        = getBool("jitter");
    g_config.clickInChests = getBool("clickInChests");
    g_config.aimAssist     = getBool("aimAssist");
    g_config.triggerbot    = getBool("triggerbot");
    g_config.nametags      = getBool("nametags");
    g_config.chestEsp      = getBool("chestEsp");
    std::string showModuleListRaw = getStr("showModuleList");
    g_config.showModuleList = showModuleListRaw.empty() ? true : (showModuleListRaw == "true");
    g_config.closestPlayer = getBool("closestPlayerInfo");
    g_config.nametagHealth = getBool("nametagShowHealth");
    g_config.nametagArmor  = getBool("nametagShowArmor");
    g_config.nametagHideVanilla = getBool("nametagHideVanilla");
    g_config.nametagMaxCount = (std::max)(1, (std::min)(20, getInt("nametagMaxCount", g_config.nametagMaxCount)));
    g_config.chestEspMaxCount = (std::max)(1, (std::min)(20, getInt("chestEspMaxCount", g_config.chestEspMaxCount)));
    g_config.rightClick    = getBool("right");
    g_config.rightMinCPS   = getFloat("rightMinCPS");
    g_config.rightMaxCPS   = getFloat("rightMaxCPS");
    g_config.moduleListStyle = (std::max)(0, (std::min)(4, getInt("moduleListStyle", 0)));
    std::string showLogoRaw = getStr("showLogo");
    g_config.showLogo = showLogoRaw.empty() ? true : (showLogoRaw == "true");
    std::string guiThemeRaw = getStr("guiTheme");
    g_config.guiTheme = guiThemeRaw.empty() ? "Default" : guiThemeRaw;
    g_config.rightBlockOnly= getBool("rightBlock");
    g_config.breakBlocks   = getBool("breakBlocks");
    g_config.gtbHelper     = getBool("gtbHelper");
    g_config.gtbHint       = getStr("gtbHint");
    g_config.gtbCount      = getInt("gtbCount", 0);
    g_config.gtbPreview    = getStr("gtbPreview");
    g_config.reachEnabled  = getBool("reachEnabled");
    g_config.reachMin      = getFloat("reachMin");
    g_config.reachMax      = getFloat("reachMax");
    g_config.reachChance   = getInt("reachChance", 100);
    g_config.velocityEnabled = getBool("velocityEnabled");
    g_config.velocityHorizontal = (std::max)(1, (std::min)(100, getInt("velocityHorizontal", 100)));
    g_config.velocityVertical = (std::max)(1, (std::min)(100, getInt("velocityVertical", 100)));
    g_config.velocityChance = (std::max)(1, (std::min)(100, getInt("velocityChance", 100)));
    g_config.reloadMappingsNonce = getInt("reloadMappingsNonce", g_config.reloadMappingsNonce);
    bool reloadPulse = (g_config.reloadMappingsNonce != prevReloadNonce);
    TRACE261_BRANCH("reloadMappingsPulse", reloadPulse);
    if (reloadPulse) {
        InterlockedExchange(&g_forceGlobalJniRemap_121, 1);
        Log("ReloadMappings: received loader pulse; scheduling full JNI remap across modules.");
    }
}

static bool TrySendCapabilities(SOCKET sock) {
    if (sock == INVALID_SOCKET) return false;

    static const std::string kCapabilitiesJson =
        "{\"type\":\"capabilities\","
        "\"modules\":[\"autoclicker\",\"rightclick\",\"jitter\",\"clickinchests\",\"breakblocks\",\"aimassist\",\"triggerbot\",\"gtbhelper\",\"nametags\",\"closestplayer\",\"chestesp\",\"reach\",\"velocity\"],"
        "\"settings\":[\"mincps\",\"maxcps\",\"left\",\"right\",\"rightmincps\",\"rightmaxcps\",\"rightblock\",\"breakblocks\",\"jitter\",\"clickinchests\",\"triggerbot\",\"gtbhint\",\"gtbcount\",\"gtbpreview\",\"nametags\",\"closestplayerinfo\",\"nametagshowhealth\",\"nametagshowarmor\",\"nametaghidevanilla\",\"nametagmaxcount\",\"chestesp\",\"chestespmaxcount\",\"reachenabled\",\"reachmin\",\"reachmax\",\"reachchance\",\"velocityenabled\",\"velocityhorizontal\",\"velocityvertical\",\"velocitychance\",\"reloadmappingsnonce\",\"showmodulelist\",\"moduleliststyle\",\"showlogo\",\"guitheme\"],"
        "\"state\":[\"actionbar\",\"holdingblock\",\"lookingatblock\",\"lookingatentity\",\"lookingatentitylatched\",\"breakingblock\",\"attackcooldown\",\"attackcooldownpertick\",\"statems\"]}\n";

    int sent = send(sock, kCapabilitiesJson.c_str(), (int)kCapabilitiesJson.size(), 0);
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return false;
        Log("Capabilities send failed, err=" + std::to_string(err));
        return false;
    }
    return true;
}

struct OverlayTheme {
    ImU32 accentPrimary;
    ImU32 accentSecondary;
    ImU32 accentTertiary;
    ImU32 logoColor;
    ImU32 logoShadow;
    ImU32 moduleBg;
    ImU32 moduleBorder;
    ImU32 moduleText;
    ImU32 moduleTextShadow;
    ImU32 moduleMinimalBg;
    ImU32 moduleOutlinedBg;
    ImU32 moduleGlassBorder;
    ImU32 moduleBoldText;
    ImU32 gtbBorder;
    ImU32 gtbTitle;
    ImU32 gtbRow;
};

static std::string ToLowerAscii(std::string value)
{
    for (char& ch : value) ch = (char)std::tolower((unsigned char)ch);
    return value;
}

static OverlayTheme ResolveOverlayTheme(const std::string& guiTheme)
{
    std::string key = ToLowerAscii(guiTheme);

    if (key == "dark blue") {
        return {
            IM_COL32(59, 130, 246, 255),
            IM_COL32(37, 99, 235, 255),
            IM_COL32(96, 165, 250, 255),
            IM_COL32(125, 195, 255, 255),
            IM_COL32(0, 0, 0, 210),
            IM_COL32(12, 23, 42, 160),
            IM_COL32(148, 163, 184, 70),
            IM_COL32(241, 245, 249, 238),
            IM_COL32(0, 0, 0, 190),
            IM_COL32(12, 23, 42, 100),
            IM_COL32(6, 18, 34, 190),
            IM_COL32(125, 211, 252, 105),
            IM_COL32(255, 255, 255, 245),
            IM_COL32(125, 195, 255, 220),
            IM_COL32(191, 219, 254, 245),
            IM_COL32(224, 242, 254, 238)
        };
    }
    if (key == "crimson") {
        return {
            IM_COL32(244, 63, 94, 255),
            IM_COL32(225, 29, 72, 255),
            IM_COL32(251, 113, 133, 255),
            IM_COL32(255, 145, 165, 255),
            IM_COL32(0, 0, 0, 210),
            IM_COL32(42, 15, 21, 168),
            IM_COL32(251, 113, 133, 80),
            IM_COL32(255, 234, 238, 238),
            IM_COL32(0, 0, 0, 190),
            IM_COL32(42, 15, 21, 108),
            IM_COL32(28, 9, 13, 198),
            IM_COL32(251, 113, 133, 118),
            IM_COL32(255, 245, 247, 245),
            IM_COL32(255, 150, 165, 220),
            IM_COL32(255, 220, 228, 245),
            IM_COL32(255, 238, 242, 238)
        };
    }
    if (key == "emerald") {
        return {
            IM_COL32(16, 185, 129, 255),
            IM_COL32(5, 150, 105, 255),
            IM_COL32(52, 211, 153, 255),
            IM_COL32(86, 255, 199, 255),
            IM_COL32(0, 0, 0, 210),
            IM_COL32(8, 33, 30, 165),
            IM_COL32(74, 222, 128, 70),
            IM_COL32(220, 252, 231, 238),
            IM_COL32(0, 0, 0, 190),
            IM_COL32(8, 33, 30, 105),
            IM_COL32(6, 24, 22, 196),
            IM_COL32(74, 222, 128, 105),
            IM_COL32(240, 253, 244, 245),
            IM_COL32(134, 239, 172, 220),
            IM_COL32(167, 243, 208, 245),
            IM_COL32(236, 253, 245, 238)
        };
    }
    if (key == "amber") {
        return {
            IM_COL32(245, 158, 11, 255),
            IM_COL32(217, 119, 6, 255),
            IM_COL32(251, 191, 36, 255),
            IM_COL32(255, 200, 108, 255),
            IM_COL32(0, 0, 0, 210),
            IM_COL32(35, 23, 4, 170),
            IM_COL32(251, 191, 36, 72),
            IM_COL32(255, 247, 237, 238),
            IM_COL32(0, 0, 0, 190),
            IM_COL32(35, 23, 4, 115),
            IM_COL32(23, 14, 3, 200),
            IM_COL32(251, 191, 36, 112),
            IM_COL32(255, 251, 235, 245),
            IM_COL32(252, 211, 77, 220),
            IM_COL32(253, 230, 138, 245),
            IM_COL32(255, 251, 235, 238)
        };
    }
    if (key == "slate") {
        return {
            IM_COL32(148, 163, 184, 255),
            IM_COL32(100, 116, 139, 255),
            IM_COL32(203, 213, 225, 255),
            IM_COL32(190, 202, 220, 255),
            IM_COL32(0, 0, 0, 210),
            IM_COL32(16, 20, 24, 170),
            IM_COL32(148, 163, 184, 72),
            IM_COL32(241, 245, 249, 238),
            IM_COL32(0, 0, 0, 190),
            IM_COL32(16, 20, 24, 110),
            IM_COL32(10, 13, 17, 200),
            IM_COL32(203, 213, 225, 106),
            IM_COL32(255, 255, 255, 245),
            IM_COL32(186, 198, 216, 220),
            IM_COL32(226, 232, 240, 245),
            IM_COL32(226, 232, 240, 238)
        };
    }
    if (key == "sunset") {
        return {
            IM_COL32(251, 113, 133, 255),
            IM_COL32(219, 39, 119, 255),
            IM_COL32(244, 114, 182, 255),
            IM_COL32(255, 154, 173, 255),
            IM_COL32(0, 0, 0, 210),
            IM_COL32(33, 17, 37, 170),
            IM_COL32(244, 114, 182, 76),
            IM_COL32(253, 242, 248, 238),
            IM_COL32(0, 0, 0, 190),
            IM_COL32(33, 17, 37, 110),
            IM_COL32(20, 10, 22, 200),
            IM_COL32(244, 114, 182, 112),
            IM_COL32(255, 245, 250, 245),
            IM_COL32(249, 168, 212, 220),
            IM_COL32(251, 207, 232, 245),
            IM_COL32(253, 242, 248, 238)
        };
    }
    if (key == "frost") {
        return {
            IM_COL32(56, 189, 248, 255),
            IM_COL32(14, 165, 233, 255),
            IM_COL32(125, 211, 252, 255),
            IM_COL32(147, 224, 255, 255),
            IM_COL32(0, 0, 0, 210),
            IM_COL32(13, 22, 36, 166),
            IM_COL32(125, 211, 252, 76),
            IM_COL32(224, 242, 254, 238),
            IM_COL32(0, 0, 0, 190),
            IM_COL32(13, 22, 36, 108),
            IM_COL32(7, 14, 26, 198),
            IM_COL32(125, 211, 252, 110),
            IM_COL32(240, 249, 255, 245),
            IM_COL32(125, 211, 252, 220),
            IM_COL32(186, 230, 253, 245),
            IM_COL32(224, 242, 254, 238)
        };
    }

    return {
        IM_COL32(167, 125, 255, 255),
        IM_COL32(124, 101, 176, 255),
        IM_COL32(196, 181, 253, 255),
        IM_COL32(155, 220, 255, 255),
        IM_COL32(0, 0, 0, 210),
        IM_COL32(0, 0, 0, 145),
        IM_COL32(255, 255, 255, 44),
        IM_COL32(255, 255, 255, 235),
        IM_COL32(0, 0, 0, 180),
        IM_COL32(0, 0, 0, 90),
        IM_COL32(0, 0, 0, 185),
        IM_COL32(255, 255, 255, 90),
        IM_COL32(255, 255, 255, 245),
        IM_COL32(255, 210, 90, 220),
        IM_COL32(255, 226, 150, 245),
        IM_COL32(240, 240, 240, 235)
    };
}

// ===================== GLOBALS =====================
static bool g_running          = true;
static bool g_imguiInitialized = false;
static bool g_ShowMenu         = false;
static bool g_realGuiOpen      = false;
static int  g_imguiWarmupFrames = 0;
static bool g_imguiPhase1Done = false;

static SOCKET g_serverSocket = INVALID_SOCKET;
static SOCKET g_clientSocket = INVALID_SOCKET;

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
static bool        g_jniLookingAtEntity = false;
static bool        g_jniLookingAtEntityLatched = false;
static bool        g_jniBreakingBlock  = false;
static bool        g_jniHoldingBlock   = false;
static float       g_jniAttackCooldown = 1.0f;
static float       g_jniAttackCooldownPerTick = 0.08f;
static unsigned long long g_jniStateMs = 0;
static unsigned long long g_lastEntitySeenMs = 0;
static bool        g_loggedCooldownProgressResolve = false;
static bool        g_loggedCooldownPerTickResolve = false;
static bool        g_loggedCooldownProgressMissing = false;
static bool        g_loggedCooldownPerTickMissing = false;
static bool        g_loggedCooldownPlayerFieldMissing = false;
static bool        g_loggedCooldownPlayerObjectMissing = false;
static unsigned long long g_lastCooldownProgressFallbackLogMs = 0;
static unsigned long long g_lastCooldownPerTickFallbackLogMs = 0;
static unsigned long long g_lastCooldownSampleLogMs = 0;
static Mutex       g_jniStateMtx;
static Mutex       g_jniRemapMtx;
static std::string g_lastLoggedScreen;
static jfieldID    g_inGameHudField_121 = nullptr; // MinecraftClient.inGameHud
static std::vector<jfieldID> g_hudTextFields_121;  // InGameHud Text fields
static DWORD       g_lastHudTextProbeMs = 0;

// ===================== REACH MODULE JNI GLOBALS =====================
static bool g_reachJniInit = false;
static jmethodID g_getAttributes_121 = nullptr;
static jmethodID g_getCustomInstance_121 = nullptr;
static jmethodID g_setBaseValue_121 = nullptr;
static jobject g_reachRegistryEntry_121 = nullptr;

static bool g_velocityMethodsResolved = false;
static jmethodID g_getVelocity_121 = nullptr;
static jmethodID g_setVelocityVec_121 = nullptr;
static jmethodID g_setVelocityXYZ_121 = nullptr;
static jfieldID g_hurtTimeField_121 = nullptr;
static jmethodID g_vec3dCtor_121 = nullptr;
static int g_lastHurtTime_121 = 0;
static bool g_loggedVelocityResolveFail_121 = false;

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
    float  fov = 70.0f;
    bool   camFound = false;
    Matrix4x4 proj = {}, view = {};
    bool   matsOk = false;
};
static BgCamState  g_bgCamState = {};
static Mutex       g_bgCamMutex;

static jfieldID  g_optionsField_121 = nullptr; // MinecraftClient.options
static jfieldID  g_fovField_121     = nullptr; // GameOptions.fov (SimpleOption)
static jmethodID g_simpleOptionGet_121 = nullptr; // SimpleOption.getValue/get

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
static jmethodID g_setCustomNameVisible_121 = nullptr; // Entity.setCustomNameVisible(bool)
static jmethodID g_isCustomNameVisible_121 = nullptr;  // Entity.isCustomNameVisible()
static jmethodID g_shouldRenderName_121 = nullptr;      // Entity.shouldRenderName()
static jmethodID g_worldGetScoreboard_121 = nullptr;    // World.getScoreboard() -> Scoreboard
static jclass    g_scoreboardClass_121 = nullptr;
static jclass    g_teamClass_121 = nullptr;
static jclass    g_abstractTeamClass_121 = nullptr;
static jclass    g_visibilityRuleClass_121 = nullptr;
static jmethodID g_scoreboardGetTeam_121 = nullptr;            // Scoreboard.getTeam(String) -> Team
static jmethodID g_scoreboardAddTeam_121 = nullptr;            // Scoreboard.addTeam(String) -> Team
static jmethodID g_scoreboardRemoveTeam_121 = nullptr;         // Scoreboard.removeTeam(Team)
static jmethodID g_scoreboardAddHolderToTeam_121 = nullptr;    // Scoreboard.addScoreHolderToTeam(String, Team)
static jmethodID g_scoreboardGetHolderTeam_121 = nullptr;      // Scoreboard.getScoreHolderTeam(String) -> Team
static jmethodID g_scoreboardClearTeam_121 = nullptr;          // Scoreboard.clearTeam(String)
static jmethodID g_abstractTeamGetName_121 = nullptr;          // AbstractTeam.getName() -> String
static jmethodID g_teamSetNameTagVisibilityRule_121 = nullptr; // Team.setNameTagVisibilityRule(VisibilityRule)
static jobject   g_visibilityRuleNever_121 = nullptr;          // AbstractTeam.VisibilityRule.NEVER
static bool g_nametagSuppressionActive_121 = false;
static bool g_loggedNametagSuppressionUnavailable_121 = false;
static bool g_loggedNametagRestoreUnavailable_121 = false;
static std::unordered_map<std::string, std::string> g_hiddenNametagOriginalTeamByPlayer_121;
static jobject g_lastNametagSuppressionWorld_121 = nullptr;
static DWORD g_nextNametagSuppressionResolveRetryMs_121 = 0;
static int g_nametagSuppressionResolveRetryCount_121 = 0;
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

static std::string NormalizeNameSpaces(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool wasSpace = false;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 32) continue;
        bool isSpace = std::isspace(c) != 0;
        if (isSpace) {
            if (!wasSpace && !out.empty()) out.push_back(' ');
        } else {
            out.push_back((char)c);
        }
        wasSpace = isSpace;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static std::string StripMinecraftFormattingCodes(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC2 && i + 2 < in.size() && (unsigned char)in[i + 1] == 0xA7) {
            i += 2;
            continue;
        }
        if (c == 0xA7) {
            if (i + 1 < in.size()) i++;
            continue;
        }
        out.push_back((char)c);
    }
    return out;
}

static bool IsLikelyProfileName(const std::string& name) {
    if (name.size() < 3 || name.size() > 16) return false;
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c) || c == '_') continue;
        return false;
    }
    return true;
}

static bool LooksLikeFakePlayerLine(const std::string& rawName) {
    std::string name = NormalizeNameSpaces(StripMinecraftFormattingCodes(rawName));
    if (name.empty()) return true;

    bool anyAlnum = false;
    bool anyNonDecor = false;
    int alnumCount = 0;
    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c)) {
            anyAlnum = true;
            anyNonDecor = true;
            alnumCount++;
            continue;
        }
        if (ch == '_' || ch == ' ') {
            anyNonDecor = true;
            continue;
        }
        if (ch == '-' || ch == '=' || ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == '<' || ch == '>' || ch == '|' || ch == '*' || ch == '~' || ch == ':') {
            continue;
        }
        anyNonDecor = true;
    }

    if (!anyNonDecor) return true;
    if (!anyAlnum && name != "_") return true;
    if (alnumCount < 2 && !IsLikelyProfileName(name)) return true;
    return false;
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
    std::string cleanDisplay = NormalizeNameSpaces(StripMinecraftFormattingCodes(name));

    // If display name looks sane, use it directly.
    if (!LooksLikeFakePlayerLine(cleanDisplay)) return cleanDisplay;

    EnsureGameProfileCaches(env, playerObj);
    if (!g_getGameProfile_121 || !g_gameProfileGetName_121) return "";

    jobject gp = env->CallObjectMethod(playerObj, g_getGameProfile_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); gp = nullptr; }
    if (!gp) return "";
    jstring js = (jstring)env->CallObjectMethod(gp, g_gameProfileGetName_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); js = nullptr; }
    std::string profileName;
    if (js) {
        const char* cs = env->GetStringUTFChars(js, nullptr);
        if (cs) {
            profileName = cs;
            env->ReleaseStringUTFChars(js, cs);
        }
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(gp);
    trim(profileName);
    if (IsLikelyProfileName(profileName)) return profileName;

    // Last fallback: if cleaned display had useful content but profile name lookup failed,
    // keep the cleaned display rather than dropping everything.
    if (!cleanDisplay.empty() && !LooksLikeFakePlayerLine(cleanDisplay)) return cleanDisplay;
    return "";
}

// HitResult / crosshair target caches (for lookingAtBlock)
static jfieldID g_crosshairTargetField_121 = nullptr; // MinecraftClient.<hitResult>
static jclass   g_hitResultClass_121 = nullptr;       // net.minecraft.class_239
static jclass   g_blockHitResultClass_121 = nullptr;  // net.minecraft.class_3965

static void EnsureHitResultCaches(JNIEnv* env) {
    TRACE261_PATH("enter");
    bool ready = TRACE261_IF("prerequisitesMet", (env && g_gameClassLoader));
    if (!ready) return;
    if (!g_hitResultClass_121) {
        TRACE261_PATH("resolve-hitresult-class");
        const char* names[] = {
            "net.minecraft.class_239",
            "net.minecraft.world.phys.HitResult",
            "net.minecraft.util.hit.HitResult",
            nullptr
        };
        for (int i = 0; names[i] && !g_hitResultClass_121; i++) {
            jclass c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
            if (env->ExceptionCheck()) env->ExceptionClear();
            TRACE261_BRANCH("hitResultClassCandidateHit", c != nullptr);
            if (c) {
                g_hitResultClass_121 = (jclass)env->NewGlobalRef(c);
                TRACE261_VALUE("hitResultClassSource", names[i]);
                env->DeleteLocalRef(c);
            }
        }
    }
    if (!g_blockHitResultClass_121) {
        TRACE261_PATH("resolve-blockhitresult-class");
        // Yarn 1.21.x BlockHitResult is class_3965; deobf name included as fallback.
        const char* names[] = {
            "net.minecraft.class_3965",
            "net.minecraft.world.phys.BlockHitResult",
            "net.minecraft.util.hit.BlockHitResult",
            nullptr
        };
        for (int i = 0; names[i]; i++) {
            jclass c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
            if (env->ExceptionCheck()) env->ExceptionClear();
            TRACE261_BRANCH("blockHitResultClassCandidateHit", c != nullptr);
            if (c) {
                g_blockHitResultClass_121 = (jclass)env->NewGlobalRef(c);
                TRACE261_VALUE("blockHitResultClassSource", names[i]);
                env->DeleteLocalRef(c);
                break;
            }
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
        "()Lnet/minecraft/world/phys/HitResult$Type;", // Mojmap 1.20+
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

static int GetHitResultTypeOrdinal(JNIEnv* env, jobject hitObj) {
    if (!env || !hitObj) return -1;

    jmethodID mGetType = ResolveHitResultGetType(env);
    if (!mGetType) return -1;

    jobject typeObj = env->CallObjectMethod(hitObj, mGetType);
    if (env->ExceptionCheck()) { env->ExceptionClear(); typeObj = nullptr; }
    if (!typeObj) return -1;

    int ordinal = -1;
    jclass enumBase = env->FindClass("java/lang/Enum");
    jmethodID mOrdinal = enumBase ? env->GetMethodID(enumBase, "ordinal", "()I") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mOrdinal = nullptr; }
    if (mOrdinal) {
        ordinal = env->CallIntMethod(typeObj, mOrdinal);
        if (env->ExceptionCheck()) { env->ExceptionClear(); ordinal = -1; }
    }

    if (enumBase) env->DeleteLocalRef(enumBase);
    env->DeleteLocalRef(typeObj);
    return ordinal;
}

static void EnsureCrosshairTargetField(JNIEnv* env, jclass mcCls) {
    TRACE261_PATH("enter");
    bool prerequisites = TRACE261_IF("prerequisitesMet", (env && mcCls));
    TRACE261_BRANCH("alreadyResolved", g_crosshairTargetField_121 != nullptr);
    if (!prerequisites || g_crosshairTargetField_121) return;
    EnsureHitResultCaches(env);

    auto tryField = [&](const char* name, const char* sig) -> jfieldID {
        jfieldID f = env->GetFieldID(mcCls, name, sig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); f = nullptr; }
        return f;
    };

    // Fast path for common Yarn field name.
    jfieldID fid = tryField("field_1765", "Lnet/minecraft/class_239;");
    TRACE261_BRANCH("crosshairFastYarnHit", fid != nullptr);
    if (!fid) fid = tryField("field_1765", "Lnet/minecraft/world/phys/HitResult;");
    TRACE261_BRANCH("crosshairFastMojmapHit", fid != nullptr);

    // Some runtimes keep a readable name; the descriptor is still the obf HitResult type.
    if (!fid) {
        TRACE261_PATH("crosshair-known-name-signature-scan");
        const char* names[] = { "hitResult", "crosshairTarget", "field_1765", nullptr };
        const char* sigs[]  = { "Lnet/minecraft/world/phys/HitResult;", "Lnet/minecraft/class_239;", nullptr };
        for (int ni = 0; names[ni] && !fid; ni++) {
            for (int si = 0; sigs[si] && !fid; si++) {
                fid = tryField(names[ni], sigs[si]);
                TRACE261_BRANCH("crosshairKnownComboHit", fid != nullptr);
            }
        }
    }

    if (fid) g_crosshairTargetField_121 = fid;
    TRACE261_BRANCH("crosshairTargetResolved", g_crosshairTargetField_121 != nullptr);
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

static jclass g_chestBlockEntityClasses[4] = { nullptr, nullptr, nullptr, nullptr };

static bool IsChestLikeToken(const std::string& valueLower) {
    return valueLower.find("chest") != std::string::npos
        || valueLower.find("barrel") != std::string::npos
        || valueLower.find("shulker") != std::string::npos;
}

static void EnsureChestStateDetectionCaches(JNIEnv* env, jobject be) {
    if (!env) return;

    if (!g_blockStateClass_121) {
        const char* stateNames[] = {
            "net.minecraft.class_2680",
            "net.minecraft.world.level.block.state.BlockState",
            "net.minecraft.block.BlockState",
            nullptr
        };
        for (int i = 0; stateNames[i] && !g_blockStateClass_121; i++) {
            jclass c = nullptr;
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, stateNames[i]);
            if (!c) {
                std::string alt = stateNames[i];
                std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
            if (c) { g_blockStateClass_121 = (jclass)env->NewGlobalRef(c); env->DeleteLocalRef(c); }
        }
    }
    if (!g_blockClass_121) {
        const char* blockNames[] = {
            "net.minecraft.class_2248",
            "net.minecraft.world.level.block.Block",
            "net.minecraft.block.Block",
            nullptr
        };
        for (int i = 0; blockNames[i] && !g_blockClass_121; i++) {
            jclass c = nullptr;
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, blockNames[i]);
            if (!c) {
                std::string alt = blockNames[i];
                std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
            if (c) { g_blockClass_121 = (jclass)env->NewGlobalRef(c); env->DeleteLocalRef(c); }
        }
    }

    if (be && !g_beGetCachedState_121) {
        jclass beCls = env->GetObjectClass(be);
        if (beCls) {
            const char* names[] = { "getCachedState", "method_11010", nullptr };
            const char* sigs[] = {
                "()Lnet/minecraft/class_2680;",
                "()Lnet/minecraft/world/level/block/state/BlockState;",
                "()Lnet/minecraft/block/BlockState;",
                nullptr
            };
            for (int ni = 0; names[ni] && !g_beGetCachedState_121; ni++) {
                for (int si = 0; sigs[si] && !g_beGetCachedState_121; si++) {
                    g_beGetCachedState_121 = env->GetMethodID(beCls, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_beGetCachedState_121 = nullptr; }
                }
            }
            if (!g_beGetCachedState_121 && g_blockStateClass_121) {
                g_beGetCachedState_121 = FindZeroArgMethodReturningClass(env, beCls, g_blockStateClass_121, nullptr, nullptr, "()Ljava/lang/Object;");
            }
            env->DeleteLocalRef(beCls);
        }
    }

    if (!g_stateGetBlock_121 && g_blockStateClass_121) {
        const char* names[] = { "getBlock", "method_26204", nullptr };
        const char* sigs[] = {
            "()Lnet/minecraft/class_2248;",
            "()Lnet/minecraft/world/level/block/Block;",
            "()Lnet/minecraft/block/Block;",
            nullptr
        };
        for (int ni = 0; names[ni] && !g_stateGetBlock_121; ni++) {
            for (int si = 0; sigs[si] && !g_stateGetBlock_121; si++) {
                g_stateGetBlock_121 = env->GetMethodID(g_blockStateClass_121, names[ni], sigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_stateGetBlock_121 = nullptr; }
            }
        }
        if (!g_stateGetBlock_121 && g_blockClass_121) {
            g_stateGetBlock_121 = FindZeroArgMethodReturningClass(env, g_blockStateClass_121, g_blockClass_121, nullptr, nullptr, "()Ljava/lang/Object;");
        }
    }

    if (!g_blockGetTranslationKey_121 && g_blockClass_121) {
        const char* names[] = { "getTranslationKey", "getDescriptionId", "method_9518", nullptr };
        for (int i = 0; names[i] && !g_blockGetTranslationKey_121; i++) {
            g_blockGetTranslationKey_121 = env->GetMethodID(g_blockClass_121, names[i], "()Ljava/lang/String;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockGetTranslationKey_121 = nullptr; }
        }
    }
}

static bool IsChestBlockEntity(JNIEnv* env, jobject be) {
    if (!env || !be) return false;
    
    static bool init = false;
    static bool s_initLogged = false;
    if (!init) {
        init = true;
        const char* yarn[] = { "net.minecraft.class_2595", "net.minecraft.class_2611", "net.minecraft.class_3719", "net.minecraft.class_2627" };
        const char* moj[]  = { "net.minecraft.world.level.block.entity.ChestBlockEntity", "net.minecraft.world.level.block.entity.EnderChestBlockEntity", "net.minecraft.world.level.block.entity.BarrelBlockEntity", "net.minecraft.world.level.block.entity.ShulkerBoxBlockEntity" };
        
        for (int i = 0; i < 4; i++) {
            jclass c = nullptr;
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, yarn[i]);
            if (!c) c = LoadClassWithLoader(env, g_gameClassLoader, moj[i]);
            if (!c) {
                std::string alt = yarn[i]; std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
            if (!c) {
                std::string alt = moj[i]; std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
            if (c) { g_chestBlockEntityClasses[i] = (jclass)env->NewGlobalRef(c); env->DeleteLocalRef(c); }
        }
        Log("IsChestBlockEntity init: classes loaded = " + std::to_string(g_chestBlockEntityClasses[0] ? 1 : 0) + "," + std::to_string(g_chestBlockEntityClasses[1] ? 1 : 0) + "," + std::to_string(g_chestBlockEntityClasses[2] ? 1 : 0) + "," + std::to_string(g_chestBlockEntityClasses[3] ? 1 : 0));
    }

    // Diagnostic: log BE class name on first call
    if (!s_initLogged) {
        s_initLogged = true;
        jclass beCls = env->GetObjectClass(be);
        std::string beClsName = beCls ? GetClassNameFromClass(env, beCls) : "null";
        if (beCls) env->DeleteLocalRef(beCls);
        Log("IsChestBlockEntity first BE class: " + beClsName);
    }
    
    for (int i = 0; i < 4; i++) {
        if (g_chestBlockEntityClasses[i] && env->IsInstanceOf(be, g_chestBlockEntityClasses[i])) return true;
    }

    // Fallback: detect chest-like block entities via BlockState -> Block translation key/class name.
    // This keeps Chest ESP resilient when class IDs shift across client remaps.
    EnsureChestStateDetectionCaches(env, be);
    if (g_beGetCachedState_121 && g_stateGetBlock_121) {
        jobject stateObj = env->CallObjectMethod(be, g_beGetCachedState_121);
        if (env->ExceptionCheck()) { env->ExceptionClear(); stateObj = nullptr; }
        if (stateObj) {
            jobject blockObj = env->CallObjectMethod(stateObj, g_stateGetBlock_121);
            if (env->ExceptionCheck()) { env->ExceptionClear(); blockObj = nullptr; }
            if (blockObj) {
                bool matched = false;

                if (g_blockGetTranslationKey_121) {
                    jstring jKey = (jstring)env->CallObjectMethod(blockObj, g_blockGetTranslationKey_121);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); jKey = nullptr; }
                    if (jKey) {
                        const char* cKey = env->GetStringUTFChars(jKey, nullptr);
                        std::string key = cKey ? cKey : "";
                        if (cKey) env->ReleaseStringUTFChars(jKey, cKey);
                        env->DeleteLocalRef(jKey);
                        matched = IsChestLikeToken(ToLowerAscii(key));
                    }
                }

                if (!matched) {
                    jclass blkCls = env->GetObjectClass(blockObj);
                    if (!env->ExceptionCheck() && blkCls) {
                        std::string clsName = GetClassNameFromClass(env, blkCls);
                        matched = IsChestLikeToken(ToLowerAscii(clsName));
                        env->DeleteLocalRef(blkCls);
                    } else {
                        env->ExceptionClear();
                    }
                }

                env->DeleteLocalRef(blockObj);
                env->DeleteLocalRef(stateObj);
                if (matched) return true;
            } else {
                env->DeleteLocalRef(stateObj);
            }
        }
    }

    return false;
}

// Global cached player and its Reach attribute instance
static jobject g_cachedLocalPlayer = nullptr;
static jobject g_cachedReachAttrInst = nullptr;

static bool g_reachMethodsResolved = false;
static jmethodID g_dynGetAttributes = nullptr;   // class_1309 -> class_5131
static jmethodID g_dynGetCustomInstance = nullptr; // class_5131 (class_6880) -> class_1324
static jmethodID g_dynGetAttributeInstance = nullptr; // class_5131 (class_6880) -> class_1324 (fallback, e.g. method_55698)
static jmethodID g_dynRegistryEntryToString = nullptr; // class_6880 () -> String
static jmethodID g_dynRegistryEntryMatchesIdentifier = nullptr; // class_6880 (class_2960) -> boolean
static jmethodID g_dynSetBaseValue = nullptr;    // class_1324 (D)V
static jclass    g_identifierClass_121 = nullptr; // net.minecraft.class_2960
static jmethodID g_identifierFromString_121 = nullptr; // Identifier.parse-like static method
static jobject   g_entityReachIdentifier_121 = nullptr; // minecraft:player.entity_interaction_range
static jobject   g_blockReachIdentifier_121 = nullptr; // minecraft:player.block_interaction_range

static jmethodID FindMethodBySignature(JNIEnv* env, jclass tgtCls, const std::string& retTypeStr, int paramCount, const std::string& p1TypeStr = "") {
    jclass cClass = env->FindClass("java/lang/Class");
    jclass cMethod = env->FindClass("java/lang/reflect/Method");
    jmethodID mGetMethods = env->GetMethodID(cClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jmethodID mGetName = env->GetMethodID(cMethod, "getName", "()Ljava/lang/String;");
    jmethodID mGetRetType = env->GetMethodID(cMethod, "getReturnType", "()Ljava/lang/Class;");
    jmethodID mGetParamTypes = env->GetMethodID(cMethod, "getParameterTypes", "()[Ljava/lang/Class;");

    jobjectArray methods = (jobjectArray)env->CallObjectMethod(tgtCls, mGetMethods);
    if (env->ExceptionCheck()) { env->ExceptionClear(); methods = nullptr; }
    if (!methods) return nullptr;
    
    jsize count = env->GetArrayLength(methods);
    jmethodID found = nullptr;
    
    for (int i=0; i<count; ++i) {
        jobject m = env->GetObjectArrayElement(methods, i);
        if (!m) continue;
        jclass rType = (jclass)env->CallObjectMethod(m, mGetRetType);
        if (env->ExceptionCheck()) { env->ExceptionClear(); rType = nullptr; }
        jobjectArray pTypes = (jobjectArray)env->CallObjectMethod(m, mGetParamTypes);
        if (env->ExceptionCheck()) { env->ExceptionClear(); pTypes = nullptr; }
        jsize pCount = pTypes ? env->GetArrayLength(pTypes) : 0;
        
        if (pCount == paramCount) {
            std::string rName = rType ? GetClassNameFromClass(env, rType) : "";
            if (rName == retTypeStr || (retTypeStr == "V" && rName == "void") || (retTypeStr == "D" && rName == "double") || (retTypeStr == "Z" && rName == "boolean")) {
                bool pMatch = true;
                if (paramCount == 1 && !p1TypeStr.empty() && pTypes) {
                    jclass p1Cls = (jclass)env->GetObjectArrayElement(pTypes, 0);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); p1Cls = nullptr; }
                    std::string p1Name = p1Cls ? GetClassNameFromClass(env, p1Cls) : "";
                    if (p1Name != p1TypeStr && !(p1TypeStr == "D" && p1Name == "double")) {
                        pMatch = false;
                    }
                    if (p1Cls) env->DeleteLocalRef(p1Cls);
                }
                if (pMatch) {
                    jstring nameStr = (jstring)env->CallObjectMethod(m, mGetName);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); nameStr = nullptr; }
                    if (nameStr) {
                        const char* nameC = env->GetStringUTFChars(nameStr, nullptr);
                        
                        std::string sig = "(";
                        if (paramCount == 1 && p1TypeStr == "D") sig += "D";
                        else if (paramCount == 1) {
                            std::string fixedP = p1TypeStr;
                            for(size_t pos=0; pos<fixedP.length(); pos++) { if (fixedP[pos] == '.') fixedP[pos] = '/'; }
                            sig += "L" + fixedP + ";";
                        }
                        sig += ")";
                        
                        if (retTypeStr == "V") sig += "V";
                        else if (retTypeStr == "D") sig += "D";
                        else if (retTypeStr == "Z") sig += "Z";
                        else {
                            std::string fixedR = retTypeStr;
                            for(size_t pos=0; pos<fixedR.length(); pos++) { if (fixedR[pos] == '.') fixedR[pos] = '/'; }
                            sig += "L" + fixedR + ";";
                        }
                        
                        found = env->GetMethodID(tgtCls, nameC, sig.c_str());
                        if (env->ExceptionCheck()) { env->ExceptionClear(); found = nullptr; }
                        
                        if (found) Log("FindMethodBySignature: Found " + std::string(nameC) + sig);
                        
                        env->ReleaseStringUTFChars(nameStr, nameC);
                        env->DeleteLocalRef(nameStr);
                    }
                }
            }
        }
        if (rType) env->DeleteLocalRef(rType);
        if (pTypes) env->DeleteLocalRef(pTypes);
        env->DeleteLocalRef(m);
        if (found) break;
    }
    env->DeleteLocalRef(methods);
    env->DeleteLocalRef(cMethod);
    env->DeleteLocalRef(cClass);
    return found;
}

static void EnsureClosestPlayerCaches(JNIEnv* env);
static void EnsureEntityMethods(JNIEnv* env, jobject entObj);
static void DiscoverWorldPlayersListField(JNIEnv* env, jobject worldObj);
static bool AreNametagSuppressionCoreMappingsReady121();
static bool AreNametagSuppressionRestoreMappingsReady121();
static void LogNametagSuppressionMissingMappings121(JNIEnv* env, jobject worldObj);
static void ResetNametagSuppressionCaches121(JNIEnv* env, const char* reason);
static void ResetModernJniRuntimeCaches121(JNIEnv* env, const char* reason);
static bool TrackSuppressionWorldContext121(JNIEnv* env, jobject worldObj);
static bool EnsureNametagSuppressionTeamMappings121(JNIEnv* env, jobject worldObj);
static jobject GetScoreboard121(JNIEnv* env, jobject worldObj);
static jobject EnsureNametagHideTeam121(JNIEnv* env, jobject scoreboardObj);
static bool ApplyVanillaNametagSuppression121(JNIEnv* env, jobject scoreboardObj, jobject hideTeamObj, const std::string& playerName);
static void RestoreVanillaNametagSuppression121(JNIEnv* env, jobject scoreboardObj);

static std::string NormalizeReachKey(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (std::isalnum((unsigned char)ch)) out.push_back((char)std::tolower((unsigned char)ch));
    }
    return out;
}

static bool IsBlockReachKey(const std::string& raw) {
    std::string normalized = NormalizeReachKey(raw);
    return normalized.find("blockinteractionrange") != std::string::npos;
}

static int ScoreEntityReachKey(const std::string& raw) {
    std::string normalized = NormalizeReachKey(raw);
    if (normalized.find("entityinteractionrange") != std::string::npos) return 220;
    if (normalized.find("playerentityinteractionrange") != std::string::npos) return 220;
    if (normalized.find("attackrange") != std::string::npos) return 140;
    if (normalized.find("interactionrange") != std::string::npos && normalized.find("entity") != std::string::npos) return 100;
    return -1;
}

static std::string SafeObjectToString(JNIEnv* env, jobject obj) {
    if (!env || !obj) return "";
    jclass objCls = env->FindClass("java/lang/Object");
    if (env->ExceptionCheck()) { env->ExceptionClear(); objCls = nullptr; }
    if (!objCls) return "";

    jmethodID mToStr = env->GetMethodID(objCls, "toString", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); mToStr = nullptr; }
    if (!mToStr) {
        env->DeleteLocalRef(objCls);
        return "";
    }

    jstring js = (jstring)env->CallObjectMethod(obj, mToStr);
    if (env->ExceptionCheck()) { env->ExceptionClear(); js = nullptr; }
    std::string out;
    if (js) {
        const char* cstr = env->GetStringUTFChars(js, nullptr);
        out = cstr ? cstr : "";
        if (cstr) env->ReleaseStringUTFChars(js, cstr);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(objCls);
    return out;
}

static void EnsureReachIdentifiers(JNIEnv* env) {
    if (!env || !g_gameClassLoader) return;

    if (!g_identifierClass_121) {
        jclass idCls = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2960");
        if (!idCls) {
            idCls = env->FindClass("net/minecraft/class_2960");
            if (env->ExceptionCheck()) { env->ExceptionClear(); idCls = nullptr; }
        }
        if (idCls) {
            g_identifierClass_121 = (jclass)env->NewGlobalRef(idCls);
            env->DeleteLocalRef(idCls);
        }
    }
    if (!g_identifierClass_121) return;

    if (!g_identifierFromString_121) {
        const char* names[] = {
            "method_60656", "method_12829", "method_60654",
            "method_45136", "method_45138", "method_48331",
            "a", "b", "c", "e", "f", "g",
            nullptr
        };

        for (int i = 0; names[i]; ++i) {
            jmethodID mid = env->GetStaticMethodID(g_identifierClass_121, names[i], "(Ljava/lang/String;)Lnet/minecraft/class_2960;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
            if (!mid) continue;

            jstring js = env->NewStringUTF("minecraft:player.entity_interaction_range");
            jobject testId = env->CallStaticObjectMethod(g_identifierClass_121, mid, js);
            if (env->ExceptionCheck()) { env->ExceptionClear(); testId = nullptr; }
            env->DeleteLocalRef(js);
            if (!testId) continue;

            std::string parsed = SafeObjectToString(env, testId);
            env->DeleteLocalRef(testId);
            if (ScoreEntityReachKey(parsed) >= 0) {
                g_identifierFromString_121 = mid;
                Log("EnsureReachIdentifiers: selected parser " + std::string(names[i]) + " -> " + parsed);
                break;
            }
        }

        if (!g_identifierFromString_121) Log("EnsureReachIdentifiers: FAILED to resolve Identifier parse method.");
    }
    if (!g_identifierFromString_121) return;

    auto makeId = [&](const char* value) -> jobject {
        jstring js = env->NewStringUTF(value);
        jobject idObj = env->CallStaticObjectMethod(g_identifierClass_121, g_identifierFromString_121, js);
        if (env->ExceptionCheck()) { env->ExceptionClear(); idObj = nullptr; }
        env->DeleteLocalRef(js);
        return idObj;
    };

    if (!g_entityReachIdentifier_121) {
        jobject id = makeId("minecraft:player.entity_interaction_range");
        if (!id) id = makeId("player.entity_interaction_range");
        if (id) {
            Log("EnsureReachIdentifiers: entity identifier = " + SafeObjectToString(env, id));
            g_entityReachIdentifier_121 = env->NewGlobalRef(id);
            env->DeleteLocalRef(id);
        } else {
            Log("EnsureReachIdentifiers: FAILED to create entity reach identifier.");
        }
    }

    if (!g_blockReachIdentifier_121) {
        jobject id = makeId("minecraft:player.block_interaction_range");
        if (!id) id = makeId("player.block_interaction_range");
        if (id) {
            Log("EnsureReachIdentifiers: block identifier = " + SafeObjectToString(env, id));
            g_blockReachIdentifier_121 = env->NewGlobalRef(id);
            env->DeleteLocalRef(id);
        } else {
            Log("EnsureReachIdentifiers: FAILED to create block reach identifier.");
        }
    }
}

static jobject CallReachAttributeLookup(JNIEnv* env, jobject attrCont, jobject registryEntry) {
    if (!env || !attrCont || !registryEntry) return nullptr;

    jobject inst = nullptr;
    if (g_dynGetCustomInstance) {
        inst = env->CallObjectMethod(attrCont, g_dynGetCustomInstance, registryEntry);
        if (env->ExceptionCheck()) { env->ExceptionClear(); inst = nullptr; }
    }

    if (!inst && g_dynGetAttributeInstance && g_dynGetAttributeInstance != g_dynGetCustomInstance) {
        inst = env->CallObjectMethod(attrCont, g_dynGetAttributeInstance, registryEntry);
        if (env->ExceptionCheck()) { env->ExceptionClear(); inst = nullptr; }
    }

    return inst;
}

static jobject TryResolveReachAttributeFromRegistry(JNIEnv* env, jobject attrCont) {
    if (!env || !attrCont || !g_gameClassLoader || (!g_dynGetCustomInstance && !g_dynGetAttributeInstance)) return nullptr;
    EnsureReachIdentifiers(env);

    jclass attrsCls = nullptr;
    const char* attrsNames[] = { "net.minecraft.world.entity.ai.attributes.Attributes", "net.minecraft.class_5134", nullptr };
    for (int i = 0; attrsNames[i] && !attrsCls; i++) {
        attrsCls = LoadClassWithLoader(env, g_gameClassLoader, attrsNames[i]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); attrsCls = nullptr; }
    }
    if (!attrsCls) return nullptr;

    jclass objCls = env->FindClass("java/lang/Object");
    if (env->ExceptionCheck()) { env->ExceptionClear(); objCls = nullptr; }
    if (!objCls) {
        env->DeleteLocalRef(attrsCls);
        return nullptr;
    }

    jmethodID mToStr = env->GetMethodID(objCls, "toString", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); mToStr = nullptr; }
    if (!mToStr) {
        env->DeleteLocalRef(objCls);
        env->DeleteLocalRef(attrsCls);
        return nullptr;
    }

    jobject bestInst = nullptr;
    int bestScore = -1;
    std::string bestLabel;
    std::vector<std::string> sampledLabels;

    jclass cClass = env->FindClass("java/lang/Class");
    jclass cField = env->FindClass("java/lang/reflect/Field");
    jclass cMod = env->FindClass("java/lang/reflect/Modifier");
    jmethodID mGetFields = cClass ? env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
    jmethodID mFType = cField ? env->GetMethodID(cField, "getType", "()Ljava/lang/Class;") : nullptr;
    jmethodID mFMod = cField ? env->GetMethodID(cField, "getModifiers", "()I") : nullptr;
    jmethodID mFGet = cField ? env->GetMethodID(cField, "get", "(Ljava/lang/Object;)Ljava/lang/Object;") : nullptr;
    jmethodID mIsStatic = cMod ? env->GetStaticMethodID(cMod, "isStatic", "(I)Z") : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mGetFields = mFType = mFMod = mFGet = mIsStatic = nullptr;
    }

    jobjectArray fields = nullptr;
    jclass regEntryCls = nullptr;
    int scannedFields = 0;
    int scannedEntries = 0;
    if (mGetFields && mFType && mFMod && mFGet && mIsStatic) {
        fields = (jobjectArray)env->CallObjectMethod(attrsCls, mGetFields);
        if (env->ExceptionCheck()) { env->ExceptionClear(); fields = nullptr; }
        const char* regEntryNames[] = { "net.minecraft.core.Holder", "net.minecraft.class_6880", nullptr };
        for (int i = 0; regEntryNames[i] && !regEntryCls; i++) {
            regEntryCls = LoadClassWithLoader(env, g_gameClassLoader, regEntryNames[i]);
            if (env->ExceptionCheck()) { env->ExceptionClear(); regEntryCls = nullptr; }
        }
        if (fields && regEntryCls) {
            jsize fc = env->GetArrayLength(fields);
            for (int i = 0; i < fc; ++i) {
                jobject fld = env->GetObjectArrayElement(fields, i);
                if (!fld) continue;
                ++scannedFields;

                jint mod = env->CallIntMethod(fld, mFMod);
                if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
                bool isStatic = (env->CallStaticBooleanMethod(cMod, mIsStatic, mod) == JNI_TRUE);
                if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(fld); continue; }
                if (!isStatic) { env->DeleteLocalRef(fld); continue; }

                jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
                if (env->ExceptionCheck()) { env->ExceptionClear(); ft = nullptr; }
                bool isRegistryEntry = (ft && env->IsAssignableFrom(ft, regEntryCls));
                if (ft) env->DeleteLocalRef(ft);
                if (!isRegistryEntry) { env->DeleteLocalRef(fld); continue; }

                jobject registryEntry = env->CallObjectMethod(fld, mFGet, nullptr);
                if (env->ExceptionCheck()) { env->ExceptionClear(); registryEntry = nullptr; }
                if (!registryEntry) { env->DeleteLocalRef(fld); continue; }
                ++scannedEntries;

                bool isEntityReachEntry = false;
                bool isBlockReachEntry = false;
                if (g_dynRegistryEntryMatchesIdentifier) {
                    if (g_entityReachIdentifier_121) {
                        isEntityReachEntry = (env->CallBooleanMethod(registryEntry, g_dynRegistryEntryMatchesIdentifier, g_entityReachIdentifier_121) == JNI_TRUE);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); isEntityReachEntry = false; }
                    }
                    if (g_blockReachIdentifier_121) {
                        isBlockReachEntry = (env->CallBooleanMethod(registryEntry, g_dynRegistryEntryMatchesIdentifier, g_blockReachIdentifier_121) == JNI_TRUE);
                        if (env->ExceptionCheck()) { env->ExceptionClear(); isBlockReachEntry = false; }
                    }
                }

                if (isBlockReachEntry) {
                    env->DeleteLocalRef(registryEntry);
                    env->DeleteLocalRef(fld);
                    continue;
                }

                if (isEntityReachEntry) {
                    jobject inst = CallReachAttributeLookup(env, attrCont, registryEntry);
                    if (inst) {
                        if (bestInst) env->DeleteLocalRef(bestInst);
                        bestInst = inst;
                        bestScore = 1000;
                        bestLabel = "identifier:player.entity_interaction_range";
                    }
                    env->DeleteLocalRef(registryEntry);
                    env->DeleteLocalRef(fld);
                    continue;
                }

                std::string entryKey;
                if (g_dynRegistryEntryToString) {
                    jstring jsKey = (jstring)env->CallObjectMethod(registryEntry, g_dynRegistryEntryToString);
                    if (!env->ExceptionCheck() && jsKey) {
                        const char* cstr = env->GetStringUTFChars(jsKey, nullptr);
                        entryKey = cstr ? cstr : "";
                        if (cstr) env->ReleaseStringUTFChars(jsKey, cstr);
                        env->DeleteLocalRef(jsKey);
                    } else {
                        env->ExceptionClear();
                    }
                }

                std::string lookupLabel = entryKey;
                if (lookupLabel.empty()) {
                    jstring jsEntry = (jstring)env->CallObjectMethod(registryEntry, mToStr);
                    if (!env->ExceptionCheck() && jsEntry) {
                        const char* cstr = env->GetStringUTFChars(jsEntry, nullptr);
                        lookupLabel = cstr ? cstr : "";
                        if (cstr) env->ReleaseStringUTFChars(jsEntry, cstr);
                        env->DeleteLocalRef(jsEntry);
                    } else {
                        env->ExceptionClear();
                    }
                }

                jobject instCandidate = nullptr;
                std::string instanceLabel;
                if (lookupLabel.empty()) {
                    instCandidate = CallReachAttributeLookup(env, attrCont, registryEntry);
                    if (instCandidate) instanceLabel = SafeObjectToString(env, instCandidate);
                }

                if (!lookupLabel.empty() && sampledLabels.size() < 16) sampledLabels.push_back(lookupLabel);
                if (!instanceLabel.empty() && sampledLabels.size() < 16) sampledLabels.push_back("inst:" + instanceLabel);
                if (IsBlockReachKey(lookupLabel)) {
                    if (instCandidate) env->DeleteLocalRef(instCandidate);
                    env->DeleteLocalRef(registryEntry);
                    env->DeleteLocalRef(fld);
                    continue;
                }

                int score = ScoreEntityReachKey(lookupLabel);
                if (score < 0 && !instanceLabel.empty()) score = ScoreEntityReachKey(instanceLabel);
                if (score >= 0) {
                    jobject inst = instCandidate;
                    if (!inst) {
                        inst = CallReachAttributeLookup(env, attrCont, registryEntry);
                    }
                    if (inst && score > bestScore) {
                        if (bestInst) env->DeleteLocalRef(bestInst);
                        bestInst = inst;
                        bestScore = score;
                        bestLabel = !lookupLabel.empty() ? lookupLabel : instanceLabel;
                    } else if (inst) {
                        env->DeleteLocalRef(inst);
                    }
                } else if (instCandidate) {
                    env->DeleteLocalRef(instCandidate);
                }

                env->DeleteLocalRef(registryEntry);
                env->DeleteLocalRef(fld);
            }
        }
    }

    if (bestInst && bestScore >= 0) {
        Log("UpdateReach: Registry-selected entity reach attribute (" + std::to_string(bestScore) + "): " + bestLabel);
    } else if (bestInst) {
        env->DeleteLocalRef(bestInst);
        bestInst = nullptr;
    } else if (!sampledLabels.empty()) {
        std::string joined = sampledLabels[0];
        for (size_t i = 1; i < sampledLabels.size(); ++i) joined += " | " + sampledLabels[i];
        Log("UpdateReach: Registry entries scanned (entity reach not found): " + joined);
    } else {
        Log("UpdateReach: Registry scan produced no usable labels.");
    }

    if (!bestInst) {
        Log("UpdateReach: scanned " + std::to_string(scannedFields) + " fields, " + std::to_string(scannedEntries) + " registry entries.");
    }

    if (regEntryCls) env->DeleteLocalRef(regEntryCls);
    if (fields) env->DeleteLocalRef(fields);
    if (cMod) env->DeleteLocalRef(cMod);
    if (cField) env->DeleteLocalRef(cField);
    if (cClass) env->DeleteLocalRef(cClass);
    env->DeleteLocalRef(objCls);
    env->DeleteLocalRef(attrsCls);
    return bestInst;
}

static void EnsureReachJni(JNIEnv* env) {
    if (g_reachJniInit) return;
    g_reachJniInit = true;

    Log("EnsureReachJni: Resolving dynamic Reach methods");
    if (!g_gameClassLoader) return;

    // 1. LivingEntity -> getAttributes
    const char* livingEntNames[] = { "net.minecraft.world.entity.LivingEntity", "net.minecraft.class_1309", nullptr };
    jclass livingEntCls = nullptr;
    for (int i = 0; livingEntNames[i] && !livingEntCls; i++) {
        livingEntCls = LoadClassWithLoader(env, g_gameClassLoader, livingEntNames[i]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); livingEntCls = nullptr; }
    }
    if (livingEntCls) {
        g_dynGetAttributes = FindMethodBySignature(env, livingEntCls, "net.minecraft.world.entity.ai.attributes.AttributeMap", 0);
        if (!g_dynGetAttributes) g_dynGetAttributes = FindMethodBySignature(env, livingEntCls, "net.minecraft.class_5131", 0);
        env->DeleteLocalRef(livingEntCls);
        if (!g_dynGetAttributes) Log("EnsureReachJni: FAILED to find getAttributes");
    }

    // 2. AttributeContainer -> getCustomInstance(RegistryEntry<EntityAttribute>)
    const char* attrContNames[] = { "net.minecraft.world.entity.ai.attributes.AttributeMap", "net.minecraft.class_5131", nullptr };
    jclass attrContCls = nullptr;
    for (int i = 0; attrContNames[i] && !attrContCls; i++) {
        attrContCls = LoadClassWithLoader(env, g_gameClassLoader, attrContNames[i]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); attrContCls = nullptr; }
    }
    if (attrContCls) {
        g_dynGetCustomInstance = FindMethodBySignature(env, attrContCls, "net.minecraft.world.entity.ai.attributes.AttributeInstance", 1, "net.minecraft.core.Holder");
        if (!g_dynGetCustomInstance) g_dynGetCustomInstance = FindMethodBySignature(env, attrContCls, "net.minecraft.class_1324", 1, "net.minecraft.class_6880");

        const char* getterNames[] = { "method_55698", "getAttributeInstance", "getInstance", "getValue", nullptr };
        const char* getterSigs[] = { "(Lnet/minecraft/core/Holder;)Lnet/minecraft/world/entity/ai/attributes/AttributeInstance;", "(Lnet/minecraft/class_6880;)Lnet/minecraft/class_1324;", nullptr };
        for (int ni = 0; getterNames[ni] && !g_dynGetAttributeInstance; ni++) {
            for (int si = 0; getterSigs[si] && !g_dynGetAttributeInstance; si++) {
                g_dynGetAttributeInstance = env->GetMethodID(attrContCls, getterNames[ni], getterSigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_dynGetAttributeInstance = nullptr; }
            }
        }
        env->DeleteLocalRef(attrContCls);
    }

    // 3. AttributeInstance -> setBaseValue
    const char* attrInstNames[] = { "net.minecraft.world.entity.ai.attributes.AttributeInstance", "net.minecraft.class_1324", nullptr };
    jclass attrInstCls = nullptr;
    for (int i = 0; attrInstNames[i] && !attrInstCls; i++) {
        attrInstCls = LoadClassWithLoader(env, g_gameClassLoader, attrInstNames[i]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); attrInstCls = nullptr; }
    }
    if (attrInstCls) {
        g_dynSetBaseValue = env->GetMethodID(attrInstCls, "setBaseValue", "(D)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_dynSetBaseValue = nullptr; }
        if (!g_dynSetBaseValue) g_dynSetBaseValue = FindMethodBySignature(env, attrInstCls, "V", 1, "D");
        env->DeleteLocalRef(attrInstCls);
    }

    const char* holderNames[] = { "net.minecraft.core.Holder", "net.minecraft.class_6880", nullptr };
    jclass regEntryCls = nullptr;
    for (int i = 0; holderNames[i] && !regEntryCls; i++) {
        regEntryCls = LoadClassWithLoader(env, g_gameClassLoader, holderNames[i]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); regEntryCls = nullptr; }
    }
    if (regEntryCls) {
        g_dynRegistryEntryToString = env->GetMethodID(regEntryCls, "toString", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_dynRegistryEntryToString = nullptr; }
        if (!g_dynRegistryEntryToString) g_dynRegistryEntryToString = FindMethodBySignature(env, regEntryCls, "java.lang.String", 0);

        g_dynRegistryEntryMatchesIdentifier = env->GetMethodID(regEntryCls, "is", "(Lnet/minecraft/resources/ResourceLocation;)Z");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_dynRegistryEntryMatchesIdentifier = nullptr; }
        if (!g_dynRegistryEntryMatchesIdentifier) g_dynRegistryEntryMatchesIdentifier = FindMethodBySignature(env, regEntryCls, "Z", 1, "net.minecraft.resources.ResourceLocation");
        if (!g_dynRegistryEntryMatchesIdentifier) g_dynRegistryEntryMatchesIdentifier = FindMethodBySignature(env, regEntryCls, "Z", 1, "net.minecraft.class_2960");
        env->DeleteLocalRef(regEntryCls);
    }
    if (g_dynGetAttributes && (g_dynGetCustomInstance || g_dynGetAttributeInstance) && g_dynSetBaseValue) {
        g_reachMethodsResolved = true;
        Log("EnsureReachJni: SUCCESS - All Reach methods resolved dynamically.");
    } else {
        Log("EnsureReachJni: FAILURE - Missing dynamic methods.");
    }
}

static void UpdateReach(JNIEnv* env, const Config& cfg) {
    if (!g_stateJniReady) return;
    
    EnsureReachJni(env);
    if (!g_reachMethodsResolved) return;
    
    if (!g_mcInstance || !g_playerField_121) return;
    
    jobject selfObj = env->GetObjectField(g_mcInstance, g_playerField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); selfObj = nullptr; }

    if (!selfObj) {
        if (g_cachedReachAttrInst) {
            env->DeleteGlobalRef(g_cachedReachAttrInst);
            g_cachedReachAttrInst = nullptr;
        }
        if (g_cachedLocalPlayer) {
            env->DeleteGlobalRef(g_cachedLocalPlayer);
            g_cachedLocalPlayer = nullptr;
        }
        return;
    }

    if (!g_cachedLocalPlayer || !env->IsSameObject(selfObj, g_cachedLocalPlayer)) {
        if (g_cachedLocalPlayer) env->DeleteGlobalRef(g_cachedLocalPlayer);
        if (g_cachedReachAttrInst) env->DeleteGlobalRef(g_cachedReachAttrInst);
        g_cachedLocalPlayer = env->NewGlobalRef(selfObj);
        g_cachedReachAttrInst = nullptr;

        jobject attrCont = env->CallObjectMethod(selfObj, g_dynGetAttributes);
        if (env->ExceptionCheck()) { env->ExceptionClear(); attrCont = nullptr; }
        if (attrCont) {
            jobject directInst = TryResolveReachAttributeFromRegistry(env, attrCont);
            if (directInst) {
                g_cachedReachAttrInst = env->NewGlobalRef(directInst);
                env->DeleteLocalRef(directInst);
            }

            if (!g_cachedReachAttrInst) {
                static bool loggedMissingReachAttr = false;
                if (!loggedMissingReachAttr) {
                    loggedMissingReachAttr = true;
                    Log("UpdateReach: Failed to resolve entity interaction range attribute.");
                }
            }
            env->DeleteLocalRef(attrCont);
        }
    }

    env->DeleteLocalRef(selfObj);

    if (g_cachedReachAttrInst) {
        double currentRange = 3.0; // Default Vanilla range
        if (cfg.reachEnabled) {
            int rval = rand() % 100;
            if (rval < cfg.reachChance) {
                float rangeSpan = cfg.reachMax - cfg.reachMin;
                if (rangeSpan < 0) rangeSpan = 0;
                float rfrac = (float)rand() / (float)RAND_MAX;
                currentRange = (double)(cfg.reachMin + (rfrac * rangeSpan));
            }
        }
        env->CallVoidMethod(g_cachedReachAttrInst, g_dynSetBaseValue, currentRange);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

static void EnsureVelocityJni(JNIEnv* env, jobject selfObj) {
    if (!env || !selfObj) return;
    if (g_velocityMethodsResolved) return;

    jclass playerCls = env->GetObjectClass(selfObj);
    if (!playerCls) return;

    if (!g_getVelocity_121) {
        const char* names[] = { "getDeltaMovement", "getVelocity", "method_18798", nullptr };
        const char* sigs[] = { "()Lnet/minecraft/world/phys/Vec3;", "()Lnet/minecraft/class_243;", nullptr };
        for (int ni = 0; names[ni] && !g_getVelocity_121; ni++) {
            for (int si = 0; sigs[si] && !g_getVelocity_121; si++) {
                g_getVelocity_121 = env->GetMethodID(playerCls, names[ni], sigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getVelocity_121 = nullptr; }
            }
        }
    }

    if (!g_setVelocityVec_121) {
        const char* names[] = { "setDeltaMovement", "setVelocity", "method_18799", nullptr };
        const char* sigs[] = { "(Lnet/minecraft/world/phys/Vec3;)V", "(Lnet/minecraft/class_243;)V", nullptr };
        for (int ni = 0; names[ni] && !g_setVelocityVec_121; ni++) {
            for (int si = 0; sigs[si] && !g_setVelocityVec_121; si++) {
                g_setVelocityVec_121 = env->GetMethodID(playerCls, names[ni], sigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_setVelocityVec_121 = nullptr; }
            }
        }
    }

    if (!g_setVelocityXYZ_121) {
        const char* names[] = { "setDeltaMovement", "setVelocity", "method_18800", nullptr };
        for (int ni = 0; names[ni] && !g_setVelocityXYZ_121; ni++) {
            g_setVelocityXYZ_121 = env->GetMethodID(playerCls, names[ni], "(DDD)V");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_setVelocityXYZ_121 = nullptr; }
        }
    }

    if (!g_hurtTimeField_121) {
        const char* leNames[] = { "net.minecraft.world.entity.LivingEntity", "net.minecraft.class_1309", nullptr };
        jclass livingCls = nullptr;
        for (int i = 0; leNames[i] && !livingCls; i++) {
            livingCls = LoadClassWithLoader(env, g_gameClassLoader, leNames[i]);
            if (env->ExceptionCheck()) { env->ExceptionClear(); livingCls = nullptr; }
        }
        if (!livingCls) livingCls = playerCls;

        const char* htNames[] = { "hurtTime", "field_6235", "f_20916_", nullptr };
        for (int i = 0; htNames[i] && !g_hurtTimeField_121; i++) {
            g_hurtTimeField_121 = env->GetFieldID(livingCls, htNames[i], "I");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_hurtTimeField_121 = nullptr; }
        }

        if (livingCls != playerCls) env->DeleteLocalRef(livingCls);
    }

    if (!g_vec3dCtor_121 && g_vec3dClass_121) {
        g_vec3dCtor_121 = env->GetMethodID(g_vec3dClass_121, "<init>", "(DDD)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_vec3dCtor_121 = nullptr; }
    }

    g_velocityMethodsResolved = (g_getVelocity_121 != nullptr) &&
        (g_setVelocityXYZ_121 != nullptr || (g_setVelocityVec_121 != nullptr && g_vec3dCtor_121 != nullptr)) &&
        (g_hurtTimeField_121 != nullptr) &&
        (g_vec3dX_121 != nullptr && g_vec3dY_121 != nullptr && g_vec3dZ_121 != nullptr);

    if (!g_velocityMethodsResolved && !g_loggedVelocityResolveFail_121) {
        g_loggedVelocityResolveFail_121 = true;
        Log(std::string("Velocity JNI unresolved: getVel=") + (g_getVelocity_121 ? "1" : "0") +
            " setVelXYZ=" + (g_setVelocityXYZ_121 ? "1" : "0") +
            " setVelVec=" + (g_setVelocityVec_121 ? "1" : "0") +
            " hurtTime=" + (g_hurtTimeField_121 ? "1" : "0") +
            " vecCtor=" + (g_vec3dCtor_121 ? "1" : "0"));
    }

    env->DeleteLocalRef(playerCls);
}

static void UpdateVelocity(JNIEnv* env, const Config& cfg) {
    if (!env || !g_mcInstance || !g_playerField_121) return;

    jobject selfObj = env->GetObjectField(g_mcInstance, g_playerField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); selfObj = nullptr; }
    if (!selfObj) return;

    EnsureVelocityJni(env, selfObj);
    if (!g_velocityMethodsResolved) {
        env->DeleteLocalRef(selfObj);
        return;
    }

    int hurtTime = env->GetIntField(selfObj, g_hurtTimeField_121);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(selfObj);
        return;
    }

    bool newHit = (hurtTime > 0 && hurtTime > g_lastHurtTime_121);
    if (cfg.velocityEnabled && newHit) {
        int rv = rand() % 100;
        bool applyThisHit = (rv < cfg.velocityChance);
        if (applyThisHit) {
            jobject velObj = env->CallObjectMethod(selfObj, g_getVelocity_121);
            if (env->ExceptionCheck()) { env->ExceptionClear(); velObj = nullptr; }
            if (velObj) {
                double vx = env->GetDoubleField(velObj, g_vec3dX_121);
                double vy = env->GetDoubleField(velObj, g_vec3dY_121);
                double vz = env->GetDoubleField(velObj, g_vec3dZ_121);
                if (!env->ExceptionCheck()) {
                    double horizMag = std::sqrt(vx * vx + vz * vz);
                    bool looksLikeKnockback = (horizMag > 0.26 || std::fabs(vy) > 0.20);
                    if (looksLikeKnockback && (cfg.velocityHorizontal != 100 || cfg.velocityVertical != 100)) {
                        double hScale = (double)cfg.velocityHorizontal / 100.0;
                        double vScale = (double)cfg.velocityVertical / 100.0;
                        double outX = vx * hScale;
                        double outY = vy * vScale;
                        double outZ = vz * hScale;

                        if (g_setVelocityXYZ_121) {
                            env->CallVoidMethod(selfObj, g_setVelocityXYZ_121, outX, outY, outZ);
                        } else if (g_setVelocityVec_121 && g_vec3dCtor_121 && g_vec3dClass_121) {
                            jobject vec = env->NewObject(g_vec3dClass_121, g_vec3dCtor_121, outX, outY, outZ);
                            if (!env->ExceptionCheck() && vec) {
                                env->CallVoidMethod(selfObj, g_setVelocityVec_121, vec);
                                if (env->ExceptionCheck()) env->ExceptionClear();
                                env->DeleteLocalRef(vec);
                            } else {
                                env->ExceptionClear();
                            }
                        }
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                } else {
                    env->ExceptionClear();
                }
                env->DeleteLocalRef(velObj);
            }
        }
    }

    g_lastHurtTime_121 = hurtTime;
    env->DeleteLocalRef(selfObj);
}

static void UpdatePlayerListOverlay(JNIEnv* env) {
    Config cfg;
    { LockGuard lk(g_configMutex); cfg = g_config; }
    const bool hideVanillaTags = cfg.nametagHideVanilla;
    const bool restoreVanillaTags = (!hideVanillaTags && g_nametagSuppressionActive_121);
    bool suppressionAppliedThisPass = false;
    bool suppressionAttemptedThisPass = false;

    DWORD now = GetTickCount();
    DWORD throttle = (cfg.aimAssist || cfg.triggerbot) ? 8 : 100;
    if (now - g_lastPlayerListUpdateMs < throttle) return;
    g_lastPlayerListUpdateMs = now;
    EnsureClosestPlayerCaches(env);
    if (!g_mcInstance || !g_worldField_121 || !g_playerField_121) return;

    jobject worldObj = env->GetObjectField(g_mcInstance, g_worldField_121);
    jobject selfObj = env->GetObjectField(g_mcInstance, g_playerField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); worldObj = nullptr; selfObj = nullptr; }
    if (!worldObj || !selfObj) {
        if (g_nametagSuppressionActive_121 || !g_hiddenNametagOriginalTeamByPlayer_121.empty()) {
            ResetNametagSuppressionCaches121(env, "world-or-player-null");
        }
        if (worldObj) env->DeleteLocalRef(worldObj);
        if (selfObj) env->DeleteLocalRef(selfObj);
        return;
    }

    TrackSuppressionWorldContext121(env, worldObj);

    EnsureEntityMethods(env, selfObj);
    bool suppressionMappingsReady = true;
    bool suppressionRestoreReady = true;
    if (hideVanillaTags || restoreVanillaTags || g_nametagSuppressionActive_121) {
        bool shouldAttemptResolve = (g_nextNametagSuppressionResolveRetryMs_121 == 0) || (now >= g_nextNametagSuppressionResolveRetryMs_121);
        if (shouldAttemptResolve) {
            suppressionMappingsReady = EnsureNametagSuppressionTeamMappings121(env, worldObj);
            if (suppressionMappingsReady) {
                g_nametagSuppressionResolveRetryCount_121 = 0;
                g_nextNametagSuppressionResolveRetryMs_121 = 0;
            } else {
                g_nametagSuppressionResolveRetryCount_121++;
                DWORD backoffMs = (DWORD)(150 + (std::min)(3000, g_nametagSuppressionResolveRetryCount_121 * 250));
                g_nextNametagSuppressionResolveRetryMs_121 = now + backoffMs;
            }
        } else {
            suppressionMappingsReady = AreNametagSuppressionCoreMappingsReady121();
        }
        suppressionRestoreReady = AreNametagSuppressionRestoreMappingsReady121();
    }

    if ((hideVanillaTags || restoreVanillaTags) && !suppressionMappingsReady && !g_loggedNametagSuppressionUnavailable_121) {
        g_loggedNametagSuppressionUnavailable_121 = true;
        Log("NametagHideVanilla: modern team-visibility mappings unresolved; fail-open (vanilla nametags remain visible).");
        LogNametagSuppressionMissingMappings121(env, worldObj);
    }
    if (hideVanillaTags && suppressionMappingsReady && !suppressionRestoreReady) {
        suppressionMappingsReady = false;
        if (!g_loggedNametagRestoreUnavailable_121) {
            g_loggedNametagRestoreUnavailable_121 = true;
            Log("NametagHideVanilla: restore mappings are incomplete; suppression disabled to avoid stuck hidden tags.");
            LogNametagSuppressionMissingMappings121(env, worldObj);
        }
    }
    if (restoreVanillaTags) {
        jobject restoreScoreboard = GetScoreboard121(env, worldObj);
        if (restoreScoreboard) {
            RestoreVanillaNametagSuppression121(env, restoreScoreboard);
            env->DeleteLocalRef(restoreScoreboard);
        } else {
            g_hiddenNametagOriginalTeamByPlayer_121.clear();
        }
        g_nametagSuppressionActive_121 = false;
    }

    DiscoverWorldPlayersListField(env, worldObj);
    if (!g_worldPlayersListField_121) {
        env->DeleteLocalRef(worldObj);
        env->DeleteLocalRef(selfObj);
        return;
    }

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
    int maxPlayersToProcess = size; // Process all players to avoid target flickering
    if (maxPlayersToProcess > 200) maxPlayersToProcess = 200; // safety sanity limit

    jobject hideScoreboardObj = nullptr;
    jobject hideTeamObj = nullptr;
    if (hideVanillaTags && suppressionMappingsReady) {
        hideScoreboardObj = GetScoreboard121(env, worldObj);
        if (hideScoreboardObj) {
            hideTeamObj = EnsureNametagHideTeam121(env, hideScoreboardObj);
            if (!hideTeamObj && !g_loggedNametagSuppressionUnavailable_121) {
                g_loggedNametagSuppressionUnavailable_121 = true;
                Log("NametagHideVanilla: modern hide team unavailable; fail-open (vanilla nametags remain visible).");
            }
        } else if (!g_loggedNametagSuppressionUnavailable_121) {
            g_loggedNametagSuppressionUnavailable_121 = true;
            Log("NametagHideVanilla: modern scoreboard unavailable; fail-open (vanilla nametags remain visible).");
        }
    }

    // Accumulate in localList; atomically publish to g_playerList at scope exit.
    std::vector<PlayerData121> localList;
    struct PublishOnExit {
        std::vector<PlayerData121>& local;
        ~PublishOnExit() { LockGuard lk(g_playerListMutex); g_playerList.swap(local); }
    } pub{localList};

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
        if (hideVanillaTags && hideScoreboardObj && hideTeamObj) {
            std::string suppressionName = GetStablePlayerName(env, entObj);
            if (!suppressionName.empty() && !LooksLikeFakePlayerLine(suppressionName)) {
                suppressionAttemptedThisPass = true;
                bool applied = ApplyVanillaNametagSuppression121(env, hideScoreboardObj, hideTeamObj, suppressionName);
                if (applied) suppressionAppliedThisPass = true;
            }
        }
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
            if (name.empty()) {
                char fallback[24];
                snprintf(fallback, sizeof(fallback), "Player_%d", processedCount + 1);
                name = fallback;
            }
            if (LooksLikeFakePlayerLine(name)) {
                env->DeleteLocalRef(lw.obj);
                continue;
            }

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
    if (hideTeamObj) env->DeleteLocalRef(hideTeamObj);
    if (hideScoreboardObj) env->DeleteLocalRef(hideScoreboardObj);
    if (listObj) env->DeleteLocalRef(listObj);
    if (worldObj) env->DeleteLocalRef(worldObj);
    if (selfObj) env->DeleteLocalRef(selfObj);

    if (hideVanillaTags && suppressionAppliedThisPass) {
        g_nametagSuppressionActive_121 = true;
    } else if (hideVanillaTags && suppressionAttemptedThisPass && !suppressionAppliedThisPass && !g_loggedNametagSuppressionUnavailable_121) {
        g_loggedNametagSuppressionUnavailable_121 = true;
        Log("NametagHideVanilla: unable to assign players to hide team; fail-open on this runtime.");
    }
}

// --- Chunk-based block entity discovery helpers (replaces flat-list approach) ---

// Ensure the base BlockEntity class is cached.
static void EnsureBlockEntityClass(JNIEnv* env) {
    if (g_blockEntityClass_121) return;
    const char* names[] = { "net.minecraft.class_2586", "net.minecraft.world.level.block.entity.BlockEntity", "net.minecraft.tileentity.TileEntity", nullptr };
    jclass be = nullptr;
    for (int i = 0; names[i] && !be; i++) {
        if (g_gameClassLoader) be = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
        if (!be) {
            std::string alt = names[i]; std::replace(alt.begin(), alt.end(), '.', '/');
            be = env->FindClass(alt.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear(); be = nullptr; }
        }
    }
    if (be) { g_blockEntityClass_121 = (jclass)env->NewGlobalRef(be); env->DeleteLocalRef(be); }
    if (!g_blockEntityClass_121) {
        static bool logged = false;
        if (!logged) { Log("EnsureBlockEntityClass: failed to find BlockEntity!"); logged = true; }
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
        { "getChunk",     "(II)Lnet/minecraft/world/level/chunk/LevelChunk;" },
        { "getChunk",     "(II)Lnet/minecraft/client/multiplayer/ClientLevel;" },
        { "method_22338", "(II)Lnet/minecraft/class_2818;" },
        { "getChunk",     "(II)Lnet/minecraft/class_2818;" },
        { "method_22338", "(II)Lnet/minecraft/class_1922;" },
        { "getChunk",     "(II)Lnet/minecraft/class_1922;" },
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

    // Work from the runtime chunk class instead of hard-failing on a single mapped class name.
    jclass chunkRootCls = env->GetObjectClass(chunkObj);
    if (env->ExceptionCheck()) { env->ExceptionClear(); chunkRootCls = nullptr; }
    if (!chunkRootCls) {
        return false;
    }

    EnsureBlockEntityClass(env);

    // Fast path: try known field names directly before the expensive reflection+validation loop.
    // field_34543 on class_2791 (Chunk) was confirmed as the blockEntities map by debug log.
    // field_12833 on class_2818 (WorldChunk) is the Yarn name for 1.21.
    // Mojmap name: blockEntities
    {
        const char* knownNames[] = { "blockEntities", "field_12833", "field_34543", "f_187611_", nullptr };
        for (int i = 0; knownNames[i]; i++) {
            jfieldID fid = env->GetFieldID(chunkRootCls, knownNames[i], "Ljava/util/Map;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
            if (fid) {
                g_chunkBlockEntitiesMapField_121 = fid;
                Log("EnsureChunkBEMap: fast-path found blockEntities map: " + std::string(knownNames[i]));
                env->DeleteLocalRef(chunkRootCls);
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
        jclass currentClass = (jclass)env->NewLocalRef(chunkRootCls);
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
                        jfieldID fid = env->GetFieldID(chunkRootCls, fn.c_str(), "Ljava/util/Map;");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
                        if (!fid) {
                            std::string tn = GetClassNameFromClass(env, ft);
                            std::string desc = "L" + tn + ";";
                            for (char& cc : desc) if (cc == '.') cc = '/';
                            fid = env->GetFieldID(chunkRootCls, fn.c_str(), desc.c_str());
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
                                                        env->DeleteLocalRef(chunkRootCls);
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
    env->DeleteLocalRef(chunkRootCls);
    return false;
}

static void EnsureBlockPosCache(JNIEnv* env, jobject beObj) {
    if (!env || !beObj) return;

    if (!g_beGetPos_121) {
        jclass beCls = env->GetObjectClass(beObj);
        if (beCls) {
            // Mojmap: getBlockPos, Yarn: getPos/method_11014
            const char* mNames[] = { "getBlockPos", "getPos", "method_11014", "f_58858_", nullptr };
            for (int i = 0; mNames[i] && !g_beGetPos_121; i++) {
                g_beGetPos_121 = env->GetMethodID(beCls, mNames[i], "()Lnet/minecraft/core/BlockPos;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_beGetPos_121 = nullptr; }
                if (!g_beGetPos_121) {
                    g_beGetPos_121 = env->GetMethodID(beCls, mNames[i], "()Lnet/minecraft/class_2338;");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_beGetPos_121 = nullptr; }
                }
            }
            env->DeleteLocalRef(beCls);
        }
    }

    if (!g_blockPosClass_121) {
        const char* bpNames[] = { "net.minecraft.core.BlockPos", "net.minecraft.class_2338", "net.minecraft.util.math.BlockPos", nullptr };
        jclass bp = nullptr;
        for (int i = 0; bpNames[i] && !bp; i++) {
            if (g_gameClassLoader) bp = LoadClassWithLoader(env, g_gameClassLoader, bpNames[i]);
            if (!bp) {
                std::string alt = bpNames[i]; std::replace(alt.begin(), alt.end(), '.', '/');
                bp = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); bp = nullptr; }
            }
        }
        if (bp) {
            g_blockPosClass_121 = (jclass)env->NewGlobalRef(bp);
            env->DeleteLocalRef(bp);
            
            const char* xNames[] = { "x", "field_11175", "field_13358", "f_123290_", nullptr };
            const char* yNames[] = { "y", "field_11174", "field_13357", "f_123291_", nullptr };
            const char* zNames[] = { "z", "field_11173", "field_11172", "field_13359", "f_123292_", nullptr };

            for (int i = 0; xNames[i] && !g_blockPosX_121; i++) {
                g_blockPosX_121 = env->GetFieldID(g_blockPosClass_121, xNames[i], "I");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosX_121 = nullptr; }
            }
            for (int i = 0; yNames[i] && !g_blockPosY_121; i++) {
                g_blockPosY_121 = env->GetFieldID(g_blockPosClass_121, yNames[i], "I");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosY_121 = nullptr; }
            }
            for (int i = 0; zNames[i] && !g_blockPosZ_121; i++) {
                g_blockPosZ_121 = env->GetFieldID(g_blockPosClass_121, zNames[i], "I");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosZ_121 = nullptr; }
            }

            if (!g_blockPosX_121 || !g_blockPosY_121 || !g_blockPosZ_121) {
                const char* v3iNames[] = { "net.minecraft.core.Vec3i", "net.minecraft.class_2382", "net.minecraft.util.math.Vec3i", nullptr };
                jclass vec3iCls = nullptr;
                for (int i = 0; v3iNames[i] && !vec3iCls; i++) {
                    if (g_gameClassLoader) vec3iCls = LoadClassWithLoader(env, g_gameClassLoader, v3iNames[i]);
                    if (!vec3iCls) {
                        std::string alt = v3iNames[i]; std::replace(alt.begin(), alt.end(), '.', '/');
                        vec3iCls = env->FindClass(alt.c_str());
                        if (env->ExceptionCheck()) { env->ExceptionClear(); vec3iCls = nullptr; }
                    }
                }
                if (vec3iCls) {
                    for (int i = 0; xNames[i] && !g_blockPosX_121; i++) {
                        g_blockPosX_121 = env->GetFieldID(vec3iCls, xNames[i], "I");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosX_121 = nullptr; }
                    }
                    for (int i = 0; yNames[i] && !g_blockPosY_121; i++) {
                        g_blockPosY_121 = env->GetFieldID(vec3iCls, yNames[i], "I");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosY_121 = nullptr; }
                    }
                    for (int i = 0; zNames[i] && !g_blockPosZ_121; i++) {
                        g_blockPosZ_121 = env->GetFieldID(vec3iCls, zNames[i], "I");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosZ_121 = nullptr; }
                    }
                    env->DeleteLocalRef(vec3iCls);
                }
            }
        }
    }

    // Last resort: dynamically find all 3 int fields via getDeclaredFields
    if (!g_blockPosX_121 || !g_blockPosY_121 || !g_blockPosZ_121) {
        jclass vec3iCls2 = LoadClassWithLoader(env, g_gameClassLoader, "net.minecraft.class_2382");
        if (!vec3iCls2) vec3iCls2 = env->FindClass("net/minecraft/class_2382");
        if (env->ExceptionCheck()) { env->ExceptionClear(); vec3iCls2 = nullptr; }
        if (vec3iCls2) {
            jclass cCls = env->FindClass("java/lang/Class");
            jmethodID mGDF = cCls ? env->GetMethodID(cCls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
            jclass fCls = env->FindClass("java/lang/reflect/Field");
            jmethodID mGT = fCls ? env->GetMethodID(fCls, "getType", "()Ljava/lang/Class;") : nullptr;
            jmethodID mGN = fCls ? env->GetMethodID(fCls, "getName", "()Ljava/lang/String;") : nullptr;
            if (mGDF && mGT && mGN) {
                jobjectArray fields = (jobjectArray)env->CallObjectMethod(vec3iCls2, mGDF);
                if (fields && !env->ExceptionCheck()) {
                    jclass intTypeCls = env->FindClass("java/lang/Integer");
                    jfieldID intTypeField = intTypeCls ? env->GetStaticFieldID(intTypeCls, "TYPE", "Ljava/lang/Class;") : nullptr;
                    jobject intPrimCls = intTypeField ? env->GetStaticObjectField(intTypeCls, intTypeField) : nullptr;
                    jsize fc = env->GetArrayLength(fields);
                    std::vector<std::string> intNames;
                    for (jsize i = 0; i < fc && intNames.size() < 3; i++) {
                        jobject f = env->GetObjectArrayElement(fields, i);
                        jobject ft = env->CallObjectMethod(f, mGT);
                        if (ft && intPrimCls && env->IsSameObject(ft, intPrimCls)) {
                            jstring jn = (jstring)env->CallObjectMethod(f, mGN);
                            const char* cn = env->GetStringUTFChars(jn, nullptr);
                            if (cn) { intNames.push_back(cn); env->ReleaseStringUTFChars(jn, cn); }
                            env->DeleteLocalRef(jn);
                        }
                        if (ft) env->DeleteLocalRef(ft);
                        env->DeleteLocalRef(f);
                    }
                    if (intNames.size() >= 3) {
                        if (!g_blockPosX_121) g_blockPosX_121 = env->GetFieldID(vec3iCls2, intNames[0].c_str(), "I");
                        if (!g_blockPosY_121) g_blockPosY_121 = env->GetFieldID(vec3iCls2, intNames[1].c_str(), "I");
                        if (!g_blockPosZ_121) g_blockPosZ_121 = env->GetFieldID(vec3iCls2, intNames[2].c_str(), "I");
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    if (intTypeCls) env->DeleteLocalRef(intTypeCls);
                }
                if (fields) env->DeleteLocalRef(fields);
            }
            if (cCls) env->DeleteLocalRef(cCls);
            if (fCls) env->DeleteLocalRef(fCls);
            env->DeleteLocalRef(vec3iCls2);
        }
    }

    // Try to find BlockEntity.pos field directly
    if (!g_beBlockPosField_121 && g_blockPosClass_121 && beObj) {
        jclass beCls2 = env->GetObjectClass(beObj);
        if (beCls2) {
            const char* posNames[] = { "worldPosition", "pos", "field_11177", "f_58851_", nullptr };
            const char* posSigs[] = { "Lnet/minecraft/core/BlockPos;", "Lnet/minecraft/class_2338;", nullptr };
            for (int ni = 0; posNames[ni] && !g_beBlockPosField_121; ni++) {
                for (int si = 0; posSigs[si] && !g_beBlockPosField_121; si++) {
                    g_beBlockPosField_121 = env->GetFieldID(beCls2, posNames[ni], posSigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_beBlockPosField_121 = nullptr; }
                }
            }
            env->DeleteLocalRef(beCls2);
        }
    }

    // BlockPos coordinate methods fallback
    if (g_blockPosClass_121) {
        if (!g_blockPosGetX_121) {
            const char* names[] = { "getX", "method_4900", nullptr };
            for (int i = 0; names[i] && !g_blockPosGetX_121; i++) {
                g_blockPosGetX_121 = env->GetMethodID(g_blockPosClass_121, names[i], "()I");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosGetX_121 = nullptr; }
            }
        }
        if (!g_blockPosGetY_121) {
            const char* names[] = { "getY", "method_4898", nullptr };
            for (int i = 0; names[i] && !g_blockPosGetY_121; i++) {
                g_blockPosGetY_121 = env->GetMethodID(g_blockPosClass_121, names[i], "()I");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosGetY_121 = nullptr; }
            }
        }
        if (!g_blockPosGetZ_121) {
            const char* names[] = { "getZ", "method_4896", nullptr };
            for (int i = 0; names[i] && !g_blockPosGetZ_121; i++) {
                g_blockPosGetZ_121 = env->GetMethodID(g_blockPosClass_121, names[i], "()I");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_blockPosGetZ_121 = nullptr; }
            }
        }
    }

    static bool s_bpDiag = false;
    if (!s_bpDiag) {
        s_bpDiag = true;
        Log(std::string("BlockPosCache: beGetPos=") + (g_beGetPos_121 ? "1" : "0")
            + " beField=" + (g_beBlockPosField_121 ? "1" : "0")
            + " bpClass=" + (g_blockPosClass_121 ? "1" : "0")
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

    // Diagnostic: log entry every 5 seconds
    static DWORD s_chestEntryLogMs = 0;
    bool entryLog = (now - s_chestEntryLogMs > 5000);
    if (entryLog) s_chestEntryLogMs = now;

    if (!g_mcInstance || !g_worldField_121 || !g_playerField_121) {
        if (entryLog) Log("ChestESP: precondition fail mc=" + std::to_string((long long)g_mcInstance) + " world=" + std::to_string((long long)g_worldField_121) + " player=" + std::to_string((long long)g_playerField_121));
        return;
    }
    jobject worldObj = env->GetObjectField(g_mcInstance, g_worldField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); worldObj = nullptr; }
    if (!worldObj) {
        if (entryLog) Log("ChestESP: worldObj is null");
        return;
    }

    // Player position — use bg cam state (already read this iteration; no extra JNI)
    double sx = 0, sy = 0, sz = 0;
    { LockGuard lk(g_bgCamMutex); sx = g_bgCamState.camX; sy = g_bgCamState.camY; sz = g_bgCamState.camZ; }

    EnsureBlockEntityClass(env);
    if (!EnsureChunkAccess(env, worldObj)) {
        if (entryLog) Log("ChestESP: EnsureChunkAccess failed");
        env->DeleteLocalRef(worldObj);
        return;
    }

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
    int chunksScanned = 0;
    int chunksWithBEMap = 0;

    for (int dx = -RANGE; dx <= RANGE; dx++) {
        for (int dz = -RANGE; dz <= RANGE; dz++) {
            // Re-check transition guard inside the loop too
            if (GetTickCount() < g_worldTransitionEndMs) break;
            jobject chunkObj = env->CallObjectMethod(worldObj, g_worldGetChunkMethod_121, pcx + dx, pcz + dz);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!chunkObj) continue;
            chunksScanned++;

            if (!g_chunkBlockEntitiesMapField_121)
                EnsureChunkBEMap(env, chunkObj);

            if (g_chunkBlockEntitiesMapField_121) {
                jobject mapObj = env->GetObjectField(chunkObj, g_chunkBlockEntitiesMapField_121);
                if (env->ExceptionCheck()) { env->ExceptionClear(); mapObj = nullptr; }
                if (mapObj) {
                    chunksWithBEMap++;
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

    if (shouldLog) { lastLogMs = now; Log("UpdateChestList: chunks=" + std::to_string(chunksScanned) + " withMap=" + std::to_string(chunksWithBEMap) + " chests=" + std::to_string(totalChests) + " listed=" + std::to_string(localList.size()) + " BEs=" + std::to_string(totalBEsScanned) + " hmDirect=" + std::to_string(g_javaHashMapTableField ? 1 : 0) + " hashMapSeen=" + std::to_string(sawHashMap ? 1 : 0) + " usedDirect=" + std::to_string(usedDirectPath ? 1 : 0) + " usedFallback=" + std::to_string(usedFallbackPath ? 1 : 0)); }

    env->DeleteLocalRef(worldObj);

    std::sort(localList.begin(), localList.end(), [](const ChestData121& a, const ChestData121& b){ return a.dist < b.dist; });
    { LockGuard lk(g_chestListMutex); g_chestList.swap(localList); }
}

static std::string CallTextToString(JNIEnv* env, jobject textObj) {
    if (!textObj) return "";

    jclass textCls = nullptr;
    const char* textClassNames[] = {
        "net.minecraft.network.chat.Component",
        "net.minecraft.text.Text",
        "net.minecraft.class_2561",
        nullptr
    };
    for (int i = 0; textClassNames[i] && !textCls; i++) {
        if (g_gameClassLoader) textCls = LoadClassWithLoader(env, g_gameClassLoader, textClassNames[i]);
        if (!textCls) {
            std::string alt = textClassNames[i]; std::replace(alt.begin(), alt.end(), '.', '/');
            textCls = env->FindClass(alt.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear(); textCls = nullptr; }
        }
    }
    // Fallback just in case
    if (!textCls) textCls = env->GetObjectClass(textObj);
    if (!textCls) return "";

    if (!g_textGetString_121) {
        const char* getStringNames[] = { "getString", "method_10851", "f_130669_", nullptr };
        for (int i = 0; getStringNames[i] && !g_textGetString_121; i++) {
            g_textGetString_121 = env->GetMethodID(textCls, getStringNames[i], "()Ljava/lang/String;");
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
        // Mojmap: net.minecraft.world.entity.player.Player, Yarn: net.minecraft.class_1657
        const char* names[] = {
            "net.minecraft.world.entity.player.Player",
            "net.minecraft.entity.player.PlayerEntity",
            "net.minecraft.class_1657",
            "net.minecraft.client.network.AbstractClientPlayerEntity",
            "net.minecraft.class_742",
            nullptr
        };
        jclass c = nullptr;
        for (int i = 0; names[i] && !c; i++) {
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
            if (!c) {
                std::string alt = names[i]; std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
        }
        if (c) { g_playerEntityClass_121 = (jclass)env->NewGlobalRef(c); env->DeleteLocalRef(c); }
    }

    jclass mcCls = env->GetObjectClass(g_mcInstance);
    if (!mcCls) return;

    if (!g_worldField_121) {
        const char* worldNames[] = { "level", "world", "field_1687", "f_91073_", nullptr };
        const char* worldSigs[] = {
            "Lnet/minecraft/class_638;",
            "Lnet/minecraft/client/multiplayer/ClientLevel;",
            "Lnet/minecraft/client/world/ClientWorld;",
            "Lnet/minecraft/world/level/Level;",
            nullptr
        };
        for (int ni = 0; worldNames[ni] && !g_worldField_121; ni++) {
            for (int si = 0; worldSigs[si] && !g_worldField_121; si++) {
                g_worldField_121 = env->GetFieldID(mcCls, worldNames[ni], worldSigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_worldField_121 = nullptr; }
                else if (g_worldField_121) break;
            }
        }
    }
    if (!g_playerField_121) {
        const char* playerNames[] = { "player", "field_1724", "f_91074_", nullptr };
        const char* playerSigs[] = {
            "Lnet/minecraft/class_746;",
            "Lnet/minecraft/client/player/LocalPlayer;",
            "Lnet/minecraft/world/entity/player/Player;",
            nullptr
        };
        for (int ni = 0; playerNames[ni] && !g_playerField_121; ni++) {
            for (int si = 0; playerSigs[si] && !g_playerField_121; si++) {
                g_playerField_121 = env->GetFieldID(mcCls, playerNames[ni], playerSigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_playerField_121 = nullptr; }
                else if (g_playerField_121) break;
            }
        }
    }

    env->DeleteLocalRef(mcCls);
}

static void DiscoverWorldPlayersListField(JNIEnv* env, jobject worldObj) {
    if (!worldObj || g_worldPlayersListField_121) return;

    jclass worldCls = env->GetObjectClass(worldObj);
    if (!worldCls) return;

    // Fast path: try known 1.21 names first (Mojmap: players, Yarn: field_24730)
    const char* knownNames[] = { "players", "field_24730", "field_44861", "f_104595_", nullptr };
    const char* knownSigs[] = {
        "Ljava/util/List;",
        "Ljava/util/Collection;",
        "Lit/unimi/dsi/fastutil/objects/ObjectArrayList;",
        "Lit/unimi/dsi/fastutil/objects/ObjectList;",
        nullptr
    };
    for (int i = 0; knownNames[i]; i++) {
        for (int si = 0; knownSigs[si]; si++) {
            jfieldID fid = env->GetFieldID(worldCls, knownNames[i], knownSigs[si]);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
            if (fid) {
                g_worldPlayersListField_121 = fid;
                Log("Discovered world players list field (fast-path): " + std::string(knownNames[i]) + " " + knownSigs[si]);
                env->DeleteLocalRef(worldCls);
                return;
            }
        }
    }

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

    jclass collectionClass = env->FindClass("java/util/Collection");
    jmethodID mToArray = collectionClass ? env->GetMethodID(collectionClass, "toArray", "()[Ljava/lang/Object;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mToArray = nullptr; }

    jobjectArray fields = (jobjectArray)env->CallObjectMethod(worldCls, mGetFields);
    if (env->ExceptionCheck() || !fields) {
        env->ExceptionClear();
        if (collectionClass) env->DeleteLocalRef(collectionClass);
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
        bool isCollection = (collectionClass && env->IsAssignableFrom(ft, collectionClass));
        if (env->ExceptionCheck()) { env->ExceptionClear(); isCollection = false; }
        if (!isCollection) { env->DeleteLocalRef(ft); env->DeleteLocalRef(fld); continue; }

        // Get field name
        jstring jfn = mFName ? (jstring)env->CallObjectMethod(fld, mFName) : nullptr;
        if (env->ExceptionCheck() || !jfn) { env->ExceptionClear(); env->DeleteLocalRef(ft); env->DeleteLocalRef(fld); continue; }
        const char* cfn = env->GetStringUTFChars(jfn, nullptr);
        std::string fn = cfn ? cfn : "";
        if (cfn) env->ReleaseStringUTFChars(jfn, cfn);
        env->DeleteLocalRef(jfn);

        // Resolve jfieldID using declared type descriptor first, then interface descriptors.
        std::string ftName = GetClassNameFromClass(env, ft);
        std::string exactSig;
        if (!ftName.empty()) {
            exactSig = "L" + ftName + ";";
            for (char& ch : exactSig) if (ch == '.') ch = '/';
        }
        jfieldID fid = nullptr;
        if (!exactSig.empty()) {
            fid = env->GetFieldID(worldCls, fn.c_str(), exactSig.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
        }
        if (!fid) {
            fid = env->GetFieldID(worldCls, fn.c_str(), "Ljava/util/List;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
        }
        if (!fid) {
            fid = env->GetFieldID(worldCls, fn.c_str(), "Ljava/util/Collection;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
        }

        if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
        if (fid) {
            jobject listObj = env->GetObjectField(worldObj, fid);
            if (!env->ExceptionCheck() && listObj) {
                bool looksLikePlayer = false;
                if (collectionClass && mToArray && env->IsInstanceOf(listObj, collectionClass) == JNI_TRUE) {
                    jobjectArray arr = (jobjectArray)env->CallObjectMethod(listObj, mToArray);
                    if (!env->ExceptionCheck() && arr) {
                        jsize sz = env->GetArrayLength(arr);
                        int sampleCount = (int)(sz > 3 ? 3 : sz);
                        for (int si = 0; si < sampleCount && !looksLikePlayer; si++) {
                            jobject sample = env->GetObjectArrayElement(arr, si);
                            if (!sample) continue;
                            if (g_playerEntityClass_121) {
                                looksLikePlayer = (env->IsInstanceOf(sample, g_playerEntityClass_121) == JNI_TRUE);
                                if (env->ExceptionCheck()) { env->ExceptionClear(); looksLikePlayer = false; }
                            }
                            if (!looksLikePlayer) {
                                jclass sampleCls = env->GetObjectClass(sample);
                                std::string sampleType = sampleCls ? GetClassNameFromClass(env, sampleCls) : "";
                                if (sampleCls) env->DeleteLocalRef(sampleCls);
                                looksLikePlayer =
                                    sampleType.find("Player") != std::string::npos ||
                                    sampleType.find("class_1657") != std::string::npos ||
                                    sampleType.find("class_742") != std::string::npos ||
                                    sampleType.find("class_746") != std::string::npos;
                            }
                            env->DeleteLocalRef(sample);
                        }
                    }
                    if (arr) env->DeleteLocalRef(arr);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }

                if (looksLikePlayer) {
                    g_worldPlayersListField_121 = fid;
                    Log("Discovered world players list field: " + fn + (exactSig.empty() ? "" : (" " + exactSig)));
                    env->DeleteLocalRef(listObj);
                    env->DeleteLocalRef(ft);
                    env->DeleteLocalRef(fld);
                    break;
                }
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
    if (collectionClass) env->DeleteLocalRef(collectionClass);
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
            const char* names[] = { "getYRot", "getYaw", "method_36454", "method_5669", nullptr };
            for (int i = 0; names[i] && !g_getYaw_121; i++) {
                g_getYaw_121 = env->GetMethodID(entCls, names[i], "()F");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getYaw_121 = nullptr; }
            }
        }
        if (!g_getPitch_121) {
            const char* names[] = { "getXRot", "getPitch", "method_36455", "method_5667", nullptr };
            for (int i = 0; names[i] && !g_getPitch_121; i++) {
                g_getPitch_121 = env->GetMethodID(entCls, names[i], "()F");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getPitch_121 = nullptr; }
            }
        }
        if (!g_getHealth_121) {
            const char* names[] = { "getHealth", "method_6032", nullptr };
            for (int i = 0; names[i] && !g_getHealth_121; i++) {
                g_getHealth_121 = env->GetMethodID(entCls, names[i], "()F");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getHealth_121 = nullptr; }
            }
        }
        if (!g_getName_121) {
            const char* names[] = { "getName", "getDisplayName", "method_5477", nullptr };
            const char* sigs[] = { "()Lnet/minecraft/network/chat/Component;", "()Lnet/minecraft/class_2561;", nullptr };
            for (int ni = 0; names[ni] && !g_getName_121; ni++) {
                for (int si = 0; sigs[si] && !g_getName_121; si++) {
                    g_getName_121 = env->GetMethodID(entCls, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_getName_121 = nullptr; }
                }
            }
        }
        if (!g_setCustomNameVisible_121) {
            const char* names[] = { "setCustomNameVisible", "method_5880", nullptr };
            for (int i = 0; names[i] && !g_setCustomNameVisible_121; i++) {
                g_setCustomNameVisible_121 = env->GetMethodID(entCls, names[i], "(Z)V");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_setCustomNameVisible_121 = nullptr; }
            }
        }
        if (!g_isCustomNameVisible_121) {
            const char* names[] = { "isCustomNameVisible", "method_5807", nullptr };
            for (int i = 0; names[i] && !g_isCustomNameVisible_121; i++) {
                g_isCustomNameVisible_121 = env->GetMethodID(entCls, names[i], "()Z");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_isCustomNameVisible_121 = nullptr; }
            }
        }
        if (!g_shouldRenderName_121) {
            const char* names[] = { "shouldRenderName", "method_5733", nullptr };
            for (int i = 0; names[i] && !g_shouldRenderName_121; i++) {
                g_shouldRenderName_121 = env->GetMethodID(entCls, names[i], "()Z");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_shouldRenderName_121 = nullptr; }
            }
        }
        if (!g_getArmor_121) {
            const char* names[] = { "getArmorValue", "getArmor", "method_6096", nullptr };
            for (int i = 0; names[i] && !g_getArmor_121; i++) {
                g_getArmor_121 = env->GetMethodID(entCls, names[i], "()I");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getArmor_121 = nullptr; }
            }
        }
        if (!g_entityPosField_121) {
            // Mojmap: pos, Yarn: field_5979
            const char* posNames[] = { "pos", "position", "field_5979", "f_19854_", nullptr };
            const char* posSigs[] = { "Lnet/minecraft/world/phys/Vec3;", "Lnet/minecraft/class_243;", "Lnet/minecraft/util/math/Vec3d;", nullptr };
            for (int ni = 0; posNames[ni] && !g_entityPosField_121; ni++) {
                for (int si = 0; posSigs[si] && !g_entityPosField_121; si++) {
                    g_entityPosField_121 = env->GetFieldID(entCls, posNames[ni], posSigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_entityPosField_121 = nullptr; }
                }
            }
        }
        if (!g_getMainHandStack_121) {
            const char* names[] = { "getMainHandItem", "getMainHandStack", "method_6047", nullptr };
            const char* sigs[] = { "()Lnet/minecraft/world/item/ItemStack;", "()Lnet/minecraft/class_1799;", nullptr };
            for (int ni = 0; names[ni] && !g_getMainHandStack_121; ni++) {
                for (int si = 0; sigs[si] && !g_getMainHandStack_121; si++) {
                    g_getMainHandStack_121 = env->GetMethodID(entCls, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_getMainHandStack_121 = nullptr; }
                }
            }
        }
        
        if (!g_itemStackClass_121) {
            const char* names[] = { "net.minecraft.world.item.ItemStack", "net.minecraft.class_1799", nullptr };
            jclass c = nullptr;
            for (int i = 0; names[i] && !c; i++) {
                if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
                if (!c) {
                    std::string alt = names[i]; std::replace(alt.begin(), alt.end(), '.', '/');
                    c = env->FindClass(alt.c_str());
                    if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
                }
            }
            if (c) { 
                g_itemStackClass_121 = (jclass)env->NewGlobalRef(c); 
                env->DeleteLocalRef(c); 
                
                const char* nNames[] = { "getHoverName", "getName", "method_7964", nullptr };
                for (int i = 0; nNames[i] && !g_itemStackGetName_121; i++) {
                    g_itemStackGetName_121 = env->GetMethodID(g_itemStackClass_121, nNames[i], "()Lnet/minecraft/network/chat/Component;");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_itemStackGetName_121 = nullptr; }
                    if (!g_itemStackGetName_121) {
                        g_itemStackGetName_121 = env->GetMethodID(g_itemStackClass_121, nNames[i], "()Lnet/minecraft/class_2561;");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_itemStackGetName_121 = nullptr; }
                    }
                }
                
                const char* dNames[] = { "getDamageValue", "getDamage", "method_7919", nullptr };
                for (int i = 0; dNames[i] && !g_itemStackGetDamage_121; i++) {
                    g_itemStackGetDamage_121 = env->GetMethodID(g_itemStackClass_121, dNames[i], "()I");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_itemStackGetDamage_121 = nullptr; }
                }

                const char* mdNames[] = { "getMaxDamage", "method_7936", nullptr };
                for (int i = 0; mdNames[i] && !g_itemStackGetMaxDamage_121; i++) {
                    g_itemStackGetMaxDamage_121 = env->GetMethodID(g_itemStackClass_121, mdNames[i], "()I");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_itemStackGetMaxDamage_121 = nullptr; }
                }
            }
        }

        // Prep stable-name fallback caches (GameProfile) using this entity object.
        EnsureGameProfileCaches(env, entObj);
        env->DeleteLocalRef(entCls);
    }
}

static bool AreNametagSuppressionCoreMappingsReady121() {
    return g_worldGetScoreboard_121
        && g_scoreboardGetTeam_121
        && g_scoreboardAddTeam_121
        && g_scoreboardAddHolderToTeam_121
        && g_teamSetNameTagVisibilityRule_121
        && g_visibilityRuleNever_121;
}

static bool AreNametagSuppressionRestoreMappingsReady121() {
    return g_scoreboardGetTeam_121
        && g_scoreboardRemoveTeam_121;
}

static void LogNametagSuppressionMissingMappings121(JNIEnv* env, jobject worldObj) {
    DWORD now = GetTickCount();
    static DWORD s_lastLogMs = 0;
    if (now - s_lastLogMs < 8000) return;
    s_lastLogMs = now;

    std::vector<std::string> missingCore;
    if (!g_worldGetScoreboard_121) missingCore.push_back("World.getScoreboard()");
    if (!g_scoreboardGetTeam_121) missingCore.push_back("Scoreboard.getTeam(String)");
    if (!g_scoreboardAddTeam_121) missingCore.push_back("Scoreboard.addTeam(String)");
    if (!g_scoreboardAddHolderToTeam_121) missingCore.push_back("Scoreboard.addScoreHolderToTeam(String,Team)");
    if (!g_teamSetNameTagVisibilityRule_121) missingCore.push_back("Team.setNameTagVisibilityRule(VisibilityRule)");
    if (!g_visibilityRuleNever_121) missingCore.push_back("VisibilityRule.NEVER");

    std::vector<std::string> missingRestore;
    if (!g_scoreboardGetHolderTeam_121) missingRestore.push_back("Scoreboard.getScoreHolderTeam(String)");
    if (!g_abstractTeamGetName_121) missingRestore.push_back("AbstractTeam.getName()");
    if (!g_scoreboardClearTeam_121) missingRestore.push_back("Scoreboard.clearTeam(String)");
    if (!g_scoreboardRemoveTeam_121) missingRestore.push_back("Scoreboard.removeTeam(Team)");

    auto Join = [](const std::vector<std::string>& parts) -> std::string {
        if (parts.empty()) return "none";
        std::string out = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) out += ", " + parts[i];
        return out;
    };

    std::string worldClass = "?";
    if (env && worldObj) {
        jclass wc = env->GetObjectClass(worldObj);
        if (!env->ExceptionCheck() && wc) {
            worldClass = GetClassNameFromClass(env, wc);
            env->DeleteLocalRef(wc);
        } else {
            env->ExceptionClear();
        }
    }
    std::string scoreboardClass = g_scoreboardClass_121 ? GetClassNameFromClass(env, g_scoreboardClass_121) : "?";
    std::string teamClass = g_teamClass_121 ? GetClassNameFromClass(env, g_teamClass_121) : "?";

    Log("NametagHideVanilla unresolved mappings: core=[" + Join(missingCore)
        + "] restore=[" + Join(missingRestore)
        + "] worldClass=" + worldClass
        + " scoreboardClass=" + scoreboardClass
        + " teamClass=" + teamClass);
}

static void ResetNametagSuppressionCaches121(JNIEnv* env, const char* reason) {
    if (g_lastNametagSuppressionWorld_121 && env) {
        env->DeleteGlobalRef(g_lastNametagSuppressionWorld_121);
        g_lastNametagSuppressionWorld_121 = nullptr;
    }

    if (g_visibilityRuleNever_121 && env) {
        env->DeleteGlobalRef(g_visibilityRuleNever_121);
        g_visibilityRuleNever_121 = nullptr;
    }

    if (g_scoreboardClass_121 && env) {
        env->DeleteGlobalRef(g_scoreboardClass_121);
        g_scoreboardClass_121 = nullptr;
    }
    if (g_teamClass_121 && env) {
        env->DeleteGlobalRef(g_teamClass_121);
        g_teamClass_121 = nullptr;
    }
    if (g_abstractTeamClass_121 && env) {
        env->DeleteGlobalRef(g_abstractTeamClass_121);
        g_abstractTeamClass_121 = nullptr;
    }
    if (g_visibilityRuleClass_121 && env) {
        env->DeleteGlobalRef(g_visibilityRuleClass_121);
        g_visibilityRuleClass_121 = nullptr;
    }

    g_worldGetScoreboard_121 = nullptr;
    g_scoreboardGetTeam_121 = nullptr;
    g_scoreboardAddTeam_121 = nullptr;
    g_scoreboardRemoveTeam_121 = nullptr;
    g_scoreboardAddHolderToTeam_121 = nullptr;
    g_scoreboardGetHolderTeam_121 = nullptr;
    g_scoreboardClearTeam_121 = nullptr;
    g_abstractTeamGetName_121 = nullptr;
    g_teamSetNameTagVisibilityRule_121 = nullptr;
    g_worldPlayersListField_121 = nullptr;

    g_hiddenNametagOriginalTeamByPlayer_121.clear();
    g_nametagSuppressionActive_121 = false;
    g_loggedNametagSuppressionUnavailable_121 = false;
    g_loggedNametagRestoreUnavailable_121 = false;
    g_nextNametagSuppressionResolveRetryMs_121 = 0;
    g_nametagSuppressionResolveRetryCount_121 = 0;

    if (reason && *reason) {
        Log(std::string("NametagHideVanilla: reset suppression caches (") + reason + ").");
    }
}

static bool TrackSuppressionWorldContext121(JNIEnv* env, jobject worldObj) {
    if (!env || !worldObj) return false;
    if (!g_lastNametagSuppressionWorld_121) {
        g_lastNametagSuppressionWorld_121 = env->NewGlobalRef(worldObj);
        return false;
    }

    bool changed = (env->IsSameObject(worldObj, g_lastNametagSuppressionWorld_121) == JNI_FALSE);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        changed = false;
    }
    if (changed) {
        ResetNametagSuppressionCaches121(env, "world-context-changed");
        g_lastNametagSuppressionWorld_121 = env->NewGlobalRef(worldObj);
    }
    return changed;
}

static std::string Utf8FromJString(JNIEnv* env, jstring js) {
    if (!env || !js) return "";
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(js, c);
    return out;
}

static bool EnsureNametagSuppressionTeamMappings121(JNIEnv* env, jobject worldObj) {
    if (!env || !worldObj) return false;

    if (!g_scoreboardClass_121) {
        const char* names[] = { "net.minecraft.world.scores.Scoreboard", "net.minecraft.scoreboard.Scoreboard", "net.minecraft.class_269", "eyg", nullptr };
        jclass c = nullptr;
        for (int i = 0; names[i] && !c; i++) {
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
            if (!c) {
                std::string alt = names[i];
                std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
        }
        if (c) {
            g_scoreboardClass_121 = (jclass)env->NewGlobalRef(c);
            env->DeleteLocalRef(c);
        }
    }
    if (!g_teamClass_121) {
        const char* names[] = { "net.minecraft.world.scores.PlayerTeam", "net.minecraft.world.scores.Team", "net.minecraft.scoreboard.Team", "net.minecraft.class_268", "eyb", nullptr };
        jclass c = nullptr;
        for (int i = 0; names[i] && !c; i++) {
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
            if (!c) {
                std::string alt = names[i];
                std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
        }
        if (c) {
            g_teamClass_121 = (jclass)env->NewGlobalRef(c);
            env->DeleteLocalRef(c);
        }
    }
    if (!g_abstractTeamClass_121) {
        const char* names[] = { "net.minecraft.world.scores.Team", "net.minecraft.scoreboard.AbstractTeam", "net.minecraft.class_270", "eyi", nullptr };
        jclass c = nullptr;
        for (int i = 0; names[i] && !c; i++) {
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
            if (!c) {
                std::string alt = names[i];
                std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
        }
        if (c) {
            g_abstractTeamClass_121 = (jclass)env->NewGlobalRef(c);
            env->DeleteLocalRef(c);
        }
    }
    if (!g_visibilityRuleClass_121) {
        const char* names[] = {
            "net.minecraft.world.scores.Team$Visibility",
            "net.minecraft.scoreboard.AbstractTeam$VisibilityRule",
            "net.minecraft.class_270$class_272",
            "eyi$b",
            nullptr
        };
        jclass c = nullptr;
        for (int i = 0; names[i] && !c; i++) {
            if (g_gameClassLoader) c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
            if (!c) {
                std::string alt = names[i];
                std::replace(alt.begin(), alt.end(), '.', '/');
                c = env->FindClass(alt.c_str());
                if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
            }
        }
        if (c) {
            g_visibilityRuleClass_121 = (jclass)env->NewGlobalRef(c);
            env->DeleteLocalRef(c);
        }
    }

    if (!g_worldGetScoreboard_121) {
        jclass worldCls = env->GetObjectClass(worldObj);
        if (worldCls && !env->ExceptionCheck()) {
            const char* names[] = { "getScoreboard", "method_8428", "M", nullptr };
            const char* sigs[] = {
                "()Lnet/minecraft/world/scores/Scoreboard;",
                "()Lnet/minecraft/scoreboard/Scoreboard;",
                "()Lnet/minecraft/class_269;",
                nullptr
            };
            for (int ni = 0; names[ni] && !g_worldGetScoreboard_121; ni++) {
                for (int si = 0; sigs[si] && !g_worldGetScoreboard_121; si++) {
                    g_worldGetScoreboard_121 = env->GetMethodID(worldCls, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_worldGetScoreboard_121 = nullptr; }
                }
            }
            env->DeleteLocalRef(worldCls);
        } else {
            env->ExceptionClear();
        }
    }

    if (g_scoreboardClass_121) {
        if (!g_scoreboardGetTeam_121) {
            const char* names[] = { "getPlayerTeam", "getTeam", "method_1153", "b", nullptr };
            const char* sigs[] = {
                "(Ljava/lang/String;)Lnet/minecraft/world/scores/PlayerTeam;",
                "(Ljava/lang/String;)Lnet/minecraft/world/scores/Team;",
                "(Ljava/lang/String;)Lnet/minecraft/scoreboard/Team;",
                "(Ljava/lang/String;)Lnet/minecraft/class_268;",
                nullptr
            };
            for (int ni = 0; names[ni] && !g_scoreboardGetTeam_121; ni++) {
                for (int si = 0; sigs[si] && !g_scoreboardGetTeam_121; si++) {
                    g_scoreboardGetTeam_121 = env->GetMethodID(g_scoreboardClass_121, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_scoreboardGetTeam_121 = nullptr; }
                }
            }
        }
        if (!g_scoreboardAddTeam_121) {
            const char* names[] = { "addPlayerTeam", "addTeam", "method_1171", "c", nullptr };
            const char* sigs[] = {
                "(Ljava/lang/String;)Lnet/minecraft/world/scores/PlayerTeam;",
                "(Ljava/lang/String;)Lnet/minecraft/world/scores/Team;",
                "(Ljava/lang/String;)Lnet/minecraft/scoreboard/Team;",
                "(Ljava/lang/String;)Lnet/minecraft/class_268;",
                nullptr
            };
            for (int ni = 0; names[ni] && !g_scoreboardAddTeam_121; ni++) {
                for (int si = 0; sigs[si] && !g_scoreboardAddTeam_121; si++) {
                    g_scoreboardAddTeam_121 = env->GetMethodID(g_scoreboardClass_121, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_scoreboardAddTeam_121 = nullptr; }
                }
            }
        }
        if (!g_scoreboardRemoveTeam_121) {
            const char* names[] = { "removePlayerTeam", "removeTeam", "method_1191", "d", nullptr };
            const char* sigs[] = {
                "(Lnet/minecraft/world/scores/PlayerTeam;)V",
                "(Lnet/minecraft/world/scores/Team;)V",
                "(Lnet/minecraft/scoreboard/Team;)V",
                "(Lnet/minecraft/class_268;)V",
                nullptr
            };
            for (int ni = 0; names[ni] && !g_scoreboardRemoveTeam_121; ni++) {
                for (int si = 0; sigs[si] && !g_scoreboardRemoveTeam_121; si++) {
                    g_scoreboardRemoveTeam_121 = env->GetMethodID(g_scoreboardClass_121, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_scoreboardRemoveTeam_121 = nullptr; }
                }
            }
        }
        if (!g_scoreboardAddHolderToTeam_121) {
            const char* names[] = { "addPlayerToTeam", "addScoreHolderToTeam", "method_1172", "a", nullptr };
            const char* sigs[] = {
                "(Ljava/lang/String;Lnet/minecraft/world/scores/PlayerTeam;)Z",
                "(Ljava/lang/String;Lnet/minecraft/world/scores/Team;)Z",
                "(Ljava/lang/String;Lnet/minecraft/scoreboard/Team;)Z",
                "(Ljava/lang/String;Lnet/minecraft/class_268;)Z",
                nullptr
            };
            for (int ni = 0; names[ni] && !g_scoreboardAddHolderToTeam_121; ni++) {
                for (int si = 0; sigs[si] && !g_scoreboardAddHolderToTeam_121; si++) {
                    g_scoreboardAddHolderToTeam_121 = env->GetMethodID(g_scoreboardClass_121, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_scoreboardAddHolderToTeam_121 = nullptr; }
                }
            }
        }
        if (!g_scoreboardGetHolderTeam_121) {
            const char* names[] = { "getPlayersTeam", "getScoreHolderTeam", "method_1164", "e", nullptr };
            const char* sigs[] = {
                "(Ljava/lang/String;)Lnet/minecraft/world/scores/PlayerTeam;",
                "(Ljava/lang/String;)Lnet/minecraft/world/scores/Team;",
                "(Ljava/lang/String;)Lnet/minecraft/scoreboard/Team;",
                "(Ljava/lang/String;)Lnet/minecraft/class_268;",
                nullptr
            };
            for (int ni = 0; names[ni] && !g_scoreboardGetHolderTeam_121; ni++) {
                for (int si = 0; sigs[si] && !g_scoreboardGetHolderTeam_121; si++) {
                    g_scoreboardGetHolderTeam_121 = env->GetMethodID(g_scoreboardClass_121, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_scoreboardGetHolderTeam_121 = nullptr; }
                }
            }
        }
        if (!g_scoreboardClearTeam_121) {
            const char* names[] = { "removePlayerFromTeam", "clearTeam", "method_1195", "d", nullptr };
            const char* sigs[] = { "(Ljava/lang/String;)Z", nullptr };
            for (int ni = 0; names[ni] && !g_scoreboardClearTeam_121; ni++) {
                for (int si = 0; sigs[si] && !g_scoreboardClearTeam_121; si++) {
                    g_scoreboardClearTeam_121 = env->GetMethodID(g_scoreboardClass_121, names[ni], sigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_scoreboardClearTeam_121 = nullptr; }
                }
            }
        }
    }

    if (g_abstractTeamClass_121 && !g_abstractTeamGetName_121) {
        const char* names[] = { "getName", "method_1197", "b", nullptr };
        for (int i = 0; names[i] && !g_abstractTeamGetName_121; i++) {
            g_abstractTeamGetName_121 = env->GetMethodID(g_abstractTeamClass_121, names[i], "()Ljava/lang/String;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_abstractTeamGetName_121 = nullptr; }
        }
    }

    if (g_teamClass_121 && !g_teamSetNameTagVisibilityRule_121) {
        const char* names[] = { "setNameTagVisibility", "setNameTagVisibilityRule", "method_1149", "a", nullptr };
        const char* sigs[] = {
            "(Lnet/minecraft/world/scores/Team$Visibility;)V",
            "(Lnet/minecraft/scoreboard/AbstractTeam$VisibilityRule;)V",
            "(Lnet/minecraft/class_270$class_272;)V",
            nullptr
        };
        for (int ni = 0; names[ni] && !g_teamSetNameTagVisibilityRule_121; ni++) {
            for (int si = 0; sigs[si] && !g_teamSetNameTagVisibilityRule_121; si++) {
                g_teamSetNameTagVisibilityRule_121 = env->GetMethodID(g_teamClass_121, names[ni], sigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_teamSetNameTagVisibilityRule_121 = nullptr; }
            }
        }
    }

    if (g_visibilityRuleClass_121 && !g_visibilityRuleNever_121) {
        const char* sigs[] = {
            "Lnet/minecraft/world/scores/Team$Visibility;",
            "Lnet/minecraft/scoreboard/AbstractTeam$VisibilityRule;",
            "Lnet/minecraft/class_270$class_272;",
            nullptr
        };
        jfieldID neverField = nullptr;
        for (int si = 0; sigs[si] && !neverField; si++) {
            neverField = env->GetStaticFieldID(g_visibilityRuleClass_121, "NEVER", sigs[si]);
            if (env->ExceptionCheck()) { env->ExceptionClear(); neverField = nullptr; }
        }
        if (neverField) {
            jobject neverObj = env->GetStaticObjectField(g_visibilityRuleClass_121, neverField);
            if (env->ExceptionCheck()) { env->ExceptionClear(); neverObj = nullptr; }
            if (neverObj) {
                g_visibilityRuleNever_121 = env->NewGlobalRef(neverObj);
                env->DeleteLocalRef(neverObj);
            }
        }

        if (!g_visibilityRuleNever_121) {
            const char* valueSigs[] = {
                "()[Lnet/minecraft/world/scores/Team$Visibility;",
                "()[Lnet/minecraft/scoreboard/AbstractTeam$VisibilityRule;",
                "()[Lnet/minecraft/class_270$class_272;",
                nullptr
            };
            jmethodID valuesMid = nullptr;
            for (int si = 0; valueSigs[si] && !valuesMid; si++) {
                valuesMid = env->GetStaticMethodID(g_visibilityRuleClass_121, "values", valueSigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); valuesMid = nullptr; }
            }
            if (valuesMid) {
                jobjectArray vals = (jobjectArray)env->CallStaticObjectMethod(g_visibilityRuleClass_121, valuesMid);
                if (env->ExceptionCheck()) { env->ExceptionClear(); vals = nullptr; }
                if (vals) {
                    jsize len = env->GetArrayLength(vals);
                    if (len > 1) {
                        jobject neverObj = env->GetObjectArrayElement(vals, 1);
                        if (neverObj) {
                            g_visibilityRuleNever_121 = env->NewGlobalRef(neverObj);
                            env->DeleteLocalRef(neverObj);
                        }
                    }
                    env->DeleteLocalRef(vals);
                }
            }
        }
    }

    bool coreReady = AreNametagSuppressionCoreMappingsReady121();
    if (!coreReady) {
        LogNametagSuppressionMissingMappings121(env, worldObj);
    }
    return coreReady;
}

static jobject GetScoreboard121(JNIEnv* env, jobject worldObj) {
    if (!env || !worldObj || !g_worldGetScoreboard_121) return nullptr;
    jobject scoreboardObj = env->CallObjectMethod(worldObj, g_worldGetScoreboard_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); scoreboardObj = nullptr; }
    return scoreboardObj;
}

static jobject EnsureNametagHideTeam121(JNIEnv* env, jobject scoreboardObj) {
    if (!env || !scoreboardObj || !g_scoreboardGetTeam_121 || !g_scoreboardAddTeam_121) return nullptr;

    static const char* kHideTeamName = "lc_hide_tags";
    jstring jTeamName = env->NewStringUTF(kHideTeamName);
    if (!jTeamName) return nullptr;

    jobject teamObj = env->CallObjectMethod(scoreboardObj, g_scoreboardGetTeam_121, jTeamName);
    if (env->ExceptionCheck()) { env->ExceptionClear(); teamObj = nullptr; }
    if (!teamObj) {
        teamObj = env->CallObjectMethod(scoreboardObj, g_scoreboardAddTeam_121, jTeamName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); teamObj = nullptr; }
    }

    if (teamObj && g_teamSetNameTagVisibilityRule_121 && g_visibilityRuleNever_121) {
        env->CallVoidMethod(teamObj, g_teamSetNameTagVisibilityRule_121, g_visibilityRuleNever_121);
        if (env->ExceptionCheck()) { env->ExceptionClear(); }
    }

    env->DeleteLocalRef(jTeamName);
    return teamObj;
}

static bool ApplyVanillaNametagSuppression121(JNIEnv* env, jobject scoreboardObj, jobject hideTeamObj, const std::string& playerName) {
    if (!env || !scoreboardObj || !hideTeamObj || playerName.empty()) return false;

    jstring jPlayerName = env->NewStringUTF(playerName.c_str());
    if (!jPlayerName) return false;

    if (g_hiddenNametagOriginalTeamByPlayer_121.find(playerName) == g_hiddenNametagOriginalTeamByPlayer_121.end()) {
        std::string originalTeamName;
        if (g_scoreboardGetHolderTeam_121 && g_abstractTeamGetName_121) {
            jobject originalTeamObj = env->CallObjectMethod(scoreboardObj, g_scoreboardGetHolderTeam_121, jPlayerName);
            if (env->ExceptionCheck()) { env->ExceptionClear(); originalTeamObj = nullptr; }
            if (originalTeamObj) {
                jstring jOriginalName = (jstring)env->CallObjectMethod(originalTeamObj, g_abstractTeamGetName_121);
                if (env->ExceptionCheck()) { env->ExceptionClear(); jOriginalName = nullptr; }
                if (jOriginalName) {
                    originalTeamName = Utf8FromJString(env, jOriginalName);
                    env->DeleteLocalRef(jOriginalName);
                }
                env->DeleteLocalRef(originalTeamObj);
            }
        }
        g_hiddenNametagOriginalTeamByPlayer_121[playerName] = originalTeamName;
    }

    jboolean added = env->CallBooleanMethod(scoreboardObj, g_scoreboardAddHolderToTeam_121, jPlayerName, hideTeamObj);
    bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    env->DeleteLocalRef(jPlayerName);
    return ok && (added == JNI_TRUE || added == JNI_FALSE);
}

static void RestoreVanillaNametagSuppression121(JNIEnv* env, jobject scoreboardObj) {
    if (!env || !scoreboardObj) {
        g_hiddenNametagOriginalTeamByPlayer_121.clear();
        return;
    }

    if (!g_scoreboardClearTeam_121 && !g_loggedNametagRestoreUnavailable_121) {
        g_loggedNametagRestoreUnavailable_121 = true;
        Log("NametagHideVanilla: Scoreboard.clearTeam missing; using fallback restore path.");
    }

    for (const auto& entry : g_hiddenNametagOriginalTeamByPlayer_121) {
        const std::string& playerName = entry.first;
        const std::string& originalTeamName = entry.second;
        if (playerName.empty()) continue;

        jstring jPlayerName = env->NewStringUTF(playerName.c_str());
        if (!jPlayerName) continue;
        if (g_scoreboardClearTeam_121) {
            env->CallBooleanMethod(scoreboardObj, g_scoreboardClearTeam_121, jPlayerName);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        if (!originalTeamName.empty() && g_scoreboardGetTeam_121 && g_scoreboardAddHolderToTeam_121) {
            jstring jOriginalTeam = env->NewStringUTF(originalTeamName.c_str());
            if (jOriginalTeam) {
                jobject teamObj = env->CallObjectMethod(scoreboardObj, g_scoreboardGetTeam_121, jOriginalTeam);
                if (env->ExceptionCheck()) { env->ExceptionClear(); teamObj = nullptr; }
                if (teamObj) {
                    env->CallBooleanMethod(scoreboardObj, g_scoreboardAddHolderToTeam_121, jPlayerName, teamObj);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    env->DeleteLocalRef(teamObj);
                }
                env->DeleteLocalRef(jOriginalTeam);
            }
        }
        env->DeleteLocalRef(jPlayerName);
    }
    g_hiddenNametagOriginalTeamByPlayer_121.clear();

    if (g_scoreboardGetTeam_121 && g_scoreboardRemoveTeam_121) {
        jstring jHideTeamName = env->NewStringUTF("lc_hide_tags");
        if (jHideTeamName) {
            jobject hideTeamObj = env->CallObjectMethod(scoreboardObj, g_scoreboardGetTeam_121, jHideTeamName);
            if (env->ExceptionCheck()) { env->ExceptionClear(); hideTeamObj = nullptr; }
            if (hideTeamObj) {
                env->CallVoidMethod(scoreboardObj, g_scoreboardRemoveTeam_121, hideTeamObj);
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(hideTeamObj);
            }
            env->DeleteLocalRef(jHideTeamName);
        }
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
static HMODULE g_hModule121 = nullptr;
static HANDLE  g_mainThreadHandle = nullptr;
static HANDLE  g_chestThreadHandle = nullptr;
static HANDLE  g_fastPollThreadHandle = nullptr;

#define GLFW_CURSOR            0x00033001
#define GLFW_CURSOR_NORMAL     0x00034001
#define GLFW_CURSOR_DISABLED   0x00034003
#define GLFW_RAW_MOUSE_MOTION  0x00033005

// ===================== LOGGER =====================
static std::string g_logPath = "bridge_261_debug.log";

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

static void DeleteGlobalRefSafe(JNIEnv* env, jobject& obj) {
    if (env && obj) {
        env->DeleteGlobalRef(obj);
        obj = nullptr;
    }
}

static void DeleteGlobalRefSafe(JNIEnv* env, jclass& cls) {
    if (env && cls) {
        env->DeleteGlobalRef(cls);
        cls = nullptr;
    }
}

static void CleanupJniGlobals(JNIEnv* env) {
    if (!env) return;

    DeleteGlobalRefSafe(env, g_gameClassLoader);
    DeleteGlobalRefSafe(env, g_mcInstance);
    DeleteGlobalRefSafe(env, g_chatScreenClass);

    DeleteGlobalRefSafe(env, g_renderSystemClass_121);
    DeleteGlobalRefSafe(env, g_matrix4fClass_121);
    DeleteGlobalRefSafe(env, g_cameraClass_121);
    DeleteGlobalRefSafe(env, g_vec3dClass_121);

    DeleteGlobalRefSafe(env, g_gameProfileClass_121);
    DeleteGlobalRefSafe(env, g_hitResultClass_121);
    DeleteGlobalRefSafe(env, g_blockHitResultClass_121);
    for (int i = 0; i < 4; i++) DeleteGlobalRefSafe(env, g_chestBlockEntityClasses[i]);
    DeleteGlobalRefSafe(env, g_blockEntityClass_121);
    DeleteGlobalRefSafe(env, g_blockPosClass_121);
    DeleteGlobalRefSafe(env, g_javaHashMapClass);
    DeleteGlobalRefSafe(env, g_playerEntityClass_121);
    DeleteGlobalRefSafe(env, g_scoreboardClass_121);
    DeleteGlobalRefSafe(env, g_teamClass_121);
    DeleteGlobalRefSafe(env, g_abstractTeamClass_121);
    DeleteGlobalRefSafe(env, g_visibilityRuleClass_121);
    DeleteGlobalRefSafe(env, g_visibilityRuleNever_121);
    DeleteGlobalRefSafe(env, g_lastNametagSuppressionWorld_121);
    DeleteGlobalRefSafe(env, g_itemStackClass_121);
    DeleteGlobalRefSafe(env, g_identifierClass_121);
    DeleteGlobalRefSafe(env, g_entityReachIdentifier_121);
    DeleteGlobalRefSafe(env, g_blockReachIdentifier_121);
    DeleteGlobalRefSafe(env, g_reachRegistryEntry_121);

    DeleteGlobalRefSafe(env, g_cachedLocalPlayer);
    DeleteGlobalRefSafe(env, g_cachedReachAttrInst);
}

static void ResetModernJniRuntimeCaches121(JNIEnv* env, const char* reason) {
    if (!env) return;

    CleanupJniGlobals(env);
    ResetNametagSuppressionCaches121(env, nullptr);

    g_setScreenMethod = nullptr;
    g_chatScreenCtor = nullptr;
    g_chatCtorKind = 0;
    g_screenField = nullptr;
    g_chatJniReady = false;
    g_stateJniReady = false;
    g_screenType.clear();
    g_lastLoggedScreen.clear();

    g_jniScreenName.clear();
    g_jniActionBar.clear();
    g_jniGuiOpen = false;
    g_jniInWorld = false;
    g_jniLookingAtBlock = false;
    g_jniLookingAtEntity = false;
    g_jniLookingAtEntityLatched = false;
    g_jniBreakingBlock = false;
    g_jniHoldingBlock = false;
    g_jniAttackCooldown = 1.0f;
    g_jniAttackCooldownPerTick = 0.08f;
    g_jniStateMs = 0;
    g_lastEntitySeenMs = 0;
    g_loggedCooldownProgressResolve = false;
    g_loggedCooldownPerTickResolve = false;
    g_loggedCooldownProgressMissing = false;
    g_loggedCooldownPerTickMissing = false;
    g_loggedCooldownPlayerFieldMissing = false;
    g_loggedCooldownPlayerObjectMissing = false;
    g_lastCooldownProgressFallbackLogMs = 0;
    g_lastCooldownPerTickFallbackLogMs = 0;
    g_lastCooldownSampleLogMs = 0;

    g_inGameHudField_121 = nullptr;
    g_hudTextFields_121.clear();
    g_lastHudTextProbeMs = 0;

    g_reachJniInit = false;
    g_reachMethodsResolved = false;
    g_getAttributes_121 = nullptr;
    g_getCustomInstance_121 = nullptr;
    g_setBaseValue_121 = nullptr;
    g_dynGetAttributes = nullptr;
    g_dynGetCustomInstance = nullptr;
    g_dynGetAttributeInstance = nullptr;
    g_dynRegistryEntryToString = nullptr;
    g_dynRegistryEntryMatchesIdentifier = nullptr;
    g_dynSetBaseValue = nullptr;
    g_identifierFromString_121 = nullptr;

    g_velocityMethodsResolved = false;
    g_getVelocity_121 = nullptr;
    g_setVelocityVec_121 = nullptr;
    g_setVelocityXYZ_121 = nullptr;
    g_hurtTimeField_121 = nullptr;
    g_vec3dCtor_121 = nullptr;
    g_lastHurtTime_121 = 0;
    g_loggedVelocityResolveFail_121 = false;

    g_getProjectionMatrix_121 = nullptr;
    g_getModelViewMatrix_121 = nullptr;
    g_matrixGetFloatArray_121 = nullptr;
    g_matrixM00 = g_matrixM01 = g_matrixM02 = g_matrixM03 = nullptr;
    g_matrixM10 = g_matrixM11 = g_matrixM12 = g_matrixM13 = nullptr;
    g_matrixM20 = g_matrixM21 = g_matrixM22 = g_matrixM23 = nullptr;
    g_matrixM30 = g_matrixM31 = g_matrixM32 = g_matrixM33 = nullptr;

    g_gameRendererField_121 = nullptr;
    g_gameRendererCameraField_121 = nullptr;
    g_cameraPosF_121 = nullptr;
    g_cameraYawF_121 = nullptr;
    g_cameraPitchF_121 = nullptr;
    g_vec3dX_121 = g_vec3dY_121 = g_vec3dZ_121 = nullptr;
    g_lunarProjField_121 = nullptr;
    g_lunarViewField_121 = nullptr;
    g_optionsField_121 = nullptr;
    g_fovField_121 = nullptr;
    g_simpleOptionGet_121 = nullptr;

    g_worldGetChunkMethod_121 = nullptr;
    g_chunkBlockEntitiesMapField_121 = nullptr;
    g_beGetPos_121 = nullptr;
    g_blockPosX_121 = nullptr;
    g_blockPosY_121 = nullptr;
    g_blockPosZ_121 = nullptr;
    g_beBlockPosField_121 = nullptr;
    g_entityPosField_121 = nullptr;
    g_javaHashMapTableField = nullptr;
    g_javaHMNodeValueField = nullptr;
    g_javaHMNodeNextField = nullptr;
    g_javaHMNodeKeyField = nullptr;
    g_blockPosGetX_121 = nullptr;
    g_blockPosGetY_121 = nullptr;
    g_blockPosGetZ_121 = nullptr;
    g_beGetCachedState_121 = nullptr;
    g_stateGetBlock_121 = nullptr;
    g_blockGetTranslationKey_121 = nullptr;
    for (int i = 0; i < 4; i++) g_chestBlockEntityClasses[i] = nullptr;

    g_hitResultGetType_121 = nullptr;
    g_crosshairTargetField_121 = nullptr;

    g_worldField_121 = nullptr;
    g_playerField_121 = nullptr;
    g_worldPlayersListField_121 = nullptr;
    g_getX_121 = nullptr;
    g_getY_121 = nullptr;
    g_getZ_121 = nullptr;
    g_getYaw_121 = nullptr;
    g_getPitch_121 = nullptr;
    g_getHealth_121 = nullptr;
    g_getName_121 = nullptr;
    g_setCustomNameVisible_121 = nullptr;
    g_isCustomNameVisible_121 = nullptr;
    g_shouldRenderName_121 = nullptr;
    g_textGetString_121 = nullptr;
    g_getArmor_121 = nullptr;
    g_getMainHandStack_121 = nullptr;
    g_itemStackGetName_121 = nullptr;
    g_itemStackGetDamage_121 = nullptr;
    g_itemStackGetMaxDamage_121 = nullptr;
    g_getGameProfile_121 = nullptr;
    g_gameProfileGetName_121 = nullptr;

    g_lastPlayerListUpdateMs = 0;
    g_lastClosestUpdateMs = 0;
    g_lastChestScanMs = 0;
    g_worldTransitionEndMs = GetTickCount() + 1000;

    {
        LockGuard lk(g_playerListMutex);
        g_playerList.clear();
    }
    {
        LockGuard lk(g_chestListMutex);
        g_chestList.clear();
    }
    {
        LockGuard lk(g_bgCamMutex);
        g_bgCamState = BgCamState();
    }

    if (reason && *reason) {
        Log(std::string("ReloadMappings: reset JNI caches (") + reason + ").");
    }
}

static void CleanupImGuiAndHooks() {
    if (g_hwnd && o_WndProc) {
        SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)o_WndProc);
        o_WndProc = nullptr;
    }

    if (g_imguiPhase1Done) {
        ImGui_ImplWin32_Shutdown();
        g_imguiPhase1Done = false;
    }
    if (g_imguiGlBackendReady || g_imguiInitialized) {
        ImGui_ImplOpenGL3_SetSkipGLDeletes(true);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplOpenGL3_SetSkipGLDeletes(false);
    }
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }

    g_imguiInitialized = false;
    g_imguiGlBackendReady = false;
    g_imguiPendingBackendReset = false;
    g_imguiPendingGlrc = nullptr;
    g_imguiGlrc = nullptr;
    g_hwnd = nullptr;

    g_ShowMenu = false;
    g_realGuiOpen = false;
    g_reqOpenMenu = 0;
    g_reqCloseMenu = 0;
    g_blockUntilMs = 0;
    g_enableRawMouseAtMs = 0;

    MH_DisableHook(MH_ALL_HOOKS);
    o_wglSwapBuffers = nullptr;
    MH_Uninitialize();
}

extern "C" __declspec(dllexport) void Detach() {
    Log("Detach requested (26.1)");
    g_running = false;

    if (g_clientSocket != INVALID_SOCKET) {
        closesocket(g_clientSocket);
        g_clientSocket = INVALID_SOCKET;
    }
    if (g_serverSocket != INVALID_SOCKET) {
        closesocket(g_serverSocket);
        g_serverSocket = INVALID_SOCKET;
    }

    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        for (int i = 0; i < 30; i++) {
            bool chestDone = !g_chestThreadHandle || WaitForSingleObject(g_chestThreadHandle, 50) == WAIT_OBJECT_0;
            bool pollDone = !g_fastPollThreadHandle || WaitForSingleObject(g_fastPollThreadHandle, 50) == WAIT_OBJECT_0;
            if (chestDone && pollDone) break;
            Sleep(25);
        }

        if (g_chestThreadHandle) { CloseHandle(g_chestThreadHandle); g_chestThreadHandle = nullptr; }
        if (g_fastPollThreadHandle) { CloseHandle(g_fastPollThreadHandle); g_fastPollThreadHandle = nullptr; }

        JNIEnv* env = nullptr;
        bool attached = false;
        if (g_jvm) {
            if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) != JNI_OK) {
                if (g_jvm->AttachCurrentThread((void**)&env, nullptr) == JNI_OK) {
                    attached = true;
                } else {
                    env = nullptr;
                }
            }
        }

        CleanupImGuiAndHooks();
        CleanupJniGlobals(env);

        if (attached && g_jvm) g_jvm->DetachCurrentThread();

        if (g_mainThreadHandle) {
            CloseHandle(g_mainThreadHandle);
            g_mainThreadHandle = nullptr;
        }

        HMODULE self = g_hModule121;
        if (!self) self = GetModuleHandleA("bridge_261.dll");
        if (self) {
            FreeLibraryAndExitThread(self, 0);
        }
        return 0;
    }, nullptr, 0, nullptr);
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
    if (!env) return nullptr;

    jclass cThread = env->FindClass("java/lang/Thread");
    if (!cThread || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (cThread) env->DeleteLocalRef(cThread);
        return nullptr;
    }

    jmethodID mGetAll = env->GetStaticMethodID(cThread, "getAllStackTraces", "()Ljava/util/Map;");
    if (!mGetAll || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(cThread);
        return nullptr;
    }

    jobject map = env->CallStaticObjectMethod(cThread, mGetAll);
    if (!map || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(cThread);
        if (map) env->DeleteLocalRef(map);
        return nullptr;
    }

    jclass cMap = env->FindClass("java/util/Map");
    jclass cSet = env->FindClass("java/util/Set");
    if (!cMap || !cSet || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (cMap) env->DeleteLocalRef(cMap);
        if (cSet) env->DeleteLocalRef(cSet);
        env->DeleteLocalRef(map);
        env->DeleteLocalRef(cThread);
        return nullptr;
    }

    jmethodID mKeySet = env->GetMethodID(cMap, "keySet", "()Ljava/util/Set;");
    jmethodID mToArray = env->GetMethodID(cSet, "toArray", "()[Ljava/lang/Object;");
    jmethodID mName = env->GetMethodID(cThread, "getName", "()Ljava/lang/String;");
    jmethodID mGetCL = env->GetMethodID(cThread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    if (!mKeySet || !mToArray || !mName || !mGetCL || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(cSet);
        env->DeleteLocalRef(cMap);
        env->DeleteLocalRef(map);
        env->DeleteLocalRef(cThread);
        return nullptr;
    }

    jobject set = env->CallObjectMethod(map, mKeySet);
    if (!set || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (set) env->DeleteLocalRef(set);
        env->DeleteLocalRef(cSet);
        env->DeleteLocalRef(cMap);
        env->DeleteLocalRef(map);
        env->DeleteLocalRef(cThread);
        return nullptr;
    }

    jobjectArray threads = (jobjectArray)env->CallObjectMethod(set, mToArray);
    if (!threads || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (threads) env->DeleteLocalRef(threads);
        env->DeleteLocalRef(set);
        env->DeleteLocalRef(cSet);
        env->DeleteLocalRef(cMap);
        env->DeleteLocalRef(map);
        env->DeleteLocalRef(cThread);
        return nullptr;
    }

    jsize count = env->GetArrayLength(threads);

    // Look for "Render thread" (1.21) or "Client thread" (1.8.9)
    for (int i = 0; i < count; i++) {
        jobject t = env->GetObjectArrayElement(threads, i);
        if (!t || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (t) env->DeleteLocalRef(t);
            continue;
        }

        jstring jn = (jstring)env->CallObjectMethod(t, mName);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(t);
            continue;
        }

        const char* cn = jn ? env->GetStringUTFChars(jn, nullptr) : nullptr;
        bool found = cn && (
            strstr(cn, "Render thread") != nullptr ||
            strstr(cn, "Client thread") != nullptr ||
            strstr(cn, "main") != nullptr);

        if (cn) env->ReleaseStringUTFChars(jn, cn);
        if (jn) env->DeleteLocalRef(jn);

        if (found) {
            jobject cl = env->CallObjectMethod(t, mGetCL);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                cl = nullptr;
            }
            env->DeleteLocalRef(t);
            env->DeleteLocalRef(threads);
            env->DeleteLocalRef(set);
            env->DeleteLocalRef(cSet);
            env->DeleteLocalRef(cMap);
            env->DeleteLocalRef(map);
            env->DeleteLocalRef(cThread);
            return cl;
        }

        env->DeleteLocalRef(t);
    }

    env->DeleteLocalRef(threads);
    env->DeleteLocalRef(set);
    env->DeleteLocalRef(cSet);
    env->DeleteLocalRef(cMap);
    env->DeleteLocalRef(map);
    env->DeleteLocalRef(cThread);
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
    TRACE261_PATH("enter");
    Log("Starting JNI discovery for 26.1...");

    // Get JVMTI
    jvmtiEnv* jvmti = nullptr;
    bool jvmtiReady = TRACE261_IF("jvmtiReady", (g_jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_2) == JNI_OK && jvmti));
    if (!jvmtiReady) {
        Log("ERROR: Failed to get JVMTI"); return false;
    }
    jint classCount = 0; jclass* classes = nullptr;
    jvmti->GetLoadedClasses(&classCount, &classes);
    Log("Loaded classes: " + std::to_string(classCount));

    // Get game classloader
    jobject gcl = GetGameClassLoader(env);
    TRACE261_BRANCH("gameClassLoaderAvailable", gcl != nullptr);
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

    // Try known names first (prioritize unobfuscated 26.1 and Mojmap)
    const char* knownMC[] = {
        "net.minecraft.client.Minecraft",
        "net.minecraft.client.MinecraftClient",
        "net.minecraft.class_310",
        nullptr
    };
    for (int i = 0; knownMC[i]; i++) {
        jclass c = LoadClassWithLoader(env, gcl, knownMC[i]);
        TRACE261_BRANCH("mcKnownNameHit", c != nullptr);
        if (c) { mcClass = c; mcName = knownMC[i]; TRACE261_VALUE("mcClassSource", "known-name"); Log("Found MC by name: " + mcName); break; }
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
    TRACE261_BRANCH("singletonFieldResolved", singletonField != nullptr);
    if (!singletonField) { Log("ERROR: No singleton field"); jvmti->Deallocate((unsigned char*)classes); return false; }

    jobject mcInst = env->GetStaticObjectField(mcClass, singletonField);
    TRACE261_BRANCH("mcInstanceAvailable", mcInst != nullptr);
    if (!mcInst) { Log("ERROR: MC instance null"); jvmti->Deallocate((unsigned char*)classes); return false; }
    Log("Got Minecraft instance.");

    // Diagnostics disabled

    // ---- Step 3: Find screen field by direct names, then hierarchy walk ----
    // In 26.x some clients use Mojmap/official names, others use intermediary.
    std::string screenType;
    auto tryScreenField = [&](const char* fieldName, const char* fieldSig) -> bool {
        if (!fieldName || !fieldSig || g_screenField) return false;
        jfieldID fid = env->GetFieldID(mcClass, fieldName, fieldSig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
        if (!fid) return false;

        g_screenField = fid;
        std::string tn = fieldSig;
        if (tn.size() > 2 && tn[0] == 'L' && tn.back() == ';') {
            tn = tn.substr(1, tn.size() - 2);
            std::replace(tn.begin(), tn.end(), '/', '.');
        }
        screenType = tn;
        g_screenType = tn;
        Log("Screen field (direct): " + std::string(fieldName) + " type=" + tn);
        return true;
    };

    const char* directScreenNames[] = {
        "field_1755",
        "screen",
        "currentScreen",
        nullptr
    };
    const char* directScreenSigs[] = {
        "Lnet/minecraft/class_437;",
        "Lnet/minecraft/client/gui/screen/Screen;",
        "Lnet/minecraft/client/gui/screens/Screen;",
        "Lnet/minecraft/client/gui/GuiScreen;",
        nullptr
    };
    for (int ni = 0; directScreenNames[ni] && !g_screenField; ni++) {
        for (int si = 0; directScreenSigs[si] && !g_screenField; si++) {
            bool hit = tryScreenField(directScreenNames[ni], directScreenSigs[si]);
            TRACE261_BRANCH("screenDirectLookupHit", hit);
        }
    }

    if (g_screenField && !screenType.empty()) {
        TRACE261_VALUE("screenType", screenType);
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
        TRACE261_BRANCH("setScreenPreferredSetScreenHit", g_setScreenMethod != nullptr);
        if (!g_setScreenMethod) {
            g_setScreenMethod = env->GetMethodID(mcClass, "method_1507", fullSig.c_str());
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_setScreenMethod = nullptr; }
            TRACE261_BRANCH("setScreenPreferredMethod1507Hit", g_setScreenMethod != nullptr);
        }
        if (g_setScreenMethod) {
            Log(std::string("setScreen method (preferred): sig=") + fullSig);
        }

    }

    // ---- Step 5: Find ChatScreen (subclass of Screen with String constructor) ----
    if (!screenType.empty()) {
        TRACE261_PATH("resolve-chat-screen");
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
            TRACE261_BRANCH("chatKnownNameClassHit", c != nullptr);
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
                        if (g_chatScreenClass) {
                            env->DeleteGlobalRef(g_chatScreenClass);
                            g_chatScreenClass = nullptr;
                        }
                        g_chatScreenClass = (jclass)env->NewGlobalRef(c);
                        g_chatScreenCtor  = ctor;
                        g_chatCtorKind    = kind;
                        TRACE261_VALUE("chatScreenSource", knownChat[i]);
                        Log("Found ChatScreen by name: " + std::string(knownChat[i]) + " ctorSig=" + sig);
                        break;
                    }
                }
            }
        }
    }

    // ---- Step 6: Find options field and FOV ----
    const char* optionsNames[] = { "options", "field_1690", nullptr };
    for (int i = 0; optionsNames[i] && !g_optionsField_121; i++) {
        const char* optionsSigs[] = { "Lnet/minecraft/client/Options;", "Lnet/minecraft/class_315;", nullptr };
        for (int si = 0; optionsSigs[si] && !g_optionsField_121; si++) {
            g_optionsField_121 = env->GetFieldID(mcClass, optionsNames[i], optionsSigs[si]);
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_optionsField_121 = nullptr; }
        }
        if (g_optionsField_121) Log("Found options field: " + std::string(optionsNames[i]));
    }

    if (g_optionsField_121) {
        jobject optsObj = env->GetObjectField(mcInst, g_optionsField_121);
        if (optsObj && !env->ExceptionCheck()) {
            jclass optsCls = env->GetObjectClass(optsObj);
            const char* fovNames[] = { "fov", "field_1903", nullptr };
            for (int i = 0; fovNames[i] && !g_fovField_121; i++) {
                const char* fovSigs[] = { "Lnet/minecraft/client/OptionInstance;", "Lnet/minecraft/class_7172;", nullptr };
                for (int si = 0; fovSigs[si] && !g_fovField_121; si++) {
                    g_fovField_121 = env->GetFieldID(optsCls, fovNames[i], fovSigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_fovField_121 = nullptr; }
                }
                if (g_fovField_121) {
                    Log("Found fov field: " + std::string(fovNames[i]));
                    // Resolve OptionInstance.get() / SimpleOption.getValue()
                    jclass optInstCls = LoadClassWithLoader(env, gcl, "net.minecraft.client.OptionInstance");
                    if (!optInstCls) optInstCls = LoadClassWithLoader(env, gcl, "net.minecraft.class_7172");
                    if (optInstCls) {
                        g_simpleOptionGet_121 = env->GetMethodID(optInstCls, "get", "()Ljava/lang/Object;");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); g_simpleOptionGet_121 = env->GetMethodID(optInstCls, "method_41753", "()Ljava/lang/Object;"); if (env->ExceptionCheck()) env->ExceptionClear(); }
                        env->DeleteLocalRef(optInstCls);
                    }
                }
            }
            env->DeleteLocalRef(optsCls);
            env->DeleteLocalRef(optsObj);
        } else if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // ---- Step 7: Store globals ----
    if (g_mcInstance) {
        env->DeleteGlobalRef(g_mcInstance);
        g_mcInstance = nullptr;
    }
    g_mcInstance = env->NewGlobalRef(mcInst);
    g_stateJniReady = (g_screenField != nullptr);
    g_chatJniReady  = (g_setScreenMethod != nullptr && g_chatScreenClass != nullptr && g_chatScreenCtor != nullptr);
    TRACE261_BRANCH("stateJniReady", g_stateJniReady);
    TRACE261_BRANCH("chatJniReady", g_chatJniReady);


    if (!g_renderSystemClass_121) {
        Log("Attempting to load RenderSystem class...");
        jclass rsCls = LoadClassWithLoader(env, gcl, "com.mojang.blaze3d.systems.RenderSystem");
        if (rsCls) {
            Log("Loaded RenderSystem class");
            g_renderSystemClass_121 = (jclass)env->NewGlobalRef(rsCls);
            
            Log("Trying getProjectionMatrix() -> Matrix4f");
            g_getProjectionMatrix_121 = env->GetStaticMethodID(rsCls, "getProjectionMatrix", "()Lorg/joml/Matrix4f;");
            if (g_getProjectionMatrix_121) {
                Log("Found getProjectionMatrix with JOML Matrix4f signature");
            } else {
                env->ExceptionClear();
                Log("Trying getProjectionMatrix() -> Minecraft Matrix4f");
                g_getProjectionMatrix_121 = env->GetStaticMethodID(rsCls, "getProjectionMatrix", "()Lnet/minecraft/class_10366;");
                if (g_getProjectionMatrix_121) {
                    Log("Found getProjectionMatrix with Minecraft Matrix4f signature");
                } else {
                    env->ExceptionClear();
                    // Try Mojmap field name variant
                    Log("Trying projectionMatrix static field (Mojmap)");
                    jfieldID projField = env->GetStaticFieldID(rsCls, "projectionMatrix", "Lorg/joml/Matrix4f;");
                    if (projField && !env->ExceptionCheck()) {
                        Log("Found projectionMatrix as static field (will need accessor wrapper)");
                        // Store the field ID for later use (we'll need to adapt the code that calls this)
                    } else {
                        env->ExceptionClear();
                        Log("WARNING: getProjectionMatrix not found as method or field");
                    }
                }
            }
            
            Log("Trying getModelViewMatrix() -> Matrix4f");
            g_getModelViewMatrix_121 = env->GetStaticMethodID(rsCls, "getModelViewMatrix", "()Lorg/joml/Matrix4f;");
            if (g_getModelViewMatrix_121) {
                Log("Found getModelViewMatrix");
            } else {
                env->ExceptionClear();
                Log("WARNING: getModelViewMatrix not found");
            }
            env->DeleteLocalRef(rsCls);
        } else {
            if (env->ExceptionCheck()) env->ExceptionClear();
            Log("WARNING: Could not load RenderSystem class");
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

    // Resolve GameRenderer to get Camera without invoking crashing reflection scans.
    if (!g_gameRendererField_121) {
        TRACE261_PATH("resolve-gamerenderer-field");
        const char* gameRendererSigs[] = {
            "Lnet/minecraft/class_757;",
            "Lnet/minecraft/client/render/GameRenderer;",
            "Lnet/minecraft/client/renderer/GameRenderer;",
            nullptr
        };
        for (int i = 0; gameRendererSigs[i] && !g_gameRendererField_121; i++) {
            Log(std::string("Trying GameRenderer sig: ") + gameRendererSigs[i]);
            g_gameRendererField_121 = FindFieldByType(env, mcClass, gameRendererSigs[i]);
            if (g_gameRendererField_121) {
                TRACE261_VALUE("gameRendererFieldSource", gameRendererSigs[i]);
                Log("Found GameRenderer field with sig: " + std::string(gameRendererSigs[i]));
            }
        }
        if (!g_gameRendererField_121) {
            Log("WARNING: GameRenderer field not found with any known signature");
        }
    }
    if (g_gameRendererField_121) {
        jclass grCls = nullptr;
        const char* gameRendererNames[] = {
            "net.minecraft.class_757",
            "net.minecraft.client.render.GameRenderer",
            "net.minecraft.client.renderer.GameRenderer",
            nullptr
        };
        for (int i = 0; gameRendererNames[i] && !grCls; i++) {
            Log(std::string("Trying to load GameRenderer class: ") + gameRendererNames[i]);
            grCls = LoadClassWithLoader(env, gcl, gameRendererNames[i]);
            if (env->ExceptionCheck()) { env->ExceptionClear(); grCls = nullptr; }
            if (grCls) {
                TRACE261_VALUE("gameRendererClassSource", gameRendererNames[i]);
                Log("Loaded GameRenderer class: " + std::string(gameRendererNames[i]));
            }
        }
        if (grCls) {
            const char* cameraFieldNames[] = { "field_18765", "camera", "mainCamera", "f_109099_", nullptr };
            const char* cameraFieldSigs[] = {
                "Lnet/minecraft/class_4184;",
                "Lnet/minecraft/client/render/Camera;",
                "Lnet/minecraft/client/renderer/Camera;",
                "Lcom/mojang/blaze3d/platform/Camera;",
                nullptr
            };
            for (int ni = 0; cameraFieldNames[ni] && !g_gameRendererCameraField_121; ni++) {
                for (int si = 0; cameraFieldSigs[si] && !g_gameRendererCameraField_121; si++) {
                    Log(std::string("Trying camera field: ") + cameraFieldNames[ni] + " sig: " + cameraFieldSigs[si]);
                    g_gameRendererCameraField_121 = env->GetFieldID(grCls, cameraFieldNames[ni], cameraFieldSigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_gameRendererCameraField_121 = nullptr; }
                    else if (g_gameRendererCameraField_121) {
                        TRACE261_VALUE("cameraFieldSource", std::string(cameraFieldNames[ni]) + "|" + cameraFieldSigs[si]);
                        Log(std::string("Found camera field: ") + cameraFieldNames[ni] + " with sig: " + cameraFieldSigs[si]);
                        break;
                    }
                }
            }
            
            // Reflection scan fallback: find first field with "Camera" in its type
            if (!g_gameRendererCameraField_121) {
                TRACE261_PATH("camera-field-reflection-fallback");
                Log("Scanning GameRenderer fields via reflection...");
                jclass cClass = env->FindClass("java/lang/Class");
                jclass cField = env->FindClass("java/lang/reflect/Field");
                jmethodID mGetFields = cClass ? env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
                jmethodID mFType = cField ? env->GetMethodID(cField, "getType", "()Ljava/lang/Class;") : nullptr;
                jmethodID mFName = cField ? env->GetMethodID(cField, "getName", "()Ljava/lang/String;") : nullptr;
                if (env->ExceptionCheck()) env->ExceptionClear();
                
                if (mGetFields && mFType && mFName) {
                    jobjectArray fields = (jobjectArray)env->CallObjectMethod(grCls, mGetFields);
                    if (fields && !env->ExceptionCheck()) {
                        jsize fc = env->GetArrayLength(fields);
                        for (int i = 0; i < fc && !g_gameRendererCameraField_121; i++) {
                            jobject fld = env->GetObjectArrayElement(fields, i);
                            if (!fld) continue;
                            
                            jclass ftype = (jclass)env->CallObjectMethod(fld, mFType);
                            jstring fname = (jstring)env->CallObjectMethod(fld, mFName);
                            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                            
                            std::string ftypeName = ftype ? GetClassNameFromClass(env, ftype) : "";
                            const char* fnameC = fname ? env->GetStringUTFChars(fname, nullptr) : nullptr;
                            std::string fnameStr = fnameC ? fnameC : "";
                            if (fnameC) env->ReleaseStringUTFChars(fname, fnameC);
                            
                            if (ftypeName.find("Camera") != std::string::npos || ftypeName.find("camera") != std::string::npos) {
                                Log("Found Camera-like field via reflection: " + fnameStr + " type=" + ftypeName);
                                // Try to get the field ID using the discovered name
                                std::string sig = "L" + ftypeName + ";";
                                std::replace(sig.begin(), sig.end(), '.', '/');
                                g_gameRendererCameraField_121 = env->GetFieldID(grCls, fnameStr.c_str(), sig.c_str());
                                if (env->ExceptionCheck()) { 
                                    env->ExceptionClear(); 
                                    g_gameRendererCameraField_121 = nullptr; 
                                } else if (g_gameRendererCameraField_121) {
                                    TRACE261_VALUE("cameraFieldSource", std::string("reflection|") + fnameStr + "|" + sig);
                                    Log("Successfully got field ID for camera field");
                                    break;
                                }
                            }
                            
                            if (ftype) env->DeleteLocalRef(ftype);
                            if (fname) env->DeleteLocalRef(fname);
                            env->DeleteLocalRef(fld);
                        }
                        env->DeleteLocalRef(fields);
                    } else if (env->ExceptionCheck()) env->ExceptionClear();
                }
                if (cClass) env->DeleteLocalRef(cClass);
                if (cField) env->DeleteLocalRef(cField);
            }
            
            if (!g_gameRendererCameraField_121) {
                Log("WARNING: Camera field not found in GameRenderer even after reflection scan");
            }
            env->DeleteLocalRef(grCls);
        } else {
            Log("WARNING: Could not load GameRenderer class");
        }
    }

    // Get Camera class by actually getting the camera instance from GameRenderer
    jclass camCls = nullptr;
    if (!camCls && g_gameRendererCameraField_121 && g_gameRendererField_121) {
        TRACE261_PATH("camera-class-runtime-path");
        Log("Getting Camera class from runtime instance...");
        jobject grObj = env->GetObjectField(g_mcInstance, g_gameRendererField_121);
        if (grObj && !env->ExceptionCheck()) {
            jobject camObj = env->GetObjectField(grObj, g_gameRendererCameraField_121);
            if (camObj && !env->ExceptionCheck()) {
                camCls = env->GetObjectClass(camObj);
                if (camCls) {
                    std::string camClassName = GetClassNameFromClass(env, camCls);
                    Log("Got Camera class from runtime instance: " + camClassName);
                }
                env->DeleteLocalRef(camObj);
            } else if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(grObj);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
    
    if (camCls) {
        g_cameraClass_121 = (jclass)env->NewGlobalRef(camCls);
        
        // Try direct field lookups first
        const char* cameraPosNames[] = { "field_18712", "pos", "position", "f_90570_", nullptr };
        const char* cameraPosSigs[] = {
            "Lnet/minecraft/class_243;",
            "Lnet/minecraft/util/math/Vec3d;",
            "Lnet/minecraft/world/phys/Vec3;",
            "Lorg/joml/Vector3f;",
            nullptr
        };
        for (int ni = 0; cameraPosNames[ni] && !g_cameraPosF_121; ni++) {
            for (int si = 0; cameraPosSigs[si] && !g_cameraPosF_121; si++) {
                Log(std::string("Trying camera pos field: ") + cameraPosNames[ni] + " sig: " + cameraPosSigs[si]);
                g_cameraPosF_121 = env->GetFieldID(camCls, cameraPosNames[ni], cameraPosSigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_cameraPosF_121 = nullptr; }
                else if (g_cameraPosF_121) {
                    TRACE261_VALUE("cameraPosFieldSource", std::string(cameraPosNames[ni]) + "|" + cameraPosSigs[si]);
                    Log(std::string("Found camera pos field: ") + cameraPosNames[ni] + " with sig: " + cameraPosSigs[si]);
                    break;
                }
            }
        }
        
        const char* yawNames[] = { "field_18715", "yaw", "yRot", "f_90571_", "yRotation", nullptr };
        for (int i = 0; yawNames[i] && !g_cameraYawF_121; i++) {
            Log(std::string("Trying camera yaw field: ") + yawNames[i]);
            g_cameraYawF_121 = env->GetFieldID(camCls, yawNames[i], "F");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_cameraYawF_121 = nullptr; }
            else if (g_cameraYawF_121) {
                Log(std::string("Found camera yaw field: ") + yawNames[i]);
                break;
            }
        }

        const char* pitchNames[] = { "field_18714", "pitch", "xRot", "f_90572_", "xRotation", nullptr };
        for (int i = 0; pitchNames[i] && !g_cameraPitchF_121; i++) {
            Log(std::string("Trying camera pitch field: ") + pitchNames[i]);
            g_cameraPitchF_121 = env->GetFieldID(camCls, pitchNames[i], "F");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_cameraPitchF_121 = nullptr; }
            else if (g_cameraPitchF_121) {
                Log(std::string("Found camera pitch field: ") + pitchNames[i]);
                break;
            }
        }

        env->DeleteLocalRef(camCls);
    } else {
        Log("WARNING: Could not load Camera class");
    }

    // Get Vec3d class from camera position field if we found it
    jclass vecCls = nullptr;
    if (g_cameraPosF_121 && g_cameraClass_121 && g_gameRendererField_121 && g_gameRendererCameraField_121) {
        Log("Getting Vec3d class from camera position runtime instance...");
        jobject grObj = env->GetObjectField(g_mcInstance, g_gameRendererField_121);
        if (grObj && !env->ExceptionCheck()) {
            jobject camObj = env->GetObjectField(grObj, g_gameRendererCameraField_121);
            if (camObj && !env->ExceptionCheck()) {
                jobject posObj = env->GetObjectField(camObj, g_cameraPosF_121);
                if (posObj && !env->ExceptionCheck()) {
                    vecCls = env->GetObjectClass(posObj);
                    if (vecCls) {
                        std::string vecClassName = GetClassNameFromClass(env, vecCls);
                        Log("Got Vec3d class from runtime instance: " + vecClassName);
                    }
                    env->DeleteLocalRef(posObj);
                } else if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                }
                env->DeleteLocalRef(camObj);
            } else if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(grObj);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
    
    if (vecCls) {
        g_vec3dClass_121 = (jclass)env->NewGlobalRef(vecCls);
        const char* vecXNames[] = { "field_1352", "x", "f_82479_", "xCoord", nullptr };
        const char* vecYNames[] = { "field_1351", "y", "f_82480_", "yCoord", nullptr };
        const char* vecZNames[] = { "field_1350", "field_1353", "z", "f_82481_", "zCoord", nullptr };
        
        // Try both double and float types
        for (int i = 0; vecXNames[i] && !g_vec3dX_121; i++) {
            Log(std::string("Trying Vec3d x field: ") + vecXNames[i]);
            g_vec3dX_121 = env->GetFieldID(vecCls, vecXNames[i], "D");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_vec3dX_121 = nullptr; }
            else if (g_vec3dX_121) {
                Log(std::string("Found Vec3d x field (double): ") + vecXNames[i]);
                break;
            }
        }
        for (int i = 0; vecYNames[i] && !g_vec3dY_121; i++) {
            Log(std::string("Trying Vec3d y field: ") + vecYNames[i]);
            g_vec3dY_121 = env->GetFieldID(vecCls, vecYNames[i], "D");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_vec3dY_121 = nullptr; }
            else if (g_vec3dY_121) {
                Log(std::string("Found Vec3d y field (double): ") + vecYNames[i]);
                break;
            }
        }
        for (int i = 0; vecZNames[i] && !g_vec3dZ_121; i++) {
            Log(std::string("Trying Vec3d z field: ") + vecZNames[i]);
            g_vec3dZ_121 = env->GetFieldID(vecCls, vecZNames[i], "D");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_vec3dZ_121 = nullptr; }
            else if (g_vec3dZ_121) {
                Log(std::string("Found Vec3d z field (double): ") + vecZNames[i]);
                break;
            }
        }
        env->DeleteLocalRef(vecCls);
    } else {
        Log("WARNING: Could not load Vec3d class");
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
    TRACE261_PATH("enter");
    bool canUseJni = TRACE261_IF("jniChatReady", (g_chatJniReady && g_jvm));
    if (canUseJni) {
        JNIEnv* env = nullptr;
        bool envReady = TRACE261_IF("jniEnvReady", (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env));
        if (envReady) {
            jobject chatInst = nullptr;
            if (g_chatCtorKind == 0) {
                TRACE261_PATH("chat-ctor-kind-0");
                chatInst = env->NewObject(g_chatScreenClass, g_chatScreenCtor);
            } else if (g_chatCtorKind == 1) {
                TRACE261_PATH("chat-ctor-kind-1");
                jstring empty = env->NewStringUTF("");
                chatInst = env->NewObject(g_chatScreenClass, g_chatScreenCtor, empty);
                env->DeleteLocalRef(empty);
            } else if (g_chatCtorKind == 2) {
                TRACE261_PATH("chat-ctor-kind-2");
                jstring empty = env->NewStringUTF("");
                chatInst = env->NewObject(g_chatScreenClass, g_chatScreenCtor, empty, (jboolean)JNI_FALSE);
                env->DeleteLocalRef(empty);
            }
            
            bool chatCreated = TRACE261_IF("chatScreenCreated", (!env->ExceptionCheck() && chatInst));
            if (chatCreated) {
                env->CallVoidMethod(g_mcInstance, g_setScreenMethod, chatInst);
                env->ExceptionClear();
                env->DeleteLocalRef(chatInst);
                TRACE261_PATH("jni-chat-open-success");
                return; // MC handles cursor natively
            }
            TRACE261_PATH("jni-chat-open-failed");
            env->ExceptionClear();
        }
    }
    // GLFW fallback (when JNI not ready or failed)
    TRACE261_PATH("glfw-chat-open-fallback");
    if (glfwGetCurrentContext_fn && glfwSetInputMode_fn) {
        void* win = glfwGetCurrentContext_fn();
        TRACE261_BRANCH("glfwWindowAvailable", win != nullptr);
        if (win) {
            glfwSetInputMode_fn(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            // Disable raw mouse motion while our GUI is open to prevent camera drift.
            if (glfwSetInputMode_fn) glfwSetInputMode_fn(win, GLFW_RAW_MOUSE_MOTION, 0);
        }
    }
}
static void CloseChatScreen() {
    TRACE261_PATH("enter");
    bool canUseJni = TRACE261_IF("jniChatReady", (g_chatJniReady && g_jvm));
    if (canUseJni) {
        JNIEnv* env = nullptr;
        bool envReady = TRACE261_IF("jniEnvReady", (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env));
        if (envReady) {
            env->CallVoidMethod(g_mcInstance, g_setScreenMethod, nullptr);
            env->ExceptionClear();
            TRACE261_PATH("jni-chat-close-issued");
        }
    }
    // GLFW safety: ensure Minecraft is put back into mouse-captured mode.
    TRACE261_PATH("glfw-chat-close-path");
    if (glfwGetCurrentContext_fn && glfwSetInputMode_fn) {
        void* win = glfwGetCurrentContext_fn();
        TRACE261_BRANCH("glfwWindowAvailable", win != nullptr);
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
    TRACE261_PATH("enter");
    bool prerequisites = TRACE261_IF("prerequisitesMet", (env && g_mcInstance));
    if (!prerequisites) return false;
    if (!g_worldField_121) {
        TRACE261_PATH("resolve-world-field");
        jclass mcCls = env->GetObjectClass(g_mcInstance);
        if (mcCls) {
            const char* worldNames[] = { "field_1687", "world", "level", "f_91073_", nullptr };
            const char* worldSigs[] = {
                "Lnet/minecraft/class_638;",
                "Lnet/minecraft/client/multiplayer/ClientLevel;",
                "Lnet/minecraft/client/world/ClientWorld;",
                "Lnet/minecraft/world/level/Level;",
                nullptr
            };
            for (int ni = 0; worldNames[ni] && !g_worldField_121; ni++) {
                for (int si = 0; worldSigs[si] && !g_worldField_121; si++) {
                    Log(std::string("Trying world field: ") + worldNames[ni] + " sig: " + worldSigs[si]);
                    g_worldField_121 = env->GetFieldID(mcCls, worldNames[ni], worldSigs[si]);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_worldField_121 = nullptr; }
                    else if (g_worldField_121) {
                        TRACE261_VALUE("worldFieldSource", std::string(worldNames[ni]) + "|" + worldSigs[si]);
                        Log(std::string("Found world field: ") + worldNames[ni] + " with sig: " + worldSigs[si]);
                        break;
                    }
                }
            }
            if (!g_worldField_121) {
                Log("WARNING: World field unresolved from known names/signatures.");
            }
            env->DeleteLocalRef(mcCls);
        }
    }
    TRACE261_BRANCH("worldFieldResolved", g_worldField_121 != nullptr);
    if (!g_worldField_121) return false;
    jobject worldCheck = env->GetObjectField(g_mcInstance, g_worldField_121);
    if (env->ExceptionCheck()) { env->ExceptionClear(); worldCheck = nullptr; }
    if (!worldCheck) {
        static bool s_worldNullDiag = false;
        if (!s_worldNullDiag) {
            s_worldNullDiag = true;
            Log("WARNING: World field found but is null (not in world yet)");
        }
        return false;
    }
    env->DeleteLocalRef(worldCheck);
    
    static bool s_worldFoundDiag = false;
    if (!s_worldFoundDiag) {
        s_worldFoundDiag = true;
        Log("World field is non-null, inWorld should be true");
    }
    return true;
}

static void EnsureHudTextFields(JNIEnv* env, jclass mcCls, jobject hudObj) {
    TRACE261_PATH("enter");
    bool prerequisites = TRACE261_IF("prerequisitesMet", (env && mcCls));
    if (!prerequisites) return;

    if (!g_inGameHudField_121) {
        TRACE261_PATH("resolve-ingamehud-field");
        const char* hudSigs[] = {
            "Lnet/minecraft/class_329;",
            "Lnet/minecraft/client/gui/Gui;",
            "Lnet/minecraft/client/gui/hud/InGameHud;",
            nullptr
        };
        for (int si = 0; hudSigs[si] && !g_inGameHudField_121; si++) {
            const char* hudNames[] = { "gui", "inGameHud", "field_1705", nullptr };
            for (int ni = 0; hudNames[ni] && !g_inGameHudField_121; ni++) {
                g_inGameHudField_121 = env->GetFieldID(mcCls, hudNames[ni], hudSigs[si]);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_inGameHudField_121 = nullptr; }
                TRACE261_BRANCH("inGameHudFieldCandidateHit", g_inGameHudField_121 != nullptr);
                if (g_inGameHudField_121) TRACE261_VALUE("inGameHudFieldSource", std::string(hudNames[ni]) + "|" + hudSigs[si]);
            }
        }
    }

    TRACE261_BRANCH("hudObjAvailable", hudObj != nullptr);
    if (!hudObj) return;
    TRACE261_BRANCH("hudTextAlreadyResolved", !g_hudTextFields_121.empty());
    if (!g_hudTextFields_121.empty()) return;
    DWORD now = GetTickCount();
    bool probeDue = (now - g_lastHudTextProbeMs >= 2000);
    TRACE261_BRANCH("hudTextProbeDue", probeDue);
    if (!probeDue) return;
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

    TRACE261_PATH("hudtext-reflection-fallback");
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

    env->DeleteLocalRef(hudCls);
}

static void UpdateJniState() {
    TRACE261_PATH("enter");
    bool prerequisites = TRACE261_IF("prerequisitesMet", (g_stateJniReady && g_jvm && g_mcInstance && g_screenField));
    if (!prerequisites) return;
    JNIEnv* env = nullptr;
    bool envReady = TRACE261_IF("jniEnvReady", (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK && env));
    if (!envReady) return;
    unsigned long long nowMs = (unsigned long long)GetTickCount64();

    jobject scr = env->GetObjectField(g_mcInstance, g_screenField);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    bool guiOpen = (scr != nullptr);
    TRACE261_BRANCH("guiOpen", guiOpen);
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
        // NOTE: avoid forcing per-screen cache resets here; it causes remap thrash and
        // prevents mapping convergence on some runtimes.
    }

    // ===== lookingAtBlock (crosshair hit = BLOCK) =====
    bool lookingAtBlock = false;
    bool lookingAtEntity = false;

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
        TRACE261_BRANCH("crosshairFieldResolved", hitFld != nullptr);
        if (!guiOpen && hitFld) {
            jobject hitObj = env->GetObjectField(g_mcInstance, hitFld);
            if (env->ExceptionCheck()) { env->ExceptionClear(); hitObj = nullptr; }
            if (hitObj) {
                int hitOrd = GetHitResultTypeOrdinal(env, hitObj);
                if (hitOrd >= 0) {
                    TRACE261_PATH("hitresult-ordinal-path");
                    lookingAtBlock = (hitOrd == 1);
                    lookingAtEntity = (hitOrd == 2);
                }
                env->DeleteLocalRef(hitObj);
            }
        }

        // ===== breakingBlock (actual mining) =====
        bool breakingBlock = false;
        jfieldID imFld = env->GetFieldID(mcCls, "gameMode", "Lnet/minecraft/client/multiplayer/MultiPlayerGameMode;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); imFld = nullptr; }
        if (!imFld) {
            imFld = env->GetFieldID(mcCls, "field_1761", "Lnet/minecraft/class_636;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); imFld = nullptr; }
        }
        if (!imFld) {
            imFld = env->GetFieldID(mcCls, "field_1761", "Lnet/minecraft/client/multiplayer/MultiPlayerGameMode;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); imFld = nullptr; }
        }
        if (imFld) {
            jobject imObj = env->GetObjectField(g_mcInstance, imFld);
            if (imObj && !env->ExceptionCheck()) {
                jclass imCls = env->GetObjectClass(imObj);
                if (imCls) {
                    jfieldID brkFld = env->GetFieldID(imCls, "field_3716", "Z"); // breakingBlock (common Yarn name)
                    if (env->ExceptionCheck()) { env->ExceptionClear(); brkFld = nullptr; }
                    if (!brkFld) {
                        brkFld = env->GetFieldID(imCls, "isDestroying", "Z");
                        if (env->ExceptionCheck()) { env->ExceptionClear(); brkFld = nullptr; }
                    }
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

        // Resolve and cache Minecraft.player field across known mappings.
        auto ResolvePlayerField = [&]() -> jfieldID {
            if (g_playerField_121) return g_playerField_121;
            const char* playerNames[] = { "player", "field_1724", "f_91074_", nullptr };
            const char* playerSigs[] = {
                "Lnet/minecraft/client/player/LocalPlayer;",
                "Lnet/minecraft/class_746;",
                "Lnet/minecraft/world/entity/player/Player;",
                nullptr
            };
            for (int ni = 0; playerNames[ni]; ni++) {
                for (int si = 0; playerSigs[si]; si++) {
                    jfieldID fid = env->GetFieldID(mcCls, playerNames[ni], playerSigs[si]);
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        fid = nullptr;
                    }
                    if (fid) {
                        g_playerField_121 = fid;
                        return fid;
                    }
                }
            }
            return nullptr;
        };

        // ===== holdingBlock (main hand item is BlockItem) =====
        bool holdingBlock = false;
        jfieldID plFld = ResolvePlayerField();
        if (plFld) {
            jobject plObj = env->GetObjectField(g_mcInstance, plFld);
            if (plObj && !env->ExceptionCheck()) {
                jclass plCls = env->GetObjectClass(plObj);
                if (plCls) {
                    // Resolve getMainHandStack() lazily.
                    static jmethodID s_getMainHand = nullptr;
                    if (!s_getMainHand) {
                        const char* names[] = { "getMainHandItem", "getMainHandStack", "method_6047", nullptr };
                        const char* sigs[] = { "()Lnet/minecraft/world/item/ItemStack;", "()Lnet/minecraft/class_1799;", nullptr };
                        for (int ni = 0; names[ni] && !s_getMainHand; ni++) {
                            for (int si = 0; sigs[si] && !s_getMainHand; si++) {
                                s_getMainHand = env->GetMethodID(plCls, names[ni], sigs[si]);
                                if (env->ExceptionCheck()) { env->ExceptionClear(); s_getMainHand = nullptr; }
                            }
                        }
                    }

                    jobject stackObj = (s_getMainHand) ? env->CallObjectMethod(plObj, s_getMainHand) : nullptr;
                    if (env->ExceptionCheck()) { env->ExceptionClear(); stackObj = nullptr; }
                    if (stackObj) {
                        jclass stCls = env->GetObjectClass(stackObj);
                        static jmethodID s_getItem = nullptr;
                        if (stCls) {
                            if (!s_getItem) {
                                const char* names[] = { "getItem", "method_7909", nullptr };
                                const char* sigs[] = {
                                    "()Lnet/minecraft/world/item/Item;",
                                    "()Lnet/minecraft/item/Item;",
                                    "()Lnet/minecraft/class_1792;",
                                    nullptr
                                };
                                for (int ni = 0; names[ni] && !s_getItem; ni++) {
                                    for (int si = 0; sigs[si] && !s_getItem; si++) {
                                        s_getItem = env->GetMethodID(stCls, names[ni], sigs[si]);
                                        if (env->ExceptionCheck()) { env->ExceptionClear(); s_getItem = nullptr; }
                                    }
                                }
                            }
                            if (s_getItem) {
                                jobject itemObj = env->CallObjectMethod(stackObj, s_getItem);
                                if (env->ExceptionCheck()) { env->ExceptionClear(); itemObj = nullptr; }

                                // Resolve BlockItem class lazily.
                                static jclass s_blockItemCls = nullptr;
                                if (!s_blockItemCls) {
                                    const char* names[] = {
                                        "net.minecraft.world.item.BlockItem",
                                        "net.minecraft.item.BlockItem",
                                        "net.minecraft.class_1747",
                                        nullptr
                                    };
                                    for (int i = 0; names[i] && !s_blockItemCls; i++) {
                                        jclass c = LoadClassWithLoader(env, g_gameClassLoader, names[i]);
                                        if (c) s_blockItemCls = (jclass)env->NewGlobalRef(c);
                                        if (env->ExceptionCheck()) env->ExceptionClear();
                                    }
                                    if (!s_blockItemCls) {
                                        const char* slashNames[] = {
                                            "net/minecraft/world/item/BlockItem",
                                            "net/minecraft/item/BlockItem",
                                            "net/minecraft/class_1747",
                                            nullptr
                                        };
                                        for (int i = 0; slashNames[i] && !s_blockItemCls; i++) {
                                            jclass c = env->FindClass(slashNames[i]);
                                            if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
                                            if (c) s_blockItemCls = (jclass)env->NewGlobalRef(c);
                                        }
                                    }
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

        float attackCooldown = 1.0f;
        float attackCooldownPerTick = 0.08f;
        if (inWorld) {
            jfieldID plFld2 = ResolvePlayerField();
            if (!plFld2 && !g_loggedCooldownPlayerFieldMissing) {
                g_loggedCooldownPlayerFieldMissing = true;
                Log("CooldownJNI: failed to resolve player field (player/field_1724/f_91074_); using cooldown defaults.");
            }
            if (plFld2) {
                jobject plObj2 = env->GetObjectField(g_mcInstance, plFld2);
                if (plObj2 && !env->ExceptionCheck()) {
                    jclass plCls2 = env->GetObjectClass(plObj2);
                    if (plCls2) {
                        static jmethodID s_getAttackCooldownProgress = nullptr;
                        static jmethodID s_getAttackCooldownPerTick = nullptr;
                        if (!s_getAttackCooldownProgress) {
                            const char* names[] = { "getAttackStrengthScale", "getAttackCooldownProgress", "method_7261", nullptr };
                            for (int ni = 0; names[ni] && !s_getAttackCooldownProgress; ni++) {
                                s_getAttackCooldownProgress = env->GetMethodID(plCls2, names[ni], "(F)F");
                                if (s_getAttackCooldownProgress && !g_loggedCooldownProgressResolve) {
                                    g_loggedCooldownProgressResolve = true;
                                    Log(std::string("CooldownJNI: resolved progress method: ") + names[ni] + "(F)F");
                                }
                                if (env->ExceptionCheck()) { env->ExceptionClear(); s_getAttackCooldownProgress = nullptr; }
                            }
                            if (!s_getAttackCooldownProgress && !g_loggedCooldownProgressMissing) {
                                g_loggedCooldownProgressMissing = true;
                                Log("CooldownJNI: failed to resolve progress method; progress will fallback to 1.0.");
                            }
                        }

                        if (!s_getAttackCooldownPerTick) {
                            const char* names[] = { "getCurrentItemAttackStrengthDelay", "getAttackCooldownProgressPerTick", "method_7279", nullptr };
                            for (int ni = 0; names[ni] && !s_getAttackCooldownPerTick; ni++) {
                                s_getAttackCooldownPerTick = env->GetMethodID(plCls2, names[ni], "()F");
                                if (s_getAttackCooldownPerTick && !g_loggedCooldownPerTickResolve) {
                                    g_loggedCooldownPerTickResolve = true;
                                    Log(std::string("CooldownJNI: resolved perTick method: ") + names[ni] + "()F");
                                }
                                if (env->ExceptionCheck()) { env->ExceptionClear(); s_getAttackCooldownPerTick = nullptr; }
                            }
                            if (!s_getAttackCooldownPerTick && !g_loggedCooldownPerTickMissing) {
                                g_loggedCooldownPerTickMissing = true;
                                Log("CooldownJNI: failed to resolve perTick method; perTick will fallback to 0.08.");
                            }
                        }

                        if (s_getAttackCooldownProgress) {
                            attackCooldown = env->CallFloatMethod(plObj2, s_getAttackCooldownProgress, 0.0f);
                            if (env->ExceptionCheck()) {
                                env->ExceptionClear();
                                attackCooldown = 1.0f;
                                if ((nowMs - g_lastCooldownProgressFallbackLogMs) > 5000ULL) {
                                    g_lastCooldownProgressFallbackLogMs = nowMs;
                                    Log("CooldownJNI: progress call threw; fallback to 1.0.");
                                }
                            }
                        }

                        if (s_getAttackCooldownPerTick) {
                            attackCooldownPerTick = env->CallFloatMethod(plObj2, s_getAttackCooldownPerTick);
                            if (env->ExceptionCheck()) {
                                env->ExceptionClear();
                                attackCooldownPerTick = 0.08f;
                                if ((nowMs - g_lastCooldownPerTickFallbackLogMs) > 5000ULL) {
                                    g_lastCooldownPerTickFallbackLogMs = nowMs;
                                    Log("CooldownJNI: perTick call threw; fallback to 0.08.");
                                }
                            }
                        }

                        env->DeleteLocalRef(plCls2);
                    }
                    env->DeleteLocalRef(plObj2);
                } else {
                    env->ExceptionClear();
                    if (!g_loggedCooldownPlayerObjectMissing) {
                        g_loggedCooldownPlayerObjectMissing = true;
                        Log("CooldownJNI: player object unavailable; using cooldown defaults.");
                    }
                }
            }
        }

        if (!std::isfinite(attackCooldown)) attackCooldown = 1.0f;
        if (attackCooldown < 0.0f) attackCooldown = 0.0f;
        if (attackCooldown > 1.0f) attackCooldown = 1.0f;
        if (!std::isfinite(attackCooldownPerTick)) attackCooldownPerTick = 0.08f;
        if (attackCooldownPerTick <= 0.0f) attackCooldownPerTick = 0.08f;
        // Keep values > 1.0f intact: some mappings return cooldown period in ticks
        // (e.g., sword ~= 12.5), and C# normalizes by inverting these values.
        if (inWorld && (nowMs - g_lastCooldownSampleLogMs) > 5000ULL) {
            g_lastCooldownSampleLogMs = nowMs;
            Log("CooldownJNI sample: attackCooldown=" + std::to_string(attackCooldown)
                + " perTick=" + std::to_string(attackCooldownPerTick));
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
            if (!inWorld || guiOpen) {
                g_lastEntitySeenMs = 0;
            } else if (lookingAtEntity) {
                g_lastEntitySeenMs = nowMs;
            }
            bool lookingAtEntityLatched = g_lastEntitySeenMs != 0 && (nowMs - g_lastEntitySeenMs) <= 12ULL;

            g_jniScreenName = screenName;
            g_jniActionBar = actionBarText;
            g_jniGuiOpen = guiOpen;
            g_jniInWorld = inWorld;
            g_jniLookingAtBlock = lookingAtBlock;
            g_jniLookingAtEntity = lookingAtEntity;
            g_jniLookingAtEntityLatched = lookingAtEntityLatched;
            g_jniBreakingBlock = breakingBlock;
            g_jniHoldingBlock = holdingBlock;
            g_jniAttackCooldown = attackCooldown;
            g_jniAttackCooldownPerTick = attackCooldownPerTick;
            g_jniStateMs = nowMs;
        }
        return;
    }

    { LockGuard lk(g_jniStateMtx);
        g_jniScreenName = screenName;
        g_jniActionBar = actionBarText;
        g_jniGuiOpen = guiOpen;
        g_jniInWorld = inWorld;
        g_jniLookingAtBlock = lookingAtBlock;
        g_jniLookingAtEntity = lookingAtEntity;
        g_jniLookingAtEntityLatched = false;
        g_jniBreakingBlock = false;
        g_jniHoldingBlock = false;
        g_jniAttackCooldown = 1.0f;
        g_jniAttackCooldownPerTick = 0.08f;
        g_jniStateMs = nowMs;
        g_lastEntitySeenMs = 0;
    }
}

// ===================== WNDPROC HOOK =====================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT CALLBACK hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
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
    catStyle(selCategory == 2);
    if (ImGui::Selectable("  Risky", selCategory == 2, 0, ImVec2(SIDE_W-4, 28))) {
        if (selCategory != 2) { selCategory = 2; selModule = 0; }
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
        ModuleRow("tb",   "Triggerbot",    cfg.triggerbot, 2, "toggleTriggerbot");
    } else if (selCategory == 1) {
        // Render
        ModuleRow("ce",  "Chest ESP",      cfg.chestEsp,   0, "toggleChestEsp");
        ModuleRow("nt",  "Nametags",       cfg.nametags,   1, "toggleNametags");
        ModuleRow("cp",  "Closest Player", cfg.closestPlayer, 2, "toggleClosestPlayerInfo");
        ModuleRow("gtb", "GTB Helper",     cfg.gtbHelper,  3, "toggleGtbHelper");
    } else {
        // Risky
        ModuleRow("rh",  "Reach",          cfg.reachEnabled, 0, "toggleReach");
        ModuleRow("vl",  "Velocity",       cfg.velocityEnabled, 1, "toggleVelocity");
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

    } else if (selCategory == 0 && selModule == 2) {
        bool tb = cfg.triggerbot;
        if (ImGui::Checkbox("Enable Triggerbot", &tb)) SendCmd("toggleTriggerbot");
        ImGui::TextDisabled("Cooldown-based timing (1.9+ combat). Tune in Control Center.");

    } else if (selCategory == 1 && selModule == 0) {
        ImGui::TextDisabled("No extra settings.");

    } else if (selCategory == 1 && selModule == 1) {
        bool health = cfg.nametagHealth;
        if (ImGui::Checkbox("Show Health", &health)) SendCmd("toggleNametagHealth");
        bool armor = cfg.nametagArmor;
        if (ImGui::Checkbox("Show Armor", &armor)) SendCmd("toggleNametagArmor");
        bool hideVanilla = cfg.nametagHideVanilla;
        if (ImGui::Checkbox("Hide Vanilla Tags", &hideVanilla)) SendCmd("toggleNametagHideVanilla");

    } else if (selCategory == 1 && selModule == 2) {
        ImGui::TextDisabled("No extra settings.");
    } else if (selCategory == 1 && selModule == 3) {
        ImGui::TextDisabled("In-game hint panel only.");
    } else if (selCategory == 2 && selModule == 0) {
        bool reach = cfg.reachEnabled;
        if (ImGui::Checkbox("Enable Reach", &reach)) SendCmd("toggleReach");

        float rMin = cfg.reachMin;
        if (ImGui::SliderFloat("Min Range", &rMin, 3.0f, 6.0f, "%.1f"))
            SendCmdFloat("setReachMin", rMin);

        float rMax = cfg.reachMax;
        if (ImGui::SliderFloat("Max Range", &rMax, 3.0f, 6.0f, "%.1f"))
            SendCmdFloat("setReachMax", rMax);

        int rChance = cfg.reachChance;
        if (ImGui::SliderInt("Chance %", &rChance, 0, 100))
            { char buf[128]; snprintf(buf, sizeof(buf), "{\"type\":\"cmd\",\"action\":\"setReachChance\",\"value\":%d}\n", rChance); LockGuard lk(g_cmdMutex); g_pendingCmds.push_back(buf); }
            
        ImGui::Spacing();
        ImGui::TextDisabled("Attribute modification for 1.21.");
    } else if (selCategory == 2 && selModule == 1) {
        bool velocity = cfg.velocityEnabled;
        if (ImGui::Checkbox("Enable Velocity", &velocity)) SendCmd("toggleVelocity");

        int vHorizontal = cfg.velocityHorizontal;
        if (ImGui::SliderInt("Horizontal %", &vHorizontal, 1, 100))
            { char buf[128]; snprintf(buf, sizeof(buf), "{\"type\":\"cmd\",\"action\":\"setVelocityHorizontal\",\"value\":%d}\n", vHorizontal); LockGuard lk(g_cmdMutex); g_pendingCmds.push_back(buf); }

        int vVertical = cfg.velocityVertical;
        if (ImGui::SliderInt("Vertical %", &vVertical, 1, 100))
            { char buf[128]; snprintf(buf, sizeof(buf), "{\"type\":\"cmd\",\"action\":\"setVelocityVertical\",\"value\":%d}\n", vVertical); LockGuard lk(g_cmdMutex); g_pendingCmds.push_back(buf); }

        int vChance = cfg.velocityChance;
        if (ImGui::SliderInt("Chance %", &vChance, 1, 100))
            { char buf[128]; snprintf(buf, sizeof(buf), "{\"type\":\"cmd\",\"action\":\"setVelocityChance\",\"value\":%d}\n", vChance); LockGuard lk(g_cmdMutex); g_pendingCmds.push_back(buf); }

        ImGui::Spacing();
        ImGui::TextDisabled("Scales incoming knockback vectors.");
    }

    ImGui::EndChild();
    ImGui::End();
}

// ===================== SCREEN DETECTION =====================
// Combine GLFW cursor polling (fast) with JNI screen name (accurate).
static void UpdateRealGuiState() {
    TRACE261_PATH("enter");
    // JNI state (screen name) is already updated in UpdateJniState() each frame.
    // g_realGuiOpen = GLFW cursor visible AND not our own GUI.
    if (!glfwGetCurrentContext_fn || !glfwGetInputMode_fn) {
        TRACE261_PATH("glfw-unavailable");
        g_realGuiOpen = false;
        return;
    }
    void* win = glfwGetCurrentContext_fn();
    TRACE261_BRANCH("glfwWindowAvailable", win != nullptr);
    if (!win) { g_realGuiOpen = false; return; }
    int mode = glfwGetInputMode_fn(win, GLFW_CURSOR);
    TRACE261_VALUE("cursorMode", std::to_string(mode));
    // Only report realGuiOpen when cursor is NORMAL and it's NOT our own GUI
    // and the JNI screen is NOT the ChatScreen we opened for cursor unlock
    if (mode == GLFW_CURSOR_NORMAL && !g_ShowMenu) {
        TRACE261_PATH("cursor-normal-path");
        std::string sn;
        { LockGuard lk(g_jniStateMtx); sn = g_jniScreenName; }
        // "ChatScreen" is our own cursor-unlock screen, not a real Minecraft GUI
        g_realGuiOpen = !sn.empty() && sn != "ChatScreen";
    } else {
        TRACE261_PATH("cursor-nonnormal-or-menu-path");
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
    TRACE261_PATH("enter");
    bool prerequisites = TRACE261_IF("prerequisitesMet", (g_mcInstance && g_gameRendererField_121 && g_gameRendererCameraField_121));
    if (!prerequisites) return;

    BgCamState cs = {};
    cs.fov = 70.0f; // default

    if (g_optionsField_121 && g_fovField_121 && g_simpleOptionGet_121) {
        jobject opts = env->GetObjectField(g_mcInstance, g_optionsField_121);
        if (opts && !env->ExceptionCheck()) {
            jobject fovOpt = env->GetObjectField(opts, g_fovField_121);
            if (fovOpt && !env->ExceptionCheck()) {
                jobject valObj = env->CallObjectMethod(fovOpt, g_simpleOptionGet_121);
                if (valObj && !env->ExceptionCheck()) {
                    jclass valCls = env->GetObjectClass(valObj);
                    jmethodID doubleVal = env->GetMethodID(valCls, "doubleValue", "()D");
                    if (env->ExceptionCheck()) { env->ExceptionClear(); doubleVal = env->GetMethodID(valCls, "intValue", "()I"); if (env->ExceptionCheck()) env->ExceptionClear(); }
                    
                    if (doubleVal) {
                        std::string cn = GetClassNameFromClass(env, valCls);
                        if (cn.find("Double") != std::string::npos) cs.fov = (float)env->CallDoubleMethod(valObj, doubleVal);
                        else cs.fov = (float)env->CallIntMethod(valObj, doubleVal);
                    }
                    env->DeleteLocalRef(valCls);
                    env->DeleteLocalRef(valObj);
                }
                env->DeleteLocalRef(fovOpt);
            }
            env->DeleteLocalRef(opts);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

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
                    
                    static bool s_posFieldDiag = false;
                    if (!s_posFieldDiag) {
                        s_posFieldDiag = true;
                        Log(std::string("Camera pos read: x=") + std::to_string(cs.camX) + 
                            " y=" + std::to_string(cs.camY) + " z=" + std::to_string(cs.camZ));
                        Log(std::string("Vec class: ") + GetClassNameFromClass(env, env->GetObjectClass(vec)));
                    }
                    
                    env->DeleteLocalRef(vec);
                } else {
                    env->ExceptionClear();
                    static bool s_vecNullDiag = false;
                    if (!s_vecNullDiag) {
                        s_vecNullDiag = true;
                        Log("WARNING: Camera position vec is null or exception occurred");
                    }
                }
            }
            env->DeleteLocalRef(camera);
        } else env->ExceptionClear();

        // Matrices: cache Lunar saved-field IDs on first call (handles version-specific field suffixes)
        if (!g_lunarProjField_121 || !g_lunarViewField_121) {
            jclass grCls = env->GetObjectClass(gr);
            if (grCls) {
                // Try known names first (including 26.1)
                const char* projNames[] = { "lunar$savedProjection$v26_1", "lunar$savedProjection$v1_21", "lunar$savedProjection$v1_19_3", "lunar$savedProjection", nullptr };
                const char* viewNames[] = { "lunar$savedModelView$v26_1", "lunar$savedModelView$v1_21", "lunar$savedModelView$v1_19_3", "lunar$savedModelView", nullptr };
                const char* matSig = "Lorg/joml/Matrix4f;";

                for (int i = 0; projNames[i]; i++) {
                    g_lunarProjField_121 = env->GetFieldID(grCls, projNames[i], matSig);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_lunarProjField_121 = nullptr; }
                    if (g_lunarProjField_121) break;
                }
                for (int i = 0; viewNames[i]; i++) {
                    g_lunarViewField_121 = env->GetFieldID(grCls, viewNames[i], matSig);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_lunarViewField_121 = nullptr; }
                    if (g_lunarViewField_121) break;
                }

                // Reflection fallback for unknown suffixes
                if (!g_lunarProjField_121 || !g_lunarViewField_121) {
                    jclass cCls = env->FindClass("java/lang/Class");
                    jclass cFld = env->FindClass("java/lang/reflect/Field");
                    jmethodID mGF = cCls ? env->GetMethodID(cCls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
                    jmethodID mFT = cFld ? env->GetMethodID(cFld, "getType", "()Ljava/lang/Class;") : nullptr;
                    jmethodID mFN = cFld ? env->GetMethodID(cFld, "getName", "()Ljava/lang/String;") : nullptr;
                    if (env->ExceptionCheck()) { env->ExceptionClear(); mGF = mFT = mFN = nullptr; }
                    if (mGF && mFT && mFN) {
                        jobjectArray fields = (jobjectArray)env->CallObjectMethod(grCls, mGF);
                        if (fields && !env->ExceptionCheck()) {
                            jsize fc = env->GetArrayLength(fields);
                            for (int i = 0; i < fc; i++) {
                                jobject f = env->GetObjectArrayElement(fields, i);
                                if (!f) continue;
                                jclass ft = (jclass)env->CallObjectMethod(f, mFT);
                                std::string ftn = ft ? GetClassNameFromClass(env, ft) : "";
                                if (ft) env->DeleteLocalRef(ft);
                                if (ftn != "org.joml.Matrix4f") { env->DeleteLocalRef(f); continue; }
                                
                                jstring jfn = (jstring)env->CallObjectMethod(f, mFN);
                                const char* cfn = env->GetStringUTFChars(jfn, nullptr);
                                std::string fn = cfn ? cfn : "";
                                if (cfn) env->ReleaseStringUTFChars(jfn, cfn);
                                env->DeleteLocalRef(jfn);
                                env->DeleteLocalRef(f);

                                if (!g_lunarProjField_121 && fn.find("lunar$savedProjection") != std::string::npos) {
                                    g_lunarProjField_121 = env->GetFieldID(grCls, fn.c_str(), matSig);
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_lunarProjField_121 = nullptr; }
                                    else Log("Discovered Lunar projection matrix: " + fn);
                                }
                                if (!g_lunarViewField_121 && fn.find("lunar$savedModelView") != std::string::npos) {
                                    g_lunarViewField_121 = env->GetFieldID(grCls, fn.c_str(), matSig);
                                    if (env->ExceptionCheck()) { env->ExceptionClear(); g_lunarViewField_121 = nullptr; }
                                    else Log("Discovered Lunar modelview matrix: " + fn);
                                }
                                if (g_lunarProjField_121 && g_lunarViewField_121) break;
                            }
                        }
                        if (fields) env->DeleteLocalRef(fields);
                    }
                    if (cCls) env->DeleteLocalRef(cCls);
                    if (cFld) env->DeleteLocalRef(cFld);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
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

static DWORD WINAPI FastPollThreadProc(LPVOID) {
    JNIEnv* env = nullptr;
    if (!g_jvm || g_jvm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK) return 1;
    while (g_running) {
        if (g_stateJniReady) {
            LockGuard remapGuard(g_jniRemapMtx);
            if (g_stateJniReady) {
                UpdateJniState();
            }
        }
        Sleep(5); // 200Hz poll
    }
    g_jvm->DetachCurrentThread();
    return 0;
}

static DWORD WINAPI ChestScanThreadProc(LPVOID) {
    JNIEnv* env = nullptr;
    if (!g_jvm || g_jvm->AttachCurrentThread((void**)&env, nullptr) != JNI_OK) return 1;
    TRACE261_PATH("thread-start");
    DWORD lastAutoRemapRetryMs = 0;
    while (g_running) {
        Config cfg;
        { LockGuard lk(g_configMutex); cfg = g_config; }

        bool forcedRemap = (InterlockedExchange(&g_forceGlobalJniRemap_121, 0) != 0);
        TRACE261_BRANCH("forcedGlobalRemapPulse", forcedRemap);
        if (forcedRemap) {
            bool remapReady = false;
            {
                LockGuard remapGuard(g_jniRemapMtx);
                TRACE261_PATH("manual-remap-cycle");
                ResetModernJniRuntimeCaches121(env, "manual-reload-request");
                for (int attempt = 0; attempt < 8 && g_running; ++attempt) {
                    if (DiscoverJniMappings(env)) {
                        remapReady = true;
                        break;
                    }
                    Sleep(500);
                }
            }
            Log(std::string("ReloadMappings: full JNI remap ")
                + (remapReady ? "completed." : "still unresolved; auto-retry will continue."));
        }

        DWORD loopNow = GetTickCount();
        bool needAutoRetry = ((!g_stateJniReady || !g_mcInstance || !g_screenField) && (loopNow - lastAutoRemapRetryMs >= 5000));
        TRACE261_BRANCH("autoRemapRetryDue", needAutoRetry);
        if (needAutoRetry) {
            lastAutoRemapRetryMs = loopNow;
            LockGuard remapGuard(g_jniRemapMtx);
            TRACE261_PATH("auto-remap-retry");
            DiscoverJniMappings(env);
        }

        if (g_stateJniReady) {
            // All CallObjectMethod work runs here — never on the render thread.
            ReadCameraState(env);   // camera pos/yaw/pitch/matrices → g_bgCamState
            bool inWorldNow = IsInWorldNow(env);
            { LockGuard lk(g_jniStateMtx); g_jniInWorld = inWorldNow; }
            if (inWorldNow) {
                // Periodic diagnostic for chest esp config state
                static DWORD s_cfgLogMs = 0;
                DWORD nowMs = GetTickCount();
                if (nowMs - s_cfgLogMs > 10000) {
                    s_cfgLogMs = nowMs;
                    Log("ScanThread cfg: chestEsp=" + std::to_string(cfg.chestEsp) + " nametags=" + std::to_string(cfg.nametags) + " closestPlayer=" + std::to_string(cfg.closestPlayer) + " hideVanilla=" + std::to_string(cfg.nametagHideVanilla));
                }

                static bool s_reachWasEnabled = false;
                if (cfg.reachEnabled || s_reachWasEnabled) {
                    UpdateReach(env, cfg);
                    s_reachWasEnabled = cfg.reachEnabled;
                }

                static bool s_velocityWasEnabled = false;
                if (cfg.velocityEnabled || s_velocityWasEnabled) {
                    UpdateVelocity(env, cfg);
                    s_velocityWasEnabled = cfg.velocityEnabled;
                }

                if (cfg.closestPlayer)
                    UpdateClosestPlayerOverlay(env);
                if (cfg.nametags || cfg.closestPlayer || cfg.aimAssist || cfg.nametagHideVanilla || g_nametagSuppressionActive_121)
                    UpdatePlayerListOverlay(env);
                if (cfg.chestEsp)
                    UpdateChestList(env);
            } else {
                if (g_nametagSuppressionActive_121 || !g_hiddenNametagOriginalTeamByPlayer_121.empty()) {
                    ResetNametagSuppressionCaches121(env, "left-world");
                }
                { LockGuard lk(g_playerListMutex); g_playerList.clear(); }
                { LockGuard lk2(g_chestListMutex); g_chestList.clear(); }
                { LockGuard lk3(g_bgCamMutex); g_bgCamState = BgCamState(); }
            }
        } else {
            if (g_nametagSuppressionActive_121 || !g_hiddenNametagOriginalTeamByPlayer_121.empty()) {
                ResetNametagSuppressionCaches121(env, "jni-not-ready");
            }
            { LockGuard lk(g_playerListMutex); g_playerList.clear(); }
            { LockGuard lk2(g_chestListMutex); g_chestList.clear(); }
            { LockGuard lk3(g_bgCamMutex); g_bgCamState = BgCamState(); }
        }
        Sleep(cfg.aimAssist ? 5 : 50); // very fast poll for aim assist
    }
    g_jvm->DetachCurrentThread();
    return 0;
}

// ===================== HOOKED SwapBuffers =====================
// Frame counter: skip first few frames after GL backend init to let driver stabilize.
// Two-phase init: phase 1 = ImGui context + Win32 (no GL), phase 2 = GL backend (deferred).

BOOL WINAPI hwglSwapBuffers(HDC hDc) {
    TRACE261_PATH("enter");
    bool hasHdc = TRACE261_IF("hasHdc", hDc != nullptr);
    if (!hasHdc) return o_wglSwapBuffers(hDc);

    // If there is no current GL context, do not touch OpenGL/ImGui.
    HGLRC currentRc = wglGetCurrentContext();
    bool hasCurrentGlrc = TRACE261_IF("hasCurrentGlrc", currentRc != nullptr);
    if (!hasCurrentGlrc) return o_wglSwapBuffers(hDc);

    HWND window = WindowFromDC(hDc);
    bool hasWindow = TRACE261_IF("windowValid", (window && IsWindow(window)));
    if (!hasWindow) return o_wglSwapBuffers(hDc);

    // Only run on the main GLFW game window
    char cls[256] = {};
    bool classReadOk = TRACE261_IF("windowClassRead", GetClassNameA(window, cls, sizeof(cls)-1) != 0);
    if (!classReadOk)
        return o_wglSwapBuffers(hDc);
    bool isGlfwGameWindow = TRACE261_IF("isGlfwGameWindow", strcmp(cls, "GLFW30") == 0);
    if (!isGlfwGameWindow)
        return o_wglSwapBuffers(hDc);

    // ── Phase 1: ImGui context + Win32 backend (NO OpenGL calls at all) ──
    // Runs on the very first GLFW swap call.  We must not touch GL here so the
    // NVIDIA driver's internal swap-chain state is completely undisturbed.
    if (!g_imguiPhase1Done) {
        TRACE261_PATH("imgui-phase1-init");
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
        TRACE261_PATH("imgui-warmup-frame-skip");
        g_imguiWarmupFrames--;
        return o_wglSwapBuffers(hDc);
    }

    // If the GL context was recreated, defer a safe OpenGL backend reset.
    // Do NOT shutdown/reinit immediately: doing glDelete* on stale IDs in a different
    // context can corrupt Minecraft/Lunar rendering and may trigger nvoglv64.dll crashes.
    if (g_imguiInitialized && g_imguiGlBackendReady && g_imguiGlrc && currentRc != g_imguiGlrc) {
        TRACE261_PATH("imgui-glrc-changed-schedule-reset");
        Log("OpenGL context changed; scheduling ImGui OpenGL backend reset.");
        g_imguiPendingBackendReset = true;
        g_imguiPendingGlrc = currentRc;
        g_imguiWarmupFrames = 3;
        return o_wglSwapBuffers(hDc);
    }

    // Execute the pending reset on a clean frame, then defer actual Init to a later frame.
    if (g_imguiPendingBackendReset) {
        TRACE261_PATH("imgui-execute-deferred-reset");
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
        TRACE261_PATH("imgui-phase2-init");
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
            TRACE261_PATH("start-background-threads");
            s_chestThreadStarted = true;
            g_chestThreadHandle = CreateThread(nullptr, 0, ChestScanThreadProc, nullptr, 0, nullptr);
            g_fastPollThreadHandle = CreateThread(nullptr, 0, FastPollThreadProc, nullptr, 0, nullptr);
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
        OverlayTheme overlayTheme = ResolveOverlayTheme(cfg.guiTheme);
        bool inWorld = false;
        { LockGuard lk(g_jniStateMtx); inWorld = g_jniInWorld; }
        
        // Diagnostic logging (only once at start)
        static bool s_overlayDiagLogged = false;
        if (!s_overlayDiagLogged) {
            s_overlayDiagLogged = true;
            Log(std::string("Overlay render check: inWorld=") + (inWorld ? "true" : "false") + 
                " showMenu=" + (g_ShowMenu ? "true" : "false") + 
                " showModuleList=" + (cfg.showModuleList ? "true" : "false"));
        }
        
        if (!g_ShowMenu && inWorld) {
            TRACE261_PATH("overlay-render-active");
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImGuiIO& io = ImGui::GetIO();
            BgCamState cpCamState;
            { LockGuard lk(g_bgCamMutex); cpCamState = g_bgCamState; }

            // Snapshot player list under lock (background thread updates it concurrently).
            std::vector<PlayerData121> playerSnap;
            { LockGuard lk(g_playerListMutex); playerSnap = g_playerList; }

            // ── Closest Player: styled HUD box above hotbar ───
            bool renderClosestPlayer = TRACE261_IF("renderClosestPlayer", (cfg.closestPlayer && !playerSnap.empty()));
            if (renderClosestPlayer) {
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
                char dirArrow = '^';

                bool dirResolved = false;
                if (cpCamState.camFound) {
                    LegoVec3 camPos = { cpCamState.camX, cpCamState.camY, cpCamState.camZ };
                    LegoVec3 targetPos = { cp.ex, cp.ey + 1.2, cp.ez };
                    float sx = 0.0f, sy = 0.0f;
                    bool projected = cpCamState.matsOk
                        ? WorldToScreen(targetPos, camPos, cpCamState.view, cpCamState.proj,
                                        (int)io.DisplaySize.x, (int)io.DisplaySize.y, &sx, &sy)
                        : WorldToScreen_Angles(targetPos, camPos, cpCamState.yaw, cpCamState.pitch, cpCamState.fov,
                                               (int)io.DisplaySize.x, (int)io.DisplaySize.y, &sx, &sy);

                    if (projected) {
                        float dxScreen = sx - (io.DisplaySize.x * 0.5f);
                        float dyScreen = sy - (io.DisplaySize.y * 0.5f);
                        if (std::fabs(dxScreen) > std::fabs(dyScreen))
                            dirArrow = (dxScreen >= 0.0f) ? '>' : '<';
                        else
                            dirArrow = (dyScreen >= 0.0f) ? 'v' : '^';
                        dirResolved = true;
                    }
                }

                if (!dirResolved) {
                    const float radToDeg = 57.29577951308232f;
                    double toX = cp.ex - cpCamState.camX;
                    double toZ = cp.ez - cpCamState.camZ;
                    float targetYaw = (float)(std::atan2(-toX, toZ) * radToDeg);
                    float delta = targetYaw - cpCamState.yaw;
                    while (delta > 180.0f) delta -= 360.0f;
                    while (delta < -180.0f) delta += 360.0f;
                    float ad = std::fabs(delta);
                    if (ad <= 32.0f) dirArrow = '^';
                    else if (ad >= 148.0f) dirArrow = 'v';
                    else dirArrow = (delta < 0.0f) ? '<' : '>';
                }
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
            bool renderNametags = TRACE261_IF("renderNametags", (!g_realGuiOpen && cfg.nametags && sharedCamFound));
            if (renderNametags) {
                drawnTags = 0;
                const int nametagRenderCap = (std::max)(1, (std::min)(20, cfg.nametagMaxCount));

                if (!playerSnap.empty()) {
                    const LegoVec3  cam      = sharedCam;
                    const float     yaw      = sharedYaw;
                    const float     pitch    = sharedPitch;
                    const Matrix4x4 projMat  = sharedProj;
                    const Matrix4x4 viewMat  = sharedView;
                    const bool      matsOk   = sharedMatsOk;
                    const float fov  = cpCamState.fov;
                    const int   winW = (int)io.DisplaySize.x;
                    const int   winH = (int)io.DisplaySize.y;

                    for (const auto& it : playerSnap) {
                        if (drawnTags >= nametagRenderCap) break;
                        if (LooksLikeFakePlayerLine(it.name)) continue;

                        // Centre of player's head
                        const LegoVec3 headPos = { it.ex, it.ey + 1.9, it.ez };

                        float sx = 0, sy = 0;

                        bool projected = false;
                        bool useMatrices = TRACE261_IF("nametagUseMatrices", matsOk);
                        if (useMatrices) {
                            projected = WorldToScreen(headPos, cam, viewMat, projMat, winW, winH, &sx, &sy);
                        } else {
                            projected = WorldToScreen_Angles(headPos, cam, yaw, pitch, fov, winW, winH, &sx, &sy);
                        }

                        if (!projected) continue;

                        // Overlay-only smoothing for visual nametags.
                        // Keep telemetry JSON coordinates raw (server loop path) so Aim Assist behavior is unchanged.
                        std::string nametagKey = it.name;
                        if (nametagKey.empty()) {
                            char fallbackKey[96];
                            snprintf(fallbackKey, sizeof(fallbackKey), "p_%.1f_%.1f_%.1f", it.ex, it.ey, it.ez);
                            nametagKey = fallbackKey;
                        }
                        bool tagSmoothFound = false;
                        for (auto& sm : s_nametagSmooth) {
                            if (sm.key == nametagKey) {
                                sm.sx += (sx - sm.sx) * overlaySmoothAlpha;
                                sm.sy += (sy - sm.sy) * overlaySmoothAlpha;
                                sm.lastSeenMs = overlayNowMs;
                                sx = sm.sx;
                                sy = sm.sy;
                                tagSmoothFound = true;
                                break;
                            }
                        }
                        if (!tagSmoothFound) {
                            s_nametagSmooth.push_back({ nametagKey, sx, sy, overlayNowMs });
                        }

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
            bool renderChestEsp = TRACE261_IF("renderChestEsp", (!g_realGuiOpen && cfg.chestEsp && sharedCamFound));
            if (renderChestEsp) {
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
                        : WorldToScreen_Angles(center, espCam, espYaw, espPitch, cpCamState.fov, (int)io.DisplaySize.x, (int)io.DisplaySize.y, &csx, &csy);
                    Log(std::string("ChestESP diag: matsOk=") + (espMatsOk?"1":"0")
                        + " yaw=" + std::to_string(espYaw) + " pitch=" + std::to_string(espPitch)
                        + " cam=(" + std::to_string(espCam.x) + "," + std::to_string(espCam.y) + "," + std::to_string(espCam.z) + ")"
                        + " chest=(" + std::to_string(ch0.x) + "," + std::to_string(ch0.y) + "," + std::to_string(ch0.z) + ")"
                        + " projected=" + (ok?"1":"0") + " sx=" + std::to_string(csx) + " sy=" + std::to_string(csy));
                }
                {
                    const int winW = (int)io.DisplaySize.x;
                    const int winH = (int)io.DisplaySize.y;
                    const float fov = cpCamState.fov;
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
                            bool useMatrices = TRACE261_IF("chestEspUseMatrices", espMatsOk);
                            bool ok = useMatrices
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

            bool renderGtbHelper = TRACE261_IF("renderGtbHelper", cfg.gtbHelper);
            if (renderGtbHelper) {
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

                const float panelW = (std::min)(360.0f, io.DisplaySize.x - 20.0f);
                const float lineH = ImGui::GetFontSize() + 2.0f;
                const float panelH = 18.0f + lineH + (lines.empty() ? 0.0f : (8.0f + lineH * (float)lines.size())) + 10.0f;
                const float x1 = io.DisplaySize.x - 10.0f;
                const float y1 = io.DisplaySize.y - 10.0f;
                const float x0 = (std::max)(10.0f, x1 - panelW);
                const float y0 = (std::max)(10.0f, y1 - panelH);

                fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), overlayTheme.moduleBg, 5.0f);
                fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), overlayTheme.gtbBorder, 5.0f, 0, 1.2f);

                char hintBuf[320];
                snprintf(hintBuf, sizeof(hintBuf), "GTB: %s (%d)", hint.c_str(), (std::max)(0, cfg.gtbCount));
                fg->AddText(ImVec2(x0 + 9.0f, y0 + 8.0f), overlayTheme.gtbTitle, hintBuf);

                float y = y0 + 8.0f + lineH + 6.0f;
                for (const auto& line : lines) {
                    std::string row = "- " + line;
                    fg->AddText(ImVec2(x0 + 10.0f, y), overlayTheme.gtbRow, row.c_str());
                    y += lineH;
                }
            }

            bool renderModuleList = TRACE261_IF("renderModuleList", cfg.showModuleList);
            if (renderModuleList) {
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
                    pushMod(acBuf, overlayTheme.accentPrimary);
                }
                if (cfg.clickInChests) pushMod("Click in Chests", overlayTheme.accentTertiary);
                if (cfg.closestPlayer) pushMod("Closest Player", overlayTheme.accentSecondary);
                if (cfg.rightClick)    pushMod("Rightclick", overlayTheme.accentTertiary);
                if (cfg.aimAssist)     pushMod("Aim Assist", overlayTheme.accentPrimary);
                if (cfg.triggerbot)    pushMod("Triggerbot", overlayTheme.accentSecondary);
                if (cfg.chestEsp)      pushMod("Chest ESP", overlayTheme.accentSecondary);
                if (cfg.nametags)      pushMod("Nametags", overlayTheme.accentPrimary);
                if (cfg.gtbHelper)     pushMod("GTB Helper", overlayTheme.accentTertiary);
                if (cfg.jitter)        pushMod("Jitter", overlayTheme.accentSecondary);
                if (cfg.breakBlocks)   pushMod("Break Blocks", overlayTheme.moduleText);
                if (cfg.reachEnabled)  pushMod("Reach", overlayTheme.accentPrimary);
                if (cfg.velocityEnabled) pushMod("Velocity", overlayTheme.accentTertiary);

                // Sort by width descending (staggered original look)
                for (int a = 0; a < modCount; a++) {
                    for (int b = a + 1; b < modCount; b++) {
                        if (mods[b].width > mods[a].width) {
                            ModLine tmp = mods[a]; mods[a] = mods[b]; mods[b] = tmp;
                        }
                    }
                }

                const float marginX = 10.0f;
                float y = 10.0f;
                
                if (cfg.showLogo) {
                    const char* logoText = "LegoClicker";
                    ImVec2 logoSz = ImGui::CalcTextSize(logoText);
                    float logoX = io.DisplaySize.x - marginX - logoSz.x;
                    // Logo Shadow
                    fg->AddText(ImVec2(logoX + 1, y + 1), overlayTheme.logoShadow, logoText);
                    fg->AddText(ImVec2(logoX, y), overlayTheme.logoColor, logoText);
                    y += logoSz.y + 8.0f;
                }

                const float padX = 8.0f;
                const float padY = 3.0f;
                const float barW = 3.0f;
                const float gapY = 2.0f;
                const float fontH = ImGui::GetFontSize();
                const int style = (std::max)(0, (std::min)(4, cfg.moduleListStyle));
                
                for (int i = 0; i < modCount; i++) {
                    const ModLine& m = mods[i];
                    ImVec2 textSz = ImGui::CalcTextSize(m.text);
                    float boxW = barW + padX + textSz.x + padX;
                    float boxH = padY + fontH + padY;
                    float x0 = io.DisplaySize.x - marginX - boxW;
                    float x1 = io.DisplaySize.x - marginX;
                    float y0 = y;
                    float y1 = y + boxH;

                    if (style == 0) {
                        fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), overlayTheme.moduleBg);
                        fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + barW, y1), m.accent);
                        fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), overlayTheme.moduleBorder);
                        ImVec2 tx = ImVec2(x0 + barW + padX, y0 + padY);
                        fg->AddText(ImVec2(tx.x + 1, tx.y + 1), overlayTheme.moduleTextShadow, m.text);
                        fg->AddText(tx, overlayTheme.moduleText, m.text);
                    } else if (style == 1) {
                        fg->AddRectFilled(ImVec2(x1 - textSz.x - 4, y0), ImVec2(x1, y1), overlayTheme.moduleMinimalBg);
                        fg->AddRectFilled(ImVec2(x1 - 2, y0), ImVec2(x1, y1), m.accent);
                        ImVec2 tx = ImVec2(x1 - textSz.x - 2, y0 + padY);
                        fg->AddText(ImVec2(tx.x + 1, tx.y + 1), overlayTheme.moduleTextShadow, m.text);
                        fg->AddText(tx, m.accent, m.text);
                    } else if (style == 2) {
                        fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), overlayTheme.moduleOutlinedBg);
                        fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), m.accent, 4.0f, 0, 1.5f);
                        ImVec2 tx = ImVec2(x0 + barW + padX, y0 + padY);
                        fg->AddText(ImVec2(tx.x + 1, tx.y + 1), overlayTheme.moduleTextShadow, m.text);
                        fg->AddText(tx, m.accent, m.text);
                    } else if (style == 3) {
                        fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), overlayTheme.moduleMinimalBg, 4.0f);
                        fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), overlayTheme.moduleGlassBorder, 4.0f, 0, 1.0f);
                        fg->AddRectFilled(ImVec2(x0 + 1.0f, y0 + 1.0f), ImVec2(x0 + barW + 1.0f, y1 - 1.0f), m.accent);
                        ImVec2 tx = ImVec2(x0 + barW + padX, y0 + padY);
                        fg->AddText(ImVec2(tx.x + 1, tx.y + 1), overlayTheme.moduleTextShadow, m.text);
                        fg->AddText(tx, overlayTheme.moduleText, m.text);
                    } else {
                        fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), m.accent, 4.0f);
                        fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), overlayTheme.moduleBorder, 4.0f, 0, 1.0f);
                        ImVec2 tx = ImVec2(x0 + barW + padX, y0 + padY);
                        fg->AddText(ImVec2(tx.x + 1, tx.y + 1), overlayTheme.moduleTextShadow, m.text);
                        fg->AddText(tx, overlayTheme.moduleBoldText, m.text);
                    }

                    y += boxH + gapY;
                }
            }
        } else {
            TRACE261_PATH("overlay-render-skipped");
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
    TRACE261_PATH("enter");
    Log("=== bridge_261.dll loaded ===");

    // Wait for JVM
    HMODULE hJvm = nullptr;
    for (int i = 0; i < 60 && !hJvm; i++) {
        hJvm = GetModuleHandleA("jvm.dll");
        if (!hJvm) Sleep(500);
    }
    TRACE261_BRANCH("jvmModuleFound", hJvm != nullptr);
    if (!hJvm) { Log("ERROR: jvm.dll not found."); return 0; }

    typedef jint (JNICALL* FnGetVMs)(JavaVM**, jsize, jsize*);
    FnGetVMs fn = (FnGetVMs)GetProcAddress(hJvm, "JNI_GetCreatedJavaVMs");
    TRACE261_BRANCH("jniGetCreatedVmsFound", fn != nullptr);
    if (!fn) { Log("ERROR: JNI_GetCreatedJavaVMs not found."); return 0; }

    JavaVM* jvm = nullptr; jsize cnt = 0;
    for (int i = 0; i < 10; i++) {
        fn(&jvm, 1, &cnt);
        if (jvm && cnt > 0) break;
        Sleep(1000);
    }
    TRACE261_BRANCH("jvmInstanceFound", (jvm && cnt > 0));
    if (!jvm || cnt == 0) { Log("ERROR: No JVM."); return 0; }
    g_jvm = jvm;
    Log("JVM found.");

    // Install MinHook
    if (MH_Initialize() != MH_OK) { Log("ERROR: MinHook init failed."); return 0; }

    // Hook wglSwapBuffers (for ImGui rendering)
    HMODULE hOgl = GetModuleHandleA("opengl32.dll");
    void* pSwap  = (void*)GetProcAddress(hOgl, "wglSwapBuffers");
    TRACE261_BRANCH("swapBuffersSymbolFound", pSwap != nullptr);
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
                {
                    LockGuard remapGuard(g_jniRemapMtx);
                    if (DiscoverJniMappings(denv)) break;
                }
                Log("Discovery attempt " + std::to_string(attempt+1) + " failed, retrying in 3s...");
                Sleep(3000);
            }
        }
        if (attached) g_jvm->DetachCurrentThread();
    }

    // TCP Server
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    g_serverSocket = srv;
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr = {}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(25590);
    bind(srv, (sockaddr*)&addr, sizeof(addr));
    listen(srv, 1);
    Log("TCP server listening on port 25590.");

    setlocale(LC_NUMERIC, "C");

    while (g_running) {
        SOCKET cli = accept(srv, nullptr, nullptr);
        TRACE261_BRANCH("clientAccepted", cli != INVALID_SOCKET);
        if (cli == INVALID_SOCKET) { Sleep(100); continue; }
        g_clientSocket = cli;
        Log("C# Loader connected.");
        u_long nb = 1; ioctlsocket(cli, FIONBIO, &nb);

        std::string readBuf;
        bool capabilitiesSent = false;
        while (g_running) {
            TRACE261_PATH("client-loop-iteration");
            if (!capabilitiesSent) {
                capabilitiesSent = TrySendCapabilities(cli);
                TRACE261_BRANCH("capabilitiesSent", capabilitiesSent);
                if (capabilitiesSent) Log("Sent bridge capabilities packet");
            }

            // Send state
            {
                std::string sn;
                std::string actionBar;
                bool jniGui;
                bool lookBlock;
                bool lookEntity;
                bool lookEntityLatched;
                bool breakBlock;
                bool holdBlock;
                float attackCooldown;
                float attackCooldownPerTick;
                unsigned long long stateMs;
                { LockGuard lk(g_jniStateMtx);
                    sn = g_jniScreenName;
                    actionBar = g_jniActionBar;
                    jniGui = g_jniGuiOpen;
                    lookBlock = g_jniLookingAtBlock;
                    lookEntity = g_jniLookingAtEntity;
                    lookEntityLatched = g_jniLookingAtEntityLatched;
                    breakBlock = g_jniBreakingBlock;
                    holdBlock = g_jniHoldingBlock;
                    attackCooldown = g_jniAttackCooldown;
                    attackCooldownPerTick = g_jniAttackCooldownPerTick;
                    stateMs = g_jniStateMs;
                }

                // guiOpen = our menu OR JNI reports a Minecraft screen is open
                // (but ChatScreen opened by US for cursor unlock is not a "real" gui)
                bool anyGui = g_ShowMenu || (jniGui && sn != "ChatScreen");
                TRACE261_BRANCH("stateAnyGui", anyGui);

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
                char fovBuf[32];
                snprintf(fovBuf, sizeof(fovBuf), "%.2f", camState.fov);
                state += ",\"fov\":";
                state += fovBuf;
                state += ",\"holdingBlock\":";
                state += holdBlock ? "true" : "false";
                state += ",\"lookingAtBlock\":";
                state += lookBlock ? "true" : "false";
                state += ",\"lookingAtEntity\":";
                state += lookEntity ? "true" : "false";
                state += ",\"lookingAtEntityLatched\":";
                state += lookEntityLatched ? "true" : "false";
                state += ",\"breakingBlock\":";
                state += breakBlock ? "true" : "false";
                char stateMsBuf[32];
                snprintf(stateMsBuf, sizeof(stateMsBuf), "%llu", stateMs);
                state += ",\"stateMs\":";
                state += stateMsBuf;
                char attackCooldownBuf[32];
                snprintf(attackCooldownBuf, sizeof(attackCooldownBuf), "%.3f", attackCooldown);
                state += ",\"attackCooldown\":";
                state += attackCooldownBuf;
                char attackCooldownPerTickBuf[32];
                snprintf(attackCooldownPerTickBuf, sizeof(attackCooldownPerTickBuf), "%.3f", attackCooldownPerTick);
                state += ",\"attackCooldownPerTick\":";
                state += attackCooldownPerTickBuf;
                state += ",\"entities\":[";

                bool first = true;
                LegoVec3 camPos = { camState.camX, camState.camY, camState.camZ };
                int sentEntities = 0;
                for (const auto& p : players) {
                    if (sentEntities >= 32) break; // Limit JSON size
                    float sx = -1.0f, sy = -1.0f;
                    bool projected = false;

                    bool canProject = TRACE261_IF("entityProjectionCamFound", camState.camFound);
                    if (canProject) {
                        auto projectPoint = [&](const LegoVec3& pos, float* outX, float* outY) -> bool {
                            bool useMatrices = TRACE261_IF("entityProjectionUseMatrices", camState.matsOk);
                            return useMatrices
                                ? WorldToScreen(pos, camPos, camState.view, camState.proj, winW, winH, outX, outY)
                                : WorldToScreen_Angles(pos, camPos, camState.yaw, camState.pitch, camState.fov, winW, winH, outX, outY);
                        };

                        // Aim-assist target point: closest projected point on player's body box.
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

                        TRACE261_BRANCH("entityProjectionBodySampleHit", bestFound);
                        if (bestFound) {
                            sx = bestSx;
                            sy = bestSy;
                            projected = true;
                        } else {
                            TRACE261_PATH("entityProjectionHeadFallback");
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
                    sentEntities++;
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

            Sleep(5);
        }
        closesocket(cli);
        g_clientSocket = INVALID_SOCKET;
        Log("C# Loader disconnected.");
    }

    closesocket(srv);
    g_serverSocket = INVALID_SOCKET;
    WSACleanup();
    return 0;
}

// ===================== DLL ENTRY =====================
extern "C" __declspec(dllexport) void Dummy261() {}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule121 = hModule;
        DisableThreadLibraryCalls(hModule);
        
        char path[MAX_PATH];
        if (GetModuleFileNameA(hModule, path, MAX_PATH)) {
            std::string fullPath(path);
            size_t  pos = fullPath.find_last_of("\\/");
            if (pos != std::string::npos) {
                g_logPath = fullPath.substr(0, pos) + "\\bridge_261_debug.log";
            }
        }

        char traceEnv[16] = {};
        DWORD traceLen = GetEnvironmentVariableA("LC_BRIDGE_TRACE", traceEnv, sizeof(traceEnv));
        if (traceLen > 0) {
            g_trace261Enabled = (traceEnv[0] == '1'
                || traceEnv[0] == 'y' || traceEnv[0] == 'Y'
                || traceEnv[0] == 't' || traceEnv[0] == 'T');
        }
        
        std::ofstream(g_logPath, std::ios_base::trunc)
            << "=== bridge_261.dll DLL_PROCESS_ATTACH ===\n";
        g_mainThreadHandle = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
    }
    return TRUE;
}
