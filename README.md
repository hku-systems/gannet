# gannet

#### Start:
1. Primary:
```bash
~/qemu$ sudo x86_64-softmmu/qemu-system-x86_64 \
-machine pc-i440fx-2.3,accel=kvm,usb=off \
-netdev tap,id=hn0,script=/etc/qemu-ifup,downscript=/etc/qemu-ifdown\
,colo_script=./scripts/colo-proxy-script.sh,forward_nic=eth1 \
-device virtio-net-pci,id=net-pci0,netdev=hn0 \
-boot c \
-drive if=virtio,id=disk1,driver=quorum,read-pattern=fifo,cache=none,aio=native\
,children.0.file.filename=/home/hkucs/colo_ubuntu.img\
,children.0.driver=raw \
-vnc :70 -m 16384 -smp 16 -device piix3-usb-uhci -device usb-tablet \
-monitor stdio -S -qmp tcp:localhost:4445,server,nowait
```
2. Secondary:
```bash
~/qemu$ sudo x86_64-softmmu/qemu-system-x86_64 \
-machine pc-i440fx-2.3,accel=kvm,usb=off \
-netdev tap,id=hn0,script=/etc/qemu-ifup,downscript=/etc/qemu-ifdown\
,colo_script=./scripts/colo-proxy-script.sh,forward_nic=eth6 \
-device virtio-net-pci,id=net-pci0,netdev=hn0 \
-drive if=none,driver=raw,file.filename=/home/hkucs/colo_ubuntu.img\
,id=colo1,cache=none,aio=native \
-drive if=virtio,driver=replication,mode=secondary,throttling.bps-total-max=70000000\
,file.file.filename=/local/ubuntu/colo_final_active.img,file.driver=qcow2\
,file.l2-cache-size=4M\
,file.backing.backing_reference=colo1 \
-vnc :70 -m 16384 -smp 16 -device piix3-usb-uhci -device usb-tablet \
-monitor stdio -incoming tcp:0:8888 -qmp tcp:localhost:4445,server,nowait
```
3. Secondary monitor:
```bash
nbd_server_start <IP addr>:<port>
nbd_server_add -w colo1
```

4. Primary monitor:

```
child_add disk1 child.driver=replication,child.mode=primary,child.file.host=<IP addr>,child.file.port=<port>,child.file.export=colo1,child.file.driver=nbd,child.ignore-errors=on
migrate_set_capability colo on
migrate tcp:<IP addr>:<port>
```

#### Basic Test:
1. Sysbench command:
```
./sysbench —test=fileio —file-total-size=1MB —file-test-mode=seqwr —file-num=1 —debug=on —file-block-size=65536 —file-fsync-all=on prepare
```
```
./sysbench —test=fileio —file-total-size=1G —file-test-mode=seqwr —file-num=1 —debug=on —file-block-size=65536 —file-fsync-all=on run
```


2. Colo adjust checkpoint period:

2.1 Enable Capabilities:
```
{ "execute": "qmp_capabilities" }
```

2.2 Print checkpoint debug information
```
{'execute': 'trace-event-set-state', 'arguments': {'name': 'colo*', 'enable': true} }
```

2.3 Set checkpoint period: measurement ms
```
{ "execute": "colo-set-checkpoint-period", "arguments": { "value": 1000 }}
{ "execute": "colo-set-checkpoint-period", "arguments": { "value": 6000000 }}
```

```
{ "execute": "colo-set-checkpoint-period", "arguments": { "value": 5000 }}
```
2.4 Print COW debug message:
```
{'execute': 'trace-event-set-state', 'arguments': {'name': 'backup*', 'enable': true} }
```
2.4 Telnet host
```
telnet localhost 4445
```

3. sysbench make
```
./configure \
	--prefix=$PWD/../sysbench-install
cp /usr/bin/libtool .
make
make install
```


4. qemu
```
cd qemu
./configure --target-list=x86_64-softmmu --enable-colo
make
```
