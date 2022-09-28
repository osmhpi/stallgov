# Compiling kernel

Using the memutil governor requires some minor changes to the linux kernel.

Follow these steps:

 1. Download kernel sources (e.g. `git clone git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git` or `git clone git://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git`)
 2. Copy your config into the downloaded source (`cd linux-stable` and `cp /boot/config-`uname -r`* .config
 3. make menuconfig to adjust the config if needed (also sets newly available config options to the default)
 4. Open the .config and adjust `CONFIG_SYSTEM_TRUSTED_KEYS="debian/canonical-certs.pem"` to `CONFIG_SYSTEM_TRUSTED_KEYS=""`. It can happen that the same has to be done for `CONFIG_SYSTEM_REVOCATION_KEYS`
 5a. On ubuntu or similar distros `make bindeb-pkg LOCALVERSION=-custom` and install the resulting header package and an image package (e.g. `linux-headers-5.11.22-custom_5.11.22-custom-4_amd64.deb` and `linux-image-5.11.22-custom_5.11.22-custom-4_amd64.deb`) (they are in the folder that contains the git repo) with `dpkg -i <package>`
 5b. Otherwise `make install` might also work

## Compiling cpupower for custom kernel

 1. In your kernel directory, go to `/tools/power/cpupower/`
 2. Install dependencies if neccessary: `gettext libpci-dev libelf-dev libpopt-dev`
 3. `sudo make install`
 4. You might have to add the resulting libraries to the lookup path in `/etc/ld.so.conf` by adding a line with `/usr/lib64/`, then run `sudo ldconfig`
 5. `cpupower` should now work

# Compiling kernel with address sanitizer etc.

Adjust the config when compiling the kernel, some sample changes used to find a memory corruption are in the resources/kasan-config.patch.

# Running custom kernel with ubuntu in qemu

Based on http://nickdesaulniers.github.io/blog/2018/10/24/booting-a-custom-linux-kernel-in-qemu-and-debugging-it-with-gdb/ and https://spyff.github.io/linux/2018/07/20/qemu-vm/

 1. Use a default config as the start (i.e. `make defconfig`) and adjust it for kvm with `make kvm_guest.config`)
 2. Build the kernel with `make`
 3. Get qemu
 4. Do `echo "add-auto-load-safe-path path/to/linux/scripts/gdb/vmlinux-gdb.py" >> ~/.gdbinit` if needed
 5. Get an ubuntu cloud image, e.g. https://cloud-images.ubuntu.com/eoan/current/eoan-server-cloudimg-amd64.img
 6. Create an image based on the downloaded one `qemu-img create -f qcow2 -b eoan-server-cloudimg-amd64.img ubuntu.img`
 7. Create the init.yaml as
```
#cloud-config
hostname: ubu 
users:
  - name: myusername
    ssh-authorized-keys:
      - ssh-rsa AAAAB3Nz[...]34twdf/ myusername@pc 
    sudo: ['ALL=(ALL) NOPASSWD:ALL']
    groups: sudo
    shell: /bin/bash
```

with your own public ssh key and create the init.img as `cloud-localds init.img init.yaml`
 8. Run qemu with the kernel and images: `qemu-system-x86_64 -kernel arch/x86_64/boot/bzImage -machine accel=kvm,type=q35 --append "root=/dev/sda1 console=ttyS0" -hda ubuntu.img -hdb init.img -m 2048 --nographic -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0`
 9. Connect via ssh as `ssh 0 -p2222`
 10. If anything goes wrong with the initialization (via init.img) the ubuntu.img has to be recreated to force initialization to happen again
