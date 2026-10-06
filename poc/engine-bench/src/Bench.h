// The engine-neutral half of a bench host: options, the frame loop, and the report. Each engine
// is one small class behind `Engine`; everything that is measured is measured here, the same
// way for both.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Metrics.h"

namespace bench {

struct Options {
  std::string workload;              // the script that defines `bench` (.js, or .hbc for Hermes)
  std::vector<std::string> preload;  // scripts evaluated before it, in order
  std::string jit;                   // the engine's own JIT mode; empty is the engine's default
  std::string label;                 // names the run in the report
  int frames = 180;                  // frames per scene and load
  int warmup = 30;                   // leading frames kept out of the steady numbers
  double pace = 0;                   // frames per second to hold to; 0 runs them back to back
  double budgetMs = 1000.0 / 60.0;   // a frame over this counts as over budget
  double scale = 1;                  // multiplies every load
  std::string scenes;                // comma list; empty is every scene
  std::vector<std::pair<std::string, std::string>> loads;  // scene -> "1,2,3", replacing its defaults
  std::vector<std::pair<std::string, std::string>> params; // engine tuning, name -> value (each engine lists its own)
  int viewWidth = 640, viewHeight = 480;
  std::string out;                   // the report's path; empty writes it to stdout
};

/// A heap as the engine describes it. Engines count different things, so `raw` keeps every
/// counter under the engine's own name and the rest is the closest common reading; -1 where the
/// engine has no such number.
struct Heap {
  std::vector<std::pair<std::string, double>> raw;
  double usedBytes = -1;       // what the GC heap holds now, live and not yet collected
  double reservedBytes = -1;   // what the GC heap has taken from the OS
  double collections = -1;     // collections so far
  double pauseMs = -1;         // time the JS thread spent stopped in collections so far
  double maxPauseMs = -1;      // the longest single stop since resetPauseWindow()
  double backgroundMs = -1;    // collection work done off the JS thread so far
};

class Engine {
 public:
  virtual ~Engine() = default;
  virtual std::string name() const = 0;
  virtual std::string version() const = 0;
  virtual std::string mode() const = 0;
  /// Engine settings worth keeping with the numbers, as fields of an open JSON object.
  virtual void describe(Json& json) const = 0;
  /// Evaluates a script file; throws std::runtime_error carrying the JavaScript error.
  virtual void evaluate(const std::string& path) = 0;
  // The workload's `bench` object (js/workload.js has the contract).
  virtual std::string info() = 0;
  virtual std::string plan(const std::string& optionsJson) = 0;
  virtual void setView(int width, int height) = 0;
  virtual void enter(const std::string& scene, int load) = 0;
  /// bench.frame(), then whatever jobs (promise reactions) it queued.
  virtual void frame() = 0;
  virtual std::string exit() = 0;
  virtual void collectGarbage() = 0;
  virtual Heap heap() = 0;
  virtual void resetPauseWindow() = 0;
};

using EngineFactory = std::function<std::unique_ptr<Engine>(const Options&)>;

/// The whole run: parse argv, make the engine, load, play every scene, write the report.
/// `jitModes` and `params` are the usage text for --jit and --param.
int run(int argc, char** argv, const char* program, const char* jitModes, const char* params, const EngineFactory& make);

/// An integer --param value, or an exception naming it.
long paramValue(const std::pair<std::string, std::string>& param);

/// The path of the shared library that holds `symbol` -- which build of an engine this process
/// actually loaded, whatever LD_LIBRARY_PATH did.
std::string libraryOf(const void* symbol);

}  // namespace bench
