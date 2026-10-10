"""Package the pinned, locally tested VFX SDK without installing Python bindings."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

WHEEL = "nvidia_vfx-0.2.0.0-cp312-abi3-win_amd64.whl"
SOURCE = "https://pypi.nvidia.com/nvidia-vfx/" + WHEEL
SHA256 = "cebaa12411f7e22b39293205c5c4d07b8073d14ca1e0e7727ee4375207a36445"
REQUIRED = {"NVCVImage.dll", "NVVideoEffects.dll", "nvngxruntime.dll",
            "nvngx_vsr.dll", "nvVFXVideoSuperRes.dll"}
LICENSES = {"NVIDIA-Open-Model-License-Agreements-24-10-2025.pdf",
            "NVIDIA-Software-License-Agreement-2025.05.05.pdf",
            "product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf",
            "ThirdPartyLicenses_VFX_SDK.txt", "THIRD_PARTY.md"}


def digest_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def package(wheel, output):
    if digest_file(wheel) != SHA256:
        raise ValueError("Maxine 1.3 SDK wheel SHA-256 mismatch")
    output.mkdir(parents=True, exist_ok=True)
    archive = output / "MPCVR-Maxine-Runtime.zip"
    files = []
    with zipfile.ZipFile(wheel) as source:
        libraries = [i for i in source.infolist()
                     if i.filename.startswith("nvvfx/libs/") and i.filename.endswith(".dll")]
        notices = [i for i in source.infolist()
                   if i.filename.startswith("nvidia_vfx-0.2.0.0.dist-info/licenses/packaging/")
                   and not i.is_dir()]
        if not REQUIRED.issubset({Path(i.filename).name for i in libraries}):
            raise ValueError("SDK is missing required Maxine libraries")
        if not LICENSES.issubset({Path(i.filename).name for i in notices}):
            raise ValueError("SDK is missing required license notices")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as dest:
            for entry in sorted(libraries + notices, key=lambda i: i.filename):
                name = Path(entry.filename).name
                relative = "nvvfx/libs/" + ("LICENSES/" if entry in notices else "") + name
                data = source.read(entry)
                dest.writestr("MPCVR-Maxine-Runtime/" + relative, data)
                files.append({"Path": relative, "Size": len(data),
                              "SHA256": hashlib.sha256(data).hexdigest()})
            manifest = {"Format": 2, "RuntimeVersion": "1.3.0.0",
                        "Profile": "FullNativeLibraries", "Source": SOURCE,
                        "SourceSHA256": SHA256, "RequiredFiles": sorted(REQUIRED),
                        "PackagedBytes": sum(f["Size"] for f in files), "Files": files}
            dest.writestr("MPCVR-Maxine-Runtime/runtime-manifest.json",
                          json.dumps(manifest, indent=2) + "\n")
    checksum = digest_file(archive)
    (output / (archive.name + ".sha256")).write_text(
        checksum + "  " + archive.name + "\n", encoding="ascii")
    print(f"Packaged Maxine 1.3: {len(libraries)} native libraries and {len(notices)} notices")
    print(checksum + "  " + archive.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    wheel = args.wheel or args.output / WHEEL
    if not wheel.exists():
        wheel.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(SOURCE, wheel)
    package(wheel, args.output)
