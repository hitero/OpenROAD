source "helpers.tcl"
# abutting blocks off grid
read_lef Nangate45/Nangate45.lef
read_lef extra.lef
read_def check5.def
check_placement -verbose
