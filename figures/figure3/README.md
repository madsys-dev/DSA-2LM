1. 为了相对公平起见，对于 DSA，我们仅启用 1 个 DSA（dsa0），但是启用里面的所有 4 个 PE
2. size 范围变化从 64B 到 2MB，这是因为 movdir64b 的最小粒度是 64B，而 DSA 一次能够传输的最大 size 是 2MB
3. 左边第一列我用 config 表示配置，例如 movdir64b-L,R-bandwidth 表示用 movdir64b，Local to Remote，测量值是带宽，不同的列代表 size
4. latency 表示延迟，单位是 ns，bandwidth 表示带宽，单位是 GB/s