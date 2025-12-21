import asyncio
import click
import logging

from aiocoap import Message, Code, Context
from aiocoap import resource

from messages import Position, Command


class PositionResource(resource.Resource):
    def __init__(self, secret: str) -> None:
        super().__init__()
        
        self._secret = secret

    async def render_post(self, request: Message) -> Message:
        payload: bytes = request.payload

        logging.debug(f"Received Message: {payload.hex()}")

        try:
            pos: Position = Position.init(payload)

            if not pos.verify(self._secret):
                logging.error(f"Received message with invalid signature, discarding message.")
                return Message(code=Code.UNAUTHORIZED)
            
            logging.info(f"Processing: {pos}")
            # TODO: extend real processing here ...

            return Message(code=Code.CHANGED)
            
        except Exception as e:
            logging.error(f"Received invalid payload, discarding message.")
            return Message(code=Code.BAD_REQUEST)
        

class CommandResource(resource.Resource):
    def __init__(self, secret: str) -> None:
        super().__init__()
        
        self._secret = secret
    
    async def render_get(self, request: Message) -> Message:
        cmd: Command = Command()
        cmd.header = b'\x05'
        cmd.arg = 'TE-ST4'

        return Message(code=Code.CONTENT, payload=cmd.serialize(self._secret))


async def run(secret: str, host: str, port: int) -> None:
    root = resource.Site()
    root.add_resource(['position'], PositionResource(secret))
    root.add_resource(['command'], CommandResource(secret))

    await Context.create_server_context(root, bind=(host, port))

    logging.info(f"CoAP Server listening on {host}:{port}")
    await asyncio.get_running_loop().create_future()

@click.command()
@click.option('--secret', required=True, help='Secret for encryption and verification')
@click.option('--host', default='0.0.0.0', help='Host to bind the server to')
@click.option('--port', default=1999, help='Port to bind the server to')
@click.option('--debug', is_flag=True, default=False, help='Enable debug logging')
def main(secret: str, host: str, port: int, debug: bool) -> None:
    # set logging default configuration
    logging.basicConfig(format="[%(levelname)s] %(asctime)s %(message)s", level=logging.DEBUG if debug else logging.INFO)
    
    asyncio.run(run(secret, host, port))

if __name__ == '__main__':
    # run the main command
    main()

