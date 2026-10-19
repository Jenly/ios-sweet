#!/usr/bin/env python3
"""Find, download and unpack Apple IPSWs for the ios-sweet userland.

The port runs the *unmodified* iOS userland of an iPhone whose A-series core
has no pointer authentication, because the Qualcomm cores the kernel runs on
are plain ARMv8.2.  That is the iPhone X (iPhone10,3 / iPhone10,6, board
D22AP / D221AP, A11) on iOS 16.7.16 (20H392), the last release for it -- see
NOTES.md.  This tool wraps ipsw.me so the steps in README.md are one command
each and nothing has to be typed from memory:

    tools/ipsw.py list                       # what ipsw.me has for the model
    tools/ipsw.py info                       # the release this port uses
    tools/ipsw.py fetch                      # download + verify (~6 GiB)
    tools/ipsw.py extract BuildManifest      # single member over HTTP ranges
    tools/ipsw.py extract DeviceTree.d22ap   # the /product source for mkadt

IPSWs are Apple's copyrighted firmware: they are never committed, and
`.gitignore` keeps `research/` out of git.

The API is https://api.ipsw.me/v4: /device/<identifier> lists every release
with its SHA-1/SHA-256 and size, /ipsw/<identifier>/<build> one release.
Pass --offline to use a cached api.json instead of the network.
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.request

API = "https://api.ipsw.me/v4"
HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "research", "ipsw-x")

# The userland source this port boots: iPhone X, iOS 16.7.16.
DEFAULT_PRODUCT = "iPhone10,3"
DEFAULT_BUILD = "20H392"


def curl_json(url):
    """ipsw.me is plain HTTPS JSON; curl because urllib can pick a broken
    IPv6 route on some hosts (same reason tools/remotezip.py shells out)."""
    try:
        out = subprocess.run(["curl", "-sfL", "--max-time", "60", url],
                             check=True, capture_output=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"error: cannot reach {url}: {exc}")
    return json.loads(out)


def device(product, offline=False):
    path = os.path.join(CACHE, f"{product}.json")
    if offline:
        if not os.path.exists(path):
            raise SystemExit(f"error: no cached {path} (drop --offline)")
        return json.load(open(path))
    data = curl_json(f"{API}/device/{product}")
    os.makedirs(CACHE, exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f)
    return data


def pick(dev, build):
    for fw in dev["firmwares"]:
        if fw["buildid"] == build:
            return fw
    have = ", ".join(fw["buildid"] for fw in dev["firmwares"][:10])
    raise SystemExit(f"error: {dev['identifier']} has no build {build} (newest: {have})")


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


def cmd_list(args):
    dev = device(args.product, args.offline)
    print(f"{dev['name']} ({dev['identifier']}, board {dev['boardconfig']}, "
          f"{dev['platform']})")
    print(f"{'version':>10}  {'build':>7}  {'size':>10}  signed  url")
    for fw in dev["firmwares"][:args.limit]:
        print(f"{fw['version']:>10}  {fw['buildid']:>7}  {human(fw['filesize']):>10}  "
              f"{'yes' if fw['signed'] else 'no ':>6}  {os.path.basename(fw['url'])}")


def cmd_info(args):
    dev = device(args.product, args.offline)
    fw = pick(dev, args.build)
    print(f"device    {dev['name']} ({dev['identifier']})")
    print(f"board     {dev['boardconfig']}  platform {dev['platform']}  "
          f"cpid {dev['cpid']}  bdid {dev['bdid']}")
    print(f"version   {fw['version']} ({fw['buildid']})")
    print(f"size      {human(fw['filesize'])} ({fw['filesize']} bytes)")
    print(f"sha1      {fw['sha1sum']}")
    print(f"sha256    {fw['sha256sum']}")
    print(f"signed    {'yes' if fw['signed'] else 'no'}")
    print(f"released  {fw['releasedate']}")
    print(f"url       {fw['url']}")
    if args.json:
        print(json.dumps(fw, indent=2))


def sha256_of(path, chunk=1 << 22):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            data = f.read(chunk)
            if not data:
                break
            h.update(data)
    return h.hexdigest()


def cmd_fetch(args):
    dev = device(args.product, args.offline)
    fw = pick(dev, args.build)
    outdir = args.dir or CACHE
    os.makedirs(outdir, exist_ok=True)
    name = os.path.basename(fw["url"])
    path = os.path.join(outdir, name)
    if os.path.exists(path) and os.path.getsize(path) == fw["filesize"]:
        print(f"{name}: already downloaded ({human(fw['filesize'])})")
    else:
        # curl -C - resumes: an IPSW is ~6 GiB and a dropped connection is normal
        print(f"downloading {name} ({human(fw['filesize'])}) -> {path}", flush=True)
        cmd = ["curl", "-fL", "-C", "-", "--retry", "5", "--retry-delay", "5",
               "-o", path, fw["url"]]
        if args.quiet:
            cmd.insert(1, "-s")
        subprocess.run(cmd, check=True)
        size = os.path.getsize(path)
        if size != fw["filesize"]:
            raise SystemExit(f"error: {name} is {size} bytes, ipsw.me says "
                             f"{fw['filesize']}")
    if not args.no_verify:
        want = fw["sha256sum"] if args.sha256 else fw["sha1sum"]
        print(f"verifying {'sha256' if args.sha256 else 'sha1'} ...", flush=True)
        got = sha256_of(path) if args.sha256 else _sha1_of(path)
        if got != want:
            raise SystemExit(f"error: checksum mismatch\n  expected {want}\n  got      {got}")
        print("checksum ok")
    with open(os.path.join(outdir, "URL.txt"), "w") as f:
        f.write(fw["url"] + "\n")
    print(path)


def _sha1_of(path, chunk=1 << 22):
    import hashlib
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            data = f.read(chunk)
            if not data:
                break
            h.update(data)
    return h.hexdigest()


def cmd_extract(args):
    """Single member (e.g. DeviceTree.d22ap.im4p) straight out of the remote
    zip with HTTP range requests: no 6 GiB download for a 100 KiB file."""
    dev = device(args.product, args.offline)
    fw = pick(dev, args.build)
    outdir = args.dir or CACHE
    os.makedirs(outdir, exist_ok=True)
    rz = os.path.join(HERE, "remotezip.py")
    if args.list:
        subprocess.run([sys.executable, rz, fw["url"]], check=True)
        return
    if not args.pattern:
        raise SystemExit("error: give a member pattern, or --list")
    subprocess.run([sys.executable, rz, fw["url"], args.pattern, outdir], check=True)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--product", default=DEFAULT_PRODUCT,
                    help=f"device identifier (default {DEFAULT_PRODUCT} = iPhone X)")
    ap.add_argument("--build", default=DEFAULT_BUILD,
                    help=f"build id (default {DEFAULT_BUILD} = iOS 16.7.16)")
    ap.add_argument("--offline", action="store_true", help="use the cached api.json")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="list the releases ipsw.me knows")
    p.add_argument("--limit", type=int, default=15)
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("info", help="show the selected release")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("fetch", help="download and verify the IPSW")
    p.add_argument("--dir", help="download directory (default research/ipsw-x)")
    p.add_argument("--sha256", action="store_true", help="verify SHA-256 instead of SHA-1")
    p.add_argument("--no-verify", action="store_true")
    p.add_argument("--quiet", action="store_true")
    p.set_defaults(func=cmd_fetch)

    p = sub.add_parser("extract", help="extract members over HTTP range requests")
    p.add_argument("pattern", nargs="?", help="substring of the member name")
    p.add_argument("--dir", help="output directory (default research/ipsw-x)")
    p.add_argument("--list", action="store_true", help="list the members instead")
    p.set_defaults(func=cmd_extract)

    args = ap.parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
