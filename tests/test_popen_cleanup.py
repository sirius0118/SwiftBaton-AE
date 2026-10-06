#!/usr/bin/env python3
"""Exercise the actual U/K PID helper and verify child cleanup on every exit."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PopenCleanupTest(unittest.TestCase):
    def test_pid_helper_reaps_children(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            docker = directory / "docker"
            docker.write_text('''#!/bin/sh
case "$2" in
  empty) exit 0;;
  header) echo "UID PID";;
  malformed) printf 'UID PID\\nbad\\n';;
  *) printf 'UID PID\\nroot 1234\\n';;
esac
''')
            docker.chmod(0o755)
            for tree in ("criu", "criu-k"):
                with self.subTest(tree=tree):
                    source = (ROOT / tree / "criu/uffd.c").read_text()
                    start = source.index("pid_t get_container_pid(")
                    end = source.index("\n}", start) + 2
                    fixture = '''#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
''' + source[start:end] + '''
int main(void) {
    const char *cases[] = {"valid", "empty", "header", "malformed"};
    for (int i = 0; i < 4; i++) {
        assert(get_container_pid(cases[i]) == (i == 0 ? 1234 : -1));
        errno = 0;
        assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    }
    return 0;
}
'''
                    (directory / "test.c").write_text(fixture)
                    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                                   ["-Wall", "-Wextra", "-Werror", str(directory / "test.c"),
                                    "-o", str(directory / "test")], check=True)
                    subprocess.run([str(directory / "test")], check=True, capture_output=True,
                                   env={**os.environ, "PATH": str(directory) + os.pathsep + os.environ["PATH"]})


if __name__ == "__main__":
    unittest.main()
