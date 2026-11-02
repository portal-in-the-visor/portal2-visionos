// The visionOS app's entry point: the game window (nxnative's iOS launcher and game view, front_ios.m) plus an immersive space (PortalCompositor.swift)
// that shows the game's side-by-side stereo frames per eye with the tracked head driving the camera (dm/host/hle_vrview.c).
import SwiftUI
import CompositorServices

@main
struct PortalVisionApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @State private var immersed = false
    init() {
        _ = dm_vision_boot()   // environment, log and game files, before any window (main.m)
        vr_tap_install()
    }
    var body: some Scene {
        WindowGroup(id: "game") {
            GameWindow(immersed: $immersed)
                .onChange(of: scenePhase, initial: true) { _, phase in   // initial: the game hears it is in front at launch (controllers are ignored until then)
                    // The window goes to the background while the immersive space is open; the game must keep running then
                    if immersed { return }
                    switch phase {
                    case .active: front_ios_focus_event(2); front_ios_focus_event(3)
                    case .inactive: front_ios_focus_event(0)
                    case .background: front_ios_focus_event(0); front_ios_focus_event(1)
                    @unknown default: break
                    }
                }
        }
        .windowStyle(.plain)
        .defaultSize(width: 1280, height: 720)

        ImmersiveSpace(id: "vr") {
            CompositorLayer(configuration: PortalLayerConfiguration()) { layerRenderer in
                PortalCompositor.start(layerRenderer)
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
        .upperLimbVisibility(.hidden)   // no passthrough hands over the game (the user: seeing them breaks immersion)
    }
}

struct GameWindow: View {
    @Binding var immersed: Bool
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    var body: some View {
        GameRoot()
            .opacity(immersed ? 0 : 1)   // in a Full Space the app's own window stays visible; the game is shown by the immersive view instead
            .ornament(attachmentAnchor: .scene(.bottom)) {
                Button(immersed ? "Exit VR" : "Enter VR") {
                    Task {
                        if immersed { await dismissImmersiveSpace(); immersed = false }
                        else {
                            immersed = true
                            switch await openImmersiveSpace(id: "vr") {
                            case .opened: break
                            default: immersed = false
                            }
                        }
                    }
                }
                .padding()
                .glassBackgroundEffect()
            }
    }
}

struct GameRoot: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController { front_ios_root() }
    func updateUIViewController(_ vc: UIViewController, context: Context) {}
}
