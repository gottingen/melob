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
#include <memory>
#include <turbo/log/logging.h>
#include <melon/rpc/server.h>
#include <melon/utility/files/file_watcher.h>
#include <melon/utility/files/scoped_file.h>
#include <melon/proto/rpc/trackme.pb.h>

TURBO_FLAG(std::string, bug_file, "./bugs", "A file containing revision and information of bugs");
TURBO_FLAG(int32_t, port, 8877, "TCP Port of this server");
TURBO_FLAG(int32_t, reporting_interval, 300, "Reporting interval of clients");

struct RevisionInfo {
    int64_t min_rev;
    int64_t max_rev;
    melon::TrackMeSeverity severity;
    std::string error_text;
};

// Load bugs from a file periodically.
class BugsLoader {
public:
    BugsLoader();

    bool start(const std::string &bugs_file);

    void stop();

    bool find(int64_t revision, melon::TrackMeResponse *response);

private:
    void load_bugs();

    void run();

    static void *run_this(void *arg);

    typedef std::vector<RevisionInfo> BugList;
    std::string _bugs_file;
    bool _started;
    bool _stop;
    pthread_t _tid;
    std::shared_ptr<BugList> _bug_list;
};

class TrackMeServiceImpl : public melon::TrackMeService {
public:
    explicit TrackMeServiceImpl(BugsLoader *bugs) : _bugs(bugs) {
    }

    ~TrackMeServiceImpl() {}

    void TrackMe(google::protobuf::RpcController *cntl_base,
                 const melon::TrackMeRequest *request,
                 melon::TrackMeResponse *response,
                 google::protobuf::Closure *done) {
        melon::ClosureGuard done_guard(done);
        melon::Controller *cntl = (melon::Controller *) cntl_base;
        // Set to OK by default.
        response->set_severity(melon::TrackMeOK);
        // Check if the version is affected by bugs if client set it.
        if (request->has_rpc_version()) {
            _bugs->find(request->rpc_version(), response);
        }
        response->set_new_interval(turbo::get_flag(FLAGS_reporting_interval));
        mutil::EndPoint server_addr;
        CHECK_EQ(0, mutil::str2endpoint(request->server_addr().c_str(), &server_addr));
        // NOTE(gejun): The ip reported is inaccessible in many cases, use 
        // remote_side instead right now.
        server_addr.ip = cntl->remote_side().ip;
        LOG(INFO) << "Pinged by " << server_addr << " (r"
                  << request->rpc_version() << ")";
    }

private:
    BugsLoader *_bugs;
};

int main(int argc, char *argv[]) {

    TURBO_SERVLET_PARSE(argc, argv);

    melon::Server server;
    server.set_version("trackme_server");
    BugsLoader bugs;
    if (!bugs.start(turbo::get_flag(FLAGS_bug_file))) {
        LOG(ERROR) << "Fail to start BugsLoader";
        return -1;
    }
    TrackMeServiceImpl echo_service_impl(&bugs);
    if (server.AddService(&echo_service_impl,
                          melon::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }
    melon::ServerOptions options;
    // I've noticed that many connections do not report. Don't know the
    // root cause yet. Set the idle_time to keep connections clean.
    options.idle_timeout_sec = turbo::get_flag(FLAGS_reporting_interval) * 2;
    if (server.Start(turbo::get_flag(FLAGS_port), &options) != 0) {
        LOG(ERROR) << "Fail to start TrackMeServer";
        return -1;
    }
    server.RunUntilAskedToQuit();
    bugs.stop();
    return 0;
}

BugsLoader::BugsLoader()
        : _started(false), _stop(false), _tid(0) {}

bool BugsLoader::start(const std::string &bugs_file) {
    _bugs_file = bugs_file;
    if (pthread_create(&_tid, NULL, run_this, this) != 0) {
        LOG(ERROR) << "Fail to create loading thread";
        return false;
    }
    _started = true;
    return true;
}

void BugsLoader::stop() {
    if (!_started) {
        return;
    }
    _stop = true;
    pthread_join(_tid, NULL);
}

void *BugsLoader::run_this(void *arg) {
    ((BugsLoader *) arg)->run();
    return NULL;
}

void BugsLoader::run() {
    // Check status of _bugs_files periodically.
    mutil::FileWatcher fw;
    if (fw.init(_bugs_file.c_str()) < 0) {
        LOG(ERROR) << "Fail to init FileWatcher on `" << _bugs_file << "'";
        return;
    }
    while (!_stop) {
        load_bugs();
        while (!_stop) {
            mutil::FileWatcher::Change change = fw.check_and_consume();
            if (change > 0) {
                break;
            }
            if (change < 0) {
                LOG(ERROR) << "`" << _bugs_file << "' was deleted";
            }
            sleep(1);
        }
    }

}

void BugsLoader::load_bugs() {
    mutil::ScopedFILE fp(fopen(_bugs_file.c_str(), "r"));
    if (!fp) {
        PLOG(WARNING) << "Fail to open `" << _bugs_file << '\'';
        return;
    }

    char *line = NULL;
    size_t line_len = 0;
    ssize_t nr = 0;
    int nline = 0;
    std::unique_ptr<BugList> m(new BugList);
    while ((nr = getline(&line, &line_len, fp.get())) != -1) {
        ++nline;
        if (line[nr - 1] == '\n') { // remove ending newline
            --nr;
        }
        // line format: 
        //   min_rev <sp> max_rev <sp> severity <sp> description
        mutil::StringMultiSplitter sp(line, line + nr, " \t");
        if (!sp) {
            continue;
        }
        long long min_rev;
        if (sp.to_longlong(&min_rev)) {
            LOG(WARNING) << "[line" << nline << "] Fail to parse column1 as min_rev";
            continue;
        }
        ++sp;
        long long max_rev;
        if (!sp || sp.to_longlong(&max_rev)) {
            LOG(WARNING) << "[line" << nline << "] Fail to parse column2 as max_rev";
            continue;
        }
        if (max_rev < min_rev) {
            LOG(WARNING) << "[line" << nline << "] max_rev=" << max_rev
                         << " is less than min_rev=" << min_rev;
            continue;
        }
        ++sp;
        if (!sp) {
            LOG(WARNING) << "[line" << nline << "] Fail to parse column3 as severity";
            continue;
        }
        melon::TrackMeSeverity severity = melon::TrackMeOK;
        std::string_view severity_str(sp.field(), sp.length());
        if (severity_str == "f" || severity_str == "F") {
            severity = melon::TrackMeFatal;
        } else if (severity_str == "w" || severity_str == "W") {
            \
            severity = melon::TrackMeWarning;
        } else {
            LOG(WARNING) << "[line" << nline << "] Invalid severity=" << severity_str;
            continue;
        }
        ++sp;
        if (!sp) {
            LOG(WARNING) << "[line" << nline << "] Fail to parse column4 as string";
            continue;
        }
        // Treat everything until end of the line as description. So don't add 
        // comments starting with # or //, they are not recognized.
        std::string_view description(sp.field(), line + nr - sp.field());
        RevisionInfo info;
        info.min_rev = min_rev;
        info.max_rev = max_rev;
        info.severity = severity;
        info.error_text.assign(description.data(), description.size());
        m->push_back(info);
    }
    LOG(INFO) << "Loaded " << m->size() << " bugs";
    free(line);
    // Just reseting the shared_ptr. Previous BugList will be destroyed when
    // no threads reference it.
    _bug_list.reset(m.release());
}

bool BugsLoader::find(int64_t revision, melon::TrackMeResponse *response) {
    // Add reference to make sure the bug list is not deleted.
    std::shared_ptr<BugList> local_list = _bug_list;
    if (local_list.get() == NULL) {
        return false;
    }
    // Reading the list in this function is always safe because a BugList
    // is never changed after creation.
    bool found = false;
    for (size_t i = 0; i < local_list->size(); ++i) {
        const RevisionInfo &info = (*local_list)[i];
        if (info.min_rev <= revision && revision <= info.max_rev) {
            found = true;
            if (info.severity > response->severity()) {
                response->set_severity(info.severity);
            }
            if (info.severity != melon::TrackMeOK) {
                std::string *error = response->mutable_error_text();
                char prefix[64];
                if (info.min_rev != info.max_rev) {
                    snprintf(prefix, sizeof(prefix), "[r%lld-r%lld] ",
                             (long long) info.min_rev, (long long) info.max_rev);
                } else {
                    snprintf(prefix, sizeof(prefix), "[r%lld] ",
                             (long long) info.min_rev);
                }
                error->append(prefix);
                error->append(info.error_text);
                error->append("; ");
            }
        }
    }
    return found;
}
