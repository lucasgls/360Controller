// XboxDriverApp.swift
// App container obrigatório para distribuir DriverKit extensions.
// Gerencia o ciclo de vida (install/uninstall) dos dois dexts.
// GPL v2 — see Licence.txt

import SwiftUI
import SystemExtensions

@main
struct XboxDriverApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .windowResizability(.contentSize)
    }
}

// MARK: - DriverKit Extension IDs

private let kReceiverDriverID    = "com.360Controller.XboxReceiverDriver"
private let kControllerDriverID  = "com.360Controller.XboxControllerDriver"

// MARK: — Main View

struct ContentView: View {
    @StateObject private var installer = DriverInstaller()

    var body: some View {
        VStack(spacing: 24) {
            // Header
            HStack(spacing: 12) {
                Image(systemName: "gamecontroller.fill")
                    .font(.system(size: 36))
                    .foregroundStyle(.green)
                VStack(alignment: .leading) {
                    Text("Xbox 360 Wireless Driver")
                        .font(.title2.bold())
                    Text("macOS 26 — DriverKit Port")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }

            Divider()

            // Status
            HStack {
                Circle()
                    .fill(installer.isInstalled ? Color.green : Color.orange)
                    .frame(width: 10, height: 10)
                Text(installer.statusMessage)
                    .font(.callout)
                Spacer()
            }

            // Action buttons
            HStack(spacing: 12) {
                Button("Install Driver") {
                    installer.install()
                }
                .buttonStyle(.borderedProminent)
                .disabled(installer.isInstalled || installer.isBusy)

                Button("Uninstall Driver") {
                    installer.uninstall()
                }
                .buttonStyle(.bordered)
                .disabled(!installer.isInstalled || installer.isBusy)
            }

            // Info
            GroupBox {
                VStack(alignment: .leading, spacing: 6) {
                    Label("Supports up to 4 simultaneous wireless controllers",
                          systemImage: "wifi")
                    Label("Requires the Xbox 360 Wireless Gaming Receiver dongle",
                          systemImage: "cable.connector")
                    Label("Rumble and LED support included",
                          systemImage: "waveform")
                }
                .font(.caption)
            }
        }
        .padding(24)
        .frame(width: 420)
    }
}

// MARK: — Installer

class DriverInstaller: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    @MainActor @Published var isInstalled  = false
    @MainActor @Published var isBusy       = false
    @MainActor @Published var statusMessage = "Checking driver status…"

    override init() {
        super.init()
        checkStatus()
    }

    func checkStatus() {
        // Heuristic: query IOKit registry for a matching service
        // A production implementation would use IOServiceGetMatchingService.
        Task { @MainActor in
            isInstalled    = false
            statusMessage  = "Not installed — click Install to activate the driver."
        }
    }

    func install() {
        Task { @MainActor in
            isBusy        = true
            statusMessage = "Installing receiver driver…"
        }
        let req = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: kReceiverDriverID,
            queue: .main
        )
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    func installControllerDriver() {
        Task { @MainActor in
            statusMessage = "Installing controller driver…"
        }
        let req = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: kControllerDriverID,
            queue: .main
        )
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    func uninstall() {
        Task { @MainActor in
            isBusy        = true
            statusMessage = "Uninstalling…"
        }
        for id in [kReceiverDriverID, kControllerDriverID] {
            let req = OSSystemExtensionRequest.deactivationRequest(
                forExtensionWithIdentifier: id,
                queue: .main
            )
            req.delegate = self
            OSSystemExtensionManager.shared.submitRequest(req)
        }
    }

    // MARK: Delegate

    func request(_ request: OSSystemExtensionRequest,
                             actionForReplacingExtension existing: OSSystemExtensionProperties,
                             withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        Task { @MainActor in
            self.statusMessage = "Approval needed — check System Settings → Privacy & Security"
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                             didFinishWithResult result: OSSystemExtensionRequest.Result) {
        Task { @MainActor in
            if request.identifier == kReceiverDriverID && result == .completed {
                // Chain: install controller driver after receiver driver
                self.installControllerDriver()
                return
            }
            self.isBusy       = false
            self.isInstalled  = (result == .completed)
            self.statusMessage = result == .completed
                ? "Driver installed and active ✓"
                : "Operation completed (restart may be required)"
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                             didFailWithError error: Error) {
        Task { @MainActor in
            self.isBusy       = false
            self.statusMessage = "Error: \(error.localizedDescription)"
        }
    }
}
