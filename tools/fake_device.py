"""Publishes readings the way the firmware does, for trying the broker and the bridge without an ESP32.

    pip install paho-mqtt
    python tools/fake_device.py --device cm-test --password <its password> --probe 28ff4a1c0b160351

The device id doubles as the MQTT user name, as the broker ACL expects. The temperature drifts around 4 °C and
sometimes rises above 8 °C, so the backend has an excursion to alert on.
"""

import argparse
import json
import random
import time

import paho.mqtt.client as mqtt


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--device", default="cm-test")
    parser.add_argument("--password", default="")
    parser.add_argument("--probe", default="28ff4a1c0b160351", help="1-Wire ROM code, as registered in BRIDGE_PROBES")
    parser.add_argument("--interval", type=float, default=5.0, help="seconds between readings")
    parser.add_argument("--count", type=int, default=0, help="stop after this many readings (0 = never)")
    args = parser.parse_args()

    prefix = f"coldmonitor/{args.device}"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=args.device)
    client.username_pw_set(args.device, args.password)
    client.will_set(f"{prefix}/status", "offline", qos=1, retain=True)
    client.on_connect = lambda c, userdata, flags, reason, props: c.publish(f"{prefix}/status", "online", 1, True)
    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()

    boot = random.getrandbits(32)
    temperature = 4.0
    seq = 0
    try:
        while args.count == 0 or seq < args.count:
            seq += 1
            temperature += random.uniform(-0.3, 0.3) + (4.0 - temperature) * 0.1
            if random.random() < 0.02:
                temperature = 9.5  # door left open
            reading = {"device": args.device, "boot": boot, "seq": seq, "probe": args.probe,
                       "t": round(temperature, 2), "ts": int(time.time())}
            client.publish(f"{prefix}/readings", json.dumps(reading, separators=(",", ":")), qos=1).wait_for_publish()
            print(f"sent {reading}")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        client.publish(f"{prefix}/status", "offline", 1, True).wait_for_publish()
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
