"""Provision the locked Windows runtime using only embedded Python's stdlib.

This is intentionally NOT a general wheel installer: the manifest contains only
prebuilt, verified wheels; purelib/platlib are relocated, CLI scripts are unused.
No pip, uv, compiler, registry changes, system Python or administrator is needed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile

sys.dont_write_bytecode = True


def fetch(item, cache, offline=False):
    target = cache / item["name"]
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == item["sha256"]:
        return target
    if offline:
        raise RuntimeError("Offline cache missing or checksum mismatch: " + str(target))
    part = target.with_suffix(target.suffix + ".part")
    for attempt in range(3):
        try:
            print("Download: " + item["name"], flush=True)
            with urllib.request.urlopen(item["url"], timeout=120) as source, part.open("wb") as dest:
                shutil.copyfileobj(source, dest)
            if hashlib.sha256(part.read_bytes()).hexdigest() != item["sha256"]:
                raise RuntimeError("SHA256 mismatch: " + item["name"])
            part.replace(target)
            return target
        except Exception:
            part.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def install_wheel(archive, site):
    with zipfile.ZipFile(archive) as wheel:
        for member in wheel.infolist():
            parts = PurePosixPath(member.filename).parts
            if not parts or ".." in parts or PurePosixPath(member.filename).is_absolute():
                raise ValueError("Unsafe wheel path")
            if parts[0].endswith(".data"):
                if len(parts) < 3 or parts[1] not in ("purelib", "platlib"):
                    # Optional CLI scripts/headers do not belong to the runtime.
                    continue
                parts = parts[2:]
            destination = site.joinpath(*parts)
            if member.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
            else:
                destination.parent.mkdir(parents=True, exist_ok=True)
                with wheel.open(member) as source, destination.open("wb") as dest:
                    shutil.copyfileobj(source, dest)


def prune_runtime(site):
    """Remove payloads not used by the offline screenshot OCR worker."""
    bundled_models = site / "rapidocr" / "models"
    if bundled_models.is_dir():
        shutil.rmtree(bundled_models)

    for path in (site / "cv2").glob("opencv_videoio_ffmpeg*.dll"):
        path.unlink()

    for package in ("sympy", "mpmath"):
        shutil.rmtree(site / package, ignore_errors=True)
        for metadata in site.glob(package + "-*.dist-info"):
            shutil.rmtree(metadata, ignore_errors=True)


def verify_worker(python, worker):
    # communicate() closes stdin, so successful initialization must emit exactly
    # ONE ready frame and then exit cleanly; anything printed to stdout fails.
    result = subprocess.run([str(python), "-I", "-B", "-X", "utf8", str(worker)],
                            input=b"", capture_output=True, timeout=120)
    output = result.stdout
    if len(output) < 4:
        raise RuntimeError("Worker emitted no ready frame: " + result.stderr.decode("utf-8", "replace"))
    size = struct.unpack("<I", output[:4])[0]
    if len(output) != size + 4:
        raise RuntimeError("Worker stdout is not exactly one protocol frame")
    response = json.loads(output[4:])
    if result.returncode != 0 or response != {"ready": True}:
        raise RuntimeError("Worker failed: " + repr(response) + "\n" + result.stderr.decode("utf-8", "replace"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    manifest = json.loads((source / "runtime-lock.json").read_text(encoding="utf-8"))
    stage, cache = args.stage.resolve(), args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    # Four concurrent transfers keep CI setup fast without parallel installation.
    artifacts = manifest["wheels"] + manifest["models"] + [manifest["vc_runtime"]]
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda item: fetch(item, cache, args.offline), artifacts))
    site = stage / "Lib/site-packages"
    site.mkdir(parents=True, exist_ok=True)
    for item in manifest["wheels"]:
        install_wheel(cache / item["name"], site)
    prune_runtime(site)
    # Do NOT register the APPX or install a system-wide VC redistributable.
    with zipfile.ZipFile(cache / manifest["vc_runtime"]["name"]) as archive:
        for name in manifest["vc_runtime"]["dlls"]:
            (stage / name).write_bytes(archive.read(name))
    (stage / "models").mkdir(exist_ok=True)
    for item in manifest["models"]:
        shutil.copy2(cache / item["name"], stage / "models" / item["name"])
    for name in ("worker.py", "runtime-lock.json"):
        shutil.copy2(source / name, stage / name)
    (stage / "python311._pth").write_text("python311.zip\n.\nLib/site-packages\nimport site\n", encoding="ascii")
    verify_worker(stage / "python.exe", stage / "worker.py")
    print("Verified: offline worker ready; all artifact SHA256 checks passed.", flush=True)


if __name__ == "__main__":
    main()
