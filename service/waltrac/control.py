import click
import logging
import random
import string

from time import time, sleep
from paho.mqtt import client
from paho.mqtt.client import Client
from urllib.parse import urlparse

from messages import *

_secret: str|None = None
_device_id: str|None = None

def _on_message_discover(mqtt: Client, userdata, message) -> None:
    global _device_id
    
    try:
        command: Command = Command.init(message.payload)
        if command.verify(_secret):
            action = command.get_header()[0]
            if action == CommandAction.DISCOVER:
                _device_id = str(command.arg)
        else:
            print("Received message with invalid signature.")

    except Exception as e:
        logging.debug("Failed to parse incoming message: %s", e)
        logging.debug(f"Message: {message.payload.hex()}")
        return
    
def _on_message_monitor(mqtt: Client, userdata, message) -> None:
    try:
        position: Position = Position.init(message.payload)
        if position.verify(_secret):
            print(str(position))
        else:
            print("Received message with invalid signature.")
    except Exception as e:
        logging.debug("Failed to parse incoming message: %s", e)
        logging.debug(f"Message: {message.payload.hex()}")
        return

def commander(secret: str, mqtt: str) -> None:
    global _secret, _device_id

    _secret = secret
    
    mqtt_uri = urlparse(mqtt)
    mqtt_params = mqtt_uri.netloc.split('@')
    mqtt_topic_base = mqtt_uri.path

    if len(mqtt_params) == 1:
        mqtt_username, mqtt_password = None, None
        mqtt_host, mqtt_port = mqtt_params[0].split(':')
    elif len(mqtt_params) == 2:
        mqtt_username, mqtt_password = mqtt_params[0].split(':')
        mqtt_host, mqtt_port = mqtt_params[1].split(':')

    client_id_seed: str = ''.join(random.choice(string.ascii_letters + string.digits) for _ in range(8))
    client_id: str = f"waltrac-control-{client_id_seed}"

    mqtt: Client = Client(client.CallbackAPIVersion.VERSION2, protocol=client.MQTTv5, client_id=client_id)

    if mqtt_username is not None and mqtt_password is not None:
        mqtt.username_pw_set(username=mqtt_username, password=mqtt_password)

    mqtt.connect(mqtt_host, int(mqtt_port))
    mqtt.loop_start()

    mqtt.subscribe(f"{mqtt_topic_base}waltrac/cmd/control")
    logging.debug("Subscribed to MQTT topic: %s", f"{mqtt_topic_base}waltrac/cmd/control")

    print("")
    print("Waltrac Control")
    
    print("")
    print("Waiting for device to discover ...");
    mqtt.on_message = _on_message_discover

    seconds: int = 0
    while seconds < 60 and _device_id is None:
        sleep(1)
        seconds += 1

    mqtt.on_message = None

    if seconds < 60:
        print(f"Discovered device {_device_id}")
    else:
        print("No device discovered within timeout period.")
        exit(1)
    
    print("")
    print("Type a command or 'exit' to close the control application.")
    print("")

    try:
        while True:
            command: str = input('> ').strip()

            if command.startswith('monitor'):
                mqtt.on_message = _on_message_monitor
                
                mqtt.subscribe(f"{mqtt_topic_base}waltrac/pos/{_device_id}")
                logging.debug("Subscribed to MQTT topic: %s", f"{mqtt_topic_base}waltrac/pos/{_device_id}")

                print("Monitoring for 5 minutes. Press Ctrl+C to stop early.")

                seconds: int = 0
                while seconds < 300:
                    try:
                        sleep(1)
                        seconds += 1
                    except KeyboardInterrupt:
                        break

                mqtt.unsubscribe(f"{mqtt_topic_base}waltrac/pos/{_device_id}")
                logging.debug("Unsubscribed from MQTT topic: %s", f"{mqtt_topic_base}waltrac/pos/{_device_id}")

                mqtt.on_message = None

            elif command.startswith('setinterval'):
                cmdargs: list[str] = command.split(':', 1)
                if len(cmdargs) != 2:
                    print("Invalid command. Usage: setinterval:<seconds>. Type 'help' for a list of commands.")
                    continue

                if not cmdargs[1].isdigit() or int(cmdargs[1]) < 1:
                    print("Invalid interval. It must be a positive integer representing seconds.")
                    continue
                
                command: Command = Command()
                command.set_header(CommandAction.SETINTERVAL)
                command.arg = cmdargs[1]

                mqtt.publish(f"{mqtt_topic_base}waltrac/cmd/{_device_id}", command.serialize(secret))
            elif command.startswith('setname'):
                cmdargs: list[str] = command.split(':', 1)
                if len(cmdargs) != 2:
                    print("Invalid command. Usage: setname:<name>. Type 'help' for a list of commands.")
                    continue
                
                command: Command = Command()
                command.set_header(CommandAction.SETNAME)
                command.arg = cmdargs[1]


                mqtt.publish(f"{mqtt_topic_base}waltrac/cmd/{_device_id}", command.serialize(secret))
            elif command == 'exit':
                command: Command = Command()
                command.set_header(CommandAction.EXIT)

                mqtt.publish(f"{mqtt_topic_base}waltrac/cmd/{_device_id}", command.serialize(secret))

                break
            elif command == 'help':
                print("Following commands are available:")
                print("monitor - Monitors incoming positions of the discovered device for 5 minutes. Device needs to be in operations mode.")
                print("setinterval:<interval> - Set the minimum update interval in seconds for the device. Requires an integer, minimum is 10.")
                print("setname:<name> - Set the name for the device. Requires a valid UTF-8 string.")
                print("exit - Quits the control application and sends a command to the discovered device to enter operations mode.")
                print("help - Displays the help for available commands.")
            else:
                print(f'Unknown command: {command}. Enter \'help\' for a list of commands.')

    except KeyboardInterrupt:
        pass
    
    sleep(5)
    print("")

    mqtt.unsubscribe(f"{mqtt_topic_base}waltrac/cmd/control")
    logging.debug("Unsubscribed from MQTT topic: %s", f"{mqtt_topic_base}waltrac/cmd/control")
    
    mqtt.loop_stop()

@click.command()
@click.argument('secret')
@click.argument('mqtt')
@click.option('--debug', is_flag=True, default=False, help='Enable debug logging')
def main(secret: str, mqtt: str, debug: bool) -> None:
    # set logging default configuration
    logging.basicConfig(format="[%(levelname)s] %(asctime)s %(message)s", level=logging.DEBUG if debug else logging.INFO)
    
    commander(secret, mqtt)

if __name__ == '__main__':
    # run the main command
    main()