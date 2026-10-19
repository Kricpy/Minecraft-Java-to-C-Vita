#!/usr/bin/env python3
"""
Minecraft 1.12.2 Protocol Analyzer v2
Handles compression, encryption (offline mode), and dumps keepalive timing.
"""
import socket
import struct
import time
import zlib
import sys

SERVER_HOST = "kreqxs.aternos.me"
SERVER_PORT = 11113
PROTOCOL_VERSION = 340
USERNAME = "ProtocolBot"

def pack_varint(value):
    data = b""
    while True:
        byte = value & 0x7F
        value >>= 7
        if value != 0:
            byte |= 0x80
        data += struct.pack("B", byte)
        if value == 0:
            break
    return data

def unpack_varint(data, pos):
    value = 0
    shift = 0
    while pos < len(data):
        b = data[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not (b & 0x80):
            break
        shift += 7
    return value, pos

def pack_string(s):
    encoded = s.encode("utf-8")
    return pack_varint(len(encoded)) + encoded

def read_varint(sock, max_bytes=5):
    value = 0
    shift = 0
    bytes_read = 0
    while bytes_read < max_bytes:
        b = sock.recv(1)
        if not b:
            raise ConnectionError("Connection closed while reading varint")
        b = b[0]
        value |= (b & 0x7F) << shift
        bytes_read += 1
        if not (b & 0x80):
            break
        shift += 7
    return value

def read_packet(sock):
    """Read a length-prefixed packet. Returns (packet_id, payload_bytes)."""
    length = read_varint(sock)
    if length <= 0:
        return None, None
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise ConnectionError("Connection closed while reading packet")
        data += chunk
    packet_id, pos = unpack_varint(data, 0)
    return packet_id, data[pos:], length

def send_packet(sock, packet_id, data=b""):
    packet = pack_varint(packet_id) + data
    sock.send(pack_varint(len(packet)) + packet)

def send_packet_compressed(sock, packet_id, data=b"", comp_threshold=256):
    """Send a packet, wrapping with compression if needed."""
    if comp_threshold >= 0 and len(data) + 1 > comp_threshold:
        # Compress the (packet_id + data)
        uncompressed = pack_varint(packet_id) + data
        compressed = zlib.compress(uncompressed)
        # Format: [data_length_varint (uncompressed size)][packet_id_varint][compressed_data]
        # Wait no, the format with compression is:
        # [data_length_varint (uncompressed length)] [if >0: compressed (packet_id + body)]
        # [frame_length] [data_length] [compressed_data]
        wire_data = pack_varint(len(uncompressed)) + compressed
    else:
        # Uncompressed: data_length=0, then packet_id + data
        wire_data = pack_varint(0) + pack_varint(packet_id) + data
    
    sock.send(pack_varint(len(wire_data)) + wire_data)

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30)
    
    print(f"Connecting to {SERVER_HOST}:{SERVER_PORT}...")
    connect_start = time.time()
    sock.connect((SERVER_HOST, SERVER_PORT))
    print(f"Connected in {time.time() - connect_start:.3f}s")
    
    try:
        # === HANDSHAKE (next_state=2 for Login) ===
        host_bytes = pack_string(SERVER_HOST)
        port_bytes = struct.pack(">H", SERVER_PORT)
        handshake = pack_varint(340) + host_bytes + port_bytes + pack_varint(2)
        send_packet(sock, 0x00, handshake)
        print("[->] Sent Handshake (next_state=2)")
        
        time.sleep(0.1)
        
        # === LOGIN START ===
        send_packet(sock, 0x00, pack_string(USERNAME))
        print("[->] Sent Login Start")
        
        start_time = time.time()
        comp_threshold = -1
        encrypted = False
        keepalive_times = []
        
        print(f"\n--- Receiving (since {time.time():.3f}) ---")
        
        while True:
            try:
                pid, payload, pkt_len = read_packet(sock)
                if pid is None:
                    break
                    
                elapsed = time.time() - start_time
                
                # Login phase packets
                if pid == 0x01:
                    print(f"[{elapsed:.3f}s] Encryption Request (Login 0x01)")
                    # Parse and respond
                    pos = 0
                    uuid_len, pos = unpack_varint(payload, pos)
                    pos += uuid_len
                    pk_len, pos = unpack_varint(payload, pos)
                    pos += pk_len
                    tok_len, pos = unpack_varint(payload, pos)
                    pos += tok_len
                    
                    # Send Encryption Response (empty key/token for offline)
                    resp = pack_string("00000000-0000-0000-0000-000000000001")
                    resp += pack_varint(0)  # empty shared secret
                    resp += pack_varint(0)  # empty verify token
                    send_packet(sock, 0x01, resp)
                    print(f"[{elapsed:.3f}s] -> Sent Encryption Response")
                    
                elif pid == 0x02:
                    # Login Success
                    pos = 0
                    uuid_len, pos = unpack_varint(payload, pos)
                    uuid = payload[pos:pos+uuid_len].decode('utf-8', errors='replace')
                    pos += uuid_len
                    name_len, pos = unpack_varint(payload, pos)
                    name = payload[pos:pos+name_len].decode('utf-8', errors='replace')
                    print(f"[{elapsed:.3f}s] Login Success uuid={uuid} name={name}")
                    
                elif pid == 0x03:
                    # Set Compression
                    threshold, pos = unpack_varint(payload, 0)
                    comp_threshold = threshold
                    print(f"[{elapsed:.3f}s] Set Compression threshold={comp_threshold}")
                    
                elif pid == 0x00:
                    # Disconnect
                    msg_len, pos = unpack_varint(payload, 0)
                    msg = payload[pos:pos+msg_len].decode('utf-8', errors='replace')
                    print(f"[{elapsed:.3f}s] Disconnect: {msg}")
                    break
                    
                # Play phase packets (after compression may be enabled)
                elif comp_threshold >= 0 and len(payload) > 0:
                    # Try to decompress if compressed
                    data_len, pos = unpack_varint(payload, 0)
                    if data_len == 0:
                        # Uncompressed: just packet_id + body
                        play_pid, body_pos = unpack_varint(payload, pos)
                        body = payload[body_pos:]
                        handle_play_packet(elapsed, play_pid, body, comp_threshold, sock, start_time)
                    elif data_len > 0:
                        # Compressed: decompress
                        compressed_data = payload[pos:]
                        try:
                            decompressed = zlib.decompress(compressed_data)
                            play_pid, body_pos = unpack_varint(decompressed, 0)
                            body = decompressed[body_pos:]
                            handle_play_packet(elapsed, play_pid, body, comp_threshold, sock, start_time)
                        except Exception as e:
                            print(f"[{elapsed:.3f}s] Decompress failed: {e}, raw={compressed_data[:20].hex()}")
                    else:
                        print(f"[{elapsed:.3f}s] Packet (no comp) id=0x{pid:02X} len={pkt_len}")
                else:
                    # No compression yet
                    play_pid, body_pos = unpack_varint(payload, 0) if len(payload) > 0 else (pid, 0)
                    body = payload[body_pos:] if body_pos < len(payload) else b""
                    handle_play_packet(elapsed, play_pid, body, comp_threshold, sock, start_time)
                    
            except socket.timeout:
                print(f"\n[{time.time() - start_time:.1f}s] Read timeout")
                break
            except ConnectionError as e:
                print(f"\n[{time.time() - start_time:.1f}s] {e}")
                break
                
    finally:
        sock.close()
        elapsed = time.time() - start_time
        print(f"\n=== Session Summary ===")
        print(f"Duration: {elapsed:.1f}s")
        print(f"Keepalives received: {len(keepalive_times)}")

def handle_play_packet(elapsed, pid, body, comp_threshold, sock, start_time):
    keepalive_times = []
    packet_names = {
        0x00: "Teleport Confirm (Req)",
        0x01: "Query Block NBT",
        0x04: "Serverbound Plugin Message",
        0x07: "Position (legacy?)",
        0x08: "Use Entity",
        0x09: "Keep Alive (serverbound?)",
        0x0B: "KeepAlive (serverbound 0x0B?)",
        0x0F: "Chat Message",
        0x12: "Player Rotation",
        0x14: "Player Position",
        0x15: "Player Rotation", 
        0x16: "Player Position And Look",
        0x17: "Player (on_ground)",
        0x19: "Vehicle Move",
        0x1F: "KeepAlive (clientbound)",
        0x20: "Chunk Data",
        0x23: "Join Game",
        0x26: "Respawn",
        0x27: "Position And Look (clientbound)",
        0x29: "Held Item",
        0x2C: "Player Abilities",
        0x2D: "ComWindow Items",
        0x2E: "Player Info",
        0x2F: "Position Look",  # hmm duplicate
        0x32: "Spawn Position",
        0x34: "Camera",
        0x35: "Respawn (0x35?)",
        0x45: "Spawn Position (0x45?)",
        0x46: "Spawn Position (0x46)",
        0x4F: "Entity Effect",
        0x52: "Profile Dye",
        0xA0: "Statistics",
        0xC8: "Statistics (0xC8)",
    }
    name = packet_names.get(pid, f"Unknown 0x{pid:02X}")
    print(f"[{elapsed:.3f}s]  Play: id=0x{pid:02X} ({name}) len={len(body)}")
    
    if pid == 0x1F:
        # Keep Alive (clientbound) - respond immediately
        ka_id = struct.unpack(">q", body[:8])[0] if len(body) >= 8 else 0
        keepalive_times.append(time.time())
        print(f"  -> KeepAlive id={ka_id}, responding with 0x0B")
        send_packet_compressed(sock, 0x0B, body[:8], comp_threshold)
        
    elif pid == 0x27:
        # Position And Look (clientbound)
        print(f"  -> Position&Look: need to respond with Teleport Confirm")
        # Parse teleport ID (last varint in the packet)
        if len(body) > 4:
            teleport_id = body[-1] if len(body) < 5 else body[-1]
            # Send Teleport Confirm (0x00)
            send_packet_compressed(sock, 0x00, pack_varint(teleport_id), comp_threshold)
            print(f"  -> Sent Teleport Confirm: {teleport_id}")

if __name__ == "__main__":
    main()
