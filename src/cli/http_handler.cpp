// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#include <hpactor/cli/http_handler.hpp>

namespace hpactor::cli {

HttpHandlerRegistry& HttpHandlerRegistry::instance() {
    static HttpHandlerRegistry reg;
    return reg;
}

void HttpHandlerRegistry::add(net::HttpMethod method, std::string pattern,
                              std::unique_ptr<IHttpHandler> handler) {
    IHttpHandler* ptr = handler.get();
    owned_.push_back(std::move(handler));
    routes_.push_back({method, std::move(pattern), ptr});
}

} // namespace hpactor::cli
