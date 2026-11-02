// Head pose → Source engine view (Portal 2 VR work, dm/games/0100ABD01785C000/README.md "VR camera hook").
// A front end publishes the latest head pose; the observation gate at CViewRender::RenderView's entry (hle_vrview.c) composes it with the game's view.
#pragma once
#include <stdint.h>
// Head orientation relative to the body/aim view, Source angles in degrees (pitch down-positive, yaw left-positive, roll),
// and head position relative to the neutral pose in Source units (x forward, y left, z up). Thread-safe; latest value wins.
void vr_view_set_head(float pitch, float yaw, float roll, float x, float y, float z);
void vr_view_clear_head(void);
// Tracked head (Apple Vision Pro, dm/visionos): an ARKit device pose, column-major 4x4 (x right, y up, -z forward, metres). The first pose after a recenter
// is the neutral one; the view then uses the game's body yaw only (no controller pitch), composed with the tracked head. Thread-safe; latest value wins
void vr_view_set_head_arkit(const float m[16], uint64_t pose_id);
// The pose id the oldest not yet presented game frame was rendered with (FIFO from RenderView to present; the immersive view hands the system that pose's
// device anchor, so visionOS reprojects the frame from where it was rendered). 0 = the frame had no 3D view (a loading screen: shown flat)
uint64_t vr_view_take_rendered_pose(void);
// 1 while the game shows no stereo view: paused, or no RenderView for 200 ms (loading screens)
int vr_view_frame_flat(void);
void vr_view_recenter(void);
// Aim (the portal gun) independent of the head: a direction in ARKit world space (a tracked hand). While fresh (150 ms), the game's view angles, which aim
// portals, grabs and movement, follow it instead of the head; the rendered view still follows the head
void vr_view_set_aim_arkit(const float dir[3]);
// While the hand aims: the yaw (degrees, left positive) from the aim to the head. hle_hid.c turns the left stick by it, so walking goes where you look
// although the game moves along its view angles, which follow the hand
float vr_view_move_rotation(void);
// The eye images the immersive view needs: per-eye tangents of the half field of view (symmetric hull of the display's), and the eye distance in metres
void vr_view_set_eye_tangents(float tan_half_h, float tan_half_v);
void vr_view_set_ipd_m(float metres);
// Per-eye off-centre frustum (eye 0 left, 1 right): the tangents of the left, right, bottom and top half-angles, as the display's views report them
// (already narrowed by the immersive view's field-of-view factors). Each eye then renders exactly that frustum through Source's off-centre projection;
// all zero turns it off (the symmetric vr_view_set_eye_tangents hull is used)
void vr_view_set_eye_frustum(int eye, float left, float right, float bottom, float top);
// Units per metre for tracked positions and the eye distance (portal2vr's VRScale)
#define VR_UNITS_PER_M 43.2f
// Side-by-side stereo on regardless of DM_VR_STEREO (the immersive view; setenv from another thread is not safe while the game reads the environment)
void vr_view_force_stereo(int on);
// The tracked hand for the portal gun model: the hand's position and pointing direction in ARKit world space (metres). While fresh (150 ms) the viewmodel is
// placed so the gun's muzzle is at the hand, pointing along that direction (hle_vrview.c calc_viewmodel)
void vr_view_set_hand_arkit(const float pos[3], const float dir[3]);
// The portal gun for the crosshair: -1 unknown; else bit 0 blue can fire, bit 1 orange can fire, bit 2 blue placed, bit 3 orange placed, bit 4 last fired orange
int vr_view_portal_state(void);
