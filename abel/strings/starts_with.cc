// Copyright (c) 2021, gottingen group.
// All rights reserved.
// Created by liyinbin lijippy@163.com

#include "abel/strings/starts_with.h"
#include "abel/strings/compare.h"
#include <algorithm>

namespace abel {

bool starts_with_case(std::string_view text, std::string_view suffix) {
    if (text.size() >= suffix.size()) {
        return abel::compare_case(text.substr(0, suffix.size()), suffix) == 0;
    }
    return false;
}

}  // namespace abel
