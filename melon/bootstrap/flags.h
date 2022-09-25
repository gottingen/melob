
/****************************************************************
 * Copyright (c) 2022, liyinbin
 * All rights reserved.
 * Author by liyinbin (jeff.li) lijippy@163.com
 *****************************************************************/

#ifndef MELON_BOOTSTRAP_FLAGS_H_
#define MELON_BOOTSTRAP_FLAGS_H_

#include <string>
#include "melon/base/profile.h"

// GFlags are parsed by melon, and in certain cases, used prior to calling
// user's entry point. Unable to override flags before melon read them can be
// inconvenient in some cases. Here we provide this macro to accomplish this.
//
// Once melon finishes parsing GFlags, new value specified by this macro is
// applied. Only after all new values are applied will melon start to read them.
//
// Note that if the same flag is specified explicitly when starting the program,
// that value will take precedence.
#define MELON_RESET_FLAGS(Name, NewValue)                                \
  static ::melon::detail::flags_overrider_registration MELON_CONCAT( \
      melon_anonymous_flag_overrider_, __LINE__)(#Name, NewValue, false)

// Same as `MELON_RESET_FLAGS` except that this one overrides flag regardless
// of whether the user set it manually when running the program.
#define MELON_FORCE_OVERRIDE_FLAG(Name, NewValue)                          \
  static ::melon::detail::flags_overrider_registration MELON_INTERNAL_PP_CAT( \
      melon_anonymous_flag_overrider_, __LINE__)(#Name, NewValue, true)

// Implementation goes below.
namespace melon::detail {

    void register_flags_overrider(const std::string &name, const std::string &value,
                                  bool forcibly);

    /// run by `bootstrap_init` after gflags's ParseCommandLineFlags
    void apply_flags_overrider();

    // Register a pending flag override operation.
    class flags_overrider_registration {
    public:
        template<class T,
                std::enable_if_t<std::is_trivial_v<T> &&
                                 !std::is_same_v<T, const char *>> * = nullptr>
        flags_overrider_registration(const std::string &name, T value, bool forcibly) {
            register_flags_overrider(name, std::to_string(value), forcibly);
        }

        flags_overrider_registration(const std::string &name, const std::string &value,
                                     bool forcibly) {
            register_flags_overrider(name, value, forcibly);
        }
    };

}  // namespace melon::detail

#endif  // MELON_BOOTSTRAP_FLAGS_H_
