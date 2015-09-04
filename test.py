#!/usr/bin/env python
# Automated script to run the pspautotests test suite in PPSSPP.

import sys
import os
import subprocess
import threading
import glob


PPSSPP_EXECUTABLES = [
  # Windows
  "Windows\\Debug\\PPSSPPHeadless.exe",
  "Windows\\Release\\PPSSPPHeadless.exe",
  "Windows\\x64\\Debug\\PPSSPPHeadless.exe",
  "Windows\\x64\\Release\\PPSSPPHeadless.exe",
  # Mac
  "build*/Debug/PPSSPPHeadless",
  "build*/Release/PPSSPPHeadless",
  "build*/RelWithDebInfo/PPSSPPHeadless",
  "build*/MinSizeRel/PPSSPPHeadless",
  # Linux
  "build*/PPSSPPHeadless",
  "./PPSSPPHeadless"
]

PPSSPP_EXE = None
TEST_ROOT = "pspautotests/tests/"
teamcity_mode = False
TIMEOUT = 5

class Command(object):
  def __init__(self, cmd, data = None):
    self.cmd = cmd
    self.data = data
    self.process = None
    self.output = None
    self.timeout = False

  def run(self, timeout):
    def target():
      self.process = subprocess.Popen(self.cmd, bufsize=1, stdin=subprocess.PIPE, stdout=sys.stdout, stderr=subprocess.STDOUT)
      self.process.stdin.write(self.data)
      self.process.stdin.close()
      self.process.communicate()

    thread = threading.Thread(target=target)
    thread.start()

    thread.join(timeout)
    if thread.isAlive():
      self.timeout = True
      if sys.version_info < (2, 6):
        os.kill(self.process.pid, signal.SIGKILL)
      else:
        self.process.terminate()
      thread.join()

# Test names are the C files without the .c extension.
# These have worked and should keep working always - regression tests.
tests_good = [
  "cpu/cpu_alu/cpu_alu",
  "cpu/vfpu/vector",
  "cpu/vfpu/matrix",
  "cpu/vfpu/convert",
  "cpu/vfpu/prefixes",
  "cpu/vfpu/colors",
  "cpu/vfpu/gum",
  "cpu/icache/icache",
  "cpu/lsu/lsu",
  "cpu/fpu/fpu",

  "audio/atrac/ids",
  "audio/atrac/setdata",
  "audio/mp3/mp3test",
  "audio/sascore/sascore",
  "ctrl/ctrl",
  "ctrl/idle/idle",
  "ctrl/sampling/sampling",
  "ctrl/sampling2/sampling2",
  "display/display",
  "display/vblankmulti",
  "dmac/dmactest",
  "gpu/callbacks/ge_callbacks",
  "gpu/ge/edram",
  "gpu/ge/context",
  "gpu/ge/queue",
  "hash/hash",
  "hle/check_not_used_uids",
  "intr/intr",
  "intr/suspended",
  "intr/vblank/vblank",
  "io/cwd/cwd",
  "loader/bss/bss",
  "malloc/malloc",
  "misc/testgp",
  "misc/libc",
  "misc/deadbeef",
  "misc/dcache",
  "misc/timeconv",
  "mstick/mstick",
  "rtc/rtc",
  "string/string",
  "sysmem/freesize",
  "sysmem/sysmem",
  "threads/alarm/alarm",
  "threads/alarm/cancel/cancel",
  "threads/alarm/refer/refer",
  "threads/alarm/set/set",
  "threads/callbacks/callbacks",
  "threads/callbacks/check",
  "threads/callbacks/create",
  "threads/callbacks/delete",
  "threads/callbacks/refer",
  "threads/events/events",
  "threads/events/cancel/cancel",
  "threads/events/clear/clear",
  "threads/events/create/create",
  "threads/events/delete/delete",
  "threads/events/poll/poll",
  "threads/events/refer/refer",
  "threads/events/set/set",
  "threads/events/wait/wait",
  "threads/fpl/fpl",
  "threads/fpl/allocate",
  "threads/fpl/cancel",
  "threads/fpl/create",
  "threads/fpl/delete",
  "threads/fpl/free",
  "threads/fpl/priority",
  "threads/fpl/tryallocate",
  "threads/k0/k0",
  "threads/lwmutex/create",
  "threads/lwmutex/delete",
  "threads/lwmutex/lock",
  "threads/lwmutex/priority",
  "threads/lwmutex/refer",
  "threads/lwmutex/try",
  "threads/lwmutex/try600",
  "threads/lwmutex/unlock",
  "threads/mbx/mbx",
  "threads/mbx/cancel/cancel",
  "threads/mbx/create/create",
  "threads/mbx/delete/delete",
  "threads/mbx/poll/poll",
  "threads/mbx/priority/priority",
  "threads/mbx/receive/receive",
  "threads/mbx/refer/refer",
  "threads/mbx/send/send",
  "threads/msgpipe/msgpipe",
  "threads/msgpipe/cancel",
  "threads/msgpipe/create",
  "threads/msgpipe/data",
  "threads/msgpipe/delete",
  "threads/msgpipe/receive",
  "threads/msgpipe/refer",
  "threads/msgpipe/send",
  "threads/msgpipe/tryreceive",
  "threads/msgpipe/trysend",
  "threads/mutex/cancel",
  "threads/mutex/create",
  "threads/mutex/delete",
  "threads/mutex/lock",
  "threads/mutex/mutex",
  "threads/mutex/priority",
  "threads/mutex/refer",
  "threads/mutex/try",
  "threads/mutex/unlock",
  "threads/semaphores/semaphores",
  "threads/semaphores/cancel",
  "threads/semaphores/create",
  "threads/semaphores/delete",
  "threads/semaphores/poll",
  "threads/semaphores/priority",
  "threads/semaphores/refer",
  "threads/semaphores/signal",
  "threads/semaphores/wait",
  "threads/threads/change",
  "threads/threads/extend",
  "threads/threads/refer",
  "threads/threads/release",
  "threads/threads/rotate",
  "threads/threads/stackfree",
  "threads/threads/start",
  "threads/threads/suspend",
  "threads/threads/threadend",
  "threads/threads/threads",
  "threads/wakeup/wakeup",
  "threads/vpl/allocate",
  "threads/vpl/cancel",
  "threads/vpl/delete",
  "threads/vpl/free",
  "threads/vpl/priority",
  "threads/vpl/refer",
  "threads/vpl/try",
  "threads/vpl/vpl",
  "threads/vtimers/vtimer",
  "threads/vtimers/cancelhandler",
  "threads/vtimers/create",
  "threads/vtimers/delete",
  "threads/vtimers/getbase",
  "threads/vtimers/gettime",
  "threads/vtimers/interrupt",
  "threads/vtimers/refer",
  "threads/vtimers/settime",
  "threads/vtimers/start",
  "threads/vtimers/stop",
  "utility/savedata/autosave",
  "utility/savedata/filelist",
  "utility/savedata/makedata",
  "utility/systemparam/systemparam",
  "power/power",
  "power/volatile/lock",
  "power/volatile/trylock",
  "power/volatile/unlock",
  "umd/callbacks/umd",
  "umd/wait/wait",
  "io/directory/directory",
]

tests_next = [
# These are the next tests up for fixing. These run by default.
  "cpu/cpu_alu/cpu_branch",
  "cpu/fpu/fcr",
  "audio/atrac/atractest",
  "audio/atrac/decode",
  "audio/atrac/resetting",
  "audio/sceaudio/datalen",
  "audio/sceaudio/output",
  "audio/sceaudio/reserve",
  "ctrl/vblank",
  "display/hcount",
  "intr/waits",
  "threads/callbacks/cancel",
  "threads/callbacks/count",
  "threads/callbacks/notify",
  "threads/scheduling/dispatch",
  "threads/scheduling/scheduling",
  "threads/threads/create",
  "threads/threads/terminate",
  "threads/vtimers/sethandler",
  "threads/vpl/create",
  "utility/savedata/getsize",
  "utility/savedata/idlist",
  "utility/savedata/sizes",
  "gpu/commands/basic",
  "gpu/commands/material",
  "gpu/complex/complex",
  "gpu/displaylist/state",
  "gpu/ge/get",
  "gpu/ge/break",
  "gpu/reflection/reflection",
  "gpu/rendertarget/rendertarget",
  "gpu/signals/jumps",
  "gpu/signals/simple",
  "gpu/simple/simple",
  "gpu/triangle/triangle",
  "font/fonttest",
  "io/file/file",
  "io/file/rename",
  "io/io/io",
  "io/iodrv/iodrv",
  "modules/loadexec/loader",
  "rtc/arithmetic",
  "rtc/convert",
  "rtc/lookup",
  "sysmem/partition",
  "umd/io/umd_io",
  "umd/raw_access/raw_access",
  "video/mpeg/basic",
  "video/pmf/pmf",
  "video/pmf_simple/pmf_simple",
  "video/psmfplayer/basic",
]

# These don't even run (or run correctly) on the real PSP
test_broken = [
]


# These are the tests we ignore (not important, or impossible to run)
tests_ignored = [
  "kirk/kirk",
  "me/me",

  "umd/umd", # mostly fixed but output seems broken? (first retval of unregister...)
]



def init():
  global PPSSPP_EXE, TEST_ROOT
  if not os.path.exists("pspautotests"):
    if os.path.exists(os.path.dirname(__file__) + "/pspautotests"):
      TEST_ROOT = os.path.dirname(__file__) + "/pspautotests/tests/";
    else:
      print("Please run git submodule init; git submodule update;")
      sys.exit(1)

  if not os.path.exists(TEST_ROOT + "cpu/cpu_alu/cpu_alu.prx"):
    print("Please install the pspsdk and run make in common/ and in all the tests")
    print("(checked for existence of cpu/cpu_alu/cpu_alu.prx)")
    sys.exit(1)

  possible_exes = [glob.glob(f) for f in PPSSPP_EXECUTABLES]
  possible_exes = [x for sublist in possible_exes for x in sublist]
  existing = filter(os.path.exists, possible_exes)
  if existing:
    PPSSPP_EXE = max((os.path.getmtime(f), f) for f in existing)[1]
  else:
    PPSSPP_EXE = None

  if not PPSSPP_EXE:
    print("PPSSPP executable missing, please build one.")
    sys.exit(1)

def tcprint(arg):
  global teamcity_mode
  if teamcity_mode:
    print(arg)

def run_tests(test_list, args):
  global PPSSPP_EXE, TIMEOUT
  tests_passed = []
  tests_failed = []

  test_filenames = []
  for test in test_list:
    # Try prx first
    elf_filename = TEST_ROOT + test + ".prx"
    if not os.path.exists(elf_filename):
      print("WARNING: no prx, trying elf")
      elf_filename = TEST_ROOT + test + ".elf"

    test_filenames.append(elf_filename)

  if len(test_filenames):
    # TODO: Maybe --compare should detect --graphics?
    cmdline = [PPSSPP_EXE, '--compare', '--timeout=' + str(TIMEOUT), '@-']
    cmdline.extend([i for i in args if i not in ['-g']])

    c = Command(cmdline, '\n'.join(test_filenames))
    c.run(TIMEOUT * len(test_filenames))

    print("Ran " + PPSSPP_EXE)


def main():
  global teamcity_mode
  init()
  tests = []
  args = []
  for arg in sys.argv[1:]:
    if arg == '--teamcity':
      teamcity_mode = True
      args.append(arg)
    elif arg[0] == '-':
      args.append(arg)
    else:
      tests.append(arg)

  if not tests:
    if '-g' in args:
      tests = tests_good
    else:
      tests = tests_next + tests_good

  run_tests(tests, args)

main()
