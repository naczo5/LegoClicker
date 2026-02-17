package me.legoclicker.injector;

import com.sun.tools.attach.VirtualMachine;
import com.sun.tools.attach.VirtualMachineDescriptor;
import java.io.File;
import java.util.List;

/**
 * Injector — finds the Minecraft/Lunar Client JVM process
 * and loads the LegoAgent into it via the Attach API.
 */
public class Injector {

    private static final String[] PROCESS_KEYWORDS = {
        "lunar", "minecraft", "javaw"
    };

    public static void main(String[] args) {
        System.out.println("[LegoInjector] Searching for Minecraft/Lunar Client process...");

        String targetPid = null;

        // If PID is provided as argument, use it directly
        if (args.length > 0) {
            targetPid = args[0];
            System.out.println("[LegoInjector] Using provided PID: " + targetPid);
        } else {
            // Auto-detect Minecraft/Lunar process
            targetPid = findMinecraftPid();
        }

        if (targetPid == null) {
            System.err.println("[LegoInjector] ERROR: Could not find Minecraft/Lunar Client process.");
            System.err.println("[LegoInjector] Make sure the game is running, then try again.");
            System.err.println("[LegoInjector] You can also pass the PID as an argument: java -jar injector.jar <PID>");
            System.exit(1);
        }

        // Resolve agent jar path (should be next to injector jar)
        String agentPath = resolveAgentPath();
        if (agentPath == null) {
            System.err.println("[LegoInjector] ERROR: Could not find agent.jar");
            System.err.println("[LegoInjector] Make sure agent.jar is in the same directory as injector.jar");
            System.exit(1);
        }

        System.out.println("[LegoInjector] Found agent at: " + agentPath);
        System.out.println("[LegoInjector] Attaching to PID " + targetPid + "...");

        try {
            VirtualMachine vm = VirtualMachine.attach(targetPid);
            vm.loadAgent(agentPath);
            vm.detach();
            System.out.println("[LegoInjector] SUCCESS! Agent injected successfully.");
            System.out.println("[LegoInjector] TCP server should be running on port 25590.");
        } catch (Exception e) {
            System.err.println("[LegoInjector] ERROR: Failed to inject agent: " + e.getMessage());
            e.printStackTrace();

            if (e.getMessage() != null && e.getMessage().contains("DisableAttachMechanism")) {
                System.err.println("[LegoInjector] HINT: The JVM has attach disabled.");
                System.err.println("[LegoInjector] Add -XX:+EnableDynamicAgentLoading to your Lunar Client JVM args.");
            }
            System.exit(1);
        }
    }

    private static String findMinecraftPid() {
        try {
            List<VirtualMachineDescriptor> vms = VirtualMachine.list();
            System.out.println("[LegoInjector] Found " + vms.size() + " JVM processes:");

            for (VirtualMachineDescriptor vmd : vms) {
                String displayName = vmd.displayName().toLowerCase();
                String pid = vmd.id();
                System.out.println("  PID " + pid + ": " + vmd.displayName());

                for (String keyword : PROCESS_KEYWORDS) {
                    if (displayName.contains(keyword)) {
                        System.out.println("[LegoInjector] Matched process: " + vmd.displayName());
                        return pid;
                    }
                }
            }

            // If no keyword match, try to find any JVM that isn't our own injector
            String selfPid = ProcessHandle.current().pid() + "";
            for (VirtualMachineDescriptor vmd : vms) {
                if (!vmd.id().equals(selfPid) && !vmd.displayName().contains("Injector")) {
                    System.out.println("[LegoInjector] No keyword match, trying: " + vmd.displayName());
                    return vmd.id();
                }
            }
        } catch (Exception e) {
            System.err.println("[LegoInjector] Error listing VMs: " + e.getMessage());
        }
        return null;
    }

    private static String resolveAgentPath() {
        // Try relative to working directory
        File agentFile = new File("agent.jar");
        if (agentFile.exists()) return agentFile.getAbsolutePath();

        // Try relative to injector jar location
        try {
            String injectorPath = Injector.class.getProtectionDomain()
                .getCodeSource().getLocation().toURI().getPath();
            // On Windows, remove leading slash from /C:/...
            if (injectorPath.startsWith("/") && injectorPath.contains(":")) {
                injectorPath = injectorPath.substring(1);
            }
            File injectorDir = new File(injectorPath).getParentFile();
            agentFile = new File(injectorDir, "agent.jar");
            if (agentFile.exists()) return agentFile.getAbsolutePath();
        } catch (Exception e) {
            // ignore
        }

        return null;
    }
}
