# DPU Ping-Pong

The repo contains code for two programs:

**dpdk_pingpong**: A simple program to evaluate raw DPDK latency with DPU forwarding. The client sends a certain number of packets to the server as a `ping`, then the server returns them to the client as a `pong`.
The client records such ping-pong round trip time.
The number of packets to send is based on the user-specified message size range.

**dpu_fwd**: A simple program to forward network packet from port `x` to port `x^1`.
Currently, every port will be automatically assigned to a core.

**Note** the programs have been evaluated on 2 Ubuntu 18.04 machines with DPDK 20.11.2 and BlueField-2 DPUs with DPDK 20.11.2.

## Prepare

For the DPDK installation and setup, please refer to the DPDK documentation (https://core.dpdk.org/doc/quick-start/).

## Build

We can use either `cmake` or `make` to build the program.

We are using `pkg-config` to locate DPDK.

```shell
export PKG_CONFIG_PATH=/path/to/DPDK/pkgconfig
```

To use `make`
```shell
cd /path/to/dpu-pingpong
make dpdk_pingpong
make dpu_fwd
```

To use `cmake`
```shell
cd /path/to/dpu-pingpong
mkdir build
cd build
cmake ..
make dpdk_pingpong
make dpu_fwd
```

## Run

On the server host
```shell
sudo ./dpdk_pingpong -- -s
```

On the client host
```shell
sudo ./dpdk_pingpong -- -c [server MAC]
```

On both the server and the client DPUs
```shell
sudo ./dpu_fwd -l 0-1
```

## Acknowledgement
The initial code is based on https://github.com/zylan29/dpdk-pingpong
