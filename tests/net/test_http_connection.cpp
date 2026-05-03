// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/event_loop.hpp>

using namespace hpactor;
using namespace hpactor::net;

// Helper: create a connected socket pair (client_fd → server_fd)
static std::pair<int, int> make_socket_pair() {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(listener, reinterpret_cast<struct sockaddr*>(&addr), &len);
    listen(listener, 1);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, reinterpret_cast<struct sockaddr*>(&addr), len);
    int server = accept(listener, nullptr, nullptr);
    close(listener);
    return {client, server};
}

static StreamBuffer make_body(const char* s) {
    StreamBuffer b;
    b.append(reinterpret_cast<const uint8_t*>(s), strlen(s));
    return b;
}

int main() {
    // Test 1: HTTPConnection Server mode — parse incoming HTTP request
    {
        auto [client_fd, server_fd] = make_socket_pair();
        EventLoop loop;
        loop.run();

        std::string received_path;
        StreamBuffer received_body;
        bool complete = false;

        auto conn = HTTPConnection::create(server_fd,
            LocalEndpoint, Ipv4Endpoint{}, &loop, HTTPConnectionMode::Server);
        conn->set_request_handler(
            [&](HTTPConnection*, HttpRequest&& req) {
                received_path = req.path;
                received_body = req.body;
                complete = true;
            });

        // Write HTTP request from client side
        const char* req = "POST /test HTTP/1.1\r\nHost: local\r\nContent-Length: 5\r\n\r\nhello";
        write(client_fd, req, strlen(req));

        // Process events until complete or timeout
        for (int i = 0; i < 20 && !complete; i++) {
            loop.wait(50);
            loop.process_completions();
        }

        assert(complete);
        assert(received_path == "/test");
        std::string body_str(received_body.begin(), received_body.end());
        assert(body_str == "hello");

        conn->close();
        close(client_fd);
        loop.stop();
    }

    // Test 2: HTTPConnection — send_response builds valid HTTP wire bytes
    {
        auto [client_fd, server_fd] = make_socket_pair();
        EventLoop loop;
        loop.run();

        auto conn = HTTPConnection::create(server_fd,
            LocalEndpoint, Ipv4Endpoint{}, &loop, HTTPConnectionMode::Server);

        // Send a request first, then the handler sends a response
        const char* req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        write(client_fd, req, strlen(req));

        bool req_done = false;
        conn->set_request_handler([&](HTTPConnection* c, HttpRequest&&) {
            c->send_response(HttpStatusCode::OK,
                {{"Content-Type", "text/plain"}}, make_body("ok"));
            req_done = true;
        });

        for (int i = 0; i < 20 && !req_done; i++) {
            loop.wait(50);
            loop.process_completions();
        }
        assert(req_done);

        // Give async send time to complete
        loop.wait(100);
        loop.process_completions();

        // Read response on client side
        char buf[4096] = {};
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        assert(n > 0);
        buf[n] = '\0';
        assert(strstr(buf, "200 OK") != nullptr);
        assert(strstr(buf, "ok") != nullptr);

        conn->close();
        close(client_fd);
        loop.stop();
    }

    // Test 3: HTTPConnection Client mode — parse incoming HTTP response
    {
        auto [client_fd, server_fd] = make_socket_pair();
        EventLoop loop;
        loop.run();

        int resp_status = 0;
        bool resp_done = false;

        auto conn = HTTPConnection::create(client_fd,
            LocalEndpoint, Ipv4Endpoint{}, &loop, HTTPConnectionMode::Client);
        conn->set_response_handler(
            [&](HTTPConnection*, int status, std::vector<HttpHeader>, StreamBuffer) {
                resp_status = status;
                resp_done = true;
            });

        // Write HTTP response from server side
        const char* resp = "HTTP/1.1 201 Created\r\nContent-Length: 3\r\n\r\nyes";
        write(server_fd, resp, strlen(resp));

        for (int i = 0; i < 20 && !resp_done; i++) {
            loop.wait(50);
            loop.process_completions();
        }

        assert(resp_done);
        assert(resp_status == 201);

        conn->close();
        close(server_fd);
        loop.stop();
    }

    return 0;
}
