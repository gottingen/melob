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



#include <turbo/bootstrap/servlet.h>
#include <turbo/log/logging.h>
#include <melon/rpc/server.h>
#include <melon/rpc/channel.h>
#include "view.pb.h"

TURBO_FLAG(int32_t ,port, 8888, "TCP Port of this server");
TURBO_FLAG(std::string ,target, "", "The server to view");
TURBO_FLAG(int32_t ,timeout_ms, 5000, "Timeout for calling the server to view");

// handle HTTP response of accessing builtin services of the target server.
static void handle_response(melon::Controller* client_cntl,
                            std::string target,
                            melon::Controller* server_cntl,
                            google::protobuf::Closure* server_done) {
    // Copy all headers. The "Content-Length" will be overwriteen.
    server_cntl->http_response() = client_cntl->http_response();
    // Copy content.
    server_cntl->response_attachment() = client_cntl->response_attachment();
    // Insert "rpc_view: <target>" before </body> so that users are always
    // visually notified with target server w/o confusions.
    mutil::IOBuf& content = server_cntl->response_attachment();
    mutil::IOBuf before_body;
    if (content.cut_until(&before_body, "</body>") == 0) {
        before_body.append(
            "<style type=\"text/css\">\n"
            ".rpcviewlogo {position: fixed; bottom: 0px; right: 0px;"
            " color: #ffffff; background-color: #000000; }\n"
            " </style>\n"
            "<span class='rpcviewlogo'>&nbsp;rpc_view: ");
        before_body.append(target);
        before_body.append("&nbsp;</span></body>");
        before_body.append(content);
        content = before_body;
    }
    // Notice that we don't set RPC to failed on http errors because we
    // want to pass unchanged content to the users otherwise RPC replaces
    // the content with ErrorText.
    if (client_cntl->Failed() &&
        client_cntl->ErrorCode() != melon::EHTTP) {
        server_cntl->SetFailed(client_cntl->ErrorCode(),
                               "%s", client_cntl->ErrorText().c_str());
    }
    delete client_cntl;
    server_done->Run();
}

// A http_master_service.
class ViewServiceImpl : public ViewService {
public:
    ViewServiceImpl() {}
    virtual ~ViewServiceImpl() {}
    virtual void default_method(google::protobuf::RpcController* cntl_base,
                                const HttpRequest*,
                                HttpResponse*,
                                google::protobuf::Closure* done) {
        melon::ClosureGuard done_guard(done);
        melon::Controller* server_cntl =
            static_cast<melon::Controller*>(cntl_base);

        // Get or set target. Notice that we don't access FLAGS_target directly
        // which is thread-unsafe (for string flags).
        std::string target;
        const std::string* newtarget =
            server_cntl->http_request().uri().GetQuery("changetarget");
        if (newtarget) {
            turbo::set_flag(&FLAGS_target,newtarget->c_str());
            target = *newtarget;
        } else {
            target = turbo::get_flag(FLAGS_target);
        }

        // Create the http channel on-the-fly. Notice that we've set 
        // `defer_close_second' in main() so that dtor of channels do not
        // close connections immediately and ad-hoc creation of channels 
        // often reuses the not-yet-closed connections.
        melon::Channel http_chan;
        melon::ChannelOptions http_chan_opt;
        http_chan_opt.protocol = melon::PROTOCOL_HTTP;
        if (http_chan.Init(target.c_str(), &http_chan_opt) != 0) {
            server_cntl->SetFailed(melon::EINTERNAL,
                                   "Fail to connect to %s", target.c_str());
            return;
        }

        // Remove "Accept-Encoding". We need to insert additional texts
        // before </body>, preventing the server from compressing the content
        // simplifies our code. The additional bandwidth consumption shouldn't
        // be an issue for infrequent checking out of builtin services pages.
        server_cntl->http_request().RemoveHeader("Accept-Encoding");

        melon::Controller* client_cntl = new melon::Controller;
        client_cntl->http_request() = server_cntl->http_request();
        // Remove "Host" so that RPC will laterly serialize the (correct)
        // target server in.
        client_cntl->http_request().RemoveHeader("host");

        // Setup the URI.
        const melon::URI& server_uri = server_cntl->http_request().uri();
        std::string uri = server_uri.path();
        if (!server_uri.query().empty()) {
            uri.push_back('?');
            uri.append(server_uri.query());
        }
        if (!server_uri.fragment().empty()) {
            uri.push_back('#');
            uri.append(server_uri.fragment());
        }
        client_cntl->http_request().uri() = uri;

        // /hotspots pages may take a long time to finish, since they all have
        // query "seconds", we set the timeout to be longer than "seconds".
        const std::string* seconds =
            server_cntl->http_request().uri().GetQuery("seconds");
        int64_t timeout_ms = turbo::get_flag(FLAGS_timeout_ms);
        if (seconds) {
            timeout_ms += atoll(seconds->c_str()) * 1000;
        }
        client_cntl->set_timeout_ms(timeout_ms);

        // Keep content as it is.
        client_cntl->request_attachment() = server_cntl->request_attachment();
        
        http_chan.CallMethod(NULL, client_cntl, NULL, NULL,
                             melon::NewCallback(
                                 handle_response, client_cntl, target,
                                 server_cntl, done_guard.release()));
    }
};

int main(int argc, char* argv[]) {

    auto app = turbo::Servlet::instance().run_app();
    app->add_option("target", FLAGS_target, FLAGS_target.help())->required(true);
    TURBO_SERVLET_PARSE(argc, argv);

    melon::Server server;
    server.set_version("rpc_view_server");
    melon::ServerOptions server_opt;
    server_opt.http_master_service = new ViewServiceImpl;
    if (server.Start(turbo::get_flag(FLAGS_port), &server_opt) != 0) {
        LOG(ERROR) << "Fail to start ViewServer";
        return -1;
    }
    server.RunUntilAskedToQuit();
    return 0;
}
