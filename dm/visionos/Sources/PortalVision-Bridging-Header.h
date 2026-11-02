// C interfaces the visionOS app's Swift calls (dm/visionos/Sources): boot and window from dm/ios, head pose from dm/host/vr_view.h, the present tap (vr_tap.m)
#import <UIKit/UIKit.h>
#include "../../host/vr_view.h"
int dm_vision_boot(void);                       // main.m: the environment and log before the window (main() is not used on visionOS)
UIViewController *front_ios_root(void);         // front_ios.m: the launcher, then the game view
void front_ios_focus_event(int ev);             // 0 resign active, 1 background, 2 foreground, 3 active
// vr_tap.m: game frames for the immersive view
void vr_tap_install(void);
void vr_tap_set_active(int on);                 // on: frames go to the immersive view, not the window, and the game renders side-by-side stereo
// The newest copied frame: texture (MTLTexture), the shared event and the value signalled when its copy is done, its size. 0 = none yet. Pointers are unretained
int vr_tap_latest(void **texture, void **event, uint64_t *value, uint64_t *pose);   // pose: the head pose id the frame was rendered with
