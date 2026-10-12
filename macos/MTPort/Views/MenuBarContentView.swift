import SwiftUI

struct MenuBarContentView: View {
    @Bindable var model: DeviceViewModel
    @State private var showDoctor = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            Divider()
            if model.devices.isEmpty {
                emptyHelp
            } else {
                ForEach(model.devices) { device in
                    DeviceRowView(
                        device: device,
                        busy: model.busyDevices.contains(device.deviceId),
                        onMount: { model.mount(device) },
                        onOpen: { model.openInFinder(device) },
                        onEject: { model.eject(device) })
                    if device.state == "mounted" || device.state == "draining", let t = model.transfer(for: device) {
                        TransferView(transfer: t, throughput: model.throughput)
                    }
                }
            }
            if let err = model.lastError {
                HStack(alignment: .top) {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.red)
                    Text(err).font(.caption).textSelection(.enabled)
                    Spacer()
                    Button { model.dismissError() } label: { Image(systemName: "xmark") }.buttonStyle(.plain)
                }
            }
            Divider()
            settings
            HStack {
                Button("action.diagnostics") { model.runDoctor(); showDoctor = true }
                Spacer()
                Button("action.quit") { NSApplication.shared.terminate(nil) }
            }
        }
        .padding(14)
        .frame(width: 360)
        .sheet(isPresented: $showDoctor) { DoctorView(result: model.doctor) { showDoctor = false } }
    }

    private var header: some View {
        HStack {
            Image(systemName: model.overall.symbol).font(.title3)
            VStack(alignment: .leading) {
                Text("app.name").font(.headline)
                Text(LocalizedStringKey(model.overall.titleKey)).font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
        }
    }

    @ViewBuilder private var emptyHelp: some View {
        switch model.overall {
        case .agentStarting: Text("help.agentStarting").font(.caption)
        case .safeToDisconnect: Text("help.safeToDisconnect").font(.caption)
        default: Text("help.noPhone").font(.caption)
        }
    }

    private var settings: some View {
        VStack(alignment: .leading, spacing: 4) {
            Toggle("setting.autoMount", isOn: Binding(get: { model.autoMount }, set: { model.setAutoMount($0) }))
            Toggle("setting.launchAtLogin", isOn: Binding(get: { model.launchAtLogin }, set: { model.setLaunchAtLogin($0) }))
            Text("setting.noCloud").font(.caption2).foregroundStyle(.secondary)
        }
        .toggleStyle(.switch)
        .controlSize(.small)
    }
}

struct DoctorView: View {
    let result: DoctorResult?
    let close: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("action.diagnostics").font(.headline)
            if let result {
                ScrollView {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(result.checks) { c in
                            VStack(alignment: .leading, spacing: 1) {
                                Text("[\(c.status)] \(c.summary)").font(.caption)
                                if !c.remedy.isEmpty { Text("→ \(c.remedy)").font(.caption2).foregroundStyle(.secondary) }
                            }
                        }
                    }
                }.frame(maxHeight: 320)
            } else {
                ProgressView()
            }
            HStack { Spacer(); Button("action.close", action: close) }
        }
        .padding(16)
        .frame(width: 420)
    }
}
