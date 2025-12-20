import asyncio
import logging

from aiocoap import Message, Code, Context
from aiocoap import resource

from messages import Position, Command

MAC_KEY = ''

class PositionResource(resource.Resource):
    async def render_post(self, request: Message) -> Message:
        payload: bytes = request.payload

        print(f"Received Position Payload: {payload!r}")

        try:
            pos: Position = Position.init(payload)

            if not pos.verify(MAC_KEY):
                return Message(code=Code.UNAUTHORIZED)
            
            logging.info(f"Processing Message: {pos}")

            return Message(code=Code.CHANGED)
            
        except Exception as e:
            logging.warning(f"Failed to parse Position: {e}")
            return Message(code=Code.BAD_REQUEST)
        

class CommandResource(resource.Resource):
    async def render_get(self, request: Message) -> Message:
        cmd: Command = Command()
        cmd.header = b'\x05'
        cmd.arg = 'TE-ST4'

        return Message(code=Code.CONTENT, payload=cmd.serialize())
    
async def main():
    root = resource.Site()
    root.add_resource(['position'], PositionResource())
    root.add_resource(['command'], CommandResource())

    await Context.create_server_context(root, bind=('0.0.0.0', 1999))

    logging.info("CoAP Server listening on 0.0.0.0:1999")
    await asyncio.get_running_loop().create_future()

if __name__ == '__main__':
    asyncio.run(main())

