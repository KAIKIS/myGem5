# Copyright (c) 2026
# All rights reserved.

from m5.params import *
from m5.objects.ClockedObject import ClockedObject

class MyL2Cache(ClockedObject):
    type = "MyL2Cache"
    cxx_header = "mem/my_l2/my_l2_cache.hh"
    cxx_class = "gem5::MyL2Cache"

    cpu_side = ResponsePort("CPU side port, receives from L1")
    mem_side = RequestPort("Memory side port, sends to memory")

    latency = Param.Cycles(10, "Hit latency in cycles")
    addr_range = Param.AddrRange(AddrRange(0), "Address range this cache handles")
