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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

struct ImageData {
    int width;
    int height;
    std::vector<std::vector<std::vector<uint8_t>>> pixels;
};

class SimpleImageServer {
private:
    static std::string getTempFilePath() {
        char filename[] = "/tmp/imgXXXXXX";
        int fd = mkstemp(filename);
        if (fd == -1) throw std::runtime_error("Failed to create temp file");
        close(fd);
        return std::string(filename);
    }

    static bool downloadImageFromUrl(const std::string& url, const std::string& localPath) {
        std::string cmd = "curl -s -o \"" + localPath + "\" \"" + url + "\"";
        int result = system(cmd.c_str());
        if (result != 0) {
            std::cerr << "curl failed with code: " << result << std::endl;
            return false;
        }
        std::ifstream file(localPath, std::ios::binary | std::ios::ate);
        if (!file.is_open() || file.tellg() == 0) {
            std::cerr << "Downloaded file is empty or doesn't exist" << std::endl;
            return false;
        }
        file.close();
        return true;
    }

    static std::string createJsonResponse(const ImageData& imageData) {
        std::string json = "{\n";
        json += "  \"width\": " + std::to_string(imageData.width) + ",\n";
        json += "  \"height\": " + std::to_string(imageData.height) + ",\n";
        json += "  \"pixels\": [\n";

        for (int y = 0; y < imageData.height; ++y) {
            json += "    [";
            for (int x = 0; x < imageData.width; ++x) {
                const auto& pixel = imageData.pixels[y][x];
                json += "[" + std::to_string(pixel[0]) + "," +
                    std::to_string(pixel[1]) + "," +
                    std::to_string(pixel[2]) + "]";
                if (x < imageData.width - 1) json += ",";
            }
            json += "]";
            if (y < imageData.height - 1) json += ",";
            json += "\n";
        }

        json += "  ]\n";
        json += "}";
        return json;
    }

public:
    static ImageData loadImage(const std::string& filename, int max_size = 0) {
        std::cout << "Loading -> " << filename << std::endl;

        std::string localPath = filename;
        bool isUrl = (filename.find("http://") == 0 || filename.find("https://") == 0);

        if (isUrl) {
            localPath = getTempFilePath();
            std::cout << "-> Downloading..." << std::endl;
            if (!downloadImageFromUrl(filename, localPath)) {
                throw std::runtime_error("Failed to download URL ->: " + filename);
            }
            std::cout << "To ->: " << localPath << std::endl;
        }

        int width, height, channels;
        unsigned char* data = stbi_load(localPath.c_str(), &width, &height, &channels, 3);

        if (isUrl) {
            std::remove(localPath.c_str());
        }

        if (!data) {
            throw std::runtime_error("Failed to load image -> " + filename);
        }

        std::vector<unsigned char> imageData(data, data + width * height * 3);
        stbi_image_free(data);

        if (max_size > 0) {
            int new_width = max_size;
            int new_height = max_size;
            std::vector<unsigned char> resized_data(new_width * new_height * 3);

            stbir_resize_uint8(
                imageData.data(), width, height, 0,
                resized_data.data(), new_width, new_height, 0,
                3
            );

            imageData = std::move(resized_data);
            width = new_width;
            height = new_height;
        }

        ImageData result;
        result.width = width;
        result.height = height;
        result.pixels.resize(height);

        for (int y = 0; y < height; ++y) {
            result.pixels[y].resize(width);
            for (int x = 0; x < width; ++x) {
                result.pixels[y][x] = {
                    imageData[(y * width + x) * 3],
                    imageData[(y * width + x) * 3 + 1],
                    imageData[(y * width + x) * 3 + 2]
                };
            }
        }

        return result;
    }

    static void startServer(int port = 8787) {
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

        if (listen(server_fd, 10) < 0) {
            perror("Listen failed");
            exit(EXIT_FAILURE);
        }

        std::cout << "Server running at http://0.0.0.0:" << port << std::endl;

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
                std::string response;

                if (request.find("GET /?url=") != std::string::npos) {
                    try {
                        size_t url_start = request.find("url=") + 4;
                        size_t url_end = request.find(" HTTP/");
                        std::string image_url = request.substr(url_start, url_end - url_start);

                        int resize = 0;
                        size_t resize_pos = image_url.find("&resize=");
                        if (resize_pos != std::string::npos) {
                            resize = std::stoi(image_url.substr(resize_pos + 8));
                            image_url = image_url.substr(0, resize_pos);
                        }

                        std::string decoded_url;
                        for (size_t i = 0; i < image_url.length(); ++i) {
                            if (image_url[i] == '%' && i + 2 < image_url.length()) {
                                std::string hex_str = image_url.substr(i + 1, 2);
                                int hex_val = std::stoi(hex_str, nullptr, 16);
                                decoded_url += (char)hex_val;
                                i += 2;
                            }
                            else {
                                decoded_url += image_url[i];
                            }
                        }

                        auto image_data = loadImage(decoded_url, resize);
                        std::string json_response = createJsonResponse(image_data);

                        response = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Access-Control-Allow-Origin: *\r\n"
                                   "Content-Length: " + std::to_string(json_response.size()) + "\r\n"
                                   "\r\n" + json_response;
                    } catch (const std::exception& e) {
                        std::string error_msg = "{\"error\":\"Failed: " + std::string(e.what()) + "\"}";
                        response = "HTTP/1.1 500 Internal Server Error\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Content-Length: " + std::to_string(error_msg.size()) + "\r\n"
                                   "\r\n" + error_msg;
                    }
                } else {
                    std::string welcome = "{\"message\":\"Image Parser Server - Use /?url=IMAGE_URL&resize=SIZE\"}";
                    response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: " + std::to_string(welcome.size()) + "\r\n"
                               "\r\n" + welcome;
                }

                write(client_fd, response.c_str(), response.size());
            }
            close(client_fd);
        }

        close(server_fd);
    }
};

int main() {
    std::cout << "=== API ===" << std::endl;
    SimpleImageServer::startServer(8787);
    return 0;
}
