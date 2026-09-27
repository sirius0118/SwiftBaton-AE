# SwiftBaton-K 全局 DMA MR 原型验证（2026-09-27）

已实现可选的内核传输后端，并通过 RXE/KASAN 隔离内核测试和 Node2→Node3 的 ConnectX RDMA READ 测试。CRIU 的地址表传输协议、运行选项及容器实验尚未接入；当前不能报告新路径的容器停机时间或 TTR。U、已封版版本、宿主机启动内核和默认 CRIU 均未改变。物理测试结束后，原模块、参数、NUMA/QoS 环境及其他容器/虚拟机状态已检查恢复。

## 实现语义

- 控制器在连接 QP 前调用 `SBK_IOC_DMA_ENABLE`。源端建立会话专用、仅允许远端读的 DMA MR；目标端预分配只读地址表目录。普通 MR 是默认行为。
- `EXPORT_REGION/EXPORT_BATCH` 在新模式下保留 `ib_umem_get()` 的页面 pin 和 DMA 映射，省去每个 region 的硬件 MR 创建。通过 `DMA_EXPORT_MAP/DMA_IMPORT_MAP` 交换每页实际 DMA 地址；region 的虚拟基址仍用于描述，不能作为 DMA 地址直接计算。
- PS、PF、prefetch、hot-first batch 使用同一传输后端。页地址表发布后不可改写，随 transport 生命周期保活；缺页路径通过数组寻址，不新增表锁或逐次引用操作。
- 全局 key 成功撤销后才释放源端 umem。并发导出、copyout 失败和重复撤销仍受会话所有权约束；撤销失败必须保留被访问内存，不能通过强制释放来完成清理。
- 使用驱动 `get_dma_mr/dereg_mr` 配对，适配 native 5.15 和 OFED；单独持有 PD usecount，不将未建立通用 MR tracker 的对象传给 `ib_dereg_mr()`。
- 全局 key 可覆盖的地址范围大于容器，不是容器内存隔离边界。当前用途是互信实验节点，key 按会话销毁，无远端写/atomic 权限。仍然只发布已 pin 的迁移页 DMA 地址。

本阶段刻意保留最终导出时的 pin。下一阶段才考虑将 pin 提前至 PS，并对最终 PFN、dirty 和映射变化进行校验。创建全局 key 并不自动保证任一进程页面的物理位置/生命周期稳定。

## 验证

隔离内核 Linux 5.15.167 + KASAN/RXE：匿名 PTE 和文件映射分别验证四条路径；PS 页改变后重新导出及 dirty invalidation；未导入/错误/重复地址描述拒绝；源地址 unmap/remap 后旧 DMA 页仍正确；撤销后未取页面受控失败；8 个导出线程与撤销并发及 copyout 失败清理。另运行原有全套 LOOPBACK_TEST/RXE 回归、双进程普通/DMA 对照和 1024 次冷缺页探针。全部通过，模块成功卸载，运行期无新增 BUG/WARNING/Oops，taint 保持外部未签名模块的预期值 12288。虚拟机测试不用于 ConnectX 性能结论。

物理环境：Node2 5.15.167 → Node3 已验证 ARM-batch 内核 5.15.167-swiftbaton-k1；mlx5_1/GID 3，NUMA 0，每方向 25 Gbps。实际 mlx5_core 版本 5.8-6.0.4，编译使用匹配的 OFED 5.8。每次模块替换前后核对实际加载 ELF build-id 和 refcount；无重启、无物理机故障注入。

1024 页四路径测试：PS=3，失效=1，PF=1，FT=4，BG=1017，合计 1024，数据逐字节正确、errors=0。源端 PS 和最终 region 复用同一全局 rkey。

三组交替测试，每组各 1024 个不同的冷 4 KiB 页，仅按需缺页，PS/FT/BG 关闭；同一测试程序、QP 配置、NUMA 和匿名 PTE/token-pool 路径。计时从应用 load 前到返回后，包含故障入口/退出；约 20–21 ns 的时钟开销未扣除。

| 轮次 | 普通 MR 平均 / p99（µs） | 全局 DMA MR 平均 / p99（µs） |
|---|---:|---:|
| 1 | 9.016 / 12.607 | 9.073 / 12.117 |
| 2 | 9.102 / 12.470 | 9.080 / 12.326 |
| 3 | 9.105 / 12.706 | 9.115 / 12.359 |

这些样本未观察到明显缺页时延退化，也不足以声称有显著缺页时延收益。其用途是验证逐页 DMA 地址表没有破坏既有低延迟路径；并非容器负载下的缺页分布。

## 证据与边界

[结构化结果](dma-mr-prototype-results.json) 保存各轮结果、模块和测试程序 SHA256、运行前后环境。源代码在 `kernel/module/sbk_rdma.c`、`kernel/include/sbk_uapi.h`，测试在 `kernel/tests/dma-mr-test.inc`，隔离验证入口为 `kernel/vm/dma-validation.py`。

Node2 原始记录根目录 `/home/k8s/SwiftBaton-AE-downtime-20260927-work`：

- `dma-host-probe.json/.log/.py`、`dma-probe-*-source.log`、`dma-probe-*-destination.log`；所有实际应用故障原始样本保留在 destination 日志。
- `dma-health-before.json`、`dma-health-after.json`；物理测试结束后的原运行时恢复证明。
- `dma-mr-vm/validation-20260927_204306.log` 与 `dma-mr-peer-vm/validation-20260927_204644.log`；两个阶段测试结果均保留。
- `dma-mr-source/build.log`、`dma-mr-target/build.log` 与对应 `module/swiftbaton_k.ko`；候选产物与历史封版路径分开保存。

最终 MR 导出之前测得的 77–101 ms 同时包含 pin、DMA 映射和硬件注册。当前原型仅去掉最后一项；并引入 DMA 地址表准备与控制面传输，实际净收益尚需容器实验测量。目标端约 203 ms ARM/token/PTE 工作独立存在，不因源端全局 key 自动消失。
