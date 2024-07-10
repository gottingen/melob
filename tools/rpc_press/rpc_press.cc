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
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/compiler/importer.h>
#include <melon/rpc/server.h>
#include <turbo/log/logging.h>
#include <melon/utility/string_splitter.h>
#include <string.h>
#include "rpc_press_impl.h"

TURBO_FLAG(int32_t ,dummy_port, 8888, "Port of dummy server"); 
TURBO_FLAG(std::string ,proto, "", " user's proto files with path");
TURBO_FLAG(std::string ,inc, "", "Include paths for proto, separated by semicolon(;)");
TURBO_FLAG(std::string ,method, "example.EchoService.Echo", "The full method name");
TURBO_FLAG(std::string ,server, "0.0.0.0:8002", "ip:port of the server when -load_balancer is empty, the naming service otherwise");
TURBO_FLAG(std::string ,input, "", "The file containing requests in json format");
TURBO_FLAG(std::string ,output, "", "The file containing responses in json format");
TURBO_FLAG(std::string ,lb_policy, "", "The load balancer algorithm: rr, random, la, c_murmurhash, c_md5");
TURBO_FLAG(int32_t ,thread_num, 0, "Number of threads to send requests. 0: automatically chosen according to -qps");
TURBO_FLAG(std::string ,protocol, "melon_std", "melon_std hulu_pbrpc http public_pbrpc...");
TURBO_FLAG(std::string ,connection_type, "", "Type of connections: single, pooled, short");
TURBO_FLAG(int32_t ,timeout_ms, 1000, "RPC timeout in milliseconds");
TURBO_FLAG(int32_t ,connection_timeout_ms, 500, " connection timeout in milliseconds");
TURBO_FLAG(int32_t ,max_retry, 3, "Maximum retry times by RPC framework");
TURBO_FLAG(int32_t ,request_compress_type, 0, "Snappy:1 Gzip:2 Zlib:3 LZ4:4 None:0");
TURBO_FLAG(int32_t ,response_compress_type, 0, "Snappy:1 Gzip:2 Zlib:3 LZ4:4 None:0");
TURBO_FLAG(int32_t ,attachment_size, 0, "Carry so many byte attachment along with requests"); 
TURBO_FLAG(int32_t ,duration, 0, "how many seconds the press keep");
TURBO_FLAG(int32_t ,qps, 100 , "how many calls  per seconds");
TURBO_FLAG(bool, pretty, true, "output pretty jsons");

bool set_press_options(pbrpcframework::PressOptions* options){
    size_t dot_pos = turbo::get_flag(FLAGS_method).find_last_of('.');
    if (dot_pos == std::string::npos) {
        LOG(ERROR) << "-method must be in form of: package.service.method";
        return false;
    }
    options->service = turbo::get_flag(FLAGS_method).substr(0, dot_pos);
    options->method = turbo::get_flag(FLAGS_method).substr(dot_pos + 1);
    options->lb_policy = turbo::get_flag(FLAGS_lb_policy);
    options->test_req_rate = turbo::get_flag(FLAGS_qps);
    if (turbo::get_flag(FLAGS_thread_num) > 0) {
        options->test_thread_num = turbo::get_flag(FLAGS_thread_num);
    } else {
        if (turbo::get_flag(FLAGS_qps) <= 0) { // unlimited qps
            options->test_thread_num = 50;
        } else {
            options->test_thread_num = turbo::get_flag(FLAGS_qps) / 10000;
            if (options->test_thread_num < 1) {
                options->test_thread_num = 1;
            }
            if (options->test_thread_num > 50) {
                options->test_thread_num = 50;
            }
        }
    }

    const int rate_limit_per_thread = 1000000;
    double req_rate_per_thread = options->test_req_rate / options->test_thread_num;
    if (req_rate_per_thread > rate_limit_per_thread) {
        LOG(ERROR) << "req_rate: " << (int64_t) req_rate_per_thread << " is too large in one thread. The rate limit is "
                <<  rate_limit_per_thread << " in one thread";
        return false;  
    }

    options->input = turbo::get_flag(FLAGS_input);
    options->output = turbo::get_flag(FLAGS_output);
    options->connection_type = turbo::get_flag(FLAGS_connection_type);
    options->connect_timeout_ms = turbo::get_flag(FLAGS_connection_timeout_ms);
    options->timeout_ms = turbo::get_flag(FLAGS_timeout_ms);
    options->max_retry = turbo::get_flag(FLAGS_max_retry);
    options->protocol = turbo::get_flag(FLAGS_protocol);
    options->request_compress_type = turbo::get_flag(FLAGS_request_compress_type);
    options->response_compress_type = turbo::get_flag(FLAGS_response_compress_type);
    options->attachment_size = turbo::get_flag(FLAGS_attachment_size);
    options->host = turbo::get_flag(FLAGS_server);
    options->proto_file = turbo::get_flag(FLAGS_proto);
    options->proto_includes = turbo::get_flag(FLAGS_inc);
    return true;
}

int main(int argc, char* argv[]) {

    TURBO_SERVLET_PARSE(argc, argv);

    if (turbo::get_flag(FLAGS_dummy_port) >= 0) {
        melon::StartDummyServerAt(turbo::get_flag(FLAGS_dummy_port));
    }

    pbrpcframework::PressOptions options;
    if (!set_press_options(&options)) {
        return -1;
    }
    pbrpcframework::RpcPress* rpc_press = new pbrpcframework::RpcPress;
    if (0 != rpc_press->init(&options)) {
        LOG(FATAL) << "Fail to init rpc_press";
        return -1;
    }

    rpc_press->start();
    if (turbo::get_flag(FLAGS_duration) <= 0) {
        while (!melon::IsAskedToQuit()) {
            sleep(1);
        }
    } else {
        sleep(turbo::get_flag(FLAGS_duration));
    }
    rpc_press->stop();
    // NOTE(gejun): Can't delete rpc_press on exit. It's probably
    // used by concurrently running done.
    return 0;
}
