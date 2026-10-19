#!/usr/bin/env python3
"""
Minecraft 1.12.2 Protocol Analyzer v4
Properly handles Login → Play transition with compression.
Focus: keepalive timing and packet flow.
"""
import socket
import struct
import time
import zlib
import sys

SERVER_HOST = "kreqxs.aternos.me"
SERVER_PORT = 11113
USERNAME = f"ProtocolBot{int(time.time()) % 10000}"

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

def read_varint_bytes(data, pos):
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

def read_varint_sock(sock):
    value = 0
    shift = 0
    while True:
        b = sock.recv(1)
        if not b:
            raise ConnectionError("Connection closed")
        b = b[0]
        value |= (b & 0x7F) << shift
        if not (b & 0x80):
            break
        shift += 7
    return value

def send_packet(sock, packet_id, data=b"", comp_threshold=-1):
    pkt = pack_varint(packet_id) + data
    if comp_threshold >= 0:
        if len(pkt) > comp_threshold:
            compressed = zlib.compress(pkt)
            wire = pack_varint(len(pkt)) + compressed
        else:
            wire = pack_varint(0) + pkt
    else:
        wire = pkt
    sock.send(pack_varint(len(wire)) + wire)

def parse_frame(frame, comp_threshold, state):
    """Parse a single frame. Returns (pid, body) or (None, None) if error."""
    if comp_threshold >= 0:
        data_len, pos = read_varint_bytes(frame, 0)
        if data_len == 0:
            # Uncompressed: [packet_id][body]
            pid, ppos = read_varint_bytes(frame, pos)
            body = frame[ppos:]
        else:
            # Compressed
            try:
                decompressed = zlib.decompress(frame[pos:])
                pid, ppos = read_varint_bytes(decompressed, 0)
                body = decompressed[ppos:]
            except Exception as e:
                return None, None, f"decompress error: {e}"
    else:
        pid, pos = read_varint_bytes(frame, 0)
        body = frame[pos:]
    return pid, body, None

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30)
    t0 = time.time()

    print(f"Connecting to {SERVER_HOST}:{SERVER_PORT}...")
    sock.connect((SERVER_HOST, SERVER_PORT))
    print(f"Connected in {time.time()-t0:.3f}s")

    try:
        # Handshake
        hs = pack_varint(340) + pack_varint(len(SERVER_HOST)) + SERVER_HOST.encode() + struct.pack(">H", SERVER_PORT) + pack_varint(2)
        send_packet(sock, 0x00, hs)
        print(f"[{time.time()-t0:.3f}s] -> Handshake")

        # Login Start
        send_packet(sock, 0x00, pack_varint(len(USERNAME)) + USERNAME.encode())
        print(f"[{time.time()-t0:.3f}s] -> Login Start ({USERNAME})")

        # State tracking
        state = "LOGIN"
        comp_threshold = -1
        keepalive_count = 0
        keepalive_times = []
        teleport_ids = []
        packet_count = 0
        join_game_received = False
        spawn_pos_received = False
        pos_look_received = False
        last_chunk_log = 0

        while True:
            try:
                # Read frame length
                flen = read_varint_sock(sock)
                if flen <= 0:
                    print(f"[{time.time()-t0:.3f}s] Zero-length frame")
                    break

                frame = b""
                while len(frame) < flen:
                    chunk = sock.recv(flen - len(frame))
                    if not chunk:
                        raise ConnectionError("Connection closed mid-frame")
                    frame += chunk

                elapsed = time.time() - t0
                packet_count += 1

                # Parse frame (handle compression if threshold is set)
                pid, body, err = parse_frame(frame, comp_threshold, state)
                if pid is None:
                    if err:
                        # Try without compression (maybe threshold was set but this packet is uncompressed)
                        if comp_threshold >= 0:
                            # Try treating as uncompressed (data_length should be 0)
                            try:
                                data_len, pos = read_varint_bytes(frame, 0)
                                if data_len == 0:
                                    pid, ppos = read_varint_bytes(frame, pos)
                                    body = frame[ppos:]
                                    err = None
                            except:
                                pass
                    if pid is None:
                        print(f"[{elapsed:.3f}s] Parse error: {err}, frame={frame[:20].hex()}")
                        continue

                if state == "LOGIN":
                    if pid == 0x00:
                        # Disconnect
                        slen, sp = read_varint_bytes(body, 0)
                        msg = body[sp:sp+slen].decode('utf-8', errors='replace')
                        print(f"[{elapsed:.3f}s] LOGIN Disconnect: {msg}")
                        break
                    elif pid == 0x01:
                        print(f"[{elapsed:.3f}s] LOGIN Encryption Request")
                        # Empty encryption response
                        resp = pack_varint(len("00000000-0000-0000-0000-000000000001")) + b"00000000-0000-0000-0000-000000000001"
                        resp += pack_varint(0)
                        resp += pack_varint(0)
                        send_packet(sock, 0x01, resp, comp_threshold)
                        print(f"[{elapsed:.3f}s] -> Encryption Response")
                    elif pid == 0x02:
                        print(f"[{elapsed:.3f}s] LOGIN Login Success -> PLAY state")
                        state = "PLAY"
                    elif pid == 0x03:
                        threshold, _ = read_varint_bytes(body, 0)
                        comp_threshold = threshold
                        print(f"[{elapsed:.3f}s] LOGIN Set Compression threshold={comp_threshold}")
                    else:
                        print(f"[{elapsed:.3f}s] LOGIN Unknown 0x{pid:02X} len={len(body)}")

                else:  # PLAY state
                    if packet_count > 50 and pid in (0x20, 0x15, 0x16, 0x17, 0x40):
                        # Skip chunk data and entity packets for logging
                        pass
                    
                    if pid == 0x1F:  # Keep Alive (clientbound)
                        ka_id = struct.unpack(">q", body[:8])[0] if len(body) >= 8 else 0
                        keepalive_count += 1
                        keepalive_times.append(elapsed)
                        print(f"[{elapsed:.3f}s] PLAY KeepAlive id={ka_id}")
                        send_packet(sock, 0x0B, body[:8], comp_threshold)
                        print(f"[{elapsed:.3f}s] -> KeepAlive Response (0x0B)")
                        
                    elif pid == 0x23:  # Join Game
                        join_game_received = True
                        # Send Client Settings (0x02 in Play serverbound... wait)
                        # In 1.12.2, Client Settings is 0x02? No, let me check:
                        # Serverbound Play in 1.12.2:
                        # 0x00: Teleport Confirm
                        # 0x02: Chat Message
                        # 0x03: Client Status (or Chat?)
                        # 0x04: Client Settings? No...
                        # Let me check: 0x04 is Chat Message in some versions, 0x03 is Client Status
                        # Actually in 1.12.2: 0x04 = Client Settings, 0x03 = Client Status
                        print(f"[{elapsed:.3f}s] PLAY Join Game (eid, gamemode, dim, diff, ...)")
                        
                        # Send Client Status (0x03) - action 0 = RESPAWN
                        # Actually, first send Client Settings (0x04)
                        # Client Settings: locale (string), view_distance (byte), chat_mode (varint), 
                        #   chat_colors (bool), displayed_skin_parts (byte), main_hand (varint)
                        settings = pack_varint(len("en_us")) + b"en_us"
                        settings += pack_varint(8)  # view distance
                        settings += pack_varint(0)  # chat mode
                        settings += b"\x01"  # chat colors
                        settings += b"\x00"  # displayed skin parts
                        settings += pack_varint(1)  # main hand (right)
                        send_packet(sock, 0x04, settings, comp_threshold)
                        print(f"[{elapsed:.3f}s] -> Client Settings (0x04)")
                        
                    elif pid == 0x27:  # Position And Look (clientbound)
                        pos_look_received = True
                        # Need to send Teleport Confirm (0x00)
                        # The teleport ID is the last byte (varint) in the packet
                        teleport_id = body[-1] if len(body) > 0 else 0
                        teleport_ids.append(teleport_id)
                        send_packet(sock, 0x00, pack_varint(teleport_id), comp_threshold)
                        print(f"[{elapsed:.3f}s] PLAY Position&Look -> Sent Teleport Confirm (0x00, id={teleport_id})")
                        
                    elif pid == 0x46:  # Spawn Position
                        spawn_pos_received = True
                        print(f"[{elapsed:.3f}s] PLAY Spawn Position len={len(body)}")
                        
                    elif pid == 0x1A:  # Disconnect
                        slen, sp = read_varint_bytes(body, 0)
                        msg = body[sp:sp+slen].decode('utf-8', errors='replace')
                        print(f"[{elapsed:.3f}s] PLAY Disconnect: {msg}")
                        break
                        
                    elif pid == 0x20:  # Chunk Data
                        if time.time() - last_chunk_log < 2:
                            pass  # Don't log too many chunks
                        else:
                            last_chunk_log = time.time()
                            print(f"[{elapsed:.3f}s] PLAY Chunk Data")
                        
                    elif pid == 0x2C:  # Player Abilities
                        print(f"[{elapsed:.3f}s] PLAY Player Abilities")
                        
                    elif pid == 0x2D:  # Player Info
                        pass
                        
                    elif pid == 0x4F:  # Entity Effect
                        pass
                        
                    else:
                        # Log interesting packets, skip spam
                        if pid not in (0x15, 0x16, 0x17, 0x40, 0x41, 0x42, 0x19, 0x2D):
                            if packet_count < 100:  # Log first 100 non-chunk packets
                                print(f"[{elapsed:.3f}s] PLAY 0x{pid:02X} len={len(body)}")

            except socket.timeout:
                print(f"\n[{time.time()-t0:.1f}s] Socket timeout (idle)")
                break
            except ConnectionError as e:
                print(f"\n[{time.time()-t0:.1f}s] Connection closed: {e}")
                break
            except Exception as e:
                print(f"\n[{time.time()-t0:.1f}s] Error: {e}")
                import traceback; traceback.print_exc()
                break

    finally:
        sock.close()
        print(f"\n=== Summary ===")
        print(f"Total packets: {packet_count}")
        print(f"State: {state}")
        print(f"Join Game received: {join_game_received}")
        print(f"Spawn Position received: {spawn_pos_received}")
        print(f"Position & Look received: {pos_look_received}")
        print(f"Keepalives received: {keepalive_count}")
        if keepalive_times:
            intervals = [keepalive_times[i] - keepalive_times[i-1] for i in range(1, len(keepalive_times))]
            print(f"  First at: {keepalive_times[0]:.3f}s")
            print(f"  Intervals: {[f'{i:.1f}s' for i in intervals[:10]]}")
        print(f"Teleport IDs: {teleport_ids}")
        print(f"Duration: {time.time()-t0:.1f}s")

if __name__ == "__main__":
    main()
