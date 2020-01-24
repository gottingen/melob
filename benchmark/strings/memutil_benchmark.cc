//

#include <abel/strings/internal/char_traits.h>

#include <algorithm>
#include <cstdlib>

#include <benchmark/benchmark.h>
#include <abel/strings/ascii.h>

// We fill the haystack with aaaaaaaaaaaaaaaaaa...aaaab.
// That gives us:
// - an easy search: 'b'
// - a medium search: 'ab'.  That means every letter is a possible match.
// - a pathological search: 'aaaaaa.......aaaaab' (half as many a's as haytack)
// We benchmark case-sensitive and case-insensitive versions of
// three char_mem implementations:
// - char_mem() from memutil.h
// - search() from STL
// - char_match(), a custom implementation using memchr and memcmp.
// Here are sample results:
//
// Run on (12 X 3800 MHz CPU s)
// CPU Caches:
//   L1 Data 32K (x6)
//   L1 Instruction 32K (x6)
//   L2 Unified 256K (x6)
//   L3 Unified 15360K (x1)
// ----------------------------------------------------------------
// Benchmark                           abel_time          CPU Iterations
// ----------------------------------------------------------------
// BM_Memmem                        3583 ns      3582 ns     196469  2.59966GB/s
// BM_MemmemMedium                 13743 ns     13742 ns      50901  693.986MB/s
// BM_MemmemPathological        13695030 ns  13693977 ns         51  713.133kB/s
// BM_Memcasemem                    3299 ns      3299 ns     212942  2.82309GB/s
// BM_MemcasememMedium             16407 ns     16406 ns      42170  581.309MB/s
// BM_MemcasememPathological    17267745 ns  17266030 ns         41  565.598kB/s
// BM_Search                        1610 ns      1609 ns     431321  5.78672GB/s
// BM_SearchMedium                 11111 ns     11110 ns      63001  858.414MB/s
// BM_SearchPathological        12117390 ns  12116397 ns         58  805.984kB/s
// BM_Searchcase                    3081 ns      3081 ns     229949  3.02313GB/s
// BM_SearchcaseMedium             16003 ns     16001 ns      44170  595.998MB/s
// BM_SearchcasePathological    15823413 ns  15821909 ns         44  617.222kB/s
// BM_Memmatch                       197 ns       197 ns    3584225  47.2951GB/s
// BM_MemmatchMedium               52333 ns     52329 ns      13280  182.244MB/s
// BM_MemmatchPathological        659799 ns    659727 ns       1058  14.4556MB/s
// BM_Memcasematch                  5460 ns      5460 ns     127606  1.70586GB/s
// BM_MemcasematchMedium           32861 ns     32857 ns      21258  290.248MB/s
// BM_MemcasematchPathological  15154243 ns  15153089 ns         46  644.464kB/s
// BM_MemmemStartup                    5 ns         5 ns  150821500
// BM_SearchStartup                    5 ns         5 ns  150644203
// BM_MemmatchStartup                  7 ns         7 ns   97068802
//
// Conclusions:
//
// The following recommendations are based on the sample results above. However,
// we have found that the performance of STL search can vary significantly
// depending on compiler and standard library implementation. We recommend you
// run the benchmarks for yourself on relevant platforms.
//
// If you need case-insensitive, STL search is slightly better than char_mem for
// all cases.
//
// Case-sensitive is more subtle:
// Custom char_match is _very_ fast at scanning, so if you have very few possible
// matches in your haystack, that's the way to go. Performance drops
// significantly with more matches.
//
// STL search is slightly faster than char_mem in the medium and pathological
// benchmarks. However, the performance of char_mem is currently more dependable
// across platforms and build configurations.

namespace {

constexpr int kHaystackSize = 10000;
constexpr int64_t kHaystackSize64 = kHaystackSize;
const char* MakeHaystack() {
  char* haystack = new char[kHaystackSize];
  for (int i = 0; i < kHaystackSize - 1; ++i) haystack[i] = 'a';
  haystack[kHaystackSize - 1] = 'b';
  return haystack;
}
const char* const kHaystack = MakeHaystack();

void BM_Memmem(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        abel::strings_internal::char_mem(kHaystack, kHaystackSize, "b", 1));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_Memmem);

void BM_MemmemMedium(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        abel::strings_internal::char_mem(kHaystack, kHaystackSize, "ab", 2));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_MemmemMedium);

void BM_MemmemPathological(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(abel::strings_internal::char_mem(
        kHaystack, kHaystackSize, kHaystack + kHaystackSize / 2,
        kHaystackSize - kHaystackSize / 2));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_MemmemPathological);

void BM_Memcasemem(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        abel::strings_internal::char_case_mem(kHaystack, kHaystackSize, "b", 1));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_Memcasemem);

void BM_MemcasememMedium(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        abel::strings_internal::char_case_mem(kHaystack, kHaystackSize, "ab", 2));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_MemcasememMedium);

void BM_MemcasememPathological(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(abel::strings_internal::char_case_mem(
        kHaystack, kHaystackSize, kHaystack + kHaystackSize / 2,
        kHaystackSize - kHaystackSize / 2));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_MemcasememPathological);

bool case_eq(const char a, const char b) {
  return abel::ascii::to_lower(a) == abel::ascii::to_lower(b);
}

void BM_Search(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(std::search(kHaystack, kHaystack + kHaystackSize,
                                         kHaystack + kHaystackSize - 1,
                                         kHaystack + kHaystackSize));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_Search);

void BM_SearchMedium(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(std::search(kHaystack, kHaystack + kHaystackSize,
                                         kHaystack + kHaystackSize - 2,
                                         kHaystack + kHaystackSize));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_SearchMedium);

void BM_SearchPathological(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(std::search(kHaystack, kHaystack + kHaystackSize,
                                         kHaystack + kHaystackSize / 2,
                                         kHaystack + kHaystackSize));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_SearchPathological);

void BM_Searchcase(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(std::search(kHaystack, kHaystack + kHaystackSize,
                                         kHaystack + kHaystackSize - 1,
                                         kHaystack + kHaystackSize, case_eq));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_Searchcase);

void BM_SearchcaseMedium(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(std::search(kHaystack, kHaystack + kHaystackSize,
                                         kHaystack + kHaystackSize - 2,
                                         kHaystack + kHaystackSize, case_eq));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_SearchcaseMedium);

void BM_SearchcasePathological(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(std::search(kHaystack, kHaystack + kHaystackSize,
                                         kHaystack + kHaystackSize / 2,
                                         kHaystack + kHaystackSize, case_eq));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_SearchcasePathological);

char* memcasechr(const char* s, int c, size_t slen) {
  c = abel::ascii::to_lower(c);
  for (; slen; ++s, --slen) {
    if (abel::ascii::to_lower(*s) == c) return const_cast<char*>(s);
  }
  return nullptr;
}

const char* memcasematch(const char* phaystack, size_t haylen,
                         const char* pneedle, size_t neelen) {
  if (0 == neelen) {
    return phaystack;  // even if haylen is 0
  }
  if (haylen < neelen) return nullptr;

  const char* match;
  const char* hayend = phaystack + haylen - neelen + 1;
  while ((match = static_cast<char*>(
              memcasechr(phaystack, pneedle[0], hayend - phaystack)))) {
    if (abel::strings_internal::char_case_cmp(match, pneedle, neelen) == 0)
      return match;
    else
      phaystack = match + 1;
  }
  return nullptr;
}

void BM_Memmatch(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        abel::strings_internal::char_match(kHaystack, kHaystackSize, "b", 1));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_Memmatch);

void BM_MemmatchMedium(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        abel::strings_internal::char_match(kHaystack, kHaystackSize, "ab", 2));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_MemmatchMedium);

void BM_MemmatchPathological(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(abel::strings_internal::char_match(
        kHaystack, kHaystackSize, kHaystack + kHaystackSize / 2,
        kHaystackSize - kHaystackSize / 2));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_MemmatchPathological);

void BM_Memcasematch(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(memcasematch(kHaystack, kHaystackSize, "b", 1));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_Memcasematch);

void BM_MemcasematchMedium(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(memcasematch(kHaystack, kHaystackSize, "ab", 2));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_MemcasematchMedium);

void BM_MemcasematchPathological(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(memcasematch(kHaystack, kHaystackSize,
                                          kHaystack + kHaystackSize / 2,
                                          kHaystackSize - kHaystackSize / 2));
  }
  state.SetBytesProcessed(kHaystackSize64 * state.iterations());
}
BENCHMARK(BM_MemcasematchPathological);

void BM_MemmemStartup(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(abel::strings_internal::char_mem(
        kHaystack + kHaystackSize - 10, 10, kHaystack + kHaystackSize - 1, 1));
  }
}
BENCHMARK(BM_MemmemStartup);

void BM_SearchStartup(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        std::search(kHaystack + kHaystackSize - 10, kHaystack + kHaystackSize,
                    kHaystack + kHaystackSize - 1, kHaystack + kHaystackSize));
  }
}
BENCHMARK(BM_SearchStartup);

void BM_MemmatchStartup(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(abel::strings_internal::char_match(
        kHaystack + kHaystackSize - 10, 10, kHaystack + kHaystackSize - 1, 1));
  }
}
BENCHMARK(BM_MemmatchStartup);

}  // namespace
