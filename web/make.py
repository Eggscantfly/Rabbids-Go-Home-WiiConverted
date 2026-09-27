"""make.py - builds and deploys WiiConverted's pages on the site: https://www.joykhloe.com/RGH/wc/home

    python web/make.py build            the pages into web/out/site, the list of downloads into web/out/downloads.json
    python web/make.py preview          build, then serve it all at http://localhost:8787/RGH/wc/home (wrangler dev,
                                        with a local copy of the R2 bucket); Ctrl+C stops it
    python web/make.py deploy           build, upload the downloads that changed to R2, deploy the Worker and the pages
    python web/make.py pack FOLDER [--list-only]
                                        put a mod on the Mods page: its mod.json, icon and screenshots are copied into
                                        web/mods/<its folder's name>/ and the folder is zipped there as the download
                                        (other games' ROMs and data are left out, and so is what that folder's
                                        leave-out.txt names; its release/ files replace the mod's own); --list-only
                                        shows the mod without a download

The pages are built from web/src (the layout, the pages, the stylesheet, _headers), the launcher's art in Assets/,
dist/WiiConverted Setup.zip (--setup-zip names another) and web/mods/. The wrangler it runs is web/node_modules'
(installed with npm the first time it is needed); deploying needs a Cloudflare login once: `npx wrangler login` in
web/.
"""
import argparse
import hashlib
import html
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
import urllib.parse
import urllib.request
import zipfile
from fnmatch import fnmatch
from datetime import datetime
from pathlib import Path

from PIL import Image

WEB = Path(__file__).resolve().parent
REPO = WEB.parent
SRC = WEB / "src"
MODS = WEB / "mods"
OUT = WEB / "out"
SITE = OUT / "site"                          # the Worker's static assets (wrangler.toml, [assets])
ART = REPO / "Assets"

ORIGIN = "https://www.joykhloe.com"          # the address the pages give for themselves (canonical, og:image)
BASE = "/RGH/wc"                             # the pages' folder on the site
PAGES = SITE / BASE.strip("/")
HOME_URL = BASE + "/home"
MODS_URL = BASE + "/mods"
SETUP_ZIP = REPO / "dist" / "WiiConverted Setup.zip"
SETUP_KEY = "wc/download/wiiconverted-setup.zip"     # its R2 key; the address is /RGH/ + the key
SETUP_NAME = "WiiConverted Setup.zip"                # the name the browser saves it under
DEPLOYED = WEB / ".deployed.json"            # what the uploads put into R2: key -> SHA-256, remote and local
# the repository; its links show on the pages once it answers
GITHUB = "https://github.com/Eggscantfly/Rabbids-Go-Home-WiiConverted"

PICTURES = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp"}
# never in a download: other games' ROMs and data files, disc images, the game's own archives
BLOCKED_EXT = {".z64", ".n64", ".v64", ".iso", ".wbfs", ".rvz", ".gcm", ".gcz", ".ciso", ".wia", ".nkit", ".wad",
               ".nds", ".3ds", ".cia", ".xci", ".nsp", ".gba", ".gb", ".gbc", ".nes", ".sfc", ".smc", ".bf"}
BLOCKED_NAMES = {"data.win", "game.ios", "game.unx", "game.droid"}
LEFT_OUT = {"thumbs.db", "desktop.ini", ".ds_store", "icon.txt"}
TEXT = {".lua", ".ini", ".cfg", ".json", ".txt", ".toml", ".xml", ".md"}


def fail(message):
    sys.exit(f"make.py: {message}")


def esc(text):
    return html.escape(text, quote=True)


def fill(template, values):
    """{{name}} -> values[name]; a name without a value is an error, not an empty string"""
    def one(match):
        if match.group(1) not in values:
            fail(f"no value for {{{{{match.group(1)}}}}}")
        return str(values[match.group(1)])
    return re.sub(r"\{\{(\w+)\}\}", one, template)


def source(name):
    return (SRC / name).read_text(encoding="utf-8")


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def size_text(n):
    if n >= 1 << 20:
        return f"{n / (1 << 20):.1f} MB"
    if n >= 1 << 10:
        return f"{max(1, round(n / (1 << 10)))} KB"
    return f"{n} bytes"


def slug(name):
    """a mod folder's name in the address: "It's a me!" -> its-a-me"""
    return re.sub(r"[^a-z0-9]+", "-", name.lower().replace("'", "").replace("’", "")).strip("-") or "mod"


def blocked(rel):
    name = rel.rsplit("/", 1)[-1].lower()
    return name in BLOCKED_NAMES or os.path.splitext(name)[1] in BLOCKED_EXT


def reachable(url):
    try:
        request = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "wiiconverted-make"})
        with urllib.request.urlopen(request, timeout=8) as response:
            return response.status == 200
    except (OSError, ValueError):
        return False


def disposition(name):
    if name.isascii():
        return f'attachment; filename="{name}"'
    fallback = name.encode("ascii", "replace").decode().replace("?", "_")
    return f"attachment; filename=\"{fallback}\"; filename*=UTF-8''{urllib.parse.quote(name)}"


# ------------------------------------------------------------------------------------------------------ pictures
def trimmed(img):
    """the picture without its transparent margins, as the launcher's Icon() cuts it"""
    img = img.convert("RGBA")
    box = img.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    return img.crop(box) if box else img


def fitted(img, w, h):
    """scaled to fit w x h: down smoothly; a small picture (pixel art) up by whole steps, so it stays sharp"""
    if img.width > w or img.height > h:
        img = img.copy()
        img.thumbnail((w, h), Image.LANCZOS)
        return img
    k = min(w // img.width, h // img.height)
    if k >= 2 and max(img.size) <= 64:
        return img.resize((img.width * k, img.height * k), Image.NEAREST)
    return img


def box_size(img, w, h):
    """the size the picture is shown at when it fits a w x h box (the width and height attributes)"""
    k = min(w / img.width, h / img.height)
    return max(1, round(img.width * k)), max(1, round(img.height * k))


def encoded(img, kind, quality=90):
    buf = io.BytesIO()
    if kind == "webp":
        img.save(buf, "WEBP", quality=quality, method=6)
    elif kind == "jpg":
        img.convert("RGB").save(buf, "JPEG", quality=quality, optimize=True, progressive=True)
    else:
        img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


class Assets:
    """web/out/site/RGH/wc/assets: every file named after its content's hash, so browsers keep it for good (_headers)"""

    def __init__(self):
        self.dir = PAGES / "assets"
        self.dir.mkdir(parents=True, exist_ok=True)

    def add(self, name, data):
        stem, ext = name.rsplit(".", 1)
        name = f"{stem}.{hashlib.sha256(data).hexdigest()[:10]}.{ext}".lower()
        (self.dir / name).write_bytes(data)
        return f"{BASE}/assets/{name}"

    def picture(self, name, img, kind="webp", quality=90):
        return self.add(f"{name}.{kind}", encoded(img, kind, quality))


def art(assets):
    """the launcher's art (launcher/launcher.qrc), sized for the pages at up to 2x or 3x the shown size"""
    v = {}
    logo = fitted(Image.open(ART / "title" / "RGH Logo.png").convert("RGBA"), 800, 800)      # shown 400 wide
    v["logo"], v["logo_h"] = assets.picture("logo", logo), box_size(logo, 400, 10**4)[1]
    mark = fitted(Image.open(ART / "title" / "WiiConverted.png").convert("RGBA"), 600, 600)   # shown 300 wide
    v["wordmark"], v["wordmark_h"] = assets.picture("wordmark", mark), box_size(mark, 300, 10**4)[1]
    wc = fitted(trimmed(Image.open(ART / "GUI" / "WC icon.png")), 144, 144)
    v["wc_icon"] = assets.picture("wc-icon", wc)
    v["wc_icon_h"], v["wc_icon_h48"] = box_size(wc, 34, 10**4)[1], box_size(wc, 48, 10**4)[1]
    v["moon"] = assets.picture("moon", fitted(trimmed(Image.open(ART / "GUI" / "rgh moon upscaled.png")), 102, 102))
    rabbid = fitted(trimmed(Image.open(ART / "GUI" / "Rabbid mod.png")), 102, 102)
    v["mods_icon"], v["mods_icon44"] = assets.picture("mods", rabbid), box_size(rabbid, 44, 44)
    v["favicon"] = assets.add("favicon.ico", (ART / "GUI" / "WC icon.ico").read_bytes())

    touch = Image.new("RGBA", (180, 180), (0x25, 0x25, 0x25, 255))
    small = fitted(wc, 136, 136)
    touch.alpha_composite(small, ((180 - small.width) // 2, (180 - small.height) // 2))
    v["touch_icon"] = assets.picture("touch-icon", touch, "png")

    # the picture a shared link shows (Discord and the like): the banner
    og = Image.new("RGBA", (1200, 630))
    for y in range(630):
        t = y / 629
        c = round(0x30 + (0x26 - 0x30) * t)
        og.paste((c, c, c, 255), (0, y, 1200, y + 1))
    big = fitted(Image.open(ART / "title" / "RGH Logo.png").convert("RGBA"), 700, 350)
    word = fitted(Image.open(ART / "title" / "WiiConverted.png").convert("RGBA"), 520, 520)
    top = (630 - (big.height + 24 + word.height)) // 2
    og.alpha_composite(big, ((1200 - big.width) // 2, top))
    og.alpha_composite(word, ((1200 - word.width) // 2, top + big.height + 24))
    v["og_image"] = ORIGIN + assets.picture("og", og, "jpg", 88)
    return v


# --------------------------------------------------------------------------------------------------------- inputs
def setup_download(path):
    if not path.is_file():
        fail(f"{path} is missing: build the setup (build_app.bat) and zip it, or name the zip with --setup-zip")
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if not any(n.endswith("/WiiConverted Setup.exe") or n == "WiiConverted Setup.exe" for n in names):
            fail(f"{path} holds no WiiConverted Setup.exe")
        # a game folder converted without the script layers never starts
        if not any(n.endswith("script_overrides/layers.txt") for n in names):
            fail(f"{path} holds no script_overrides/layers.txt: a game this setup converts would never start")
        bad = [n for n in names if blocked(n)]
        if bad:
            fail(f"{path} holds files that are never published: {', '.join(bad[:5])}")
    stat = path.stat()
    return {"key": SETUP_KEY, "file": str(path), "name": SETUP_NAME, "size": stat.st_size,
            "sha256": file_sha256(path), "date": datetime.fromtimestamp(stat.st_mtime)}


def mod_pictures(folder, listed):
    """the screenshots: the ones mod.json lists, else the pictures of the screenshots folder (as the launcher does)"""
    if listed:
        return [folder / s for s in listed if (folder / s).is_file()]
    shots = folder / "screenshots"
    if not shots.is_dir():
        return []
    return sorted((p for p in shots.iterdir() if p.suffix.lower() in PICTURES), key=lambda p: p.name.lower())


def load_mods(assets):
    mods = []
    for folder in sorted(p for p in MODS.iterdir() if p.is_dir()) if MODS.is_dir() else []:
        meta_file = folder / "mod.json"
        if not meta_file.is_file():
            print(f"  {folder.name}: no mod.json, left off the Mods page")
            continue
        try:
            meta = json.loads(meta_file.read_text(encoding="utf-8-sig"))
        except json.JSONDecodeError as e:
            fail(f"{meta_file}: {e}")
        m = {"folder": folder.name, "slug": slug(folder.name), "name": meta.get("name") or folder.name,
             "author": meta.get("author") or "", "version": meta.get("version") or "",
             "description": meta.get("description") or ""}

        icon = folder / (meta.get("icon") or "icon.png")
        if icon.is_file():
            img = fitted(trimmed(Image.open(icon)), 144, 144)
            m["icon"] = assets.picture(f"mod-{m['slug']}", img)
            m["icon44"], m["icon48"] = box_size(img, 44, 44), box_size(img, 48, 48)

        m["shots"] = []
        for i, path in enumerate(mod_pictures(folder, meta.get("screenshots") or [])):
            img = Image.open(path)
            img = img.convert("RGBA" if "A" in img.getbands() else "RGB")
            thumb = fitted(img, 10**4, 600)                                    # shown 300 high
            full = fitted(img, 2560, 1440)                                     # what a click on it opens
            m["shots"].append({"thumb": assets.picture(f"shot-{m['slug']}-{i}", thumb, quality=85),
                               "full": assets.picture(f"shot-{m['slug']}-{i}-full", full, quality=90),
                               "w": box_size(thumb, 10**4, 300)[0], "name": path.name})

        zips = sorted(folder.glob("*.zip"))
        m["download"] = None
        if len(zips) > 1:
            fail(f"{folder} has {len(zips)} zips; the download is one")
        if zips:
            with zipfile.ZipFile(zips[0]) as z:
                bad = [n for n in z.namelist() if blocked(n)]
                if bad:
                    fail(f"{zips[0]} holds files that are never published: {', '.join(bad[:5])}")
                if z.testzip() is not None:
                    fail(f"{zips[0]} is damaged")
            m["download"] = {"key": f"wc/mods/{m['slug']}.zip", "file": str(zips[0]), "name": f"{folder.name}.zip",
                             "size": zips[0].stat().st_size, "sha256": file_sha256(zips[0])}
        mods.append(m)
    # the ones to download first, then by name, as the launcher sorts
    mods.sort(key=lambda m: (m["download"] is None, m["name"].lower()))
    return mods


# ---------------------------------------------------------------------------------------------------------- pages
DOWNLOAD_ICON = ('<svg class="btn-icon" viewBox="0 0 16 16" aria-hidden="true"><path fill="currentColor" '
                 'd="M7 1h2v7.59l2.3-2.3 1.4 1.42L8 12.41 3.3 7.71l1.4-1.42L7 8.59V1zM2 13.5h12V15H2z"/></svg>')


def render(v, *, name, title, description, page_title, content, active, head_extra="", page_class=""):
    nav = []
    for key, label, icon, url in (("home", "HOME", v["moon"], HOME_URL), ("mods", "MODS", v["mods_icon"], MODS_URL)):
        current = ' aria-current="page"' if key == active else ""
        nav.append(f'      <a class="nav-item" href="{url}"{current}><img class="nav-icon" src="{icon}" alt="" '
                   f'width="34" height="34"><span class="nav-label">{label}</span></a>')
    page = fill(source("layout.html"), dict(
        v, title=esc(title), description=esc(description), head_extra=head_extra,
        canonical=f"{ORIGIN}{BASE}/{name}", home_url=HOME_URL, nav="\n".join(nav), page_title=page_title,
        page_class=page_class, content=content.rstrip("\n")))
    out = PAGES / f"{name}.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page, encoding="utf-8", newline="\n")


def badge(m):
    if m["download"]:
        return '<span class="badge">Available</span>'
    return '<span class="badge badge-soon">Coming soon</span>'


def icon_tag(m, box):
    if not m.get("icon"):
        return ""
    w, h = m[f"icon{box}"]
    return f'<img src="{m["icon"]}" alt="" width="{w}" height="{h}">'


def mod_line(m):
    line = f"by {m['author']}" if m["author"] else ""
    if m["version"]:
        line += ("  -  " if line else "") + f"Version {m['version']}"
    return line


def first_sentence(text, limit=160):
    text = " ".join(text.split())
    cut = re.search(r"^(.{20,}?[.!?])(\s|$)", text)
    text = cut.group(1) if cut else text
    return text if len(text) <= limit else text[:limit - 1].rsplit(" ", 1)[0] + "…"


def mods_pages(v, mods):
    w, h = v["mods_icon44"]
    rabbid = f'<img src="{v["mods_icon"]}" alt="" width="{w}" height="{h}">'      # a mod without an icon
    cards = []
    for m in mods:
        pic = icon_tag(m, 44) or rabbid
        cards.append(f'        <a class="card" href="{MODS_URL}/{m["slug"]}">\n'
                     f'          <span class="card-pic">{pic}</span>\n'
                     f'          <span class="card-text"><span class="card-title">{esc(m["name"])}</span>'
                     f'<span class="card-line">{esc(mod_line(m))}</span></span>\n'
                     f'          {badge(m)}\n'
                     f'        </a>')
    if not mods:
        cards.append(f'        <div class="card"><span class="card-pic">{rabbid}</span><span class="card-text">'
                     f'<span class="card-title">No mods yet</span><span class="card-line">They are on their way.'
                     f'</span></span></div>')
    ready = sum(1 for m in mods if m["download"])
    count = f"{len(mods)} mod{'s' if len(mods) != 1 else ''}, {ready} to download"
    render(v, name="mods", title="Mods - WiiConverted",
           description="Mods for the WiiConverted launcher of Rabbids Go Home: download one, unzip it into the "
                       "game folder's mods folder and turn it on in the launcher.",
           page_title="MODS", active="mods", content=fill(source("mods.html"), {
               "count": count, "cards": "\n".join(cards), "guide": v["guide"]}))

    for m in mods:
        shots = ""
        if m["shots"]:
            shots = "\n".join(['      <div class="shots">'] + [
                f'        <a class="shot" href="{s["full"]}" title="{esc(s["name"])}"><img src="{s["thumb"]}" '
                f'alt="Screenshot of {esc(m["name"])}" width="{s["w"]}" height="300" loading="lazy"></a>'
                for s in m["shots"]] + ['      </div>'])
        if m["download"]:
            action = (f'        <span class="muted">{size_text(m["download"]["size"])} zip</span>\n'
                      f'        <a class="btn btn-enable" href="/RGH/{m["download"]["key"]}">{DOWNLOAD_ICON}'
                      f'Download</a>')
        else:
            action = '        <span class="btn btn-enable" aria-disabled="true">Coming soon</span>'
        content = fill(source("mod.html"), {
            "mods_url": MODS_URL, "icon": icon_tag(m, 48) or rabbid, "name": esc(m["name"]), "badge": badge(m),
            "shots": shots,
            "author": esc(m["author"] or "unknown"),
            "version": f'      <p class="mod-meta">Version: {esc(m["version"])}</p>' if m["version"] else "",
            "description": esc(m["description"] or "No description."), "action": action})
        render(v, name=f"mods/{m['slug']}", title=f"{m['name']} - WiiConverted mods",
               description=first_sentence(m["description"]) or f"{m['name']}, a mod for WiiConverted",
               page_title="MODS", active="mods", content=content, page_class=" page-mod")


def emptied(folder):
    """removes the folder; on Windows a file just written can stay locked for a moment (a virus scan, the indexer)"""
    for attempt in range(10):
        try:
            if folder.exists():
                shutil.rmtree(folder)
            return
        except PermissionError:
            if attempt == 9:
                raise
            time.sleep(0.5)


def build(setup_zip):
    print("building web/out/site")
    emptied(SITE)
    PAGES.mkdir(parents=True)
    shutil.copyfile(SRC / "_headers", SITE / "_headers")
    assets = Assets()
    v = art(assets)
    v["css"] = assets.add("wc.css", source("wc.css").encode("utf-8"))
    setup = setup_download(setup_zip)
    v["download_url"] = f"/RGH/{setup['key']}"

    # the repository's links, once it is public: the source and the issues by the download, the modding guide
    # (docs/mods.md) on the Mods page
    v["github_links"] = v["guide"] = ""
    if reachable(GITHUB):
        v["github_links"] = (f'          <p class="get-links"><a class="link" href="{GITHUB}">Source code on GitHub</a>'
                             f'<a class="link" href="{GITHUB}/issues">Report a problem</a></p>')
        if reachable(GITHUB + "/blob/HEAD/docs/mods.md"):
            v["guide"] = (f'        <p class="body">Want to make one? The <a class="link" href="{GITHUB}/blob/HEAD/'
                          f'docs/mods.md">modding guide</a> is on GitHub.</p>')
    print(f"  GitHub links: {'on' if v['github_links'] else 'off (the repository is not public yet)'}"
          f"{', with the modding guide' if v['guide'] else ''}")

    d = setup["date"]
    render(v, name="home", title="WiiConverted - Rabbids Go Home for PC",
           description="WiiConverted builds a Windows version of the Wii game Rabbids Go Home from files you own, "
                       "with a launcher for Play, mods and settings.",
           page_title="HOME", active="home", content=fill(source("home.html"), dict(
               v, mods_url=MODS_URL, download_size=size_text(setup["size"]),
               download_date=f"{d:%B} {d.day}, {d.year}")))
    mods = load_mods(assets)
    mods_pages(v, mods)
    render(v, name="404", title="Not found - WiiConverted", description="There is no page at this address.",
           head_extra='\n<meta name="robots" content="noindex">', page_title="NOT FOUND", active="",
           content=fill(source("404.html"), {"home_url": HOME_URL, "mods_url": MODS_URL}))

    downloads = [{k: setup[k] for k in ("key", "file", "name", "size", "sha256")}]
    downloads += [m["download"] for m in mods if m["download"]]
    (OUT / "downloads.json").write_text(json.dumps(downloads, indent=2), encoding="utf-8")
    files = sum(1 for p in SITE.rglob("*") if p.is_file())
    print(f"  {files} files, {len(mods)} mods ({len(downloads) - 1} to download), setup {size_text(setup['size'])}")
    return downloads


# ------------------------------------------------------------------------------------------------------- wrangler
def wrangler(*args, capture=False):
    script = WEB / "node_modules" / "wrangler" / "bin" / "wrangler.js"
    if not script.is_file():
        npm = shutil.which("npm") or fail("npm is not on the PATH: install Node.js")
        print("installing wrangler into web/node_modules")
        subprocess.run([npm, "install", "--no-audit", "--no-fund"], cwd=WEB, check=True)
    node = shutil.which("node") or fail("node is not on the PATH: install Node.js")
    env = dict(os.environ, WRANGLER_SEND_METRICS="false")
    done = subprocess.run([node, str(script), *args], cwd=WEB, env=env, capture_output=capture, text=True,
                          encoding="utf-8", errors="replace")
    if done.returncode != 0:
        if capture:
            sys.stderr.write(done.stdout + done.stderr)
        fail(f"wrangler {' '.join(args[:3])} failed ({done.returncode})")
    return done.stdout if capture else ""


def bucket_name():
    with open(WEB / "wrangler.toml", "rb") as f:
        return tomllib.load(f)["r2_buckets"][0]["bucket_name"]


def upload(downloads, where, force=False):
    """puts the downloads whose bytes changed since the last upload into the bucket and takes out the ones that are
    gone; where: "remote" (Cloudflare) or "local" (wrangler dev's copy)"""
    bucket = bucket_name()
    target = ["--remote"] if where == "remote" else ["--local", "--persist-to", str(WEB / ".wrangler" / "state")]
    state = json.loads(DEPLOYED.read_text(encoding="utf-8")) if DEPLOYED.is_file() else {}
    done = state.setdefault(f"{where}:{bucket}", {})

    def save():
        DEPLOYED.write_text(json.dumps(state, indent=2), encoding="utf-8")

    for d in downloads:
        if done.get(d["key"]) == d["sha256"] and not force:
            print(f"  {d['key']}: unchanged")
            continue
        print(f"  {d['key']}: uploading {size_text(d['size'])}")
        wrangler("r2", "object", "put", f"{bucket}/{d['key']}", "--file", d["file"], "--content-type",
                 "application/zip", "--content-disposition", disposition(d["name"]), *target, capture=True)
        done[d["key"]] = d["sha256"]
        save()
    keys = {d["key"] for d in downloads}
    for key in [k for k in done if k not in keys]:
        print(f"  {key}: no longer published, deleting")
        wrangler("r2", "object", "delete", f"{bucket}/{key}", *target, capture=True)
        del done[key]
        save()


def ensure_bucket():
    bucket = bucket_name()
    listing = wrangler("r2", "bucket", "list", capture=True)          # "name:  <bucket>" lines
    if not re.search(rf"^name:\s+{re.escape(bucket)}\s*$", listing, re.M):
        print(f"creating the R2 bucket {bucket}")
        wrangler("r2", "bucket", "create", bucket, capture=True)


# ------------------------------------------------------------------------------------------------------------ pack
def pack(folder, list_only):
    """web/mods/<mod>/ keeps what the page shows and the zip, and two things of the site's own that pack follows:
    leave-out.txt, patterns (one a line, * reaches into folders, # a comment) of the mod's files that never go into
    the download - another game's assets the mod reads from the player's own copy - and release/, files that take
    the place of the mod's own in the download (a config without this computer's paths in it)"""
    src = Path(folder).resolve()
    meta_file = src / "mod.json"
    if not meta_file.is_file():
        fail(f"{src} has no mod.json: a mod is a folder with one")
    meta = json.loads(meta_file.read_text(encoding="utf-8-sig"))
    dst = MODS / src.name
    dst.mkdir(parents=True, exist_ok=True)

    rules = dst / "leave-out.txt"
    patterns = []
    for line in rules.read_text(encoding="utf-8-sig").splitlines() if rules.is_file() else []:
        line = line.split("#", 1)[0].strip().replace("\\", "/")
        if line:
            patterns.append(line.lower() + ("*" if line.endswith("/") else ""))
    release_dir = dst / "release"
    release = {p.relative_to(release_dir).as_posix(): p for p in sorted(release_dir.rglob("*")) if p.is_file()} \
        if release_dir.is_dir() else {}

    def kept(rel):
        return not any(fnmatch(rel.lower(), p) for p in patterns)

    # what the Mods page shows
    listing = ["mod.json", meta.get("icon") or "icon.png"]
    listing += [p.relative_to(src).as_posix() for p in mod_pictures(src, meta.get("screenshots") or [])]
    for rel in listing:
        origin = release.get(rel) or src / rel
        if kept(rel) and origin.is_file():
            (dst / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(origin, dst / rel)
    for old in list(dst.glob("*.zip")) + [dst / "icon.txt"]:
        old.unlink(missing_ok=True)
    if list_only:
        print(f"{src.name}: on the Mods page without a download (web/mods/{src.name})")
        return

    target = dst / f"{src.name}.zip"
    blocked_files, dropped, paths = [], [], []
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(src.rglob("*")):
            rel = path.relative_to(src).as_posix()
            if path.is_dir() or any(part.startswith(".") or part == "__pycache__" for part in rel.split("/")):
                continue
            if path.name.lower() in LEFT_OUT:
                continue
            if blocked(rel):
                blocked_files.append(rel)
            elif not kept(rel):
                dropped.append(rel)
            elif rel in release:
                z.write(release[rel], f"{src.name}/{rel}")
            else:
                if path.suffix.lower() in TEXT and path.stat().st_size < 1 << 20:
                    if re.search(r"(?<![A-Za-z])[A-Za-z]:[\\/]", path.read_text(encoding="utf-8", errors="replace")):
                        paths.append(rel)
                z.write(path, f"{src.name}/{rel}")
        for rel, path in release.items():                          # what release/ adds of its own
            if not (src / rel).is_file():
                z.write(path, f"{src.name}/{rel}")
    print(f"{src.name}: web/mods/{src.name}/{target.name}, {size_text(target.stat().st_size)}")
    for rel in blocked_files:
        print(f"  left out {rel} (never published: players bring their own)")
    if dropped:
        folders = sorted({rel.rsplit("/", 1)[0] + "/" if "/" in rel else rel for rel in dropped})
        print(f"  left out {len(dropped)} file(s) by leave-out.txt: {', '.join(folders[:6])}")
    for rel in release:
        print(f"  {rel} from release/")
    for rel in paths:
        print(f"  note: {rel} names a folder on this computer (a drive letter); players get it as it is")


# ------------------------------------------------------------------------------------------------------------ main
def main():
    sys.stdout.reconfigure(line_buffering=True)          # in order with wrangler's own output
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("build", "preview", "deploy"):
        p = sub.add_parser(name)
        p.add_argument("--setup-zip", type=Path, default=SETUP_ZIP, help="the setup's zip (default: %(default)s)")
        if name == "preview":
            p.add_argument("--port", type=int, default=8787)
        if name != "build":
            p.add_argument("--force", action="store_true", help="upload every download, changed or not")
    p = sub.add_parser("pack")
    p.add_argument("folder", help="the mod's folder (the one with its mod.json)")
    p.add_argument("--list-only", action="store_true", help="show the mod on the Mods page without a download")
    args = parser.parse_args()

    if args.command == "pack":
        pack(args.folder, args.list_only)
        return
    downloads = build(args.setup_zip.resolve())
    if args.command == "preview":
        print("copying the downloads into the local bucket")
        upload(downloads, "local", args.force)
        print(f"serving http://localhost:{args.port}{HOME_URL}  (Ctrl+C stops it)")
        wrangler("dev", "--persist-to", str(WEB / ".wrangler" / "state"), "--port", str(args.port), "--ip",
                 "127.0.0.1")
    elif args.command == "deploy":
        ensure_bucket()
        print("uploading the downloads")
        upload(downloads, "remote", args.force)
        print("deploying the Worker and the pages")
        wrangler("deploy")
        print(f"live: {ORIGIN}{HOME_URL}")


if __name__ == "__main__":
    main()
