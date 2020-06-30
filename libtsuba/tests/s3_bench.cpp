#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <time.h>

#include <vector>
#include <limits>
#include <numeric>

#include "fmt/core.h"
#include "fmt/format.h"
#include "tsuba/tsuba.h"

constexpr static const char* const s3_url_base =
    "s3://witchel-tests-east2/test-";

// TODO: 2020/06/15 - Across different regions

/******************************************************************************/
/* Utilities */

// Prints formatted error message.
void vreport_error(const char* format, fmt::format_args args) {
  fmt::vprint(stderr, format, args);
}
template <typename... Args>
void LogE(const char* format, const Args&... args) {
  vreport_error(format, fmt::make_format_args(args...));
}

const char* unit[] = {"us", "ms", " s"};
long DivFactor(double us) {
  if (us < 1'000.0) {
    return 1;
  }
  us /= 1'000.0;
  if (us < 1'000.0) {
    return 1'000.0;
  }
  return 1'000'000.0;
}

static const std::unordered_map<long, std::string> df2unit{
    {1, "us"},
    {1'000, "ms"},
    {1'000'000, " s"},
};

std::string FmtResults(const std::vector<long>& v) {
  if (v.size() == 0)
    return "no results";
  long sum       = std::accumulate(v.begin(), v.end(), 0L);
  double mean    = (double)sum / v.size();
  long divFactor = DivFactor(mean);

  double accum = 0.0;
  std::for_each(std::begin(v), std::end(v),
                [&](const double d) { accum += (d - mean) * (d - mean); });
  double stdev = 0.0;
  if (v.size() > 1) {
    stdev = sqrt(accum / (v.size() - 1));
  }

  return fmt::format("{:>5.1f} {} (N={:d}) sd {:.1f}", mean / divFactor,
                     df2unit.at(divFactor), v.size(), stdev / divFactor);
}

struct timespec now() {
  struct timespec tp;
  // CLOCK_BOOTTIME is probably better, but Linux specific
  int ret = clock_gettime(CLOCK_MONOTONIC, &tp);
  if (ret < 0) {
    perror("clock_gettime");
    LogE("Bad return\n");
  }
  return tp;
}

struct timespec timespec_sub(struct timespec time, struct timespec oldTime) {
  if (time.tv_nsec < oldTime.tv_nsec)
    return (struct timespec){.tv_sec  = time.tv_sec - 1 - oldTime.tv_sec,
                             .tv_nsec = 1'000'000'000L + time.tv_nsec -
                                        oldTime.tv_nsec};
  else
    return (struct timespec){.tv_sec  = time.tv_sec - oldTime.tv_sec,
                             .tv_nsec = time.tv_nsec - oldTime.tv_nsec};
}

long timespec_to_us(struct timespec ts) {
  return ts.tv_sec * 1'000'000 + ts.tv_nsec / 1'000;
}

// 21 chars, with 1 null byte
void get_time_string(char* buf, int limit) {
  time_t rawtime;
  struct tm* timeinfo;
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  strftime(buf, limit, "%Y/%m/%d %H:%M:%S ", timeinfo);
}

void init_data(uint8_t* buf, int limit) {
  if (limit < 0)
    return;
  if (limit < 19) {
    for (; limit; limit--) {
      *buf++ = 'a';
    }
    return;
  } else {
    char tmp[32];             // Generous with space
    get_time_string(tmp, 31); // Trailing null
                              // Copy without trailing null
    memcpy(buf, tmp, 19);
    buf += 19;
    if (limit > 19) {
      *buf++ = ' ';
      for (limit -= 20; limit; limit--) {
        *buf++ = 'a';
      }
    }
  }
}

// Thank you, fmt!
std::string CntStr(int i, int width) {
  return fmt::format("{:0{}d}", i, width);
}
std::string MkS3url(int i, int width) {
  std::string url(s3_url_base);
  return url.append(CntStr(i, width));
}

/******************************************************************************/
// Storage interaction
//    Each function is a timed test, returns vector of times in microseconds
//    (longs)

std::vector<long> test_mem(const uint8_t* data, uint64_t size, int batch,
                           int numExperiments) {
  std::vector<int> fds(batch, 0);
  std::vector<long> results;

  struct timespec start;
  for (auto j = 0; j < numExperiments; ++j) {
    start = now();
    for (int i = 0; i < batch; ++i) {
      fds[i] = memfd_create(CntStr(i, 4).c_str(), 0);
      if (fds[i] < 0) {
        LogE("fd {:04d}\n", i);
        perror("memfd_create");
      }
      ssize_t bwritten = write(fds[i], data, size);
      if (bwritten != (ssize_t)size) {
        LogE("Short write tried {:d} wrote {:d}\n", size, bwritten);
        perror("write");
      }
    }
    results.push_back(timespec_to_us(timespec_sub(now(), start)));

    for (int i = 0; i < batch; ++i) {
      int sysret = close(fds[i]);
      if (sysret < 0) {
        perror("close");
      }
    }
  }
  return results;
}

std::vector<long> test_tmp(const uint8_t* data, uint64_t size, int batch,
                           int numExperiments) {
  std::vector<int> fds(batch, 0);
  std::vector<std::string> fnames;
  std::vector<long> results;
  for (int i = 0; i < batch; ++i) {
    std::string s("/tmp/witchel/");
    fnames.push_back(s.append(CntStr(i, 4)));
  }

  struct timespec start;
  for (auto j = 0; j < numExperiments; ++j) {
    start = now();
    for (int i = 0; i < batch; ++i) {
      fds[i] = open(fnames[i].c_str(), O_CREAT | O_TRUNC | O_RDWR,
                    S_IRWXU | S_IRWXG);
      if (fds[i] < 0) {
        LogE("fd {:d}\n", i);
        perror("/tmp O_CREAT");
      }
      ssize_t bwritten = write(fds[i], data, size);
      if (bwritten != (ssize_t)size) {
        LogE("Short write tried {:d} wrote {:d}\n", size, bwritten);
        perror("write");
      }
      // Make all data and directory changes persistent
      // sync is overkill, could sync fd and parent directory, but I'm being
      // lazy
      sync();
    }
    for (int i = 0; i < batch; ++i) {
      int sysret = close(fds[i]);
      if (sysret < 0) {
        perror("close");
      }
    }
    results.push_back(timespec_to_us(timespec_sub(now(), start)));

    for (int i = 0; i < batch; ++i) {
      int sysret = unlink(fnames[i].c_str());
      if (sysret < 0) {
        perror("unlink");
      }
    }
  }
  return results;
}

std::vector<long> test_s3(const uint8_t* data, uint64_t size, int batch,
                          int numExperiments) {
  std::vector<long> results;

  struct timespec start;
  for (auto j = 0; j < numExperiments; ++j) {
    start = now();
    for (int i = 0; i < batch; ++i) {
      std::string s3url = MkS3url(i, 4);
      int tsubaret      = tsuba::Store(s3url, data, size);
      if (tsubaret != 0) {
        LogE("Tsuba store bad return {:d}\n", tsubaret);
      }
    }
    results.push_back(timespec_to_us(timespec_sub(now(), start)));
  }
  return results;
}

std::vector<long> test_s3_sync(const uint8_t* data, uint64_t size, int batch,
                               int numExperiments) {
  std::vector<std::string> s3urls;
  std::vector<long> results;
  for (int i = 0; i < batch; ++i) {
    s3urls.push_back(MkS3url(i, 4));
  }

  struct timespec start;
  for (auto j = 0; j < numExperiments; ++j) {
    start = now();
    for (auto s3url : s3urls) {
      // Current API rejects empty writes
      int tsubaret = tsuba::StoreSync(s3url, data, size);
      if (tsubaret != 0) {
        LogE("Tsuba store sync bad return {:d}\n", tsubaret);
      }
    }
    results.push_back(timespec_to_us(timespec_sub(now(), start)));
  }
  return results;
}

std::vector<long> test_s3_async_one(const uint8_t* data, uint64_t size,
                                    int batch, int numExperiments) {
  std::vector<std::string> s3urls;
  std::vector<long> results;
  for (int i = 0; i < batch; ++i) {
    s3urls.push_back(MkS3url(i, 4));
  }

  struct timespec start;
  for (auto j = 0; j < numExperiments; ++j) {
    start = now();
    for (auto s3url : s3urls) {
      // Current API rejects empty writes
      int tsubaret = tsuba::StoreAsync(s3url, data, size);
      if (tsubaret != 0) {
        LogE("Tsuba store async bad return {:d}\n", tsubaret);
      }
      // Only 1 outstanding store at a time
      tsuba::StoreAsyncFinish(s3url);
    }
    results.push_back(timespec_to_us(timespec_sub(now(), start)));
  }
  return results;
}

std::vector<long> test_s3_async_batch(const uint8_t* data, uint64_t size,
                                      int batch, int numExperiments) {
  std::vector<std::string> s3urls;
  std::vector<long> results;
  for (int i = 0; i < batch; ++i) {
    s3urls.push_back(MkS3url(i, 4));
  }

  struct timespec start;
  for (auto j = 0; j < numExperiments; ++j) {
    start = now();
    for (auto s3url : s3urls) {
      // Current API rejects empty writes
      int tsubaret = tsuba::StoreAsync(s3url, data, size);
      if (tsubaret != 0) {
        LogE("Tsuba store async bad return {:d}\n", tsubaret);
      }
    }
    for (auto s3url : s3urls) {
      int ret = tsuba::StoreAsyncFinish(s3url);
      if (ret != 0) {
        LogE("Tsuba store async bad return {:d}\n", ret);
      }
    }
    results.push_back(timespec_to_us(timespec_sub(now(), start)));
  }
  return results;
}

std::vector<long> test_s3_multi_async_batch(const uint8_t* data, uint64_t size,
                                            int batch, int numExperiments) {
  std::vector<std::string> s3urls;
  std::vector<long> results;
  for (int i = 0; i < batch; ++i) {
    s3urls.push_back(MkS3url(i, 4));
  }

  struct timespec start;
  for (auto j = 0; j < numExperiments; ++j) {
    start = now();
    for (auto s3url : s3urls) {
      // Current API rejects empty writes
      int tsubaret = tsuba::StoreMultiAsync1(s3url, data, size);
      if (tsubaret != 0) {
        LogE("Tsuba store multi async1 bad return {:d}\n", tsubaret);
      }
    }
    for (auto s3url : s3urls) {
      // Current API rejects empty writes
      int tsubaret = tsuba::StoreMultiAsync2(s3url);
      if (tsubaret != 0) {
        LogE("Tsuba store multi async2 bad return {:d}\n", tsubaret);
      }
    }
    for (auto s3url : s3urls) {
      // Current API rejects empty writes
      int tsubaret = tsuba::StoreMultiAsync3(s3url);
      if (tsubaret != 0) {
        LogE("Tsuba store multi async3 bad return {:d}\n", tsubaret);
      }
    }
    for (auto s3url : s3urls) {
      int ret = tsuba::StoreMultiAsyncFinish(s3url);
      if (ret != 0) {
        LogE("Tsuba store multi async finish bad return {:d}\n", ret);
      }
    }
    results.push_back(timespec_to_us(timespec_sub(now(), start)));
  }
  return results;
}

/******************************************************************************/
/* Main */

static uint8_t data_19B[19];
static uint8_t data_10MB[10 * (1UL << 20)];
static uint8_t data_100MB[100 * (1UL << 20)];
static uint8_t data_500MB[500 * (1UL << 20)];
static uint8_t data_1GB[(1UL << 30)];

struct {
  uint8_t* data;
  uint64_t size;
  int batch;
  int numExperiments; // For stats
  const char* name;
} datas[] = {
    {.data           = data_19B,
     .size           = sizeof(data_19B),
     .batch          = 8,
     .numExperiments = 3,
     .name           = "  19B"},
    {.data           = data_10MB,
     .size           = sizeof(data_10MB),
     .batch          = 8,
     .numExperiments = 3,
     .name           = " 10MB"},
    {.data           = data_100MB,
     .size           = sizeof(data_100MB),
     .batch          = 8,
     .numExperiments = 3,
     .name           = "100MB"},
    {.data           = data_500MB,
     .size           = sizeof(data_500MB),
     .batch          = 8,
     .numExperiments = 3,
     .name           = "500MB"},
    {.data           = data_1GB,
     .size           = sizeof(data_1GB),
     .batch          = 6,
     .numExperiments = 1,
     .name           = "1GB"},

};

struct {
  const char* name;
  std::vector<long> (*func)(const uint8_t*, uint64_t, int, int);
} tests[] = {
    {.name = "memfd_create", .func = test_mem},
    {.name = "/tmp create", .func = test_tmp},
    {.name = "S3 Put ASync One", .func = test_s3_async_one},
    {.name = "S3 Put Sync", .func = test_s3_sync},
    {.name = "S3 Put Async Batch", .func = test_s3_async_batch},
    {.name = "S3 Put", .func = test_s3},
    {.name = "S3 Put Multi Async Batch", .func = test_s3_multi_async_batch},
};

int main() {
  tsuba::Init();
  for (unsigned long i = 0; i < sizeof(datas) / sizeof(datas[0]); ++i) {
    init_data(datas[i].data, datas[i].size);
  }

  if (access("/tmp/witchel", R_OK) != 0) {
    if (mkdir("/tmp/witchel", 0775) != 0) {
      perror("mkdir /tmp/witchel");
      exit(10);
    }
  }

  printf("*** VM and bucket same region\n");
  for (unsigned long i = 0; i < sizeof(datas) / sizeof(datas[0]); ++i) {
    printf("** size %s\n", datas[i].name);
    const uint8_t* data = datas[i].data;
    uint64_t size       = datas[i].size;
    int batch           = datas[i].batch;
    int numExperiments  = datas[i].numExperiments;

    for (unsigned long j = 0; j < sizeof(tests) / sizeof(tests[0]); ++j) {
      std::vector<long> results =
          tests[j].func(data, size, batch, numExperiments);
      fmt::print("{:<24} ({:2d}) {}\n", tests[j].name, batch,
                 FmtResults(results));
    }
  }

  return 0;
}

// 2020/06/29

// *** VM and bucket same region
// ** size   19B
// memfd_create             ( 8)  41.7 us (N=3) sd 21.1
// /tmp create              ( 8)  47.3 ms (N=3) sd 44.1
// S3 Put ASync One         ( 8) 261.0 ms (N=3) sd 188.2
// S3 Put Sync              ( 8) 500.0 ms (N=3) sd 172.1
// S3 Put Async Batch       ( 8)  48.7 ms (N=3) sd 30.2
// S3 Put                   ( 8)   1.2  s (N=3) sd 0.0
// S3 Put Multi Async Batch ( 8) 186.6 ms (N=3) sd 34.9
// ** size  10MB
// memfd_create             ( 8)  33.5 ms (N=3) sd 0.7
// /tmp create              ( 8) 345.0 ms (N=3) sd 35.5
// S3 Put ASync One         ( 8)   1.7  s (N=3) sd 0.2
// S3 Put Sync              ( 8)   2.1  s (N=3) sd 0.3
// S3 Put Async Batch       ( 8) 815.8 ms (N=3) sd 73.1
// S3 Put                   ( 8)   2.6  s (N=3) sd 0.3
// S3 Put Multi Async Batch ( 8) 914.9 ms (N=3) sd 30.9
// ** size 100MB
// memfd_create             ( 8) 376.7 ms (N=3) sd 0.2
// /tmp create              ( 8)   6.2  s (N=3) sd 0.5
// S3 Put ASync One         ( 8)  10.3  s (N=3) sd 0.5
// S3 Put Sync              ( 8)  10.7  s (N=3) sd 0.3
// S3 Put Async Batch       ( 8)   6.9  s (N=3) sd 0.0
// S3 Put                   ( 8)   9.1  s (N=3) sd 0.3
// S3 Put Multi Async Batch ( 8)   7.0  s (N=3) sd 0.0
// ** size 500MB
// memfd_create             ( 8)   1.9  s (N=3) sd 0.0
// /tmp create              ( 8)  32.2  s (N=3) sd 0.1
// S3 Put ASync One         ( 8)  50.0  s (N=3) sd 0.8
// S3 Put Sync              ( 8)  47.8  s (N=3) sd 1.6
// S3 Put Async Batch       ( 8)  38.1  s (N=3) sd 6.4
// S3 Put                   ( 8)  36.8  s (N=3) sd 0.8
// S3 Put Multi Async Batch ( 8)  35.6  s (N=3) sd 2.0
// ** size 1GB
// memfd_create             ( 6)   2.9  s (N=1) sd 0.0
// /tmp create              ( 6)  50.1  s (N=1) sd 0.0
// S3 Put ASync One         ( 6)  68.2  s (N=1) sd 0.0
// S3 Put Sync              ( 6)  70.8  s (N=1) sd 0.0
// S3 Put Async Batch       ( 6)  52.6  s (N=1) sd 0.0
// S3 Put                   ( 6)  54.3  s (N=1) sd 0.0
// S3 Put Multi Async Batch ( 6)  52.9  s (N=1) sd 0.0