// wm_media.cpp - what a mod needs to put something of its own on the screen and through the speakers.
//
// None of this knows anything about any particular mod: it is pictures, rectangles, sounds, music, and two
// switches (the world's clock, and the game's own noise).  The mods themselves are Lua or the game's own script -
// this is only what the platform lends them, the way a game with modding support exposes drawing and audio and
// leaves the actual mod to the scripts.
//
//   pictures   MediaImage(bytes) reads a png (or anything else stb_image knows) and keeps it; MediaDrawImage
//              puts a piece of it on the picture.  Textures are made on the device that is drawing and dropped
//              when it goes away.
//   sounds     MediaSound(bytes) plays a RIFF/WAVE or ogg right away, a handful at a time.
//   music      MediaMusic(path) streams an ogg in a thread of its own and loops it; one at a time.
//   the world  MediaFreeze(true) leaves the engine's frame time at almost nothing, so nothing in the game moves
//              while something else has the screen.  MediaBlockInput(true) hands the game a still controller.
//              MediaMuteGame(true) takes the game's own music, effects and voices down.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d9.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "wiimote.h"

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb_vorbis.c"                   // Sean Barrett's ogg decoder, public domain
#undef STB_VORBIS_HEADER_ONLY
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "vendor/stb_image.h"
}

namespace {

// ---------------------------------------------------------------------------------------------------------------
// pictures
// ---------------------------------------------------------------------------------------------------------------
struct Image {
    std::vector<uint8_t> px;                     // BGRA
    int w, h;
    IDirect3DTexture9* tex;
    IDirect3DDevice9* dev;
    Image() : w(0), h(0), tex(NULL), dev(NULL) {}
};

std::map<int, Image> s_images;
int s_nextImage = 1;

struct Vtx { float x, y, z, rhw; DWORD colour; float u, v; };

IDirect3DDevice9* s_drawing;                     // set while a draw is under way
RECT s_picture;

void Release(Image& im) {
    if (im.tex) im.tex->Release();
    im.tex = NULL;
    im.dev = NULL;
}

bool Upload(IDirect3DDevice9* dev, Image& im) {
    if (im.px.empty()) return false;
    if (im.tex && im.dev == dev) return true;
    Release(im);
    if (FAILED(dev->CreateTexture(im.w, im.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &im.tex, NULL))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(im.tex->LockRect(0, &lr, NULL, 0))) { Release(im); return false; }
    for (int y = 0; y < im.h; ++y)
        memcpy((uint8_t*)lr.pBits + y * lr.Pitch, &im.px[(size_t)y * im.w * 4], (size_t)im.w * 4);
    im.tex->UnlockRect(0);
    im.dev = dev;
    return true;
}

// the state a flat overlay wants, whatever the engine left behind (its own shaders included)
void Flat(IDirect3DDevice9* dev, IDirect3DTexture9* tex) {
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetPixelShader(NULL);
    dev->SetVertexShader(NULL);
    dev->SetTexture(0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, tex ? D3DTA_TEXTURE : D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, tex ? D3DTA_TEXTURE : D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    if (tex) {
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);   // pixels stay pixels
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
}

// ---------------------------------------------------------------------------------------------------------------
// sounds: a few at a time, each its own little output
// ---------------------------------------------------------------------------------------------------------------
enum { VOICES = 8 };
struct Voice {
    HWAVEOUT out;
    WAVEHDR hdr;
    std::vector<short> pcm;
};
Voice s_voice[VOICES];

void FreeVoice(Voice& v) {
    if (!v.out) return;
    waveOutReset(v.out);
    if (v.hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(v.out, &v.hdr, sizeof(WAVEHDR));
    waveOutClose(v.out);
    v.out = NULL;
    memset(&v.hdr, 0, sizeof(v.hdr));
}

bool Decode(const uint8_t* b, size_t n, std::vector<short>& pcm, int& rate, int& channels) {
    if (n > 12 && memcmp(b, "OggS", 4) == 0) {
        int ch = 0, hz = 0;
        short* out = NULL;
        int frames = stb_vorbis_decode_memory((const unsigned char*)b, (int)n, &ch, &hz, &out);
        if (frames <= 0 || !out) return false;
        pcm.assign(out, out + (size_t)frames * ch);
        free(out);
        rate = hz;
        channels = ch;
        return true;
    }
    if (n < 44 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) return false;
    uint32_t p = 12, dataAt = 0, dataLen = 0;
    int bits = 16;
    rate = 22050;
    channels = 1;
    while (p + 8 <= n) {
        uint32_t id, size;
        memcpy(&id, b + p, 4);
        memcpy(&size, b + p + 4, 4);
        if (id == 0x20746D66 && p + 24 <= n) {                       // "fmt "
            channels = (int)(b[p + 10] | (b[p + 11] << 8));
            memcpy(&rate, b + p + 12, 4);
            bits = (int)(b[p + 22] | (b[p + 23] << 8));
        } else if (id == 0x61746164) {                               // "data"
            dataAt = p + 8;
            dataLen = size;
            break;
        }
        uint32_t next = p + 8 + size + (size & 1);
        if (next <= p) break;
        p = next;
    }
    if (!dataAt || dataAt + dataLen > n || bits != 16 || channels < 1 || channels > 2) return false;
    pcm.assign((const short*)(b + dataAt), (const short*)(b + dataAt) + dataLen / 2);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// music: one ogg, streamed and looped
// ---------------------------------------------------------------------------------------------------------------
enum { MUS_BLOCKS = 4, MUS_FRAMES = 4096 };
struct Music {
    stb_vorbis* v;
    HWAVEOUT out;
    WAVEHDR hdr[MUS_BLOCKS];
    short buf[MUS_BLOCKS][MUS_FRAMES * 2];
    HANDLE thread;
    volatile LONG run;
    int volume;
    bool loop;
};
Music s_mus;

DWORD WINAPI MusicThread(LPVOID) {
    while (InterlockedCompareExchange(&s_mus.run, 1, 1)) {
        bool wrote = false;
        for (int i = 0; i < MUS_BLOCKS; ++i) {
            if (s_mus.hdr[i].dwFlags & WHDR_INQUEUE) continue;
            int got = stb_vorbis_get_samples_short_interleaved(s_mus.v, 2, s_mus.buf[i], MUS_FRAMES * 2);
            if (got <= 0) {
                if (!s_mus.loop) break;
                stb_vorbis_seek_start(s_mus.v);
                got = stb_vorbis_get_samples_short_interleaved(s_mus.v, 2, s_mus.buf[i], MUS_FRAMES * 2);
                if (got <= 0) break;
            }
            int n = got * 2;
            if (s_mus.volume != 100)
                for (int k = 0; k < n; ++k) s_mus.buf[i][k] = (short)(s_mus.buf[i][k] * s_mus.volume / 100);
            s_mus.hdr[i].dwBufferLength = (DWORD)(n * sizeof(short));
            if (waveOutWrite(s_mus.out, &s_mus.hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) break;
            wrote = true;
        }
        if (!wrote) Sleep(4);
    }
    return 0;
}

bool s_frozen, s_blocking, s_muted;

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// what the mods get
// ---------------------------------------------------------------------------------------------------------------
int MediaImage(const void* bytes, size_t n) {
    int w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory((const unsigned char*)bytes, (int)n, &w, &h, &comp, 4);
    if (!rgba) return 0;
    Image im;
    im.w = w;
    im.h = h;
    im.px.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {                     // RGBA -> BGRA
        im.px[i * 4 + 0] = rgba[i * 4 + 2];
        im.px[i * 4 + 1] = rgba[i * 4 + 1];
        im.px[i * 4 + 2] = rgba[i * 4 + 0];
        im.px[i * 4 + 3] = rgba[i * 4 + 3];
    }
    stbi_image_free(rgba);
    int id = s_nextImage++;
    s_images[id] = im;
    return id;
}

bool MediaImageSize(int id, int& w, int& h) {
    std::map<int, Image>::const_iterator it = s_images.find(id);
    if (it == s_images.end()) return false;
    w = it->second.w;
    h = it->second.h;
    return true;
}

void MediaImageFree(int id) {
    std::map<int, Image>::iterator it = s_images.find(id);
    if (it == s_images.end()) return;
    Release(it->second);
    s_images.erase(it);
}

void MediaDrawBegin(IDirect3DDevice9* dev, const RECT& picture) {
    s_drawing = dev;
    s_picture = picture;
}

void MediaDrawEnd() {
    s_drawing = NULL;
}

bool MediaDrawing() { return s_drawing != NULL; }

RECT MediaPicture() { return s_picture; }

void MediaRect(float x, float y, float w, float h, uint32_t colour) {
    if (!s_drawing) return;
    Flat(s_drawing, NULL);
    Vtx q[4] = {
        { x, y, 0.0f, 1.0f, colour, 0.0f, 0.0f }, { x + w, y, 0.0f, 1.0f, colour, 1.0f, 0.0f },
        { x, y + h, 0.0f, 1.0f, colour, 0.0f, 1.0f }, { x + w, y + h, 0.0f, 1.0f, colour, 1.0f, 1.0f },
    };
    s_drawing->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
}

// a piece of a picture (u0..u1, v0..v1 are pixels of it) drawn into x, y, w, h of the window
void MediaDrawImage(int id, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    uint32_t tint) {
    if (!s_drawing) return;
    std::map<int, Image>::iterator it = s_images.find(id);
    if (it == s_images.end() || !Upload(s_drawing, it->second)) return;
    Image& im = it->second;
    float tu0 = u0 / im.w, tu1 = u1 / im.w, tv0 = v0 / im.h, tv1 = v1 / im.h;
    Flat(s_drawing, im.tex);
    Vtx q[4] = {
        { x, y, 0.0f, 1.0f, tint, tu0, tv0 }, { x + w, y, 0.0f, 1.0f, tint, tu1, tv0 },
        { x, y + h, 0.0f, 1.0f, tint, tu0, tv1 }, { x + w, y + h, 0.0f, 1.0f, tint, tu1, tv1 },
    };
    s_drawing->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
}

void MediaDeviceLost() {
    for (std::map<int, Image>::iterator it = s_images.begin(); it != s_images.end(); ++it) Release(it->second);
    s_drawing = NULL;
}

bool MediaSound(const void* bytes, size_t n, int volume) {
    Voice* v = NULL;
    for (int i = 0; i < VOICES; ++i) {
        if (!s_voice[i].out) { v = &s_voice[i]; break; }
        if (s_voice[i].hdr.dwFlags & WHDR_DONE) { FreeVoice(s_voice[i]); v = &s_voice[i]; break; }
    }
    if (!v) return false;
    int rate = 0, channels = 0;
    if (!Decode((const uint8_t*)bytes, n, v->pcm, rate, channels) || v->pcm.empty()) return false;
    if (volume != 100)
        for (size_t i = 0; i < v->pcm.size(); ++i) v->pcm[i] = (short)(v->pcm[i] * volume / 100);
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = (DWORD)rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    if (waveOutOpen(&v->out, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { v->out = NULL; return false; }
    memset(&v->hdr, 0, sizeof(v->hdr));
    v->hdr.lpData = (LPSTR)&v->pcm[0];
    v->hdr.dwBufferLength = (DWORD)(v->pcm.size() * sizeof(short));
    waveOutPrepareHeader(v->out, &v->hdr, sizeof(WAVEHDR));
    waveOutWrite(v->out, &v->hdr, sizeof(WAVEHDR));
    return true;
}

void MediaSoundStopAll() {
    for (int i = 0; i < VOICES; ++i) FreeVoice(s_voice[i]);
}

void MediaMusicStop() {
    if (s_mus.run) {
        InterlockedExchange(&s_mus.run, 0);
        if (s_mus.thread) {
            WaitForSingleObject(s_mus.thread, 2000);
            CloseHandle(s_mus.thread);
            s_mus.thread = NULL;
        }
    }
    if (s_mus.out) {
        waveOutReset(s_mus.out);
        for (int i = 0; i < MUS_BLOCKS; ++i)
            if (s_mus.hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(s_mus.out, &s_mus.hdr[i], sizeof(WAVEHDR));
        waveOutClose(s_mus.out);
        s_mus.out = NULL;
    }
    if (s_mus.v) {
        stb_vorbis_close(s_mus.v);
        s_mus.v = NULL;
    }
}

bool MediaMusic(const char* path, int volume, bool loop) {
    MediaMusicStop();
    int err = 0;
    s_mus.v = stb_vorbis_open_filename(path, &err, NULL);
    if (!s_mus.v) return false;
    stb_vorbis_info info = stb_vorbis_get_info(s_mus.v);
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = info.sample_rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * 2);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    if (waveOutOpen(&s_mus.out, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        stb_vorbis_close(s_mus.v);
        s_mus.v = NULL;
        return false;
    }
    for (int i = 0; i < MUS_BLOCKS; ++i) {
        memset(&s_mus.hdr[i], 0, sizeof(WAVEHDR));
        s_mus.hdr[i].lpData = (LPSTR)s_mus.buf[i];
        s_mus.hdr[i].dwBufferLength = sizeof(s_mus.buf[i]);
        waveOutPrepareHeader(s_mus.out, &s_mus.hdr[i], sizeof(WAVEHDR));
        s_mus.hdr[i].dwFlags &= ~WHDR_INQUEUE;
    }
    s_mus.volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    s_mus.loop = loop;
    InterlockedExchange(&s_mus.run, 1);
    s_mus.thread = CreateThread(NULL, 0, MusicThread, NULL, 0, NULL);
    return true;
}

void MediaFreeze(bool on) { s_frozen = on; }
bool MediaFrozen() { return s_frozen; }
void MediaBlockInput(bool on) { s_blocking = on; }
bool MediaInputBlocked() { return s_blocking; }

void MediaMuteGame(bool on) {
    if (s_muted == on) return;
    s_muted = on;
    OptionsSilenceMusic(on);
    OptionsSilenceEffects(on);
    OptionsSilenceVoices(on);
}

// everything a mod left on, put back - called when the mods are reloaded
void MediaReset() {
    MediaMusicStop();
    MediaSoundStopAll();
    for (std::map<int, Image>::iterator it = s_images.begin(); it != s_images.end(); ++it) Release(it->second);
    s_images.clear();
    s_frozen = false;
    s_blocking = false;
    MediaMuteGame(false);
}
