# Install Benchmarks

## PageRank

```
git clone https://github.com/sbeamer/gapbs.git

# compile it with twitter dataset
cd gapbs
patch -p1 < ../gapbs-pr.diff
make pr; make pr gen-twitter
```

## Graph500

```
wget https://github.com/graph500/graph500
```

## XSBench
```
git clone https://github.com/ANL-CESAR/XSBench.git
```

## Btree

```
git clone https://github.com/mitosis-project/vmitosis-workloads.git

# change the number of elements and lookup requests
vim btree/btree.c
# see line 61
```

## Pandas

[Polars Decision Support (PDS) benchmarks](https://pola.rs/posts/benchmarks/)

Follow the README in the [polars-benchmark](https://github.com/pola-rs/polars-benchmark) repository to install. 

Note that we change the scale factor to `10.0` and only run pandas test (i.e., `make run-pandas`).