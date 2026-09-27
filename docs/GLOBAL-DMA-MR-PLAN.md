# SwiftBaton-K：借鉴 MITOSIS 的全局 DMA MR

结论：可以省掉每个迁移区域单独创建普通 MR 的过程。仍然需要 MR/rkey、正确的 DMA 地址和页面生命周期管理。尚未在 SwiftBaton 中实现或启用全局远端访问。

## 已核对的依据

- MITOSIS 本地版本 `3f9f11af3a58b53646d27b79dbd2e91ea55bef2e` 固定 KRCore 子模块到 `c0ca11582dc6937fc274e805fa696762d39e08f7`。
- KRCore 的 [Context 初始化](https://github.com/SJTU-IPADS/krcore-artifacts/blob/c0ca11582dc6937fc274e805fa696762d39e08f7/KRdmaKit/src/context.rs#L60) 分配 PD，并调用 `ib_get_dma_mr` 建立全局 KMR；[内核 MemoryRegion](https://github.com/SJTU-IPADS/krcore-artifacts/blob/c0ca11582dc6937fc274e805fa696762d39e08f7/KRdmaKit/src/memory_region.rs#L158) 复用 context 的 key，将地址转成物理地址，不逐次调用普通用户 MR 注册。
- MITOSIS 的 [COW4KPage](https://github.com/ProjectMitosisOS/mitosis-core/blob/3f9f11af3a58b53646d27b79dbd2e91ea55bef2e/mitosis/src/shadow_process/page.rs#L73) 仍调用 `pmem_get_page/pmem_put_page` 并管理 rmap；因此不能把它描述成完全无需引用管理。
- Node2 当前运行 Linux 5.15.167、MLNX_OFED 5.4-1.0.3.0。`mlx5_1` 对应 PCI `0000:af:00.1`，IOMMU group 129 的实时 `type` 为 `identity`。安装的 OFED 源码 `/usr/src/ofa_kernel-5.4/drivers/infiniband/hw/mlx5/mr.c:678` 中 `mlx5_ib_get_dma_mr` 创建 PA 模式、length64 的 mkey，且 `umem=NULL`。这是实现可行性的证据，尚非实际 RDMA 读测试。

## 对当前实现的改动方向

1. 在 PS 建立迁移会话专用的 DMA MR。只需要远端读，不能照抄 MITOSIS 的远端写/atomic 权限。全局 key 的保护范围大于容器地址空间，只适用于互信实验节点，且应在会话结束后撤销。
2. 将页面保活和 MR 注册分离：PS 提前准备页面 pin/所有权，最终冻结后校验 PFN/dirty 和映射变化，更新实际源地址。仅保留进程 mm 或暂停线程不足以等价替代页面 pin；必须覆盖内核迁页、回收和共享写入等条件。
3. 为最终 region 增加逐页 DMA 地址表，目标端将 `region_base + page_index*4096` 改为 `dma_addresses[page_index]`。虚拟连续不代表物理连续，不能直接把第一个 PFN 当作整段起始地址。当前 batch 本来就是每页一条 WR，再链式提交，能够适配这种表。
4. PF、prefetch、hot-first batch 共用这套地址表和传输后端；PF 的独立 QP/队列及优先级保留。PS 快照路径先保留现有语义，不混淆运行中源页面与不可变快照。
5. 源端只在目标端全部页面完成或明确放弃、所有在途 RDMA 已停下后释放 pin/会话。错误路径也必须撤销访问并停止 DMA 后再释放内存。

## 收益边界

当前最终 MR 导出 77–101 ms 包含 pin、DMA 映射和网卡注册等工作。全局 DMA MR 可省去逐区域网卡注册；如果 PS 页面保活也提前完成，才有机会将更多耗时移出停机窗口。尚未测出这些内部子项的比例，不能承诺全部 77–101 ms 都消失。

源端全局 MR 不会自动移除目标端约 203 ms 的 ARM/token/PTE 工作，也不能替代目标端并发缺页和页面安装的所有权管理。

[ODP](https://networking-docs.nvidia.com/mlnxofedswum/24104140lts/Optimized-Memory-Access) 是另一种按需翻译办法，implicit ODP 可覆盖进程地址空间；它仍使用 key，并把部分映射成本放到首次网卡访问。为追求最低冷 remote fault 时延，当前优先评估预先准备的 DMA MR，不直接假定 ODP 更快。
