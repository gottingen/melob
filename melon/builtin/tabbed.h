// Copyright 2023 The Elastic-AI Authors.
// part of Elastic AI Search
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//



#ifndef MELON_BUILTIN_TABBED_H_
#define MELON_BUILTIN_TABBED_H_

#include <string>
#include <vector>

namespace melon {

    // Contain the information for showing a tab.
    struct TabInfo {
        std::string tab_name;
        std::string path;

        bool valid() const { return !tab_name.empty() && !path.empty(); }
    };

    // For appending TabInfo
    class TabInfoList {
    public:
        TabInfoList() {}

        TabInfo *add() {
            _list.push_back(TabInfo());
            return &_list[_list.size() - 1];
        }

        size_t size() const { return _list.size(); }

        const TabInfo &operator[](size_t i) const { return _list[i]; }

        void resize(size_t newsize) { _list.resize(newsize); }

    private:
        TabInfoList(const TabInfoList &);

        void operator=(const TabInfoList &);

        std::vector<TabInfo> _list;
    };

    // Inherit this class to show the service with one or more tabs.
    // NOTE: tabbed services are not shown in /status.
    // Example:
    //   #include <melon/builtin/common.h>
    //   ...
    //   void MySerivce::GetTabInfo(melon::TabInfoList* info_list) const {
    //     melon::TabInfo* info = info_list->add();
    //     info->tab_name = "my_tab";
    //     info->path = "/MyService/MyMethod";
    //   }
    //   void MyService::MyMethod(::google::protobuf::RpcController* controller,
    //                            const XXXRequest* request,
    //                            XXXResponse* response,
    //                            ::google::protobuf::Closure* done) {
    //     ...
    //     if (use_html) {
    //       os << "<!DOCTYPE html><html><head>\n"
    //          << "<script language=\"javascript\" type=\"text/javascript\" src=\"/js/jquery_min\"></script>\n"
    //          << melon::TabsHead() << "</head><body>";
    //       cntl->server()->PrintTabsBody(os, "my_tab");
    //     }
    //     ...
    //     if (use_html) {
    //       os << "</body></html>";
    //     }
    //   }
    // Note: don't forget the jquery.
    class Tabbed {
    public:
        virtual ~Tabbed() = default;

        virtual void GetTabInfo(TabInfoList *info_list) const = 0;
    };

}  // namespace melon


#endif // MELON_BUILTIN_TABBED_H_
