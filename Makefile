obj-m += rzchroma.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

sign:
	kmodsign sha512 /var/lib/shim-signed/mok/MOK.priv /var/lib/shim-signed/mok/MOK.der $(shell basename ${PWD}).ko

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

.PHONY: all clean sign
