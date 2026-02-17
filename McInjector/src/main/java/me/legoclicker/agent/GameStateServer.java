package me.legoclicker.agent;

import java.io.*;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

/**
 * GameStateServer — TCP server running inside the Minecraft JVM.
 * Sends game state as JSON lines to connected C# client.
 *
 * Protocol: Each line is a JSON object terminated by newline.
 * Client connects to localhost:25590 and receives updates every 50ms.
 *
 * JSON format:
 * {
 * "guiOpen": true/false,
 * "screenName": "GuiInventory",
 * "health": 20.0,
 * "posX": 100.5, "posY": 64.0, "posZ": -200.3,
 * "entities": [
 * {"x": 105.0, "y": 64.0, "z": -198.0, "hp": 20.0},
 * ...
 * ],
 * "mapped": true
 * }
 */
public class GameStateServer implements Runnable {

    private final ClassMapper mapper;
    private final int port;
    private volatile boolean running = true;

    public GameStateServer(ClassMapper mapper, int port) {
        this.mapper = mapper;
        this.port = port;
    }

    @Override
    public void run() {
        try (ServerSocket serverSocket = new ServerSocket(port)) {
            serverSocket.setReuseAddress(true);
            System.out.println("[GameStateServer] Listening on port " + port);

            while (running) {
                try {
                    Socket client = serverSocket.accept();
                    System.out.println("[GameStateServer] Client connected: " + client.getRemoteSocketAddress());

                    // Handle each client in a new thread
                    Thread clientThread = new Thread(() -> handleClient(client), "LegoAgent-Client");
                    clientThread.setDaemon(true);
                    clientThread.start();
                } catch (IOException e) {
                    if (running) {
                        System.err.println("[GameStateServer] Accept error: " + e.getMessage());
                    }
                }
            }
        } catch (IOException e) {
            System.err.println("[GameStateServer] Failed to start server: " + e.getMessage());
            e.printStackTrace();
        }
    }

    private void handleClient(Socket client) {
        try (OutputStream out = client.getOutputStream();
                BufferedReader in = new BufferedReader(
                        new InputStreamReader(client.getInputStream(), StandardCharsets.UTF_8))) {

            client.setTcpNoDelay(true);
            client.setSoTimeout(5000);

            while (running && !client.isClosed()) {
                String json = buildStateJson();
                out.write((json + "\n").getBytes(StandardCharsets.UTF_8));
                out.flush();

                // Check if client sent a command
                if (in.ready()) {
                    String line = in.readLine();
                    if (line == null)
                        break; // Client disconnected
                    handleCommand(line);
                }

                Thread.sleep(50); // 20 updates per second
            }
        } catch (IOException e) {
            System.out.println("[GameStateServer] Client disconnected: " + e.getMessage());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private String buildStateJson() {
        StringBuilder sb = new StringBuilder();
        sb.append("{");

        // Mapped status
        sb.append("\"mapped\":").append(mapper.isFullyMapped());

        // GUI state
        boolean guiOpen = mapper.isGuiOpen();
        sb.append(",\"guiOpen\":").append(guiOpen);
        sb.append(",\"screenName\":\"").append(escape(mapper.getCurrentScreenName())).append("\"");

        // Player health
        float health = mapper.getPlayerHealth();
        sb.append(",\"health\":").append(health);

        // Player position
        double[] pos = mapper.getPlayerPosition();
        if (pos != null) {
            sb.append(",\"posX\":").append(String.format("%.2f", pos[0]));
            sb.append(",\"posY\":").append(String.format("%.2f", pos[1]));
            sb.append(",\"posZ\":").append(String.format("%.2f", pos[2]));
        } else {
            sb.append(",\"posX\":0,\"posY\":0,\"posZ\":0");
        }

        // Nearby entities
        double[][] entities = mapper.getNearbyEntities();
        sb.append(",\"entities\":[");
        for (int i = 0; i < entities.length; i++) {
            if (i > 0)
                sb.append(",");
            sb.append("{\"x\":").append(String.format("%.2f", entities[i][0]));
            sb.append(",\"y\":").append(String.format("%.2f", entities[i][1]));
            sb.append(",\"z\":").append(String.format("%.2f", entities[i][2]));
            sb.append(",\"hp\":").append(String.format("%.1f", entities[i][3]));
            sb.append("}");
        }
        sb.append("]");

        sb.append("}");
        return sb.toString();
    }

    private void handleCommand(String command) {
        // Future: handle commands from C# client
        System.out.println("[GameStateServer] Received command: " + command);
    }

    private String escape(String s) {
        if (s == null)
            return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    public void stop() {
        running = false;
    }
}
