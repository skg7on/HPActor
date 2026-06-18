// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/net/http_types.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hpactor {
namespace net {
class HTTPConnection;
struct HttpRequest;
} // namespace net
namespace cli {

class CliHttpServerActor;

class IHttpHandler {
  public:
    virtual ~IHttpHandler() = default;
    virtual void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                        net::HttpRequest&& req) = 0;
};

class HttpHandlerRegistry {
  public:
    static HttpHandlerRegistry& instance();
    struct Entry {
        net::HttpMethod method;
        std::string pattern;
        IHttpHandler* handler;
    };
    void add(net::HttpMethod method, std::string pattern,
             std::unique_ptr<IHttpHandler> handler);
    const std::vector<Entry>& routes() const {
        return routes_;
    }

  private:
    HttpHandlerRegistry() = default;
    std::vector<std::unique_ptr<IHttpHandler>> owned_;
    std::vector<Entry> routes_;
};

bool match_route_pattern(const std::string& pattern, const std::string& path,
                         std::unordered_map<std::string, std::string>& params);

} // namespace cli
} // namespace hpactor
