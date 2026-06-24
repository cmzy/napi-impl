# napi-impl 技术方案与实施计划

> 目标：构建一个跨平台、可复现的 V8 Node-API 实现，把不同 JS 引擎的 API 统一封装成 Node-API (NAPI) C ABI。首版仅实现 V8 后端。

---

## 1. 项目定位

- **核心交付**：一个名为 `napi_v8` 的单一动态库（每平台一份），导出仅 `napi_*` 符号，内部静态吞入 V8 monolithic。
- **目标用户**：需要在自有 App / SDK 中嵌入 JS 引擎、希望用统一 NAPI ABI 屏蔽引擎差异的客户端开发者。
- **参考项目**：[microsoft/v8-jsi](https://github.com/microsoft/v8-jsi)（借鉴 V8 monolithic 集成、iOS jitless 配置、平台 toolchain 处理思路）。

---

## 2. 范围

### In Scope

- V8 引擎实现 `js_native_api.h`（值操作、函数调用、对象属性、引用、HandleScope、异常）。
- 实现 `node_api_embedding.h`（experimental）以提供 platform / runtime / env 创建入口。
- 跨平台构建：Android / iOS / macOS / Linux / Windows。
- 自动化脚本：V8 拉取、Patch 应用、NAPI 头文件同步、构建、打包。
- 平台原生包：Android AAR、Apple xcframework、Linux/Windows CMake package。

### Out of Scope（首版不做）

- `node_api.h` 主体扩展：`napi_create_async_work` / `napi_create_threadsafe_function` / `napi_get_uv_event_loop` / cleanup hooks 等 Node.js 运行时绑定。
- 其他引擎后端（Hermes / JSC / QuickJS），但目录结构预留。
- V8 Inspector / Debug Protocol。
- 32 位 ABI（armeabi-v7a / x86）。
- libuv / 内建事件循环。

---

## 3. 技术决策汇总

| # | 维度 | 决策 |
|---|------|------|
| 1 | 构建系统 | **双轨**：GN 走 V8 路径，CMake 走 Hermes/JSC/QuickJS 路径；我们自己的代码双构建支持（详见 6.9） |
| 2 | V8 版本 | Stable tag 钉死，写入仓库根 `V8_VERSION` |
| 3 | C++ 标准 | C++20 |
| 4 | V8 链接 | Monolithic 静态库，最终单一 `.so` / `.dylib` / `.dll` |
| 5 | 暴露的 NAPI 头 | `js_native_api.h` + `node_api_embedding.h`（官方 `nodejs/node-api-headers`） |
| 6 | async / TaskRunner | 不实现 |
| 7 | iOS 模式 | jitless |
| 8 | iOS 最低版本 | 13.0 |
| 9 | Android minSdk | 21 |
| 10 | ABI 支持 | `arm64-v8a` / `x86_64` / Apple `arm64+x86_64` / Windows `x64` |
| 11 | 符号导出 | 仅 `napi_*`，其余全部隐藏（含 V8 internals） |
| 12 | Apple 输出 | `NapiV8.xcframework`（iOS device + sim + macOS） |
| 13 | Android 输出 | AAR（含 Prefab v2） |
| 14 | Linux/Windows 输出 | CMake package |
| 15 | 源码依赖目录 | `third_party/`（gitignored） |
| 16 | 构建产物目录 | `out/build/` + `out/dist/`（gitignored） |
| 17 | 多引擎目录预留 | `src/{v8,hermes,jsc,quickjs}/` |
| 18 | V8 编译参数管理 | YAML 单一信源：`config/v8_args.yml`（默认）+ `config/v8_args.local.yml`（用户覆盖，gitignored）+ CLI `--gn-arg key=value` |
| 19 | jitless 套餐展开 | YAML 写 `v8_jitless: true`，`build.py` 自动推导锁定 webassembly/pointer_compression/sandbox/maglev/turbofan |
| 20 | 源文件清单 | 每个 `src/*/` 一个 `sources.txt`，BUILD.gn 与 CMakeLists.txt 各自读取，避免双系统漂移 |

---

## 4. 整体架构

```
+--------------------------------------------------------------+
|        User Native Module (.so / .dylib / .framework)        |
|                  uses napi/js_native_api.h                   |
+--------------------------------------------------------------+
                         |  C ABI (napi_*)
+--------------------------------------------------------------+
|                   libnapi_v8 (本项目产物)                    |
|  +--------------------------------------------------------+  |
|  |  src/common/    引擎无关的 helper / 错误码 / 内部类型   |  |
|  +--------------------------------------------------------+  |
|  |  src/v8/        js_native_api_v8.cc                    |  |
|  |                 napi_v8_engine.cc (embedding)          |  |
|  |                 reference.cc                           |  |
|  +--------------------------------------------------------+  |
|  |  v8_monolith.a  (statically linked, symbols hidden)    |  |
|  +--------------------------------------------------------+  |
+--------------------------------------------------------------+
```

**核心原则：** 所有 `napi_*` 实现只依赖 `v8.h` 公开 API，不触碰 V8 internal，最小化 V8 升级时的 patch 面。

---

## 5. 目录结构

```
napi-impl/
├── .gn                           GN 根标识（V8 路径用）
├── BUILDCONFIG.gn                GN 顶层构建配置
├── BUILD.gn                      GN 顶层 target（V8 路径）
├── CMakeLists.txt                CMake 顶层（Hermes/JSC/QuickJS 路径）
├── DEPS                          gclient deps（V8 → third_party/v8）
├── V8_VERSION                    V8 stable tag 钉死
├── .gitignore                    忽略 third_party/、out/、config/v8_args.local.yml
├── PLAN.md                       本文档
│
├── config/                       构建参数 YAML 单一信源
│   ├── v8_args.yml               默认 V8 编译参数，checked in
│   └── v8_args.local.yml         用户本机覆盖，gitignored
│
├── include/
│   └── napi/
│       ├── js_native_api.h       同步自 nodejs/node-api-headers
│       ├── js_native_api_types.h
│       ├── node_api_embedding.h  experimental，platform/runtime/env 创建
│       └── ...
│
├── src/
│   ├── BUILD.gn                  GN 顶层 src 入口（按 napi_engine 选）
│   ├── CMakeLists.txt            CMake 顶层 src 入口（按 NAPI_ENGINE 选）
│   ├── common/                   双构建支持：引擎无关代码
│   │   ├── sources.txt           源文件清单（两个构建系统的单一信源）
│   │   ├── BUILD.gn              薄壳，读 sources.txt
│   │   ├── CMakeLists.txt        薄壳，读 sources.txt
│   │   ├── napi_error.cc
│   │   └── napi_internal.h
│   ├── v8/                       GN only（V8 必须走 GN）
│   │   ├── sources.txt
│   │   ├── BUILD.gn
│   │   ├── js_native_api_v8_internals.h  Node 上游 internals 适配（去 Node 依赖）
│   │   ├── js_native_api_v8.h    Node 上游 NAPI internal header（含 RefTracker/Reference/TryCatch）
│   │   ├── js_native_api_v8.cc   Node 上游 NAPI 完整 V8 实现，1:1 port（3500+ 行）
│   │   └── napi_v8_engine.cc     embedding：platform/runtime/env + EmbedEnv 子类
│   ├── hermes/                   CMake only（未来），预留
│   │   └── CMakeLists.txt        空
│   ├── jsc/                      CMake only（未来），预留
│   └── quickjs/                  CMake only（未来），预留
│
├── patches/
│   └── v8/
│       ├── series                quilt 风格 patch 列表
│       └── 00xx-*.patch
│
├── gn/                           GN 路径专用
│   ├── napi_flags.gni            编译 flag（C++20、-fvisibility=hidden 等）
│   └── exports/                  符号导出列表生成产物
│       ├── napi_v8.lds           Linux / Android
│       ├── napi_v8.exp           macOS / iOS
│       └── napi_v8.def           Windows
│
├── cmake/                        CMake 路径专用
│   ├── toolchains/               跨编译 toolchain file
│   │   ├── android.cmake
│   │   ├── ios.cmake
│   │   ├── linux.cmake
│   │   └── windows.cmake
│   ├── napi_flags.cmake          编译 flag（与 gn/napi_flags.gni 镜像）
│   └── modules/
│       ├── FindHermes.cmake
│       └── FindJSC.cmake
│
├── scripts/
│   ├── setup.py                  V8 路径：depot_tools + gclient + patches + napi headers + tests
│   ├── setup_hermes.py           Hermes 路径环境准备（未来）
│   ├── build.py                  统一入口，按 --engine 分流 GN / CMake
│   ├── sync_napi_headers.py      同步 nodejs/node-api-headers（公共头 + .def 符号清单）
│   ├── sync_napi_tests.py        同步 nodejs/node 的 test/js-native-api 套件
│   ├── gen_export_list.py        从 .def 生成 .lds/.exp/.def 导出列表
│   ├── verify_flags_parity.py    CI：校验 gn/napi_flags.gni 与 cmake/napi_flags.cmake 一致
│   ├── package_android.py        打 AAR
│   ├── package_apple.py          打 xcframework
│   └── package_cmake.py          打 CMake package（Linux / Windows）
│
├── third_party/                  gitignored
│   ├── depot_tools/              GN 路径用
│   └── v8/                       GN 路径用
│
├── out/                          gitignored，所有产物统一在此
│   ├── build/                    构建中间产物
│   │   ├── v8-mac-arm64-release/
│   │   ├── v8-ios-arm64-release/
│   │   ├── v8-android-arm64-release/
│   │   ├── v8-linux-x64-release/
│   │   ├── v8-windows-x64-release/
│   │   └── hermes-*               未来
│   └── dist/                     最终交付
│       ├── napi-v8.aar
│       ├── NapiV8.xcframework/
│       ├── napi-v8-linux-x64/
│       └── napi-v8-windows-x64/
│
└── test/
    ├── BUILD.gn
    ├── CMakeLists.txt
    ├── run.cc                    M1 冒烟用例：napi_run_script("1+2")
    ├── runner.cc                 M2+：上游 js-native-api 套件 host runner
    └── js-native-api/            gitignored，由 scripts/sync_napi_tests.py 同步
```

---

## 6. 关键技术点

### 6.1 V8 拉取与版本钉死

- `V8_VERSION` 文件存具体 stable tag（如 `13.6.233.10`）。
- `scripts/setup.py` 行为：
  1. 检查 / 拉取 `depot_tools` 到 `third_party/depot_tools/`（支持 `DEPOT_TOOLS_URL` 环境变量做镜像）。
  2. 写 `.gclient` 配置，`gclient sync --revision v8@<TAG> --no-history --shallow`。
  3. 应用 `patches/v8/series` 中的所有 patch（幂等，依赖 `.applied` 标记）。
  4. 调 `scripts/sync_napi_headers.py` 同步官方 NAPI 头到 `include/napi/`。
- 升级流程：改 `V8_VERSION` → 跑 setup → 修补 patch（如失败） → 全平台 CI 跑通 → PR。

### 6.2 Patch 策略

- 90% 配置走 `args.gn`，源码 patch 最小化。
- 必须 patch 的典型场景：
  - 个别平台 GN 文件小修补（如 Windows ARM64、Android 新 NDK）。
  - V8 默认 build 假设不成立时（如非 Chromium toolchain 下的某些 ASSERT）。
- 命名遵循 quilt 习惯：`00xx-<topic>.patch`，`series` 文件列序。

### 6.3 NAPI 头文件与测试套件同步

**头文件：** 源 [nodejs/node-api-headers](https://github.com/nodejs/node-api-headers)。

- `scripts/sync_napi_headers.py`：
  1. 浅克隆到临时目录，指定 tag（默认 `v1.9.0`，可通过 `NAPI_HEADERS_VERSION` 覆盖）。
  2. 复制 `include/*` 到 `include/napi/`。
  3. 复制 `def/js_native_api.def` 和 `def/node_api.def` 到 `include/napi/`，供 `gen_export_list.py` 提取符号。
  4. 记录版本到 `include/napi/.version_stamp`。

**测试套件：** 源 [nodejs/node](https://github.com/nodejs/node) 的 `test/js-native-api/`。

- `scripts/sync_napi_tests.py`：
  1. 浅克隆 + sparse-checkout 仅拉 `test/js-native-api`，避免下整个 1GB+ 仓库。
  2. 默认钉 `v22.11.0`（Node 22 LTS），可通过 `NODE_TESTS_VERSION` 或 `--tag` 覆盖。
  3. 同步到 `test/js-native-api/`（gitignored），版本写入 `.version_stamp`。
  4. 上游测试形态：每个子目录含 `binding.c` / `binding.cc`（NAPI 调用者）+ `test.js`（断言驱动）。我们的 host runner（M2 实现）：
     - 编译每个 binding.c 为独立 .so 加载，或链入静态库索引
     - 提供最小 JS 执行器跑 test.js，输出 TAP 格式

**embedding API：** 上游不提供 `node_api_embedding.h`，本项目自定义 `include/napi_v8/embedding.h`，独立于 `include/napi/`，不被 sync 脚本覆盖。引擎初始化函数 `napi_create_platform` / `napi_create_runtime` / `napi_create_env` 等由此头声明，跨引擎统一。

### 6.4 符号隐藏（关键）

**编译侧：** 所有 target 默认 `-fvisibility=hidden -fvisibility-inlines-hidden`。

**链接侧导出白名单：**

| 平台 | 文件 | 形式 |
|------|------|------|
| Linux / Android | `napi_v8.lds` | `{ global: napi_*; local: *; };` |
| macOS / iOS | `napi_v8.exp` | 每行 `_napi_xxx`（Mach-O 下划线前缀） |
| Windows | `napi_v8.def` | `EXPORTS\nnapi_xxx\n...` |

**CI 校验：** 构建后跑 `nm -D --defined-only`（ELF）/ `nm -gU`（Mach-O）/ `dumpbin /EXPORTS`（PE），断言除 `napi_*` 外无任何外部符号。

### 6.5 GN 顶层 target 简化形态

```gn
# BUILD.gn
declare_args() {
  napi_engine = "v8"   # 未来可扩 hermes / jsc / quickjs
}

shared_library("napi_v8") {
  deps = [
    "//src/common",
    "//src/${napi_engine}",
  ]
  if (napi_engine == "v8") {
    deps += [ "//third_party/v8:v8_monolith" ]
  }
  configs += [ "//gn:hide_non_napi_symbols" ]

  if (is_android)      { output_name = "napi_v8" }
  else if (is_apple)   { output_name = "NapiV8"  }
  else if (is_win)     { output_name = "napi_v8" }
  else                 { output_name = "napi_v8" }
}
```

### 6.6 V8 编译参数：YAML 单一信源

V8 的编译参数从硬编码 `.gn` 文件迁到 `config/v8_args.yml`，对用户友好且可覆盖。

**默认文件 `config/v8_args.yml`（checked in）：**

```yaml
common:
  is_debug: false
  is_component_build: false
  v8_monolithic: true
  v8_use_external_startup_data: false
  use_custom_libcxx: false        # 关键：用系统 libc++ 避免与宿主 ABI 冲突
  treat_warnings_as_errors: false
  symbol_level: 1
  v8_enable_i18n_support: false   # 减体积；按需放开
  v8_enable_inspector: false

platforms:
  mac_arm64:
    target_os: mac
    target_cpu: arm64
    mac_deployment_target: "11.0"

  mac_x86_64:
    target_os: mac
    target_cpu: x64
    mac_deployment_target: "11.0"

  ios_arm64:
    target_os: ios
    target_cpu: arm64
    ios_deployment_target: "13.0"
    ios_enable_code_signing: false
    v8_jitless: true              # 自动展开 jitless 套餐（见下）

  ios_sim_arm64:
    target_os: ios
    target_cpu: arm64
    target_environment: simulator
    ios_deployment_target: "13.0"
    v8_jitless: true

  ios_sim_x86_64:
    target_os: ios
    target_cpu: x64
    target_environment: simulator
    ios_deployment_target: "13.0"
    v8_jitless: true

  android_arm64:
    target_os: android
    target_cpu: arm64
    android64_ndk_api_level: 21

  android_x86_64:
    target_os: android
    target_cpu: x64
    android64_ndk_api_level: 21

  linux_x64:
    target_os: linux
    target_cpu: x64

  windows_x64:
    target_os: win
    target_cpu: x64
```

**用户覆盖文件 `config/v8_args.local.yml`（gitignored，示例）：**

```yaml
common:
  v8_enable_inspector: true       # 本机调试需要
  is_debug: true
  symbol_level: 2
```

**合并优先级（高 → 低）：**

```
1. CLI:           --gn-arg key=value   （可重复，最高）
2. 环境变量:      NAPI_GN_EXTRA_ARGS_FILE=/path/to/extra.yml
3. 本地覆盖:      config/v8_args.local.yml
4. 平台块:        v8_args.yml -> platforms.<platform>_<arch>
5. 公共块:        v8_args.yml -> common              （最低）
```

**jitless 套餐自动展开：** YAML 只需写 `v8_jitless: true`，`build.py` 内置约束表强制带出整套：

```python
# scripts/build.py 内置约束
JITLESS_BUNDLE = {
    "v8_enable_webassembly": False,
    "v8_enable_pointer_compression": False,
    "v8_enable_sandbox": False,
    "v8_enable_maglev": False,
    "v8_enable_turbofan": False,
}
# 用户若手动设了相反值，build.py 报错退出
```

**构建时流程：**

1. `build.py` 加载 YAML，按优先级合并。
2. 应用约束表（jitless 套餐等）。
3. 生成 `out/build/<engine>-<platform>-<arch>-<config>/args.gn`。
4. `gn gen` + `ninja`。
5. 生成的 `args.gn` 即最终事实，便于排查。

### 6.7 平台打包

#### Android → AAR

```
napi-v8.aar
├── AndroidManifest.xml             最小 manifest
├── classes.jar                     空 jar
├── jni/
│   ├── arm64-v8a/libnapi_v8.so
│   └── x86_64/libnapi_v8.so
├── prefab/                         Prefab v2
│   └── modules/napi_v8/...
└── R.txt
```

下游 App 通过 `find_package(napi_v8 REQUIRED CONFIG)` 集成。

#### Apple → XCFramework

`xcodebuild -create-xcframework` 合并三 slice：

```
NapiV8.xcframework/
├── Info.plist
├── ios-arm64/NapiV8.framework/
├── ios-arm64_x86_64-simulator/NapiV8.framework/
└── macos-arm64_x86_64/NapiV8.framework/
```

Framework 内含 `Headers/napi/*.h` + `Modules/module.modulemap`。

#### Linux / Windows → CMake package

```
napi-v8-linux-x64/
├── include/napi/...
├── lib/libnapi_v8.so
└── cmake/
    ├── napi_v8-config.cmake
    └── napi_v8-config-version.cmake
```

下游 `find_package(napi_v8 REQUIRED)` → `target_link_libraries(myapp PRIVATE napi_v8::napi_v8)`。

### 6.8 构建命令统一入口

```bash
# 一次性环境准备（拉 depot_tools、V8、patches、napi headers）
python3 scripts/setup.py

# 单平台单架构构建
python3 scripts/build.py --engine=v8 --platform=mac --arch=arm64 --config=release
python3 scripts/build.py --engine=v8 --platform=ios --arch=arm64
python3 scripts/build.py --engine=v8 --platform=android --arch=arm64
python3 scripts/build.py --engine=v8 --platform=linux --arch=x64
python3 scripts/build.py --engine=v8 --platform=windows --arch=x64

# 一键打包（自动跑所有需要的 arch + slice 合并）
python3 scripts/build.py --platform=apple --package    # iOS + iOS Sim + macOS → xcframework
python3 scripts/build.py --platform=android --package  # arm64 + x86_64 → aar
python3 scripts/build.py --platform=linux --package    # → CMake package
python3 scripts/build.py --platform=windows --package
```

### 6.9 双构建系统的单一信源策略

我们自己的代码需在 GN 路径（V8）与 CMake 路径（Hermes/JSC/QuickJS）下都能编。三个易漂移点的处理：

**1. 源文件清单 → `sources.txt`**

每个目录一个纯文本清单，两个构建文件各自原生读取，零 codegen。

```
# src/common/sources.txt
napi_error.cc
scope_stack.cc
```

```gn
# src/common/BUILD.gn（薄壳）
sources_list = read_file("sources.txt", "list lines")
source_set("common") {
  sources = filter_exclude(sources_list, [ "" ])
  configs += [ "//gn:napi_flags" ]
}
```

```cmake
# src/common/CMakeLists.txt（薄壳）
file(STRINGS sources.txt COMMON_SOURCES)
add_library(napi_common OBJECT ${COMMON_SOURCES})
target_link_libraries(napi_common PRIVATE napi_flags)
```

**2. 编译 flag → 镜像两份 + CI parity 校验**

```
gn/napi_flags.gni        # GN 侧
cmake/napi_flags.cmake   # CMake 侧
```

`scripts/verify_flags_parity.py` 规范化两边后 diff，CI gate；漂移立即报错。

**3. 符号导出列表 → 脚本生成，两侧共消费**

`scripts/gen_export_list.py` 跑出 `napi_*.lds / .exp / .def`，GN 与 CMake 都直接消费同一份产物，无任何额外工作。

**入口分流（用户无感）：**

```python
# scripts/build.py 内部分流
if engine == "v8":
    gn_path(platform, arch, config)     # gn gen + ninja
else:                                    # hermes / jsc / quickjs
    cmake_path(engine, platform, arch, config)
    # cmake -B out/build/<...> -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/<platform>.cmake
    # cmake --build out/build/<...>
```

打包脚本（`package_android.py` / `package_apple.py` / `package_cmake.py`）**完全引擎无关**，输入只看 `out/build/<engine>-<platform>-<arch>/` 下产物，输出统一格式。

---

## 7. 实施里程碑

### M1 — 骨架与 macOS 跑通（1–2 周）

**目标：** 在 macOS arm64 上端到端跑通最小 NAPI 调用链：`napi_create_platform → napi_create_runtime → napi_create_env → napi_run_script("1+2") → napi_get_value_int32 == 3`。

**任务清单：**

- [ ] `.gn` / `BUILDCONFIG.gn` / 顶层 `BUILD.gn` 骨架
- [ ] `DEPS` + `V8_VERSION` 钉死 stable tag
- [ ] `.gitignore`（`third_party/` + `out/`）
- [ ] `scripts/setup.py`：depot_tools + V8 + 空 patch + napi headers
- [ ] `scripts/sync_napi_headers.py`
- [ ] `scripts/build.py`：mac arm64 release 一条路径（含 YAML 加载、合并、约束展开、写 `args.gn`）
- [ ] `config/v8_args.yml` 默认值（仅含 `common` + `mac_arm64` 块即可启动）
- [ ] `gn/napi_flags.gni`（C++20、`-fvisibility=hidden` 等）
- [ ] `src/common/sources.txt` + `src/common/BUILD.gn`（薄壳）
- [ ] `src/v8/sources.txt` + `src/v8/BUILD.gn`
- [ ] `src/v8/napi_v8_engine.cc`：`napi_create_platform` / `napi_create_runtime` / `napi_create_env` / `napi_run_script` 最小实现
- [ ] `src/v8/js_native_api_v8.cc`：足够支撑 `1+2` 用例的最小函数集（`napi_open_handle_scope` / `napi_close_handle_scope` / `napi_create_string_utf8` / `napi_get_value_int32` 等）
- [ ] `scripts/gen_export_list.py` + `gn/exports/napi_v8.exp`
- [ ] `test/run.cc` + `test/BUILD.gn`
- [ ] CI 符号校验脚本（`nm -gU` 断言）

**验收：** `python3 scripts/build.py --platform=mac --arch=arm64 && out/build/mac-arm64-release/test/run` 返回 0。

### M2 — 完整 NAPI 函数集（4–6 周）

**目标：** 在 Linux + macOS 上实现 `js_native_api.h` 全部 ~150 个函数，跑通 `nodejs/node` 上游 `test/js-native-api/` 测试集。

**任务清单：**

- [ ] 值类型：String / Number / BigInt / Boolean / Symbol / Object / Array / Function / TypedArray / DataView / ArrayBuffer / External / Date / Error
- [ ] 函数：`napi_call_function` / `napi_new_instance` / `napi_create_function` / `napi_define_class`
- [ ] 属性：`napi_get_property` / `napi_set_property` / `napi_define_properties` / Symbol property
- [ ] 引用：`napi_create_reference` / `napi_delete_reference` / Finalizer (V8 WeakCallback 两阶段)
- [ ] HandleScope / EscapableHandleScope 栈管理
- [ ] 异常：`napi_throw` / `napi_throw_error` / `napi_get_and_clear_last_exception`
- [ ] Wrap / Unwrap 对象绑定 native 数据
- [ ] `napi_get_last_error_info` 配套
- [ ] Linux x64 构建路径打通
- [ ] `test/runner.cc` 实现：dlopen 每个 binding.so + 跑 test.js
- [ ] CI gate：`test/js-native-api/` 由 `scripts/sync_napi_tests.py` 在 setup 期间已同步

**验收：** `js-native-api/` 测试通过率 ≥ 95%（剩余允许是 N-API 较新版本的实验 API）。

### M3 — Windows 适配（1 周）

- [ ] `config/v8_args.yml` 补 `windows_x64` 平台块
- [ ] MSVC 兼容性修正（如果有）
- [ ] `.def` 导出文件生成与校验
- [ ] CMake package 打包脚本

**验收：** Windows x64 下 M2 测试集通过率持平 macOS / Linux。

### M4 — Android 适配 + AAR（1.5 周）

- [ ] `config/v8_args.yml` 补 `android_arm64` / `android_x86_64` 平台块
- [ ] NDK r26+ toolchain 校准
- [ ] minSdk 21 编译/运行验证
- [ ] `scripts/package_android.py`：AAR + Prefab v2
- [ ] Android 模拟器跑 M2 测试集

**验收：** 模拟器 + 真机 arm64 均能加载 `libnapi_v8.so` 并跑通测试。

### M5 — iOS 适配 + XCFramework（1.5 周）

- [ ] `config/v8_args.yml` 补 `ios_arm64` / `ios_sim_arm64` / `ios_sim_x86_64` 平台块（含 `v8_jitless: true`）
- [ ] jitless 套餐约束在 `build.py` 内落地并加测试
- [ ] `scripts/package_apple.py`：xcframework 合并 + module.modulemap
- [ ] iOS 真机 + 模拟器跑 M2 测试集（jitless 性能可接受）

**验收：** xcframework 能被 Xcode 项目直接拖入并使用。

### M6 — CI、文档、稳定化（1 周）

- [ ] GitHub Actions 矩阵 CI（5 平台 × Release）
- [ ] 符号校验集成进 CI gate
- [ ] `scripts/verify_flags_parity.py` 加入 CI（即便此时 CMake 路径未启用，预先就绪）
- [ ] 性能基准（Octane / 自研用例）
- [ ] 集成示例：每平台一个 hello-world consumer
- [ ] 升级 V8 版本的 runbook

**验收：** 所有平台 release 产物自动产出并附在 GitHub Release。

### M7 — Hermes 集成

**目标：** 把 `napi_*` 后端接到 Hermes，证明双构建系统设计与 `src/common/` 抽象有效；产物 `libnapi_hermes.so` 与 `libnapi_v8.so` 同 ABI 可互换。

**实现策略（关键决策）：策略 A —— 链接上游 Hermes 自带的 Node-API，不自己手写 `js_native_api`。**

- 源码钉死 `microsoft/hermes-windows`（`HERMES_VERSION`）。其 `API/hermes_node_api/`（`hermesNodeApi` 静态库）直接在 Hermes VM 上实现了完整 `napi_*`；`API/hermes_shared/js_runtime_api.h` 提供 `jsr_*` embedding C ABI。
- 我们**唯一新写的 C++** 是 embedding 适配层 `src/hermes/napi_hermes_engine.cc`：在 `hermes::vm::Runtime::create` + `hermes::node_api::getOrCreateNodeApiEnvironment` + `openNodeApiScope` 之上实现项目统一的 `napi_create_platform/runtime/env`（+ tick 钩子，见 `include/napi_v8/embedding.h`）。不写 `js_native_api_hermes.cc`。
- **不使用** Hermes JSI（`API/hermes`）与 Chrome inspector（`API/hermes_shared/inspector`）。后者在非 Windows 上无法编译（`__declspec`、MSVC 版 `std::exception(const char*)`、folly `memrchr` 冲突），且调试不在范围内。
- 链接关系：Hermes 单独构建（它必须是自己的 CMake root，无法 `add_subdirectory`），我们链接三个预编译静态库 `hermesNodeApi + hermesvm_a + boost_context` 外加系统 ICU —— 见 `cmake/modules/FindHermes.cmake`。
- 符号收口：复用 `gn/exports/napi_v8.lds`（version script），最终 `libnapi_hermes.so` 仅导出 `napi_*`/`node_api_*`（已验证 131 个，零泄漏）。

**任务清单：**

- [x] `scripts/setup_hermes.py`：clone + patch（`HERMES_VERSION` 钉 commit）
- [x] `patches/hermes/`：`0001-cmake-lit-ctest-case-insensitive-include.patch`（hermes-windows 的 `include(ctest)` 大小写在 Linux 上失败）
- [x] `cmake/modules/FindHermes.cmake`（定位预编译归档 + ICU，导出 `Hermes::Hermes`）
- [x] `cmake/napi_flags.cmake`（visibility hidden + 头文件，镜像 GN 侧意图）
- [x] 顶层 `CMakeLists.txt` + `src/CMakeLists.txt`（按 `NAPI_ENGINE` 分流）
- [x] `src/hermes/sources.txt` + `src/hermes/CMakeLists.txt`
- [x] `src/hermes/napi_hermes_engine.cc`（embedding 适配层）
- [x] `src/common/CMakeLists.txt`（CMake 路径下 common 编译通过，common 现为空 → INTERFACE）
- [x] `scripts/build.py` 启用 `--engine=hermes` CMake 分流
- [x] **M7.1 烟囱测试**：`napi_run_script("1 + 2") == 3`（`test/hermes_smoke.cc`，CTest `hermes_smoke` 通过，Linux x86_64）
- [~] **M7.2 js-native-api 套件（43/50 通过，零崩溃，Linux x86_64）**：
  - **napi_remove_wrap finalizer 修复(patch 0004)**:`NodeApiReferenceWithFinalizer` 缺 `resetFinalizer()` 覆写,导致 `napi_remove_wrap` 后用户 finalizer 仍被调用(撞 binding 的 `unreachable`)。补上覆写清空 finalizer → 救回 `6_object_wrap/test-object-wrap-ref`。
  - **finalizer 异常路由**:finalizer 抛出的异常经 env 的 unhandledErrorCallback 路由到 JS `process.on('uncaughtException')`(adapter 先清 pending 异常再回调 JS;runner 提供 `__emitUncaughtException` 分发器,无 handler 时打印 `Error during Finalize`)→ 救回 `test_reference/test_finalizer`、`test_exception/testFinalizerException`。
  - **teardown UAF 修复**:`napi_destroy_runtime` 的 deferred-finalizer-task UAF 已修(`EmbedTaskRunner` teardown 内联,见 commit)——令完整 teardown 下 env 清理钩子可用;但残留的 Hermes UAF(多 external 在 `~Runtime` finalize 时 freed-RefTracker 仍挂链表)使 runner 默认仍跳 teardown,`testEnvCleanup` 暂留失败。
  - **引擎兼容修复(patches/hermes)**:(1) `napi_create_function` 现带 `.prototype`(原 `createWithoutPrototype` 致 `new fn()`/`instanceof`/`class extends fn` 全断 → 救回 `test_new_target`);(2) `napi_adjust_external_memory` 实现(原 `Not implemented` stub);(3) adapter 把 env apiVersion 报为 9(对齐 vendored node-api-headers v1.9.0/Node22,8↔9 不跨 Hermes 行为门槛)→ 救回 `test_general`。
  - **finalizer 时序修复**:runner 在 mustCall 检查前对 Hermes 强制 `gc()`×8 + drain,让可回收对象的 finalizer 在断言前触发(救回 `test_instance_data`、`test_general/testFinalizer`、`test_cannot_run_js`、`test_reference/test.js`)。强制 GC 后 Hermes 在 `~Runtime` 的 `finalizeAll` 会二次 finalize external(typedarray 崩溃),故 runner 对 Hermes 跳过显式 teardown(一进程一测试,进程退出回收);代价是 `test_general/testEnvCleanup`(它正是测 env 清理钩子)转为失败——属 runner 取舍,非 NAPI 实现缺陷。
  - 测试 runner 复用 `test/runner.cc`（同一份源码,经 `NAPI_RUNNER_HERMES` 编译为引擎无关——跳过 V8 inspector 与内部 finalizer drain）。`scripts/{build_tests,run_tests}.py` 加 `--engine=hermes`。
  - **harness 多目标修复（双引擎受益）**:`build_tests.py` 现解析 `binding.gyp` 的 `targets`,每个 target 各编一个 `.so`（如 `test_object` 目录产 `test_object.so` + `test_exceptions.so`）;`run_tests.py` 按每个 `test*.js` 的 `require('./build/.../<name>')` 选对应 `.so`。`test_object`/`test_reference` 等不再 build 失败,34/34 target 全编过,0 skip。
  - **关键修复 —— TaskRunner**:Hermes node-api 的 `NodeApiEnvironment::enqueueFinalizer` 会无条件 `taskRunner_->post(...)` 来延迟第二趟(GC 后)finalizer;传 `nullptr` 时,任何 `napi_wrap` 对象被 GC 回收即 SIGSEGV。adapter 现提供 `EmbedTaskRunner`(排队 + 在 `napi_v8_run_event_loop_tasks` tick 里 drain),消除全部崩溃,29/45 → 33/45。
  - 剩余 7 个失败两类:(a) **引擎语义差异**(预期,行为正确仅文案/上限不同,不修):`test_array`(Hermes 拒绝 4G 元素数组)、`test_constructor`/`6_object_wrap`/`test_object`/`test_properties`(只读/只-getter/不可扩展赋值的 `TypeError` **错误信息文案**与 V8 不同——抛错本身正确);(b) **深层 finalizer / teardown**:`test_finalizer/test_fatal_finalize`(需 experimental finalizer 策略:finalizer 内联且调用影响 GC 的代码即 fatal,与我们 v9 deferred 策略冲突)、`testEnvCleanup`(teardown UAF 残留)。
- [~] **M7.3 跨平台打包**：
  - [x] `napi_hermes.lds`/`.exp`/`.def`:`gen_export_list.py --engine=hermes`(嵌入符号拆 common / v8-only,Hermes 不导出 inspector+SAB);`src/hermes` 改链 `napi_hermes.lds`。
  - [x] `cmake/toolchains/{linux,android,ios,windows}.cmake` 填充;`build.py` 按 platform 传 `CMAKE_TOOLCHAIN_FILE`(linux 原生已验证)。
  - [x] `package_cmake.py` / `package_android.py` 引擎参数化(`--engine`);`build.py --package` 走 `package_cmake`(linux CMake package 已验证)。
  - [x] **Android 交叉编译 + AAR(已验证 arm64-v8a)**:`build.py --engine=hermes --platform=android` 自动:先原生构建 host `hermesc`/`shermes` 并经 `-DIMPORT_HOST_COMPILERS=<host>/ImportHostCompilers.cmake` 导入;目标 Hermes 用 NDK toolchain + `HERMES_UNICODE_LITE=ON`(Android 无系统 ICU)+ `BOOST_CONTEXT_ASSEMBLER=gas`(arm64 elf 的 clang_gas 变体只存在于 Windows pe)交叉编译 `hermesNodeApi`/`hermesvm_a`;再链我们的 `libnapi_hermes.so`(`FindHermes` 加 `NO_CMAKE_FIND_ROOT_PATH`、ICU 改可选、Android 补 `liblog`);strip 后 `package_android.py` 产 `napi-hermes.aar`(Prefab v2,arm64-v8a 的 `.so` 4.0M)。导出仍仅 131 个 `napi_*`/`node_api_*`,零泄漏。
  - [ ] iOS 交叉编译(同 host-hermesc 机制,toolchain 用 ios.cmake;未跑)。
  - [x] **CI**(`.github/workflows/hermes-ci.yml`):linux job 跑 build + smoke + 套件基线门(`run_tests.py --min-pass 40`)+ 导出符号纪律检查;android job 用 runner 预装 NDK 产 `napi-hermes.aar` 并上传 artifact。基线回退(<40)即红。
  - [ ] `scripts/verify_flags_parity.py` 接入。

**已验收（M7.1）：** `python3 scripts/build.py --engine=hermes --platform=linux --arch=x86_64` 产出 `out/build/hermes-linux-x86_64/src/hermes/libnapi_hermes.so`，仅导出 `napi_*`，下游仅用 `napi_*` 即可端到端跑通。

**最终验收（M7.3）：** `python3 scripts/build.py --engine=hermes --platform=android --arch=arm64 --package` 输出 `napi-hermes.aar`，下游 App 切换 `libnapi_v8` ↔ `libnapi_hermes` 无需改源码。

---

## M8 — JSC 集成

**目标：** 把 `napi_*` 后端接到 JavaScriptCore，产物 `libnapi_jsc` 与 `libnapi_v8` / `libnapi_hermes` 同 ABI 可互换。

**实现策略（关键决策）：策略 B —— 在 JSC 的 C API 上自己手写 `js_native_api`。**

- 与 Hermes 不同：**JSC 不自带 Node-API**（Hermes 的 `hermesNodeApi` 是策略 A 的前提，JSC 没有等价物）。因此 JSC 必须像 V8 那样把整套 `napi_*` 手写在引擎公开 API 上——只是底座从 `v8.h` 换成 JSC 的稳定 C API（`JSValueRef` / `JSObjectRef` / `JSStringRef`，头 `<JavaScriptCore/JavaScript.h>`）。
- **J1 引擎来源：苹果系统 `JavaScriptCore.framework`**（macOS/iOS 自带，无需拉取/构建/vendoring）。因为 JSC 是**动态系统框架**（不像 V8 monolith / Hermes 静态归档被吞入），它始终是外部动态依赖——`otool -L` 可见，符号不会被吸收进我们的 dylib。Linux/Android/Windows 需要自构建 WebKit JSC，属 J2。
- **唯一新写的 C++**：`src/jsc/`——`js_native_api_jsc.{h,cc}`（core）+ `jsc_object.cc`（对象/函数/类/wrap/引用/promise）+ `napi_jsc_engine.cc`（embedding 适配层，把 `JSContextGroupRef`→`napi_runtime`、`JSGlobalContextRef`→`napi_env` 映射到项目统一的 `napi_create_platform/runtime/env` + tick 钩子）。
- 构建走 CMake 轨（与 Hermes 同），`cmake/modules/FindJSC.cmake` 定位系统框架并导出 `JSC::JSC`；`build.py --engine=jsc` 分流。

**值模型与生命周期：**

- `napi_value` **即** `JSValueRef`（reinterpret_cast）。每个交出的值都 `JSValueProtect` 并记入当前 handle scope，关 scope 时统一 `JSValueUnprotect`。env 创建时开一个 root scope，承载 host 在显式 scope 之外创建的值。
- 异常：env 持有 `pending_exception`；JSC 各调用的 `JSValueRef* exception` out 参被收口为「pending」，`napi_get_and_clear_last_exception` 取走。
- external / `napi_wrap`：用一个带 finalize 回调的自定义 `JSClass`（holder 对象，private 挂 native 指针 + finalizer）。wrap 把 holder 以一个隐藏 Symbol 键挂到目标对象上——目标被 GC 时 holder 随之被回收触发 finalize，finalizer **不在 GC 中重入 JS**，而是入队、由 tick（`napi_v8_run_event_loop_tasks`）与 env 销毁时 drain（镜像 Hermes 的 second-pass 思路）。
- **弱引用（真实 GC 语义）**：JSC 公开 C API 无弱句柄原语，故用「holder + 共享 `RefControl`」实现：对象型 ref 在目标上挂一个 finalize holder，目标被回收时 holder 的 finalize 把 `control->alive` 置 false 并清空指针，`napi_get_reference_value` 此后返回空（napi_value NULL）。`refcount>0` 时额外 `JSValueProtect`（强保活），降到 0 解保护（转弱）。`napi_wrap` 返回的 ref 与 wrap holder 共用同一 `control`（对齐 Node 单 Reference 语义）。原始型（string/number/symbol）目标无法挂 holder 且无可观测回收，退化为全程强引用。
- 函数 / `napi_define_class`：用带 `callAsFunction` / `callAsConstructor` 的自定义 `JSClass`（private 挂 `napi_callback` + data）。属性/访问器/特性统一经缓存的 `Object.defineProperty` 落地（C API 无直接定义访问器的入口）。

**任务清单：**

- [x] `cmake/modules/FindJSC.cmake`（Apple 系统框架，导出 `JSC::JSC`）
- [x] 顶层 `src/CMakeLists.txt` 的 `jsc` 分流 + `src/jsc/CMakeLists.txt` + `sources.txt`
- [x] `src/jsc/napi_jsc_engine.cc`（embedding 适配层）
- [x] `src/jsc/js_native_api_jsc.{h,cc}` + `jsc_object.cc`（手写 napi 面）
- [x] `scripts/build.py` 启用 `--engine=jsc`（macOS CMake 分流；无引擎预构建）
- [x] `scripts/gen_export_list.py` 支持 `--engine=jsc`；`gn/exports/napi_jsc.{lds,exp,def}`（139 符号，与 Hermes 表一致：同样略去 V8-only inspector+SAB）
- [x] **弱引用 / finalizer 真实 GC 语义**（RefControl + holder；强引用保活、弱引用回收后置空）
- [x] **J1 烟囱测试**：`test/jsc_smoke.cc`——`napi_run_script("1+2")==3`，外加字符串往返 / 对象属性 / JS 调原生函数；CTest `jsc_smoke` 通过（macOS x86_64）
- [x] **弱引用测试**：`test/jsc_weakref.cc`（white-box，经内部头取 `env->ctx` 调 `JSGarbageCollect` 强制回收）——验证 wrap finalizer 回收后触发、弱 ref 置空、强 ref(refcount 1) 跨 GC 存活、unref 后转弱可回收；CTest `jsc_weakref` 通过（确定性，需对保守式栈扫描做栈卫生：目标只活在已弹出的 noinline 帧里、回收前不把值取到栈上）
- [x] **公共 API 完整性核对**：`js_native_api.h` 全部 123 个函数均导出（0 遗漏）；`node_api.h` 的 32 个 Node 运行时扩展按 §2 范围不导出（与 V8/Hermes 后端一致）
- [x] **符号纪律**：`nm -gU` 断言仅 139 个 `napi_*`/`node_api_*` 导出，零泄漏；`otool -L` 确认 JSC 为外部动态依赖

**J1 覆盖范围：** 值（undefined/null/boolean/number/string utf8+utf16+latin1/symbol）、typeof/coerce/strict_equals、`run_script`、错误与异常（throw/create/syntax/pending/clear/last_error）、handle/escapable scope、对象与属性（named/keyed/element/property_names/prototype/freeze/seal/has_own）、数组、函数（create/call/new_instance/cb_info/new_target/instanceof/define_class/define_properties）、external/wrap/unwrap/remove_wrap/add_finalizer、引用、promise（deferred）、date、property key、instance_data。全套 ABI 符号均**有定义**（drop-in 互换），故导出列表干净链接。

**J2（已 defined 为占位，返回明确状态而非崩溃，待补全）：** BigInt（JSC C API 无构造入口）、TypedArray / ArrayBuffer / DataView 的 create+info+detach、`type_tag` 系列、`get_all_property_names` 的 filter/mode/symbol 键。跨平台（iOS/非 Apple 的 WebKit JSC 构建）与打包（xcframework）亦属 J2。**弱引用已在 J1 实现**（见上）。

**V8 专属扩展的适配（待定，需拍板）：** `napi_v8/inspector.h`（8 函数，CDP 调试）与 `napi_v8/sab.h`（3 函数，SharedArrayBuffer 零拷贝）是 V8 后端独有，JSC 导出列表当前不含它们。适配方案见下节《V8 专属 API 适配》。

**已验收（J1）：** `python3 scripts/build.py --engine=jsc --platform=mac --arch=x86_64` 产出 `out/build/jsc-mac-x86_64-release/src/jsc/libnapi_jsc.dylib`，仅导出 `napi_*`/`node_api_*`，`jsc_smoke` 端到端跑通。

---

## 8. 风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| V8 升级破坏性变更 | patch 失效、API 改名 | 钉死 stable，升级走 PR + 全平台 CI |
| iOS jitless 性能差 | 调用方体感慢 3–10× | 文档明示；提供 Sparkplug-only 备选构建（若 V8 版本支持） |
| `use_custom_libcxx=false` 与宿主 ABI 冲突 | 运行时 crash | 文档强约束宿主 NDK / Xcode 版本；CI 在多个版本上跑 |
| Android NDK 版本飘移 | 编译失败 | DEPS 钉 NDK 版本；setup 脚本校验 |
| 包体积大（30–40MB / arch stripped） | App 体积压力 | 提供裁剪开关（i18n、WASM 等）；文档说明 |
| 首次构建慢（5GB 下载 + 30min+） | 开发体验差 | ccache + CI artifact cache；浅 clone |
| Windows depot_tools 工具链坑 | 本地构建失败 | 强制 `DEPOT_TOOLS_WIN_TOOLCHAIN=0`，文档详述前置 |
| 符号意外泄漏 | 宿主进程符号污染 | CI gate 校验；review 时人工抽查 |

---

## 9. 开放问题（待 M1 启动前最终确认）

无 —— 所有关键决策已在第 3 节锁定。后续若出现：

- V8 stable tag 的具体值（M1 启动时定，默认取启动当周最新 stable）
- Android NDK 版本（默认跟 V8 当前 stable 推荐版本）
- CI 平台（GitHub Actions 还是其他）

由实施过程中按惯例选用，必要时再回到本文档修订。

---

## 10. 附录：参考资料

- V8 Build 文档：<https://v8.dev/docs/build>
- V8 Embedder Guide：<https://v8.dev/docs/embed>
- Node-API 规范：<https://nodejs.org/api/n-api.html>
- nodejs/node-api-headers：<https://github.com/nodejs/node-api-headers>
- microsoft/v8-jsi：<https://github.com/microsoft/v8-jsi>
- GN 语言参考：<https://gn.googlesource.com/gn/+/main/docs/reference.md>
- Prefab v2 规范：<https://google.github.io/prefab/>
