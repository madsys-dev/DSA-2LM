# Author: LRL
import re
import matplotlib.pyplot as plt
import csv

# float_reg = r'[+-]?\d+(?:\.\d*)?(?:[eE][+-]?\d+)?'
re_exp1 = r"average copy time is (\d+) ns"
# re_exp2 = r"Bandwidth: (\d+) MB/s"

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
   

def main():
    METHODS = ["cpu_copy_page_lists", "mix_copy_page_lists", "dsa_copy_page_lists"]
    WORKLOADS = ["workload-a", "workload-b"]
    
    header = ["config", "latency", "bandwidth"]
    with open(f"data.csv", mode='w') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)
        
    for workload in WORKLOADS:
        if workload == "workload-a":
            size_GB = 1023 * 4 * 1024 + 1 * 2 * 1024 * 1024
        else:
            assert workload == "workload-b"
            size_GB = 1000 * 4 * 1024 + 24 * 2 * 1024 * 1024
        size_GB /= 1024 * 1024 * 1024
        for method in METHODS:
            LOG_FILE = f"{LOG_DIR}/{method}-{workload}.log"
            x = read(LOG_FILE, re_exp1)[0]
            time_s = x * 1e-9
            bandwidth = size_GB / time_s
            with open(f"data.csv", mode='a') as csvfile:
                writer = csv.writer(csvfile)
                writer.writerow([f"{method}-{workload}", x, bandwidth])
            
    
LOG_DIR= "results"
main()