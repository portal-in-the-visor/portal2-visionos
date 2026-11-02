// Portal 2 VR camera hook: observation gate OBS_SOURCE_RENDERVIEW at the entry of CViewRender::RenderView in client.nro
// (Portal 2 1.0.0 client.nro build 6f1172dd16fa27eb697921364f8a78cb, +0x39977c; vtable slot 6 of CViewRender's vtable client+0x1281fb0, found through RTTI).
// RenderView(this x0, const CViewSetup &view x1, const CViewSetup &hudView x2, int clearFlags w3, int whatToDraw w4) copies view into this+0x234 first,
// so changing view's origin/angles here changes the rendered main view only; aim, movement and the server keep the game's own angles.
//
// CViewSetup offsets (Portal 2 Switch, unverified until DM_VR_VIEW_DUMP confirms them): origin float[3] at +0x70, angles float[3] (pitch, yaw, roll) at +0x7c.
// Switches:
//   DM_VR_VIEW_DUMP=1   log the first 5 views as floats (+0x00..+0xd0) to confirm the offsets
//   DM_VR_VIEW_TEST=<deg> sweep a synthetic head yaw of ±<deg> (4 s period) and pitch of ±<deg>/3, to see the hook move the camera on a Mac
//   DM_VR_STEREO=1      side-by-side stereo in the game's own back buffer: the gate calls RenderView itself for the left eye (left half), then lets the game's
//                       call render the right eye (right half). Each eye is a full RenderView, so portals recurse from that eye's origin (as portal2vr does on PC)
//   DM_VR_IPD=<units>   eye separation in Source units (default 2.76 = 64 mm at portal2vr's 43.2 units per metre)
#include "hle.h"
#include "vr_view.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_int g_tracked, g_recenter = 1;   // a tracked head pose is current / the next pose becomes the neutral one
static float g_ipd = 2.76f;               // eye separation, Source units (DM_VR_IPD, vr_view_set_ipd_m)
static float g_eye_tan_h, g_eye_tan_v;   // per-eye half-FOV tangents (vr_view_set_eye_tangents); 0 = eyes keep the game's vertical field of view

enum { VS_X = 0x00, VS_UX = 0x04, VS_Y = 0x08, VS_UY = 0x0c, VS_W = 0x10, VS_UW = 0x14, VS_H = 0x18, VS_UH = 0x1c,
       VS_FOV = 0x68, VS_FOVVM = 0x6c, VS_ORIGIN = 0x70, VS_ANGLES = 0x7c, VS_ASPECT = 0x98, VS_DUMP_END = 0x108,
       VS_OC_TOP = 0xf0, VS_OC_BOTTOM = 0xf4, VS_OC_LEFT = 0xf8, VS_OC_RIGHT = 0xfc, VS_FLAGS = 0x100,   // off-centre fractions and the flag bitfield (bit 0 m_bOffCenter)
       VS_COPY = 0x140 };   // portal2vr's PC CViewSetup is 0x104 bytes; copy a little more than that
typedef void (*render_view_fn)(void *self, void *view, void *hud, int clear_flags, int what_to_draw);
static _Thread_local int g_nested;   // inside our own left-eye RenderView call: its gate must pass through
static int g_stereo = -1;
static _Atomic uint64_t g_stereo_frames;
static float g_frust[2][4]; static atomic_int g_frust_on;   // per-eye off-centre frustum tangents (l, r, b, t)
static int g_oc_set;                                       // we set m_bOffCenter in a view: cleared again when the frustum goes off
void vr_view_set_eye_frustum(int eye, float l, float r, float b, float t) {
    if (eye < 0 || eye > 1) return;
    g_frust[eye][0] = l; g_frust[eye][1] = r; g_frust[eye][2] = b; g_frust[eye][3] = t;
    atomic_store(&g_frust_on, l > 0 && r > 0 && b > 0 && t > 0);
}
// One eye: half of the original viewport, same vertical field of view, origin moved along the view's right axis.
static void eye_setup(uint8_t *v, int eye, float m[3][3], int viewport, int fovfix) {
    int32_t x, w, ux, uw; memcpy(&x, v + VS_X, 4); memcpy(&w, v + VS_W, 4); memcpy(&ux, v + VS_UX, 4); memcpy(&uw, v + VS_UW, 4);
    int32_t h; memcpy(&h, v + VS_H, 4);
    float fov, aspect; memcpy(&fov, v + VS_FOV, 4); memcpy(&aspect, v + VS_ASPECT, 4);
    if (!(aspect > 0.1f)) aspect = h > 0 ? (float)w / (float)h : 1.7778f;
    const float d = (float)M_PI / 180.f;
    float tan_v = tanf(fov * d / 2) / aspect;   // fov is horizontal at the view's aspect
    int32_t hw = w / 2, huw = uw / 2; x += eye * hw; ux += eye * huw;
    float eye_aspect = h > 0 ? (float)hw / (float)h : aspect / 2;
    float eye_fov = 2 * atanf(tan_v * eye_aspect) / d;
    if (g_eye_tan_h > 0 && g_eye_tan_v > 0) { eye_aspect = g_eye_tan_h / g_eye_tan_v; eye_fov = 2 * atanf(g_eye_tan_h) / d; }   // the headset's per-eye field of view (vr_view_set_eye_tangents)
    if (viewport) { memcpy(v + VS_X, &x, 4); memcpy(v + VS_W, &hw, 4); memcpy(v + VS_UX, &ux, 4); memcpy(v + VS_UW, &huw, 4); }
    // The viewmodel (portal gun) gets the world's field of view too: left at the game's 54° (63.7° at 16:9) it was drawn magnified, "far too close to the eye" on the
    // headset, with a disparity that did not match the world (first off-centre run: the gun mostly in the right eye, its glow alone in the left)
    static int calls, eyevm = 1; if (calls++ % 60 == 0) { const char *e = getenv("DM_VR_EYE_FOVVM"); eyevm = !(e && !atoi(e)); }   // diagnostic: 0 keeps the game's viewmodel fov
    float keep_vm; memcpy(&keep_vm, v + VS_FOVVM, 4); float keep_fov, keep_asp; memcpy(&keep_fov, v + VS_FOV, 4); memcpy(&keep_asp, v + VS_ASPECT, 4);
    if (viewport && fovfix) { memcpy(v + VS_FOV, &eye_fov, 4); memcpy(v + VS_FOVVM, &eye_fov, 4); memcpy(v + VS_ASPECT, &eye_aspect, 4); }
    // Off-centre: the symmetric hull of this eye's frustum, cropped to the frustum itself with Source's fractions (0 = the hull's left / bottom edge, 1 = right / top).
    // Verified on the Mac: DM_VR_OFFCENTER=0,0.5,0,1 renders the left half of the view across the whole viewport, 0.25,0.75,0.25,0.75 the centre
    if (viewport && fovfix && atomic_load(&g_frust_on)) {
        const float *f = g_frust[eye ? 1 : 0];
        float H = fmaxf(f[0], f[1]), V = fmaxf(f[2], f[3]);
        float hf = 2 * atanf(H) / d, ha = H / V;
        float oc_l = (H - f[0]) / (2 * H), oc_r = (H + f[1]) / (2 * H), oc_b = (V - f[2]) / (2 * V), oc_t = (V + f[3]) / (2 * V);
        memcpy(v + VS_FOV, &hf, 4); memcpy(v + VS_FOVVM, &hf, 4); memcpy(v + VS_ASPECT, &ha, 4);
        if (!eyevm) memcpy(v + VS_FOVVM, &keep_vm, 4);
        memcpy(v + VS_OC_LEFT, &oc_l, 4); memcpy(v + VS_OC_RIGHT, &oc_r, 4); memcpy(v + VS_OC_BOTTOM, &oc_b, 4); memcpy(v + VS_OC_TOP, &oc_t, 4);
        v[VS_FLAGS] |= 1; g_oc_set = 1;
    }
    if (!eyevm) memcpy(v + VS_FOVVM, &keep_vm, 4);
    // diagnostic DM_VR_EYE_KEEP: bit 0 keeps the game's fov, bit 1 its aspect, bit 2 clears the off-centre flag (the glow hunt, README)
    { static int keep; if (calls % 60 == 1) { const char *e = getenv("DM_VR_EYE_KEEP"); keep = e ? atoi(e) : 0; }
      if (keep & 1) memcpy(v + VS_FOV, &keep_fov, 4);
      if (keep & 2) memcpy(v + VS_ASPECT, &keep_asp, 4);
      if (keep & 4) v[VS_FLAGS] &= (uint8_t)~1u; }
    float *org = (float *)(v + VS_ORIGIN), off = (eye ? 0.5f : -0.5f) * g_ipd;   // right axis = -left column
    for (int i = 0; i < 3; i++) org[i] -= m[i][1] * off;
}

static _Atomic uint64_t g_seq;               // even = stable; seqlock over g_head
static float g_head[6]; static atomic_int g_have;
void vr_view_set_head(float p, float y, float r, float x, float yy, float z) {
    atomic_fetch_add(&g_seq, 1); float v[6] = {p, y, r, x, yy, z}; memcpy(g_head, v, sizeof v); atomic_fetch_add(&g_seq, 1); atomic_store(&g_have, 1);
}
static void move_rot_set(float deg);
static float g_head_yaw_set;   // the head yaw last put into the game's view angles (engine_set_angles)
void vr_view_clear_head(void) { move_rot_set(0); atomic_store(&g_have, 0); atomic_store(&g_tracked, 0); atomic_store(&g_recenter, 1); g_head_yaw_set = 0; }
// Tracked head: a basis (forward, left, up) and a position in the neutral frame, Source axes; seqlock as g_head
static _Atomic uint64_t g_tseq; static float g_track[12]; static uint64_t g_track_id;
static pthread_mutex_t g_pose_mu = PTHREAD_MUTEX_INITIALIZER; static uint64_t g_pose_fifo[4], g_pose_last; static int g_pose_n;
static void pose_rendered(uint64_t id) {   // RenderView (main thread) → present (present thread): at most 4 frames in flight, the oldest dropped beyond that
    pthread_mutex_lock(&g_pose_mu); if (g_pose_n == 4) { memmove(g_pose_fifo, g_pose_fifo + 1, 3 * sizeof *g_pose_fifo); g_pose_n = 3; } g_pose_fifo[g_pose_n++] = id; pthread_mutex_unlock(&g_pose_mu);
}
int vr_view_frame_flat(void);
uint64_t vr_view_take_rendered_pose(void) {
    // A frame is flat (loading screen, pause menu) by time, not by an empty FIFO: one present more than RenderViews (an autosave or a level change) used to
    // take a later frame's pose and leave the next stereo frame poseless, shown flat for a frame
    if (vr_view_frame_flat()) return 0;
    pthread_mutex_lock(&g_pose_mu); if (g_pose_n) { g_pose_last = g_pose_fifo[0]; memmove(g_pose_fifo, g_pose_fifo + 1, (size_t)--g_pose_n * sizeof *g_pose_fifo); }
    uint64_t id = g_pose_last; pthread_mutex_unlock(&g_pose_mu); return id;
}
static _Atomic uint64_t g_last_stereo_ns; static atomic_int g_paused_now;
int vr_view_frame_flat(void) { return atomic_load(&g_paused_now) || now_ns() - atomic_load(&g_last_stereo_ns) > 200000000ull; }
static float g_ref_yaw, g_ref_pos[3];
static atomic_int g_force_stereo;
void vr_view_force_stereo(int on) { atomic_store(&g_force_stereo, on); }
void vr_view_recenter(void) { atomic_store(&g_recenter, 1); }
void vr_view_set_eye_tangents(float th, float tv) { g_eye_tan_h = th; g_eye_tan_v = tv; }
void vr_view_set_ipd_m(float m) { if (m > 0.04f && m < 0.09f) g_ipd = m * VR_UNITS_PER_M; }
static void src_axes(const float *v, float o[3]) { o[0] = -v[2]; o[1] = -v[0]; o[2] = v[1]; }   // ARKit (x right, y up, z back) → Source (x forward, y left, z up)
void vr_view_set_head_arkit(const float m[16], uint64_t pose_id) {
    float c0[3], c1[3], c2[3], c3[3], f[3], l[3], u[3], p[3];
    src_axes(m + 0, c0); src_axes(m + 4, c1); src_axes(m + 8, c2); src_axes(m + 12, c3);
    for (int i = 0; i < 3; i++) { f[i] = -c2[i]; l[i] = -c0[i]; u[i] = c1[i]; p[i] = c3[i] * VR_UNITS_PER_M; }
    if (atomic_exchange(&g_recenter, 0)) { g_ref_yaw = atan2f(f[1], f[0]); memcpy(g_ref_pos, p, sizeof p); }
    float cy = cosf(-g_ref_yaw), sy = sinf(-g_ref_yaw), out[12];
    const float *in[3] = {f, l, u};
    for (int k = 0; k < 3; k++) { out[3 * k] = cy * in[k][0] - sy * in[k][1]; out[3 * k + 1] = sy * in[k][0] + cy * in[k][1]; out[3 * k + 2] = in[k][2]; }
    float d[3] = {p[0] - g_ref_pos[0], p[1] - g_ref_pos[1], p[2] - g_ref_pos[2]};
    out[9] = cy * d[0] - sy * d[1]; out[10] = sy * d[0] + cy * d[1]; out[11] = d[2];
    atomic_fetch_add(&g_tseq, 1); memcpy(g_track, out, sizeof out); g_track_id = pose_id; atomic_fetch_add(&g_tseq, 1); atomic_store(&g_tracked, 1);
}
static float wrap180(float a) { a = fmodf(a + 180.f, 360.f); if (a < 0) a += 360.f; return a - 180.f; }
static _Atomic uint64_t g_aim_ns, g_aim_bits;   // the aim's pitch / yaw (degrees, neutral frame) packed as two floats, and when it was set
void vr_view_set_aim_arkit(const float dir[3]) {
    float d[3]; src_axes(dir, d);
    const float r = 180.f / (float)M_PI;
    float yaw = wrap180((atan2f(d[1], d[0]) - g_ref_yaw) * r), pitch = -atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * r;
    uint32_t a, b; memcpy(&a, &pitch, 4); memcpy(&b, &yaw, 4);
    atomic_store(&g_aim_bits, (uint64_t)a | (uint64_t)b << 32); atomic_store(&g_aim_ns, now_ns());
}
// The tracked hand's pose for the gun model (vr_view_set_hand_arkit): position and pointing direction in the neutral frame (as the head's, Source axes and units)
static _Atomic uint64_t g_hseq, g_hand_ns; static float g_hand[6];
void vr_view_set_hand_arkit(const float pos[3], const float dir[3]) {
    float p[3], d[3]; src_axes(pos, p); src_axes(dir, d);
    for (int i = 0; i < 3; i++) p[i] = p[i] * VR_UNITS_PER_M - g_ref_pos[i];
    float cy = cosf(-g_ref_yaw), sy = sinf(-g_ref_yaw), o[6] = {cy * p[0] - sy * p[1], sy * p[0] + cy * p[1], p[2], cy * d[0] - sy * d[1], sy * d[0] + cy * d[1], d[2]};
    atomic_fetch_add(&g_hseq, 1); memcpy(g_hand, o, sizeof o); atomic_fetch_add(&g_hseq, 1); atomic_store(&g_hand_ns, now_ns());
}
static int hand_get(float out[6]) {
    if (now_ns() - atomic_load(&g_hand_ns) > 150000000ull) return 0;
    for (int tries = 0; tries < 4; tries++) { uint64_t s = atomic_load(&g_hseq); if (s & 1) continue; memcpy(out, g_hand, sizeof g_hand); if (atomic_load(&g_hseq) == s) return 1; }
    return 0;
}
static _Atomic uint32_t g_move_rot_bits;
float vr_view_move_rotation(void) { uint32_t b = atomic_load(&g_move_rot_bits); float f; memcpy(&f, &b, 4); return f; }
static void move_rot_set(float deg) { uint32_t b; memcpy(&b, &deg, 4); atomic_store(&g_move_rot_bits, b); }
static int aim_get(float *pitch, float *yaw) {
    if (now_ns() - atomic_load(&g_aim_ns) > 150000000ull) return 0;
    uint64_t v = atomic_load(&g_aim_bits); uint32_t a = (uint32_t)v, b = (uint32_t)(v >> 32);
    memcpy(pitch, &a, 4); memcpy(yaw, &b, 4); return 1;
}
static int track_get(float out[12], uint64_t *id) {
    if (!atomic_load(&g_tracked)) return 0;
    for (int tries = 0; tries < 4; tries++) { uint64_t s = atomic_load(&g_tseq); if (s & 1) continue; memcpy(out, g_track, sizeof g_track); *id = g_track_id; if (atomic_load(&g_tseq) == s) return 1; }
    return 0;
}
static int head_get(float out[6]) {
    if (!atomic_load(&g_have)) return 0;
    for (int tries = 0; tries < 4; tries++) {
        uint64_t s = atomic_load(&g_seq); if (s & 1) continue;
        memcpy(out, g_head, sizeof g_head);
        if (atomic_load(&g_seq) == s) return 1;
    }
    return 0;
}

// Source mathlib: AngleMatrix / MatrixAngles (columns forward, left, up)
static void angle_matrix(const float a[3], float m[3][3]) {
    const float d = (float)M_PI / 180.f;
    float sp = sinf(a[0] * d), cp = cosf(a[0] * d), sy = sinf(a[1] * d), cy = cosf(a[1] * d), sr = sinf(a[2] * d), cr = cosf(a[2] * d);
    m[0][0] = cp * cy; m[1][0] = cp * sy; m[2][0] = -sp;
    m[0][1] = sr * sp * cy - cr * sy; m[1][1] = sr * sp * sy + cr * cy; m[2][1] = sr * cp;
    m[0][2] = cr * sp * cy + sr * sy; m[1][2] = cr * sp * sy - sr * cy; m[2][2] = cr * cp;
}
static void matrix_angles(float m[3][3], float a[3]) {
    const float r = 180.f / (float)M_PI;
    float xy = sqrtf(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
    if (xy > 0.001f) { a[1] = atan2f(m[1][0], m[0][0]) * r; a[0] = atan2f(-m[2][0], xy) * r; a[2] = atan2f(m[2][1], m[2][2]) * r; }
    else { a[1] = atan2f(-m[0][1], m[1][1]) * r; a[0] = atan2f(-m[2][0], xy) * r; a[2] = 0; }
}


// The material system's render target across our extra RenderView. RenderView leaves the render context's current target changed (pass dump F5364: the first
// eye drew the world into the full-frame texture, the game's call then drew straight into the back buffer and the result was black), so the target the game
// set up before RenderView is read before our call and set again after it.
// client.nro 1.0.0: `materials` (IMaterialSystem *) at client+0x1641288, GetRenderContext = vtable +0x390 (RenderView calls it so at +0x39ade4);
// IMatRenderContext slots (materialsystem.nro CMatRenderContext / CMatQueuedRenderContext vtables): Release +0x08, BeginRender +0x10, EndRender +0x18,
// SetRenderTarget +0x30 (→ SetRenderTargetEx(0, tex)), GetRenderTarget +0x38 (top of the render target stack)
enum { CLIENT_CVIEWRENDER_VT = 0x1281fb0, CLIENT_RENDERVIEW = 0x39977c, CLIENT_MATERIALS = 0x1641288 };
typedef void *(*vfn_p)(void *); typedef void (*vfn_v)(void *); typedef void (*vfn_vp)(void *, void *);
static void **vt_of(void *o) { return *(void ***)o; }
static void *mat_ctx(uint64_t client) {
    void *ms = *(void **)(uintptr_t)(client + CLIENT_MATERIALS); if (!ms) return NULL;
    void *ctx = ((vfn_p)vt_of(ms)[0x390 / 8])(ms); if (ctx) ((vfn_v)vt_of(ctx)[0x10 / 8])(ctx);
    return ctx;
}
static void mat_done(void *ctx) { ((vfn_v)vt_of(ctx)[0x18 / 8])(ctx); ((vfn_v)vt_of(ctx)[0x08 / 8])(ctx); }
static void *rt_get(uint64_t client) { void *c = mat_ctx(client); if (!c) return NULL; void *t = ((vfn_p)vt_of(c)[0x38 / 8])(c); mat_done(c); return t; }
static void rt_set(uint64_t client, void *t) { void *c = mat_ctx(client); if (!c) return; ((vfn_vp)vt_of(c)[0x30 / 8])(c, t); mat_done(c); }

// The game's view angles: IVEngineClient (`engine`, client+0x1562408) GetViewAngles / SetViewAngles, vtable slots 18 / 19 (+0x90 / +0x98). Checked in engine.nro 1.0.0:
// CEngineClient's vtable engine+0x921bc8, slot 18 +0x12b878 copies the split-screen slot's angles at client state +0x8ae8, slot 19 +0x12b8d4 validates and stores them.
// The pointer's vtable is checked against that layout before any call
enum { CLIENT_ENGINE = 0x1562408, ENGINE_CLIENT_VT = 0x921bc8, ENGINE_GETVIEWANGLES = 0x12b878 };
// The engine client, its vtable checked once against engine.nro 1.0.0 (slot 18 GetViewAngles, slot 7 ClientCmd: +0x12b52c, which hands its string to Cbuf_AddText)
enum { ENGINE_CLIENTCMD = 0x12b52c };
static void *engine_client(uint64_t *regs) {
    static int ok = -1;
    uint64_t vt = *(uint64_t *)(uintptr_t)regs[0], client = vt - CLIENT_CVIEWRENDER_VT;
    void *eng = *(void **)(uintptr_t)(client + CLIENT_ENGINE); if (!eng) return NULL;
    void **evt = vt_of(eng);
    if (ok < 0) { uint64_t base = (uint64_t)(uintptr_t)evt - ENGINE_CLIENT_VT;
        ok = (uint64_t)(uintptr_t)evt[18] - base == ENGINE_GETVIEWANGLES && (uint64_t)(uintptr_t)evt[7] - base == ENGINE_CLIENTCMD;
        LOG("%svr view: engine client %s", ok ? "" : "[RED] ", ok ? "found (view angles and console commands)" : "vtable does not match engine 1.0.0: view angles and console commands off"); }
    return ok ? eng : NULL;
}
// Console commands from the host (VR settings presets, A/B tests): IVEngineClient::ClientCmd queues them for the engine's next command-buffer run.
// The game's console output (unknown or blocked commands) lands in the log as [game] lines
static void engine_cmd(uint64_t *regs, const char *cmd) {
    void *eng = engine_client(regs); if (!eng || !cmd || !*cmd) return;
    LOG("vr view: console: %s", cmd);
    ((void (*)(void *, const char *))vt_of(eng)[7])(eng, cmd);
}
// IVEngineClient slot 18 GetViewAngles: the engine's current angles (after a portal turned the player), not the view set up earlier in the frame
static int engine_get_angles(uint64_t *regs, float a[3]) {
    void *eng = engine_client(regs); if (!eng) return 0;
    ((void (*)(void *, float *))vt_of(eng)[18])(eng, a);
    return 1;
}
// IVEngineClient slot 85 (engine+0x12eb98): reads the client state's paused flag (+0xf0), then asks the server: IsPaused (portal2vr's header has it at 86;
// slot 86 here returns a float, so one overload sits earlier in this build). The pause menu pauses single player
static int engine_paused(uint64_t *regs) {
    void *eng = engine_client(regs); if (!eng) return 0;
    void **evt = vt_of(eng); uint64_t base = (uint64_t)(uintptr_t)evt - ENGINE_CLIENT_VT;
    if ((uint64_t)(uintptr_t)evt[85] - base != 0x12eb98) return 0;
    return ((int (*)(void *))evt[85])(eng) & 1;
}
static int engine_set_angles(uint64_t *regs, const float a[3]) {
    void *eng = engine_client(regs); if (!eng) return 0;
    void **evt = vt_of(eng);
    float q[3] = {a[0], a[1], a[2]};
    ((vfn_vp)evt[19])(eng, q);
    return 1;
}
static int g_dump = -1; static float g_test;
static uint64_t g_client;   // client.nro's base, from CViewRender's vtable (render_view)
static void fake_track(void);
// Off-centre projection, experimental (DM_VR_OFFCENTER=<left>,<right>,<bottom>,<top>: fractions of the symmetric frustum, Source's convention: 0,1,0,1 = all of it;
// source-sdk-2013 view_shared.h, replayrenderer.cpp). Candidate Portal 2 layout from the CS:GO-branch CViewSetup: after the motion-blur shutter fields,
// m_flOffCenterTop / Bottom / Left / Right at +0xf0 / +0xf4 / +0xf8 / +0xfc and the flag bitfield at +0x100 with m_bOffCenter its bit 0 (portal2vr calls that word m_EdgeBlur).
// Unverified: a Mac run with this set must show the picture cropped to that part of the view before the headset uses it
static void offcenter_apply(uint8_t *v) {
    static int calls; static float oc[4]; static int on;
    if (calls++ % 30 == 0) { const char *e = getenv("DM_VR_OFFCENTER"); on = e && sscanf(e, "%f,%f,%f,%f", &oc[0], &oc[1], &oc[2], &oc[3]) == 4; }
    // The game keeps its view between frames (a Mac run showed the last crop after the switch was cleared), so turning it off clears the flag
    if (!on) { if (g_oc_set == 2) { v[VS_FLAGS] &= (uint8_t)~1u; g_oc_set = 0; } return; }
    g_oc_set = 2;
    memcpy(v + VS_OC_LEFT, &oc[0], 4); memcpy(v + VS_OC_RIGHT, &oc[1], 4); memcpy(v + VS_OC_BOTTOM, &oc[2], 4); memcpy(v + VS_OC_TOP, &oc[3], 4);
    v[VS_FLAGS] |= 1;
}
static void stereo_eyes(uint64_t *regs, uint8_t *vs, float v[3][3]) {
    static int calls;   // the diagnostic knobs are reread with render_view's switches
    calls++;
    offcenter_apply(vs);
    if (!g_stereo) return;
    // Left eye: our own call of the same function (the object's vtable slot 6), on a copy; its entry gate passes through (g_nested).
    // Right eye: the game's call, which proceeds after this gate returns, with the view rewritten in place
    // Diagnostic knobs (reread with the switches): DM_VR_DIAG_EYE = the eye modes 2 / 4 draw; DM_VR_FIRST_EYE = the eye our own (first) call draws;
    // DM_VR_CLEAR_FIRST / DM_VR_CLEAR_SECOND = masks ANDed into the clear flags of the first / second RenderView (hex, default all bits)
    static int diag_eye, first_eye; static unsigned clear1 = ~0u, clear2 = ~0u;
    if (calls % 30 == 1) {
        const char *e = getenv("DM_VR_DIAG_EYE"); diag_eye = e ? atoi(e) & 1 : 0; e = getenv("DM_VR_FIRST_EYE"); first_eye = e ? atoi(e) & 1 : 0;
        e = getenv("DM_VR_CLEAR_FIRST"); clear1 = e ? (unsigned)strtoul(e, NULL, 16) : ~0u; e = getenv("DM_VR_CLEAR_SECOND"); clear2 = e ? (unsigned)strtoul(e, NULL, 16) : ~0u;
    }
    static int told; if (!told++) LOG("vr view: RenderView clear flags %#x, what to draw %#x, view x/y %d/%d", (unsigned)regs[3], (unsigned)regs[4], *(int32_t *)(vs + VS_X), *(int32_t *)(vs + VS_Y));
    if (g_stereo == 2 || g_stereo == 4) { eye_setup(vs, diag_eye, v, 1, g_stereo == 2); return; }   // diagnostics: one eye's viewport, no second render
    int viewport = g_stereo != 3;
    uint8_t left[VS_COPY]; memcpy(left, vs, sizeof left);
    eye_setup(left, first_eye, v, viewport, 1);
    render_view_fn fn = (render_view_fn)(uintptr_t)(*(uint64_t **)(uintptr_t)regs[0])[6];
    uint64_t vt = *(uint64_t *)(uintptr_t)regs[0], client = vt - CLIENT_CVIEWRENDER_VT;
    static int rt_fix = -1;
    if (rt_fix < 0) { rt_fix = (uint64_t)(uintptr_t)fn - client == CLIENT_RENDERVIEW;
        if (!rt_fix) LOG("[RED] vr view: CViewRender vtable does not match client 1.0.0 (RenderView at +%#llx): render target not restored", (unsigned long long)((uint64_t)(uintptr_t)fn - client)); }
    const char *e_rt = getenv("DM_VR_RT_RESTORE"); int restore = rt_fix && !(e_rt && !atoi(e_rt));
    void *rt0 = restore ? rt_get(client) : NULL;
    g_nested = 1;
    fn((void *)(uintptr_t)regs[0], left, (void *)(uintptr_t)regs[2], (int)((unsigned)regs[3] & clear1), (int)regs[4]);
    g_nested = 0;
    if (restore) {
        void *rt1 = rt_get(client);
        static int told_rt; if (told_rt++ < 3) LOG("vr view: render target before our RenderView %p, after %p%s", rt0, rt1, rt0 != rt1 ? ": restored" : "");
        if (rt1 != rt0) rt_set(client, rt0);
    }
    eye_setup(vs, !first_eye, v, viewport, 1);
    regs[3] = (unsigned)regs[3] & clear2;   // written back to w3 when the gate returns
    if (atomic_fetch_add(&g_stereo_frames, 1) == 0) LOG("vr view: first stereo frame, left eye rendered through %p", (void *)fn);
}
static void render_view(uint64_t *regs, int param) {
    (void)param;
    if (g_nested) return;
    uint8_t *vs = (uint8_t *)(uintptr_t)regs[1];
    if (!vs) return;
    g_client = *(uint64_t *)(uintptr_t)regs[0] - CLIENT_CVIEWRENDER_VT;
    fake_track();
    // Switches are reread every 30 views, so `env KEY=VALUE` lines in the DM_CTL file change them while the game runs
    static int calls;
    if (calls++ % 30 == 0) {
        const char *e = getenv("DM_VR_STEREO"); int st = atomic_load(&g_force_stereo) ? (e && atoi(e) > 0 ? atoi(e) : 1) : e ? atoi(e) : 0;   // forced (immersive view / fake track): DM_VR_STEREO picks a diagnostic mode e = getenv("DM_VR_IPD"); if (e) g_ipd = (float)atof(e);
        e = getenv("DM_VR_VIEW_TEST"); g_test = e ? (float)atof(e) : 0;
        if (st != g_stereo) LOG("vr view: stereo mode %d (DM_VR_STEREO: 1 side by side, 2 left viewport only, 3 two full-frame renders, 4 left viewport without fov change), eye separation %.2f units", st, g_ipd);
        g_stereo = st;
        // DM_VR_CMD: run whenever its value changes (`env DM_VR_CMD=r_shadows 0` in the DM_CTL file on a Mac, or Documents/env.txt at start)
        static char last_cmd[512];
        e = getenv("DM_VR_CMD");
        if (e && strncmp(e, last_cmd, sizeof last_cmd - 1)) { snprintf(last_cmd, sizeof last_cmd, "%s", e); engine_cmd(regs, last_cmd); }
    }
    // DM_VR_CVARS: a settings preset run once each time tracked VR starts (the immersive view), and DM_VR_CVARS_OFF once when it ends
    {
        static int was_tracked;
        int now = atomic_load(&g_tracked);
        // Defaults: the portal gun lower and back, from the eyes to about hand height (the user: "right in front of the face", "down at hand level"), and one
        // level less of portal-in-portal (Mac A/B: -8.5% draws with two portals in view); DM_VR_CVARS / DM_VR_CVARS_OFF replace them (empty = none)
        // The game's crosshair is 2D, drawn once at the centre of the side-by-side frame (the seam: an artefact at the edge of one eye); the immersive view
        // draws its own at the aim point instead (PortalCompositor.swift)
        static const char *def_on = "viewmodel_offset_y -4; viewmodel_offset_z -10; r_portal_stencil_depth 1; crosshair 0; hud_quickinfo 0",
                          *def_off = "viewmodel_offset_y 0; viewmodel_offset_z 0; r_portal_stencil_depth 2; crosshair 1; hud_quickinfo 1";
        if (now != was_tracked) { const char *e = getenv(now ? "DM_VR_CVARS" : "DM_VR_CVARS_OFF"); engine_cmd(regs, e ? e : now ? def_on : def_off); was_tracked = now; }
    }
    if (g_dump < 0) { const char *e = getenv("DM_VR_VIEW_DUMP"); g_dump = e && atoi(e) ? 5 : 0; }
    if (g_oc_set && !atomic_load(&g_frust_on)) { vs[VS_FLAGS] &= (uint8_t)~1u; g_oc_set = 0; }   // the game's own view again: no off-centre crop
    float *ang = (float *)(vs + VS_ANGLES), *org = (float *)(vs + VS_ORIGIN);
    if (g_dump > 0) {
        g_dump--;
        char line[2600]; int n = 0;
        for (int o = 0; o < VS_DUMP_END && n < (int)sizeof line - 40; o += 4) { float f; int32_t i; memcpy(&f, vs + o, 4); memcpy(&i, vs + o, 4);
            n += snprintf(line + n, sizeof line - (size_t)n, " +%02x=%g/%d", o, f, i); }
        LOG("vr view: RenderView CViewSetup%s", line);
    }
    if (!(isfinite(ang[0]) && isfinite(ang[1]) && fabsf(ang[0]) <= 90.5f)) { static int once; if (!once++) LOG("[RED] vr view: angles at +%#x do not look like pitch/yaw (%g %g %g): not applied", VS_ANGLES, ang[0], ang[1], ang[2]); return; }
    float h[6], tr[12]; uint64_t pose_id = 0; int tracked = track_get(tr, &pose_id);
    if (tracked) {   // body yaw only, composed with the tracked head
        // Paused (the pause / save menu): the game's own flat frame, which the immersive view shows on a screen (no pose = flat), like a loading screen
        int paused = engine_paused(regs);
        static int was_paused;
        if (paused != was_paused) { LOG("vr view: game %s: %s", paused ? "paused" : "running", paused ? "flat screen in VR" : "stereo again"); was_paused = paused; }
        atomic_store(&g_paused_now, paused);
        if (paused) return;
        // Viewmodel attachments (the portal gun's electricity, its particles) are mapped between the viewmodel's and the world's projections with the player view
        // setup's fov / fovViewmodel ratio (source-sdk-2013 c_baseviewmodel.cpp FormatViewModelAttachment, GetPlayerViewSetup): the eyes draw the gun with the
        // world's field of view, so the ratio must be 1 too, or the effects land beside the gun (the corner artefact). Player view setup = CViewRender+8 (slot 12)
        // Mac A/B (DM_VR_FAKE_TRACK, 2026-10-31): the gun's glow (the "electricity") shows with the game's own fields of view (DM_VR_STEREO=4) and vanishes
        // once the eyes get the headset's field of view and aspect (mode 2), whatever fovViewmodel is. So the player view setup gets the eyes' field of view
        // and aspect as an experiment (DM_VR_PS_FOVVM: 0 untouched, 1 fovViewmodel = fov (default), 2 the eyes' fov and aspect: did not bring the glow back)
        { const char *e = getenv("DM_VR_PS_FOVVM"); int mode = e ? atoi(e) : 1; uint8_t *ps = (uint8_t *)(uintptr_t)regs[0] + 8;
          float H = g_eye_tan_h, V = g_eye_tan_v;
          if (atomic_load(&g_frust_on)) { H = fmaxf(g_frust[0][0], g_frust[0][1]); V = fmaxf(g_frust[0][2], g_frust[0][3]); }
          if (mode == 2 && H > 0 && V > 0) { float f = 2 * atanf(H) * 180.f / (float)M_PI, a = H / V; memcpy(ps + VS_FOV, &f, 4); memcpy(ps + VS_FOVVM, &f, 4); memcpy(ps + VS_ASPECT, &a, 4); }
          else if (mode) memcpy(ps + VS_FOVVM, ps + VS_FOV, 4); }
        // The engine's angles carry the head yaw we set last frame (engine_set_angles below) plus the controller's turning and any portal's turn since: the body
        // is what remains. Read from the engine, not this view: after a portal the view could still hold the angles from before it (exits facing sideways)
        float eng_ang[3] = {ang[0], ang[1], ang[2]};
        engine_get_angles(regs, eng_ang);
        float body = wrap180(eng_ang[1] - g_head_yaw_set);
        float b0[3] = {0, body, 0}, b[3][3], v[3][3], hm[3][3];
        angle_matrix(b0, b);
        for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) hm[i][k] = tr[3 * k + i];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) v[i][j] = b[i][0] * hm[0][j] + b[i][1] * hm[1][j] + b[i][2] * hm[2][j];
        for (int i = 0; i < 3; i++) org[i] += b[i][0] * tr[9] + b[i][1] * tr[10] + b[i][2] * tr[11];
        matrix_angles(v, ang);
        // The game looks where the head looks: its own view angles (aim, the tutorial's look up / look down checks) become body yaw + head, roll 0, as portal2vr does
        float set[3] = {fmaxf(-89.f, fminf(89.f, ang[0])), ang[1], 0};
        // With a tracked hand the game aims (portals, grabbing, the gun) where the hand points instead, relative to the same body yaw; the eyes keep the head
        float ap, ay;
        static int aiming = -1;
        int aim = aim_get(&ap, &ay);
        if (aim != aiming) { LOG("vr view: aim from %s", aim ? "the tracked hand" : "the head"); aiming = aim; }
        if (aim) { set[0] = fmaxf(-89.f, fminf(89.f, ap)); set[1] = wrap180(body + ay); }
        move_rot_set(aim ? wrap180(wrap180(ang[1] - body) - ay) : 0);   // head yaw - aim yaw, both relative to the body
        static int setang = 1; if (calls % 60 == 1) { const char *e = getenv("DM_VR_SET_ANGLES"); setang = !(e && !atoi(e)); }   // diagnostic
        if (setang && engine_set_angles(regs, set)) g_head_yaw_set = wrap180(set[1] - body);   // the yaw we add to the body, head or hand
        pose_rendered(pose_id); atomic_store(&g_last_stereo_ns, now_ns());
        stereo_eyes(regs, vs, v);
        return;
    }
    if (!head_get(h)) {
        memset(h, 0, sizeof h);
        if (!g_test && !g_stereo) return;
        if (g_test) {
        double t = (double)now_ns() / 1e9;
        h[1] = g_test * (float)sin(t * M_PI / 2); h[0] = g_test / 3 * (float)sin(t * M_PI);
        }
    }
    // view = body * head: rotate the head pose into the body frame, offset the origin along the body axes
    float b[3][3], hm[3][3], v[3][3];
    angle_matrix(ang, b); angle_matrix(h, hm);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) v[i][j] = b[i][0] * hm[0][j] + b[i][1] * hm[1][j] + b[i][2] * hm[2][j];
    for (int i = 0; i < 3; i++) org[i] += b[i][0] * h[3] + b[i][1] * h[4] + b[i][2] * h[5];
    matrix_angles(v, ang);
    stereo_eyes(regs, vs, v);
}

// ---- The portal gun in the hand, and the portal gun's state for the crosshair ----
// Observation gate OBS_SOURCE_VIEWMODEL at C_BaseViewModel::CalcViewModelView(this x0, owner x1, const Vector &eyePosition x2, const QAngle &eyeAngles x3)
// (client+0x145064, slot 288 of "15C_BaseViewModel"; patches.py). The function places the viewmodel at eyePosition/eyeAngles plus viewmodel_offset_x/y/z along
// the angles' right/forward/up (ConVars client+0x1538d10/da0/e30, value at [ConVar+0x38]+0x54), bob and lag. With a fresh tracked hand the gate points x2/x3
// at a pose of our own: the gun's muzzle at the hand (user: "where my hand's holding the Joy-Con should be at the end of the portal gun where it emits the
// portals"), pointing along the hand's ray, with the offset cvars' contribution taken back out. The viewmodel is then one object in the world that both eyes see.
// DM_VR_GUN=0 leaves the gun at the eye; DM_VR_GUN_OFS=<forward>,<right>,<up> = the muzzle in the viewmodel's own frame (Source units)
enum { CLIENT_VM_OFS_X = 0x1538d10, CLIENT_VM_OFS_Y = 0x1538da0, CLIENT_VM_OFS_Z = 0x1538e30, CLIENT_ENTLIST = 0x1562cb0,
       VM_HWEAPON = 0x1450, CLIENT_PORTALGUN_VT = 0x1469630,
       PG_CANFIRE1 = 0x1528, PG_CANFIRE2 = 0x1529, PG_LASTFIRED = 0x152c, PG_HPRIMARY = 0x1568, PG_HSECONDARY = 0x156c, PORTAL_ACTIVATED = 0x3ce8 };
static float cvar_f(uint64_t cv) { uint64_t parent = *(uint64_t *)(uintptr_t)(g_client + cv + 0x38); if (!parent) return 0; float f; memcpy(&f, (void *)(uintptr_t)(parent + 0x54), 4); return isfinite(f) ? f : 0; }
// CBaseHandle → entity: index = low 16 bits, serial = high 16 bits, entries of 32 bytes at client+0x1562cb0 (serial at +0x10, entity at +8), as the function's own m_hWeapon lookup
static uint8_t *ent_from_handle(uint32_t h) {
    if (h == 0xffffffffu) return NULL;
    uint8_t *e = (uint8_t *)(uintptr_t)(g_client + CLIENT_ENTLIST + (uint64_t)(h & 0xffff) * 32);
    if (*(uint32_t *)(e + 0x10) != h >> 16) return NULL;
    return *(uint8_t **)(e + 8);
}
// Portal gun state (vr_view_portal_state): bit 0 can fire blue (portal 1), bit 1 can fire orange (portal 2), bit 2 blue placed, bit 3 orange placed, bit 4 the
// last fired was orange. C_WeaponPortalgun fields from its prediction table (client+0x1469390...) and RecvTable (+0x8e18c...): m_bCanFirePortal1/2 +0x1528/9,
// m_iLastFiredPortal +0x152c, m_hPrimaryPortal / m_hSecondaryPortal +0x1568/c; C_Portal_Base2D m_bActivated +0x3ce8 (RecvTable at +0x632e0)
static atomic_int g_pg_state = -1; static _Atomic uint64_t g_pg_ns;
int vr_view_portal_state(void) { if (now_ns() - atomic_load(&g_pg_ns) > 500000000ull) return atomic_load(&g_pg_ns) ? 0 : -1; return atomic_load(&g_pg_state); }
static void portalgun_read(uint8_t *vm) {
    uint8_t *w = ent_from_handle(*(uint32_t *)(vm + VM_HWEAPON));
    if (!w || *(uint64_t *)w != g_client + CLIENT_PORTALGUN_VT) return;   // no weapon, or not the portal gun (the second viewmodel has none)
    int st = (w[PG_CANFIRE1] ? 1 : 0) | (w[PG_CANFIRE2] ? 2 : 0);
    uint8_t *p1 = ent_from_handle(*(uint32_t *)(w + PG_HPRIMARY)), *p2 = ent_from_handle(*(uint32_t *)(w + PG_HSECONDARY));
    if (p1 && p1[PORTAL_ACTIVATED]) st |= 4;
    if (p2 && p2[PORTAL_ACTIVATED]) st |= 8;
    if (*(int32_t *)(w + PG_LASTFIRED) == 1) st |= 16;
    { static int32_t last_fx = -99; int32_t fx = *(int32_t *)(w + 0x1564); if (fx != last_fx) { LOG("vr view: portal gun %p effect state %d", (void *)w, fx); last_fx = fx; } }
    int old = atomic_exchange(&g_pg_state, st); atomic_store(&g_pg_ns, now_ns());
    if (old != st) LOG("vr view: portal gun: blue %s%s, orange %s%s", st & 1 ? "ready" : "off", st & 4 ? " (placed)" : "", st & 2 ? "ready" : "off", st & 8 ? " (placed)" : "");
}
static float g_vm_pose[8];   // the pose x2/x3 point at: origin [0..2], angles [4..6]
static void calc_viewmodel(uint64_t *regs, int param) {
    (void)param;
    if (!g_client || !regs[0] || !regs[2] || !regs[3]) return;
    portalgun_read((uint8_t *)(uintptr_t)regs[0]);
    static int calls, on = 1; static float mx = 16, my = 8, mz = -5;   // user, first headset run at 32: "too close on the hand", wanted "in between": the hand now holds the middle of the gun
    if (calls++ % 60 == 0) { const char *e = getenv("DM_VR_GUN"); on = !(e && !atoi(e)); e = getenv("DM_VR_GUN_OFS"); if (e) sscanf(e, "%f,%f,%f", &mx, &my, &mz); }
    float hp[6];
    if (!on || !atomic_load(&g_tracked) || atomic_load(&g_paused_now) || !hand_get(hp)) return;
    const float *eye = (const float *)(uintptr_t)regs[2], *eang = (const float *)(uintptr_t)regs[3];
    // The body yaw as render_view derives it: the game's yaw minus the yaw we added (hand or head)
    float b0[3] = {0, wrap180(eang[1] - g_head_yaw_set), 0}, b[3][3], m[3][3], hw[3], dw[3];
    angle_matrix(b0, b);
    for (int i = 0; i < 3; i++) { hw[i] = eye[i] + b[i][0] * hp[0] + b[i][1] * hp[1] + b[i][2] * hp[2]; dw[i] = b[i][0] * hp[3] + b[i][1] * hp[4] + b[i][2] * hp[5]; }
    const float r2d = 180.f / (float)M_PI;
    float a[3] = {-atan2f(dw[2], sqrtf(dw[0] * dw[0] + dw[1] * dw[1])) * r2d, atan2f(dw[1], dw[0]) * r2d, 0};
    angle_matrix(a, m);   // columns forward, left, up; right = -left
    float X = cvar_f(CLIENT_VM_OFS_X), Y = cvar_f(CLIENT_VM_OFS_Y), Z = cvar_f(CLIENT_VM_OFS_Z);
    for (int i = 0; i < 3; i++) {
        float f = m[i][0], rt = -m[i][1], u = m[i][2];
        g_vm_pose[i] = hw[i] - (f * mx + rt * my + u * mz) - (rt * X + f * Y + u * Z);
    }
    memcpy(g_vm_pose + 4, a, sizeof a);
    regs[2] = (uint64_t)(uintptr_t)g_vm_pose; regs[3] = (uint64_t)(uintptr_t)(g_vm_pose + 4);
    static int told; if (!told++) LOG("vr view: portal gun held in the hand (muzzle offset %g %g %g; viewmodel offsets %g %g %g)", mx, my, mz, X, Y, Z);
}
// Mac test of the tracked path without a headset (DM_VR_FAKE_TRACK=1): a still head at the neutral pose, the Vision Pro's per-eye frustum (narrowed 0.65 / 0.85),
// stereo, and a hand DM_VR_FAKE_HAND=<x>,<y>,<z> metres from the head (ARKit axes: x right, y up, z back; default 0.15,-0.25,-0.35) pointing straight ahead
static void fake_track(void) {
    static int on = -1; static float hx = 0.15f, hy = -0.25f, hz = -0.35f;
    static int calls;
    if (calls++ % 60 == 0) { const char *e = getenv("DM_VR_FAKE_TRACK"); int now = e && atoi(e);
        e = getenv("DM_VR_FAKE_HAND"); if (e) sscanf(e, "%f,%f,%f", &hx, &hy, &hz);
        if (now != on) { if (on == 1 && !now) { vr_view_clear_head(); vr_view_force_stereo(0); for (int k = 0; k < 2; k++) vr_view_set_eye_frustum(k, 0, 0, 0, 0); } on = now; if (now) LOG("vr view: fake tracked head (Mac test)"); } }
    if (on != 1) return;
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    vr_view_set_head_arkit(id, 0);
    vr_view_set_eye_tangents(1.732f * 0.65f, 1.192f * 0.85f); vr_view_set_ipd_m(0.063f);
    static int fr = 1; if (calls % 60 == 1) { const char *e = getenv("DM_VR_FAKE_FRUSTUM"); fr = !(e && !atoi(e)); }
    if (!fr) { for (int k = 0; k < 2; k++) vr_view_set_eye_frustum(k, 0, 0, 0, 0); } else {
    vr_view_set_eye_frustum(0, 1.732f * 0.65f, 1.0f * 0.65f, 1.192f * 0.85f, 1.0f * 0.85f);
    vr_view_set_eye_frustum(1, 1.0f * 0.65f, 1.732f * 0.65f, 1.192f * 0.85f, 1.0f * 0.85f); }
    vr_view_force_stereo(1);
    float hp[3] = {hx, hy, hz}, d[3] = {0, 0, -1};
    vr_view_set_hand_arkit(hp, d);
    float aim[3] = {-hx, -hy, -4.0f - hz}; float n = sqrtf(aim[0] * aim[0] + aim[1] * aim[1] + aim[2] * aim[2]);
    for (int i = 0; i < 3; i++) aim[i] /= n;   // the head aims at the hand ray's point 4 m out, as the immersive view does
    vr_view_set_aim_arkit(aim);
}
__attribute__((constructor)) static void vrview_init(void) { gate_obs_set(OBS_SOURCE_RENDERVIEW, render_view); gate_obs_set(OBS_SOURCE_VIEWMODEL, calc_viewmodel); }
