Import('env')

#
# Dump build environment (for debug)
#print env.Dump()
#

# Flattened: platform espressif32 8.0.0 (Arduino 3.3.12) puts lists in LINKFLAGS (the libs' ld_flags
# and ld_scripts), which SCons flattens on the command line but str.join() does not (ePixC).
flags = " ".join(env.Flatten(env['LINKFLAGS']))
flags = flags.replace("-u _printf_float", "")
flags = flags.replace("-u _scanf_float", "")
newflags = flags.split()

env.Replace(
  LINKFLAGS=newflags
)