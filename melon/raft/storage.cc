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

#include <errno.h>
#include <melon/utility/string_printf.h>
#include <melon/utility/string_splitter.h>
#include <melon/utility/logging.h>
#include <melon/rpc/reloadable_flags.h>

#include "melon/raft/storage.h"
#include "melon/raft/log.h"
#include "melon/raft/raft_meta.h"
#include "melon/raft/snapshot.h"

namespace melon::raft {

    DEFINE_bool(raft_sync, true, "call fsync when need");
    MELON_VALIDATE_GFLAG(raft_sync, ::melon::PassValidate);
    DEFINE_int32(raft_sync_per_bytes, INT32_MAX,
                 "sync raft log per bytes when raft_sync set to true");
    MELON_VALIDATE_GFLAG(raft_sync_per_bytes, ::melon::NonNegativeInteger);
    DEFINE_bool(raft_create_parent_directories, true,
                "Create parent directories of the path in local storage if true");
    DEFINE_int32(raft_sync_policy, 0,
                 "raft sync policy when raft_sync set to true, 0 mean sync immediately, 1 mean sync by "
                 "writed bytes");
    DEFINE_bool(raft_sync_meta, false, "sync log meta, snapshot meta and raft meta");
    MELON_VALIDATE_GFLAG(raft_sync_meta, ::melon::PassValidate);

    LogStorage *LogStorage::create(const std::string &uri) {
        mutil::StringPiece copied_uri(uri);
        std::string parameter;
        mutil::StringPiece protocol = parse_uri(&copied_uri, &parameter);
        if (protocol.empty()) {
            LOG(ERROR) << "Invalid log storage uri=`" << uri << '\'';
            return NULL;
        }
        const LogStorage *type = log_storage_extension()->Find(
                protocol.as_string().c_str());
        if (type == NULL) {
            LOG(ERROR) << "Fail to find log storage type " << protocol
                       << ", uri=" << uri;
            return NULL;
        }
        return type->new_instance(parameter);
    }

    mutil::Status LogStorage::destroy(const std::string &uri) {
        mutil::Status status;
        mutil::StringPiece copied_uri(uri);
        std::string parameter;
        mutil::StringPiece protocol = parse_uri(&copied_uri, &parameter);
        if (protocol.empty()) {
            LOG(ERROR) << "Invalid log storage uri=`" << uri << '\'';
            status.set_error(EINVAL, "Invalid log storage uri = %s", uri.c_str());
            return status;
        }
        const LogStorage *type = log_storage_extension()->Find(
                protocol.as_string().c_str());
        if (type == NULL) {
            LOG(ERROR) << "Fail to find log storage type " << protocol
                       << ", uri=" << uri;
            status.set_error(EINVAL, "Fail to find log storage type %s uri %s",
                             protocol.as_string().c_str(), uri.c_str());
            return status;
        }
        return type->gc_instance(parameter);
    }

    SnapshotStorage *SnapshotStorage::create(const std::string &uri) {
        mutil::StringPiece copied_uri(uri);
        std::string parameter;
        mutil::StringPiece protocol = parse_uri(&copied_uri, &parameter);
        if (protocol.empty()) {
            LOG(ERROR) << "Invalid snapshot storage uri=`" << uri << '\'';
            return NULL;
        }
        const SnapshotStorage *type = snapshot_storage_extension()->Find(
                protocol.as_string().c_str());
        if (type == NULL) {
            LOG(ERROR) << "Fail to find snapshot storage type " << protocol
                       << ", uri=" << uri;
            return NULL;
        }
        return type->new_instance(parameter);
    }

    mutil::Status SnapshotStorage::destroy(const std::string &uri) {
        mutil::Status status;
        mutil::StringPiece copied_uri(uri);
        std::string parameter;
        mutil::StringPiece protocol = parse_uri(&copied_uri, &parameter);
        if (protocol.empty()) {
            LOG(ERROR) << "Invalid snapshot storage uri=`" << uri << '\'';
            status.set_error(EINVAL, "Invalid log storage uri = %s", uri.c_str());
            return status;
        }
        const SnapshotStorage *type = snapshot_storage_extension()->Find(
                protocol.as_string().c_str());
        if (type == NULL) {
            LOG(ERROR) << "Fail to find snapshot storage type " << protocol
                       << ", uri=" << uri;
            status.set_error(EINVAL, "Fail to find snapshot storage type %s uri %s",
                             protocol.as_string().c_str(), uri.c_str());
            return status;
        }
        return type->gc_instance(parameter);
    }

    RaftMetaStorage *RaftMetaStorage::create(const std::string &uri) {
        mutil::StringPiece copied_uri(uri);
        std::string parameter;
        mutil::StringPiece protocol = parse_uri(&copied_uri, &parameter);
        if (protocol.empty()) {
            LOG(ERROR) << "Invalid meta storage uri=`" << uri << '\'';
            return NULL;
        }
        const RaftMetaStorage *type = meta_storage_extension()->Find(
                protocol.as_string().c_str());
        if (type == NULL) {
            LOG(ERROR) << "Fail to find meta storage type " << protocol
                       << ", uri=" << uri;
            return NULL;
        }
        return type->new_instance(parameter);
    }

    mutil::Status RaftMetaStorage::destroy(const std::string &uri,
                                           const VersionedGroupId &vgid) {
        mutil::Status status;
        mutil::StringPiece copied_uri(uri);
        std::string parameter;
        mutil::StringPiece protocol = parse_uri(&copied_uri, &parameter);
        if (protocol.empty()) {
            LOG(ERROR) << "Invalid meta storage uri=`" << uri << '\'';
            status.set_error(EINVAL, "Invalid meta storage uri = %s", uri.c_str());
            return status;
        }
        const RaftMetaStorage *type = meta_storage_extension()->Find(
                protocol.as_string().c_str());
        if (type == NULL) {
            LOG(ERROR) << "Fail to find meta storage type " << protocol
                       << ", uri=" << uri;
            status.set_error(EINVAL, "Fail to find meta storage type %s uri %s",
                             protocol.as_string().c_str(), uri.c_str());
            return status;
        }
        return type->gc_instance(parameter, vgid);
    }

}  //  namespace melon::raft
