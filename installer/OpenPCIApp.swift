import SwiftUI
import Combine
import SystemExtensions

final class ExtensionController: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    static let extensionID = "org.openpci.driver"
    @Published var status = "OpenPCI is ready to install after its build and signature have been verified."
    @Published var requestPending = false

    func submit(activate: Bool) {
        guard !requestPending else { return }
        requestPending = true
        status = activate ? "Submitting extension activation request…" : "Submitting extension deactivation request…"
        let request = activate
            ? OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: Self.extensionID, queue: .main)
            : OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: Self.extensionID, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        status = "macOS requires your approval in System Settings."
        requestPending = false
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        status = result == .completed ? "Extension request completed." : "macOS will complete this request after a restart."
        requestPending = false
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        status = "Extension request failed: \(error.localizedDescription)"
        requestPending = false
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        // Reached only after the explicit Activate/update button request.
        // Launching the app never installs, updates or activates anything.
        status = "Updating OpenPCI from \(existing.bundleVersion) to \(replacement.bundleVersion)…"
        return .replace
    }
}

struct OpenPCIApp: App {
    @StateObject private var controller = ExtensionController()
    var body: some Scene {
        WindowGroup("OpenPCI") {
            VStack(alignment: .leading, spacing: 16) {
                Text("OpenPCI").font(.title2)
                Text("AMD · NVIDIA · Intel · external PCI display-class devices")
                Text(controller.status).textSelection(.enabled)
                Text("Authorized applications can use an external GPU through the OpenPCI SDK. Activate the extension here, then choose the GPU from your application.")
                    .font(.callout)
                HStack {
                    Button("Activate or update transport") { controller.submit(activate: true) }
                    Button("Deactivate transport") { controller.submit(activate: false) }
                }.disabled(controller.requestPending)
            }.padding(24).frame(width: 580)
        }
    }
}
