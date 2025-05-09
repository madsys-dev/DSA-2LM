#!/bin/bash
set -e

# kexec -p ~/linux-6.4.16/arch/x86_64/boot/bzImage --initrd=/boot/initramfs-6.4.16-export+kdump.img –-reuse-cmdline

# Disable hyper-threading
echo off > /sys/devices/system/cpu/smt/control

# Fix the CPU frequency to 3.2GHz
echo 3200000 | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_min_freq

dmesg -C

