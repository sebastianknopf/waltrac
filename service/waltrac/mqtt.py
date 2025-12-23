import asyncio
import logging
import random
import string

from aiomqtt import Client


class MqttPublisher:
    def __init__(self, host: str, port: int, username: str|None, password: str|None, toplevel: str = 'waltrac'):
        client_random: str = ''.join(random.choices(string.ascii_letters + string.digits, k=8))
        client_id: str = f"waltrac-gateway-{client_random}"
        
        if username is not None and password is not None:
            self._client: Client = Client(
                hostname=host,
                port=port,
                username=username,
                password=password,
                client_id=client_id,
                keepalive=60,
            )
        else:
            self._client: Client = Client(
                hostname=host,
                port=port,
                client_id=client_id,
                keepalive=60,
            )

        self._toplevel = toplevel
        self._connected = asyncio.Event()

    async def start(self):
        asyncio.create_task(self._run())

    async def _run(self):
        while True:
            try:
                async with self._client:
                    logging.info("Connected to MQTT broker.")

                    self._connected.set()
                    await asyncio.get_running_loop().create_future()

            except Exception as e:
                logging.error(f"Connection to MQTT broker interrupted, re-connecting in 5s.")

                self._connected.clear()
                await asyncio.sleep(5)

    async def publish(self, topic: str, payload: bytes):
        await self._connected.wait()
        await self._client.publish(f"{self._toplevel}/{topic}", payload)