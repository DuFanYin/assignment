import struct
import binascii

def inspect_dbn_file(file_path, num_bytes=256):
    """
    Inspect a .dbn file (likely binary MBO or market data stream) to infer structure.
    Shows header bytes, printable text fragments, and byte patterns.
    """
    print(f"Inspecting: {file_path}\n{'='*60}")
    
    with open(file_path, "rb") as f:
        data = f.read(num_bytes)

    # 1️⃣ Show first few bytes (hex)
    print("🔹 First 64 bytes (hex):")
    print(binascii.hexlify(data[:64]).decode("utf-8"))
    print()

    # 2️⃣ Check for ASCII strings (might contain metadata like 'nanomsg', 'schema', etc.)
    printable = ''.join(chr(b) if 32 <= b <= 126 else '.' for b in data)
    print("🔹 Printable characters:")
    print(printable[:200])
    print()

    # 3️⃣ Try common signatures
    if data.startswith(b"\x1f\x8b"):
        print("🧩 Detected gzip-compressed file")
    elif data.startswith(b"PK"):
        print("🧩 Detected ZIP archive")
    elif data[:4] == b"\x89HDF":
        print("🧩 Detected HDF5 file")
    elif data[:4] == b"\x00\x00\x00\x14":
        print("🧩 Possibly protobuf / cap’n proto framed data")
    else:
        print("🧩 No known magic header — likely custom binary format (e.g., MBO .dbn)")

    # 4️⃣ Rough entropy check (to see if binary or text-heavy)
    binary_ratio = sum(1 for b in data if b < 9 or b > 126) / len(data)
    if binary_ratio > 0.5:
        print("🧠 Mostly binary data (likely structured market stream)")
    else:
        print("📄 Text-based or semi-readable format")
    print("="*60)

# Example usage:
inspect_dbn_file("src/data/CLX5_mbo.dbn")