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
// Created by jeff on 24-6-27.
//

#include <melon/br/launch.h>
#include <collie/nlohmann/json.hpp>
#include <turbo/bootstrap/servlet.h>
#include <turbo/log/logging.h>
#include <turbo/flags/reflection.h>
#include <turbo/strings/str_split.h>
#include <turbo/strings/match.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <mutex>

extern "C" {
extern char**environ;
} // extern "C"
namespace melon {

    using EnvList = std::vector<std::pair<std::string, std::string>>;
    static EnvList all_envs;
    static std::string cmd_str;
    static std::string args_list;
    static std::string work_dir;
    std::vector<std::string> launch_default_conf;

    std::once_flag env_flag;
    static void init_envs() {
        char **env = environ;
        while (*env) {
            std::vector<std::string_view> parts = turbo::str_split(*env, '=', turbo::SkipWhitespace());
            if(parts.size() != 2) {
                env++;
                continue;
            }
            all_envs.push_back({std::string(parts[0]), std::string(parts[1])});
            env++;
        }
        auto * args = turbo::Servlet::instance().launch_params();
        if(args == nullptr) {
            LOG_FIRST_N(WARNING, 1)<< "launch_params is nullptr";
            return;
        }
        if(args->size() > 0) {
            cmd_str = (*args)[0];
        }
        for(size_t i = 1; i < args->size(); i++) {
            args_list += (*args)[i];
            args_list += " ";
        }
        launch_default_conf = turbo::Servlet::instance().default_flags_files();
        char buf[1024];
        if(getcwd(buf, sizeof(buf)) != nullptr) {
            work_dir = buf;
        }
    }

    static const EnvList &get_all_env() {
        std::call_once(env_flag, init_envs);
        return all_envs;
    }
    static const std::string &get_cmd() {
        std::call_once(env_flag, init_envs);
        return cmd_str;
    }

    static const std::string &get_args() {
        std::call_once(env_flag, init_envs);
        return args_list;
    }

    static const std::string &get_work_dir() {
        std::call_once(env_flag, init_envs);
        return work_dir;
    }



    void ListLaunchProcessor::process(const melon::RestfulRequest *request, melon::RestfulResponse *response) {
        std::string match_str;
        auto *ptr = request->uri().GetQuery("match");
        if (ptr) {
            match_str = *ptr;
        }
        response->set_content_json();
        response->set_access_control_all_allow();
        response->set_status_code(200);
        nlohmann::json j;
        j["code"] = 0;
        j["message"] = "success";
        const auto & envs = get_all_env();
        for (const auto &env : envs) {
            if(!match_str.empty() && !turbo::str_contains(env.first, match_str)) {
                continue;
            }
            nlohmann::json env_json;
            env_json["name"] = env.first;
            env_json["value"] = env.second;
            j["envs"].push_back(env_json);
        }
        j["cmd"] = get_cmd();
        j["args"] = get_args();
        j["work_dir"] = get_work_dir();
        if(!launch_default_conf.empty()) {
            j["default_conf"] = launch_default_conf;
        } else {
            j["default_conf"] = "";
        }
        response->set_body(j.dump());
    }

}  // namespace melon