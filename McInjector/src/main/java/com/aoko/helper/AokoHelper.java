package com.aoko.helper;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;

/**
 * AokoHelper - injected into the game JVM at runtime.
 *
 * All methods are static and take only JDK types (reflection handles, ByteBuffer,
 * List) so this class never imports any Minecraft class.  The native bridge
 * resolves the Field/Method objects during discovery and passes them in.
 *
 * Entity frame layout (per entity, little-endian):
 *   double x      (8 bytes)
 *   double y      (8 bytes)
 *   double z      (8 bytes)
 *   float  health (4 bytes)
 *   int    nameLen (4 bytes)
 *   byte[] name   (nameLen bytes, UTF-8, no null terminator)
 *
 * Total fixed part: 32 bytes + variable name.
 * The native side pre-allocates a large enough direct ByteBuffer.
 */
public final class AokoHelper {

    private AokoHelper() {}

    /**
     * Pack entity snapshots into a direct ByteBuffer.
     *
     * @param entities     java.util.List of entity objects (already resolved by native)
     * @param fPosVec      Field: Entity.pos (Vec3d) — may be null if using getX/Y/Z methods
     * @param fVecX        Field: Vec3d.x
     * @param fVecY        Field: Vec3d.y
     * @param fVecZ        Field: Vec3d.z
     * @param mGetX        Method: Entity.getX() — fallback if fPosVec is null
     * @param mGetY        Method: Entity.getY()
     * @param mGetZ        Method: Entity.getZ()
     * @param mGetHealth   Method: LivingEntity.getHealth() — may be null
     * @param mGetName     Method: Entity.getName() or GameProfile.getName() — may be null
     * @param selfEntity   The local player object to skip (may be null)
     * @param out          Direct ByteBuffer owned by native; position=0 on entry
     * @return number of entities written
     */
    public static int collectEntityFrame(
            List<?> entities,
            Field   fPosVec,
            Field   fVecX,
            Field   fVecY,
            Field   fVecZ,
            Method  mGetX,
            Method  mGetY,
            Method  mGetZ,
            Method  mGetHealth,
            Method  mGetName,
            Object  selfEntity,
            ByteBuffer out) {

        out.order(ByteOrder.LITTLE_ENDIAN);
        out.clear();

        int count = 0;
        int size = entities.size();

        for (int i = 0; i < size; i++) {
            Object e = entities.get(i);
            if (e == null || e == selfEntity) continue;

            // Position
            double x, y, z;
            try {
                if (fPosVec != null) {
                    Object pos = fPosVec.get(e);
                    x = fVecX.getDouble(pos);
                    y = fVecY.getDouble(pos);
                    z = fVecZ.getDouble(pos);
                } else {
                    x = (Double) mGetX.invoke(e);
                    y = (Double) mGetY.invoke(e);
                    z = (Double) mGetZ.invoke(e);
                }
            } catch (Exception ex) {
                continue;
            }

            // Health
            float health = 20.0f;
            if (mGetHealth != null) {
                try {
                    health = (Float) mGetHealth.invoke(e);
                } catch (Exception ex) { /* keep default */ }
            }

            // Name
            byte[] nameBytes = new byte[0];
            if (mGetName != null) {
                try {
                    Object nameObj = mGetName.invoke(e);
                    if (nameObj != null) {
                        // getName() may return a Text/Component; call getString() if needed
                        String nameStr;
                        if (nameObj instanceof String) {
                            nameStr = (String) nameObj;
                        } else {
                            // Try getString() (Fabric Text) or getContents() (Mojmap)
                            try {
                                Method mStr = nameObj.getClass().getMethod("getString");
                                nameStr = (String) mStr.invoke(nameObj);
                            } catch (Exception ex2) {
                                nameStr = nameObj.toString();
                            }
                        }
                        nameBytes = nameStr.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    }
                } catch (Exception ex) { /* keep empty */ }
            }

            // Check capacity: 8+8+8+4+4 = 32 fixed + nameLen
            int needed = 32 + nameBytes.length;
            if (out.remaining() < needed) break;

            out.putDouble(x);
            out.putDouble(y);
            out.putDouble(z);
            out.putFloat(health);
            out.putInt(nameBytes.length);
            out.put(nameBytes);
            count++;
        }

        out.flip(); // limit = written bytes, position = 0
        return count;
    }
}
