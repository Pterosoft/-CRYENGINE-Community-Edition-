import os
import subprocess

MBEDTLS_DIR = os.getenv("MBEDTLS_DIR")
tmp = os.getcwd()

os.chdir(MBEDTLS_DIR)
os.chdir("tf-psa-crypto")
subprocess.call(
    [
        "python",
        "framework/scripts/make_generated_files.py"
    ]
)
os.chdir("..")
subprocess.call(
    [
        "python",
        "framework/scripts/make_generated_files.py"
    ]
)
os.chdir(tmp)
