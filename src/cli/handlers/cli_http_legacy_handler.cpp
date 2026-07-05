// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "cli_http_handler_helpers.hpp"
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli.pb.h>
#include <hpactor/cli/actor/cli_http_server_actor.hpp>
#include <hpactor/cli/command/cli_session.hpp>
#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/cli/http_handler.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>
#include <string>

namespace hpactor::cli::handlers {
namespace {
using adt::extract_json_array_raw, adt::extract_json_object_raw,
    adt::extract_json_string, adt::json_escape;
using adt::parse_json_string_array, adt::parse_json_string_map, adt::skip_json_ws;

bool parse_cli_command_json(const std::string& json, CliCommand& cmd) {
    size_t pos = skip_json_ws(json, 0);
    if (pos >= json.size() || json[pos] != '{')
        return false;
    ++pos;
    std::string ck;
    while (pos < json.size()) {
        pos = skip_json_ws(json, pos);
        if (pos >= json.size())
            break;
        if (json[pos] == '}') {
            ++pos;
            break;
        }
        if (json[pos] == ',') {
            ++pos;
            ck.clear();
            continue;
        }
        if (json[pos] == '"' && ck.empty()) {
            ck = extract_json_string(json, pos);
            pos = skip_json_ws(json, pos);
            if (pos < json.size() && json[pos] == ':')
                ++pos;
            pos = skip_json_ws(json, pos);
            if (pos >= json.size())
                break;
            if (ck == "path") {
                if (json[pos] == '"')
                    cmd.set_path(extract_json_string(json, pos));
                ck.clear();
            } else if (ck == "format") {
                if (json[pos] == '"')
                    cmd.set_format(extract_json_string(json, pos));
                ck.clear();
            } else if (ck == "params") {
                if (json[pos] == '{') {
                    auto obj_raw = extract_json_object_raw(json, pos);
                    auto pairs = parse_json_string_map(obj_raw);
                    auto* pm = cmd.mutable_params();
                    for (auto& [k, v] : pairs)
                        (*pm)[k] = v;
                }
                ck.clear();
            } else if (ck == "args") {
                if (json[pos] == '[') {
                    auto arr_raw = extract_json_array_raw(json, pos);
                    for (auto& v : parse_json_string_array(arr_raw))
                        cmd.add_args(std::move(v));
                }
                ck.clear();
            } else {
                if (pos < json.size() && json[pos] == '"')
                    extract_json_string(json, pos);
                else if (pos < json.size() && json[pos] == '{')
                    extract_json_object_raw(json, pos);
                else if (pos < json.size() && json[pos] == '[')
                    extract_json_array_raw(json, pos);
                else
                    while (pos < json.size() && json[pos] != ',' && json[pos] != '}')
                        ++pos;
                ck.clear();
            }
        } else {
            ++pos;
        }
    }
    return true;
}

void send_json_response(net::HTTPConnection* conn, net::HttpStatusCode sc,
                        const CliResponse& resp) {
    std::string j =
        "{\"content_type\":\"" + json_escape(resp.content_type()) +
        "\",\"payload\":\"" + json_escape(resp.payload()) +
        "\",\"is_error\":" + std::string(resp.is_error() ? "true" : "false") +
        ",\"error_code\":" + std::to_string(resp.error_code()) +
        ",\"is_structured\":" +
        std::string(resp.is_structured() ? "true" : "false") + "}";
    StreamBuffer sb(reinterpret_cast<const uint8_t*>(j.data()),
                    reinterpret_cast<const uint8_t*>(j.data() + j.size()));
    conn->send_response(sc, {{"Content-Type", "application/json"}}, sb);
}
} // namespace

class LegacyPostCliHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::POST;
    static constexpr std::string_view kPath = "/cli";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        if (!actor.config().legacy_cli_endpoint) {
            send_error(&conn, net::HttpStatusCode::NotFound, "NOT_FOUND",
                       "Legacy /cli not enabled");
            return;
        }
        if (!validate_json_content_type(&conn, req))
            return;
        std::string bs(reinterpret_cast<const char*>(req.body.data()),
                       req.body.size());
        CliCommand cmd;
        if (!parse_cli_command_json(bs, cmd)) {
            CliResponse er;
            er.set_content_type("text/plain");
            er.set_payload("Invalid JSON in request body");
            er.set_is_error(true);
            er.set_error_code(400);
            send_json_response(&conn, net::HttpStatusCode::BadRequest, er);
            return;
        }
        std::string cl = "/";
        if (!cmd.path().empty()) {
            for (char c : cmd.path()) {
                if (c == '/')
                    cl += ' ';
                else
                    cl += c;
            }
        }
        for (auto& [k, v] : cmd.params()) {
            if (v == "true")
                cl += " --" + k;
            else
                cl += " --" + k + " " + v;
        }
        std::string fmt = cmd.format();
        if (!fmt.empty())
            cl += " --format " + fmt;
        for (auto& a : cmd.args())
            cl += " " + a;
        std::string ct = "text/plain";
        if (fmt == "json")
            ct = "application/json";
        const auto& c = actor.config();
        std::string out;
        {
            auto s = std::make_unique<CliSession>(
                &actor.system(), actor.command_tree(),
                OutputFormatter::create(c.default_format),
                [&](const std::string& t) { out = t; }, c.page_size);
            s->set_system_host(&actor);
            s->set_lifecycle_host(&actor);
            s->set_command_host(&actor);
            s->process_line(cl);
        }
        if (!out.empty() && out.back() == '\n')
            out.pop_back();
        CliResponse r;
        r.set_content_type(ct);
        r.set_payload(out);
        r.set_is_error(false);
        r.set_error_code(0);
        send_json_response(&conn, net::HttpStatusCode::OK, r);
    }
};

void register_legacy_handler() {
    HttpHandlerRegistry::instance().add(LegacyPostCliHandler::kMethod,
                                        std::string(LegacyPostCliHandler::kPath),
                                        std::make_unique<LegacyPostCliHandler>());
}

} // namespace hpactor::cli::handlers
