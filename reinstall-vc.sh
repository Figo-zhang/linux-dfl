
# echo 'func __kernfs_remove +p' > /sys/kernel/debug/dynamic_debug/control
echo 'module platform +p' > /sys/kernel/debug/dynamic_debug/control
#echo 'module dir +p' > /sys/kernel/debug/dynamic_debug/control
echo 'module core +p' > /sys/kernel/debug/dynamic_debug/control

rmmod dfl_fme
rmmod dfl_fme_br
rmmod dfl_fme_region
rmmod dfl_afu
rmmod dfl_intel_s10_iopll
rmmod dfl_emif
rmmod uio_dfl
rmmod dfl_fme_br
rmmod dfl_fme_mgr
rmmod dfl_pci
rmmod dfl_fpga_reload

rmmod n5010_hssi
rmmod n5010_phy
rmmod qsfp_mem
rmmod intel_m10_bmc_spi
rmmod intel_m10_bmc_hwmon
rmmod intel_m10_bmc_log

rmmod intel_m10_bmc_sec_update
rmmod intel_m10_bmc_pmci
rmmod intel_m10_bmc
rmmod fpga_image_load

rmmod ptp_dfl_tod
rmmod dfl_n3000_nios

# rmmod dfl_fme_region
# rmmod dfl_fme

rmmod dfl

rmmod fpga_region
rmmod fpga_bridge
rmmod fpga_mgr

insmod drivers/fpga/fpga-mgr.ko dyndbg=+p
insmod drivers/fpga/fpga-bridge.ko dyndbg=+p
insmod drivers/fpga/fpga-region.ko dyndbg=+p

insmod drivers/fpga/dfl.ko dyndbg=+p
insmod drivers/fpga/dfl-n3000-nios.ko dyndbg=+p
insmod drivers/fpga/dfl-fme-mgr.ko dyndbg=+p
insmod drivers/fpga/dfl-fme-br.ko dyndbg=+p
insmod drivers/fpga/dfl-fme-region.ko dyndbg=+p
insmod drivers/fpga/dfl-fme.ko dyndbg=+p
insmod drivers/fpga/dfl-afu.ko dyndbg=+p

# insmod drivers/fpga/uio-dfl.ko dyndbg=+p

insmod drivers/fpga/dfl-fpga-reload.ko dyndbg=+p
insmod drivers/fpga/dfl-pci.ko dyndbg=+p
