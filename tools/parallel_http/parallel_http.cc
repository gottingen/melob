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
#include <deque>
#include <melon/fiber/fiber.h>
#include <turbo/log/logging.h>
#include <melon/utility/files/scoped_file.h>
#include <melon/rpc/channel.h>
#include <turbo/flags/parse.h>
#include <turbo/strings/ascii.h>
#include <turbo/strings/match.h>

TURBO_FLAG(std::string, url_file, "", "The file containing urls to fetch. If this flag is"
              " empty, read urls from stdin");
TURBO_FLAG(int32_t, timeout_ms, 1000, "RPC timeout in milliseconds");
TURBO_FLAG(int32_t, max_retry, 3, "Max retries(not including the first RPC)");
TURBO_FLAG(int32_t, thread_num, 8, "Number of threads to access urls");
TURBO_FLAG(int32_t, concurrency, 1000, "Max number of http calls in parallel");
TURBO_FLAG(bool, one_line_mode, false, "Output as `URL HTTP-RESPONSE' on true");
TURBO_FLAG(bool, only_show_host, false, "Print host name only");

struct AccessThreadArgs {
    const std::deque<std::string>* url_list;
    size_t offset;
    std::deque<std::pair<std::string, mutil::IOBuf> > output_queue;
    mutil::Mutex output_queue_mutex;
    mutil::atomic<int> current_concurrency;
};

class OnHttpCallEnd : public google::protobuf::Closure {
public:
    void Run();
public:
    melon::Controller cntl;
    AccessThreadArgs* args;
    std::string url;
};

void OnHttpCallEnd::Run() {
    std::unique_ptr<OnHttpCallEnd> delete_self(this);
    {
        MELON_SCOPED_LOCK(args->output_queue_mutex);
        if (cntl.Failed()) {
            args->output_queue.push_back(std::make_pair(url, mutil::IOBuf()));
        } else {
            args->output_queue.push_back(
                std::make_pair(url, cntl.response_attachment()));
        }
    }
    args->current_concurrency.fetch_sub(1, mutil::memory_order_relaxed);
}

void* access_thread(void* void_args) {
    AccessThreadArgs* args = (AccessThreadArgs*)void_args;
    melon::ChannelOptions options;
    options.protocol = melon::PROTOCOL_HTTP;
    options.connect_timeout_ms = turbo::get_flag(FLAGS_timeout_ms) / 2;
    options.timeout_ms = turbo::get_flag(FLAGS_timeout_ms)/*milliseconds*/;
    options.max_retry = turbo::get_flag(FLAGS_max_retry);
    const int concurrency_for_this_thread = turbo::get_flag(FLAGS_concurrency) / turbo::get_flag(FLAGS_thread_num);

    for (size_t i = args->offset; i < args->url_list->size(); i += turbo::get_flag(FLAGS_thread_num)) {
        std::string const& url = (*args->url_list)[i];
        melon::Channel channel;
        if (channel.Init(url.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to create channel to url=" << url;
            MELON_SCOPED_LOCK(args->output_queue_mutex);
            args->output_queue.push_back(std::make_pair(url, mutil::IOBuf()));
            continue;
        }
        while (args->current_concurrency.fetch_add(1, mutil::memory_order_relaxed)
               > concurrency_for_this_thread) {
            args->current_concurrency.fetch_sub(1, mutil::memory_order_relaxed);
            fiber_usleep(5000);
        }
        OnHttpCallEnd* done = new OnHttpCallEnd;
        done->cntl.http_request().uri() = url;
        done->args = args;
        done->url = url;
        channel.CallMethod(NULL, &done->cntl, NULL, NULL, done);
    }
    return NULL;
}

int main(int argc, char** argv) {
   TURBO_SERVLET_PARSE(argc, argv);
    
    // if (FLAGS_path.empty() || FLAGS_path[0] != '/') {
    //     FLAGS_path = "/" + FLAGS_path;
    // }

    mutil::ScopedFILE fp_guard;
    FILE* fp = NULL;
    if (!turbo::get_flag(FLAGS_url_file).empty()) {
        fp_guard.reset(fopen(turbo::get_flag(FLAGS_url_file).c_str(), "r"));
        if (!fp_guard) {
            PLOG(ERROR) << "Fail to open `" << turbo::get_flag(FLAGS_url_file) << '\'';
            return -1;
        }
        fp = fp_guard.get();
    } else {
        fp = stdin;
    }
    char* line_buf = NULL;
    size_t line_len = 0;
    ssize_t nr = 0;
    std::deque<std::string> url_list;
    while ((nr = getline(&line_buf, &line_len, fp)) != -1) {
        if (line_buf[nr - 1] == '\n') { // remove ending newline
            line_buf[nr - 1] = '\0';
            --nr;
        }
        std::string_view line(line_buf, nr);
        line = turbo::trim_all(line);
        if (!line.empty()) {
            url_list.push_back({line.data(), line.size()});
        }
    }
    if (url_list.empty()) {
        return 0;
    }
    AccessThreadArgs* args = new AccessThreadArgs[turbo::get_flag(FLAGS_thread_num)];
    for (int i = 0; i < turbo::get_flag(FLAGS_thread_num); ++i) {
        args[i].url_list = &url_list;
        args[i].offset = i;
        args[i].current_concurrency.store(0, mutil::memory_order_relaxed);
    }
    std::vector<fiber_t> tids;
    tids.resize(turbo::get_flag(FLAGS_thread_num));
    for (int i = 0; i < turbo::get_flag(FLAGS_thread_num); ++i) {
        CHECK_EQ(0, fiber_start_background(&tids[i], NULL, access_thread, &args[i]));
    }
    std::deque<std::pair<std::string, mutil::IOBuf> > output_queue;
    size_t nprinted = 0;
    while (nprinted != url_list.size()) {
        for (int i = 0; i < turbo::get_flag(FLAGS_thread_num); ++i) {
            {
                MELON_SCOPED_LOCK(args[i].output_queue_mutex);
                output_queue.swap(args[i].output_queue);
            }
            for (size_t i = 0; i < output_queue.size(); ++i) {
                std::string_view url = output_queue[i].first;
                std::string_view hostname;
                if (turbo::starts_with(url, "http://")) {
                    url.remove_prefix(7);
                }
                size_t slash_pos = url.find('/');
                if (slash_pos != std::string_view::npos) {
                    hostname = url.substr(0, slash_pos);
                } else {
                    hostname = url;
                }
                if (turbo::get_flag(FLAGS_one_line_mode)) {
                    if (turbo::get_flag(FLAGS_only_show_host)) {
                        std::cout << hostname;
                    } else {
                        std::cout << "http://" << url;
                    }
                    if (output_queue[i].second.empty()) {
                        std::cout << " ERROR" << std::endl;
                    } else {
                        std::cout << ' ' << output_queue[i].second << std::endl;
                    }
                } else {
                    // The prefix is unlikely be part of a ordinary http body,
                    // thus the line can be easily removed by shell utilities.
                    std::cout << "#### ";
                    if (turbo::get_flag(FLAGS_only_show_host)) {
                        std::cout << hostname;
                    } else {
                        std::cout << "http://" << url;
                    }
                    if (output_queue[i].second.empty()) {
                        std::cout << " ERROR" << std::endl;
                    } else {
                        std::cout << '\n' << output_queue[i].second << std::endl;
                    }
                }
            }
            nprinted += output_queue.size();
            output_queue.clear();
        }
        usleep(10000);
    }

    for (int i = 0; i < turbo::get_flag(FLAGS_thread_num); ++i) {
        fiber_join(tids[i], NULL);
    }
    for (int i = 0; i < turbo::get_flag(FLAGS_thread_num); ++i) {
        while (args[i].current_concurrency.load(mutil::memory_order_relaxed) != 0) {
            usleep(10000);
        }
    }
    return 0;
}
