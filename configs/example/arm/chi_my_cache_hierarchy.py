"""
CHI协议 + 自定义MyCHICache demo

架构:
    ARM TimingCPU → L1 (gem5 CHI) → Network → MyCHICache (你的实现) → Memory (gem5 CHI)

其中:
    - L1 Cache使用gem5现有的CHI PrivateL1MOESICache
    - MyCHICache替换目录(Home Node)，你在这里自己实现CHI处理逻辑
    - MemoryController使用gem5现有的CHI MemoryController

用法:
    scons build/ARM/gem5.opt -j$(nproc)
    ./build/ARM/gem5.opt configs/example/arm/chi_my_cache_hierarchy.py
"""

import os
from itertools import chain

import m5
from m5.objects import (
    MessageBuffer,
    MyCHICache,
    NULL,
    RubyPortProxy,
    RubySequencer,
    RubySystem,
)
from m5.objects.SubSystem import SubSystem

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.abstract_cache_hierarchy import (
    AbstractCacheHierarchy,
)
from gem5.components.cachehierarchies.ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)
from gem5.components.cachehierarchies.chi.nodes.abstract_node import (
    AbstractNode,
)
from gem5.components.cachehierarchies.chi.nodes.private_l1_moesi_cache import (
    PrivateL1MOESICache,
)
from gem5.components.cachehierarchies.chi.nodes.memory_controller import (
    MemoryController,
)
from gem5.components.cachehierarchies.ruby.topologies.simple_pt2pt import (
    SimplePt2Pt,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.abstract_core import AbstractCore
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires
from gem5.utils.override import overrides


class MyCHICacheHierarchy(AbstractRubyCacheHierarchy):
    """
    CHI缓存层次：
    L1 (gem5 CHI) → Network → MyCHICache (你自己的实现) → MemoryController (gem5)

    MyCHICache扮演 Home Node 角色，替代gem5的SimpleDirectory。
    """

    def __init__(self, l1_size: str, l1_assoc: int) -> None:
        super().__init__()
        self._l1_size = l1_size
        self._l1_assoc = l1_assoc

    @overrides(AbstractCacheHierarchy)
    def get_coherence_protocol(self):
        return CoherenceProtocol.CHI

    @overrides(AbstractRubyCacheHierarchy)
    def _reset_version_numbers(self):
        AbstractNode._version = 0
        MemoryController._version = 0

    @overrides(AbstractCacheHierarchy)
    def incorporate_cache(self, board: AbstractBoard) -> None:
        super().incorporate_cache(board)
        self.ruby_system = RubySystem()

        # Ruby's global network
        self.ruby_system.network = SimplePt2Pt(self.ruby_system)
        self.ruby_system.number_of_virtual_networks = 4
        self.ruby_system.network.number_of_virtual_networks = 4

        # ---- 先创建L1 Cache (消耗version 0, 1 for icache, dcache) ----
        # 注意：此时还没有self.directory，downstream_destinations后面再设置
        self.core_clusters = [
            self._create_core_cluster(core, i, board)
            for i, core in enumerate(board.get_processor().get_cores())
        ]

        # ---- 然后创建 MyCHICache (version会是2，避免冲突) ----
        self.directory = self._create_my_chi_cache(board)
        self.directory.ruby_system = self.ruby_system

        # ---- 现在设置L1的downstream_destinations指向directory ----
        for cluster in self.core_clusters:
            cluster.dcache.downstream_destinations = [self.directory]
            cluster.icache.downstream_destinations = [self.directory]

        # ---- 创建 Memory Controller ----
        self.memory_controllers = self._create_memory_controllers(board)
        self.directory.downstream_destinations = self.memory_controllers

        # ---- Sequencer数量 ----
        self.ruby_system.num_of_sequencers = len(self.core_clusters) * 2

        # ---- 连接网络 ----
        self.ruby_system.network.connectControllers(
            list(
                chain.from_iterable(
                    [
                        (cluster.dcache, cluster.icache)
                        for cluster in self.core_clusters
                    ]
                )
            )
            + self.memory_controllers
            + [self.directory]
        )

        self.ruby_system.network.setup_buffers()

        # System port proxy
        self.ruby_system.sys_port_proxy = RubyPortProxy(
            ruby_system=self.ruby_system
        )
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)

    def _create_my_chi_cache(self, board: AbstractBoard) -> MyCHICache:
        """创建MyCHICache，使用与AbstractNode.connectQueues相同的模式"""
        network = self.ruby_system.network

        cache = MyCHICache(
            version=AbstractNode._version,
            data_channel_size=32,
        )

        # CHI 4通道 MessageBuffer（CHIGenericController的参数）
        cache.reqOut = MessageBuffer()
        cache.rspOut = MessageBuffer()
        cache.snpOut = MessageBuffer()
        cache.datOut = MessageBuffer()
        cache.reqIn = MessageBuffer()
        cache.rspIn = MessageBuffer()
        cache.snpIn = MessageBuffer()
        cache.datIn = MessageBuffer()

        # 连接MessageBuffer到网络（与connectQueues相同的端口连接）
        cache.reqOut.out_port = network.in_port
        cache.rspOut.out_port = network.in_port
        cache.snpOut.out_port = network.in_port
        cache.datOut.out_port = network.in_port
        cache.reqIn.in_port = network.out_port
        cache.rspIn.in_port = network.out_port
        cache.snpIn.in_port = network.out_port
        cache.datIn.in_port = network.out_port

        cache.clk_domain = board.get_clock_domain()

        return cache

    def _create_core_cluster(
        self, core: AbstractCore, core_num: int, board: AbstractBoard
    ) -> SubSystem:
        """创建L1 I/D cache集群"""
        cluster = SubSystem()
        cluster.dcache = PrivateL1MOESICache(
            size=self._l1_size,
            assoc=self._l1_assoc,
            network=self.ruby_system.network,
            core=core,
            cache_line_size=board.get_cache_line_size(),
            target_isa=board.get_processor().get_isa(),
            clk_domain=board.get_clock_domain(),
        )
        cluster.icache = PrivateL1MOESICache(
            size=self._l1_size,
            assoc=self._l1_assoc,
            network=self.ruby_system.network,
            core=core,
            cache_line_size=board.get_cache_line_size(),
            target_isa=board.get_processor().get_isa(),
            clk_domain=board.get_clock_domain(),
        )

        cluster.icache.sequencer = RubySequencer(
            version=core_num,
            dcache=NULL,
            clk_domain=cluster.icache.clk_domain,
            ruby_system=self.ruby_system,
        )
        cluster.dcache.sequencer = RubySequencer(
            version=core_num,
            dcache=cluster.dcache.cache,
            clk_domain=cluster.dcache.clk_domain,
            ruby_system=self.ruby_system,
        )

        if board.has_io_bus():
            cluster.dcache.sequencer.connectIOPorts(board.get_io_bus())

        cluster.dcache.ruby_system = self.ruby_system
        cluster.icache.ruby_system = self.ruby_system

        core.connect_icache(cluster.icache.sequencer.in_ports)
        core.connect_dcache(cluster.dcache.sequencer.in_ports)

        core.connect_walker_ports(
            cluster.dcache.sequencer.in_ports,
            cluster.icache.sequencer.in_ports,
        )

        if board.get_processor().get_isa() == ISA.X86:
            int_req_port = cluster.dcache.sequencer.interrupt_out_port
            int_resp_port = cluster.dcache.sequencer.in_ports
            core.connect_interrupt(int_req_port, int_resp_port)
        else:
            core.connect_interrupt()

        return cluster

    def _create_memory_controllers(
        self, board: AbstractBoard
    ):
        memory_controllers = []
        for rng, port in board.get_mem_ports():
            mc = MemoryController(self.ruby_system.network, [rng], port)
            mc.ruby_system = self.ruby_system
            memory_controllers.append(mc)
        return memory_controllers


# ---- 主程序 ----

requires(
    isa_required=ISA.ARM,
    coherence_protocol_required=CoherenceProtocol.CHI,
)

cache_hierarchy = MyCHICacheHierarchy(
    l1_size="64KiB",
    l1_assoc=8,
)

memory = SingleChannelDDR3_1600(size="32MiB")

processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING,
    isa=ISA.ARM,
    num_cores=1,
)

board = SimpleBoard(
    clk_freq="1GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

thispath = os.path.dirname(os.path.realpath(__file__))
binpath = os.path.join(
    thispath, "../../../", "tests/test-progs/hello/bin/arm/linux/hello"
)
from gem5.resources.resource import BinaryResource

board.set_se_binary_workload(
    binary=BinaryResource(binpath),
)

print("=" * 60)
print("CHI + MyCHICache demo")
print(f"Binary: {binpath}")
print("Architecture: CPU -> L1(CHI) -> Network -> MyCHICache(你的实现) -> Mem(CHI)")
print("=" * 60)

simulator = Simulator(board=board)
simulator.run()
