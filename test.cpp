#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>

#define STB_IMAGE_IMPLEMENTATION
#include "C:\Users\K9nx._\Downloads\stb-master\stb-master\stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../../../Downloads/stb-master/stb-master/deprecated/stb_image_resize.h"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wininet.lib")

struct ImageData {
    int width;
    int height;
    std::vector<std::vector<std::vector<uint8_t>>> pixels;
};

class SimpleImageServer {
private:
    static std::string getTempFilePath() {
        char tempPath[MAX_PATH];
        char tempFile[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPath);
        GetTempFileNameA(tempPath, "IMG", 0, tempFile);
        return std::string(tempFile);
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
            std::cout << "->..." << std::endl;
            if (!downloadImageFromUrl(filename, localPath)) {
                throw std::runtime_error("WTF URL ->: " + filename);
            }
            std::cout << "To ->: " << localPath << std::endl;
        }

        int width, height, channels;
        unsigned char* data = stbi_load(localPath.c_str(), &width, &height, &channels, 3);
        
        if (isUrl) {
            std::remove(localPath.c_str());
        }

        if (!data) {
            throw std::runtime_error("WTF IMAGE ->" + filename);
        }

        std::cout << "Size X,Y,Z->: " << width << "x" << height << std::endl;

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

            std::cout << "Resized ->" << width << "x" << height << std::endl;
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

        std::cout << "==============================" << std::endl;
        return result;
    }

    static void startServer(int port = 8787) {

        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed" << std::endl;
            return;
        }

        SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (serverSocket == INVALID_SOCKET) {
            std::cerr << "Socket creation failed" << std::endl;
            WSACleanup();
            return;
        }

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port);

        if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "Bind failed" << std::endl;
            closesocket(serverSocket);
            WSACleanup();
            return;
        }

        if (listen(serverSocket, 10) == SOCKET_ERROR) {
            std::cerr << "Listen failed" << std::endl;
            closesocket(serverSocket);
            WSACleanup();
            return;
        }

        std::cout << "->: http://localhost:" << port << std::endl;

        while (true) {
            SOCKET clientSocket = accept(serverSocket, NULL, NULL);
            if (clientSocket == INVALID_SOCKET) {
                std::cerr << "Accept failed" << std::endl;
                continue;
            }

            char buffer[4096];
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
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

                        std::cout << "Processing: " << decoded_url << " (resize: " << resize << ")" << std::endl;

                        auto image_data = loadImage(decoded_url, resize);
                        std::string json_response = createJsonResponse(image_data);

                        response = "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Content-Length: " + std::to_string(json_response.length()) + "\r\n"
                            "\r\n" + json_response;

                    }
                    catch (const std::exception& e) {
                        std::string error_msg = "{\"error\":\"Failed to process image: " + std::string(e.what()) + "\"}";
                        response = "HTTP/1.1 500 Internal Server Error\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: " + std::to_string(error_msg.length()) + "\r\n"
                            "\r\n" + error_msg;
                    }
                }
                else {
                    std::string welcome = "{\"message\":\"Image Parser Server - Use /?url=IMAGE_URL&resize=SIZE\"}";
                    response = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(welcome.length()) + "\r\n"
                        "\r\n" + welcome;
                }

                send(clientSocket, response.c_str(), response.length(), 0);
            }

            closesocket(clientSocket);
        }

        closesocket(serverSocket);
        WSACleanup();
    }
};

int main() {
    std::cout << "=== API ===" << std::endl;

    SimpleImageServer::startServer(8787);

    return 0;
}