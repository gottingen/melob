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


// Date: Mon Jul 20 12:39:39 CST 2015

// Redirect LOG() into comlog.

#ifndef MUTIL_COMLOG_SINK_H
#define MUTIL_COMLOG_SINK_H

#include <melon/utility/logging.h>

struct com_device_t;
template <typename T> struct DefaultSingletonTraits;

namespace comspace {
class Event;
}

namespace logging {

enum ComlogSplitType {
    COMLOG_SPLIT_TRUNCT = 0,
    COMLOG_SPLIT_SIZECUT = 1,
    COMLOG_SPLIT_DATECUT = 2,
};

// Options to setup ComlogSink.
struct ComlogSinkOptions {
    ComlogSinkOptions();

    // true - "AFILE", false - "FILE"
    // default: false.
    bool async;

    // Use F W N T instead of FATAL WARNING NOTICE TRACE for shorter prefixes
    // and better alignment.
    // default: true
    bool shorter_log_level;

    // The directory to put logs. Could be absolute or relative path.
    // default: "log"
    std::string log_dir;

    // Name of the process. Use argv[0] when it's empty.
    // default: ""
    std::string process_name;

    // Logs longer than this value are truncated.
    // default: 2048
    int max_log_length;

    // Print VMLOG(n) as WARNING instead of TRACE since many online servers
    // disable TRACE logs.
    // default: true;
    bool print_vlog_as_warning;

    // Split Comlog type:
    //  COMLOG_SPLIT_TRUNCT: rotate the log file every 2G written.
    //  COMLOG_SPLIT_SIZECUT: move existing logs into a separate file every xxx MB written.
    //  COMLOG_SPLIT_DATECUT: move existing logs into a separate file periodically.
    // default: COMLOG_SPLIT_TRUNCT
    ComlogSplitType split_type;

    // [ Effective when split_type is COMLOG_SPLIT_SIZECUT ]
    // Move existing logs into a separate file suffixed with datetime every so many MB written.
    // Default: 2048
    int cut_size_megabytes;
    // Remove oldest cutoff log files when they exceed so many megabytes(roughly)
    // Default: 0 (unlimited)
    int quota_size;

    // [ Effective when split_type is COMLOG_SPLIT_DATECUT ]
    // Move existing logs into a separate file suffixed with datetime every so many minutes.
    // Example: my_app.log is moved to my_app.log.20160905113104
    // Default: 60
    int cut_interval_minutes;
    // Remove cutoff log files older than so many minutes:
    //   quota_day * 24 * 60 + quota_hour * 60 + quota_min
    // Default: 0 (unlimited)
    int quota_day;
    int quota_hour;
    int quota_min;

    // Open wf appender device for WARNING/ERROR/FATAL
    // default: false
    bool enable_wf_device;
};

// The LogSink to flush logs into comlog. Notice that this is a singleton class.
// [ Setup from a Configure file ]
//   if (logging::ComlogSink::GetInstance()->SetupFromConfig("log/log.conf") != 0) {
//       MLOG(ERROR) << "Fail to setup comlog";
//       return -1;
//   }
//   logging::SetLogSink(ComlogSink::GetInstance());
//
// [ Setup from ComlogSinkOptions ]
//   if (logging::ComlogSink::GetInstance()->Setup(NULL/*default options*/) != 0) {
//       MLOG(ERROR) << "Fail to setup comlog";
//       return -1;
//   }
//   logging::SetLogSink(ComlogSink::GetInstance());

class ComlogSink : public LogSink {
public:
    // comlog can have only one instance due to its global open/close.
    static ComlogSink* GetInstance();

    // Setup comlog in different ways: from a Configure file or
    // ComlogSinkOptions. Notice that setup can be done multiple times.
    int SetupFromConfig(const std::string& conf_path);
    int Setup(const ComlogSinkOptions* options);

    // Close comlog and release resources. This method is automatically
    // called before Setup and destruction.
    void Unload();

    // @LogSink
    bool OnLogMessage(int severity, const char* file, int line,
                      const mutil::StringPiece& content);
private:
    ComlogSink();
    ~ComlogSink();
friend struct DefaultSingletonTraits<ComlogSink>;
    int SetupDevice(com_device_t* dev, const char* type, const char* file, bool is_wf);

    bool _init;
    ComlogSinkOptions _options;
    com_device_t* _dev;
};

class ComlogInitializer {
public:
    ComlogInitializer() {
        if (com_logstatus() != LOG_NOT_DEFINED) {
            com_openlog_r();
        }
    }
    ~ComlogInitializer() {
        if (com_logstatus() != LOG_NOT_DEFINED) {
            com_closelog_r();
        }
    }
    
private:
    DISALLOW_COPY_AND_ASSIGN(ComlogInitializer);
};

}  // namespace logging

#endif  // MUTIL_COMLOG_SINK_H
