//
// Copyright (C) 2024 EA group inc.
// Author: Jeff.li lijippy@163.com
// All rights reserved.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
//
// Created by jeff on 24-6-14.
//

#include <melon/rpc/restful_service.h>
#include <melon/rpc/webui.h>
#include <melon/rpc/server.h>
#include <melon/br/registry.h>
#include <turbo/bootstrap/servlet.h>

TURBO_FLAG(int32_t, port, 8068, "TCP Port of this server");
TURBO_FLAG(int32_t, idle_timeout_s, -1, "Connection will be closed if there is no "
                                        "read/write operations during the last `idle_timeout_s'");

TURBO_FLAG(std::string, certificate, "cert.pem", "Certificate file path to enable SSL");
TURBO_FLAG(std::string, private_key, "key.pem", "Private key file path to enable SSL");
TURBO_FLAG(std::string, ciphers, "", "Cipher suite used for SSL connections");
TURBO_FLAG(std::string, mapping, "/ea", "Mapping path for restful service");
TURBO_FLAG(std::string, root_path, "www", "Root path for webui service");
TURBO_FLAG(std::string, webui_mapping, "/ea/ui", "Mapping path for webui service");

namespace webserver {

    class NotFoundProcessor : public melon::RestfulProcessor {
    private:
        void process(const melon::RestfulRequest *request, melon::RestfulResponse *response) override {
            auto path = request->unresolved_path();
            response->set_status_code(404);
            response->set_header("Access-Control-Allow-Origin", "*");
            response->set_header("Access-Control-Allow-Method", "*");
            response->set_header("Access-Control-Allow-Headers", "*");
            response->set_header("Access-Control-Allow-Credentials", "true");
            response->set_header("Access-Control-Expose-Headers", "*");
            response->set_header("Content-Type", "text/plain");
            response->set_body("not found\n");
            response->append_body("Request path: ");
            response->append_body(path);
            response->append_body("\n");
        }
    };

    class RootProcessor : public melon::RestfulProcessor {
    private:
        void process(const melon::RestfulRequest *request, melon::RestfulResponse *response) override {
            auto path = request->unresolved_path();
            response->set_status_code(200);
            response->set_header("Access-Control-Allow-Origin", "*");
            response->set_header("Access-Control-Allow-Method", "*");
            response->set_header("Access-Control-Allow-Headers", "*");
            response->set_header("Access-Control-Allow-Credentials", "true");
            response->set_header("Access-Control-Expose-Headers", "*");
            response->set_header("Content-Type", "text/plain");
            response->set_body("I am  root\n");
            response->append_body("\n");
        }
    };

    class PathProcessor : public melon::RestfulProcessor {
    private:
        void process(const melon::RestfulRequest *request, melon::RestfulResponse *response) override {
            auto path = request->unresolved_path();
            response->set_status_code(200);
            response->set_header("Access-Control-Allow-Origin", "*");
            response->set_header("Access-Control-Allow-Method", "*");
            response->set_header("Access-Control-Allow-Headers", "*");
            response->set_header("Access-Control-Allow-Credentials", "true");
            response->set_header("Access-Control-Expose-Headers", "*");
            response->set_header("Content-Type", "text/plain");
            response->set_body("hala restful\n");
            response->append_body("Request path: ");
            response->append_body(path);
            response->append_body("\n");
        }
    };

}  // namespace myservice

int main(int argc, char *argv[]) {
    auto *rapp = turbo::Servlet::instance().run_app();
    rapp->add_option("-p, --port", FLAGS_port, FLAGS_port.help());
    rapp->add_option("-i, --idle", FLAGS_idle_timeout_s, FLAGS_idle_timeout_s.help());
    rapp->add_option("-c, --certificate", FLAGS_certificate, FLAGS_certificate.help());
    rapp->add_option("-k, --private_key", FLAGS_private_key, FLAGS_private_key.help());
    rapp->add_option("-s, --ciphers", FLAGS_ciphers, FLAGS_ciphers.help());
    rapp->add_option("-m, --mapping", FLAGS_mapping, FLAGS_mapping.help());
    rapp->add_option("-r, --root_path", FLAGS_root_path, FLAGS_root_path.help());
    rapp->add_option("-w, --webui_mapping", FLAGS_webui_mapping, FLAGS_webui_mapping.help());
    auto [exit, code] = turbo::Servlet::instance().run(argc, argv);
    if (exit) {
        return code;
    }
    LOG(INFO)<<"config done..";
    // Generally you only need one Server.
    melon::Server server;
    auto service = melon::RestfulService::instance();
    service->set_mapping_path(turbo::get_flag(FLAGS_mapping));
    service->set_not_found_processor(std::make_shared<webserver::NotFoundProcessor>())
            ->set_root_processor(std::make_shared<webserver::RootProcessor>())
            ->set_processor("/path", std::make_shared<webserver::PathProcessor>())
            ->set_processor("path1//", std::make_shared<webserver::PathProcessor>())
            ->set_processor("///path3//path0/", std::make_shared<webserver::PathProcessor>());
    auto rs = service->register_server(&server);
    if (!rs.ok()) {
        LOG(ERROR) << "register server failed: " << rs;
        return -1;
    }
    LOG(INFO)<< "restful service has been registered";
    melon::WebuiConfig conf = melon::WebuiConfig::default_config();
    conf.mapping_path = turbo::get_flag(FLAGS_webui_mapping);
    conf.root_path = turbo::get_flag(FLAGS_root_path);
    auto *webui = melon::WebuiService::instance();
    rs = webui->register_server(conf, &server);
    if (!rs.ok()) {
        LOG(ERROR) << "register server failed: " << rs;
        return -1;
    }
    LOG(INFO)<< "webui has been registered";
    melon::ServerOptions options;
    options.idle_timeout_sec = turbo::get_flag(FLAGS_idle_timeout_s);
    options.mutable_ssl_options()->default_cert.certificate = turbo::get_flag(FLAGS_certificate);
    options.mutable_ssl_options()->default_cert.private_key = turbo::get_flag(FLAGS_private_key);
    options.mutable_ssl_options()->ciphers = turbo::get_flag(FLAGS_ciphers);
    LOG(INFO)<< "ready to start server";
    if (server.Start(turbo::get_flag(FLAGS_port), &options) != 0) {
        LOG(ERROR) << "Fail to start HttpServer";
        return -1;
    }
    LOG(INFO)<< "Server started at port: " << turbo::get_flag(FLAGS_port);
    server.RunUntilAskedToQuit();
    return 0;
}