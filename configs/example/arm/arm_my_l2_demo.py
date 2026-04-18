"""
ARM单核SE模式demo，使用gem5自带L1 Cache + 自定义MyL2Cache。

架构: ARM CPU → gem5 L1I + L1D → L2XBar → MyL2Cache → Memory

用法:
    scons build/ARM/gem5.opt -j$(nproc)
    ./build/ARM/gem5.opt configs/example/arm/arm_my_l2_demo.py

在标准输出中可以看到 MyL2Cache 打印的请求信息（地址、类型等）。
"""

import os

import m5
from m5.objects import *

# 导入gem5自带的L1 Cache定义
import sys
sys.path.append(os.path.join(os.path.dirname(__file__), '..', '..'))
from common.Caches import L1_ICache, L1_DCache

# ---- 系统基本设置 ----
system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

# ---- CPU ----
system.cpu = TimingSimpleCPU()

# ---- L1 Cache（gem5自带）----
# 与 configs/common/Caches.py 中的定义一致
system.cpu.icache = L1_ICache(size="32KiB", assoc=2)
system.cpu.dcache = L1_DCache(size="32KiB", assoc=2)

# ---- L2 总线 ----
# L1的mem_side 连接到 toL2Bus，toL2Bus 连接到 MyL2Cache
system.toL2Bus = L2XBar()

# ---- MyL2Cache（我们自定义的）----
system.my_l2 = MyL2Cache(latency=10)

# ---- 连接: CPU icache/dcache → toL2Bus → MyL2Cache → membus ----

# CPU icache → L1_ICache
system.cpu.icache.cpu_side = system.cpu.icache_port
system.cpu.icache.mem_side = system.toL2Bus.cpu_side_ports

# CPU dcache → L1_DCache
system.cpu.dcache.cpu_side = system.cpu.dcache_port
system.cpu.dcache.mem_side = system.toL2Bus.cpu_side_ports

# toL2Bus → MyL2Cache
system.toL2Bus.mem_side_ports = system.my_l2.cpu_side

# MyL2Cache → membus
system.membus = SystemXBar()
system.my_l2.mem_side = system.membus.cpu_side_ports

# ---- 中断控制器 (ARM不需要pio连接) ----
system.cpu.createInterruptController()

# ---- 内存控制器 ----
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# ---- System port ----
system.system_port = system.membus.cpu_side_ports

# ---- Workload: ARM hello world ----
process = Process()
thispath = os.path.dirname(os.path.realpath(__file__))
binpath = os.path.join(
    thispath, "../../../", "tests/test-progs/hello/bin/arm/linux/hello"
)
process.cmd = [binpath]
system.cpu.workload = process
system.cpu.createThreads()
system.workload = SEWorkload.init_compatible(binpath)

# ---- 运行仿真 ----
root = Root(full_system=False, system=system)
m5.instantiate()

print("Beginning simulation!")
print(f"Binary: {binpath}")
print("Architecture: CPU -> L1I/L1D -> L2XBar -> MyL2Cache -> Memory")
print("=" * 60)

exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
