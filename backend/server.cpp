// server.cpp
// Music recognition backend - single-threaded HTTP server with per-connection timeouts & upload limits.
// Depends on engine.h providing: engine_init(const char* data_dir),
// get_song_list(), add_song_to_db(path, name), identify_from_file(path)

#include "engine.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <cerrno>
#include <csignal>

using namespace std;

static const int PORT = 5001;
static const int BACKLOG = 16;
static const size_t MAX_HEADER = 64 * 1024;
static const size_t MAX_BODY = 200 * 1024 * 1024; // 200 MB upload cap
static const int RECV_TIMEOUT_SEC = 200;           // per-connection timeout in seconds

// Helper to send HTTP responses (includes CORS headers)
static void send_response(int client, const string& status, const string& body, const string& contentType="application/json") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: *\r\n"
        << "\r\n"
        << body;
    auto s = oss.str();
    // best-effort send (ignore return)
    send(client, s.data(), s.size(), 0);
}

static void send_204(int client) {
    std::string headers = "HTTP/1.1 204 No Content\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                          "Access-Control-Allow-Headers: *\r\n"
                          "Content-Length: 0\r\n\r\n";
    send(client, headers.data(), headers.size(), 0);
}

// URL-decode utility
static string url_decode(const string& s) {
    string out; out.reserve(s.size());
    for (size_t i=0;i<s.size();++i) {
        if (s[i]=='%' && i+2<s.size()) {
            int v = 0;
            std::istringstream iss(s.substr(i+1,2));
            iss >> std::hex >> v;
            out.push_back(static_cast<char>(v));
            i += 2;
        } else if (s[i]=='+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

static string get_query_param(const string& target, const string& key) {
    auto pos = target.find('?');
    if (pos==string::npos) return "";
    string qs = target.substr(pos+1);
    string k = key + "=";
    size_t p = qs.find(k);
    if (p==string::npos) return "";
    size_t s = p + k.size();
    size_t e = qs.find('&', s);
    string val = (e==string::npos) ? qs.substr(s) : qs.substr(s, e-s);
    return url_decode(val);
}

static string now_ts() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));
    return string(buf);
}

// read HTTP request: headers and body (honors Content-Length and enforces MAX_BODY & timeouts)
static bool read_request(int client, string& method, string& target, string& headers, string& body) {
    string data; data.reserve(MAX_HEADER);
    char buf[4096];
    size_t total=0;

    // Read headers (until \r\n\r\n)
    while (true) {
        ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // timed out while reading headers
                send_response(client, "408 Request Timeout", R"({"error":"timeout"})");
                return false;
            }
            return false;
        }
        if (n == 0) return false;
        data.append(buf, buf+n);
        total += static_cast<size_t>(n);
        auto pos = data.find("\r\n\r\n");
        if (pos != string::npos) {
            headers = data.substr(0, pos+4);
            body = data.substr(pos+4);
            break;
        }
        if (total > MAX_HEADER) {
            send_response(client, "431 Request Header Fields Too Large", R"({"error":"headers_too_large"})");
            return false;
        }
    }

    // Parse request line
    std::istringstream iss(headers);
    string line; getline(iss, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    std::istringstream rl(line);
    rl >> method >> target;

    // Find Content-Length (if any)
    size_t clen = 0;
    string hline;
    while (getline(iss, hline)) {
        if (!hline.empty() && hline.back()=='\r') hline.pop_back();
        auto p = hline.find(':');
        if (p!=string::npos) {
            string key = hline.substr(0,p);
            string val = hline.substr(p+1);
            auto l = val.find_first_not_of(" \t"); if (l!=string::npos) val=val.substr(l);
            if (strcasecmp(key.c_str(), "Content-Length")==0) {
                try { clen = static_cast<size_t>(stoll(val)); } catch(...) { clen = 0; }
            }
        }
    }

    // Enforce maximum body size (before receiving more)
    if (clen > MAX_BODY) {
        send_response(client, "413 Request Entity Too Large", R"({"error":"payload_too_large"})");
        return false;
    }

    // Read remaining body (if any)
    while (body.size() < clen) {
        ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                send_response(client, "408 Request Timeout", R"({"error":"timeout"})");
                return false;
            }
            return false;
        }
        if (n == 0) return false;
        body.append(buf, buf+n);
        if (body.size() > MAX_BODY) {
            send_response(client, "413 Request Entity Too Large", R"({"error":"payload_too_large"})");
            return false;
        }
    }

    return true;
}

int main() {
    // Ignore SIGPIPE so that a broken socket send() doesn't kill the process
    std::signal(SIGPIPE, SIG_IGN);

    engine_init("./data");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
    addr.sin_port = htons(PORT);

    if (::bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (::listen(server_fd, BACKLOG) < 0) {
        perror("listen"); return 1;
    }
    cerr << "Server listening on http://localhost:" << PORT << "\n";

    while (true) {
        int client = ::accept(server_fd, nullptr, nullptr);
        if (client < 0) continue;

        // set per-connection timeouts so slow/stalled clients don't hang the server
        timeval tv{};
        tv.tv_sec = RECV_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        string method, target, headers, body;
        if (!read_request(client, method, target, headers, body)) {
            close(client); continue;
        }

        if (method=="OPTIONS") { send_204(client); close(client); continue; }

        // Health check
        if (method=="GET" && target=="/ping") {
            send_response(client, "200 OK", R"({"ok":true})");
            close(client);
            continue;
        }

        // List songs
        if (method=="GET" && target.rfind("/songs",0)==0) {
            auto songs = get_song_list();
            std::ostringstream oss; oss << R"({"songs":[)";
            for (size_t i=0;i<songs.size();++i) {
                if (i) oss << ",";
                oss << R"({"id":)" << songs[i].id
                    << R"(,"name":")" << songs[i].name
                    << R"(","fingerprints":)" << songs[i].numFingerprints
                    << R"(,"url":")" << songs[i].youtube_url << R"("})";
            }
            oss << "]}";
            send_response(client, "200 OK", oss.str());
            close(client);
            continue;
        }

        // Upload - add song (expects audio bytes in body; uses add_song_to_db)
        if (method=="POST" && target.rfind("/upload",0)==0) {
            string name = get_query_param(target, "name");
            if (name.empty()) {
                send_response(client, "400 Bad Request", R"({"error":"A song label is required."})");
                close(client); continue;
            }
            try {
                std::filesystem::create_directories("./data/uploads");
                string path = "./data/uploads/upload_" + now_ts() + ".wav";
                FILE* f = fopen(path.c_str(), "wb");
                if (!f) {
                    send_response(client, "500 Internal Server Error", R"({"error":"Failed to write temporary file."})");
                    close(client); continue;
                }
                fwrite(body.data(), 1, body.size(), f);
                fclose(f);

                int id = add_song_to_db(path, name);
                std::filesystem::remove(path);

                if (id < 0) {
                    send_response(client, "400 Bad Request", R"({"error":"Fingerprinting failed. Check WAV format."})");
                } else {
                    std::ostringstream oss;
                    oss << R"({"name":")" << name << R"("})";
                    send_response(client, "200 OK", oss.str());
                }
            } catch (const std::exception &ex) {
                std::ostringstream err; err << R"({"error":"server_exception","msg":")" << ex.what() << R"("})";
                send_response(client, "500 Internal Server Error", err.str());
            }
            close(client);
            continue;
        }

        // Recognize
        if (method=="POST" && target.rfind("/recognize",0)==0) {
            try {
                std::filesystem::create_directories("./data/queries");
                string qpath = "./data/queries/query_" + now_ts() + ".wav";
                FILE* f = fopen(qpath.c_str(), "wb");
                if (!f) {
                    send_response(client, "500 Internal Server Error", R"({"error":"write_failed"})");
                    close(client); continue;
                }
                fwrite(body.data(), 1, body.size(), f);
                fclose(f);
                auto res = identify_from_file(qpath);
                // Optionally remove query file if you don't want disk persistence
                // std::filesystem::remove(qpath);
                send_response(client, "200 OK", res);
            } catch (const std::exception &ex) {
                std::ostringstream err; err << R"({"error":"server_exception","msg":")" << ex.what() << R"("})";
                send_response(client, "500 Internal Server Error", err.str());
            }
            close(client);
            continue;
        }

        // Not found
        send_response(client, "404 Not Found", R"({"error":"not_found"})");
        close(client);
    }

    close(server_fd);
    return 0;
}
