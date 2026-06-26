// gtest harness for the upstream nodejs/node js-native-api conformance suite.
//
// The suite is a sync-managed mirror of nodejs/node test/js-native-api (C addon
// + test*.js per module, fetched by scripts/sync_napi_tests.py and gitignored),
// so rewriting each module's JS assertions into C++ would diverge from upstream
// and be unmaintainable. Instead this makes gtest the *runner*: it discovers
// every (module, test.js) pair (the same way scripts/run_tests.py does — parse
// the `require('./build/<type>/<module>')` in each test*.js) and registers one
// gtest case per pair that spawns the existing `runner` binary and asserts a
// clean (exit 0) run. A module whose addon .so is not built is reported as a
// SKIP (run scripts/build_tests.py first).
//
// Build/paths are injected by CMake: NAPI_RUNNER_PATH (the runner executable),
// NAPI_TESTS_DIR (…/test/js-native-api), NAPI_SO_SUBDIR (e.g. "Release").

#include <gtest/gtest.h>

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#ifndef NAPI_RUNNER_PATH
#define NAPI_RUNNER_PATH ""
#endif
#ifndef NAPI_TESTS_DIR
#define NAPI_TESTS_DIR ""
#endif
#ifndef NAPI_SO_SUBDIR
#define NAPI_SO_SUBDIR "Release"
#endif

namespace {

constexpr int kTimeoutSeconds = 30;

bool FileExists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

bool IsDir(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> ListDir(const std::string& path) {
  std::vector<std::string> out;
  DIR* d = ::opendir(path.c_str());
  if (!d) return out;
  for (dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name != "." && name != "..") out.push_back(name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

// Module name(s) a test.js requires, from require('../build/<type>/<name>').
std::vector<std::string> RequiredModules(const std::string& testjs) {
  std::ifstream in(testjs);
  std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // require( ./ or ../ /build/ <type> / <module> ); <type> may be ${...}.
  static const std::regex re(
      R"(require\(\s*['"`]\.{1,2}/build/(?:\$\{[^}]+\}|[^/'"`]+)/([A-Za-z0-9_]+))");
  std::vector<std::string> mods;
  for (auto it = std::sregex_iterator(txt.begin(), txt.end(), re); it != std::sregex_iterator(); ++it) {
    std::string m = (*it)[1];
    if (std::find(mods.begin(), mods.end(), m) == mods.end()) mods.push_back(m);
  }
  return mods;
}

// Spawn `runner <so> <module> <testjs>`; return exit status (-1 on timeout).
int RunRunner(const std::string& so, const std::string& module, const std::string& testjs) {
  pid_t pid = ::fork();
  if (pid < 0) return -2;
  if (pid == 0) {
    const char* argv[] = {NAPI_RUNNER_PATH, so.c_str(), module.c_str(), testjs.c_str(), nullptr};
    ::execv(NAPI_RUNNER_PATH, const_cast<char* const*>(argv));
    ::_exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTimeoutSeconds);
  for (;;) {
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (r < 0) return -2;
    if (std::chrono::steady_clock::now() > deadline) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

// One gtest case = one (module, test.js) pair.
class SuiteCase : public ::testing::Test {
 public:
  SuiteCase(std::string so, std::string module, std::string testjs, bool built)
      : so_(std::move(so)), module_(std::move(module)), testjs_(std::move(testjs)), built_(built) {}
  void TestBody() override {
    if (!built_) {
      GTEST_SKIP() << "addon not built: " << so_ << " (run scripts/build_tests.py --engine <e>)";
    }
    int rc = RunRunner(so_, module_, testjs_);
    if (rc == -1) FAIL() << "timed out after " << kTimeoutSeconds << "s: " << testjs_;
    EXPECT_EQ(rc, 0) << "runner exit " << rc << " for module '" << module_ << "', " << testjs_;
  }

 private:
  std::string so_, module_, testjs_;
  bool built_;
};

int RegisterAll() {
  const std::string tests_dir = NAPI_TESTS_DIR;
  int registered = 0;
  for (const std::string& feature : ListDir(tests_dir)) {
    const std::string fdir = tests_dir + "/" + feature;
    if (!IsDir(fdir)) continue;
    for (const std::string& fname : ListDir(fdir)) {
      // test*.js, but not test_null.js helpers? Node includes all test*.js.
      if (fname.rfind("test", 0) != 0) continue;
      if (fname.size() < 3 || fname.substr(fname.size() - 3) != ".js") continue;
      const std::string testjs = fdir + "/" + fname;
      std::string stem = fname.substr(0, fname.size() - 3);
      std::vector<std::string> mods = RequiredModules(testjs);
      if (mods.empty()) continue;  // not an addon test
      for (const std::string& module : mods) {
        const std::string so = fdir + "/build/" NAPI_SO_SUBDIR "/" + module + ".so";
        const bool built = FileExists(so);
        std::string test_name = stem;
        if (mods.size() > 1) test_name += "__" + module;
        std::string suite_name = "JsNativeApi_" + feature;
        ::testing::RegisterTest(
            suite_name.c_str(), test_name.c_str(), nullptr, nullptr, __FILE__, __LINE__,
            [so, module, testjs, built]() -> ::testing::Test* { return new SuiteCase(so, module, testjs, built); });
        ++registered;
      }
    }
  }
  return registered;
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  int n = RegisterAll();
  if (n == 0) {
    std::fprintf(stderr,
                 "[suite] no js-native-api cases discovered under %s\n"
                 "        (is the suite synced? run scripts/sync_napi_tests.py)\n",
                 NAPI_TESTS_DIR);
  } else {
    std::fprintf(stderr, "[suite] discovered %d js-native-api case(s)\n", n);
  }
  return RUN_ALL_TESTS();
}
