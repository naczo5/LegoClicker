package me.legoclicker.agent;

import java.lang.instrument.Instrumentation;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * ClassMapper — Finds Minecraft classes and fields at runtime using reflection.
 * Supports both vanilla 1.8.9 obfuscated (notch) names and Lunar Client
 * mappings.
 *
 * MC 1.8.9 Notch mappings:
 * Minecraft = ave
 * EntityPlayerSP = bew
 * GuiScreen = axu
 * Entity = pk
 * WorldClient = bdb
 * EntityLivingBase = pr
 */
public class ClassMapper {

    private final Instrumentation inst;

    // Resolved classes
    private Class<?> minecraftClass;
    private Class<?> entityPlayerSPClass;
    private Class<?> guiScreenClass;
    private Class<?> entityClass;
    private Class<?> worldClientClass;
    private Class<?> entityLivingBaseClass;

    // Resolved fields/methods on Minecraft class
    private Method getMinecraftMethod; // Minecraft.getMinecraft() / ave.A()
    private Field currentScreenField; // Minecraft.currentScreen / ave.m
    private Field thePlayerField; // Minecraft.thePlayer / ave.h
    private Field theWorldField; // Minecraft.theWorld / ave.f

    // Resolved fields on Entity/EntityPlayer
    private Field posXField; // Entity.posX
    private Field posYField; // Entity.posY
    private Field posZField; // Entity.posZ
    private Field healthField; // EntityLivingBase.health

    // Flag
    private boolean fullyMapped = false;

    // Name tables: { MCP name, notch name, Lunar-specific alternatives }
    // Minecraft class
    private static final String[] MC_CLASS_NAMES = {
            "net.minecraft.client.Minecraft", "ave"
    };

    // EntityPlayerSP class
    private static final String[] PLAYER_CLASS_NAMES = {
            "net.minecraft.client.entity.EntityPlayerSP", "bew"
    };

    // GuiScreen class
    private static final String[] GUI_CLASS_NAMES = {
            "net.minecraft.client.gui.GuiScreen", "axu"
    };

    // Entity class
    private static final String[] ENTITY_CLASS_NAMES = {
            "net.minecraft.entity.Entity", "pk"
    };

    // WorldClient class
    private static final String[] WORLD_CLASS_NAMES = {
            "net.minecraft.client.multiplayer.WorldClient", "bdb"
    };

    // EntityLivingBase class
    private static final String[] LIVING_CLASS_NAMES = {
            "net.minecraft.entity.EntityLivingBase", "pr"
    };

    public ClassMapper(Instrumentation inst) {
        this.inst = inst;
    }

    public boolean initialize() {
        System.out.println("[ClassMapper] Starting class resolution...");

        // First, list all loaded classes to help debug
        if (inst != null) {
            Class<?>[] loadedClasses = inst.getAllLoadedClasses();
            System.out.println("[ClassMapper] Total loaded classes: " + loadedClasses.length);

            // Try to find classes by scanning loaded classes
            for (Class<?> cls : loadedClasses) {
                String name = cls.getName();
                // Check if this matches any of our target classes
                for (String mcName : MC_CLASS_NAMES) {
                    if (name.equals(mcName)) {
                        minecraftClass = cls;
                        System.out.println("[ClassMapper] Found Minecraft class: " + name);
                    }
                }
                for (String pName : PLAYER_CLASS_NAMES) {
                    if (name.equals(pName)) {
                        entityPlayerSPClass = cls;
                        System.out.println("[ClassMapper] Found EntityPlayerSP class: " + name);
                    }
                }
                for (String gName : GUI_CLASS_NAMES) {
                    if (name.equals(gName)) {
                        guiScreenClass = cls;
                        System.out.println("[ClassMapper] Found GuiScreen class: " + name);
                    }
                }
                for (String eName : ENTITY_CLASS_NAMES) {
                    if (name.equals(eName)) {
                        entityClass = cls;
                        System.out.println("[ClassMapper] Found Entity class: " + name);
                    }
                }
                for (String wName : WORLD_CLASS_NAMES) {
                    if (name.equals(wName)) {
                        worldClientClass = cls;
                        System.out.println("[ClassMapper] Found WorldClient class: " + name);
                    }
                }
                for (String lName : LIVING_CLASS_NAMES) {
                    if (name.equals(lName)) {
                        entityLivingBaseClass = cls;
                        System.out.println("[ClassMapper] Found EntityLivingBase class: " + name);
                    }
                }
            }
        }

        // Fallback: try Class.forName for each
        if (minecraftClass == null)
            minecraftClass = tryLoadClass(MC_CLASS_NAMES);
        if (entityPlayerSPClass == null)
            entityPlayerSPClass = tryLoadClass(PLAYER_CLASS_NAMES);
        if (guiScreenClass == null)
            guiScreenClass = tryLoadClass(GUI_CLASS_NAMES);
        if (entityClass == null)
            entityClass = tryLoadClass(ENTITY_CLASS_NAMES);
        if (worldClientClass == null)
            worldClientClass = tryLoadClass(WORLD_CLASS_NAMES);
        if (entityLivingBaseClass == null)
            entityLivingBaseClass = tryLoadClass(LIVING_CLASS_NAMES);

        // If still no Minecraft class, try auto-detection via heuristics
        if (minecraftClass == null && inst != null) {
            System.out.println("[ClassMapper] Attempting auto-detection...");
            autoDetectClasses();
        }

        // Now resolve fields and methods
        if (minecraftClass != null) {
            resolveMinecraftMembers();
        }

        if (entityClass != null) {
            resolveEntityMembers();
        }

        // Report status
        System.out.println("[ClassMapper] === Mapping Results ===");
        System.out.println(
                "[ClassMapper] Minecraft class: " + (minecraftClass != null ? minecraftClass.getName() : "NOT FOUND"));
        System.out.println("[ClassMapper] EntityPlayerSP class: "
                + (entityPlayerSPClass != null ? entityPlayerSPClass.getName() : "NOT FOUND"));
        System.out.println(
                "[ClassMapper] GuiScreen class: " + (guiScreenClass != null ? guiScreenClass.getName() : "NOT FOUND"));
        System.out
                .println("[ClassMapper] Entity class: " + (entityClass != null ? entityClass.getName() : "NOT FOUND"));
        System.out.println("[ClassMapper] WorldClient class: "
                + (worldClientClass != null ? worldClientClass.getName() : "NOT FOUND"));
        System.out.println("[ClassMapper] getMinecraft(): " + (getMinecraftMethod != null ? "OK" : "NOT FOUND"));
        System.out.println("[ClassMapper] currentScreen: " + (currentScreenField != null ? "OK" : "NOT FOUND"));
        System.out.println("[ClassMapper] thePlayer: " + (thePlayerField != null ? "OK" : "NOT FOUND"));
        System.out.println("[ClassMapper] posX/Y/Z: " + (posXField != null ? "OK" : "NOT FOUND"));
        System.out.println("[ClassMapper] health: " + (healthField != null ? "OK" : "NOT FOUND"));

        fullyMapped = minecraftClass != null && getMinecraftMethod != null;
        return fullyMapped;
    }

    /**
     * Auto-detect the Minecraft main class by looking for characteristic
     * signatures.
     * The Minecraft class has:
     * - A static method that returns itself (singleton)
     * - Contains a field of its own type
     * - Has fields for current screen, player, world
     */
    private void autoDetectClasses() {
        Class<?>[] loadedClasses = inst.getAllLoadedClasses();
        for (Class<?> cls : loadedClasses) {
            try {
                // Skip non-game classes
                String name = cls.getName();
                if (name.startsWith("java.") || name.startsWith("javax.") ||
                        name.startsWith("sun.") || name.startsWith("jdk.") ||
                        name.startsWith("com.sun.") || name.startsWith("org.") ||
                        name.contains(".") && name.split("\\.").length > 3) {
                    continue;
                }

                // Look for a class with a static method returning itself
                for (Method m : cls.getDeclaredMethods()) {
                    if (java.lang.reflect.Modifier.isStatic(m.getModifiers()) &&
                            m.getReturnType() == cls &&
                            m.getParameterCount() == 0) {

                        // Check if it has enough fields (Minecraft class has many)
                        if (cls.getDeclaredFields().length > 20) {
                            System.out.println("[ClassMapper] Auto-detected potential Minecraft class: " + name);
                            System.out.println("[ClassMapper]   Static singleton method: " + m.getName());
                            System.out.println("[ClassMapper]   Field count: " + cls.getDeclaredFields().length);
                            minecraftClass = cls;
                            return;
                        }
                    }
                }
            } catch (Throwable t) {
                // Some classes may fail to inspect, skip them
            }
        }
    }

    private void resolveMinecraftMembers() {
        // Find getMinecraft() — static method returning the Minecraft class instance
        for (Method m : minecraftClass.getDeclaredMethods()) {
            if (java.lang.reflect.Modifier.isStatic(m.getModifiers()) &&
                    m.getReturnType() == minecraftClass &&
                    m.getParameterCount() == 0) {
                getMinecraftMethod = m;
                getMinecraftMethod.setAccessible(true);
                System.out.println("[ClassMapper] Resolved getMinecraft: " + m.getName() + "()");
                break;
            }
        }

        if (getMinecraftMethod == null)
            return;

        // Find currentScreen field (type is GUI class or a class with draw methods)
        // Find thePlayer field (type is entity-like)
        // Find theWorld field (type is world-like)
        for (Field f : minecraftClass.getDeclaredFields()) {
            f.setAccessible(true);
            Class<?> type = f.getType();

            // currentScreen: look for GuiScreen type or matching names
            if (guiScreenClass != null && type == guiScreenClass) {
                currentScreenField = f;
                System.out.println("[ClassMapper] Resolved currentScreen: " + f.getName());
            } else if (matchesName(f, "currentScreen", "m", "currentScreen")) {
                currentScreenField = f;
                System.out.println("[ClassMapper] Resolved currentScreen by name: " + f.getName());
            }

            // thePlayer: look for EntityPlayerSP type or matching names
            if (entityPlayerSPClass != null && type == entityPlayerSPClass) {
                thePlayerField = f;
                System.out.println("[ClassMapper] Resolved thePlayer: " + f.getName());
            } else if (matchesName(f, "thePlayer", "h", "thePlayer")) {
                thePlayerField = f;
                System.out.println("[ClassMapper] Resolved thePlayer by name: " + f.getName());
            }

            // theWorld
            if (worldClientClass != null && type == worldClientClass) {
                theWorldField = f;
                System.out.println("[ClassMapper] Resolved theWorld: " + f.getName());
            } else if (matchesName(f, "theWorld", "f", "theWorld")) {
                theWorldField = f;
                System.out.println("[ClassMapper] Resolved theWorld by name: " + f.getName());
            }
        }

        // If we couldn't find by type, try heuristic: scan by field order and type
        // characteristics
        if (currentScreenField == null || thePlayerField == null) {
            System.out.println("[ClassMapper] Attempting field heuristic resolution...");
            resolveByHeuristic();
        }
    }

    private void resolveByHeuristic() {
        try {
            Object mcInstance = getMinecraftMethod.invoke(null);
            if (mcInstance == null) {
                System.out.println("[ClassMapper] Minecraft instance is null, can't use heuristic");
                return;
            }

            for (Field f : minecraftClass.getDeclaredFields()) {
                f.setAccessible(true);

                // Skip static fields and primitives
                if (java.lang.reflect.Modifier.isStatic(f.getModifiers()))
                    continue;
                if (f.getType().isPrimitive())
                    continue;

                try {
                    Object val = f.get(mcInstance);
                    if (val == null) {
                        // currentScreen is often null (when not in a GUI)
                        // Record as candidate for currentScreen
                        if (currentScreenField == null) {
                            // Heuristic: if this field's type has "draw" or "render" methods
                            Class<?> ft = f.getType();
                            for (Method m : ft.getDeclaredMethods()) {
                                if (m.getName().contains("draw") || m.getName().contains("render")) {
                                    currentScreenField = f;
                                    System.out.println("[ClassMapper] Heuristic: currentScreen = " + f.getName()
                                            + " (type: " + ft.getName() + ")");
                                    break;
                                }
                            }
                        }
                    } else {
                        // Check if this object has posX/posY/posZ fields (player entity)
                        if (thePlayerField == null) {
                            Class<?> valClass = val.getClass();
                            if (hasPositionFields(valClass)) {
                                thePlayerField = f;
                                System.out.println("[ClassMapper] Heuristic: thePlayer = " + f.getName() + " (type: "
                                        + valClass.getName() + ")");

                                // Also resolve the entity class from here
                                if (entityClass == null) {
                                    // Walk up hierarchy to find Entity base class
                                    Class<?> c = valClass;
                                    while (c.getSuperclass() != null && c.getSuperclass() != Object.class) {
                                        if (hasPositionFields(c.getSuperclass())) {
                                            c = c.getSuperclass();
                                        } else {
                                            break;
                                        }
                                    }
                                    entityClass = c;
                                    System.out
                                            .println("[ClassMapper] Heuristic: Entity base = " + entityClass.getName());
                                }
                            }
                        }
                    }
                } catch (Throwable t) {
                    // skip
                }
            }
        } catch (Exception e) {
            System.err.println("[ClassMapper] Heuristic error: " + e.getMessage());
        }
    }

    private boolean hasPositionFields(Class<?> cls) {
        int doubleCount = 0;
        for (Field f : cls.getDeclaredFields()) {
            if (f.getType() == double.class && !java.lang.reflect.Modifier.isStatic(f.getModifiers())) {
                doubleCount++;
            }
        }
        // Entity class has at least 3 doubles (posX, posY, posZ) and usually more
        return doubleCount >= 3;
    }

    private void resolveEntityMembers() {
        // Entity has posX, posY, posZ as doubles
        // Known notch names for 1.8.9: s=posX, t=posY, u=posZ (Entity class pk)
        String[] posXNames = { "posX", "s", "field_70165_t" };
        String[] posYNames = { "posY", "t", "field_70163_u" };
        String[] posZNames = { "posZ", "u", "field_70161_v" };

        posXField = findField(entityClass, posXNames, double.class);
        posYField = findField(entityClass, posYNames, double.class);
        posZField = findField(entityClass, posZNames, double.class);

        // If named resolution fails, try positional: first 3 non-static doubles
        if (posXField == null || posYField == null || posZField == null) {
            System.out.println("[ClassMapper] Trying positional double field resolution for Entity...");
            int idx = 0;
            for (Field f : entityClass.getDeclaredFields()) {
                if (f.getType() == double.class && !java.lang.reflect.Modifier.isStatic(f.getModifiers())) {
                    f.setAccessible(true);
                    if (idx == 0) {
                        posXField = f;
                        System.out.println("[ClassMapper] posX = " + f.getName());
                    } else if (idx == 1) {
                        posYField = f;
                        System.out.println("[ClassMapper] posY = " + f.getName());
                    } else if (idx == 2) {
                        posZField = f;
                        System.out.println("[ClassMapper] posZ = " + f.getName());
                    }
                    idx++;
                    if (idx >= 3)
                        break;
                }
            }
        }

        // Health: in EntityLivingBase, it's a float
        // Known notch names for 1.8.9: bn (EntityLivingBase = pr)
        Class<?> healthClass = entityLivingBaseClass != null ? entityLivingBaseClass : entityClass;
        if (healthClass != null) {
            String[] healthNames = { "health", "bn", "field_70725_aQ" };
            healthField = findField(healthClass, healthNames, float.class);

            if (healthField == null) {
                // Try getHealth method instead
                for (Method m : healthClass.getDeclaredMethods()) {
                    if (m.getReturnType() == float.class && m.getParameterCount() == 0) {
                        // Could be getHealth — we'll try it
                        if (m.getName().equals("getHealth") || m.getName().equals("bj")) {
                            m.setAccessible(true);
                            System.out.println("[ClassMapper] Found getHealth method: " + m.getName());
                            // Store as special marker
                            break;
                        }
                    }
                }
            }
        }
    }

    private Field findField(Class<?> cls, String[] names, Class<?> type) {
        if (cls == null)
            return null;
        // Search this class and all superclasses
        Class<?> current = cls;
        while (current != null && current != Object.class) {
            for (String name : names) {
                try {
                    Field f = current.getDeclaredField(name);
                    if (type == null || f.getType() == type) {
                        f.setAccessible(true);
                        System.out.println("[ClassMapper] Resolved field " + name + " in " + current.getName());
                        return f;
                    }
                } catch (NoSuchFieldException e) {
                    // continue
                }
            }
            current = current.getSuperclass();
        }
        return null;
    }

    private boolean matchesName(Field f, String... names) {
        for (String name : names) {
            if (f.getName().equals(name))
                return true;
        }
        return false;
    }

    private Class<?> tryLoadClass(String[] names) {
        for (String name : names) {
            try {
                Class<?> cls = Class.forName(name);
                System.out.println("[ClassMapper] Loaded class via forName: " + name);
                return cls;
            } catch (ClassNotFoundException e) {
                // try next
            }
        }

        // Also try with the thread context classloader
        ClassLoader cl = Thread.currentThread().getContextClassLoader();
        if (cl != null) {
            for (String name : names) {
                try {
                    Class<?> cls = cl.loadClass(name);
                    System.out.println("[ClassMapper] Loaded class via contextCL: " + name);
                    return cls;
                } catch (ClassNotFoundException e) {
                    // try next
                }
            }
        }
        return null;
    }

    // === Public getters for GameStateServer ===

    public Object getMinecraftInstance() {
        if (getMinecraftMethod == null)
            return null;
        try {
            return getMinecraftMethod.invoke(null);
        } catch (Exception e) {
            return null;
        }
    }

    public boolean isGuiOpen() {
        if (currentScreenField == null)
            return false;
        try {
            Object mc = getMinecraftInstance();
            if (mc == null)
                return false;
            return currentScreenField.get(mc) != null;
        } catch (Exception e) {
            return false;
        }
    }

    public String getCurrentScreenName() {
        if (currentScreenField == null)
            return "unknown";
        try {
            Object mc = getMinecraftInstance();
            if (mc == null)
                return "unknown";
            Object screen = currentScreenField.get(mc);
            if (screen == null)
                return "none";
            return screen.getClass().getSimpleName();
        } catch (Exception e) {
            return "error";
        }
    }

    public double[] getPlayerPosition() {
        if (thePlayerField == null || posXField == null)
            return null;
        try {
            Object mc = getMinecraftInstance();
            if (mc == null)
                return null;
            Object player = thePlayerField.get(mc);
            if (player == null)
                return null;

            double x = posXField.getDouble(player);
            double y = posYField.getDouble(player);
            double z = posZField.getDouble(player);
            return new double[] { x, y, z };
        } catch (Exception e) {
            return null;
        }
    }

    public float getPlayerHealth() {
        if (thePlayerField == null)
            return -1;
        try {
            Object mc = getMinecraftInstance();
            if (mc == null)
                return -1;
            Object player = thePlayerField.get(mc);
            if (player == null)
                return -1;

            if (healthField != null) {
                return healthField.getFloat(player);
            }

            // Try getHealth method as fallback
            Method getHealth = null;
            Class<?> cls = player.getClass();
            while (cls != null && cls != Object.class) {
                for (Method m : cls.getDeclaredMethods()) {
                    if (m.getReturnType() == float.class && m.getParameterCount() == 0) {
                        String name = m.getName();
                        if (name.equals("getHealth") || name.equals("bj") || name.equals("dj")) {
                            getHealth = m;
                            break;
                        }
                    }
                }
                if (getHealth != null)
                    break;
                cls = cls.getSuperclass();
            }

            if (getHealth != null) {
                getHealth.setAccessible(true);
                return (float) getHealth.invoke(player);
            }

            return -1;
        } catch (Exception e) {
            return -1;
        }
    }

    /**
     * Get nearby entities with positions.
     * Returns array of {x, y, z, health} for each entity.
     */
    public double[][] getNearbyEntities() {
        if (theWorldField == null || thePlayerField == null)
            return new double[0][];
        try {
            Object mc = getMinecraftInstance();
            if (mc == null)
                return new double[0][];

            Object world = theWorldField.get(mc);
            if (world == null)
                return new double[0][];
            Object player = thePlayerField.get(mc);
            if (player == null)
                return new double[0][];

            // Get loadedEntityList from world
            // Notch name for 1.8.9: WorldClient.h or WorldClient.loadedEntityList
            java.util.List<?> entities = null;
            for (Field f : world.getClass().getDeclaredFields()) {
                f.setAccessible(true);
                if (java.util.List.class.isAssignableFrom(f.getType())) {
                    Object val = f.get(world);
                    if (val instanceof java.util.List) {
                        java.util.List<?> list = (java.util.List<?>) val;
                        if (!list.isEmpty()) {
                            // Check if first element has position fields
                            Object first = list.get(0);
                            if (first != null && hasPositionFields(first.getClass())) {
                                entities = list;
                                break;
                            }
                        }
                    }
                }
            }
            // Also check superclass fields
            if (entities == null) {
                Class<?> superWorld = world.getClass().getSuperclass();
                while (superWorld != null && superWorld != Object.class) {
                    for (Field f : superWorld.getDeclaredFields()) {
                        f.setAccessible(true);
                        if (java.util.List.class.isAssignableFrom(f.getType())) {
                            Object val = f.get(world);
                            if (val instanceof java.util.List) {
                                java.util.List<?> list = (java.util.List<?>) val;
                                if (!list.isEmpty()) {
                                    Object first = list.get(0);
                                    if (first != null && hasPositionFields(first.getClass())) {
                                        entities = list;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (entities != null)
                        break;
                    superWorld = superWorld.getSuperclass();
                }
            }

            if (entities == null)
                return new double[0][];

            // Collect entity data (skip the player)
            java.util.List<double[]> result = new java.util.ArrayList<>();
            for (Object entity : entities) {
                if (entity == player)
                    continue;
                if (entity == null)
                    continue;
                if (posXField == null || posYField == null || posZField == null)
                    continue;

                try {
                    double ex = posXField.getDouble(entity);
                    double ey = posYField.getDouble(entity);
                    double ez = posZField.getDouble(entity);
                    float eh = -1;

                    // Try to get health if entity is a living entity
                    if (healthField != null) {
                        try {
                            eh = healthField.getFloat(entity);
                        } catch (Exception ignore) {
                        }
                    }

                    result.add(new double[] { ex, ey, ez, eh });
                } catch (Exception ignore) {
                }

                // Limit to 50 entities to prevent lag
                if (result.size() >= 50)
                    break;
            }

            return result.toArray(new double[0][]);
        } catch (Exception e) {
            return new double[0][];
        }
    }

    public boolean isFullyMapped() {
        return fullyMapped;
    }
}
