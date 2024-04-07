import shutil, os, subprocess
from testing import Test, BuildTest, pexpect

class Xv6Test(BuildTest, Test):
   name = "all"
   description = "build xv6 using make"
   timeout = 30
   tester = "tester.c"
   make_qemu_args = ""
   point_value=0
   success_pattern = "SUCCESS"
   failure_pattern = 'FAILED'

   def __call__(self):
       return run(self)

   def run(self):
      tester_path = self.test_path + "/" + self.tester
      self.log("Running xv6 user progam " + str(tester_path))
      shutil.copy(tester_path, self.project_path + "/tester.c")

      # shawgerj copy Makefile to Makefile.test and gawk tester.c into UPROGS
#      cmd = "gawk '($1 == \"_mkdir\\\") { printf(\"\t_tester\\\n\"); } { print $0 }'"
      cmd = [self.test_path + "/edit-makefile.sh",
             self.project_path + "/Makefile",
             self.project_path + "/Makefile.test"]

      # with open(self.test_path + "/Makefile", "r") as m:
      #    with open(self.project_path + "/Makefile.test", "w+") as mtest:
      #       subprocess.Popen(cmd, stdin=m, stdout=mtest, shell=True)

      subprocess.Popen(cmd)
      is_success = self.make(["xv6.img", "fs.img"])
      if not is_success:
         return # stop test on if make fails

      target = "qemu-nox " + self.make_qemu_args
      if self.use_gdb:
         target = "qemu-gdb " + self.make_qemu_args
      self.log("make " + target)
      child = pexpect.spawn("make -f Makefile.test " + target,
            cwd=self.project_path,
            logfile=self.logfd,
            timeout=None)
      self.children.append(child)

      if self.use_gdb:
         gdb_child = subprocess.Popen(
               ["xterm", "-title", "\"gdb\"", "-e", "gdb"],
               cwd=self.project_path)
         self.children.append(gdb_child)

      child.expect_exact("init: starting sh")
      child.expect_exact("$ ")
      child.sendline("tester")
      child.expect_exact("tester")
       
      # check if test passed
      # Omid: Define an expected output for each test
      patterns = ["FAILED", "cpu\d: panic: .*\n", "SUCCESS", self.success_pattern, "[$][ ]", self.failure_pattern]
      index = child.expect(patterns)
      if index == 0:
         self.fail("tester failed")
      elif index == 1:
         self.fail("xv6 kernel panic")
      elif index == 4:
        self.fail("tester failed")
      elif index == 5:
        self.fail("seg fault")

      if self.use_gdb:
         child.wait()
      else:
         child.close()
      self.done()

class Xv6Build(BuildTest):
   name = "build"
   description = "build xv6 using make"
   timeout = 60
   targets = ["xv6.img", "fs.img"]


   def __call__(self):
       return self.done()
