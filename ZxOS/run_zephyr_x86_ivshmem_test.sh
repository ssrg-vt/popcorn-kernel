#/home/ashwin/zephyr-sdk-0.13.2/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-objcopy -O elf32-i386 /home/ashwin/zephyrproject/build/zephyr/zephyr.elf /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu.elf

#/home/ashwin/zephyr-sdk-0.13.2/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-objcopy -j .locore /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu.elf /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu-locore.elf 2>&1 | grep -iv "empty loadable segment detected" || true
/home/ashwin/zephyr-sdk-0.13.2/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-objcopy -R .locore /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu.elf /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu-main.elf 2>&1 | grep -iv "empty loadable segment detected" || true

#sudo /home/ashwin/Stramash-QEMU/build/qemu-system-x86_64 -m 8G -cpu qemu64,+x2apic,mmx,mmxext,sse,sse2 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -nographic -no-acpi -net none -pidfile qemu.pid -chardev stdio,id=con,mux=on -serial chardev:con -mon chardev=con,mode=readline -device ivshmem-doorbell,vectors=2,chardev=ivsh -chardev socket,path=/tmp/ivshmem_socket,id=vintchar -chardev socket,path=/tmp/ivshmem_socket,id=ivsh  -chardev socket,path=/tmp/cross_ipi_chr,id=x86_chr  -icount shift=5,align=off,sleep=off -rtc clock=vm -device loader,file=/home/ashwin/zephyrproject/build/zephyr/zephyr-qemu-main.elf -kernel /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu-locore.elf 

/home/ashwin/zephyr-sdk-0.13.2/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-objcopy -O elf32-i386 /home/ashwin/zephyrproject/build/zephyr/zephyr.elf /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu.elf

/home/ashwin/zephyr-sdk-0.13.2/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-objcopy -j .locore /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu.elf /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu-locore.elf 2>&1 | grep -iv "empty loadable segment detected" || true

/home/ashwin/zephyr-sdk-0.13.2/x86_64-zephyr-elf/bin/x86_64-zephyr-elf-objcopy -R .locore /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu.elf /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu-main.elf 2>&1 | grep -iv "empty loadable segment detected" || true

sudo /home/ashwin/Stramash-QEMU/build/qemu-system-x86_64 -m 8G -cpu qemu64,+x2apic,mmx,mmxext,sse,sse2 -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -nographic -no-acpi -net none  -chardev stdio,id=con,mux=on -chardev socket,path=/tmp/ivshmem_socket,id=vintchar -chardev socket,path=/tmp/ivshmem_socket,id=ivsh  -chardev socket,path=/tmp/cross_ipi_chr,id=x86_chr  -serial chardev:con -mon chardev=con,mode=readline -icount shift=5,align=off,sleep=off -rtc clock=vm -device loader,file=/home/ashwin/zephyrproject/build/zephyr/zephyr-qemu-main.elf -kernel /home/ashwin/zephyrproject/build/zephyr/zephyr-qemu-locore.elf
