#include <winsock2.h>
#include <windows.h>
#include <jni.h>
#include <jvmti.h>
#include <GL/gl.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <unordered_map>
// Custom Mutex for MinGW win32 threads
class Mutex {
    CRITICAL_SECTION cs;
public:
    Mutex() { InitializeCriticalSection(&cs); }
    ~Mutex() { DeleteCriticalSection(&cs); }
    void lock() { EnterCriticalSection(&cs); }
    void unlock() { LeaveCriticalSection(&cs); }
};

class LockGuard {
    Mutex& m;
public:
    LockGuard(Mutex& m) : m(m) { m.lock(); }
    ~LockGuard() { m.unlock(); }
};

// ===================== GLOBALS =====================
JavaVM* g_jvm = nullptr;
bool g_running = true;
static Mutex g_jniMutex; // prevents concurrent JNI between Client thread (SwapBuffers) and LegoBridge thread
SOCKET g_serverSocket = INVALID_SOCKET;
SOCKET g_clientSocket = INVALID_SOCKET;

// Rendering
static GLuint g_fontTexture = 0;
static bool g_glInitialized = false;
static bool g_guiOpen = false;
static WNDPROC g_origWndProc = nullptr;
static HWND g_gameHwnd = nullptr;
static HWND g_wndProcHookedHwnd = nullptr;
static int g_mouseX = 0, g_mouseY = 0;

struct Config {
    bool leftClick = true, rightClick = false;
    bool jitter = true;
    bool rightBlockOnly = false;
    bool breakBlocks = false;
    bool clickInChests = false;
    bool aimAssist = false;
    bool triggerbot = false;
    bool gtbHelper = false;
    bool nametags = false;
    bool closestPlayerInfo = false;
    bool nametagShowHealth = true;
    bool nametagShowArmor = true;
    int nametagMaxCount = 8;
    bool chestEsp = false;
    int chestEspMaxCount = 5;
    std::string gtbHint;
    int gtbCount = 0;
    std::string gtbPreview;
    bool reachEnabled = false;
    float reachMin = 3.0f;
    float reachMax = 3.0f;
    int reachChance = 100;
    bool velocityEnabled = false;
    int velocityHorizontal = 100;
    int velocityVertical = 100;
    int velocityChance = 100;
    bool showModuleList = true;
    int moduleListStyle = 0;
    bool showLogo = true;
    std::string guiTheme = "Default";
    float minCPS = 10, maxCPS = 14;
    float rightMinCPS = 10, rightMaxCPS = 14;
    bool armed = false; // "Self Destruct" / Enable toggle
    bool clicking = false; // Internal state
    // Per-module keybinds (VK codes; 0 = unbound)
    int keybindAutoclicker = 0xC0; // backtick default
    int keybindNametags = 0;
    int keybindClosestPlayer = 0;
    int keybindChestEsp = 0;
};
static Config g_config;
static Mutex g_configMutex;

// Game State
struct GameState {
    bool mapped = false;
    bool guiOpen = false;
    std::string screenName;
    std::string actionBar;
    float health = 20.0f;
    double posX = 0, posY = 0, posZ = 0;
    bool holdingBlock = false;
    bool lookingAtBlock = false;
    bool lookingAtEntity = false;
    bool lookingAtEntityLatched = false;
    bool breakingBlock = false;
    float attackCooldown = 1.0f;
    float attackCooldownPerTick = 0.08f;
    unsigned long long stateMs = 0;
};
static GameState g_gameState;
static Mutex g_stateMutex;
static Mutex g_socketMutex; // Protects socket writes
static Mutex g_jsonMutex;   // Protects shared JSON buffer
static std::string g_pendingJson; // Data from Render Thread
static bool g_mouseClicked = false;
static bool g_mouseDown = false;
static bool g_mouseRightClicked = false;
static bool g_mouseRightDown = false;
static int g_scrollDelta = 0;
static float g_guiScrollY = 0;
static bool g_nativeChatOpenedByClickGui = false;
static unsigned long long g_lastEntitySeenMs = 0;

// 1.8.9 GTB/action-bar extraction mappings
enum ActionBarFieldKind {
    ActionBarFieldString = 0,
    ActionBarFieldChatComponent = 1
};

struct ActionBarFieldRef {
    jfieldID field = nullptr;
    ActionBarFieldKind kind = ActionBarFieldString;
};

static jfieldID g_ingameGuiField = nullptr;
static jmethodID g_chatComponentGetTextMethod = nullptr;
static std::vector<ActionBarFieldRef> g_actionBarFields;
static DWORD g_lastActionBarProbeMs = 0;
static int g_lastActionBarFieldCountLogged = -1;

struct UiState {
    float accentHue = 0.46f;      // muted teal default
    float accentSat = 0.55f;
    float accentVal = 0.78f;
    bool chromaText = true;
    float chromaSpeed = 0.06f;
};
static UiState g_uiState;

// Font
// Font (High Resolution)
#define CHAR_W 16
#define CHAR_H 32
#define ATLAS_COLS 16
#define ATLAS_ROWS 8
#define ATLAS_W (ATLAS_COLS * CHAR_W) // 256
#define ATLAS_H (ATLAS_ROWS * CHAR_H) // 256

// Hook
typedef BOOL(WINAPI* SwapBuffersFn)(HDC);
static SwapBuffersFn g_origSwapBuffers = nullptr;


// JNI cached refs
static jclass g_mcClass = nullptr;
static jobject g_mcInstance = nullptr;
static jobject g_gameClassLoader = nullptr;
static jfieldID g_thePlayerField = nullptr;
static jfieldID g_currentScreenField = nullptr;
static jmethodID g_getHealthMethod = nullptr;
static jfieldID g_posXField = nullptr, g_posYField = nullptr, g_posZField = nullptr;
static bool g_mapped = false;

// Native GUI handling
static jclass g_guiChatClass = nullptr;
static jmethodID g_guiChatConstructor = nullptr;
static jmethodID g_displayGuiScreenMethod = nullptr;

// Nametags / ESP globals
static jfieldID g_theWorldField = nullptr;
static jfieldID g_playerEntitiesField = nullptr;
static jfieldID g_loadedTileEntityListField = nullptr;
static jmethodID g_getNameMethod = nullptr;
static jmethodID g_getGameProfileMethod = nullptr;
static jclass g_gameProfileClass = nullptr;
static jmethodID g_gameProfileGetNameMethod = nullptr;
static jmethodID g_objectHashCodeMethod = nullptr;
static jfieldID g_rotationYawField = nullptr;
static jfieldID g_rotationPitchField = nullptr;
static jmethodID g_listSizeMethod = nullptr;
static jmethodID g_listGetMethod = nullptr;
static jclass g_listClass = nullptr; // java/util/List
static jfieldID g_gameSettingsField = nullptr;
static jfieldID g_fovSettingField = nullptr;
static jfieldID g_tileEntityPosField = nullptr;
static jmethodID g_blockPosGetX = nullptr;
static jmethodID g_blockPosGetY = nullptr;
static jmethodID g_blockPosGetZ = nullptr;
static jclass g_tileEntityChestClass = nullptr;
static jclass g_tileEntityEnderChestClass = nullptr;

// Interpolation globals
static jfieldID g_timerField = nullptr;
static jfieldID g_renderPartialTicksField = nullptr;
static jfieldID g_lastTickPosXField = nullptr;
static jfieldID g_lastTickPosYField = nullptr;
static jfieldID g_lastTickPosZField = nullptr;

// Inventory / Held Item Globals
static jfieldID  g_inventoryField          = nullptr; // EntityPlayer.inventory
static jmethodID g_getCurrentItemMethod    = nullptr; // InventoryPlayer.getCurrentItem()
static jclass    g_itemBlockClass          = nullptr; // ItemBlock class (instanceof check)
static jmethodID g_getHeldItemMethod       = nullptr; // EntityLivingBase.getHeldItem()
static jmethodID g_getTotalArmorValueMethod= nullptr; // EntityPlayer.getTotalArmorValue()
static jclass    g_itemSwordClass          = nullptr; // ItemSword class (instanceof check)
static jmethodID g_getRenderItemFromMcMethod          = nullptr;
static jmethodID g_renderItemAndEffectIntoGUIMethod   = nullptr;
static jmethodID g_renderItemIntoGUIMethod            = nullptr;

static jfieldID g_objectMouseOverField = nullptr;
static jfieldID g_pointedEntityField = nullptr;
static jclass g_movingObjectPositionClass = nullptr;
static jfieldID g_typeOfHitField = nullptr;
static jmethodID g_enumNameMethod = nullptr;
static jfieldID g_entityHitField = nullptr;
static jclass g_movingObjectTypeClass = nullptr;
static jobject g_mopEntityTypeConst = nullptr;
static jmethodID g_mopEntityCtor = nullptr;
static bool g_mopCtorNeedsVec3 = false;
static jclass g_vec3Class = nullptr;
static jmethodID g_vec3Ctor = nullptr;
static jfieldID g_motionXField = nullptr;
static jfieldID g_motionYField = nullptr;
static jfieldID g_motionZField = nullptr;
static jfieldID g_hurtTimeField = nullptr;
static int g_lastHurtTime = 0;
static bool g_reachClickPrevDown = false;
static bool g_reachClickPrevSynthetic = false;
static bool g_reachRawInputPrevDown = false;
static bool g_reachAllowCurrentClick = false;
static double g_reachCurrentClickRange = 3.0;
static DWORD g_reachClickWindowUntilMs = 0;
static DWORD g_reachLastRollMs = 0;
static DWORD g_lastReachDebugLogMs = 0;
static jobject g_reachCurrentTarget = nullptr;
// NOTE: g_getItemMethod, g_getDisplayNameMethod, g_getUnlocalizedNameMethod,
// g_getDamageVsEntityMethod are intentionally NOT cached globally — they must
// be fetched from the actual object's class each call to avoid calling a
// method ID from the wrong class (causes JVM crash when entity classes differ).

// OpenGL Matrix Globals
static jclass g_activeRenderInfoClass = nullptr;
static jfieldID g_modelViewField = nullptr;
static jfieldID g_projectionField = nullptr;
static jmethodID g_floatBufferGet = nullptr; // FloatBuffer.get(I)F

struct Matrix4x4 {
    float m[16];
};

// RenderManager Globals
static jfieldID g_renderManagerField = nullptr; // Minecraft.renderManager
static jfieldID g_viewerPosXField = nullptr;
static jfieldID g_viewerPosYField = nullptr;
static jfieldID g_viewerPosZField = nullptr;

struct TagSmoothingState {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    int lastFrame = 0;
    bool init = false;
};
static std::unordered_map<int, TagSmoothingState> g_tagSmoothing;
static int g_tagFrameCounter = 0;

// Commands to send to C#
static std::vector<std::string> g_pendingCommands;
static Mutex g_cmdMutex;

static std::string g_logPath = "bridge_debug.log";

// Forward declarations for player-name filtering helpers used by reach/telemetry paths.
static bool LooksLikeFakePlayerLine(const std::string& rawName);
static std::string GetStablePlayerName(JNIEnv* env, jobject playerObj);

// ===================== LOGGING =====================
void InitLogPath(HMODULE hModule) {
    char dllPath[MAX_PATH] = {};
    if (!GetModuleFileNameA(hModule, dllPath, MAX_PATH)) {
        return;
    }

    std::string path = dllPath;
    size_t sep = path.find_last_of("\\/");
    if (sep == std::string::npos) {
        return;
    }
    g_logPath = path.substr(0, sep + 1) + "bridge_debug.log";
}

void Log(const std::string& msg) {
    std::ofstream f(g_logPath.c_str(), std::ios_base::app);
    f << "[Bridge] " << msg << std::endl;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

// ===================== JNI HELPERS =====================
jobject GetGameClassLoader(JNIEnv* env) {
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
    for (int i = 0; i < count; i++) {
        jobject t = env->GetObjectArrayElement(threads, i);
        jstring jn = (jstring)env->CallObjectMethod(t, mName);
        const char* cn = env->GetStringUTFChars(jn, nullptr);
        bool isClient = strstr(cn, "Client thread") != nullptr;
        env->ReleaseStringUTFChars(jn, cn);
        if (isClient) return env->CallObjectMethod(t, mGetCL);
    }
    return nullptr;
}

jobject EnsureGameClassLoader(JNIEnv* env) {
    if (!env) return nullptr;
    if (g_gameClassLoader) return g_gameClassLoader;

    jobject local = GetGameClassLoader(env);
    if (!local) return nullptr;

    g_gameClassLoader = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    if (g_gameClassLoader) {
        Log("Cached game classloader");
    }
    return g_gameClassLoader;
}

std::string GetClassNameFromClass(JNIEnv* env, jclass cls) {
    if (!env || !cls) return "";
    jclass cClass = env->FindClass("java/lang/Class");
    if (!cClass) { if (env->ExceptionCheck()) env->ExceptionClear(); return ""; }
    jmethodID m = env->GetMethodID(cClass, "getName", "()Ljava/lang/String;");
    if (!m) { env->DeleteLocalRef(cClass); if (env->ExceptionCheck()) env->ExceptionClear(); return ""; }
    jstring jn = (jstring)env->CallObjectMethod(cls, m);
    env->DeleteLocalRef(cClass);
    if (!jn) { if (env->ExceptionCheck()) env->ExceptionClear(); return ""; }
    const char* cn = env->GetStringUTFChars(jn, nullptr);
    std::string r = cn ? cn : "";
    if (cn) env->ReleaseStringUTFChars(jn, cn);
    env->DeleteLocalRef(jn);
    return r;
}

jclass LoadClassWithLoader(JNIEnv* env, jobject cl, const char* name) {
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

// ===================== CLASS DISCOVERY =====================
bool DiscoverMappings(JNIEnv* env) {
    Log("Starting dynamic class discovery...");
    jvmtiEnv* jvmti = nullptr;
    if (g_jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_2) != JNI_OK || !jvmti) {
        Log("ERROR: Failed to get JVMTI"); return false;
    }
    jint classCount = 0; jclass* classes = nullptr;
    jvmti->GetLoadedClasses(&classCount, &classes);
    Log("Loaded classes: " + std::to_string(classCount));

    jobject gcl = EnsureGameClassLoader(env);
    if (!gcl) { Log("ERROR: No game classloader"); return false; }

    jclass cClass = env->FindClass("java/lang/Class");
    jmethodID mGetName = env->GetMethodID(cClass, "getName", "()Ljava/lang/String;");
    jmethodID mGetFields = env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jmethodID mGetMethods = env->GetMethodID(cClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jmethodID mGetSuper = env->GetMethodID(cClass, "getSuperclass", "()Ljava/lang/Class;");
    
    jclass cField = env->FindClass("java/lang/reflect/Field");
    jmethodID mFType = env->GetMethodID(cField, "getType", "()Ljava/lang/Class;");
    jmethodID mFName = env->GetMethodID(cField, "getName", "()Ljava/lang/String;");
    jmethodID mFMod = env->GetMethodID(cField, "getModifiers", "()I");
    
    jclass cMethod = env->FindClass("java/lang/reflect/Method");
    jmethodID mMName = env->GetMethodID(cMethod, "getName", "()Ljava/lang/String;");
    jmethodID mMRet = env->GetMethodID(cMethod, "getReturnType", "()Ljava/lang/Class;");
    jmethodID mMParams = env->GetMethodID(cMethod, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID mMMod = env->GetMethodID(cMethod, "getModifiers", "()I");
    
    jclass cMod = env->FindClass("java/lang/reflect/Modifier");
    jmethodID mIsStatic = env->GetStaticMethodID(cMod, "isStatic", "(I)Z");

    // Try known names first
    jclass mcClass = nullptr;
    std::string mcName;
    const char* known[] = {"net.minecraft.client.Minecraft", nullptr};
    for (int i = 0; known[i]; i++) {
        jclass c = LoadClassWithLoader(env, gcl, known[i]);
        if (c) { mcClass = c; mcName = known[i]; Log("Found MC by name: " + mcName); break; }
    }

    // Scan for singleton pattern
    if (!mcClass) {
        for (int i = 0; i < classCount; i++) {
            jclass cls = classes[i];
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            jstring jn = (jstring)env->CallObjectMethod(cls, mGetName);
            if (!jn || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            const char* cn = env->GetStringUTFChars(jn, nullptr);
            std::string name = cn; env->ReleaseStringUTFChars(jn, cn);
            if (name.find("java.") == 0 || name.find("sun.") == 0 || name.find("javax.") == 0 ||
                name.find("com.sun.") == 0 || name.find("org.") == 0 || name.find("jdk.") == 0 ||
                name.find("com.google.") == 0 || name.find("io.") == 0 || name[0] == '[') continue;

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
                mcClass = (jclass)env->NewGlobalRef(cls);
                mcName = name;
                Log("Found MC class: " + name + " fields=" + std::to_string(objCount));
                break;
            }
        }
    }
    if (!mcClass) { Log("ERROR: MC class not found"); jvmti->Deallocate((unsigned char*)classes); return false; }

    // Find singleton field
    jobjectArray mcFields = (jobjectArray)env->CallObjectMethod(mcClass, mGetFields);
    jsize mcFC = env->GetArrayLength(mcFields);
    jfieldID singletonField = nullptr;
    std::string playerType;

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
        std::string fn = cfn; env->ReleaseStringUTFChars(jfn, cfn);

        if (isS && tn == mcName) {
            std::string sig = "L" + mcName + ";"; std::replace(sig.begin(), sig.end(), '.', '/');
            singletonField = env->GetStaticFieldID(mcClass, fn.c_str(), sig.c_str());
            if (env->ExceptionCheck()) env->ExceptionClear();
            Log("Singleton: " + fn);
        }
    }
    if (!singletonField) { Log("ERROR: No singleton"); jvmti->Deallocate((unsigned char*)classes); return false; }

    jobject mcInst = env->GetStaticObjectField(mcClass, singletonField);
    if (!mcInst) { Log("ERROR: MC null"); jvmti->Deallocate((unsigned char*)classes); return false; }
    Log("Got MC instance");

    // Find player & screen fields
    for (int f = 0; f < mcFC; f++) {
        jobject fld = env->GetObjectArrayElement(mcFields, f);
        if (!fld) continue;
        jint mod = env->CallIntMethod(fld, mFMod);
        if (env->CallStaticBooleanMethod(cMod, mIsStatic, mod)) continue;
        jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
        if (!ft || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        std::string tn = GetClassNameFromClass(env, ft);
        jstring jfn = (jstring)env->CallObjectMethod(fld, mFName);
        const char* cfn = env->GetStringUTFChars(jfn, nullptr);
        std::string fn = cfn; env->ReleaseStringUTFChars(jfn, cfn);

        jclass walk = ft; int depth = 0;
        bool isPlayer = false, isScreen = false;
        while (walk && depth < 10) {
            jobjectArray methods = (jobjectArray)env->CallObjectMethod(walk, mGetMethods);
            if (methods && !env->ExceptionCheck()) {
                jsize mc2 = env->GetArrayLength(methods);
                for (int m2 = 0; m2 < mc2; m2++) {
                    jobject mth = env->GetObjectArrayElement(methods, m2);
                    if (!mth) continue;
                    jstring jmn = (jstring)env->CallObjectMethod(mth, mMName);
                    const char* cmn = env->GetStringUTFChars(jmn, nullptr);
                    jclass rt = (jclass)env->CallObjectMethod(mth, mMRet);
                    std::string rtn = rt ? GetClassNameFromClass(env, rt) : "";
                    if (std::string(cmn) == "getHealth" && rtn == "float") isPlayer = true;
                    if (std::string(cmn) == "drawScreen" || std::string(cmn) == "drawDefaultBackground") isScreen = true;
                    env->ReleaseStringUTFChars(jmn, cmn);
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            walk = (jclass)env->CallObjectMethod(walk, mGetSuper);
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            depth++;
        }
        if (isPlayer && !g_thePlayerField) {
            std::string sig = "L" + tn + ";"; std::replace(sig.begin(), sig.end(), '.', '/');
            g_thePlayerField = env->GetFieldID(mcClass, fn.c_str(), sig.c_str());
            if (env->ExceptionCheck()) env->ExceptionClear();
            playerType = tn;
            Log("Player field: " + fn + " type=" + tn);
        }
        if (isScreen && !g_currentScreenField) {
            std::string sig = "L" + tn + ";"; std::replace(sig.begin(), sig.end(), '.', '/');
            g_currentScreenField = env->GetFieldID(mcClass, fn.c_str(), sig.c_str());
            if (env->ExceptionCheck()) env->ExceptionClear();
            Log("Screen field: " + fn + " type=" + tn);
        }
    }

    // Find getHealth and position fields on player hierarchy
    if (g_thePlayerField && !playerType.empty()) {
        jclass pc = LoadClassWithLoader(env, gcl, playerType.c_str());
        if (!pc) { std::string sn = playerType; std::replace(sn.begin(), sn.end(), '.', '/');
            pc = env->FindClass(sn.c_str()); if (env->ExceptionCheck()) env->ExceptionClear(); }
        jclass wc = pc; int d = 0;
        while (wc && d < 10) {
            std::string wcn = GetClassNameFromClass(env, wc);
            if (!g_getHealthMethod) {
                jobjectArray ms = (jobjectArray)env->CallObjectMethod(wc, mGetMethods);
                if (ms && !env->ExceptionCheck()) {
                    jsize mc3 = env->GetArrayLength(ms);
                    for (int m3 = 0; m3 < mc3; m3++) {
                        jobject mt = env->GetObjectArrayElement(ms, m3);
                        if (!mt) continue;
                        jstring jmn = (jstring)env->CallObjectMethod(mt, mMName);
                        const char* cmn = env->GetStringUTFChars(jmn, nullptr);
                        jclass rt = (jclass)env->CallObjectMethod(mt, mMRet);
                        std::string rtn = rt ? GetClassNameFromClass(env, rt) : "";
                        jobjectArray ps = (jobjectArray)env->CallObjectMethod(mt, mMParams);
                        jsize pc2 = ps ? env->GetArrayLength(ps) : 0;
                        if (std::string(cmn) == "getHealth" && rtn == "float" && pc2 == 0) {
                            g_getHealthMethod = env->GetMethodID(wc, "getHealth", "()F");
                            if (env->ExceptionCheck()) env->ExceptionClear();
                            Log("getHealth() in " + wcn);
                        }
                        env->ReleaseStringUTFChars(jmn, cmn);
                    }
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            if (!g_posXField || !g_posYField || !g_posZField) {
                // Prefer canonical field names (including inherited Entity fields) over reflection-order fallbacks.
                if (!g_posXField) {
                    g_posXField = env->GetFieldID(wc, "posX", "D");
                    if (!g_posXField) {
                        env->ExceptionClear();
                        g_posXField = env->GetFieldID(wc, "field_70165_t", "D");
                    }
                    if (!g_posXField) env->ExceptionClear();
                    else Log("posX: resolved by direct lookup");
                }
                if (!g_posYField) {
                    g_posYField = env->GetFieldID(wc, "posY", "D");
                    if (!g_posYField) {
                        env->ExceptionClear();
                        g_posYField = env->GetFieldID(wc, "field_70163_u", "D");
                    }
                    if (!g_posYField) env->ExceptionClear();
                    else Log("posY: resolved by direct lookup");
                }
                if (!g_posZField) {
                    g_posZField = env->GetFieldID(wc, "posZ", "D");
                    if (!g_posZField) {
                        env->ExceptionClear();
                        g_posZField = env->GetFieldID(wc, "field_70161_v", "D");
                    }
                    if (!g_posZField) env->ExceptionClear();
                    else Log("posZ: resolved by direct lookup");
                }

                jobjectArray fs = (jobjectArray)env->CallObjectMethod(wc, mGetFields);
                if (fs && !env->ExceptionCheck()) {
                    jsize fc2 = env->GetArrayLength(fs);
                    for (int f2 = 0; f2 < fc2; f2++) {
                        jobject fl = env->GetObjectArrayElement(fs, f2);
                        if (!fl) continue;
                        jclass ftt = (jclass)env->CallObjectMethod(fl, mFType);
                        if (!ftt || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                        jint mod = env->CallIntMethod(fl, mFMod);
                        if (env->CallStaticBooleanMethod(cMod, mIsStatic, mod)) continue;
                        if (GetClassNameFromClass(env, ftt) == "double") {
                            jstring jfn = (jstring)env->CallObjectMethod(fl, mFName);
                            const char* cfn = env->GetStringUTFChars(jfn, nullptr);
                            std::string n = cfn ? cfn : "";
                            env->ReleaseStringUTFChars(jfn, cfn);

                            if (!g_posXField && (n == "posX" || n == "field_70165_t")) {
                                g_posXField = env->GetFieldID(wc, n.c_str(), "D");
                                if (env->ExceptionCheck()) { env->ExceptionClear(); g_posXField = nullptr; }
                                else Log("posX: " + n);
                            }
                            if (!g_posYField && (n == "posY" || n == "field_70163_u")) {
                                g_posYField = env->GetFieldID(wc, n.c_str(), "D");
                                if (env->ExceptionCheck()) { env->ExceptionClear(); g_posYField = nullptr; }
                                else Log("posY: " + n);
                            }
                            if (!g_posZField && (n == "posZ" || n == "field_70161_v")) {
                                g_posZField = env->GetFieldID(wc, n.c_str(), "D");
                                if (env->ExceptionCheck()) { env->ExceptionClear(); g_posZField = nullptr; }
                                else Log("posZ: " + n);
                            }
                        }
                        env->DeleteLocalRef(ftt);
                        env->DeleteLocalRef(fl);
                    }
                    env->DeleteLocalRef(fs);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            wc = (jclass)env->CallObjectMethod(wc, mGetSuper);
            if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
            d++;
        }
    }

    // Find GuiChat class to use for native cursor release
    jclass chatClass = LoadClassWithLoader(env, gcl, "net.minecraft.client.gui.GuiChat");
    if (chatClass) {
        g_guiChatClass = (jclass)env->NewGlobalRef(chatClass);
        // GuiChat in 1.8.9 takes a default string
        g_guiChatConstructor = env->GetMethodID(chatClass, "<init>", "(Ljava/lang/String;)V");
        if (g_guiChatConstructor) {
             Log("Found GuiChat constructor");
        } else {
             Log("ERROR: Could not find GuiChat constructor (String)");
        }
    } else {
        Log("ERROR: Could not find GuiChat class");
    }

    // Find displayGuiScreen method in Minecraft
    if (mcClass) {
         // Scan methods for (Lnet/minecraft/client/gui/GuiScreen;)V
         // Name is usually displayGuiScreen or func_147108_a
         jobjectArray methods = (jobjectArray)env->CallObjectMethod(mcClass, mGetMethods);
         jsize mc2 = env->GetArrayLength(methods);
         for (int m2 = 0; m2 < mc2; m2++) {
             jobject mth = env->GetObjectArrayElement(methods, m2);
             if (!mth) continue;
             jstring jmn = (jstring)env->CallObjectMethod(mth, mMName);
             const char* cmn = env->GetStringUTFChars(jmn, nullptr);
             jstring jsig = (jstring)env->CallObjectMethod(mth, env->GetMethodID(cMethod, "toString", "()Ljava/lang/String;")); // Or scan params
             
             // Simpler: Just get it by name if possible, or mapping
             // Let's rely on standard names first
             env->ReleaseStringUTFChars(jmn, cmn);
         }
         
         // Try standard names
         g_displayGuiScreenMethod = env->GetMethodID(mcClass, "displayGuiScreen", "(Lnet/minecraft/client/gui/GuiScreen;)V");
         if (!g_displayGuiScreenMethod) {
             g_displayGuiScreenMethod = env->GetMethodID(mcClass, "func_147108_a", "(Lnet/minecraft/client/gui/GuiScreen;)V");
         }
         if (!g_displayGuiScreenMethod) {
             // Fallback: look for method taking GuiScreen
             // ... for now assume one of the above works or logging error
             Log("WARNING: Could not find displayGuiScreen method");
         } else {
             Log("Found displayGuiScreen method");
         }
    }

    g_mcClass = (jclass)env->NewGlobalRef(mcClass);
    g_mcInstance = env->NewGlobalRef(mcInst);
    
    // Nametags Discovery
    if (g_mcClass) {
         // Find theWorld
         jobjectArray fs = (jobjectArray)env->CallObjectMethod(g_mcClass, mGetFields);
         // Simplified search for WorldClient field
         jint worldCount = 0;
         jsize fc = env->GetArrayLength(fs);
         for(int i=0; i<fc; i++) {
             jobject f = env->GetObjectArrayElement(fs, i);
             if(!f) continue;
             jclass ft = (jclass)env->CallObjectMethod(f, mFType);
             if (ft && GetClassNameFromClass(env, ft).find("WorldClient") != std::string::npos) {
                  g_theWorldField = env->FromReflectedField(f);
                  Log("Found theWorld field");
                  break;
             }
         }
         if (!g_theWorldField) {
              g_theWorldField = env->GetFieldID(mcClass, "theWorld", "Lnet/minecraft/client/multiplayer/WorldClient;");
              if (!g_theWorldField) g_theWorldField = env->GetFieldID(mcClass, "field_71441_e", "Lnet/minecraft/client/multiplayer/WorldClient;");
         }
    }

    if (g_theWorldField && g_mcInstance) {
        jobject world = env->GetObjectField(g_mcInstance, g_theWorldField);
        if (world) {
             jclass worldClass = env->GetObjectClass(world);
             g_playerEntitiesField = env->GetFieldID(worldClass, "playerEntities", "Ljava/util/List;");
             if(!g_playerEntitiesField) g_playerEntitiesField = env->GetFieldID(worldClass, "field_73010_i", "Ljava/util/List;");
             g_loadedTileEntityListField = env->GetFieldID(worldClass, "loadedTileEntityList", "Ljava/util/List;");
             if(!g_loadedTileEntityListField) g_loadedTileEntityListField = env->GetFieldID(worldClass, "field_147482_g", "Ljava/util/List;");
             
             if (g_playerEntitiesField) Log("Found playerEntities field");
             if (g_loadedTileEntityListField) Log("Found loadedTileEntityList field");
             env->DeleteLocalRef(worldClass);
             env->DeleteLocalRef(world);
        }
    }

    jclass tileEntityClass = LoadClassWithLoader(env, gcl, "net/minecraft/tileentity/TileEntity");
    if (!tileEntityClass) {
        tileEntityClass = env->FindClass("net/minecraft/tileentity/TileEntity");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (tileEntityClass) {
        g_tileEntityPosField = env->GetFieldID(tileEntityClass, "pos", "Lnet/minecraft/util/BlockPos;");
        if (!g_tileEntityPosField) {
            env->ExceptionClear();
            g_tileEntityPosField = env->GetFieldID(tileEntityClass, "field_174879_c", "Lnet/minecraft/util/BlockPos;");
        }
        if (!g_tileEntityPosField) env->ExceptionClear();
    }

    jclass blockPosClass = LoadClassWithLoader(env, gcl, "net/minecraft/util/BlockPos");
    if (!blockPosClass) {
        blockPosClass = env->FindClass("net/minecraft/util/BlockPos");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (blockPosClass) {
        g_blockPosGetX = env->GetMethodID(blockPosClass, "getX", "()I");
        if (!g_blockPosGetX) { env->ExceptionClear(); g_blockPosGetX = env->GetMethodID(blockPosClass, "func_177958_n", "()I"); }
        if (!g_blockPosGetX) env->ExceptionClear();

        g_blockPosGetY = env->GetMethodID(blockPosClass, "getY", "()I");
        if (!g_blockPosGetY) { env->ExceptionClear(); g_blockPosGetY = env->GetMethodID(blockPosClass, "func_177956_o", "()I"); }
        if (!g_blockPosGetY) env->ExceptionClear();

        g_blockPosGetZ = env->GetMethodID(blockPosClass, "getZ", "()I");
        if (!g_blockPosGetZ) { env->ExceptionClear(); g_blockPosGetZ = env->GetMethodID(blockPosClass, "func_177952_p", "()I"); }
        if (!g_blockPosGetZ) env->ExceptionClear();
    }

    jclass chestCls = LoadClassWithLoader(env, gcl, "net/minecraft/tileentity/TileEntityChest");
    if (!chestCls) chestCls = env->FindClass("net/minecraft/tileentity/TileEntityChest");
    if (chestCls && !env->ExceptionCheck()) g_tileEntityChestClass = (jclass)env->NewGlobalRef(chestCls);
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass enderChestCls = LoadClassWithLoader(env, gcl, "net/minecraft/tileentity/TileEntityEnderChest");
    if (!enderChestCls) enderChestCls = env->FindClass("net/minecraft/tileentity/TileEntityEnderChest");
    if (enderChestCls && !env->ExceptionCheck()) g_tileEntityEnderChestClass = (jclass)env->NewGlobalRef(enderChestCls);
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Game Settings & FOV
    g_gameSettingsField = env->GetFieldID(mcClass, "gameSettings", "Lnet/minecraft/client/settings/GameSettings;");
    if(!g_gameSettingsField) g_gameSettingsField = env->GetFieldID(mcClass, "field_71474_y", "Lnet/minecraft/client/settings/GameSettings;");

    if (g_gameSettingsField) {
        jobject gs = env->GetObjectField(g_mcInstance, g_gameSettingsField);
        if (gs) {
            jclass gsClass = env->GetObjectClass(gs);
            g_fovSettingField = env->GetFieldID(gsClass, "fovSetting", "F");
            if (!g_fovSettingField) g_fovSettingField = env->GetFieldID(gsClass, "field_74334_X", "F");
            if (g_fovSettingField) Log("Found fovSetting field");
            env->DeleteLocalRef(gsClass);
            env->DeleteLocalRef(gs);
        }
    }

    // objectMouseOver
    if (mcClass) {
        g_objectMouseOverField = env->GetFieldID(mcClass, "objectMouseOver", "Lnet/minecraft/util/MovingObjectPosition;");
        if (!g_objectMouseOverField) { env->ExceptionClear(); g_objectMouseOverField = env->GetFieldID(mcClass, "field_71476_x", "Lnet/minecraft/util/MovingObjectPosition;"); }
        if (!g_objectMouseOverField) env->ExceptionClear();
        else Log("Found objectMouseOver field");

        g_pointedEntityField = env->GetFieldID(mcClass, "pointedEntity", "Lnet/minecraft/entity/Entity;");
        if (!g_pointedEntityField) { env->ExceptionClear(); g_pointedEntityField = env->GetFieldID(mcClass, "field_147125_j", "Lnet/minecraft/entity/Entity;"); }
        if (!g_pointedEntityField) env->ExceptionClear();
        else Log("Found pointedEntity field");
    }

    jclass mopClass = LoadClassWithLoader(env, gcl, "net.minecraft.util.MovingObjectPosition");
    if (!mopClass) mopClass = env->FindClass("net/minecraft/util/MovingObjectPosition");
    if (mopClass && !env->ExceptionCheck()) {
        g_movingObjectPositionClass = (jclass)env->NewGlobalRef(mopClass);
        g_typeOfHitField = env->GetFieldID(mopClass, "typeOfHit", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;");
        if (!g_typeOfHitField) { env->ExceptionClear(); g_typeOfHitField = env->GetFieldID(mopClass, "field_72313_a", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;"); }
        if (!g_typeOfHitField) env->ExceptionClear();
        else Log("Found typeOfHit field");

        g_entityHitField = env->GetFieldID(mopClass, "entityHit", "Lnet/minecraft/entity/Entity;");
        if (!g_entityHitField) { env->ExceptionClear(); g_entityHitField = env->GetFieldID(mopClass, "field_72308_g", "Lnet/minecraft/entity/Entity;"); }
        if (!g_entityHitField) env->ExceptionClear();
        else Log("Found entityHit field");
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (mopClass && !g_movingObjectTypeClass) {
        jclass mopTypeLocal = env->FindClass("net/minecraft/util/MovingObjectPosition$MovingObjectType");
        if (!mopTypeLocal) {
            env->ExceptionClear();
            mopTypeLocal = LoadClassWithLoader(env, gcl, "net.minecraft.util.MovingObjectPosition$MovingObjectType");
        }
        if (mopTypeLocal && !env->ExceptionCheck()) {
            g_movingObjectTypeClass = (jclass)env->NewGlobalRef(mopTypeLocal);

            jfieldID entityConstField = env->GetStaticFieldID(mopTypeLocal, "ENTITY", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;");
            if (!entityConstField) {
                env->ExceptionClear();
                entityConstField = env->GetStaticFieldID(mopTypeLocal, "field_72310_e", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;");
            }
            if (entityConstField) {
                jobject entityConstLocal = env->GetStaticObjectField(mopTypeLocal, entityConstField);
                if (!env->ExceptionCheck() && entityConstLocal) {
                    g_mopEntityTypeConst = env->NewGlobalRef(entityConstLocal);
                    env->DeleteLocalRef(entityConstLocal);
                    Log("Found MovingObjectType.ENTITY constant");
                } else if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                }
            } else {
                env->ExceptionClear();
            }

            env->DeleteLocalRef(mopTypeLocal);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
    
    jclass enumClass = env->FindClass("java/lang/Enum");
    if (enumClass) {
        g_enumNameMethod = env->GetMethodID(enumClass, "name", "()Ljava/lang/String;");
        env->DeleteLocalRef(enumClass);
    }





    if (!g_getRenderItemFromMcMethod) {
        g_getRenderItemFromMcMethod = env->GetMethodID(mcClass, "getRenderItem", "()Lnet/minecraft/client/renderer/entity/RenderItem;");
        if (!g_getRenderItemFromMcMethod) {
            env->ExceptionClear();
            g_getRenderItemFromMcMethod = env->GetMethodID(mcClass, "func_175599_af", "()Lnet/minecraft/client/renderer/entity/RenderItem;");
        }
        if (!g_getRenderItemFromMcMethod) env->ExceptionClear();
        else Log("Found Minecraft.getRenderItem");
    }

    if (!g_renderItemAndEffectIntoGUIMethod || !g_renderItemIntoGUIMethod) {
        jclass renderItemClass = LoadClassWithLoader(env, gcl, "net/minecraft/client/renderer/entity/RenderItem");
        if (!renderItemClass) renderItemClass = env->FindClass("net/minecraft/client/renderer/entity/RenderItem");
        if (renderItemClass && !env->ExceptionCheck()) {
            if (!g_renderItemAndEffectIntoGUIMethod) {
                g_renderItemAndEffectIntoGUIMethod = env->GetMethodID(renderItemClass, "renderItemAndEffectIntoGUI", "(Lnet/minecraft/item/ItemStack;II)V");
                if (!g_renderItemAndEffectIntoGUIMethod) {
                    env->ExceptionClear();
                    g_renderItemAndEffectIntoGUIMethod = env->GetMethodID(renderItemClass, "func_180450_b", "(Lnet/minecraft/item/ItemStack;II)V");
                }
                if (!g_renderItemAndEffectIntoGUIMethod) env->ExceptionClear();
                else Log("Found RenderItem.renderItemAndEffectIntoGUI");
            }
            if (!g_renderItemIntoGUIMethod) {
                g_renderItemIntoGUIMethod = env->GetMethodID(renderItemClass, "renderItemIntoGUI", "(Lnet/minecraft/item/ItemStack;II)V");
                if (!g_renderItemIntoGUIMethod) {
                    env->ExceptionClear();
                    g_renderItemIntoGUIMethod = env->GetMethodID(renderItemClass, "func_175042_a", "(Lnet/minecraft/item/ItemStack;II)V");
                }
                if (!g_renderItemIntoGUIMethod) env->ExceptionClear();
                else Log("Found RenderItem.renderItemIntoGUI");
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Find rotation fields on Player class (we already found g_posXField's class 'wc')
    // We need to find the class that has rotationYaw/Pitch. It is Entity.
    // 'wc' in previous loop was playerType or super.
    // Let's re-scan if we have playerType.
    // Check for ItemBlock class
    if (!g_itemBlockClass) {
        jclass ibClass = LoadClassWithLoader(env, gcl, "net.minecraft.item.ItemBlock");
        if (!ibClass) ibClass = env->FindClass("net/minecraft/item/ItemBlock");
        
        // Robust Scan: Find class extending Item that has a field of type Block
        if (!ibClass) {
             Log("ItemBlock not found by name, scanning...");
             // First find Item and Block classes
             jclass itemCls = LoadClassWithLoader(env, gcl, "net.minecraft.item.Item");
             if (!itemCls) itemCls = env->FindClass("net/minecraft/item/Item");
             jclass blockCls = LoadClassWithLoader(env, gcl, "net.minecraft.block.Block");
             if (!blockCls) blockCls = env->FindClass("net/minecraft/block/Block");

             if (itemCls && blockCls) {
                 for (int i = 0; i < classCount; i++) {
                     jclass cls = classes[i];
                     if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                     
                     // Check superclass
                     jclass super = (jclass)env->CallObjectMethod(cls, mGetSuper);
                     if (!super || !env->IsSameObject(super, itemCls)) continue;

                     // Check fields for one of type Block
                     jobjectArray fs = (jobjectArray)env->CallObjectMethod(cls, mGetFields);
                     bool hasBlock = false;
                     jsize fc = fs ? env->GetArrayLength(fs) : 0;
                     for(int f=0; f<fc; f++) {
                          jobject fld = env->GetObjectArrayElement(fs, f);
                          jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
                          if (ft && env->IsSameObject(ft, blockCls)) {
                              hasBlock = true;
                              break;
                          }
                     }
                     if (hasBlock) {
                         ibClass = cls;
                         Log("Found ItemBlock candidate by signature");
                         break;
                     }
                 }
             }
        }

        if (ibClass && !env->ExceptionCheck()) {
             g_itemBlockClass = (jclass)env->NewGlobalRef(ibClass);
             Log("Found ItemBlock class");
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (!g_itemSwordClass) {
        jclass swordClass = LoadClassWithLoader(env, gcl, "net.minecraft.item.ItemSword");
        if (!swordClass) swordClass = env->FindClass("net/minecraft/item/ItemSword");
        if (swordClass && !env->ExceptionCheck()) {
            g_itemSwordClass = (jclass)env->NewGlobalRef(swordClass);
            Log("Found ItemSword class");
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (!playerType.empty()) {
        jclass pc = LoadClassWithLoader(env, gcl, playerType.c_str());

        if (!pc) {
             std::string slashName = playerType;
             std::replace(slashName.begin(), slashName.end(), '.', '/');
             pc = env->FindClass(slashName.c_str()); 
        }
        if (!pc) Log("ERROR: Could not find class " + playerType);

        jclass wc = pc; int d = 0;
        while (wc && d < 10 && (!g_rotationYawField || !g_rotationPitchField || !g_getNameMethod || !g_getHeldItemMethod || !g_getTotalArmorValueMethod)) {
             std::string clsName = GetClassNameFromClass(env, wc);
             Log("Scanning hierarchy: " + clsName);

             if (!g_rotationYawField) {
                 g_rotationYawField = env->GetFieldID(wc, "rotationYaw", "F");
                 if(!g_rotationYawField) { env->ExceptionClear(); g_rotationYawField = env->GetFieldID(wc, "field_70177_z", "F"); }
                 if(!g_rotationYawField) env->ExceptionClear();
                 else Log("Found rotationYaw in " + clsName);
             }
             if (!g_rotationPitchField) {
                 g_rotationPitchField = env->GetFieldID(wc, "rotationPitch", "F");
                 if(!g_rotationPitchField) { env->ExceptionClear(); g_rotationPitchField = env->GetFieldID(wc, "field_70125_A", "F"); }
                 if(!g_rotationPitchField) env->ExceptionClear();
                 else Log("Found rotationPitch in " + clsName);
             }
             if (!g_getNameMethod) {
                 g_getNameMethod = env->GetMethodID(wc, "getName", "()Ljava/lang/String;");
                 if(!g_getNameMethod) { env->ExceptionClear(); g_getNameMethod = env->GetMethodID(wc, "func_70005_c_", "()Ljava/lang/String;"); }
                 if(!g_getNameMethod) env->ExceptionClear();
                 else Log("Found getName in " + clsName);
             }
             if (!g_getHeldItemMethod) {
                 g_getHeldItemMethod = env->GetMethodID(wc, "getHeldItem", "()Lnet/minecraft/item/ItemStack;");
                 if(!g_getHeldItemMethod) { env->ExceptionClear(); g_getHeldItemMethod = env->GetMethodID(wc, "func_70694_bm", "()Lnet/minecraft/item/ItemStack;"); }
                 if(!g_getHeldItemMethod) env->ExceptionClear();
                 else Log("Found getHeldItem in " + clsName);
             }
             if (!g_getTotalArmorValueMethod) {
                 g_getTotalArmorValueMethod = env->GetMethodID(wc, "getTotalArmorValue", "()I");
                 if(!g_getTotalArmorValueMethod) { env->ExceptionClear(); g_getTotalArmorValueMethod = env->GetMethodID(wc, "func_70658_aO", "()I"); }
                 if(!g_getTotalArmorValueMethod) env->ExceptionClear();
                 else Log("Found getTotalArmorValue in " + clsName);
             }

             // Last Tick Pos for Interpolation
             if (!g_lastTickPosXField) {
                 g_lastTickPosXField = env->GetFieldID(wc, "lastTickPosX", "D");
                 if(!g_lastTickPosXField) { env->ExceptionClear(); g_lastTickPosXField = env->GetFieldID(wc, "field_70142_S", "D"); }
                 if(!g_lastTickPosXField) { env->ExceptionClear(); g_lastTickPosXField = env->GetFieldID(wc, "prevPosX", "D"); }
                 if(g_lastTickPosXField) Log("Found lastTickPosX");
                 else env->ExceptionClear();
             }
             if (!g_lastTickPosYField) {
                 g_lastTickPosYField = env->GetFieldID(wc, "lastTickPosY", "D");
                 if(!g_lastTickPosYField) { env->ExceptionClear(); g_lastTickPosYField = env->GetFieldID(wc, "field_70137_T", "D"); }
                 if(!g_lastTickPosYField) { env->ExceptionClear(); g_lastTickPosYField = env->GetFieldID(wc, "prevPosY", "D"); }
                 if(g_lastTickPosYField) Log("Found lastTickPosY");
                 else env->ExceptionClear();
             }
             if (!g_lastTickPosZField) {
                 g_lastTickPosZField = env->GetFieldID(wc, "lastTickPosZ", "D");
                 if(!g_lastTickPosZField) { env->ExceptionClear(); g_lastTickPosZField = env->GetFieldID(wc, "field_70136_U", "D"); }
                 if(!g_lastTickPosZField) { env->ExceptionClear(); g_lastTickPosZField = env->GetFieldID(wc, "prevPosZ", "D"); }
                 if(g_lastTickPosZField) Log("Found lastTickPosZ");
                 else env->ExceptionClear();
             }

             wc = (jclass)env->CallObjectMethod(wc, mGetSuper);
             d++;
        }
        
        // Find Inventory
        if (g_thePlayerField) {
             // We need to look at EntityPlayer (superclass of EntityPlayerSP)
             // But we can just scan fields of the player object instance's class hierarchy again
             // or assume it's "inventory" / "field_71071_by"
             
             // Quick scan on player class for InventoryPlayer
             if (!g_inventoryField) {
                  jobject pObj = env->GetObjectField(g_mcInstance, g_thePlayerField);
                  if (pObj) {
                       jclass pClass = env->GetObjectClass(pObj);
                       jclass w = pClass; int depth = 0;
                       while (w && depth < 5) {
                            jobjectArray fs = (jobjectArray)env->CallObjectMethod(w, mGetFields);
                            jsize fc = fs ? env->GetArrayLength(fs) : 0;
                            for (int i=0; i<fc; i++) {
                                 jobject f = env->GetObjectArrayElement(fs, i);
                                 jclass ft = (jclass)env->CallObjectMethod(f, mFType);
                                 std::string ftn = ft ? GetClassNameFromClass(env, ft) : "";
                                 if (ftn.find("InventoryPlayer") != std::string::npos) {
                                      g_inventoryField = env->FromReflectedField(f);
                                      Log("Found inventory field in " + GetClassNameFromClass(env, w));
                                      break;
                                 }
                            }
                            if (g_inventoryField) break;
                            w = (jclass)env->CallObjectMethod(w, mGetSuper);
                            depth++;
                       }
                       env->DeleteLocalRef(pObj);
                  }
                  
                  if (!g_inventoryField) {
                       // Try Common names if reflection failed (e.g. if we didn't have instance yet?)
                       // Actually we have g_mcInstance, so we should be good.
                       // Fallback
                  }
             }

             if (g_inventoryField) {
                  // Find getCurrentItem in InventoryPlayer
                  // We need the class of the field
                   // ... lazily we can just search for method "getCurrentItem" in the object we get
             }
        }
    }

    // Timer Discovery
    if (g_mcClass) {
        g_timerField = env->GetFieldID(g_mcClass, "timer", "Lnet/minecraft/util/Timer;");
        if (!g_timerField) { env->ExceptionClear(); g_timerField = env->GetFieldID(g_mcClass, "field_71428_T", "Lnet/minecraft/util/Timer;"); }
        if (!g_timerField) env->ExceptionClear();
        
        if (g_timerField) {
             Log("Found timer field");
             jobject timerObj = env->GetObjectField(g_mcInstance, g_timerField);
             if (timerObj) {
                 jclass timerClass = env->GetObjectClass(timerObj);
                 g_renderPartialTicksField = env->GetFieldID(timerClass, "renderPartialTicks", "F");
                 if (!g_renderPartialTicksField) { env->ExceptionClear(); g_renderPartialTicksField = env->GetFieldID(timerClass, "field_74281_c", "F"); }
                 if (!g_renderPartialTicksField) { env->ExceptionClear(); g_renderPartialTicksField = env->GetFieldID(timerClass, "elapsedPartialTicks", "F"); }
                 
                 if (g_renderPartialTicksField) Log("Found renderPartialTicks");
                 else { Log("ERROR: Could not find renderPartialTicks"); env->ExceptionClear(); }
                 
                 env->DeleteLocalRef(timerClass);
                 env->DeleteLocalRef(timerObj);
             }
        } else {
             Log("ERROR: Could not find timer field in Minecraft");
        }
    }

    // ActiveRenderInfo Discovery (Dynamic)
    g_activeRenderInfoClass = env->FindClass("net/minecraft/client/renderer/ActiveRenderInfo");
    if (!g_activeRenderInfoClass) {
        env->ExceptionClear();
        Log("ActiveRenderInfo not found by name, scanning classes...");
        for (int i = 0; i < classCount; i++) {
            jclass cls = classes[i];
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            
            // Check static fields signature: 3 FloatBuffers, 1 IntBuffer
            jobjectArray fs = (jobjectArray)env->CallObjectMethod(cls, mGetFields);
            if (!fs || env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            
            int fbCount = 0;
            int ibCount = 0;
            jsize fc = env->GetArrayLength(fs);
            for (int f = 0; f < fc; f++) {
                 jobject fld = env->GetObjectArrayElement(fs, f);
                 if (!fld) continue;
                 jint mod = env->CallIntMethod(fld, mFMod);
                 if (!env->CallStaticBooleanMethod(cMod, mIsStatic, mod)) continue; // Must be static
                 
                 jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
                 if (!ft) continue;
                 std::string ftn = GetClassNameFromClass(env, ft);
                 if (ftn == "java.nio.FloatBuffer") fbCount++;
                 else if (ftn == "java.nio.IntBuffer") ibCount++;
            }
            
            if (fbCount >= 2 && ibCount >= 1) { // At least 2 FB (ModelView, Projection) and 1 IB (Viewport)
                 jstring jn = (jstring)env->CallObjectMethod(cls, mGetName);
                 const char* cn = env->GetStringUTFChars(jn, nullptr);
                 Log("Found ActiveRenderInfo candidate: " + std::string(cn));
                 g_activeRenderInfoClass = (jclass)env->NewGlobalRef(cls);
                 env->ReleaseStringUTFChars(jn, cn);
                 break;
            }
        }
    } else {
        g_activeRenderInfoClass = (jclass)env->NewGlobalRef(g_activeRenderInfoClass);
    }

    if (g_activeRenderInfoClass) {
        g_modelViewField = env->GetStaticFieldID(g_activeRenderInfoClass, "MODELVIEW", "Ljava/nio/FloatBuffer;");
        if (!g_modelViewField) { env->ExceptionClear(); g_modelViewField = env->GetStaticFieldID(g_activeRenderInfoClass, "field_178812_b", "Ljava/nio/FloatBuffer;"); }
        
        g_projectionField = env->GetStaticFieldID(g_activeRenderInfoClass, "PROJECTION", "Ljava/nio/FloatBuffer;");
        if (!g_projectionField) { env->ExceptionClear(); g_projectionField = env->GetStaticFieldID(g_activeRenderInfoClass, "field_178813_c", "Ljava/nio/FloatBuffer;"); }
        
        if (!g_modelViewField || !g_projectionField) {
             Log("Scanning fields for ModelView/Projection fallback...");
             jobjectArray fs = (jobjectArray)env->CallObjectMethod(g_activeRenderInfoClass, mGetFields);
             jsize fc = env->GetArrayLength(fs);
             int fbIdx = 0;
             for (int f = 0; f < fc; f++) {
                 jobject fld = env->GetObjectArrayElement(fs, f);
                 jint mod = env->CallIntMethod(fld, mFMod);
                 if (!env->CallStaticBooleanMethod(cMod, mIsStatic, mod)) continue;
                 
                 jclass ft = (jclass)env->CallObjectMethod(fld, mFType);
                 if (GetClassNameFromClass(env, ft) == "java.nio.FloatBuffer") {
                      if (fbIdx == 0) {
                           g_modelViewField = env->FromReflectedField(fld);
                           Log("Assumed MODELVIEW (idx 0)");
                      } else if (fbIdx == 1) {
                           g_projectionField = env->FromReflectedField(fld);
                           Log("Assumed PROJECTION (idx 1)");
                      }
                      fbIdx++;
                 }
             }
        }

        if (g_modelViewField && g_projectionField) Log("Found OpenGL Matrix fields");
        else Log("ERROR: Could not find OpenGL Matrix fields");
    } else {
        Log("ERROR: Could not find ActiveRenderInfo class");
        env->ExceptionClear();
    }
    
    // FloatBuffer helper
    jclass floatBufferClass = env->FindClass("java/nio/FloatBuffer");
    if (floatBufferClass) {
        g_floatBufferGet = env->GetMethodID(floatBufferClass, "get", "(I)F");
        env->DeleteLocalRef(floatBufferClass);
    }

    // RenderManager Discovery
    if (g_mcClass) {
        g_renderManagerField = env->GetFieldID(g_mcClass, "renderManager", "Lnet/minecraft/client/renderer/entity/RenderManager;");
        if (!g_renderManagerField) { env->ExceptionClear(); g_renderManagerField = env->GetFieldID(g_mcClass, "field_175616_W", "Lnet/minecraft/client/renderer/entity/RenderManager;"); }
        if (!g_renderManagerField) env->ExceptionClear();

        if (g_renderManagerField) {
             Log("Found renderManager field");
             jobject rmObj = env->GetObjectField(g_mcInstance, g_renderManagerField);
             if (rmObj) {
                 jclass rmClass = env->GetObjectClass(rmObj);
                 
                 g_viewerPosXField = env->GetFieldID(rmClass, "viewerPosX", "D");
                 if (!g_viewerPosXField) { env->ExceptionClear(); g_viewerPosXField = env->GetFieldID(rmClass, "field_78725_b", "D"); } // o
                 if (!g_viewerPosXField) { env->ExceptionClear(); g_viewerPosXField = env->GetFieldID(rmClass, "o", "D"); }
                 
                 g_viewerPosYField = env->GetFieldID(rmClass, "viewerPosY", "D");
                 if (!g_viewerPosYField) { env->ExceptionClear(); g_viewerPosYField = env->GetFieldID(rmClass, "field_78726_c", "D"); } // p
                 if (!g_viewerPosYField) { env->ExceptionClear(); g_viewerPosYField = env->GetFieldID(rmClass, "p", "D"); }

                 g_viewerPosZField = env->GetFieldID(rmClass, "viewerPosZ", "D");
                 if (!g_viewerPosZField) { env->ExceptionClear(); g_viewerPosZField = env->GetFieldID(rmClass, "field_78723_d", "D"); } // q
                 if (!g_viewerPosZField) { env->ExceptionClear(); g_viewerPosZField = env->GetFieldID(rmClass, "q", "D"); }

                 if (g_viewerPosXField) Log("Found RenderManager viewerPos fields");
                 else Log("ERROR: Could not find RenderManager viewerPos fields");
                 
                 env->DeleteLocalRef(rmClass);
                 env->DeleteLocalRef(rmObj);
             }
        }
    }

    // List methods
    jclass listCls = env->FindClass("java/util/List");
    if (listCls) {
        g_listSizeMethod = env->GetMethodID(listCls, "size", "()I");
        g_listGetMethod = env->GetMethodID(listCls, "get", "(I)Ljava/lang/Object;");
        g_listClass = (jclass)env->NewGlobalRef(listCls);
    }

    jclass objectCls = env->FindClass("java/lang/Object");
    if (objectCls) {
        g_objectHashCodeMethod = env->GetMethodID(objectCls, "hashCode", "()I");
        env->DeleteLocalRef(objectCls);
    }

    g_mapped = (g_thePlayerField != nullptr);
    Log("=== Mapping Report ===");
    Log("Player: " + std::string(g_thePlayerField ? "YES" : "NO"));
    Log("World: " + std::string(g_theWorldField ? "YES" : "NO"));
    Log("Entities: " + std::string(g_playerEntitiesField ? "YES" : "NO"));
    Log("TileEntities: " + std::string(g_loadedTileEntityListField ? "YES" : "NO"));
    Log("TilePos: " + std::string(g_tileEntityPosField ? "YES" : "NO"));
    Log("BlockPosXYZ: " + std::string((g_blockPosGetX && g_blockPosGetY && g_blockPosGetZ) ? "YES" : "NO"));
    Log("Yaw: " + std::string(g_rotationYawField ? "YES" : "NO"));
    Log("Pitch: " + std::string(g_rotationPitchField ? "YES" : "NO"));
    Log("Name: " + std::string(g_getNameMethod ? "YES" : "NO"));
    Log("Inventory: " + std::string(g_inventoryField ? "YES" : "NO"));
    Log("ItemBlock: " + std::string(g_itemBlockClass ? "YES" : "NO"));
    Log("=== End Report ===");
    jvmti->Deallocate((unsigned char*)classes);
    return g_mapped;
}

void TryResolveWorldMappings(JNIEnv* env) {
    if (!env || !g_mcInstance || !g_theWorldField) return;
    if (g_playerEntitiesField && g_loadedTileEntityListField) return;

    jobject world = env->GetObjectField(g_mcInstance, g_theWorldField);
    if (!world) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    jclass worldClass = env->GetObjectClass(world);
    if (worldClass) {
        if (!g_playerEntitiesField) {
            g_playerEntitiesField = env->GetFieldID(worldClass, "playerEntities", "Ljava/util/List;");
            if (!g_playerEntitiesField) {
                env->ExceptionClear();
                g_playerEntitiesField = env->GetFieldID(worldClass, "field_73010_i", "Ljava/util/List;");
            }
            if (!g_playerEntitiesField) {
                env->ExceptionClear();
            } else {
                Log("Late-bound playerEntities field");
            }
        }

        if (!g_loadedTileEntityListField) {
            g_loadedTileEntityListField = env->GetFieldID(worldClass, "loadedTileEntityList", "Ljava/util/List;");
            if (!g_loadedTileEntityListField) {
                env->ExceptionClear();
                g_loadedTileEntityListField = env->GetFieldID(worldClass, "field_147482_g", "Ljava/util/List;");
            }
            if (!g_loadedTileEntityListField) {
                env->ExceptionClear();
            } else {
                Log("Late-bound loadedTileEntityList field");
            }
        }

        env->DeleteLocalRef(worldClass);
    }
    env->DeleteLocalRef(world);
}

void TryResolveRenderMappings(JNIEnv* env, bool includeActionBarMappings = false) {
    if (!env || !g_mcInstance) return;

    jobject gcl = EnsureGameClassLoader(env);

    if (!g_activeRenderInfoClass) {
        jclass ariLocal = nullptr;
        if (gcl) {
            ariLocal = LoadClassWithLoader(env, gcl, "net.minecraft.client.renderer.ActiveRenderInfo");
        }
        if (!ariLocal) {
            ariLocal = env->FindClass("net/minecraft/client/renderer/ActiveRenderInfo");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!ariLocal) {
            if (env->ExceptionCheck()) env->ExceptionClear();
        } else {
            g_activeRenderInfoClass = (jclass)env->NewGlobalRef(ariLocal);
            env->DeleteLocalRef(ariLocal);
            Log("Late-bound ActiveRenderInfo class");
        }
    }

    if (g_activeRenderInfoClass) {
        if (!g_modelViewField) {
            g_modelViewField = env->GetStaticFieldID(g_activeRenderInfoClass, "MODELVIEW", "Ljava/nio/FloatBuffer;");
            if (!g_modelViewField) {
                env->ExceptionClear();
                g_modelViewField = env->GetStaticFieldID(g_activeRenderInfoClass, "field_178812_b", "Ljava/nio/FloatBuffer;");
            }
            if (!g_modelViewField) {
                env->ExceptionClear();
            } else {
                Log("Late-bound MODELVIEW field");
            }
        }

        if (!g_projectionField) {
            g_projectionField = env->GetStaticFieldID(g_activeRenderInfoClass, "PROJECTION", "Ljava/nio/FloatBuffer;");
            if (!g_projectionField) {
                env->ExceptionClear();
                g_projectionField = env->GetStaticFieldID(g_activeRenderInfoClass, "field_178813_c", "Ljava/nio/FloatBuffer;");
            }
            if (!g_projectionField) {
                env->ExceptionClear();
            } else {
                Log("Late-bound PROJECTION field");
            }
        }

        if (includeActionBarMappings) {
            // Ingame GUI / action-bar text mappings (for GTB on 1.8.9)
            if (!g_ingameGuiField) {
                g_ingameGuiField = env->GetFieldID(g_mcClass, "ingameGUI", "Lnet/minecraft/client/gui/GuiIngame;");
                if (!g_ingameGuiField) {
                    env->ExceptionClear();
                    g_ingameGuiField = env->GetFieldID(g_mcClass, "field_71456_v", "Lnet/minecraft/client/gui/GuiIngame;");
                }
                if (!g_ingameGuiField) env->ExceptionClear();
                else Log("Found ingameGUI field");
            }

            if (g_ingameGuiField) {
                DWORD nowMs = GetTickCount();
                bool shouldProbe = g_actionBarFields.empty() || (nowMs - g_lastActionBarProbeMs) > 4000;
                if (shouldProbe) {
                    g_lastActionBarProbeMs = nowMs;
                    jobject ingameGui = env->GetObjectField(g_mcInstance, g_ingameGuiField);
                    if (ingameGui && !env->ExceptionCheck()) {
                        jclass hudClass = env->GetObjectClass(ingameGui);
                        if (hudClass) {
                            std::vector<ActionBarFieldRef> discovered;
                            auto addFieldByName = [&](const char* name, ActionBarFieldKind kind) {
                                const char* sig = (kind == ActionBarFieldChatComponent)
                                    ? "Lnet/minecraft/util/IChatComponent;"
                                    : "Ljava/lang/String;";
                                jfieldID fid = env->GetFieldID(hudClass, name, sig);
                                if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
                                if (!fid) return;
                                for (size_t i = 0; i < discovered.size(); i++) {
                                    if (discovered[i].field == fid) return;
                                }
                                ActionBarFieldRef ref;
                                ref.field = fid;
                                ref.kind = kind;
                                discovered.push_back(ref);
                            };

                            const char* actionBarNameCandidates[] = {
                                // 1.8.9 often stores actionbar-like text in recordPlaying (String).
                                "recordPlaying",
                                "overlayMessage",
                                "field_73838_g",
                                // Title/subtitle paths used by some plugins/clients.
                                "displayedTitle",
                                "field_73845_h",
                                nullptr
                            };
                            for (int i = 0; actionBarNameCandidates[i]; i++) {
                                // Try both signatures on each candidate name because 1.8.9 forks
                                // differ on whether overlay text is String vs IChatComponent.
                                addFieldByName(actionBarNameCandidates[i], ActionBarFieldString);
                                addFieldByName(actionBarNameCandidates[i], ActionBarFieldChatComponent);
                            }

                            {
                                jclass cClass = env->FindClass("java/lang/Class");
                                jclass cField = env->FindClass("java/lang/reflect/Field");
                                jclass cMod = env->FindClass("java/lang/reflect/Modifier");
                                if (env->ExceptionCheck()) {
                                    env->ExceptionClear();
                                    cClass = nullptr;
                                    cField = nullptr;
                                    cMod = nullptr;
                                }

                                if (cClass && cField && cMod) {
                                    jmethodID mGetFields = env->GetMethodID(cClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
                                    jmethodID mFType = env->GetMethodID(cField, "getType", "()Ljava/lang/Class;");
                                    jmethodID mFMod = env->GetMethodID(cField, "getModifiers", "()I");
                                    jmethodID mIsStatic = env->GetStaticMethodID(cMod, "isStatic", "(I)Z");
                                    if (env->ExceptionCheck()) {
                                        env->ExceptionClear();
                                        mGetFields = nullptr;
                                    }

                                    if (mGetFields && mFType && mFMod && mIsStatic) {
                                        jobjectArray fields = (jobjectArray)env->CallObjectMethod(hudClass, mGetFields);
                                        if (env->ExceptionCheck()) { env->ExceptionClear(); fields = nullptr; }
                                        if (fields) {
                                            jsize fieldCount = env->GetArrayLength(fields);
                                            for (jsize i = 0; i < fieldCount; i++) {
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

                                                ActionBarFieldKind kind;
                                                bool typeOk = false;
                                                if (typeName == "net.minecraft.util.IChatComponent") {
                                                    kind = ActionBarFieldChatComponent;
                                                    typeOk = true;
                                                } else if (typeName == "java.lang.String") {
                                                    kind = ActionBarFieldString;
                                                    typeOk = true;
                                                }

                                                if (typeOk) {
                                                    jfieldID fid = env->FromReflectedField(fld);
                                                    if (env->ExceptionCheck()) { env->ExceptionClear(); fid = nullptr; }
                                                    if (fid) {
                                                        bool exists = false;
                                                        for (size_t k = 0; k < discovered.size(); k++) {
                                                            if (discovered[k].field == fid) { exists = true; break; }
                                                        }
                                                        if (!exists) {
                                                            ActionBarFieldRef ref;
                                                            ref.field = fid;
                                                            ref.kind = kind;
                                                            discovered.push_back(ref);
                                                        }
                                                    }
                                                }

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

                            if (!discovered.empty()) {
                                std::vector<ActionBarFieldRef> prioritized;
                                prioritized.reserve(discovered.size());

                                auto appendKind = [&](ActionBarFieldKind kind) {
                                    for (size_t i = 0; i < discovered.size(); i++) {
                                        if (discovered[i].kind == kind) {
                                            prioritized.push_back(discovered[i]);
                                        }
                                    }
                                };

                                // 1.8.9 GTB usually uses title/string path for the masked word.
                                appendKind(ActionBarFieldString);
                                appendKind(ActionBarFieldChatComponent);

                                if (!prioritized.empty()) {
                                    g_actionBarFields = prioritized;
                                } else {
                                    g_actionBarFields = discovered;
                                }
                                if ((int)g_actionBarFields.size() != g_lastActionBarFieldCountLogged) {
                                    int chatCount = 0;
                                    int stringCount = 0;
                                    for (size_t i = 0; i < g_actionBarFields.size(); i++) {
                                        if (g_actionBarFields[i].kind == ActionBarFieldChatComponent) chatCount++;
                                        else if (g_actionBarFields[i].kind == ActionBarFieldString) stringCount++;
                                    }
                                    Log("Found actionbar field candidates on GuiIngame: total="
                                        + std::to_string((int)g_actionBarFields.size())
                                        + " chat=" + std::to_string(chatCount)
                                        + " string=" + std::to_string(stringCount));
                                    g_lastActionBarFieldCountLogged = (int)g_actionBarFields.size();
                                }
                            } else if (g_lastActionBarFieldCountLogged != 0) {
                                Log("WARNING: no actionbar-like fields found on GuiIngame");
                                g_lastActionBarFieldCountLogged = 0;
                            }
                        }
                        if (hudClass) env->DeleteLocalRef(hudClass);
                        env->DeleteLocalRef(ingameGui);
                    } else if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                    }
                }
            }

            if (!g_chatComponentGetTextMethod) {
                jclass chatCompClass = LoadClassWithLoader(env, gcl, "net.minecraft.util.IChatComponent");
                if (!chatCompClass) {
                    chatCompClass = env->FindClass("net/minecraft/util/IChatComponent");
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
                if (chatCompClass) {
                    g_chatComponentGetTextMethod = env->GetMethodID(chatCompClass, "getUnformattedText", "()Ljava/lang/String;");
                    if (!g_chatComponentGetTextMethod) {
                        env->ExceptionClear();
                        g_chatComponentGetTextMethod = env->GetMethodID(chatCompClass, "func_150260_c", "()Ljava/lang/String;");
                    }
                    if (!g_chatComponentGetTextMethod) env->ExceptionClear();
                    else Log("Found IChatComponent#getUnformattedText method");
                }
            }
        }
    }

    if (!g_renderManagerField && g_mcClass) {
        g_renderManagerField = env->GetFieldID(g_mcClass, "renderManager", "Lnet/minecraft/client/renderer/entity/RenderManager;");
        if (!g_renderManagerField) {
            env->ExceptionClear();
            g_renderManagerField = env->GetFieldID(g_mcClass, "field_175616_W", "Lnet/minecraft/client/renderer/entity/RenderManager;");
        }
        if (!g_renderManagerField) {
            env->ExceptionClear();
        } else {
            Log("Late-bound renderManager field");
        }
    }

    if (g_renderManagerField && (!g_viewerPosXField || !g_viewerPosYField || !g_viewerPosZField)) {
        jobject rmObj = env->GetObjectField(g_mcInstance, g_renderManagerField);
        if (rmObj) {
            jclass rmClass = env->GetObjectClass(rmObj);
            if (rmClass) {
                if (!g_viewerPosXField) {
                    g_viewerPosXField = env->GetFieldID(rmClass, "viewerPosX", "D");
                    if (!g_viewerPosXField) {
                        env->ExceptionClear();
                        g_viewerPosXField = env->GetFieldID(rmClass, "field_78725_b", "D");
                    }
                    if (!g_viewerPosXField) {
                        env->ExceptionClear();
                        g_viewerPosXField = env->GetFieldID(rmClass, "o", "D");
                    }
                    if (!g_viewerPosXField) env->ExceptionClear();
                }

                if (!g_viewerPosYField) {
                    g_viewerPosYField = env->GetFieldID(rmClass, "viewerPosY", "D");
                    if (!g_viewerPosYField) {
                        env->ExceptionClear();
                        g_viewerPosYField = env->GetFieldID(rmClass, "field_78726_c", "D");
                    }
                    if (!g_viewerPosYField) {
                        env->ExceptionClear();
                        g_viewerPosYField = env->GetFieldID(rmClass, "p", "D");
                    }
                    if (!g_viewerPosYField) env->ExceptionClear();
                }

                if (!g_viewerPosZField) {
                    g_viewerPosZField = env->GetFieldID(rmClass, "viewerPosZ", "D");
                    if (!g_viewerPosZField) {
                        env->ExceptionClear();
                        g_viewerPosZField = env->GetFieldID(rmClass, "field_78723_d", "D");
                    }
                    if (!g_viewerPosZField) {
                        env->ExceptionClear();
                        g_viewerPosZField = env->GetFieldID(rmClass, "q", "D");
                    }
                    if (!g_viewerPosZField) env->ExceptionClear();
                }

                if (g_viewerPosXField && g_viewerPosYField && g_viewerPosZField) {
                    Log("Late-bound RenderManager viewerPos fields");
                }

                env->DeleteLocalRef(rmClass);
            }
            env->DeleteLocalRef(rmObj);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
}

void MaybeRefreshMappings(JNIEnv* env) {
    if (!env || !g_jvm) return;

    bool needRefresh = !g_mapped
        || !g_mcInstance
        || !g_theWorldField
        || !g_playerEntitiesField
        || !g_loadedTileEntityListField
        || !g_activeRenderInfoClass
        || !g_modelViewField
        || !g_projectionField
        || !g_renderManagerField
        || !g_viewerPosXField
        || !g_viewerPosYField
        || !g_viewerPosZField;

    if (!needRefresh) return;

    static DWORD nextRetryAt = 0;
    DWORD now = GetTickCount();
    if (now < nextRetryAt) return;
    nextRetryAt = now + 4000;

    Log("Mappings incomplete, retrying discovery...");
    bool ok = DiscoverMappings(env);
    Log(ok ? "Mapping refresh complete" : "Mapping refresh failed");
}

static void EnsureVelocityMappings(JNIEnv* env, jobject playerObj) {
    if (!env || !playerObj) return;
    if (g_motionXField && g_motionYField && g_motionZField && g_hurtTimeField) return;

    jclass playerCls = env->GetObjectClass(playerObj);
    if (!playerCls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }

    if (!g_motionXField) {
        g_motionXField = env->GetFieldID(playerCls, "motionX", "D");
        if (!g_motionXField) {
            env->ExceptionClear();
            g_motionXField = env->GetFieldID(playerCls, "field_70159_w", "D");
        }
        if (!g_motionXField) env->ExceptionClear();
    }

    if (!g_motionYField) {
        g_motionYField = env->GetFieldID(playerCls, "motionY", "D");
        if (!g_motionYField) {
            env->ExceptionClear();
            g_motionYField = env->GetFieldID(playerCls, "field_70181_x", "D");
        }
        if (!g_motionYField) env->ExceptionClear();
    }

    if (!g_motionZField) {
        g_motionZField = env->GetFieldID(playerCls, "motionZ", "D");
        if (!g_motionZField) {
            env->ExceptionClear();
            g_motionZField = env->GetFieldID(playerCls, "field_70179_y", "D");
        }
        if (!g_motionZField) env->ExceptionClear();
    }

    if (!g_hurtTimeField) {
        g_hurtTimeField = env->GetFieldID(playerCls, "hurtTime", "I");
        if (!g_hurtTimeField) {
            env->ExceptionClear();
            g_hurtTimeField = env->GetFieldID(playerCls, "field_70737_aN", "I");
        }
        if (!g_hurtTimeField) env->ExceptionClear();
    }

    env->DeleteLocalRef(playerCls);
}

static void UpdateVelocity(JNIEnv* env, const Config& cfg) {
    if (!env || !g_mcInstance || !g_thePlayerField) return;

    jobject selfObj = env->GetObjectField(g_mcInstance, g_thePlayerField);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        selfObj = nullptr;
    }
    if (!selfObj) {
        g_lastHurtTime = 0;
        return;
    }

    EnsureVelocityMappings(env, selfObj);
    if (!g_motionXField || !g_motionYField || !g_motionZField || !g_hurtTimeField) {
        env->DeleteLocalRef(selfObj);
        return;
    }

    int hurtTime = env->GetIntField(selfObj, g_hurtTimeField);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(selfObj);
        return;
    }

    bool newHit = (hurtTime > 0 && hurtTime > g_lastHurtTime);
    if (cfg.velocityEnabled && newHit) {
        int rv = rand() % 100;
        if (rv < cfg.velocityChance) {
            double vx = env->GetDoubleField(selfObj, g_motionXField);
            double vy = env->GetDoubleField(selfObj, g_motionYField);
            double vz = env->GetDoubleField(selfObj, g_motionZField);
            if (!env->ExceptionCheck()) {
                double horizMag = std::sqrt(vx * vx + vz * vz);
                bool looksLikeKnockback = (horizMag > 0.18 || std::fabs(vy) > 0.14);
                if (looksLikeKnockback && (cfg.velocityHorizontal != 100 || cfg.velocityVertical != 100)) {
                    double hScale = (double)cfg.velocityHorizontal / 100.0;
                    double vScale = (double)cfg.velocityVertical / 100.0;
                    env->SetDoubleField(selfObj, g_motionXField, vx * hScale);
                    env->SetDoubleField(selfObj, g_motionYField, vy * vScale);
                    env->SetDoubleField(selfObj, g_motionZField, vz * hScale);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                }
            } else {
                env->ExceptionClear();
            }
        }
    }

    g_lastHurtTime = hurtTime;
    env->DeleteLocalRef(selfObj);
}

static bool EnsureReachMappings(JNIEnv* env) {
    if (!env || !g_mcClass || !g_movingObjectPositionClass) return false;

    if (!g_mopEntityCtor) {
        g_mopEntityCtor = env->GetMethodID(g_movingObjectPositionClass, "<init>", "(Lnet/minecraft/entity/Entity;)V");
        g_mopCtorNeedsVec3 = false;
        if (!g_mopEntityCtor) {
            env->ExceptionClear();
            g_mopEntityCtor = env->GetMethodID(g_movingObjectPositionClass, "<init>", "(Lnet/minecraft/entity/Entity;Lnet/minecraft/util/Vec3;)V");
            g_mopCtorNeedsVec3 = (g_mopEntityCtor != nullptr);
        }
        if (!g_mopEntityCtor) env->ExceptionClear();
    }

    if (g_mopCtorNeedsVec3) {
        if (!g_vec3Class) {
            jclass vec3Local = env->FindClass("net/minecraft/util/Vec3");
            if (!vec3Local) {
                env->ExceptionClear();
                jobject gcl = EnsureGameClassLoader(env);
                if (gcl) vec3Local = LoadClassWithLoader(env, gcl, "net.minecraft.util.Vec3");
            }
            if (vec3Local && !env->ExceptionCheck()) {
                g_vec3Class = (jclass)env->NewGlobalRef(vec3Local);
                env->DeleteLocalRef(vec3Local);
            } else if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
        }
        if (g_vec3Class && !g_vec3Ctor) {
            g_vec3Ctor = env->GetMethodID(g_vec3Class, "<init>", "(DDD)V");
            if (!g_vec3Ctor) env->ExceptionClear();
        }
    }

    if (!g_entityHitField) {
        g_entityHitField = env->GetFieldID(g_movingObjectPositionClass, "entityHit", "Lnet/minecraft/entity/Entity;");
        if (!g_entityHitField) {
            env->ExceptionClear();
            g_entityHitField = env->GetFieldID(g_movingObjectPositionClass, "field_72308_g", "Lnet/minecraft/entity/Entity;");
        }
        if (!g_entityHitField) env->ExceptionClear();
    }

    if (!g_mopEntityTypeConst) {
        jclass mopTypeLocal = env->FindClass("net/minecraft/util/MovingObjectPosition$MovingObjectType");
        if (!mopTypeLocal) {
            env->ExceptionClear();
            jobject gcl = EnsureGameClassLoader(env);
            if (gcl) mopTypeLocal = LoadClassWithLoader(env, gcl, "net.minecraft.util.MovingObjectPosition$MovingObjectType");
        }
        if (mopTypeLocal && !env->ExceptionCheck()) {
            jfieldID entityConstField = env->GetStaticFieldID(mopTypeLocal, "ENTITY", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;");
            if (!entityConstField) {
                env->ExceptionClear();
                entityConstField = env->GetStaticFieldID(mopTypeLocal, "field_72310_e", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;");
            }
            if (entityConstField) {
                jobject entityConstLocal = env->GetStaticObjectField(mopTypeLocal, entityConstField);
                if (!env->ExceptionCheck() && entityConstLocal) {
                    g_mopEntityTypeConst = env->NewGlobalRef(entityConstLocal);
                    env->DeleteLocalRef(entityConstLocal);
                } else if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                }
            } else {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(mopTypeLocal);
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    return g_objectMouseOverField != nullptr;
}

static void UpdateReach(JNIEnv* env, const Config& cfg, const GameState& state, bool forceClick = false) {
    if (!env) return;

    DWORD nowMs = GetTickCount();
    bool lmbPhysicalDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool syntheticClicking = cfg.clicking && cfg.leftClick;
    bool clickEdge = forceClick
        || (lmbPhysicalDown && !g_reachClickPrevDown)
        || (syntheticClicking && !g_reachClickPrevSynthetic);
    g_reachClickPrevDown = lmbPhysicalDown;
    g_reachClickPrevSynthetic = syntheticClicking;

    if (!cfg.reachEnabled) {
        g_reachAllowCurrentClick = false;
        g_reachCurrentClickRange = 3.0;
        g_reachClickWindowUntilMs = 0;
        g_reachLastRollMs = 0;
        g_reachClickPrevSynthetic = false;
        if (g_reachCurrentTarget) {
            env->DeleteGlobalRef(g_reachCurrentTarget);
            g_reachCurrentTarget = nullptr;
        }
        return;
    }

    if (clickEdge || ((lmbPhysicalDown || syntheticClicking) && (nowMs - g_reachLastRollMs) >= 120)) {
        g_reachAllowCurrentClick = ((rand() % 100) < cfg.reachChance);
        float reachSpan = cfg.reachMax - cfg.reachMin;
        if (reachSpan < 0.0f) reachSpan = 0.0f;
        float rfrac = (float)rand() / (float)RAND_MAX;
        g_reachCurrentClickRange = (double)(cfg.reachMin + (rfrac * reachSpan));
        if (g_reachCurrentClickRange < 3.0) g_reachCurrentClickRange = 3.0;
        g_reachLastRollMs = nowMs;
        g_reachClickWindowUntilMs = nowMs + 120;
    }

    bool clickWindowActive = forceClick || lmbPhysicalDown || syntheticClicking || nowMs <= g_reachClickWindowUntilMs;
    if (!clickWindowActive) {
        g_reachAllowCurrentClick = false;
        if (g_reachCurrentTarget) {
            env->DeleteGlobalRef(g_reachCurrentTarget);
            g_reachCurrentTarget = nullptr;
        }
        return;
    }

    auto maybeLogReach = [&](const std::string& msg) {
        DWORD t = GetTickCount();
        if (t - g_lastReachDebugLogMs >= 1000) {
            g_lastReachDebugLogMs = t;
            Log(std::string("Reach: ") + msg);
        }
    };

    if (!g_reachAllowCurrentClick) {
        maybeLogReach("roll denied this click");
        return;
    }
    if (!g_mcInstance || !g_thePlayerField || (!g_objectMouseOverField && !g_pointedEntityField)) return;
    if (!EnsureReachMappings(env)) return;
    if (!state.mapped || state.guiOpen) return;

    jobject player = env->GetObjectField(g_mcInstance, g_thePlayerField);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        player = nullptr;
    }
    if (!player) {
        maybeLogReach("no local player");
        return;
    }

    jobject world = env->GetObjectField(g_mcInstance, g_theWorldField);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        world = nullptr;
    }
    if (!world) {
        maybeLogReach("no world");
        env->DeleteLocalRef(player);
        return;
    }

    if (!g_playerEntitiesField || !g_listSizeMethod || !g_listGetMethod) {
        TryResolveWorldMappings(env);
    }
    if (!g_playerEntitiesField || !g_listSizeMethod || !g_listGetMethod || !g_posXField || !g_posYField || !g_posZField) {
        maybeLogReach("missing mappings: players/list/pos");
        env->DeleteLocalRef(world);
        env->DeleteLocalRef(player);
        return;
    }

    jobject list = env->GetObjectField(world, g_playerEntitiesField);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        list = nullptr;
    }
    if (!list) {
        maybeLogReach("player list unavailable");
        env->DeleteLocalRef(world);
        env->DeleteLocalRef(player);
        return;
    }

    int size = env->CallIntMethod(list, g_listSizeMethod);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        size = 0;
    }

    double px = env->GetDoubleField(player, g_posXField);
    double py = env->GetDoubleField(player, g_posYField);
    double pz = env->GetDoubleField(player, g_posZField);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        size = 0;
    }

    float localYaw = 0.0f;
    float localPitch = 0.0f;
    bool haveViewAngles = false;
    if (g_rotationYawField && g_rotationPitchField) {
        localYaw = env->GetFloatField(player, g_rotationYawField);
        localPitch = env->GetFloatField(player, g_rotationPitchField);
        if (!env->ExceptionCheck()) {
            haveViewAngles = true;
        } else {
            env->ExceptionClear();
        }
    }

    const double kPi = 3.14159265358979323846;
    double yawRad = localYaw * (kPi / 180.0);
    double pitchRad = localPitch * (kPi / 180.0);
    double lookX = -std::sin(yawRad) * std::cos(pitchRad);
    double lookY = -std::sin(pitchRad);
    double lookZ = std::cos(yawRad) * std::cos(pitchRad);
    double lookLenSq = lookX * lookX + lookY * lookY + lookZ * lookZ;
    if (lookLenSq > 1e-9) {
        double invLen = 1.0 / std::sqrt(lookLenSq);
        lookX *= invLen;
        lookY *= invLen;
        lookZ *= invLen;
    } else {
        haveViewAngles = false;
    }

    double eyeX = px;
    double eyeY = py + 1.62;
    double eyeZ = pz;

    double sampleReach = g_reachCurrentClickRange;

    jobject bestEntity = nullptr;
    double bestDistSq = sampleReach * sampleReach;
    double bestPerpSq = 1e18;
    double bestForward = 1e18;

    for (int i = 0; i < size; i++) {
        jobject entity = env->CallObjectMethod(list, g_listGetMethod, i);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            break;
        }
        if (!entity) continue;
        if (env->IsSameObject(entity, player)) {
            env->DeleteLocalRef(entity);
            continue;
        }

        std::string stableName = GetStablePlayerName(env, entity);
        if (stableName.empty()) {
            env->DeleteLocalRef(entity);
            continue;
        }

        double ex = env->GetDoubleField(entity, g_posXField);
        double ey = env->GetDoubleField(entity, g_posYField);
        double ez = env->GetDoubleField(entity, g_posZField);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(entity);
            continue;
        }

        double targetX = ex;
        double targetY = ey + 1.62;
        double targetZ = ez;

        double dx = targetX - eyeX;
        double dy = targetY - eyeY;
        double dz = targetZ - eyeZ;
        double distSq = dx * dx + dy * dy + dz * dz;

        bool candidate = false;
        double candidatePerpSq = 0.0;
        double candidateForward = 0.0;

        if (haveViewAngles) {
            candidateForward = dx * lookX + dy * lookY + dz * lookZ;
            if (candidateForward > 0.0 && candidateForward <= sampleReach) {
                candidatePerpSq = distSq - (candidateForward * candidateForward);
                if (candidatePerpSq < 0.0) candidatePerpSq = 0.0;

                // Approximates a permissive hit cylinder around the crosshair ray.
                // This keeps targeting intuitive while still requiring aim direction.
                double maxPerp = 0.95;
                if (candidatePerpSq <= (maxPerp * maxPerp)) {
                    candidate = true;
                }
            }
        } else {
            if (distSq <= (sampleReach * sampleReach)) {
                candidate = true;
            }
        }

        if (candidate) {
            bool better = false;
            if (haveViewAngles) {
                if (candidatePerpSq + 1e-6 < bestPerpSq) {
                    better = true;
                } else if (std::abs(candidatePerpSq - bestPerpSq) <= 1e-6 && candidateForward < bestForward) {
                    better = true;
                }
            } else if (distSq <= bestDistSq) {
                better = true;
            }

            if (better) {
                if (bestEntity) env->DeleteLocalRef(bestEntity);
                bestEntity = entity;
                bestDistSq = distSq;
                bestPerpSq = candidatePerpSq;
                bestForward = candidateForward;
                continue;
            }
        }

        if (!haveViewAngles && distSq <= bestDistSq) {
            if (bestEntity) env->DeleteLocalRef(bestEntity);
            bestEntity = entity;
            bestDistSq = distSq;
            continue;
        }

        env->DeleteLocalRef(entity);
    }

    if (bestEntity) {
        if (g_reachCurrentTarget) {
            env->DeleteGlobalRef(g_reachCurrentTarget);
            g_reachCurrentTarget = nullptr;
        }
        g_reachCurrentTarget = env->NewGlobalRef(bestEntity);

        jobject mop = nullptr;
        if (g_mopCtorNeedsVec3) {
            if (g_vec3Class && g_vec3Ctor) {
                double ex = env->GetDoubleField(bestEntity, g_posXField);
                double ey = env->GetDoubleField(bestEntity, g_posYField);
                double ez = env->GetDoubleField(bestEntity, g_posZField);
                if (!env->ExceptionCheck()) {
                    jobject vec = env->NewObject(g_vec3Class, g_vec3Ctor, ex, ey + 1.62, ez);
                    if (!env->ExceptionCheck() && vec) {
                        mop = env->NewObject(g_movingObjectPositionClass, g_mopEntityCtor, bestEntity, vec);
                        env->DeleteLocalRef(vec);
                    } else if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                    }
                } else {
                    env->ExceptionClear();
                }
            }
            if (!mop && !env->ExceptionCheck()) {
                // Fallback: entity-only ctor may still exist on some forks.
                jobject fallback = env->NewObject(g_movingObjectPositionClass, g_mopEntityCtor, bestEntity);
                if (!env->ExceptionCheck() && fallback) {
                    mop = fallback;
                } else if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                }
            }
        } else {
            mop = env->NewObject(g_movingObjectPositionClass, g_mopEntityCtor, bestEntity);
        }

        bool pointedPatched = false;
        if (g_pointedEntityField) {
            env->SetObjectField(g_mcInstance, g_pointedEntityField, bestEntity);
            if (!env->ExceptionCheck()) {
                pointedPatched = true;
            } else {
                env->ExceptionClear();
            }
        }

        if (!mop && !env->ExceptionCheck() && g_objectMouseOverField) {
            jobject curMop = env->GetObjectField(g_mcInstance, g_objectMouseOverField);
            if (!env->ExceptionCheck() && curMop) {
                if (g_entityHitField) {
                    env->SetObjectField(curMop, g_entityHitField, bestEntity);
                    if (!env->ExceptionCheck()) {
                        if (g_typeOfHitField && g_mopEntityTypeConst) {
                            env->SetObjectField(curMop, g_typeOfHitField, g_mopEntityTypeConst);
                            if (env->ExceptionCheck()) env->ExceptionClear();
                        }
                        env->SetObjectField(g_mcInstance, g_objectMouseOverField, curMop);
                        maybeLogReach(std::string("applied (entityHit patch") + (pointedPatched ? "+pointedEntity" : "") + ") range=" + std::to_string(sampleReach) + " dist=" + std::to_string(std::sqrt(bestDistSq)));
                    } else {
                        env->ExceptionClear();
                    }
                } else {
                    maybeLogReach("entityHit field unresolved");
                }
                env->DeleteLocalRef(curMop);
            } else if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
        }

        if (!env->ExceptionCheck() && mop) {
            env->SetObjectField(g_mcInstance, g_objectMouseOverField, mop);
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(mop);
            maybeLogReach(std::string("applied (mop") + (pointedPatched ? "+pointedEntity" : "") + ") range=" + std::to_string(sampleReach) + " dist=" + std::to_string(std::sqrt(bestDistSq)));
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
            maybeLogReach("failed to build/apply MOP");
        } else if (pointedPatched) {
            maybeLogReach(std::string("applied (pointedEntity only) range=") + std::to_string(sampleReach) + " dist=" + std::to_string(std::sqrt(bestDistSq)));
        }
        env->DeleteLocalRef(bestEntity);
    } else {
        if (g_reachCurrentTarget) {
            env->DeleteGlobalRef(g_reachCurrentTarget);
            g_reachCurrentTarget = nullptr;
        }
        maybeLogReach(std::string("no target in range=") + std::to_string(sampleReach));
    }

    env->DeleteLocalRef(list);
    env->DeleteLocalRef(world);
    env->DeleteLocalRef(player);
}

GameState ReadGameState(JNIEnv* env) {
    GameState s = {}; s.mapped = g_mapped;
    if (!g_mapped || !g_mcInstance) {
        MaybeRefreshMappings(env);
        s.mapped = g_mapped;
        if (!g_mapped || !g_mcInstance) return s;
    }

    MaybeRefreshMappings(env);
    TryResolveWorldMappings(env);

    bool gtbHelperEnabled = false;
    {
        LockGuard lk(g_configMutex);
        gtbHelperEnabled = g_config.gtbHelper;
    }
    TryResolveRenderMappings(env, gtbHelperEnabled);

    if (g_currentScreenField) {
        jobject scr = env->GetObjectField(g_mcInstance, g_currentScreenField);
        s.guiOpen = (scr != nullptr);
        if (scr) {
            jclass c = env->GetObjectClass(scr);
            jclass classClass = env->FindClass("java/lang/Class");
            if (classClass) {
                jmethodID m = env->GetMethodID(classClass, "getSimpleName", "()Ljava/lang/String;");
                if (m) {
                    jstring jn = (jstring)env->CallObjectMethod(c, m);
                    if (jn) {
                        const char* cn = env->GetStringUTFChars(jn, nullptr); 
                        s.screenName = cn;
                        env->ReleaseStringUTFChars(jn, cn);
                        env->DeleteLocalRef(jn);
                    }
                }
                env->DeleteLocalRef(classClass);
            }
            env->DeleteLocalRef(c);
            env->DeleteLocalRef(scr);
        } else {
            s.screenName = "none";
        }
    }
    
        jobject player = env->GetObjectField(g_mcInstance, g_thePlayerField);
        if (player) {
            if (g_getHealthMethod) { s.health = env->CallFloatMethod(player, g_getHealthMethod); if (env->ExceptionCheck()) env->ExceptionClear(); }
            if (g_posXField) s.posX = env->GetDoubleField(player, g_posXField);
            if (g_posYField) s.posY = env->GetDoubleField(player, g_posYField);
            if (g_posZField) s.posZ = env->GetDoubleField(player, g_posZField);
            
            // Check current item
            s.holdingBlock = false;
            if (g_inventoryField) {
                jobject inventory = env->GetObjectField(player, g_inventoryField);
                if (inventory) {
                    if (!g_getCurrentItemMethod) {
                        jclass invClass = env->GetObjectClass(inventory);
                        g_getCurrentItemMethod = env->GetMethodID(invClass, "getCurrentItem", "()Lnet/minecraft/item/ItemStack;");
                        if (!g_getCurrentItemMethod) g_getCurrentItemMethod = env->GetMethodID(invClass, "func_70448_g", "()Lnet/minecraft/item/ItemStack;");
                        if (!g_getCurrentItemMethod) {
                            // Try to find by signature if name fails
                             // Not implemented for now
                        }
                    }
                    
                    if (g_getCurrentItemMethod) {
                        jobject itemStack = env->CallObjectMethod(inventory, g_getCurrentItemMethod);
                        if (itemStack) {
                            jclass stackClass = env->GetObjectClass(itemStack);
                            jmethodID getItem = env->GetMethodID(stackClass, "getItem", "()Lnet/minecraft/item/Item;");
                            if (!getItem) { env->ExceptionClear(); getItem = env->GetMethodID(stackClass, "func_77973_b", "()Lnet/minecraft/item/Item;"); }
                            if (!getItem) env->ExceptionClear();
                            env->DeleteLocalRef(stackClass);
                            
                            if (getItem) {
                                jobject item = env->CallObjectMethod(itemStack, getItem);
                                if (item) {
                                    bool isBlock = false;

                                    // 1. Instance Check (Fastest)
                                    if (g_itemBlockClass && env->IsInstanceOf(item, g_itemBlockClass)) {
                                        isBlock = true;
                                    }

                                    // 2. Name Check (Fallback & Special Items)
                                    if (!isBlock) {
                                        jclass ic = env->GetObjectClass(item);
                                        std::string inm = GetClassNameFromClass(env, ic);
                                        
                                        // Debug Log for Item Class (throttled)
                                        static int itemLogCtr = 0;
                                        if (itemLogCtr++ % 200 == 0) {
                                            Log("Held Item Class: " + inm);
                                        }

                                        if (inm.find("ItemBlock") != std::string::npos || 
                                            inm.find("Block") != std::string::npos ||
                                            inm.find("ItemReed") != std::string::npos || // Sugar Cane
                                            inm.find("ItemRedstone") != std::string::npos || // Redstone Dust
                                            inm.find("ItemSkull") != std::string::npos) { // Heads
                                            isBlock = true; 
                                        }
                                        env->DeleteLocalRef(ic);
                                    }

                                    s.holdingBlock = isBlock;
                                    env->DeleteLocalRef(item);
                                }
                            }
                            env->DeleteLocalRef(itemStack);
                        }
                    }
                    env->DeleteLocalRef(inventory);
                }
            }
            
            env->DeleteLocalRef(player);
    }

    // Check objectMouseOver
    s.lookingAtBlock = false;
    s.lookingAtEntity = false;
    s.lookingAtEntityLatched = false;
    s.breakingBlock = false;
    s.attackCooldown = 1.0f;
    s.attackCooldownPerTick = 0.08f;
    s.stateMs = (unsigned long long)GetTickCount64();
    s.actionBar.clear();
    if (g_objectMouseOverField && g_typeOfHitField && g_enumNameMethod) {
        jobject mop = env->GetObjectField(g_mcInstance, g_objectMouseOverField);
        if (mop) {
            jobject typeOfHit = env->GetObjectField(mop, g_typeOfHitField);
            if (typeOfHit) {
                jstring nameStr = (jstring)env->CallObjectMethod(typeOfHit, g_enumNameMethod);
                if (nameStr && !env->ExceptionCheck()) {
                    const char* nameChars = env->GetStringUTFChars(nameStr, nullptr);
                    if (nameChars) {
                        if (strcmp(nameChars, "BLOCK") == 0) {
                            s.lookingAtBlock = true;
                            bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                            s.breakingBlock = lmbDown;
                        } else if (strcmp(nameChars, "ENTITY") == 0) {
                            s.lookingAtEntity = true;
                            s.lookingAtEntityLatched = true;
                            g_lastEntitySeenMs = s.stateMs;
                        }
                        env->ReleaseStringUTFChars(nameStr, nameChars);
                    }
                    env->DeleteLocalRef(nameStr);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(typeOfHit);
            }
            env->DeleteLocalRef(mop);
        }
    }

    if (!s.lookingAtEntity) {
        unsigned long long nowMs = s.stateMs;
        unsigned long long delta = nowMs - g_lastEntitySeenMs;
        if (delta <= 120ULL) {
            s.lookingAtEntityLatched = true;
        }
    }

    // Read action-bar overlay text (used by GTB helper on C# side).
    if (gtbHelperEnabled && g_ingameGuiField && !g_actionBarFields.empty()) {
        int bestActionBarScore = -1;
        auto tryUseActionBarCandidate = [&](const std::string& cand) {
            if (cand.empty()) return;
            int score = (int)cand.size();
            if (cand.find('_') != std::string::npos) score += 1000;
            std::string lower = cand;
            for (size_t i = 0; i < lower.size(); i++) {
                lower[i] = (char)std::tolower((unsigned char)lower[i]);
            }
            if (lower.find("theme") != std::string::npos) score += 200;
            if (lower.find("guess") != std::string::npos) score += 120;
            if (lower.find("word") != std::string::npos) score += 120;
            if (lower.find("_ _") != std::string::npos || lower.find("__") != std::string::npos) score += 180;
            if (lower.find("record") != std::string::npos) score -= 60;
            if (score > bestActionBarScore) {
                s.actionBar = cand;
                bestActionBarScore = score;
            }
        };

        static std::string s_lastActionBarLog;
        static DWORD s_lastActionBarLogAt = 0;

        jobject ingameGui = env->GetObjectField(g_mcInstance, g_ingameGuiField);
        if (ingameGui && !env->ExceptionCheck()) {
            for (size_t i = 0; i < g_actionBarFields.size(); i++) {
                const ActionBarFieldRef& ref = g_actionBarFields[i];
                if (!ref.field) continue;

                if (ref.kind == ActionBarFieldChatComponent) {
                    if (!g_chatComponentGetTextMethod) continue;
                    jobject chatComp = env->GetObjectField(ingameGui, ref.field);
                    if (!env->ExceptionCheck() && chatComp) {
                        jstring jtxt = (jstring)env->CallObjectMethod(chatComp, g_chatComponentGetTextMethod);
                        if (!env->ExceptionCheck() && jtxt) {
                            const char* c = env->GetStringUTFChars(jtxt, nullptr);
                            if (c) {
                                std::string cand = c;
                                env->ReleaseStringUTFChars(jtxt, c);
                                tryUseActionBarCandidate(cand);
                            }
                            env->DeleteLocalRef(jtxt);
                        } else if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                        }
                        env->DeleteLocalRef(chatComp);
                    } else if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                    }
                } else {
                    jstring jtxt = (jstring)env->GetObjectField(ingameGui, ref.field);
                    if (!env->ExceptionCheck() && jtxt) {
                        const char* c = env->GetStringUTFChars(jtxt, nullptr);
                        if (c) {
                            std::string cand = c;
                            env->ReleaseStringUTFChars(jtxt, c);
                            tryUseActionBarCandidate(cand);
                        }
                        env->DeleteLocalRef(jtxt);
                    } else if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                    }
                }
            }
        } else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        if (ingameGui) env->DeleteLocalRef(ingameGui);

        DWORD nowMs = GetTickCount();
        if ((nowMs - s_lastActionBarLogAt) > 2000) {
            s_lastActionBarLogAt = nowMs;
            if (s.actionBar != s_lastActionBarLog) {
                Log(std::string("ActionBar sample: '") + s.actionBar + "'");
                s_lastActionBarLog = s.actionBar;
            }
        }
    }

    if (env->ExceptionCheck()) env->ExceptionClear();
    return s;
}

// ===================== DETACH =====================
extern "C" __declspec(dllexport) void Detach() {
    Log("Detach requested");
    g_running = false;
    
    // Restore WndProc
    if (g_wndProcHookedHwnd && g_origWndProc) {
        SetWindowLongPtrA(g_wndProcHookedHwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        g_wndProcHookedHwnd = nullptr;
    }
    
    // Restore SwapBuffers (simplified: just unhook IAT if possible, but honestly
    // safely unhooking IAT without race conditions is hard. 
    // For now we just stop rendering logic via g_running flag and let DLL stay loaded but dormant?)
    // A true unload requires FreeLibraryAndExitThread.
    
    // Create a thread to free library safely
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(100);
        FreeLibraryAndExitThread(GetModuleHandleA("bridge.dll"), 0);
        return 0;
    }, nullptr, 0, nullptr);
}

// ===================== FONT & GL RENDERING =====================
void InitFont() {
    HDC hdc = CreateCompatibleDC(NULL);
    BITMAPINFO bmi = {}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = ATLAS_W; bmi.bmiHeader.biHeight = -ATLAS_H;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void* bits; HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(hdc, hBmp);
    HFONT hFont = CreateFontA(30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH | FF_SWISS, "Consolas");
    SelectObject(hdc, hFont);
    SetBkColor(hdc, RGB(0, 0, 0)); SetTextColor(hdc, RGB(255, 255, 255));
    SetTextAlign(hdc, TA_LEFT | TA_TOP);
    for (int i = 0; i < 128; i++) {
        char c = (char)i; if (c < 32) c = ' ';
        int col = i % ATLAS_COLS, row = i / ATLAS_COLS;
        TextOutA(hdc, col * CHAR_W, row * CHAR_H, &c, 1);
    }
    unsigned char* px = new unsigned char[ATLAS_W * ATLAS_H * 4];
    for (int i = 0; i < ATLAS_W * ATLAS_H; i++) {
        unsigned char* s = ((unsigned char*)bits) + i * 4;
        unsigned char bright = s[0] > s[1] ? (s[0] > s[2] ? s[0] : s[2]) : (s[1] > s[2] ? s[1] : s[2]);
        px[i*4] = 255; px[i*4+1] = 255; px[i*4+2] = 255; px[i*4+3] = bright;
    }
    glGenTextures(1, &g_fontTexture);
    glBindTexture(GL_TEXTURE_2D, g_fontTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    // Use linear filtering for better readability at small scales across the client.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    delete[] px; DeleteObject(hBmp); DeleteObject(hFont); DeleteDC(hdc);
    g_glInitialized = true;
    Log("Font texture created (High Res - Consolas)");
}

void DrawRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x+w, y); glVertex2f(x+w, y+h); glVertex2f(x, y+h);
    glEnd();
}

void DrawText2D(float x, float y, const char* text, float r, float g, float b, float a, float scale = 1.0f) {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_fontTexture);
    glColor4f(r, g, b, a);
    float cx = x;
    float cw = CHAR_W * scale, ch = CHAR_H * scale;
    glBegin(GL_QUADS);
    for (int i = 0; text[i]; i++) {
        unsigned char c = text[i];
        if (c < 32 || c > 127) c = '?';
        float tx = (float)(c % ATLAS_COLS) / ATLAS_COLS;
        float ty = (float)(c / ATLAS_COLS) / ATLAS_ROWS;
        float tw = 1.0f / ATLAS_COLS, th = 1.0f / ATLAS_ROWS;
        glTexCoord2f(tx, ty);      glVertex2f(cx, y);
        glTexCoord2f(tx+tw, ty);   glVertex2f(cx+cw, y);
        glTexCoord2f(tx+tw, ty+th);glVertex2f(cx+cw, y+ch);
        glTexCoord2f(tx, ty+th);   glVertex2f(cx, y+ch);
        cx += cw;
    }
    glEnd();
}

float TextWidth(const char* text, float scale = 1.0f) { return strlen(text) * CHAR_W * scale; }

// Text with shadow for readability
void DrawTextShadow(float x, float y, const char* text, float r, float g, float b, float a, float scale = 1.0f) {
    DrawText2D(x - 1, y, text, 0, 0, 0, a * 0.42f, scale);
    DrawText2D(x + 1, y, text, 0, 0, 0, a * 0.42f, scale);
    DrawText2D(x, y - 1, text, 0, 0, 0, a * 0.42f, scale);
    DrawText2D(x, y + 1, text, 0, 0, 0, a * 0.42f, scale);
    DrawText2D(x + 1, y + 1, text, 0, 0, 0, a * 0.55f, scale);
    DrawText2D(x, y, text, r, g, b, a, scale);
}

struct Color3 {
    float r, g, b;
};

float Clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

Color3 HsvToRgb(float h, float s, float v) {
    h = std::fmod(h, 1.0f);
    if (h < 0.0f) h += 1.0f;
    s = Clamp01(s);
    v = Clamp01(v);

    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float m = v - c;

    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    int segment = (int)(h * 6.0f);
    switch (segment) {
    case 0: r1 = c; g1 = x; b1 = 0.0f; break;
    case 1: r1 = x; g1 = c; b1 = 0.0f; break;
    case 2: r1 = 0.0f; g1 = c; b1 = x; break;
    case 3: r1 = 0.0f; g1 = x; b1 = c; break;
    case 4: r1 = x; g1 = 0.0f; b1 = c; break;
    default: r1 = c; g1 = 0.0f; b1 = x; break;
    }

    Color3 out = { r1 + m, g1 + m, b1 + m };
    return out;
}

Color3 AccentColor(float offset = 0.0f) {
    return HsvToRgb(g_uiState.accentHue + offset, g_uiState.accentSat, g_uiState.accentVal);
}

Color3 ChromaTextColor(float offset = 0.0f) {
    if (!g_uiState.chromaText) return AccentColor(0.0f);
    double t = (double)GetTickCount64() / 1000.0;
    float hue = g_uiState.accentHue + (float)(t * g_uiState.chromaSpeed) + offset;
    return HsvToRgb(hue, 0.70f, 0.95f);
}

std::string StripMinecraftFormatting(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        // UTF-8 section sign sequence (§) -> C2 A7, followed by format code char.
        if (c == 0xC2 && i + 2 < in.size() && (unsigned char)in[i + 1] == 0xA7) {
            i += 2;
            continue;
        }
        // Raw section sign byte (§) followed by format code char.
        if (c == 0xA7) {
            if (i + 1 < in.size()) i++;
            continue;
        }
        if (c >= 32 && c <= 126) out.push_back((char)c);
    }
    if (out.size() > 96) out.resize(96);
    return out;
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
        }
        else {
            out.push_back((char)c);
        }
        wasSpace = isSpace;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
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
    std::string name = NormalizeNameSpaces(StripMinecraftFormatting(rawName));
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

static void EnsureGameProfileCaches(JNIEnv* env, jobject anyPlayerObj) {
    if (!env) return;
    if (!g_gameProfileClass) {
        jclass c = env->FindClass("com/mojang/authlib/GameProfile");
        if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
        if (c) {
            g_gameProfileClass = (jclass)env->NewGlobalRef(c);
            env->DeleteLocalRef(c);
        }
    }
    if (g_gameProfileClass && !g_gameProfileGetNameMethod) {
        g_gameProfileGetNameMethod = env->GetMethodID(g_gameProfileClass, "getName", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_gameProfileGetNameMethod = nullptr; }
    }
    if (!g_getGameProfileMethod && anyPlayerObj && g_gameProfileClass) {
        jclass pCls = env->GetObjectClass(anyPlayerObj);
        if (pCls && !env->ExceptionCheck()) {
            g_getGameProfileMethod = env->GetMethodID(pCls, "getGameProfile", "()Lcom/mojang/authlib/GameProfile;");
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_getGameProfileMethod = nullptr; }
            if (!g_getGameProfileMethod) {
                g_getGameProfileMethod = env->GetMethodID(pCls, "func_146103_bH", "()Lcom/mojang/authlib/GameProfile;");
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_getGameProfileMethod = nullptr; }
            }
            env->DeleteLocalRef(pCls);
        }
        else {
            env->ExceptionClear();
        }
    }
}

static std::string GetStablePlayerName(JNIEnv* env, jobject playerObj) {
    if (!env || !playerObj) return "";

    std::string name;
    if (g_getNameMethod) {
        jstring js = (jstring)env->CallObjectMethod(playerObj, g_getNameMethod);
        if (!env->ExceptionCheck() && js) {
            const char* cs = env->GetStringUTFChars(js, nullptr);
            if (cs) {
                name = cs;
                env->ReleaseStringUTFChars(js, cs);
            }
            env->DeleteLocalRef(js);
        }
        else if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }

    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        if (i > 0) s.erase(0, i);
    };

    trim(name);
    std::string cleanDisplay = NormalizeNameSpaces(StripMinecraftFormatting(name));
    if (!LooksLikeFakePlayerLine(cleanDisplay)) return cleanDisplay;

    EnsureGameProfileCaches(env, playerObj);
    if (!g_getGameProfileMethod || !g_gameProfileGetNameMethod) return "";

    jobject gp = env->CallObjectMethod(playerObj, g_getGameProfileMethod);
    if (env->ExceptionCheck()) { env->ExceptionClear(); gp = nullptr; }
    if (!gp) return "";

    jstring js = (jstring)env->CallObjectMethod(gp, g_gameProfileGetNameMethod);
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
    if (!cleanDisplay.empty() && !LooksLikeFakePlayerLine(cleanDisplay)) return cleanDisplay;
    return "";
}

std::string ToLowerAscii(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

struct Color4 {
    float r, g, b, a;
};

static Color4 MakeColor4(int r, int g, int b, int a) {
    Color4 c;
    c.r = (float)r / 255.0f;
    c.g = (float)g / 255.0f;
    c.b = (float)b / 255.0f;
    c.a = (float)a / 255.0f;
    return c;
}

struct OverlayTheme {
    Color4 accentPrimary;
    Color4 accentSecondary;
    Color4 accentTertiary;
    Color4 logoColor;
    Color4 logoShadow;
    Color4 moduleBg;
    Color4 moduleBorder;
    Color4 moduleText;
    Color4 moduleTextShadow;
    Color4 moduleMinimalBg;
    Color4 moduleOutlinedBg;
    Color4 moduleGlassBorder;
    Color4 moduleBoldText;
};

static OverlayTheme ResolveOverlayTheme(const std::string& guiTheme) {
    std::string key = ToLowerAscii(guiTheme);

    if (key == "dark blue") {
        return {
            MakeColor4(59, 130, 246, 255),
            MakeColor4(37, 99, 235, 255),
            MakeColor4(96, 165, 250, 255),
            MakeColor4(125, 195, 255, 255),
            MakeColor4(0, 0, 0, 210),
            MakeColor4(12, 23, 42, 160),
            MakeColor4(148, 163, 184, 70),
            MakeColor4(241, 245, 249, 238),
            MakeColor4(0, 0, 0, 190),
            MakeColor4(12, 23, 42, 100),
            MakeColor4(6, 18, 34, 190),
            MakeColor4(125, 211, 252, 105),
            MakeColor4(255, 255, 255, 245)
        };
    }
    if (key == "crimson") {
        return {
            MakeColor4(244, 63, 94, 255),
            MakeColor4(225, 29, 72, 255),
            MakeColor4(251, 113, 133, 255),
            MakeColor4(255, 145, 165, 255),
            MakeColor4(0, 0, 0, 210),
            MakeColor4(42, 15, 21, 168),
            MakeColor4(251, 113, 133, 80),
            MakeColor4(255, 234, 238, 238),
            MakeColor4(0, 0, 0, 190),
            MakeColor4(42, 15, 21, 108),
            MakeColor4(28, 9, 13, 198),
            MakeColor4(251, 113, 133, 118),
            MakeColor4(255, 245, 247, 245)
        };
    }
    if (key == "emerald") {
        return {
            MakeColor4(16, 185, 129, 255),
            MakeColor4(5, 150, 105, 255),
            MakeColor4(52, 211, 153, 255),
            MakeColor4(86, 255, 199, 255),
            MakeColor4(0, 0, 0, 210),
            MakeColor4(8, 33, 30, 165),
            MakeColor4(74, 222, 128, 70),
            MakeColor4(220, 252, 231, 238),
            MakeColor4(0, 0, 0, 190),
            MakeColor4(8, 33, 30, 105),
            MakeColor4(6, 24, 22, 196),
            MakeColor4(74, 222, 128, 105),
            MakeColor4(240, 253, 244, 245)
        };
    }
    if (key == "amber") {
        return {
            MakeColor4(245, 158, 11, 255),
            MakeColor4(217, 119, 6, 255),
            MakeColor4(251, 191, 36, 255),
            MakeColor4(255, 200, 108, 255),
            MakeColor4(0, 0, 0, 210),
            MakeColor4(35, 23, 4, 170),
            MakeColor4(251, 191, 36, 72),
            MakeColor4(255, 247, 237, 238),
            MakeColor4(0, 0, 0, 190),
            MakeColor4(35, 23, 4, 115),
            MakeColor4(23, 14, 3, 200),
            MakeColor4(251, 191, 36, 112),
            MakeColor4(255, 251, 235, 245)
        };
    }
    if (key == "slate") {
        return {
            MakeColor4(148, 163, 184, 255),
            MakeColor4(100, 116, 139, 255),
            MakeColor4(203, 213, 225, 255),
            MakeColor4(190, 202, 220, 255),
            MakeColor4(0, 0, 0, 210),
            MakeColor4(16, 20, 24, 170),
            MakeColor4(148, 163, 184, 72),
            MakeColor4(241, 245, 249, 238),
            MakeColor4(0, 0, 0, 190),
            MakeColor4(16, 20, 24, 110),
            MakeColor4(10, 13, 17, 200),
            MakeColor4(203, 213, 225, 106),
            MakeColor4(255, 255, 255, 245)
        };
    }
    if (key == "sunset") {
        return {
            MakeColor4(251, 113, 133, 255),
            MakeColor4(219, 39, 119, 255),
            MakeColor4(244, 114, 182, 255),
            MakeColor4(255, 154, 173, 255),
            MakeColor4(0, 0, 0, 210),
            MakeColor4(33, 17, 37, 170),
            MakeColor4(244, 114, 182, 76),
            MakeColor4(253, 242, 248, 238),
            MakeColor4(0, 0, 0, 190),
            MakeColor4(33, 17, 37, 110),
            MakeColor4(20, 10, 22, 200),
            MakeColor4(244, 114, 182, 112),
            MakeColor4(255, 245, 250, 245)
        };
    }
    if (key == "frost") {
        return {
            MakeColor4(56, 189, 248, 255),
            MakeColor4(14, 165, 233, 255),
            MakeColor4(125, 211, 252, 255),
            MakeColor4(147, 224, 255, 255),
            MakeColor4(0, 0, 0, 210),
            MakeColor4(13, 22, 36, 166),
            MakeColor4(125, 211, 252, 76),
            MakeColor4(224, 242, 254, 238),
            MakeColor4(0, 0, 0, 190),
            MakeColor4(13, 22, 36, 108),
            MakeColor4(7, 14, 26, 198),
            MakeColor4(125, 211, 252, 110),
            MakeColor4(240, 249, 255, 245)
        };
    }

    return {
        MakeColor4(167, 125, 255, 255),
        MakeColor4(124, 101, 176, 255),
        MakeColor4(196, 181, 253, 255),
        MakeColor4(155, 220, 255, 255),
        MakeColor4(0, 0, 0, 210),
        MakeColor4(0, 0, 0, 145),
        MakeColor4(255, 255, 255, 44),
        MakeColor4(255, 255, 255, 235),
        MakeColor4(0, 0, 0, 180),
        MakeColor4(0, 0, 0, 90),
        MakeColor4(0, 0, 0, 185),
        MakeColor4(255, 255, 255, 90),
        MakeColor4(255, 255, 255, 245)
    };
}

float SwordDamageFromUnlocalizedName(const std::string& unlocLower) {
    if (unlocLower.find("sword") == std::string::npos) return 0.0f;
    if (unlocLower.find("stone") != std::string::npos) return 5.0f;
    if (unlocLower.find("iron") != std::string::npos) return 6.0f;
    if (unlocLower.find("diamond") != std::string::npos) return 7.0f;
    if (unlocLower.find("wood") != std::string::npos || unlocLower.find("gold") != std::string::npos) return 4.0f;
    return 0.0f;
}

std::string NormalizeSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool wasSpace = false;
    for (char ch : s) {
        bool isSpace = std::isspace((unsigned char)ch) != 0;
        if (isSpace) {
            if (!wasSpace) out.push_back(' ');
        }
        else {
            out.push_back(ch);
        }
        wasSpace = isSpace;
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool ContainsItemKeyword(const std::string& lower) {
    return lower.find("sword") != std::string::npos ||
        lower.find("axe") != std::string::npos ||
        lower.find("pick") != std::string::npos ||
        lower.find("bow") != std::string::npos ||
        lower.find("rod") != std::string::npos ||
        lower.find("potion") != std::string::npos ||
        lower.find("pearl") != std::string::npos ||
        lower.find("block") != std::string::npos;
}

std::string CleanupHeldBaseName(const std::string& rawName) {
    std::string clean = NormalizeSpaces(rawName);
    if (clean.empty()) return clean;

    std::string lower = ToLowerAscii(clean);

    // Common leftover from MC formatting removal: leading single color/style code letter.
    while (clean.size() >= 2) {
        char first = (char)std::tolower((unsigned char)clean[0]);
        unsigned char second = (unsigned char)clean[1];
        bool looksLikeCode = ((first >= '0' && first <= '9') || (first >= 'a' && first <= 'f') || (first >= 'k' && first <= 'o') || first == 'r');
        if (looksLikeCode && (std::isupper(second) || second == '[' || second == '(')) {
            clean.erase(clean.begin());
            clean = NormalizeSpaces(clean);
        }
        else {
            break;
        }
    }

    size_t possessive = lower.find("'s ");
    if (possessive != std::string::npos && possessive < 20) {
        std::string right = clean.substr(possessive + 3);
        if (ContainsItemKeyword(ToLowerAscii(right))) {
            clean = right;
            lower = ToLowerAscii(clean);
        }
    }

    while (!clean.empty() && !std::isalnum((unsigned char)clean.front())) {
        clean.erase(clean.begin());
    }

    // Strip short numeric junk prefixes like "7(" or "3 -".
    while (!clean.empty() && std::isdigit((unsigned char)clean.front())) {
        size_t p = 1;
        while (p < clean.size() && !std::isalpha((unsigned char)clean[p])) p++;
        if (p > 0 && p < clean.size() && std::isalpha((unsigned char)clean[p])) {
            clean.erase(0, p);
            while (!clean.empty() && !std::isalnum((unsigned char)clean.front())) clean.erase(clean.begin());
        }
        else {
            break;
        }
    }

    return NormalizeSpaces(clean);
}

int RomanValue(char c) {
    switch (std::tolower((unsigned char)c)) {
    case 'i': return 1;
    case 'v': return 5;
    case 'x': return 10;
    case 'l': return 50;
    case 'c': return 100;
    case 'd': return 500;
    case 'm': return 1000;
    default: return 0;
    }
}

int ParseRoman(const std::string& token) {
    if (token.empty()) return 0;
    int total = 0;
    int prev = 0;
    for (int i = (int)token.size() - 1; i >= 0; --i) {
        int v = RomanValue(token[(size_t)i]);
        if (v == 0) return 0;
        if (v < prev) total -= v;
        else total += v;
        prev = v;
    }
    return total;
}

int ExtractSharpnessLevel(const std::string& lowerText) {
    size_t sharp = lowerText.find("sharpness");
    if (sharp != std::string::npos) {
        size_t p = sharp + 9;
        while (p < lowerText.size() && !std::isalnum((unsigned char)lowerText[p])) p++;
        if (p < lowerText.size()) {
            if (std::isdigit((unsigned char)lowerText[p])) {
                int lvl = 0;
                while (p < lowerText.size() && std::isdigit((unsigned char)lowerText[p])) {
                    lvl = lvl * 10 + (lowerText[p] - '0');
                    p++;
                }
                if (lvl > 0) return lvl;
            }
            else {
                size_t start = p;
                while (p < lowerText.size() && std::isalpha((unsigned char)lowerText[p])) p++;
                int roman = ParseRoman(lowerText.substr(start, p - start));
                if (roman > 0) return roman;
            }
        }
    }

    size_t swordPos = lowerText.find("sword");
    if (swordPos != std::string::npos) {
        size_t p = swordPos + 5;
        while (p < lowerText.size() && lowerText[p] == ' ') p++;
        if (p < lowerText.size() && std::isdigit((unsigned char)lowerText[p])) {
            int lvl = 0;
            while (p < lowerText.size() && std::isdigit((unsigned char)lowerText[p])) {
                lvl = lvl * 10 + (lowerText[p] - '0');
                p++;
            }
            if (lvl > 0) return lvl;
        }
    }

    return 0;
}

std::string BuildCappedHeldText(const std::string& rawName, float swordDmg) {
    std::string base = CleanupHeldBaseName(rawName);
    if (base.empty()) base = "Item";

    const int maxTotal = 36;
    std::string suffix;
    if (swordDmg > 0.0f) {
        char dmgBuf[24];
        snprintf(dmgBuf, sizeof(dmgBuf), " (%.1f dmg)", swordDmg);
        suffix = dmgBuf;
    }

    int allowedBase = maxTotal - (int)suffix.size();
    if (allowedBase < 8) allowedBase = 8;
    if ((int)base.size() > allowedBase) {
        if (allowedBase > 3) base = base.substr(0, (size_t)(allowedBase - 3)) + "...";
        else base = base.substr(0, (size_t)allowedBase);
    }

    return base + suffix;
}

void DrawHeldItemIcon(float x, float y, float size, const std::string& iconCode, float alpha) {
    DrawRect(x - 0.8f, y - 0.8f, size + 1.6f, size + 1.6f, 0.0f, 0.0f, 0.0f, 0.55f * alpha);
    DrawRect(x, y, size, size, 0.14f, 0.14f, 0.16f, 0.95f * alpha);
    DrawRect(x, y, size, 1.0f, 0.72f, 0.90f, 1.0f, 0.90f * alpha);

    if (iconCode == "SW") {
        DrawRect(x + size * 0.58f, y + size * 0.12f, size * 0.10f, size * 0.58f, 0.85f, 0.87f, 0.91f, alpha);
        DrawRect(x + size * 0.47f, y + size * 0.56f, size * 0.32f, size * 0.10f, 0.72f, 0.56f, 0.35f, alpha);
        DrawRect(x + size * 0.59f, y + size * 0.67f, size * 0.08f, size * 0.20f, 0.62f, 0.44f, 0.26f, alpha);
    }
    else if (iconCode == "PK") {
        DrawRect(x + size * 0.18f, y + size * 0.23f, size * 0.64f, size * 0.10f, 0.80f, 0.83f, 0.88f, alpha);
        DrawRect(x + size * 0.46f, y + size * 0.30f, size * 0.10f, size * 0.48f, 0.68f, 0.50f, 0.31f, alpha);
    }
    else if (iconCode == "AX") {
        DrawRect(x + size * 0.48f, y + size * 0.20f, size * 0.10f, size * 0.58f, 0.66f, 0.48f, 0.29f, alpha);
        DrawRect(x + size * 0.30f, y + size * 0.22f, size * 0.26f, size * 0.20f, 0.80f, 0.84f, 0.89f, alpha);
    }
    else if (iconCode == "BW") {
        DrawRect(x + size * 0.26f, y + size * 0.14f, size * 0.08f, size * 0.72f, 0.67f, 0.50f, 0.30f, alpha);
        DrawRect(x + size * 0.62f, y + size * 0.18f, size * 0.04f, size * 0.64f, 0.84f, 0.86f, 0.90f, alpha);
    }
    else if (iconCode == "BL") {
        DrawRect(x + size * 0.26f, y + size * 0.26f, size * 0.48f, size * 0.48f, 0.46f, 0.70f, 0.45f, alpha);
        DrawRect(x + size * 0.30f, y + size * 0.30f, size * 0.40f, size * 0.40f, 0.34f, 0.56f, 0.34f, alpha);
    }
    else {
        DrawRect(x + size * 0.33f, y + size * 0.33f, size * 0.34f, size * 0.34f, 0.78f, 0.80f, 0.86f, alpha);
    }
}

bool DrawMinecraftHeldItemIcon(JNIEnv* env, jobject heldStack, float x, float y, float size, float alpha) {
    // DISABLED: Calling Minecraft's renderItemAndEffectIntoGUI from SwapBuffers hook causes
    // state corruption and crashes. Minecraft's item renderer expects to be called from
    // within the normal render pipeline, not from an external overlay context.
    // The GL state (framebuffers, textures, matrices) cannot be safely isolated.
    return false;
}

std::string DetermineHeldIconCode(const std::string& lowerText, bool isBlock) {
    if (isBlock) return "BL";
    if (lowerText.find("sword") != std::string::npos) return "SW";
    if (lowerText.find("pick") != std::string::npos) return "PK";
    if (lowerText.find("axe") != std::string::npos) return "AX";
    if (lowerText.find("bow") != std::string::npos) return "BW";
    if (lowerText.find("rod") != std::string::npos) return "RD";
    if (lowerText.find("potion") != std::string::npos) return "PT";
    if (lowerText.find("pearl") != std::string::npos) return "EP";
    return "IT";
}

jobject GetEntityHeldItemStack(JNIEnv* env, jobject entity) {
    if (!env || !entity || !g_getHeldItemMethod) return nullptr;
    jobject heldStack = env->CallObjectMethod(entity, g_getHeldItemMethod);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return heldStack;
}

std::string GetHeldItemInfoFromStack(JNIEnv* env, jobject heldStack, std::string* iconCodeOut) {
    if (!env || !heldStack) return "";

    std::string heldRawName;
    bool isBlock = false;
    std::string unlocalizedLower;
    float itemBaseDamage = 0.0f;

    // --- ItemStack.getDisplayName() ---
    // Look up fresh from this stack's actual class every call.
    {
        jclass stackClass = env->GetObjectClass(heldStack);
        jmethodID getDisplayName = env->GetMethodID(stackClass, "getDisplayName", "()Ljava/lang/String;");
        if (!getDisplayName) {
            env->ExceptionClear();
            getDisplayName = env->GetMethodID(stackClass, "func_82833_r", "()Ljava/lang/String;");
        }
        if (!getDisplayName) env->ExceptionClear();
        env->DeleteLocalRef(stackClass);

        if (getDisplayName) {
            jstring heldName = (jstring)env->CallObjectMethod(heldStack, getDisplayName);
            if (!env->ExceptionCheck() && heldName) {
                const char* heldChars = env->GetStringUTFChars(heldName, nullptr);
                if (heldChars) {
                    heldRawName = StripMinecraftFormatting(heldChars);
                    env->ReleaseStringUTFChars(heldName, heldChars);
                }
                env->DeleteLocalRef(heldName);
            } else if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
        }
    }

    // --- ItemStack.getItem() -> Item ---
    {
        jclass stackClass2 = env->GetObjectClass(heldStack);
        jmethodID getItem = env->GetMethodID(stackClass2, "getItem", "()Lnet/minecraft/item/Item;");
        if (!getItem) {
            env->ExceptionClear();
            getItem = env->GetMethodID(stackClass2, "func_77973_b", "()Lnet/minecraft/item/Item;");
        }
        if (!getItem) env->ExceptionClear();
        env->DeleteLocalRef(stackClass2);

        if (getItem) {
            jobject heldItem = env->CallObjectMethod(heldStack, getItem);
            if (!env->ExceptionCheck() && heldItem) {
                isBlock = (g_itemBlockClass && env->IsInstanceOf(heldItem, g_itemBlockClass));

                jclass itemClass = env->GetObjectClass(heldItem);

                // --- Item.getUnlocalizedName() ---
                jmethodID getUnloc = env->GetMethodID(itemClass, "getUnlocalizedName", "()Ljava/lang/String;");
                if (!getUnloc) {
                    env->ExceptionClear();
                    getUnloc = env->GetMethodID(itemClass, "func_77658_a", "()Ljava/lang/String;");
                }
                if (!getUnloc) env->ExceptionClear();
                if (getUnloc) {
                    jstring unloc = (jstring)env->CallObjectMethod(heldItem, getUnloc);
                    if (!env->ExceptionCheck() && unloc) {
                        const char* u = env->GetStringUTFChars(unloc, nullptr);
                        if (u) { unlocalizedLower = ToLowerAscii(u); env->ReleaseStringUTFChars(unloc, u); }
                        env->DeleteLocalRef(unloc);
                    } else if (env->ExceptionCheck()) { env->ExceptionClear(); }
                }

                // --- Item.getDamageVsEntity() ---
                jmethodID getDmg = env->GetMethodID(itemClass, "getDamageVsEntity", "()F");
                if (!getDmg) {
                    env->ExceptionClear();
                    getDmg = env->GetMethodID(itemClass, "func_150931_i", "()F");
                }
                if (!getDmg) env->ExceptionClear();
                if (getDmg) {
                    float rawDmg = env->CallFloatMethod(heldItem, getDmg);
                    if (!env->ExceptionCheck() && rawDmg > 0.0f && rawDmg < 20.0f)
                        itemBaseDamage = rawDmg;
                    else if (env->ExceptionCheck()) env->ExceptionClear();
                }

                env->DeleteLocalRef(itemClass);
                env->DeleteLocalRef(heldItem);
            } else if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
        }
    }

    float finalSwordDamage = 0.0f;
    std::string lowerHeldName = ToLowerAscii(heldRawName);
    bool isSwordName = lowerHeldName.find("sword") != std::string::npos;
    if (isSwordName) {
        finalSwordDamage = SwordDamageFromUnlocalizedName(unlocalizedLower);
        if (finalSwordDamage <= 0.0f && itemBaseDamage > 0.0f) finalSwordDamage = itemBaseDamage;

        int sharpLevel = ExtractSharpnessLevel(lowerHeldName);
        if (sharpLevel > 0) {
            finalSwordDamage += 1.25f * (float)sharpLevel;
        }
    }

    std::string heldText = BuildCappedHeldText(heldRawName, finalSwordDamage);

    if (iconCodeOut) {
        std::string iconSrc = !unlocalizedLower.empty() ? unlocalizedLower : ToLowerAscii(heldText);
        *iconCodeOut = DetermineHeldIconCode(iconSrc, isBlock);
    }

    return heldText;
}

std::string GetEntityHeldItemInfo(JNIEnv* env, jobject entity, std::string* iconCodeOut) {
    jobject heldStack = GetEntityHeldItemStack(env, entity);
    if (!heldStack) return "";
    std::string out = GetHeldItemInfoFromStack(env, heldStack, iconCodeOut);
    env->DeleteLocalRef(heldStack);
    return out;
}

std::string RelativeDirectionText(float localYawDeg, double toX, double toZ) {
    const double radToDeg = 57.29577951308232;
    float targetYaw = (float)(std::atan2(-toX, toZ) * radToDeg);
    float delta = targetYaw - localYawDeg;
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;

    float ad = std::fabs(delta);
    if (ad <= 32.0f) return "Front";
    if (ad >= 148.0f) return "Back";
    return delta < 0.0f ? "Left" : "Right";
}

// ===================== HUD RENDERING =====================
void RenderHUD(int winW, int winH) {
    if (g_guiOpen) return; // hide HUD when config is open

    Config cfg; { LockGuard lk(g_configMutex); cfg = g_config; }

    GameState state;
    { LockGuard lk(g_stateMutex); state = g_gameState; }
    bool isChatScreen = false;
    if (!state.screenName.empty()) {
        isChatScreen = state.screenName.find("GuiChat") != std::string::npos
            || state.screenName.find("ChatScreen") != std::string::npos
            || state.screenName.find("class_408") != std::string::npos;
    }
    if (state.guiOpen && !isChatScreen) {
        return;
    }

    if (!cfg.showModuleList) {
        return;
    }

    OverlayTheme theme = ResolveOverlayTheme(cfg.guiTheme);

    struct ModLine {
        std::string text;
        Color4 accent;
        float width;
    };

    std::vector<ModLine> mods;
    mods.reserve(16);
    auto pushMod = [&](const std::string& text, const Color4& accent) {
        if (text.empty()) return;
        mods.push_back({ text, accent, TextWidth(text.c_str(), 0.58f) });
    };

    if (cfg.armed) {
        char acBuf[64];
        int lo = (int)cfg.minCPS;
        int hi = (int)cfg.maxCPS;
        if (hi < lo) std::swap(hi, lo);
        snprintf(acBuf, sizeof(acBuf), "Autoclicker %d-%d", lo, hi);
        pushMod(acBuf, theme.accentPrimary);
    }
    if (cfg.clickInChests)    pushMod("Click in Chests", theme.accentTertiary);
    if (cfg.closestPlayerInfo) pushMod("Closest Player", theme.accentSecondary);
    if (cfg.rightClick)       pushMod("Rightclick", theme.accentTertiary);
    if (cfg.aimAssist)        pushMod("Aim Assist", theme.accentPrimary);
    if (cfg.chestEsp)         pushMod("Chest ESP", theme.accentSecondary);
    if (cfg.nametags)         pushMod("Nametags", theme.accentPrimary);
    if (cfg.gtbHelper)        pushMod("GTB Helper", theme.accentTertiary);
    if (cfg.jitter)           pushMod("Jitter", theme.accentSecondary);
    if (cfg.breakBlocks)      pushMod("Break Blocks", theme.moduleText);
    if (cfg.reachEnabled)     pushMod("Reach", theme.accentPrimary);
    if (cfg.velocityEnabled)  pushMod("Velocity", theme.accentTertiary);

    std::sort(mods.begin(), mods.end(), [](const ModLine& a, const ModLine& b) {
        if (a.width != b.width) return a.width > b.width;
        return a.text < b.text;
    });

    const float scale = 0.58f;
    const float marginX = 10.0f;
    float y = 10.0f;

    if (cfg.showLogo) {
        const char* logoText = "LegoClicker";
        float logoW = TextWidth(logoText, scale);
        float logoX = (float)winW - marginX - logoW;
        DrawTextShadow(logoX, y, logoText, theme.logoColor.r, theme.logoColor.g, theme.logoColor.b, theme.logoColor.a, scale);
        y += CHAR_H * scale + 8.0f;
    }

    const float padX = 8.0f;
    const float padY = 3.0f;
    const float barW = 3.0f;
    const float gapY = 2.0f;
    const float fontH = CHAR_H * scale;
    const int style = (std::max)(0, (std::min)(4, cfg.moduleListStyle));

    for (size_t i = 0; i < mods.size(); i++) {
        const ModLine& m = mods[i];
        float textW = TextWidth(m.text.c_str(), scale);
        float boxW = barW + padX + textW + padX;
        float boxH = padY + fontH + padY;
        float x0 = (float)winW - marginX - boxW;
        float x1 = (float)winW - marginX;
        float y0 = y;
        float y1 = y + boxH;

        if (style == 0) {
            DrawRect(x0, y0, boxW, boxH, theme.moduleBg.r, theme.moduleBg.g, theme.moduleBg.b, theme.moduleBg.a);
            DrawRect(x0, y0, barW, boxH, m.accent.r, m.accent.g, m.accent.b, m.accent.a);
            DrawRect(x0, y0, boxW, 1.0f, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
            DrawRect(x0, y1 - 1.0f, boxW, 1.0f, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
            DrawRect(x0, y0, 1.0f, boxH, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
            DrawRect(x1 - 1.0f, y0, 1.0f, boxH, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
            DrawTextShadow(x0 + barW + padX, y0 + padY, m.text.c_str(), theme.moduleText.r, theme.moduleText.g, theme.moduleText.b, theme.moduleText.a, scale);
        } else if (style == 1) {
            float minW = textW + 4.0f;
            DrawRect(x1 - minW, y0, minW, boxH, theme.moduleMinimalBg.r, theme.moduleMinimalBg.g, theme.moduleMinimalBg.b, theme.moduleMinimalBg.a);
            DrawRect(x1 - 2.0f, y0, 2.0f, boxH, m.accent.r, m.accent.g, m.accent.b, m.accent.a);
            DrawTextShadow(x1 - textW - 2.0f, y0 + padY, m.text.c_str(), m.accent.r, m.accent.g, m.accent.b, m.accent.a, scale);
        } else if (style == 2) {
            DrawRect(x0, y0, boxW, boxH, theme.moduleOutlinedBg.r, theme.moduleOutlinedBg.g, theme.moduleOutlinedBg.b, theme.moduleOutlinedBg.a);
            DrawRect(x0, y0, boxW, 1.5f, m.accent.r, m.accent.g, m.accent.b, m.accent.a);
            DrawRect(x0, y1 - 1.5f, boxW, 1.5f, m.accent.r, m.accent.g, m.accent.b, m.accent.a);
            DrawRect(x0, y0, 1.5f, boxH, m.accent.r, m.accent.g, m.accent.b, m.accent.a);
            DrawRect(x1 - 1.5f, y0, 1.5f, boxH, m.accent.r, m.accent.g, m.accent.b, m.accent.a);
            DrawTextShadow(x0 + barW + padX, y0 + padY, m.text.c_str(), m.accent.r, m.accent.g, m.accent.b, m.accent.a, scale);
        } else if (style == 3) {
            DrawRect(x0, y0, boxW, boxH, theme.moduleMinimalBg.r, theme.moduleMinimalBg.g, theme.moduleMinimalBg.b, theme.moduleMinimalBg.a);
            DrawRect(x0, y0, boxW, 1.0f, theme.moduleGlassBorder.r, theme.moduleGlassBorder.g, theme.moduleGlassBorder.b, theme.moduleGlassBorder.a);
            DrawRect(x0, y1 - 1.0f, boxW, 1.0f, theme.moduleGlassBorder.r, theme.moduleGlassBorder.g, theme.moduleGlassBorder.b, theme.moduleGlassBorder.a);
            DrawRect(x0, y0, 1.0f, boxH, theme.moduleGlassBorder.r, theme.moduleGlassBorder.g, theme.moduleGlassBorder.b, theme.moduleGlassBorder.a);
            DrawRect(x1 - 1.0f, y0, 1.0f, boxH, theme.moduleGlassBorder.r, theme.moduleGlassBorder.g, theme.moduleGlassBorder.b, theme.moduleGlassBorder.a);
            DrawRect(x0 + 1.0f, y0 + 1.0f, barW, boxH - 2.0f, m.accent.r, m.accent.g, m.accent.b, m.accent.a);
            DrawTextShadow(x0 + barW + padX, y0 + padY, m.text.c_str(), theme.moduleText.r, theme.moduleText.g, theme.moduleText.b, theme.moduleText.a, scale);
        } else {
            DrawRect(x0, y0, boxW, boxH, m.accent.r, m.accent.g, m.accent.b, m.accent.a);
            DrawRect(x0, y0, boxW, 1.0f, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
            DrawRect(x0, y1 - 1.0f, boxW, 1.0f, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
            DrawRect(x0, y0, 1.0f, boxH, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
            DrawRect(x1 - 1.0f, y0, 1.0f, boxH, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
            DrawTextShadow(x0 + barW + padX, y0 + padY, m.text.c_str(), theme.moduleBoldText.r, theme.moduleBoldText.g, theme.moduleBoldText.b, theme.moduleBoldText.a, scale);
        }

        y += boxH + gapY;
    }

    if (cfg.gtbHelper && !cfg.gtbHint.empty()) {
        std::vector<std::string> lines;
        std::stringstream pss(cfg.gtbPreview);
        std::string line;
        while (std::getline(pss, line, ',')) {
            while (!line.empty() && std::isspace((unsigned char)line.front())) line.erase(line.begin());
            while (!line.empty() && std::isspace((unsigned char)line.back())) line.pop_back();
            if (!line.empty()) lines.push_back(line);
        }

        const size_t maxPreviewLines = 25;
        if (lines.size() > maxPreviewLines) {
            lines.resize(maxPreviewLines);
        }

        float textScale = 0.52f;
        float lineH = CHAR_H * textScale + 2.0f;
        float panelW = 180.0f;
        float panelH = 12.0f + lineH;
        panelH += lineH * (float)lines.size();

        float x1 = (float)winW - 10.0f;
        float y1 = (float)winH - 10.0f;
        float x0 = x1 - panelW;
        float y0 = y1 - panelH;
        if (x0 < 10.0f) x0 = 10.0f;
        if (y0 < 10.0f) y0 = 10.0f;

        float maxPanelH = (float)winH - 20.0f;
        if (panelH > maxPanelH) {
            panelH = maxPanelH;
        }

        size_t maxVisibleLines = 0;
        if (lineH > 0.0f && panelH > 12.0f + lineH) {
            float visible = (panelH - (12.0f + lineH)) / lineH;
            if (visible > 0.0f) {
                maxVisibleLines = (size_t)visible;
            }
        }
        if (maxVisibleLines < 1) {
            maxVisibleLines = 1;
        }
        if (lines.size() > maxVisibleLines) {
            lines.resize(maxVisibleLines);
        }

        DrawRect(x0, y0, panelW, panelH, theme.moduleBg.r, theme.moduleBg.g, theme.moduleBg.b, 0.92f);
        DrawRect(x0, y0, panelW, 1.0f, theme.accentPrimary.r, theme.accentPrimary.g, theme.accentPrimary.b, 0.95f);
        DrawRect(x0, y0 + panelH - 1.0f, panelW, 1.0f, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
        DrawRect(x0, y0, 1.0f, panelH, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);
        DrawRect(x0 + panelW - 1.0f, y0, 1.0f, panelH, theme.moduleBorder.r, theme.moduleBorder.g, theme.moduleBorder.b, theme.moduleBorder.a);

        char hintBuf[320];
        snprintf(hintBuf, sizeof(hintBuf), "GTB: %s (%d)", cfg.gtbHint.c_str(), (std::max)(0, cfg.gtbCount));
        DrawTextShadow(x0 + 8.0f, y0 + 6.0f, hintBuf,
            theme.moduleText.r, theme.moduleText.g, theme.moduleText.b, theme.moduleText.a, textScale);

        float ty = y0 + 6.0f + lineH;
        for (const std::string& raw : lines) {
            std::string row = "- " + raw;
            DrawTextShadow(x0 + 9.0f, ty, row.c_str(),
                theme.accentTertiary.r, theme.accentTertiary.g, theme.accentTertiary.b, 0.95f, textScale);
            ty += lineH;
        }
    }
}

// ===================== NAMETAGS =====================
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct LegoVec3 { double x, y, z; };

bool WorldToScreen(LegoVec3 pos, LegoVec3 cam, float yaw, float pitch, float fov, int winW, int winH, float* sx, float* sy) {
    // Relative position
    double dx = pos.x - cam.x;
    double dy = pos.y - cam.y;
    double dz = pos.z - cam.z;

    // Convert to radians
    float yawRad = yaw * (float)M_PI / 180.0f;
    float pitchRad = pitch * (float)M_PI / 180.0f;

    // Rotate around Y (Yaw)
    // Note: MC Yaw 0 = +Z (South). We want to align with -Z (Forward in OpenGL view).
    // The rotation formula must match MC's coordinate system.
    // Let's use the standard rotation that works for most ESPs:
    double x1 = dx * std::cos(yawRad) + dz * std::sin(yawRad); 
    double z1 = dz * std::cos(yawRad) - dx * std::sin(yawRad); 
    
    // Rotate 180 degrees to align with camera forward
    double x2 = -x1; 
    double z2 = -z1;

    // Rotate around X (Pitch)
    double y3 = dy * std::cos(pitchRad) + z2 * std::sin(pitchRad);
    double z3 = z2 * std::cos(pitchRad) - dy * std::sin(pitchRad);

    // In this transformed space, -Z is forward.
    // If z3 > 0, it is behind the camera.
    if (z3 > 0) return false;

    // Projection
    float aspect = (float)winW / (float)winH;
    float fovRad = fov * (float)M_PI / 180.0f;
    
    // Perspective division
    // screenX = x / -z * scale
    // scale = height / (2 * tan(fov/2))
    double dist = -z3;
    if (dist < 0.1) return false;

    double scale = (double)winH / (2.0 * std::tan(fovRad / 2.0));
    
    double screenX = x2 / dist * scale;
    double screenY = y3 / dist * scale;

    *sx = (float)winW / 2.0f - (float)screenX; 
    *sy = (float)winH / 2.0f + (float)screenY; 
    
    return true;
}

struct ScopedJNIEnv {
    JNIEnv* env;
    bool attached;
    JavaVM* jvm;

    ScopedJNIEnv(JavaVM* vm) : jvm(vm), env(nullptr), attached(false) {
        if (vm->GetEnv((void**)&env, JNI_VERSION_1_8) != JNI_OK) {
            vm->AttachCurrentThread((void**)&env, nullptr);
            attached = true;
        }
    }

    ~ScopedJNIEnv() {
        if (attached && jvm) {
            jvm->DetachCurrentThread();
        }
    }

    operator JNIEnv*() const { return env; }
    JNIEnv* operator->() const { return env; }
};

Matrix4x4 GetMatrix(JNIEnv* env, jfieldID field) {
    Matrix4x4 m = {0};
    if (!env || !g_activeRenderInfoClass || !field) return m;
    
    jobject floatBuffer = env->GetStaticObjectField(g_activeRenderInfoClass, field);
    if (!floatBuffer) return m;

    // Revert to safe path to prevent crashes/garbage data
    if (g_floatBufferGet) {
        for (int i = 0; i < 16; i++) {
            m.m[i] = env->CallFloatMethod(floatBuffer, g_floatBufferGet, i);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                env->DeleteLocalRef(floatBuffer);
                Matrix4x4 zero = {0};
                return zero;
            }
        }
    }
    
    env->DeleteLocalRef(floatBuffer);
    return m;
}

bool WorldToScreen(const double x, const double y, const double z, const Matrix4x4& view, const Matrix4x4& proj, int w, int h, float& outX, float& outY) {
    // 1. View Transformation
    float vX = x * view.m[0] + y * view.m[4] + z * view.m[8] + view.m[12];
    float vY = x * view.m[1] + y * view.m[5] + z * view.m[9] + view.m[13];
    float vZ = x * view.m[2] + y * view.m[6] + z * view.m[10] + view.m[14];
    float vW = x * view.m[3] + y * view.m[7] + z * view.m[11] + view.m[15];

    // 2. Project Transformation
    float pX = vX * proj.m[0] + vY * proj.m[4] + vZ * proj.m[8] + proj.m[12];
    float pY = vX * proj.m[1] + vY * proj.m[5] + vZ * proj.m[9] + proj.m[13];
    float pZ = vX * proj.m[2] + vY * proj.m[6] + vZ * proj.m[10] + proj.m[14];
    float pW = vX * proj.m[3] + vY * proj.m[7] + vZ * proj.m[11] + proj.m[15];

    if (pW < 0.02f) return false; // Behind camera

    // 3. Perspective Divide
    float ndcX = pX / pW;
    float ndcY = pY / pW;

    // 4. Viewport Map
    outX = (ndcX + 1.0f) * 0.5f * w;
    outY = (1.0f - ndcY) * 0.5f * h;

    return true;
}
void RenderNametags(int w, int h) {
   static bool warnedMissingMappings = false;
    bool showHealth = true;
    bool showArmor = true;
    bool nametagsEnabled = false;
    bool entityTelemetryNeeded = false;
    int nametagMaxCount = 8;
    {
         LockGuard lk(g_configMutex);
         nametagsEnabled = g_config.nametags;
         entityTelemetryNeeded = g_config.nametags || g_config.closestPlayerInfo || g_config.aimAssist;
         if (!entityTelemetryNeeded) return;
         showHealth = g_config.nametagShowHealth;
         showArmor = g_config.nametagShowArmor;
         nametagMaxCount = (std::max)(1, (std::min)(20, g_config.nametagMaxCount));
    }

    // Default to empty telemetry each run so stale entities are not reused.
    {
        LockGuard lk(g_jsonMutex);
        g_pendingJson = "[]";
    }
   static unsigned int dbgTick = 0;

   if (!g_mapped || !g_mcInstance) return;
   if (!g_theWorldField || !g_listSizeMethod || !g_listGetMethod ||
       !g_thePlayerField || !g_posXField || !g_posYField || !g_posZField) {
       if (!warnedMissingMappings) {
           warnedMissingMappings = true;
           Log("Nametags missing core JNI mappings.");
       }
       return;
   }

    ScopedJNIEnv env(g_jvm);
    if (!env) return;

    MaybeRefreshMappings(env);
    TryResolveWorldMappings(env);
    TryResolveRenderMappings(env, false);

    unsigned int now = GetTickCount();
    if (now - dbgTick > 2500) {
        dbgTick = now;
        Log("Nametags tick: mapped=" + std::string(g_mapped ? "1" : "0")
            + " worldField=" + std::string(g_theWorldField ? "1" : "0")
            + " playersField=" + std::string(g_playerEntitiesField ? "1" : "0")
            + " ari=" + std::string(g_activeRenderInfoClass ? "1" : "0")
            + " mv=" + std::string(g_modelViewField ? "1" : "0")
            + " pr=" + std::string(g_projectionField ? "1" : "0")
            + " nametagsCfg=" + std::string(nametagsEnabled ? "1" : "0")
            + " telemetry=" + std::string(entityTelemetryNeeded ? "1" : "0"));
    }

    if (!g_playerEntitiesField) {
        if (!warnedMissingMappings) {
            warnedMissingMappings = true;
            Log("Nametags waiting for JNI world mappings.");
        }
        return;
    }

    if (!g_activeRenderInfoClass || !g_modelViewField || !g_projectionField) {
        if (!warnedMissingMappings) {
            warnedMissingMappings = true;
            Log("Nametags waiting for render mappings.");
        }
        return;
    }

   warnedMissingMappings = false;
   g_tagFrameCounter++;

    // Safety check: Don't render if we have pending exceptions
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    
    // Ensure we can create local references (limit to 256 for this frame)
    if (env->PushLocalFrame(256) < 0) {
        env->ExceptionClear();
        return;
    }
    
    // Ensure C locale for dot decimals
    setlocale(LC_NUMERIC, "C");

    // 1. Get Matrices
    Matrix4x4 view = GetMatrix(env, g_modelViewField);
    Matrix4x4 proj = GetMatrix(env, g_projectionField);

    auto MatrixMagnitude = [](const Matrix4x4& m) {
        float sum = 0.0f;
        for (int i = 0; i < 16; i++) sum += std::fabs(m.m[i]);
        return sum;
    };

    bool matrixProjectionUsable = MatrixMagnitude(view) > 0.0001f && MatrixMagnitude(proj) > 0.0001f;

    static int logctr = 0;
    if (logctr++ % 600 == 0) {
         Log("Matrix Debug - View[0]: " + std::to_string(view.m[0]) + " Proj[0]: " + std::to_string(proj.m[0]));
         if (!matrixProjectionUsable) Log("WARNING: Matrix projection unavailable, using angle fallback.");
    }

    // 2. Get Partial Ticks
    float pt = 1.0f;
    if (g_timerField && g_renderPartialTicksField) {
        jobject timer = env->GetObjectField(g_mcInstance, g_timerField);
        if (timer) {
             pt = env->GetFloatField(timer, g_renderPartialTicksField);
             env->DeleteLocalRef(timer);
        }
    }

    // 3. Get RenderManager Viewer Position (the matrix path expects camera-relative coords)
    double vX = 0.0, vY = 0.0, vZ = 0.0;
    if (g_renderManagerField && g_viewerPosXField && g_viewerPosYField && g_viewerPosZField) {
        jobject rm = env->GetObjectField(g_mcInstance, g_renderManagerField);
        if (rm) {
            vX = env->GetDoubleField(rm, g_viewerPosXField);
            vY = env->GetDoubleField(rm, g_viewerPosYField);
            vZ = env->GetDoubleField(rm, g_viewerPosZField);
            env->DeleteLocalRef(rm);
        }
    }

    // 4. Iterate Entities
    jobject world = env->GetObjectField(g_mcInstance, g_theWorldField);
    if (!world) {
        if (logctr % 600 == 0) Log("WARNING: World is null");
        env->PopLocalFrame(nullptr);
        return;
    }
    
    jobject startList = env->GetObjectField(world, g_playerEntitiesField);
    if (!startList) {
        if (logctr % 600 == 0) Log("WARNING: playerEntities list is null");
        env->PopLocalFrame(nullptr);
        return;
    }

    int size = env->CallIntMethod(startList, g_listSizeMethod);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->PopLocalFrame(nullptr);
        return;
    }
    if (logctr % 600 == 0) Log("Entity List Size: " + std::to_string(size));
    
    int count = 0;

    bool guiOpen = g_guiOpen;
    std::string screenName = "none";
    if (g_currentScreenField) {
        jobject currentScreen = env->GetObjectField(g_mcInstance, g_currentScreenField);
        if (currentScreen) {
            guiOpen = true;
            jclass cls = env->GetObjectClass(currentScreen);
            if (cls) {
                jclass cc = env->GetObjectClass(cls);
                jmethodID mGetName = cc ? env->GetMethodID(cc, "getName", "()Ljava/lang/String;") : nullptr;
                if (mGetName) {
                    jstring jn = (jstring)env->CallObjectMethod(cls, mGetName);
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                    } else if (jn) {
                        const char* cn = env->GetStringUTFChars(jn, nullptr);
                        if (cn) {
                            screenName = cn;
                            env->ReleaseStringUTFChars(jn, cn);
                        } else if (env->ExceptionCheck()) {
                            env->ExceptionClear();
                        }
                        env->DeleteLocalRef(jn);
                    }
                }
                if (cc) env->DeleteLocalRef(cc);
                env->DeleteLocalRef(cls);
            }
            env->DeleteLocalRef(currentScreen);
        }
    }

    std::stringstream ss;
    ss << "[";
    constexpr int kEntityJsonCap = 20;
    const int entityProcessCap = nametagsEnabled
        ? (std::max)(1, (std::min)(20, nametagMaxCount))
        : kEntityJsonCap;
    
    // Get Local Player for distance & health debug
    jobject player = env->GetObjectField(g_mcInstance, g_thePlayerField);
    float fallbackYaw = 0.0f;
    float fallbackPitch = 0.0f;
    float fallbackFov = 70.0f;
    
    double localPX = 0.0, localPY = 0.0, localPZ = 0.0;
    bool haveLocalPos = false;
    if (player) {
        localPX = env->GetDoubleField(player, g_posXField);
        localPY = env->GetDoubleField(player, g_posYField);
        localPZ = env->GetDoubleField(player, g_posZField);
        haveLocalPos = true;
    }
    if (!haveLocalPos) {
        env->DeleteLocalRef(startList);
        env->DeleteLocalRef(world);
        goto exit_frame;
    }

    if (player) {
        if (g_rotationYawField) {
            fallbackYaw = env->GetFloatField(player, g_rotationYawField);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fallbackYaw = 0.0f; }
        }
        if (g_rotationPitchField) {
            fallbackPitch = env->GetFloatField(player, g_rotationPitchField);
            if (env->ExceptionCheck()) { env->ExceptionClear(); fallbackPitch = 0.0f; }
        }
    }
    if (g_gameSettingsField && g_fovSettingField) {
        jobject gs = env->GetObjectField(g_mcInstance, g_gameSettingsField);
        if (gs) {
            float fov = env->GetFloatField(gs, g_fovSettingField);
            if (!env->ExceptionCheck() && fov >= 10.0f && fov <= 170.0f) {
                fallbackFov = fov;
            } else if (env->ExceptionCheck()) {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(gs);
        }
    }

    for (int i = 0; i < size && count < entityProcessCap; i++) {
        jobject entity = env->CallObjectMethod(startList, g_listGetMethod, i);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            break;
        }
        if (!entity) continue;

        if (env->IsSameObject(entity, player)) {
            env->DeleteLocalRef(entity);
            continue;
        }

        // Positions
        double ex = env->GetDoubleField(entity, g_posXField);
        double ey = env->GetDoubleField(entity, g_posYField);
        double ez = env->GetDoubleField(entity, g_posZField);
        
        double lx = g_lastTickPosXField ? env->GetDoubleField(entity, g_lastTickPosXField) : ex;
        double ly = g_lastTickPosYField ? env->GetDoubleField(entity, g_lastTickPosYField) : ey;
        double lz = g_lastTickPosZField ? env->GetDoubleField(entity, g_lastTickPosZField) : ez;

        // Interpolate
        double iX = lx + (ex - lx) * pt;
        double iY = ly + (ey - ly) * pt;
        double iZ = lz + (ez - lz) * pt;
        double rX = iX - vX;
        double rY = iY - vY;
        double rZ = iZ - vZ;

        // Name with fake/bot line filtering parity
        std::string displayName = GetStablePlayerName(env, entity);
        if (displayName.empty() || LooksLikeFakePlayerLine(displayName)) {
            env->DeleteLocalRef(entity);
            continue;
        }
        
        // Health
        float health = 20.0f;
        if (g_getHealthMethod) {
            health = env->CallFloatMethod(entity, g_getHealthMethod);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }

        // Project (prefer matrix when stable, otherwise fallback to angle/FOV)
        float sX = 0, sY = 0;
        bool matrixProjected = false;
        float matrixSX = 0, matrixSY = 0;
        if (matrixProjectionUsable) {
            matrixProjected = WorldToScreen(rX, rY + 2.3, rZ, view, proj, w, h, matrixSX, matrixSY);
        }

        LegoVec3 camPos = { localPX, localPY + 1.62, localPZ };

        bool projected = false;
        if (matrixProjected) {
            sX = matrixSX;
            sY = matrixSY;
            projected = true;
        }
        // Do not use angle/FOV fallback here. On 1.8.9 this path can be misaligned and causes ghost tracking.

        // Aim-assist target point: closest projected point on a player body box
        // to the current crosshair center. Keep nametag anchor separate.
        float aimSX = sX;
        float aimSY = sY;
        bool aimProjected = projected;
        {
            const double halfW = 0.30;
            const double xOffsets[3] = { -halfW, 0.0, halfW };
            const double zOffsets[3] = { -halfW, 0.0, halfW };
            const double yOffsets[5] = { 0.15, 0.55, 0.95, 1.35, 1.75 };
            const double centerX = w * 0.5;
            const double centerY = h * 0.5;

            double bestScore = 1e30;
            float bestSx = aimSX;
            float bestSy = aimSY;
            bool bestFound = false;

            for (double yo : yOffsets) {
                for (double xo : xOffsets) {
                    for (double zo : zOffsets) {
                        float tx = 0.0f;
                        float ty = 0.0f;
                        bool sampleProjected = false;

                        if (matrixProjectionUsable) {
                            sampleProjected = WorldToScreen(
                                (iX + xo) - vX,
                                (iY + yo) - vY,
                                (iZ + zo) - vZ,
                                view,
                                proj,
                                w,
                                h,
                                tx,
                                ty);
                        }

                        if (!sampleProjected) continue;

                        double dxCenter = tx - centerX;
                        double dyCenter = ty - centerY;
                        double score = dxCenter * dxCenter + dyCenter * dyCenter;
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
                aimSX = bestSx;
                aimSY = bestSy;
                aimProjected = true;
            }
        }

        if (projected) {
             if (logctr % 600 == 0 && count == 0) {
                 Log("Tagged: " + displayName + " SX: " + std::to_string(sX) + " SY: " + std::to_string(sY));
             }
             double dx = iX - localPX;
             double dy = iY - localPY;
             double dz = iZ - localPZ;
             double dist = sqrt(dx*dx + dy*dy + dz*dz);
             if (dist > 48.0) {
                 env->DeleteLocalRef(entity);
                 continue;
             }
             if (sX < -64.0f || sX > (float)w + 64.0f || sY < -64.0f || sY > (float)h + 64.0f) {
                 env->DeleteLocalRef(entity);
                 continue;
             }
              if (!displayName.empty()) {
                  float hpClampedSimple = health < 0 ? 0 : (health > 40 ? 40 : health);

                   if (!nametagsEnabled) {
                       if (count < kEntityJsonCap) {
                           if (count > 0) ss << ",";
                           ss << "{";
                           ss << "\"sx\":" << (aimProjected ? aimSX : sX) << ",";
                           ss << "\"sy\":" << (aimProjected ? aimSY : sY) << ",";
                           ss << "\"dist\":" << dist << ",";
                           ss << "\"name\":\"" << JsonEscape(displayName) << "\",";
                           ss << "\"hp\":" << hpClampedSimple;
                           ss << "}";
                      }
                      count++;
                      if (count >= entityProcessCap) {
                          env->DeleteLocalRef(entity);
                          break;
                      }
                      continue;
                  }

                  float nameScale = (float)(0.78 / (dist * 0.04 + 1.0));
                  if (nameScale < 0.38f) nameScale = 0.38f;
                  if (nameScale > 0.60f) nameScale = 0.60f;

                 float infoScale = nameScale * 0.88f;
                 float hpClamped = health < 0 ? 0 : (health > 40 ? 40 : health);
                 float hpBarValue = health < 0 ? 0 : (health > 20 ? 20 : health);
                 float hpPct = hpBarValue / 20.0f;
                 if (hpPct < 0.0f) hpPct = 0.0f;
                 if (hpPct > 1.0f) hpPct = 1.0f;

                 int armorPoints = -1;
                 if (showArmor && g_getTotalArmorValueMethod) {
                     armorPoints = env->CallIntMethod(entity, g_getTotalArmorValueMethod);
                     if (env->ExceptionCheck()) { env->ExceptionClear(); armorPoints = -1; }
                 }

                 std::string statsText;
                 if (showHealth) {
                     char hpBuf[32];
                     snprintf(hpBuf, sizeof(hpBuf), "%.0f HP", hpClamped);
                     statsText += hpBuf;
                 }
                 if (showArmor && armorPoints >= 0) {
                     if (!statsText.empty()) statsText += " | ";
                     char armorBuf[32];
                     snprintf(armorBuf, sizeof(armorBuf), "%d ARM", armorPoints);
                     statsText += armorBuf;
                 }

                 std::string heldText = GetEntityHeldItemInfo(env, entity, nullptr);

                 bool hasStatsLine = !statsText.empty();
                 bool hasHeldLine = !heldText.empty();
                 bool lowHp = showHealth && hpClamped <= 8.0f;

                 float nameW = TextWidth(displayName.c_str(), nameScale);
                 float statsW = hasStatsLine ? TextWidth(statsText.c_str(), infoScale) : 0.0f;
                 float heldScale = infoScale * 0.92f;
                 float heldTextW = hasHeldLine ? TextWidth(heldText.c_str(), heldScale) : 0.0f;
                 float heldW = hasHeldLine ? heldTextW : 0.0f;
                 float contentW = nameW;
                 if (statsW > contentW) contentW = statsW;
                 if (heldW > contentW) contentW = heldW;
                 float mainPanelW = contentW + 14.0f;
                 float panelH = CHAR_H * nameScale + 8.0f;
                 if (hasStatsLine) panelH += CHAR_H * infoScale;
                 if (hasHeldLine) panelH += CHAR_H * heldScale;
                 if (showHealth) panelH += 4.0f;
                 float panelW = mainPanelW;
                 int key = 0;
                 if (g_objectHashCodeMethod) {
                     key = env->CallIntMethod(entity, g_objectHashCodeMethod);
                     if (env->ExceptionCheck()) { env->ExceptionClear(); key = 0; }
                 }
                 if (key == 0) {
                     key = (int)(iX * 17.0 + iZ * 31.0) ^ i;
                 }

                 float px = sX - panelW * 0.5f;
                 float py = sY - panelH - 6.0f;
                 auto& smooth = g_tagSmoothing[key];
                 if (!smooth.init) {
                     smooth.x = px;
                     smooth.y = py;
                     smooth.vx = 0.0f;
                     smooth.vy = 0.0f;
                     smooth.init = true;
                 }
                 else {
                     float dxs = px - smooth.x;
                     float dys = py - smooth.y;
                     float deltaSq = dxs * dxs + dys * dys;

                     if (deltaSq > 24000.0f) {
                         smooth.x = px;
                         smooth.y = py;
                         smooth.vx = 0.0f;
                         smooth.vy = 0.0f;
                     }
                     else {
                         float blend = 0.12f;
                         if (dist < 12.0) blend = 0.14f;
                         if (deltaSq < 6.0f) blend = 0.22f;
                         smooth.x += dxs * blend;
                         smooth.y += dys * blend;
                     }
                 }
                 smooth.lastFrame = g_tagFrameCounter;
                 px = smooth.x;
                 py = smooth.y;

                 float alpha = 0.86f - (float)(dist / 80.0);
                 if (alpha < 0.42f) alpha = 0.42f;
                 if (alpha > 0.9f) alpha = 0.9f;

                 float hpR = (1.0f - hpPct) * 0.95f + 0.05f;
                 float hpG = hpPct * 0.85f + 0.15f;
                 float hpB = 0.20f;
                 float infoR = showHealth ? hpR : 0.88f;
                 float infoG = showHealth ? hpG : 0.89f;
                 float infoB = showHealth ? hpB : 0.92f;

                 DrawRect(px - 1.0f, py - 1.0f, panelW + 2.0f, panelH + 2.0f, 0.0f, 0.0f, 0.0f, 0.55f * alpha);
                 DrawRect(px, py, mainPanelW, panelH, 0.02f, 0.02f, 0.03f, 0.92f * alpha);
                 DrawRect(px, py, mainPanelW, 1.0f, 0.70f, 0.88f, 1.0f, 0.82f * alpha);
                 DrawRect(px, py + panelH - 1.0f, mainPanelW, 1.0f, 0.0f, 0.0f, 0.0f, 0.35f * alpha);

                 float nameX = px + (mainPanelW - nameW) * 0.5f;
                 float nameY = py + 2.0f;
                 DrawTextShadow(nameX, nameY, displayName.c_str(), 0.98f, 0.99f, 1.0f, alpha, nameScale);

                 float lineY = py + 4.0f + CHAR_H * nameScale;
                 if (hasStatsLine) {
                     float statsX = px + (mainPanelW - statsW) * 0.5f;
                     DrawTextShadow(statsX, lineY, statsText.c_str(), infoR, infoG, infoB, alpha, infoScale);
                     lineY += CHAR_H * infoScale;
                 }

                 if (hasHeldLine) {
                     float heldX = px + (mainPanelW - heldTextW) * 0.5f;
                     DrawTextShadow(heldX, lineY, heldText.c_str(), 0.84f, 0.88f, 0.95f, alpha, heldScale);
                 }

                 if (showHealth) {
                     float barX = px + 4.0f;
                     float barY = py + panelH - 5.0f;
                     float barW = mainPanelW - 8.0f;
                     DrawRect(barX, barY, barW, 2.0f, 0.08f, 0.08f, 0.10f, 0.9f * alpha);
                     DrawRect(barX, barY, barW * hpPct, 2.0f, hpR, hpG, hpB, 1.0f * alpha);
                 }

                 if (lowHp) {
                     DrawTextShadow(px + mainPanelW - TextWidth("LOW", infoScale * 0.9f) - 4.0f, py + 2.0f, "LOW", 1.0f, 0.32f, 0.32f, alpha, infoScale * 0.9f);
                 }
                 if (count < kEntityJsonCap) {
                     if (count > 0) ss << ",";
                     ss << "{";
                     ss << "\"sx\":" << (aimProjected ? aimSX : sX) << ",";
                     ss << "\"sy\":" << (aimProjected ? aimSY : sY) << ",";
                     ss << "\"dist\":" << dist << ",";
                     ss << "\"name\":\"" << JsonEscape(displayName) << "\",";
                     ss << "\"hp\":" << hpClamped;
                     ss << "}";
                 }

                  count++;
                  if (count >= entityProcessCap) {
                      env->DeleteLocalRef(entity);
                      break;
                  }
             }
        }

        env->DeleteLocalRef(entity);
    }
    
    // Cleanup stale smoothing state
    for (auto it = g_tagSmoothing.begin(); it != g_tagSmoothing.end(); ) {
        if (g_tagFrameCounter - it->second.lastFrame > 45) {
            it = g_tagSmoothing.erase(it);
        } else {
            ++it;
        }
    }

    // Close entities array
    ss << "]";

    env->DeleteLocalRef(startList);
    env->DeleteLocalRef(world);

    // Store entities JSON for ServerLoop injection
    {
        std::string json = ss.str();
        {
            LockGuard lk(g_jsonMutex);
            g_pendingJson = json;
        }
    }
    
exit_frame:
    env->PopLocalFrame(nullptr);
}

void RenderClosestPlayerInfo(int w, int h) {
    bool enabled = false;
    bool showHealth = true;
    bool showArmor = true;
    {
        LockGuard lk(g_configMutex);
        enabled = g_config.closestPlayerInfo;
        showHealth = g_config.nametagShowHealth;
        showArmor = g_config.nametagShowArmor;
    }
    if (!enabled) return;
    if (!g_mapped || !g_mcInstance || !g_theWorldField || !g_listSizeMethod || !g_listGetMethod) return;
    if (!g_thePlayerField || !g_posXField || !g_posYField || !g_posZField) return;

    ScopedJNIEnv env(g_jvm);
    if (!env) return;

    TryResolveWorldMappings(env);
    if (!g_playerEntitiesField) return;

    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (env->PushLocalFrame(256) < 0) { env->ExceptionClear(); return; }

    auto finish = [&]() {
        env->PopLocalFrame(nullptr);
    };

    jobject world = env->GetObjectField(g_mcInstance, g_theWorldField);
    if (!world) { finish(); return; }
    jobject list = env->GetObjectField(world, g_playerEntitiesField);
    if (!list) { finish(); return; }

    jobject localPlayer = env->GetObjectField(g_mcInstance, g_thePlayerField);
    if (!localPlayer) { finish(); return; }

    double lpx = env->GetDoubleField(localPlayer, g_posXField);
    double lpy = env->GetDoubleField(localPlayer, g_posYField);
    double lpz = env->GetDoubleField(localPlayer, g_posZField);
    float localYaw = 0.0f;
    if (g_rotationYawField) localYaw = env->GetFloatField(localPlayer, g_rotationYawField);

    int size = env->CallIntMethod(list, g_listSizeMethod);
    if (env->ExceptionCheck()) { env->ExceptionClear(); finish(); return; }

    int closestIndex = -1;
    double bestDist = 99999.0;
    double bestDx = 0.0, bestDz = 0.0;

    for (int i = 0; i < size; i++) {
        jobject entity = env->CallObjectMethod(list, g_listGetMethod, i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        if (!entity) continue;
        if (env->IsSameObject(entity, localPlayer)) { env->DeleteLocalRef(entity); continue; }

        double ex = env->GetDoubleField(entity, g_posXField);
        double ey = env->GetDoubleField(entity, g_posYField);
        double ez = env->GetDoubleField(entity, g_posZField);
        double dx = ex - lpx;
        double dy = ey - lpy;
        double dz = ez - lpz;
        double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < bestDist) {
            std::string candidateName = GetStablePlayerName(env, entity);
            if (!candidateName.empty() && !LooksLikeFakePlayerLine(candidateName)) {
                bestDist = dist;
                closestIndex = i;
                bestDx = dx;
                bestDz = dz;
            }
        }

        env->DeleteLocalRef(entity);
    }

    if (closestIndex < 0 || bestDist > 96.0) { finish(); return; }

    jobject closest = env->CallObjectMethod(list, g_listGetMethod, closestIndex);
    if (env->ExceptionCheck()) { env->ExceptionClear(); finish(); return; }
    if (!closest) { finish(); return; }

    std::string name = GetStablePlayerName(env, closest);
    if (name.empty() || LooksLikeFakePlayerLine(name)) {
        finish();
        return;
    }

    float health = 20.0f;
    if (showHealth && g_getHealthMethod) {
        health = env->CallFloatMethod(closest, g_getHealthMethod);
        if (env->ExceptionCheck()) { env->ExceptionClear(); health = 20.0f; }
    }

    int armorPoints = -1;
    if (showArmor && g_getTotalArmorValueMethod) {
        armorPoints = env->CallIntMethod(closest, g_getTotalArmorValueMethod);
        if (env->ExceptionCheck()) { env->ExceptionClear(); armorPoints = -1; }
    }

    std::string heldText = GetEntityHeldItemInfo(env, closest, nullptr);

    std::string statsText;
    if (showHealth) {
        char hp[24];
        float hpClamped = health < 0 ? 0 : (health > 40 ? 40 : health);
        snprintf(hp, sizeof(hp), "%.0f HP", hpClamped);
        statsText += hp;
    }
    if (showArmor && armorPoints >= 0) {
        if (!statsText.empty()) statsText += " | ";
        char ar[24];
        snprintf(ar, sizeof(ar), "%d ARM", armorPoints);
        statsText += ar;
    }

    std::string dirText = RelativeDirectionText(localYaw, bestDx, bestDz);
    char distDir[64];
    snprintf(distDir, sizeof(distDir), "%.1fm | %s", (float)bestDist, dirText.c_str());

    float nameScale = 0.56f;
    float infoScale = 0.46f;
    float heldScale = 0.42f;

    float nameW = TextWidth(name.c_str(), nameScale);
    float distDirW = TextWidth(distDir, infoScale);
    float statsW = statsText.empty() ? 0.0f : TextWidth(statsText.c_str(), infoScale);
    float heldTextW = heldText.empty() ? 0.0f : TextWidth(heldText.c_str(), heldScale);
    float heldW = heldText.empty() ? 0.0f : heldTextW;
    float mainPanelW = nameW;
    if (distDirW > mainPanelW) mainPanelW = distDirW;
    if (statsW > mainPanelW) mainPanelW = statsW;
    if (heldW > mainPanelW) mainPanelW = heldW;
    mainPanelW += 22.0f;

    float panelH = 10.0f + CHAR_H * nameScale + CHAR_H * infoScale;
    if (!statsText.empty()) panelH += CHAR_H * infoScale;
    if (!heldText.empty()) panelH += CHAR_H * heldScale;
    float panelW = mainPanelW;

    float px = (float)w * 0.5f + 98.0f;
    float py = (float)h - 94.0f - panelH;
    if (px + panelW > w - 8.0f) px = (float)w - panelW - 8.0f;

    DrawRect(px - 1.0f, py - 1.0f, panelW + 2.0f, panelH + 2.0f, 0.0f, 0.0f, 0.0f, 0.60f);
    DrawRect(px, py, mainPanelW, panelH, 0.03f, 0.03f, 0.04f, 0.92f);
    DrawRect(px, py, mainPanelW, 1.0f, 0.66f, 0.86f, 1.0f, 0.85f);

    float y = py + 3.0f;
    DrawTextShadow(px + (mainPanelW - nameW) * 0.5f, y, name.c_str(), 0.98f, 0.99f, 1.0f, 1.0f, nameScale);
    y += CHAR_H * nameScale;

    DrawTextShadow(px + (mainPanelW - distDirW) * 0.5f, y, distDir, 0.82f, 0.88f, 0.98f, 0.98f, infoScale);
    y += CHAR_H * infoScale;

    if (!statsText.empty()) {
        DrawTextShadow(px + (mainPanelW - statsW) * 0.5f, y, statsText.c_str(), 0.86f, 0.92f, 0.88f, 0.98f, infoScale);
        y += CHAR_H * infoScale;
    }

    if (!heldText.empty()) {
        DrawTextShadow(px + (mainPanelW - heldTextW) * 0.5f, y, heldText.c_str(), 0.84f, 0.88f, 0.95f, 0.98f, heldScale);
    }

    finish();
}

void RenderChestESP(int w, int h) {
    static bool warnedChestMappings = false;
    int chestEspMaxCount = 5;
    {
        LockGuard lk(g_configMutex);
        if (!g_config.chestEsp) return;
        chestEspMaxCount = (std::max)(1, (std::min)(20, g_config.chestEspMaxCount));
    }
    if (!g_mapped || !g_mcInstance) return;
    if (!g_theWorldField || !g_listSizeMethod || !g_listGetMethod) return;
    if (!g_thePlayerField || !g_posXField || !g_posYField || !g_posZField) return;
    if (!g_tileEntityPosField || !g_blockPosGetX || !g_blockPosGetY || !g_blockPosGetZ) return;

    ScopedJNIEnv env(g_jvm);
    if (!env) return;

    TryResolveWorldMappings(env);
    TryResolveRenderMappings(env, false);

    if (!g_loadedTileEntityListField) {
        if (!warnedChestMappings) {
            warnedChestMappings = true;
            Log("ChestESP waiting for JNI world mappings.");
        }
        return;
    }
    if (!g_activeRenderInfoClass || !g_modelViewField || !g_projectionField) {
        if (!warnedChestMappings) {
            warnedChestMappings = true;
            Log("ChestESP waiting for render mappings.");
        }
        return;
    }

    warnedChestMappings = false;

    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (env->PushLocalFrame(256) < 0) { env->ExceptionClear(); return; }

    Matrix4x4 view = GetMatrix(env, g_modelViewField);
    Matrix4x4 proj = GetMatrix(env, g_projectionField);

    double vX = 0.0, vY = 0.0, vZ = 0.0;
    if (g_renderManagerField && g_viewerPosXField && g_viewerPosYField && g_viewerPosZField) {
        jobject rm = env->GetObjectField(g_mcInstance, g_renderManagerField);
        if (rm) {
            vX = env->GetDoubleField(rm, g_viewerPosXField);
            vY = env->GetDoubleField(rm, g_viewerPosYField);
            vZ = env->GetDoubleField(rm, g_viewerPosZField);
            env->DeleteLocalRef(rm);
        }
    }

    jobject world = env->GetObjectField(g_mcInstance, g_theWorldField);
    if (!world) { env->PopLocalFrame(nullptr); return; }
    jobject tileList = env->GetObjectField(world, g_loadedTileEntityListField);
    if (!tileList) {
        env->PopLocalFrame(nullptr);
        return;
    }

    jobject player = env->GetObjectField(g_mcInstance, g_thePlayerField);
    if (!player) {
        env->PopLocalFrame(nullptr);
        return;
    }

    double localPX = env->GetDoubleField(player, g_posXField);
    double localPY = env->GetDoubleField(player, g_posYField);
    double localPZ = env->GetDoubleField(player, g_posZField);

    int size = env->CallIntMethod(tileList, g_listSizeMethod);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->PopLocalFrame(nullptr);
        return;
    }

    int drawn = 0;
    for (int i = 0; i < size && drawn < chestEspMaxCount; i++) {
        jobject te = env->CallObjectMethod(tileList, g_listGetMethod, i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        if (!te) continue;

        bool isChest = false;
        if (g_tileEntityChestClass && env->IsInstanceOf(te, g_tileEntityChestClass)) isChest = true;
        if (!isChest && g_tileEntityEnderChestClass && env->IsInstanceOf(te, g_tileEntityEnderChestClass)) isChest = true;
        if (!isChest) {
            jclass teClass = env->GetObjectClass(te);
            if (teClass) {
                std::string clsName = GetClassNameFromClass(env, teClass);
                if (clsName.find("Chest") != std::string::npos) isChest = true;
                env->DeleteLocalRef(teClass);
            }
        }
        if (!isChest) {
            env->DeleteLocalRef(te);
            continue;
        }

        jobject posObj = env->GetObjectField(te, g_tileEntityPosField);
        if (!posObj) {
            env->DeleteLocalRef(te);
            continue;
        }

        int bx = env->CallIntMethod(posObj, g_blockPosGetX);
        int by = env->CallIntMethod(posObj, g_blockPosGetY);
        int bz = env->CallIntMethod(posObj, g_blockPosGetZ);
        env->DeleteLocalRef(posObj);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(te);
            continue;
        }

        double cx = (double)bx + 0.5;
        double cy = (double)by + 0.5;
        double cz = (double)bz + 0.5;
        double dx = cx - localPX;
        double dy = cy - localPY;
        double dz = cz - localPZ;
        double dist = sqrt(dx*dx + dy*dy + dz*dz);
        if (dist > 32.0) {
            env->DeleteLocalRef(te);
            continue;
        }

        const double minX = (double)bx;
        const double minY = (double)by;
        const double minZ = (double)bz;
        const double maxX = minX + 1.0;
        const double maxY = minY + 1.0;
        const double maxZ = minZ + 1.0;

        double corners[8][3] = {
            {minX, minY, minZ}, {maxX, minY, minZ}, {maxX, maxY, minZ}, {minX, maxY, minZ},
            {minX, minY, maxZ}, {maxX, minY, maxZ}, {maxX, maxY, maxZ}, {minX, maxY, maxZ}
        };

        float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
        bool projected = true;
        for (int c = 0; c < 8; c++) {
            float sx = 0.0f, sy = 0.0f;
            if (!WorldToScreen(corners[c][0] - vX, corners[c][1] - vY, corners[c][2] - vZ, view, proj, w, h, sx, sy)) {
                projected = false;
                break;
            }
            if (sx < left) left = sx;
            if (sy < top) top = sy;
            if (sx > right) right = sx;
            if (sy > bottom) bottom = sy;
        }

        if (projected && right > left && bottom > top) {
            float alpha = 0.34f - (float)(dist / 120.0);
            if (alpha < 0.14f) alpha = 0.14f;
            DrawRect(left, top, right - left, bottom - top, 0.10f, 0.80f, 0.70f, alpha);
            DrawRect(left, top, right - left, 1.0f, 0.35f, 1.00f, 0.85f, 0.75f);
            DrawRect(left, bottom - 1.0f, right - left, 1.0f, 0.35f, 1.00f, 0.85f, 0.75f);
            DrawRect(left, top, 1.0f, bottom - top, 0.35f, 1.00f, 0.85f, 0.75f);
            DrawRect(right - 1.0f, top, 1.0f, bottom - top, 0.35f, 1.00f, 0.85f, 0.75f);
            drawn++;
        }

        env->DeleteLocalRef(te);
    }

    env->PopLocalFrame(nullptr);
}

// ===================== CLICKGUI =====================

// Key capture state for keybind rebinding
static int  g_capturingBindForModule = -1; // index into g_modules, or -1
static bool g_prevKeyState[256];

// Convert a VK code to a human-readable name
static std::string VkToName(int vk) {
    if (vk <= 0) return "None";
    char buf[64] = {};
    UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lparam = (LONG)(scan << 16);
    // Extended-key flag for navigation/numpad keys
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
        vk == VK_PRIOR  || vk == VK_NEXT   || vk == VK_UP   || vk == VK_DOWN ||
        vk == VK_LEFT   || vk == VK_RIGHT  || vk == VK_DIVIDE || vk == VK_NUMLOCK)
        lparam |= (1L << 24);
    if (!GetKeyNameTextA(lparam, buf, sizeof(buf)) || buf[0] == '\0')
        snprintf(buf, sizeof(buf), "VK %02X", (unsigned)vk);
    return buf;
}

// Retrieve the keybind VK for a given module id from cfg
static int GetModuleKeybind(const Config& cfg, const char* id) {
    if (strcmp(id, "autoclicker")  == 0) return cfg.keybindAutoclicker;
    if (strcmp(id, "nametags")     == 0) return cfg.keybindNametags;
    if (strcmp(id, "closestplayer")== 0) return cfg.keybindClosestPlayer;
    if (strcmp(id, "chestesp")     == 0) return cfg.keybindChestEsp;
    return 0;
}

enum ClickCategory {
    CAT_COMBAT = 0,
    CAT_RENDER = 1
};

struct ModuleSpec {
    const char* id;
    const char* name;
    const char* action;
    ClickCategory category;
};

static const ModuleSpec g_modules[] = {
    { "autoclicker", "AutoClicker", "toggleArmed", CAT_COMBAT },
    { "nametags", "Nametags", "toggleNametags", CAT_RENDER },
    { "closestplayer", "Closest Player", "toggleClosestPlayerInfo", CAT_RENDER },
    { "chestesp", "Chest ESP", "toggleChestEsp", CAT_RENDER },
};

static bool g_guiWindowInit = false;
static float g_guiWindowX = 0.0f;
static float g_guiWindowY = 0.0f;
static bool g_guiDragging = false;
static float g_guiDragOffsetX = 0.0f;
static float g_guiDragOffsetY = 0.0f;
static ClickCategory g_selectedCategory = CAT_COMBAT;
static int g_selectedModuleIdx = 0;
static bool g_wasMouseDown = false;
static bool g_wasMouseRightDown = false;

bool IsPointInRect(float x, float y, float w, float h) {
    return g_mouseX >= x && g_mouseX <= x + w && g_mouseY >= y && g_mouseY <= y + h;
}

// Returns 1 = left-click (start capture), 2 = right-click (unbind), 0 = nothing
int GuiKeybindButton(float x, float y, float w, int keyVk, int moduleIdx, float scale) {
    float h = CHAR_H * scale + 6.0f;
    bool hovered = IsPointInRect(x, y, w, h);
    Color3 accent = AccentColor(0.0f);
    bool capturing = (g_capturingBindForModule == moduleIdx);

    DrawRect(x, y, w, h, hovered ? 0.14f : 0.11f, hovered ? 0.14f : 0.11f, hovered ? 0.17f : 0.14f, 0.94f);
    DrawText2D(x + 7.0f, y + 3.0f, "Keybind", 0.72f, 0.74f, 0.80f, 1.0f, scale);

    std::string keyName = capturing ? "[Press key...]" : VkToName(keyVk);
    float kw = TextWidth(keyName.c_str(), scale * 0.85f);
    float bx = x + w - kw - 14.0f;
    float by = y + 2.0f;
    float bw = kw + 8.0f;
    float bh = h - 4.0f;
    DrawRect(bx, by, bw, bh,
             capturing ? accent.r * 0.25f + 0.06f : 0.17f,
             capturing ? accent.g * 0.25f + 0.06f : 0.17f,
             capturing ? accent.b * 0.25f + 0.08f : 0.21f, 0.97f);
    if (capturing)
        DrawRect(bx, by, bw, bh, accent.r, accent.g, accent.b, 0.22f);
    DrawText2D(bx + 4.0f, y + 3.0f, keyName.c_str(),
               capturing ? accent.r : 0.82f,
               capturing ? accent.g : 0.85f,
               capturing ? accent.b : 0.90f, 1.0f, scale * 0.85f);

    if (hovered && g_mouseClicked)      return 1;
    if (hovered && g_mouseRightClicked) return 2;
    return 0;
}

bool GuiSettingToggle(float x, float y, float w, const char* label, bool value, float scale) {
    float h = CHAR_H * scale + 6.0f;
    bool hovered = IsPointInRect(x, y, w, h);
    Color3 accent = AccentColor(0.0f);

    DrawRect(x, y, w, h, hovered ? 0.13f : 0.10f, hovered ? 0.13f : 0.10f, hovered ? 0.14f : 0.12f, 0.94f);
    DrawText2D(x + 7.0f, y + 3.0f, label, 0.90f, 0.90f, 0.93f, 1.0f, scale);

    float tw = 24.0f, th = 11.0f;
    float tx = x + w - tw - 8.0f;
    float ty = y + (h - th) * 0.5f;
    if (value) {
        DrawRect(tx, ty, tw, th, accent.r, accent.g, accent.b, 1.0f);
        DrawRect(tx + tw - th, ty, th, th, 0.96f, 0.97f, 0.98f, 1.0f);
    } else {
        DrawRect(tx, ty, tw, th, 0.27f, 0.27f, 0.30f, 1.0f);
        DrawRect(tx, ty, th, th, 0.62f, 0.62f, 0.67f, 1.0f);
    }
    return hovered && g_mouseClicked;
}

float GuiSettingSlider(float x, float y, float w, const char* id, const char* label, float value, float mn, float mx, float scale, bool integerText) {
    float h = CHAR_H * scale + 19.0f;
    bool hovered = IsPointInRect(x, y, w, h);
    Color3 accent = AccentColor(0.0f);

    DrawRect(x, y, w, h, hovered ? 0.13f : 0.10f, hovered ? 0.13f : 0.10f, hovered ? 0.14f : 0.12f, 0.94f);

    char buf[80];
    if (integerText) snprintf(buf, sizeof(buf), "%s: %.0f", label, value);
    else snprintf(buf, sizeof(buf), "%s: %.2f", label, value);
    DrawText2D(x + 7.0f, y + 2.0f, buf, 0.90f, 0.90f, 0.93f, 1.0f, scale);

    float sx = x + 7.0f;
    float sy = y + CHAR_H * scale + 7.0f;
    float sw = w - 14.0f;
    float sh = 4.0f;
    DrawRect(sx, sy, sw, sh, 0.20f, 0.20f, 0.23f, 1.0f);

    static std::string g_activeSliderId;
    bool hoverTrack = IsPointInRect(sx - 4.0f, sy - 6.0f, sw + 8.0f, sh + 12.0f);
    if (g_mouseClicked && hoverTrack) g_activeSliderId = id;
    if (!g_mouseDown && g_activeSliderId == id) g_activeSliderId.clear();

    if (g_mouseDown && g_activeSliderId == id) {
        float pct = (g_mouseX - sx) / sw;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        value = mn + pct * (mx - mn);
    }

    float pct = (value - mn) / (mx - mn);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    DrawRect(sx, sy, sw * pct, sh, accent.r, accent.g, accent.b, 1.0f);
    float knobX = sx + sw * pct;
    DrawRect(knobX - 2.0f, sy - 2.0f, 4.0f, sh + 4.0f, 0.93f, 0.95f, 0.97f, 1.0f);
    return value;
}

void DrawModuleButton(float x, float y, float w, const ModuleSpec& m, bool enabled, bool selected, float scale, bool* leftPressed, bool* rightPressed) {
    float h = CHAR_H * scale + 10.0f;
    bool hovered = IsPointInRect(x, y, w, h);
    Color3 accent = AccentColor(0.0f);

    float bg = selected ? 0.15f : (hovered ? 0.13f : 0.10f);
    DrawRect(x, y, w, h, bg, bg, bg + 0.02f, 0.95f);

    if (enabled) {
        DrawRect(x, y, 2.0f, h, accent.r, accent.g, accent.b, 1.0f);
        DrawText2D(x + 8.0f, y + 5.0f, m.name, 0.93f, 0.94f, 0.96f, 1.0f, scale);
    } else {
        DrawRect(x, y, 2.0f, h, 0.33f, 0.33f, 0.37f, 0.9f);
        DrawText2D(x + 8.0f, y + 5.0f, m.name, 0.67f, 0.67f, 0.71f, 1.0f, scale);
    }

    DrawText2D(x + w - TextWidth("[RMB]", scale * 0.72f) - 8.0f, y + 7.0f, "[RMB]", 0.45f, 0.45f, 0.50f, 1.0f, scale * 0.72f);

    *leftPressed = hovered && g_mouseClicked;
    *rightPressed = hovered && g_mouseRightClicked;
}

bool IsModuleEnabled(const ModuleSpec& m, const Config& cfg) {
    if (strcmp(m.id, "autoclicker") == 0) return cfg.armed;
    if (strcmp(m.id, "leftclick") == 0) return cfg.leftClick;
    if (strcmp(m.id, "rightclick") == 0) return cfg.rightClick;
    if (strcmp(m.id, "jitter") == 0) return cfg.jitter;

    if (strcmp(m.id, "nametags") == 0) return cfg.nametags;
    if (strcmp(m.id, "closestplayer") == 0) return cfg.closestPlayerInfo;
    if (strcmp(m.id, "chestesp") == 0) return cfg.chestEsp;
    if (strcmp(m.id, "clickinchests") == 0) return cfg.clickInChests;
    return false;
}

void CloseInternalGui();
void OpenInternalGui();

void RenderClickGUI(int winW, int winH) {
    if (!g_guiOpen) return;

    // Native GUI handling: Open GuiChat to handle cursor/camera
    if (g_guiChatClass && g_displayGuiScreenMethod && g_mcInstance) {
        JNIEnv* env = nullptr;
        g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8);
        if (!env) g_jvm->AttachCurrentThread((void**)&env, nullptr);
        if (env) {
            jobject currentScreen = nullptr;
            if (g_currentScreenField) {
                currentScreen = env->GetObjectField(g_mcInstance, g_currentScreenField);
            }
            
            // Open GuiChat once to unlock cursor while ClickGUI is open.
            if (!currentScreen && !g_nativeChatOpenedByClickGui) {
                // Construct GuiChat with empty string
                jobject chatGui = nullptr;
                if (g_guiChatConstructor) {
                     jstring empty = env->NewStringUTF("");
                     if (empty) {
                        chatGui = env->NewObject(g_guiChatClass, g_guiChatConstructor, empty);
                        env->DeleteLocalRef(empty);
                     }
                }
                
                if (chatGui) {
                    env->CallVoidMethod(g_mcInstance, g_displayGuiScreenMethod, chatGui);
                    env->DeleteLocalRef(chatGui);
                    g_nativeChatOpenedByClickGui = true;
                }
            }
            // CRITICAL: Must delete local refs created in loop!
            if (currentScreen) env->DeleteLocalRef(currentScreen);
            
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    // Poll mouse position (still useful for our internal UI logic)
    POINT pt;
    if (GetCursorPos(&pt) && ScreenToClient(g_gameHwnd, &pt)) {
        g_mouseX = pt.x;
        g_mouseY = pt.y;
    }
    
    // Poll mouse states
    bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    g_mouseClicked = leftDown && !g_wasMouseDown;
    g_mouseRightClicked = rightDown && !g_wasMouseRightDown;
    g_mouseDown = leftDown;
    g_mouseRightDown = rightDown;
    g_wasMouseDown = leftDown;
    g_wasMouseRightDown = rightDown;

    // Update keyboard state and handle keybind capture
    {
        int captured = -1;
        for (int vk = 1; vk < 255; vk++) {
            bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool justPressed = down && !g_prevKeyState[vk];
            g_prevKeyState[vk] = down;
            if (justPressed && g_capturingBindForModule >= 0 && captured < 0)
                captured = vk;
        }
        if (captured > 0 && g_capturingBindForModule >= 0) {
            if (captured == VK_ESCAPE) {
                g_capturingBindForModule = -1;
            } else if (captured != VK_LBUTTON && captured != VK_RBUTTON && captured != VK_MBUTTON &&
                       captured != VK_SHIFT   && captured != VK_CONTROL  && captured != VK_MENU   &&
                       captured != VK_LSHIFT  && captured != VK_RSHIFT   &&
                       captured != VK_LCONTROL&& captured != VK_RCONTROL &&
                       captured != VK_LMENU   && captured != VK_RMENU) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"cmd\",\"action\":\"setKeybind\",\"module\":\"%s\",\"key\":%d}\n",
                    g_modules[g_capturingBindForModule].id, captured);
                LockGuard lk(g_cmdMutex);
                g_pendingCommands.push_back(buf);
                g_capturingBindForModule = -1;
            }
        }
    }

    // Dim background
    DrawRect(0, 0, (float)winW, (float)winH, 0, 0, 0, 0.82f);

    float scale = 0.66f;
    float panelW = 640.0f, panelH = 460.0f;
    if (!g_guiWindowInit) {
        g_guiWindowX = (winW - panelW) * 0.5f;
        g_guiWindowY = (winH - panelH) * 0.5f;
        g_guiWindowInit = true;
    }

    if (g_guiWindowX < 8.0f) g_guiWindowX = 8.0f;
    if (g_guiWindowY < 8.0f) g_guiWindowY = 8.0f;
    if (g_guiWindowX + panelW > winW - 8.0f) g_guiWindowX = winW - panelW - 8.0f;
    if (g_guiWindowY + panelH > winH - 8.0f) g_guiWindowY = winH - panelH - 8.0f;

    float px = g_guiWindowX;
    float py = g_guiWindowY;
    float headerH = 34.0f;
    float closeW = 16.0f;
    Color3 accent = AccentColor(0.0f);

    float closeX = px + panelW - closeW - 10.0f;
    float closeY = py + 9.0f;
    bool closeHovered = IsPointInRect(closeX, closeY, closeW, closeW);

    if (g_mouseClicked && IsPointInRect(px, py, panelW, headerH) && !closeHovered) {
        g_guiDragging = true;
        g_guiDragOffsetX = g_mouseX - px;
        g_guiDragOffsetY = g_mouseY - py;
    }
    if (!g_mouseDown) g_guiDragging = false;
    if (g_guiDragging) {
        px = g_mouseX - g_guiDragOffsetX;
        py = g_mouseY - g_guiDragOffsetY;
        g_guiWindowX = px;
        g_guiWindowY = py;
        if (g_guiWindowX < 8.0f) g_guiWindowX = 8.0f;
        if (g_guiWindowY < 8.0f) g_guiWindowY = 8.0f;
        if (g_guiWindowX + panelW > winW - 8.0f) g_guiWindowX = winW - panelW - 8.0f;
        if (g_guiWindowY + panelH > winH - 8.0f) g_guiWindowY = winH - panelH - 8.0f;
        px = g_guiWindowX;
        py = g_guiWindowY;
        closeX = px + panelW - closeW - 10.0f;
        closeY = py + 9.0f;
    }

    // Main panel
    DrawRect(px, py, panelW, panelH, 0.10f, 0.10f, 0.11f, 0.98f);
    DrawRect(px, py, panelW, headerH, 0.08f, 0.08f, 0.09f, 0.98f);
    DrawRect(px, py, 2.0f, panelH, accent.r, accent.g, accent.b, 1.0f);
    DrawRect(px, py + headerH - 1.0f, panelW, 1.0f, 0.17f, 0.17f, 0.18f, 1.0f);

    DrawText2D(px + 12.0f, py + 8.0f, "LegoClicker", 0.93f, 0.94f, 0.96f, 1.0f, scale * 1.05f);
    DrawText2D(px + 130.0f, py + 9.0f, "Internal ClickGUI", 0.45f, 0.45f, 0.50f, 1.0f, scale * 0.78f);

    DrawRect(closeX, closeY, closeW, closeW, closeHovered ? 0.62f : 0.22f, 0.12f, 0.15f, 0.92f);
    DrawText2D(closeX + 4.5f, closeY + 1.0f, "X", 0.98f, 0.93f, 0.95f, 1.0f, scale * 0.8f);
    if (closeHovered && g_mouseClicked) {
        CloseInternalGui();
        return;
    }

    // Layout columns
    float sidebarW = 118.0f;
    float modulesW = 238.0f;
    float gap = 8.0f;
    float contentY = py + headerH + 8.0f;
    float contentH = panelH - headerH - 16.0f;
    float sideX = px + 8.0f;
    float listX = sideX + sidebarW + gap;
    float settingsX = listX + modulesW + gap;
    float settingsW = panelW - (settingsX - px) - 8.0f;

    DrawRect(sideX, contentY, sidebarW, contentH, 0.08f, 0.08f, 0.09f, 0.95f);
    DrawRect(listX, contentY, modulesW, contentH, 0.08f, 0.08f, 0.09f, 0.95f);
    DrawRect(settingsX, contentY, settingsW, contentH, 0.08f, 0.08f, 0.09f, 0.95f);

    // Sidebar categories
    static const char* categoryNames[] = { "Combat", "Render" };
    float catY = contentY + 8.0f;
    for (int i = 0; i < 2; i++) {
        float btnH = 28.0f;
        bool hovered = IsPointInRect(sideX + 6.0f, catY, sidebarW - 12.0f, btnH);
        bool active = (int)g_selectedCategory == i;
        float bg = active ? 0.15f : (hovered ? 0.12f : 0.10f);
        DrawRect(sideX + 6.0f, catY, sidebarW - 12.0f, btnH, bg, bg, bg + 0.01f, 0.97f);
        if (active) DrawRect(sideX + 6.0f, catY, 2.0f, btnH, accent.r, accent.g, accent.b, 1.0f);
        DrawText2D(sideX + 15.0f, catY + 7.0f, categoryNames[i], active ? 0.93f : 0.70f, active ? 0.94f : 0.70f, active ? 0.96f : 0.74f, 1.0f, scale * 0.86f);
        if (hovered && g_mouseClicked) g_selectedCategory = (ClickCategory)i;
        catY += btnH + 6.0f;
    }

    float hudBaseY = contentY + contentH - 120.0f;
    DrawText2D(sideX + 9.0f, hudBaseY - 16.0f, "HUD Style", 0.60f, 0.62f, 0.68f, 1.0f, scale * 0.74f);
    if (GuiSettingToggle(sideX + 6.0f, hudBaseY, sidebarW - 12.0f, "Chroma", g_uiState.chromaText, scale * 0.7f)) {
        g_uiState.chromaText = !g_uiState.chromaText;
    }
    g_uiState.accentHue = GuiSettingSlider(sideX + 6.0f, hudBaseY + 26.0f, sidebarW - 12.0f, "accent_hue", "Accent", g_uiState.accentHue, 0.0f, 1.0f, scale * 0.7f, false);
    g_uiState.chromaSpeed = GuiSettingSlider(sideX + 6.0f, hudBaseY + 58.0f, sidebarW - 12.0f, "chroma_speed", "Speed", g_uiState.chromaSpeed, 0.01f, 0.20f, scale * 0.7f, false);

    Config cfg; { LockGuard lk(g_configMutex); cfg = g_config; }
    std::vector<std::string> frameCmds;
    auto queueCmd = [&](const std::string& c) {
        frameCmds.push_back(c);
    };
    auto queueToggle = [&](const char* action) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"type\":\"cmd\",\"action\":\"%s\"}\n", action);
        queueCmd(buf);
    };

    // Module list for selected category
    DrawText2D(listX + 9.0f, contentY + 6.0f, "Modules", 0.60f, 0.62f, 0.68f, 1.0f, scale * 0.74f);
    float rowY = contentY + 24.0f;
    for (int i = 0; i < (int)(sizeof(g_modules) / sizeof(g_modules[0])); i++) {
        const ModuleSpec& m = g_modules[i];
        if (m.category != g_selectedCategory) continue;

        bool leftPressed = false;
        bool rightPressed = false;
        bool enabled = IsModuleEnabled(m, cfg);
        DrawModuleButton(listX + 8.0f, rowY, modulesW - 16.0f, m, enabled, g_selectedModuleIdx == i, scale * 0.9f, &leftPressed, &rightPressed);

        if (leftPressed) queueToggle(m.action);
        if (rightPressed) g_selectedModuleIdx = i;
        rowY += CHAR_H * scale * 0.9f + 14.0f;
    }

    // Settings panel for selected module
    DrawText2D(settingsX + 9.0f, contentY + 6.0f, "Settings", 0.60f, 0.62f, 0.68f, 1.0f, scale * 0.74f);
    int moduleCount = (int)(sizeof(g_modules) / sizeof(g_modules[0]));
    if (g_selectedModuleIdx < 0 || g_selectedModuleIdx >= moduleCount) g_selectedModuleIdx = 0;
    const ModuleSpec& selected = g_modules[g_selectedModuleIdx];
    DrawText2D(settingsX + 9.0f, contentY + 24.0f, selected.name, 0.90f, 0.92f, 0.95f, 1.0f, scale * 0.93f);

    // Keybind row — fixed at the bottom of the settings column
    int selectedModuleGlobalIdx = g_selectedModuleIdx;
    {
        float bindY = contentY + contentH - 52.0f;
        // Separator line
        DrawRect(settingsX + 8.0f, bindY - 6.0f, settingsW - 16.0f, 1.0f, 0.20f, 0.20f, 0.23f, 0.75f);
        int bindResult = GuiKeybindButton(settingsX + 8.0f, bindY, settingsW - 16.0f,
                                         GetModuleKeybind(cfg, selected.id),
                                         selectedModuleGlobalIdx, scale * 0.88f);
        if (bindResult == 1) {
            // Start capture
            g_capturingBindForModule = selectedModuleGlobalIdx;
        } else if (bindResult == 2) {
            // Unbind (right-click)
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"cmd\",\"action\":\"setKeybind\",\"module\":\"%s\",\"key\":0}\n",
                selected.id);
            queueCmd(buf);
        }
    }

    float sy = contentY + 44.0f;
    if (strcmp(selected.id, "autoclicker") == 0) {
        float minVal = GuiSettingSlider(settingsX + 8.0f, sy, settingsW - 16.0f, "min_cps", "Min CPS", cfg.minCPS, 1.0f, 20.0f, scale * 0.88f, true);
        sy += 32.0f;
        float maxVal = GuiSettingSlider(settingsX + 8.0f, sy, settingsW - 16.0f, "max_cps", "Max CPS", cfg.maxCPS, 1.0f, 20.0f, scale * 0.88f, true);
        sy += 32.0f;

        int minInt = (int)(minVal + 0.5f);
        int maxInt = (int)(maxVal + 0.5f);
        if (minInt > maxInt) {
            maxInt = minInt;
        }
        int curMinInt = (int)(cfg.minCPS + 0.5f);
        int curMaxInt = (int)(cfg.maxCPS + 0.5f);

        if (minInt != curMinInt) {
            char b[128];
            snprintf(b, sizeof(b), "{\"type\":\"cmd\",\"action\":\"setMinCPS\",\"value\":%d}\n", minInt);
            queueCmd(b);
        }
        if (maxInt != curMaxInt) {
            char b[128];
            snprintf(b, sizeof(b), "{\"type\":\"cmd\",\"action\":\"setMaxCPS\",\"value\":%d}\n", maxInt);
            queueCmd(b);
        }

        if (GuiSettingToggle(settingsX + 8.0f, sy, settingsW - 16.0f, "Left Click", cfg.leftClick, scale * 0.88f)) queueToggle("toggleLeft");
        sy += 25.0f;
        
        float rMinVal = GuiSettingSlider(settingsX + 8.0f, sy, settingsW - 16.0f, "rmin_cps", "R Min CPS", cfg.rightMinCPS, 1.0f, 20.0f, scale * 0.88f, true);
        sy += 32.0f;
        float rMaxVal = GuiSettingSlider(settingsX + 8.0f, sy, settingsW - 16.0f, "rmax_cps", "R Max CPS", cfg.rightMaxCPS, 1.0f, 20.0f, scale * 0.88f, true);
        sy += 32.0f;

        int rMinInt = (int)(rMinVal + 0.5f);
        int rMaxInt = (int)(rMaxVal + 0.5f);
        if (rMinInt > rMaxInt) rMaxInt = rMinInt;
        int curRMinInt = (int)(cfg.rightMinCPS + 0.5f);
        int curRMaxInt = (int)(cfg.rightMaxCPS + 0.5f);

        if (rMinInt != curRMinInt) {
            char b[128];
            snprintf(b, sizeof(b), "{\"type\":\"cmd\",\"action\":\"setRightMinCPS\",\"value\":%d}\n", rMinInt);
            queueCmd(b);
        }
        if (rMaxInt != curRMaxInt) {
            char b[128];
            snprintf(b, sizeof(b), "{\"type\":\"cmd\",\"action\":\"setRightMaxCPS\",\"value\":%d}\n", rMaxInt);
            queueCmd(b);
        }

        if (GuiSettingToggle(settingsX + 8.0f, sy, settingsW - 16.0f, "Right Click", cfg.rightClick, scale * 0.88f)) queueToggle("toggleRight");
        sy += 25.0f;
        if (GuiSettingToggle(settingsX + 8.0f, sy, settingsW - 16.0f, "Block Only", cfg.rightBlockOnly, scale * 0.88f)) queueToggle("toggleRightBlockOnly");
        sy += 25.0f;
        if (GuiSettingToggle(settingsX + 8.0f, sy, settingsW - 16.0f, "Break Blocks", cfg.breakBlocks, scale * 0.88f)) queueToggle("toggleBreakBlocks");
        sy += 25.0f;
        if (GuiSettingToggle(settingsX + 8.0f, sy, settingsW - 16.0f, "Jitter", cfg.jitter, scale * 0.88f)) queueToggle("toggleJitter");
        sy += 25.0f;
        if (GuiSettingToggle(settingsX + 8.0f, sy, settingsW - 16.0f, "Click In Chests", cfg.clickInChests, scale * 0.88f)) queueToggle("toggleClickInChests");
    } else if (strcmp(selected.id, "nametags") == 0) {
        if (GuiSettingToggle(settingsX + 8.0f, sy, settingsW - 16.0f, "Show Health", cfg.nametagShowHealth, scale * 0.88f)) queueToggle("toggleNametagHealth");
        sy += 30.0f;
        if (GuiSettingToggle(settingsX + 8.0f, sy, settingsW - 16.0f, "Show Armor", cfg.nametagShowArmor, scale * 0.88f)) queueToggle("toggleNametagArmor");
        sy += 34.0f;
        DrawText2D(settingsX + 9.0f, sy, "Armor is displayed as points.", 0.54f, 0.56f, 0.61f, 1.0f, scale * 0.72f);
        sy += 18.0f;
        DrawText2D(settingsX + 9.0f, sy, "Shown alongside player names.", 0.54f, 0.56f, 0.61f, 1.0f, scale * 0.72f);
    } else if (strcmp(selected.id, "closestplayer") == 0) {
        DrawText2D(settingsX + 9.0f, sy, "Player HUD above the hotbar.", 0.60f, 0.62f, 0.68f, 1.0f, scale * 0.74f);
        sy += 20.0f;
        DrawText2D(settingsX + 9.0f, sy, "Distance and direction:", 0.54f, 0.56f, 0.61f, 1.0f, scale * 0.72f);
        sy += 16.0f;
        DrawText2D(settingsX + 9.0f, sy, "Front / Left / Right / Back", 0.54f, 0.56f, 0.61f, 1.0f, scale * 0.72f);
        sy += 18.0f;
        DrawText2D(settingsX + 9.0f, sy, "Follows Nametag health/armor.", 0.54f, 0.56f, 0.61f, 1.0f, scale * 0.72f);
    } else {
        DrawText2D(settingsX + 9.0f, sy, "RMB a module for its settings.", 0.62f, 0.62f, 0.67f, 1.0f, scale * 0.72f);
        sy += 20.0f;
        DrawText2D(settingsX + 9.0f, sy, "No extra settings.", 0.50f, 0.50f, 0.55f, 1.0f, scale * 0.72f);
    }

    DrawText2D(px + 10.0f, py + panelH - 22.0f, "Drag header to move  |  [RMB] keybind = unbind", 0.44f, 0.44f, 0.49f, 1.0f, scale * 0.73f);

    if (!frameCmds.empty()) {
        LockGuard lk(g_cmdMutex);
        for (const std::string& c : frameCmds) g_pendingCommands.push_back(c);
    }
}

// ===================== INPUT HOOK =====================
// WndProc only handles mouse for ClickGUI. Keyboard uses GetAsyncKeyState.
LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    bool leftNowDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rawInputClickEdge = (msg == WM_INPUT) && leftNowDown && !g_reachRawInputPrevDown;
    if (msg == WM_INPUT) {
        g_reachRawInputPrevDown = leftNowDown;
    }

    if ((msg == WM_LBUTTONDOWN || rawInputClickEdge) && !g_guiOpen) {
        Config cfgSnapshot;
        {
            LockGuard lk(g_configMutex);
            cfgSnapshot = g_config;
        }

        if (cfgSnapshot.reachEnabled) {
            GameState stateSnapshot;
            {
                LockGuard lk(g_stateMutex);
                stateSnapshot = g_gameState;
            }

            JNIEnv* env = nullptr;
            bool attached = false;
            if (g_jvm && g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) != JNI_OK) {
                if (g_jvm->AttachCurrentThread((void**)&env, nullptr) == JNI_OK) {
                    attached = true;
                } else {
                    env = nullptr;
                }
            }
            if (env) {
                LockGuard jniLk(g_jniMutex);
                UpdateReach(env, cfgSnapshot, stateSnapshot, true);
            }
            if (attached && g_jvm) g_jvm->DetachCurrentThread();
        }
    }

    switch (msg) {
    case WM_MOUSEMOVE:
        g_mouseX = LOWORD(lParam); g_mouseY = HIWORD(lParam); 
        if (g_guiOpen) return 0; // BLOCK input to game
        break;
    case WM_LBUTTONDOWN:
        g_mouseClicked = true; g_mouseDown = true;
        if (g_guiOpen) return 0;
        break;
    case WM_LBUTTONUP:
        g_mouseDown = false;
        if (g_guiOpen) return 0;
        break;
    case WM_RBUTTONDOWN:
        g_mouseRightClicked = true;
        g_mouseRightDown = true;
        if (g_guiOpen) return 0;
        break;
    case WM_RBUTTONUP:
        g_mouseRightDown = false;
        if (g_guiOpen) return 0;
        break;
    case WM_MOUSEWHEEL:
        g_scrollDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (g_guiOpen) return 0;
        break;
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
}

void CloseInternalGui() {
    if (!g_guiOpen) return;
    g_guiOpen = false;
    g_guiDragging = false;
    g_capturingBindForModule = -1; // cancel any in-progress keybind capture
    Log("ClickGUI closed");

    if (!g_nativeChatOpenedByClickGui) return;
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_OK || g_jvm->AttachCurrentThread((void**)&env, nullptr) == JNI_OK) {
        if (g_mcInstance && g_displayGuiScreenMethod) {
            env->CallVoidMethod(g_mcInstance, g_displayGuiScreenMethod, nullptr);
        }
    }
    g_nativeChatOpenedByClickGui = false;
}

void OpenInternalGui() {
    if (g_guiOpen) return;
    g_guiOpen = true;
    g_nativeChatOpenedByClickGui = false;
    g_wasMouseDown = false;
    g_wasMouseRightDown = false;
    g_mouseClicked = false;
    g_mouseRightClicked = false;
    Log("ClickGUI opened");
}

void PollKeyboardToggle() {}

void EnsureWndProcHook(HWND targetHwnd) {
    if (!targetHwnd) return;
    if (g_wndProcHookedHwnd == targetHwnd && g_origWndProc) return;

    if (g_wndProcHookedHwnd && g_origWndProc) {
        SetWindowLongPtrA(g_wndProcHookedHwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
    }

    WNDPROC previous = (WNDPROC)SetWindowLongPtrA(targetHwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    if (previous) {
        g_origWndProc = previous;
        g_wndProcHookedHwnd = targetHwnd;
        Log("WndProc hooked/refreshed");
    }
}

// ===================== SWAPBUFFERS HOOK (IAT) =====================
BOOL WINAPI HookedSwapBuffers(HDC hdc) {
    // First-time init
    if (!g_glInitialized) InitFont();

    if (g_glInitialized) {
        HWND currentHwnd = WindowFromDC(hdc);
        if (currentHwnd) g_gameHwnd = currentHwnd;
        EnsureWndProcHook(currentHwnd);

        // Get window size
        RECT rect; GetClientRect(WindowFromDC(hdc), &rect);
        int w = rect.right, h = rect.bottom;

        if (w > 0 && h > 0) {
            // Save GL state
            glPushAttrib(GL_ALL_ATTRIB_BITS);
            glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
            glOrtho(0, w, h, 0, -1, 1);
            glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

            glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            RenderHUD(w, h);
            {
                LockGuard jniLk(g_jniMutex);
                RenderNametags(w, h);
                RenderClosestPlayerInfo(w, h);
                RenderChestESP(w, h);
            }

            // Restore GL state
            glMatrixMode(GL_PROJECTION); glPopMatrix();
            glMatrixMode(GL_MODELVIEW); glPopMatrix();
            glPopAttrib();
        }
    }

    // Call original SwapBuffers via saved pointer (IAT hook - no trampoline)
    return g_origSwapBuffers(hdc);
}

// IAT hook: patch import table entries for SwapBuffers in a specific module
bool PatchIAT(HMODULE hModule, const char* targetDll, const char* funcName, void* hookFunc, void** origFunc) {
    BYTE* base = (BYTE*)hModule;
    IMAGE_DOS_HEADER* dosHdr = (IMAGE_DOS_HEADER*)base;
    if (dosHdr->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* ntHdr = (IMAGE_NT_HEADERS*)(base + dosHdr->e_lfanew);
    if (ntHdr->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_DATA_DIRECTORY* importDir = &ntHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir->VirtualAddress || !importDir->Size) return false;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir->VirtualAddress);
    for (; imp->Name; imp++) {
        const char* dllName = (const char*)(base + imp->Name);
        if (lstrcmpiA(dllName, targetDll) != 0) continue;

        IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* firstThunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

        for (; origThunk->u1.AddressOfData; origThunk++, firstThunk++) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            IMAGE_IMPORT_BY_NAME* importByName = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
            if (strcmp((const char*)importByName->Name, funcName) == 0) {
                *origFunc = (void*)firstThunk->u1.Function;
                DWORD oldProt;
                VirtualProtect(&firstThunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
                firstThunk->u1.Function = (ULONGLONG)hookFunc;
                VirtualProtect(&firstThunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                return true;
            }
        }
    }
    return false;
}

void InstallSwapBuffersHook() {
    // Enumerate ALL loaded modules and find the one that imports SwapBuffers
    HANDLE hProc = GetCurrentProcess();
    HMODULE hMods[1024];
    DWORD cbNeeded = 0;
    bool hooked = false;

    // Use K32EnumProcessModules (available since Windows 7)
    typedef BOOL(WINAPI* EnumModsFn)(HANDLE, HMODULE*, DWORD, LPDWORD);
    HMODULE hPsapi = GetModuleHandleA("kernel32.dll");
    EnumModsFn enumMods = (EnumModsFn)GetProcAddress(hPsapi, "K32EnumProcessModules");
    if (!enumMods) {
        hPsapi = LoadLibraryA("psapi.dll");
        if (hPsapi) enumMods = (EnumModsFn)GetProcAddress(hPsapi, "EnumProcessModules");
    }

    if (enumMods && enumMods(hProc, hMods, sizeof(hMods), &cbNeeded)) {
        int modCount = cbNeeded / sizeof(HMODULE);
        Log("Scanning " + std::to_string(modCount) + " modules for SwapBuffers import...");

        for (int i = 0; i < modCount && !hooked; i++) {
            // Skip our own DLL and known system DLLs that we don't want to hook
            char modName[MAX_PATH] = "";
            GetModuleFileNameA(hMods[i], modName, MAX_PATH);
            std::string name = modName;
            
            // Convert to lowercase for comparison
            std::string nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            
            // Skip system DLLs that won't have SwapBuffers
            if (nameLower.find("\\windows\\") != std::string::npos &&
                nameLower.find("opengl32.dll") == std::string::npos) continue;

            void* origFunc = nullptr;
            const char* gdiVariants[] = { "gdi32.dll", "GDI32.dll", "GDI32.DLL", "Gdi32.dll", nullptr };
            for (int g = 0; gdiVariants[g] && !hooked; g++) {
                if (PatchIAT(hMods[i], gdiVariants[g], "SwapBuffers", (void*)HookedSwapBuffers, &origFunc)) {
                    g_origSwapBuffers = (SwapBuffersFn)origFunc;
                    // Get just the filename for logging
                    std::string shortName = name;
                    size_t lastSlash = shortName.find_last_of("\\/");
                    if (lastSlash != std::string::npos) shortName = shortName.substr(lastSlash + 1);
                    Log("IAT hooked SwapBuffers in: " + shortName + " (via " + gdiVariants[g] + ")");
                    hooked = true;
                }
            }
        }
    } else {
        Log("WARNING: EnumProcessModules failed, trying known modules...");
        // Fallback to known module names
        const char* modules[] = { "opengl32.dll", "lwjgl.dll", "lwjgl64.dll", nullptr };
        for (int i = 0; modules[i] && !hooked; i++) {
            HMODULE hMod = GetModuleHandleA(modules[i]);
            if (!hMod) continue;
            void* origFunc = nullptr;
            if (PatchIAT(hMod, "gdi32.dll", "SwapBuffers", (void*)HookedSwapBuffers, &origFunc)) {
                g_origSwapBuffers = (SwapBuffersFn)origFunc;
                Log(std::string("IAT hooked SwapBuffers in ") + modules[i]);
                hooked = true;
            }
        }
    }

    if (!hooked) {
        HMODULE hGdi = GetModuleHandleA("gdi32.dll");
        g_origSwapBuffers = (SwapBuffersFn)GetProcAddress(hGdi, "SwapBuffers");
        Log("WARNING: Could not install IAT hook on any module. HUD will not render.");
    } else {
        Log("SwapBuffers hook installed successfully");
    }
}

// ===================== TCP SERVER =====================
void ParseConfig(const std::string& line) {
    // Simple JSON parsing for config updates
    auto getStr = [&](const char* key) -> std::string {
        std::string k = std::string("\"") + key + "\":";
        size_t p = line.find(k);
        if (p == std::string::npos) return "";
        p += k.length();
        if (line[p] == '"') { size_t e = line.find('"', p+1); return line.substr(p+1, e-p-1); }
        size_t e = line.find_first_of(",}", p);
        return line.substr(p, e-p);
    };
    auto getBool = [&](const char* key) -> bool { return getStr(key) == "true"; };
    auto getFloat = [&](const char* key) -> float { std::string v = getStr(key); return v.empty() ? 0 : std::stof(v); };
    auto getInt   = [&](const char* key) -> int   { std::string v = getStr(key); return v.empty() ? -1 : std::stoi(v); };

    std::string type = getStr("type");
    if (type == "config") {
        LockGuard lk(g_configMutex);
        g_config.armed = getBool("armed");
        g_config.clicking = getBool("clicking");
        g_config.minCPS = getFloat("minCPS");
        g_config.maxCPS = getFloat("maxCPS");
        g_config.leftClick = getBool("left");
        g_config.rightClick = getBool("right");
        g_config.rightMinCPS = getFloat("rightMinCPS");
        g_config.rightMaxCPS = getFloat("rightMaxCPS");
        g_config.rightBlockOnly = getBool("rightBlock");
        g_config.breakBlocks = getBool("breakBlocks");
        g_config.jitter = getBool("jitter");
        g_config.clickInChests = getBool("clickInChests");
        g_config.aimAssist = getBool("aimAssist");
        g_config.triggerbot = getBool("triggerbot");
        g_config.gtbHelper = getBool("gtbHelper");
        g_config.nametags = getBool("nametags");
        g_config.closestPlayerInfo = getBool("closestPlayerInfo");
        g_config.nametagShowHealth = getBool("nametagShowHealth");
        g_config.nametagShowArmor = getBool("nametagShowArmor");
        g_config.chestEsp = getBool("chestEsp");
        g_config.reachEnabled = getBool("reachEnabled");
        g_config.velocityEnabled = getBool("velocityEnabled");

        std::string showModuleListRaw = getStr("showModuleList");
        g_config.showModuleList = showModuleListRaw.empty() ? true : (showModuleListRaw == "true");

        int style = getInt("moduleListStyle");
        if (style >= 0) g_config.moduleListStyle = (std::max)(0, (std::min)(4, style));

        std::string showLogoRaw = getStr("showLogo");
        g_config.showLogo = showLogoRaw.empty() ? true : (showLogoRaw == "true");

        std::string guiThemeRaw = getStr("guiTheme");
        g_config.guiTheme = guiThemeRaw.empty() ? "Default" : guiThemeRaw;

        g_config.nametagShowHealth = getBool("nametagShowHealth");
        g_config.nametagShowArmor = getBool("nametagShowArmor");

        int nametagMaxCount = getInt("nametagMaxCount");
        if (nametagMaxCount < 1) nametagMaxCount = g_config.nametagMaxCount;
        if (nametagMaxCount > 20) nametagMaxCount = 20;
        g_config.nametagMaxCount = nametagMaxCount;

        int chestEspMaxCount = getInt("chestEspMaxCount");
        if (chestEspMaxCount < 1) chestEspMaxCount = g_config.chestEspMaxCount;
        if (chestEspMaxCount > 20) chestEspMaxCount = 20;
        g_config.chestEspMaxCount = chestEspMaxCount;

        int reachChance = getInt("reachChance");
        if (reachChance < 1) reachChance = g_config.reachChance;
        if (reachChance > 100) reachChance = 100;
        g_config.reachChance = reachChance;

        int velocityHorizontal = getInt("velocityHorizontal");
        if (velocityHorizontal < 1) velocityHorizontal = g_config.velocityHorizontal;
        if (velocityHorizontal > 100) velocityHorizontal = 100;
        g_config.velocityHorizontal = velocityHorizontal;

        int velocityVertical = getInt("velocityVertical");
        if (velocityVertical < 1) velocityVertical = g_config.velocityVertical;
        if (velocityVertical > 100) velocityVertical = 100;
        g_config.velocityVertical = velocityVertical;

        int velocityChance = getInt("velocityChance");
        if (velocityChance < 1) velocityChance = g_config.velocityChance;
        if (velocityChance > 100) velocityChance = 100;
        g_config.velocityChance = velocityChance;

        std::string gtbHint = getStr("gtbHint");
        int gtbCount = getInt("gtbCount");
        if (gtbCount < 0) gtbCount = g_config.gtbCount;
        std::string gtbPreview = getStr("gtbPreview");
        if (!gtbHint.empty()) g_config.gtbHint = gtbHint;
        g_config.gtbCount = gtbCount;
        if (!gtbPreview.empty()) g_config.gtbPreview = gtbPreview;

        float reachMin = getFloat("reachMin");
        float reachMax = getFloat("reachMax");
        if (reachMin <= 0.0f) reachMin = g_config.reachMin;
        if (reachMax < reachMin) reachMax = reachMin;
        g_config.reachMin = reachMin;
        g_config.reachMax = reachMax;

        static bool lastNametagsLogged = false;
        if (g_config.nametags != lastNametagsLogged) {
            lastNametagsLogged = g_config.nametags;
            Log(std::string("Config: nametags=") + (g_config.nametags ? "true" : "false"));
        }

        static bool loggedExtendedFields = false;
        if (!loggedExtendedFields) {
            loggedExtendedFields = true;
            Log("Config parser accepts extended parity fields (GTB/Reach/Velocity/Caps).");
        }

        // Per-module keybinds (-1 means absent / don't override)
        { int v = getInt("keybindAutoclicker");  if (v >= 0) g_config.keybindAutoclicker  = v; }
        { int v = getInt("keybindNametags");      if (v >= 0) g_config.keybindNametags      = v; }
        { int v = getInt("keybindClosestPlayer"); if (v >= 0) g_config.keybindClosestPlayer = v; }
        { int v = getInt("keybindChestEsp");      if (v >= 0) g_config.keybindChestEsp      = v; }
    }
}

bool TrySendCapabilities(SOCKET sock) {
    if (sock == INVALID_SOCKET) return false;

    static const char* kCapabilitiesJson =
        "{\"type\":\"capabilities\","
        "\"modules\":[\"autoclicker\",\"rightclick\",\"jitter\",\"clickinchests\",\"breakblocks\",\"aimassist\",\"gtbhelper\",\"nametags\",\"closestplayer\",\"chestesp\",\"reach\",\"velocity\"],"
        "\"settings\":[\"mincps\",\"maxcps\",\"left\",\"right\",\"rightmincps\",\"rightmaxcps\",\"rightblock\",\"breakblocks\",\"jitter\",\"clickinchests\",\"aimassistfov\",\"aimassistrange\",\"aimassiststrength\",\"nametags\",\"closestplayerinfo\",\"nametagshowhealth\",\"nametagshowarmor\",\"nametagmaxcount\",\"chestesp\",\"chestespmaxcount\",\"reachenabled\",\"reachmin\",\"reachmax\",\"reachchance\",\"velocityenabled\",\"velocityhorizontal\",\"velocityvertical\",\"velocitychance\",\"gtbhint\",\"gtbcount\",\"gtbpreview\",\"showmodulelist\",\"moduleliststyle\",\"showlogo\",\"guitheme\",\"keybindautoclicker\",\"keybindnametags\",\"keybindclosestplayer\",\"keybindchestesp\"],"
        "\"state\":[\"actionbar\",\"holdingblock\",\"lookingatblock\",\"lookingatentity\",\"lookingatentitylatched\",\"breakingblock\",\"attackcooldown\",\"attackcooldownpertick\",\"statems\"]}\n";

    int sent = send(sock, kCapabilitiesJson, (int)strlen(kCapabilitiesJson), 0);
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return false;
        Log("Capabilities send failed, err=" + std::to_string(err));
        return false;
    }
    return true;
}

void ServerLoop() {
    WSADATA wsaData; WSAStartup(MAKEWORD(2, 2), &wsaData);
    g_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(g_serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr = {}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(25590);
    bind(g_serverSocket, (sockaddr*)&addr, sizeof(addr));
    listen(g_serverSocket, 1);

    // FIX: Force C locale for correct JSON float formatting (dots not commas)
    setlocale(LC_NUMERIC, "C"); 

    JNIEnv* env; JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_8; args.name = (char*)"LegoBridge"; args.group = nullptr;
    g_jvm->AttachCurrentThread((void**)&env, &args);

    Log("Discovering classes...");
    bool mapped = DiscoverMappings(env);
    Log(mapped ? "Discovery OK" : "Discovery FAILED");

    while (g_running) {
        Log("Waiting for client...");
        g_clientSocket = accept(g_serverSocket, nullptr, nullptr);
        if (g_clientSocket == INVALID_SOCKET) { if (!g_running) break; continue; }
        Log("Client connected");

        // Set non-blocking for reading config from C#
        u_long mode = 1; ioctlsocket(g_clientSocket, FIONBIO, &mode);

        std::string readBuf;
        bool capabilitiesSent = false;
        while (g_running) {
            if (!capabilitiesSent) {
                capabilitiesSent = TrySendCapabilities(g_clientSocket);
                if (capabilitiesSent) Log("Sent bridge capabilities packet");
            }

            
            // Check config
            bool nametagsEnabled = false;
            bool needEntityTelemetry = false;
            Config cfgSnapshot;
            {
                LockGuard lk(g_configMutex);
                nametagsEnabled = g_config.nametags;
                needEntityTelemetry = g_config.nametags || g_config.closestPlayerInfo || g_config.aimAssist;
                cfgSnapshot = g_config;
            }

            // ALWAYS Read Game State to update global state (for block detection etc.)
            GameState state;
            {
                LockGuard jniLk(g_jniMutex);
                state = ReadGameState(env);
                if (cfgSnapshot.reachEnabled || g_reachAllowCurrentClick || g_reachClickPrevDown) {
                    UpdateReach(env, cfgSnapshot, state);
                }
                if (cfgSnapshot.velocityEnabled || g_lastHurtTime > 0) {
                    UpdateVelocity(env, cfgSnapshot);
                }
            }
            if (!cfgSnapshot.reachEnabled) {
                g_reachAllowCurrentClick = false;
                g_reachCurrentClickRange = 3.0;
                if (g_reachCurrentTarget) {
                    LockGuard jniLk(g_jniMutex);
                    env->DeleteGlobalRef(g_reachCurrentTarget);
                    g_reachCurrentTarget = nullptr;
                }
            }
            if (!cfgSnapshot.velocityEnabled) {
                g_lastHurtTime = 0;
            }
            { LockGuard lk(g_stateMutex); g_gameState = state; }

            // Standard Logic: Build JSON from state
            std::string jsonToSend = "{";
            jsonToSend += "\"mapped\":" + std::string(state.mapped ? "true" : "false") + ",";
            jsonToSend += "\"guiOpen\":" + std::string(state.guiOpen ? "true" : "false") + ",";
            jsonToSend += "\"screenName\":\"" + JsonEscape(state.screenName) + "\",";
            jsonToSend += "\"actionBar\":\"" + JsonEscape(state.actionBar) + "\",";
            jsonToSend += "\"health\":" + std::to_string(state.health) + ",";
            jsonToSend += "\"posX\":" + std::to_string(state.posX) + ",";
            jsonToSend += "\"posY\":" + std::to_string(state.posY) + ",";
            jsonToSend += "\"posZ\":" + std::to_string(state.posZ) + ",";
            jsonToSend += "\"holdingBlock\":" + std::string(state.holdingBlock ? "true" : "false") + ",";
            jsonToSend += "\"lookingAtBlock\":" + std::string(state.lookingAtBlock ? "true" : "false") + ",";
            jsonToSend += "\"lookingAtEntity\":" + std::string(state.lookingAtEntity ? "true" : "false") + ",";
            jsonToSend += "\"lookingAtEntityLatched\":" + std::string(state.lookingAtEntityLatched ? "true" : "false") + ",";
            jsonToSend += "\"breakingBlock\":" + std::string(state.breakingBlock ? "true" : "false") + ",";
            jsonToSend += "\"attackCooldown\":" + std::to_string(state.attackCooldown) + ",";
            jsonToSend += "\"attackCooldownPerTick\":" + std::to_string(state.attackCooldownPerTick) + ",";
            jsonToSend += "\"stateMs\":" + std::to_string(state.stateMs) + ",";
            jsonToSend += "\"entities\":";

            std::string entitiesJson = "[]";

            if (needEntityTelemetry) {
                LockGuard lk(g_jsonMutex);
                if (!g_pendingJson.empty()) {
                    entitiesJson = g_pendingJson;
                    g_pendingJson.clear();
                }
            }
            jsonToSend += entitiesJson;
            jsonToSend += "}\n";

            // Send if we have data
            if (!jsonToSend.empty()) {
                LockGuard lk(g_socketMutex);
                int sent = send(g_clientSocket, jsonToSend.c_str(), (int)jsonToSend.length(), 0);
                if (sent == SOCKET_ERROR) {
                   if (WSAGetLastError() != WSAEWOULDBLOCK) break; 
                }
            }
            
            // Keep telemetry responsive without pegging CPU.
            Sleep(needEntityTelemetry ? 6 : 12);



            // Send pending commands from ClickGUI
            // FIX: Copy commands inside lock, send OUTSIDE lock to avoid DEADLOCK
            std::vector<std::string> cmds;
            { LockGuard lk(g_cmdMutex);
              cmds = g_pendingCommands;
              g_pendingCommands.clear(); 
            }
            
            
            for (const auto& c : cmds) {
                 int s = send(g_clientSocket, c.c_str(), (int)c.length(), 0);
                 if (s == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
                     break; 
                 }
            }

            // Read config from C# (non-blocking)
            char buf[2048];
            int r = recv(g_clientSocket, buf, sizeof(buf) - 1, 0);
            if (r > 0) {
                buf[r] = 0; readBuf += buf;
                size_t pos;
                while ((pos = readBuf.find('\n')) != std::string::npos) {
                    std::string line = readBuf.substr(0, pos);
                    readBuf.erase(0, pos + 1);
                    if (!line.empty()) ParseConfig(line);
                }
            }

            Sleep(needEntityTelemetry ? 10 : 25);
        }
        closesocket(g_clientSocket); g_clientSocket = INVALID_SOCKET;
        Log("Client disconnected");
    }
    g_jvm->DetachCurrentThread();
    closesocket(g_serverSocket); WSACleanup();
}

// ===================== MAIN THREAD & DLLMAIN =====================
DWORD WINAPI MainThread(LPVOID lpParam) {
    Log("MainThread started | build 2026-03-29 14:40 reach-clickedge-wndproc");
    HMODULE hJvm = GetModuleHandleA("jvm.dll");
    if (!hJvm) { Log("ERROR: jvm.dll not found"); return 0; }
    typedef jint(JNICALL* FnGetVMs)(JavaVM**, jsize, jsize*);
    FnGetVMs fn = (FnGetVMs)GetProcAddress(hJvm, "JNI_GetCreatedJavaVMs");
    if (!fn) { Log("ERROR: GetCreatedJavaVMs not found"); return 0; }
    jsize cnt; jint res = fn(&g_jvm, 1, &cnt);
    if (res != JNI_OK || cnt == 0) { Log("ERROR: No JVM"); return 0; }
    Log("JVM found");

    // Install rendering hook
    InstallSwapBuffersHook();

    // Start TCP server
    Log("Starting server...");
    ServerLoop();
    return 0;
}

extern "C" __declspec(dllexport) void Dummy() {}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitLogPath(hModule);
        std::ofstream f(g_logPath.c_str(), std::ios_base::trunc);
        f << "[Bridge] DLL_PROCESS_ATTACH" << std::endl; f.close();
        Log("Log path: " + g_logPath);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("DLL_PROCESS_DETACH");
    }
    return TRUE;
}
