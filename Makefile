obj-m := reloaded.o
reloaded-objs := main.o reboot.o proc-arm920.o arm-mmu.o

clean:
	rm -f $(reloaded-objs) $(obj-m) *.o reloaded.ko
	rm -f .*.cmd
	rm -rf .tmp_versions
	rm -f *.mod.c



