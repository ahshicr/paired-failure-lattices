"""Download, resume and reassemble the experimental artifact without login."""
import argparse
import concurrent.futures
import hashlib
import json
import pathlib
import time
import urllib.request

BASE_URL = "https://github.com/ahshicr/paired-failure-lattices/releases/download/v20261110/"
EXPECTED_SHA256 = "cf233b24b28e6f3a3a1240629a9ae2c14d9b2b9b65c4dab211887815164894fc"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("."))
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    request = urllib.request.Request(BASE_URL + "download_manifest.json",
                                     headers={"User-Agent": "paired-failure-lattices-artifact"})
    with urllib.request.urlopen(request, timeout=120) as stream:
        manifest = json.load(stream)
    assert manifest["archive_sha256"] == EXPECTED_SHA256
    assert manifest["base_url"] == BASE_URL
    target = output / "reproducibility.zip"
    if target.exists() and sha(target) == EXPECTED_SHA256:
        print("The complete artifact is already present and verified.")
        return
    parts_dir = output / "reproduction_download_parts"
    parts_dir.mkdir(exist_ok=True)

    def download(part):
        name = part["name"]
        assert pathlib.PurePosixPath(name).name == name
        path = parts_dir / name
        if path.exists() and path.stat().st_size == part["bytes"] and sha(path) == part["sha256"]:
            print("Verified existing " + name, flush=True)
            return path
        temporary = path.with_suffix(path.suffix + ".partial")
        for attempt in range(4):
            try:
                request = urllib.request.Request(BASE_URL + name,
                    headers={"User-Agent": "paired-failure-lattices-artifact"})
                with urllib.request.urlopen(request, timeout=180) as stream, temporary.open("wb") as out:
                    while chunk := stream.read(1024 * 1024):
                        out.write(chunk)
                assert temporary.stat().st_size == part["bytes"]
                assert sha(temporary) == part["sha256"]
                temporary.replace(path)
                print("Downloaded and verified " + name, flush=True)
                return path
            except Exception:
                if attempt == 3:
                    raise
                time.sleep(min(2**attempt, 8))

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.workers, 8))) as pool:
        paths = list(pool.map(download, manifest["parts"]))
    temporary = output / "reproducibility.zip.partial"
    with temporary.open("wb") as stream:
        for path in paths:
            with path.open("rb") as part:
                while chunk := part.read(1024 * 1024):
                    stream.write(chunk)
    assert temporary.stat().st_size == manifest["archive_bytes"]
    assert sha(temporary) == EXPECTED_SHA256
    temporary.replace(target)
    print("Complete archive verified at " + str(target))
    print("Extract it and follow REPRODUCE.txt. The retained parts allow interrupted downloads to resume.")


if __name__ == "__main__":
    main()
