#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <zlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

// Thread-safe cache system
class ImageCache {
private:
    struct CacheEntry {
        std::vector<uint8_t> compressed_data;
        size_t original_size;
        time_t timestamp;
    };
    
    std::unordered_map<std::string, CacheEntry> cache;
    std::mutex cache_mutex;
    const size_t MAX_CACHE_SIZE = 100; // 100 images max
    const time_t CACHE_TTL = 3600; // 1 hour

public:
    std::string get(const std::string& key) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            // Check TTL
            if (std::time(nullptr) - it->second.timestamp < CACHE_TTL) {
                // Decompress
                std::string result(it->second.original_size, 0);
                uLongf dest_len = it->second.original_size;
                uncompress((Bytef*)result.data(), &dest_len, 
                          it->second.compressed_data.data(), it->second.compressed_data.size());
                return result;
            } else {
                cache.erase(it);
            }
        }
        return "";
    }

    void put(const std::string& key, const std::string& data) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        // Clean old entries if cache is full
        if (cache.size() >= MAX_CACHE_SIZE) {
            auto oldest = cache.begin();
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (it->second.timestamp < oldest->second.timestamp) {
                    oldest = it;
                }
            }
            cache.erase(oldest);
        }

        // Compress and store
        uLongf compressed_size = compressBound(data.size());
        std::vector<uint8_t> compressed(compressed_size);
        compress(compressed.data(), &compressed_size, 
                (const Bytef*)data.data(), data.size());
        compressed.resize(compressed_size);
        
        cache[key] = {compressed, data.size(), std::time(nullptr)};
    }
};

// Thread pool for handling requests
class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;

public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while(true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if(this->stop && this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        stop = true;
        condition.notify_all();
        for(std::thread &worker : workers)
            worker.join();
    }
};

struct ImageData {
    int width;
    int height;
    std::vector<std::vector<std::vector<uint8_t>>> pixels;
};

class SimpleImageServer {
private:
    static ImageCache response_cache;
    
    static std::string getTempFilePath() {
        char filename[] = "/tmp/imgXXXXXX";
        int fd = mkstemp(filename);
        if (fd == -1) throw std::runtime_error("Failed to create temp file");
        close(fd);
        return std::string(filename);
    }

    static bool downloadImageFromUrl(const std::string& url, const std::string& localPath) {
        std::string cmd = "curl -s --connect-timeout 10 --max-time 30 -o \"" + localPath + "\" \"" + url + "\"";
        int result = system(cmd.c_str());
        return result == 0;
    }

    static std::string createJsonResponseOptimized(const ImageData& imageData) {
        std::ostringstream json;
        json << "{\n";
        json << "  \"width\": " << imageData.width << ",\n";
        json << "  \"height\": " << imageData.height << ",\n";
        json << "  \"pixels\": [\n";

        // Pre-allocate memory for better performance
        json.str().reserve(imageData.width * imageData.height * 15); // Estimate size

        for (int y = 0; y < imageData.height; ++y) {
            json << "    [";
            for (int x = 0; x < imageData.width; ++x) {
                const auto& pixel = imageData.pixels[y][x];
                json << "[" << (int)pixel[0] << "," << (int)pixel[1] << "," << (int)pixel[2] << "]";
                if (x < imageData.width - 1) json << ",";
            }
            json << "]";
            if (y < imageData.height - 1) json << ",";
            json << "\n";
        }

        json << "  ]\n";
        json << "}";
        return json.str();
    }

    static std::string compressString(const std::string& str) {
        uLongf compressed_size = compressBound(str.size());
        std::vector<Bytef> compressed(compressed_size);
        
        if (compress(compressed.data(), &compressed_size, 
                    reinterpret_cast<const Bytef*>(str.data()), str.size()) == Z_OK) {
            return std::string(compressed.begin(), compressed.begin() + compressed_size);
        }
        return str;
    }

    static std::string generateCacheKey(const std::string& url, int resize) {
        return url + "|" + std::to_string(resize);
    }

public:
    static ImageData loadImageOptimized(const std::string& filename, int max_size = 0) {
        std::cout << "Loading -> " << filename << " (resize: " << max_size << ")" << std::endl;

        std::string localPath = filename;
        bool isUrl = (filename.find("http://") == 0 || filename.find("https://") == 0);

        if (isUrl) {
            localPath = getTempFilePath();
            if (!downloadImageFromUrl(filename, localPath)) {
                throw std::runtime_error("Failed to download URL: " + filename);
            }
        }

        int width, height, channels;
        unsigned char* data = stbi_load(localPath.c_str(), &width, &height, &channels, 3);

        if (isUrl) {
            std::remove(localPath.c_str());
        }

        if (!data) {
            throw std::runtime_error("Failed to load image: " + filename);
        }

        // Handle resizing more efficiently
        std::vector<unsigned char> imageData;
        if (max_size > 0 && (width > max_size || height > max_size)) {
            int new_width, new_height;
            if (width > height) {
                new_width = max_size;
                new_height = (height * max_size) / width;
            } else {
                new_height = max_size;
                new_width = (width * max_size) / height;
            }
            
            std::vector<unsigned char> resized_data(new_width * new_height * 3);
            stbir_resize_uint8(
                data, width, height, 0,
                resized_data.data(), new_width, new_height, 0,
                3
            );
            imageData = std::move(resized_data);
            width = new_width;
            height = new_height;
        } else {
            imageData.assign(data, data + width * height * 3);
        }

        stbi_image_free(data);

        ImageData result;
        result.width = width;
        result.height = height;
        result.pixels.resize(height);

        // Optimized pixel conversion
        for (int y = 0; y < height; ++y) {
            result.pixels[y].resize(width);
            for (int x = 0; x < width; ++x) {
                int index = (y * width + x) * 3;
                result.pixels[y][x] = {
                    imageData[index],
                    imageData[index + 1],
                    imageData[index + 2]
                };
            }
        }

        return result;
    }

    static void handleClient(int client_fd, const std::string& request) {
        std::string response;

        try {
            if (request.find("GET /?url=") != std::string::npos) {
                size_t url_start = request.find("url=") + 4;
                size_t url_end = request.find(" HTTP/");
                std::string image_url = request.substr(url_start, url_end - url_start);

                int resize = 0;
                size_t resize_pos = image_url.find("&resize=");
                if (resize_pos != std::string::npos) {
                    resize = std::stoi(image_url.substr(resize_pos + 8));
                    image_url = image_url.substr(0, resize_pos);
                }

                // URL decode
                std::string decoded_url;
                for (size_t i = 0; i < image_url.length(); ++i) {
                    if (image_url[i] == '%' && i + 2 < image_url.length()) {
                        std::string hex_str = image_url.substr(i + 1, 2);
                        int hex_val = std::stoi(hex_str, nullptr, 16);
                        decoded_url += (char)hex_val;
                        i += 2;
                    } else {
                        decoded_url += image_url[i];
                    }
                }

                // Check cache first
                std::string cache_key = generateCacheKey(decoded_url, resize);
                std::string cached_response = response_cache.get(cache_key);
                
                if (!cached_response.empty()) {
                    std::cout << "Cache hit for: " << cache_key << std::endl;
                    response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Access-Control-Allow-Origin: *\r\n"
                               "X-Cache: HIT\r\n"
                               "Content-Length: " + std::to_string(cached_response.size()) + "\r\n"
                               "\r\n" + cached_response;
                } else {
                    std::cout << "Cache miss for: " << cache_key << std::endl;
                    auto image_data = loadImageOptimized(decoded_url, resize);
                    std::string json_response = createJsonResponseOptimized(image_data);
                    
                    // Cache the response
                    response_cache.put(cache_key, json_response);
                    
                    response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Access-Control-Allow-Origin: *\r\n"
                               "X-Cache: MISS\r\n"
                               "Content-Length: " + std::to_string(json_response.size()) + "\r\n"
                               "\r\n" + json_response;
                }
            } else {
                std::string welcome = "{\"message\":\"Image Parser Server - Use /?url=IMAGE_URL[&resize=SIZE]\"}";
                response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: " + std::to_string(welcome.size()) + "\r\n"
                           "\r\n" + welcome;
            }
        } catch (const std::exception& e) {
            std::string error_msg = "{\"error\":\"Failed to process image: " + std::string(e.what()) + "\"}";
            response = "HTTP/1.1 500 Internal Server Error\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(error_msg.size()) + "\r\n"
                       "\r\n" + error_msg;
        }

        write(client_fd, response.c_str(), response.size());
        close(client_fd);
    }

    static void startServer(int port = 8787, int thread_count = 4) {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == 0) {
            perror("Socket failed");
            exit(EXIT_FAILURE);
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            perror("Bind failed");
            exit(EXIT_FAILURE);
        }

        if (listen(server_fd, 100) < 0) { // Increased backlog
            perror("Listen failed");
            exit(EXIT_FAILURE);
        }

        std::cout << "Server running at http://0.0.0.0:" << port << " with " << thread_count << " threads" << std::endl;

        ThreadPool pool(thread_count);

        while (true) {
            int addrlen = sizeof(address);
            int client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
            if (client_fd < 0) {
                perror("Accept failed");
                continue;
            }

            char buffer[8192];
            int bytesReceived = read(client_fd, buffer, sizeof(buffer) - 1);
            if (bytesReceived > 0) {
                buffer[bytesReceived] = '\0';
                std::string request(buffer);
                
                pool.enqueue([client_fd, request]() {
                    handleClient(client_fd, request);
                });
            } else {
                close(client_fd);
            }
        }

        close(server_fd);
    }
};

ImageCache SimpleImageServer::response_cache;

int main() {
    std::cout << "=== Optimized Image Parser API ===" << std::endl;
    std::cout << "Features: Thread Pool, Response Cache, Compression" << std::endl;
    
    // Use number of CPU cores for thread pool
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    
    SimpleImageServer::startServer(8787, num_threads);
    return 0;
}
