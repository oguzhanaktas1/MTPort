import SwiftUI

@main
struct MTPortApp: App {
    @State private var model: DeviceViewModel

    init() {
        let m = DeviceViewModel()
        m.start()          // poll from launch, not only when the menu is opened
        _model = State(initialValue: m)
    }

    var body: some Scene {
        // A normal window, shown at launch: the menu bar item alone is easy to miss (crowded menu bar, notch).
        Window("MTPort", id: "main") {
            MenuBarContentView(model: model)
                .onAppear { NSApplication.shared.activate(ignoringOtherApps: true) }
        }
        .windowResizability(.contentSize)
        .defaultLaunchBehavior(.presented)

        MenuBarExtra {
            MenuBarContentView(model: model)
        } label: {
            Label("app.name", systemImage: model.overall.symbol)
        }
        .menuBarExtraStyle(.window)
    }
}
