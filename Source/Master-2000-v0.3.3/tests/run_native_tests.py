"""Run source-including native regression tests; outputs go to a temporary directory."""
import os, pathlib, shutil, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
compiler=os.environ.get("CXX", "g++")
if not shutil.which(compiler): raise SystemExit("Set CXX to an available C++17 compiler")
with tempfile.TemporaryDirectory(prefix="master2000-tests-") as tmp:
    for test in sorted((root/"Master-2000-App/tests").glob("*_test.cpp")):
        out=pathlib.Path(tmp)/(test.stem+(".exe" if os.name=="nt" else ""))
        subprocess.run([compiler,"-std=c++17",os.environ.get("TEST_OPT", "-O2"),str(test),"-o",str(out)],check=True)
        subprocess.run([str(out)],check=True)
        print("PASS",test.stem,flush=True)
