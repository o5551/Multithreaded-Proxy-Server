# 🚀 Multithreaded Proxy Server with LRU Cache and Rate Limiting  

## 📌 Overview  
This is a **C++ -based multithreaded proxy server** that efficiently handles HTTP `GET` requests.  
It includes:  
- **LRU caching** to speed up repeated requests  
- **Rate limiting** to prevent excessive traffic  
- **Cache statistics tracking** for performance monitoring  

## ⚡ Features  

### ✅ Multithreaded Request Handling  
- Uses `ExecutorService` to process multiple client connections concurrently.  
- Supports `GET` requests.  

### ✅ LRU Caching for Faster Responses  
- Implements an **LRU (Least Recently Used) cache** to store frequently accessed responses.  
- Reduces repeated network calls by serving cached content.  

### ✅ Cache Statistics Tracking  
- Tracks:  
  - **Cache Hits** (Requests served from cache)  
  - **Cache Misses** (Requests fetched from the origin server)  
  - **Cache Evictions** (Entries removed due to capacity limits)  

### ✅ Rate Limiting (Token Bucket Algorithm)  
- Limits the number of requests per client within a given time window.  
- Prevents excessive traffic and server overload.  

---
## 🛠 Example Usage with Results  

### 1️⃣ GET Request through Proxy  

#### **Request:**  
GET http://example.com

#### **Result (First Request - Cache Miss):**  
Fetching from original server: http://example.com Cache Miss - Storing response in cache. <Response Content of example.com>

#### **Result (Subsequent Request - Cache Hit):**  
Serving from cache: http://example.com Cache Hit - Response served from cache. <Response Content of example.com>

---

### 2️⃣ Rate Limiting in Action  

#### **Request:**  
GET http://example.com (Sent multiple times rapidly)

#### **Result:**  
Serving from cache: http://example.com Serving from cache: http://example.com Request blocked - Too many requests from this client. Request blocked - Too many requests from this client. Serving from cache: http://example.com Request blocked - Too many requests from this client.

---

### 3️⃣ Cache Statistics  

#### **Request:**  
GET http://localhost:8080/stats

#### **Result:**  
{ "cache_hits": 5, "cache_misses": 2, "cache_evictions": 1 }

---
