// Copyright 2018 The abel Authors.
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

#include <cstddef>
#include <string>

#include <testing/time_util.h>
#include <abel/chrono/time.h>
#include <benchmark/benchmark.h>

namespace {

namespace {
const char* const kFormats[] = {
    abel::RFC1123_full,     // 0
    abel::RFC1123_no_wday,  // 1
    abel::RFC3339_full,     // 2
    abel::RFC3339_sec,      // 3
    "%Y-%m-%dT%H:%M:%S",    // 4
    "%Y-%m-%d",             // 5
};
const int kNumFormats = sizeof(kFormats) / sizeof(kFormats[0]);
}  // namespace

void BM_Format_FormatTime(benchmark::State& state) {
  const std::string fmt = kFormats[state.range(0)];
  state.SetLabel(fmt);
  const abel::time_zone lax =
      abel::time_internal::load_time_zone("America/Los_Angeles");
  const abel::abel_time t =
      abel::from_chrono(abel::chrono_second(1977, 6, 28, 9, 8, 7), lax) +
      abel::nanoseconds(1);
  while (state.KeepRunning()) {
    benchmark::DoNotOptimize(abel::format_time(fmt, t, lax).length());
  }
}
BENCHMARK(BM_Format_FormatTime)->DenseRange(0, kNumFormats - 1);

void BM_Format_ParseTime(benchmark::State& state) {
  const std::string fmt = kFormats[state.range(0)];
  state.SetLabel(fmt);
  const abel::time_zone lax =
      abel::time_internal::load_time_zone("America/Los_Angeles");
  abel::abel_time t = abel::from_chrono(abel::chrono_second(1977, 6, 28, 9, 8, 7), lax) +
                 abel::nanoseconds(1);
  const std::string when = abel::format_time(fmt, t, lax);
  std::string err;
  while (state.KeepRunning()) {
    benchmark::DoNotOptimize(abel::parse_time(fmt, when, lax, &t, &err));
  }
}
BENCHMARK(BM_Format_ParseTime)->DenseRange(0, kNumFormats - 1);

}  // namespace
