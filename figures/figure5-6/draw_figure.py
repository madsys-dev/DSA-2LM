# Author: LRL
import re
import matplotlib.pyplot as plt

re_exp1 = r"average copy time is (\d+) ns"
re_exp2 = r"Bandwidth: (\d+) MB/s"

def read(filepath: str, reg_exp: str) -> list[int]:
    ret = []
    with open(filepath, mode='r') as f:
        data = f.readlines()
    pattern = re.compile(reg_exp)
    for s in data:
        match = pattern.search(s)
        if match:
            ret.append(match.group(1))
    if len(ret) == 0:
        print(filepath)
    ret = list(map(int, ret))
    return ret

# 以 channel 为横坐标，绘制 latency 和 bandwidth
def draw_chan_xticks():
    LOG_DIR = f"results/latency"
    CHAN = [1, 2, 4, 8]
    ORDER = 9
    COPY_METHODS= "dsa_multi_copy_pages"
    r = [x for x in range(len(CHAN))]
    data: list[float] = []
    for chan in CHAN:
        try:
            latency = read(f"{LOG_DIR}/{COPY_METHODS}-{chan}-{ORDER}.log", re_exp1)[0]
        except FileNotFoundError:
            latency = 0
        data.append(latency / 1000.0)
        
    fig, ax1 = plt.subplots(figsize=(10, 6))
    ax1: plt.Axes
    ax1.set_xticks(r, CHAN)
    ax1.set_xlabel('Channel')
    ax1.plot(r, data, marker='o', color='yellowgreen', label='Latency')
    ax1.set_ylabel('Latency (us)')
    ax1.set_title(f'Latency and bandwidth on 2MB HugePage Copy using {COPY_METHODS} with different channels')
    ax1.grid(True, linestyle='--')

    data: list[float] = []
    LOG_DIR = f"results/bandwidth"
    for chan in CHAN:
        try:
            bandwidth = read(f"{LOG_DIR}/{COPY_METHODS}-{chan}-{ORDER}.log", re_exp2)[0]
        except FileNotFoundError:
            bandwidth = 0
        data.append(bandwidth / 1024.0)

    ax2: plt.Axes = ax1.twinx()
    ax2.plot(r, data, marker='s', color='orange', label='Bandwidth')
    ax2.set_ylabel('Bandwidth (GB/s)')
    ax2.grid(True, linestyle='--')

    handles1, labels1 = ax1.get_legend_handles_labels()
    handles2, labels2 = ax2.get_legend_handles_labels()
    ax2.legend(handles1 + handles2, labels1 + labels2, loc='upper right', bbox_to_anchor=(1, 0.2))


    plt.savefig(f"channel.png", format='png')

# 以 batch size 为横坐标，绘制平均拷贝单个页面（4KB) 的 latency 和 bandwidth
def draw_batch_size_xticks():
    LOG_DIR = f"results/latency"
    CHAN = 1
    ORDER = [0, 1, 2, 3, 4, 5, 6, 7]
    COPY_METHODS= "dsa_batch_copy_pages"
    BATCH_SIZE = [f"{(2 ** x) * 4}KB" for x in ORDER]
    r = [x for x in range(len(BATCH_SIZE))]
    data: list[float] = []
    for order in ORDER:
        try:
            file = f"{LOG_DIR}/dsa_multi_copy_pages-1-0.log" if order == 0 else f"{LOG_DIR}/{COPY_METHODS}-{CHAN}-{order}.log"
            latency = read(file, re_exp1)[0]
        except FileNotFoundError:
            latency = 0
        data.append(latency / (2 ** order))
    
    fig, ax1 = plt.subplots(figsize=(10, 6))
    ax1: plt.Axes
    ax1.set_xticks(r, BATCH_SIZE)
    ax1.set_xlabel('Batch Size')
    ax1.plot(r, data, marker='o', color='yellowgreen', label='Latency')
    ax1.set_ylabel('Latency (ns)')
    ax1.set_title(f'Average latency on 4KB page and bandwidth using {COPY_METHODS} with different batch size')
    ax1.grid(True, linestyle='--')

    data: list[float] = []
    LOG_DIR = f"results/bandwidth"
    for order in ORDER:
        try:
            file = f"{LOG_DIR}/dsa_multi_copy_pages-1-0.log" if order == 0 else f"{LOG_DIR}/{COPY_METHODS}-{CHAN}-{order}.log"
            bandwidth = read(file, re_exp2)[0]
        except FileNotFoundError:
            bandwidth = 0
        data.append(bandwidth / 1024.0)
    
    ax2: plt.Axes = ax1.twinx()
    ax2.plot(r, data, marker='s', color='orange', label='Bandwidth')
    ax2.set_ylabel('Bandwidth (GB/s)')
    ax2.grid(True, linestyle='--')

    handles1, labels1 = ax1.get_legend_handles_labels()
    handles2, labels2 = ax2.get_legend_handles_labels()
    ax2.legend(handles1 + handles2, labels1 + labels2, loc='upper right', bbox_to_anchor=(1, 0.2))

    plt.savefig(f"batch_size.png", format='png')
    

def main():
    draw_chan_xticks()
    draw_batch_size_xticks()

DSA_CONFIG = "dsas-4e1w-d"
main()