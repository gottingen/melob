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
#include <turbo/strings/str_format.h>
#include <melon/rpc/server.h>
#include <turbo/log/logging.h>
#include <melon/utility/file_util.h>
#include <turbo/strings/str_split.h>
#include <melon/fiber/fiberpp.h>

namespace melon {

    static std::string normalize_path(const std::string &path) {
        std::string result;
        std::vector<std::string_view> segments = turbo::str_split(path, "/", turbo::SkipEmpty());
        bool first = true;
        for (const auto &seg : segments) {
            if(first) {
                first = false;
            } else {
                result.push_back('/');
            }
            result.append(seg);
        }
        return result;
    }

    void RestfulService::impl_method(::google::protobuf::RpcController *controller,
                                        const ::melon::NoUseRestfulRequest *request,
                                        ::melon::NoUseRestfulResponse *response,
                                        ::google::protobuf::Closure *done) {
        (void) request;
        (void) response;
        auto func = [this, controller, done] {
            melon::ClosureGuard done_guard(done);
            auto *ctrl = dynamic_cast<Controller *>(controller);
            const RestfulRequest req(ctrl);
            RestfulResponse resp(ctrl);
            auto &path = req.unresolved_path();
            if (path.empty()) {
                if (root_processor_) {
                    root_processor_->process(&req, &resp);
                    return;
                }
                if (any_path_processor_) {
                    any_path_processor_->process(&req, &resp);
                    return;
                }
                if (not_found_processor_) {
                    not_found_processor_->process(&req, &resp);
                    return;
                }
                LOG(FATAL) << "no processor found for path: /";
            }
            auto it = processors_.find(path);
            if (it != processors_.end()) {
                it->second->process(&req, &resp);
                return;
            }
            if (any_path_processor_) {
                any_path_processor_->process(&req, &resp);
                return;
            }
            if (not_found_processor_) {
                not_found_processor_->process(&req, &resp);
                return;
            }
            LOG(FATAL) << "no processor found for path: " << path;
        };
        melon::Fiber f;
        f.run(std::move(func));
        f.join();
    }

    turbo::Status RestfulService::register_server(Server *server) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(registered_) {
            return turbo::internal_error("register_server can only be called once");
        }
        registered_ = true;
        if(mapping_path_.empty() || server == nullptr) {
            return turbo::invalid_argument_error("mapping_path or server is empty");
        }
        if(not_found_processor_ == nullptr && any_path_processor_ == nullptr) {
            return turbo::invalid_argument_error("not_found_processor and any_path_processor are both empty, you must set one of them");
        }

        if(any_path_processor_ == nullptr && processors_.empty()) {
            return turbo::invalid_argument_error("any_path_processor and processors are both empty, you must set one of them");
        }

        std::string map_str = turbo::str_format("%s/* => impl_method", mapping_path_.c_str());
        auto r = server->AddService(this,
                          melon::SERVER_DOESNT_OWN_SERVICE,
                          map_str);
        if(r != 0) {
            return turbo::internal_error("register restful service failed");
        }
        return turbo::OkStatus();
    }

    RestfulService* RestfulService::set_not_found_processor(std::shared_ptr<RestfulProcessor> processor) {
        std::lock_guard<std::mutex> lock(mutex_);
        LOG_IF(FATAL, registered_)<<"set_not_found_processor must be called before register_server";
        not_found_processor_ = std::move(processor);
        return this;
    }
    RestfulService* RestfulService::set_any_path_processor(std::shared_ptr<RestfulProcessor> processor) {
        std::lock_guard<std::mutex> lock(mutex_);
        LOG_IF(FATAL, registered_)<<"set_not_found_processor must be called before register_server";
        any_path_processor_ = std::move(processor);
        return this;
    }
    RestfulService* RestfulService::set_root_processor(std::shared_ptr<RestfulProcessor> processor) {
        std::lock_guard<std::mutex> lock(mutex_);
        LOG_IF(FATAL, registered_)<<"set_not_found_processor must be called before register_server";
        root_processor_ = std::move(processor);
        return this;
    }
    RestfulService* RestfulService::set_processor(const std::string &path, std::shared_ptr<RestfulProcessor> processor, bool overwrite) {
        std::lock_guard<std::mutex> lock(mutex_);
        LOG_IF(FATAL, registered_)<<"set_not_found_processor must be called before register_server";
        auto fpath = normalize_path(path);
        if(fpath.empty()) {
            LOG(FATAL)<<"path is empty";
        }
        if(!overwrite) {
            auto it = processors_.find(fpath);
            if(it != processors_.end()) {
                LOG(FATAL)<<"processor already exists for path: "<<path;
            }
        }
        processors_[fpath] = std::move(processor);
        return this;
    }

    RestfulService* RestfulService::set_mapping_path(const std::string &mapping_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        LOG_IF(FATAL, registered_)<<"set_not_found_processor must be called before register_server";
        mapping_path_ = mapping_path;
        return this;
    }

}  // namespace melon
