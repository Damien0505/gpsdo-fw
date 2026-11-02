# Runs before each PlatformIO build: gzips web/ into data/, which becomes the LittleFS image.
import gzip
import os
import shutil

try:
    Import("env")  # noqa: F821 (provided by PlatformIO)
    project = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    project = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

source = os.path.join(project, "web")
target = os.path.join(project, "data")
shutil.rmtree(target, ignore_errors=True)
total = 0
for folder, _, files in os.walk(source):
    for name in files:
        path = os.path.join(folder, name)
        relative = os.path.relpath(path, source)
        destination = os.path.join(target, relative + ".gz")
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        with open(path, "rb") as raw:
            content = raw.read()
        with open(destination, "wb") as out:
            # mtime 0: same input, same output
            with gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=out, mtime=0) as packed:
                packed.write(content)
        total += os.path.getsize(destination)
print("Web interface: %d bytes gzipped" % total)
