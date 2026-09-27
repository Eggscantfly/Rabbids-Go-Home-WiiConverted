// wm_afx.cpp - the Wii after effects ([video] afx=1 in wiimote.ini).
//
// Why: most RGH levels carry an AFX modifier (after effect) that the Wii executable draws over the 3D image every
// frame: a colour grade (saturation, a contrast gain up to 1.5, a brightness offset), a blurred copy of the frame
// added on top of it (a soft glow) and a small tint. The effect records are the same in both releases, but the PC
// executable renders an AFX only through the HLSL model its HLSL instance names, and RGH ships no such model: the
// instance's model pointer is NULL, K3D_HLSL_Instance::Execute returns at once and the PC frame stays ungraded.
//
// Switch: the Wii turns the effects on for good in K3D::AFX_Init (display +0x1AF50 = 1; only the debug console's AFX
// command turns them off). The PC executable keeps the switch in a global (009DE174) that the display constructor sets
// to 0 and only a PC-only script native (id 0x232A, handler 005226B0) changes; the Wii scripts never call it, so the
// PC AFX_User returns at once in every level. The replacement ignores that global: [video] afx is the switch.
//
// Where: K3D::Render2D calls K3D::AFX_User(0) right after the 3D image (before the 3D strings and the 2D layers) and
// AFX_User(1) at its end, like the Wii. Every frame AFX::ApplyAlways of each applied AFX modifier registers its HLSL
// instance, mode and viewport mask in the display's table (K3D +0x68444, count +0x684A4). The PC AFX_User (004451A0)
// is replaced: for each entry of the requested mode and the current view, the effect named by the instance's FX type
// is drawn with Direct3D 9 the way the Wii executable draws it with GX:
//
//   type 10 AFX_S_OldMovie        colour grade + glow + tint (39 records); CircleRatio / CenterWidth / vBorderColor
//                                 are read but, like on the Wii, not drawn
//   type  4 AFX_S_Remanence       frame + GlowFactor^2 * BigBlur(frame)
//   type  3 AFX_S_ColorCorrection add colour, saturation, contrast gain, brightness offset (blended passes)
//   type  1 AFX_S_BigBlur         BigBlur(frame)
//   type  2 AFX_S_DepthBlur       not drawn (2 records, both off until a script applies them); logged once
//
// Variables: scripts change a built effect through MDF_Setf / MDF_Setv (the Wii's global fade to black or white and
// its black-and-white mode write the full-screen ColorCorrection's add colour, add coefficient and saturation). The
// Wii writes through the field each model variable was bound to when the effect was built; the PC writes a variable
// buffer nothing draws. The AFX vtable's variable slots are hooked to apply the Wii binding on top.
//
// Parameters: the Wii builds an effect from the instance's model variables (bound) and then its saved variables, by
// case-insensitive substring match of the variable names (SetParamsFromVars of each AFX_S class); the same lists are
// in the PC instance (+0x14/+0x18 model, +0x1C/+0x20 saved; 24-byte entries {name, flags, type, size, data}).
//
// The GX maths is kept: 8-bit konst factors (trunc(255*f) & 0xFF, weight (k + (k >> 7)) / 256), each TEV stage and
// blend clamped to [0, 1], the grey of the saturation stages taken from the red channel (the Wii swaps R into G and
// B), and BigBlur's schedule: a 2x box-filtered copy, 4-tap cross passes at UV offsets 2*factor (at most 0.0125),
// halved per pass while the next half is above 1/640, weights 15/64 15/64 18/64 16/64, bilinear stretch back. The
// blur buffers are 320x240 like the Wii's half-size EFB copies, so the glow keeps the Wii's softness at any resolution.
#include "wiimote.h"

#include <d3d9.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace wmpatch;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// the PC executable (2010 build)
// ---------------------------------------------------------------------------------------------------------------
const uint32_t AFX_USER      = 0x004451A0;   // K3D::AFX_User(u32 mode), thiscall, ret 4 (replaced)
const uint32_t G_AFX_ON      = 0x009DE174;   // the PC's AFX_User returns at once while 0 (never set by the Wii scripts)
const uint32_t G_DISPLAY     = 0x009DE178;   // K3D_gpo_Display
const uint32_t G_DRIVER      = 0x00A6E834;   // GRD_Driver*
const uint32_t DRV_DEVICE    = 0x26F0;       // GRD_Driver: IDirect3DDevice9*
const uint32_t D_DRAWGLOBAL  = 0x4D4;        // K3D::u32_GetDrawGlobalMask (00417100)
const uint32_t D_DRAWFLAGS   = 0x4DC;
const uint32_t D_VIEWNUM     = 0x6B0;        // K3D::u32_GetEngineViewNum (004171F0)
const uint32_t D_AFX_TABLE   = 0x68444;      // K3D_tt_AFX_[8] {u16 mode, u8 viewports, u8, K3D_HLSL_Instance*}
const uint32_t D_AFX_NUM     = 0x684A4;
const uint32_t I_FXTYPE      = 0x08;         // K3D_HLSL_Instance
const uint32_t I_MODELVARS   = 0x14, I_MODELVARN = 0x18, I_SAVEVARS = 0x1C, I_SAVEVARN = 0x20;
const uint32_t AFX_INSTANCE  = 0x20;         // AFX (modifier): K3D_HLSL_Instance*
const uint32_t AFX_VTABLE    = 0x008AC7CC;
const uint32_t INST_DESTROY  = 0x0086A9B0;   // K3D_HLSL_Instance::Destroy, thiscall

struct Code { uint32_t va, len, crc; const char* what; };

const Code kCode[] = {
    { 0x004451A0, 0x1AE, 0xFF4FD461, "K3D::AFX_User" },
    { 0x00417100, 0x010, 0x903F7692, "K3D::u32_GetDrawGlobalMask" },
    { 0x004171F0, 0x010, 0x6512C3EB, "K3D::u32_GetEngineViewNum" },
    { 0x006232A0, 0x036, 0xB058E69C, "AFX::IFVSetf" },
    { 0x00623360, 0x043, 0x90E01CCA, "AFX::IFVSetv" },
    { 0x006232E0, 0x037, 0xD2684ED7, "AFX::IFVGetf" },
    { 0x00623320, 0x034, 0x5CAB3A24, "AFX::IFVGetv" },
    { 0x0086A9B0, 0x02E, 0x0A9407BB, "K3D_HLSL_Instance::Destroy" },
};

struct CallSite { uint32_t va, target; const char* what; };

const CallSite kCalls[] = {
    { 0x0086A424, 0x004451A0, "K3D::Render2D -> AFX_User(0)" },
    { 0x0086A5AB, 0x004451A0, "K3D::Render2D -> AFX_User(1)" },
    { 0x0086A9E8, 0, "K3D::AFX_SetHLSL: [ecx+edx*8+0x68448]" },
    { 0x00567ACE, INST_DESTROY, "AFX::Destroy -> Instance::Destroy" },
    { 0x005E15FA, INST_DESTROY, "EVE::AFXEvent_Destroy -> Instance::Destroy" },
};

// AFX vtable slots of the variable natives: MDF_Setf_C (+0x8C), MDF_Setv_C (+0x90), MDF_Getf_C (+0x98), MDF_Getv_C
// (+0xA0); each passes the variable index as (b << 16) | a
struct VtSlot { uint32_t slot, target; const char* what; };

const VtSlot kSlots[] = {
    { 35, 0x006232A0, "AFX vtable +0x8C IFVSetf" },
    { 36, 0x00623360, "AFX vtable +0x90 IFVSetv" },
    { 38, 0x006232E0, "AFX vtable +0x98 IFVGetf" },
    { 40, 0x00623320, "AFX vtable +0xA0 IFVGetv" },
};

// ---------------------------------------------------------------------------------------------------------------
// effect parameters (pure, self-tested)
// ---------------------------------------------------------------------------------------------------------------
enum { FX_BIGBLUR = 1, FX_DEPTHBLUR = 2, FX_COLORCORRECTION = 3, FX_REMANENCE = 4, FX_OLDMOVIE = 10 };
enum { VT_INT = 3, VT_FLOAT = 4, VT_VEC3 = 6 };
enum { MAX_VARS = 64 };

struct Var {                                 // one K3D_HLSL_tt_StoreVar, already read
    const char* name;
    int type;
    const float* data;                       // type 4: 1 float, type 6: 3 floats
    int index;                               // in its list
};

struct Params {
    int type;
    // OldMovie
    float circleRatio, blur, remanence, centerWidth, centerSat, centerCon, centerBri;
    float centerColor[3], borderColor[3];
    // ColorCorrection
    float bri, sat, con, fBri, fSat, fCon, colorAdd[3], addCoef;
    // Remanence / BigBlur
    float glow, bigBlur;
    // DepthBlur
    float zEnd, zStart, bigBlurEnd;
};

bool HasI(const char* s, const char* sub) {  // STD_strstri
    if (!s || !sub) return false;
    size_t n = strlen(sub);
    for (; *s; ++s)
        if (_strnicmp(s, sub, n) == 0) return true;
    return false;
}

void Defaults(int type, Params& p) {
    memset(&p, 0, sizeof(p));
    p.type = type;
    // AFX_S_OldMovie::__ct
    p.circleRatio = 0.7f; p.blur = 0.005f; p.remanence = 0.1f; p.centerWidth = 0.7f;
    p.centerColor[0] = 0.5f; p.centerColor[1] = 0.5f; p.centerColor[2] = 0.0f;
    p.centerSat = 0.5f; p.centerCon = 1.0f; p.centerBri = 1.0f;
    p.borderColor[0] = 0.0f; p.borderColor[1] = 0.0f; p.borderColor[2] = 0.5f;
    // AFX_S_ColorCorrection::__ct
    p.bri = p.sat = p.con = 0.5f;
    p.fBri = p.fSat = p.fCon = 1.0f;
    // AFX_S_Remanence::__ct (AFX_S_BigBlur::__ct: factor 0)
    p.glow = 0.5f;
    p.bigBlur = type == FX_BIGBLUR ? 0.0f : 0.085f;
}

// The effect fields a variable can bind to
enum Field {
    F_NONE = -1,
    F_CIRCLE, F_BLUR, F_REMANENCE, F_CENTERWIDTH, F_SAT, F_CON, F_BRI, F_BORDERCOLOR, F_CENTERCOLOR,  // OldMovie
    F_CC_ADD, F_CC_BRI, F_CC_FBRI, F_CC_SAT, F_CC_FSAT, F_CC_CON, F_CC_FCON, F_CC_ADDCOEF,            // ColorCorr.
    F_GLOW, F_BIGBLUR,                                                                                // Remanence
    F_ZEND, F_ZSTART, F_BIGBLUREND,                                                                   // DepthBlur
};

// The field a variable sets, as the class's SetParamsFromVars matches it (first case-insensitive substring match)
int MatchField(int fxType, const char* nm, int varType) {
    if (!nm) return F_NONE;
    switch (fxType) {
    case FX_OLDMOVIE:
        if (varType == VT_FLOAT) {
            if (HasI(nm, "CircleRatio")) return F_CIRCLE;
            if (HasI(nm, "BlurFactor")) return F_BLUR;
            if (HasI(nm, "RemanenceFactor")) return F_REMANENCE;
            if (HasI(nm, "CenterWidth")) return F_CENTERWIDTH;
            if (HasI(nm, "Saturation")) return F_SAT;
            if (HasI(nm, "Contrast")) return F_CON;
            if (HasI(nm, "Brightness")) return F_BRI;
        } else if (varType == VT_VEC3) {
            if (HasI(nm, "vBorderColor")) return F_BORDERCOLOR;
            if (HasI(nm, "Color")) return F_CENTERCOLOR;
        }
        break;
    case FX_COLORCORRECTION:
        if (varType == VT_VEC3) {
            if (HasI(nm, "vcoloradd")) return F_CC_ADD;
        } else if (varType == VT_FLOAT) {
            if (HasI(nm, "colorbrightness")) return F_CC_BRI;
            if (HasI(nm, "factorbrightness")) return F_CC_FBRI;
            if (HasI(nm, "colorsaturation")) return F_CC_SAT;
            if (HasI(nm, "factorsaturation")) return F_CC_FSAT;
            if (HasI(nm, "colorcontrast")) return F_CC_CON;
            if (HasI(nm, "factorcontrast")) return F_CC_FCON;
            if (HasI(nm, "coloraddcoef")) return F_CC_ADDCOEF;
        }
        break;
    case FX_REMANENCE:
        if (varType == VT_FLOAT) {
            if (HasI(nm, "glowfactor")) return F_GLOW;
            if (HasI(nm, "bigblurfactor")) return F_BIGBLUR;
        }
        break;
    case FX_BIGBLUR:
        if (varType == VT_FLOAT && HasI(nm, "BigBlurFactor")) return F_BIGBLUR;
        break;
    case FX_DEPTHBLUR:
        if (varType == VT_FLOAT) {
            if (HasI(nm, "zend")) return F_ZEND;
            if (HasI(nm, "zstart")) return F_ZSTART;
            if (HasI(nm, "bigblurfactor")) return F_BIGBLUREND;
        }
        break;
    }
    return F_NONE;
}

float* FieldPtr(Params& p, int f) {
    switch (f) {
    case F_CIRCLE: return &p.circleRatio;
    case F_BLUR: return &p.blur;
    case F_REMANENCE: return &p.remanence;
    case F_CENTERWIDTH: return &p.centerWidth;
    case F_SAT: return &p.centerSat;
    case F_CON: return &p.centerCon;
    case F_BRI: return &p.centerBri;
    case F_BORDERCOLOR: return p.borderColor;
    case F_CENTERCOLOR: return p.centerColor;
    case F_CC_ADD: return p.colorAdd;
    case F_CC_BRI: return &p.bri;
    case F_CC_FBRI: return &p.fBri;
    case F_CC_SAT: return &p.sat;
    case F_CC_FSAT: return &p.fSat;
    case F_CC_CON: return &p.con;
    case F_CC_FCON: return &p.fCon;
    case F_CC_ADDCOEF: return &p.addCoef;
    case F_GLOW: return &p.glow;
    case F_BIGBLUR: return &p.bigBlur;
    case F_ZEND: return &p.zEnd;
    case F_ZSTART: return &p.zStart;
    case F_BIGBLUREND: return &p.bigBlurEnd;
    }
    return NULL;
}

inline int FieldSize(int f) { return f == F_BORDERCOLOR || f == F_CENTERCOLOR || f == F_CC_ADD ? 3 : 1; }

// SetParamsFromVars of the class of `p.type`, over one variable list. With `bind` (the model variables), the Wii
// points each matched variable at its field, and the variable natives read and write the field through it:
// bind[variable index] = field.
void ApplyVars(Params& p, const Var* v, int n, signed char* bind) {
    for (int i = 0; i < n; ++i) {
        if (!v[i].name || !v[i].data) continue;
        int f = MatchField(p.type, v[i].name, v[i].type);
        float* dst = FieldPtr(p, f);
        if (!dst) continue;
        memcpy(dst, v[i].data, FieldSize(f) * sizeof(float));
        if (bind && v[i].index >= 0 && v[i].index < MAX_VARS) bind[v[i].index] = (signed char)f;
        if (p.type == FX_OLDMOVIE && f == F_BLUR && p.blur > 0.003125f) p.blur = 0.003125f;   // after binding
    }
}

// fctiwz + stb: the byte a GXColor konst gets
inline int KByte(float f) {
    double t = (double)f * 255.0;
    if (!(t > -2147483648.0 && t < 2147483647.0)) return 0;
    return (int)t & 0xFF;
}

// TEV weight of a konst byte
inline float KWeight(int k) { return (float)(k + (k >> 7)) / 256.0f; }

// BigBlur: the UV offsets of the passes it draws (2*factor at most 0.0125, halved while the next half > 1/640)
int BlurPasses(float factor, float* out, int maxOut) {
    if (!(factor > 0.0f)) return -1;                 // no blur at all
    float f = 2.0f * factor;
    if (f > 0.0125f) f = 0.0125f;
    if (!(f > 0.0015625f)) return -1;
    int n = 0;
    for (;;) {
        float off = f;
        f *= 0.5f;
        if (!(f > 0.0015625f)) break;
        if (n < maxOut) out[n] = off;
        ++n;
    }
    return n;                                        // 0: only the half-size copy, stretched back
}

// AFX_S_OldMovie::Apply's konst colours as the constants of kPsOldMovie (c1.w, the blur source, is set by the caller).
// TEV arithmetic per stage, k' = k + (k >> 7): lerp and scale by a konst weigh k' / 256; ONE scaled by a konst adds
// (255 * k') >> 8; a konst scaled by the source adds about k / 255 of it; a konst added as is adds k / 255.
// Returns false when the effect leaves the image unchanged.
bool OldMovieConstants(const Params& p, float c0[4], float c1[4], float c2[4]) {
    for (int i = 0; i < 4; ++i) c0[i] = c1[i] = c2[i] = 0.0f;
    if (p.centerSat != 1.0f) {
        if (p.centerSat - 1.0f < 0.0f) { c1[0] = 1; c0[0] = KWeight(KByte(p.centerSat)); }
        else { c1[0] = 2; c0[0] = KWeight(KByte(p.centerSat - 1.0f)); }
    }
    if (p.centerCon != 1.0f) {
        if (p.centerCon - 1.0f > 0.0f) { c1[1] = 1; c0[1] = KWeight(KByte(p.centerCon - 1.0f)); }
        else { c1[1] = 2; c0[1] = KWeight(KByte(1.0f - p.centerCon)); }
    }
    if (p.centerBri != 1.0f) {
        bool up = p.centerBri - 1.0f > 0.0f;
        int k = up ? KByte(p.centerBri - 1.0f) : KByte(1.0f - p.centerBri);
        c1[2] = up ? 1.0f : 2.0f;
        c0[2] = (float)((255 * (k + (k >> 7))) >> 8) / 255.0f;
    }
    bool rem = p.remanence > 0.0f;
    bool tint = p.centerColor[0] > 0.0f || p.centerColor[1] > 0.0f || p.centerColor[2] > 0.0f;
    bool color = p.centerSat != 1.0f || p.centerCon != 1.0f || p.centerBri != 1.0f;
    if (rem) c0[3] = (float)KByte(p.remanence) / 255.0f;
    if (tint) {
        for (int i = 0; i < 3; ++i) c2[i] = (float)KByte(0.1f * p.centerColor[i]) / 255.0f;
        c2[3] = 1;
    }
    return rem || tint || color;
}

// ---------------------------------------------------------------------------------------------------------------
// shaders (ps_2_0; the quad's TEXCOORD0 is 0..1 over the target)
// ---------------------------------------------------------------------------------------------------------------
const char kPsCopy[] =
    "sampler s0 : register(s0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(tex2D(s0, uv).rgb, 1); }\n";

// AFX_S::BigBlur pass: stage 0 tex(+u), stage 1 lerp 4/8 tex(-v), stage 2 lerp 3/8 tex(-u), stage 3 lerp 2/8 tex(+v)
const char kPsBlur[] =
    "sampler s0 : register(s0);\n"
    "float4 c0 : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float f = c0.x;\n"
    "  float3 p = tex2D(s0, uv + float2(f, 0)).rgb;\n"
    "  p = lerp(p, tex2D(s0, uv + float2(0, -f)).rgb, c0.y);\n"
    "  p = lerp(p, tex2D(s0, uv + float2(-f, 0)).rgb, c0.z);\n"
    "  p = lerp(p, tex2D(s0, uv + float2(0, f)).rgb, c0.w);\n"
    "  return float4(p, 1);\n"
    "}\n";

// AFX_S_OldMovie::Apply TEV chain. c0 = konst weights (saturation, contrast, brightness, remanence); c1 = modes
// (saturation 1 below/2 above 1, contrast 1 above/2 below 1, brightness 1 add/2 subtract, remanence source 1 = blur);
// c2 = tint konst weights, w = on.
const char kPsOldMovie[] =
    "sampler s0 : register(s0);\n"
    "sampler s1 : register(s1);\n"
    "float4 c0 : register(c0);\n"
    "float4 c1 : register(c1);\n"
    "float4 c2 : register(c2);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 S = tex2D(s0, uv).rgb;\n"
    "  float3 X = S;\n"
    "  float3 g = S.rrr;\n"
    "  if (c1.x > 1.5) X = saturate(S + saturate(S - g) * c0.x);\n"
    "  else if (c1.x > 0.5) X = lerp(g, S, c0.x);\n"
    "  if (c1.y > 1.5) X = lerp(X, 128.0 / 255.0, c0.y);\n"
    "  else if (c1.y > 0.5) X = saturate(X + X * c0.y);\n"
    "  if (c1.z > 1.5) X = saturate(X - c0.z);\n"
    "  else if (c1.z > 0.5) X = saturate(X + c0.z);\n"
    "  if (c0.w > 0) {\n"
    "    float3 R = c1.w > 0.5 ? tex2D(s1, uv).rgb : S;\n"
    "    X = saturate(X + R * c0.w);\n"
    "  }\n"
    "  if (c2.w > 0.5) X = saturate(X + c2.rgb);\n"
    "  return float4(X, 1);\n"
    "}\n";

// AFX_S_Remanence::Apply: frame (s0) blended ONE over the blurred frame (s1) weighted by the TEV alpha k*k
const char kPsRemanence[] =
    "sampler s0 : register(s0);\n"
    "sampler s1 : register(s1);\n"
    "float4 c0 : register(c0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  return float4(saturate(tex2D(s0, uv).rgb + tex2D(s1, uv).rgb * c0.x), 1);\n"
    "}\n";

// AFX_S_ColorCorrection::Apply, its framebuffer passes in one shader. c0 = add colour (bytes/255), w = 1 - add alpha;
// c1 = (on add, saturation weight a, contrast mode 1 gain/2 toward grey, brightness mode 1 add/2 subtract);
// c2 = (contrast weight, second contrast gain weight, brightness weight, saturation above 1 weight).
const char kPsColorCorrection[] =
    "sampler s0 : register(s0);\n"
    "float4 c0 : register(c0);\n"
    "float4 c1 : register(c1);\n"
    "float4 c2 : register(c2);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 X = tex2D(s0, uv).rgb;\n"
    "  if (c1.x > 0.5) X = saturate(c0.rgb + X * c0.w);\n"
    "  if (c1.y > 0) X = lerp(X, X.rrr, c1.y);\n"
    "  if (c2.w > 0) X = saturate(X + saturate(X - X.rrr) * c2.w);\n"
    "  if (c1.z > 1.5) X = lerp(X, 127.0 / 255.0, c2.x);\n"
    "  else if (c1.z > 0.5) { X = saturate(X + X * c2.x); X = saturate(X + X * c2.y); }\n"
    "  if (c1.w > 1.5) X = saturate(X - c2.z);\n"
    "  else if (c1.w > 0.5) X = saturate(X + c2.z);\n"
    "  return float4(X, 1);\n"
    "}\n";

// ---------------------------------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------------------------------
bool s_enabled;
bool s_userOn = true;                // the Options screen's WII EFFECTS switch
bool s_installed;
bool s_inAttach;
bool s_log;
unsigned s_dumpMax;                          // [video] afx_dump: frame pairs to save (1 = 20)
std::vector<std::string> s_pending;
std::string* s_testLog;

void AfxLog(const char* fmt, ...) {
    char buf[700];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (s_testLog) {
        *s_testLog += "    ";
        *s_testLog += buf;
        *s_testLog += "\n";
    } else if (s_inAttach) {
        s_pending.push_back(buf);
    } else {
        Log("AFX: %s", buf);
    }
}

struct ID3DXBufferMin : IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};
typedef HRESULT (WINAPI* CompileFn)(LPCSTR, UINT, const void*, void*, LPCSTR, LPCSTR, DWORD, ID3DXBufferMin**,
                                    ID3DXBufferMin**, void**);
typedef HRESULT (WINAPI* SaveTexFn)(LPCSTR, DWORD, IDirect3DBaseTexture9*, const void*);
typedef HRESULT (STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

enum { HALF_W = 320, HALF_H = 240, MID_W = 640, MID_H = 480 };

struct Target {
    IDirect3DTexture9* tex;
    IDirect3DSurface9* surf;
    UINT w, h;
    D3DFORMAT fmt;
};

struct Gpu {
    IDirect3DDevice9* dev;
    IDirect3DStateBlock9* sb;
    IDirect3DPixelShader9* psCopy, *psBlur, *psOldMovie, *psRemanence, *psColor;
    Target scene;                            // the frame, full size
    Target mid;                              // at most 640x480 on the way down
    Target half[2];                          // BigBlur buffers, 320x240
    Target work;                             // full size: the blurred frame stretched back / pass results
    bool broken;                             // shader compile or resource failure: do nothing from now on
};

Gpu s_gpu;
ResetFn s_origReset;
bool s_resetHooked;
bool s_effectsOn = true;                     // [video] afx: the effects drawn (the hook stays for the scene draw)
void (*s_sceneDraw)(IDirect3DDevice9*, uint32_t) = NULL;   // draws into the 3D image before the effects (wm_sm64)
void (*s_sceneLost)() = NULL;
uint32_t s_lastSig[2];                       // parameters last logged, per mode
uint32_t s_lastTypes[2];                     // instances and types last logged, per mode
DWORD s_lastLogTick[2];
unsigned s_dumps;

void ReleaseTarget(Target& t) {
    if (t.surf) t.surf->Release();
    if (t.tex) t.tex->Release();
    memset(&t, 0, sizeof(t));
}

void ReleaseGpu(bool keepDevice) {
    ReleaseTarget(s_gpu.scene);
    ReleaseTarget(s_gpu.mid);
    ReleaseTarget(s_gpu.half[0]);
    ReleaseTarget(s_gpu.half[1]);
    ReleaseTarget(s_gpu.work);
    if (s_gpu.sb) s_gpu.sb->Release();
    s_gpu.sb = NULL;
    IDirect3DPixelShader9** ps[] = { &s_gpu.psCopy, &s_gpu.psBlur, &s_gpu.psOldMovie, &s_gpu.psRemanence,
                                     &s_gpu.psColor };
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); ++i) {
        if (*ps[i]) (*ps[i])->Release();
        *ps[i] = NULL;
    }
    if (!keepDevice) s_gpu.dev = NULL;
}

HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (dev == s_gpu.dev) ReleaseGpu(true);         // D3DPOOL_DEFAULT resources must be gone before Reset
    if (s_sceneLost) s_sceneLost();
    return s_origReset(dev, pp);
}

bool EnsureTarget(Target& t, UINT w, UINT h, D3DFORMAT fmt) {
    if (t.tex && t.w == w && t.h == h && t.fmt == fmt) return true;
    ReleaseTarget(t);
    if (FAILED(s_gpu.dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &t.tex, NULL))) {
        t.tex = NULL;
        return false;
    }
    if (FAILED(t.tex->GetSurfaceLevel(0, &t.surf))) {
        ReleaseTarget(t);
        return false;
    }
    t.w = w;
    t.h = h;
    t.fmt = fmt;
    return true;
}

IDirect3DPixelShader9* Compile(const char* src, const char* what) {
    HMODULE dx = GetModuleHandleA("d3dx9_37.dll");
    CompileFn compile = dx ? (CompileFn)GetProcAddress(dx, "D3DXCompileShader") : NULL;
    if (!compile) {
        AfxLog("d3dx9_37.dll D3DXCompileShader not found: after effects off");
        return NULL;
    }
    ID3DXBufferMin* code = NULL, *errors = NULL;
    HRESULT hr = compile(src, (UINT)strlen(src), NULL, NULL, "main", "ps_2_0", 0, &code, &errors, NULL);
    IDirect3DPixelShader9* ps = NULL;
    if (SUCCEEDED(hr) && code) {
        if (FAILED(s_gpu.dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps))) ps = NULL;
    } else {
        AfxLog("shader %s does not compile (hr %08lX): %s", what, (unsigned long)hr,
               errors ? (const char*)errors->GetBufferPointer() : "");
    }
    if (code) code->Release();
    if (errors) errors->Release();
    return ps;
}

bool EnsureGpu(IDirect3DDevice9* dev) {
    if (s_gpu.broken) return false;
    if (dev != s_gpu.dev) {
        if (s_gpu.dev) ReleaseGpu(false);
        s_gpu.dev = dev;
        if (!s_resetHooked) {                       // vtable slot 16: IDirect3DDevice9::Reset
            void** vt = *(void***)dev;
            s_origReset = (ResetFn)vt[16];
            void* hook = (void*)&ResetHook;
            if (WriteCode((uint32_t)(uintptr_t)&vt[16], &hook, 4)) s_resetHooked = true;
            else AfxLog("could not hook IDirect3DDevice9::Reset; device resets may fail while an effect is on");
        }
    }
    if (!s_gpu.sb && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &s_gpu.sb))) {
        s_gpu.sb = NULL;
        return false;
    }
    if (!s_gpu.psCopy) {
        s_gpu.psCopy = Compile(kPsCopy, "copy");
        s_gpu.psBlur = Compile(kPsBlur, "BigBlur");
        s_gpu.psOldMovie = Compile(kPsOldMovie, "OldMovie");
        s_gpu.psRemanence = Compile(kPsRemanence, "Remanence");
        s_gpu.psColor = Compile(kPsColorCorrection, "ColorCorrection");
        if (!s_gpu.psCopy || !s_gpu.psBlur || !s_gpu.psOldMovie || !s_gpu.psRemanence || !s_gpu.psColor) {
            s_gpu.broken = true;
            ReleaseGpu(true);
            return false;
        }
    }
    return true;
}

struct QuadV { float x, y, z, rhw, u, v; };

void DrawQuad(IDirect3DDevice9* dev, float x, float y, float w, float h) {
    QuadV q[4] = {
        { x - 0.5f, y - 0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
        { x + w - 0.5f, y - 0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
        { x - 0.5f, y + h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
        { x + w - 0.5f, y + h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadV));
}

void SetSampler(IDirect3DDevice9* dev, DWORD s, IDirect3DBaseTexture9* tex, bool linear) {
    dev->SetTexture(s, tex);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
}

// Render states every pass uses; the state block captured before the effects puts the engine's back.
void BaseStates(IDirect3DDevice9* dev) {
    dev->SetVertexShader(NULL);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    for (DWORD s = 2; s < 8; ++s) dev->SetTexture(s, NULL);
}

void PassInto(IDirect3DDevice9* dev, Target& dst, IDirect3DPixelShader9* ps) {
    dev->SetRenderTarget(0, dst.surf);
    D3DVIEWPORT9 vp = { 0, 0, dst.w, dst.h, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    dev->SetPixelShader(ps);
    DrawQuad(dev, 0.0f, 0.0f, (float)dst.w, (float)dst.h);
}

// AFX_S::BigBlur over the frame copy: returns the half-size buffer holding the result (NULL: no blur)
Target* BigBlur(IDirect3DDevice9* dev, float factor) {
    float offs[8];
    int n = BlurPasses(factor, offs, 8);
    if (n < 0) return NULL;
    if (n > 8) n = 8;
    // the 2x box copy of the Wii's 640x480 image: halve the PC frame down to at most 640x480, then to 320x240
    Target* src = &s_gpu.scene;
    RECT all = { 0, 0, (LONG)src->w, (LONG)src->h };
    if (src->w > MID_W || src->h > MID_H) {
        if (!EnsureTarget(s_gpu.mid, MID_W, MID_H, s_gpu.scene.fmt)) return NULL;
        dev->StretchRect(src->surf, &all, s_gpu.mid.surf, NULL, D3DTEXF_LINEAR);
        src = &s_gpu.mid;
    }
    if (!EnsureTarget(s_gpu.half[0], HALF_W, HALF_H, s_gpu.scene.fmt) ||
        !EnsureTarget(s_gpu.half[1], HALF_W, HALF_H, s_gpu.scene.fmt))
        return NULL;
    SetSampler(dev, 0, src->tex, true);                 // 640x480 -> 320x240: bilinear taps between the 2x2 texels
    PassInto(dev, s_gpu.half[0], s_gpu.psCopy);
    int cur = 0;
    for (int i = 0; i < n; ++i) {
        float c[4] = { offs[i], 0.5f, 0.375f, 0.25f };
        dev->SetPixelShaderConstantF(0, c, 1);
        SetSampler(dev, 0, s_gpu.half[cur].tex, true);
        PassInto(dev, s_gpu.half[1 - cur], s_gpu.psBlur);
        cur = 1 - cur;
    }
    SetSampler(dev, 0, NULL, true);
    return &s_gpu.half[cur];
}

// AFX_S_OldMovie::Apply: BigBlur leaves the blur stretched (bilinear) over the 640x480 EFB, the effect copies the EFB
// back at half size with the 2x box filter (FB_CopyEFB mode 2) and the composite samples that copy bilinearly
Target* HalfCopyOfStretchedBlur(IDirect3DDevice9* dev, Target* b) {
    if (!EnsureTarget(s_gpu.mid, MID_W, MID_H, s_gpu.scene.fmt)) return b;
    SetSampler(dev, 0, b->tex, true);
    PassInto(dev, s_gpu.mid, s_gpu.psCopy);
    Target* out = b == &s_gpu.half[0] ? &s_gpu.half[1] : &s_gpu.half[0];
    SetSampler(dev, 0, s_gpu.mid.tex, true);
    PassInto(dev, *out, s_gpu.psCopy);
    SetSampler(dev, 0, NULL, true);
    return out;
}

void Dump(const Target& t, const char* name) {
    HMODULE dx = GetModuleHandleA("d3dx9_37.dll");
    SaveTexFn save = dx ? (SaveTexFn)GetProcAddress(dx, "D3DXSaveTextureToFileA") : NULL;
    if (!save || !t.tex) return;
    char path[MAX_PATH];
    _snprintf(path, sizeof(path) - 1, "%safx_%02u_%s.png", g_dllDir.c_str(), s_dumps, name);
    path[sizeof(path) - 1] = 0;
    save(path, 3 /* D3DXIFF_PNG */, t.tex, NULL);
}

// Final pass over the engine's target: the shader reads the frame copy (s0) and the blur (s1)
void Composite(IDirect3DDevice9* dev, IDirect3DSurface9* rt, const D3DVIEWPORT9& vp, IDirect3DPixelShader9* ps,
               Target* blur) {
    dev->SetRenderTarget(0, rt);
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                D3DCOLORWRITEENABLE_BLUE);
    SetSampler(dev, 0, s_gpu.scene.tex, false);
    SetSampler(dev, 1, blur ? blur->tex : NULL, true);
    dev->SetPixelShader(ps);
    DrawQuad(dev, (float)vp.X, (float)vp.Y, (float)vp.Width, (float)vp.Height);
    SetSampler(dev, 0, NULL, false);
    SetSampler(dev, 1, NULL, true);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
}

// Copy the engine's target (viewport) into the frame copy
bool CopyFrame(IDirect3DDevice9* dev, IDirect3DSurface9* rt, const D3DVIEWPORT9& vp, D3DFORMAT fmt) {
    if (!EnsureTarget(s_gpu.scene, vp.Width, vp.Height, fmt)) return false;
    RECT r = { (LONG)vp.X, (LONG)vp.Y, (LONG)(vp.X + vp.Width), (LONG)(vp.Y + vp.Height) };
    return SUCCEEDED(dev->StretchRect(rt, &r, s_gpu.scene.surf, NULL, D3DTEXF_NONE));
}

void DrawOldMovie(IDirect3DDevice9* dev, IDirect3DSurface9* rt, const D3DVIEWPORT9& vp, const Params& p) {
    float c0[4], c1[4], c2[4];
    if (!OldMovieConstants(p, c0, c1, c2)) return;
    Target* b = (p.remanence > 0.0f && p.blur > 0.0f) ? BigBlur(dev, p.blur) : NULL;
    if (b) b = HalfCopyOfStretchedBlur(dev, b);
    c1[3] = b ? 1.0f : 0.0f;
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetPixelShaderConstantF(2, c2, 1);
    Composite(dev, rt, vp, s_gpu.psOldMovie, b);
}

void DrawRemanence(IDirect3DDevice9* dev, IDirect3DSurface9* rt, const D3DVIEWPORT9& vp, const Params& p) {
    Target* b = BigBlur(dev, p.bigBlur);
    int k = KByte(p.glow);
    int a = (k * (k + (k >> 7))) >> 8;                // TEV alpha: KONST * KONST
    float c0[4] = { (float)a / 255.0f, 0, 0, 0 };
    if (!b) b = &s_gpu.scene;                         // no blur: the frame itself
    dev->SetPixelShaderConstantF(0, c0, 1);
    Composite(dev, rt, vp, s_gpu.psRemanence, b);
}

void DrawBigBlur(IDirect3DDevice9* dev, IDirect3DSurface9* rt, const D3DVIEWPORT9& vp, const Params& p) {
    Target* b = BigBlur(dev, p.bigBlur);
    if (!b) return;
    dev->SetRenderTarget(0, rt);
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                D3DCOLORWRITEENABLE_BLUE);
    SetSampler(dev, 0, b->tex, true);
    dev->SetPixelShader(s_gpu.psCopy);
    DrawQuad(dev, (float)vp.X, (float)vp.Y, (float)vp.Width, (float)vp.Height);
    SetSampler(dev, 0, NULL, true);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
}

void DrawColorCorrection(IDirect3DDevice9* dev, IDirect3DSurface9* rt, const D3DVIEWPORT9& vp, const Params& p) {
    float bri = p.bri * p.fBri, con = p.con * p.fCon, sat = p.sat * p.fSat;
    float c0[4] = { 0, 0, 0, 1 }, c1[4] = { 0, 0, 0, 0 }, c2[4] = { 0, 0, 0, 0 };
    bool any = false;
    if (p.addCoef != 0.0f) {
        float add[3];
        for (int i = 0; i < 3; ++i) {
            add[i] = p.colorAdd[i] * p.addCoef;
            if (add[i] > 1.0f) add[i] = 1.0f;
        }
        if (add[0] < 0.0f || add[1] < 0.0f || add[2] < 0.0f) {
            float m = add[0];
            if (add[1] < m) m = add[1];
            if (add[2] < m) m = add[2];
            for (int i = 0; i < 3; ++i) add[i] -= m;
        }
        for (int i = 0; i < 3; ++i) c0[i] = (float)(KByte(add[i])) / 255.0f;
        int alpha = p.addCoef < 1.0f ? KByte(p.addCoef) : 255;
        c0[3] = 1.0f - (float)alpha / 255.0f;
        c1[0] = 1;
        any = true;
    }
    if (sat != 1.0f) {
        if (sat - 1.0f < 0.0f) c1[1] = (float)KByte(1.0f - sat) / 255.0f;
        else c2[3] = KWeight(KByte(sat - 1.0f));
        any = true;
    }
    if (con != 1.0f) {
        float d = con - 1.0f;
        if (d > 0.0f) {
            c1[2] = 1;
            unsigned k1 = (unsigned)(255.0f * d);
            c2[0] = (float)(k1 > 255 ? 255 : k1) / 255.0f;
            d -= 1.0f;
            if (d > 2.0f) d = 2.0f;
            if (d > 0.0f) {
                unsigned k2 = (unsigned)(255.0f * d);
                c2[1] = (float)(k2 > 255 ? 255 : k2) / 255.0f;
            }
        } else {
            c1[2] = 2;
            float e = -d > 1.0f ? 1.0f : -d;
            c2[0] = (float)KByte(e) / 255.0f;
        }
        any = true;
    }
    if (bri != 1.0f) {
        float d = bri - 1.0f;
        bool sub = d < 0.0f;
        if (sub) d = -d;
        if (d > 1.0f) d = 1.0f;
        c1[3] = sub ? 2.0f : 1.0f;
        c2[2] = (float)KByte(d) / 255.0f;
        any = true;
    }
    if (!any) return;
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetPixelShaderConstantF(2, c2, 1);
    Composite(dev, rt, vp, s_gpu.psColor, NULL);
}

// ---------------------------------------------------------------------------------------------------------------
// reading the engine
// ---------------------------------------------------------------------------------------------------------------
int ReadVarList(uint32_t list, uint32_t count, Var* out, int max) {
    if (!list || count > (uint32_t)max) return 0;
    int n = 0;
    __try {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = (const uint8_t*)(uintptr_t)(list + i * 24);
            const char* name = *(const char**)e;
            int type = e[5];
            uint16_t size = (uint16_t)(e[8] | (e[9] << 8));
            const float* data = *(const float**)(e + 12);
            if (!name || !data) continue;
            if (type == VT_FLOAT && size < 4) continue;
            if (type == VT_VEC3 && size < 12) continue;
            volatile char probe = name[0];
            (void)probe;
            volatile float fp = data[0];
            (void)fp;
            out[n].name = name;
            out[n].type = type;
            out[n].data = data;
            out[n].index = (int)i;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

// Identity of an instance's effect: FX type and variable lists (0: unreadable or not an effect)
uint32_t InstanceKey(uint32_t inst, uint32_t* modelVars) {
    uint32_t v[9] = { 0 };
    __try {
        memcpy(v, (void*)(uintptr_t)inst, sizeof(v));   // model, tech, fx type, buffer, version, lists
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    uint32_t type = v[I_FXTYPE / 4];
    if (type == 0xFFFFFFFF || type > 12) return 0;
    if (modelVars) *modelVars = v[I_MODELVARN / 4];
    uint32_t key = Crc32((const uint8_t*)&v[I_FXTYPE / 4], sizeof(uint32_t)) ^
                   Crc32((const uint8_t*)&v[I_MODELVARS / 4], 4 * sizeof(uint32_t));
    return key ? key : 1;
}

// AFX_b_Build: defaults, the model variables (bound), then the saved variables
bool BuildParams(uint32_t inst, Params& p, signed char* bind) {
    __try {
        uint32_t type = *(uint32_t*)(uintptr_t)(inst + I_FXTYPE);
        if (type == 0) type = FX_OLDMOVIE;              // the loaders' default
        if (type == 0xFFFFFFFF || type > 12) return false;
        Defaults((int)type, p);
        memset(bind, F_NONE, MAX_VARS);
        Var vars[MAX_VARS];
        int n = ReadVarList(*(uint32_t*)(uintptr_t)(inst + I_MODELVARS), *(uint32_t*)(uintptr_t)(inst + I_MODELVARN),
                            vars, MAX_VARS);
        ApplyVars(p, vars, n, bind);
        n = ReadVarList(*(uint32_t*)(uintptr_t)(inst + I_SAVEVARS), *(uint32_t*)(uintptr_t)(inst + I_SAVEVARN), vars,
                        MAX_VARS);
        ApplyVars(p, vars, n, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// built effects (the Wii's AFX_S objects): parameters kept per instance, so the variable natives can change them
// ---------------------------------------------------------------------------------------------------------------
enum { MAX_STATES = 32 };

struct InstState {
    uint32_t inst;                               // K3D_HLSL_Instance*; 0 = free
    uint32_t key;                                // InstanceKey when built
    uint32_t used;                               // s_useClock when last drawn or written
    uint32_t nModel;                             // model variables: the variable natives' index range
    Params p;
    signed char bind[MAX_VARS];                  // model variable -> Field
    DWORD logged[MAX_VARS];                      // tick of the last logged write, per variable
};

InstState s_states[MAX_STATES];
uint32_t s_useClock;

// The built effect of an instance. The Wii builds an effect the first time AFX_User draws it; before that, and
// after the instance is destroyed, a variable native changes nothing (create = false).
InstState* StateOf(uint32_t inst, bool create) {
    if (!inst) return NULL;
    uint32_t nModel = 0, key = InstanceKey(inst, &nModel);
    if (!key) return NULL;
    InstState* slot = NULL;
    for (int i = 0; i < MAX_STATES; ++i) {
        InstState& s = s_states[i];
        if (s.inst == inst) {
            if (s.key == key) {
                s.used = ++s_useClock;
                return &s;
            }
            s.inst = 0;                          // another effect at the same address
            slot = &s;
            break;
        }
    }
    if (!create) return NULL;
    for (int i = 0; !slot && i < MAX_STATES; ++i)
        if (!s_states[i].inst) slot = &s_states[i];
    if (!slot) {                                 // all in use: the least recently used goes
        slot = &s_states[0];
        for (int i = 1; i < MAX_STATES; ++i)
            if (s_states[i].used < slot->used) slot = &s_states[i];
    }
    memset(slot, 0, sizeof(*slot));
    if (!BuildParams(inst, slot->p, slot->bind)) return NULL;
    slot->inst = inst;
    slot->key = key;
    slot->nModel = nModel;
    slot->used = ++s_useClock;
    return slot;
}

void DropState(uint32_t inst) {
    for (int i = 0; i < MAX_STATES; ++i)
        if (s_states[i].inst == inst) s_states[i].inst = 0;
}

const char* TypeName(int type) {
    switch (type) {
    case FX_OLDMOVIE: return "OldMovie";
    case FX_COLORCORRECTION: return "ColorCorrection";
    case FX_REMANENCE: return "Remanence";
    case FX_BIGBLUR: return "BigBlur";
    case FX_DEPTHBLUR: return "DepthBlur";
    }
    return "effect";
}

const char* VarName(uint32_t inst, uint32_t index) {
    static char name[64];
    name[0] = 0;
    __try {
        uint32_t list = *(uint32_t*)(uintptr_t)(inst + I_MODELVARS);
        const char* s = *(const char**)(uintptr_t)(list + index * 24);
        if (s) {
            strncpy(name, s, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        name[0] = 0;
    }
    return name;
}

// The effect instance of an AFX modifier (0: none)
uint32_t InstanceOf(void* afx) {
    __try {
        return *(uint32_t*)((uint8_t*)afx + AFX_INSTANCE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

typedef void (__thiscall* IfvSetfFn)(void*, uint32_t, float);
typedef void (__thiscall* IfvSetvFn)(void*, uint32_t, const float*);
typedef float (__thiscall* IfvGetfFn)(void*, uint32_t);
typedef const float* (__thiscall* IfvGetvFn)(void*, uint32_t);
typedef void (__thiscall* InstDestroyFn)(void*);

IfvSetfFn s_origSetf;
IfvSetvFn s_origSetv;
bool s_varsInstalled;

// The field a variable native reaches on the Wii: the bound field of a built effect (NULL: none)
float* BoundField(uint32_t inst, uint32_t index, InstState** st) {
    InstState* s = StateOf(inst, false);
    if (st) *st = s;
    if (!s || index >= s->nModel || index >= MAX_VARS) return NULL;
    return FieldPtr(s->p, s->bind[index]);
}

void LogWrite(InstState* s, uint32_t index, const char* native, const float* v, int n) {
    if (!s_log || !s || index >= MAX_VARS) return;
    DWORD now = GetTickCount();
    if (s->logged[index] && now - s->logged[index] < 2000) return;
    s->logged[index] = now ? now : 1;
    if (n == 3)
        AfxLog("%s: %s variable %u (%s) = (%.3f %.3f %.3f)", native, TypeName(s->p.type), index,
               VarName(s->inst, index), v[0], v[1], v[2]);
    else
        AfxLog("%s: %s variable %u (%s) = %.3f", native, TypeName(s->p.type), index, VarName(s->inst, index), v[0]);
}

// AFX::IFVSetf (MDF_Setf). The PC stores the value in its instance's variable buffer, which nothing draws; the Wii
// stores it through the variable's field pointer into the built effect.
void __fastcall AfxIfvSetf(void* afx, void* /*edx*/, uint32_t index, float value) {
    s_origSetf(afx, index, value);
    InstState* s;
    float* f = BoundField(InstanceOf(afx), index, &s);
    if (!f) return;
    bool changed = f[0] != value;
    f[0] = value;
    if (changed) LogWrite(s, index, "MDF_Setf", f, 1);
}

// AFX::IFVSetv (MDF_Setv): three floats through the field pointer
void __fastcall AfxIfvSetv(void* afx, void* /*edx*/, uint32_t index, const float* value) {
    s_origSetv(afx, index, value);
    InstState* s;
    float* f = BoundField(InstanceOf(afx), index, &s);
    if (!f || !value) return;
    int n = FieldSize(s->bind[index]);           // a float field takes the vector's first component
    bool changed;
    __try {
        changed = memcmp(f, value, n * sizeof(float)) != 0;
        memcpy(f, value, n * sizeof(float));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (changed) LogWrite(s, index, "MDF_Setv", f, n);
}

// AFX::IFVGetf (MDF_Getf): the Wii reads the field; 0 without one
float __fastcall AfxIfvGetf(void* afx, void* /*edx*/, uint32_t index) {
    float* f = BoundField(InstanceOf(afx), index, NULL);
    return f ? f[0] : 0.0f;
}

// AFX::IFVGetv (MDF_Getv): the field (the Wii returns NULL without one, which the native reads through; a zero
// vector here, as a float field's following values are not kept)
const float* __fastcall AfxIfvGetv(void* afx, void* /*edx*/, uint32_t index) {
    static const float zero[4] = { 0, 0, 0, 0 };
    InstState* s;
    float* f = BoundField(InstanceOf(afx), index, &s);
    if (!f) return zero;
    if (FieldSize(s->bind[index]) == 3) return f;
    static float vec[3];
    vec[0] = f[0];
    vec[1] = vec[2] = 0.0f;
    return vec;
}

// K3D_HLSL_Instance::Destroy at its two call sites (AFX::Destroy, EVE::AFXEvent_Destroy): the built effect goes too
void __fastcall InstanceDestroyHook(void* inst) {
    DropState((uint32_t)(uintptr_t)inst);
    ((InstDestroyFn)(uintptr_t)INST_DESTROY)(inst);
}

// Whether an effect changes the image (the Wii also copies and blurs for effects that end up as the identity)
bool Draws(const Params& p) {
    switch (p.type) {
    case FX_OLDMOVIE: {
        float c0[4], c1[4], c2[4];
        return OldMovieConstants(p, c0, c1, c2);
    }
    case FX_COLORCORRECTION:
        return p.addCoef != 0.0f || p.sat * p.fSat != 1.0f || p.con * p.fCon != 1.0f || p.bri * p.fBri != 1.0f;
    case FX_REMANENCE: {
        int k = KByte(p.glow);
        return ((k * (k + (k >> 7))) >> 8) != 0;
    }
    case FX_BIGBLUR:
        return BlurPasses(p.bigBlur, NULL, 0) >= 0;
    }
    return false;
}

uint32_t Sig(const Params& p) {
    return Crc32((const uint8_t*)&p, sizeof(p));
}

void Describe(const Params& p, char* out, size_t n) {
    switch (p.type) {
    case FX_OLDMOVIE:
        _snprintf(out, n, "OldMovie saturation %.3f contrast %.3f brightness %.3f remanence %.3f blur %.5f tint "
                  "(%.2f %.2f %.2f)", p.centerSat, p.centerCon, p.centerBri, p.remanence, p.blur, p.centerColor[0],
                  p.centerColor[1], p.centerColor[2]);
        break;
    case FX_COLORCORRECTION:
        _snprintf(out, n, "ColorCorrection brightness %.3f saturation %.3f contrast %.3f add (%.2f %.2f %.2f) x %.3f",
                  p.bri * p.fBri, p.sat * p.fSat, p.con * p.fCon, p.colorAdd[0], p.colorAdd[1], p.colorAdd[2],
                  p.addCoef);
        break;
    case FX_REMANENCE:
        _snprintf(out, n, "Remanence glow %.3f blur %.5f", p.glow, p.bigBlur);
        break;
    case FX_BIGBLUR:
        _snprintf(out, n, "BigBlur %.5f", p.bigBlur);
        break;
    case FX_DEPTHBLUR:
        _snprintf(out, n, "DepthBlur z %.4f..%.4f blur %.5f (not drawn)", p.zStart, p.zEnd, p.bigBlurEnd);
        break;
    default:
        _snprintf(out, n, "type %d (not drawn)", p.type);
        break;
    }
    out[n - 1] = 0;
}

void __fastcall AfxUserHook(void* display, void* /*edx*/, uint32_t mode) {
    if (!display) return;
    uint8_t* disp = (uint8_t*)display;
    if (*(uint32_t*)(disp + D_DRAWGLOBAL) & 0x400000) return;
    uint8_t* gd = *(uint8_t**)(uintptr_t)G_DISPLAY;
    if (gd && (*(uint32_t*)(gd + D_DRAWFLAGS) & 0x100000)) return;
    if (mode == 0 && s_sceneDraw) {                  // the 3D image is complete: what draws into it goes here
        IDirect3DDevice9* dev = GfxDevice();
        if (dev) s_sceneDraw(dev, *(uint32_t*)(disp + D_VIEWNUM));
    }
    if (!s_userOn || !s_effectsOn) return;           // G_AFX_ON is not checked: the Wii's switch is always on
    uint32_t num = *(uint32_t*)(disp + D_AFX_NUM);
    if (!num) return;
    if (num > 8) num = 8;
    uint32_t view = *(uint32_t*)(disp + D_VIEWNUM);

    Params list[8];
    uint32_t types = 0x9E3779B9u;
    int count = 0;
    for (uint32_t i = 0; i < num; ++i) {
        uint8_t* e = disp + D_AFX_TABLE + i * 8;
        uint16_t emode = (uint16_t)(e[0] | (e[1] << 8));
        uint8_t vps = e[2];
        uint32_t inst = *(uint32_t*)(e + 4);
        if (emode != mode || !inst || view > 7 || !(vps & (1u << view))) continue;
        InstState* s = StateOf(inst, true);      // built the first time it is drawn, like AFX_b_Build
        if (!s) continue;
        list[count++] = s->p;
        types = types * 31u + inst + (uint32_t)s->p.type;
    }
    if (!count) return;

    // log (and dump) a new set of effects at once, changed values (a fade) at most once a second
    uint32_t sig = 0x9E3779B9u ^ (uint32_t)count;
    for (int i = 0; i < count; ++i) sig = sig * 31u + Sig(list[i]);
    DWORD now = GetTickCount();
    bool changed = mode < 2 && sig != s_lastSig[mode] &&
                   (types != s_lastTypes[mode] || now - s_lastLogTick[mode] >= 1000);
    if (changed) {
        s_lastSig[mode] = sig;
        s_lastTypes[mode] = types;
        s_lastLogTick[mode] = now;
        static bool s_switchLogged;
        if (s_log && !s_switchLogged) {
            s_switchLogged = true;
            AfxLog("first effect: the PC switch (%08X) is %u, not checked (the Wii's is always on)", G_AFX_ON,
                   *(uint32_t*)(uintptr_t)G_AFX_ON);
        }
        if (s_log) {
            for (int i = 0; i < count; ++i) {
                char d[300];
                Describe(list[i], d, sizeof(d));
                AfxLog("mode %u effect %d/%d: %s", mode, i + 1, count, d);
            }
        }
    }

    uint32_t drv = *(uint32_t*)(uintptr_t)G_DRIVER;
    IDirect3DDevice9* dev = drv ? *(IDirect3DDevice9**)(uintptr_t)(drv + DRV_DEVICE) : NULL;
    if (!dev || !EnsureGpu(dev)) return;

    IDirect3DSurface9* rt = NULL;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC desc;
    D3DVIEWPORT9 vp;
    if (FAILED(rt->GetDesc(&desc)) || FAILED(dev->GetViewport(&vp)) || !vp.Width || !vp.Height ||
        vp.X + vp.Width > desc.Width || vp.Y + vp.Height > desc.Height) {
        rt->Release();
        return;
    }
    s_gpu.sb->Capture();
    BaseStates(dev);
    bool dump = changed && s_dumps < s_dumpMax, drawn = false;
    for (int i = 0; i < count; ++i) {
        const Params& p = list[i];
        if (!Draws(p)) continue;
        if (!CopyFrame(dev, rt, vp, desc.Format)) break;
        if (dump && !drawn) Dump(s_gpu.scene, "before");
        drawn = true;
        switch (p.type) {
        case FX_OLDMOVIE: DrawOldMovie(dev, rt, vp, p); break;
        case FX_REMANENCE: DrawRemanence(dev, rt, vp, p); break;
        case FX_COLORCORRECTION: DrawColorCorrection(dev, rt, vp, p); break;
        case FX_BIGBLUR: DrawBigBlur(dev, rt, vp, p); break;
        }
    }
    if (dump && drawn && CopyFrame(dev, rt, vp, desc.Format)) {
        Dump(s_gpu.scene, "after");
        ++s_dumps;
    }
    dev->SetRenderTarget(0, rt);
    s_gpu.sb->Apply();
    dev->SetViewport(&vp);
    rt->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// verification and installation
// ---------------------------------------------------------------------------------------------------------------
int AfxVerify(const Image& img, std::string& rep) {
    int bad = 0;
    for (size_t i = 0; i < sizeof(kCode) / sizeof(kCode[0]); ++i) {
        const Code& c = kCode[i];
        uint32_t got = 0;
        bool ok = CheckCrc(img, c.va, c.len, c.crc, &got);
        Report(rep, ok, "%-34s %08X len 0x%03X crc %08X (want %08X)", c.what, c.va, c.len, got, c.crc);
        if (!ok) ++bad;
    }
    for (size_t i = 0; i < sizeof(kCalls) / sizeof(kCalls[0]); ++i) {
        const CallSite& s = kCalls[i];
        uint8_t b[8] = { 0 };
        bool ok = img.Read(s.va, b, 8);
        if (s.target) {
            int32_t rel = 0;
            memcpy(&rel, b + 1, 4);
            ok = ok && b[0] == 0xE8 && s.va + 5 + (uint32_t)rel == s.target;
            Report(rep, ok, "%-34s call at %08X -> %08X", s.what, s.va, s.target);
        } else {                                     // 89 84 D1 48 84 06 00: mov [ecx+edx*8+0x68448], eax
            static const uint8_t want[7] = { 0x89, 0x84, 0xD1, 0x48, 0x84, 0x06, 0x00 };
            ok = ok && memcmp(b, want, 7) == 0;
            Report(rep, ok, "%-34s table write at %08X", s.what, s.va);
        }
        if (!ok) ++bad;
    }
    for (size_t i = 0; i < sizeof(kSlots) / sizeof(kSlots[0]); ++i) {
        const VtSlot& v = kSlots[i];
        uint32_t got = 0;
        bool ok = img.Read(AFX_VTABLE + v.slot * 4, &got, 4) && got == v.target;
        Report(rep, ok, "%-34s %08X -> %08X", v.what, AFX_VTABLE + v.slot * 4, v.target);
        if (!ok) ++bad;
    }
    return bad;
}

// The variable natives on model-less effects (vtable slots) and the instance destruction (call sites)
bool InstallVariables() {
    void* hooks[] = { (void*)&AfxIfvSetf, (void*)&AfxIfvSetv, (void*)&AfxIfvGetf, (void*)&AfxIfvGetv };
    s_origSetf = (IfvSetfFn)(uintptr_t)kSlots[0].target;
    s_origSetv = (IfvSetvFn)(uintptr_t)kSlots[1].target;
    const uint32_t sites[] = { 0x00567ACE, 0x005E15FA };
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {      // first, so no state outlives an instance
        uint8_t call[5] = { 0xE8 };
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)&InstanceDestroyHook - (sites[i] + 5));
        memcpy(call + 1, &rel, 4);
        if (!WriteCode(sites[i], call, 5)) return false;
    }
    for (size_t i = 0; i < sizeof(kSlots) / sizeof(kSlots[0]); ++i)
        if (!WriteCode(AFX_VTABLE + kSlots[i].slot * 4, &hooks[i], 4)) return false;
    return true;
}

bool IsOn(const std::string& v) {
    return _stricmp(v.c_str(), "1") == 0 || _stricmp(v.c_str(), "true") == 0 || _stricmp(v.c_str(), "yes") == 0 ||
           _stricmp(v.c_str(), "on") == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// self-test (wmtest afx)
// ---------------------------------------------------------------------------------------------------------------
#define AFX_EXPECT(cond, ...) do { bool ok_ = (cond); Report(rep, ok_, __VA_ARGS__); if (!ok_) ++fails; } while (0)

inline bool Near(float a, float b) { return fabs(a - b) < 1e-5f; }

int RunAfxSelfTest(std::string& rep) {
    int fails = 0;
    s_testLog = &rep;

    // record 19009146 (a level of package FFF06058): model variables, then saved ones
    float blur = 0.0085f, rem = 0.1f, sat = 0.9f, con = 1.42f, bri = 0.95f, circle = 0.5f, zero3[3] = { 0, 0, 0 };
    Var model[] = { { "BlurFactor", VT_FLOAT, &blur, 0 }, { "RemanenceFactor", VT_FLOAT, &rem, 1 },
                    { "vColor", VT_VEC3, zero3, 2 }, { "Saturation", VT_FLOAT, &sat, 3 },
                    { "Contrast", VT_FLOAT, &con, 4 }, { "Brightness", VT_FLOAT, &bri, 5 } };
    float sat2 = 0.95f, con2 = 1.2f, width1 = 0.001f, width2 = 0.71f;
    Var saved[] = { { "ColorCenterWidth", VT_FLOAT, &width1, 0 }, { "CircleRatio", VT_FLOAT, &circle, 1 },
                    { "ClearCenterWidth", VT_FLOAT, &width2, 2 }, { "Contrast", VT_FLOAT, &con2, 3 },
                    { "Saturation", VT_FLOAT, &sat2, 4 } };
    Params p;
    Defaults(FX_OLDMOVIE, p);
    AFX_EXPECT(Near(p.centerSat, 0.5f) && Near(p.blur, 0.005f) && Near(p.remanence, 0.1f) && Near(p.centerCon, 1.0f),
               "OldMovie defaults: saturation 0.5, blur 0.005, remanence 0.1, contrast 1");
    signed char bind[MAX_VARS];
    memset(bind, F_NONE, sizeof(bind));
    ApplyVars(p, model, 6, bind);
    AFX_EXPECT(Near(p.blur, 0.003125f) && Near(p.centerSat, 0.9f) && Near(p.centerCon, 1.42f) &&
               Near(p.centerBri, 0.95f) && Near(p.centerColor[0], 0.0f) && Near(p.centerColor[1], 0.0f),
               "model variables: BlurFactor clamped to 0.003125, saturation 0.9, contrast 1.42, brightness 0.95, "
               "vColor -> centre colour 0");
    ApplyVars(p, saved, 5, NULL);
    AFX_EXPECT(Near(p.centerWidth, 0.71f) && Near(p.circleRatio, 0.5f) && Near(p.centerCon, 1.2f) &&
               Near(p.centerSat, 0.95f),
               "saved variables override: substring match (ClearCenterWidth -> CenterWidth 0.71), contrast 1.2, "
               "saturation 0.95");
    AFX_EXPECT(bind[0] == F_BLUR && bind[1] == F_REMANENCE && bind[2] == F_CENTERCOLOR && bind[3] == F_SAT &&
               bind[4] == F_CON && bind[5] == F_BRI && bind[6] == F_NONE,
               "model variables bind their fields (0 BlurFactor .. 5 Brightness); saved variables do not");
    *FieldPtr(p, bind[4]) = 1.3f;                      // MDF_Setf(4, 0, 1.3) after the build
    AFX_EXPECT(Near(p.centerCon, 1.3f), "a variable write after the build replaces a saved value (contrast 1.3)");

    Defaults(FX_OLDMOVIE, p);                          // the A1 record: saturation 1.2, contrast 1.1, brightness 1.04
    p.centerSat = 1.2f; p.centerCon = 1.1f; p.centerBri = 1.04f; p.remanence = 0.3f; p.blur = 0.003125f;
    p.centerColor[0] = p.centerColor[1] = p.centerColor[2] = 0.0f;
    float k0[4], k1[4], k2[4];
    bool om = OldMovieConstants(p, k0, k1, k2);
    AFX_EXPECT(om && k1[0] == 2 && Near(k0[0], 51.0f / 256) && k1[1] == 1 && Near(k0[1], 25.0f / 256) && k1[2] == 1 &&
               Near(k0[2], 9.0f / 255) && Near(k0[3], 76.0f / 255) && k2[3] == 0,
               "OldMovie konsts: saturation k 51 (above 1), contrast k 25 (gain), brightness k 10 adds 9/255, "
               "remanence k 76, no tint");
    p.centerSat = p.centerCon = p.centerBri = 1.0f; p.remanence = 0.0f;
    AFX_EXPECT(!OldMovieConstants(p, k0, k1, k2), "OldMovie with only a blur factor leaves the image unchanged");
    p.centerColor[0] = 0.5f;
    AFX_EXPECT(OldMovieConstants(p, k0, k1, k2) && Near(k2[0], 12.0f / 255) && k2[1] == 0 && k2[3] == 1,
               "OldMovie tint: 0.1 * 0.5 -> konst 12 added");

    float glow = 0.55f, bb = 0.005f;
    Var r[] = { { "GlowFactor", VT_FLOAT, &glow, 0 }, { "BigBlurFactor", VT_FLOAT, &bb, 1 } };
    Defaults(FX_REMANENCE, p);
    ApplyVars(p, r, 2, NULL);
    AFX_EXPECT(Near(p.glow, 0.55f) && Near(p.bigBlur, 0.005f), "Remanence: glow 0.55, blur 0.005");
    int k = KByte(p.glow);
    int a = (k * (k + (k >> 7))) >> 8;
    AFX_EXPECT(k == 140 && a == 77, "Remanence TEV alpha: konst %d, alpha %d (0.55^2 in 8 bits)", k, a);

    float cbri = 0.97f, csat = 0.9f, ccon = 1.2f;
    Var cc[] = { { "fColorBrightness", VT_FLOAT, &cbri, 0 }, { "fColorSaturation", VT_FLOAT, &csat, 1 },
                 { "fColorContrasteTrans", VT_FLOAT, &ccon, 2 } };
    Defaults(FX_COLORCORRECTION, p);
    ApplyVars(p, cc, 3, NULL);
    AFX_EXPECT(Near(p.bri, 0.97f) && Near(p.sat, 0.9f) && Near(p.con, 1.2f) && Near(p.fCon, 1.0f),
               "ColorCorrection: fColorContrasteTrans matches colorcontrast; factors default to 1");

    // the global fade (GST_GlobalFade): a neutral ColorCorrection whose add colour and coefficient the script writes
    float one = 1.0f, none = 0.0f, black[3] = { 0, 0, 0 }, white[3] = { 1, 1, 1 };
    Var fade[] = { { "fColorBrightness", VT_FLOAT, &one, 0 }, { "fColorSaturation", VT_FLOAT, &one, 3 },
                   { "fColorContrast", VT_FLOAT, &one, 4 }, { "vColorAdd", VT_VEC3, black, 5 },
                   { "fColorAddCoef", VT_FLOAT, &none, 6 } };
    Defaults(FX_COLORCORRECTION, p);
    memset(bind, F_NONE, sizeof(bind));
    ApplyVars(p, fade, 5, bind);
    AFX_EXPECT(!Draws(p) && bind[3] == F_CC_SAT && bind[5] == F_CC_ADD && bind[6] == F_CC_ADDCOEF &&
               FieldSize(bind[5]) == 3, "fade effect: neutral until written; variables 3, 5, 6 bound");
    memcpy(FieldPtr(p, bind[5]), white, sizeof(white));
    *FieldPtr(p, bind[6]) = 0.5f;
    AFX_EXPECT(Draws(p) && Near(p.colorAdd[1], 1.0f) && Near(p.addCoef, 0.5f), "MDF_Setv(5) white, MDF_Setf(6) 0.5: "
               "the fade draws");
    *FieldPtr(p, bind[6]) = 0.0f;
    *FieldPtr(p, bind[3]) = 0.0f;
    AFX_EXPECT(Draws(p) && Near(p.sat, 0.0f), "MDF_Setf(3) 0: black and white draws");

    AFX_EXPECT(KByte(0.42f) == 107 && Near(KWeight(107), 107.0f / 256.0f) && KWeight(255) == 1.0f &&
               KByte(1.5f) == 126 && KByte(-0.025f) == 250,
               "konst bytes: 0.42 -> 107, 255 weighs 1, 1.5 wraps to 126, -0.025 wraps to 250 (fctiwz + stb)");

    float offs[8];
    int n1 = BlurPasses(0.003125f, offs, 8);
    AFX_EXPECT(n1 == 1 && Near(offs[0], 0.00625f), "BigBlur 0.003125: 1 pass at offset 0.00625");
    int n2 = BlurPasses(0.085f, offs, 8);
    AFX_EXPECT(n2 == 2 && Near(offs[0], 0.0125f) && Near(offs[1], 0.00625f),
               "BigBlur 0.085: offset clamped to 0.0125, passes 0.0125 and 0.00625");
    int n3 = BlurPasses(0.0056f, offs, 8);
    AFX_EXPECT(n3 == 2 && Near(offs[0], 0.0112f) && Near(offs[1], 0.0056f), "BigBlur 0.0056: passes 0.0112, 0.0056");
    AFX_EXPECT(BlurPasses(0.0f, offs, 8) == -1 && BlurPasses(0.0007f, offs, 8) == -1 &&
               BlurPasses(0.0012f, offs, 8) == 0,
               "BigBlur 0 and 0.0007: no blur; 0.0012: the half-size copy only");
    AFX_EXPECT(HasI("fColorContrasteTrans", "colorcontrast") && !HasI("Saturation", "saturationx") &&
               HasI("vBorderColor", "Color"), "case-insensitive substring match");

    s_testLog = NULL;
    return fails;
}

void CopyOut(const std::string& rep, char* out, int outSize) {
    if (!out || outSize <= 0) return;
    size_t n = rep.size() < (size_t)outSize - 1 ? rep.size() : (size_t)outSize - 1;
    memcpy(out, rep.c_str(), n);
    out[n] = 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------------------------------------------
bool AfxInstalled() { return s_enabled; }
void AfxSetSceneDraw(void (*draw)(IDirect3DDevice9*, uint32_t), void (*lost)()) { s_sceneDraw = draw; s_sceneLost = lost; }
bool AfxUserEnabled() { return s_userOn; }
bool AfxDefaultEnabled() { return true; }
void AfxSetUserEnabled(bool on) {
    if (on != s_userOn) AfxLog("Wii effects %s (Options screen)", on ? "on" : "off");
    s_userOn = on;
}

void AfxAttach(const std::string& iniPath) {
    s_inAttach = true;
    std::string v;
    bool on = true, generalLog = false;
    if (ReadIniKey(iniPath, "video", "afx", v)) on = IsOn(v);
    if (ReadIniKey(iniPath, "general", "log", v)) generalLog = IsOn(v);
    s_log = generalLog;
    if (ReadIniKey(iniPath, "video", "afx_dump", v)) {
        int n = atoi(v.c_str());
        s_dumpMax = n > 1 ? (unsigned)n : IsOn(v) ? 20u : 0u;
    }
    if (!on) {
        AfxLog("[video] afx=0: the after effects are not drawn (PC behaviour)");
        s_effectsOn = false;                         // the hook is still installed: the scene draw needs it
    }
    ProcessImage img;
    uint8_t probe[5];
    if (!img.Read(AFX_USER, probe, 5)) {
        AfxLog("host process is not the RGH PC executable (no code at %08X): after effects not installed", AFX_USER);
        s_inAttach = false;
        return;
    }
    std::string rep;
    int bad = AfxVerify(img, rep);
    if (bad) {
        AfxLog("the PC executable differs from the expected 2010 build in %d place(s): after effects not installed",
               bad);
        size_t pos = 0;
        while (pos < rep.size()) {
            size_t end = rep.find('\n', pos);
            std::string line = rep.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (line.find("FAIL") != std::string::npos) AfxLog("%s", line.c_str());
            pos = end == std::string::npos ? rep.size() : end + 1;
        }
        s_inAttach = false;
        return;
    }
    s_varsInstalled = InstallVariables();
    uint8_t jmp[5] = { 0xE9 };
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)&AfxUserHook - (AFX_USER + 5));
    memcpy(jmp + 1, &rel, 4);
    s_installed = WriteCode(AFX_USER, jmp, 5);
    s_enabled = s_installed && s_effectsOn;
    AfxLog("PC executable verified: K3D::AFX_User %s (effects drawn: OldMovie, Remanence, ColorCorrection, BigBlur); "
           "effect variables (MDF_Setf/Setv/Getf/Getv) %s", s_installed ? "replaced" : "could NOT be replaced",
           s_varsInstalled ? "kept like the Wii" : "NOT installed");
    s_inAttach = false;
}

void AfxAfterConfig() {
    for (size_t i = 0; i < s_pending.size(); ++i) Log("AFX: %s", s_pending[i].c_str());
    s_pending.clear();
}

extern "C" {

// wmtest only: verify the after effect patch sites against an exe file. Returns the number of differences.
int __cdecl WiimoteAfxCheckExe(const char* exePath, char* out, int outSize) {
    FileImage img;
    std::string rep;
    int bad = -1;
    if (!img.Load(exePath)) rep = "  FAIL  cannot read " + std::string(exePath ? exePath : "(null)") + "\n";
    else bad = AfxVerify(img, rep);
    CopyOut(rep, out, outSize);
    return bad;
}

// wmtest only: parameter mapping, konst quantisation and the BigBlur schedule. Returns the number of failures.
int __cdecl WiimoteAfxSelfTest(char* out, int outSize) {
    std::string rep;
    int fails = RunAfxSelfTest(rep);
    CopyOut(rep, out, outSize);
    return fails;
}

}  // extern "C"
