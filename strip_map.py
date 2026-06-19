Import("env")

# The toolchain writes the linker map file via a temp response file whose path
# encoding breaks on non-ASCII project directories (e.g. "Yazılım"). We don't
# need the .map artifact, so drop the -Wl,-Map=... flag to keep linking robust.
flags = env.get("LINKFLAGS", [])
env.Replace(LINKFLAGS=[f for f in flags
                       if "Map" not in str(f) and "cref" not in str(f)])
