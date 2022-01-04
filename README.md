# memutil - A CpuFreq Governor based on memory access patterns

This module is based on the guide: https://thegeekstuff.com/2013/07/write-linux-kernel-module/

## Prerequsites

Unfortunately, as of the time of writing, you need a patched kernel.

See [this page](https://kernelnewbies.org/KernelBuild) on how to compile & install your own kernel.

For fedora, I recommend you follow [this guide](https://fedoraproject.org/wiki/Building_a_custom_kernel#Building_a_kernel_from_the_exploded_git_trees) and use the instructions under the *Building a kernel from the exploded git trees* section.

Then add this line:

```
EXPORT_SYMBOL_GPL(perf_event_read_local);
```

After the definition of `perf_event_read_local` in `kernel/events/core.c` .

At last, compile and install the new kernel.

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

## Compilation
To compile the module, simply run `make`.
To remove the binaries, run `make clean`.

## Inserting & Removing the module
After compilation, you can inspect the module by using `modinfo memutil.ko` (possibly with sudo).

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
To insert the module, run: `sudo insmod memutil.ko`

`cpupower frequency-info` should now list `memutil` as one of the available governors.
You'll possibly have to disable intel_pstate first.

Switch to the governor by using `cpupower frequency-set -g memutil`.

### Removing
Before removing the memutil kernel module, please switch back to another governor like schedutil.

Then run `sudo rmmod memutil.ko` to remove the module from your kernel.

## Output log
You can view the debug output of memutil via `dmesg`.
