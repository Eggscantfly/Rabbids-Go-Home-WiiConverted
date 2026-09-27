# web - WiiConverted's pages on the site

The download page and the mods pages: **https://www.joykhloe.com/RGH/wc/home** (also without `www`, and under the
lower-case `/rgh`, which redirects). They wear the launcher's look - its colours, sidebar, top bar, banner, panels and
cards (`launcher/ui.cpp`) - and its art from `Assets/`.

    python web/make.py build      builds the pages into web/out/site
    python web/make.py preview    the same, then serves them at http://localhost:8787/RGH/wc/home (Ctrl+C stops it)
    python web/make.py deploy     the same, then puts them on the site

## Pages

| Address | Page | Template |
|---|---|---|
| `/RGH/wc/home` | the download: the setup's zip, what it needs, the credits, the setup's steps | `src/home.html` |
| `/RGH/wc/mods` | the mods: a card for every folder of `web/mods` | `src/mods.html` |
| `/RGH/wc/mods/<mod>` | one mod: its screenshots, author, version, description and download | `src/mod.html` |
| anything else under `/RGH/` | the 404 page | `src/404.html` |

`/RGH`, `/RGH/` and `/RGH/wc/` redirect to the Home page. Every page shares `src/layout.html` (the sidebar, and the
top bar with the Download button) and `src/wc.css`. A page's address is its file name in `out/site` without `.html`:
another page is another template and another `render()` call in `make.py`.

## The download

The Download buttons give `dist/WiiConverted Setup.zip` (`--setup-zip` names another): the zipped setup folder, with
`WiiConverted Setup.exe` and `script_overrides`. The build refuses a zip without either (a game converted without the
script layers never starts). The Home page shows the zip's size and date. A new release: replace the zip, run
`deploy`.

## Mods

A folder of `web/mods` is a mod on the Mods page: its `mod.json` (the launcher's format: name, author, version,
description, icon, screenshots), its icon and its screenshots. A zip in the folder is the mod's download; without one
the mod says Coming soon.

    python web/make.py pack "<game folder>\mods\<mod>"                copies what the page shows and zips the mod
    python web/make.py pack "<game folder>\mods\<mod>" --list-only    only what the page shows

`pack` zips the whole mod folder under the mod's name, so it unzips into `mods\` as it is. It leaves out what is never
published - other games' ROMs and data files (`.z64`, `data.win`, disc images) and bigfiles, which players bring
themselves - and `icon.txt`. Two files of the site's own, in `web/mods/<mod>/`, go further:

- `leave-out.txt`: patterns of the mod's files that stay out of the download, one a line (`*` reaches into folders,
  a line ending in `/` is a whole folder, `#` starts a comment): another game's assets the mod reads from the
  player's own copy. It's a me! leaves out `entries/MUS/` (Super Mario 64's music as audio files; the mod plays it
  from the player's ROM), Undertale battle `*.ogg` (that game's songs).
- `release/`: files that take the place of the mod's own in the download, or that it adds - It's a me!'s
  `config.ini` without this computer's ROM folder and with the crash hunt off (`debug=0`), and its `sm64.dll`
  (`platform\sm64\build.bat`'s build, copied in by hand and kept out of git): the platform DLL loads it from the mod's
  own folder when the game folder has none. Undertale battle's `config.lua` reads Undertale from Steam's folder.

`pack` points out text files that still name a folder on this computer. The build refuses a zip holding a ROM or a
data file. To take a mod off the page, delete its folder and deploy (its zip goes from R2 too). The zips stay out of
git (the root `.gitignore`'s `*.zip`).

## GitHub

The build asks whether the repository in `make.py` (`GITHUB`: https://github.com/Eggscantfly/Rabbids-Go-Home-WiiConverted)
answers: while it is public, the Download panel links the source and the issues, and once it holds `docs/mods.md`,
the Mods page links the modding guide. Deploy again after pushing `docs/mods.md` and that link appears too.

## How it is served

One Cloudflare Worker, `wiiconverted-web` (`wrangler.toml`), on the routes `joykhloe.com/RGH*` and
`www.joykhloe.com/RGH*`, plus the lower-case `/rgh*`, which only redirects (route paths are case sensitive). The
zone's other Worker, the Robox pages on `/robox*`, is left alone, and every other address is still the main site's.

- The pages, the stylesheet and the pictures are the Worker's static assets (`out/site`). Cloudflare serves them
  before the Worker's code runs, with the headers of `src/_headers`: the files in `assets/` are named after a hash
  of their content and kept by browsers for a year; the pages are checked for changes on every visit.
- The downloads live in the R2 bucket `wiiconverted-downloads`, since the setup's zip is larger than the 25 MiB a
  static asset may be. `worker.js` serves them (with ranges, so a download can resume), the redirects and the 404
  page.
- `deploy` uploads a download only when its bytes changed since the last upload (`.deployed.json`), then runs
  `wrangler deploy`, which uploads the changed assets and the Worker.

wrangler runs from `web/node_modules` (`make.py` installs it with npm the first time). Deploying needs a Cloudflare
login once: `npx wrangler login` in `web/`.

To check the live site:

    curl -I https://www.joykhloe.com/RGH/wc/home

`200` with a `content-security-policy` header is the Worker; the main site's "Coming Soon" page (an `x-contextid`
header) means the route did not match.
