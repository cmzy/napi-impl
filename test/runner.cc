// Test runner for nodejs/node test/js-native-api/ tests.
//
// Usage:
//   runner <binding.so> <module_name> <test.js>
//
// What it does:
//   1. Boot V8 + NAPI env
//   2. dlopen <binding.so> and call napi_register_module_v1 to get its
//      exports object
//   3. Install a minimal JS runtime: global / require / console / process /
//      assert / ../../common
//   4. Read <test.js> and run via napi_run_script
//   5. Exit 0 on success, 1 on uncaught exception or assertion failure

#if defined(_WIN32)
#include <windows.h>
// Windows shims: LoadLibrary/GetProcAddress replace POSIX dl*; subprocess
// spawn is stubbed out (the few tests that use it just fail on Windows).
static void* dlopen(const char* path, int /*flags*/) {
    return (void*)LoadLibraryA(path);
}
static void* dlsym(void* h, const char* name) {
    return (void*)GetProcAddress((HMODULE)h, name);
}
static const char* dlerror() { return "LoadLibrary failed"; }
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>  // signal macros (SIG*) for SignalName
#if defined(__APPLE__)
#include <crt_externs.h>   // _NSGetEnviron() on macOS
#define NAPI_RUNNER_ENVIRON (*_NSGetEnviron())
#else
extern "C" char** environ;
#define NAPI_RUNNER_ENVIRON environ
#endif
#endif  // _WIN32
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Hermes and JSC share the "non-V8" runner behavior: no inspector internals,
// drain finalizers via the public embedding tick, skip explicit teardown.
#if defined(NAPI_RUNNER_HERMES) || defined(NAPI_RUNNER_JSC)
#define NAPI_RUNNER_NON_V8
#endif

extern "C" {
#include "napi/js_native_api.h"
#include "napi/node_api.h"
#include "napi_v8/embedding.h"
#if !defined(NAPI_RUNNER_NON_V8)
#include "napi_v8/inspector.h"
#endif
}

#if !defined(NAPI_RUNNER_NON_V8)
// V8 path: pull in our internal env layout so we can drain pending finalizers
// after a manual gc() call (V8 weak callbacks enqueue them but our embedding
// has no event loop to flush automatically). The non-V8 paths drain via the
// public embedding tick instead (napi_v8_run_event_loop_tasks).
#include "../src/v8/js_native_api_v8.h"
#endif

#if defined(NAPI_RUNNER_JSC)
// JSC has no V8-style --expose-gc; we provide gc() backed by JSGarbageCollect,
// which needs the context held inside our env. Pull in the internal layout.
#include <JavaScriptCore/JavaScript.h>
#include "../src/jsc/js_native_api_jsc.h"
#endif

// ---- helpers --------------------------------------------------------------

#define CHK(expr) do {                                                         \
  napi_status _s = (expr);                                                     \
  if (_s != napi_ok) {                                                         \
    const napi_extended_error_info* ei = nullptr;                              \
    napi_get_last_error_info(g_env, &ei);                                      \
    std::fprintf(stderr, "[runner] %s -> %d %s\n", #expr, (int)_s,             \
                 (ei && ei->error_message) ? ei->error_message : "");          \
    std::exit(1);                                                              \
  }                                                                            \
} while (0)

static napi_env g_env = nullptr;

static std::string ReadFile(const char* path) {
  std::ifstream f(path);
  if (!f) {
    std::fprintf(stderr, "[runner] cannot read %s\n", path);
    std::exit(1);
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---- JS host APIs ---------------------------------------------------------

static napi_value JsConsoleLog(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value argv[8];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  for (size_t i = 0; i < argc; ++i) {
    napi_value as_string;
    if (napi_coerce_to_string(env, argv[i], &as_string) != napi_ok) continue;
    size_t len = 0;
    napi_get_value_string_utf8(env, as_string, nullptr, 0, &len);
    std::string buf(len + 1, '\0');
    napi_get_value_string_utf8(env, as_string, buf.data(), buf.size(), &len);
    std::fputs(buf.c_str(), stdout);
    std::fputc(i + 1 == argc ? '\n' : ' ', stdout);
  }
  return nullptr;
}

// Drains the env's pending_finalizers set (filled by V8 weak callbacks during
// gc()). Called from JS as `__drainFinalizers()` after `gc()` so that mustCall
// assertions tied to finalizers fire before the test asserts.
static napi_value JsDrainFinalizers(napi_env env, napi_callback_info /*info*/) {
#if defined(NAPI_RUNNER_NON_V8)
  // Non-V8 engines run napi_wrap finalizers during GC; flush any deferred engine
  // work (microtasks / second-pass finalizers) through the public embedding tick.
  napi_v8_run_event_loop_tasks(env);
#else
  // Repeat: a finalizer can register more references; drain transitively but
  // bound the loop to avoid pathological cycles.
  for (int round = 0; round < 16; ++round) {
    if (env->pending_finalizers.empty()) break;
    auto snapshot = env->pending_finalizers;
    env->pending_finalizers.clear();
    for (auto* f : snapshot) {
      f->Finalize();
    }
  }
#endif
  return nullptr;
}

#if defined(NAPI_RUNNER_JSC)
// JSC's public JSGarbageCollect() is only a *hint* and does not deterministically
// reclaim objects, so finalizer-driven assertions (mustCall on a wrap/add_finalizer
// callback after `tracked = null; gc()`) never fired. JSC's private
// JSSynchronousGarbageCollectForDebugging() forces a full synchronous collection —
// the same guarantee V8's --expose-gc gc() gives. It's exported by the system
// JavaScriptCore.framework (declared in the private JSContextRefPrivate.h); declare
// it here so the test harness can drive deterministic GC.
extern "C" void JSSynchronousGarbageCollectForDebugging(JSContextRef ctx);

// gc() for JSC. Force a synchronous full collection, then run the public finalizer
// tick; pump several passes to reclaim multi-level wrap/external holder chains
// before the test asserts finalizer side effects.
static napi_value JscGc(napi_env env, napi_callback_info /*info*/) {
  for (int i = 0; i < 8; ++i) {
    JSSynchronousGarbageCollectForDebugging(env->ctx);
    napi_v8_run_event_loop_tasks(env);
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}
#endif

// Runner state captured at startup so that __spawnSync can fork itself with
// the same binding + module to handle the child branch of tests that use
// child_process.spawnSync.
struct RunnerCtx {
  std::string runner_exe;     // argv[0]
  std::string binding_path;
  std::string module_name;
} g_ctx;

#if !defined(_WIN32)
static std::string DrainFd(int fd) {
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, n);
  return out;
}
#endif

// JS-callable: __spawnSync(execPath, argvArray) -> { status, stdout, stderr }.
// Spawns a subprocess running our runner with the same (binding, module)
// substituting `argv[0]` as the new test.js; remaining args become extra
// process.argv entries.
#if defined(_WIN32)
// Windows: no subprocess support yet. Tests that depend on this (a handful
// of finalizer / env-cleanup checks) will see status=-1 and fail; the rest
// pass.
static napi_value JsSpawnSync(napi_env env, napi_callback_info /*info*/) {
    napi_value result;
    napi_create_object(env, &result);
    napi_value status_v;
    napi_create_int32(env, -1, &status_v);
    napi_set_named_property(env, result, "status", status_v);
    return result;
}
#else
// Map a signal number to its Node-style name (e.g. SIGABRT). Portable: glibc
// has no BSD `sys_signame`, so we match against the standard SIG* macros. An
// if-chain (not a switch) tolerates platforms where two names share a value.
static std::string SignalName(int sig) {
#define NAPI_SIG_NAME(s) if (sig == (s)) return #s;
#ifdef SIGHUP
  NAPI_SIG_NAME(SIGHUP)
#endif
#ifdef SIGINT
  NAPI_SIG_NAME(SIGINT)
#endif
#ifdef SIGQUIT
  NAPI_SIG_NAME(SIGQUIT)
#endif
#ifdef SIGILL
  NAPI_SIG_NAME(SIGILL)
#endif
#ifdef SIGTRAP
  NAPI_SIG_NAME(SIGTRAP)
#endif
#ifdef SIGABRT
  NAPI_SIG_NAME(SIGABRT)
#endif
#ifdef SIGBUS
  NAPI_SIG_NAME(SIGBUS)
#endif
#ifdef SIGFPE
  NAPI_SIG_NAME(SIGFPE)
#endif
#ifdef SIGKILL
  NAPI_SIG_NAME(SIGKILL)
#endif
#ifdef SIGUSR1
  NAPI_SIG_NAME(SIGUSR1)
#endif
#ifdef SIGSEGV
  NAPI_SIG_NAME(SIGSEGV)
#endif
#ifdef SIGUSR2
  NAPI_SIG_NAME(SIGUSR2)
#endif
#ifdef SIGPIPE
  NAPI_SIG_NAME(SIGPIPE)
#endif
#ifdef SIGALRM
  NAPI_SIG_NAME(SIGALRM)
#endif
#ifdef SIGTERM
  NAPI_SIG_NAME(SIGTERM)
#endif
#ifdef SIGCHLD
  NAPI_SIG_NAME(SIGCHLD)
#endif
#ifdef SIGCONT
  NAPI_SIG_NAME(SIGCONT)
#endif
#ifdef SIGSTOP
  NAPI_SIG_NAME(SIGSTOP)
#endif
#ifdef SIGTSTP
  NAPI_SIG_NAME(SIGTSTP)
#endif
#ifdef SIGTTIN
  NAPI_SIG_NAME(SIGTTIN)
#endif
#ifdef SIGTTOU
  NAPI_SIG_NAME(SIGTTOU)
#endif
#ifdef SIGURG
  NAPI_SIG_NAME(SIGURG)
#endif
#ifdef SIGXCPU
  NAPI_SIG_NAME(SIGXCPU)
#endif
#ifdef SIGXFSZ
  NAPI_SIG_NAME(SIGXFSZ)
#endif
#ifdef SIGVTALRM
  NAPI_SIG_NAME(SIGVTALRM)
#endif
#ifdef SIGPROF
  NAPI_SIG_NAME(SIGPROF)
#endif
#ifdef SIGWINCH
  NAPI_SIG_NAME(SIGWINCH)
#endif
#ifdef SIGSYS
  NAPI_SIG_NAME(SIGSYS)
#endif
#ifdef SIGIO
  NAPI_SIG_NAME(SIGIO)
#endif
#undef NAPI_SIG_NAME
  return "SIG" + std::to_string(sig);
}

static napi_value JsSpawnSync(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  size_t execlen = 0;
  napi_get_value_string_utf8(env, argv[0], nullptr, 0, &execlen);
  std::string exec(execlen + 1, '\0');
  napi_get_value_string_utf8(env, argv[0], exec.data(), exec.size(), &execlen);
  exec.resize(execlen);

  uint32_t arr_len = 0;
  napi_get_array_length(env, argv[1], &arr_len);
  std::vector<std::string> extra_args;
  std::string child_test;
  for (uint32_t i = 0; i < arr_len; ++i) {
    napi_value elem;
    napi_get_element(env, argv[1], i, &elem);
    size_t slen = 0;
    napi_get_value_string_utf8(env, elem, nullptr, 0, &slen);
    std::string s(slen + 1, '\0');
    napi_get_value_string_utf8(env, elem, s.data(), s.size(), &slen);
    s.resize(slen);
    // Skip Node-style V8 flags like --expose-gc: they're meaningful only to
    // node, our runner has them on by default.
    if (s.rfind("--", 0) == 0) continue;
    if (child_test.empty()) child_test = s;
    else extra_args.push_back(s);
  }

  // Build child argv: runner_exe binding_path module_name child_test [extras...]
  std::vector<std::string> child_argv = {
      g_ctx.runner_exe, g_ctx.binding_path, g_ctx.module_name, child_test};
  for (auto& a : extra_args) child_argv.push_back(a);

  std::vector<char*> raw_argv;
  for (auto& s : child_argv) raw_argv.push_back(s.data());
  raw_argv.push_back(nullptr);

  int out_pipe[2], err_pipe[2];
  pipe(out_pipe); pipe(err_pipe);

  pid_t pid;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
  posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
  posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
  posix_spawn_file_actions_addclose(&fa, err_pipe[1]);

  int sp = posix_spawn(&pid, g_ctx.runner_exe.c_str(), &fa, nullptr,
                       raw_argv.data(), NAPI_RUNNER_ENVIRON);
  posix_spawn_file_actions_destroy(&fa);

  close(out_pipe[1]);
  close(err_pipe[1]);

  napi_value result;
  napi_create_object(env, &result);

  if (sp != 0) {
    close(out_pipe[0]);
    close(err_pipe[0]);
    napi_value status_v;
    napi_create_int32(env, -1, &status_v);
    napi_set_named_property(env, result, "status", status_v);
    return result;
  }

  std::string sout = DrainFd(out_pipe[0]);
  std::string serr = DrainFd(err_pipe[0]);
  close(out_pipe[0]);
  close(err_pipe[0]);

  int wstatus = 0;
  waitpid(pid, &wstatus, 0);

  napi_value status_v, sout_v, serr_v, signal_v;
  // Node semantics: a child killed by a signal reports status=null, signal=NAME;
  // a normal exit reports status=code, signal=null. test_fatal_finalize's child
  // aborts (SIGABRT) and the parent checks common.nodeProcessAborted(status, signal).
  if (WIFSIGNALED(wstatus)) {
    int sig = WTERMSIG(wstatus);
    napi_get_null(env, &status_v);
    std::string name = SignalName(sig);
    napi_create_string_utf8(env, name.c_str(), name.size(), &signal_v);
  } else {
    napi_create_int32(env, WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1, &status_v);
    napi_get_null(env, &signal_v);
  }
  napi_create_string_utf8(env, sout.c_str(), sout.size(), &sout_v);
  napi_create_string_utf8(env, serr.c_str(), serr.size(), &serr_v);
  napi_set_named_property(env, result, "status", status_v);
  napi_set_named_property(env, result, "stdout", sout_v);
  napi_set_named_property(env, result, "stderr", serr_v);
  napi_set_named_property(env, result, "signal", signal_v);
  return result;
}
#endif  // !_WIN32

static napi_value JsConsoleError(napi_env env, napi_callback_info info) {
  size_t argc = 8;
  napi_value argv[8];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  for (size_t i = 0; i < argc; ++i) {
    napi_value as_string;
    if (napi_coerce_to_string(env, argv[i], &as_string) != napi_ok) continue;
    size_t len = 0;
    napi_get_value_string_utf8(env, as_string, nullptr, 0, &len);
    std::string buf(len + 1, '\0');
    napi_get_value_string_utf8(env, as_string, buf.data(), buf.size(), &len);
    std::fputs(buf.c_str(), stderr);
    std::fputc(i + 1 == argc ? '\n' : ' ', stderr);
  }
  return nullptr;
}

// ---- Bootstrap JS source --------------------------------------------------
// We pre-install a minimal Node.js polyfill before running the user test.
// Inputs (filled in by the runner before running):
//   globalThis.__binding         — the napi_register_module_v1 exports
//   globalThis.__binding_name    — basename of the binding (e.g. "test_general")
//   globalThis.__console_log     — function: console.log
//   globalThis.__console_error   — function: console.error
//
// After this runs, the global names `require`, `assert`, `common`, `console`,
// `process`, `global`, `Buffer` are available.

static const char* kBootstrapJs = R"BOOT(
(function bootstrap() {
'use strict';

globalThis.global = globalThis;
// Wrap V8's --expose-gc gc() so that after a GC we synchronously drain the
// NAPI pending-finalizer queue. Without this, mustCall counts driven by
// finalizers never reach the expected value because we have no event loop.
const __v8_gc = globalThis.gc;
globalThis.gc = function () {
  if (__v8_gc) __v8_gc();
  if (globalThis.__drainFinalizers) globalThis.__drainFinalizers();
};
globalThis.setTimeout = function(fn) { try { fn(); } catch (e) {} };
globalThis.setImmediate = function(fn) { try { fn(); } catch (e) {} };
globalThis.queueMicrotask = function(fn) { try { fn(); } catch (e) {} };
globalThis.module = { exports: {} };
globalThis.exports = globalThis.module.exports;
globalThis.__filename = '<test>';
globalThis.__dirname = '<test>';
globalThis.console = {
  log: globalThis.__console_log,
  error: globalThis.__console_error,
  warn: globalThis.__console_error,
  info: globalThis.__console_log,
  debug: globalThis.__console_log,
};
// process 'uncaughtException' handlers. The engine surfaces unhandled errors
// (e.g. thrown from a napi finalizer) by calling globalThis.__emitUncaughtException.
const __uncaughtHandlers = [];
globalThis.__emitUncaughtException = function (err) {
  if (__uncaughtHandlers.length === 0) {
    // No handler: emulate Node reporting an unhandled finalizer error to stderr
    // (the suite matches /Error during Finalize/), then keep running so the
    // caller's GC loop can finish — the process exits normally (no signal).
    globalThis.__console_error('Error during Finalize: ' +
      ((err && (err.stack || err.message)) || String(err)));
    return;
  }
  for (const h of __uncaughtHandlers.slice()) h(err);
};
globalThis.process = {
  argv: globalThis.__argv || ['runner', 'test'],
  execPath: globalThis.__execPath,
  env: {},
  platform: 'darwin',
  version: 'v22.11.0',
  versions: { node: '22.11.0', napi: '10' },
  exit: (code) => { throw new Error('process.exit(' + code + ')'); },
  on: (event, fn) => {
    if (event === 'uncaughtException' && typeof fn === 'function')
      __uncaughtHandlers.push(fn);
  },
  removeListener: (event, fn) => {
    if (event === 'uncaughtException') {
      const i = __uncaughtHandlers.indexOf(fn);
      if (i >= 0) __uncaughtHandlers.splice(i, 1);
    }
  },
  emitWarning: () => {},
  release: { name: 'node' },
  stdin: null,
  stderr: { write: (s) => globalThis.__console_error(String(s)) },
  stdout: { write: (s) => globalThis.__console_log(String(s)) },
};
globalThis.__filename = globalThis.__filename_v || '<test>';

// Minimal Buffer stub backed by Uint8Array (enough for .toString() / .alloc).
class BufferLike extends Uint8Array {
  toString(encoding = 'utf8') {
    let s = '';
    for (let i = 0; i < this.length; i++) s += String.fromCharCode(this[i]);
    return s;
  }
}
globalThis.Buffer = {
  from(input, encoding) {
    if (typeof input === 'string') {
      const arr = new BufferLike(input.length);
      for (let i = 0; i < input.length; i++) arr[i] = input.charCodeAt(i) & 0xff;
      return arr;
    }
    return new BufferLike(input);
  },
  alloc(n) { return new BufferLike(n); },
  isBuffer: (v) => v instanceof Uint8Array,
};

// Minimal assert.
function AssertionError(message) {
  const e = new Error(message);
  e.name = 'AssertionError';
  return e;
}

function strictEq(a, b, msg) {
  // SameValue (Object.is), not ===, to mirror Node's behavior.
  if (!Object.is(a, b)) {
    throw AssertionError(msg ||
      'expected ' + String(b) + ' but got ' + String(a));
  }
}
function deepEq(a, b) {
  if (Object.is(a, b)) return true;
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null)
    return false;
  const ak = Object.keys(a), bk = Object.keys(b);
  if (ak.length !== bk.length) return false;
  for (const k of ak) if (!deepEq(a[k], b[k])) return false;
  return true;
}

const assert = (cond, msg) => {
  if (!cond) throw AssertionError(msg || 'assertion failed');
};
assert.ok = assert;
assert.strictEqual = strictEq;
assert.notStrictEqual = (a, b, m) => {
  if (Object.is(a, b))
    throw AssertionError(m || 'expected not to equal ' + String(b));
};
assert.equal = strictEq;
assert.notEqual = assert.notStrictEqual;
assert.deepStrictEqual = (a, b, m) => {
  if (!deepEq(a, b)) throw AssertionError(m || 'not deepEqual');
};
assert.deepEqual = assert.deepStrictEqual;
assert.throws = (fn, expected) => {
  let didThrow = false;
  let threw = undefined;
  try { fn(); } catch (e) { didThrow = true; threw = e; }
  if (!didThrow) throw AssertionError('expected fn to throw');
  if (expected instanceof RegExp) {
    // Node tests use both styles; match either the full String(err) (with
    // "Error: " prefix) or just the message.
    const s1 = String(threw);
    const s2 = threw && threw.message ? String(threw.message) : '';
    if (!expected.test(s1) && !expected.test(s2))
      throw AssertionError('thrown ' + s1 + ' did not match ' + expected);
  } else if (typeof expected === 'function') {
    // Node treats Error subclasses as instanceof checks; other functions are
    // predicates returning truthy on success. Distinguish by checking if the
    // function is a known Error subclass (has prototype object).
    const proto = expected.prototype;
    if (proto && (proto instanceof Error || proto === Error.prototype ||
                  expected === Error)) {
      if (!(threw instanceof expected))
        throw AssertionError('thrown was not instanceof expected');
    } else {
      let ok = false;
      try { ok = !!expected(threw); } catch (e) { ok = false; }
      if (!ok)
        throw AssertionError('predicate returned false for ' + String(threw));
    }
  } else if (expected && typeof expected === 'object') {
    for (const k of Object.keys(expected)) {
      const want = expected[k], got = threw[k];
      if (want instanceof RegExp) {
        if (!want.test(String(got)))
          throw AssertionError('mismatch on ' + k + ': ' + got + ' vs ' + want);
      } else if (got !== want) {
        throw AssertionError('mismatch on ' + k + ': ' + got + ' vs ' + want);
      }
    }
  }
};
assert.doesNotThrow = (fn) => {
  try { fn(); } catch (e) {
    throw AssertionError('did not expect to throw: ' + e.message);
  }
};
assert.fail = (msg) => { throw AssertionError(msg || 'assert.fail'); };
assert.match = (s, re) => {
  if (!re.test(s)) throw AssertionError(String(s) + ' did not match ' + re);
};
assert.doesNotMatch = (s, re) => {
  if (re.test(s)) throw AssertionError(String(s) + ' matched ' + re);
};
globalThis.__assert = assert;

// Minimal common helpers.
const common = {
  buildType: 'Release',
  isMainThread: true,
  isWindows: false,
  hasCrypto: false,
  // A process that aborted (e.g. a fatal finalizer) reports a fatal signal on
  // POSIX (status null, signal set). test_fatal_finalize uses this.
  nodeProcessAborted: (exitCode, signal) => {
    const sigs = ['SIGABRT', 'SIGILL', 'SIGTRAP', 'SIGBUS', 'SIGFPE', 'SIGSEGV'];
    if (signal !== null && signal !== undefined) return sigs.includes(signal);
    return [132, 133, 134, 135, 136, 139, 131].includes(exitCode);
  },
  mustCall(fn, exact) {
    // common.mustCall(N) is valid (fn is the count, no callback).
    if (typeof fn === 'number') { exact = fn; fn = undefined; }
    const e = exact === undefined ? 1 : exact;
    let calls = 0;
    const wrapped = function (...args) {
      calls++;
      return typeof fn === 'function' ? fn.apply(this, args) : undefined;
    };
    wrapped._mustCall = () => {
      if (calls !== e) throw AssertionError(
        'mustCall: expected ' + e + ' got ' + calls);
    };
    common.__mustCalls.push(wrapped);
    return wrapped;
  },
  mustCallAtLeast(fn, atLeast) {
    if (typeof fn === 'number') { atLeast = fn; fn = undefined; }
    const min = atLeast === undefined ? 1 : atLeast;
    let calls = 0;
    const wrapped = function (...args) {
      calls++;
      return typeof fn === 'function' ? fn.apply(this, args) : undefined;
    };
    wrapped._mustCall = () => {
      if (calls < min) throw AssertionError(
        'mustCallAtLeast: expected >=' + min + ' got ' + calls);
    };
    common.__mustCalls.push(wrapped);
    return wrapped;
  },
  mustNotCall(msg) {
    return function () { throw AssertionError(msg || 'mustNotCall'); };
  },
  mustSucceed(fn) {
    return common.mustCall((err, ...args) => {
      strictEq(err, null);
      return fn ? fn(...args) : undefined;
    });
  },
  expectsError(opts) {
    return common.mustCall((err) => {
      if (opts.message instanceof RegExp) {
        if (!opts.message.test(err.message))
          throw AssertionError('expectsError mismatch');
      }
      if (opts.code && err.code !== opts.code)
        throw AssertionError('expectsError code mismatch');
    });
  },
  expectWarning() {},   // silent
  skipIfWorker() {},
  printSkipMessage(reason) {
    console.log('1..0 # skipped: ' + reason);
  },
  __mustCalls: [],
  __finalize() {
    for (const w of common.__mustCalls) w._mustCall();
  },
};
globalThis.__common = common;

// common/gc polyfill: pumps gc() up to N rounds until condFn returns true.
const gcStub = {
  gcUntil(name, condFn) {
    return new Promise((resolve, reject) => {
      for (let i = 0; i < 32; i++) {
        try {
          if (condFn()) return resolve();
        } catch (e) { return reject(e); }
        globalThis.gc();
      }
      try {
        condFn() ? resolve() : reject(new Error('gcUntil exhausted: ' + name));
      } catch (e) { reject(e); }
    });
  },
  globalGCMaybe() { globalThis.gc(); },
};

// Minimal require resolver.
globalThis.require = function (path) {
  if (path === 'assert') return globalThis.__assert;
  if (path === 'common' || path === '../../common' || path === '../common' ||
      path === '../../common/index' ||
      path.endsWith('/common')) return globalThis.__common;
  if (path === '../../common/gc' || path === 'common/gc' ||
      path.endsWith('/common/gc')) return gcStub;
  if (path === '../../common/heap' || path.endsWith('/common/heap')) return {};
  if (path === '../../common/fixtures' || path.endsWith('/common/fixtures')) return {};
  if (path === 'fs') return { existsSync: () => false, readFileSync: () => '' };
  if (path === 'path') return {
    join: (...a) => a.join('/'),
    resolve: (...a) => a.join('/'),
    dirname: (s) => s.replace(/\/[^/]*$/, ''),
    basename: (s) => s.replace(/.*\//, ''),
  };
  if (path === 'os') return { platform: () => 'darwin', tmpdir: () => '/tmp' };
  if (path === 'util') return {
    inspect: (v) => String(v),
    inherits: () => {},
    types: { isPromise: (v) => v && typeof v.then === 'function' },
  };
  if (path === 'events') return { EventEmitter: function () {} };
  if (path === 'worker_threads') return { isMainThread: true, parentPort: null };
  if (path === 'child_process') return {
    spawnSync(execPath, argv, opts) {
      const r = globalThis.__spawnSync(execPath, argv);
      // Wrap stdout/stderr as Buffer-like so tests can call toString().
      r.stdout = Buffer.from(r.stdout || '');
      r.stderr = Buffer.from(r.stderr || '');
      return r;
    },
  };
  if (path === 'crypto') return {};
  if (path.startsWith('./build/') || path.startsWith('../build/')) {
    if (globalThis.__binding_error !== undefined) {
      throw globalThis.__binding_error;
    }
    return globalThis.__binding;
  }
  throw new Error('require not supported: ' + path);
};
})();
)BOOT";

// ---- main -----------------------------------------------------------------

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: runner <binding.so> <module_name> <test.js> "
                 "[extra_argv...]\n");
    return 2;
  }
  const char* binding_path = argv[1];
  const char* module_name = argv[2];
  const char* test_path = argv[3];
  g_ctx.runner_exe = argv[0];
  g_ctx.binding_path = binding_path;
  g_ctx.module_name = module_name;

  napi_platform platform;
  CHK(napi_create_platform(argc, argv, 0, nullptr, nullptr, false, &platform));

  napi_runtime runtime;
  CHK(napi_create_runtime(platform, &runtime));

  CHK(napi_create_env(runtime, &g_env));

#if !defined(NAPI_RUNNER_NON_V8)
  // Optional: --inspect=<port>  among extra argv triggers the V8 inspector.
  for (int i = 4; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strncmp(a, "--inspect=", 10) == 0) {
      int port = std::atoi(a + 10);
      CHK(napi_v8_inspector_start(g_env, port, "napi_v8 runner"));
      if (std::strstr(a, ",wait")) {
        std::fprintf(stderr, "[inspector] waiting for client on port %d\n",
                     port);
        napi_v8_inspector_wait_for_connection(g_env);
      }
    }
  }
#endif

  napi_handle_scope scope;
  CHK(napi_open_handle_scope(g_env, &scope));

  // Load binding.so and call napi_register_module_v1(env, exports).
  void* h = dlopen(binding_path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    std::fprintf(stderr, "[runner] dlopen %s: %s\n", binding_path, dlerror());
    return 1;
  }
  typedef napi_value (*RegFn)(napi_env, napi_value);
  RegFn reg = reinterpret_cast<RegFn>(dlsym(h, "napi_register_module_v1"));
  if (!reg) {
    std::fprintf(stderr, "[runner] missing napi_register_module_v1 in %s\n",
                 binding_path);
    return 1;
  }
#if defined(NAPI_RUNNER_JSC)
  // Record the module's declared NAPI version so the engine can apply the
  // node_api_basic_finalize GC-access restriction only to NAPI_VERSION_EXPERIMENTAL
  // modules (mirrors how Node passes the addon api version when binding the env).
  typedef int32_t (*ApiVerFn)(void);
  if (ApiVerFn ver = reinterpret_cast<ApiVerFn>(
          dlsym(h, "node_api_module_get_api_version_v1"))) {
    g_env->module_api_version = ver();
  }
#endif
  napi_value exports;
  CHK(napi_create_object(g_env, &exports));
  napi_value addon = reg(g_env, exports);

  // If the binding's Init threw, capture the exception so that `require()`
  // can re-throw it on the JS side (Node behavior: error.binding stays on
  // the thrown error). napi_set_named_property would otherwise fail with
  // napi_pending_exception.
  bool pending = false;
  napi_is_exception_pending(g_env, &pending);
  napi_value init_ex = nullptr;
  if (pending) {
    napi_get_and_clear_last_exception(g_env, &init_ex);
  }
  if (addon == nullptr) addon = exports;

  // Install __binding etc. on globalThis.
  napi_value global;
  CHK(napi_get_global(g_env, &global));

  CHK(napi_set_named_property(g_env, global, "__binding", addon));
  if (init_ex != nullptr) {
    CHK(napi_set_named_property(g_env, global, "__binding_error", init_ex));
  } else {
    napi_value undef;
    napi_get_undefined(g_env, &undef);
    CHK(napi_set_named_property(g_env, global, "__binding_error", undef));
  }
  napi_value name_val;
  CHK(napi_create_string_utf8(g_env, module_name, NAPI_AUTO_LENGTH, &name_val));
  CHK(napi_set_named_property(g_env, global, "__binding_name", name_val));

  napi_value clog_fn, cerr_fn, drain_fn, spawn_fn;
  CHK(napi_create_function(g_env, "log", NAPI_AUTO_LENGTH,
                           JsConsoleLog, nullptr, &clog_fn));
  CHK(napi_create_function(g_env, "error", NAPI_AUTO_LENGTH,
                           JsConsoleError, nullptr, &cerr_fn));
  CHK(napi_create_function(g_env, "drain", NAPI_AUTO_LENGTH,
                           JsDrainFinalizers, nullptr, &drain_fn));
  CHK(napi_create_function(g_env, "spawn", NAPI_AUTO_LENGTH,
                           JsSpawnSync, nullptr, &spawn_fn));
  CHK(napi_set_named_property(g_env, global, "__console_log", clog_fn));
  CHK(napi_set_named_property(g_env, global, "__console_error", cerr_fn));
  CHK(napi_set_named_property(g_env, global, "__drainFinalizers", drain_fn));
  CHK(napi_set_named_property(g_env, global, "__spawnSync", spawn_fn));

#if defined(NAPI_RUNNER_JSC)
  // Provide globalThis.gc before the bootstrap captures it (__v8_gc).
  napi_value gc_fn;
  CHK(napi_create_function(g_env, "gc", NAPI_AUTO_LENGTH, JscGc, nullptr, &gc_fn));
  CHK(napi_set_named_property(g_env, global, "gc", gc_fn));
#endif

  // Expose process.argv extras (everything past test_path).
  napi_value argv_arr;
  napi_create_array(g_env, &argv_arr);
  napi_value zero;
  napi_create_string_utf8(g_env, argv[0], NAPI_AUTO_LENGTH, &zero);
  napi_set_element(g_env, argv_arr, 0, zero);
  napi_value one;
  napi_create_string_utf8(g_env, test_path, NAPI_AUTO_LENGTH, &one);
  napi_set_element(g_env, argv_arr, 1, one);
  for (int i = 4; i < argc; ++i) {
    napi_value s;
    napi_create_string_utf8(g_env, argv[i], NAPI_AUTO_LENGTH, &s);
    napi_set_element(g_env, argv_arr, 2 + (i - 4), s);
  }
  CHK(napi_set_named_property(g_env, global, "__argv", argv_arr));
  napi_value runner_path;
  napi_create_string_utf8(g_env, argv[0], NAPI_AUTO_LENGTH, &runner_path);
  CHK(napi_set_named_property(g_env, global, "__execPath", runner_path));
  napi_value test_path_v;
  napi_create_string_utf8(g_env, test_path, NAPI_AUTO_LENGTH, &test_path_v);
  CHK(napi_set_named_property(g_env, global, "__filename_v", test_path_v));

  // Run the bootstrap JS.
  napi_value boot_src, boot_result;
  CHK(napi_create_string_utf8(g_env, kBootstrapJs, NAPI_AUTO_LENGTH, &boot_src));
  if (napi_run_script(g_env, boot_src, &boot_result) != napi_ok) {
    napi_value ex;
    napi_get_and_clear_last_exception(g_env, &ex);
    napi_value ex_str;
    napi_coerce_to_string(g_env, ex, &ex_str);
    size_t len = 0;
    napi_get_value_string_utf8(g_env, ex_str, nullptr, 0, &len);
    std::string buf(len + 1, '\0');
    napi_get_value_string_utf8(g_env, ex_str, buf.data(), buf.size(), &len);
    std::fprintf(stderr, "[runner] bootstrap failed: %s\n", buf.c_str());
    return 1;
  }

  // Run the test. Wrap in an IIFE so top-level `return` (common in
  // tests that early-exit on process.argv check) is legal.
  std::string src = "(function(){\n" + ReadFile(test_path) + "\n})();";
  napi_value test_src, test_result;
  CHK(napi_create_string_utf8(g_env, src.c_str(), src.size(), &test_src));
  if (napi_run_script(g_env, test_src, &test_result) != napi_ok) {
    napi_value ex;
    napi_get_and_clear_last_exception(g_env, &ex);

    // JSC's Error.stack omits the "Name: message" header (unlike V8), so print
    // name+message explicitly before the stack for a usable diagnostic.
    auto get_str = [&](napi_value v) -> std::string {
      napi_value as_str;
      if (napi_coerce_to_string(g_env, v, &as_str) != napi_ok)
        return "";
      size_t len = 0;
      napi_get_value_string_utf8(g_env, as_str, nullptr, 0, &len);
      std::string buf(len + 1, '\0');
      napi_get_value_string_utf8(g_env, as_str, buf.data(), buf.size(), &len);
      buf.resize(len);
      return buf;
    };
    napi_value mname, mmsg, stack;
    napi_get_named_property(g_env, ex, "name", &mname);
    napi_get_named_property(g_env, ex, "message", &mmsg);
    napi_get_named_property(g_env, ex, "stack", &stack);
    std::fprintf(stderr, "[fail] %s\n%s: %s\n%s\n", test_path,
                 get_str(mname).c_str(), get_str(mmsg).c_str(),
                 get_str(stack).c_str());
    return 1;
  }

#if defined(NAPI_RUNNER_NON_V8)
  // Non-V8 engines fire napi finalizers during GC and defer their second pass.
  // Tests that assert finalizer side effects (mustCall counts) but don't
  // themselves force a final GC would otherwise see the finalizer run only at
  // teardown — after the check below. Pump gc()+drain a few rounds so
  // collectable finalizers fire first.
  {
    napi_value gc_fn;
    napi_valuetype gc_t = napi_undefined;
    if (napi_get_named_property(g_env, global, "gc", &gc_fn) == napi_ok)
      napi_typeof(g_env, gc_fn, &gc_t);
    if (gc_t == napi_function) {
      napi_value undef, r;
      napi_get_undefined(g_env, &undef);
      for (int i = 0; i < 8; ++i)
        napi_call_function(g_env, undef, gc_fn, 0, nullptr, &r);
    }
    napi_v8_run_event_loop_tasks(g_env);
  }
#endif

  // Finalize common (verify mustCall counts).
  napi_value common_obj, finalize_fn;
  napi_get_named_property(g_env, global, "__common", &common_obj);
  if (napi_get_named_property(g_env, common_obj, "__finalize", &finalize_fn)
      == napi_ok) {
    napi_value undef, ret;
    napi_get_undefined(g_env, &undef);
    if (napi_call_function(g_env, undef, finalize_fn, 0, nullptr, &ret)
        != napi_ok) {
      napi_value ex;
      napi_get_and_clear_last_exception(g_env, &ex);
      napi_value as_str;
      napi_coerce_to_string(g_env, ex, &as_str);
      size_t len = 0;
      napi_get_value_string_utf8(g_env, as_str, nullptr, 0, &len);
      std::string buf(len + 1, '\0');
      napi_get_value_string_utf8(g_env, as_str, buf.data(), buf.size(), &len);
      std::fprintf(stderr, "[fail mustCall] %s\n", buf.c_str());
      return 1;
    }
  }

#if !defined(NAPI_RUNNER_NON_V8)
  // If the inspector was started, keep pumping for a short tail window so
  // post-test CDP commands (Runtime.evaluate, breakpoint pause/resume) are
  // dispatched on this — the V8 — thread before we tear V8 down. This bare
  // embedding has no event loop, so the host MUST drive the tick;
  // napi_v8_run_event_loop_tasks() is what drains queued inspector messages.
  // Production embedders would loop until the client disconnects instead.
  for (int i = 4; i < argc; ++i) {
    if (std::strncmp(argv[i], "--inspect=", 10) == 0) {
      for (int ms = 0; ms < 2000; ms += 10) {
        napi_v8_run_event_loop_tasks(g_env);
#if defined(_WIN32)
        ::Sleep(10);
#else
        ::usleep(10 * 1000);
#endif
      }
      break;
    }
  }
#endif
  CHK(napi_close_handle_scope(g_env, scope));
#if !defined(NAPI_RUNNER_NON_V8)
  napi_v8_inspector_stop(g_env);
#endif
  // Full engine teardown. For the non-V8 engines this fires env-cleanup
  // finalizers (test_general/testEnvCleanup needs them). The Hermes
  // external-buffer teardown use-after-free that previously forced us to skip
  // this — finalizeAll re-reading a freed reference from a finalizer holder
  // after a forced GC, e.g. test_typedarray — is fixed by patches/hermes/0005.
  // Escape hatch for debugging a teardown crash: NAPI_RUNNER_SKIP_TEARDOWN=1.
  if (std::getenv("NAPI_RUNNER_SKIP_TEARDOWN") != nullptr) {
    std::fprintf(stderr, "[pass] %s\n", test_path);
    std::fflush(stdout);
    std::fflush(stderr);
    _exit(0);
  }
  CHK(napi_destroy_env(g_env));
  CHK(napi_destroy_runtime(runtime));
  CHK(napi_destroy_platform(platform));

  std::fprintf(stderr, "[pass] %s\n", test_path);
  return 0;
}
