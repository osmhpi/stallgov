# memutil - A CpuFreq Governor based on memory access patterns

This module is based on the guide: https://thegeekstuff.com/2013/07/write-linux-kernel-module/


## Dependencies

- Ubuntu
    - build-essential
    - linux-headers-$(uname -r)
- Fedora
    - make
    - automake
    - gcc
    - gcc-c++
    - kernel-devel
    - kernel-headers

### Disabling intel_pstate (Intel CPUs only)

To disable intel_pstate add the kernel commandline parameter "intel_pstate=disable". This can be done temporarily by:
1. Restarting your computer
2. During boot force the "GNU GRUB" menu to appear (e.g. by repeatedly pressing ESC)
3. Highlight the line which should be used for booting and press e to enter edit mode
4. Move to the line starting with "linux" and move the cursor to the end of that line.
5. Add a blank space and then insert the kernel command line parameter (e.g. "intel_pstate=disable")
6. Press Ctrl+X to boot the system with these changes.
7. As mentioned these changes are temporary, i.e. the adjustment to the command line will only affect this one boot and the changes to the command line
    will not be there for the next boot.

## Compilation

To compile the module, simply run `make`.
To remove the binaries, run `make clean`.

## Inserting & Removing the module

**Note:** Many of the commands listed below will need to be run as a super user.
If any of them fail, try running them with `sudo` first.

After compilation, you can inspect the module by using `modinfo memutil.ko`.

The output should look something like this:
```
filename:       /home/user/Documents/someproject/memutil.ko
nel-module/memutil.ko
description:    A CpuFreq governor based on Memory Access Patterns.
author:         Erik Griese <erik.griese@student.hpi.de>, Leon Matthes <leon.matthe
s@student.hpi.de>, Maximilian Stiede <maximilian.stiede@student.hpi.de>
license:        GPL
depends:
retpoline:      Y
name:           memutil
vermagic:       5.15.5-100.fc34.x86_64 SMP mod_unload
```


### Inserting

To insert the module, run: `insmod memutil.ko`

`cpupower frequency-info` should now list `memutil` as one of the available governors.
If you have an intel cpu you likely have to disable intel_pstate first. See "Disabling intel_pstate".

Switch to the governor by using `cpupower frequency-set -g memutil`.


#### Module Parameters

You can customize the memutil module by providing parameters on insertion. **These are read on startup only!**

List all parameters by reading the directory `ls /sys/module/memutil/parameters`.

We currently support the parameters `event_name1`, `event_name2`, `event_name3` to customize the perf counters to read from. Provide them by stating them on insertion e.g. `insmod memutil.ko event_name1="inst_retired.any"`.


### Removing

Before removing the memutil kernel module, please switch back to another governor like schedutil.

Then run `rmmod memutil.ko` to remove the module from your kernel.


### Reloading the governor

After making changes to the governors code, run the `make_reload.sh` shell script, to disable, remove, rebuild, reinsert and enable memutil with one command.
The memutil governor must be active when this command is run, otherwise it will fail.


## Output log
You can view the debug output of memutil via `dmesg`.
