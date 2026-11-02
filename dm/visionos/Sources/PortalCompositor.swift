// Compositor Services side of Portal 2's immersive view. Each display frame: query the device pose for the frame's presentation time (WorldTrackingProvider),
// publish it to the camera hook (vr_view_set_head_arkit), wait for the newest game frame's copy (vr_tap.m's shared event) and draw its left / right half into the
// left / right eye. The game renders each eye with a symmetric field of view covering the display's tangents (vr_view_set_eye_tangents), so the composite maps
// every display pixel's ray into that projection. No reprojection yet: head motion between the game frame and display shows as latency.
// Modelled on Klepton's KleptonCompositor.swift (~/Developer/Klepton/visionos).
import CompositorServices
import SwiftUI
import Metal
import ARKit
import simd
import QuartzCore
import Foundation

/// Into the host's log (Documents/log.txt is stderr), where NSLog does not go
func vrlog(_ s: String) { fputs("[dm] [vr] " + s + "\n", stderr) }

struct PortalLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        configuration.isFoveationEnabled = false   // the composite draws no rate map yet
        configuration.layout = .dedicated          // one texture per eye
    }
}

final class PortalCompositor {
    nonisolated(unsafe) private static var current: PortalCompositor?
    private let renderer: LayerRenderer
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let depthState: MTLDepthStencilState
    private let dummy: MTLTexture
    private let session = ARKitSession()
    private let world = WorldTrackingProvider()
    // The right hand aims the portal gun (with the right Joy-Con in it for the triggers): the direction from the wrist to the middle-finger knuckle.
    // DM_VR_HAND_AIM=0 keeps aiming with the head
    private let hands = HandTrackingProvider()
    private let handAim = getenv("DM_VR_HAND_AIM").map { String(cString: $0) != "0" } ?? true
    private var handSeen = false
    // The aim converges this far out (DM_VR_AIM_DIST, metres): the game fires from the eye, so it aims at the point the hand's ray reaches here, and the
    // crosshair sits on that point; the ray then starts at your hand, where the gun's emitter is in your grip (user: the gun should end where the hand is)
    private let aimDist: Float = getenv("DM_VR_AIM_DIST").flatMap { Float(String(cString: $0)) }.map { min(max($0, 1), 20) } ?? 4
    // One Euro filter on the hand's ray (raw hand tracking jitters: "a little janky, not very smooth"): smooth when still, quick when moving
    private var fPos = OneEuro3(minCutoff: 1.2, beta: 0.6), fDir = OneEuro3(minCutoff: 1.5, beta: 4.0)
    private var frames = 0, anchors = 0, noAnchor = 0, reprojected = 0
    // The device anchors published to the game, by pose id, so a game frame is presented with the pose it was rendered from and visionOS reprojects it to
    // the display's (the first stereo run: drawing a 60 Hz game frame with the newest 90 Hz pose judders and blurs with every head movement)
    private var poseRing = [(UInt64, DeviceAnchor?)](repeating: (0, nil), count: 64)
    private var poseId: UInt64 = 0
    private var trackedSinceStart = 0
    private var tangentsSent = false
    // The game's per-eye field of view, as a fraction of the display's in tangent space (horizontal, vertical). Each eye has only 960x1080 pixels; spreading
    // them over the display's whole ±60° x ±50° gave about 5 px/degree at the centre (the first stereo runs: "quite blurry"). Narrowing it puts the pixels
    // where the eye looks, and horizontally more, where the half-width eye is thinnest. DM_VR_FOV_H / DM_VR_FOV_V (Documents/env.txt) override; 1 = the whole display
    private static func envScale(_ k: String, _ d: Float) -> Float { getenv(k).flatMap { Float(String(cString: $0)) }.map { min(max($0, 0.3), 1) } ?? d }
    private let fovH = PortalCompositor.envScale("DM_VR_FOV_H", 0.65)
    private let fovV = PortalCompositor.envScale("DM_VR_FOV_V", 0.85)
    // Off-centre eyes (DM_VR_OFFCENTER_EYES=0 falls back to the symmetric hull): the display's views are asymmetric (the nose side is narrower), and a symmetric
    // frustum spends pixels on the side nobody sees
    private let crosshair = getenv("DM_VR_CROSSHAIR").map { String(cString: $0) != "0" } ?? true
    private let offCentre = getenv("DM_VR_OFFCENTER_EYES").map { String(cString: $0) != "0" } ?? true
    private func eyeFrustum(_ v: LayerRenderer.Drawable.View) -> SIMD4<Float> {   // left, right, top, bottom, narrowed like the symmetric hull
        SIMD4<Float>(v.tangents.x * fovH, v.tangents.y * fovH, v.tangents.z * fovV, v.tangents.w * fovV)
    }
    private let testPattern = getenv("DM_VR_TEST_PATTERN").map { String(cString: $0) == "1" } ?? false

    static func start(_ r: LayerRenderer) {
        guard let c = PortalCompositor(r) else { vrlog("compositor could not start"); return }
        current = c
        let t = Thread { c.run() }
        t.name = "Portal VR compositor"; t.qualityOfService = .userInteractive
        t.start()
    }

    private static let shader = """
    #include <metal_stdlib>
    using namespace metal;
    struct VOut { float4 pos [[position]]; float2 uv; };
    vertex VOut vmain(uint vid [[vertex_id]], constant float &depth [[buffer(0)]]) {
        float2 p = float2((vid << 1) & 2, vid & 2);
        VOut o; o.pos = float4(p * 2.0 - 1.0, depth, 1.0); o.uv = float2(p.x, 1.0 - p.y); return o;
    }
    struct Params { float4 tangents; float4 game_tan; float eye; float srgb_src; float pattern; float flat; float crosshair; float xh_x, xh_y, pad2; };   // xh: the crosshair centre in this eye's tangents   // flat: a frame with no 3D view, shown whole on a virtual screen   // game_tan: the eye's rendered frustum (left, right, top, bottom)
    fragment float4 fmain(VOut in [[stage_in]], texture2d<float> game [[texture(0)]], constant Params &p [[buffer(0)]]) {
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        // the display pixel's ray: tangents are (left, right, top, bottom) magnitudes
        float tx = mix(-p.tangents.x, p.tangents.y, in.uv.x);
        float ty = mix(p.tangents.z, -p.tangents.w, in.uv.y);
        float2 g = float2((tx + p.game_tan.x) / (p.game_tan.x + p.game_tan.y), (p.game_tan.z - ty) / (p.game_tan.z + p.game_tan.w));
        if (p.flat > 0.5) {
            // A loading screen or other frame without a 3D view: the whole 16:9 frame on a screen 3 m wide, 3 m ahead, seen by each eye from its own
            // position (32 mm either side), so both eyes agree on its distance instead of each getting half of a flat picture
            float eye_x = (p.eye > 0.5 ? 0.0315 : -0.0315);
            float2 q = float2(0.5 + (eye_x + tx * 3.0) / 3.0, 0.5 - (ty * 3.0) / 1.6875);
            if (any(q < 0.0) || any(q > 1.0)) return float4(0, 0, 0, 1);
            float4 fc = game.sample(s, q);
            return float4(p.srgb_src > 0.5 ? fc.rgb : pow(fc.rgb, 2.2), 1.0);
        }
        if (p.pattern > 0.5) {   // DM_VR_TEST_PATTERN=1: a grid every 10 degrees, red left eye / green right eye, to check the composite without the game
            float2 a = atan(float2(tx, ty)) * 57.29578 / 10.0; float2 f = abs(fract(a) - 0.5);
            float line = (f.x > 0.45 || f.y > 0.45) ? 1.0 : 0.15;
            return float4(line * (1.0 - p.eye), line * p.eye, line * 0.5, 1.0);
        }
        if (any(g < 0.0) || any(g > 1.0)) return float4(0, 0, 0, 1);
        // Catmull-Rom (bicubic) instead of bilinear: the game's eye is upscaled about 2x onto the display, and bilinear smears it. 9 bilinear taps,
        // kept inside this eye's half of the side-by-side frame
        float2 size = float2(game.get_width(), game.get_height());
        float2 lo = float2(p.eye * 0.5 * size.x + 0.5, 0.5), hi = float2((p.eye * 0.5 + 0.5) * size.x - 0.5, size.y - 0.5);
        float2 pos = float2((p.eye + g.x) * 0.5, g.y) * size;
        float2 tc = floor(pos - 0.5) + 0.5, f = pos - tc;
        float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
        float2 w12 = w1 + w2, t0 = clamp(tc - 1.0, lo, hi) / size, t3 = clamp(tc + 2.0, lo, hi) / size, t12 = clamp(tc + w2 / w12, lo, hi) / size;
        float4 c = game.sample(s, float2(t0.x, t0.y)) * w0.x * w0.y + game.sample(s, float2(t12.x, t0.y)) * w12.x * w0.y + game.sample(s, float2(t3.x, t0.y)) * w3.x * w0.y
                 + game.sample(s, float2(t0.x, t12.y)) * w0.x * w12.y + game.sample(s, float2(t12.x, t12.y)) * w12.x * w12.y + game.sample(s, float2(t3.x, t12.y)) * w3.x * w12.y
                 + game.sample(s, float2(t0.x, t3.y)) * w0.x * w3.y + game.sample(s, float2(t12.x, t3.y)) * w12.x * w3.y + game.sample(s, float2(t3.x, t3.y)) * w3.x * w3.y;
        c = max(c, 0.0);
        // a soft edge where the game's (narrowed) field of view ends, instead of a hard cut to black
        float2 edge = min(g, 1.0 - g);
        c.rgb *= smoothstep(0.0, 0.03, min(edge.x, edge.y));
        // Crosshair: the game aims where the head looks (hle_vrview.c sets its view angles from the head), so the aim point is straight ahead of the head.
        // A ring 2 m out, seen from each eye (32 mm either side of centre), so both eyes agree on its depth; the game's own 2D crosshair is turned off in VR
        // The aim point 2 m along the aim (head or tracked hand), projected into this eye by the compositor: a ring in Portal's colours, blue left half, orange right
        // A thin outline (user: the filled ring was "too thick and too distracting"), each half shown only while the gun can fire that colour and drawn
        // bright and a little bolder once that portal is placed, like the original's indicator. pad2 = vr_view_portal_state (-1 unknown: both halves, dim)
        float r = length(float2(tx - p.xh_x, ty - p.xh_y));
        float aa = max(fwidth(r), 1e-5);
        int st = int(p.pad2);
        bool lefth = tx < p.xh_x;
        float show = st < 0 ? 1.0 : float((st >> (lefth ? 0 : 1)) & 1);
        float placed = st < 0 ? 0.0 : float((st >> (lefth ? 2 : 3)) & 1);
        float hw = mix(0.0011, 0.0019, placed);
        float ring = (1.0 - smoothstep(hw, hw + aa, abs(r - 0.012))) * p.crosshair * show;
        ring *= smoothstep(0.0016, 0.0016 + aa, abs(tx - p.xh_x));   // a small gap where the halves meet, top and bottom
        float3 rc = lefth ? float3(0.25, 0.62, 1.0) : float3(1.0, 0.55, 0.1);
        c.rgb = mix(c.rgb, rc, ring * mix(0.6, 1.0, placed));
        float3 rgb = p.srgb_src > 0.5 ? c.rgb : pow(c.rgb, 2.2);   // a non-sRGB source holds display-encoded values; the drawable is sRGB
        return float4(rgb, 1.0);
    }
    """

    private init?(_ r: LayerRenderer) {
        renderer = r
        device = r.device
        guard let q = device.makeCommandQueue() else { return nil }
        queue = q
        do {
            let lib = try device.makeLibrary(source: Self.shader, options: nil)
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "vmain")
            d.fragmentFunction = lib.makeFunction(name: "fmain")
            d.colorAttachments[0].pixelFormat = r.configuration.colorFormat
            d.depthAttachmentPixelFormat = r.configuration.depthFormat
            pipeline = try device.makeRenderPipelineState(descriptor: d)
        } catch { vrlog("pipeline: \(error)"); return nil }
        let ds = MTLDepthStencilDescriptor(); ds.depthCompareFunction = .always; ds.isDepthWriteEnabled = true   // see the depth note in frame()
        depthState = device.makeDepthStencilState(descriptor: ds)!
        dummy = device.makeTexture(descriptor: MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1, height: 1, mipmapped: false))!
    }

    private func run() {
        Task {
            var providers: [any DataProvider] = [world]
            if handAim && HandTrackingProvider.isSupported {
                let auth = await session.requestAuthorization(for: [.handTracking])
                if auth[.handTracking] == .allowed { providers.append(hands) } else { vrlog("hand tracking not allowed: aiming with the head") }
            }
            do { try await session.run(providers) } catch { vrlog("ARKit session: \(error)") }
        }
        vr_view_recenter()
        vr_tap_set_active(1)
        vrlog("immersive view started")
        while true {
            switch renderer.state {
            case .paused: renderer.waitUntilRunning()
            case .running: autoreleasepool { frame() }
            case .invalidated:
                vr_tap_set_active(0); vr_view_clear_head(); vr_view_set_eye_tangents(0, 0); vr_view_set_eye_frustum(0, 0, 0, 0, 0); vr_view_set_eye_frustum(1, 0, 0, 0, 0)
                vrlog("immersive view ended after \(frames) frames")
                Self.current = nil
                return
            @unknown default: return
            }
        }
    }

    private func frame() {
        guard let frame = renderer.queryNextFrame() else { return }
        frame.startUpdate(); frame.endUpdate()
        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        frame.startSubmission()
        // No drawable (the space is closing): the frame is not valid, and ending its submission is a client bug that traps (first headset run)
        guard let drawable = frame.queryDrawable() else { return }
        let t = LayerRenderer.Clock.Instant.epoch.duration(to: drawable.frameTiming.presentationTime)
        let seconds = Double(t.components.seconds) + Double(t.components.attoseconds) * 1e-18
        let anchor = world.queryDeviceAnchor(atTimestamp: seconds)
        drawable.deviceAnchor = anchor   // replaced below by the game frame's own pose when known
        if anchor == nil { noAnchor += 1 } else { anchors += 1 }
        // Only tracked poses reach the game, and the neutral pose is taken again after 30 of them: the first pose of a session could be wrong (entering VR
        // sometimes put the view far too high, the portal gun at your feet, until VR was entered again)
        if let a = anchor, a.isTracked {
            trackedSinceStart += 1
            if trackedSinceStart == 30 { vr_view_recenter(); vrlog("neutral head pose taken after 30 tracked poses") }
            poseId += 1
            poseRing[Int(poseId % 64)] = (poseId, a)
            var m = a.originFromAnchorTransform
            let id = poseId
            withUnsafePointer(to: &m) { $0.withMemoryRebound(to: Float.self, capacity: 16) { vr_view_set_head_arkit($0, id) } }
        }
        // per-eye tangents and eye distance, once
        let views = drawable.views
        if !tangentsSent && views.count == 2 {
            var th: Float = 0, tv: Float = 0
            for v in views { th = max(th, v.tangents.x, v.tangents.y); tv = max(tv, v.tangents.z, v.tangents.w) }
            for (i, v) in views.enumerated() { vrlog("view \(i) tangents left \(v.tangents.x) right \(v.tangents.y) top \(v.tangents.z) bottom \(v.tangents.w)") }
            th *= fovH; tv *= fovV
            vrlog("game eye field of view: horizontal \(fovH) and vertical \(fovV) of the display's, tangents \(th) / \(tv) (±\(atan(th) * 57.3)° x ±\(atan(tv) * 57.3)°)")
            vr_view_set_eye_tangents(th, tv)
            if offCentre {   // each eye renders exactly its own (narrowed) frustum through Source's off-centre projection (hle_vrview.c)
                for (i, v) in views.enumerated() where i < 2 {
                    let f = eyeFrustum(v)
                    vr_view_set_eye_frustum(Int32(i), f.x, f.y, f.w, f.z)
                    vrlog("eye \(i) frustum tangents left \(f.x) right \(f.y) top \(f.z) bottom \(f.w)")
                }
            }
            vr_view_set_ipd_m(simd_distance(views[0].transform.columns.3, views[1].transform.columns.3))
            tangentsSent = true
            vrlog("eye tangents h \(th) v \(tv), eye distance \(simd_distance(views[0].transform.columns.3, views[1].transform.columns.3)) m, drawable \(drawable.colorTextures[0].width)x\(drawable.colorTextures[0].height)")
        }
        // Aim: the tracked right hand, else the head's forward
        var aimDir: SIMD3<Float>? = nil
        var aimTarget: SIMD3<Float>? = nil
        if handAim, hands.state == .running {
            let (_, right) = hands.handAnchors(at: seconds)
            if let r = right, r.isTracked, let sk = r.handSkeleton {
                let w = r.originFromAnchorTransform * sk.joint(.wrist).anchorFromJointTransform.columns.3
                let k = r.originFromAnchorTransform * sk.joint(.middleFingerKnuckle).anchorFromJointTransform.columns.3
                let d = SIMD3<Float>(k.x - w.x, k.y - w.y, k.z - w.z)
                if simd_length(d) > 0.01 {
                    let hand = fPos.filter(SIMD3<Float>(k.x, k.y, k.z), t: seconds)
                    let dir = simd_normalize(fDir.filter(simd_normalize(d), t: seconds))
                    aimTarget = hand + dir * aimDist
                    let head = SIMD3<Float>(anchor?.originFromAnchorTransform.columns.3.x ?? 0, anchor?.originFromAnchorTransform.columns.3.y ?? 0, anchor?.originFromAnchorTransform.columns.3.z ?? 0)
                    aimDir = simd_normalize(aimTarget! - head)   // the game's eye aims at the hand ray's point
                    var dv = [aimDir!.x, aimDir!.y, aimDir!.z]
                    vr_view_set_aim_arkit(&dv)
                    // The gun model: its muzzle at the hand, along the hand's ray (hle_vrview.c calc_viewmodel)
                    var hp = [hand.x, hand.y, hand.z], hd = [dir.x, dir.y, dir.z]
                    vr_view_set_hand_arkit(&hp, &hd)
                }
            }
        }
        if (aimDir != nil) != handSeen { handSeen = aimDir != nil; vrlog(handSeen ? "right hand tracked: aiming with the hand" : "right hand lost: aiming with the head") }
        guard let cb = queue.makeCommandBuffer() else { return }
        var texP: UnsafeMutableRawPointer?, evP: UnsafeMutableRawPointer?, value: UInt64 = 0, framePose: UInt64 = 0
        var game: MTLTexture?
        var flatFrame = false
        let pattern = testPattern || frames < 270   // the first 3 s show the test grid: tells a composite problem from a game-frame problem
        if !pattern, vr_tap_latest(&texP, &evP, &value, &framePose) != 0, let tp = texP, let ep = evP {
            let ev = Unmanaged<MTLSharedEvent>.fromOpaque(ep).takeUnretainedValue()
            cb.encodeWaitForEvent(ev, value: value)
            game = Unmanaged<MTLTexture>.fromOpaque(tp).takeUnretainedValue()
            let slot = poseRing[Int(framePose % 64)]
            if framePose != 0, slot.0 == framePose, let a = slot.1 { drawable.deviceAnchor = a; reprojected += 1 }
            flatFrame = framePose == 0
        }
        let th = (views.map { max($0.tangents.x, $0.tangents.y) }.max() ?? 1) * fovH
        let tv = (views.map { max($0.tangents.z, $0.tangents.w) }.max() ?? 1) * fovV
        let symmetric = SIMD4<Float>(th, th, tv, tv)
        let srgb: Float = (game?.pixelFormat == .rgba8Unorm_srgb || game?.pixelFormat == .bgra8Unorm_srgb) ? 1 : 0
        // The crosshair: 2 m from the head along the aim, in each eye's tangents (relative to the pose this drawable is shown with)
        let shownHead = drawable.deviceAnchor?.originFromAnchorTransform ?? matrix_identity_float4x4
        let headPos = SIMD3<Float>(shownHead.columns.3.x, shownHead.columns.3.y, shownHead.columns.3.z)
        let fwd = -SIMD3<Float>(shownHead.columns.2.x, shownHead.columns.2.y, shownHead.columns.2.z)
        let aimPoint = aimTarget ?? (headPos + fwd * 2.0)
        let pgState = vr_view_portal_state()   // -1 unknown, 0 no portal gun (no crosshair), else which colours can fire / are placed
        for (i, view) in views.enumerated() {
            let rp = MTLRenderPassDescriptor()
            rp.colorAttachments[0].texture = drawable.colorTextures[i]
            rp.colorAttachments[0].loadAction = .clear
            rp.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            rp.colorAttachments[0].storeAction = .store
            rp.depthAttachment.texture = drawable.depthTextures[i]
            rp.depthAttachment.loadAction = .clear
            rp.depthAttachment.clearDepth = 0   // reverse Z: far
            rp.depthAttachment.storeAction = .store
            guard let enc = cb.makeRenderCommandEncoder(descriptor: rp) else { continue }
            enc.setViewport(view.textureMap.viewport)
            if let g = game ?? (pattern ? dummy : nil) {
                enc.setRenderPipelineState(pipeline)
                enc.setDepthStencilState(depthState)
                let eyeFromWorld = simd_inverse(shownHead * view.transform)
                let pe = eyeFromWorld * SIMD4<Float>(aimPoint.x, aimPoint.y, aimPoint.z, 1)
                let xh = pe.z < -0.01 ? SIMD2<Float>(pe.x / -pe.z, pe.y / -pe.z) : SIMD2<Float>(0, 0)
                var p = (view.tangents, offCentre ? eyeFrustum(view) : symmetric, Float(i), srgb, Float(pattern ? 1 : 0), Float(flatFrame ? 1 : 0), Float(crosshair && pgState != 0 && pe.z < -0.01 ? 1 : 0), xh.x, xh.y, Float(pgState))
                enc.setFragmentBytes(&p, length: MemoryLayout.size(ofValue: p), index: 0)
                enc.setFragmentTexture(g, index: 0)
                // Depth: visionOS reprojects with the submitted depth, and a reverse-Z depth of 0 (the clear) means "nothing here": a picture with
                // only cleared depth is shown black (the first immersive runs; Klepton's KleptonCompositor.swift notes the same). The picture is
                // written at a constant 3 m for now: reverse-Z with near plane n puts distance d at n / d
                let near = min(drawable.depthRange.x, drawable.depthRange.y)
                var depth = max(near, 0.01) / 3.0
                enc.setVertexBytes(&depth, length: MemoryLayout<Float>.size, index: 0)
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
            enc.endEncoding()
        }
        drawable.encodePresent(commandBuffer: cb)
        cb.commit()
        frame.endSubmission()
        frames += 1
        if frames == 270 { vrlog("test grid done; game frames from now (head poses \(anchors) tracked / \(noAnchor) missing)") }
        if frames == 1 || frames % 900 == 0 { vrlog("composite frame \(frames), game frame \(value), source \(game.map { "\($0.width)x\($0.height) format \($0.pixelFormat.rawValue)" } ?? "none"), head poses \(anchors) tracked / \(noAnchor) missing, frames shown at their render pose \(reprojected), test pattern \(testPattern)") }
    }
}

/// One Euro filter (Casiez et al. 2012) on a 3-vector: low jitter at rest, little lag in fast motion
struct OneEuro3 {
    let minCutoff: Float, beta: Float, dCutoff: Float = 1.0
    var x: SIMD3<Float>? = nil, dx = SIMD3<Float>(repeating: 0), last: Double = 0
    init(minCutoff: Float, beta: Float) { self.minCutoff = minCutoff; self.beta = beta }
    private func alpha(_ cutoff: Float, _ dt: Float) -> Float { let tau = 1 / (2 * Float.pi * cutoff); return 1 / (1 + tau / dt) }
    mutating func filter(_ v: SIMD3<Float>, t: Double) -> SIMD3<Float> {
        guard let px = x, t > last else { x = v; last = t; return v }
        let dt = Float(min(max(t - last, 1.0 / 240), 0.1)); last = t
        let ndx = (v - px) / dt
        dx = dx + (ndx - dx) * alpha(dCutoff, dt)
        let cutoff = minCutoff + beta * simd_length(dx)
        let nx = px + (v - px) * alpha(cutoff, dt)
        x = nx; return nx
    }
}
