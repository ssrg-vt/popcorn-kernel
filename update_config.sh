#!/bin/bash      
# Make sure you have the kernel config right
for option in SWAP TRANSPARENT_HUGEPAGE CMA MIGRATION COMPACTION KSM MEM_SOFT_DIRTY      
do       
sed -i '/^CONFIG_'$option'/s/y/n/' .config     
done
