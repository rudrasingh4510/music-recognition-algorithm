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
#include <cstdlib> // For system()
#include <cstdio>  // For popen()
#include <memory>  // For unique_ptr

using namespace std;

static const int PORT = 5001;
static const int BACKLOG = 16;
static const size_t MAX_HEADER = 64 * 1024;

// Helper function to execute a command and capture its standard output.
static string exec_and_get_output(const string& cmd) {
    char buffer[128];
    string result = "";
    unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return ""; // popen failed
    }
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    // Remove trailing newline character, if it exists.
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

// Helper function to remove timestamps and other params from a YouTube URL.
static string sanitize_youtube_url(const string& url) {
    string video_id;

    size_t v_pos = url.find("v=");
    if (v_pos != string::npos) {
        size_t id_start = v_pos + 2;
        size_t id_end = url.find('&', id_start);
        video_id = url.substr(id_start, id_end - id_start);
    } else {
        size_t short_pos = url.find("youtu.be/");
        if (short_pos != string::npos) {
            size_t id_start = short_pos + 9;
            size_t id_end = url.find('?', id_start);
            video_id = url.substr(id_start, id_end - id_start);
        }
    }
    
    if (!video_id.empty()) {
        if (video_id.length() > 11) {
            video_id = video_id.substr(0, 11);
        }
        return "https://www.youtube.com/watch?v=" + video_id;
    }

    return url; // Fallback
}


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

// Simple JSON value extractor
static string get_json_val(const string& json, const string& key) {
    string key_str = "\"" + key + "\":\"";
    auto p1 = json.find(key_str);
    if (p1 == string::npos) return "";
    p1 += key_str.length();
    auto p2 = json.find("\"", p1);
    if (p2 == string::npos) return "";
    return json.substr(p1, p2-p1);
}

static string now_ts() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));
    return string(buf);
}

static bool read_request(int client, string& method, string& target, string& headers, string& body) {
    string data; data.reserve(MAX_HEADER);
    char buf[4096];
    size_t total=0;
    while (true) {
        ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n<=0) return false;
        data.append(buf, buf+n);
        total += n;
        auto pos = data.find("\r\n\r\n");
        if (pos != string::npos) {
            headers = data.substr(0, pos+4);
            body = data.substr(pos+4);
            break;
        }
        if (total > MAX_HEADER) return false;
    }

    std::istringstream iss(headers);
    string line; getline(iss, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    std::istringstream rl(line);
    rl >> method >> target;

    size_t clen = 0;
    string hline;
    while (getline(iss, hline)) {
        if (!hline.empty() && hline.back()=='\r') hline.pop_back();
        auto p = hline.find(':');
        if (p!=string::npos) {
            string key = hline.substr(0,p);
            string val = hline.substr(p+1);
            auto l = val.find_first_not_of(" \t"); if (l!=string::npos) val=val.substr(l);
            if (strcasecmp(key.c_str(), "Content-Length")==0) clen = static_cast<size_t>(stoll(val));
        }
    }
    while (body.size() < clen) {
        ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n<=0) break;
        body.append(buf, buf+n);
    }
    return true;
}

int main() {
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

        string method, target, headers, body;
        if (!read_request(client, method, target, headers, body)) {
            close(client); continue;
        }

        if (method=="OPTIONS") { send_204(client); close(client); continue; }

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
        }
        else if (method=="POST" && target.rfind("/add-youtube",0)==0) {
            string url = get_json_val(body, "url");
            string name = get_json_val(body, "name");
            
            if (url.empty()) {
                send_response(client, "400 Bad Request", R"({"error":"missing_url"})");
            } else {
                string clean_url = sanitize_youtube_url(url);
                string display_name = name;
                string basename = "yt_" + now_ts();

                if (display_name.empty()) {
                    string title_cmd = "yt-dlp --get-title \"" + clean_url + "\"";
                    display_name = exec_and_get_output(title_cmd);
                }
                if (display_name.empty()) {
                    display_name = basename;
                }

                std::filesystem::create_directories("./data/uploads");
                string out_template = "./data/uploads/" + basename;
                string cmd = "yt-dlp -x --audio-format wav --postprocessor-args \"-ar 44100\" -o \"" + out_template + ".%(ext)s\" \"" + clean_url + "\"";

                int ret = system(cmd.c_str());
                string final_path = out_template + ".wav";

                if (ret != 0 || !filesystem::exists(final_path)) {
                    send_response(client, "500 Internal Server Error", R"({"error":"download_failed"})");
                } else {
                    int id = add_song_to_db(final_path, display_name, clean_url);
                    filesystem::remove(final_path);

                    if (id < 0) {
                        send_response(client, "400 Bad Request", R"({"error":"fingerprint_failed"})");
                    } else {
                        // Return a simpler JSON object, just the name.
                        std::ostringstream oss;
                        oss << R"({"name":")" << display_name << R"("})";
                        send_response(client, "200 OK", oss.str());
                    }
                }
            }
        }
        else if (method=="POST" && target.rfind("/recognize",0)==0) {
            std::filesystem::create_directories("./data/queries");
            string qpath = "./data/queries/query_" + now_ts() + ".wav";
            FILE* f = fopen(qpath.c_str(), "wb");
            if (!f) {
                send_response(client, "500 Internal Server Error", R"({"error":"write_failed"})");
            } else {
                fwrite(body.data(), 1, body.size(), f);
                fclose(f);
                auto res = identify_from_file(qpath);
                send_response(client, "200 OK", res);
            }
        }
        else {
            send_response(client, "404 Not Found", R"({"error":"not_found"})");
        }
        close(client);
    }
    return 0;
}