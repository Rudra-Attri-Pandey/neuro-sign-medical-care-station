#!/usr/bin/env python3
"""
=============================================================================
Assistive Care Medical Station - Backend Server
Compatible with Arduino UNO Q / Linux OS / Windows PC
Features:
  - Non-blocking, thread-safe USB Serial Stream Reader (115200 & 9600 baud)
  - Instant Port Connect / Disconnect without UI hanging
  - Clean COM port sanitization (extracts 'COM6' from 'COM6 (Arduino Uno (COM6))')
  - Automatic port-lock diagnosis & error reporting
  - Server-Sent Events (SSE) for real-time telemetry streaming
  - Bidirectional USB relay & siren control
  - Simulation Mode for offline UI testing
=============================================================================
"""

import json
import time
import threading
import queue
import sys
import re
import serial
import serial.tools.list_ports
from flask import Flask, render_template, Response, request, jsonify

app = Flask(__name__)

# Global State
latest_data = {
    "glove_connected": False,
    "message": "Waiting for Glove Signal",
    "mode": 1,
    "roll": 0.0,
    "pitch": 0.0,
    "relay1": False,
    "relay2": False,
    "relay3": False,
    "temperature": 24.5,
    "humidity": 55.0,
    "pressure": 1013.2,
    "aqi": 28,
    "voc_raw": 28000,
    "timestamp": time.time(),
    "port": "Disconnected",
    "is_connected": False,
    "status_message": "Ready to connect",
    "is_simulation": False
}

serial_connection = None
serial_lock = threading.Lock()
subscribers = []
subscribers_lock = threading.Lock()
simulation_active = False

# Control Signals for Worker
requested_port = None
should_disconnect = False
connection_in_progress = False

def sanitize_port(port_str):
    """Extracts clean device name like 'COM6' or '/dev/ttyUSB0' from descriptive labels."""
    if not port_str:
        return None
    port_str = str(port_str).strip()
    match = re.search(r'(COM\d+|/dev/tty[A-Za-z0-9_]+)', port_str, re.IGNORECASE)
    if match:
        dev = match.group(1)
        return dev.upper() if dev.upper().startswith('COM') else dev
    return port_str.split()[0].strip()

def find_arduino_port():
    """Scans and auto-detects Arduino or USB serial devices."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "arduino" in desc or "ch340" in desc or "cp210" in desc or "usb serial" in desc or "ftdi" in desc or "cdc" in desc:
            return sanitize_port(p.device)
    if ports:
        return sanitize_port(ports[0].device)
    return None

def broadcast_data(data):
    """Pushes new telemetry data to all active browser SSE streams."""
    payload = f"data: {json.dumps(data)}\n\n"
    with subscribers_lock:
        dead_subs = []
        for q in subscribers:
            try:
                q.put_nowait(payload)
            except queue.Full:
                dead_subs.append(q)
        for q in dead_subs:
            subscribers.remove(q)

def try_open_port(port_name):
    """Attempts opening serial port quickly with timeout=0.1."""
    clean = sanitize_port(port_name)
    if not clean:
        return None, "Invalid port name"

    # Try 115200 baud first, then 9600
    for baud in [115200, 9600]:
        try:
            ser = serial.Serial(
                port=clean,
                baudrate=baud,
                timeout=0.1,
                write_timeout=0.5
            )
            # Reset Arduino DTR line
            try:
                ser.dtr = False
                time.sleep(0.05)
                ser.dtr = True
            except:
                pass
            ser.reset_input_buffer()
            print(f"[SERIAL] Successfully opened {clean} @ {baud} baud.")
            return ser, None
        except serial.SerialException as e:
            err = str(e)
            if "PermissionError" in err or "Access is denied" in err or "could not open port" in err:
                return None, f"Port {clean} is busy. Please close Arduino IDE Serial Monitor."
            continue
        except Exception as e:
            return None, str(e)

    return None, f"Could not connect to {clean}."

def serial_worker():
    """Single thread managing serial lifecycle and non-blocking line reading."""
    global serial_connection, latest_data, requested_port, should_disconnect, simulation_active

    buffer = ""

    while True:
        if simulation_active:
            time.sleep(0.5)
            continue

        # Handle Disconnect Request
        if should_disconnect:
            with serial_lock:
                if serial_connection:
                    try:
                        serial_connection.close()
                    except:
                        pass
                    serial_connection = None
                latest_data["port"] = "Disconnected"
                latest_data["is_connected"] = False
                latest_data["status_message"] = "Disconnected"
                should_disconnect = False
                requested_port = None
            broadcast_data(latest_data)
            time.sleep(0.2)
            continue

        # Handle Manual Connect Request or Auto-Detect
        target = requested_port
        if not target and not serial_connection:
            target = find_arduino_port()

        if target and (not serial_connection or not serial_connection.is_open):
            ser, err = try_open_port(target)
            with serial_lock:
                if ser:
                    serial_connection = ser
                    latest_data["port"] = ser.port
                    latest_data["is_connected"] = True
                    latest_data["status_message"] = f"Connected to {ser.port}"
                    requested_port = None
                else:
                    latest_data["port"] = "Disconnected"
                    latest_data["is_connected"] = False
                    latest_data["status_message"] = err or "Connection failed"
                    requested_port = None
            broadcast_data(latest_data)
            if not ser:
                time.sleep(2.0)
                continue

        # Read Data Non-blockingly
        if serial_connection and serial_connection.is_open:
            try:
                if serial_connection.in_waiting > 0:
                    chunk = serial_connection.read(serial_connection.in_waiting).decode('utf-8', errors='ignore')
                    buffer += chunk
                    
                    if '\n' in buffer:
                        lines = buffer.split('\n')
                        buffer = lines[-1] # Keep partial last line
                        
                        for line in lines[:-1]:
                            line = line.strip()
                            s_idx = line.find('{')
                            e_idx = line.rfind('}')
                            if s_idx != -1 and e_idx != -1 and e_idx > s_idx:
                                json_str = line[s_idx:e_idx+1]
                                try:
                                    parsed = json.loads(json_str)
                                    parsed["timestamp"] = time.time()
                                    parsed["port"] = serial_connection.port
                                    parsed["is_connected"] = True
                                    parsed["status_message"] = "Connected & Streaming"
                                    parsed["is_simulation"] = False
                                    
                                    with serial_lock:
                                        latest_data.update(parsed)
                                    broadcast_data(latest_data)
                                except json.JSONDecodeError:
                                    pass
                else:
                    time.sleep(0.02)
            except Exception as e:
                print(f"[SERIAL] Read error: {e}")
                with serial_lock:
                    try:
                        serial_connection.close()
                    except:
                        pass
                    serial_connection = None
                    latest_data["port"] = "Disconnected"
                    latest_data["is_connected"] = False
                    latest_data["status_message"] = "Connection lost"
                broadcast_data(latest_data)
                time.sleep(1.0)
        else:
            time.sleep(0.5)

def simulation_worker():
    """Simulates realistic patient gestures and telemetry when enabled."""
    global simulation_active, latest_data
    messages = ["Patient Resting", "I need Water", "I need Food", "I need Medicine/Help"]
    idx = 0
    
    while True:
        if simulation_active:
            import random
            time.sleep(2.5)
            idx = (idx + 1) % len(messages)
            sim_msg = messages[idx]
            sim_mode = 1 if idx != 1 else 2
            
            with serial_lock:
                latest_data.update({
                    "glove_connected": True,
                    "message": sim_msg,
                    "mode": sim_mode,
                    "roll": -42.0 if sim_mode == 2 else random.uniform(-10.0, 10.0),
                    "pitch": random.uniform(-15.0, 15.0),
                    "relay1": random.choice([True, False]),
                    "relay2": latest_data["relay2"],
                    "relay3": latest_data["relay3"],
                    "temperature": round(24.0 + random.uniform(-0.5, 0.8), 1),
                    "humidity": round(52.0 + random.uniform(-2.0, 3.0), 1),
                    "pressure": round(1012.5 + random.uniform(-0.8, 0.8), 1),
                    "aqi": int(random.uniform(22, 48)),
                    "voc_raw": int(27500 + random.uniform(-500, 1200)),
                    "timestamp": time.time(),
                    "port": "DEMO_SIMULATION",
                    "is_connected": True,
                    "status_message": "Simulation Active",
                    "is_simulation": True
                })
            broadcast_data(latest_data)
        else:
            time.sleep(1.0)

# ---------------------------------------------------------------------------
# HTTP & SSE Routes
# ---------------------------------------------------------------------------
@app.route('/')
def index():
    return render_template('index.html')

@app.route('/stream')
def stream():
    """SSE Stream for real-time telemetry updates."""
    def event_stream():
        q = queue.Queue(maxsize=30)
        with subscribers_lock:
            subscribers.append(q)
        initial_payload = f"data: {json.dumps(latest_data)}\n\n"
        yield initial_payload
        try:
            while True:
                data = q.get()
                yield data
        except GeneratorExit:
            with subscribers_lock:
                if q in subscribers:
                    subscribers.remove(q)

    return Response(event_stream(), mimetype="text/event-stream")

@app.route('/api/telemetry')
def get_telemetry():
    return jsonify(latest_data)

@app.route('/api/ports', methods=['GET'])
def list_ports():
    """Scans and returns clean USB / COM serial ports."""
    port_list = []
    for p in serial.tools.list_ports.comports():
        clean_name = sanitize_port(p.device)
        label = clean_name
        if p.description and p.description != "n/a":
            label = f"{clean_name} ({p.description})"
        port_list.append({
            "device": clean_name,
            "description": p.description or "",
            "label": label
        })
    
    current_port = sanitize_port(latest_data.get("port", "Disconnected"))
    is_connected = bool(serial_connection and serial_connection.is_open)
    
    return jsonify({
        "ports": port_list,
        "current_port": current_port,
        "is_connected": is_connected,
        "status_message": latest_data.get("status_message", ""),
        "is_simulation": simulation_active
    })

@app.errorhandler(404)
def not_found(e):
    return jsonify({"success": False, "error": "Route not found"}), 404

@app.errorhandler(500)
def server_error(e):
    return jsonify({"success": False, "error": str(e)}), 500

@app.route('/api/port/connect', methods=['POST'])
@app.route('/api/connect', methods=['POST'])
def connect_port():
    """Instructs worker to connect to the requested port without blocking the HTTP request."""
    global requested_port, should_disconnect, simulation_active
    data = request.get_json(silent=True) or {}
    raw_port = data.get("port")
    
    if not raw_port:
        return jsonify({"success": False, "error": "No port selected"}), 400
    
    clean_port = sanitize_port(raw_port)
    simulation_active = False
    latest_data["is_simulation"] = False
    
    # Signal worker to switch port
    should_disconnect = False
    requested_port = clean_port

    # Test open immediately to return quick status
    ser, err = try_open_port(clean_port)
    if ser:
        with serial_lock:
            if serial_connection:
                try:
                    serial_connection.close()
                except:
                    pass
            serial_connection = ser
            latest_data["port"] = ser.port
            latest_data["is_connected"] = True
            latest_data["status_message"] = f"Connected to {ser.port}"
            requested_port = None
        broadcast_data(latest_data)
        return jsonify({"success": True, "port": ser.port})
    else:
        with serial_lock:
            latest_data["port"] = "Disconnected"
            latest_data["is_connected"] = False
            latest_data["status_message"] = err or "Connection failed"
        broadcast_data(latest_data)
        return jsonify({"success": False, "error": err or "Failed to connect"}), 200

@app.route('/api/port/disconnect', methods=['POST'])
@app.route('/api/disconnect', methods=['POST'])
def disconnect_port():
    """Signals worker to disconnect cleanly."""
    global should_disconnect, requested_port
    requested_port = None
    should_disconnect = True
    return jsonify({"success": True})

@app.route('/api/relay/<int:relay_id>/toggle', methods=['POST'])
def toggle_relay(relay_id):
    """Sends relay toggle command to Arduino via USB Serial."""
    global serial_connection, latest_data, simulation_active
    
    cmd = f"R{relay_id}_TOGGLE\n"
    if simulation_active:
        key = f"relay{relay_id}"
        latest_data[key] = not latest_data.get(key, False)
        broadcast_data(latest_data)
        return jsonify({"success": True, "state": latest_data[key], "mode": "simulation"})

    with serial_lock:
        if serial_connection and serial_connection.is_open:
            try:
                serial_connection.write(cmd.encode('utf-8'))
                serial_connection.flush()
                return jsonify({"success": True, "command": cmd.strip()})
            except Exception as e:
                return jsonify({"success": False, "error": str(e)}), 500
        else:
            return jsonify({"success": False, "error": "Arduino USB not connected"}), 503

@app.route('/api/siren/test', methods=['POST'])
def test_siren():
    """Triggers siren test command to Arduino over USB."""
    global serial_connection, simulation_active
    if simulation_active:
        return jsonify({"success": True, "mode": "simulation"})
        
    with serial_lock:
        if serial_connection and serial_connection.is_open:
            try:
                serial_connection.write(b"SIREN_TEST\n")
                serial_connection.flush()
                return jsonify({"success": True})
            except Exception as e:
                return jsonify({"success": False, "error": str(e)}), 500
        else:
            return jsonify({"success": False, "error": "Arduino USB not connected"}), 503

@app.route('/api/simulation/toggle', methods=['POST'])
def toggle_simulation():
    global simulation_active, requested_port
    simulation_active = not simulation_active
    latest_data["is_simulation"] = simulation_active
    latest_data["port"] = "DEMO_SIMULATION" if simulation_active else "Disconnected"
    latest_data["is_connected"] = simulation_active
    latest_data["status_message"] = "Simulation Mode" if simulation_active else "Ready to connect"
    broadcast_data(latest_data)
    return jsonify({"simulation_active": simulation_active})

if __name__ == '__main__':
    t_serial = threading.Thread(target=serial_worker, daemon=True)
    t_serial.start()

    t_sim = threading.Thread(target=simulation_worker, daemon=True)
    t_sim.start()

    port = 5000
    print("\n" + "="*65)
    print("  🧠 NEURO SIGN - ASSISTIVE MEDICAL STATION ONLINE")
    print("  👨‍💻 Developed by: Rudra Attri Pandey")
    print(f"  🌐 Dashboard URL: http://localhost:{port}")
    print("="*65 + "\n")
    
    app.run(host='0.0.0.0', port=port, debug=False, threaded=True)
