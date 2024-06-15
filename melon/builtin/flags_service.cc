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



#include <ostream>
#include <vector>                           // std::vector
#include <set>
#include <gflags/gflags.h>                  // GetAllFlags
// CommandLineFlagInfo
#include <melon/utility/string_splitter.h>

#include <melon/rpc/closure_guard.h>        // ClosureGuard
#include <melon/rpc/controller.h>           // Controller
#include <melon/proto/rpc/errno.pb.h>
#include <melon/rpc/server.h>
#include <melon/builtin/common.h>
#include <melon/builtin/flags_service.h>


namespace melon {

    DEFINE_bool(immutable_flags, false, "gflags on /flags page can't be modified");

    // Replace some characters with html replacements. If the input string does
    // not need to be changed, return its const reference directly, otherwise put
    // the replaced string in backing string and return its const reference.
    // TODO(gejun): Make this function more general.
    static std::string HtmlReplace(const std::string &s) {
        std::string b;
        size_t last_pos = 0;
        while (1) {
            size_t new_pos = s.find_first_of("<>&", last_pos);
            if (new_pos == std::string::npos) {
                if (b.empty()) {  // no special characters.
                    return s;
                }
                b.append(s.data() + last_pos, s.size() - last_pos);
                return b;
            }
            b.append(s.data() + last_pos, new_pos - last_pos);
            switch (s[new_pos]) {
                case '<' :
                    b.append("&lt;");
                    break;
                case '>' :
                    b.append("&gt;");
                    break;
                case '&' :
                    b.append("&amp;");
                    break;
                default:
                    b.push_back(s[new_pos]);
                    break;
            }
            last_pos = new_pos + 1;
        }
    }

    static void PrintFlag(std::ostream &os, const google::CommandLineFlagInfo &flag,
                          bool use_html) {
        if (use_html) {
            os << "<tr><td>";
        }
        os << flag.name;
        if (flag.has_validator_fn) {
            if (use_html) {
                os << " (<a href='/flags/" << flag.name
                   << "?setvalue&withform'>R</a>)";
            } else {
                os << " (R)";
            }
        }
        os << (use_html ? "</td><td>" : " | ");
        if (!flag.is_default && use_html) {
            os << "<span style='color:#FF0000'>";
        }
        if (!flag.current_value.empty()) {
            os << (use_html ? HtmlReplace(flag.current_value)
                            : flag.current_value);
        } else {
            os << (use_html ? "&nbsp;" : " ");
        }
        if (!flag.is_default) {
            if (flag.default_value != flag.current_value) {
                os << " (default:" << (use_html ?
                                       HtmlReplace(flag.default_value) :
                                       flag.default_value) << ')';
            }
            if (use_html) {
                os << "</span>";
            }
        }
        os << (use_html ? "</td><td>" : " | ") << flag.description
           << (use_html ? "</td><td>" : " | ") << flag.filename;
        if (use_html) {
            os << "</td></tr>";
        }
    }

    void FlagsService::set_value_page(Controller *cntl,
                                      ::google::protobuf::Closure *done) {
        ClosureGuard done_guard(done);
        const std::string &name = cntl->http_request().unresolved_path();
        google::CommandLineFlagInfo info;
        if (!google::GetCommandLineFlagInfo(name.c_str(), &info)) {
            cntl->SetFailed(ENOMETHOD, "No such gflag");
            return;
        }
        mutil::IOBufBuilder os;
        const bool is_string = (info.type == "string");
        os << "<!DOCTYPE html><html><body>"
              "<form action='' method='get'>"
              " Set `" << name << "' from ";
        if (is_string) {
            os << '"';
        }
        os << info.current_value;
        if (is_string) {
            os << '"';
        }
        os << " to <input name='setvalue' value=''>"
              "  <button>go</button>"
              "</form>"
              "</body></html>";
        os.move_to(cntl->response_attachment());
    }

    void FlagsService::default_method(::google::protobuf::RpcController *cntl_base,
                                      const ::melon::FlagsRequest *,
                                      ::melon::FlagsResponse *,
                                      ::google::protobuf::Closure *done) {
        ClosureGuard done_guard(done);
        Controller *cntl = static_cast<Controller *>(cntl_base);
        const std::string *value_str =
                cntl->http_request().uri().GetQuery(SETVALUE_STR);
        const std::string &constraint = cntl->http_request().unresolved_path();

        const bool use_html = UseHTML(cntl->http_request());
        cntl->http_response().set_content_type(
                use_html ? "text/html" : "text/plain");

        if (value_str != NULL) {
            // reload value if ?setvalue=VALUE is present.
            if (constraint.empty()) {
                cntl->SetFailed(ENOMETHOD, "Require gflag name");
                return;
            }
            if (use_html && cntl->http_request().uri().GetQuery("withform")) {
                return set_value_page(cntl, done_guard.release());
            }
            google::CommandLineFlagInfo info;
            if (!google::GetCommandLineFlagInfo(constraint.c_str(), &info)) {
                cntl->SetFailed(ENOMETHOD, "No such gflag");
                return;
            }
            if (!info.has_validator_fn) {
                cntl->SetFailed(EPERM, "A reloadable gflag must have validator");
                return;
            }
            if (FLAGS_immutable_flags) {
                cntl->SetFailed(EPERM, "Cannot modify `%s' because -immutable_flags is on",
                                constraint.c_str());
                return;
            }
            if (google::SetCommandLineOption(constraint.c_str(),
                                                value_str->c_str()).empty()) {
                cntl->SetFailed(EPERM, "Fail to set `%s' to %s",
                                constraint.c_str(),
                                (value_str->empty() ? "empty string" : value_str->c_str()));
                return;
            }
            mutil::IOBufBuilder os;
            os << "Set `" << constraint << "' to " << *value_str;
            if (use_html) {
                os << "<br><a href='/flags'>[back to flags]</a>";
            }
            os.move_to(cntl->response_attachment());
            return;
        }

        // Parse query-string which is comma-separated flagnames/wildcards.
        std::vector<std::string> wildcards;
        std::set<std::string> exact;
        if (!constraint.empty()) {
            for (mutil::StringMultiSplitter sp(constraint.c_str(), ",;"); sp != NULL; ++sp) {
                std::string name(sp.field(), sp.length());
                if (name.find_first_of("$*") != std::string::npos) {
                    wildcards.push_back(name);
                } else {
                    exact.insert(name);
                }
            }
        }

        // Print header of the table
        mutil::IOBufBuilder os;
        if (use_html) {
            os << "<!DOCTYPE html><html><head>\n" << gridtable_style()
               << "<script language=\"javascript\" type=\"text/javascript\" src=\"/js/jquery_min\"></script>\n"
               << TabsHead()
               << "</head><body>";
            cntl->server()->PrintTabsBody(os, "flags");
            os << "<table class=\"gridtable\" border=\"1\"><tr><th>Name</th><th>Value</th>"
                  "<th>Description</th><th>Defined At</th></tr>\n";
        } else {
            os << "Name | Value | Description | Defined At\n"
                  "---------------------------------------\n";
        }

        if (!constraint.empty() && wildcards.empty()) {
            // Only exact names. We don't have to iterate all flags in this case.
            for (std::set<std::string>::iterator it = exact.begin();
                 it != exact.end(); ++it) {
                google::CommandLineFlagInfo info;
                if (google::GetCommandLineFlagInfo(it->c_str(), &info)) {
                    PrintFlag(os, info, use_html);
                    os << '\n';
                }
            }

        } else {
            // Iterate all flags and filter.
            std::vector<google::CommandLineFlagInfo> flag_list;
            flag_list.reserve(128);
            google::GetAllFlags(&flag_list);
            for (std::vector<google::CommandLineFlagInfo>::iterator
                         it = flag_list.begin(); it != flag_list.end(); ++it) {
                if (!constraint.empty() &&
                    exact.find(it->name) == exact.end() &&
                    !MatchAnyWildcard(it->name, wildcards)) {
                    continue;
                }
                PrintFlag(os, *it, use_html);
                os << '\n';
            }
        }
        if (use_html) {
            os << "</table></body></html>\n";
        }
        os.move_to(cntl->response_attachment());
        cntl->set_response_compress_type(COMPRESS_TYPE_GZIP);
    }

    void FlagsService::GetTabInfo(TabInfoList *info_list) const {
        TabInfo *info = info_list->add();
        info->path = "/flags";
        info->tab_name = "flags";
    }

} // namespace melon
