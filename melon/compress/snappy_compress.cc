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


#include <turbo/log/logging.h>
#include <melon/compress/snappy_compress.h>
#include <melon/rpc/protocol.h>
#include <snappy.h>


namespace melon::compress {

    bool SnappyCompress(const google::protobuf::Message &res, mutil::IOBuf *buf) {
        mutil::IOBuf serialized_pb;
        mutil::IOBufAsZeroCopyOutputStream wrapper(&serialized_pb);
        if (res.SerializeToZeroCopyStream(&wrapper)) {
            mutil::IOBufAsSnappySource source(serialized_pb);
            mutil::IOBufAsSnappySink sink(*buf);
            return snappy::Compress(&source, &sink);
        }
        LOG(WARNING) << "Fail to serialize input pb=" << &res;
        return false;
    }

    bool SnappyDecompress(const mutil::IOBuf &data, google::protobuf::Message *req) {
        mutil::IOBufAsSnappySource source(data);
        mutil::IOBuf binary_pb;
        mutil::IOBufAsSnappySink sink(binary_pb);
        if (snappy::Uncompress(&source, &sink)) {
            return ParsePbFromIOBuf(req, binary_pb);
        }
        LOG(WARNING) << "Fail to snappy::Uncompress, size=" << data.size();
        return false;
    }

    bool SnappyCompress(const mutil::IOBuf &in, mutil::IOBuf *out) {
        mutil::IOBufAsSnappySource source(in);
        mutil::IOBufAsSnappySink sink(*out);
        return snappy::Compress(&source, &sink);
    }

    bool SnappyDecompress(const mutil::IOBuf &in, mutil::IOBuf *out) {
        mutil::IOBufAsSnappySource source(in);
        mutil::IOBufAsSnappySink sink(*out);
        return snappy::Uncompress(&source, &sink);
    }

} // namespace melon::compress
