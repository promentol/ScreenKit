// What the process costs, read from the kernel, and a JSON writer for the report.
//
// Linux only: everything here comes from /proc and /sys, which is what a Raspberry Pi has.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bench {

/// Milliseconds on the monotonic clock since this process first asked.
double nowMs();

/// The process's memory as the kernel counts it. -1 where the kernel did not say.
struct Memory {
  long rssKb = -1;           // VmRSS: resident, everything
  long rssAnonKb = -1;       // RssAnon: memory the process made -- heaps, JIT code, stacks
  long rssFileKb = -1;       // RssFile: resident pages of mapped files -- libraries, mapped bytecode
  long hwmKb = -1;           // VmHWM: the peak of VmRSS so far
  long vmSizeKb = -1;        // VmSize: address space reserved, resident or not
  long pssKb = -1;           // Pss (smaps_rollup): resident, shared pages divided among their users
  long privateDirtyKb = -1;  // Private_Dirty (smaps_rollup): what only this process has written
  long swapKb = -1;          // VmSwap
  int threads = -1;
};
Memory readMemory();

/// VmRSS in KB from /proc/self/statm: one read on a descriptor kept open, cheap enough to
/// take after every frame for a peak.
long residentKb();

/// getrusage(RUSAGE_SELF): every thread's CPU, so background GC and JIT work count.
struct Usage {
  double userMs = 0, sysMs = 0;
  long minorFaults = 0, majorFaults = 0;
  long voluntarySwitches = 0, involuntarySwitches = 0;
};
Usage readUsage();

struct Stats {
  std::size_t count = 0;
  double mean = 0, min = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
  std::size_t overBudget = 0;  // samples longer than the budget
};
Stats summarize(std::vector<double> samples, double budgetMs);

/// An indenting JSON writer: containers, keys, scalars, and raw JSON handed over by the engine.
class Json {
 public:
  Json& open(char bracket);  // '{' or '['
  Json& close();
  Json& key(const std::string& name);
  Json& value(double v);
  Json& value(long v);
  Json& value(int v) { return value(static_cast<long>(v)); }
  Json& value(std::size_t v) { return value(static_cast<long>(v)); }
  Json& value(bool v);
  Json& value(const std::string& v);
  Json& value(const char* v) { return value(std::string(v)); }
  Json& raw(const std::string& json);
  template <typename T>
  Json& field(const std::string& name, const T& v) {
    return key(name).value(v);
  }
  const std::string& str() const { return out_; }

 private:
  struct Level {
    char closer;
    bool empty;
  };
  void element();
  std::string out_;
  std::vector<Level> levels_;
  bool afterKey_ = false;
};

void write(Json& json, const Memory& memory);
void write(Json& json, const Usage& usage);
void write(Json& json, const Stats& stats);

/// The device: CPU model, cores, memory, load, and the clock and temperature that say whether
/// a Pi was throttling.
void writeMachine(Json& json);
/// Only what moves during a run: CPU clock, temperature, load, available memory.
void writeMachineNow(Json& json);

std::string readFile(const std::string& path);
std::string jsonEscape(const std::string& text);

}  // namespace bench
