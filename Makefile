obj-m := udmabuf.o
size = 1048576

all:
	make -C /usr/src/kernel M=$(PWD) modules

clean:
	make -C /usr/src/kernel M=$(PWD) clean

ins:
	insmod $(obj-m:.o=.ko) udmabuf0=$(size)

rm:
	rmmod $(obj-m:.o=)
