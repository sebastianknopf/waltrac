import click
import logging
import random
import string

from time import time, sleep
from paho.mqtt import client
from paho.mqtt.client import Client
from urllib.parse import urlparse

def commander(secret: str, mqtt: str) -> None:
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
    client_id: str = f"waltrac-commander-{client_id_seed}"

    mqtt: Client = Client(client.CallbackAPIVersion.VERSION2, protocol=client.MQTTv5, client_id=client_id)

    if mqtt_username is not None and mqtt_password is not None:
        mqtt.username_pw_set(username=mqtt_username, password=mqtt_password)

    mqtt.connect(mqtt_host, int(mqtt_port))
    mqtt.loop_start()

    mqtt.subscribe(f"{mqtt_topic_base.lstrip('/')}/waltrac/cmd/commander")
    
    print("")
    print("Waltrac Commander v1.0.0")
    print("Type 'exit' to close the commander.")
    print("")

    try:
        while True:
            command: str = input('> ').strip()

            if command == 'discover':
                print('Discovering ...')
            elif command.startswith('setinterval'):
                print('Setting interval ...')
            elif command.startswith('setname'):
                print('Setting name ...')
            elif command == 'exit':
                break
            else:
                print(f'Unknown command: {command}. Enter \'help\' for a list of commands.')

    except KeyboardInterrupt:
        print("")

        mqtt.unsubscribe(f"{mqtt_topic_base.lstrip('/')}/waltrac/cmd/commander")
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