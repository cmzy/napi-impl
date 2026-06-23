# napi-impl Fast Call 接口技术方案

> **范围声明（重要）**：本文档**只覆盖 napi-impl 内部的改动**——新增一组 fast-call 的 C ABI、其 V8 实现、
> 多引擎回退实现、符号导出、自测与发布打包。**任何消费方/引擎侧的接入**（注册钩子、codegen、绑定层改造、
> wrap 改造、端到端一致性验收等）**不在本文档**，须另起项目跟踪。本文档与下游的契约边界 = §4 的公开头
> `include/napi/fast_call.h` 与 §5 的 ABI 约定；napi-impl 只负责「提供并保证这组接口」，不负责「谁怎么用」。
>
> 状态：**方案定稿（决策已锁，待实现）**。

## 0. 已锁决策（napi-impl API 设计取定）

| # | 决策 | 取定 |
|---|---|---|
| D1 | **internal field 给法** | `napi_define_class` 实例**默认预留 1 个 internal field**——纯内部实现改动、**非 API 变更**、向后兼容（不 fast-wrap 就空着）。调用方 `napi_define_class` 调用零改动 |
| D2 | **fast 回调首参形态** | **receiver 作不透明首参**，调用方用 `napi_fast_unwrap` 取 native 指针（napi-impl 不注入 `void* ctx`）——支持任意签名、**napi-impl 无需维护 trampoline 目录** |
| D3 | **多引擎兜底** | **回退实现为主**（所有后端导出同名符号，非 V8 等价 slow）**＋ 编译门控可叠加**（`NAPI_HAS_FAST_CALL` 仅 V8 定义） |
| D4 | **可移植头归属** | 放 `include/napi/fast_call.h`（跨引擎同一 include），区别于纯 V8 专属的 `include/napi_v8/*.h` |
| D5 | **支持的 fast 形参种类** | 标量（bool/int32/uint32/int64/uint64/float32/float64）+ `kV8Value` 不透明句柄（receiver / 对象 / TypedArray / AB / 泛值）+ 末位 options。其中 TypedArray/AB 的**字节**经 helper 暴露（见 F5/F6） |

## 1. 调研事实基线（设计的硬约束，均为 V8/napi-impl 内部事实）

| # | 事实 | 来源 |
|---|---|---|
| F1 | `napi_create_function` 走 `v8::Function::New`，**挂不了 CFunction**；fast 必须经 `FunctionTemplate` | `src/v8/function.cc:46`、`js_native_api_v8_impl.h:286` |
| F2 | 只有 `FunctionTemplate::New(...,const CFunction*,...)` / `NewWithCFunctionOverloads` 能挂 fast | `third_party/v8/include/v8-template.h:587/598` |
| F3 | `CFunction(address, const CFunctionInfo*)` 可**手工构造**（不必走模板 `Make`）→ 可据运行期描述符拼装 | `v8-fast-api-calls.h:427` |
| F4 | fast 形参型枚举：`kVoid/kBool/kUint8/kInt32/kUint32/kInt64/kUint64/kFloat32/kFloat64/kPointer/kV8Value/kSeqOneByteString/kAny`，末位可 `kCallbackOptionsType` | `v8-fast-api-calls.h:224/253` |
| **F5** | **V8 14.2 fast API 不支持 TypedArray/ArrayBuffer 作 fast 形参**（头注「To be supported types: TypedArrays and ArrayBuffers」），`FastApiTypedArray`/`SequenceType` 在本版**已无** | `v8-fast-api-calls.h:166-168`（grep 无 `FastApiTypedArray`） |
| F6 | `ArrayBufferView::GetContents(MemorySpan storage)` / `CopyContents(dest,len)`：**无需 `Buffer()`、不建 Local** 取字节——off-heap 返回零拷贝视图，小 on-heap 拷进调用方 storage | `v8-array-buffer.h:441/451` |
| F7 | napi_wrap 用**私有属性**存指针；`napi_define_class` 实例**无 internal field** → fast 回调读不了私有属性 | `wrap.cc`、`js_native_api_v8_impl.h:385` |
| F8 | 扩展惯例：`extern "C"` + `include/napi_v8/*.h` + `src/v8/*.cc` + `gn/exports/*.{def,exp,lds}`；V8 走 GN、非 V8 走 CMake（`NAPI_ENGINE`） | `include/napi_v8/sab.h`、`src/v8/BUILD.gn`、`CMakeLists.txt` |
| F9 | int64/uint64 已是 fast 支持型（IDL unsigned long long 截断语义）→ 偏移类参数可 fast | `v8-fast-api-calls.h:150-156` |
| **F10** | **V8 14.2 已移除 per-call fallback**：`FastApiCallbackOptions` 仅余 `isolate`+`data`，**无 `fallback` 字段**；`CFunction::MakeWithFallbackSupport` 仅存于陈旧注释、API 已无。fast 回调**不能**在体内请求回落 slow | `v8-fast-api-calls.h:454-469`（struct）、grep 无 `MakeWithFallbackSupport` 声明 |

**F5 是范围决定性事实**：TypedArray/ArrayBuffer**不能**做成 V8 原生 fast 形参。但 F6 给了「在 fast 回调内
handle-free 读字节」的口子。于是 napi-impl 把字节型入参统一表示成 `kV8Value` 不透明句柄，再提供 fast-safe
helper 提字节——见 §2。

## 2. 核心设计：万物皆「不透明指针句柄 + fast-safe helper」

V8 fast 形参只允许「标量 + `kV8Value`(单指针) + options」。napi-impl 把**一切非标量**——receiver、对象入参、
TypedArray/AB 入参、options——都表示成**单指针宽的不透明句柄**，调用方用 napi-impl 提供的 **fast-safe helper**
解读。三条关键收益：

1. **调用方 fast 回调的 C 签名永远是「标量 / 单指针」组合** → 逐位匹配某个合法 `CFunction` 签名
   → 调用方那份函数**本身就能当 `CFunction` 用，napi-impl 不必插任何转换 trampoline**（含字节型入参也不用）。
2. **调用方全程 v8-free**：句柄是 napi-impl 的不透明 typedef，解读靠 helper，永不出现 `v8::Local` 等 v8 类型。
3. **napi-impl 据运行期「签名描述符」手工拼 `CFunctionInfo`**（F3）：描述符里非标量一律记 `napi_fast_value`
   （→ `kV8Value`），napi-impl 无需区分它语义上是对象还是数组——区分纯在调用方（调哪个 helper）。

**关于 fallback（F10，重要）**：V8 14.2 已无 per-call fallback——fast 回调**不能在体内请求回落 slow**。
因此「某方法是否走 fast」是**注册期决策**（`sig=NULL` ⇒ 纯 slow）；fast 回调须对其签名所允许的输入**是
全函数（total）**：能纯原生处理完（WebGL 语义多为 glError+no-op，天然满足），需要抛异常/分配的方法**注册期就
留 slow**。非法输入（理应被上层类型保证挡掉）在 fast 体内只能原生 no-op，不能回落。

最小示意（一个把字节写入某 native sink 的 fast 方法；与具体引擎语义无关）：

```c
// 调用方（v8-free）。对应 sig = {ret=void, args=[receiver, uint32, value, uint32], wants_options=false}
void write_fast(napi_fast_recv recv, uint32_t target, napi_fast_value src, uint32_t flags) {
    void* ctx = napi_fast_unwrap(recv);                  // 读 internal field 0（fast-safe）
    uint8_t scratch[64]; void* data; size_t len;
    if (!ctx || !napi_fast_get_buffersource(src, scratch, sizeof scratch, &data, &len, NULL))
        return;                                          // 非法输入：原生 no-op（无 per-call fallback）
    native_write((NativeSink*)ctx, target, data, len, flags);   // 纯原生，零装箱
}
```

## 3. 接收方解包机制（D1 + D2 落地）

**问题（F7）**：fast 回调禁止慢操作，读不了私有属性 → 拿不到 native 指针。
**解法**：把 native 裸指针存进 **V8 internal field**，`GetAlignedPointerFromInternalField(0)` 是官方 fast-safe 读法。

- **D1**：`napi_define_class` 内 `tpl->InstanceTemplate()->SetInternalFieldCount(2)`（field 0=native、field 1=type
  tag；内部实现改动，非 API 变更）。
- **`napi_fast_wrap`** = 正常 wrap（私有属性，供 slow 的 `napi_unwrap`）**＋** 把 native 存 field 0、type_tag 存
  field 1。两处的 native 指向同一指针 → slow 与 fast 取到同一 ctx。
- **`napi_fast_unwrap(recv, tag)`** / **`napi_fast_value_unwrap(val, tag)`**：先 `IsObject` 守卫，再读 field 0；
  非对象 / field count 为 0 / （tag≠NULL 时）field 1 的 tag 不匹配 → 返回 NULL。
- **构造期初始化**：每个实例构造时 field 0/1 先置 NULL（`InvokeCallback` 内，`IsConstructCall` 守卫），
  避免读未初始化 aligned-pointer field 的 UB（§7.1 BUG-1）。
- **`napi_remove_wrap` 清场**：移除 wrap 时一并把 field 0/1 清 NULL，防 remove 后仍在世的 JS 对象经 fast 解出
  野/已释放指针（§7.1 BUG-3）。

**调用方契约（信任边界，已由 type-tag 强化）**：fast 命中时 receiver / 对象入参的类型由 **(a) 调用方上层规整**
**＋ (b) field 1 的 type-tag 原生校验** 双保险。type-tag 是关键——本引擎跑**不可信网页 JS**，页面能用
`fn.call(异类handle)` 绕过 JS facade，单靠 (a) 不够；(b) 让异类 receiver 在 native 侧即被拒（解出 NULL），调用方
`if(!ctx) return;` 即安全。每类用一个稳定 token（如 static 地址）作 tag。

## 4. 新增 API（全部加法）

```c
// include/napi/fast_call.h —— 引擎中立声明；每个后端都导出这些符号（非 V8 = 回退实现）
#ifndef NAPI_FAST_CALL_H_
#define NAPI_FAST_CALL_H_
#include "napi/js_native_api.h"
#include "napi/js_native_api_types.h"
#ifdef __cplusplus
extern "C" {
#endif

// 能力探针：真正走 fast 的后端编译期注入 -DNAPI_HAS_FAST_CALL=1（仅 V8）。调用方【可选】用它编译门控；
// 【非必须】——不门控也对（回退实现保证行为正确）。

// 不透明句柄（ABI 与引擎原生表示一致，调用方只当 token 透传）：
typedef struct napi_fast_recv__*    napi_fast_recv;     // fast 回调 receiver（实参 0）   ≡ v8::Local<Object>
typedef struct napi_fast_value__*   napi_fast_value;    // fast 回调里的对象/数组等 JS 值  ≡ v8::Local<Value>
typedef struct napi_fast_options__* napi_fast_options;  // fast 回调末位 options          ≡ v8::FastApiCallbackOptions&

// fast 形参/返回的 C 类型标签（一一映射 CTypeInfo::Type，F4）。
// 注意：对象 / TypedArray / 泛 JS 值统统记 napi_fast_value（=kV8Value）——区分在调用方调哪个 helper。
typedef enum {
  napi_fast_void = 0,
  napi_fast_bool,
  napi_fast_int32,  napi_fast_uint32,
  napi_fast_int64,  napi_fast_uint64,   // IDL (unsigned) long long 截断语义（F9）
  napi_fast_float32, napi_fast_float64,
  napi_fast_pointer,    // 裸 void*（进阶）
  napi_fast_receiver,   // JS `this`（约定实参 0；映射 kV8Value）
  napi_fast_value,      // 对象/TypedArray/AB/泛值（映射 kV8Value）
} napi_fast_type;

typedef struct {
  napi_fast_type        return_type;
  uint32_t              arg_count;       // 含 receiver，不含 options
  const napi_fast_type* arg_types;       // 长度 = arg_count；arg_types[0] 约定 napi_fast_receiver
  bool                  wants_options;   // true ⇒ fast 回调末位多收 napi_fast_options（取 data 用）；默认 false
} napi_fast_signature;

// 【C-1】建「fast + slow 双入口」JS 函数。
//   slow_cb : 必填。标准 napi 回调——非 fast 引擎唯一路径，亦 V8 deopt/类型不匹配/fallback 的回落路径。
//   sig/fast_fn : fast 签名 + 调用方 fast 函数指针（签名须匹配 sig，§5）。任一 NULL ⇒ 纯 slow。
//   data    : 绑定数据；slow 经 napi_get_cb_info 取，fast 经 napi_fast_options_get_data 取。
// 契约：sig/fast_fn 为 NULL 或后端无 fast 能力时，结果可观测地等价 napi_create_function(slow_cb,data)。
NAPI_EXTERN napi_status NAPI_CDECL napi_create_fast_function(
    napi_env env, const char* utf8name, size_t length, napi_callback slow_cb,
    const napi_fast_signature* sig, const void* fast_fn, void* data, napi_value* result);

// 【C-1’，可选】按实参个数重载（V8 NewWithCFunctionOverloads；仅按 arg count 决议）。为字节型重载方法保留；
// 调用方若已在上层决议成单态入口，则用不到。
typedef struct { napi_fast_signature sig; const void* fast_fn; } napi_fast_overload;
NAPI_EXTERN napi_status NAPI_CDECL napi_create_fast_function_overloads(
    napi_env env, const char* utf8name, size_t length, napi_callback slow_cb,
    const napi_fast_overload* overloads, size_t overload_count, void* data, napi_value* result);

// 【C-2】fast-safe wrap：等价 napi_wrap，且把 native 存入 internal field 0、type_tag 存入 field 1（fast 可读）。
// type_tag = 每类一个稳定 token（如某 static 的地址）；解包按 tag 拒绝异类 receiver（防 fn.call(异类) 型混淆）。
// NULL = 不打 tag。native/type_tag 须 ≥2 字节对齐（存为 V8 aligned pointer，低位保留）——不对齐返 napi_invalid_arg。
// napi_unwrap 照常可用。非 fast 引擎：等价 napi_wrap（忽略 tag）。
NAPI_EXTERN napi_status NAPI_CDECL napi_fast_wrap(
    napi_env env, napi_value js_object, void* native, const void* type_tag,
    napi_finalize finalize_cb, void* finalize_hint, napi_ref* result);

// 【C-2】fast 回调内解包（读 internal field，O(1)，fast-safe）。非对象 / 无 field / tag 不匹配 → NULL（调用方须当
// 「错误/缺失 receiver」直接退出、不解引用）。expected_type_tag 传与 wrap 相同的 tag；NULL = 跳过类型校验。
NAPI_EXTERN void* NAPI_CDECL napi_fast_unwrap(napi_fast_recv recv, const void* expected_type_tag);
NAPI_EXTERN void* NAPI_CDECL napi_fast_value_unwrap(napi_fast_value v, const void* expected_type_tag);
NAPI_EXTERN bool  NAPI_CDECL napi_fast_value_is_nullish(napi_fast_value v); // null/undefined?

// 【C-2】字节视图（基于 F6 的 GetContents/CopyContents，fast-safe）：
//   把 TypedArray/ArrayBuffer/DataView 字节暴露给 fast 回调。off-heap 零拷贝；小 on-heap 拷进 scratch。
//   out_elem（可空）回写元素类型供区分。非 buffersource → 返回 false。
typedef enum { napi_fast_bs_unknown=0, napi_fast_bs_i8, napi_fast_bs_u8, napi_fast_bs_u8c,
  napi_fast_bs_i16, napi_fast_bs_u16, napi_fast_bs_i32, napi_fast_bs_u32,
  napi_fast_bs_f32, napi_fast_bs_f64, napi_fast_bs_i64, napi_fast_bs_u64, napi_fast_bs_arraybuffer
} napi_fast_bs_type;
NAPI_EXTERN bool NAPI_CDECL napi_fast_get_buffersource(
    napi_fast_value v, void* scratch, size_t scratch_len,
    void** out_data, size_t* out_byte_length, napi_fast_bs_type* out_elem);

// 【C-2】options 访问器（fast-safe）。仅当 sig.wants_options=true 时 fast 回调才收到 options。
// 取 napi_create_fast_function 绑定的 data（= options.data 内 External 所裹）。
// 注：V8 14.2 无 per-call fallback（F10），故不提供 set_fallback。
NAPI_EXTERN void* NAPI_CDECL napi_fast_options_get_data(napi_fast_options opts);

#ifdef __cplusplus
}
#endif
#endif  // NAPI_FAST_CALL_H_
```

> **进阶逃生口（可选）**：另出 V8 专属 `napi_v8_create_fast_function_raw(env,name,slow,const v8::CFunction*,data,&out)`
> 放 `napi_v8/`，给本就 v8-aware、想自己拼 `CFunction` 的调用方。不可移植。

## 5. fast 回调形态与 ABI 契约（napi-impl 对调用方的约定）

采 **receiver-passthrough（D2）**：调用方那份函数**本身就是** `CFunction` 指向的地址，napi-impl 不插 trampoline。
其 C 签名须与 V8 fast ABI 逐位对齐，靠不透明 typedef 做到 v8-free：

| 调用方写的形参型 | sig 标签 | napi-impl 内部 V8 fast ABI | ABI |
|---|---|---|---|
| `napi_fast_recv`（实参 0）| `napi_fast_receiver` | `v8::Local<v8::Object>` | 单指针 |
| `int32_t/uint32_t/int64_t/uint64_t/float/double/bool` | 对应标量 | 同型 | 同型 |
| `napi_fast_value`（对象/数组/泛值）| `napi_fast_value` | `v8::Local<v8::Value>` | 单指针 |
| `napi_fast_options`（末位，仅 `wants_options`）| （置位时追加）| `v8::FastApiCallbackOptions&` | 引用=指针 |

ABI 对齐论证：napi-impl 编译时持真 v8 头、调用方不持。fast 调用中 receiver/value 以机器指针传入、引用即指针、
标量同型；调用方拿到的实参正是 V8 传入的位表示，原样回传给 helper，helper 内 `reinterpret_cast` 回 v8 类型。
**调用方形参型须与 sig 一致**。

napi-impl 支持的入参种类（调用方据此组合签名）：
- **纯标量**：bool / int32 / uint32 / int64 / uint64 / float32 / float64。
- **对象/null 入参**：`napi_fast_value` + `napi_fast_value_unwrap`（null → NULL）。
- **字节入参（TypedArray/AB/DV）**：`napi_fast_value` + `napi_fast_get_buffersource`（依赖 F6，见 §7 风险点）。
- **重载**：上层决议成单态入口（每入口单签名，首选）；或用 `napi_create_fast_function_overloads` 按 arg count 决议。
- **返回对象/字符串/需分配**：fast 返回型不支持 → 这类方法只能纯 slow（`sig=NULL`）。

## 6. 多引擎策略（D3）

**(A) 回退实现（主）**：`napi_create_fast_function*` / `napi_fast_wrap` / `napi_fast_unwrap` /
`napi_fast_value_*` / `napi_fast_get_buffersource` / `napi_fast_options_*` **加进每个后端导出表**
（`gn/exports/napi_v8.*` 与 `napi_hermes.*` …）。
- V8 后端：真实现（`src/v8/fast_call.cc`）。
- 非 V8 后端：共享回退（`src/common/fast_call_fallback.cc`）——`create_fast_function*` 忽略 fast 参数、等价 slow
  建函数；`fast_wrap`≡`wrap`；`fast_unwrap`/`value_unwrap`/`get_buffersource` 返 NULL/false（这些引擎上 fast 回调
  不会被调）；options 访问器 no-op。
- 由此：**同一份调用方源码在任何后端都能编译链接**，行为正确（非 V8 退 slow）。

**(B) 编译门控（可叠加）**：V8 后端编译期 `-DNAPI_HAS_FAST_CALL=1`（GN `napi_flags` / 各后端 CMake flag），
非 V8 不定义。供想在源码层剔除 fast 代码的调用方可选使用。

## 7. 正确性红线与风险

- **slow 永远在且语义须等价**：V8 可在任意点不走 fast（未优化/去优化/类型不符/receiver 非预期）→ 走 `slow_cb`。
  napi-impl 的保证是「fast 与 slow 取到**同一 native 指针**」（§3）；「让 fast_fn 与 slow_cb 语义等价」是**调用方**
  的责任（napi-impl 无法校验）。
- **无 per-call fallback（F10）**：V8 14.2 fast 回调**不能在体内回落 slow**。故「是否 fast」是**注册期**决策
  （`sig=NULL` ⇒ slow-only）；fast 回调须对其签名输入是全函数（total），需抛异常/分配的方法注册期就留 slow。
- **fast 内禁忌**：分配 JS 对象、抛异常、回 JS、建 HandleScope。有这类需求的方法 → 注册期留 slow（不传 sig）。
- **整型收窄**：`CFunctionInfo` 用默认截断（`Int64Representation::kNumber`），不加 EnforceRange/Clamp（范围规整
  是调用方上层的事）。
- **✅ 风险点 1（字节视图 fast-safe 性）——已在真 V8 14.2 验证通过**：`test/fast_call_smoke.cc` 的 `sumf32` 在
  TurboFan 优化后的 fast 路径里经 `napi_fast_get_buffersource`（`ArrayBufferView::GetContents`）读 Float32Array
  字节，结果正确、无崩溃（`mac-x86_64-release`，2026-06-22）。故字节入参可走 fast。若未来某 V8 版本回归此点，
  退路仍是把该类方法在注册期留 slow（`sig=NULL`），仅字节入参降级。
- **✅ 风险点 2（internal field 广面）——已验零回归**：D1 给所有 define_class 实例 +1 槽。js-native-api 套件
  with/without 本改动均 **47/50**（同 3 个既有 finalizer/cannot-run-js 偏差），对象 wrap/constructor 全过。
- **风险点 3（kV8Value 解包信任）**：见 §3 调用方契约；helper 防御 null 与零 field；类型保证在调用方。

### 7.1 二次审查发现并已修的 bug（2026-06-22，真机验证）

- **🐛 BUG-1（严重，已修）：未初始化 internal field → UB 野指针。** 起初仅 fast_wrap 时 `SetAlignedPointerInInternalField`，
  但**所有** define_class 实例都被 D1 预留了 field 0；一个**没经 fast_wrap** 的实例（或任意 define_class 实例作对象
  入参）传进 fast fn 时，`napi_fast_unwrap`/`value_unwrap` 对**未设置**的 aligned-pointer field 读取 = **V8 明文 UB**
  （v8-object.h:521「must have been set ... everything else leads to undefined behavior」）。实测返回**非空野指针**
  （`unwrap(raw)->null?=0`），调用方解引用即崩。**修复**：在共享构造 trampoline（`FunctionCallbackWrapper::InvokeCallback`）
  里，对 construct call 且实例有保留 field 的，先把 field 0 置 `nullptr` 再跑用户 ctor（ctor 内 fast_wrap 仍覆盖）；
  `IsConstructCall()` 守卫使热点方法路径零成本。修后 `unwrap(raw)->null?=1`，js-native-api 仍 47/50 零回归。
- **🐛 BUG-2（内存安全，已修）：`napi_fast_get_buffersource` scratch 溢出。** `GetContents` 把 on-heap TypedArray
  整体拷进调用方 `scratch`（上限 = V8 `typed_array_max_size_in_heap`，默认 64B）；调用方给 < 64B 的 scratch 会**栈
  溢出**。**修复**：`scratch==NULL || scratch_len < 64` 直接返 false；头文件标注「scratch 必须 ≥ 64B 且非空」。
- **✅ 内存泄漏——已验无泄漏。** `FASTCALL_CHURN=50000`（每个 fast fn = FastFnHolder + CallbackBundle + 2 个
  Reference + FunctionTemplate，逐个建后 drop + GC + drain finalizers）跑 macOS `leaks`：**0 leaks / 0 bytes**。
  holder 经挂在 cbdata External 上的 runtime finalizer 回收（与 CallbackBundle 同款，已证不泄漏）。
- **✅ 覆盖补全（真机过）：** `wants_options=true` 经 `napi_fast_options_get_data` 在 fast 路径取回绑定 data；
  `napi_create_fast_function_overloads` 双 arity 经 V8 按实参数分发——均断言通过。

### 7.1.1 三度审查：receiver 处理硬化（2026-06-22，真机验证；针对不可信网页 JS）

本引擎跑**不可信网页 JS**，页面能用 `.call()`/`.apply()` 绕过 facade 的 JS 壳、任意控制 fast 方法的 receiver；
而 TurboFan 走 fast 看被调函数、不看 receiver。下面三个 receiver 缝隙因此具安全性，已全部修复 + 真机断言：

- **🐛 #1（类型混淆，已修）：fast 方法无 `Signature` → 异类 receiver 误解包。** slow 的 define_class 方法绑了
  `v8::Signature`，fast 没有。`gl.drawArrays.call(某WebGLBuffer,…)` 会把 buffer 的 native 指针当 context 用。
  **修复**：每类一个 **type-tag** 存 field 1，`napi_fast_wrap(…, tag, …)` 写、`napi_fast_unwrap(recv, tag)` 校验；
  tag 不符 → NULL。真机：同形异 tag 实例在优化后的 fast 路径上被拒（`wrong-tag → 1`）。
- **🐛 #2（崩溃，已修）：`napi_fast_unwrap` 缺 `IsObject` 守卫**（`value_unwrap` 有，不对称）。`m.call(42)` →
  对 Smi 调 `InternalFieldCount` → UB/崩。**修复**：解包统一走 `UnwrapNative`，先 `IsObject`。真机：String receiver
  走 fast 不崩、解出 NULL。
- **🐛 #3（use-after-remove，已修）：`napi_remove_wrap` 不清 internal field。** 只删私有属性、field 0 残留；remove
  后仍在世的 JS 对象经 fast 解出**已释放指针** → UAF。**修复**：`Unwrap(RemoveWrap)` 一并清 field 0/1。真机：
  remove 前 fast 槽=native、remove 后=NULL。
- **附带修：tag 对齐 footgun。** `static const char` tag（字节对齐）传进 `SetAlignedPointerInInternalField` 会 V8
  **fatal「Unaligned pointer」**。**修复**：`napi_fast_wrap` 对 native/tag 低位非零**优雅返 `napi_invalid_arg`**（不再 fatal）；
  头文件要求 tag ≥2 对齐。
- **代价**：define_class 实例 field 1→2（再 +1 指针/实例）；解包多一次 tag load+比较（仅 expected_tag≠NULL 时）。
  js-native-api 仍 **47/50 零回归**；churn 50k 仍 **0 泄漏**。

### 7.2 已知次要项（非 bug，权衡后保留）

- **`InternalFieldCount()` 守卫成本**：解包每次多一次 map 读，用于安全处理「0 field 的普通对象作入参」与
  「未设置 field」两种情形——故**不能省**（省了即 BUG-1 类 UB）。相对 slow 路径仍快一个量级。
- **`IsConstructCall()` 分支**：BUG-1 修复给所有 napi 函数调用加了 1 个分支；非 construct（即全部方法调用）直接
  短路，成本可忽略；已验零回归。
- **`ElemKind` 类型链**：`out_elem` 传 NULL 时整段跳过（已门控）；需要时最坏 ~11 次廉价谓词。
- **部分校验**：`native`/`type_tag` 是否 2 对齐**已在 `napi_fast_wrap` 校验**（不对齐返 `napi_invalid_arg`，§7.1.1）。
  `arg_types[0]` 是否 receiver、重载 arg_count 是否唯一仍属调用方契约（文档已述），未加运行时校验（热点零成本）。

## 8. 文件 / 符号清单（napi-impl 内）

**V8 后端（新增/改）**：
- 新增 `include/napi/fast_call.h`（§4）。
- 新增 `src/v8/fast_call.cc`（C-1/C-1’/C-2 全套实现）→ 追加进 `src/v8/sources.txt`。
- 改 `src/v8/function.cc`：`napi_define_class` 内 `InstanceTemplate()->SetInternalFieldCount(1)`（D1）。
- 改 `gn/exports/napi_v8.{def,exp,lds}`：追加新符号（`napi_create_fast_function`、`napi_create_fast_function_overloads`、
  `napi_fast_wrap`、`napi_fast_unwrap`、`napi_fast_value_unwrap`、`napi_fast_value_is_nullish`、
  `napi_fast_get_buffersource`、`napi_fast_options_get_data`）。
- GN `napi_flags`（或 `:external_config`）注入 `-DNAPI_HAS_FAST_CALL=1`。

**非 V8 后端（新增）**：
- 新增 `src/common/fast_call_fallback.cc`（共享回退）→ 进各后端 sources。
- 改 `gn/exports/napi_hermes.{def,exp,lds}`（及未来 jsc/quickjs）：追加**同名**符号。

**发布打包**：重打包后 `dist/thrid_part/napi_v8/<os>` 多出 `include/napi/fast_call.h` 与上述导出符号；
`scripts/fetch_napi_impl.py` 流程不变（下载+校验+解压）。

## 9. 测试（napi-impl 自测，`test/js-native-api/fast_call/`）

- **V8**：注册 `double add(recv,a,b)` 的 fast+slow，紧循环触发 TurboFan 后断言走 fast（fast/slow 各自计数器对比）；
  逐项验：fast 命中 vs deopt 走 slow、`napi_fast_unwrap` 取值、对象入参 `napi_fast_value_unwrap`、null 入参、
  `napi_fast_get_buffersource`（off-heap 零拷贝 + 小 on-heap 拷贝两路）、`wants_options` 取 data、
  **风险点 1 的 fast 上下文压测**。
- **回退后端（Hermes）**：同一份测试源码编译运行，结果与纯 slow 一致（行为等价断言）。
- **回归**：现有 `test/js-native-api` 全绿（重点 wrap/unwrap/define_class/finalize——确认 D1 的 internal field 无副作用）。

## 10. 分期落地（napi-impl 范围）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **S1 ✅** | V8 后端：`napi/fast_call.h` + `src/v8/fast_call.cc`（C-1/C-1’/C-2 全套）+ define_class 内部字段 + 导出符号 + 自测 | **已完成**：`test/fast_call_smoke.cc` 在真 V8 14.2 全绿（fast 命中 fast=5/slow=2、unwrap/value_unwrap/get_buffersource 均 OK，含风险点 1 验证）；js-native-api **47/50 零回归**（with/without 同 3 既有偏差） |
| **S2 ✅** | 非 V8 共享回退 `fast_call_fallback.cc`（`#if !NAPI_HAS_FAST_CALL` 自守卫）+ Hermes 导出符号 | **已完成**：带/不带宏两种编译均通过；V8 build 中为空 TU（无重复符号）；Hermes 运行时验证待 Hermes 工具链 |
| S3 | 重打包 `napi_v8` 预编包（含新头与符号）；版本/校验更新。打包脚本（cmake/apple/android）已 `copytree(include/napi)` 自动带 `fast_call.h`，apple umbrella 已补 | 下游可 `find_package` 到新符号 |

## 11. 不在本文档范围（须另起项目）

以下属**引擎/消费方**侧，napi-impl 不涉及，单列于此仅作交接指引：
- 安装 fast 注册钩子、把方法表翻成 fast（codegen 的 fast 表）。
- 绑定层/wrap 层改调 `napi_fast_wrap`、fast 回调体的编写与 GL/业务语义。
- 端到端一致性套件验收、微基准对比、灰度策略。

napi-impl 与之的唯一接口面 = `include/napi/fast_call.h`（§4）+ §5 的 ABI 约定 + §7 的语义红线。
