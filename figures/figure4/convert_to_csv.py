# Author: LRL
import re
import csv

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
    COPY_METHODS= "dsa_multi_copy_pages"
    r = [x for x in range(len(CHAN))]
    
    header = ['config'] + CHAN
    
    with open("channel.csv", mode='a') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)
    
    for i, order in enumerate(ORDER):
        data: list[float] = []
        
        for chan in CHAN:
            try:
                bandwidth = read(f"{LOG_DIR}/{COPY_METHODS}-{chan}-{order}.log", re_exp2)[0]
            except FileNotFoundError:
                bandwidth = 0
            data.append(bandwidth / 1024.0)
        
        label_name = f"dsa({4 * (2 ** order)}KB)"
        
        with open("channel.csv", mode='a') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow([label_name] + data)
        
    

def main():
    draw_chan_xticks()

DSA_CONFIG = "dsas-4e1w-d"
main()