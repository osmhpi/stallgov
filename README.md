# StallGov - A CpuFreq Governor based on memory access patterns

StallGov is a Linux CPU frequency governor based on hardware performance measurement counters (PMCs). These counters are special purpose registers providing low-overhead runtime measurements of microarchitectural hardware events. StallGov estimates the data throughput of the cache hierarchy resulting from the access patterns of the currently running CPU process. In response, it makes dynamic voltage and frequency scaling decisions to minimize the energy consumption during cycles when the CPU experiences a memory stall.

At some point in time this project was called `memutil`, you will find the name still around.

See also:

* [Documentation](https://github.com/osmhpi/stallgov-docs)
* [Evaluation scripts](https://github.com/osmhpi/stallgov-evaluation)


## Dependencies

A Linux Kernel with version 5.14 or above.

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
    - kmod

### Modifing the Linux kernel

Some minor tweaks are required to use the kernel module. See [KERNEL_HACKING.md](KERNEL_HACKING.md) for details.


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

After compilation, you can inspect the module by using `modinfo stallgov.ko`.

The output should look something like this:
```
filename:       <yourpath>/stallgov.ko
nel-module/stallgov.ko
description:    A CpuFreq governor based on Memory Access Patterns.
author:         Erik Griese, Leon Matthes, Maximilian Stiede
license:        GPL
depends:
retpoline:      Y
name:           stallgov
vermagic:       5.15.5-100.fc34.x86_64 SMP mod_unload
```


### Inserting

To insert the module, run: `insmod stallgov.ko`

`cpupower frequency-info` should now list `stallgov` as one of the available governors.
If you have an intel cpu you likely have to disable intel\_pstate first. See "Disabling intel\_pstate".

Switch to the governor by using `cpupower frequency-set -g stallgov`.


#### Module Parameters

You can customize the stallgov module by providing parameters on insertion. **These are read on startup only!**

List all parameters by reading the directory `ls /sys/module/stallgov/parameters`.

We currently support the parameters `event_name1`, `event_name2`, `event_name3` to customize the perf counters to read from. Provide them by stating them on insertion e.g. `insmod stallgov.ko event_name1="inst_retired.any"`.

Additionally we support `max_ipc` and `min_ipc` if the module is build with the IPC heuristic. These can be used to adjust the heuristic's behaviour.
For the offcore stalls heuristic `max_stalls_per_cycle` and `min_stalls_per_cycle` are available.


### Removing

Before removing the stallgov kernel module, please switch back to another governor like schedutil.

Then run `rmmod stallgov.ko` to remove the module from your kernel.


### Reloading the governor

After making changes to the governors code, run the `make_reload.sh` shell script, to disable, remove, rebuild, reinsert and enable stallgov with one command.
The stallgov governor must be active when this command is run, otherwise it will fail.


## Output log
You can view the debug output of stallgov via `dmesg`.
Further debug data can be read from DebugFS at `/sys/kernel/debug/stallgov/` and `copy-log.sh` for details.
