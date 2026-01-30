// ProxyWSRateLimiter.cpp
// Compile: g++ ProxyWSRateLimiter.cpp -o ProxyWSRateLimiter -lcurl -pthread -std=c++17

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;
using ms = chrono::milliseconds;

// ----------------------------- Utility: Read buffer callback for libcurl -----------------------------
static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    string* s = static_cast<string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

// ----------------------------- Node for LRU Cache -----------------------------
struct Node {
    string key;
    string value;
    long long timestamp_ms; // milliseconds since epoch
    Node(const string& k, const string& v) : key(k), value(v) {
        timestamp_ms = chrono::duration_cast<ms>(chrono::system_clock::now().time_since_epoch()).count();
    }
};

// ----------------------------- Thread-safe LRU Cache with statistics -----------------------------
class LRUCache {
public:
    LRUCache(size_t capacity) : capacity_(capacity), hitCount_(0), missCount_(0), evictionCount_(0) {}

    // Get returns empty string if not present
    string get(const string& key) {
        lock_guard<mutex> lock(mtx_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            ++missCount_;
            return "";
        }
        // move to front
        list_.splice(list_.begin(), list_, it->second);
        ++hitCount_;
        return it->second->value;
    }

    void put(const string& key, const string& value) {
        lock_guard<mutex> lock(mtx_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->value = value;
            it->second->timestamp_ms = now_ms();
            list_.splice(list_.begin(), list_, it->second);
            return;
        }

        if (map_.size() >= capacity_) {
            // evict least recently used (back)
            auto &node = list_.back();
            map_.erase(node.key);
            list_.pop_back();
            ++evictionCount_;
        }

        list_.emplace_front(key, value);
        map_[key] = list_.begin();
    }

    // Stats accessors
    int getHitCount() { lock_guard<mutex> lock(mtx_); return hitCount_; }
    int getMissCount() { lock_guard<mutex> lock(mtx_); return missCount_; }
    int getEvictionCount() { lock_guard<mutex> lock(mtx_); return evictionCount_; }
    void resetStatistics() { lock_guard<mutex> lock(mtx_); hitCount_ = missCount_ = evictionCount_ = 0; }

private:
    long long now_ms() {
        return chrono::duration_cast<ms>(chrono::system_clock::now().time_since_epoch()).count();
    }

    size_t capacity_;
    list<Node> list_; // front = most recent
    unordered_map<string, list<Node>::iterator> map_;
    mutex mtx_;

    // stats
    int hitCount_;
    int missCount_;
    int evictionCount_;
};

// ----------------------------- Rate Limiter (Token Bucket per client) -----------------------------
class RateLimiter {
public:
    // maxTokens per windowMillis; tokens refill continuously proportional to time
    RateLimiter(int maxTokens, long long windowMillis) :
        maxTokens_(maxTokens),
        windowMillis_(windowMillis) {}

    bool allowRequest(const string& clientId) {
        lock_guard<mutex> lock(mtx_);
        auto &bucket = buckets_[clientId];
        long long now = epoch_ms();
        if (bucket.lastTime == 0) {
            bucket.lastTime = now;
            bucket.tokens = maxTokens_;
        }
        // elapsed time since last update
        long long elapsed = now - bucket.lastTime;
        if (elapsed > 0) {
            double refillRatePerMs = static_cast<double>(maxTokens_) / static_cast<double>(windowMillis_);
            int add = static_cast<int>(elapsed * refillRatePerMs);
            if (add > 0) {
                bucket.tokens = min(bucket.tokens + add, maxTokens_);
                bucket.lastTime = now;
            }
        }

        if (bucket.tokens > 0) {
            --bucket.tokens;
            // update lastTime
            bucket.lastTime = now;
            return true;
        }
        return false;
    }

private:
    struct Bucket {
        int tokens = 0;
        long long lastTime = 0; // ms
    };

    long long epoch_ms() {
        return chrono::duration_cast<ms>(chrono::system_clock::now().time_since_epoch()).count();
    }

    int maxTokens_;
    long long windowMillis_;
    unordered_map<string, Bucket> buckets_;
    mutex mtx_;
};

// ----------------------------- Simple Counting Semaphore -----------------------------
class SimpleSemaphore {
public:
    SimpleSemaphore(int count = 0) : count_(count) {}
    void acquire() {
        unique_lock<mutex> lock(mtx_);
        cv_.wait(lock, [this]() { return count_ > 0; });
        --count_;
    }
    void release() {
        {
            lock_guard<mutex> lock(mtx_);
            ++count_;
        }
        cv_.notify_one();
    }
private:
    int count_;
    mutex mtx_;
    condition_variable cv_;
};

// ----------------------------- Thread Pool -----------------------------
class ThreadPool {
public:
    ThreadPool(size_t n) : stop_(false) {
        for (size_t i = 0; i < n; ++i)
            workers_.emplace_back([this]() { this->worker(); });
    }

    ~ThreadPool() {
        {
            unique_lock<mutex> lock(queue_mtx_);
            stop_ = true;
        }
        queue_cv_.notify_all();
        for (auto &t : workers_) if (t.joinable()) t.join();
    }

    void enqueue(function<void()> fn) {
        {
            unique_lock<mutex> lock(queue_mtx_);
            tasks_.push(move(fn));
        }
        queue_cv_.notify_one();
    }

private:
    void worker() {
        while (true) {
            function<void()> task;
            {
                unique_lock<mutex> lock(queue_mtx_);
                queue_cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = move(tasks_.front());
                tasks_.pop();
            }
            try { task(); } catch (...) { /* swallow */ }
        }
    }

    vector<thread> workers_;
    queue<function<void()>> tasks_;
    mutex queue_mtx_;
    condition_variable queue_cv_;
    bool stop_;
};

// ----------------------------- Proxy Server -----------------------------
class ProxyServer {
public:
    ProxyServer(int port, int maxClients, int cacheSize, int maxRequests, long long timeWindowMillis)
        : port_(port),
          semaphore_(maxClients),
          pool_(maxClients),
          cache_(cacheSize),
          rateLimiter_(maxRequests, timeWindowMillis),
          maxClients_(maxClients) {
        curl_global_init(CURL_GLOBAL_ALL);
    }

    ~ProxyServer() {
        curl_global_cleanup();
    }

    void start() {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) {
            cerr << "Socket creation failed\n";
            return;
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            cerr << "Bind failed\n";
            close(server_fd);
            return;
        }

        if (listen(server_fd, 128) < 0) {
            cerr << "Listen failed\n";
            close(server_fd);
            return;
        }

        cout << "Proxy server is running on port: " << port_ << " ...\n";

        while (true) {
            sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientSocket = accept(server_fd, (struct sockaddr*)&clientAddr, &clientLen);
            if (clientSocket < 0) {
                cerr << "Accept failed\n";
                continue;
            }

            // wait for slot
            semaphore_.acquire();

            // submit to thread pool
            pool_.enqueue([this, clientSocket, clientAddr]() {
                handleClient(clientSocket, clientAddr);
                semaphore_.release();
            });
        }

        close(server_fd);
    }

private:
    string ipFromSockaddr(const sockaddr_in& a) {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
        return string(buf);
    }

    void handleClient(int clientSocket, sockaddr_in clientAddr) {
        // Read request (simple)
        string client_ip = ipFromSockaddr(clientAddr);
        string request;
        {
            // read first line
            char buffer[4096];
            ssize_t n = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) {
                close(clientSocket);
                return;
            }
            buffer[n] = '\0';
            request = string(buffer);
        }

        // parse first line for GET
        stringstream ss(request);
        string method, url;
        ss >> method >> url;
        if (method != "GET" || url.empty()) {
            sendError(clientSocket, "400 Bad Request");
            close(clientSocket);
            return;
        }

        // Rate limit per client IP
        if (!rateLimiter_.allowRequest(client_ip)) {
            sendError(clientSocket, "429 Too Many Requests");
            close(clientSocket);
            return;
        }

        // Use url as cache key
        string cached = cache_.get(url);
        if (!cached.empty()) {
            sendResponse(clientSocket, cached);
            close(clientSocket);
            return;
        }

        // fetch remote via libcurl
        string remoteBody = fetchFromRemote(url);
        if (remoteBody.empty()) {
            sendError(clientSocket, "404 Not Found");
            close(clientSocket);
            return;
        }

        cache_.put(url, remoteBody);
        sendResponse(clientSocket, remoteBody);
        close(clientSocket);
    }

    string fetchFromRemote(const string& url) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // 10 second timeout

        CURLcode res = curl_easy_perform(curl);
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            cerr << "libcurl error: " << curl_easy_strerror(res) << "\n";
            return "";
        }
        if (response_code != 200) {
            // treat non-200 as not found/failed for simplicity
            cerr << "Remote server responded with code: " << response_code << "\n";
            return "";
        }
        return response;
    }

    void sendResponse(int sock, const string& body) {
        stringstream ss;
        ss << "HTTP/1.1 200 OK\r\n";
        ss << "Content-Length: " << body.size() << "\r\n";
        ss << "Content-Type: text/html\r\n";
        ss << "\r\n";
        ss << body;
        string out = ss.str();
        sendAll(sock, out.c_str(), out.size());
    }

    void sendError(int sock, const string& statusLine) {
        stringstream ss;
        ss << "HTTP/1.1 " << statusLine << "\r\n";
        ss << "Content-Length: 0\r\n";
        ss << "\r\n";
        string out = ss.str();
        sendAll(sock, out.c_str(), out.size());
    }

    bool sendAll(int sock, const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(sock, data + sent, len - sent, 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    int port_;
    SimpleSemaphore semaphore_;
    ThreadPool pool_;
    LRUCache cache_;
    RateLimiter rateLimiter_;
    int maxClients_;
};

// ----------------------------- main -----------------------------
int main() {
    int port = 8080;
    int maxClients = 10;
    int cacheSize = 5;
    int maxRequests = 5;               // per time window
    long long timeWindowMillis = 60000; // 1 minute

    ProxyServer server(port, maxClients, cacheSize, maxRequests, timeWindowMillis);
    server.start();

    return 0;
}
