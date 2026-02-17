package me.legoclicker.agent;

import java.lang.instrument.Instrumentation;

/**
 * LegoAgent — Entry point for the Java agent loaded via Attach API.
 * This runs inside the Minecraft/Lunar Client JVM.
 */
public class LegoAgent {

    private static boolean initialized = false;
    private static GameStateServer server;

    /**
     * Called when agent is loaded into a running JVM via Attach API.
     */
    private static void log(String msg) {
        try {
            java.io.File f = new java.io.File("C:\\LegoAgent.log");
            java.io.FileWriter fw = new java.io.FileWriter(f, true);
            fw.write(msg + "\n");
            fw.close();
        } catch (Exception e) {
        }
        System.out.println(msg);
    }

    public static void agentmain(String agentArgs, Instrumentation inst) {
        log("[LegoAgent] Agent loaded via agentmain");
        internalMain(inst);
    }

    public static void premain(String agentArgs, Instrumentation inst) {
        log("[LegoAgent] Agent loaded via premain");
        internalMain(inst);
    }

    private static void internalMain(Instrumentation inst) {
        if (initialized) {
            log("[LegoAgent] Already initialized, skipping.");
            return;
        }
        initialized = true;

        log("[LegoAgent] Instrumentation available: " + (inst != null));

        try {
            ClassMapper mapper = new ClassMapper(inst);
            // We need to pass the logger to ClassMapper too, or just rely on console for
            // that
            // for now let's just log success/fail here
            boolean mapped = mapper.initialize();

            if (!mapped) {
                log("[LegoAgent] WARNING: Could not find all Minecraft classes.");
            } else {
                log("[LegoAgent] Mapped successfully.");
            }

            server = new GameStateServer(mapper, 25590);
            Thread serverThread = new Thread(server, "LegoAgent-Server");
            serverThread.setDaemon(true);
            serverThread.start();

            log("[LegoAgent] TCP server started on port 25590");

        } catch (Exception e) {
            log("[LegoAgent] ERROR: " + e.getMessage());
            e.printStackTrace();
        }
    }
}
