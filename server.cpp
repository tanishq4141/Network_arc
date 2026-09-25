#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>
#include <cstdlib>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string path;                                // e.g. "/add"
    std::map<std::string, std::string> query_params; // e.g. {"a":"2", "b":"3"}
    std::map<std::string, std::string> headers;      // lowercase keys
    std::string body;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Lowercase a string in-place and return it.
static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// URL-decode a percent-encoded string (minimal: handles %XX and +).
static std::string url_decode(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size()) {
            int hi = std::stoi(src.substr(i + 1, 2), nullptr, 16);
            out += static_cast<char>(hi);
            i += 2;
        } else if (src[i] == '+') {
            out += ' ';
        } else {
            out += src[i];
        }
    }
    return out;
}

// Parse "a=2&b=3" into a map.
static std::map<std::string, std::string> parse_query_string(const std::string& qs) {
    std::map<std::string, std::string> params;
    std::istringstream stream(qs);
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq));
            std::string val = url_decode(pair.substr(eq + 1));
            params[key] = val;
        }
    }
    return params;
}

// ---------------------------------------------------------------------------
// HTTP request reading — the hard part
// ---------------------------------------------------------------------------

// Read exactly one HTTP request from `client_fd`, using `buffer` to carry over
// leftover bytes between calls on the same persistent connection.
// Returns false if the client disconnected (nothing left to read).
static bool read_request(int client_fd, std::string& buffer, HttpRequest& req) {
    const size_t CHUNK = 4096;
    char tmp[CHUNK];

    // 1. Read until we have the complete header block (\r\n\r\n).
    while (true) {
        size_t hdr_end = buffer.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            break; // headers are fully in the buffer
        }
        ssize_t n = recv(client_fd, tmp, CHUNK, 0);
        if (n <= 0) {
            return false; // client closed or error
        }
        buffer.append(tmp, static_cast<size_t>(n));
    }

    // 2. Split header block from any trailing data.
    size_t hdr_end = buffer.find("\r\n\r\n");
    std::string header_block = buffer.substr(0, hdr_end);
    buffer.erase(0, hdr_end + 4); // remove headers + \r\n\r\n

    // 3. Parse the request line.
    std::istringstream hdr_stream(header_block);
    std::string request_line;
    std::getline(hdr_stream, request_line);
    // Remove trailing \r if present
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }

    // "GET /add?a=2&b=3 HTTP/1.1"
    std::istringstream rl(request_line);
    std::string http_version;
    std::string raw_target;
    rl >> req.method >> raw_target >> http_version;

    // Separate path and query string
    auto qmark = raw_target.find('?');
    if (qmark != std::string::npos) {
        req.path = raw_target.substr(0, qmark);
        req.query_params = parse_query_string(raw_target.substr(qmark + 1));
    } else {
        req.path = raw_target;
        req.query_params.clear();
    }

    // 4. Parse headers.
    req.headers.clear();
    std::string line;
    while (std::getline(hdr_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = to_lower(line.substr(0, colon));
            std::string val = line.substr(colon + 1);
            // Trim leading whitespace from value
            size_t start = val.find_first_not_of(' ');
            if (start != std::string::npos) {
                val = val.substr(start);
            }
            req.headers[key] = val;
        }
    }

    // 5. Read body if Content-Length is present.
    req.body.clear();
    auto cl_it = req.headers.find("content-length");
    if (cl_it != req.headers.end()) {
        size_t content_length = static_cast<size_t>(std::stoul(cl_it->second));
        // We may already have some (or all) body bytes in the buffer.
        while (buffer.size() < content_length) {
            ssize_t n = recv(client_fd, tmp, CHUNK, 0);
            if (n <= 0) {
                return false;
            }
            buffer.append(tmp, static_cast<size_t>(n));
        }
        req.body = buffer.substr(0, content_length);
        buffer.erase(0, content_length);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Routing & calculation
// ---------------------------------------------------------------------------

static void route_request(const HttpRequest& req, int& status, std::string& body) {
    body.clear();

    // 1. Only GET is allowed → 405 Method Not Allowed
    if (req.method != "GET") {
        status = 405;
        return;
    }

    // 2. Only known paths → 404 Not Found
    static const std::set<std::string> valid_paths = {"/add", "/sub", "/mul", "/div"};
    if (valid_paths.find(req.path) == valid_paths.end()) {
        status = 404;
        return;
    }

    // 3. Both 'a' and 'b' must be present
    auto it_a = req.query_params.find("a");
    auto it_b = req.query_params.find("b");
    if (it_a == req.query_params.end() || it_b == req.query_params.end()) {
        status = 400;
        return;
    }

    // 4. Both must be valid integers
    int a, b;
    try {
        size_t pos_a = 0, pos_b = 0;
        a = std::stoi(it_a->second, &pos_a);
        b = std::stoi(it_b->second, &pos_b);
        // Make sure the entire string was consumed (reject "2x", etc.)
        if (pos_a != it_a->second.size() || pos_b != it_b->second.size()) {
            status = 400;
            return;
        }
    } catch (...) {
        status = 400;
        return;
    }

    // 5. Division by zero → 400
    if (req.path == "/div" && b == 0) {
        status = 400;
        return;
    }

    // 6. Compute
    int result = 0;
    if      (req.path == "/add") result = a + b;
    else if (req.path == "/sub") result = a - b;
    else if (req.path == "/mul") result = a * b;
    else if (req.path == "/div") result = a / b;

    status = 200;
    body = std::to_string(result);
}

// ---------------------------------------------------------------------------
// Response builder
// ---------------------------------------------------------------------------

static std::string build_response(int status, const std::string& body) {
    const char* status_text = "OK";
    switch (status) {
        case 200: status_text = "OK";                  break;
        case 400: status_text = "Bad Request";          break;
        case 404: status_text = "Not Found";            break;
        case 405: status_text = "Method Not Allowed";   break;
        default:  status_text = "Internal Server Error"; break;
    }

    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << status_text << "\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    resp << "Content-Type: text/plain\r\n";
    resp << "Connection: keep-alive\r\n";
    resp << "\r\n";
    resp << body;
    return resp.str();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    // 1. Create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }

    // Allow port reuse so we can restart quickly
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Bind to port 8080
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(8080);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed\n";
        close(server_fd);
        return 1;
    }

    // 3. Listen
    if (listen(server_fd, 10) < 0) {
        std::cerr << "listen() failed\n";
        close(server_fd);
        return 1;
    }

    std::cout << "Calculator server listening on :8080\n";

    // 4. Accept loop
    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            std::cerr << "accept() failed\n";
            continue;
        }

        std::string buffer; // persistent buffer for this connection

        // Inner loop: handle multiple requests on the same connection
        HttpRequest req;
        while (read_request(client_fd, buffer, req)) {
            int status = 200;
            std::string body;

            // HTTP/1.1 requires a Host header → 400 if missing
            if (req.headers.find("host") == req.headers.end()) {
                status = 400;
                body = "";
            } else {
                route_request(req, status, body);
            }

            std::string response = build_response(status, body);

            // Send the full response
            const char* data = response.c_str();
            size_t remaining = response.size();
            while (remaining > 0) {
                ssize_t sent = send(client_fd, data, remaining, 0);
                if (sent <= 0) {
                    break; // client gone
                }
                data += sent;
                remaining -= static_cast<size_t>(sent);
            }
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}
