add_interchange_group_test(
    name counter
    family ${family}
    board_list basys3 arty35t arty100t
    tcl run_xilinx.tcl
    sources counter.v
    techmap ../../remap_xilinx.v
)

add_interchange_group_test(
    name counter
    family ${family}
    board_list lifcl17
    tcl run_nexus.tcl
    sources counter.v
    techmap ../../remap_nexus.v
)
