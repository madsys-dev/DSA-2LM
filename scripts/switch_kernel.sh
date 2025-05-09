#!/bin/bash

# Kernel Switch Script - For Alibaba Cloud Linux
# Author: LRL

# Check if running as root
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: Please run this script with root privileges"
    exit 1
fi

# Ensure entries directory exists
if [ ! -d "/boot/loader/entries" ]; then
    echo "Error: /boot/loader/entries directory does not exist, cannot continue"
    exit 1
fi

# Get prefix (assuming all entries have the same prefix)
PREFIX=$(ls /boot/loader/entries/*.conf 2>/dev/null | head -n 1 | sed 's/.*\/\([^-]*\)-.*/\1/')

if [ -z "$PREFIX" ]; then
    echo "Error: No kernel configuration files found"
    exit 1
fi

echo "=== Alibaba Cloud Linux Kernel Switch Tool ==="
echo "Current system: $(cat /etc/os-release | grep "PRETTY_NAME" | cut -d '"' -f 2)"
echo

# Display current kernel info
echo "Current running kernel: $(uname -r)"
CURRENT_KERNEL=$(grub2-editenv list | grep saved_entry | cut -d '=' -f 2)
echo "Current default boot kernel: ${CURRENT_KERNEL#*-}"
echo

# Get all available kernels
echo "Available kernel list:"
KERNELS=()
i=1

# Get all kernel config files
for conf in $(ls /boot/loader/entries/*.conf | sort -V); do
    # Extract kernel name, remove prefix and suffix
    kernel_name=$(basename "$conf" .conf)
    kernel_name=${kernel_name#"$PREFIX-"}
    
    # Skip rescue kernels
    if [[ "$kernel_name" == *"rescue"* ]]; then
        continue
    fi
    
    # Display kernel options
    echo "[$i] $kernel_name"
    KERNELS[$i]=$kernel_name
    ((i++))
done

# Exit if no kernels found
if [ ${#KERNELS[@]} -eq 0 ]; then
    echo "Error: No available kernels found"
    exit 1
fi

# Let user select a kernel
echo
read -p "Please select a kernel number to switch to [1-$((i-1))]: " choice

# Validate input
if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -ge "$i" ]; then
    echo "Error: Invalid selection"
    exit 1
fi

# Get selected kernel
selected_kernel="${KERNELS[$choice]}"
echo "You selected: $selected_kernel"

# Execute kernel switch
echo "Switching to kernel $selected_kernel..."
grub_entry="$PREFIX-$selected_kernel"

echo "Executing: grub2-set-default $grub_entry"
grub2-set-default "$grub_entry"

echo "Executing: grub2-mkconfig -o /boot/efi/EFI/alinux/grub.cfg"
grub2-mkconfig -o /boot/efi/EFI/alinux/grub.cfg

# Verify switch was successful
echo
echo "Verifying switch result:"
grub2-editenv list | grep saved_entry

echo
echo "Kernel switch completed. Reboot the system to use the new kernel."
echo "Use 'reboot' command to restart the system"