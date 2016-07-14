echo 7 > /proc/sys/kernel/printk
mount -t debugfs none /debug
mknod /dev/bm_stdio c 0 242
#modprobe virtio
#modprobe virtio_ring
#modprobe virtio_rpmsg_bus
#insmod cfg_mgmt.ko
#insmod bm_stdio.ko
modprobe remoteproc.ko
modprobe zynq_remoteproc.ko
usleep 10000
modprobe virtio_rpmsg_bus
insmod bm_stdio.ko
insmod cfg_mgmt.ko
#echo "updating config vars"
#cat /debug/cfg_mgmt/update
echo "DONE"
#sleep 1
#cat trace
