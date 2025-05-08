1. 注意 latency 和 bandwidth 的小区别，测量 latency 每次拷贝时会 wait 直到拷贝完成，bandwidth 测量时只在必要时（即将超出 workqueue size 或者最后一次测试）才会 sync 一下。因此通过 latency 计算出的带宽会比测量的 bandwidth 略低。
2. batch_size 图里计算的 latency 是平均每个 4KB 页的 latency。
3. latency 表示延迟，单位是 ns，bandwidth 表示带宽，单位是 GB/s。