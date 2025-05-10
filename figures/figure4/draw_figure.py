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

# 以 channel 为横坐标，绘制 bandwidth
def draw_chan_xticks():
    LOG_DIR = f"results/bandwidth"
    CHAN = [1, 2, 4, 8]
    ORDER = [0, 3, 6, 9]
    COLORS = ['yellowgreen', 'orange', 'blue', 'red']
    markers = ['o', 's', '^', 'D']
    COPY_METHODS= "dsa_multi_copy_pages"
    r = [x for x in range(len(CHAN))]
        
    fig, ax1 = plt.subplots(figsize=(10, 6))
    ax1: plt.Axes
    ax1.set_xticks(r, CHAN)
    ax1.set_xlabel('Channel')
    for i, order in enumerate(ORDER):
        data: list[float] = []
        
        for chan in CHAN:
            try:
                bandwidth = read(f"{LOG_DIR}/{COPY_METHODS}-{chan}-{order}.log", re_exp2)[0]
            except FileNotFoundError:
                bandwidth = 0
            data.append(bandwidth / 1024.0)
        
        label_name = f"dsa({4 * (2 ** order)}KB)"
        ax1.plot(r, data, marker=markers[i], color=COLORS[i], label=label_name)
        
    ax1.set_ylabel('Bandwidth (GB/s)')
    ax1.set_title(f'Bandwidth on various size using {COPY_METHODS} with different channels')
    ax1.grid(True, linestyle='--')
    ax1.legend()

    plt.savefig(f"channel.png", format='png')
    

def main():
    draw_chan_xticks()

DSA_CONFIG = "dsas-4e1w-d"
main()