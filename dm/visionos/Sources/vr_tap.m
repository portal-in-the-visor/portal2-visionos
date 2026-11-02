// Present tap for the immersive view: nvn_mtl.m hands each presented frame (the game's back buffer, side-by-side eyes when DM_VR_STEREO=1) with a command buffer of
// the render queue; this copies it into a ring texture and signals a shared event with the present number, so the compositor (PortalCompositor.swift, its own queue)
// waits for exactly that copy before sampling. While active the window is not presented (the immersive space hides it).
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../host/vr_view.h"
extern int (*mtl_present_tap)(void *cb, void *src, const NSUInteger rect[4], uint64_t n);
int mtl_tap_copy(void *dst, void *src, const NSUInteger r[4], void *event, uint64_t value, int slot);
enum { RING = 3 };
static id<MTLTexture> g_ring[RING];
static id<MTLSharedEvent> g_event;
static _Atomic int g_active, g_latest = -1;
static _Atomic uint64_t g_value[RING], g_pose[RING];
static int g_next;
static int tap(void *cbp, void *srcp, const NSUInteger r[4], uint64_t n) {
    if (!atomic_load(&g_active)) return 0;
    (void)cbp;
    id<MTLTexture> src = (__bridge id<MTLTexture>)srcp;
    int slot = g_next;
    id<MTLTexture> t = g_ring[slot];
    if (!t || t.width != r[2] || t.height != r[3] || t.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:r[2] height:r[3] mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        t = g_ring[slot] = [src.device newTextureWithDescriptor:d];
        static int told; if (told++ < 2) fprintf(stderr, "[dm] vr tap: ring texture %lux%lu format %lu\n", (unsigned long)r[2], (unsigned long)r[3], (unsigned long)src.pixelFormat);
    }
    if (!g_event) g_event = [src.device newSharedEvent];
    // On the render queue's Metal 4 path, behind the frame's work (nvn_mtl.m mtl_tap_copy); a slot whose last copy is still running is skipped
    if (!mtl_tap_copy((__bridge void *)t, (__bridge void *)src, r, (__bridge void *)g_event, n, slot)) return 1;
    g_next = (g_next + 1) % RING;
    atomic_store(&g_value[slot], n); atomic_store(&g_pose[slot], vr_view_take_rendered_pose()); atomic_store(&g_latest, slot);
    return 1;
}
void vr_tap_install(void) { mtl_present_tap = tap; }
void vr_tap_set_active(int on) {
    vr_view_force_stereo(on);   // hle_vrview.c picks it up within 30 views
    atomic_store(&g_active, on);
    fprintf(stderr, "[dm] vr tap: immersive view %s\n", on ? "on: frames go to the headset, stereo on" : "off: frames go to the window");
}
int vr_tap_latest(void **texture, void **event, uint64_t *value, uint64_t *pose) {
    int s = atomic_load(&g_latest);
    if (s < 0 || !g_event) return 0;
    *texture = (__bridge void *)g_ring[s]; *event = (__bridge void *)g_event; *value = atomic_load(&g_value[s]); *pose = atomic_load(&g_pose[s]);
    return 1;
}
