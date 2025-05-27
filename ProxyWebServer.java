import java.io.*;
import java.net.*;
import java.util.*;
import java.util.concurrent.*;

// Node class for the doubly linked list in LRU Cache
class Node {
    String key;
    String value;
    Node prev, next;

    Node(String key, String value) {
        this.key = key;
        this.value = value;
        this.prev = null;
        this.next = null;
    }
}

// LRU Cache class
class LRUCache {
    private final int capacity;
    private final Map<String, Node> cacheMap;
    private Node head, tail;

    // Constructor
    public LRUCache(int capacity) {
        this.capacity = capacity;
        this.cacheMap = new HashMap<>();
        this.head = null;
        this.tail = null;
    }

    // Get a value from the cache
    public synchronized String get(String key) {
        if (cacheMap.containsKey(key)) {
            Node node = cacheMap.get(key);
            moveToHead(node); // Mark as recently used
            return node.value;
        }
        return null; // Not in cache
    }

    // Put a key-value pair in the cache
    public synchronized void put(String key, String value) {
        if (cacheMap.containsKey(key)) {
            Node node = cacheMap.get(key);
            node.value = value;
            moveToHead(node); // Update and mark as recently used
        } else {
            Node newNode = new Node(key, value);
            if (cacheMap.size() >= capacity) {
                removeTail(); // Remove least recently used
            }
            addToHead(newNode); // Add to cache
            cacheMap.put(key, newNode);
        }
    }

    // Remove the least recently used node (tail)
    private void removeTail() {
        if (tail != null) {
            cacheMap.remove(tail.key);
            if (tail.prev != null) {
                tail.prev.next = null;
            } else {
                head = null; // Cache is now empty
            }
            tail = tail.prev;
        }
    }

    // Move a node to the head (most recently used)
    private void moveToHead(Node node) {
        if (node == head) return; // Already the most recently used

        // Remove node from its current position
        if (node.prev != null) {
            node.prev.next = node.next;
        }
        if (node.next != null) {
            node.next.prev = node.prev;
        }

        // Update tail if needed
        if (node == tail) {
            tail = node.prev;
        }

        // Insert node at the head
        node.next = head;
        node.prev = null;

        if (head != null) {
            head.prev = node;
        }
        head = node;

        // Update tail for a single element case
        if (tail == null) {
            tail = head;
        }
    }

    // Add a new node to the head of the list
    private void addToHead(Node node) {
        node.next = head;
        node.prev = null;

        if (head != null) {
            head.prev = node;
        }
        head = node;

        if (tail == null) {
            tail = head; // First element in the cache
        }
    }
}

// Proxy Server class
class ProxyServer {
    private final int port;
    private final LRUCache cache;
    private final Semaphore semaphore;
    private final ExecutorService threadPool;

    public ProxyServer(int port, int maxClients, int cacheSize) {
        this.port = port;
        this.cache = new LRUCache(cacheSize); // Replace with the LRU Cache
        this.semaphore = new Semaphore(maxClients);
        this.threadPool = Executors.newFixedThreadPool(maxClients);
    }

    public void start() {
        try (ServerSocket serverSocket = new ServerSocket(port)) {
            System.out.println("Proxy server is running on port: " + port);

            while (true) {
                Socket clientSocket = serverSocket.accept();
                semaphore.acquire();
                threadPool.submit(() -> handleClient(clientSocket));
            }
        } catch (IOException | InterruptedException e) {
            System.err.println("Server error: " + e.getMessage());
        }
    }

    private void handleClient(Socket clientSocket) {
        System.out.println("Received request: " );

        try (BufferedReader clientReader = new BufferedReader(new InputStreamReader(clientSocket.getInputStream()));
             BufferedWriter clientWriter = new BufferedWriter(new OutputStreamWriter(clientSocket.getOutputStream()))) {

            String requestLine = clientReader.readLine();
            if (requestLine == null || !requestLine.startsWith("GET")) {
                sendError(clientWriter, "400 Bad Request");
                return;
            }

            String[] requestParts = requestLine.split(" ");
            if (requestParts.length < 2) {
                sendError(clientWriter, "400 Bad Request");
                return;
            }

            String url = requestParts[1];
            String cachedResponse = cache.get(url);

            if (cachedResponse != null) {
                sendResponse(clientWriter, cachedResponse);
            } else {
                String remoteResponse = fetchFromRemoteServer(url);
                if (remoteResponse != null) {
                    cache.put(url, remoteResponse);
                    sendResponse(clientWriter, remoteResponse);
                } else {
                    sendError(clientWriter, "404 Not Found");
                }
            }
        } catch (IOException e) {
            System.err.println("Error handling client: " + e.getMessage());
        } finally {
            try {
                clientSocket.close();
            } catch (IOException e) {
                System.err.println("Error closing client socket: " + e.getMessage());
            }
            semaphore.release();
        }
    }

    private String fetchFromRemoteServer(String url) {
        try {
            URL remoteUrl = new URL(url);
            HttpURLConnection connection = (HttpURLConnection) remoteUrl.openConnection();
            connection.setRequestMethod("GET");

            int responseCode = connection.getResponseCode();
            if (responseCode != 200) {
                return null;
            }

            BufferedReader remoteReader = new BufferedReader(new InputStreamReader(connection.getInputStream()));
            StringBuilder responseBuilder = new StringBuilder();
            String line;

            while ((line = remoteReader.readLine()) != null) {
                responseBuilder.append(line).append("\n");
            }
            remoteReader.close();

            return responseBuilder.toString();
        } catch (IOException e) {
            System.err.println("Error fetching from remote server: " + e.getMessage());
            return null;
        }
    }

    private void sendResponse(BufferedWriter writer, String response) throws IOException {
        writer.write("HTTP/1.1 200 OK\r\n");
        writer.write("Content-Length: " + response.length() + "\r\n");
        writer.write("\r\n");
        writer.write(response);
        writer.flush();
    }

    private void sendError(BufferedWriter writer, String errorMessage) throws IOException {
        writer.write("HTTP/1.1 " + errorMessage + "\r\n");
        writer.write("Content-Length: 0\r\n");
        writer.write("\r\n");
        writer.flush();
    }
}

// Main class to run the server
public class ProxyWebServer {
    public static void main(String[] args) {
        int port = 8080;
        int maxClients = 10;
        int cacheSize = 5;

        ProxyServer proxyServer = new ProxyServer(port, maxClients, cacheSize);
        proxyServer.start();
    }
}
