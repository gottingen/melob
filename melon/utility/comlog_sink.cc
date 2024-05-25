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

#include <com_log.h>
#include "melon/utility/memory/singleton.h"
#include "melon/utility/comlog_sink.h"
#include <melon/utility/files/file_path.h>
#include <melon/utility/fd_guard.h>
#include <melon/utility/file_util.h>
#include <melon/utility/endpoint.h>

namespace logging {
DECLARE_bool(log_year);
DECLARE_bool(log_hostname);

struct ComlogLayoutOptions {
    ComlogLayoutOptions() : shorter_log_level(true) {}
    
    bool shorter_log_level;
};

class ComlogLayout : public comspace::Layout {
public:
    explicit ComlogLayout(const ComlogLayoutOptions* options);
    ~ComlogLayout();
    int format(comspace::Event *evt);
private:
    ComlogLayoutOptions _options;
};

ComlogLayout::ComlogLayout(const ComlogLayoutOptions* options) {
    if (options) {
        _options = *options;
    }
}

ComlogLayout::~ComlogLayout() {
}

// Override Layout::format to have shorter prefixes. Patterns are just ignored.
int ComlogLayout::format(comspace::Event *evt) {
    const int bufsize = evt->_render_msgbuf_size;
    char* const buf = evt->_render_msgbuf;
    if (bufsize < 2){
        return -1;
    }

    time_t t = evt->_print_time.tv_sec;
    struct tm local_tm = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL};
#if _MSC_VER >= 1400
    localtime_s(&local_tm, &t);
#else
    localtime_r(&t, &local_tm);
#endif
    int len = 0;
    if (_options.shorter_log_level) {
        buf[len++] = *comspace::getLogName(evt->_log_level);
    } else {
        const char* const name = comspace::getLogName(evt->_log_level);
        int cp_len = std::min(bufsize - len, (int)strlen(name));
        memcpy(buf + len, name, cp_len);
        len += cp_len;
        if (len < bufsize - 1) {
            buf[len++] = ' ';
        }
    }
    if (len < bufsize - 1) {
        int ret = 0;
        if (FLAGS_log_year) {
            ret = snprintf(buf + len, bufsize - len,
                           "%04d%02d%02d %02d:%02d:%02d.%06d %5u ",
                           local_tm.tm_year + 1900,
                           local_tm.tm_mon + 1,
                           local_tm.tm_mday,
                           local_tm.tm_hour,
                           local_tm.tm_min,
                           local_tm.tm_sec,
                           (int)evt->_print_time.tv_usec,
                           (unsigned int)evt->_thread_id);
        } else {
            ret = snprintf(buf + len, bufsize - len,
                           "%02d%02d %02d:%02d:%02d.%06d %5u ",
                           local_tm.tm_mon + 1,
                           local_tm.tm_mday,
                           local_tm.tm_hour,
                           local_tm.tm_min,
                           local_tm.tm_sec,
                           (int)evt->_print_time.tv_usec,
                           (unsigned int)evt->_thread_id);
        }
        if (ret >= 0) {
            len += ret;
        } else {
            // older glibc may return negative which means the buffer is full.
            len = bufsize;
        }
    }
    if (len > 0 && len < bufsize - 1) {  // not truncated.
        // Although it's very stupid, we have to copy the message again due
        // to the design of comlog.
        int cp_len = std::min(bufsize - len, evt->_msgbuf_len);
        memcpy(buf + len, evt->_msgbuf, cp_len);
        len += cp_len;
    }
    if (len >= bufsize - 1) {
        len = bufsize - 2;
    }
    buf[len++] = '\n';
    buf[len] = 0;
    evt->_render_msgbuf_len = len;
    return 0;
}

ComlogSink* ComlogSink::GetInstance() {
    return Singleton<ComlogSink, LeakySingletonTraits<ComlogSink> >::get();
}

ComlogSinkOptions::ComlogSinkOptions()
    : async(false)
    , shorter_log_level(true)
    , log_dir("log")
    , max_log_length(2048)
    , print_vlog_as_warning(true)
    , split_type(COMLOG_SPLIT_TRUNCT)
    , cut_size_megabytes(2048)
    , quota_size(0)
    , cut_interval_minutes(60)
    , quota_day(0)
    , quota_hour(0)
    , quota_min(0)
    , enable_wf_device(false) {
}

ComlogSink::ComlogSink() 
    : _init(false), _dev(NULL) {
}

int ComlogSink::SetupFromConfig(const std::string& conf_path_str) {
    Unload();
    mutil::FilePath path(conf_path_str);
    if (com_loadlog(path.DirName().value().c_str(),
                    path.BaseName().value().c_str()) != 0) {
        MLOG(ERROR) << "Fail to create ComlogSink from `" << conf_path_str << "'";
        return -1;
    }
    _init = true;
    return 0;
}

// This is definitely linux specific.
static std::string GetProcessName() {
    mutil::fd_guard fd(open("/proc/self/cmdline", O_RDONLY));
    if (fd < 0) {
        return "unknown";
    }
    char buf[512];
    const ssize_t len = read(fd, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return "unknown";
    }
    buf[len] = '\0';
    // Not string(buf, len) because we needs to buf to be truncated at first \0.
    // Under gdb, the first part of cmdline may include path.
    return mutil::FilePath(std::string(buf)).BaseName().value();
}

int ComlogSink::SetupDevice(com_device_t* dev, const char* type, const char* file, bool is_wf) {
    mutil::FilePath path(file);
    snprintf(dev->host, sizeof(dev->host), "%s", path.DirName().value().c_str());
    if (!is_wf) {
        snprintf(dev->name, sizeof(dev->name), "%s_0", type);
        COMLOG_SETSYSLOG(*dev);

        //snprintf(dev->file, COM_MAXFILENAME, "%s", file);
        snprintf(dev->file, sizeof(dev->file), "%s", path.BaseName().value().c_str());
    } else {
        snprintf(dev->name, sizeof(dev->name), "%s_1", type);
        dev->log_mask = 0;
        COMLOG_ADDMASK(*dev, COMLOG_WARNING);
        COMLOG_ADDMASK(*dev, COMLOG_FATAL);

        //snprintf(dev->file, COM_MAXFILENAME, "%s.wf", file);
        snprintf(dev->file, sizeof(dev->file), "%s.wf", path.BaseName().value().c_str());
    }
    
    snprintf(dev->type, COM_MAXAPPENDERNAME, "%s", type);
    dev->splite_type = static_cast<int>(_options.split_type);
    dev->log_size = _options.cut_size_megabytes; // SIZECUT precision in MB
    dev->compress = 0;
    dev->cuttime = _options.cut_interval_minutes; // DATECUT time precision in min

    // set quota conf
    int index = dev->reserved_num;
    if (dev->splite_type == COMLOG_SPLIT_SIZECUT) {
        if (_options.cut_size_megabytes <= 0) {
            MLOG(ERROR) << "Invalid ComlogSinkOptions.cut_size_megabytes="
                       << _options.cut_size_megabytes;
            return -1;
        }
        if (_options.quota_size < 0) {
            MLOG(ERROR) << "Invalid ComlogSinkOptions.quota_size="
                       << _options.quota_size;
            return -1;
        }
        snprintf(dev->reservedext[index].name, sizeof(dev->reservedext[index].name),
                 "%s_QUOTA_SIZE", dev->name);
        snprintf(dev->reservedext[index].value, sizeof(dev->reservedext[index].value),
                 "%d", _options.quota_size);
        index++;
    } else if (dev->splite_type == COMLOG_SPLIT_DATECUT) {
        if (_options.quota_day < 0) {
            MLOG(ERROR) << "Invalid ComlogSinkOptions.quota_day=" << _options.quota_day;
            return -1;
        }
        if (_options.quota_hour < 0) {
            MLOG(ERROR) << "Invalid ComlogSinkOptions.quota_hour=" << _options.quota_hour;
            return -1;
        }
        if (_options.quota_min < 0) {
            MLOG(ERROR) << "Invalid ComlogSinkOptions.quota_min=" << _options.quota_min;
            return -1;
        }
        if (_options.quota_day > 0) {
            snprintf(dev->reservedext[index].name, sizeof(dev->reservedext[index].name),
                     "%s_QUOTA_DAY", (char*)dev->name);
            snprintf(dev->reservedext[index].value, sizeof(dev->reservedext[index].value),
                     "%d", _options.quota_day);
            index++;
        }
        if (_options.quota_hour > 0) {
            snprintf(dev->reservedext[index].name, sizeof(dev->reservedext[index].name),
                     "%s_QUOTA_HOUR", (char*)dev->name);
            snprintf(dev->reservedext[index].value, sizeof(dev->reservedext[index].value),
                     "%d", _options.quota_hour);
            index++;
        }
        if (_options.quota_min > 0) {
            snprintf(dev->reservedext[index].name, sizeof(dev->reservedext[index].name),
                     "%s_QUOTA_MIN", (char*)dev->name);
            snprintf(dev->reservedext[index].value, sizeof(dev->reservedext[index].value),
                     "%d", _options.quota_min);
            index++;
        }
    }
    dev->reserved_num = index;
    dev->reservedconf.item = &dev->reservedext[0];
    dev->reservedconf.num = dev->reserved_num;
    dev->reservedconf.size = dev->reserved_num;

    ComlogLayoutOptions layout_options;
    layout_options.shorter_log_level = _options.shorter_log_level;
    ComlogLayout* layout = new (std::nothrow) ComlogLayout(&layout_options);
    if (layout == NULL) {
        MLOG(FATAL) << "Fail to new layout";
        return -1;
    }
    dev->layout = layout;

    return 0;
}

int ComlogSink::Setup(const ComlogSinkOptions* options) {
    Unload();
    if (options) {
        _options = *options;
    }
    if (_options.max_log_length > 0) {
        comspace::Event::setMaxLogLength(_options.max_log_length);
    }
    if (_options.process_name.empty()) {
        _options.process_name = GetProcessName();
    }

    char type[COM_MAXAPPENDERNAME];
    if (_options.async) {
        snprintf(type, COM_MAXAPPENDERNAME, "AFILE");
    } else {
        snprintf(type, COM_MAXAPPENDERNAME, "FILE");
    }
    mutil::FilePath cwd;
    if (!_options.log_dir.empty()) {
        mutil::FilePath log_dir(_options.log_dir);
        if (log_dir.IsAbsolute()) {
            cwd = log_dir;
        } else {
            if (!mutil::GetCurrentDirectory(&cwd)) {
                MLOG(ERROR) << "Fail to get cwd";
                return -1;
            }
            cwd = cwd.Append(log_dir);
        }
    } else {
        if (!mutil::GetCurrentDirectory(&cwd)) {
            MLOG(ERROR) << "Fail to get cwd";
            return -1;
        }
    }
    mutil::File::Error err;
    if (!mutil::CreateDirectoryAndGetError(cwd, &err)) {
        MLOG(ERROR) << "Fail to create directory, " << err;
        return -1;
    }
    char file[COM_MAXFILENAME];
    snprintf(file, COM_MAXFILENAME, "%s",
             cwd.Append(_options.process_name + ".log").value().c_str());

    int dev_num = (_options.enable_wf_device ? 2 : 1);
    _dev = new (std::nothrow) com_device_t[dev_num];
    if (NULL == _dev) {
        MLOG(FATAL) << "Fail to new com_device_t";
        return -1;
    }
    if (0 != SetupDevice(&_dev[0], type, file, false)) {
        MLOG(ERROR) << "Fail to setup first com_device_t";
        return -1;
    }
    if (dev_num == 2) {
        if (0 != SetupDevice(&_dev[1], type, file, true)) {
            MLOG(ERROR) << "Fail to setup second com_device_t";
            return -1;
        }
    }
    if (com_openlog(_options.process_name.c_str(), _dev, dev_num, NULL) != 0) {
        MLOG(ERROR) << "Fail to com_openlog";
        return -1;
    }
    _init = true;
    return 0;
}
        
void ComlogSink::Unload() {
    if (_init) {
        com_closelog(0);
        _init = false;
    }
    if (_dev) {
        // FIXME(gejun): Can't delete layout, somewhere in comlog may still
        // reference the layout after com_closelog.
        //delete _dev->layout;
        delete [] _dev;
        _dev = NULL;
    }
}

ComlogSink::~ComlogSink() {
    Unload();
}

int const comlog_levels[LOG_NUM_SEVERITIES] = {
    COMLOG_TRACE, COMLOG_NOTICE, COMLOG_WARNING, COMLOG_FATAL, COMLOG_FATAL };

bool ComlogSink::OnLogMessage(int severity, const char* file, int line,
                              const mutil::StringPiece& content) {
    // Print warning for VMLOG since many online servers do not enable COMLOG_TRACE.
    int comlog_level = 0;
    if (severity < 0) {
        comlog_level = _options.print_vlog_as_warning ? COMLOG_WARNING : COMLOG_TRACE;
    } else {
        comlog_level = comlog_levels[severity];
    }
    if (FLAGS_log_hostname) {
        mutil::StringPiece hostname(mutil::my_hostname());
        if (hostname.ends_with(".baidu.com")) { // make it shorter
            hostname.remove_suffix(10);
        }
        return com_writelog(comlog_level, "%.*s %s:%d] %.*s",
                            (int)hostname.size(), hostname.data(),
                            file, line,
                            (int)content.size(), content.data()) == 0;
    }
    // Using %.*s is faster than %s.
    return com_writelog(comlog_level, "%s:%d] %.*s", file, line,
                        (int)content.size(), content.data()) == 0;
}

}  // namespace logging
