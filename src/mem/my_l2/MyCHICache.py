# Copyright (c) 2026
# All rights reserved.

from m5.objects import CHIGenericController
from m5.params import *


class MyCHICache(CHIGenericController):
    type = "MyCHICache"
    cxx_header = "mem/my_l2/my_chi_cache.hh"
    cxx_class = "gem5::ruby::MyCHICache"
