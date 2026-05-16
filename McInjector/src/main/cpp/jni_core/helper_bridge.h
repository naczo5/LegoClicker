#pragma once
// jni_core/helper_bridge.h
// Loads AokoHelper.class into the game JVM and exposes a typed C++ call to
// collectEntityFrame().
//
// Usage (once, during discovery):
//   HelperBridge::Load(env, gameClassLoader);
//
// Usage (per scan tick):
//   HelperBridge::EntityFrame frame;
//   int n = HelperBridge::CollectEntities(env, entityList, selfEntity,
//                                         posVecField, vecXField, vecYField, vecZField,
//                                         getXMethod, getYMethod, getZMethod,
//                                         getHealthMethod, getNameMethod,
//                                         frame);
//   for (int i = 0; i < n; i++) { auto& e = frame.entities[i]; ... }

#include <jni.h>
#include <string>
#include <vector>

namespace HelperBridge {

// Per-entity snapshot decoded from the ByteBuffer.
struct EntitySnapshot {
    double x, y, z;
    float  health;
    std::string name;
};

// Decoded frame (filled by CollectEntities).
struct EntityFrame {
    std::vector<EntitySnapshot> entities;
};

// Load AokoHelper.class into the JVM via classLoader.defineClass().
// Safe to call multiple times — no-op if already loaded.
// Returns true if the class is ready.
bool Load(JNIEnv* env, jobject classLoader);

// Returns true if Load() has succeeded.
bool IsLoaded();

// Release all JNI global refs and free the native buffer.
// Call during DLL detach or bridge shutdown.
void Unload(JNIEnv* env);

// Call AokoHelper.collectEntityFrame() and decode the result into frame.
// Any of the Field/Method arguments may be nullptr — the helper handles nulls.
// Returns the number of entities written, or -1 on error.
int CollectEntities(
    JNIEnv*    env,
    jobject    entityList,
    jobject    selfEntity,
    jobject    fPosVec,     // java.lang.reflect.Field (Entity.pos)
    jobject    fVecX,       // java.lang.reflect.Field (Vec3d.x)
    jobject    fVecY,
    jobject    fVecZ,
    jobject    mGetX,       // java.lang.reflect.Method (Entity.getX)
    jobject    mGetY,
    jobject    mGetZ,
    jobject    mGetHealth,  // java.lang.reflect.Method (LivingEntity.getHealth)
    jobject    mGetName,    // java.lang.reflect.Method (Entity.getName)
    EntityFrame& out);

} // namespace HelperBridge
