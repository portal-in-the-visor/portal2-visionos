# Portal 2 (0100ABD01785C000): the rewriter's patches of this game's own builds (dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's build ID).
OBS = {
  # Portal 2 1.0.0 client.nro (build 6f1172dd16fa27eb697921364f8a78cb): CViewRender::RenderView, found through RTTI: typeinfo "11CViewRender" → vtable client+0x1281fb0, slot 6
  # (IViewRender: Init, LevelInit, LevelShutdown, Shutdown, OnRenderStart, Render, RenderView). CViewRender::Render (slot 5, +0x374df8) calls it through [vtable+0x30] at +0x3759d4.
  # Entry: x1 = const CViewSetup &view, which RenderView copies into this+0x234 at once. The VR camera hook (dm/host/hle_vrview.c) composes the head pose into its angles/origin.
  '6f1172dd16fa27eb697921364f8a78cb': [
    (0x39977c, 26, '6db63bef', 'CViewRender::RenderView entry: x1 = CViewSetup; VR head pose (hle_vrview.c); orig stp d15, d14, [sp, #-0xa0]!'),
    # C_BaseViewModel::CalcViewModelView(this x0, C_BasePlayer *owner x1, const Vector &eyePosition x2, const QAngle &eyeAngles x3): vtable slot 288 of
    # "15C_BaseViewModel" (vtable client+0x11e6528). Found as the one function reading viewmodel_offset_x/y/z (ConVars client+0x1538d10/da0/e30); it copies
    # x2/x3, adds the offsets along the angles' right/forward/up, the weapon's bob (m_hWeapon at this+0x1450) and lag, then SetLocalOrigin/Angles.
    # VR: the gate points x2/x3 at the tracked hand's pose so the portal gun is held in the hand (hle_vrview.c)
    (0x145064, 27, 'd10303ff', 'C_BaseViewModel::CalcViewModelView entry: x2/x3 = eye origin/angles; VR gun in the hand (hle_vrview.c); orig sub sp, sp, #0xc0'),
  ],
}
