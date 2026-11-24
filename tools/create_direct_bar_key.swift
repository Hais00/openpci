import CryptoKit
import Darwin
import Foundation

guard CommandLine.arguments.count == 3 else {
    fatalError("Usage: swift tools/create_direct_bar_key.swift PRIVATE_KEY PUBLIC_HEADER")
}

let seedPath = CommandLine.arguments[1]
let header = URL(fileURLWithPath: CommandLine.arguments[2])
let identity = Curve25519.Signing.PrivateKey()
let seed = identity.rawRepresentation
let fd = open(seedPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
guard fd >= 0 else { fatalError("Could not create a new private key file") }
let written = seed.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
let closed = close(fd)
guard written == seed.count, closed == 0 else {
    unlink(seedPath)
    fatalError("Could not write the private key")
}

let publicBytes = identity.publicKey.rawRepresentation.map { String(format: "0x%02x", $0) }
let declarations = """
static constexpr bool openpci_direct_bar_key_configured = true;
static constexpr uint8_t openpci_direct_bar_public_key[32] = {\(publicBytes.joined(separator: ", "))};

"""
do {
    try Data(declarations.utf8).write(to: header, options: .withoutOverwriting)
} catch {
    unlink(seedPath)
    throw error
}
