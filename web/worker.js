/**
 * worker.js - WiiConverted's pages on the site: https://www.joykhloe.com/RGH/wc/home and the pages next to it.
 *
 * The pages, the stylesheet and the pictures are this Worker's static assets (web/out/site, which `wrangler deploy`
 * uploads); Cloudflare serves them before this script runs, with the headers of their _headers file. This script
 * only sees the requests that match no asset:
 *
 *   /RGH, /RGH/, /RGH/wc, /RGH/wc/    the Home page (a redirect)
 *   /rgh/..., /RGH/WC/... and so on   the same address in the site's own case (a redirect); route paths are case
 *                                     sensitive, which is why wrangler.toml routes the lower-case /rgh too
 *   /RGH/wc/....zip                   a download, out of the R2 bucket: the setup and the mods (the setup is larger
 *                                     than the 25 MiB a static asset may be)
 *   anything else under /RGH/         the 404 page
 *   /RGHanything, /rghanything        not ours (the route's * matched past the prefix): the main site answers
 */

const ROOT = '/RGH';
const HOME = '/RGH/wc/home';
const NOT_FOUND = '/RGH/wc/404';

// the same as _headers gives the assets
function secure(headers) {
    headers.set('X-Content-Type-Options', 'nosniff');
    headers.set('Referrer-Policy', 'strict-origin-when-cross-origin');
    headers.set('X-Frame-Options', 'DENY');
    headers.set('Content-Security-Policy',
        "default-src 'none'; img-src 'self'; style-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    return headers;
}

function redirect(url, status) {
    return new Response(null, { status, headers: secure(new Headers({ Location: url, 'Cache-Control': 'no-cache' })) });
}

function plain(status, text, extra = {}) {
    return new Response(text, {
        status,
        headers: secure(new Headers({ 'Content-Type': 'text/plain; charset=utf-8', ...extra })),
    });
}

async function notFound(request, env, url) {
    const page = await env.ASSETS.fetch(new Request(new URL(NOT_FOUND, url.origin), { method: 'GET' }));
    if (!page.ok) return plain(404, 'Not found');
    const headers = secure(new Headers(page.headers));
    headers.set('Cache-Control', 'no-cache');
    return new Response(request.method === 'HEAD' ? null : page.body, { status: 404, headers });
}

/**
 * A download out of R2. Ranges are answered, so a download manager can resume the 60 MB setup; the object's
 * Content-Disposition (set when it was uploaded: make.py) names the saved file.
 */
async function download(request, env, key) {
    // Only hand R2 the headers as a range when the client asked for one: passed unconditionally, R2 reports a range
    // on every hit and plain GETs turn into 206s. onlyIf is safe either way.
    const wantsRange = request.headers.has('Range');
    const object = await env.DOWNLOADS.get(key, {
        range: wantsRange ? request.headers : undefined,
        onlyIf: request.headers,
    });
    if (object === null) return null;

    const headers = secure(new Headers());
    object.writeHttpMetadata(headers);
    headers.set('Content-Type', 'application/zip');
    if (!headers.has('Content-Disposition')) headers.set('Content-Disposition', 'attachment');
    headers.set('ETag', object.httpEtag);
    headers.set('Accept-Ranges', 'bytes');
    // the addresses stay the same from one release to the next: always ask whether the file changed
    headers.set('Cache-Control', 'no-cache');

    // no body: the If-None-Match / If-Modified-Since precondition held, nothing changed
    if (!('body' in object) || object.body === null) {
        return new Response(null, { status: 304, headers });
    }

    let status = 200;
    if (wantsRange && object.range) {
        let start, length;
        if ('suffix' in object.range) {
            length = Math.min(object.range.suffix, object.size);
            start = object.size - length;
        } else {
            start = object.range.offset ?? 0;
            length = object.range.length ?? object.size - start;
        }
        status = 206;
        headers.set('Content-Range', `bytes ${start}-${start + length - 1}/${object.size}`);
    }
    return new Response(request.method === 'HEAD' ? null : object.body, { status, headers });
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);
        const path = url.pathname;
        const lower = path.toLowerCase();

        // the route pattern's * also matches /RGHfoo: that is the main site's address, not ours
        if (lower !== '/rgh' && !lower.startsWith('/rgh/')) return fetch(request);

        if (request.method !== 'GET' && request.method !== 'HEAD') {
            return plain(405, 'Method not allowed', { Allow: 'GET, HEAD' });
        }

        // every address of these pages is /RGH/ and lower case after it
        const canonical = ROOT + path.slice(ROOT.length).toLowerCase();
        if (canonical !== path) return redirect(canonical + url.search, 301);

        if (path === '/RGH' || path === '/RGH/' || path === '/RGH/wc' || path === '/RGH/wc/') {
            return redirect(HOME, 302);
        }

        if (path.endsWith('.zip')) {
            let key;
            try {
                key = decodeURIComponent(path.slice(ROOT.length + 1));
            } catch (e) {
                return plain(400, 'Bad request');
            }
            if (!key.includes('..')) {
                const response = await download(request, env, key);
                if (response) return response;
            }
        }

        return notFound(request, env, url);
    },
};
