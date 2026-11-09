#!/usr/bin/env python3
"""
Master Test Runner for ESPHome Neurio OpenEVSE Bridge
Runs both native C++ unit tests and Python simulation tests.
"""

import subprocess
import sys
import os

def run_cpp_tests():
    print("=" * 60)
    print("1. Running Native C++ Unit Tests (Math, 64-bit counters, Queue, ACKs)")
    print("=" * 60)
    
    test_dir = os.path.dirname(os.path.abspath(__file__))
    source_file = os.path.join(test_dir, "test_unit.cpp")
    exe_file = os.path.join(test_dir, "test_unit.exe" if os.name == "nt" else "test_unit")
    
    compiler = None
    for candidate in ("g++", "clang++"):
        if subprocess.run(["which", candidate], stdout=subprocess.DEVNULL).returncode != 0:
            continue
        probe = subprocess.run(
            [candidate, "-std=c++17", "-x", "c++", "-c", "-o", "/tmp/neurio-compiler-probe.o", "-"],
            input=b"int main(){return 0;}\n",
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if probe.returncode == 0:
            compiler = candidate
            break
    if compiler is None:
        print("[FAIL] No working C++ compiler (g++ or clang++).")
        return False
    # Compile
    compile_cmd = [compiler, "-std=c++17", "-Wall", "-Wextra", source_file, "-o", exe_file]
    print(f"Compiling: {' '.join(compile_cmd)}")
    res = subprocess.run(compile_cmd)
    if res.returncode != 0:
        print("[FAIL] Compilation of test_unit.cpp failed!")
        return False
        
    # Execute
    res = subprocess.run([exe_file])
    return res.returncode == 0

def run_python_tests():
    print("\n" + "=" * 60)
    print("2. Running Python Simulation Tests (Payload parsing, Outage simulation)")
    print("=" * 60)
    
    test_dir = os.path.dirname(os.path.abspath(__file__))
    cmd = [sys.executable, "-m", "unittest", "discover", "-s", test_dir, "-p", "test_*.py", "-v"]
    res = subprocess.run(cmd)
    return res.returncode == 0

if __name__ == "__main__":
    cpp_ok = run_cpp_tests()
    py_ok = run_python_tests()
    
    print("\n" + "=" * 60)
    print("TEST SUMMARY")
    print("=" * 60)
    print(f"  C++ Unit Tests:       {'PASS' if cpp_ok else 'FAIL'}")
    print(f"  Python Simulation:    {'PASS' if py_ok else 'FAIL'}")
    print("=" * 60)
    
    if cpp_ok and py_ok:
        print("ALL TESTS PASSED SUCCESSFULLY! (100% OK)")
        sys.exit(0)
    else:
        print("SOME TESTS FAILED.")
        sys.exit(1)
