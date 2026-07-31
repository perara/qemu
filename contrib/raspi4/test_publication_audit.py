#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from contrib.raspi4.publication_audit import AuditError, audit
except ModuleNotFoundError:
    from publication_audit import AuditError, audit


def git(repo: Path, *args: str) -> str:
    return subprocess.check_output(
        ("git", *args), cwd=repo, text=True).strip()


class PublicationAuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name)
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.name", "Test Author")
        git(self.repo, "config", "user.email", "author@example.com")
        (self.repo / "README").write_text("baseline\n", encoding="utf-8")
        git(self.repo, "add", "README")
        git(self.repo, "commit", "-q", "-s", "-m", "baseline")
        self.baseline = git(self.repo, "rev-parse", "HEAD")

    def tearDown(self):
        self.temp.cleanup()

    def commit(self, name: str, content: str, *, signoff: bool = True) -> None:
        (self.repo / name).write_text(content, encoding="utf-8")
        git(self.repo, "add", name)
        command = ["commit", "-q", "-m", f"add {name}"]
        if signoff:
            command.insert(1, "-s")
        git(self.repo, *command)

    def test_accepts_licensed_signed_source(self):
        self.commit(
            "tool.py",
            "#!/usr/bin/env python3\n" +
            "# SPDX-License-" + "Identifier: GPL-2.0-or-later\n",
        )
        result = audit(self.repo, self.baseline, 1024)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["commits"], 1)
        self.assertEqual(result["licensed_added_sources"], 1)

    def test_rejects_missing_signoff(self):
        self.commit(
            "tool.py",
            "# SPDX-License-" + "Identifier: GPL-2.0-or-later\n",
            signoff=False,
        )
        with self.assertRaisesRegex(AuditError, "sign-off"):
            audit(self.repo, self.baseline, 1024)

    def test_rejects_missing_spdx(self):
        self.commit("tool.py", "print('unsafe')\n")
        with self.assertRaisesRegex(AuditError, "SPDX"):
            audit(self.repo, self.baseline, 1024)

    def test_rejects_private_key(self):
        self.commit(
            "notes.txt",
            "-----BEGIN " + "PRIVATE KEY-----\nnot-a-real-key\n",
        )
        with self.assertRaisesRegex(AuditError, "credential"):
            audit(self.repo, self.baseline, 1024)

    def test_rejects_restricted_image(self):
        self.commit("disk.img", "not-an-image")
        with self.assertRaisesRegex(AuditError, "binary artifact"):
            audit(self.repo, self.baseline, 1024)

    def test_rejects_tracked_symbolic_link(self):
        (self.repo / "outside").write_text(
            "-----BEGIN " + "PRIVATE KEY-----\n", encoding="utf-8")
        (self.repo / "notes.txt").symlink_to("outside")
        git(self.repo, "add", "notes.txt")
        git(self.repo, "commit", "-q", "-s", "-m", "add symbolic link")
        with self.assertRaisesRegex(AuditError, "symbolic link"):
            audit(self.repo, self.baseline, 1024)


if __name__ == "__main__":
    unittest.main()
