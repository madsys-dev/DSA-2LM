- [Kernel & Test Config](#kernel---test-config)
  * [Kernel Config](#kernel-config)
  * [Test Config](#test-config)
- [Our Completed Work](#our-completed-work)
  * [In-kernel Use of DSA Profiling](#in-kernel-use-of-dsa-profiling)
  * [Page Migration Profiling](#page-migration-profiling)
- [Pending Work](#pending-work)
- [Unresolved & Puzzled Problems](#unresolved---puzzled-problems)
- [Timeline](#timeline)

## Kernel & Test Config

### Kernel Config

确保 CPU 支持 [Intel@ Built-In Accelerators](https://www.supermicro.com/en/accelerators/intel/built-in-on-demand)，在内核编译选项中启用 IOMMU 和 DSA 驱动：

```
CONFIG_INTEL_IOMMU=y
CONFIG_INTEL_IOMMU_SVM=y
CONFIG_INTEL_IOMMU_DEFAULT_ON=y
CONFIG_INTEL_IOMMU_SCALABLE_MODE_DEFAULT_ON=y
 
CONFIG_INTEL_IDXD=m
CONFIG_INTEL_IDXD_SVM=y
CONFIG_INTEL_IDXD_PERFMON=y
```

对于 Vanilla Kernel，默认情况下，上面大部分选项都已经打开了。

建议启用 5 级页表：`Processor type and features -> Enable 5-level page tables support`。

---

### Test Config

linux-5.15.19-memtis 目前还是旧版代码，以下均指代 linux-5.13-rc6-tpp。

在 `/proc/sys/vm/` 下留有多个 sysctl 接口用于控制，分别是 `cpu_multi_copy_pages`、`use_concur_to_compact`、`use_concur_to_demote`、`limit_mt_num`、`dsa_state`、`use_dsa_copy_pages`、`limit_chans`、`dsa_copy_threshold`，以及 `/proc/timer` 用于记录 page copy 相关统计信息。

以下默认 root 用户，省略 sudo。

```shell
git clone https://github.com/LRL52/kernels-with-dsa
cd kernels-with-dsa

# Step 1：Disable CPU cores on node 1。Ignore if node 1 is already a CPU-less Node.
./disable_cpu.sh 1

# Step 2：Enable concurrent migrate pages
echo 1 > /proc/sys/vm/use_concur_to_compact # concurrent migrate pages for memory compact
echo 1 > /proc/sys/vm/use_concur_to_demote # concurrent migrate pages for demote pages

# Step 3 (please choose one of following two）
# Step 3 for DSA copy pages
accel-config load-config -c ./dsas-4e1w-d.conf -e
echo 1 > /proc/sys/vm/dsa_state # initialize and enable DSA
# PLEASE CHECK dmesg TO MAKE SURE THERE ARE NO ERROR BEFORE CONTINUTING!!!
echo 1 > /proc/sys/vm/use_dsa_copy_pages # use DSA to copy pages

# Step 3 for multi-thread copy pages
echo 1 > /proc/sys/vm/cpu_multi_copy_pages # use multi-thread to copy pages

# Step 4：TPP start tiering
echo 1 >/sys/kernel/mm/numa/demotion_enabled
echo 2 >/proc/sys/kernel/numa_balancing
swapoff -a
echo 1000 >/proc/sys/vm/demote_scale_factor

# Step 5 (optional)：Enable timer for profiling
# echo 0 > /proc/timer # if timer state is already on
echo 1 > /proc/timer # clear old data of timer and enable it
# after running workload
echo 0 > /proc/timer # stop timer
cat /proc/timer # then you can get related statistics during this workload running time

# After step 1-4 above, then you can start end-to-end workload test.

# Release DSA
echo 0 > /proc/sys/vm/use_dsa_copy_pages # disable use_dsa_copy_pages
# make sure disable use_dsa_copy_pages first and then release DSA!!!
echo 0 > /proc/sys/vm/dsa_state # release DSA

# Key Parameter Config
# You may config it after Step 1 and before Step 2
echo 4 > /proc/sys/vm/limit_mt_num # use 4-thread to parallel-copy pages (Note：only for multi-thread copy pages), defualt: 4
echo 8 > /proc/sys/vm/limit_chans # use 8-channel to parallel-copy pages (Note：only for DSA copy pages), and make sure corresponding DSA config meets the requirements, and DO NOT CHANGE IT when DSA copy page is enabled!!! defualt: 8
echo 12 > /proc/sys/vm/dsa_copy_threshold # use dsa to copy pages if number of pages in migration lists is greater or equal to 12 (Note: a 2MB THP/HugePage equals 512 pages), defualt: 12

```

**注意：**现有论文的内核（例如 TPP 和 MEMTIS）基本已经硬编码或者**仅实现了** node 0 是含 CPU 的快速层内存，node 1 是不含 CPU 的慢速层内存的 2-layer 分层内存。对于其它情况均不适用。

请确保 Step 1 已经完成后，再启用分层内存（Step 4），否则启用后可能没有实际效果。Step 2 也是独立可选的。如果不使用 concurrent migrate pages，则 DSA/multi-thread 仅对 HugePage/THP 有效。

建议严格安照上面的步骤按顺序执行，如果顺序错误或漏掉可能导致 panic。

## Our Completed Work

- [x] 第一个在 Kernel Space 下应用 DSA 透明加速了页面迁移和页面规整，不必修改应用程序源码即可享受到加速效果。

  > DSA 目前生态比较匮乏，现有的很多 DSA 的使用案例也是在 User Space 下的。据我所知，目前开源的唯一在 Kernel Space 有应用的是 Anolis 里用 DSA 来实现了[页清零](https://gitee.com/anolis/cloud-kernel/pulls/702)。（注：当时为了学习如何在内核中使用 DSA 找了好久示例代码，后面才意外找到，可惜找到的时候我已经知道该怎么用了）

- [x] 放弃直接使用 DMA Engine API，利用 IDXD Driver 实现对 DSA 更精细的控制。通过预分配、定制的 PER_CPU_VAR 的 Descriptor Pool，避免 DMA Engine 带来的额外开销。

  > Anolis 里[页清零](https://gitee.com/anolis/cloud-kernel/pulls/702)的代码也只是比较 naive 的调用 DMA Engine API 来完成。实际上 IDXD 实现 DMA Prepare Descriptor 回调函数的内容是写死固定的，在不更改驱动代码的情况下无法使用 DMA 对 DSA 完成除 memcpy 以外的操作。

- [x] 实现了 Multi-channel (up to 8-channel) 2MB HugePage/THP 的页面迁移，相比 CPU 的 Latency 有 3~14x 的加速。对于 4KB base page，利用 DSA Batch Processing 实现了对一组页面的批量迁移，相比 CPU 的 Latency 有 1.5~4x 的加速。

  > 核心实现在于 `dsa_copy_page_lists`，将 Multi-channel 和 Batch Processing 融为一体，对于一个 List 中 4KB 基页和 2MB THP 的混合页面场景，也能够统一高效地处理。

- [x] 在 Linux 5.13 上实现了 Concurrent Migrate Pages（参考 Nimble 作者的研究成果），提高了页面迁移的效率。 对于一组待迁移的页面，支持基页和 THP 混合迁移，目前最大可支持 24 个大页 + 1024 个基页同时迁移。

  > 这部分我仔细读了 Nimble 在 5.4 上实现的 migrate_pages_concur 相关的代码，并结合 5.13 代码的变化，在 5.13 上成功实现了 migrate_pages_concur。

- [x] 对于 In-kernel Use of DSA 以及 Page Migration 做了 Profiling，同时我们将 IDXD Driver 移植到 5.13 和 5.15 上，将 DSA 应用于 TPP、MEMTIS 等现有分层内存内核，取得了良好效果。

  > 将 6.4.16 的 IDXD Driver 的代码移植到 5.15.19 和 5.13-rc6 上，将 5.15.19 的 IOMMU 的代码移植到 5.13-rc6 上。

目前最新的 microbenchmark 结果，第一张图是 microbench-large，第二张图是 microbench-small。

![microbenchlarge](https://images.lrl52.top/i/2024/08/17/microbenchlarge-1723821580708-13-0.png)

![microbenchsmall](https://images.lrl52.top/i/2024/08/17/microbenchsmall-1723821565927-11-0.png)

注：tpp-dsa 表示 TPP + DSA（仅大页有效）、tpp-concur-multithreads 表示 TPP + Concurrent Migrate Pages + multithread（类似 Nimble），tpp-concur-dsa 表示 TPP + Concurrent Migrate Pages + DSA（大页与基页均有效）。

Insight：在 microbench-large 场景下，DSA 有不错的性能表现，read 场景下略优于 multithread。在 microbench-small 场景下，DSA 在 write 场景下 migrate in progress 阶段有很好的性能表现，但在 read 场景下 migrate in progress 阶段性能最低。

### In-kernel Use of DSA Profiling

![order-0-1](https://images.lrl52.top/i/2024/08/17/order-0-1-0.png)

![order-2-3](https://images.lrl52.top/i/2024/08/17/order-2-3-0.png)

![order-4-5](https://images.lrl52.top/i/2024/08/17/order-4-5-0.png)

![order-6-7](https://images.lrl52.top/i/2024/08/17/order-6-7-0.png)

![order-8-9](https://images.lrl52.top/i/2024/08/17/order-8-9-0.png)

Insight：当 page order >= 3 时，使用 DSA 来 copy page 相比 CPU 能够有更低的 latency。对于非连续的页，我们使用 batch processing 或直接 single page submit。对于连续的页（2MB 大页），我们使用 multi-channel 来并行拷贝。

实际上，在 `dsa_copy_page_lists` 代码中，我将上述三种办法融合在一起了，可以处理 4KB 基页和 2MB THP 的混合页面。

### Page Migration Profiling

在 TPP 内核中，通过 trace `migrate_pages`，运行 microbenchmark 得到了调用 `migrate_pages` 的相关统计信息分布。对 (reason, mode) 分类独立统计，总共有 3 种情况：(compaction, MIGRATE_SYNC_LIGHT)、(demotion, MIGRATE_ASYNC) 和 (numa_misplaced, MIGRATE_ASYNC)。

![image-20240816191531769](https://images.lrl52.top/i/2024/08/17/image-20240816191531769-0.png)

![image-20240816191542999](https://images.lrl52.top/i/2024/08/17/image-20240816191542999-0.png)

![image-20240816191559960](https://images.lrl52.top/i/2024/08/17/image-20240816191559960-0.png)

Insight：对于 compaction，会一次性迁移几十到几百个页面，可以利用 DSA 的 batch processing 来处理。对于 demote，可以观察到一次性迁移的页面数量被限制在了 32 以内，最大允许 1 个大页（对应 512-543 的情况），可以利用 DSA 的 batch processing 和 multi-channel 来处理。对于 promote，TPP 通过 NUMA hint fault 来实现，对应 numa_misplaced，因此每次仅会迁移 1 个 page，无法应用 DSA，但实际上对于 THP 的 promote，它不经过 migrate_pages 调用，而是走 `migrate_misplaced_transhuge_page -> migrate_page_copy -> copy_huge_page`，因此 DSA 对于 THP 的 promote 依然有效。另外，热启动/混部场景产生的大量页面迁移，很大一部分也是由于 compaction 造成的，而不是分层内存的页面迁移。

<img src="https://images.lrl52.top/i/2024/08/17/baseline-0.png" alt="baseline" style="zoom: 50%;" />

<img src="https://images.lrl52.top/i/2024/08/17/dsa-0.png" alt="dsa" style="zoom:50%;" />

上面这两张图分别是用 CPU 和 DSA 在 MEMTIS 跑 Graph500 1 : 4 场景下的测试结果，来自于 6 月底的成果，DSA 仅应用于 HugePage/THP。

Insight：在 workload 启动初期，由于快速层内存不够，由此产生大量 page compaction 和 demote，迁移量接近直线快速上升。在大概 20-80s 这段时间，根据页面热度发生冷热交换。80s 以后，进入 migration stable 状态，工作集热度分布趋于稳定，基本不在产生页面迁移。

## Pending Work

- [ ] DSA 在 batch processing 下 limit_chans 和 dsa_copy_threshold 参数调节，以及 DSA config 或许仍有优化空间（见论文 [Intel Accelerators Ecosystem: An SoC-Oriented Perspective : Industry Product](https://ieeexplore.ieee.org/abstract/document/10609705) 中的 Fig. 7: DSA QoS for different priority values for WQs.）
- [ ] 寻找和测试更多的 workload，调整 workload 工作集大小以最大化发挥 DSA 性能
- [ ] 调节 TPP 的参数，compaction 和 demotion 应该有参数可以调，以及 demote_scale_factor 等
- [ ] ⭐⭐⭐ 在 CXL、PM 等其它存储介质上完成测试

> 以上几个统一为调参和测试

- [ ] 将最新成果移植到 MEMTIS 内核上，但需要解决下文提到的问题
- [ ] 将最新成果移植到 NOMAD 内核上，但需要有证据表明 NOMAD 比 TPP 更优

## Unresolved & Puzzled Problems

- [ ] DSA 的性能比较诡异，在一些 case 下表现为“高性能”，在一些 case 下表现为“低性能”。以 2MB 为例，在一些 case 下 latency 是 1.7w ns，一些 case 下 latency 是 6.8w ns，两者速度差了整整 4 倍。至于一些 case  指什么呢，**在 module test 中（无论什么版本的内核）**，如果预先分配好所有测试用例的 page，再测试，再释放所有 page，表现为“高性能”；如果每一次测试循环的时候再分配 page，测一次后再释放 page，表现为“低性能”。注意，在两种情况下，测量时间均只包含 DSA copy 的时间，没有计入分配和释放 page 的时间。如果预先分配好所有测试用例的 page，但测试的时候只用同一个 page，那么也表现为“低性能”，与前者所述实质为同一种情况。但更玄乎的地方在于，根据之前和现在的测试数据，**在 MEMTIS 内核中应用后**表现为“高性能”，**在 TPP 内核中应用后**表现为“低性能”。

  > 早就在 MEMTIS 内核的时候，我就发现了一个诡异的现象是：将 module test 的成果实际应用后，**CPU 拷贝的速度慢了几倍，DSA 的拷贝的速度快了几倍**。目前为止，CPU 的速度变化是由于 Cache 引起的，已经解决。DSA 的性能变化原因仍是未解之谜。

- [ ] MEMTIS 内核存在很多问题。比如 offline node 1 的 CPU 会耗费很多时间，kill_sample 极易导致内核崩溃、vscode-server 会让内核崩溃等。NOMAD 作者在第一版论文里表示他修复了 MEMTIS 一些 BUG 才让 MEMTIS 跑起来，但 NOMAD 作者最终也没法跑 YCSB。据我所知，以上一些问题至少有 3 人遇到。以及我该选择 NOMAD 作者修改后的 MEMTIS 代码还是 MEMTIS 代码，以及哪个 MEMTIS 版本的代码是正常 work 了的也是个问题。 

- [ ] 阿里云服务器在 5.x 内核下 crash 后无法正常重启，也无法通过 Kdump + Kexec 成功转储内核并捕捉到错误。问题原因大概与 Kdump + Kexec 有关，在 6.4.16 下能够正常工作，在 5.x 似乎就会阻碍 crash 后的重启。这个问题给我开发和测试带来了一些不便和麻烦。

## Timeline

2024.02 - 2024.03：阅读相关论文，复现和测试现有分层内存系统（TPP、MEMTIS），以及测试 workload。探索 DSA 的使用。因为某些原因和社区支持匮乏，DSA 一直无法正常启用。

2024.04 ：第一次成功启用 DSA，并在用户态下完成了 [A Quantitative Analysis and Guidelines of Data Streaming Accelerator in Modern Intel Xeon Scalable Processors](https://dl.acm.org/doi/abs/10.1145/3620665.3640401)的复现和测试。尝试在 QEMU 里启用 DSA 最终未能实现。

2024.05 上旬：第一次在内核态下成功使用 DSA。

2024.05 中下旬：利用 module 对 DSA 进行反复测试和性能分析，但存在一些并发问题。

2024.06：将 6.4.16 IDXD driver 移植到 5.15.19，第一次成功将 DSA 集成到 MEMTIS 内核，并实现加速大页迁移，并对页面迁移的部分数据进行了分析。

2024.07 下旬 - 2024.08 中旬：实现了 DSA batch procesing，在内核态下对 DSA 各种 copy method 的性能完成分析。实现了 concurrent migrate pages，并将 DSA batch procesing 实际应用。将 6.4.16 IDXD driver 移植到 5.13-rc6，将 5.15.19 的 IOMMU 移植到 5.13-rc6 上，并将 DSA 成功应用到 TPP 上。