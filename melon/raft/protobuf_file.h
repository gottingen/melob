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


#ifndef MELON_RAFT_PROTOBUF_FILE_H_
#define MELON_RAFT_PROTOBUF_FILE_H_

#include <string>
#include <google/protobuf/message.h>
#include "melon/raft/file_system_adaptor.h"

namespace melon::raft {

    // protobuf file format:
    // len [4B, in network order]
    // protobuf data
    class ProtoBufFile {
    public:
        ProtoBufFile(const char *path, FileSystemAdaptor *fs = NULL);

        ProtoBufFile(const std::string &path, FileSystemAdaptor *fs = NULL);

        ~ProtoBufFile() {}

        int save(const ::google::protobuf::Message *message, bool sync);

        int load(::google::protobuf::Message *message);

    private:
        std::string _path;
        scoped_refptr<FileSystemAdaptor> _fs;
    };

}

#endif // MELON_RAFT_PROTOBUF_FILE_H_
