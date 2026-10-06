#include "Bench.h"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace bench {

namespace {

void usage(const char* program, const char* jitModes, const char* params) {
  std::fprintf(stderr,
               "usage: %s [options] <workload>\n"
               "\n"
               "  <workload>              the script that defines `bench` (js/workload.js)\n"
               "  --preload <script>      evaluate a script first (repeatable): the bulk code\n"
               "  --jit <mode>            %s\n"
               "  --param <name=value>    engine tuning (repeatable): %s\n"
               "  --frames <n>            frames per scene and load (180)\n"
               "  --warmup <n>            leading frames left out of the steady numbers (30)\n"
               "  --pace <fps>            hold to a frame rate instead of running flat out\n"
               "  --scenes <a,b,...>      only these scenes\n"
               "  --loads <scene=1,2,3>   replace a scene's loads (repeatable)\n"
               "  --scale <x>             multiply every load\n"
               "  --view <WxH>            the screen the camera fits (640x480)\n"
               "  --label <name>          names this run in the report\n"
               "  --out <report.json>     where the report goes (stdout)\n",
               program, jitModes, params);
}

Options parse(int argc, char** argv, const char* program, const char* jitModes, const char* params) {
  Options o;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(arg + " needs a value");
      return argv[++i];
    };
    if (arg == "--preload") o.preload.push_back(next());
    else if (arg == "--jit") o.jit = next();
    else if (arg == "--frames") o.frames = std::stoi(next());
    else if (arg == "--warmup") o.warmup = std::stoi(next());
    else if (arg == "--pace") o.pace = std::stod(next());
    else if (arg == "--scenes") o.scenes = next();
    else if (arg == "--scale") o.scale = std::stod(next());
    else if (arg == "--label") o.label = next();
    else if (arg == "--out") o.out = next();
    else if (arg == "--loads") {
      const auto spec = next();
      const auto eq = spec.find('=');
      if (eq == std::string::npos) throw std::invalid_argument("--loads takes scene=1,2,3");
      o.loads.emplace_back(spec.substr(0, eq), spec.substr(eq + 1));
    } else if (arg == "--param") {
      const auto spec = next();
      const auto eq = spec.find('=');
      if (eq == std::string::npos) throw std::invalid_argument("--param takes name=value");
      o.params.emplace_back(spec.substr(0, eq), spec.substr(eq + 1));
    } else if (arg == "--view") {
      const auto spec = next();
      if (std::sscanf(spec.c_str(), "%dx%d", &o.viewWidth, &o.viewHeight) != 2) throw std::invalid_argument("--view takes WxH");
    } else if (arg == "-h" || arg == "--help") {
      usage(program, jitModes, params);
      std::exit(0);
    } else if (arg.rfind("--", 0) == 0) {
      throw std::invalid_argument("unknown option " + arg);
    } else {
      o.workload = arg;
    }
  }
  if (o.workload.empty()) throw std::invalid_argument("no workload script");
  if (o.frames < 1 || o.warmup < 0) throw std::invalid_argument("--frames must be positive and --warmup not negative");
  return o;
}

std::string planOptions(const Options& o) {
  Json json;
  json.open('{').field("scenes", o.scenes).field("scale", o.scale).key("loads").open('{');
  for (const auto& load : o.loads) json.field(load.first, load.second);
  json.close().close();
  return json.str();
}

std::string basename(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void write(Json& json, const Heap& h) {
  json.open('{')
      .field("usedBytes", h.usedBytes)
      .field("reservedBytes", h.reservedBytes)
      .field("collections", h.collections)
      .field("pauseMs", h.pauseMs)
      .field("backgroundMs", h.backgroundMs)
      .key("raw")
      .open('{');
  for (const auto& entry : h.raw) json.field(entry.first, entry.second);
  json.close().close();
}

double delta(double after, double before) {
  return after < 0 || before < 0 ? -1 : after - before;
}

struct Step {
  std::string name;
  double ms;
  Memory memory;
  Heap heap;
};

struct Segment {
  std::string scene;
  int load = 0;
  double enterMs = 0, exitMs = 0, wallMs = 0;
  std::vector<double> frameMs;
  Usage usageBefore, usageAfter;
  long peakKb = -1;
  Memory memoryBefore, memoryEnd;
  Heap heapBefore, heapEnd;
  std::string result;
};

void writeSegment(Json& json, const Segment& s, const Options& o) {
  const auto warm = std::min<std::size_t>(static_cast<std::size_t>(o.warmup), s.frameMs.size());
  const std::vector<double> warmup(s.frameMs.begin(), s.frameMs.begin() + static_cast<long>(warm));
  const std::vector<double> steady(s.frameMs.begin() + static_cast<long>(warm), s.frameMs.end());
  const double cpu = (s.usageAfter.userMs + s.usageAfter.sysMs) - (s.usageBefore.userMs + s.usageBefore.sysMs);
  json.open('{')
      .field("scene", s.scene)
      .field("load", s.load)
      .field("enterMs", s.enterMs)
      .field("exitMs", s.exitMs)
      .field("firstFrameMs", s.frameMs.empty() ? -1.0 : s.frameMs.front())
      .key("steady");
  write(json, summarize(steady, o.budgetMs));
  json.key("warmup");
  write(json, summarize(warmup, o.budgetMs));
  json.field("wallMs", s.wallMs)
      .field("cpuMs", cpu)
      // All threads' CPU per frame: a JIT compiling or a GC marking in the background shows here
      // and not in the frame time.
      .field("cpuPerFrameMs", cpu / static_cast<double>(std::max<std::size_t>(1, s.frameMs.size())))
      .field("peakRssKb", s.peakKb)
      .field("rssGrowthKb", s.memoryEnd.rssKb - s.memoryBefore.rssKb)
      .key("gc")
      .open('{')
      .field("collections", delta(s.heapEnd.collections, s.heapBefore.collections))
      .field("pauseMs", delta(s.heapEnd.pauseMs, s.heapBefore.pauseMs))
      .field("maxPauseMs", s.heapEnd.maxPauseMs)
      .field("backgroundMs", delta(s.heapEnd.backgroundMs, s.heapBefore.backgroundMs))
      .close()
      .key("memory");
  write(json, s.memoryEnd);
  json.key("heap");
  write(json, s.heapEnd);
  json.key("result").raw(s.result).close();
}

}  // namespace

long paramValue(const std::pair<std::string, std::string>& param) {
  char* end = nullptr;
  const long value = std::strtol(param.second.c_str(), &end, 10);
  if (param.second.empty() || *end != '\0') throw std::invalid_argument("--param " + param.first + " takes an integer");
  return value;
}

std::string libraryOf(const void* symbol) {
  Dl_info info{};
  if (::dladdr(symbol, &info) == 0 || info.dli_fname == nullptr) return "";
  char resolved[4096];
  return ::realpath(info.dli_fname, resolved) ? resolved : info.dli_fname;
}

int run(int argc, char** argv, const char* program, const char* jitModes, const char* params, const EngineFactory& make) {
  nowMs();  // the clock's zero
  Options o;
  try {
    o = parse(argc, argv, program, jitModes, params);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n\n", program, e.what());
    usage(program, jitModes, params);
    return 2;
  }

  Json report;
  report.open('{').field("label", o.label).key("machine");
  writeMachine(report);

  std::vector<Step> startup;
  std::vector<Segment> segments;
  std::unique_ptr<Engine> engine;
  std::string info, failure;
  try {
    startup.push_back({"process", 0, readMemory(), {}});
    double t = nowMs();
    engine = make(o);
    startup.push_back({"engine", nowMs() - t, readMemory(), engine->heap()});
    std::fprintf(stderr, "[%s %s] %s, %.1f ms to start, RSS %.1f MB\n", engine->name().c_str(), engine->mode().c_str(),
                 engine->version().c_str(), startup.back().ms, startup.back().memory.rssKb / 1024.0);

    std::vector<std::string> scripts = o.preload;
    scripts.push_back(o.workload);
    for (const auto& script : scripts) {
      t = nowMs();
      engine->evaluate(script);
      startup.push_back({"load " + basename(script), nowMs() - t, readMemory(), engine->heap()});
      std::fprintf(stderr, "[%s %s] %s: %.1f ms, RSS %.1f MB\n", engine->name().c_str(), engine->mode().c_str(),
                   basename(script).c_str(), startup.back().ms, startup.back().memory.rssKb / 1024.0);
    }

    engine->setView(o.viewWidth, o.viewHeight);
    info = engine->info();
    std::istringstream plan(engine->plan(planOptions(o)));
    std::string scene;
    int load = 0;
    while (plan >> scene >> load) {
      Segment s;
      s.scene = scene;
      s.load = load;
      s.memoryBefore = readMemory();
      t = nowMs();
      engine->enter(scene, load);
      s.enterMs = nowMs() - t;

      s.heapBefore = engine->heap();
      engine->resetPauseWindow();
      s.usageBefore = readUsage();
      s.frameMs.reserve(static_cast<std::size_t>(o.frames));
      const auto start = std::chrono::steady_clock::now();
      for (int f = 0; f < o.frames; f++) {
        const double f0 = nowMs();
        engine->frame();
        s.frameMs.push_back(nowMs() - f0);
        s.peakKb = std::max(s.peakKb, residentKb());
        if (o.pace > 0) {
          std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                    std::chrono::duration<double>((f + 1) / o.pace)));
        }
      }
      s.wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      s.usageAfter = readUsage();
      s.memoryEnd = readMemory();
      s.heapEnd = engine->heap();

      t = nowMs();
      s.result = engine->exit();
      s.exitMs = nowMs() - t;
      segments.push_back(std::move(s));

      const auto& done = segments.back();
      const auto steady = summarize(
          std::vector<double>(done.frameMs.begin() + std::min<long>(o.warmup, static_cast<long>(done.frameMs.size())),
                              done.frameMs.end()),
          o.budgetMs);
      std::fprintf(stderr, "[%s %s] %-10s %5d: p50 %7.2f ms  p95 %7.2f  max %7.2f | RSS peak %6.1f MB | gc %g, %.1f ms\n",
                   engine->name().c_str(), engine->mode().c_str(), done.scene.c_str(), done.load, steady.p50,
                   steady.p95, steady.max, done.peakKb / 1024.0, delta(done.heapEnd.collections, done.heapBefore.collections),
                   delta(done.heapEnd.pauseMs, done.heapBefore.pauseMs));
    }
  } catch (const std::exception& e) {
    failure = e.what();
    std::fprintf(stderr, "%s: %s\n", program, failure.c_str());
  }

  if (engine) {
    report.field("engine", engine->name()).field("version", engine->version()).field("mode", engine->mode());
    report.key("settings").open('{');
    engine->describe(report);
    report.close();
  }
  report.key("options")
      .open('{')
      .field("workload", o.workload)
      .field("frames", o.frames)
      .field("warmup", o.warmup)
      .field("pace", o.pace)
      .field("budgetMs", o.budgetMs)
      .field("scale", o.scale)
      .field("view", std::to_string(o.viewWidth) + "x" + std::to_string(o.viewHeight))
      .key("params")
      .open('{');
  for (const auto& p : o.params) report.field(p.first, p.second);
  report.close().key("preload").open('[');
  for (const auto& p : o.preload) report.value(p);
  report.close().close();
  report.key("workload").raw(info);

  report.key("startup").open('[');
  for (const auto& step : startup) {
    report.open('{').field("step", step.name).field("ms", step.ms).key("memory");
    write(report, step.memory);
    report.key("heap");
    write(report, step.heap);
    report.close();
  }
  report.close();

  report.key("segments").open('[');
  for (const auto& s : segments) writeSegment(report, s, o);
  report.close();

  if (engine && failure.empty()) {
    report.key("end").open('{').key("memory");
    write(report, readMemory());
    report.key("heap");
    write(report, engine->heap());
    report.key("usage");
    write(report, readUsage());
    // What the engine keeps once everything is let go: the floor it returns to.
    engine->collectGarbage();
    report.key("afterGc").open('{').key("memory");
    write(report, readMemory());
    report.key("heap");
    write(report, engine->heap());
    report.close().close();
  }
  report.key("machineEnd");
  writeMachineNow(report);
  report.field("wallMs", nowMs());
  if (!failure.empty()) report.field("error", failure);
  report.close();

  const auto text = report.str() + "\n";
  if (o.out.empty()) {
    std::fwrite(text.data(), 1, text.size(), stdout);
  } else {
    std::ofstream(o.out) << text;
    std::fprintf(stderr, "report: %s\n", o.out.c_str());
  }
  return failure.empty() ? 0 : 1;
}

}  // namespace bench
