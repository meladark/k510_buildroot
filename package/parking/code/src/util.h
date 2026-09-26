#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace util {

int64_t now_ms();                 // wall clock, ms since epoch
int64_t mono_ms();                // monotonic clock, ms
std::string time_str(int64_t ms, const char *fmt);  // local time formatting
bool clock_is_sane();             // false while the clock still sits at the 2010 boot default

bool read_file(const std::string &path, std::string &out);
bool write_file_atomic(const std::string &path, const std::string &data);
bool mkdirs(const std::string &path);
bool is_dir(const std::string &path);
std::vector<std::string> list_dir(const std::string &path);  // sorted names, no . and ..
int64_t file_size(const std::string &path);
bool remove_tree(const std::string &path);
double free_space_pct(const std::string &path);

// Runs argv[0] with arguments (no shell), returns exit code, captures stdout.
int run(const std::vector<std::string> &argv, std::string *out = nullptr, int timeout_ms = 10000);

// IPv4 address of an interface, empty if none.
std::string iface_ip(const char *ifname);

std::string json_escape(const std::string &s);
std::string to_hex(const std::string &s);
bool safe_rel_path(const std::string &p);  // rejects "..", absolute paths, odd chars

}  // namespace util
