#include <bits/stdc++.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <semaphore>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

using namespace std;

// ----------------------------- Node for LRU Cache -----------------------------
struct Node {
    string key;
    string value;
    Node* prev;
    Node* next;

    Node(string k, string v) : key(k), value(v), prev(nullptr), next(nullptr) {}
};

// ----------------------------- LRU Cache -----------------------------
class LRUCache {
private:
    int capacity;
    unordered_map<string, Node*> cacheMap;
    Node* head;
    Node* tail;
    mutex mtx;

    void moveToHead(Node* node) {
        if (node == head) return;

        if (node->prev) node->prev->next = node->next;
        if (node->next) node->next->prev = node->prev;
        if (node == tail) tail = node->prev;

        node->prev = nullptr;
        node->next = head;

        if (head) head->prev = node;
        head = node;
        if (!tail) tail = head;
    }

    void addToHead(Node* node) {
        node->next = head;
        node->prev = nullptr;
        if (head) head->prev = node;
        head = node;
        if (!tail) tail = head;
    }

    void removeTail() {
        if (!tail) return;
        cacheMap.erase(tail->key);
        if (tail->prev) tail->prev->next = nullptr;
        else head = nullptr;
        Node* oldTail = tail;
        tail = tail->prev;
        delete oldTail;
    }

public:
    LRUCache(int cap) : capacity(cap), head(nullptr), tail(nullptr) {}

    string get(string key) {
        lock_guard<mutex> lock(mtx);
        if (cacheMap.find(key) != cacheMap.end()) {
            Node* node = cacheMap[key];
            moveToHead(node);
            return node->value;
        }
        return "";
    }

    void put(string key, string value) {
        lock_guard<mutex> lock(mtx);
        if (cacheMap.find(key) != cacheMap.end()) {
            Node* node = cacheMap[key];
            node->value = value;
            moveToHead(node);
        } else {
            Node* newNode = new Node(key, value);
            if ((int)cacheMap.size() >= capacity)
                removeTail();
            addToHead(newNode);
            cacheMap[key] = newNode;
        }
    }
};

// ----------------------------- Proxy Server -----------------------------
class ProxyServer {
private:
    int port;
    int maxClients;
    LRUCache cache;
    counting_semaphore<> sem;
    vector<thread> threadPool;

public:
    ProxyServer(int p, int clients, int cacheSize)
        : port(p), maxClients(clients), cache(cacheSize), sem(clients) {}

    void start() {
        int server_fd;
        struct sockaddr_in address;
        int opt = 1;
        int addrlen = sizeof(address);

        if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
            cerr << "Socket creation failed\n";
            return;
        }

        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            cerr << "Bind failed\n";
            return;
        }

        if (listen(server_fd, 10) < 0) {
            cerr << "Listen failed\n";
            return;
        }

        cout << "Proxy server running on port " << port << "...\n";

        while (true) {
            int new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
            if (new_socket < 0) {
                cerr << "Accept failed\n";
                continue;
            }

            sem.acquire();  // limit active clients
            threadPool.emplace_back(&ProxyServer::handleClient, this, new_socket);
        }
    }

    void handleClient(int clientSocket) {
        char buffer[4096] = {0};
        read(clientSocket, buffer, sizeof(buffer));
        string request(buffer);
        cout << "Received request: " << request.substr(0, 30) << "...\n";

        string url = extractURL(request);
        string cachedResponse = cache.get(url);
        string response;

        if (!cachedResponse.empty()) {
            response = formatHTTPResponse(cachedResponse);
        } else {
            string remoteResponse = fetchFromRemote(url);
            if (!remoteResponse.empty()) {
                cache.put(url, remoteResponse);
                response = formatHTTPResponse(remoteResponse);
            } else {
                response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            }
        }

        send(clientSocket, response.c_str(), response.size(), 0);
        close(clientSocket);
        sem.release();
    }

    string extractURL(const string& request) {
        stringstream ss(request);
        string method, url;
        ss >> method >> url;
        return url;
    }

    string fetchFromRemote(const string& url) {
        string cmd = "curl -s " + url;
        array<char, 4096> buffer;
        string result;

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "";

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
            result += buffer.data();

        pclose(pipe);
        return result;
    }

    string formatHTTPResponse(const string& body) {
        stringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Length: " << body.size() << "\r\n";
        response << "\r\n";
        response << body;
        return response.str();
    }
};

// ----------------------------- Main -----------------------------
int main() {
    int port = 8080;
    int maxClients = 10;
    int cacheSize = 5;

    ProxyServer server(port, maxClients, cacheSize);
    server.start();

    return 0;
}
