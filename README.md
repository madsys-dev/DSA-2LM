# DSA-2LM: A CPU-Free Tiered Memory Architecture with Intel DSA

Tiered Memory is critical to manage heterogeneous memory devices, such as Persistent Memory or CXL Memory. Existing works make difficult trade-offs between optimal data placement and costly data movement. With the advent of Intel Data Streaming Accelerator (DSA), a CPU-free hardware to move data between memory regions, data movement can be up to 4x faster than a single CPU core. However, the fine memory movement granularity in Linux kernel undermines the potential performance improvement. To this end, we have developed DSA-2LM, a new tired memory system that adaptively integrates DSA into page migration. The proposed framework integrates fast memory migrating process and adaptable concurrent data path with well-tuned DSA configurations.

This artifact provides the source code of MEMTIS and TPP kernels, both with DSA integrated, and scripts to reproduce the evaluation results in our paper.

## Note

All commands have been tested on Alibaba Cloud Linux 3 with root privileges, where the `sudo` prefix may be omitted. 

## Prerequisites

### Hardware requirements

Intel DSA is typically available on systems equipped with **4th/5th Generation Intel Sapphire Rapids CPU**. Please ensure that the CPU supports Intel DSA. See the [Intel® Built-In Accelerators](https://www.supermicro.com/en/accelerators/intel/built-in-on-demand) for more details.

For tiered memory, our system requires one CPU with **exactly one memory NUMA node and one CPU-less NUMA memory node. We do not support multiple CPU NUMA nodes.** If your system has more than one CPU NUMA node, you can use the provided `scripts/disable_cpu.sh` script to disable the others. As for the memory-only NUMA node, you can use Persistent Memory, CXL Memory (we use an ASIC-based CXL device in our paper), or virtualize such configurations to run our code in virtual machines.

A typical hardware configuration appears as below:

```
# numactl -H
available: 2 nodes (0-1)
node 0 cpus: 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47
node 0 size: 257617 MB
node 0 free: 255233 MB
node 1 cpus:
node 1 size: 257980 MB
node 1 free: 256490 MB
node distances:
node   0   1
  0:  10  21
  1:  21  10
```

### Software requirements

We compiled our code and performed the evaluation on Alibaba Cloud Linux 3. If you are using Ubuntu or other distributions, you may need to change the package management commands accordingly.

```sh
# To compile the kernels
yum install binutils ncurses-devel \
    /usr/include/{libelf.h,openssl/pkcs7.h} \
    /usr/bin/{bc,bison,flex,gcc,git,gpg2,gzip,make,openssl,pahole,perl,rsync,tar,xz,zstd}
# To install libnuma
yum install numactl numactl-devel numactl-libs
```

We also require `libaccel-config >= 4.1.6` to use Intel DSA. The version available from the system package manager might be outdated. We recommend building and installing `libaccel-config` from source as described in the next section.

⭐️⭐️⭐️ For artifact evaluation, we can provide you temporary access to a bare-metal Alibaba Cloud instance with two DRAM NUMA nodes. For some reasons and constraints, we are sorry that we are unable to provide a CXL cloud testbed. As a result, some reproduction results may differ from those in our paper. Please feel free to contact us if you need the instance.

## Usage

### Download the Code

```sh
git clone https://github.com/madsys-dev/DSA-2LM.git
cd DSA-2LM
git submodule update --init --recursive
```

### Compiling the Code

#### Compile accel-config

You could skip this step if you have installed accel-config via package manager before.

```sh 
yum groupinstall "Development Tools"
yum install autoconf automake libtool pkgconf rpm-build rpmdevtools
yum install asciidoc xmlto libuuid-devel json-c-devel zlib-devel openssl-devel

cd idxd-config
./autogen.sh
./configure CFLAGS='-g -O2' --prefix=/usr --sysconfdir=/etc --libdir=/usr/lib64 --enable-test=yes
make
sudo make install
```

To verify that it was installed successfully, 

```
# accel-config --version
4.1.6.gitb7211059
```

#### Compile dsa-perf-micros

```sh
cd dsa-perf-micros
./autogen.sh
./configure CFLAGS='-g -O2 -DENABLE_LOGGING' --prefix=/usr --sysconfdir=/etc --libdir=/usr/lib64
make
```

#### Compile userspace

```sh
cd userspace
make
```

#### Compile kernels with DSA

Enable IOMMU and IDXD modules in kernel configuration:

```
CONFIG_INTEL_IOMMU=y
CONFIG_INTEL_IOMMU_SVM=y
CONFIG_INTEL_IOMMU_DEFAULT_ON=y
CONFIG_INTEL_IOMMU_SCALABLE_MODE_DEFAULT_ON=y
 
CONFIG_INTEL_IDXD=m
CONFIG_INTEL_IDXD_SVM=y
CONFIG_INTEL_IDXD_PERFMON=y
```

⚠️ Based on our practical experience, Intel DSA may not work properly under the default 4-level page table. We recommend enabling 5-level page table support via: `Processor type and features -> Enable 5-level page tables support`。

💡 We have provided kernel config files for both MEMTIS and TPP kernels. You can use them as a starting point for building your own kernel.

```sh
# Example for MEMTIS
cd src
cp ./kernel_config/memtis.config ./linux-5.15.19-memtis/.config
cd linux-5.15.19-memtis
make olddefconfig
```

Then, compile and install the kernel,

```sh
make -j$(nproc)
make modules_install -j$(nproc)
make install
```

Once the kernel is installed, the system’s default boot kernel will be updated. You may refer to the "Switch a kernel" section to boot into your desired kernel.

### Switch a Kernel

If you need to switch a kernel version, please use the following script and follow the prompts in the terminal.

```
# ./scripts/switch_kernel.sh
=== Alibaba Cloud Linux Kernel Switch Tool ===
Current system: Alibaba Cloud Linux 3.2104 U10 (OpenAnolis Edition)

Current running kernel: 6.4.16-export+
Current default boot kernel: 6.4.16-export+

Available kernel list:
[1] 5.10.134-17.1.al8.x86_64
[2] 5.10.134-17.2.al8.x86_64
[3] 5.10.134-17.3.al8.x86_64
[4] 5.13.0-rc6nomad
[5] 5.13.0-rc6tpp
[6] 5.13.0-rc6tpp-dsa
[7] 5.15.19-htmm
[8] 5.15.19-htmm-dsa
[9] 6.4.16-export+

Please select a kernel number to switch to [1-9]: 8
You selected: 5.15.19-htmm-dsa
Switching to kernel 5.15.19-htmm-dsa...
Executing: grub2-set-default f4f087e744494f36a45c7175b4ac5fa7-5.15.19-htmm-dsa
Executing: grub2-mkconfig -o /boot/efi/EFI/alinux/grub.cfg
Generating grub configuration file ...
Adding boot menu entry for EFI firmware configuration
done

Verifying switch result:
saved_entry=f4f087e744494f36a45c7175b4ac5fa7-5.15.19-htmm-dsa

Kernel switch completed. Reboot the system to use the new kernel.
Use 'reboot' command to restart the system
```

Then you need to:

```sh
reboot
```

💡 For artifact evaluation, we have pre-installed three kernels for your convenience: **the vanilla 6.4.16, MEMTIS-with-DSA, and TPP-with-DSA** (hereafter referred to as TPP and MEMTIS, respectively). These correspond to options `6.4.16-export+`, `5.15.19-htmm-dsa`, and `5.13.0-rc6tpp-dsa` in the available kernel list above.

### sysAPI Interface

Several sysAPI interfaces are exposed under `/proc/sys/vm/` for configuration. Detailed descriptions are as follows:

- `cpu_multi_copy_pages`: Enable CPU multi-thread page copying.
- `limit_mt_num`: Set the number of threads used for CPU multi-threaded page copying.
- `use_concur_to_compact`: Enable concurrent memory compaction.
- `use_concur_to_demote`: Enable concurrent page demotion (TPP only).
   *Note: TPP performs page promotion on-demand via page faults, so concurrent promotion is not applicable.*
- `use_concur_for_htmm`: Enable concurrent page promotion/demotion (MEMTIS only).
- `dsa_state`:
  - `0 -> 1`: Request and initialize DSA for page migration
  - `1 -> 0`: Release DSA resources
- `use_dsa_copy_pages`: Enable using DSA for page copying.
   *Note: You must first set `dsa_state` before enabling `use_dsa_copy_pages`, and reverse the order when disabling.*
- `limit_chans`: Specify the number of DSA devices used for page migration.
- `dsa_copy_threshold`: Use DSA for page migration only when the number of 4KB pages in a single operation exceeds this threshold.

Additionally, `/proc/timer` is used to collect statistics related to page migration:

```sh
# echo 0 > /proc/timer # if timer is already enabled
echo 1 > /proc/timer # clear previous data and enable the timer
# after running the workload
echo 0 > /proc/timer # stop the timer
cat /proc/timer # retrieve migration statistics
```

Example output:

```
# cat /proc/timer
timer_state = 0 total_time = 255ms last_time = 22281ns last_cnt = 512 dsa_hpage_cnt = 9797 dsa_bpage_cnt = 0 dsa_copy_fail = 0 hpage_cnt = 0 bpage_cnt = 3385897
```

The output fields from left to right indicate:
 current timer state, total page copy time, duration of the last page copy, number of pages migrated in the last operation (in 4KB units, e.g., 512 usually indicates a THP/HugePage), number of HugePages migrated with DSA, number of base pages (4KB) migrated with DSA, number of failed DSA copy attempts, number of HugePages copied with CPU, number of base pages (4KB) copied with CPU.

💡 These sysAPI interfaces will be automatically configured when running with provided scripts. You can view the statistics from `/proc/timer` either in the terminal stdout or in the log file at `results_{memtis, tpp}/{workload_name}/{version}/{ratio}/output.log`.

## Install Benchmarks

Please read `userspace/bench_dir/README.md` for detailed steps.

💡 For artifact evaluation, we have pre-installed all needed benchmarks and paths in `userspace/bench_cmds` are correct. So you can skip this step. 

## Reproducing Steps

We provide the necessary code and scripts to reproduce Figures 3–6, 10, and 13–14.

⚠️ Since the Linux 6.x kernel provides more stable support for DSA, we recommend running the microbenchmarks (Figures 3–6, 10) under a 6.x kernel. Since we have backported the IDXD/IOMMU drivers to MEMTIS and TPP kernels, you may also run on those kernels and get similar results.

💡 For artifact evaluation, please refer to the *Switch a kernel* section to boot into the `6.4.16-export+` kernel with 6.x support.


For python scripts, you need a Python 3.11.8 environment with `matplotlib` installed.

💡 For artifact evaluation, you could directly use our python environment via `conda activate matplotlib`.

### Initialize

Before proceeding with the following reproduction steps, please initialize the system first.

⚠️ This step must be performed every time the system is rebooted.

```sh
cd scripts
# For 6.x kernel
./init_6.x.sh
# For TPP/MEMTIS kernel
./init_5.x.sh
# For TPP kernel
./disable_cpu.sh 1 # Disable all CPU cores on NUMA node 1. MEMTIS could skip this step because tht promotion/demotion node is hardcoded.
```

### Figure 3

Reproducing Figure 3 involves two steps. The first run generates results for DSA and `memcpy`. Then, after modifying the source code, a second run will generate results for SIMD (`movdir64b`).

First, copy the scripts into the `dsa-perf-micros` directory,

```sh
cp figures/figure3/run-dsa-perf-micro*.sh dsa-perf-micros
```

Then execute the first script. It takes about **45 minutes** to complete for `memcpy` and DSA. The raw results will be saved in the `dsa-perf-micros/results/4e1w-d` directory.

```sh
cd dsa-perf-micros
./run-dsa-perf-micro1.sh
```

Next, apply a patch to enable `movdir64b` (SIMD) support in `dsa-perf-micros`, 

```sh
patch -p1 < ../figures/figure3/movdir64b.patch
```

> ⚠️ Note that this patch actually replaces `memcpy` operation with `movdir64b`. If you want to test `memcpy` operation afterwards, you need to revert the patch using `patch -p1 -R < ../figures/figure3/movdir64b.patch`. 

Run the second script. It takes about **39 minutes** to complete for `movdir64b`. The raw results will be saved in `dsa-perf-micros/results/movdir64b`.

```sh
./run-dsa-perf-micro2.sh
```

Move the `results` directory to `figures/figure3` and process the data into CSV format using `convert_to_csv.py`, 

```sh
mv ./results ../figures/figure3
cd ../figures/figure3
python3 ./convert_to_csv.py
```

You can find the final results in `figures/figure3/data.csv`.

### Figure 4

The script will take approximately 20 seconds. The raw results will be saved in `figures/figure4/results`.

```sh
cd figure4
./dsa_test_bandwidth.sh
```

Then, convert the results into CSV format and draw the figure,

```sh
python3 ./convert_to_csv.py
python3 ./draw_figure.py
```

See `figures/figure4/channel.{csv, png}`.

### Figure 5-6

The following three test scripts take approximately 9s + 9s + 1min 35s (total about 2 minutes). The raw results will be saved in the `figures/figure5-6/results` directory,

```sh
cd figure5-6
./dsa_test_bandwidth1.sh
./dsa_test_bandwidth2.sh
./dsa_test_latency.sh
```

Then, convert the results into CSV format and draw figures,

```sh
python3 ./convert_to_csv.py
python3 ./draw_figure.py
```

Please refer to `batch_size.{csv, png}` and `channel.{csv, png}` in the `figures/figure5-6` directory.

### Figure 10

This test script takes approximately 1.5 minutes to complete. The raw results are stored in the `figures/figure10/results` directory,

```sh
cd figure10
./dsa_test.sh
```

Then convert the raw results into CSV format,

```sh
python3 ./convert_to_csv.py
```

### Figure 13-14

⚠️ Note that MEMTIS and TPP limit fast-tier memory size in different ways.  For **TPP**, you need to add `GRUB_CMDLINE_LINUX="memmap=nn[KMG]!ss[KMG]"` in `/etc/default/grub` file. For more details, you may check [this link](https://pmem.io/blog/2016/02/how-to-emulate-persistent-memory/). Note that after modifying `/etc/default/grub`, you must regenerate the GRUB configuration using `grub2-mkconfig -o /boot/efi/EFI/alinux/grub.cfg` and reboot. For **MEMTIS**, there is no need to impose a global memory limit. MEMTIS constrains appropriate fast-tier memory capacity according to workload configuration via *cgroup*. Please ensure fast-tier memory is sufficient.

⚠️ Before running the evaluation for the first time, edit **line 13** in `scripts/run_bench_{memtis, tpp}.sh`, and set the `DIR` variable to the **absolute path** of the `userspace` directory:

```sh
###### update DIR!
DIR=/data2/atc25-dsa2lm-artifact/userspace
```

The raw results for real-world workloads are saved in the `userspace/results_{memtis, tpp}` directory. **Remember to initialize the system after switching a kernel.**

Limit the fast-tier memory to **16 GB** and switch to the **TPP** kernel to reproduce the **upper-left** part of Figure 13. This test takes approximately **50 minutes**.

```sh
cd userspace
./run-fig13-tpp-16G.sh
```

Limit the fast-tier memory to **32 GB** and switch to the **TPP** kernel to reproduce the **upper-right** part of Figure 13. This test takes approximately **40 minutes**.

```sh
cd userspace
./run-fig13-tpp-32G.sh
```

**Remove the fast-tier memory limit** and switch to the **MEMTIS** kernel to reproduce the **bottom** part of Figure 13. This test takes approximately **70 minutes**.

```sh
cd userspace
./run-fig13-memtis-1_2.sh
./run-fig13-memtis-1_16.sh
```

Summarize the results and convert them to CSV format. You can find the test results in `results_{memtis, tpp}.csv`,

```sh
python3 ./convert_memtis_results_to_csv.py ./results_memtis
python3 ./convert_tpp_results_to_csv.py ./results_tpp
```

Before reproducing Figure 14,  you may rename the `results_memtis` folder and `results_memtis.csv` to avoid confusion with the results of Figure 14. Of course, you can choose not to do this, so that `results_memtis.csv` will contain all the test results of Figure 13-14, and you can distinguish different workload configurations through the `workload_config` field in `results_memtis.csv`.

**Remove the fast-tier memory limit** and switch to the **MEMTIS** kernel to reproduce Figure 14. This test takes approximately **50 minute**s.

```sh
cd userspace
./run-fig14.sh
```

Summarize the results and convert them to CSV format. You can find the test results in `results_memtis.csv`.

```sh
python3 ./convert_memtis_results_to_csv.py ./results_memtis
```
