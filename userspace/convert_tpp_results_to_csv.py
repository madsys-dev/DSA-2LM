# Author: LRL52
import os
import sys
import csv
import re

def read(filepath: str, reg_exp: str) -> list:
    ret = []
    with open(filepath, mode='r') as f:
        data = f.readlines()
    pattern = re.compile(reg_exp)
    for s in data:
        matches = pattern.findall(s)
        for match in matches:
            ret.append(match)
        
    if len(ret) == 0:
        print(f"Warning: {filepath} has no match")
        return []
    try:
        ret = list(map(int, ret))
    except:
        ret = list(map(float, ret))
    return ret


float_reg = r'([+-]?\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)'

def get_reg_exp(workload: str) -> str:
    return rf"execution time {float_reg} \(s\)"

def main():
    if len(sys.argv) < 2:
        print("Usage: python convert_results_to_csv.py <results_folder>")
        sys.exit(1)
    
    results_folder = sys.argv[1]
    
    header = ["workload_config", "performance"]
    with open("results_tpp.csv", mode='w') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)
        
    data = dict()

    for root, dirs, files in os.walk(results_folder):
        # if ("pandas" in dirs):
        #     dirs.remove("pandas")
        if "output.log" not in files:
            continue
        workload = root.strip("./")
        workload = workload.removeprefix("results_tpp")
        workload = workload.strip("/")
        workload_name = workload.split("/")[0]
        ratio = workload.split("/")[-1]
        workload_config = workload.replace("/", "-")
        
        if "dsa" in workload_config:
            method = "dsa"
        else:
            method = "baseline"
        
        reg_exp = get_reg_exp(workload_name)
        file_path = os.path.join(root, "output.log")
        result = read(file_path, reg_exp)
        
        if len(result) == 0:
            continue
        
        if workload_name not in data:
            data[workload_name] = dict()
        if ratio not in data[workload_name]:
            data[workload_name][ratio] = dict()
        if method not in data[workload_name][ratio]:
            data[workload_name][ratio][method] = []
        data[workload_name][ratio][method].extend(result)
        
    with open("results_tpp.csv", mode='a') as csvfile:
        writer = csv.writer(csvfile)
        for workload_name, ratios in data.items():
            for ratio, methods in ratios.items():
                for method, results in methods.items():
                    writer.writerow([f"{workload_name}-{ratio}-{method}"] + results)

if __name__ == "__main__":
    main()
