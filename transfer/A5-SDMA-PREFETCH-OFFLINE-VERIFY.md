# A5 SDMA Prefetch 离线环境验证手册

版本：v1.1（2026-09-15；v1.1 新增 §2.4 fork 仓下载方式）
目标：在离线（无外网）950 环境上，端到端验证 AIV SDMA direct-drive prefetch（A5/Ascend950）的代码修改。
适用：验证人按步骤执行，任何一步不符合"期望"即停止并按第 9 章反馈。

---

## 1. 修改方案总结

### 1.1 目标

打通并验证 A5（Ascend950）上 `pl.prefetch.*`（GM→L2 异步预取）的完整链路。
此前 A5 不可用的根因有两个，均已修复：

1. **host 侧**：simpler runtime 的 a5 平台没有实现 SDMA workspace 供给（4 个桩函数直接返回失败）。
2. **device 侧**：pto-isa 的 A5 STARS v2 SQE 写入函数未初始化全部字段（qos 未设、header 位、预留位为内存垃圾值），硬件可能拒绝该 SQE。

### 1.2 三个仓的修改内容

| 仓 | 位置 | 修改 | 状态 |
|---|---|---|---|
| **simpler**（runtime 子模块，`hw-native-sys/simpler` @ `39ce891d`） | `src/a5/platform/onboard/host/comm_hccl.cpp` | 4 个 `dma_workspace_*` 桩函数 → 完整实现（镜像 a2a3）：`ensure_sdma_workspace()` → `SdmaWorkspaceManager::Init()` → dlsym `aclnnShmemSdmaStarsQuery` → 48 条 STARS 流 + 16KB workspace | 未提交，补丁 `0001-a5-sdma-host-plumbing.patch` |
| 同上 | `src/common/platform/include/common/dma_workspace.h` | 注释更新（a5 支持说明） | 同上 |
| **pypto**（superrepo，分支 `feat/aiv-sdma-prefetch` @ `62cbe0f7`） | `tests/st/runtime/ops/test_prefetch_async.py` | `@pytest.mark.platforms` 从 `("a2a3")` 放开为 `("a2a3", "a5")` + docstring | 未提交，补丁 `0002-a5-prefetch-st-test.patch` |
| **pto-isa**（fork `lulu-Coding/pto-isa`，分支 `fix/a5-stars-v2-sqe-init` @ `ebb1d62e`） | `include/pto/comm/async/sdma/sdma_cmo_intrin.hpp` | `AddOneCmoSqe` A5 分支：SQE **全字段显式初始化**（qos=6 HCCL、wrCqe=1 其余 header 位=0、sssv/dssv/sns/dns=1、sro/dro/stride/ie2/compEn=0、流 id/offset/sqeId/mpam=0、预留位清零、taskId 收窄为 uint16_t） | 已推送 fork，待上游 PR；补丁 `0003-fix-a5-sqe.patch` |
| 同上 | `include/pto/comm/async/sdma/sdma_async_detail_basic.hpp` | `AddOneMemcpySqe` A5 分支：同样处理 | 同上 |

**说明**：
- superrepo 里的 prefetch→PTOAS→codegen→backend 注入链（`pto_ops_prefetch.cpp`、`pto_codegen.cpp` 的 hidden `__pypto_sdma_workspace` 参数注入、`pto_backend.py`）是**上游已有基础设施**，本次未改；我们只放开了 a5 测试标记。
- pto-isa 的 a2a3 分支硬件已验证，**未改动**。
- pypto 分支上已提交的 `62cbe0f7`（sim guide 文档）与本任务无关。
- 本机（Windows）工作区另有 `cmake/libbacktrace.cmake`、`include/pypto/core/any_cast.h`、`common.h` 三处未提交改动，**属于 Windows 本地编译兼容，不带到 Linux**。

### 1.3 E2E 链路（验证对象）

```
pl.prefetch.async_prefetch        (Python DSL)
  → IR prefetch.*                 (tests/ut/ir/operators/test_prefetch_ops.py)
  → PTOAS pto.tprefetch_async
  → pto_backend.py 注入 __pypto_sdma_workspace 隐藏参数
  → host: ensure_dma_workspace_provisioned() → comm_hccl.cpp 4 个 C 函数 ←【补丁 0001】
      → aclnnShmemSdmaStarsQuery (libopapi.so, AICPU op)
  → ccec 编译 kernel（-I runtime/build/pto-isa/include）
      → TPrefetchAsync.hpp → __sdma_cmo_prefetch → AddOneCmoSqe ←【补丁 0003】
      → BeginSdmaPost + AddOneCmoSqe + FinishSdmaPost(AddOneMemcpySqe flag SQE)
      → PersistSqTails + RingDoorbell(sq_reg_base+0)
  → 硬件 STARS v2 CMO → L2 预取
```

### 1.4 关键设计事实（排障时对照）

- CMO prefetch opcode = **6**（SHMEM `CMO_TYPE_PREFETCH`，硬件验证值）。guide 文档 §1.4 写的 0 是**错的**。
- SQE 布局与 SHMEM PR #459 字节级一致；唯二差距（qos、未初始化字段）由补丁 0003 补齐。
- **pin 机制**：`runtime/pto_isa.pin` 是唯一版本源。resolver（`simpler_setup/pto_isa.py`）规则：本地 `runtime/build/pto-isa` 干净且 HEAD==pin → 直接复用（**零网络，离线唯一可行路径**）；否则从上游 GitHub/GitCode 重新 clone（**离线必失败**）。
- 上游 pto-isa main（`32b08bbe`）与 pin 基线（`5a4f74cb`）之间 include/ 已漂移 120 文件 +5193/-2156。**因此 fix 必须打在 pin 基线上**（0003 已验证对 `5a4f74cb` 干净可应用），不要直接用 fork main 分支。
- 换 pin 后必须重建 runtime：runtime 加载时校验烘焙的 `SIMPLER_PTO_ISA_BUILD_COMMIT` == 当前 pin。
- `PTO_ISA_ROOT` 环境变量已被上游废弃（#1403），**不能**用它绕过 pin。

---

## 2. 阶段〇：联网机准备产物（Windows 本机，PowerShell）

工作目录：`D:\Projects\github\pypto`。产物目录 `$T`（U 盘/共享盘拷这个目录）：

```
C:\Users\Administrator\AppData\Local\Temp\opencode\transfer\
```

### 2.1 已就绪的补丁（本次已生成，无需重做）

| 文件 | 大小 | 内容 |
|---|---|---|
| `0001-a5-sdma-host-plumbing.patch` | 6288 B | simpler：comm_hccl.cpp + dma_workspace.h（+86/-19） |
| `0002-a5-prefetch-st-test.patch` | 1109 B | superrepo：ST 测试放开 a5 |
| `0003-fix-a5-sqe.patch` | 5949 B | pto-isa：A5 SQE 全字段初始化（+81/-8） |

### 2.2 生成 git bundle（完整历史、离线自包含）

```powershell
$T = "C:\Users\Administrator\AppData\Local\Temp\opencode\transfer"

# 1) 补齐子模块（libbacktrace 当前未初始化；msgpack 重复 init 无害）
git submodule update --init 3rdparty/libbacktrace 3rdparty/msgpack-c

# 2) 检查子模块是否浅克隆（bundle 不支持 shallow）
git -C runtime rev-parse --is-shallow-repository
git -C 3rdparty/msgpack-c rev-parse --is-shallow-repository
git -C 3rdparty/libbacktrace rev-parse --is-shallow-repository
# 任何一个输出 true 时执行（本机有网）：
#   git -C <该路径> fetch --unshallow

# 3) 生成 bundle
git bundle create "$T\pypto.bundle" feat/aiv-sdma-prefetch
git -C runtime bundle create "$T\simpler.bundle" HEAD
git -C 3rdparty/msgpack-c bundle create "$T\msgpack-c.bundle" HEAD
git -C 3rdparty/libbacktrace bundle create "$T\libbacktrace.bundle" HEAD
git -C "C:\Users\Administrator\AppData\Local\Temp\opencode\pto-isa-fork" bundle create "$T\pto-isa.bundle" main fix/a5-stars-v2-sqe-init
```

### 2.3 产物清单（拷贝到离线机，如 `/data/transfer/`）

> ✅ **状态（2026-09-15）**：以下 5 个 bundle + 3 个补丁已全部生成，且阶段二全流程（§4.1–§4.3）已在 Windows 本机用这些产物完整彩排验证通过。

| 文件 | 用途 |
|---|---|
| `pypto.bundle` | superrepo 完整仓（分支 feat/aiv-sdma-prefetch），35.9 MB |
| `simpler.bundle` | runtime 子模块 @ 39ce891d，29.3 MB |
| `msgpack-c.bundle` | 3rdparty 子模块 @ 91990874，6.0 MB |
| `libbacktrace.bundle` | 3rdparty 子模块 @ 6f8310e，1.4 MB |
| `pto-isa.bundle` | pto-isa（含上游历史、pin 基线 5a4f74cb、fix 分支 ebb1d62e），23.9 MB |
| `0001-a5-sdma-host-plumbing.patch` | simpler 改动 |
| `0002-a5-prefetch-st-test.patch` | superrepo 改动 |
| `0003-fix-a5-sqe.patch` | pto-isa 改动 |
| 本文档 | — |

注意：若 pypto.bundle 体积过大，且离线机已有 pypto 仓（见附录 A），可只带 3 个补丁 + `pto-isa.bundle`。

### 2.4 产物获取：从 GitHub fork 仓直接下载（推荐）

产物已上传至 fork 仓 **`lulu-Coding/pto-isa`** 专用分支 **`offline-verify-transfer`**（`9e2262fb`）的 `transfer/` 目录，与本地母本完全一致（含 `SHA256SUMS` 校验文件）。该分支独立于 `main` 与 `fix/a5-stars-v2-sqe-init`，不影响后续 PR。

**方式一：git clone（一次全下，推荐）**

```bash
git clone --depth 1 -b offline-verify-transfer https://github.com/lulu-Coding/pto-isa.git pto-isa-transfer
mkdir -p /data/transfer
cp pto-isa-transfer/transfer/* /data/transfer/
cd /data/transfer && sha256sum -c SHA256SUMS
```

**方式二：wget 单文件（免 git，支持断点续传）**

```bash
mkdir -p /data/transfer && cd /data/transfer
base=https://raw.githubusercontent.com/lulu-Coding/pto-isa/offline-verify-transfer/transfer
for f in 0001-a5-sdma-host-plumbing.patch 0002-a5-prefetch-st-test.patch \
         0003-fix-a5-sqe.patch A5-SDMA-PREFETCH-OFFLINE-VERIFY.md SHA256SUMS \
         libbacktrace.bundle msgpack-c.bundle pto-isa.bundle pypto.bundle simpler.bundle; do
    wget -c $base/$f
done
sha256sum -c SHA256SUMS
```

说明：
- `sha256sum -c SHA256SUMS` **必须全部 OK** 再继续（防止下载中断/损坏）
- 若 github.com 访问不稳：clone 方式可挂代理 `git config --global http.proxy http://<proxy>:<port>` 后重试
- 无外网机器仍走 U 盘/共享盘拷贝 `transfer/` 目录（§2.3 清单）
- 下载完成后按第 3 章开始，`export TR=/data/transfer`（按实际目录调整）

---

## 3. 阶段一：离线机环境检查（Linux bash）

以下每步都有**期望**；不符合按第 9 章反馈后停止。

```bash
export TR=/data/transfer        # 产物目录，按实际修改
```

### 3.1 驱动与硬件

```bash
npu-smi info
ls /dev/davinci* /dev/npu* 2>/dev/null | head
```
期望：npu-smi 列出 950 设备、无报错；存在 `/dev/davinci*` 设备节点。

### 3.2 CANN 环境

```bash
ls /usr/local/Ascend/                       # 期望：cann-9.0.0 或 9.1.0（记录版本）
source /usr/local/Ascend/cann-9.0.0/bin/setenv.bash    # 按实际版本路径
which ccec                                  # 期望：.../bin/ccec
echo $ASCEND_HOME_PATH
```

### 3.3 libopapi 符号检查（A5 SDMA 供给的硬前提）

```bash
nm -D $ASCEND_HOME_PATH/lib64/libopapi.so 2>/dev/null | grep -i ShmemSdmaStarsQuery
# 若无输出，先定位再试：
find $ASCEND_HOME_PATH -name 'libopapi.so' 2>/dev/null
```
期望：能看到 `aclnnShmemSdmaStarsQuery` 相关符号。**无输出 → CANN 版本过老，停止并反馈版本号。**

### 3.4 编译工具链

```bash
gcc --version | head -1 && g++ --version | head -1
cmake --version | head -1
ls $ASCEND_HOME_PATH/tools/hcc/bin/aarch64-target-linux-gnu-g++   # a5 runtime 构建需要
```

### 3.5 Python 环境

```bash
python3 --version
python3 -c "import torch; print('torch', torch.__version__)"
python3 -m pytest --version
```
缺依赖见附录 B。

### 3.6 设备可用性 canary（**上次远端机就死在这里**）

```bash
python3 -c "
import torch, torch_npu
torch.npu.set_device(0)
x = torch.randn(1024, device='npu')
print('npu ok:', float(x.sum()))
"
```
期望：`npu ok: <数值>`。
若报 `drvGetDevNum failed: drvRetCode=7` 或 `rtSetDevice(0) failed: 507899` → 驱动坏（与上次 101.245.68.6 相同特征），**换机器**，其余步骤无意义。

---

## 4. 阶段二：代码布置（离线机）

### 4.1 从 bundle 克隆

```bash
cd /data
git clone $TR/pypto.bundle pypto -b feat/aiv-sdma-prefetch
cd pypto
git submodule init
# 注意 1：URL 覆盖必须用子模块的 name（见 .gitmodules），不是 path。
#         runtime 子模块的 name 是 simpler（path=runtime），其余两个 name==path。
git config submodule.simpler.url $TR/simpler.bundle
git config submodule.3rdparty/msgpack-c.url $TR/msgpack-c.bundle
git config submodule.3rdparty/libbacktrace.url $TR/libbacktrace.bundle
# 注意 2：从本地 bundle 克隆时，git 默认禁止 file 传输协议（CVE-2022-39253 加固）。
#         必须用 -c 传入（protocol.* 属于受保护配置，写在 repo-local config 里无效）。
git -c protocol.file.allow=always submodule update runtime 3rdparty/msgpack-c 3rdparty/libbacktrace
```

验证：
```bash
git -C runtime rev-parse HEAD                 # 期望 39ce891d...
git -C 3rdparty/msgpack-c rev-parse HEAD      # 期望 91990874...
git -C 3rdparty/libbacktrace rev-parse HEAD   # 期望 6f8310e...
git log --oneline -1                          # 期望 62cbe0f7 add guide md
```
若报 `transport 'file' not allowed` → 检查是否漏了 `-c protocol.file.allow=always`；
若 runtime 报尝试连 `github.com` → URL 覆盖用错了键（必须是 `submodule.simpler.url`）。

### 4.2 应用 superrepo 与 simpler 补丁

```bash
cd /data/pypto
git apply $TR/0002-a5-prefetch-st-test.patch
cd runtime
git apply $TR/0001-a5-sdma-host-plumbing.patch
cd ..
```

验证：
```bash
git status --short tests/st/runtime/ops/test_prefetch_async.py    # 期望 M
git -C runtime status --short | head                              # 期望 M comm_hccl.cpp / M dma_workspace.h
```
⚠️ 第二条验证是**硬性判据**：`git apply` 在仓库子目录内运行时会**静默忽略**落在当前目录之外的路径（exit 0 但什么都没做）。若 runtime status 为空 → 子模块未就位或补丁没真正落地，回 4.1 检查，不要继续。

### 4.3 布置 pto-isa（pin 基线 + fix）并更新 pin

```bash
cd /data/pypto/runtime
mkdir -p build
git clone $TR/pto-isa.bundle build/pto-isa
cd build/pto-isa

# 落到 pin 基线（当前 pin = 5a4f74cb）
git checkout --detach 5a4f74cbf627d4aac2e0ce10d5e0d8b118343265

# 应用 SQE 修复（git am 保留作者与 commit message）
git am $TR/0003-fix-a5-sqe.patch
FIX_SHA=$(git rev-parse HEAD)
echo "FIX_SHA = $FIX_SHA"          # 记录此值，后续对照用

# 更新 pin 并验证干净
printf '%s\n' "$FIX_SHA" > ../../pto_isa.pin
git status --porcelain              # 期望：空输出（必须干净，否则 resolver 会尝试联网重克隆）
cd /data/pypto
cat runtime/pto_isa.pin             # 期望 = $FIX_SHA
```

### 4.4 pin 解析自检（验证零网络路径成立）

```bash
cd /data/pypto/runtime
python3 -c "
import sys; sys.path.insert(0, '.')
from simpler_setup.pto_isa import read_pto_isa_pin, ensure_pto_isa_root
print('pin  =', read_pto_isa_pin())
print('root =', ensure_pto_isa_root(verbose=True))
"
```
期望：`root = /data/pypto/runtime/build/pto-isa`，日志中**无 clone 动作**（复用本地 checkout）。
若出现 `Removing pto-isa checkout ... to re-clone` → checkout 不干净或 pin 不一致，回 4.3 检查。

---

## 5. 阶段三：构建

### 5.1 pypto C++（pypto_core 扩展）

```bash
cd /data/pypto
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel $(nproc)
```

### 5.2 a5 onboard runtime（自动经 pin 解析用 build/pto-isa）

```bash
cd /data/pypto/runtime
python3 simpler_setup/build_runtimes.py --platforms a5
```
期望：日志出现 `pto-isa cloned from ...`（首次）或直接复用；a5 相关 runtime 全部 Building 成功，无 error。

### 5.3 构建产物验证

```bash
cd /data/pypto

# 1) host runtime 的 4 个 dma_workspace 符号（补丁 0001 的落地证据）
for f in $(find runtime/build/lib -name '*.so' 2>/dev/null); do
  n=$(nm -D "$f" 2>/dev/null | grep -c ' T dma_workspace')
  if [ "$n" -gt 0 ]; then
    echo "== $f ($n symbols)"; nm -D "$f" | grep ' T dma_workspace'
  fi
done
```
期望：a5 的 host runtime `.so` 上恰好 **4 个** `dma_workspace_*` 的 T 符号（与 a2a3 同名）。

```bash
# 2) pin 烘焙校验（pto-isa 版本一致性）
cat runtime/build/lib/pto_isa_build.json
```
期望：`required_commit` == `$FIX_SHA`（4.3 记录的值）。
不一致 → 重跑 5.2（元数据随构建写入）。

---

## 6. 阶段四：UT 回归（无需设备）

```bash
cd /data/pypto
export PYTHONPATH=$(pwd)/python:$PYTHONPATH
python3 -m pytest tests/ut/codegen/test_prefetch_codegen.py \
                    tests/ut/ir/operators/test_prefetch_ops.py \
                    -v 2>&1 | tee ut_prefetch.log
```
期望：**除 `test_prefetch_stays_off_the_cube_lane` 外全部通过**（该用例是改动前就存在的既有 IR parser 失败，与本次无关）。
基线（改动前同机）：21 passed / 1 failed。若失败数 > 1 或失败者不同 → 反馈。

---

## 7. 阶段五：真机 E2E（核心验证）

### 7.1 运行

```bash
cd /data/pypto
export PYTHONPATH=$(pwd)/python:$PYTHONPATH
source /usr/local/Ascend/cann-9.0.0/bin/setenv.bash   # 按实际版本
python3 -m pytest tests/st/runtime/ops/test_prefetch_async.py \
    --platform a5 --device 0 -v --runtime-log-level info \
    2>&1 | tee prefetch_a5.log
```

### 7.2 通过判据（四点全满足）

1. **编译**：ccec 成功编译含 `__sdma_cmo_prefetch` 的 kernel（日志可见 kernel 编译过程，无报错）；
2. **供给**：无 workspace provisioning 相关 ERROR（a5 无 warmup kernel，首次出现一条相关 WARN **属正常**）；
3. **不挂起**：`pl.prefetch.wait` 返回（= 硬件接受 SQE + doorbell 敲响）；
4. **断言**：`torch.equal(out, a)` 通过，测试 PASSED。

### 7.3 失败模式对照表

| 阶段 | 症状 | 最可能原因 | 反馈材料 |
|---|---|---|---|
| 供给 | `aclnnShmemSdmaStarsQuery` 找不到 / 调用失败 | libopapi 版本；950 CMO 官方要求 CANN 9.1.0 + 950 ops-legacy（远端 9.0.0 存在此风险） | 完整日志 + `npu-smi info` + CANN 版本 |
| 编译 | ccec 报错 | include 路径 / 头文件问题 | 报错全文 + 5.3 的 json |
| 执行 | hang 在 wait（超时） | SQE 字段被硬件拒绝 / doorbell 问题 | 日志 + `npu-smi info` + `dmesg \| tail -50` |
| 断言 | 数据不匹配 | 极少见 | 日志 |

### 7.4 可选：SQE 编译 canary（若想在上真机前单独验证头文件）

```bash
cat > /tmp/sqe_validate.cpp <<'EOF'
#include "pto/pto-inst.hpp"

using pto::comm::sdma::detail::AddOneCmoSqe;
using pto::comm::sdma::detail::AddOneMemcpySqe;
namespace sdma = pto::comm::sdma;

AICORE void validate_cmo()
{
    __gm__ sdma::BatchWriteChannelInfo* ch =
        reinterpret_cast<__gm__ sdma::BatchWriteChannelInfo*>(0x1000ULL);
    __gm__ uint8_t* src = reinterpret_cast<__gm__ uint8_t*>(0x2000ULL);
    AddOneCmoSqe<>(ch, src, 128U, 0U, 1U);
}

AICORE void validate_memcpy()
{
    __gm__ sdma::BatchWriteChannelInfo* ch =
        reinterpret_cast<__gm__ sdma::BatchWriteChannelInfo*>(0x1000ULL);
    __gm__ uint8_t* src = reinterpret_cast<__gm__ uint8_t*>(0x2000ULL);
    __gm__ uint8_t* dst = reinterpret_cast<__gm__ uint8_t*>(0x3000ULL);
    AddOneMemcpySqe(ch, src, dst, 0ULL, 128U, 0U, 1U);
}
EOF

ccec -c -O2 -x cce -std=c++17 --cce-aicore-only \
  -mllvm -cce-aicore-stack-size=0x8000 \
  -mllvm -cce-aicore-record-overflow=false \
  -mllvm -cce-aicore-addr-transform \
  -mllvm -cce-aicore-dcci-insert-for-scalar=false \
  --cce-aicore-arch=dav-c310-vec \
  -I/data/pypto/runtime/build/pto-isa/include \
  -o /tmp/sqe_validate.o /tmp/sqe_validate.cpp
echo "exit=$?"; ls -l /tmp/sqe_validate.o
```
期望：`exit=0` 且生成 `.o`（此前在 101.245.68.6 已验证通过）。
注：TU 必须从 `pto/pto-inst.hpp` 伞形头进入（直接 include sdma 头会成环）。

### 7.5 可选：性能观测（验证通过后）

```bash
# PMU（4 = MEMORY 事件）
python3 -m pytest tests/st/runtime/ops/test_prefetch_async.py \
    --platform a5 --device 0 -v --enable-pmu 4 2>&1 | tee prefetch_a5_pmu.log
# 或 chip swimlane
... --enable-chip-swimlane
```
产物在 `<work_dir>/dfx_outputs/`。

---

## 8. 已知风险（不意外=正常）

| 风险 | 说明 |
|---|---|
| CANN 9.0.0 vs 9.1.0 | SHMEM 声明 950 CMO 需 9.1.0 + 950 ops-legacy。9.0.0 上 `aclnnShmemSdmaStarsQuery` 符号存在但 AICPU op 对 950 的行为未验证——**供给阶段可能失败，这是本次验证要回答的头号问题** |
| 驱动健康 | 上次远端机驱动坏（drvRetCode=7 / 507899）。3.6 canary 先行拦截 |
| a5 无 sdma_warmup_kernel | a2a3 有、a5 没移植。首次访问慢 + 一条 WARN，**功能不受影响** |
| 既有 UT 失败 | `test_prefetch_stays_off_the_cube_lane` 改动前即失败，忽略 |
| pin 与 fork SHA | 离线机上 `$FIX_SHA` 是本地新 commit（基线 5a4f74cb + 0003），与 fork 的 `ebb1d62e` 不同属正常；pypto 仓的正式 pin bump 等上游合并后再做 |

---

## 9. 反馈模板（失败或存疑时）

按序提供：
1. 卡住的步骤编号（如 "5.2 构建失败"）；
2. 该步骤的**完整命令输出**（`tee` 保存的日志文件）；
3. `npu-smi info` 输出、CANN 版本（`cat /usr/local/Ascend/cann-*/version.cfg` 或 `ls /usr/local/Ascend/`）；
4. `cat runtime/build/lib/pto_isa_build.json`（若已构建）；
5. 若 hang/崩溃：`dmesg | tail -50`；
6. `python3 --version`、`torch --version`。

---

## 附录 A：离线机已有 pypto checkout 的捷径

若目标机已有可用的 pypto + runtime 环境（如另一台测试机）：

1. 带 3 个补丁 + `pto-isa.bundle` 即可（不需要其他 bundle）；
2. superrepo 切到与 `62cbe0f7` 等价基线（其上游父提交即可），`git apply 0002`；
3. runtime 子模块确保在 `39ce891d`，`git apply 0001`；
4. pto-isa 布置同 4.3（`runtime/build/pto-isa` 若已存在，先 `rm -rf` 再从 bundle clone）；
5. 后续同第 5 章起。

## 附录 B：Python 依赖离线安装

若离线机缺 torch/pytest 等：
1. 联网机上按**目标机相同 python 版本与架构**准备 wheelhouse：
   ```bash
   pip download torch pytest numpy -d wheelhouse/ --platform manylinux2014_aarch64 \
       --python-version 3.10 --only-binary=:all:    # 按目标机实际版本调整
   ```
2. 拷贝 `wheelhouse/` 到离线机：
   ```bash
   pip install --no-index --find-links=wheelhouse/ torch pytest numpy
   ```
3. pypto 本体用源码 + PYTHONPATH（第 6/7 章命令已含），无需 pip 安装。

## 附录 C：commit/SHA 速查

| 项 | SHA |
|---|---|
| pypto 分支 `feat/aiv-sdma-prefetch` | `62cbe0f7`（已推送 `github.com:lulu-Coding/pypto.git`） |
| simpler 子模块基线 | `39ce891d` |
| pto-isa 当前 pin（改动前） | `5a4f74cbf627d4aac2e0ce10d5e0d8b118343265` |
| pto-isa fork fix 分支 | `fix/a5-stars-v2-sqe-init` @ `ebb1d62e`（基线 fork main `32b08bbe`） |
| 离线机将生成的 FIX_SHA | 基线 `5a4f74cb` + 0003（4.3 步骤产生，**每次执行都不同**——committer 时间戳参与 SHA 计算，属预期，以 4.3 实际输出为准） |
| pto-isa 上游 PR 入口 | `https://github.com/hw-native-sys/pto-isa/compare/main...lulu-Coding:pto-isa:fix/a5-stars-v2-sqe-init` |

## 附录 D：本手册的验证状态与已知坑

全流程（§4.1–§4.3）已于 2026-09-15 在 Windows 本机用真实产物彩排通过。彩排中发现并已写入正文的坑：

| 坑 | 现象 | 规避（已写入正文） |
|---|---|---|
| git file 传输协议禁令 | `fatal: transport 'file' not allowed`，submodule clone 失败并整体中止 | §4.1 用 `git -c protocol.file.allow=always submodule update ...`（repo-local 配置无效，必须 `-c`） |
| 子模块 name ≠ path | runtime 尝试从 github.com 克隆 | §4.1 URL 覆盖用 `submodule.simpler.url`（name），不是 `submodule.runtime.url`（path） |
| git apply 静默忽略 | exit 0 但补丁没落地（仓库子目录内、路径落在目录外时） | §4.2 的 status 验证为硬性判据 |
| Windows MAX_PATH 260 | 仅影响 Windows 上彩排（长路径文件创建失败）；**Linux 目标机不受影响** | 已设 `git config --global core.longpaths true` |
