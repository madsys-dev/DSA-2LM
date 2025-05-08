# DSA-2LM: A CPU-Free Tiered Memory Architecture with Intel DSA

Tiered Memory is critical to manage heterogeneous memory devices, such as Persistent Memory or CXL Memory. Existing works make difficult trade-offs between optimal data placement and costly data movement. With the advent of Intel Data Streaming Accelerator (DSA), a CPU-free hardware to move data between memory regions, data movement can be up to 4x faster than a single CPU core. However, the fine memory movement granularity in Linux kernel undermines the potential performance improvement. To this end, we have developed DSA-2LM, a new tired memory system that adaptively integrates DSA into page migration. The proposed framework integrates fast memory migrating process and adaptable concurrent data path with well-tuned DSA configurations.

This artifact provides the source code of MEMTIS and TPP kernels, both with DSA integrated, and scripts to reproduce the evaluation results in our paper.

## Note

All commands have been tested on Alibaba Cloud Linux 3 with root privileges, where the `sudo` prefix may be omitted. If you are using Ubuntu or other distributions, you may need to change the package management commands accordingly.

## Prerequisites

### Hardware requirements

T.B.D

### Software requirements

T.B.D

## Usage

### Download the code

```sh
git clone https://github.com/madsys-dev/DSA-2LM.git
cd DSA-2LM
git submodule update --init --recursive
```

### Compiling the code

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

⚠️ Based on our experience, Intel DSA may not work properly under the default 4-level page table. We recommend enabling 5-level page table support via: `Processor type and features -> Enable 5-level page tables support`。

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

### Switch a kernel

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

## Reproducing Steps

We provide the necessary code and scripts to reproduce Figures 3–6, 10, and 13–14.

⚠️ Since the Linux 6.x kernel provides more stable support for DSA, we recommend running the microbenchmarks (Figures 3–6, 10) under a 6.x kernel. Since we have backported the IDXD/IOMMU drivers to MEMTIS and TPP kernels, you may also run on those kernels and get similar results.

💡 For artifact evaluation, please refer to the *Switch a kernel* section to boot into the `6.4.16-export+` kernel with 6.x support.


For Python scripts, you need a Python 3.11.8 environment with matplotlib installed.

💡 For artifact evaluation, you could directly use our python environment via `conda activate matplotlib`.


### Figure 3

Reproducing Figure 3 involves two steps. The first run generates results for DSA and `memcpy`. Then, after modifying the source code, a second run will generate results for SIMD (`movdir64b`).

First, copy the scripts into the `dsa-perf-micros` directory:

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

T.B.D

### Figure 5-6

The following three test scripts take approximately 9s + 9s + 1min35s (total about 2min). The raw results will be saved in the `figures/figure5-6/results` directory:

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

This test script takes approximately 1.5 minutes to complete. The raw results are stored in the `figures/figure10/results` directory:

```sh
cd figure10
./dsa_test.sh
```

Then convert the raw results into CSV format:

```sh
python3 ./convert_to_csv.py
```

### Figure 13-14

T.B.D