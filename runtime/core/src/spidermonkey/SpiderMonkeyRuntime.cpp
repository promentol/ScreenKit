// Copyright (c) ScreenKit contributors. MIT.
//
// JSI over SpiderMonkey. The shape, piece by piece:
//
//   JSI handle        a Node: one GC edge (JS::Heap) the runtime traces as a root,
//                     pooled, released from any thread
//   PropNameID        a Node holding a jsid -- an atom, an index or a symbol
//   host function     a native JSFunction whose reserved slot holds an object
//                     owning the std::function (freed by that object's finalizer)
//   host object       a proxy whose handler forwards to the jsi::HostObject
//   NativeState       an entry in a WeakMap from the object to a holder
//   WeakObject        a Node traced weakly, cleared after a GC that frees its object
//   microtasks        our own job queue: promise jobs and queueMicrotask callbacks
//                     run when the host drains it, never on their own
//   scripts           compiled to a stencil, then instantiated and run; a buffer
//                     that is already a stencil (compileToStencil) skips parsing
//
// Everything runs on the runtime's thread unless it says otherwise.
//
// Compiled with -fno-rtti, as libmozjs is: the proxy handler subclasses an
// engine class whose typeinfo the library does not export.
#include "SpiderMonkeyRuntime.h"

#include <js/Array.h>
#include <js/ArrayBuffer.h>
#include <js/BigInt.h>
#include <js/BuildId.h>
#include <js/CallAndConstruct.h>
#include <js/CharacterEncoding.h>
#include <js/CompilationAndEvaluation.h>
#include <js/CompileOptions.h>
#include <js/Context.h>
#include <js/Conversions.h>
#include <js/Equality.h>
#include <js/ErrorReport.h>
#include <js/Exception.h>
#include <js/GCAPI.h>
#include <js/GlobalObject.h>
#include <js/Initialization.h>
#include <js/Object.h>
#include <js/Promise.h>
#include <js/PropertyAndElement.h>
#include <js/PropertyDescriptor.h>
#include <js/Proxy.h>
#include <js/Realm.h>
#include <js/SourceText.h>
#include <js/Stack.h>
#include <js/String.h>
#include <js/Symbol.h>
#include <js/Transcoding.h>
#include <js/WeakMap.h>
#include <js/experimental/JSStencil.h>
#include <js/experimental/TypedData.h>
#include <jsapi.h>
#include <jsfriendapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace jsi = facebook::jsi;

namespace screenkit {
namespace spidermonkey {

namespace {

// ---- process-wide ---------------------------------------------------------------

/// What a stencil records it was compiled by. The engine refuses to encode or
/// decode one without a build id; ours is the engine's version and pointer
/// size, since the serialised format follows the engine's source.
bool buildId(JS::BuildIdCharVector* out) {
  const std::string id = std::string("screenkit-mozjs/") + JS_GetImplementationVersion() + "/" +
                         std::to_string(sizeof(void*) * 8);
  return out->append(id.data(), id.size());
}

std::string currentBuildId() {
  JS::BuildIdCharVector id;
  if (!buildId(&id)) return {};
  return std::string(id.begin(), id.end());
}

std::atomic<int> gLiveRuntimes{0};

/// JS_Init once per process, and JS_ShutDown only at exit: the engine cannot be
/// set up again after it, and a process makes runtimes one after another (the
/// conformance suite makes one per test). Without the shutdown the library's
/// own static destructors crash at exit, with its helper threads still running.
void initEngineOnce() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    JS::SetProcessBuildIdOp(buildId);
    ok = JS_Init();
    if (ok) {
      std::atexit([] {
        if (gLiveRuntimes.load() == 0) JS_ShutDown();
      });
    }
  });
  if (!ok) throw std::runtime_error("SpiderMonkey: JS_Init failed");
}

const JSClass kGlobalClass = {"global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};

/// SpiderMonkey has one JSContext per thread, and a second one on a thread that
/// already has one is undefined behaviour rather than an error. So is this
/// runtime: one per thread at a time, refused cleanly (a host runs one runtime
/// on its JS thread; compileToStencil uses a thread of its own).
thread_local bool tRuntimeOnThread = false;

// A stencil file: this magic, a 4-byte little-endian length and the build id it
// was compiled by, zero padding to a multiple of 8, then the engine's own
// serialisation -- aligned, because pinned bytecode is read in place.
constexpr char kStencilMagic[8] = {'S', 'K', 'S', 'T', 'N', 'C', 'L', '1'};
constexpr std::size_t kStencilAlign = 8;

std::size_t alignUp(std::size_t n) { return (n + kStencilAlign - 1) / kStencilAlign * kStencilAlign; }

/// Buffers whose bytecode the engine runs in place: they must live until
/// JS_ShutDown (CompileOptions.h, usePinnedBytecode), which is process exit.
std::mutex gPinnedMutex;
std::vector<std::shared_ptr<const jsi::Buffer>> gPinned;

class SMRuntime;

// ---- JSI handles ----------------------------------------------------------------------

/// One JSI handle. Strong nodes are GC roots (traced every GC, updated when an
/// object moves); weak ones are cleared when their object dies. JSI's handle
/// classes own a node and call `invalidate` when they go -- possibly on another
/// thread, or from a finalizer in the middle of a GC.
struct Node final : jsi::Runtime::PointerValue {
  enum class Kind : std::uint8_t { Value, Id, Weak };

  JS::Heap<JS::Value> value;
  JS::Heap<jsid> id;
  JS::Heap<JSObject*> weak;
  Node* prev = nullptr;
  Node* next = nullptr;
  /// Null once the runtime is gone: an orphan frees itself.
  SMRuntime* runtime = nullptr;
  Kind kind = Kind::Value;

  void invalidate() noexcept override;
};

struct HostFunctionData;

// ---- the runtime ----------------------------------------------------------------------

class SMRuntime final : public jsi::Runtime {
 public:
  explicit SMRuntime(Options options);
  ~SMRuntime() override;

  JSContext* cx() const { return cx_; }

  // -- IRuntime ---------------------------------------------------------------------
  jsi::Value evaluateJavaScript(const std::shared_ptr<const jsi::Buffer>& buffer,
                                const std::string& sourceURL) override;
  std::shared_ptr<const jsi::PreparedJavaScript> prepareJavaScript(
      const std::shared_ptr<const jsi::Buffer>& buffer, std::string sourceURL) override;
  jsi::Value evaluatePreparedJavaScript(const std::shared_ptr<const jsi::PreparedJavaScript>& js) override;
  void queueMicrotask(const jsi::Function& callback) override;
  bool drainMicrotasks(int maxMicrotasksHint = -1) override;
  jsi::Object global() override;
  std::string description() override;
  bool isInspectable() override { return false; }

  PointerValue* cloneSymbol(const PointerValue* pv) override { return clone(pv); }
  PointerValue* cloneBigInt(const PointerValue* pv) override { return clone(pv); }
  PointerValue* cloneString(const PointerValue* pv) override { return clone(pv); }
  PointerValue* cloneObject(const PointerValue* pv) override { return clone(pv); }
  PointerValue* clonePropNameID(const PointerValue* pv) override { return clone(pv); }

  jsi::PropNameID createPropNameIDFromAscii(const char* str, size_t length) override;
  jsi::PropNameID createPropNameIDFromUtf8(const uint8_t* utf8, size_t length) override;
  jsi::PropNameID createPropNameIDFromString(const jsi::String& str) override;
  jsi::PropNameID createPropNameIDFromSymbol(const jsi::Symbol& sym) override;
  std::string utf8(const jsi::PropNameID&) override;
  bool compare(const jsi::PropNameID&, const jsi::PropNameID&) override;

  std::string symbolToString(const jsi::Symbol&) override;

  jsi::BigInt createBigIntFromInt64(int64_t) override;
  jsi::BigInt createBigIntFromUint64(uint64_t) override;
  bool bigintIsInt64(const jsi::BigInt&) override;
  bool bigintIsUint64(const jsi::BigInt&) override;
  uint64_t truncate(const jsi::BigInt&) override;
  jsi::String bigintToString(const jsi::BigInt&, int) override;

  jsi::String createStringFromAscii(const char* str, size_t length) override;
  jsi::String createStringFromUtf8(const uint8_t* utf8, size_t length) override;
  std::string utf8(const jsi::String&) override;
  std::u16string utf16(const jsi::String&) override;
  size_t length(const jsi::String&) override;

  jsi::Object createObject() override;
  jsi::Object createObject(std::shared_ptr<jsi::HostObject> ho) override;
  std::shared_ptr<jsi::HostObject> getHostObject(const jsi::Object&) override;
  jsi::HostFunctionType& getHostFunction(const jsi::Function&) override;

  bool hasNativeState(const jsi::Object&) override;
  std::shared_ptr<jsi::NativeState> getNativeState(const jsi::Object&) override;
  void setNativeState(const jsi::Object&, std::shared_ptr<jsi::NativeState> state) override;

  jsi::Value getProperty(const jsi::Object&, const jsi::PropNameID& name) override;
  jsi::Value getProperty(const jsi::Object&, const jsi::String& name) override;
  bool hasProperty(const jsi::Object&, const jsi::PropNameID& name) override;
  bool hasProperty(const jsi::Object&, const jsi::String& name) override;
  void setPropertyValue(const jsi::Object&, const jsi::PropNameID& name, const jsi::Value& value) override;
  void setPropertyValue(const jsi::Object&, const jsi::String& name, const jsi::Value& value) override;
  void deleteProperty(const jsi::Object&, const jsi::PropNameID& name) override;
  void deleteProperty(const jsi::Object&, const jsi::String& name) override;

  bool isArray(const jsi::Object&) const override;
  bool isArrayBuffer(const jsi::Object&) const override;
  bool isFunction(const jsi::Object&) const override;
  bool isHostObject(const jsi::Object&) const override;
  bool isHostFunction(const jsi::Function&) const override;
  jsi::Array getPropertyNames(const jsi::Object&) override;

  jsi::WeakObject createWeakObject(const jsi::Object&) override;
  jsi::Value lockWeakObject(const jsi::WeakObject&) override;

  jsi::Array createArray(size_t length) override;
  jsi::ArrayBuffer createArrayBuffer(std::shared_ptr<jsi::MutableBuffer> buffer) override;
  size_t size(const jsi::Array&) override;
  size_t size(const jsi::ArrayBuffer&) override;
  uint8_t* data(const jsi::ArrayBuffer&) override;
  bool detached(const jsi::ArrayBuffer&) override;
  jsi::Value getValueAtIndex(const jsi::Array&, size_t i) override;
  void setValueAtIndexImpl(const jsi::Array&, size_t i, const jsi::Value& value) override;

  // The engine's own slots, not the (shadowable) JS properties jsi.cpp reads.
  bool isTypedArray(const jsi::Object&) const override;
  bool isUint8Array(const jsi::Object&) const override;
  jsi::ArrayBuffer buffer(const jsi::TypedArray&) override;
  size_t byteOffset(const jsi::TypedArray&) override;
  size_t byteLength(const jsi::TypedArray&) override;
  size_t length(const jsi::TypedArray&) override;
  jsi::Uint8Array createUint8Array(size_t length) override;
  jsi::Uint8Array createUint8Array(const jsi::ArrayBuffer& buffer, size_t offset, size_t length) override;

  jsi::Function createFunctionFromHostFunction(const jsi::PropNameID& name, unsigned int paramCount,
                                               jsi::HostFunctionType func) override;
  jsi::Value call(const jsi::Function&, const jsi::Value& jsThis, const jsi::Value* args, size_t count) override;
  jsi::Value callAsConstructor(const jsi::Function&, const jsi::Value* args, size_t count) override;

  bool strictEquals(const jsi::Symbol& a, const jsi::Symbol& b) const override;
  bool strictEquals(const jsi::BigInt& a, const jsi::BigInt& b) const override;
  bool strictEquals(const jsi::String& a, const jsi::String& b) const override;
  bool strictEquals(const jsi::Object& a, const jsi::Object& b) const override;
  bool instanceOf(const jsi::Object& o, const jsi::Function& f) override;
  void setExternalMemoryPressure(const jsi::Object&, size_t) override {}

  // -- for the engine callbacks below ------------------------------------------------
  jsi::Value toJsi(const JS::Value& v);
  JS::Value toJS(const jsi::Value& v) const;
  jsi::PropNameID nameOf(jsid id) { return make<jsi::PropNameID>(idNode(id)); }
  /// Turn the pending JS exception into a C++ one. Never returns.
  [[noreturn]] void throwPendingException();
  /// Make whatever a host callback threw the pending JS exception.
  void setPendingFromCurrentException(const char* where);
  void release(Node* node) noexcept;

  static const Node* nodeOf(const jsi::Pointer& p) { return static_cast<const Node*>(getPointerValue(p)); }
  static const Node* nodeOf(const jsi::Value& v) { return static_cast<const Node*>(getPointerValue(v)); }

  static void traceRoots(JSTracer* trc, void* data);
  static void sweepWeak(JSTracer* trc, void* data);
  static void trackRejection(JSContext* cx, bool mutedErrors, JS::HandleObject promise,
                             JS::PromiseRejectionHandlingState state, void* data);

 private:
  class Jobs;

  Node* newNode(Node::Kind kind);
  Node* clone(const PointerValue* pv);
  Node* valueNode(const JS::Value& v);
  Node* idNode(jsid id);
  void freeDeferred();
  JSObject* objectOf(const jsi::Pointer& o) const { return &nodeOf(o)->value.get().toObject(); }
  JSString* stringOf(const jsi::Pointer& s) const { return nodeOf(s)->value.get().toString(); }
  jsid idOf(const jsi::PropNameID& name) const { return nodeOf(name)->id.get(); }
  jsid idOf(const jsi::String& name);
  JSString* newStringFromUtf8(const uint8_t* utf8, size_t length);
  std::string describe(JS::HandleValue value);
  void reportUnhandledRejections();

  Options options_;
  std::thread::id thread_;
  JSContext* cx_ = nullptr;
  JS::PersistentRootedObject* global_ = nullptr;
  JSAutoRealm* realm_ = nullptr;
  std::unique_ptr<Jobs> jobs_;

  // Every live node, for the tracer. Touched only on the runtime's thread.
  Node* head_ = nullptr;
  std::vector<Node*> free_;
  // Nodes released off the runtime's thread, during a GC, or during teardown.
  std::mutex deferredMutex_;
  std::vector<Node*> deferred_;
  std::atomic<bool> hasDeferred_{false};
  bool destroying_ = false;

  JS::Heap<JSObject*> nativeStates_;             // WeakMap: object -> NativeState holder
  JS::Heap<JSObject*> propertyNamesFn_;          // for-in, as JSI defines getPropertyNames
  std::vector<JS::Heap<JSObject*>> unhandled_;  // rejected promises with no handler yet
};

// ---- node pool ------------------------------------------------------------------------

void Node::invalidate() noexcept {
  if (runtime) {
    runtime->release(this);
  } else {
    delete this;  // the runtime has gone; its edges were cleared before it did
  }
}

Node* SMRuntime::newNode(Node::Kind kind) {
  if (hasDeferred_.load(std::memory_order_acquire)) freeDeferred();
  Node* n;
  if (!free_.empty()) {
    n = free_.back();
    free_.pop_back();
  } else {
    n = new Node();
    n->runtime = this;
  }
  n->kind = kind;
  n->prev = nullptr;
  n->next = head_;
  if (head_) head_->prev = n;
  head_ = n;
  return n;
}

void SMRuntime::release(Node* n) noexcept {
  // Off the runtime's thread, inside a GC (a finalizer freeing what a host
  // function captured) or during teardown, the list and the GC edges must not
  // be touched now.
  if (destroying_ || std::this_thread::get_id() != thread_ || JS::RuntimeHeapIsBusy()) {
    std::lock_guard<std::mutex> lock(deferredMutex_);
    deferred_.push_back(n);
    hasDeferred_.store(true, std::memory_order_release);
    return;
  }
  if (n->prev) n->prev->next = n->next;
  else head_ = n->next;
  if (n->next) n->next->prev = n->prev;
  n->prev = n->next = nullptr;
  n->value = JS::UndefinedValue();
  n->id = JS::PropertyKey::Void();
  n->weak = nullptr;
  free_.push_back(n);
}

void SMRuntime::freeDeferred() {
  std::vector<Node*> batch;
  {
    std::lock_guard<std::mutex> lock(deferredMutex_);
    batch.swap(deferred_);
    hasDeferred_.store(false, std::memory_order_release);
  }
  for (Node* n : batch) release(n);
}

Node* SMRuntime::clone(const PointerValue* pv) {
  if (!pv) return nullptr;
  const Node* from = static_cast<const Node*>(pv);
  Node* n = newNode(from->kind);
  switch (from->kind) {
    case Node::Kind::Value: n->value = from->value.get(); break;
    case Node::Kind::Id: n->id = from->id.get(); break;
    case Node::Kind::Weak: n->weak = from->weak.unbarrieredGet(); break;
  }
  return n;
}

Node* SMRuntime::valueNode(const JS::Value& v) {
  Node* n = newNode(Node::Kind::Value);
  n->value = v;
  return n;
}

Node* SMRuntime::idNode(jsid id) {
  Node* n = newNode(Node::Kind::Id);
  n->id = id;
  return n;
}

// ---- microtasks ------------------------------------------------------------------------

/// Promise jobs and queueMicrotask callbacks, in one FIFO the host drains.
class SMRuntime::Jobs final : public JS::JobQueue {
 public:
  explicit Jobs(SMRuntime* runtime) : runtime_(runtime) {}

  JSObject* getIncumbentGlobal(JSContext* cx) override { return JS::CurrentGlobalOrNull(cx); }
  bool enqueuePromiseJob(JSContext*, JS::HandleObject, JS::HandleObject job, JS::HandleObject,
                         JS::HandleObject) override {
    queue.emplace_back(job.get());
    return true;
  }
  void runJobs(JSContext*) override {
    try {
      runtime_->drainMicrotasks(-1);
    } catch (...) {
    }
  }
  bool empty() const override { return queue.empty(); }
  bool isDrainingStopped() const override { return false; }

  std::deque<JS::Heap<JSObject*>> queue;

 private:
  // The debugger's: park the queue while it runs, put it back after.
  class Saved final : public SavedJobQueue {
   public:
    explicit Saved(Jobs* jobs) : jobs_(jobs), saved_(std::move(jobs->queue)) { jobs->queue.clear(); }
    ~Saved() override { jobs_->queue = std::move(saved_); }

   private:
    Jobs* jobs_;
    std::deque<JS::Heap<JSObject*>> saved_;
  };
  js::UniquePtr<SavedJobQueue> saveJobQueue(JSContext*) override { return js::MakeUnique<Saved>(this); }

  SMRuntime* runtime_;
};

// ---- host functions -------------------------------------------------------------------

struct HostFunctionData {
  SMRuntime* runtime;
  jsi::HostFunctionType func;
};

void finalizeHostFunctionData(JS::GCContext*, JSObject* obj) {
  delete JS::GetMaybePtrFromReservedSlot<HostFunctionData>(obj, 0);
}

const JSClassOps kHostFunctionDataOps = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, finalizeHostFunctionData, nullptr, nullptr, nullptr,
};
// Foreground: the std::function it frees may release JSI handles, which has to
// happen on the runtime's thread.
const JSClass kHostFunctionDataClass = {"HostFunctionData",
                                        JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
                                        &kHostFunctionDataOps};

HostFunctionData* hostFunctionData(JSObject* fn) {
  const JS::Value& slot = js::GetFunctionNativeReserved(fn, 0);
  return JS::GetMaybePtrFromReservedSlot<HostFunctionData>(&slot.toObject(), 0);
}

bool callHostFunction(JSContext*, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  HostFunctionData* data = hostFunctionData(&args.callee());
  SMRuntime& rt = *data->runtime;
  constexpr unsigned kInline = 8;
  jsi::Value inlineArgs[kInline];
  std::vector<jsi::Value> heapArgs;
  jsi::Value* jsiArgs = inlineArgs;
  if (argc > kInline) {
    heapArgs.resize(argc);
    jsiArgs = heapArgs.data();
  }
  try {
    for (unsigned i = 0; i < argc; i++) jsiArgs[i] = rt.toJsi(args[i]);
    const jsi::Value thisValue = rt.toJsi(args.thisv());
    const jsi::Value result = data->func(rt, thisValue, jsiArgs, argc);
    args.rval().set(rt.toJS(result));
    return true;
  } catch (...) {
    rt.setPendingFromCurrentException("HostFunction");
    return false;
  }
}

// ---- host objects ---------------------------------------------------------------------

struct HostObjectData {
  SMRuntime* runtime;
  std::shared_ptr<jsi::HostObject> object;
};

/// A proxy whose every property operation asks the jsi::HostObject. Its `get` is
/// authoritative -- a property it does not know reads as whatever it returns,
/// `undefined` included -- as in Hermes and JSC. No `hasPrototype`: with it the
/// engine answers get/set/has from the prototype chain and never calls the traps.
class HostObjectHandler final : public js::BaseProxyHandler {
 public:
  static const char family;
  constexpr HostObjectHandler() : js::BaseProxyHandler(&family) {}

  static HostObjectData* data(JSObject* proxy) {
    return static_cast<HostObjectData*>(js::GetProxyPrivate(proxy).toPrivate());
  }

  bool getOwnPropertyDescriptor(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                                JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) const override {
    JS::RootedValue value(cx);
    if (!read(proxy, id, &value)) return false;
    if (value.isUndefined()) {
      desc.set(mozilla::Nothing());
    } else {
      desc.set(mozilla::Some(JS::PropertyDescriptor::Data(
          value, {JS::PropertyAttribute::Configurable, JS::PropertyAttribute::Enumerable,
                  JS::PropertyAttribute::Writable})));
    }
    return true;
  }
  bool defineProperty(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                      JS::Handle<JS::PropertyDescriptor> desc, JS::ObjectOpResult& result) const override {
    if (desc.hasValue()) {
      JS::RootedValue value(cx, desc.value());
      if (!write(proxy, id, value)) return false;
    }
    return result.succeed();
  }
  bool ownPropertyKeys(JSContext*, JS::HandleObject proxy, JS::MutableHandleIdVector props) const override {
    HostObjectData* d = data(proxy);
    SMRuntime& rt = *d->runtime;
    try {
      const std::vector<jsi::PropNameID> names = d->object->getPropertyNames(rt);
      if (!props.reserve(props.length() + names.size())) return false;
      // A proxy may not report a key twice; a host object may list one twice.
      for (const auto& name : names) {
        const jsid id = SMRuntime::nodeOf(name)->id.get();
        if (std::find(props.begin(), props.end(), id) == props.end()) props.infallibleAppend(id);
      }
      return true;
    } catch (...) {
      rt.setPendingFromCurrentException("HostObject::getPropertyNames");
      return false;
    }
  }
  bool delete_(JSContext*, JS::HandleObject, JS::HandleId, JS::ObjectOpResult& result) const override {
    return result.succeed();
  }
  bool getPrototypeIfOrdinary(JSContext*, JS::HandleObject proxy, bool* isOrdinary,
                              JS::MutableHandleObject protop) const override {
    *isOrdinary = true;
    protop.set(js::GetStaticPrototype(proxy));
    return true;
  }
  bool preventExtensions(JSContext*, JS::HandleObject, JS::ObjectOpResult& result) const override {
    return result.failCantPreventExtensions();
  }
  bool isExtensible(JSContext*, JS::HandleObject, bool* extensible) const override {
    *extensible = true;
    return true;
  }
  bool has(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, bool* bp) const override {
    JS::RootedValue value(cx);
    if (!read(proxy, id, &value)) return false;
    *bp = !value.isUndefined();
    return true;
  }
  bool hasOwn(JSContext* cx, JS::HandleObject proxy, JS::HandleId id, bool* bp) const override {
    return has(cx, proxy, id, bp);
  }
  bool get(JSContext*, JS::HandleObject proxy, JS::HandleValue, JS::HandleId id,
           JS::MutableHandleValue vp) const override {
    return read(proxy, id, vp);
  }
  bool set(JSContext*, JS::HandleObject proxy, JS::HandleId id, JS::HandleValue v, JS::HandleValue,
           JS::ObjectOpResult& result) const override {
    if (!write(proxy, id, v)) return false;
    return result.succeed();
  }
  bool finalizeInBackground(const JS::Value&) const override { return false; }
  void finalize(JS::GCContext*, JSObject* proxy) const override { delete data(proxy); }

 private:
  static bool read(JS::HandleObject proxy, JS::HandleId id, JS::MutableHandleValue vp) {
    HostObjectData* d = data(proxy);
    SMRuntime& rt = *d->runtime;
    try {
      const jsi::Value value = d->object->get(rt, rt.nameOf(id));
      vp.set(rt.toJS(value));
      return true;
    } catch (...) {
      rt.setPendingFromCurrentException("HostObject::get");
      return false;
    }
  }
  static bool write(JS::HandleObject proxy, JS::HandleId id, JS::HandleValue v) {
    HostObjectData* d = data(proxy);
    SMRuntime& rt = *d->runtime;
    try {
      d->object->set(rt, rt.nameOf(id), rt.toJsi(v));
      return true;
    } catch (...) {
      rt.setPendingFromCurrentException("HostObject::set");
      return false;
    }
  }
};
const char HostObjectHandler::family = 0;
const HostObjectHandler kHostObjectHandler;

// ---- native state ---------------------------------------------------------------------

struct NativeStateHolder {
  std::shared_ptr<jsi::NativeState> state;
};

void finalizeNativeState(JS::GCContext*, JSObject* obj) {
  delete JS::GetMaybePtrFromReservedSlot<NativeStateHolder>(obj, 0);
}

const JSClassOps kNativeStateOps = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, finalizeNativeState, nullptr, nullptr, nullptr,
};
const JSClass kNativeStateClass = {"NativeState", JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_FOREGROUND_FINALIZE,
                                   &kNativeStateOps};

// ---- array buffers ----------------------------------------------------------------------

void freeMutableBuffer(void*, void* owner) {
  delete static_cast<std::shared_ptr<jsi::MutableBuffer>*>(owner);
}

// ---- strings ----------------------------------------------------------------------------

/// UTF-8 to UTF-16 with U+FFFD for every malformed sequence (WHATWG's decoder).
std::u16string lossyUtf8ToUtf16(const uint8_t* s, size_t n) {
  std::u16string out;
  out.reserve(n);
  size_t i = 0;
  while (i < n) {
    const uint8_t c = s[i];
    uint32_t cp = 0xFFFD;
    size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else {
      size_t need = 0;
      uint32_t min = 0;
      if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; min = 0x80; }
      else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; min = 0x800; }
      else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; min = 0x10000; }
      size_t k = 0;
      for (; k < need && i + 1 + k < n && (s[i + 1 + k] & 0xC0) == 0x80; k++) cp = (cp << 6) | (s[i + 1 + k] & 0x3F);
      if (need == 0 || k < need || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = 0xFFFD;
        len = need == 0 ? 1 : 1 + k;
      } else {
        len = 1 + need;
      }
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
    i += len;
  }
  return out;
}

// ---- scripts ---------------------------------------------------------------------------

class Prepared final : public jsi::PreparedJavaScript {
 public:
  Prepared(RefPtr<JS::Stencil> stencil, std::string url) : stencil(std::move(stencil)), url(std::move(url)) {}
  RefPtr<JS::Stencil> stencil;
  std::string url;
};

bool stencilHeader(const std::uint8_t* data, std::size_t size, std::string* id, std::size_t* payload) {
  if (size < sizeof(kStencilMagic) + 4 || std::memcmp(data, kStencilMagic, sizeof(kStencilMagic)) != 0) return false;
  const std::uint8_t* p = data + sizeof(kStencilMagic);
  const std::uint32_t idLength = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
  const std::size_t header = alignUp(sizeof(kStencilMagic) + 4 + idLength);
  if (header > size) return false;
  if (id) id->assign(reinterpret_cast<const char*>(p + 4), idLength);
  if (payload) *payload = header;
  return true;
}

// ---- SMRuntime --------------------------------------------------------------------------

SMRuntime::SMRuntime(Options options) : options_(std::move(options)), thread_(std::this_thread::get_id()) {
  if (tRuntimeOnThread) {
    throw std::runtime_error("SpiderMonkey: this thread already has a runtime -- the engine allows one per thread");
  }
  initEngineOnce();
  cx_ = JS_NewContext(JS::DefaultHeapMaxBytes);
  if (!cx_) throw std::runtime_error("SpiderMonkey: JS_NewContext failed");
  tRuntimeOnThread = true;
  gLiveRuntimes++;
  // DefaultHeapMaxBytes (32 MB) is a hard ceiling, not a hint; Firefox lifts it too.
  JS_SetGCParameter(cx_, JSGC_MAX_BYTES, 0xffffffff);
  if (options_.maxNurseryBytes) JS_SetGCParameter(cx_, JSGC_MAX_NURSERY_BYTES, options_.maxNurseryBytes);
  if (options_.incrementalGc) JS_SetGCParameter(cx_, JSGC_INCREMENTAL_GC_ENABLED, 1);
  // Without a quota the engine never checks the native stack, and deep
  // recursion is a segfault rather than a RangeError-style exception.
  JS_SetNativeStackQuota(cx_, options_.stackQuotaBytes);
  if (!JS::InitSelfHostedCode(cx_)) {
    JS_DestroyContext(cx_);
    throw std::runtime_error("SpiderMonkey: InitSelfHostedCode failed");
  }
  if (options_.jit == "off") {
    JS_SetGlobalJitCompilerOption(cx_, JSJITCOMPILER_BASELINE_INTERPRETER_ENABLE, 0);
    JS_SetGlobalJitCompilerOption(cx_, JSJITCOMPILER_BASELINE_ENABLE, 0);
  }
  if (options_.jit != "on") JS_SetGlobalJitCompilerOption(cx_, JSJITCOMPILER_ION_ENABLE, 0);

  jobs_ = std::make_unique<Jobs>(this);
  JS::SetJobQueue(cx_, jobs_.get());
  JS::SetPromiseRejectionTrackerCallback(cx_, &SMRuntime::trackRejection, this);
  JS_AddExtraGCRootsTracer(cx_, &SMRuntime::traceRoots, this);
  JS_AddWeakPointerZonesCallback(cx_, &SMRuntime::sweepWeak, this);

  JS::RealmOptions realmOptions;
  global_ = new JS::PersistentRootedObject(
      cx_, JS_NewGlobalObject(cx_, &kGlobalClass, nullptr, JS::FireOnNewGlobalHook, realmOptions));
  if (!*global_) throw std::runtime_error("SpiderMonkey: JS_NewGlobalObject failed");
  realm_ = new JSAutoRealm(cx_, *global_);
  if (!JS::InitRealmStandardClasses(cx_)) throw std::runtime_error("SpiderMonkey: InitRealmStandardClasses failed");

  nativeStates_ = JS::NewWeakMapObject(cx_);
  const char kNames[] = "(function (o) { var names = []; for (var k in o) names.push(k); return names; })";
  JS::CompileOptions compile(cx_);
  compile.setFileAndLine("jsi:getPropertyNames", 1);
  JS::SourceText<mozilla::Utf8Unit> source;
  JS::RootedValue fn(cx_);
  if (!nativeStates_ || !source.init(cx_, kNames, sizeof(kNames) - 1, JS::SourceOwnership::Borrowed) ||
      !JS::Evaluate(cx_, compile, source, &fn)) {
    throw std::runtime_error("SpiderMonkey: runtime setup failed");
  }
  propertyNamesFn_ = &fn.toObject();
}

SMRuntime::~SMRuntime() {
  destroying_ = true;
  if (jobs_) jobs_->queue.clear();
  unhandled_.clear();
  // Every node still alive belongs to a JSI handle that outlives us. Clear the
  // edges now, while the engine can still take the write barriers, and orphan
  // them: an orphan frees itself when its handle goes. Nodes whose handles
  // already went (the deferred ones) are freed here.
  std::vector<Node*> dead;
  {
    std::lock_guard<std::mutex> lock(deferredMutex_);
    dead.swap(deferred_);
  }
  for (Node* n = head_; n;) {
    Node* next = n->next;
    n->value = JS::UndefinedValue();
    n->id = JS::PropertyKey::Void();
    n->weak = nullptr;
    n->runtime = nullptr;
    n->prev = n->next = nullptr;
    n = next;
  }
  head_ = nullptr;
  for (Node* n : dead) delete n;
  for (Node* n : free_) delete n;
  free_.clear();
  nativeStates_ = nullptr;
  propertyNamesFn_ = nullptr;
  JS_RemoveExtraGCRootsTracer(cx_, &SMRuntime::traceRoots, this);
  JS_RemoveWeakPointerZonesCallback(cx_, &SMRuntime::sweepWeak);
  delete realm_;
  delete global_;
  // The final GC finalizes host functions and objects; handles they captured
  // are orphans by now and free themselves.
  JS_DestroyContext(cx_);
  tRuntimeOnThread = false;
  gLiveRuntimes--;
  std::lock_guard<std::mutex> lock(deferredMutex_);
  for (Node* n : deferred_) delete n;
  deferred_.clear();
}

void SMRuntime::traceRoots(JSTracer* trc, void* data) {
  auto* self = static_cast<SMRuntime*>(data);
  for (Node* n = self->head_; n; n = n->next) {
    if (n->kind == Node::Kind::Value) JS::TraceEdge(trc, &n->value, "jsi value");
    else if (n->kind == Node::Kind::Id) JS::TraceEdge(trc, &n->id, "jsi name");
  }
  JS::TraceEdge(trc, &self->nativeStates_, "jsi native states");
  JS::TraceEdge(trc, &self->propertyNamesFn_, "jsi property names");
  for (auto& p : self->unhandled_) JS::TraceEdge(trc, &p, "unhandled rejection");
  if (self->jobs_) {
    for (auto& job : self->jobs_->queue) JS::TraceEdge(trc, &job, "microtask");
  }
}

void SMRuntime::sweepWeak(JSTracer* trc, void* data) {
  auto* self = static_cast<SMRuntime*>(data);
  for (Node* n = self->head_; n; n = n->next) {
    if (n->kind == Node::Kind::Weak) JS_UpdateWeakPointerAfterGC(trc, &n->weak);
  }
}

void SMRuntime::trackRejection(JSContext*, bool, JS::HandleObject promise, JS::PromiseRejectionHandlingState state,
                               void* data) {
  auto* self = static_cast<SMRuntime*>(data);
  if (state == JS::PromiseRejectionHandlingState::Unhandled) {
    self->unhandled_.emplace_back(promise.get());
    return;
  }
  auto& list = self->unhandled_;
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const JS::Heap<JSObject*>& p) { return p.unbarrieredGet() == promise.get(); }),
             list.end());
}

std::string SMRuntime::describe(JS::HandleValue value) {
  std::string text;
  if (value.isObject()) {
    JS::RootedObject error(cx_, &value.toObject());
    JS::RootedValue stack(cx_);
    if (JS_GetProperty(cx_, error, "stack", &stack) && stack.isString()) {
      JS::RootedString message(cx_, JS::ToString(cx_, value));
      if (message) text = utf8(make<jsi::String>(valueNode(JS::StringValue(message))));
      text += "\n" + utf8(make<jsi::String>(valueNode(stack)));
      JS_ClearPendingException(cx_);
      return text;
    }
  }
  JS::RootedString str(cx_, JS::ToString(cx_, value));
  if (!str) {
    JS_ClearPendingException(cx_);
    return "<a value that cannot be converted to a string>";
  }
  return utf8(make<jsi::String>(valueNode(JS::StringValue(str))));
}

void SMRuntime::reportUnhandledRejections() {
  if (unhandled_.empty()) return;
  std::vector<JS::Heap<JSObject*>> pending;
  pending.swap(unhandled_);
  if (!options_.onUnhandledRejection) return;
  for (auto& p : pending) {
    JS::RootedObject promise(cx_, p.get());
    JS::RootedValue reason(cx_, JS::GetPromiseResult(promise));
    options_.onUnhandledRejection(describe(reason));
  }
}

// ---- values and errors ------------------------------------------------------------------

jsi::Value SMRuntime::toJsi(const JS::Value& v) {
  if (v.isUndefined()) return jsi::Value::undefined();
  if (v.isNull()) return jsi::Value::null();
  if (v.isBoolean()) return jsi::Value(v.toBoolean());
  if (v.isInt32()) return jsi::Value(static_cast<double>(v.toInt32()));
  if (v.isDouble()) return jsi::Value(v.toDouble());
  if (v.isString()) return make<jsi::String>(valueNode(v));
  if (v.isObject()) return make<jsi::Object>(valueNode(v));
  if (v.isSymbol()) return make<jsi::Symbol>(valueNode(v));
  if (v.isBigInt()) return make<jsi::BigInt>(valueNode(v));
  return jsi::Value::undefined();  // magic values never reach JS code
}

JS::Value SMRuntime::toJS(const jsi::Value& v) const {
  if (v.isUndefined()) return JS::UndefinedValue();
  if (v.isNull()) return JS::NullValue();
  if (v.isBool()) return JS::BooleanValue(v.getBool());
  if (v.isNumber()) return JS::NumberValue(v.getNumber());
  return nodeOf(v)->value.get();
}

void SMRuntime::throwPendingException() {
  JS::RootedValue exception(cx_);
  if (!JS_IsExceptionPending(cx_) || !JS_GetPendingException(cx_, &exception)) {
    JS_ClearPendingException(cx_);
    throw jsi::JSINativeException("SpiderMonkey: uncatchable exception (out of memory, or interrupted)");
  }
  JS_ClearPendingException(cx_);
  // Hermes, V8 and JSC say "Maximum call stack size exceeded" where SpiderMonkey
  // says "too much recursion", and hosts match on the former; the C++ side of
  // the error says both. JS code still sees SpiderMonkey's own InternalError.
  if (exception.isObject()) {
    JS::RootedObject error(cx_, &exception.toObject());
    JS::RootedValue message(cx_);
    bool recursion = false;
    if (JS_GetProperty(cx_, error, "message", &message) && message.isString() &&
        JS_StringEqualsAscii(cx_, message.toString(), "too much recursion", &recursion) && recursion) {
      JS_ClearPendingException(cx_);
      // The constructor that runs no JS: the stack is exhausted, and building an
      // Error here would fail the same way, forever.
      throw jsi::JSError(toJsi(exception), "Maximum call stack size exceeded (too much recursion)", "");
    }
    JS_ClearPendingException(cx_);
  }
  throw jsi::JSError(*this, toJsi(exception));
}

void SMRuntime::setPendingFromCurrentException(const char* where) {
  try {
    throw;
  } catch (const jsi::JSError& e) {
    JS::RootedValue value(cx_, toJS(e.value()));
    JS_SetPendingException(cx_, value);
  } catch (const std::exception& e) {
    JS_ReportErrorUTF8(cx_, "Exception in %s: %s", where, e.what());
  } catch (...) {
    JS_ReportErrorUTF8(cx_, "Exception in %s: <unknown>", where);
  }
}

// ---- scripts ---------------------------------------------------------------------------

jsi::Value SMRuntime::evaluateJavaScript(const std::shared_ptr<const jsi::Buffer>& buffer,
                                         const std::string& sourceURL) {
  return evaluatePreparedJavaScript(prepareJavaScript(buffer, sourceURL));
}

std::shared_ptr<const jsi::PreparedJavaScript> SMRuntime::prepareJavaScript(
    const std::shared_ptr<const jsi::Buffer>& buffer, std::string sourceURL) {
  const std::uint8_t* data = buffer->data();
  const std::size_t size = buffer->size();
  JS::CompileOptions options(cx_);
  options.setFileAndLine(sourceURL.c_str(), 1);
  RefPtr<JS::Stencil> stencil;

  std::string id;
  std::size_t payload = 0;
  if (stencilHeader(data, size, &id, &payload)) {
    if (id != currentBuildId()) {
      throw jsi::JSINativeException(sourceURL + " is a stencil compiled by " + id + "; this runtime is " +
                                    currentBuildId() + " -- recompile it, or load the source");
    }
    JS::DecodeOptions decode(options);
    // Run the bytecode where it lies: no copy of it on the heap. The engine may
    // point into the buffer until it shuts down, so the buffer is kept for good.
    const bool aligned = reinterpret_cast<std::uintptr_t>(data + payload) % kStencilAlign == 0;
    if (aligned) {
      decode.borrowBuffer = true;
      decode.usePinnedBytecode = true;
      std::lock_guard<std::mutex> lock(gPinnedMutex);
      gPinned.push_back(buffer);
    }
    JS::TranscodeRange range(data + payload, size - payload);
    JS::Stencil* decoded = nullptr;
    const JS::TranscodeResult result = JS::DecodeStencil(cx_, decode, range, &decoded);
    if (result != JS::TranscodeResult::Ok || !decoded) {
      if (JS_IsExceptionPending(cx_)) throwPendingException();
      throw jsi::JSINativeException(sourceURL + ": the stencil could not be decoded");
    }
    stencil = already_AddRefed<JS::Stencil>(decoded);
  } else {
    JS::SourceText<mozilla::Utf8Unit> source;
    if (!source.init(cx_, reinterpret_cast<const char*>(data), size, JS::SourceOwnership::Borrowed)) {
      throwPendingException();
    }
    stencil = JS::CompileGlobalScriptToStencil(cx_, options, source);
    if (!stencil) throwPendingException();
  }
  return std::make_shared<Prepared>(std::move(stencil), std::move(sourceURL));
}

jsi::Value SMRuntime::evaluatePreparedJavaScript(const std::shared_ptr<const jsi::PreparedJavaScript>& js) {
  const auto* prepared = static_cast<const Prepared*>(js.get());
  JS::CompileOptions options(cx_);
  options.setFileAndLine(prepared->url.c_str(), 1);
  JS::InstantiateOptions instantiate(options);
  JS::RootedScript script(cx_, JS::InstantiateGlobalStencil(cx_, instantiate, prepared->stencil.get()));
  if (!script) throwPendingException();
  JS::RootedValue result(cx_);
  if (!JS_ExecuteScript(cx_, script, &result)) throwPendingException();
  return toJsi(result);
}

void SMRuntime::queueMicrotask(const jsi::Function& callback) {
  jobs_->queue.emplace_back(objectOf(callback));
}

bool SMRuntime::drainMicrotasks(int maxMicrotasksHint) {
  int ran = 0;
  while (!jobs_->queue.empty()) {
    if (maxMicrotasksHint >= 0 && ran >= maxMicrotasksHint) return false;
    JS::RootedValue job(cx_, JS::ObjectValue(*jobs_->queue.front().get()));
    jobs_->queue.pop_front();
    ran++;
    JS::RootedValue result(cx_);
    if (!JS::Call(cx_, JS::UndefinedHandleValue, job, JS::HandleValueArray::empty(), &result)) {
      throwPendingException();  // the job is already off the queue: a second drain resumes after it
    }
  }
  reportUnhandledRejections();
  return true;
}

jsi::Object SMRuntime::global() { return make<jsi::Object>(valueNode(JS::ObjectValue(**global_))); }

std::string SMRuntime::description() { return engineVersion(); }

// ---- names ------------------------------------------------------------------------------

jsid SMRuntime::idOf(const jsi::String& name) {
  JS::RootedString str(cx_, stringOf(name));
  JS::RootedId id(cx_);
  if (!JS_StringToId(cx_, str, &id)) throwPendingException();
  return id;
}

jsi::PropNameID SMRuntime::createPropNameIDFromAscii(const char* str, size_t length) {
  JS::RootedString atom(cx_, JS_AtomizeStringN(cx_, str, length));
  JS::RootedId id(cx_);
  if (!atom || !JS_StringToId(cx_, atom, &id)) throwPendingException();
  return make<jsi::PropNameID>(idNode(id));
}

jsi::PropNameID SMRuntime::createPropNameIDFromUtf8(const uint8_t* utf8, size_t length) {
  JS::RootedString str(cx_, newStringFromUtf8(utf8, length));
  JS::RootedId id(cx_);
  if (!str || !JS_StringToId(cx_, str, &id)) throwPendingException();
  return make<jsi::PropNameID>(idNode(id));
}

jsi::PropNameID SMRuntime::createPropNameIDFromString(const jsi::String& str) {
  return make<jsi::PropNameID>(idNode(idOf(str)));
}

jsi::PropNameID SMRuntime::createPropNameIDFromSymbol(const jsi::Symbol& sym) {
  return make<jsi::PropNameID>(idNode(JS::PropertyKey::Symbol(nodeOf(sym)->value.get().toSymbol())));
}

std::string SMRuntime::utf8(const jsi::PropNameID& name) {
  JS::RootedId id(cx_, idOf(name));
  if (id.isSymbol()) {
    JS::RootedSymbol sym(cx_, id.toSymbol());
    JSString* description = JS::GetSymbolDescription(sym);
    return description ? utf8(make<jsi::String>(valueNode(JS::StringValue(description)))) : std::string();
  }
  JS::RootedValue value(cx_);
  if (!JS_IdToValue(cx_, id, &value)) throwPendingException();
  JS::RootedString str(cx_, JS::ToString(cx_, value));
  if (!str) throwPendingException();
  return utf8(make<jsi::String>(valueNode(JS::StringValue(str))));
}

bool SMRuntime::compare(const jsi::PropNameID& a, const jsi::PropNameID& b) { return idOf(a) == idOf(b); }

std::string SMRuntime::symbolToString(const jsi::Symbol& sym) {
  JS::RootedSymbol symbol(cx_, nodeOf(sym)->value.get().toSymbol());
  JSString* description = JS::GetSymbolDescription(symbol);
  std::string text = description ? utf8(make<jsi::String>(valueNode(JS::StringValue(description)))) : "";
  return "Symbol(" + text + ")";
}

// ---- BigInt -----------------------------------------------------------------------------

jsi::BigInt SMRuntime::createBigIntFromInt64(int64_t v) {
  JS::BigInt* b = JS::NumberToBigInt(cx_, v);
  if (!b) throwPendingException();
  return make<jsi::BigInt>(valueNode(JS::BigIntValue(b)));
}

jsi::BigInt SMRuntime::createBigIntFromUint64(uint64_t v) {
  JS::BigInt* b = JS::NumberToBigInt(cx_, v);
  if (!b) throwPendingException();
  return make<jsi::BigInt>(valueNode(JS::BigIntValue(b)));
}

bool SMRuntime::bigintIsInt64(const jsi::BigInt& b) {
  int64_t out;
  return JS::BigIntFits(nodeOf(b)->value.get().toBigInt(), &out);
}

bool SMRuntime::bigintIsUint64(const jsi::BigInt& b) {
  uint64_t out;
  return JS::BigIntFits(nodeOf(b)->value.get().toBigInt(), &out);
}

uint64_t SMRuntime::truncate(const jsi::BigInt& b) { return JS::ToBigUint64(nodeOf(b)->value.get().toBigInt()); }

jsi::String SMRuntime::bigintToString(const jsi::BigInt& b, int radix) {
  if (radix < 2 || radix > 36) throw jsi::JSINativeException("BigInt radix must be between 2 and 36");
  JS::Rooted<JS::BigInt*> big(cx_, nodeOf(b)->value.get().toBigInt());
  JSString* str = JS::BigIntToString(cx_, big, static_cast<uint8_t>(radix));
  if (!str) throwPendingException();
  return make<jsi::String>(valueNode(JS::StringValue(str)));
}

// ---- strings ----------------------------------------------------------------------------

jsi::String SMRuntime::createStringFromAscii(const char* str, size_t length) {
  JSString* s = JS_NewStringCopyN(cx_, str, length);
  if (!s) throwPendingException();
  return make<jsi::String>(valueNode(JS::StringValue(s)));
}

JSString* SMRuntime::newStringFromUtf8(const uint8_t* utf8, size_t length) {
  JSString* s = JS_NewStringCopyUTF8N(cx_, JS::UTF8Chars(reinterpret_cast<const char*>(utf8), length));
  if (s) return s;
  // SpiderMonkey refuses malformed UTF-8; JSI engines replace each bad sequence
  // with U+FFFD, as a browser's decoder does.
  JS_ClearPendingException(cx_);
  const std::u16string wide = lossyUtf8ToUtf16(utf8, length);
  return JS_NewUCStringCopyN(cx_, wide.data(), wide.size());
}

jsi::String SMRuntime::createStringFromUtf8(const uint8_t* utf8, size_t length) {
  JSString* s = newStringFromUtf8(utf8, length);
  if (!s) throwPendingException();
  return make<jsi::String>(valueNode(JS::StringValue(s)));
}

std::string SMRuntime::utf8(const jsi::String& str) {
  JS::RootedString s(cx_, stringOf(str));
  JSLinearString* linear = JS_EnsureLinearString(cx_, s);
  if (!linear) throwPendingException();
  const size_t length = JS::GetDeflatedUTF8StringLength(linear);
  std::string out(length, '\0');
  JS::DeflateStringToUTF8Buffer(linear, mozilla::Span<char>(out.data(), length));
  return out;
}

std::u16string SMRuntime::utf16(const jsi::String& str) {
  JS::RootedString s(cx_, stringOf(str));
  std::u16string out(JS_GetStringLength(s), u'\0');
  if (!JS_CopyStringChars(cx_, mozilla::Range<char16_t>(out.data(), out.size()), s)) throwPendingException();
  return out;
}

size_t SMRuntime::length(const jsi::String& str) { return JS_GetStringLength(stringOf(str)); }

// ---- objects ----------------------------------------------------------------------------

jsi::Object SMRuntime::createObject() {
  JSObject* obj = JS_NewPlainObject(cx_);
  if (!obj) throwPendingException();
  return make<jsi::Object>(valueNode(JS::ObjectValue(*obj)));
}

jsi::Object SMRuntime::createObject(std::shared_ptr<jsi::HostObject> ho) {
  auto* data = new HostObjectData{this, std::move(ho)};
  JS::RootedValue priv(cx_, JS::PrivateValue(data));
  JSObject* proto = JS::GetRealmObjectPrototype(cx_);
  JSObject* proxy = proto ? js::NewProxyObject(cx_, &kHostObjectHandler, priv, proto) : nullptr;
  if (!proxy) {
    delete data;
    throwPendingException();
  }
  return make<jsi::Object>(valueNode(JS::ObjectValue(*proxy)));
}

std::shared_ptr<jsi::HostObject> SMRuntime::getHostObject(const jsi::Object& obj) {
  return HostObjectHandler::data(objectOf(obj))->object;
}

jsi::HostFunctionType& SMRuntime::getHostFunction(const jsi::Function& fn) {
  return hostFunctionData(objectOf(fn))->func;
}

bool SMRuntime::hasNativeState(const jsi::Object& obj) {
  JS::RootedObject map(cx_, nativeStates_);
  JS::RootedValue key(cx_, JS::ObjectValue(*objectOf(obj)));
  JS::RootedValue holder(cx_);
  if (!JS::GetWeakMapEntry(cx_, map, key, &holder)) throwPendingException();
  return holder.isObject();
}

std::shared_ptr<jsi::NativeState> SMRuntime::getNativeState(const jsi::Object& obj) {
  JS::RootedObject map(cx_, nativeStates_);
  JS::RootedValue key(cx_, JS::ObjectValue(*objectOf(obj)));
  JS::RootedValue holder(cx_);
  if (!JS::GetWeakMapEntry(cx_, map, key, &holder)) throwPendingException();
  if (!holder.isObject()) return nullptr;
  auto* state = JS::GetMaybePtrFromReservedSlot<NativeStateHolder>(&holder.toObject(), 0);
  return state ? state->state : nullptr;
}

void SMRuntime::setNativeState(const jsi::Object& obj, std::shared_ptr<jsi::NativeState> state) {
  JS::RootedObject map(cx_, nativeStates_);
  JS::RootedValue key(cx_, JS::ObjectValue(*objectOf(obj)));
  // A holder even for nullptr: JSI has no way to remove native state, and an
  // object whose state was reset still reports having some.
  JS::RootedObject holder(cx_, JS_NewObject(cx_, &kNativeStateClass));
  if (!holder) throwPendingException();
  JS::SetReservedSlot(holder, 0, JS::PrivateValue(new NativeStateHolder{std::move(state)}));
  JS::RootedValue entry(cx_, JS::ObjectValue(*holder));
  if (!JS::SetWeakMapEntry(cx_, map, key, entry)) throwPendingException();
}

jsi::Value SMRuntime::getProperty(const jsi::Object& obj, const jsi::PropNameID& name) {
  JS::RootedObject o(cx_, objectOf(obj));
  JS::RootedId id(cx_, idOf(name));
  JS::RootedValue value(cx_);
  if (!JS_GetPropertyById(cx_, o, id, &value)) throwPendingException();
  return toJsi(value);
}

jsi::Value SMRuntime::getProperty(const jsi::Object& obj, const jsi::String& name) {
  JS::RootedObject o(cx_, objectOf(obj));
  JS::RootedId id(cx_, idOf(name));
  JS::RootedValue value(cx_);
  if (!JS_GetPropertyById(cx_, o, id, &value)) throwPendingException();
  return toJsi(value);
}

bool SMRuntime::hasProperty(const jsi::Object& obj, const jsi::PropNameID& name) {
  JS::RootedObject o(cx_, objectOf(obj));
  JS::RootedId id(cx_, idOf(name));
  bool found = false;
  if (!JS_HasPropertyById(cx_, o, id, &found)) throwPendingException();
  return found;
}

bool SMRuntime::hasProperty(const jsi::Object& obj, const jsi::String& name) {
  JS::RootedObject o(cx_, objectOf(obj));
  JS::RootedId id(cx_, idOf(name));
  bool found = false;
  if (!JS_HasPropertyById(cx_, o, id, &found)) throwPendingException();
  return found;
}

void SMRuntime::setPropertyValue(const jsi::Object& obj, const jsi::PropNameID& name, const jsi::Value& value) {
  JS::RootedObject o(cx_, objectOf(obj));
  JS::RootedId id(cx_, idOf(name));
  JS::RootedValue v(cx_, toJS(value));
  if (!JS_SetPropertyById(cx_, o, id, v)) throwPendingException();
}

void SMRuntime::setPropertyValue(const jsi::Object& obj, const jsi::String& name, const jsi::Value& value) {
  JS::RootedObject o(cx_, objectOf(obj));
  JS::RootedId id(cx_, idOf(name));
  JS::RootedValue v(cx_, toJS(value));
  if (!JS_SetPropertyById(cx_, o, id, v)) throwPendingException();
}

void SMRuntime::deleteProperty(const jsi::Object& obj, const jsi::PropNameID& name) {
  JS::RootedObject o(cx_, objectOf(obj));
  JS::RootedId id(cx_, idOf(name));
  JS::ObjectOpResult result;
  if (!JS_DeletePropertyById(cx_, o, id, result)) throwPendingException();
  // Strict-mode semantics: a property that cannot be deleted throws a TypeError.
  if (!result.ok()) {
    throw jsi::JSError(*this, createTypeError(jsi::String::createFromUtf8(
                                  *this, "Cannot delete property '" + utf8(name) + "' (it is not configurable)")));
  }
}

void SMRuntime::deleteProperty(const jsi::Object& obj, const jsi::String& name) {
  deleteProperty(obj, createPropNameIDFromString(name));
}

bool SMRuntime::isArray(const jsi::Object& obj) const {
  JS::RootedObject o(cx_, objectOf(obj));
  bool isArray = false;
  if (!JS::IsArrayObject(cx_, o, &isArray)) {
    JS_ClearPendingException(cx_);
    return false;
  }
  return isArray;
}

bool SMRuntime::isArrayBuffer(const jsi::Object& obj) const { return JS::IsArrayBufferObject(objectOf(obj)); }

bool SMRuntime::isFunction(const jsi::Object& obj) const { return JS::IsCallable(objectOf(obj)); }

bool SMRuntime::isHostObject(const jsi::Object& obj) const {
  JSObject* o = objectOf(obj);
  return js::IsProxy(o) && js::GetProxyHandler(o) == &kHostObjectHandler;
}

bool SMRuntime::isHostFunction(const jsi::Function& fn) const {
  return JS_IsNativeFunction(objectOf(fn), callHostFunction);
}

jsi::Array SMRuntime::getPropertyNames(const jsi::Object& obj) {
  JS::RootedValue fn(cx_, JS::ObjectValue(*propertyNamesFn_.get()));
  JS::RootedValueArray<1> args(cx_);
  args[0].setObject(*objectOf(obj));
  JS::RootedValue names(cx_);
  if (!JS::Call(cx_, JS::UndefinedHandleValue, fn, args, &names)) throwPendingException();
  return make<jsi::Object>(valueNode(names)).getArray(*this);
}

jsi::WeakObject SMRuntime::createWeakObject(const jsi::Object& obj) {
  Node* n = newNode(Node::Kind::Weak);
  n->weak = objectOf(obj);
  return make<jsi::WeakObject>(n);
}

jsi::Value SMRuntime::lockWeakObject(const jsi::WeakObject& weak) {
  JSObject* obj = nodeOf(weak)->weak.get();
  if (!obj) return jsi::Value::undefined();
  return make<jsi::Object>(valueNode(JS::ObjectValue(*obj)));
}

// ---- arrays and buffers -------------------------------------------------------------------

jsi::Array SMRuntime::createArray(size_t length) {
  JSObject* arr = JS::NewArrayObject(cx_, length);
  if (!arr) throwPendingException();
  return make<jsi::Array>(valueNode(JS::ObjectValue(*arr)));
}

jsi::ArrayBuffer SMRuntime::createArrayBuffer(std::shared_ptr<jsi::MutableBuffer> buffer) {
  JSObject* ab = nullptr;
  if (!buffer || buffer->size() == 0) {
    ab = JS::NewArrayBuffer(cx_, 0);
  } else {
    // The engine calls freeMutableBuffer when the ArrayBuffer dies, which
    // releases our reference: the bytes live exactly as long as either side
    // needs them.
    auto* owner = new std::shared_ptr<jsi::MutableBuffer>(std::move(buffer));
    mozilla::UniquePtr<void, JS::BufferContentsDeleter> contents((*owner)->data(), {freeMutableBuffer, owner});
    ab = JS::NewExternalArrayBuffer(cx_, (*owner)->size(), std::move(contents));
  }
  if (!ab) throwPendingException();
  return make<jsi::ArrayBuffer>(valueNode(JS::ObjectValue(*ab)));
}

size_t SMRuntime::size(const jsi::Array& arr) {
  JS::RootedObject o(cx_, objectOf(arr));
  uint32_t length = 0;
  if (!JS::GetArrayLength(cx_, o, &length)) throwPendingException();
  return length;
}

size_t SMRuntime::size(const jsi::ArrayBuffer& ab) { return JS::GetArrayBufferByteLength(objectOf(ab)); }

uint8_t* SMRuntime::data(const jsi::ArrayBuffer& ab) {
  JS::RootedObject o(cx_, objectOf(ab));
  // Small buffers keep their bytes inside the object, which a GC may move; JSI
  // callers hold the pointer across calls, so take the bytes out of line first.
  if (!JS::EnsureNonInlineArrayBufferOrView(cx_, o)) throwPendingException();
  bool shared = false;
  JS::AutoCheckCannotGC nogc;
  return JS::GetArrayBufferData(o, &shared, nogc);
}

bool SMRuntime::detached(const jsi::ArrayBuffer& ab) { return JS::IsDetachedArrayBufferObject(objectOf(ab)); }

jsi::Value SMRuntime::getValueAtIndex(const jsi::Array& arr, size_t i) {
  JS::RootedObject o(cx_, objectOf(arr));
  JS::RootedValue value(cx_);
  if (!JS_GetElement(cx_, o, static_cast<uint32_t>(i), &value)) throwPendingException();
  return toJsi(value);
}

void SMRuntime::setValueAtIndexImpl(const jsi::Array& arr, size_t i, const jsi::Value& value) {
  JS::RootedObject o(cx_, objectOf(arr));
  JS::RootedValue v(cx_, toJS(value));
  if (!JS_SetElement(cx_, o, static_cast<uint32_t>(i), v)) throwPendingException();
}

bool SMRuntime::isTypedArray(const jsi::Object& obj) const { return JS_IsTypedArrayObject(objectOf(obj)); }

bool SMRuntime::isUint8Array(const jsi::Object& obj) const {
  JSObject* o = objectOf(obj);
  return JS_IsTypedArrayObject(o) && JS_GetArrayBufferViewType(o) == js::Scalar::Uint8;
}

jsi::ArrayBuffer SMRuntime::buffer(const jsi::TypedArray& ta) {
  JS::RootedObject o(cx_, objectOf(ta));
  bool shared = false;
  JSObject* buffer = JS_GetArrayBufferViewBuffer(cx_, o, &shared);
  if (!buffer) throwPendingException();
  return make<jsi::ArrayBuffer>(valueNode(JS::ObjectValue(*buffer)));
}

size_t SMRuntime::byteOffset(const jsi::TypedArray& ta) { return JS_GetTypedArrayByteOffset(objectOf(ta)); }
size_t SMRuntime::byteLength(const jsi::TypedArray& ta) { return JS_GetTypedArrayByteLength(objectOf(ta)); }
size_t SMRuntime::length(const jsi::TypedArray& ta) { return JS_GetTypedArrayLength(objectOf(ta)); }

jsi::Uint8Array SMRuntime::createUint8Array(size_t length) {
  JSObject* arr = JS_NewUint8Array(cx_, length);
  if (!arr) throwPendingException();
  return make<jsi::Uint8Array>(valueNode(JS::ObjectValue(*arr)));
}

jsi::Uint8Array SMRuntime::createUint8Array(const jsi::ArrayBuffer& buffer, size_t offset, size_t length) {
  JS::RootedObject ab(cx_, objectOf(buffer));
  JSObject* arr = JS_NewUint8ArrayWithBuffer(cx_, ab, offset, static_cast<int64_t>(length));
  if (!arr) throwPendingException();
  return make<jsi::Uint8Array>(valueNode(JS::ObjectValue(*arr)));
}

// ---- functions ----------------------------------------------------------------------------

jsi::Function SMRuntime::createFunctionFromHostFunction(const jsi::PropNameID& name, unsigned int paramCount,
                                                        jsi::HostFunctionType func) {
  const std::string functionName = utf8(name);
  JS::RootedObject holder(cx_, JS_NewObject(cx_, &kHostFunctionDataClass));
  if (!holder) throwPendingException();
  JS::SetReservedSlot(holder, 0, JS::PrivateValue(new HostFunctionData{this, std::move(func)}));
  JSFunction* fn = js::NewFunctionWithReserved(cx_, callHostFunction, paramCount, 0, functionName.c_str());
  if (!fn) throwPendingException();
  JS::RootedObject object(cx_, JS_GetFunctionObject(fn));
  js::SetFunctionNativeReserved(object, 0, JS::ObjectValue(*holder));
  return make<jsi::Function>(valueNode(JS::ObjectValue(*object)));
}

jsi::Value SMRuntime::call(const jsi::Function& fn, const jsi::Value& jsThis, const jsi::Value* args,
                           size_t count) {
  JS::RootedValue callee(cx_, JS::ObjectValue(*objectOf(fn)));
  JS::RootedValue thisValue(cx_, toJS(jsThis));
  JS::RootedValueVector argv(cx_);
  if (!argv.reserve(count)) throw jsi::JSINativeException("SpiderMonkey: out of memory");
  for (size_t i = 0; i < count; i++) argv.infallibleAppend(toJS(args[i]));
  JS::RootedValue result(cx_);
  if (!JS::Call(cx_, thisValue, callee, JS::HandleValueArray(argv), &result)) throwPendingException();
  return toJsi(result);
}

jsi::Value SMRuntime::callAsConstructor(const jsi::Function& fn, const jsi::Value* args, size_t count) {
  JS::RootedValue callee(cx_, JS::ObjectValue(*objectOf(fn)));
  JS::RootedValueVector argv(cx_);
  if (!argv.reserve(count)) throw jsi::JSINativeException("SpiderMonkey: out of memory");
  for (size_t i = 0; i < count; i++) argv.infallibleAppend(toJS(args[i]));
  JS::RootedObject result(cx_);
  if (!JS::Construct(cx_, callee, JS::HandleValueArray(argv), &result)) throwPendingException();
  return toJsi(JS::ObjectValue(*result));
}

bool SMRuntime::strictEquals(const jsi::Symbol& a, const jsi::Symbol& b) const {
  return nodeOf(a)->value.get().toSymbol() == nodeOf(b)->value.get().toSymbol();
}

bool SMRuntime::strictEquals(const jsi::BigInt& a, const jsi::BigInt& b) const {
  JS::RootedValue va(cx_, nodeOf(a)->value.get()), vb(cx_, nodeOf(b)->value.get());
  bool equal = false;
  if (!JS::StrictlyEqual(cx_, va, vb, &equal)) JS_ClearPendingException(cx_);
  return equal;
}

bool SMRuntime::strictEquals(const jsi::String& a, const jsi::String& b) const {
  JS::RootedValue va(cx_, nodeOf(a)->value.get()), vb(cx_, nodeOf(b)->value.get());
  bool equal = false;
  if (!JS::StrictlyEqual(cx_, va, vb, &equal)) JS_ClearPendingException(cx_);
  return equal;
}

bool SMRuntime::strictEquals(const jsi::Object& a, const jsi::Object& b) const { return objectOf(a) == objectOf(b); }

bool SMRuntime::instanceOf(const jsi::Object& o, const jsi::Function& f) {
  JS::RootedObject ctor(cx_, objectOf(f));
  JS::RootedValue value(cx_, JS::ObjectValue(*objectOf(o)));
  bool result = false;
  if (!JS_HasInstance(cx_, ctor, value, &result)) throwPendingException();
  return result;
}

}  // namespace

// ---- public -------------------------------------------------------------------------------

std::unique_ptr<jsi::Runtime> makeSpiderMonkeyRuntime(Options options) {
  return std::make_unique<SMRuntime>(std::move(options));
}

std::string engineVersion() {
  std::string version = JS_GetImplementationVersion();  // "JavaScript-C128.14.0"
  const auto digit = version.find_first_of("0123456789");
  return "SpiderMonkey " + (digit == std::string::npos ? version : version.substr(digit));
}

namespace {

std::vector<std::uint8_t> compileOnThisThread(const std::string& source, const std::string& url, bool eager) {
  SMRuntime runtime(Options{});
  JSContext* cx = runtime.cx();
  JS::CompileOptions options(cx);
  options.setFileAndLine(url.c_str(), 1);
  if (eager) options.setForceFullParse();
  JS::SourceText<mozilla::Utf8Unit> text;
  if (!text.init(cx, source.data(), source.size(), JS::SourceOwnership::Borrowed)) {
    throw std::runtime_error(url + ": out of memory");
  }
  RefPtr<JS::Stencil> stencil = JS::CompileGlobalScriptToStencil(cx, options, text);
  if (!stencil) {
    // "file:line: SyntaxError: ...", from the engine's own report of the error.
    std::string message = url + ": compilation failed";
    JS::ExceptionStack stack(cx);
    if (JS::StealPendingExceptionStack(cx, &stack)) {
      JS::ErrorReportBuilder report(cx);
      if (report.init(cx, stack, JS::ErrorReportBuilder::WithSideEffects)) {
        message = report.toStringResult().c_str();
        if (const JSErrorReport* r = report.report(); r && r->filename) {
          message = std::string(r->filename.c_str()) + ":" + std::to_string(r->lineno) + ": " + message;
        }
      }
    }
    throw std::runtime_error(message);
  }
  JS::TranscodeBuffer encoded;
  if (JS::EncodeStencil(cx, stencil, encoded) != JS::TranscodeResult::Ok) {
    throw std::runtime_error(url + ": SpiderMonkey could not serialise the compiled script");
  }
  const std::string id = currentBuildId();
  std::vector<std::uint8_t> out(kStencilMagic, kStencilMagic + sizeof(kStencilMagic));
  const auto length = static_cast<std::uint32_t>(id.size());
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(length >> shift));
  out.insert(out.end(), id.begin(), id.end());
  out.resize(alignUp(out.size()), 0);
  out.insert(out.end(), encoded.begin(), encoded.end());
  return out;
}

}  // namespace

std::vector<std::uint8_t> compileToStencil(const std::string& source, const std::string& url, bool eager) {
  // A runtime of its own, on a thread of its own: the caller's thread may
  // already have one.
  std::vector<std::uint8_t> out;
  std::exception_ptr error;
  std::thread([&] {
    try {
      out = compileOnThisThread(source, url, eager);
    } catch (...) {
      error = std::current_exception();
    }
  }).join();
  if (error) std::rethrow_exception(error);
  return out;
}

bool looksLikeStencil(const std::uint8_t* data, std::size_t size) {
  return stencilHeader(data, size, nullptr, nullptr);
}

bool isLoadableStencil(const std::uint8_t* data, std::size_t size) {
  std::string id;
  return stencilHeader(data, size, &id, nullptr) && id == currentBuildId();
}

}  // namespace spidermonkey
}  // namespace screenkit
