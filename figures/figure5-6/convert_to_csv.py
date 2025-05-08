# Author: LRL
import re
import csv

# float_reg = r'[+-]?\d+(?:\.\d*)?(?:[eE][+-]?\d+)?'
re_exp1 = r"average copy time is (\d+) ns"
re_exp2 = r"Bandwidth: (\d+) MB/s"

def read(filepath: str, reg_exp: str) -> list[int]:
    ret = []
    with open(filepath, mode='r') as f:
        data = f.readlines()
    pattern = re.compile(reg_exp)
    for s in data:
        matches = pattern.findall(s)
        for match in matches:
            ret.append(match)
        
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
        # data.append(latency / 1000.0) # ns->us
        data.append(latency) # ns
        
    header = ['config'] + CHAN
    
    with open("channel.csv", mode='w') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)
        writer.writerow(["latency"] + data)

    data: list[float] = []
    LOG_DIR = f"results/bandwidth"
    for chan in CHAN:
        try:
            bandwidth = read(f"{LOG_DIR}/{COPY_METHODS}-{chan}-{ORDER}.log", re_exp2)[0]
        except FileNotFoundError:
            bandwidth = 0
        data.append(bandwidth / 1024.0) # MB/s->GB/s
        
    with open("channel.csv", mode='a') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["bandwidth"] + data)

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
        data.append(latency / (2 ** order)) # average latency per page
        
    header = ['config'] + BATCH_SIZE
    
    with open("batch_size.csv", mode='w') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)
        writer.writerow(["latency"] + data)

    data: list[float] = []
    LOG_DIR = f"results/bandwidth"
    for order in ORDER:
        try:
            file = f"{LOG_DIR}/dsa_multi_copy_pages-1-0.log" if order == 0 else f"{LOG_DIR}/{COPY_METHODS}-{CHAN}-{order}.log"
            bandwidth = read(file, re_exp2)[0]
        except FileNotFoundError:
            bandwidth = 0
        data.append(bandwidth / 1024.0) # MB/s->GB/s
    
    with open("batch_size.csv", mode='a') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["bandwidth"] + data)
    

def main():
    draw_chan_xticks()
    draw_batch_size_xticks()

DSA_CONFIG = "dsas-4e1w-d"
main()