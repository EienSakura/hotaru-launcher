"""从已发布资产的本地核对副本生成目录，并用离线 Ed25519 密钥签名。"""
import argparse
import datetime as dt
import hashlib
import json
import re
from tempfile import TemporaryDirectory
from pathlib import Path
from zipfile import ZipFile

from cryptography.hazmat.primitives.serialization import load_pem_private_key
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

IID = "app.hotaru.LanPackage/1.0"
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,119}\Z")
MIB = 1024 * 1024
CATALOG_VALIDITY_DAYS = 365

# 维护者批准 1.0.1 的双机验收豁免，仅限下列已构建 ZIP。
LAN_1_0_1_WAIVER = {
    "lan-core": "d4cb600fb4593b58996c99d3b673ea6fa395b5ed7590bfaf569742c642eca030",
    "lan-clipboard": "3099646d8064261124a4a6bb620a1dccefd33321320b3d118c6c9a58b3c52457",
    "lan-files": "79c022ff50e51900f21c3a8088d619cd48178fbc39f28e0bb6ea86c9c08cb9cd",
}


def create(config, assets):
    entries = []
    seen = set()
    for item in config:
        meta = json.loads((assets / item["metadata"]).read_text(encoding="utf-8"))
        key = meta["Id"], meta["Version"]
        if key in seen:
            raise ValueError(f"版本重复：{key}")
        seen.add(key)
        name = f"hotaru-plugin-{key[0]}-{key[1]}-x64.zip"
        path = assets / name
        if not 0 < path.stat().st_size <= 128 * MIB:
            raise ValueError(f"插件包大小超限：{name}")
        dependencies = meta.get("Dependencies", {})
        if meta.get("Requires"):
            dependencies.setdefault(meta["Requires"], {"min": "", "max": ""})
        files = []
        with ZipFile(path) as package:
            members = package.infolist()
            names = [file.filename.lower() for file in members]
            if not 0 < len(members) <= 32 or len(set(names)) != len(names) or sum(n.endswith(".dll") for n in names) != 1:
                raise ValueError(f"插件包结构无效：{name}")
            if sum(file.file_size for file in members) > 256 * MIB:
                raise ValueError(f"解压大小超限：{name}")
            for file in sorted(members, key=lambda f: f.filename):
                if (not NAME.fullmatch(file.filename) or file.flag_bits & 1 or (file.external_attr >> 16) & 0xF000 == 0xA000
                        or not file.filename.lower().endswith((".dll", ".txt")) or file.file_size <= 0):
                    raise ValueError(f"非法包内条目：{file.filename}")
                with package.open(file) as stream:
                    files.append({"name": file.filename, "size": file.file_size, "sha256": hashlib.file_digest(stream, "sha256").hexdigest()})
        with path.open("rb") as stream:
            asset = {"name": name, "size": path.stat().st_size, "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
        channel = {"development-preview": "preview"}.get(meta["Channel"], meta["Channel"])
        if channel not in ("preview", "stable"):
            raise ValueError(f"渠道无效：{channel}")
        if channel == "stable" and key[0] in ("lan-core", "lan-clipboard", "lan-files"):
            record = item.get("windows_dual_machine")
            waived = key[1] == "1.0.1" and LAN_1_0_1_WAIVER.get(key[0]) == asset["sha256"]
            if not waived and (not record or record["sha256"] != asset["sha256"]
                               or record["version"] != key[1] or not record["report"]):
                raise ValueError(f"正式 LAN 需要对应版本和哈希的双机验收记录：{key}")
        tag = item["tag"]
        if not NAME.fullmatch(tag) or tag.startswith("catalog-v1-"):
            raise ValueError(f"版本发布标签无效：{tag}")
        entries.append({"id": key[0], "version": key[1], "name": meta["Name"], "description": meta["Desc"],
                        "category": meta["Category"], "channel": channel, "iid": item.get("iid", IID), "api": meta["Api"],
                        "abi": meta["Abi"], "platform": "windows", "arch": "x64", "host": item["host"],
                        "dependencies": dependencies, "tag": tag, "asset": asset, "files": files})
    return sorted(entries, key=lambda entry: (entry["id"], entry["version"]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--days", type=int, default=CATALOG_VALIDITY_DAYS)
    parser.add_argument("--key", required=True, action="append", type=Path)
    args = parser.parse_args()
    if not 0 < args.sequence <= 2147483647 or not 0 < args.days <= CATALOG_VALIDITY_DAYS or len(args.key) > 4:
        parser.error("序号、有效期或签名数超限")
    config = json.loads(args.config.read_text(encoding="utf-8"))
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    data = {"format": 1, "sequence": args.sequence, "issued": now.isoformat(),
            "expires": (now + dt.timedelta(days=args.days)).isoformat(), "plugins": create(config, args.assets)}
    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode()
    if len(payload) > MIB:
        parser.error("目录超限")
    if args.out.exists():
        parser.error("目标目录已存在")
    signatures = bytearray()
    for path in args.key:
        key = load_pem_private_key(path.read_bytes(), password=None)
        if not isinstance(key, Ed25519PrivateKey):
            raise ValueError("仅允许 Ed25519 签名")
        signatures.extend(key.sign(payload))
    with TemporaryDirectory(prefix=".hotaru-catalog-", dir=args.out.parent) as temporary:
        ready = Path(temporary) / "catalog"
        ready.mkdir()
        (ready / "index-v1.json").write_bytes(payload)
        (ready / "index-v1.sig").write_bytes(signatures)
        ready.rename(args.out)


if __name__ == "__main__":
    main()
