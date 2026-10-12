import SwiftUI

struct TransferView: View {
    let transfer: TransferInfo
    let throughput: Double

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("transfer.title").font(.subheadline.bold())
            Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 2) {
                GridRow { Text("transfer.read"); Text(ByteFormat.string(transfer.bytesRead)).monospacedDigit() }
                GridRow { Text("transfer.uploaded"); Text(ByteFormat.string(transfer.bytesUploaded)).monospacedDigit() }
                GridRow { Text("transfer.files"); Text("\(transfer.uploadsOk) / \(transfer.uploadsFailed)").monospacedDigit() }
                GridRow { Text("transfer.speed"); Text(ByteFormat.rate(throughput)).monospacedDigit() }
            }
            .font(.caption)
            if transfer.uploadsActive > 0, transfer.uploadBytesTotal > 0 {
                ProgressView(value: Double(transfer.uploadBytesDone), total: Double(transfer.uploadBytesTotal))
            }
            if transfer.uploadsFailed > 0 || transfer.recoverableUploads > 0 {
                Label("transfer.preserved \(transfer.recoverableUploads)", systemImage: "exclamationmark.triangle.fill")
                    .font(.caption).foregroundStyle(.orange)
                Text("transfer.preservedHint").font(.caption2).foregroundStyle(.secondary)
            }
            Text("transfer.cancelUnsupported").font(.caption2).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }
}
