#include "Metrics.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bench {

double nowMs() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

namespace {

// "VmRSS:     1234 kB" -> 1234, for the named field of a /proc file.
long fieldKb(const std::string& text, const char* name) {
  const auto at = text.find(name);
  if (at == std::string::npos) return -1;
  return std::strtol(text.c_str() + at + std::strlen(name), nullptr, 10);
}

std::string firstLine(const std::string& path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

}  // namespace

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

Memory readMemory() {
  Memory m;
  std::string status;
  try {
    status = readFile("/proc/self/status");
  } catch (const std::exception&) {
    return m;
  }
  m.rssKb = fieldKb(status, "\nVmRSS:");
  m.rssAnonKb = fieldKb(status, "\nRssAnon:");
  m.rssFileKb = fieldKb(status, "\nRssFile:");
  m.hwmKb = fieldKb(status, "\nVmHWM:");
  m.vmSizeKb = fieldKb(status, "\nVmSize:");
  m.swapKb = fieldKb(status, "\nVmSwap:");
  m.threads = static_cast<int>(fieldKb(status, "\nThreads:"));
  try {
    const auto rollup = readFile("/proc/self/smaps_rollup");
    m.pssKb = fieldKb(rollup, "\nPss:");
    m.privateDirtyKb = fieldKb(rollup, "\nPrivate_Dirty:");
  } catch (const std::exception&) {
  }
  return m;
}

long residentKb() {
  static const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
  static const long pageKb = ::sysconf(_SC_PAGESIZE) / 1024;
  if (fd < 0) return -1;
  char buf[128];
  const auto n = ::pread(fd, buf, sizeof buf - 1, 0);
  if (n <= 0) return -1;
  buf[n] = '\0';
  char* end = nullptr;
  std::strtol(buf, &end, 10);  // size
  return std::strtol(end, nullptr, 10) * pageKb;
}

Usage readUsage() {
  rusage r{};
  ::getrusage(RUSAGE_SELF, &r);
  Usage u;
  u.userMs = r.ru_utime.tv_sec * 1000.0 + r.ru_utime.tv_usec / 1000.0;
  u.sysMs = r.ru_stime.tv_sec * 1000.0 + r.ru_stime.tv_usec / 1000.0;
  u.minorFaults = r.ru_minflt;
  u.majorFaults = r.ru_majflt;
  u.voluntarySwitches = r.ru_nvcsw;
  u.involuntarySwitches = r.ru_nivcsw;
  return u;
}

Stats summarize(std::vector<double> samples, double budgetMs) {
  Stats s;
  s.count = samples.size();
  if (samples.empty()) return s;
  std::sort(samples.begin(), samples.end());
  double sum = 0;
  for (const double v : samples) {
    sum += v;
    if (v > budgetMs) s.overBudget++;
  }
  // Nearest rank: the smallest sample at or above the percentile.
  const auto rank = [&](double p) {
    const auto i = static_cast<std::size_t>(std::ceil(p * static_cast<double>(samples.size()))) - 1;
    return samples[std::min(i, samples.size() - 1)];
  };
  s.mean = sum / static_cast<double>(samples.size());
  s.min = samples.front();
  s.p50 = rank(0.50);
  s.p95 = rank(0.95);
  s.p99 = rank(0.99);
  s.max = samples.back();
  return s;
}

// ---- JSON ---------------------------------------------------------------------

std::string jsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void Json::element() {
  if (afterKey_) {
    afterKey_ = false;
    return;
  }
  if (!levels_.empty()) {
    if (!levels_.back().empty) out_ += ',';
    levels_.back().empty = false;
    out_ += '\n';
    out_.append(levels_.size() * 2, ' ');
  }
}

Json& Json::open(char bracket) {
  element();
  out_ += bracket;
  levels_.push_back({bracket == '{' ? '}' : ']', true});
  return *this;
}

Json& Json::close() {
  const Level level = levels_.back();
  levels_.pop_back();
  if (!level.empty) {
    out_ += '\n';
    out_.append(levels_.size() * 2, ' ');
  }
  out_ += level.closer;
  return *this;
}

Json& Json::key(const std::string& name) {
  element();
  out_ += '"' + jsonEscape(name) + "\": ";
  afterKey_ = true;
  return *this;
}

Json& Json::value(double v) {
  element();
  if (!std::isfinite(v)) {
    out_ += "null";
    return *this;
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  // %g drops to exponent form past 4 significant digits; keep integers whole.
  if (std::fabs(v) >= 1e4 && std::fabs(v) < 1e15) std::snprintf(buf, sizeof buf, "%.1f", v);
  out_ += buf;
  return *this;
}

Json& Json::value(long v) {
  element();
  out_ += std::to_string(v);
  return *this;
}

Json& Json::value(bool v) {
  element();
  out_ += v ? "true" : "false";
  return *this;
}

Json& Json::value(const std::string& v) {
  element();
  out_ += '"' + jsonEscape(v) + '"';
  return *this;
}

Json& Json::raw(const std::string& json) {
  element();
  out_ += json.empty() ? "null" : json;
  return *this;
}

// ---- the report's pieces --------------------------------------------------------

void write(Json& json, const Memory& m) {
  json.open('{')
      .field("rssKb", m.rssKb)
      .field("rssAnonKb", m.rssAnonKb)
      .field("rssFileKb", m.rssFileKb)
      .field("hwmKb", m.hwmKb)
      .field("pssKb", m.pssKb)
      .field("privateDirtyKb", m.privateDirtyKb)
      .field("vmSizeKb", m.vmSizeKb)
      .field("swapKb", m.swapKb)
      .field("threads", m.threads)
      .close();
}

void write(Json& json, const Usage& u) {
  json.open('{')
      .field("userMs", u.userMs)
      .field("sysMs", u.sysMs)
      .field("minorFaults", u.minorFaults)
      .field("majorFaults", u.majorFaults)
      .field("voluntarySwitches", u.voluntarySwitches)
      .field("involuntarySwitches", u.involuntarySwitches)
      .close();
}

void write(Json& json, const Stats& s) {
  json.open('{')
      .field("count", s.count)
      .field("mean", s.mean)
      .field("min", s.min)
      .field("p50", s.p50)
      .field("p95", s.p95)
      .field("p99", s.p99)
      .field("max", s.max)
      .field("overBudget", s.overBudget)
      .close();
}

namespace {

std::string cpuModel() {
  std::ifstream in("/proc/cpuinfo");
  std::string line, model;
  while (std::getline(in, line)) {
    // A Pi names the board in "Model"; x86 names the CPU in "model name".
    if (line.rfind("Model", 0) == 0 || line.rfind("model name", 0) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos) model = line.substr(line.find_first_not_of(' ', colon + 1));
      if (line.rfind("Model", 0) == 0) break;
    }
  }
  return model;
}

/// One line of a command's output, or "" -- for vcgencmd, which is only on a Pi.
std::string commandLine(const char* command) {
  FILE* pipe = ::popen(command, "r");
  if (!pipe) return {};
  char buf[256] = {0};
  const bool got = std::fgets(buf, sizeof buf, pipe) != nullptr;
  ::pclose(pipe);
  std::string line = got ? buf : "";
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  return line;
}

/// "key=value" -> value as a number (hex allowed), or -1.
double afterEquals(const std::string& line) {
  const auto eq = line.find('=');
  if (eq == std::string::npos) return -1;
  return static_cast<double>(std::strtoull(line.c_str() + eq + 1, nullptr, 0));
}

long sysLong(const char* path) {
  const auto line = firstLine(path);
  return line.empty() ? -1 : std::strtol(line.c_str(), nullptr, 10);
}

}  // namespace

void writeMachineNow(Json& json) {
  std::string meminfo;
  try {
    meminfo = readFile("/proc/meminfo");
  } catch (const std::exception&) {
  }
  const long temp = sysLong("/sys/class/thermal/thermal_zone0/temp");
  json.open('{')
      .field("cpuFreqKHz", sysLong("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"))
      .field("temperatureC", temp < 0 ? -1.0 : temp / 1000.0)
      .field("loadavg", firstLine("/proc/loadavg"))
      .field("memAvailableKb", fieldKb("\n" + meminfo, "\nMemAvailable:"))
      // The firmware's word on it, which cpufreq does not show: on a Pi with a weak
      // supply the ARM runs at 600 MHz while scaling_cur_freq still says 1.4 GHz.
      // Bits 0-3 are now (under-voltage, capped, throttled, soft limit), 16-19 since boot.
      .field("throttled", afterEquals(commandLine("vcgencmd get_throttled 2>/dev/null")))
      .field("armClockHz", afterEquals(commandLine("vcgencmd measure_clock arm 2>/dev/null")))
      .close();
}

void writeMachine(Json& json) {
  std::string meminfo;
  try {
    meminfo = readFile("/proc/meminfo");
  } catch (const std::exception&) {
  }
  json.open('{')
      .field("cpu", cpuModel())
      .field("cores", ::sysconf(_SC_NPROCESSORS_ONLN))
      .field("kernel", firstLine("/proc/sys/kernel/osrelease"))
      .field("memTotalKb", fieldKb("\n" + meminfo, "\nMemTotal:"))
      .field("cpuMaxFreqKHz", sysLong("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"))
      .field("governor", firstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"))
      .key("now");
  writeMachineNow(json);
  json.close();
}

}  // namespace bench
