from m5.objects import CHIGenericController
from m5.params import *


class CHIMiddleware(CHIGenericController):
    type = "CHIMiddleware"
    cxx_header = "mem/my_l2/chi_middleware.hh"
    cxx_class = "gem5::ruby::CHIMiddleware"
