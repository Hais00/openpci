import Foundation
import SwiftUI
import SystemExtensions
import Darwin

// Explicit process entry points for internal, repeatable lifecycle tests.
// This uses the same OSSystemExtensionManager checks as the GUI; it neither
// changes system security settings nor approves an OS permission prompt.
private final class DriverCommand: NSObject, OSSystemExtensionRequestDelegate {
    private var result: Int32?
    private let activate: Bool

    init(activate: Bool) { self.activate = activate }

    private func emit(_ state: String, _ fields: [String: Any] = [:]) {
        var event = fields
        event["state"] = state
        event["bundleID"] = ExtensionController.extensionID
        event["operation"] = activate ? "activate" : "deactivate"
        if let data = try? JSONSerialization.data(withJSONObject: event, options: [.sortedKeys]),
           let text = String(data: data, encoding: .utf8) {
            print(text)
            fflush(stdout)
        }
    }

    func run() -> Int32 {
        let request = activate
            ? OSSystemExtensionRequest.activationRequest(
                forExtensionWithIdentifier: ExtensionController.extensionID, queue: .main)
            : OSSystemExtensionRequest.deactivationRequest(
                forExtensionWithIdentifier: ExtensionController.extensionID, queue: .main)
        request.delegate = self
        emit("submitted")
        OSSystemExtensionManager.shared.submitRequest(request)
        let deadline = Date().addingTimeInterval(90)
        while result == nil && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        }
        if result == nil { emit("timed-out"); return 5 }
        return result!
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        emit("needs-user-approval")
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult completed: OSSystemExtensionRequest.Result) {
        let immediate = completed == .completed
        emit(immediate ? "completed" : "requires-restart")
        result = immediate ? 0 : 4
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let detail = error as NSError
        emit("failed", ["domain": detail.domain, "code": detail.code,
                        "message": detail.localizedDescription])
        result = 1
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension replacement: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction {
        emit("replacing", ["previousVersion": existing.bundleVersion,
                           "replacementVersion": replacement.bundleVersion])
        return .replace
    }
}

@main
struct OpenPCIEntry {
    static func main() {
        let args = Array(CommandLine.arguments.dropFirst())
        if args == ["--help"] {
            print("OpenPCI [--activate-driver | --deactivate-driver | --help]")
            print("No arguments opens the GUI. Driver commands require explicit invocation.")
            return
        }
        if args == ["--activate-driver"] || args == ["--deactivate-driver"] {
            let command = DriverCommand(activate: args[0] == "--activate-driver")
            let result = withExtendedLifetime(command) { command.run() }
            exit(result)
        }
        OpenPCIApp.main()
    }
}
