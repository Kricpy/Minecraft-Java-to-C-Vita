#!/usr/bin/env python3
"""
Minecraft 1.12.2 Protocol Analyzer
Connects to server, performs login, and dumps all packets with timing.
Helps diagnose keepalive timeout and packet ID issues.
"""
import socket
import struct
import time
import sys
import threading

SERVER_HOST = "kreqxs.aternos.me"
SERVER_PORT = 11113
PROTOCOL_VERSION = 340
USERNAME = "ProtocolBot"

def pack_varint(value):
    """Pack a value as a Minecraft varint."""
    data = b""
    while True:
        byte = value & 0x7F
        value >>= 7
        if value != 0:
            byte |= 0x80
            data += struct.pack("B", byte)
        else:
            data += struct.pack("B", byte)
            break
    return data

def unpack_varint(sock):
    """Unpack a varint from socket."""
    value = 0
    shift = 0
    while True:
        byte = sock.recv(1)[0]
        value |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            break
        shift += 7
    return value

def pack_string(s):
    """Pack a UTF-8 string with varint length prefix."""
    encoded = s.encode("utf-8")
    return pack_varint(len(encoded)) + encoded

def recv_packet(sock):
    """Receive a full packet from socket. Returns (timestamp, raw_bytes)."""
    start = time.time()
    length = unpack_varint(sock)
    if length <= 0:
        return None, None
    
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            break
        data += chunk
    
    elapsed = time.time() - start
    return elapsed, data, length

def send_packet(sock, packet_id, data=b""):
    """Send a packet with given ID and data."""
    packet = pack_varint(packet_id) + data
    sock.send(pack_varint(len(packet)) + packet)

def hex_dump(data):
    """Format bytes as hex string."""
    return " ".join(f"{b:02x}" for b in data)

def analyze_server():
    """Main analysis function."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30)
    
    print(f"Connecting to {SERVER_HOST}:{SERVER_PORT}...")
    connect_start = time.time()
    sock.connect((SERVER_HOST, SERVER_PORT))
    print(f"Connected in {time.time() - connect_start:.3f}s")
    
    receive_times = []
    keepalive_times = []
    packet_count = 0
    
    try:
        # === HANDSHAKE (0x00) ===
        # protocol_version (varint), server_host (string), server_port (ushort), next_state (varint)
        host_bytes = pack_string(SERVER_HOST)
        port_bytes = struct.pack(">H", SERVER_PORT)
        next_state_bytes = pack_varint(1)  # Login state
        
        handshake_data = pack_varint(PROTOCOL_VERSION) + host_bytes + port_bytes + next_state_bytes
        send_packet(sock, 0x00, handshake_data)
        print("[->] Sent Handshake (id=0x00)")
        
        # === LOGIN START (0x00 in Login state) ===
        # In 1.12.2, Login Start only contains player name (no UUID)
        login_data = pack_string(USERNAME)
        send_packet(sock, 0x00, login_data)
        print("[->] Sent Login Start (id=0x00)")
        
        # === RECEIVE PHASE ===
        # First, receive Encryption Request (0x01) or we go straight to play
        start_time = time.time()
        print(f"\n--- Receiving packets (since {time.time():.3f}) ---")
        
        while True:
            try:
                elapsed, data, length = recv_packet(sock)
                if data is None:
                    print("Connection closed by server")
                    break
                    
                packet_count += 1
                total_elapsed = time.time() - start_time
                
                if len(data) < 1:
                    print(f"[{total_elapsed:.3f}s] Short packet ({length} bytes)")
                    continue
                
                packet_id = data[0] & 0xFF
                packet_data = data[1:]
                
                # Classify packet
                packet_names = {
                    0x00: "Login Success (Keep 0x00)",
                    0x01: "Encryption Request",
                    0x02: "Login Success",
                    0x30: "Spawn Object",
                    0x25: "Join Game",
                    0x26: "Respawn",
                    0x27: "Player Position And Look (Clientbound)",
                    0x1F: "Keep Alive (Clientbound)",
                    0x1F: "Keep Alive (Plugin Message?)",
                    0x1F: "Keep Alive",
                }
                
                # Determine state based on packet_id and data
                name = packet_names.get(packet_id, f"Unknown 0x{packet_id:02X}")
                
                print(f"[{total_elapsed:.3f}s | +{elapsed*1000:.1f}ms] Packet #{packet_count} (id=0x{packet_id:02X}, {length}B) - {name}")
                
                if packet_id == 0x01:
                    # Encryption Request -> respond with Encryption Response (offline mode compatible)
                    print("  Encryption Request received")
                    uuid = "00000000-0000-0000-0000-000000000001"
                    # Encryption Response: UUID (string), Shared Secret (empty byte array), Verify Token (empty byte array)
                    resp_data = pack_string(uuid) + pack_varint(0) + pack_varint(0)
                    send_packet(sock, 0x01, resp_data)
                    print("[->] Sent Encryption Response (id=0x01)")
                    
                elif packet_id == 0x02 or packet_id == 0x00:
                    # Login Success → now we're in Play state
                    print("  Login Success - entering Play state")
                    
                elif packet_id == 0x1F:
                    # Keep Alive (clientbound)
                    keepalive_times.append(time.time())
                    payload = data[1:] if len(data) > 1 else b""
                    keepalive_id = None
                    if len(payload) >= 8:
                        keepalive_id = struct.unpack(">q", payload[:8])[0]
                    print(f"  Keep Alive (payload={keepalive_id}, time={time.time():.3f})")
                    
                    # Immediately send keepalive response (0x0B serverbound)
                    send_packet(sock, 0x0B, payload[:8] if len(payload) >= 8 else b"\x00\x00\x00\x00\x00\x00\x00\x00")
                    response_time = time.time()
                    print(f"[-> {time.time() - start_time:.3f}s] Sent Keep Alive Response (0x0B)")
                    
                elif packet_id == 0x25:
                    # Join Game
                    print(f"  Join Game received")
                    
                elif packet_id == 0x27:
                    # Player Position And Look (clientbound)
                    print(f"  Position And Look received")
                    # Respond with Position And Look (0x0E) to complete teleport
                    # Send a dummy position response
                    x, y, z, yaw, pitch, flags_byte = struct.unpack("<dqdddff", packet_data[:40] if len(packet_data) >= 40 else packet_data.ljust(40, b"\x00"))
                    # Send Player Position And Look (0x0E) response with teleport confirm
                    # Actually, first we should send Teleport Confirm (0x00) with the given teleport ID
                    # But we don't know the teleport ID without parsing the packet properly
                    print(f"  Position: ({x}, {y}, {z}), Yaw: {yaw}, Pitch: {pitch}")
                    
                elif packet_id == 0x00:
                    # Could be Spawn Object or other
                    print(f"  (0x00 packet, possibly Spawn Object or status)")
                
                # Log raw bytes for unknown packets
                if length > 10:
                    print(f"  First 16 bytes: {hex_dump(data[:16])}...")
                    
            except socket.timeout:
                print(f"\nSocket timeout at {time.time() - start_time:.3f}s")
                break
            except ConnectionResetError:
                print(f"\nConnection reset by server at {time.time() - start_time:.3f}s")
                break
            except Exception as e:
                print(f"\nError at {time.time() - start_time:.3f}s: {e}")
                import traceback
                traceback.print_exc()
                break
                
    finally:
        sock.close()
        print(f"\n=== Summary ===")
        print(f"Total packets received: {packet_count}")
        print(f"Keep alive packets received: {len(keepalive_times)}")
        if keepalive_times:
            print(f"Keep alive times: {[f'{t - start_time:.3f}s' for t in keepalive_times]}")
            if len(keepalive_times) > 1:
                intervals = [keepalive_times[i] - keepalive_times[i-1] for i in range(1, len(keepalive_times))]
                print(f"Keep alive intervals: {[f'{i:.3f}s' for i in intervals]}")
        elapsed = time.time() - start_time
        print(f"Session duration: {elapsed:.3f}s")

if __name__ == "__main__":
    analyze_server()
