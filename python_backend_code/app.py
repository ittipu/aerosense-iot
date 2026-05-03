from flask import Flask, request, render_template
from flask_socketio import SocketIO
import mysql.connector
import paho.mqtt.client as mqtt
import json
from datetime import datetime, timedelta
import threading

# --- 1. INITIALIZATION ---
app = Flask(__name__)
# Enable Socket.IO with CORS so the frontend can connect from anywhere
socketio = SocketIO(app, cors_allowed_origins="*")

# --- 2. CONFIGURATION ---
DB_CONFIG = {
    'host': 'localhost',
    'user': 'tipu',
    'password': 'tipu',
    'database': 'iot_dashboard'
}

MQTT_BROKER = "iot.reyax.com"
MQTT_PORT = 1883
MQTT_TOPIC = "greenhouse/telemetry"
MQTT_USER = "sRkG5DeaQT"
MQTT_PASS = "H5ZQydxeFM"

def get_db_connection():
    return mysql.connector.connect(**DB_CONFIG)

# --- DB CONNECTION TEST ---
print("Testing Database Connection...")
try:
    test_conn = get_db_connection()
    print("[OK] Connected to MySQL Database.")
    test_conn.close()
except Exception as e:
    print(f"[ERROR] Failed to connect to MySQL Database: {e}")

# --- 3. DATABASE FUNCTIONS ---
# --- 3. DATABASE FUNCTIONS ---
def get_historical_data(hours=24):
    """Fetches historical data based on requested hours (default 24)."""
    try:
        conn = get_db_connection()
        cursor = conn.cursor(dictionary=True)
        time_threshold = datetime.now() - timedelta(hours=hours)
        
        cursor.execute(
            "SELECT * FROM sensor_data WHERE timestamp >= %s ORDER BY timestamp ASC", 
            (time_threshold,)
        )
        rows = cursor.fetchall()
        
        # Convert datetime objects to ISO strings for the frontend/JSON
        for row in rows:
            row['timestamp'] = row['timestamp'].isoformat()
            
        cursor.close()
        conn.close()
        return rows
    except Exception as e:
        print(f"DB Read Error: {e}")
        return []

def insert_sensor_data(data, timestamp):
    """Saves the incoming MQTT payload to MySQL."""
    try:
        conn = get_db_connection()
        cursor = conn.cursor()
        
        query = """
            INSERT INTO sensor_data 
            (timestamp, temp, humidity, co2, lux) 
            VALUES (%s, %s, %s, %s, %s)
        """
        
        # Pull the keys directly from the MQTT payload into the matching columns
        values = (
            timestamp,
            data.get('temp'),
            data.get('humidity'),
            data.get('co2'),
            data.get('lux')
        )
        
        cursor.execute(query, values)
        conn.commit()
        cursor.close()
        conn.close()
        print(f"[{timestamp}] Saved to Database: {data}")
    except Exception as e:
        print(f"DB Insert Error: {e}")

# --- 4. MQTT FUNCTIONS (BACKGROUND TASK) ---
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("Connected to MQTT Broker!")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"MQTT Connection failed with code {rc}")

def on_message(client, userdata, msg):
    try:
        payload = msg.payload.decode('utf-8')
        data = json.loads(payload)
        
        # Generate timestamp here so it matches between DB and WebSocket
        now = datetime.now()
        timestamp_str = now.strftime('%Y-%m-%d %H:%M:%S')
        iso_time = now.isoformat()
        
        # 1. Save to Database
        insert_sensor_data(data, timestamp_str)
        
        # 2. Push to all connected web clients instantly via WebSockets!
        data['timestamp'] = iso_time
        socketio.emit('live_data', data)
        print("Emitted live_data to UI")
        
    except Exception as e:
        print(f"Error processing MQTT message: {e}")

def start_mqtt():
    """Runs the MQTT client loop in a background thread."""
    client = mqtt.Client()
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message
    
    try:
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
        client.loop_forever()
    except Exception as e:
        print(f"MQTT start failed: {e}")

# --- 5. SOCKET.IO EVENTS ---
@app.route('/')
@app.route('/index.html')
def dashboard():
    """Serve the main dashboard UI through Flask templates."""
    return render_template('index.html')

@socketio.on('connect')
def handle_connect():
    print(f"Client connected: {request.sid}")
    # When a user opens the dashboard, immediately send them the default 24h historical data
    history = get_historical_data(hours=24)
    # emit to ONLY the client that just connected
    socketio.emit('initial_data', history, to=request.sid) 

@socketio.on('request_history')
def handle_request_history(payload):
    """Allows the frontend to request data for different timeframes (e.g., 7 days)"""
    try:
        hours = int(payload.get('hours', 24))
        print(f"Client {request.sid} requested {hours} hours of history")
        
        # Cap the max history to 30 days (720 hours) to protect the server memory
        if hours > 720:
            hours = 720
            
        history = get_historical_data(hours=hours)
        socketio.emit('history_response', history, to=request.sid)
    except Exception as e:
        print(f"Error serving history request: {e}")

@socketio.on('disconnect')
def handle_disconnect():
    print(f"Client disconnected: {request.sid}")

# --- 6. START SERVER ---
if __name__ == '__main__':
    # Start the MQTT listener in a background thread
    mqtt_thread = threading.Thread(target=start_mqtt, daemon=True)
    mqtt_thread.start()
    
    print("Starting Project AeroSense Real-Time Server...")
    # Run the Flask-SocketIO app (use_reloader=False prevents double-starting the MQTT thread)
    socketio.run(
        app,
        host='0.0.0.0',
        port=5000,
        debug=True,
        use_reloader=False,
        allow_unsafe_werkzeug=True
    )