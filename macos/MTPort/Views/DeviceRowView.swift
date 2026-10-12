import SwiftUI

struct DeviceRowView: View {
    let device: DeviceInfo
    let busy: Bool
    let onMount: () -> Void
    let onOpen: () -> Void
    let onEject: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline) {
                Image(systemName: "iphone.gen3").foregroundStyle(.secondary)
                Text(device.name).font(.headline).lineLimit(2).fixedSize(horizontal: false, vertical: true)
                Spacer()
                Text(LocalizedStringKey("device.state." + device.state)).font(.caption).foregroundStyle(.secondary)
            }
            if device.capacityBytes > 0 {
                Text("device.capacity \(ByteFormat.string(device.freeBytes)) \(ByteFormat.string(device.capacityBytes))")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if device.state == "awaitingMtp" {
                Text("hint.selectFileTransfer").font(.caption)
            } else if !device.detail.isEmpty, device.state == "error" {
                Text(device.detail).font(.caption).foregroundStyle(.red).textSelection(.enabled)
            }
            HStack {
                switch device.state {
                case "detected", "error":
                    Button("action.mount", action: onMount).disabled(busy || device.deviceId.isEmpty)
                    Text("hint.readOnlyDefault").font(.caption2).foregroundStyle(.secondary)
                case "mounted":
                    Button("action.openInFinder", action: onOpen)
                    Button("action.eject", action: onEject).disabled(busy)
                case "connecting", "draining":
                    ProgressView().controlSize(.small)
                default: EmptyView()
                }
            }
            if device.state == "mounted", !device.readOnly {
                Text("hint.readWrite").font(.caption2).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.vertical, 4)
    }
}
