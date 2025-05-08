# Author: LRL
import re
import csv

float_reg = r'[+-]?\d+(?:\.\d*)?(?:[eE][+-]?\d+)?'

def read(filepath: str, reg_exp: str) -> list[float]:
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
    ret = list(map(float, ret))
    return ret

def convert_to_csv(data: dict, filename: str, transfer_sizes: list):
    # Prepare the header with transfer sizes
    header = ['config'] + transfer_sizes
    
    with open(filename, mode='w') as csvfile:
        writer = csv.writer(csvfile)
        # writer = csv.writer(csvfile, quoting=csv.QUOTE_MINIMAL)
        writer.writerow(header)  # Write header
        
        # Iterate through the data and write each row for GB and latency
        for lr in data:
            for mode in data[lr]:
                bandwidth = data[lr][mode]['GB']
                
                # Create row for bandwidth
                mode_name = "memcpy" if mode == "cpu" else \
                            "DSA" if mode == "sync" else \
                            "movdir64b" if mode == "simd" else \
                            "??"
                config = f"{mode_name}-{lr}-bandwidth"
                writer.writerow([config] + bandwidth)
                
        for lr in data:
            for mode in data[lr]:
                latency = data[lr][mode]['latency']
                
                # Create row for latency
                mode_name = "memcpy" if mode == "cpu" else \
                            "DSA" if mode == "sync" else \
                            "movdir64b" if mode == "simd" else \
                            "??"
                config = f"{mode_name}-{lr}-latency"
                writer.writerow([config] + latency)

def main():
    LR="L,L L,R R,L R,R".split()
    MODE="cpu sync".split()
    TRANSFER_SIZE="64 128 256 512 1K 2K 4K 8K 16K 64K 128K 256K 512K 1M 2M".split()
    LOG_DIR = f"{LOG_DIR_PREFIX1}"

    data: dict[str, dict[str, dict[str, list[float]]]] = {}
    for lr in LR:
        data[lr] = {}
        for mode in MODE:
            data[lr][mode] = {}
            data[lr][mode]['GB'], data[lr][mode]['latency'] = [], []
            for transfer_size in TRANSFER_SIZE:
                GB_ps = read(f"{LOG_DIR}/{mode}-{lr}-{transfer_size}.log", rf"GB per sec = ({float_reg})")[0]
                data[lr][mode]['GB'].append(GB_ps)
                latency = read(f"{LOG_DIR}/{mode}-{lr}-{transfer_size}-latency.log", rf"({float_reg}) ns")[0]
                data[lr][mode]['latency'].append(latency)

    MODE="simd".split()
    LOG_DIR = f"{LOG_DIR_PREFIX2}"
    for lr in LR:
        for mode in MODE:
            data[lr][mode] = {}
            data[lr][mode]['GB'], data[lr][mode]['latency'] = [], []
            for transfer_size in TRANSFER_SIZE:
                GB_ps = read(f"{LOG_DIR}/cpu-{lr}-{transfer_size}.log", rf"GB per sec = ({float_reg})")[0]
                data[lr][mode]['GB'].append(GB_ps)
                latency = read(f"{LOG_DIR}/cpu-{lr}-{transfer_size}-latency.log", rf"({float_reg}) ns")[0]
                data[lr][mode]['latency'].append(latency)
    
    # Call the function to save data to CSV
    convert_to_csv(data, "data.csv", TRANSFER_SIZE)
    
    
    
LOG_DIR_PREFIX1 = "./results/4e1w-d"
LOG_DIR_PREFIX2 = "./results/movdir64b"
main()