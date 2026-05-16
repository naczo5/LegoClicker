// jni_core/helper_bridge.cpp
#include "helper_bridge.h"
#include "aoko_helper.inc"   // kAokoHelperClassBytes[], kAokoHelperClassLen
#include <cstring>

namespace HelperBridge {

// ── State ─────────────────────────────────────────────────────────────────────

static jclass    s_helperClass   = nullptr; // global ref
static jmethodID s_collectMethod = nullptr;
static jobject   s_directBuffer  = nullptr; // global ref, 256 KB
static int       s_bufCapacity   = 0;

static const int kBufSize = 256 * 1024; // 256 KB — enough for ~6000 entities

// ── Load ──────────────────────────────────────────────────────────────────────

bool Load(JNIEnv* env, jobject classLoader) {
    if (s_helperClass) return true;
    if (!env || !classLoader) return false;

    jclass clClass = env->FindClass("java/lang/ClassLoader");
    if (!clClass || env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    // GetMethodID works for protected methods when called from native code.
    jmethodID defineClass = env->GetMethodID(clClass, "defineClass",
                                             "(Ljava/lang/String;[BII)Ljava/lang/Class;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); defineClass = nullptr; }

    env->DeleteLocalRef(clClass);
    if (!defineClass) return false;

    // Copy class bytes into a jbyteArray
    jbyteArray ba = env->NewByteArray(kAokoHelperClassLen);
    if (!ba) { env->ExceptionClear(); return false; }
    env->SetByteArrayRegion(ba, 0, kAokoHelperClassLen,
                            reinterpret_cast<const jbyte*>(kAokoHelperClassBytes));

    // Class name as jstring
    jstring jname = env->NewStringUTF("com/aoko/helper/AokoHelper");
    if (!jname) { env->DeleteLocalRef(ba); return false; }

    // Call defineClass
    jclass defined = (jclass)env->CallObjectMethod(
        classLoader, defineClass, jname, ba, (jint)0, (jint)kAokoHelperClassLen);
    if (env->ExceptionCheck()) { env->ExceptionClear(); defined = nullptr; }

    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(ba);

    if (!defined) {
        // Class may already be defined — try loading it
        defined = (jclass)env->FindClass("com/aoko/helper/AokoHelper");
        if (env->ExceptionCheck()) { env->ExceptionClear(); defined = nullptr; }
    }
    if (!defined) return false;

    // Resolve collectEntityFrame
    s_collectMethod = env->GetStaticMethodID(defined, "collectEntityFrame",
        "(Ljava/util/List;"
        "Ljava/lang/reflect/Field;"
        "Ljava/lang/reflect/Field;"
        "Ljava/lang/reflect/Field;"
        "Ljava/lang/reflect/Field;"
        "Ljava/lang/reflect/Method;"
        "Ljava/lang/reflect/Method;"
        "Ljava/lang/reflect/Method;"
        "Ljava/lang/reflect/Method;"
        "Ljava/lang/reflect/Method;"
        "Ljava/lang/Object;"
        "Ljava/nio/ByteBuffer;"
        ")I");
    if (env->ExceptionCheck()) { env->ExceptionClear(); s_collectMethod = nullptr; }
    if (!s_collectMethod) { env->DeleteLocalRef(defined); return false; }

    // Allocate direct ByteBuffer (native memory, owned by us)
    void* mem = malloc(kBufSize);
    if (!mem) { env->DeleteLocalRef(defined); return false; }
    jobject localBuf = env->NewDirectByteBuffer(mem, kBufSize);
    if (!localBuf || env->ExceptionCheck()) {
        env->ExceptionClear();
        free(mem);
        env->DeleteLocalRef(defined);
        return false;
    }

    s_helperClass  = (jclass)env->NewGlobalRef(defined);
    s_directBuffer = env->NewGlobalRef(localBuf);
    s_bufCapacity  = kBufSize;

    env->DeleteLocalRef(localBuf);
    env->DeleteLocalRef(defined);
    return true;
}

bool IsLoaded() { return s_helperClass != nullptr && s_collectMethod != nullptr; }

void Unload(JNIEnv* env) {
    if (s_directBuffer) {
        // Free the native backing buffer before releasing the JNI global ref.
        if (env) {
            void* addr = env->GetDirectBufferAddress(s_directBuffer);
            if (addr) free(addr);
            env->DeleteGlobalRef(s_directBuffer);
        }
        s_directBuffer = nullptr;
    }
    if (s_helperClass && env) {
        env->DeleteGlobalRef(s_helperClass);
        s_helperClass = nullptr;
    }
    s_collectMethod = nullptr;
    s_bufCapacity   = 0;
}

// ── CollectEntities ───────────────────────────────────────────────────────────

int CollectEntities(
    JNIEnv*    env,
    jobject    entityList,
    jobject    selfEntity,
    jobject    fPosVec,
    jobject    fVecX,
    jobject    fVecY,
    jobject    fVecZ,
    jobject    mGetX,
    jobject    mGetY,
    jobject    mGetZ,
    jobject    mGetHealth,
    jobject    mGetName,
    EntityFrame& out)
{
    out.entities.clear();
    if (!IsLoaded() || !env || !entityList) return -1;

    jint n = env->CallStaticIntMethod(
        s_helperClass, s_collectMethod,
        entityList,
        fPosVec, fVecX, fVecY, fVecZ,
        mGetX, mGetY, mGetZ,
        mGetHealth, mGetName,
        selfEntity,
        s_directBuffer);

    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    if (n <= 0) return 0;

    // Decode the ByteBuffer (little-endian, position=0 after flip() in Java)
    const unsigned char* buf = static_cast<const unsigned char*>(
        env->GetDirectBufferAddress(s_directBuffer));
    jlong cap = env->GetDirectBufferCapacity(s_directBuffer);
    if (!buf) return -1;

    // Read limit from ByteBuffer.limit()
    jclass bbCls = env->GetObjectClass(s_directBuffer);
    jmethodID mLimit = bbCls ? env->GetMethodID(bbCls, "limit", "()I") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); mLimit = nullptr; }
    jint limit = mLimit ? env->CallIntMethod(s_directBuffer, mLimit) : (jint)cap;
    if (env->ExceptionCheck()) { env->ExceptionClear(); limit = (jint)cap; }
    if (bbCls) env->DeleteLocalRef(bbCls);

    out.entities.reserve(n);
    int pos = 0;
    for (int i = 0; i < n && pos + 32 <= limit; i++) {
        EntitySnapshot snap;
        memcpy(&snap.x,      buf + pos,      8); pos += 8;
        memcpy(&snap.y,      buf + pos,      8); pos += 8;
        memcpy(&snap.z,      buf + pos,      8); pos += 8;
        memcpy(&snap.health, buf + pos,      4); pos += 4;
        int nameLen = 0;
        memcpy(&nameLen,     buf + pos,      4); pos += 4;
        if (nameLen < 0 || pos + nameLen > limit) break;
        snap.name.assign(reinterpret_cast<const char*>(buf + pos), nameLen);
        pos += nameLen;
        out.entities.push_back(std::move(snap));
    }

    return (int)out.entities.size();
}

} // namespace HelperBridge
