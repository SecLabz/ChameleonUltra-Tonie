#!/usr/bin/env python3
"""Local ARM compilation; never connects to hardware."""
import hashlib
import pathlib
import shlex
import subprocess
import sys

prefix, flags, includes, sources, *roots = sys.argv[1:]
out = pathlib.Path("build/app")
out.mkdir(parents=True, exist_ok=True)
maps = [(pathlib.Path.cwd(), ".")]
maps += list(zip(map(pathlib.Path, roots), ["nrf5-sdk", "tinyusb", "lwip"]))
paths = [f"-ffile-prefix-map={path.resolve()}={name}" for path, name in maps]
args = shlex.split(flags) + paths + shlex.split(includes)
objects = []
for source in shlex.split(sources):
    p = pathlib.Path(source)
    obj = out / (p.stem + "_" + hashlib.sha256(source.encode()).hexdigest()[:8] + ".o")
    objects.append(str(obj))
    # Include changes must also invalidate objects, hence compile each invocation.
    subprocess.run([prefix + "gcc", *args, "-c", source, "-o", str(obj)], check=True)
elf = "build/app.elf"
subprocess.run(
    [
        prefix + "gcc",
        *shlex.split(flags),
        "-Wl,--gc-sections",
        "-Wl,-Map=build/app.map",
        "-Tsrc/config/app.ld",
        "--specs=nano.specs",
        "--specs=nosys.specs",
        *objects,
        "-o",
        elf,
    ],
    check=True,
)
symbols = subprocess.check_output([prefix + "nm", elf], text=True)
handlers = {
    line.split()[-1]: line.split()[-2]
    for line in symbols.splitlines()
    if len(line.split()) == 3
}
required = [
    "USBD_IRQHandler",
    "POWER_CLOCK_IRQHandler",
    "NFCT_IRQHandler",
    "TIMER4_IRQHandler",
    "RTC1_IRQHandler",
]
if "-DNFCV_DIAGNOSTICS" in args:
    required += ["HardFault_Handler", "__assert_func"]
for handler in required:
    if handlers.get(handler) != "T":
        raise RuntimeError("Missing strong interrupt handler: " + handler)
subprocess.run([prefix + "objcopy", "-O", "ihex", elf, "build/app.hex"], check=True)
subprocess.run([prefix + "objcopy", "-O", "binary", elf, "build/app.bin"], check=True)
subprocess.run([prefix + "size", elf], check=True)
