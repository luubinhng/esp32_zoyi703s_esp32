import asyncio
import json
import sys

try:
    import websockets
except ImportError:
    print('Missing dependency: pip install websockets')
    sys.exit(1)

URI = 'ws://192.168.4.1:81/'

async def main():
    try:
        async with websockets.connect(URI, ping_interval=None) as ws:
            print('Connected to ESP32 VOM dashboard')
            while True:
                msg = await ws.recv()
                obj = json.loads(msg)
                mode = obj.get('mode', 'UNKNOWN')
                value = obj.get('value')
                unit = obj.get('unit', '--')
                raw = obj.get('raw', '')
                print(f"[{mode}] {value} {unit} | raw: {raw}")
    except Exception as exc:
        print(f'Connection failed: {exc}')
        print('Check that the ESP32 AP is open and the Wi-Fi is connected to: VOM-703S')
        print('Then update URI inside this script if needed.')

if __name__ == '__main__':
    asyncio.run(main())
