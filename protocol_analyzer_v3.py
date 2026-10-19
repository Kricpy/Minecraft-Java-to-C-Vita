#!/usr/bin/env python3
"""
Minecraft 1.12.2 Protocol Analyzer v3
Properly tracks Login → Play state transitions with compression.
"""
import socket
import struct
import time
import zlib

SERVER_HOST = "kreqxs.aternos.me"
SERVER_PORT = 11113
USERNAME = "ProtocolBot3"

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
    """Send a packet with optional compression wrapper."""
    pkt = pack_varint(packet_id) + data
    if comp_threshold >= 0 and len(pkt) > comp_threshold:
        compressed = zlib.compress(pkt)
        wire = pack_varint(len(pkt)) + compressed
    elif comp_threshold >= 0:
        # data_length = 0 means uncompressed
        wire = pack_varint(0) + pkt
    else:
        # No compression yet
        wire = pkt
    sock.send(pack_varint(len(wire)) + wire)

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30)

    print(f"Connecting to {SERVER_HOST}:{SERVER_PORT}...")
    t0 = time.time()
    sock.connect((SERVER_HOST, SERVER_PORT))
    print(f"Connected in {time.time()-t0:.3f}s")

    try:
        # === Handshake (next_state=2 = Login) ===
        hs = pack_varint(340) + pack_varint(len(SERVER_HOST)) + SERVER_HOST.encode() + struct.pack(">H", SERVER_PORT) + pack_varint(2)
        send_packet(sock, 0x00, hs)
        print(f"[{time.time()-t0:.3f}s] -> Handshake (next_state=2)")

        # === Login Start ===
        send_packet(sock, 0x00, pack_varint(len(USERNAME)) + USERNAME.encode())
        print(f"[{time.time()-t0:.3f}s] -> Login Start ({USERNAME})")

        # === Receive loop ===
        state = "LOGIN"
        comp_threshold = -1
        keepalive_count = 0
        keepalive_responses = 0
        packet_count = 0

        while True:
            try:
                # Read frame length
                flen = read_varint_sock(sock)
                if flen <= 0:
                    print(f"[{time.time()-t0:.3f}s] Zero-length frame, connection closing")
                    break

                # Read frame data
                frame = b""
                while len(frame) < flen:
                    chunk = sock.recv(flen - len(frame))
                    if not chunk:
                        raise ConnectionError("Connection closed mid-frame")
                    frame += chunk

                elapsed = time.time() - t0
                packet_count += 1

                if state == "LOGIN":
                    pid, pos = read_varint_bytes(frame, 0)
                    payload = frame[pos:]

                    if pid == 0x00:
                        msg, _ = read_varint_bytes(payload, 0)
                        msg_len, msg_pos = read_varint_bytes(payload, 0)
                        msg_text = payload[msg_pos:msg_pos+msg_len].decode('utf-8', errors='replace')
                        print(f"[{elapsed:.3f}s] LOGIN Disconnect: {msg_text}")
                        break

                    elif pid == 0x01:
                        print(f"[{elapsed:.3f}s] LOGIN Encryption Request")
                        # Respond with empty Encryption Response
                        resp = pack_varint(len("00000000-0000-0000-0000-000000000001")) + b"00000000-0000-0000-0000-000000000001"
                        resp += pack_varint(0)  # empty shared secret
                        resp += pack_varint(0)  # empty verify token
                        send_packet(sock, 0x01, resp)
                        print(f"[{elapsed:.3f}s] -> Encryption Response (empty)")

                    elif pid == 0x02:
                        uuid_len, pos = read_varint_bytes(payload, 0)
                        uuid = payload[pos:pos+uuid_len].decode('utf-8', errors='replace')
                        pos += uuid_len
                        name_len, pos = read_varint_bytes(payload, pos)
                        name = payload[pos:pos+name_len].decode('utf-8', errors='replace')
                        print(f"[{elapsed:.3f}s] LOGIN Login Success uuid={uuid} name={name}")
                        state = "PLAY"

                    elif pid == 0x03:
                        threshold, pos = read_varint_bytes(payload, 0)
                        comp_threshold = threshold
                        print(f"[{elapsed:.3f}s] LOGIN Set Compression threshold={comp_threshold}")

                    else:
                        print(f"[{elapsed:.3f}s] LOGIN Unknown 0x{pid:02X} len={flen}")

                else:  # PLAY state
                    # Handle compression
                    if comp_threshold >= 0:
                        data_len, pos = read_varint_bytes(frame, 0)
                        if data_len == 0:
                            # Uncompressed
                            pid, ppos = read_varint_bytes(frame, pos)
                            body = frame[ppos:]
                        else:
                            # Compressed
                            compressed_data = frame[pos:]
                            try:
                                decompressed = zlib.decompress(compressed_data)
                                pid, ppos = read_varint_bytes(decompressed, 0)
                                body = decompressed[ppos:]
                            except Exception as e:
                                print(f"[{elapsed:.3f}s] Decompress error: {e}, frame={frame[:30].hex()}")
                                continue
                    else:
                        pid, pos = read_varint_bytes(frame, 0)
                        body = frame[pos:]

                    # Classify play packets
                    if pid == 0x1F:  # Keep Alive (clientbound)
                        ka_id = struct.unpack(">q", body[:8])[0] if len(body) >= 8 else 0
                        keepalive_count += 1
                        print(f"[{elapsed:.3f}s] PLAY KeepAlive id={ka_id}")
                        send_packet(sock, 0x0B, body[:8], comp_threshold)
                        keepalive_responses += 1

                    elif pid == 0x23:  # Join Game
                        print(f"[{elapsed:.3f}s] PLAY Join Game len={len(body)}")

                    elif pid == 0x27:  # Position And Look
                        print(f"[{elapsed:.3f}s] PLAY Position&Look len={len(body)}")

                    elif pid == 0x20:  # Chunk Data
                        pass  # Skip chunk data (too verbose)

                    elif pid == 0x46:  # Spawn Position
                        print(f"[{elapsed:.3f}s] PLAY Spawn Position len={len(body)}")

                    elif pid == 0x2C:  # Entity Status
                        print(f"[{elapsed:.3f}s] PLAY Entity Status len={len(body)}")

                    elif pid == 0x1A:  # Disconnect
                        msg_len, pos = read_varint_bytes(body, 0)
                        msg = body[pos:pos+msg_len].decode('utf-8', errors='replace')
                        print(f"[{elapsed:.3f}s] PLAY Disconnect: {msg}")
                        break

                    else:
                        # Log interesting packets but skip chunk data
                        if packet_count < 200 or pid not in (0x20, 0x15, 0x16, 0x40, 0x41, 0x42):
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
        print(f"Keepalives received: {keepalive_count}")
        print(f"Keepalive responses sent: {keepalive_responses}")
        print(f"Duration: {time.time()-t0:.1f}s")

if __name__ == "__main__":
    main()
