import asyncio
import click
import json
import logging

from aiocoap import Message, Code, Context
from aiocoap import resource
from urllib.parse import urlparse

from messages import Position, Command
from mqtt import MqttPublisher


class PositionResource(resource.Resource):
    def __init__(self, secret: str, mqtt: MqttPublisher) -> None:
        super().__init__()
        
        self._secret = secret
        self._mqtt = mqtt

    async def render_post(self, request: Message) -> Message:
        payload: bytes = request.payload

        logging.debug(f"Received Message: {payload.hex()}")

        try:
            pos: Position = Position.init(payload)

            if not pos.verify(self._secret):
                logging.error(f"Received message with invalid signature, discarding message.")
                return Message(code=Code.UNAUTHORIZED, payload=bytes.fromhex('00'))
            
            logging.info(f"Processing: {pos}")

            data: dict = {
                'dv': pos.device.hex(),
                'vl': True,
                'st': 5,
                'lt': pos.latitude,
                'lg': pos.longitude,
                'nm': pos.name
            }

            self._mqtt.publish(f"waltrac/position/{pos.device.hex()}", json.dumps(data).encode('utf-8'))

            return Message(code=Code.CHANGED, payload=bytes.fromhex('00'))
            
        except Exception as e:
            logging.error(f"Received invalid payload, discarding message.")
            return Message(code=Code.BAD_REQUEST, payload=bytes.fromhex('00'))
        

class CommandResource(resource.Resource):
    def __init__(self, secret: str) -> None:
        super().__init__()
        
        self._secret = secret
    
    async def render_get(self, request: Message) -> Message:
        cmd: Command = Command()
        cmd.header = b'\x05'
        cmd.arg = 'TE-ST4'

        return Message(code=Code.CONTENT, payload=cmd.serialize(self._secret))


async def run(secret: str, mqtt: str, host: str, port: int) -> None:
    mqtt_uri = urlparse(mqtt)
    mqtt_params = mqtt_uri.netloc.split('@')
    mqtt_topic = mqtt_uri.path

    if len(mqtt_params) == 1:
        mqtt_username, mqtt_password = None, None
        mqtt_host, mqtt_port = mqtt_params[0].split(':')
    elif len(mqtt_params) == 2:
        mqtt_username, mqtt_password = mqtt_params[0].split(':')
        mqtt_host, mqtt_port = mqtt_params[1].split(':')
    
    mqtt: MqttPublisher = MqttPublisher(
        host=mqtt_host,
        port=int(mqtt_port),
        username=mqtt_username,
        password=mqtt_password,
    )

    await mqtt.start()
    
    root = resource.Site()
    root.add_resource(['position'], PositionResource(secret, mqtt))
    root.add_resource(['command'], CommandResource(secret))

    await Context.create_server_context(root, bind=(host, port))

    logging.info(f"CoAP Server listening on {host}:{port}")
    await asyncio.get_running_loop().create_future()

@click.command()
@click.option('--secret', required=True, help='Secret for encryption and verification')
@click.option('--mqtt', required=True, help='MQTT connection and topic URI')
@click.option('--host', default='0.0.0.0', help='Host to bind the server to')
@click.option('--port', default=1999, help='Port to bind the server to')
@click.option('--debug', is_flag=True, default=False, help='Enable debug logging')
def main(secret: str, mqtt: str, host: str, port: int, debug: bool) -> None:
    # set logging default configuration
    logging.basicConfig(format="[%(levelname)s] %(asctime)s %(message)s", level=logging.DEBUG if debug else logging.INFO)
    
    asyncio.run(run(secret, mqtt, host, port))

if __name__ == '__main__':
    # run the main command
    main()

