import asyncio
import logging

from aiomqtt import Client


class MqttPublisher:
    def __init__(self, host: str, port: int, username: str|None, password: str|None, toplevel: str = 'waltrac'):
        self._host = host
        self._port = port
        self._username = username
        self._password = password
        self._keepalive = 60

        self._toplevel = toplevel
        self._connected = asyncio.Event()
        self._task: asyncio.Task | None = None
        self._client: Client|None = None

    async def start(self):
        if self._task is None or self._task.done():
            self._task = asyncio.create_task(self._run())

    async def stop(self):
        if self._task is not None and not self._task.done():
            self._task.cancel()

            try:
                await self._task
            except asyncio.CancelledError:
                pass

        self._task = None

    async def _run(self):
        while True:
            try:
                if self._username is not None and self._password is not None:
                    client = Client(
                        hostname=self._host,
                        port=self._port,
                        username=self._username,
                        password=self._password,
                        keepalive=self._keepalive,
                    )
                else:
                    client = Client(
                        hostname=self._host,
                        port=self._port,
                        keepalive=self._keepalive,
                    )

                async with client:
                    self._client = client
                    logging.info("Connected to MQTT broker.")

                    self._connected.set()
                    
                    while True:
                        await asyncio.sleep(10)
                        await client.ping()

            except asyncio.CancelledError:
                raise
            except Exception:
                logging.exception("Connection to MQTT broker interrupted, re-connecting in 5s.")

                self._client = None
                self._connected.clear()
                await asyncio.sleep(5)

    async def publish(self, topic: str, payload: bytes):
        await self._connected.wait()
        await self._client.publish(f"{self._toplevel}/{topic}", payload)